/* Loads binary registry hives (regf files), which Wine can't.
 *
 * Autodesk's installer (adixhandler.dll) opens each package's registry.dat
 * with RegLoadAppKeyW and copies it into the registry. Wine's RegLoadAppKeyW is
 * a stub that hands back a dummy key (0xdeadbeef), so every package fails with
 * "invalid handle"; its RegLoadKeyW only reads Wine's own text format. This
 * repoints both imports (in every module that has them) to a loader that reads
 * the hive and writes its keys and values: for RegLoadKeyW under root\name,
 * which RegUnLoadKeyW deletes again; for RegLoadAppKeyW under a private key of
 * this process (HKCU\Software\Mira\AppKeys\<pid>-<n>), deleted when this
 * DLL unloads. Anything that isn't a regf file goes to Wine. */
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

typedef LONG(WINAPI *LoadKeyFn)(HKEY, LPCWSTR, LPCWSTR);
typedef LONG(WINAPI *UnLoadKeyFn)(HKEY, LPCWSTR);

/* A mapped hive. Cell offsets count from the first hbin, 4096 bytes in. */
typedef struct {
  const BYTE *data;
  DWORD size;
} Hive;

static const BYTE *Cell(const Hive *h, DWORD offset, DWORD need) {
  DWORD at = 4096 + offset + 4; /* past the cell's size field */
  if (offset == 0xffffffff || at < 4096 || at > h->size || h->size - at < need) return NULL;
  return h->data + at;
}

/* No C runtime here, so no memcpy. */
static void Copy(BYTE *to, const BYTE *from, DWORD n) {
  while (n--) *to++ = *from++;
}

