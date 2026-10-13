/* Stand-in for wintrust.dll: forwards everything to Wine's own WinTrust
 * (wintrustw.dll) and verifies AppX/MSIX signature blobs itself.
 *
 * MSIX Core (which Autodesk's installer uses for its .adix packages) checks a
 * package's signature chain at the current time, and when that fails, as it
 * does once the signing certificate has expired, asks WinVerifyTrust about
 * the signature blob (the P7X subject). Windows then validates the chain at
 * the signature's RFC 3161 timestamp. Wine has no P7X SIP and reads only
 * legacy countersignatures, so every such package is rejected as untrusted.
 *
 * This checks the blob the way Windows does: the signer's signature, the
 * timestamp's signature and its binding to the signer, and both certificate
 * chains at the timestamp's time under the Authenticode policy. Nothing is
 * trusted that would not be on Windows. */
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <bcrypt.h>

typedef LONG(WINAPI *WinVerifyTrustFn)(HWND, GUID *, LPVOID);

/* The P7X SIP's subject: an AppX/MSIX package signature (AppxSignature.p7x). */
static const GUID kP7xSubject = {0x5598cff1, 0x68db, 0x4340, {0xb5, 0x7f, 0x1c, 0xac, 0xf8, 0x8c, 0x9a, 0x51}};
/* WINTRUST_ACTION_GENERIC_VERIFY_V2 */
static const GUID kGenericVerifyV2 = {0x00aac56b, 0xcd44, 0x11d0, {0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}};
static const char kRfc3161Countersign[] = "1.3.6.1.4.1.311.3.3.1";
static const DWORD kEncoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

void RegLoadInstall(void); /* regload.c */
void RegLoadCleanup(void);

static WinVerifyTrustFn real_verify;
static WinVerifyTrustFn real_verify_ex;

static BOOL SameGuid(const GUID *a, const GUID *b) {
  const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
  for (int i = 0; i < (int)sizeof(GUID); i++)
    if (x[i] != y[i]) return FALSE;
  return TRUE;
}

static BOOL SameBytes(const BYTE *a, const BYTE *b, DWORD size) {
  for (DWORD i = 0; i < size; i++)
    if (a[i] != b[i]) return FALSE;
  return TRUE;
}

static BOOL SameString(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return *a == *b;
}

/* A DER element: its tag, and where its contents start and end. */
typedef struct {
  BYTE tag;
  const BYTE *data;
  DWORD size;
} Der;

static BOOL ReadDer(const BYTE **at, const BYTE *end, Der *out) {
  const BYTE *p = *at;
  if (end - p < 2) return FALSE;
  out->tag = *p++;
  DWORD size = *p++;
  if (size & 0x80) {
    int bytes = size & 0x7f;
    if (bytes < 1 || bytes > 4 || end - p < bytes) return FALSE;
    size = 0;
    while (bytes--) size = (size << 8) | *p++;
  }
  if ((DWORD)(end - p) < size) return FALSE;
  out->data = p;
  out->size = size;
  *at = p + size;
  return TRUE;
}

/* The decoded message's parameter `type` (signer `index`), in a buffer to LocalFree. */
static BYTE *MsgParam(HCRYPTMSG msg, DWORD type, DWORD index, DWORD *size) {
  *size = 0;
  if (!CryptMsgGetParam(msg, type, index, NULL, size) || !*size) return NULL;
  BYTE *buffer = LocalAlloc(LMEM_FIXED, *size);
  if (buffer && !CryptMsgGetParam(msg, type, index, buffer, size)) {
    LocalFree(buffer);
    return NULL;
  }
  return buffer;
}

static HCRYPTMSG OpenMessage(const BYTE *der, DWORD size) {
  HCRYPTMSG msg = CryptMsgOpenToDecode(kEncoding, 0, 0, 0, NULL, NULL);
  if (msg && !CryptMsgUpdate(msg, der, size, TRUE)) {
    CryptMsgClose(msg);
    return NULL;
  }
  return msg;
}

