/* Points a module's import of a function Wine lacks at our own version.
 *
 * Wine fills an import it cannot resolve with a stub that aborts the process
 * when called. Office's MSO.DLL imports KERNEL32!SetFileShortNameW, which Wine
 * does not have, so Word and the other apps die on startup. This replaces the
 * import slot with a function that fails cleanly instead. It runs when this
 * DLL loads and again for every DLL loaded after it. */
#include <windows.h>
#include <winternl.h>

typedef VOID(NTAPI *LdrDllNotification)(ULONG reason, const void *data, void *context);
NTSTATUS NTAPI LdrRegisterDllNotification(ULONG flags, LdrDllNotification fn, void *context, void **cookie);

typedef struct {
  ULONG flags;
  const UNICODE_STRING *full_name;
  const UNICODE_STRING *base_name;
  void *base;
  ULONG size;
} LoadedData;

static BOOL WINAPI FailShortName(HANDLE file, LPCWSTR short_name) {
  (void)file;
  (void)short_name;
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

static void PatchModule(HMODULE mod) {
  BYTE *base = (BYTE *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return;
  for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
    if (lstrcmpiA((const char *)(base + imp->Name), "KERNEL32.dll") != 0) continue;
    IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      IMAGE_IMPORT_BY_NAME *by_name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
      if (lstrcmpA((const char *)by_name->Name, "SetFileShortNameW") != 0) continue;
      DWORD old;
      if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
      slots->u1.Function = (ULONG_PTR)FailShortName;
      VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
    }
  }
}

static VOID NTAPI OnLoad(ULONG reason, const void *data, void *context) {
  (void)context;
  if (reason == 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */) PatchModule((HMODULE)((const LoadedData *)data)->base);
}

void ImportFixInstall(void) {
  PEB *peb = NtCurrentTeb()->ProcessEnvironmentBlock;
  LIST_ENTRY *head = &peb->Ldr->InMemoryOrderModuleList;
  for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *entry = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
    if (entry->DllBase) PatchModule((HMODULE)entry->DllBase);
  }
  void *cookie;
  LdrRegisterDllNotification(0, OnLoad, NULL, &cookie);
}