static WORD U16(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD U32(const BYTE *p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

/* A key or value name: Latin-1 when `ascii`, else UTF-16. */
static void Name(const BYTE *p, WORD length, BOOL ascii, WCHAR *out, int max) {
  int n = 0;
  if (ascii) {
    for (; n < length && n < max - 1; n++) out[n] = p[n];
  } else {
    for (; n < length / 2 && n < max - 1; n++) out[n] = U16(p + 2 * n);
  }
  out[n] = 0;
}

/* A value's data, gathered from a big-data ("db") record when it spans cells. */
static BYTE *ValueData(const Hive *h, const BYTE *vk, DWORD *length) {
  DWORD size = U32(vk + 4);
  DWORD offset = U32(vk + 8);
  if (size & 0x80000000) { /* up to 4 bytes, kept in the offset field */
    *length = size & 0x7fffffff;
    if (*length > 4) return NULL;
    BYTE *copy = HeapAlloc(GetProcessHeap(), 0, 4);
    if (copy) Copy(copy, vk + 8, *length);
    return copy;
  }
  *length = size;
  BYTE *copy = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
  if (!copy) return NULL;
  const BYTE *cell = Cell(h, offset, 4);
  if (size > 16344 && cell && cell[0] == 'd' && cell[1] == 'b') {
    WORD segments = U16(cell + 2);
    const BYTE *list = Cell(h, U32(cell + 4), 4u * segments);
    DWORD done = 0;
    for (WORD i = 0; list && i < segments && done < size; i++) {
      DWORD part = size - done < 16344 ? size - done : 16344;
      const BYTE *segment = Cell(h, U32(list + 4 * i), part);
      if (!segment) break;
      Copy(copy + done, segment, part);
      done += part;
    }
    if (done == size) return copy;
  } else if ((cell = Cell(h, offset, size))) {
    Copy(copy, cell, size);
    return copy;
  }
  HeapFree(GetProcessHeap(), 0, copy);
  return NULL;
}

static void CopyKey(const Hive *h, const BYTE *nk, HKEY into, int depth);

/* Each key in a subkey list ("lf", "lh", "li", or "ri" of lists). */
static void CopySubkeys(const Hive *h, DWORD offset, HKEY into, int depth) {
  const BYTE *list = Cell(h, offset, 4);
  if (!list) return;
  WORD count = U16(list + 2);
  BOOL hashed = list[0] == 'l' && (list[1] == 'f' || list[1] == 'h');
  BOOL index = list[0] == 'r' && list[1] == 'i';
  if (!hashed && !(list[0] == 'l' && list[1] == 'i') && !index) return;
  DWORD stride = hashed ? 8 : 4;
  if (!Cell(h, offset, 4 + stride * count)) return;
  for (WORD i = 0; i < count; i++) {
    DWORD child = U32(list + 4 + stride * i);
    if (index) {
      CopySubkeys(h, child, into, depth);
    } else {
      const BYTE *nk = Cell(h, child, 0x4c);
      if (nk && nk[0] == 'n' && nk[1] == 'k') CopyKey(h, nk, into, depth + 1);
    }
  }
}

static void CopyValues(const Hive *h, const BYTE *nk, HKEY key) {
  DWORD count = U32(nk + 0x24);
  const BYTE *list = Cell(h, U32(nk + 0x28), 4 * count);
  for (DWORD i = 0; list && i < count; i++) {
    const BYTE *vk = Cell(h, U32(list + 4 * i), 0x14);
    if (!vk || vk[0] != 'v' || vk[1] != 'k') continue;
    WORD name_length = U16(vk + 2);
    if (!Cell(h, U32(list + 4 * i), 0x14u + name_length)) continue;
    WCHAR *name = HeapAlloc(GetProcessHeap(), 0, (name_length + 1) * sizeof(WCHAR));
    DWORD length;
    BYTE *data = name ? ValueData(h, vk, &length) : NULL;
    if (data) {
      Name(vk + 0x14, name_length, U16(vk + 0x10) & 1, name, name_length + 1);
      RegSetValueExW(key, name, 0, U32(vk + 0x0c), data, length);
      HeapFree(GetProcessHeap(), 0, data);
    }
    if (name) HeapFree(GetProcessHeap(), 0, name);
  }
}

static void CopyContents(const Hive *h, const BYTE *nk, HKEY key, int depth) {
  CopyValues(h, nk, key);
  if (depth < 64 && U32(nk + 0x14)) CopySubkeys(h, U32(nk + 0x1c), key, depth);
}

static void CopyKey(const Hive *h, const BYTE *nk, HKEY into, int depth) {
  WORD name_length = U16(nk + 0x48);
  if ((DWORD)(nk - h->data) + 0x4c + name_length > h->size) return;
  WCHAR name[256];
  Name(nk + 0x4c, name_length, U16(nk + 2) & 0x20, name, 256);
  HKEY key;
  if (RegCreateKeyExW(into, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) != ERROR_SUCCESS) return;
  CopyContents(h, nk, key, depth);
  RegCloseKey(key);
}

/* Copies the hive in `file` into root\subkey and opens that key into `opened` (when not NULL). ERROR_BAD_FORMAT:
 * not a regf file. */
static LONG CopyHive(LPCWSTR file, HKEY root, LPCWSTR subkey, HKEY *opened) {
  HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (handle == INVALID_HANDLE_VALUE) return (LONG)GetLastError();
  DWORD size = GetFileSize(handle, NULL);
  HANDLE mapping = size >= 4096 + 32 ? CreateFileMappingW(handle, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
  const BYTE *data = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
  LONG result;
  if (!data || data[0] != 'r' || data[1] != 'e' || data[2] != 'g' || data[3] != 'f') {
    result = ERROR_BAD_FORMAT;
  } else {
    Hive hive = {data, size};
    const BYTE *top = Cell(&hive, U32(data + 0x24), 0x4c);
    HKEY key;
    if (!top || top[0] != 'n' || top[1] != 'k') {
      result = ERROR_BADDB;
    } else if ((result = RegCreateKeyExW(root, subkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL)) == 0) {
      CopyContents(&hive, top, key, 0);
      if (opened) {
        *opened = key;
      } else {
        RegCloseKey(key);
      }
    }
  }
  if (data) UnmapViewOfFile(data);
  if (mapping) CloseHandle(mapping);
  CloseHandle(handle);
  return result;
}

static LONG WINAPI LoadKeyW(HKEY root, LPCWSTR subkey, LPCWSTR file) {
  LONG result = CopyHive(file, root, subkey, NULL);
  if (result != ERROR_BAD_FORMAT) return result;
  LoadKeyFn wine = (LoadKeyFn)(void *)GetProcAddress(GetModuleHandleA("advapi32.dll"), "RegLoadKeyW");
  return wine ? wine(root, subkey, file) : ERROR_PROC_NOT_FOUND;
}

static const WCHAR kAppKeys[] = L"Software\\Mira\\AppKeys";

/* Writes `n` in decimal; returns the length. */
static int Decimal(WCHAR *out, DWORD n) {
  WCHAR digits[12];
  int len = 0, at = 0;
  do digits[len++] = (WCHAR)('0' + n % 10); while (n /= 10);
  while (len) out[at++] = digits[--len];
  return at;
}

/* "<pid>-<n>", or "<pid>-" (this process's prefix) when n is 0. */
static void AppKeyName(WCHAR *out, LONG n) {
  int at = Decimal(out, GetCurrentProcessId());
  out[at++] = '-';
  if (n > 0) at += Decimal(out + at, (DWORD)n);
  out[at] = 0;
}

static LONG app_keys;

static LONG WINAPI LoadAppKeyW(LPCWSTR file, HKEY *result, REGSAM sam, DWORD options, DWORD reserved) {
  (void)sam;
  (void)options;
  if (!file || !result || reserved) return ERROR_INVALID_PARAMETER;
  WCHAR name[64] = L"Software\\Mira\\AppKeys\\";
  int at = lstrlenW(name);
  AppKeyName(name + at, InterlockedIncrement(&app_keys));
  return CopyHive(file, HKEY_CURRENT_USER, name, result);
}

/* Deletes this process's app keys. */
void RegLoadCleanup(void) {
  HKEY keys;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppKeys, 0, KEY_ALL_ACCESS, &keys) != ERROR_SUCCESS) return;
  WCHAR mine[24];
  AppKeyName(mine, 0);
  int length = lstrlenW(mine);
  WCHAR name[256];
  for (DWORD i = 0, size = 256; RegEnumKeyExW(keys, i, name, &size, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; size = 256) {
    if (CompareStringW(LOCALE_INVARIANT, 0, name, length, mine, length) == CSTR_EQUAL) {
      RegDeleteTreeW(keys, name);
      RegDeleteKeyW(keys, name);
    } else {
      i++;
    }
  }
  RegCloseKey(keys);
}

static LONG WINAPI UnLoadKeyW(HKEY root, LPCWSTR subkey) {
  UnLoadKeyFn wine = (UnLoadKeyFn)(void *)GetProcAddress(GetModuleHandleA("advapi32.dll"), "RegUnLoadKeyW");
  if (wine && wine(root, subkey) == ERROR_SUCCESS) return ERROR_SUCCESS;
  RegDeleteTreeW(root, subkey);
  return RegDeleteKeyW(root, subkey);
}

static void PatchSlots(BYTE *base, const char *dll, IMAGE_THUNK_DATA *names, IMAGE_THUNK_DATA *slots) {
  if (lstrcmpiA(dll, "ADVAPI32.dll") != 0) return;
  for (; names->u1.AddressOfData; names++, slots++) {
    if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
    const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
    void *fn = lstrcmpA(name, "RegLoadKeyW") == 0      ? (void *)LoadKeyW
               : lstrcmpA(name, "RegUnLoadKeyW") == 0  ? (void *)UnLoadKeyW
               : lstrcmpA(name, "RegLoadAppKeyW") == 0 ? (void *)LoadAppKeyW
                                                       : NULL;
    DWORD old;
    if (!fn || !VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
    slots->u1.Function = (ULONG_PTR)fn;
    VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
  }
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
    DWORD names = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
    PatchSlots(base, (const char *)(base + imp->Name), (IMAGE_THUNK_DATA *)(base + names),
               (IMAGE_THUNK_DATA *)(base + imp->FirstThunk));
  }
}

static VOID NTAPI OnLoad(ULONG reason, const void *data, void *context) {
  (void)context;
  if (reason == 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */) PatchModule((HMODULE)((const LoadedData *)data)->base);
}

void RegLoadInstall(void) {
  PEB *peb = NtCurrentTeb()->ProcessEnvironmentBlock;
  LIST_ENTRY *head = &peb->Ldr->InMemoryOrderModuleList;
  for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
    LDR_DATA_TABLE_ENTRY *entry = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
    if (entry->DllBase) PatchModule((HMODULE)entry->DllBase);
  }
  void *cookie;
  LdrRegisterDllNotification(0, OnLoad, NULL, &cookie);
  /* The patched imports and the load callback point here, so this DLL must never unload. */
  HMODULE self;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)RegLoadInstall, &self);
}