/* The first signer's certificate, after checking its signature over the message. */
static PCCERT_CONTEXT VerifiedSigner(HCRYPTMSG msg, HCERTSTORE store) {
  DWORD size;
  CERT_INFO *info = (CERT_INFO *)MsgParam(msg, CMSG_SIGNER_CERT_INFO_PARAM, 0, &size);
  if (!info) return NULL;
  PCCERT_CONTEXT cert = CertGetSubjectCertificateFromStore(store, kEncoding, info);
  LocalFree(info);
  if (cert && !CryptMsgControl(msg, 0, CMSG_CTRL_VERIFY_SIGNATURE, cert->pCertInfo)) {
    CertFreeCertificateContext(cert);
    return NULL;
  }
  return cert;
}

/* Whether `cert`'s chain, built at `when`, passes the Authenticode and base policies. */
static LONG CheckChain(PCCERT_CONTEXT cert, HCERTSTORE store, FILETIME *when) {
  CERT_CHAIN_PARA para = {0};
  para.cbSize = sizeof(para);
  PCCERT_CHAIN_CONTEXT chain = NULL;
  if (!CertGetCertificateChain(NULL, cert, when, store, &para, 0, NULL, &chain)) return CERT_E_CHAINING;
  LONG result = ERROR_SUCCESS;
  const LPCSTR policies[] = {CERT_CHAIN_POLICY_AUTHENTICODE, CERT_CHAIN_POLICY_BASE};
  for (int i = 0; i < 2 && result == ERROR_SUCCESS; i++) {
    CERT_CHAIN_POLICY_PARA policy = {0};
    CERT_CHAIN_POLICY_STATUS status = {0};
    policy.cbSize = sizeof(policy);
    status.cbSize = sizeof(status);
    if (!CertVerifyCertificateChainPolicy(policies[i], chain, &policy, &status)) result = CERT_E_CHAINING;
    else result = (LONG)status.dwError;
  }
  CertFreeCertificateChain(chain);
  return result;
}

static LPCWSTR HashFor(const char *oid) {
  if (SameString(oid, "2.16.840.1.101.3.4.2.1")) return BCRYPT_SHA256_ALGORITHM;
  if (SameString(oid, "2.16.840.1.101.3.4.2.2")) return BCRYPT_SHA384_ALGORITHM;
  if (SameString(oid, "2.16.840.1.101.3.4.2.3")) return BCRYPT_SHA512_ALGORITHM;
  if (SameString(oid, "1.3.14.3.2.26")) return BCRYPT_SHA1_ALGORITHM;
  return NULL;
}

/* Whether hashing `data` with `oid_der` (a DER OBJECT IDENTIFIER's contents) gives `expected`. */
static BOOL HashMatches(const Der *oid_der, const BYTE *data, DWORD size, const Der *expected) {
  char oid[64];
  DWORD length = 0;
  /* Decode the OID's arcs into dotted text. */
  if (oid_der->size < 1) return FALSE;
  DWORD first = oid_der->data[0];
  length = wsprintfA(oid, "%lu.%lu", first / 40, first % 40);
  DWORD arc = 0;
  for (DWORD i = 1; i < oid_der->size && length < sizeof(oid) - 12; i++) {
    arc = (arc << 7) | (oid_der->data[i] & 0x7f);
    if (!(oid_der->data[i] & 0x80)) {
      length += wsprintfA(oid + length, ".%lu", arc);
      arc = 0;
    }
  }
  LPCWSTR algorithm = HashFor(oid);
  if (!algorithm) return FALSE;
  BCRYPT_ALG_HANDLE provider;
  if (BCryptOpenAlgorithmProvider(&provider, algorithm, NULL, 0)) return FALSE;
  BYTE digest[64];
  DWORD digest_size = 0, got;
  BOOL ok = !BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, (BYTE *)&digest_size, sizeof(digest_size), &got, 0) &&
            digest_size <= sizeof(digest) && !BCryptHash(provider, NULL, 0, (BYTE *)data, size, digest, digest_size) &&
            digest_size == expected->size && SameBytes(digest, expected->data, digest_size);
  BCryptCloseAlgorithmProvider(provider, 0);
  return ok;
}

