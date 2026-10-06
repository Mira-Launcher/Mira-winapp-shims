/* Points a module's imports of functions Wine lacks at our own versions.
 *
 * Wine fills an import it cannot resolve with a stub that aborts the process
 * when called, and a delay-loaded import it cannot resolve raises an exception
 * on first call. Office imports a few functions Wine does not have:
 * - KERNEL32!SetFileShortNameW (MSO.DLL, at startup)
 * - USER32!CalculatePopupWindowPosition (Mso30win32client.dll, delay-loaded,
 *   when the sign-in window opens)
 * Some exist but reject options Office sets or reads during sign-in, failing
 * with 12009 (invalid option), which Office reports as sign-in error 53u4r:
 * - WINHTTP!WinHttpQueryOption: WINHTTP_OPTION_AUTOLOGON_POLICY
 * - WINHTTP!WinHttpSetOption: WINHTTP_OPTION_IPV6_FAST_FALLBACK
 * - WININET!InternetSetOptionW/A: INTERNET_OPTION_LISTEN_TIMEOUT
 * The set options are tuning hints, so accepting and ignoring them is safe.
 * This replaces those import slots, normal and delay-load, with our own
 * functions. It runs when this DLL loads and again for every DLL loaded after
 * it. */
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

typedef struct {
  DWORD attributes;
  DWORD dll_name;
  DWORD module_handle;
  DWORD address_table;
  DWORD name_table;
  DWORD bound_table;
  DWORD unload_table;
  DWORD timestamp;
} DelayDescriptor;

static BOOL WINAPI FailShortName(HANDLE file, LPCWSTR short_name) {
  (void)file;
  (void)short_name;
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

/* Places a popup of `size` at `anchor` using the TPM_* alignment flags, keeps
 * it on the anchor's monitor and moves it off `exclude` when they overlap. */
static BOOL WINAPI PopupPosition(const POINT *anchor, const SIZE *size, UINT flags, RECT *exclude, RECT *out) {
  if (!anchor || !size || !out) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  LONG x = anchor->x, y = anchor->y;
  if (flags & TPM_RIGHTALIGN) x -= size->cx;
  else if (flags & TPM_CENTERALIGN) x -= size->cx / 2;
  if (flags & TPM_BOTTOMALIGN) y -= size->cy;
  else if (flags & TPM_VCENTERALIGN) y -= size->cy / 2;

  MONITORINFO info = {0};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromPoint(*anchor, MONITOR_DEFAULTTONEAREST), &info);
  RECT area = (flags & TPM_WORKAREA) ? info.rcWork : info.rcMonitor;

  RECT r = {x, y, x + size->cx, y + size->cy}, overlap;
  if (exclude && IntersectRect(&overlap, &r, exclude)) {
    if (flags & TPM_VERTICAL) {
      y = exclude->bottom;
      if (y + size->cy > area.bottom) y = exclude->top - size->cy;
    } else {
      x = exclude->right;
      if (x + size->cx > area.right) x = exclude->left - size->cx;
    }
  }
  if (x + size->cx > area.right) x = area.right - size->cx;
  if (y + size->cy > area.bottom) y = area.bottom - size->cy;
  if (x < area.left) x = area.left;
  if (y < area.top) y = area.top;
  SetRect(out, x, y, x + size->cx, y + size->cy);
  return TRUE;
}

static FARPROC Real(const char *dll, const char *name) {
  HMODULE mod = GetModuleHandleA(dll);
  return mod ? GetProcAddress(mod, name) : NULL;
}

typedef BOOL(WINAPI *QueryOptionFn)(void *handle, DWORD option, void *buffer, DWORD *size);

/* Answers WINHTTP_OPTION_AUTOLOGON_POLICY with the Windows default (medium);
 * every other option goes to Wine's winhttp. */
