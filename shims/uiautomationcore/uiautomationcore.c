/* Stand-in for uiautomationcore.dll: forwards every export to Wine's own copy
 * (uiautomationcorew.dll) and serves CLSID_CUIAutomationRegistrar, which Wine
 * lacks. Office asks for it at startup to register its custom UI Automation
 * properties, events and patterns, and Excel and others fail when it is missing.
 *
 * The registrar hands out ids and remembers them per GUID, so asking twice for
 * the same GUID gives the same id. Nothing is wired into Wine's UI Automation
 * provider: the ids are only bookkeeping for the caller. */
#include <windows.h>
#include <objbase.h>
#include <uiautomationcore.h>

typedef HRESULT(WINAPI *GetClassObjectFn)(REFCLSID, REFIID, void **);

#define MAX_IDS 512
static GUID g_guids[MAX_IDS];
static int g_ids[MAX_IDS];
static int g_count;
static int g_next = 0x10000;
static CRITICAL_SECTION g_lock;

static int IdFor(const GUID *guid) {
  EnterCriticalSection(&g_lock);
  int id = 0;
  for (int i = 0; i < g_count; i++) {
    if (IsEqualGUID(&g_guids[i], guid)) {
      id = g_ids[i];
      break;
    }
  }
  if (!id) {
    id = g_next++;
    if (g_count < MAX_IDS) {
      g_guids[g_count] = *guid;
      g_ids[g_count++] = id;
    }
  }
  LeaveCriticalSection(&g_lock);
  return id;
}

static int FreshId(void) {
  EnterCriticalSection(&g_lock);
  int id = g_next++;
  LeaveCriticalSection(&g_lock);
  return id;
}

/* All three info structs start with the GUID that names the item. */
static HRESULT WINAPI Reg_QueryInterface(IUIAutomationRegistrar *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IUIAutomationRegistrar)) {
    *out = self;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI Reg_AddRef(IUIAutomationRegistrar *self) { (void)self; return 2; }
static ULONG WINAPI Reg_Release(IUIAutomationRegistrar *self) { (void)self; return 1; }

static HRESULT WINAPI Reg_RegisterProperty(IUIAutomationRegistrar *self, const struct UIAutomationPropertyInfo *info, PROPERTYID *id) {
  (void)self;
  if (!info || !id) return E_POINTER;
  *id = IdFor((const GUID *)info);
  return S_OK;
}

static HRESULT WINAPI Reg_RegisterEvent(IUIAutomationRegistrar *self, const struct UIAutomationEventInfo *info, EVENTID *id) {
  (void)self;
  if (!info || !id) return E_POINTER;
  *id = IdFor((const GUID *)info);
  return S_OK;
}

static HRESULT WINAPI Reg_RegisterPattern(IUIAutomationRegistrar *self, const struct UIAutomationPatternInfo *info, PATTERNID *pattern,
                                          PROPERTYID *available, UINT property_count, PROPERTYID *properties, UINT event_count, EVENTID *events) {
  (void)self;
  if (!info || !pattern || !available) return E_POINTER;
  *pattern = IdFor((const GUID *)info);
  *available = FreshId();
  for (UINT i = 0; properties && i < property_count; i++) properties[i] = FreshId();
  for (UINT i = 0; events && i < event_count; i++) events[i] = FreshId();
  return S_OK;
}

static IUIAutomationRegistrarVtbl g_reg_vtbl = {Reg_QueryInterface, Reg_AddRef, Reg_Release, Reg_RegisterProperty, Reg_RegisterEvent, Reg_RegisterPattern};
static IUIAutomationRegistrar g_registrar = {&g_reg_vtbl};

static HRESULT WINAPI Fac_QueryInterface(IClassFactory *self, REFIID riid, void **out) {
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
    *out = self;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG WINAPI Fac_AddRef(IClassFactory *self) { (void)self; return 2; }
static ULONG WINAPI Fac_Release(IClassFactory *self) { (void)self; return 1; }
static HRESULT WINAPI Fac_CreateInstance(IClassFactory *self, IUnknown *outer, REFIID riid, void **out) {
  (void)self;
  if (outer) return CLASS_E_NOAGGREGATION;
  return Reg_QueryInterface(&g_registrar, riid, out);
}
static HRESULT WINAPI Fac_LockServer(IClassFactory *self, BOOL lock) { (void)self; (void)lock; return S_OK; }

static IClassFactoryVtbl g_fac_vtbl = {Fac_QueryInterface, Fac_AddRef, Fac_Release, Fac_CreateInstance, Fac_LockServer};
static IClassFactory g_factory = {&g_fac_vtbl};

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out) {
  if (IsEqualCLSID(clsid, &CLSID_CUIAutomationRegistrar)) return Fac_QueryInterface(&g_factory, riid, out);
  HMODULE real = LoadLibraryW(L"uiautomationcorew.dll");
  GetClassObjectFn fn = real ? (GetClassObjectFn)GetProcAddress(real, "DllGetClassObject") : NULL;
  if (!fn) {
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  return fn(clsid, riid, out);
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_lock);
  }
  return TRUE;
}
