// The engine side of System.Management.Automation. There's no PowerShell engine here: a script added to a
// PowerShell object runs in PowerShell 7 (pwsh.exe, installed in the prefix), with its parameters and arguments
// as a script's, its output and errors fed back, and a nonzero exit code passed to the host's SetShouldExit.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Management.Automation.Host;
using System.Management.Automation.Runspaces;
using System.Security;
using System.Text;
using System.Threading;

namespace System.Management.Automation
{
    public class PSObject
    {
        public PSObject() { }
        public PSObject(object obj) { BaseObject = obj; }
        public object BaseObject { get; private set; }
        public object ImmediateBaseObject { get { return BaseObject; } }
        public static PSObject AsPSObject(object obj) { return obj as PSObject ?? new PSObject(obj); }
        public override string ToString() { return BaseObject == null ? "" : BaseObject.ToString(); }
    }

    public sealed class PSCredential
    {
        public PSCredential(string userName, SecureString password) { UserName = userName; Password = password; }
        public string UserName { get; private set; }
        public SecureString Password { get; private set; }
    }

    [Flags]
    public enum PSCredentialTypes { Generic = 1, Domain = 2, Default = Generic | Domain }

    [Flags]
    public enum PSCredentialUIOptions { None = 0, ValidateUserNameSyntax = 1, Default = ValidateUserNameSyntax, AlwaysPrompt = 2, ReadOnlyUserName = 3 }

    public enum ErrorCategory { NotSpecified = 0 }

    public class ErrorRecord
    {
        public ErrorRecord(Exception exception, string errorId, ErrorCategory errorCategory, object targetObject)
        {
            Exception = exception;
            FullyQualifiedErrorId = errorId;
            TargetObject = targetObject;
        }
        public Exception Exception { get; private set; }
        public string FullyQualifiedErrorId { get; private set; }
        public object TargetObject { get; private set; }
        public override string ToString() { return Exception.Message; }
    }

    public class ProgressRecord
    {
        public ProgressRecord(int activityId, string activity, string statusDescription)
        {
            ActivityId = activityId;
            Activity = activity;
            StatusDescription = statusDescription;
            PercentComplete = -1;
        }
        public int ActivityId { get; private set; }
        public string Activity { get; set; }
        public string StatusDescription { get; set; }
        public int PercentComplete { get; set; }
    }

    public sealed class DataAddedEventArgs : EventArgs
    {
        internal DataAddedEventArgs(int index) { Index = index; }
        public int Index { get; private set; }
        public Guid PSInstanceId { get { return Guid.Empty; } }
    }

    public class PSDataCollection<T> : IList<T>, IDisposable
    {
        readonly List<T> items = new List<T>();
        public event EventHandler<DataAddedEventArgs> DataAdded;
        public bool IsOpen { get; private set; }
        public PSDataCollection() { IsOpen = true; }

        public void Add(T item)
        {
            int index;
            lock (items)
            {
                items.Add(item);
                index = items.Count - 1;
            }
            var added = DataAdded;
            if (added != null) added(this, new DataAddedEventArgs(index));
        }
        public void Complete() { IsOpen = false; }
        public T this[int index]
        {
            get { lock (items) return items[index]; }
            set { lock (items) items[index] = value; }
        }
        public int Count { get { lock (items) return items.Count; } }
        public bool IsReadOnly { get { return false; } }
        public void Clear() { lock (items) items.Clear(); }
        public bool Contains(T item) { lock (items) return items.Contains(item); }
        public void CopyTo(T[] array, int arrayIndex) { lock (items) items.CopyTo(array, arrayIndex); }
        public int IndexOf(T item) { lock (items) return items.IndexOf(item); }
        public void Insert(int index, T item) { lock (items) items.Insert(index, item); }
        public bool Remove(T item) { lock (items) return items.Remove(item); }
        public void RemoveAt(int index) { lock (items) items.RemoveAt(index); }
        public IEnumerator<T> GetEnumerator() { lock (items) return new List<T>(items).GetEnumerator(); }
        IEnumerator IEnumerable.GetEnumerator() { return GetEnumerator(); }
        public void Dispose() { }
    }

