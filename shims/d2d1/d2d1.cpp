/* Stand-in for d2d1.dll: forwards everything to Wine's own Direct2D (d2d1w.dll)
 * and makes ID2D1DeviceContext5::CreateSvgDocument succeed.
 *
 * Wine's CreateSvgDocument is a stub that fails. Excel's ribbon asks for an SVG
 * document while it starts and crashes when it cannot get one. The document made
 * here is empty: it accepts the viewport size and nothing else, so SVG icons are
 * not drawn, but the app keeps running.
 *
 * Better still, the context is made to deny it supports ID2D1DeviceContext5, the
 * interface that carries SVG, so Office never asks.
 *
 * Wine's Direct2D objects share their method tables, so the fix swaps table
 * entries in place. It starts from the three exported functions that create
 * objects, follows factory -> device -> device context, and replaces
 * CreateSvgDocument on the context. */
#include <windows.h>
#include <d2d1_3.h>
#include <d2d1svg.h>

typedef HRESULT(WINAPI *CreateFactoryFn)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIDevice *, const D2D1_CREATION_PROPERTIES *, ID2D1Device **);
typedef HRESULT(WINAPI *CreateDeviceContextFn)(IDXGISurface *, const D2D1_CREATION_PROPERTIES *, ID2D1DeviceContext **);

typedef HRESULT(STDMETHODCALLTYPE *CreateDeviceFnPtr)(void *, IDXGIDevice *, void **);
typedef HRESULT(STDMETHODCALLTYPE *CreateContextFnPtr)(void *, D2D1_DEVICE_CONTEXT_OPTIONS, void **);
typedef HRESULT(STDMETHODCALLTYPE *CreateSvgFnPtr)(void *, IStream *, D2D1_SIZE_F, ID2D1SvgDocument **);

/* Calls are logged to %D2D1_SHIM_LOG%, default C:\\d2d1-shim.log. */
static void Log(const char *what) {
  char path[MAX_PATH];
  if (!GetEnvironmentVariableA("D2D1_SHIM_LOG", path, sizeof path)) lstrcpyA(path, "C:\\d2d1-shim.log");
  HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char line[160];
  int n = wsprintfA(line, "%lu %s\r\n", GetCurrentProcessId(), what);
  DWORD written;
  WriteFile(f, line, n, &written, nullptr);
  CloseHandle(f);
}

