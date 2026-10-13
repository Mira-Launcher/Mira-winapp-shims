/* Stand-in for bcryptprimitives.dll: forwards everything to Wine's own copy
 * (bcryptprimitivesw.dll) and lets Go services start under Wine.
 *
 * A Go program decides whether it runs as a Windows service with
 * svc.IsWindowsService: its parent must be services.exe in session 0. Wine runs
 * every process in session 1, so a Go service takes itself for a console
 * program, never connects to the service manager, and StartService fails with
 * 1053 (an installer then rolls back with 1627: Autodesk's CER service).
 *
 * Every Go program since 1.22 loads this DLL at startup (for ProcessPrng). When
 * it loads, this points the process's GetProcAddress imports at a version that
 * hands out its own NtQuerySystemInformation, which reports services.exe in
 * session 0, as on Windows. Go looks both up through GetProcAddress. */
#include <windows.h>
#include <winternl.h>

typedef NTSTATUS(NTAPI *QuerySystemFn)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
typedef FARPROC(WINAPI *GetProcFn)(HMODULE, LPCSTR);

static QuerySystemFn real_query;
static GetProcFn real_get_proc;

static NTSTATUS NTAPI QuerySystem(SYSTEM_INFORMATION_CLASS cls, PVOID info, ULONG size, PULONG returned) {
  NTSTATUS status = real_query(cls, info, size, returned);
  if (status != 0 || cls != SystemProcessInformation || !info) return status;
  for (SYSTEM_PROCESS_INFORMATION *p = info;; p = (SYSTEM_PROCESS_INFORMATION *)((BYTE *)p + p->NextEntryOffset)) {
    const UNICODE_STRING *name = &p->ImageName;
    if (name->Buffer && CompareStringOrdinal(name->Buffer, name->Length / 2, L"services.exe", -1, TRUE) == CSTR_EQUAL) {
      p->SessionId = 0;
    }
    if (!p->NextEntryOffset) break;
  }
  return status;
}

static FARPROC WINAPI GetProc(HMODULE module, LPCSTR name) {
  if (!IS_INTRESOURCE(name) && lstrcmpA(name, "NtQuerySystemInformation") == 0 &&
      module == GetModuleHandleA("ntdll.dll")) {
    return (FARPROC)(void *)QuerySystem;
  }
  return real_get_proc(module, name);
}

/* Points the main program's GetProcAddress imports at GetProc. */
static void PatchMain(void) {
  BYTE *base = (BYTE *)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return;
  for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
    if (lstrcmpiA((const char *)(base + imp->Name), "kernel32.dll") != 0) continue;
    IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
      DWORD old;
      if (lstrcmpA(name, "GetProcAddress") != 0 ||
          !VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
        continue;
      }
      slots->u1.Function = (ULONG_PTR)GetProc;
      VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
    }
  }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    real_get_proc = (GetProcFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
    real_query = (QuerySystemFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    if (real_get_proc && real_query) {
      PatchMain();
      /* The patched import points here, so this DLL must never unload. */
      HMODULE self;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)instance, &self);
    }
  }
  return TRUE;
}