/* GeneralizedTime "YYYYMMDDHHMMSS[.f]Z" as a FILETIME. */
static BOOL ParseTime(const Der *time, FILETIME *out) {
  if (time->tag != 0x18 || time->size < 15) return FALSE;
  const BYTE *t = time->data;
  for (int i = 0; i < 14; i++)
    if (t[i] < '0' || t[i] > '9') return FALSE;
#define DIGITS2(i) ((t[i] - '0') * 10 + (t[i + 1] - '0'))
  SYSTEMTIME st = {0};
  st.wYear = (WORD)(DIGITS2(0) * 100 + DIGITS2(2));
  st.wMonth = (WORD)DIGITS2(4);
  st.wDay = (WORD)DIGITS2(6);
  st.wHour = (WORD)DIGITS2(8);
  st.wMinute = (WORD)DIGITS2(10);
  st.wSecond = (WORD)DIGITS2(12);
#undef DIGITS2
  return SystemTimeToFileTime(&st, out);
}

/* The signer's RFC 3161 timestamp time, after checking the timestamp's signature, its chain, and that it
 * covers the signer's signature. */
static LONG TimestampTime(HCRYPTMSG msg, FILETIME *when) {
  DWORD attrs_size;
  CRYPT_ATTRIBUTES *attrs = (CRYPT_ATTRIBUTES *)MsgParam(msg, CMSG_SIGNER_UNAUTH_ATTR_PARAM, 0, &attrs_size);
  if (!attrs) return TRUST_E_TIME_STAMP;
  const CRYPT_ATTR_BLOB *token = NULL;
  for (DWORD i = 0; i < attrs->cAttr && !token; i++)
    if (SameString(attrs->rgAttr[i].pszObjId, kRfc3161Countersign) && attrs->rgAttr[i].cValue)
      token = &attrs->rgAttr[i].rgValue[0];
  LONG result = TRUST_E_TIME_STAMP;
  HCRYPTMSG ts = token ? OpenMessage(token->pbData, token->cbData) : NULL;
  HCERTSTORE ts_store = ts ? CertOpenStore(CERT_STORE_PROV_MSG, kEncoding, 0, 0, ts) : NULL;
  PCCERT_CONTEXT tsa = ts_store ? VerifiedSigner(ts, ts_store) : NULL;
  DWORD info_size = 0, signer_size = 0;
  BYTE *info = tsa ? MsgParam(ts, CMSG_CONTENT_PARAM, 0, &info_size) : NULL;
  /* The signature the timestamp covers. Wine's CMSG_ENCRYPTED_DIGEST isn't these bytes; the signer info's are. */
  CMSG_SIGNER_INFO *signer = info ? (CMSG_SIGNER_INFO *)MsgParam(msg, CMSG_SIGNER_INFO_PARAM, 0, &signer_size) : NULL;
  if (signer) {
    /* TSTInfo ::= SEQUENCE { version, policy, messageImprint SEQUENCE { AlgorithmIdentifier, OCTET STRING },
     *                        serialNumber, genTime GeneralizedTime, ... } */
    const BYTE *at = info, *end = info + info_size;
    Der tst, version, policy, imprint, serial, gen_time;
    /* Some CryptMsg implementations hand back the TSTInfo still in its OCTET STRING. */
    if (ReadDer(&at, end, &tst) && tst.tag == 0x04) {
      at = tst.data;
      end = tst.data + tst.size;
      ReadDer(&at, end, &tst);
    }
    if (tst.tag == 0x30) {
      at = tst.data;
      end = tst.data + tst.size;
      if (ReadDer(&at, end, &version) && ReadDer(&at, end, &policy) && ReadDer(&at, end, &imprint) &&
          ReadDer(&at, end, &serial) && ReadDer(&at, end, &gen_time) && ParseTime(&gen_time, when)) {
        const BYTE *in = imprint.data, *in_end = imprint.data + imprint.size;
        Der algorithm, hash, oid;
        const BYTE *alg_at;
        if (ReadDer(&in, in_end, &algorithm) && ReadDer(&in, in_end, &hash) && hash.tag == 0x04 &&
            (alg_at = algorithm.data, ReadDer(&alg_at, algorithm.data + algorithm.size, &oid)) && oid.tag == 0x06 &&
            HashMatches(&oid, signer->EncryptedHash.pbData, signer->EncryptedHash.cbData, &hash)) {
          result = CheckChain(tsa, ts_store, when);
        }
      }
    }
  }
  LocalFree(signer);
  LocalFree(info);
  if (tsa) CertFreeCertificateContext(tsa);
  if (ts_store) CertCloseStore(ts_store, 0);
  if (ts) CryptMsgClose(ts);
  LocalFree(attrs);
  return result;
}

