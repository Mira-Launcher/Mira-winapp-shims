/* ncrypt.dll stand-in: forwards to Wine's ncrypt (ncryptw.dll), whose NCryptImportKey takes RSA keys only.
 * EC key blobs (ECDSA/ECDH, P-256/384/521) are imported, signed and verified here through bcrypt, which
 * Revit's PersistenceDB needs to sign and check its data. */
#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>

#define KEY_MAGIC 0x4152494D /* "MIRA": our key objects; Wine's start with a small object type */

struct key {
  DWORD magic;
  BCRYPT_ALG_HANDLE alg;
  BCRYPT_KEY_HANDLE key;
};

typedef SECURITY_STATUS(WINAPI *import_fn)(NCRYPT_PROV_HANDLE, NCRYPT_KEY_HANDLE, LPCWSTR, NCryptBufferDesc *,
                                           NCRYPT_KEY_HANDLE *, PBYTE, DWORD, DWORD);
typedef SECURITY_STATUS(WINAPI *sign_fn)(NCRYPT_KEY_HANDLE, void *, PBYTE, DWORD, PBYTE, DWORD, DWORD *, DWORD);
typedef SECURITY_STATUS(WINAPI *verify_fn)(NCRYPT_KEY_HANDLE, void *, PBYTE, DWORD, PBYTE, DWORD, DWORD);
typedef SECURITY_STATUS(WINAPI *free_fn)(NCRYPT_HANDLE);

static FARPROC wine_fn(const char *name) {
  static HMODULE wine;
  if (!wine) wine = LoadLibraryA("ncryptw.dll");
  return wine ? GetProcAddress(wine, name) : NULL;
}

static struct key *ours(NCRYPT_HANDLE handle) {
  struct key *k = (struct key *)handle;
  return k && k->magic == KEY_MAGIC ? k : NULL;
}

static SECURITY_STATUS from_ntstatus(NTSTATUS status) {
  switch (status) {
    case 0: return ERROR_SUCCESS;
    case (NTSTATUS)0xC000A000: return NTE_BAD_SIGNATURE;     /* STATUS_INVALID_SIGNATURE */
    case (NTSTATUS)0xC0000023: return NTE_BUFFER_TOO_SMALL;  /* STATUS_BUFFER_TOO_SMALL */
    case (NTSTATUS)0xC000000D: return NTE_INVALID_PARAMETER; /* STATUS_INVALID_PARAMETER */
    case (NTSTATUS)0xC00000BB: return NTE_NOT_SUPPORTED;     /* STATUS_NOT_SUPPORTED */
    default: return NTE_FAIL;
  }
}

/* The bcrypt algorithm for an EC key blob's magic. */
static LPCWSTR ec_algorithm(DWORD magic) {
  switch (magic) {
    case BCRYPT_ECDSA_PUBLIC_P256_MAGIC: case BCRYPT_ECDSA_PRIVATE_P256_MAGIC: return BCRYPT_ECDSA_P256_ALGORITHM;
    case BCRYPT_ECDSA_PUBLIC_P384_MAGIC: case BCRYPT_ECDSA_PRIVATE_P384_MAGIC: return BCRYPT_ECDSA_P384_ALGORITHM;
    case BCRYPT_ECDSA_PUBLIC_P521_MAGIC: case BCRYPT_ECDSA_PRIVATE_P521_MAGIC: return BCRYPT_ECDSA_P521_ALGORITHM;
    case BCRYPT_ECDH_PUBLIC_P256_MAGIC: case BCRYPT_ECDH_PRIVATE_P256_MAGIC: return BCRYPT_ECDH_P256_ALGORITHM;
    case BCRYPT_ECDH_PUBLIC_P384_MAGIC: case BCRYPT_ECDH_PRIVATE_P384_MAGIC: return BCRYPT_ECDH_P384_ALGORITHM;
    case BCRYPT_ECDH_PUBLIC_P521_MAGIC: case BCRYPT_ECDH_PRIVATE_P521_MAGIC: return BCRYPT_ECDH_P521_ALGORITHM;
    default: return NULL;
  }
}

SECURITY_STATUS WINAPI NCryptImportKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE decrypt_key, LPCWSTR type,
                                       NCryptBufferDesc *params, NCRYPT_KEY_HANDLE *handle, PBYTE data, DWORD size,
                                       DWORD flags) {
  const int priv = !lstrcmpW(type, BCRYPT_ECCPRIVATE_BLOB);
  if ((priv || !lstrcmpW(type, BCRYPT_ECCPUBLIC_BLOB)) && data && size >= sizeof(BCRYPT_ECCKEY_BLOB) && handle) {
    LPCWSTR algorithm = ec_algorithm(((BCRYPT_ECCKEY_BLOB *)data)->dwMagic);
    if (!algorithm) return NTE_BAD_DATA;
    struct key *k = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*k));
    if (!k) return NTE_NO_MEMORY;
    k->magic = KEY_MAGIC;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&k->alg, algorithm, NULL, 0);
    if (!status) status = BCryptImportKeyPair(k->alg, NULL, type, &k->key, data, size, 0);
    if (status) {
      if (k->alg) BCryptCloseAlgorithmProvider(k->alg, 0);
      HeapFree(GetProcessHeap(), 0, k);
      return from_ntstatus(status);
    }
    *handle = (NCRYPT_KEY_HANDLE)k;
    return ERROR_SUCCESS;
  }
  import_fn wine = (import_fn)wine_fn("NCryptImportKey");
  return wine ? wine(provider, decrypt_key, type, params, handle, data, size, flags) : NTE_FAIL;
}

SECURITY_STATUS WINAPI NCryptSignHash(NCRYPT_KEY_HANDLE handle, void *padding, PBYTE hash, DWORD hash_size,
                                      PBYTE signature, DWORD signature_size, DWORD *result, DWORD flags) {
  struct key *k = ours(handle);
  if (k) return from_ntstatus(BCryptSignHash(k->key, padding, hash, hash_size, signature, signature_size, result,
                                             flags & ~NCRYPT_SILENT_FLAG));
  sign_fn wine = (sign_fn)wine_fn("NCryptSignHash");
  return wine ? wine(handle, padding, hash, hash_size, signature, signature_size, result, flags) : NTE_FAIL;
}

SECURITY_STATUS WINAPI NCryptVerifySignature(NCRYPT_KEY_HANDLE handle, void *padding, PBYTE hash, DWORD hash_size,
                                             PBYTE signature, DWORD signature_size, DWORD flags) {
  struct key *k = ours(handle);
  if (k) return from_ntstatus(BCryptVerifySignature(k->key, padding, hash, hash_size, signature, signature_size,
                                                    flags & ~NCRYPT_SILENT_FLAG));
  verify_fn wine = (verify_fn)wine_fn("NCryptVerifySignature");
  return wine ? wine(handle, padding, hash, hash_size, signature, signature_size, flags) : NTE_FAIL;
}

SECURITY_STATUS WINAPI NCryptFreeObject(NCRYPT_HANDLE handle) {
  struct key *k = ours(handle);
  if (k) {
    BCryptDestroyKey(k->key);
    BCryptCloseAlgorithmProvider(k->alg, 0);
    k->magic = 0;
    HeapFree(GetProcessHeap(), 0, k);
    return ERROR_SUCCESS;
  }
  free_fn wine = (free_fn)wine_fn("NCryptFreeObject");
  return wine ? wine(handle) : NTE_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
  return TRUE;
}
