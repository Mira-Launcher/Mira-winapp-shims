/* Stand-in for ole32.dll: forwards every export to Wine's own ole32 (ole32w.dll)
 * and adds CoRegisterActivationFilter, which Wine lacks and Office's
 * Mso30win32client.dll looks for.
 *
 * The filter is only remembered, never called. Nothing in Wine activates objects
 * through it, so Office gets the answer it expects and behaves as before. */
#include <windows.h>

static IUnknown *g_filter;

HRESULT WINAPI CoRegisterActivationFilter(IUnknown *filter) {
  if (filter && g_filter) return CO_E_ALREADYINITIALIZED;
  if (filter) filter->lpVtbl->AddRef(filter);
  if (!filter && g_filter) g_filter->lpVtbl->Release(g_filter);
  g_filter = filter;
  return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
  return TRUE;
}