    public sealed class PSDataStreams
    {
        internal PSDataStreams()
        {
            Error = new PSDataCollection<ErrorRecord>();
            Warning = new PSDataCollection<string>();
            Verbose = new PSDataCollection<string>();
            Debug = new PSDataCollection<string>();
        }
        public PSDataCollection<ErrorRecord> Error { get; set; }
        public PSDataCollection<string> Warning { get; set; }
        public PSDataCollection<string> Verbose { get; set; }
        public PSDataCollection<string> Debug { get; set; }
    }

    public sealed class PSInvocationSettings { }

    public enum PSInvocationState { NotStarted = 0, Running = 1, Stopping = 2, Stopped = 3, Completed = 4, Failed = 5, Disconnected = 6 }

    public sealed class PSInvocationStateInfo
    {
        internal PSInvocationStateInfo(PSInvocationState state, Exception reason) { State = state; Reason = reason; }
        public PSInvocationState State { get; private set; }
        public Exception Reason { get; private set; }
    }

    sealed class Completed : IAsyncResult
    {
        readonly ManualResetEvent done = new ManualResetEvent(false);
        public object AsyncState { get; set; }
        public WaitHandle AsyncWaitHandle { get { return done; } }
        public bool CompletedSynchronously { get { return false; } }
        public bool IsCompleted { get; private set; }
        public void Finish(AsyncCallback callback)
        {
            IsCompleted = true;
            done.Set();
            if (callback != null) callback(this);
        }
    }

    public sealed class PowerShell : IDisposable
    {
        string script;
        readonly List<string> scriptArgs = new List<string>();
        bool pastScript;  // a command added after the script (PS2EXE's Out-String -Stream): pwsh's output is text already
        Process process;
        Completed running;

        PowerShell()
        {
            Streams = new PSDataStreams();
            InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.NotStarted, null);
        }
        public static PowerShell Create() { return new PowerShell(); }

        public Runspace Runspace { get; set; }
        public PSDataStreams Streams { get; private set; }
        public PSInvocationStateInfo InvocationStateInfo { get; private set; }

        public PowerShell AddScript(string text)
        {
            if (script == null) script = text;
            else pastScript = true;
            return this;
        }
        public PowerShell AddCommand(string cmdlet)
        {
            pastScript = true;
            return this;
        }
        public PowerShell AddParameter(string parameterName)
        {
            if (!pastScript) scriptArgs.Add("-" + parameterName);
            return this;
        }
        public PowerShell AddParameter(string parameterName, object value)
        {
            if (pastScript) return this;
            if (value is bool) scriptArgs.Add("-" + parameterName + ((bool)value ? ":$true" : ":$false"));
            else
            {
                scriptArgs.Add("-" + parameterName);
                scriptArgs.Add(Convert.ToString(value));
            }
            return this;
        }
        public PowerShell AddArgument(object value)
        {
            if (!pastScript) scriptArgs.Add(Convert.ToString(value));
            return this;
        }