static BOOL WINAPI QueryOption(void *handle, DWORD option, void *buffer, DWORD *size) {
  if (option == 77 /* WINHTTP_OPTION_AUTOLOGON_POLICY */ && size) {
    if (!buffer || *size < sizeof(DWORD)) {
      *size = sizeof(DWORD);
      SetLastError(ERROR_INSUFFICIENT_BUFFER);
      return FALSE;
    }
    *(DWORD *)buffer = 0; /* WINHTTP_AUTOLOGON_SECURITY_LEVEL_MEDIUM */
    *size = sizeof(DWORD);
    return TRUE;
  }
  QueryOptionFn real = (QueryOptionFn)(void *)Real("winhttp.dll", "WinHttpQueryOption");
  if (!real) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return FALSE;
  }
  return real(handle, option, buffer, size);
}

typedef BOOL(WINAPI *SetOptionFn)(void *handle, DWORD option, void *buffer, DWORD size);

static BOOL WINAPI HttpSetOption(void *handle, DWORD option, void *buffer, DWORD size) {
  if (option == 140 /* WINHTTP_OPTION_IPV6_FAST_FALLBACK */) return TRUE;
  SetOptionFn real = (SetOptionFn)(void *)Real("winhttp.dll", "WinHttpSetOption");
  return real ? real(handle, option, buffer, size) : FALSE;
}

static BOOL WINAPI InetSetOptionW(void *handle, DWORD option, void *buffer, DWORD size) {
  if (option == 11 /* INTERNET_OPTION_LISTEN_TIMEOUT */) return TRUE;
  SetOptionFn real = (SetOptionFn)(void *)Real("wininet.dll", "InternetSetOptionW");
  return real ? real(handle, option, buffer, size) : FALSE;
}

static BOOL WINAPI InetSetOptionA(void *handle, DWORD option, void *buffer, DWORD size) {
  if (option == 11 /* INTERNET_OPTION_LISTEN_TIMEOUT */) return TRUE;
  SetOptionFn real = (SetOptionFn)(void *)Real("wininet.dll", "InternetSetOptionA");
  return real ? real(handle, option, buffer, size) : FALSE;
}

static const struct {
  const char *dll;
  const char *name;
  void *fn;
} kFixes[] = {
    {"KERNEL32.dll", "SetFileShortNameW", (void *)FailShortName},
    {"USER32.dll", "CalculatePopupWindowPosition", (void *)PopupPosition},
    {"WINHTTP.dll", "WinHttpQueryOption", (void *)QueryOption},
    {"WINHTTP.dll", "WinHttpSetOption", (void *)HttpSetOption},
    {"WININET.dll", "InternetSetOptionW", (void *)InetSetOptionW},
    {"WININET.dll", "InternetSetOptionA", (void *)InetSetOptionA},
};

static void PatchSlots(BYTE *base, const char *dll, IMAGE_THUNK_DATA *names, IMAGE_THUNK_DATA *slots) {
  for (; names->u1.AddressOfData; names++, slots++) {
    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
    const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
    for (size_t i = 0; i < sizeof(kFixes) / sizeof(kFixes[0]); i++) {
      if (lstrcmpiA(dll, kFixes[i].dll) != 0 || lstrcmpA(name, kFixes[i].name) != 0) continue;
      DWORD old;
      if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
      slots->u1.Function = (ULONG_PTR)kFixes[i].fn;
      VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
    }
  }
}

static void PatchModule(HMODULE mod) {
  BYTE *base = (BYTE *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;

  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (dir.VirtualAddress) {
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
      DWORD names = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
      PatchSlots(base, (const char *)(base + imp->Name), (IMAGE_THUNK_DATA *)(base + names),
                 (IMAGE_THUNK_DATA *)(base + imp->FirstThunk));
    }
  }

  dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  if (dir.VirtualAddress) {
    for (DelayDescriptor *d = (DelayDescriptor *)(base + dir.VirtualAddress); d->dll_name; d++) {
      if (!(d->attributes & 1)) continue; /* only RVA-based descriptors */
      PatchSlots(base, (const char *)(base + d->dll_name), (IMAGE_THUNK_DATA *)(base + d->name_table),
                 (IMAGE_THUNK_DATA *)(base + d->address_table));
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
