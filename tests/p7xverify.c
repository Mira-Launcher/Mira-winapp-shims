/* Asks WinVerifyTrust about an AppX/MSIX signature (an AppxSignature.p7x), as MSIX Core does.
 * Usage: p7xverify.exe <AppxSignature.p7x>   prints the result and exits 0 when trusted. */
#include <windows.h>
#include <wintrust.h>
#include <stdio.h>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE) return 2;
  static BYTE buffer[1 << 20];
  DWORD size = 0;
  ReadFile(file, buffer, sizeof(buffer), &size, NULL);
  CloseHandle(file);

  WINTRUST_BLOB_INFO blob = {0};
  blob.cbStruct = sizeof(blob);
  blob.gSubject = (GUID){0x5598cff1, 0x68db, 0x4340, {0xb5, 0x7f, 0x1c, 0xac, 0xf8, 0x8c, 0x9a, 0x51}};
  blob.cbMemObject = size;
  blob.pbMemObject = buffer;
  WINTRUST_DATA data = {0};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_BLOB;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  data.pBlob = &blob;
  GUID action = {0x00aac56b, 0xcd44, 0x11d0, {0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}};
  LONG result = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
  printf("WinVerifyTrust: 0x%08lx\n", (unsigned long)result);
  /* Also beside the file, for runners that drop the console. */
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s.result", argv[1]);
  FILE *out = fopen(path, "w");
  if (out) fprintf(out, "0x%08lx\n", (unsigned long)result), fclose(out);
  return result == 0 ? 0 : 1;
}
