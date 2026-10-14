// tar.exe for Wine: Windows 10's bsdtar extracts zip archives, and installers call it for that
// (Inventor's Electrical Catalog Browser runs `tar -xf x.zip -C dir --keep-newer-files`).
// Only extraction of zip archives is supported: -x, -f FILE, -C DIR, --keep-newer-files.
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "miniz.h"

static char *Utf8(const wchar_t *s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
  char *out = malloc(n);
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
  return out;
}

static wchar_t *Wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *out = malloc(n * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
  return out;
}

// Creates every folder above `path` (a file path, or a folder path ending in a slash).
static void MakeParents(wchar_t *path) {
  for (wchar_t *p = path + 3; *p; ++p) {
    if (*p != L'\\') continue;
    *p = 0;
    CreateDirectoryW(path, NULL);
    *p = L'\\';
  }
}

// The zip entry's time as a FILETIME.
static FILETIME EntryTime(const mz_zip_archive_file_stat *st) {
  const ULONGLONG t = ((ULONGLONG)st->m_time + 11644473600ULL) * 10000000ULL;
  FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
  return ft;
}

int wmain(int argc, wchar_t **argv) {
  const wchar_t *archive = NULL, *dir = L".";
  int extract = 0, keep_newer = 0;
  for (int i = 1; i < argc; ++i) {
    const wchar_t *a = argv[i];
    if (!wcscmp(a, L"--keep-newer-files")) {
      keep_newer = 1;
    } else if (!wcscmp(a, L"-C") && i + 1 < argc) {
      dir = argv[++i];
    } else if (a[0] == L'-' && a[1] != L'-') {
      // Bundled short flags, as in -xf FILE or -xvf FILE.
      for (const wchar_t *f = a + 1; *f; ++f) {
        if (*f == L'x') extract = 1;
        if (*f == L'f' && i + 1 < argc) archive = argv[++i];
      }
    }
  }
  if (!extract || !archive) {
    fwprintf(stderr, L"tar: only `tar -xf ARCHIVE.zip [-C DIR]` is supported\n");
    return 1;
  }

  char *archive8 = Utf8(archive);
  mz_zip_archive zip;
  memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, archive8, 0)) {
    fwprintf(stderr, L"tar: %ls: not a zip archive or unreadable\n", archive);
    return 1;
  }
  int failed = 0;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      failed = 1;
      continue;
    }
    wchar_t *name = Wide(st.m_filename);
    for (wchar_t *p = name; *p; ++p) {
      if (*p == L'/') *p = L'\\';
    }
    // An absolute or parent-relative entry would write outside DIR.
    if (name[0] == L'\\' || (name[0] && name[1] == L':') || wcsstr(name, L"..\\") || !wcscmp(name, L"..")) {
      fwprintf(stderr, L"tar: skipping unsafe path %ls\n", name);
      free(name);
      continue;
    }
    const size_t len = wcslen(dir) + wcslen(name) + 2;
    wchar_t *out = malloc(len * sizeof(wchar_t));
    swprintf(out, len, L"%ls%ls%ls", dir, dir[wcslen(dir) - 1] == L'\\' ? L"" : L"\\", name);
    MakeParents(out);
    if (st.m_is_directory) {
      CreateDirectoryW(out, NULL);
    } else {
      const FILETIME when = EntryTime(&st);
      WIN32_FILE_ATTRIBUTE_DATA have;
      if (keep_newer && GetFileAttributesExW(out, GetFileExInfoStandard, &have) &&
          CompareFileTime(&have.ftLastWriteTime, &when) >= 0) {
        free(out);
        free(name);
        continue;
      }
      char *out8 = Utf8(out);
      if (mz_zip_reader_extract_to_file(&zip, i, out8, 0)) {
        HANDLE h = CreateFileW(out, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
          SetFileTime(h, NULL, NULL, &when);
          CloseHandle(h);
        }
      } else {
        fwprintf(stderr, L"tar: couldn't extract %ls\n", name);
        failed = 1;
      }
      free(out8);
    }
    free(out);
    free(name);
  }
  mz_zip_reader_end(&zip);
  free(archive8);
  return failed;
}