/* Verifies a P7X blob ("PKCX" then a PKCS #7 SignedData) as Windows' P7X SIP does. */
static LONG VerifyP7x(const WINTRUST_BLOB_INFO *blob) {
  if (blob->cbMemObject < 4 || !SameBytes(blob->pbMemObject, (const BYTE *)"PKCX", 4)) return TRUST_E_NOSIGNATURE;
  HCRYPTMSG msg = OpenMessage(blob->pbMemObject + 4, blob->cbMemObject - 4);
  if (!msg) return TRUST_E_NOSIGNATURE;
  HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MSG, kEncoding, 0, 0, msg);
  PCCERT_CONTEXT signer = store ? VerifiedSigner(msg, store) : NULL;
  LONG result = TRUST_E_BAD_DIGEST;
  if (signer) {
    /* Valid now, or else when it was timestamped. */
    result = CheckChain(signer, store, NULL);
    if (result != ERROR_SUCCESS) {
      FILETIME when;
      const LONG stamped = TimestampTime(msg, &when);
      result = stamped == ERROR_SUCCESS ? CheckChain(signer, store, &when) : result;
    }
    CertFreeCertificateContext(signer);
  }
  if (store) CertCloseStore(store, 0);
  CryptMsgClose(msg);
  return result;
}

static BOOL IsP7xVerify(GUID *action, LPVOID data) {
  const WINTRUST_DATA *trust = data;
  return action && trust && SameGuid(action, &kGenericVerifyV2) && trust->cbStruct >= sizeof(WINTRUST_DATA) &&
         trust->dwUnionChoice == WTD_CHOICE_BLOB && trust->pBlob && SameGuid(&trust->pBlob->gSubject, &kP7xSubject) &&
         (trust->dwStateAction == WTD_STATEACTION_IGNORE || trust->dwStateAction == WTD_STATEACTION_VERIFY);
}

LONG WINAPI WinVerifyTrust(HWND window, GUID *action, LPVOID data) {
  if (IsP7xVerify(action, data)) {
    ((WINTRUST_DATA *)data)->hWVTStateData = NULL;
    return VerifyP7x(((WINTRUST_DATA *)data)->pBlob);
  }
  return real_verify ? real_verify(window, action, data) : TRUST_E_PROVIDER_UNKNOWN;
}

HRESULT WINAPI WinVerifyTrustEx(HWND window, GUID *action, WINTRUST_DATA *data) {
  if (IsP7xVerify(action, data)) {
    data->hWVTStateData = NULL;
    return VerifyP7x(data->pBlob);
  }
  return real_verify_ex ? real_verify_ex(window, action, data) : TRUST_E_PROVIDER_UNKNOWN;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    RegLoadInstall();
    HMODULE wine = LoadLibraryA("wintrustw.dll");
    if (wine) {
      real_verify = (WinVerifyTrustFn)GetProcAddress(wine, "WinVerifyTrust");
      real_verify_ex = (WinVerifyTrustFn)GetProcAddress(wine, "WinVerifyTrustEx");
    }
  }
  if (reason == DLL_PROCESS_DETACH) RegLoadCleanup();
  return TRUE;
}
