/* Stand-in for ole32.dll: forwards every export to Wine's own ole32 (ole32w.dll)
 * and adds CoRegisterActivationFilter, which Wine lacks and Office's
 * Mso30win32client.dll looks for.
 *
 * importfix.c also runs from here: it keeps Wine's stub for SetFileShortNameW from
 * aborting Office (see that file).
 *
 * The filter is only remembered, never called. Nothing in Wine activates objects
 * through it, so Office gets the answer it expects and behaves as before. */
#include <windows.h>

void ImportFixInstall(void);

static IUnknown *g_filter;

HRESULT WINAPI CoRegisterActivationFilter(IUnknown *filter) {
  if (filter && g_filter) return CO_E_ALREADYINITIALIZED;
  if (filter) filter->lpVtbl->AddRef(filter);
  if (!filter && g_filter) g_filter->lpVtbl->Release(g_filter);
  g_filter = filter;
  return S_OK;
}

typedef HRESULT(WINAPI *OpenOnLockBytesFn)(ILockBytes *, IStorage *, DWORD, SNB, DWORD, IStorage **);

// Wine flushes the ILockBytes while rejecting a file that is not a compound file,
// and Office's OneDrive stream closes itself on that flush. Reject such files first.
HRESULT WINAPI OpenOnLockBytes(ILockBytes *bytes, IStorage *prio, DWORD mode, SNB exclude, DWORD reserved,
                               IStorage **out) {
  static const BYTE kSig[8] = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
  static const BYTE kOldSig[8] = {0x0e, 0x11, 0xfc, 0x0d, 0xd0, 0xcf, 0x11, 0x0e};
  BYTE head[8];
  ULONG got = 0;
  ULARGE_INTEGER zero = {{0, 0}};
  if (bytes && out && SUCCEEDED(bytes->lpVtbl->ReadAt(bytes, zero, head, sizeof(head), &got))) {
    BOOL sig = got == sizeof(head), old = sig;
    for (ULONG i = 0; i < got; i++) {
      sig &= head[i] == kSig[i];
      old &= head[i] == kOldSig[i];
    }
    if (!sig && !old) {
      *out = NULL;
      return STG_E_FILEALREADYEXISTS;
    }
  }
  HMODULE real = GetModuleHandleA("ole32w.dll");
  OpenOnLockBytesFn fn = real ? (OpenOnLockBytesFn)(void *)GetProcAddress(real, "StgOpenStorageOnILockBytes") : NULL;
  return fn ? fn(bytes, prio, mode, exclude, reserved, out) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    ImportFixInstall();
  }
  return TRUE;
}