/* Empty SVG document. */
class EmptySvgDocument final : public ID2D1SvgDocument {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1Resource) || riid == __uuidof(ID2D1SvgDocument)) {
      *out = static_cast<ID2D1SvgDocument *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  void STDMETHODCALLTYPE GetFactory(ID2D1Factory **factory) const override { Log("svg::GetFactory"); *factory = nullptr; }
  HRESULT STDMETHODCALLTYPE SetViewportSize(D2D1_SIZE_F size) override { Log("svg::SetViewportSize");
    size_ = size;
    return S_OK;
  }
  D2D1_SIZE_F *STDMETHODCALLTYPE GetViewportSize(D2D1_SIZE_F *ret) const override { Log("svg::GetViewportSize");
    *ret = size_;
    return ret;
  }
  HRESULT STDMETHODCALLTYPE SetRoot(ID2D1SvgElement *) override { Log("svg::SetRoot"); return E_NOTIMPL; }
  void STDMETHODCALLTYPE GetRoot(ID2D1SvgElement **root) override { Log("svg::GetRoot"); *root = nullptr; }
  HRESULT STDMETHODCALLTYPE FindElementById(PCWSTR, ID2D1SvgElement **element) override { Log("svg::FindElementById");
    *element = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Serialize(IStream *, ID2D1SvgElement *) override { Log("svg::Serialize"); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Deserialize(IStream *, ID2D1SvgElement **element) override { Log("svg::Deserialize");
    *element = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreatePaint(D2D1_SVG_PAINT_TYPE, const D2D1_COLOR_F *, PCWSTR, ID2D1SvgPaint **paint) override { Log("svg::CreatePaint");
    *paint = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateStrokeDashArray(const D2D1_SVG_LENGTH *, UINT32, ID2D1SvgStrokeDashArray **array) override { Log("svg::CreateStrokeDashArray");
    *array = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreatePointCollection(const D2D1_POINT_2F *, UINT32, ID2D1SvgPointCollection **points) override { Log("svg::CreatePointCollection");
    *points = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreatePathData(const FLOAT *, UINT32, const D2D1_SVG_PATH_COMMAND *, UINT32, ID2D1SvgPathData **data) override { Log("svg::CreatePathData");
    *data = nullptr;
    return E_NOTIMPL;
  }

 private:
  LONG refs_ = 1;
  D2D1_SIZE_F size_ = {0, 0};
};

static HRESULT STDMETHODCALLTYPE EmptyCreateSvgDocument(void *, IStream *, D2D1_SIZE_F size, ID2D1SvgDocument **doc) {
  Log("CreateSvgDocument");
  if (!doc) return E_POINTER;
  auto *made = new EmptySvgDocument();
  made->SetViewportSize(size);
  *doc = made;
  return S_OK;
}

/* Table patching. */
static void **SwapSlot(void **table, size_t index, void *replacement, void **original) {
  if (table[index] == replacement) return nullptr;
  DWORD old;
  if (!VirtualProtect(&table[index], sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
  if (original) *original = table[index];
  table[index] = replacement;
  VirtualProtect(&table[index], sizeof(void *), old, &old);
  return &table[index];
}

/* Index of a virtual method, from a pointer to it (Itanium C++ ABI layout). */
template <typename Fn>
static size_t SlotOf(Fn method) {
  union {
    Fn fn;
    struct {
      size_t ptr;
      ptrdiff_t adj;
    } raw;
  } u = {method};
  return (u.raw.ptr - 1) / sizeof(void *);
}

/* Hide the SVG-capable interfaces, so Office draws its icons without SVG. */
static HRESULT(STDMETHODCALLTYPE *g_orig_query)(void *, REFIID, void **);

static HRESULT STDMETHODCALLTYPE ContextQueryInterface(void *self, REFIID riid, void **out) {
  if (riid == __uuidof(ID2D1DeviceContext5) || riid == __uuidof(ID2D1DeviceContext6)) {
    Log("QueryInterface hidden: ID2D1DeviceContext5/6");
    if (out) *out = nullptr;
    return E_NOINTERFACE;
  }
  return g_orig_query(self, riid, out);
}

static void PatchContext(IUnknown *context) {
  if (!context) return;
  void **table = *reinterpret_cast<void ***>(context);
  SwapSlot(table, 0, reinterpret_cast<void *>(ContextQueryInterface), reinterpret_cast<void **>(&g_orig_query));
  SwapSlot(table, SlotOf(&ID2D1DeviceContext5::CreateSvgDocument), reinterpret_cast<void *>(EmptyCreateSvgDocument), nullptr);
}

/* One hook per overload of Device::CreateDeviceContext; N picks the slot and
 * the saved original. */
static const int kLevels = 7;
static CreateContextFnPtr g_orig_context[kLevels];
static CreateDeviceFnPtr g_orig_device[kLevels];

template <int N>
struct ContextHook {
  static HRESULT STDMETHODCALLTYPE Call(void *self, D2D1_DEVICE_CONTEXT_OPTIONS options, void **out) {
    HRESULT hr = g_orig_context[N](self, options, out);
    if (SUCCEEDED(hr) && out && *out) PatchContext(static_cast<IUnknown *>(*out));
    return hr;
  }
};

static void PatchDevice(IUnknown *device);

template <int N>
struct DeviceHook {
  static HRESULT STDMETHODCALLTYPE Call(void *self, IDXGIDevice *dxgi, void **out) {
    HRESULT hr = g_orig_device[N](self, dxgi, out);
    if (SUCCEEDED(hr) && out && *out) PatchDevice(static_cast<IUnknown *>(*out));
    return hr;
  }
};

#define HOOK_CONTEXT(N, DEVICE, CONTEXT)                                                                                          \
  {                                                                                                                               \
    auto pm = static_cast<HRESULT (STDMETHODCALLTYPE DEVICE::*)(D2D1_DEVICE_CONTEXT_OPTIONS, CONTEXT **)>(&DEVICE::CreateDeviceContext); \
    SwapSlot(table, SlotOf(pm), reinterpret_cast<void *>(ContextHook<N>::Call), reinterpret_cast<void **>(&g_orig_context[N]));  \
  }

static void PatchDevice(IUnknown *device) {
  if (!device) return;
  void **table = *reinterpret_cast<void ***>(device);
  if (g_orig_context[0] && table[SlotOf(static_cast<HRESULT (STDMETHODCALLTYPE ID2D1Device::*)(D2D1_DEVICE_CONTEXT_OPTIONS, ID2D1DeviceContext **)>(&ID2D1Device::CreateDeviceContext))] ==
                               reinterpret_cast<void *>(ContextHook<0>::Call))
    return;
  HOOK_CONTEXT(0, ID2D1Device, ID2D1DeviceContext)
  HOOK_CONTEXT(1, ID2D1Device1, ID2D1DeviceContext1)
  HOOK_CONTEXT(2, ID2D1Device2, ID2D1DeviceContext2)
  HOOK_CONTEXT(3, ID2D1Device3, ID2D1DeviceContext3)
  HOOK_CONTEXT(4, ID2D1Device4, ID2D1DeviceContext4)
  HOOK_CONTEXT(5, ID2D1Device5, ID2D1DeviceContext5)
  HOOK_CONTEXT(6, ID2D1Device6, ID2D1DeviceContext6)
}

#define HOOK_DEVICE(N, FACTORY, DEVICE)                                                                                           \
  {                                                                                                                               \
    auto pm = static_cast<HRESULT (STDMETHODCALLTYPE FACTORY::*)(IDXGIDevice *, DEVICE **)>(&FACTORY::CreateDevice);               \
    SwapSlot(table, SlotOf(pm), reinterpret_cast<void *>(DeviceHook<N>::Call), reinterpret_cast<void **>(&g_orig_device[N]));      \
  }

static void PatchFactory(IUnknown *factory) {
  if (!factory) return;
  void **table = *reinterpret_cast<void ***>(factory);
  HOOK_DEVICE(0, ID2D1Factory1, ID2D1Device)
  HOOK_DEVICE(1, ID2D1Factory2, ID2D1Device1)
  HOOK_DEVICE(2, ID2D1Factory3, ID2D1Device2)
  HOOK_DEVICE(3, ID2D1Factory4, ID2D1Device3)
  HOOK_DEVICE(4, ID2D1Factory5, ID2D1Device4)
  HOOK_DEVICE(5, ID2D1Factory6, ID2D1Device5)
  HOOK_DEVICE(6, ID2D1Factory7, ID2D1Device6)
}

static FARPROC RealExport(const char *name) {
  static HMODULE real;
  if (!real) real = LoadLibraryW(L"d2d1w.dll");
  return real ? GetProcAddress(real, name) : nullptr;
}

extern "C" {

HRESULT WINAPI D2D1CreateFactory(D2D1_FACTORY_TYPE type, REFIID riid, const D2D1_FACTORY_OPTIONS *options, void **out) {
  auto fn = reinterpret_cast<CreateFactoryFn>(RealExport("D2D1CreateFactory"));
  if (!fn) return E_FAIL;
  HRESULT hr = fn(type, riid, options, out);
  if (SUCCEEDED(hr) && out && *out) PatchFactory(static_cast<IUnknown *>(*out));
  return hr;
}

HRESULT WINAPI D2D1CreateDevice(IDXGIDevice *dxgi, const D2D1_CREATION_PROPERTIES *props, ID2D1Device **out) {
  auto fn = reinterpret_cast<CreateDeviceFn>(RealExport("D2D1CreateDevice"));
  if (!fn) return E_FAIL;
  HRESULT hr = fn(dxgi, props, out);
  if (SUCCEEDED(hr) && out && *out) PatchDevice(*out);
  return hr;
}

HRESULT WINAPI D2D1CreateDeviceContext(IDXGISurface *surface, const D2D1_CREATION_PROPERTIES *props, ID2D1DeviceContext **out) {
  auto fn = reinterpret_cast<CreateDeviceContextFn>(RealExport("D2D1CreateDeviceContext"));
  if (!fn) return E_FAIL;
  HRESULT hr = fn(surface, props, out);
  if (SUCCEEDED(hr) && out && *out) PatchContext(*out);
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
  return TRUE;
}

}  // extern "C"
