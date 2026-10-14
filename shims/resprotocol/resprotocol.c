/* A res: protocol handler that wraps Wine's (mshtml's CLSID_ResProtocol). Everything goes to Wine's, except a
 * security URL Wine's can't make: for a res: URL whose module it can't find, such as AutoCAD's
 * res:///%25prodname%25btn.xmx/... (the program fills in the name itself), this answers the program's own exe.
 * Without it urlmon puts the URL in no zone and WPF refuses to load the image. */
#define COBJMACROS
#include <windows.h>
#include <urlmon.h>

static const CLSID CLSID_ResProtocol = {0x3050f3bc, 0x98b5, 0x11cf, {0xbb, 0x82, 0x00, 0xaa, 0x00, 0xbd, 0xce, 0x0b}};
/* {6D1E4C5A-3F0B-4E5C-9A7E-4D2B1C8F0A11} */
static const CLSID CLSID_MiraResProtocol = {0x6d1e4c5a, 0x3f0b, 0x4e5c, {0x9a, 0x7e, 0x4d, 0x2b, 0x1c, 0x8f, 0x0a, 0x11}};

// A res: URL with no module, like res:///name/$10/$RCDATA: the only kind Wine fails to map.
static BOOL IsModulelessRes(LPCWSTR url) {
  return url && (url[0] | 0x20) == 'r' && (url[1] | 0x20) == 'e' && (url[2] | 0x20) == 's' && url[3] == ':' &&
         url[4] == '/' && url[5] == '/' && url[6] == '/';
}

static void Copy(WCHAR *to, const WCHAR *from, DWORD count) {
  for (DWORD i = 0; i < count; i++) to[i] = from[i];
}

typedef HRESULT(WINAPI *GetClassObjectFn)(REFCLSID, REFIID, void **);

static IClassFactory *WineFactory(void) {
  static IClassFactory *factory;
  if (!factory) {
    HMODULE mshtml = LoadLibraryW(L"mshtml.dll");
    GetClassObjectFn get = mshtml ? (GetClassObjectFn)GetProcAddress(mshtml, "DllGetClassObject") : NULL;
    if (get) get(&CLSID_ResProtocol, &IID_IClassFactory, (void **)&factory);
  }
  return factory;
}

static IInternetProtocolInfo *WineInfo(void) {
  static IInternetProtocolInfo *info;
  IClassFactory *factory = WineFactory();
  if (!info && factory && FAILED(IClassFactory_QueryInterface(factory, &IID_IInternetProtocolInfo, (void **)&info))) {
    IClassFactory_CreateInstance(factory, NULL, &IID_IInternetProtocolInfo, (void **)&info);
  }
  return info;
}

/* One static object serves IClassFactory and IInternetProtocolInfo. */
static IClassFactory g_factory;
static IInternetProtocolInfo g_info;

static HRESULT QueryBoth(REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
    *out = &g_factory;
    return S_OK;
  }
  if (IsEqualIID(riid, &IID_IInternetProtocolInfo)) {
    *out = &g_info;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI AddRef(void *self) { (void)self; return 2; }
static ULONG WINAPI Release(void *self) { (void)self; return 1; }

static HRESULT WINAPI Factory_QueryInterface(IClassFactory *self, REFIID riid, void **out) {
  (void)self;
  return QueryBoth(riid, out);
}
static HRESULT WINAPI Factory_CreateInstance(IClassFactory *self, IUnknown *outer, REFIID riid, void **out) {
  (void)self;
  if (!outer && IsEqualIID(riid, &IID_IInternetProtocolInfo)) return QueryBoth(riid, out);
  IClassFactory *factory = WineFactory();
  if (!factory) return CLASS_E_CLASSNOTAVAILABLE;
  return IClassFactory_CreateInstance(factory, outer, riid, out);
}
static HRESULT WINAPI Factory_LockServer(IClassFactory *self, BOOL lock) {
  (void)self;
  (void)lock;
  return S_OK;
}
static IClassFactoryVtbl g_factory_vtbl = {Factory_QueryInterface, (void *)AddRef, (void *)Release,
                                           Factory_CreateInstance, Factory_LockServer};

static HRESULT WINAPI Info_QueryInterface(IInternetProtocolInfo *self, REFIID riid, void **out) {
  (void)self;
  return QueryBoth(riid, out);
}
static HRESULT WINAPI Info_ParseUrl(IInternetProtocolInfo *self, LPCWSTR url, PARSEACTION action, DWORD flags,
                                    LPWSTR result, DWORD size, DWORD *needed, DWORD reserved) {
  (void)self;
  IInternetProtocolInfo *info = WineInfo();
  HRESULT hr = info ? IInternetProtocolInfo_ParseUrl(info, url, action, flags, result, size, needed, reserved)
                    : INET_E_DEFAULT_ACTION;
  if (action != PARSE_SECURITY_URL || SUCCEEDED(hr) || !IsModulelessRes(url)) return hr;
  WCHAR exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
  if (!len || len >= MAX_PATH) return hr;
  DWORD total = 7 + len + 1;
  if (needed) *needed = total;
  if (total > size) return S_FALSE;
  Copy(result, L"file://", 7);
  Copy(result + 7, exe, len + 1);
  return S_OK;
}
static HRESULT WINAPI Info_CombineUrl(IInternetProtocolInfo *self, LPCWSTR base, LPCWSTR relative, DWORD flags,
                                      LPWSTR result, DWORD size, DWORD *needed, DWORD reserved) {
  (void)self;
  IInternetProtocolInfo *info = WineInfo();
  return info ? IInternetProtocolInfo_CombineUrl(info, base, relative, flags, result, size, needed, reserved)
              : INET_E_DEFAULT_ACTION;
}
static HRESULT WINAPI Info_CompareUrl(IInternetProtocolInfo *self, LPCWSTR url1, LPCWSTR url2, DWORD flags) {
  (void)self;
  IInternetProtocolInfo *info = WineInfo();
  return info ? IInternetProtocolInfo_CompareUrl(info, url1, url2, flags) : INET_E_DEFAULT_ACTION;
}
static HRESULT WINAPI Info_QueryInfo(IInternetProtocolInfo *self, LPCWSTR url, QUERYOPTION option, DWORD flags,
                                     LPVOID buffer, DWORD size, DWORD *needed, DWORD reserved) {
  (void)self;
  IInternetProtocolInfo *info = WineInfo();
  return info ? IInternetProtocolInfo_QueryInfo(info, url, option, flags, buffer, size, needed, reserved)
              : INET_E_DEFAULT_ACTION;
}
static IInternetProtocolInfoVtbl g_info_vtbl = {Info_QueryInterface, (void *)AddRef,   (void *)Release,
                                                Info_ParseUrl,       Info_CombineUrl, Info_CompareUrl,
                                                Info_QueryInfo};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
  if (!IsEqualCLSID(clsid, &CLSID_MiraResProtocol)) return CLASS_E_CLASSNOTAVAILABLE;
  return QueryBoth(riid, out);
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    g_factory.lpVtbl = &g_factory_vtbl;
    g_info.lpVtbl = &g_info_vtbl;
  }
  return TRUE;
}