        public IAsyncResult BeginInvoke<TInput, TOutput>(PSDataCollection<TInput> input, PSDataCollection<TOutput> output,
                                                         PSInvocationSettings settings, AsyncCallback callback, object state)
        {
            running = new Completed { AsyncState = state };
            InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.Running, null);
            var thread = new Thread(() =>
            {
                Run(input, output);
                running.Finish(callback);
            });
            thread.IsBackground = true;
            thread.Start();
            return running;
        }

        public void Stop()
        {
            var running_process = process;
            try
            {
                if (running_process != null && !running_process.HasExited) running_process.Kill();
            }
            catch (InvalidOperationException) { }
        }
        public IAsyncResult BeginStop(AsyncCallback callback, object state)
        {
            Stop();
            var stopped = new Completed { AsyncState = state };
            stopped.Finish(callback);
            return stopped;
        }
        public void Dispose() { Stop(); }

        static string Pwsh()
        {
            foreach (var root in new[] { Environment.GetEnvironmentVariable("ProgramW6432"), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) })
            {
                if (string.IsNullOrEmpty(root)) continue;
                var pwsh = Path.Combine(root, @"PowerShell\7\pwsh.exe");
                if (File.Exists(pwsh)) return pwsh;
            }
            return null;
        }

        // One argument as CommandLineToArgvW reads it back.
        static string Quote(string arg)
        {
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return arg;
            var quoted = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in arg)
            {
                if (c == '\\') { slashes++; continue; }
                quoted.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
                slashes = 0;
                quoted.Append(c);
            }
            quoted.Append('\\', slashes * 2).Append('"');
            return quoted.ToString();
        }

        void Fail(string message)
        {
            InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.Failed, new InvalidOperationException(message));
            if (Runspace != null && Runspace.Host != null) Runspace.Host.SetShouldExit(1);
        }

        void Run<TInput, TOutput>(PSDataCollection<TInput> input, PSDataCollection<TOutput> output)
        {
            var pwsh = Pwsh();
            if (pwsh == null)
            {
                Fail("PowerShell 7 (pwsh.exe) isn't installed, and this program's script needs it");
                return;
            }
            var file = Path.Combine(Path.GetTempPath(), "sma-" + Guid.NewGuid().ToString("N") + ".ps1");
            File.WriteAllText(file, script ?? "", new UTF8Encoding(true));
            try
            {
                var args = new StringBuilder("-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ").Append(Quote(file));
                foreach (var arg in scriptArgs) args.Append(' ').Append(Quote(arg));
                var info = new ProcessStartInfo(pwsh, args.ToString())
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardInput = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8,
                };
                // .NET in pwsh can't load Wine's ICU; without it, it stops before running anything.
                if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT")))
                    info.EnvironmentVariables["DOTNET_SYSTEM_GLOBALIZATION_INVARIANT"] = "1";
                process = Process.Start(info);
                process.OutputDataReceived += (sender, e) =>
                {
                    if (e.Data == null || output == null) return;
                    object line = typeof(TOutput) == typeof(PSObject) ? (object)new PSObject(e.Data) : e.Data;
                    output.Add((TOutput)line);
                };
                process.ErrorDataReceived += (sender, e) =>
                {
                    if (e.Data != null) Streams.Error.Add(new ErrorRecord(new Exception(e.Data), "pwsh", ErrorCategory.NotSpecified, null));
                };
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
                if (input != null)
                {
                    foreach (var item in input) process.StandardInput.WriteLine(Convert.ToString(item));
                }
                process.StandardInput.Close();
                process.WaitForExit();
                if (process.ExitCode != 0 && Runspace != null && Runspace.Host != null) Runspace.Host.SetShouldExit(process.ExitCode);
                InvocationStateInfo = new PSInvocationStateInfo(PSInvocationState.Completed, null);
            }
            catch (Exception e)
            {
                Fail("couldn't run pwsh.exe: " + e.Message);
            }
            finally
            {
                try { File.Delete(file); } catch (IOException) { }
            }
        }
    }
}

namespace System.Management.Automation.Runspaces
{
    public abstract class Runspace : IDisposable
    {
        public abstract PSHost Host { get; }
        public ApartmentState ApartmentState { get; set; }
        public abstract void Open();
        public abstract void Close();
        public void Dispose() { Close(); }
    }

    sealed class HostRunspace : Runspace
    {
        readonly PSHost host;
        public HostRunspace(PSHost host) { this.host = host; }
        public override PSHost Host { get { return host; } }
        public override void Open() { }
        public override void Close() { }
    }

    public static class RunspaceFactory
    {
        public static Runspace CreateRunspace() { return new HostRunspace(null); }
        public static Runspace CreateRunspace(PSHost host) { return new HostRunspace(host); }
    }
}
