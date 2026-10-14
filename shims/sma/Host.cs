// The host side of System.Management.Automation: what a program that hosts PowerShell derives from and passes
// around (PS2EXE's wrappers, which Autodesk's installers run). Members are virtual, not abstract, so a host
// written against a newer PowerShell still loads.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Security;

namespace System.Management.Automation.Host
{
    public abstract class PSHost
    {
        public virtual string Name { get { return ""; } }
        public virtual Version Version { get { return new Version(0, 0); } }
        public virtual Guid InstanceId { get { return Guid.Empty; } }
        public virtual PSHostUserInterface UI { get { return null; } }
        public virtual CultureInfo CurrentCulture { get { return CultureInfo.CurrentCulture; } }
        public virtual CultureInfo CurrentUICulture { get { return CultureInfo.CurrentUICulture; } }
        public virtual PSObject PrivateData { get { return null; } }
        public virtual void SetShouldExit(int exitCode) { }
        public virtual void EnterNestedPrompt() { }
        public virtual void ExitNestedPrompt() { }
        public virtual void NotifyBeginApplication() { }
        public virtual void NotifyEndApplication() { }
    }

    public abstract class PSHostUserInterface
    {
        public virtual PSHostRawUserInterface RawUI { get { return null; } }
        public virtual string ReadLine() { return Console.ReadLine(); }
        public virtual SecureString ReadLineAsSecureString() { return new SecureString(); }
        public virtual void Write(string value) { Console.Write(value); }
        public virtual void Write(ConsoleColor foregroundColor, ConsoleColor backgroundColor, string value) { Write(value); }
        public virtual void WriteLine() { WriteLine(""); }
        public virtual void WriteLine(string value) { Write(value + Environment.NewLine); }
        public virtual void WriteLine(ConsoleColor foregroundColor, ConsoleColor backgroundColor, string value) { WriteLine(value); }
        public virtual void WriteErrorLine(string value) { Console.Error.WriteLine(value); }
        public virtual void WriteDebugLine(string message) { }
        public virtual void WriteVerboseLine(string message) { }
        public virtual void WriteWarningLine(string message) { }
        public virtual void WriteProgress(long sourceId, ProgressRecord record) { }
        public virtual Dictionary<string, PSObject> Prompt(string caption, string message, Collection<FieldDescription> descriptions) { return new Dictionary<string, PSObject>(); }
        public virtual int PromptForChoice(string caption, string message, Collection<ChoiceDescription> choices, int defaultChoice) { return defaultChoice; }
        public virtual PSCredential PromptForCredential(string caption, string message, string userName, string targetName) { return null; }
        public virtual PSCredential PromptForCredential(string caption, string message, string userName, string targetName, PSCredentialTypes allowedCredentialTypes, PSCredentialUIOptions options) { return null; }
    }

    public abstract class PSHostRawUserInterface
    {
        public virtual ConsoleColor ForegroundColor { get; set; }
        public virtual ConsoleColor BackgroundColor { get; set; }
        public virtual Coordinates CursorPosition { get; set; }
        public virtual Coordinates WindowPosition { get; set; }
        public virtual int CursorSize { get; set; }
        public virtual Size BufferSize { get; set; }
        public virtual Size WindowSize { get; set; }
        public virtual Size MaxWindowSize { get { return WindowSize; } }
        public virtual Size MaxPhysicalWindowSize { get { return WindowSize; } }
        public virtual bool KeyAvailable { get { return false; } }
        public virtual string WindowTitle { get; set; }
        public virtual KeyInfo ReadKey(ReadKeyOptions options) { return new KeyInfo(); }
        public KeyInfo ReadKey() { return ReadKey(ReadKeyOptions.IncludeKeyDown); }
        public virtual void FlushInputBuffer() { }
        public virtual void SetBufferContents(Coordinates origin, BufferCell[,] contents) { }
        public virtual void SetBufferContents(Rectangle rectangle, BufferCell fill) { }
        public virtual BufferCell[,] GetBufferContents(Rectangle rectangle) { return new BufferCell[0, 0]; }
        public virtual void ScrollBufferContents(Rectangle source, Coordinates destination, Rectangle clip, BufferCell fill) { }
        public virtual int LengthInBufferCells(string source) { return source == null ? 0 : source.Length; }
        public virtual int LengthInBufferCells(char source) { return 1; }
    }

    public struct Coordinates
    {
        public Coordinates(int x, int y) : this() { X = x; Y = y; }
        public int X { get; set; }
        public int Y { get; set; }
    }

    public struct Size
    {
        public Size(int width, int height) : this() { Width = width; Height = height; }
        public int Width { get; set; }
        public int Height { get; set; }
    }

    public struct Rectangle
    {
        public Rectangle(int left, int top, int right, int bottom) : this() { Left = left; Top = top; Right = right; Bottom = bottom; }
        public int Left { get; set; }
        public int Top { get; set; }
        public int Right { get; set; }
        public int Bottom { get; set; }
    }

    public enum BufferCellType { Complete = 0, Leading = 1, Trailing = 2 }

    public struct BufferCell
    {
        public BufferCell(char character, ConsoleColor foreground, ConsoleColor background, BufferCellType bufferCellType) : this()
        {
            Character = character;
            ForegroundColor = foreground;
            BackgroundColor = background;
            BufferCellType = bufferCellType;
        }
        public char Character { get; set; }
        public ConsoleColor ForegroundColor { get; set; }
        public ConsoleColor BackgroundColor { get; set; }
        public BufferCellType BufferCellType { get; set; }
    }

    [Flags]
    public enum ControlKeyStates
    {
        RightAltPressed = 0x1, LeftAltPressed = 0x2, RightCtrlPressed = 0x4, LeftCtrlPressed = 0x8, ShiftPressed = 0x10,
        NumLockOn = 0x20, ScrollLockOn = 0x40, CapsLockOn = 0x80, EnhancedKey = 0x100,
    }

    [Flags]
    public enum ReadKeyOptions { AllowCtrlC = 1, NoEcho = 2, IncludeKeyDown = 4, IncludeKeyUp = 8 }

    public struct KeyInfo
    {
        public KeyInfo(int virtualKeyCode, char ch, ControlKeyStates controlKeyState, bool keyDown) : this()
        {
            VirtualKeyCode = virtualKeyCode;
            Character = ch;
            ControlKeyState = controlKeyState;
            KeyDown = keyDown;
        }
        public int VirtualKeyCode { get; set; }
        public char Character { get; set; }
        public ControlKeyStates ControlKeyState { get; set; }
        public bool KeyDown { get; set; }
    }

    public sealed class ChoiceDescription
    {
        public ChoiceDescription(string label) { Label = label; HelpMessage = ""; }
        public ChoiceDescription(string label, string helpMessage) { Label = label; HelpMessage = helpMessage; }
        public string Label { get; private set; }
        public string HelpMessage { get; set; }
    }

    public class FieldDescription
    {
        public FieldDescription(string name) { Name = name; Label = ""; HelpMessage = ""; ParameterAssemblyFullName = ""; }
        public string Name { get; private set; }
        public string Label { get; set; }
        public string HelpMessage { get; set; }
        public bool IsMandatory { get; set; }
        public PSObject DefaultValue { get; set; }
        public string ParameterTypeName { get; private set; }
        public string ParameterTypeFullName { get; private set; }
        public string ParameterAssemblyFullName { get; private set; }
    }
}
