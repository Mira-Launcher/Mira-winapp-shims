// qmgr: a BITS (Background Intelligent Transfer Service) that downloads.
//
// Wine's qmgr cannot do what Office's Click-to-Run asks of BITS: it adds each
// file with its byte ranges (IBackgroundCopyJob3::AddFileWithRanges), which
// Wine leaves unimplemented, so the job stays empty and Resume fails with
// BG_E_EMPTY. Click-to-Run then falls back to a transport that opens a new TLS
// connection for every request and crawls at a tenth of the link's speed.
//
// This service implements the BITS COM API those clients use and moves the
// bytes with a pool of WinHTTP workers, each holding one keep-alive
// connection and pulling ranges from a shared counter. Written from the
// public BITS interface definitions; the proxy/stub DLL (qmgrprxy.dll) and the
// COM registration are Wine's own.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <initguid.h>  // define the BITS GUIDs here: libuuid does not carry them
#include <bits.h>
#include <bits1_5.h>
#include <bits2_0.h>
#include <bits2_5.h>
#include <bits3_0.h>
#include <bitsmsg.h>

#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

class Job;

namespace {

constexpr UINT64 kChunk = 2u << 20;  // bytes a worker asks for at a time
constexpr int kWorkers = 8;          // parallel keep-alive connections
constexpr DWORD kMaxBackoffMs = 10000;  // longest wait between tries after the network drops
constexpr DWORD kReadBuffer = 256 * 1024;

// ---------------------------------------------------------------- logging

CRITICAL_SECTION log_lock;
bool log_ready = false;

void Log(const char* format, ...) {
  if (!log_ready) return;
  char line[1024];
  SYSTEMTIME now;
  GetLocalTime(&now);
  int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
  va_list args;
  va_start(args, format);
  n += vsnprintf(line + n, sizeof line - n - 2, format, args);
  va_end(args);
  if (n > static_cast<int>(sizeof line) - 2) n = sizeof line - 2;
  line[n++] = '\n';
  EnterCriticalSection(&log_lock);
  HANDLE file = CreateFileW(L"C:\\windows\\temp\\mira-qmgr.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written;
    WriteFile(file, line, n, &written, nullptr);
    CloseHandle(file);
  }
  LeaveCriticalSection(&log_lock);
}

// ---------------------------------------------------------------- progress file

// Bytes moved and bytes expected over every file this service has been given, published as "done total" in a
// small file so a launcher can show a real progress bar (Office reports none while it downloads).
volatile LONG64 all_done = 0;
volatile LONG64 all_total = 0;
volatile LONGLONG status_written = 0;  // tick of the last write

void PublishProgress(bool force = false) {
  const LONGLONG now = static_cast<LONGLONG>(GetTickCount64());
  const LONGLONG last = status_written;
  if (!force && now - last < 500) return;
  if (InterlockedCompareExchange64(&status_written, now, last) != last) return;  // another thread is writing it
  char text[64];
  const int n = snprintf(text, sizeof text, "%lld %lld\n", static_cast<long long>(all_done), static_cast<long long>(all_total));
  HANDLE file = CreateFileW(L"C:\\windows\\temp\\mira-bits.txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written;
  WriteFile(file, text, n, &written, nullptr);
  CloseHandle(file);
}

// ---------------------------------------------------------------- helpers

LPWSTR CoString(const std::wstring& text) {
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  auto* out = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
  if (out) memcpy(out, text.c_str(), bytes);
  return out;
}

HRESULT ReturnString(const std::wstring& text, LPWSTR* out) {
  if (!out) return E_POINTER;
  *out = CoString(text);
  return *out ? S_OK : E_OUTOFMEMORY;
}

// An HTTP status as the HRESULT BITS reports it (BG_E_HTTP_ERROR_xxx).
HRESULT HttpStatusError(DWORD status) { return static_cast<HRESULT>(0x80190000u | status); }

struct Lock {
  explicit Lock(CRITICAL_SECTION& cs) : cs_(cs) { EnterCriticalSection(&cs_); }
  ~Lock() { LeaveCriticalSection(&cs_); }
  CRITICAL_SECTION& cs_;
};

FILETIME Now() {
  FILETIME t;
  GetSystemTimeAsFileTime(&t);
  return t;
}

// ---------------------------------------------------------------- data

struct FileEntry {
  std::wstring remote;
  std::wstring local;
  std::vector<BG_FILE_RANGE> ranges;  // empty: the whole file
  volatile LONG64 total = -1;         // -1 until the server has said
  volatile LONG64 transferred = 0;
  volatile LONG completed = 0;
};
using FilePtr = std::shared_ptr<FileEntry>;

// ---------------------------------------------------------------- errors

class CopyError final : public IBackgroundCopyError {
 public:
  CopyError(BG_ERROR_CONTEXT context, HRESULT code, FilePtr file) : context_(context), code_(code), file_(file) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IBackgroundCopyError) {
      *out = static_cast<IBackgroundCopyError*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetError(BG_ERROR_CONTEXT* context, HRESULT* code) override {
    if (!context || !code) return E_POINTER;
    *context = context_;
    *code = code_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFile(IBackgroundCopyFile** file) override;
  HRESULT STDMETHODCALLTYPE GetErrorDescription(DWORD language, LPWSTR* text) override {
    wchar_t buffer[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code_, language, buffer,
                   ARRAYSIZE(buffer), nullptr);
    if (!*buffer) swprintf(buffer, ARRAYSIZE(buffer), L"Error 0x%08lx", static_cast<unsigned long>(code_));
    return ReturnString(buffer, text);
  }
  HRESULT STDMETHODCALLTYPE GetErrorContextDescription(DWORD, LPWSTR* text) override {
    return ReturnString(L"The error occurred while transferring the file.", text);
  }
  HRESULT STDMETHODCALLTYPE GetProtocol(LPWSTR* protocol) override { return ReturnString(L"HTTP", protocol); }

 private:
  LONG refs_ = 1;
  BG_ERROR_CONTEXT context_;
  HRESULT code_;
  FilePtr file_;
};

// ---------------------------------------------------------------- files

class BitsFile final : public IBackgroundCopyFile2 {
 public:
  explicit BitsFile(FilePtr file) : file_(std::move(file)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IBackgroundCopyFile || riid == IID_IBackgroundCopyFile2) {
      *out = static_cast<IBackgroundCopyFile2*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetRemoteName(LPWSTR* out) override { return ReturnString(file_->remote, out); }
  HRESULT STDMETHODCALLTYPE GetLocalName(LPWSTR* out) override { return ReturnString(file_->local, out); }
  HRESULT STDMETHODCALLTYPE GetProgress(BG_FILE_PROGRESS* out) override {
    if (!out) return E_POINTER;
    out->BytesTotal = static_cast<UINT64>(file_->total);
    out->BytesTransferred = static_cast<UINT64>(file_->transferred);
    out->Completed = file_->completed != 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFileRanges(DWORD* count, BG_FILE_RANGE** ranges) override {
    if (!count || !ranges) return E_POINTER;
    *count = static_cast<DWORD>(file_->ranges.size());
    *ranges = nullptr;
    if (file_->ranges.empty()) return S_OK;
    const size_t bytes = file_->ranges.size() * sizeof(BG_FILE_RANGE);
    *ranges = static_cast<BG_FILE_RANGE*>(CoTaskMemAlloc(bytes));
    if (!*ranges) return E_OUTOFMEMORY;
    memcpy(*ranges, file_->ranges.data(), bytes);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetRemoteName(LPCWSTR) override { return BG_E_INVALID_STATE; }

 private:
  LONG refs_ = 1;
  FilePtr file_;
};

HRESULT CopyError::GetFile(IBackgroundCopyFile** file) {
  if (!file) return E_POINTER;
  *file = nullptr;
  if (!file_) return BG_E_FILE_NOT_AVAILABLE;
  *file = new BitsFile(file_);
  return S_OK;
}

// A snapshot enumerator over COM objects of one kind.
template <typename Enum, typename Item, const IID& Iid>
class Enumerator final : public Enum {
 public:
  explicit Enumerator(std::vector<Item*> items) : items_(std::move(items)) {
    for (Item* item : items_) item->AddRef();
  }
  ~Enumerator() {
    for (Item* item : items_) item->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == Iid) {
      *out = static_cast<Enum*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, Item** out, ULONG* fetched) override {
    if (!out) return E_POINTER;
    ULONG got = 0;
    while (got < count && at_ < items_.size()) {
      out[got] = items_[at_++];
      out[got]->AddRef();
      ++got;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    at_ = at_ + count > items_.size() ? items_.size() : at_ + count;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    at_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(Enum** out) override {
    if (!out) return E_POINTER;
    auto* copy = new Enumerator(items_);
    copy->at_ = at_;
    *out = copy;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCount(ULONG* count) override {
    if (!count) return E_POINTER;
    *count = static_cast<ULONG>(items_.size());
    return S_OK;
  }

 private:
  LONG refs_ = 1;
  std::vector<Item*> items_;
  size_t at_ = 0;
};
using FileEnum = Enumerator<IEnumBackgroundCopyFiles, IBackgroundCopyFile, IID_IEnumBackgroundCopyFiles>;
using JobEnum = Enumerator<IEnumBackgroundCopyJobs, IBackgroundCopyJob, IID_IEnumBackgroundCopyJobs>;

// ---------------------------------------------------------------- transfer

// A piece of a file: bytes [remote, remote+length) of the remote file land at `local` in the local one. They are
// the same for a whole file; for a file added with ranges the local file holds the ranges packed back to back, in
// the order they were given.
struct Chunk {
  UINT64 remote;
  UINT64 local;
  UINT64 length;
};

// What one file's download shares between its workers.
struct Transfer {
  Job* job;
  FilePtr file;
  std::wstring host;
  std::wstring path;  // path and query
  INTERNET_PORT port = 0;
  bool secure = false;
  std::wstring headers;
  std::wstring target;  // file being written
  std::vector<Chunk> chunks;
  volatile LONG next = 0;
  volatile LONG failed = 0;
  volatile HRESULT error = S_OK;
  volatile LONG* stop;
  ULONGLONG give_up_ms = 0;               // how long without a byte before the job fails (the job's no-progress timeout)
  volatile LONGLONG last_progress = 0;    // tick of the last byte (or the start)
};

bool CrackUrl(const std::wstring& url, Transfer& t) {
  URL_COMPONENTSW parts = {};
  parts.dwStructSize = sizeof parts;
  parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
  t.host.assign(parts.lpszHostName, parts.dwHostNameLength);
  t.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) t.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  t.port = parts.nPort;
  t.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  return true;
}

// Whether a failed request is worth trying again. A refusal the server means (not found, forbidden, a bad range)
// will not change; anything else (the network dropping, a timeout, a busy server) usually does.
bool Retryable(HRESULT hr) {
  for (DWORD status : {400u, 401u, 403u, 404u, 410u, 416u}) {
    if (hr == HttpStatusError(status)) return false;
  }
  return true;
}

// Waits `ms`, in short steps, until `stop` is set.
void SleepUnlessStopped(DWORD ms, volatile LONG* stop) {
  for (DWORD waited = 0; waited < ms && !*stop; waited += 200) Sleep(200);
}

struct Connection {
  HINTERNET session = nullptr;
  HINTERNET connect = nullptr;
  ~Connection() { Close(); }
  void Close() {
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    connect = session = nullptr;
  }
  bool Open(const Transfer& t) {
    Close();
    session = WinHttpOpen(L"Microsoft BITS/7.8", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    connect = WinHttpConnect(session, t.host.c_str(), t.port, 0);
    return connect != nullptr;
  }
};

// One GET of bytes [offset, offset+length) (or the whole body when `length`
// is 0). Hands each piece of the body to `sink`. Returns the HTTP status via
// `status`, or a Win32 error as an HRESULT.
template <typename Sink>
HRESULT Get(Connection& c, const Transfer& t, UINT64 offset, UINT64 length, DWORD* status, UINT64* total, Sink sink,
            std::wstring* final_url = nullptr) {
  HINTERNET request = WinHttpOpenRequest(c.connect, L"GET", t.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, t.secure ? WINHTTP_FLAG_SECURE : 0);
  if (!request) return HRESULT_FROM_WIN32(GetLastError());
  struct Close {
    HINTERNET h;
    ~Close() { WinHttpCloseHandle(h); }
  } close{request};

  std::wstring header = t.headers;
  if (length) {
    wchar_t range[96];
    swprintf(range, ARRAYSIZE(range), L"Range: bytes=%llu-%llu\r\n", static_cast<unsigned long long>(offset),
             static_cast<unsigned long long>(offset + length - 1));
    header += range;
  }
  if (!WinHttpSendRequest(request, header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(),
                          header.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  DWORD code = 0, size = sizeof code;
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  *status = code;
  if (total) {
    wchar_t text[128] = L"";
    size = sizeof text;
    unsigned long long first = 0, last = 0, all = 0;
    if (code == 206 &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, text, &size,
                            WINHTTP_NO_HEADER_INDEX) &&
        swscanf(text, L"bytes %llu-%llu/%llu", &first, &last, &all) == 3) {
      *total = all;
    } else {
      unsigned long long len = 0;
      size = sizeof text;
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &size,
                              WINHTTP_NO_HEADER_INDEX) &&
          swscanf(text, L"%llu", &len) == 1) {
        *total = len;
      }
    }
  }
  if (final_url) {
    wchar_t url[2048];
    size = sizeof url;
    if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, url, &size)) *final_url = url;
  }
  if (code != 200 && code != 206) return HttpStatusError(code);

  std::vector<char> buffer(kReadBuffer);
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(request, buffer.data(), kReadBuffer, &got)) return HRESULT_FROM_WIN32(GetLastError());
    if (!got) break;
    if (HRESULT hr = sink(buffer.data(), got); FAILED(hr)) return hr;
  }
  return S_OK;
}

}  // namespace

static void Notify(Job* job, bool failed);

namespace {
CRITICAL_SECTION jobs_lock;
std::vector<Job*> jobs;
HANDLE service_stop = nullptr;
}  // namespace

// ---------------------------------------------------------------- the job

class Job final : public IBackgroundCopyJob4, public IBackgroundCopyJobHttpOptions {
 public:
  Job(const std::wstring& name, BG_JOB_TYPE type) : name_(name), type_(type) {
    InitializeCriticalSection(&lock_);
    CoCreateGuid(&id_);
    created_ = modified_ = Now();
  }
  ~Job() {
    if (thread_) CloseHandle(thread_);
    if (notify_) notify_->Release();
    DeleteCriticalSection(&lock_);
  }

  GUID Id() const { return id_; }

  // ---- IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IBackgroundCopyJob || riid == IID_IBackgroundCopyJob2 ||
        riid == IID_IBackgroundCopyJob3 || riid == IID_IBackgroundCopyJob4) {
      *out = static_cast<IBackgroundCopyJob4*>(this);
    } else if (riid == IID_IBackgroundCopyJobHttpOptions) {
      *out = static_cast<IBackgroundCopyJobHttpOptions*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }

  // ---- files
  HRESULT STDMETHODCALLTYPE AddFileSet(ULONG count, BG_FILE_INFO* set) override {
    if (!set) return E_POINTER;
    for (ULONG i = 0; i < count; ++i) {
      if (HRESULT hr = Add(set[i].RemoteName, set[i].LocalName, 0, nullptr); FAILED(hr)) return hr;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE AddFile(LPCWSTR remote, LPCWSTR local) override { return Add(remote, local, 0, nullptr); }
  HRESULT STDMETHODCALLTYPE AddFileWithRanges(LPCWSTR remote, LPCWSTR local, DWORD count,
                                              BG_FILE_RANGE* ranges) override {
    if (count && !ranges) return E_POINTER;
    return Add(remote, local, count, ranges);
  }
  HRESULT STDMETHODCALLTYPE EnumFiles(IEnumBackgroundCopyFiles** out) override {
    if (!out) return E_POINTER;
    std::vector<IBackgroundCopyFile*> items;
    {
      Lock hold(lock_);
      for (const FilePtr& file : files_) items.push_back(new BitsFile(file));
    }
    *out = new FileEnum(items);
    for (auto* item : items) item->Release();  // the enumerator took its own
    return S_OK;
  }

  // ---- control
  HRESULT STDMETHODCALLTYPE Suspend() override {
    Lock hold(lock_);
    if (state_ == BG_JOB_STATE_CANCELLED || state_ == BG_JOB_STATE_ACKNOWLEDGED) return BG_E_INVALID_STATE;
    InterlockedExchange(&stop_, 1);
    if (state_ != BG_JOB_STATE_TRANSFERRED) state_ = BG_JOB_STATE_SUSPENDED;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Resume() override {
    Lock hold(lock_);
    if (state_ == BG_JOB_STATE_CANCELLED || state_ == BG_JOB_STATE_ACKNOWLEDGED) return BG_E_INVALID_STATE;
    if (files_.empty()) return BG_E_EMPTY;
    if (running_ || state_ == BG_JOB_STATE_TRANSFERRED) return S_OK;
    if (thread_) {
      CloseHandle(thread_);
      thread_ = nullptr;
    }
    InterlockedExchange(&stop_, 0);
    state_ = BG_JOB_STATE_QUEUED;
    running_ = true;
    AddRef();  // the worker's
    thread_ = CreateThread(nullptr, 0, &Job::Main, this, 0, nullptr);
    if (!thread_) {
      running_ = false;
      state_ = BG_JOB_STATE_SUSPENDED;
      Release();
      return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Cancel() override {
    HANDLE wait = nullptr;
    {
      Lock hold(lock_);
      if (state_ == BG_JOB_STATE_CANCELLED || state_ == BG_JOB_STATE_ACKNOWLEDGED) return BG_E_INVALID_STATE;
      InterlockedExchange(&stop_, 1);
      if (running_ && thread_ && GetCurrentThreadId() != thread_id_) wait = thread_;
    }
    if (wait) WaitForSingleObject(wait, 30000);
    {
      Lock hold(lock_);
      state_ = BG_JOB_STATE_CANCELLED;
      for (const FilePtr& file : files_) {
        if (!file->completed) DeleteFileW(Temp(*file).c_str());
      }
    }
    Forget();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Complete() override {
    Lock hold(lock_);
    if (state_ != BG_JOB_STATE_TRANSFERRED && state_ != BG_JOB_STATE_ACKNOWLEDGED) return BG_E_INVALID_STATE;
    HRESULT result = S_OK;
    for (const FilePtr& file : files_) {
      if (!MoveFileExW(Temp(*file).c_str(), file->local.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (GetFileAttributesW(Temp(*file).c_str()) != INVALID_FILE_ATTRIBUTES) {
          result = HRESULT_FROM_WIN32(GetLastError());
        }
      }
    }
    if (FAILED(result)) return result;
    state_ = BG_JOB_STATE_ACKNOWLEDGED;
    Forget();
    return S_OK;
  }

  // ---- state
  HRESULT STDMETHODCALLTYPE GetId(GUID* out) override {
    if (!out) return E_POINTER;
    *out = id_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetType(BG_JOB_TYPE* out) override {
    if (!out) return E_POINTER;
    *out = type_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetProgress(BG_JOB_PROGRESS* out) override {
    if (!out) return E_POINTER;
    Lock hold(lock_);
    UINT64 total = 0, done = 0, transferred_files = 0;
    for (const FilePtr& file : files_) {
      if (file->total < 0) {
        total = BG_SIZE_UNKNOWN;
        break;
      }
      total += static_cast<UINT64>(file->total);
    }
    for (const FilePtr& file : files_) {
      done += static_cast<UINT64>(file->transferred);
      if (file->completed) ++transferred_files;
    }
    out->BytesTotal = total;
    out->BytesTransferred = done;
    out->FilesTotal = static_cast<ULONG>(files_.size());
    out->FilesTransferred = static_cast<ULONG>(transferred_files);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTimes(BG_JOB_TIMES* out) override {
    if (!out) return E_POINTER;
    Lock hold(lock_);
    out->CreationTime = created_;
    out->ModificationTime = modified_;
    out->TransferCompletionTime = completed_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetState(BG_JOB_STATE* out) override {
    if (!out) return E_POINTER;
    Lock hold(lock_);
    *out = state_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetError(IBackgroundCopyError** out) override {
    if (!out) return E_POINTER;
    Lock hold(lock_);
    *out = nullptr;
    if (state_ != BG_JOB_STATE_ERROR) return BG_E_ERROR_INFORMATION_UNAVAILABLE;
    *out = new CopyError(error_context_, error_, error_file_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOwner(LPWSTR* out) override { return ReturnString(L"S-1-5-18", out); }
  HRESULT STDMETHODCALLTYPE SetDisplayName(LPCWSTR value) override {
    if (!value) return E_POINTER;
    Lock hold(lock_);
    name_ = value;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR* out) override {
    Lock hold(lock_);
    return ReturnString(name_, out);
  }
  HRESULT STDMETHODCALLTYPE SetDescription(LPCWSTR value) override {
    if (!value) return E_POINTER;
    Lock hold(lock_);
    description_ = value;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDescription(LPWSTR* out) override {
    Lock hold(lock_);
    return ReturnString(description_, out);
  }
  HRESULT STDMETHODCALLTYPE SetPriority(BG_JOB_PRIORITY value) override {
    priority_ = value;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPriority(BG_JOB_PRIORITY* out) override {
    if (!out) return E_POINTER;
    *out = priority_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetNotifyFlags(ULONG value) override {
    notify_flags_ = value;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNotifyFlags(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = notify_flags_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetNotifyInterface(IUnknown* value) override {
    Lock hold(lock_);
    if (value) value->AddRef();
    if (notify_) notify_->Release();
    notify_ = value;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNotifyInterface(IUnknown** out) override {
    if (!out) return E_POINTER;
    Lock hold(lock_);
    *out = notify_;
    if (notify_) notify_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMinimumRetryDelay(ULONG seconds) override {
    retry_delay_ = seconds;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMinimumRetryDelay(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = retry_delay_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetNoProgressTimeout(ULONG seconds) override {
    no_progress_ = seconds;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetNoProgressTimeout(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = no_progress_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetErrorCount(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = static_cast<ULONG>(error_count_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetProxySettings(BG_JOB_PROXY_USAGE, const WCHAR*, const WCHAR*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetProxySettings(BG_JOB_PROXY_USAGE* usage, LPWSTR* list, LPWSTR* bypass) override {
    if (!usage || !list || !bypass) return E_POINTER;
    *usage = BG_JOB_PROXY_USAGE_PRECONFIG;
    *list = CoString(L"");
    *bypass = CoString(L"");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE TakeOwnership() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetNotifyCmdLine(LPCWSTR, LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetNotifyCmdLine(LPWSTR* program, LPWSTR* parameters) override {
    if (!program || !parameters) return E_POINTER;
    *program = *parameters = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetReplyProgress(BG_JOB_REPLY_PROGRESS* out) override {
    if (!out) return E_POINTER;
    out->BytesTotal = BG_SIZE_UNKNOWN;
    out->BytesTransferred = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetReplyData(byte** data, UINT64* length) override {
    if (!data || !length) return E_POINTER;
    *data = nullptr;
    *length = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetReplyFileName(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetReplyFileName(LPWSTR* out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetCredentials(BG_AUTH_CREDENTIALS*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE RemoveCredentials(BG_AUTH_TARGET, BG_AUTH_SCHEME) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE ReplaceRemotePrefix(LPCWSTR from, LPCWSTR to) override {
    if (!from || !to) return E_POINTER;
    Lock hold(lock_);
    const std::wstring old_prefix = from;
    for (const FilePtr& file : files_) {
      if (_wcsnicmp(file->remote.c_str(), old_prefix.c_str(), old_prefix.size()) == 0) {
        file->remote = std::wstring(to) + file->remote.substr(old_prefix.size());
      }
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetFileACLFlags(DWORD flags) override {
    acl_flags_ = flags;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFileACLFlags(DWORD* out) override {
    if (!out) return E_POINTER;
    *out = acl_flags_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPeerCachingFlags(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetPeerCachingFlags(DWORD* out) override {
    if (!out) return E_POINTER;
    *out = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOwnerIntegrityLevel(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = 0x3000;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOwnerElevationState(WINBOOL* out) override {
    if (!out) return E_POINTER;
    *out = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMaximumDownloadTime(ULONG) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetMaximumDownloadTime(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = 0;
    return S_OK;
  }

  // ---- IBackgroundCopyJobHttpOptions
  HRESULT STDMETHODCALLTYPE SetClientCertificateByID(BG_CERT_STORE_LOCATION, LPCWSTR, BYTE*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetClientCertificateByName(BG_CERT_STORE_LOCATION, LPCWSTR, LPCWSTR) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE RemoveClientCertificate() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetClientCertificate(BG_CERT_STORE_LOCATION*, LPWSTR*, BYTE**, LPWSTR*) override {
    return BG_E_ERROR_INFORMATION_UNAVAILABLE;
  }
  HRESULT STDMETHODCALLTYPE SetCustomHeaders(LPCWSTR headers) override {
    Lock hold(lock_);
    headers_ = headers ? headers : L"";
    if (!headers_.empty() && headers_.compare(headers_.size() - 2, 2, L"\r\n") != 0) headers_ += L"\r\n";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCustomHeaders(LPWSTR* out) override {
    Lock hold(lock_);
    return headers_.empty() ? (out ? (*out = nullptr, S_OK) : E_POINTER) : ReturnString(headers_, out);
  }
  HRESULT STDMETHODCALLTYPE SetSecurityFlags(ULONG flags) override {
    security_flags_ = flags;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSecurityFlags(ULONG* out) override {
    if (!out) return E_POINTER;
    *out = security_flags_;
    return S_OK;
  }

 private:
  static std::wstring Temp(const FileEntry& file) { return file.local + L".miraqmgr"; }

  HRESULT Add(LPCWSTR remote, LPCWSTR local, DWORD range_count, const BG_FILE_RANGE* ranges) {
    if (!remote || !local) return E_POINTER;
    auto file = std::make_shared<FileEntry>();
    file->remote = remote;
    file->local = local;
    UINT64 total = 0;
    bool open_ended = false;
    for (DWORD i = 0; i < range_count; ++i) {
      if (ranges[i].InitialOffset == BG_LENGTH_TO_EOF) return BG_E_INVALID_RANGE;
      if (ranges[i].Length == BG_LENGTH_TO_EOF) open_ended = true;  // the size comes from the server
      file->ranges.push_back(ranges[i]);
      if (!open_ended) total += ranges[i].Length;
    }
    // Overlapping or duplicate ranges are refused (the ranges are checked in offset order).
    std::vector<BG_FILE_RANGE> sorted = file->ranges;
    std::sort(sorted.begin(), sorted.end(),
              [](const BG_FILE_RANGE& a, const BG_FILE_RANGE& b) { return a.InitialOffset < b.InitialOffset; });
    for (size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i - 1].Length == BG_LENGTH_TO_EOF || sorted[i - 1].InitialOffset + sorted[i - 1].Length > sorted[i].InitialOffset) {
        return BG_E_OVERLAPPING_RANGES;
      }
    }
    if (range_count && !open_ended) file->total = static_cast<LONG64>(total);
    Lock hold(lock_);
    if (state_ != BG_JOB_STATE_SUSPENDED && state_ != BG_JOB_STATE_QUEUED && state_ != BG_JOB_STATE_ERROR) {
      return BG_E_INVALID_STATE;
    }
    files_.push_back(file);
    Log("job %ls: %ls -> %ls (%lu ranges, %llu bytes)", name_.c_str(), remote, local, static_cast<unsigned long>(range_count),
        static_cast<unsigned long long>(total));
    return S_OK;
  }

  // The job is waiting out a network problem (BG_JOB_STATE_TRANSIENT_ERROR) or moving data again.
  void SetTransient(bool on) {
    Lock hold(lock_);
    if (on && state_ == BG_JOB_STATE_TRANSFERRING) state_ = BG_JOB_STATE_TRANSIENT_ERROR;
    if (!on && state_ == BG_JOB_STATE_TRANSIENT_ERROR) state_ = BG_JOB_STATE_TRANSFERRING;
  }

  void Forget() {
    {
      Lock hold(jobs_lock);
      for (size_t i = 0; i < jobs.size(); ++i) {
        if (jobs[i] == this) {
          jobs.erase(jobs.begin() + i);
          Release();  // the manager's
          return;
        }
      }
    }
  }

  void Fail(BG_ERROR_CONTEXT context, HRESULT code, const FilePtr& file) {
    {
      Lock hold(lock_);
      state_ = BG_JOB_STATE_ERROR;
      error_context_ = context;
      error_ = code;
      error_file_ = file;
      ++error_count_;
      modified_ = Now();
    }
    Log("job %ls failed: 0x%08lx", name_.c_str(), static_cast<unsigned long>(code));
  }

  static DWORD WINAPI Main(void* self) {
    auto* job = static_cast<Job*>(self);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    job->thread_id_ = GetCurrentThreadId();
    job->Run();
    CoUninitialize();
    job->Release();
    return 0;
  }

  void Run() {
    const ULONGLONG started = GetTickCount64();
    {
      Lock hold(lock_);
      state_ = BG_JOB_STATE_CONNECTING;
    }
    std::vector<FilePtr> files;
    {
      Lock hold(lock_);
      files = files_;
    }
    for (const FilePtr& file : files) {
      if (file->completed) continue;
      {
        Lock hold(lock_);
        state_ = BG_JOB_STATE_TRANSFERRING;
      }
      const HRESULT hr = Download(file);
      if (stop_) {
        Lock hold(lock_);
        running_ = false;
        if (state_ != BG_JOB_STATE_CANCELLED) state_ = BG_JOB_STATE_SUSPENDED;
        return;
      }
      if (FAILED(hr)) {
        Fail(BG_ERROR_CONTEXT_REMOTE_FILE, hr, file);
        {
          Lock hold(lock_);
          running_ = false;
        }
        Notify(this, true);
        return;
      }
    }
    {
      Lock hold(lock_);
      state_ = BG_JOB_STATE_TRANSFERRED;
      completed_ = modified_ = Now();
      running_ = false;
    }
    UINT64 bytes = 0;
    for (const FilePtr& file : files) bytes += static_cast<UINT64>(file->transferred);
    const ULONGLONG ms = GetTickCount64() - started;
    Log("job %ls transferred %llu bytes in %llu ms (%.1f MB/s)", name_.c_str(), static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(ms), ms ? bytes / 1048.576 / ms : 0.0);
    Notify(this, false);
  }

  struct Worker {
    Transfer* transfer;
  };

  static DWORD WINAPI WorkerMain(void* arg) {
    auto* t = static_cast<Transfer*>(arg);
    Connection connection;
    HANDLE out = CreateFileW(t->target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
      t->error = HRESULT_FROM_WIN32(GetLastError());
      InterlockedExchange(&t->failed, 1);
      return 1;
    }
    while (!t->failed && !*t->stop) {
      const LONG index = InterlockedIncrement(&t->next) - 1;
      if (index >= static_cast<LONG>(t->chunks.size())) break;
      const Chunk chunk = t->chunks[index];
      HRESULT hr = E_FAIL;
      for (int attempt = 0; !*t->stop && !t->failed; ++attempt) {
        if (attempt) {
          // Offline, or the server stumbled: keep trying while it has not been too long since a byte moved.
          if (static_cast<ULONGLONG>(GetTickCount64() - t->last_progress) > t->give_up_ms) break;
          t->job->SetTransient(true);
          SleepUnlessStopped(std::min<DWORD>(500u << std::min(attempt, 5), kMaxBackoffMs), t->stop);
          if (*t->stop) break;
        }
        if (!connection.connect && !connection.Open(*t)) {
          hr = HRESULT_FROM_WIN32(GetLastError());
          continue;
        }
        UINT64 written = 0;
        DWORD status = 0;
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(chunk.local);
        SetFilePointerEx(out, at, nullptr, FILE_BEGIN);
        hr = Get(connection, *t, chunk.remote, chunk.length, &status, nullptr,
                 [&](const char* data, DWORD size) -> HRESULT {
                   if (written + size > chunk.length) size = static_cast<DWORD>(chunk.length - written);
                   DWORD did = 0;
                   if (size && !WriteFile(out, data, size, &did, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
                   written += did;
                   InterlockedExchangeAdd64(&t->file->transferred, did);
                   InterlockedExchangeAdd64(&all_done, did);
                   InterlockedExchange64(&t->last_progress, static_cast<LONGLONG>(GetTickCount64()));
                   PublishProgress();
                   return S_OK;
                 });
        if (SUCCEEDED(hr) && status != 206 && !(status == 200 && chunk.remote == 0)) hr = HttpStatusError(status);
        if (SUCCEEDED(hr) && written != chunk.length) hr = HRESULT_FROM_WIN32(ERROR_WINHTTP_CONNECTION_ERROR);
        if (SUCCEEDED(hr)) {
          t->job->SetTransient(false);
          break;
        }
        InterlockedExchangeAdd64(&t->file->transferred, -static_cast<LONG64>(written));
        InterlockedExchangeAdd64(&all_done, -static_cast<LONG64>(written));
        connection.Close();  // a fresh connection for the retry
        if (!Retryable(hr)) break;
      }
      if (FAILED(hr)) {
        if (!*t->stop) {
          t->error = hr;
          InterlockedExchange(&t->failed, 1);
        }
        break;
      }
    }
    CloseHandle(out);
    return 0;
  }

  // Fetches one file: probes it, then lets the workers drain its chunks.
  HRESULT Download(const FilePtr& file) {
    Transfer t;
    t.job = this;
    t.file = file;
    t.stop = &stop_;
    {
      Lock hold(lock_);
      t.headers = headers_;
    }
    if (!CrackUrl(file->remote, t)) return HRESULT_FROM_WIN32(ERROR_WINHTTP_INVALID_URL);
    file->transferred = 0;
    {
      Lock hold(lock_);  // as long as the job's no-progress timeout, but not more than a day
      t.give_up_ms = static_cast<ULONGLONG>(std::min<ULONG>(no_progress_, 86400)) * 1000;
    }
    t.last_progress = static_cast<LONGLONG>(GetTickCount64());

    // Probe: one byte, to learn whether ranges work, how big the file is and
    // where redirects end up (so every worker goes straight there).
    DWORD status = 0;
    UINT64 total = 0;
    std::wstring final_url;
    {
      // Offline at the start is no reason to fail: wait for the network, as the transfers themselves do.
      HRESULT hr = E_FAIL;
      for (int attempt = 0; !stop_; ++attempt) {
        if (attempt) {
          if (static_cast<ULONGLONG>(GetTickCount64() - t.last_progress) > t.give_up_ms) break;
          SetTransient(true);
          SleepUnlessStopped(std::min<DWORD>(500u << std::min(attempt, 5), kMaxBackoffMs), &stop_);
          if (stop_) break;
        }
        Connection probe;
        if (!probe.Open(t)) {
          hr = HRESULT_FROM_WIN32(GetLastError());
          continue;
        }
        Transfer one = t;
        hr = Get(probe, one, 0, 1, &status, &total, [](const char*, DWORD) { return S_OK; }, &final_url);
        if (SUCCEEDED(hr) || !Retryable(hr)) break;
      }
      SetTransient(false);
      if (stop_) return S_OK;
      if (FAILED(hr)) return hr;
    }
    const bool ranged = status == 206;
    if (!final_url.empty() && final_url != file->remote) {
      Transfer moved;
      if (CrackUrl(final_url, moved)) {
        t.host = moved.host;
        t.path = moved.path;
        t.port = moved.port;
        t.secure = moved.secure;
      }
    }
    const bool whole = file->ranges.empty();
    if (!ranged && !whole) return BG_E_INSUFFICIENT_RANGE_SUPPORT;

    // The ranges to fetch, and where each lands: packed back to back in the local file, in the order given.
    std::vector<BG_FILE_RANGE> source;
    if (whole) {
      source.push_back({0, total});
    } else {
      source = file->ranges;
    }
    UINT64 local_size = 0;
    for (const BG_FILE_RANGE& range : source) {
      UINT64 length = range.Length;
      if (length == BG_LENGTH_TO_EOF) length = total > range.InitialOffset ? total - range.InitialOffset : 0;
      if (total && range.InitialOffset + length > total) return BG_E_INVALID_RANGE;
      for (UINT64 at = 0; at < length; at += kChunk) {
        t.chunks.push_back({range.InitialOffset + at, local_size + at, length - at < kChunk ? length - at : kChunk});
      }
      local_size += length;
    }
    file->total = static_cast<LONG64>(local_size);
    total = local_size;
    InterlockedExchangeAdd64(&all_total, static_cast<LONG64>(local_size));
    PublishProgress(true);

    t.target = file->local + L".miraqmgr";  // moved into place by Complete
    HANDLE created = CreateFileW(t.target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size;  // the final size up front, so the workers' writes land in place
    size.QuadPart = static_cast<LONGLONG>(local_size);
    SetFilePointerEx(created, size, nullptr, FILE_BEGIN);
    SetEndOfFile(created);
    CloseHandle(created);
    if (t.chunks.empty()) {  // an empty file
      file->completed = 1;
      return S_OK;
    }

    const int count = static_cast<int>(t.chunks.size() < static_cast<size_t>(kWorkers) ? t.chunks.size() : kWorkers);
    Log("%ls: %llu bytes, %d chunks, %d workers%s", file->remote.c_str(), static_cast<unsigned long long>(total),
        static_cast<int>(t.chunks.size()), count, ranged ? "" : " (no range support)");
    std::vector<HANDLE> threads;
    for (int i = 0; i < count; ++i) {
      if (HANDLE h = CreateThread(nullptr, 0, &Job::WorkerMain, &t, 0, nullptr)) threads.push_back(h);
    }
    if (threads.empty()) return HRESULT_FROM_WIN32(GetLastError());
    for (size_t i = 0; i < threads.size(); i += MAXIMUM_WAIT_OBJECTS) {
      const DWORD n = static_cast<DWORD>(threads.size() - i < MAXIMUM_WAIT_OBJECTS ? threads.size() - i : MAXIMUM_WAIT_OBJECTS);
      WaitForMultipleObjects(n, threads.data() + i, TRUE, INFINITE);
    }
    for (HANDLE h : threads) CloseHandle(h);
    PublishProgress(true);  // the last bytes, which the rate limit may have held back
    if (stop_) return S_OK;
    if (t.failed) return t.error;
    file->completed = 1;
    return S_OK;
  }

  friend void ::Notify(Job*, bool);

  LONG refs_ = 1;
  CRITICAL_SECTION lock_;
  GUID id_;
  std::wstring name_, description_, headers_;
  BG_JOB_TYPE type_;
  BG_JOB_STATE state_ = BG_JOB_STATE_SUSPENDED;
  BG_JOB_PRIORITY priority_ = BG_JOB_PRIORITY_NORMAL;
  ULONG notify_flags_ = BG_NOTIFY_JOB_TRANSFERRED | BG_NOTIFY_JOB_ERROR;
  IUnknown* notify_ = nullptr;
  ULONG retry_delay_ = 600, no_progress_ = 1209600, security_flags_ = 0;
  DWORD acl_flags_ = 0;
  LONG error_count_ = 0;
  std::vector<FilePtr> files_;
  BG_ERROR_CONTEXT error_context_ = BG_ERROR_CONTEXT_NONE;
  HRESULT error_ = S_OK;
  FilePtr error_file_;
  FILETIME created_, modified_, completed_ = {};
  HANDLE thread_ = nullptr;
  DWORD thread_id_ = 0;
  bool running_ = false;
  volatile LONG stop_ = 0;
};

// Tells the client the job it asked about is done or has failed.
static void Notify(Job* job, bool failed) {
  IUnknown* target = nullptr;
  ULONG flags;
  {
    Lock hold(job->lock_);
    target = job->notify_;
    flags = job->notify_flags_;
    if (target) target->AddRef();
  }
  if (!target) return;
  if (IBackgroundCopyCallback* callback = nullptr;
      SUCCEEDED(target->QueryInterface(IID_IBackgroundCopyCallback, reinterpret_cast<void**>(&callback)))) {
    if (!failed && (flags & BG_NOTIFY_JOB_TRANSFERRED)) {
      callback->JobTransferred(static_cast<IBackgroundCopyJob*>(static_cast<IBackgroundCopyJob4*>(job)));
    } else if (failed && (flags & BG_NOTIFY_JOB_ERROR)) {
      IBackgroundCopyError* error = nullptr;
      job->GetError(&error);
      callback->JobError(static_cast<IBackgroundCopyJob*>(static_cast<IBackgroundCopyJob4*>(job)), error);
      if (error) error->Release();
    }
    callback->Release();
  }
  target->Release();
}

// ---------------------------------------------------------------- manager

namespace {

class Manager final : public IBackgroundCopyManager {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IBackgroundCopyManager) {
      *out = static_cast<IBackgroundCopyManager*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE CreateJob(LPCWSTR name, BG_JOB_TYPE type, GUID* id, IBackgroundCopyJob** out) override {
    if (!name || !id || !out) return E_POINTER;
    if (type != BG_JOB_TYPE_DOWNLOAD) return E_NOTIMPL;  // only downloads are served
    auto* job = new Job(name, type);
    job->AddRef();  // the manager's, until Complete or Cancel
    {
      Lock hold(jobs_lock);
      jobs.push_back(job);
    }
    *id = job->Id();
    *out = static_cast<IBackgroundCopyJob4*>(job);  // the caller's reference is the creation one
    Log("created job %ls", name);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetJob(REFGUID id, IBackgroundCopyJob** out) override {
    if (!out) return E_POINTER;
    Lock hold(jobs_lock);
    for (Job* job : jobs) {
      if (job->Id() == id) {
        *out = static_cast<IBackgroundCopyJob4*>(job);
        (*out)->AddRef();
        return S_OK;
      }
    }
    *out = nullptr;
    return BG_E_NOT_FOUND;
  }
  HRESULT STDMETHODCALLTYPE EnumJobs(DWORD, IEnumBackgroundCopyJobs** out) override {
    if (!out) return E_POINTER;
    std::vector<IBackgroundCopyJob*> items;
    {
      Lock hold(jobs_lock);
      for (Job* job : jobs) items.push_back(static_cast<IBackgroundCopyJob4*>(job));
    }
    *out = new JobEnum(items);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetErrorDescription(HRESULT hr, DWORD language, LPWSTR* out) override {
    wchar_t buffer[512] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, language, buffer,
                   ARRAYSIZE(buffer), nullptr);
    if (!*buffer) swprintf(buffer, ARRAYSIZE(buffer), L"Error 0x%08lx", static_cast<unsigned long>(hr));
    return ReturnString(buffer, out);
  }

 private:
  LONG refs_ = 1;
};

class Factory final : public IClassFactory {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *out = static_cast<IClassFactory*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* manager = new Manager;
    const HRESULT hr = manager->QueryInterface(riid, out);
    manager->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
Factory factory;

SERVICE_STATUS_HANDLE status_handle;
DWORD class_cookie;

void SetStatus(DWORD state) {
  SERVICE_STATUS status = {};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  SetServiceStatus(status_handle, &status);
}

DWORD WINAPI Handler(DWORD control, DWORD, void*, void*) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    SetStatus(SERVICE_STOP_PENDING);
    SetEvent(service_stop);
  }
  return NO_ERROR;
}

}  // namespace

extern "C" {

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  status_handle = RegisterServiceCtrlHandlerExW(L"BITS", Handler, nullptr);
  if (!status_handle) return;
  SetStatus(SERVICE_START_PENDING);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  service_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const HRESULT hr = CoRegisterClassObject(CLSID_BackgroundCopyManager, &factory, CLSCTX_LOCAL_SERVER,
                                           REGCLS_MULTIPLEUSE, &class_cookie);
  if (FAILED(hr)) {
    Log("could not register the BITS class: 0x%08lx", static_cast<unsigned long>(hr));
    SetStatus(SERVICE_STOPPED);
    return;
  }
  Log("BITS service running");
  SetStatus(SERVICE_RUNNING);
  WaitForSingleObject(service_stop, INFINITE);
  CoRevokeClassObject(class_cookie);
  CloseHandle(service_stop);
  CoUninitialize();
  SetStatus(SERVICE_STOPPED);
}

// Wine's registration of the BITS classes and proxies is already in the prefix.
HRESULT WINAPI DllRegisterServer() { return S_OK; }
HRESULT WINAPI DllUnregisterServer() { return S_OK; }
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
  if (clsid == CLSID_BackgroundCopyManager) return factory.QueryInterface(riid, out);
  *out = nullptr;
  return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    InitializeCriticalSection(&log_lock);
    InitializeCriticalSection(&jobs_lock);
    log_ready = true;
  }
  return TRUE;
}

}  // extern "C"
