/* Stand-in for sppc.dll (Software Protection Platform client).
 *
 * Wine's sppc exports the SL* names but aborts on the ones Office's
 * Click-to-Run integrator calls. This one logs every call and answers
 * the few that let an install finish. It never reports a product as
 * licensed: activation and sign-in stay with Office itself.
 *
 * Log: %SPPC_SHIM_LOG%, default C:\sppc-shim.log. */
#include <windows.h>
#include <stdio.h>

#define E_NOTIMPL_ ((HRESULT)0x80004001L)
#define SL_E_VALUE_NOT_FOUND ((HRESULT)0xC004F012L)
#define SL_E_PRODUCT_SKU_NOT_INSTALLED ((HRESULT)0xC004F014L)

static void Log(const char *name) {
  char path[MAX_PATH];
  if (!GetEnvironmentVariableA("SPPC_SHIM_LOG", path, sizeof path)) lstrcpyA(path, "C:\\sppc-shim.log");
  HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  char line[160];
  int n = wsprintfA(line, "%lu %s\r\n", GetCurrentProcessId(), name);
  DWORD w;
  WriteFile(f, line, n, &w, NULL);
  CloseHandle(f);
}

/* All SL* functions take at most four register arguments we care about. */
#define STUB(name) HRESULT WINAPI name(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; Log(#name); return E_NOTIMPL_; }
STUB(SLCallServer)
STUB(SLDepositMigrationBlob)
STUB(SLDepositOfflineConfirmationId)
STUB(SLDepositOfflineConfirmationIdEx)
STUB(SLDepositStoreToken)
STUB(SLFireEvent)
STUB(SLGatherMigrationBlob)
STUB(SLGatherMigrationBlobEx)
STUB(SLGenerateOfflineInstallationId)
STUB(SLGenerateOfflineInstallationIdEx)
STUB(SLGetActiveLicenseInfo)
STUB(SLGetApplicationInformation)
STUB(SLGetApplicationPolicy)
STUB(SLGetAuthenticationResult)
STUB(SLGetEncryptedPIDEx)
STUB(SLGetGenuineInformation)
STUB(SLGetInstalledProductKeyIds)
STUB(SLGetLicense)
STUB(SLGetLicenseFileId)
STUB(SLGetLicenseInformation)
STUB(SLGetPKeyId)
STUB(SLGetPKeyInformation)
STUB(SLGetPolicyInformation)
STUB(SLGetPolicyInformationDWORD)
STUB(SLGetProductSkuInformation)
STUB(SLGetServiceInformation)
STUB(SLInstallProofOfPurchase)
STUB(SLInstallProofOfPurchaseEx)
STUB(SLIsGenuineLocalEx)
STUB(SLpAuthenticateGenuineTicketResponse)
STUB(SLpBeginGenuineTicketTransaction)
STUB(SLpClearActivationInProgress)
STUB(SLpDepositDownlevelGenuineTicket)
STUB(SLpDepositTokenActivationResponse)
STUB(SLPersistApplicationPolicies)
STUB(SLPersistRTSPayloadOverride)
STUB(SLpGenerateTokenActivationChallenge)
STUB(SLpGetGenuineBlob)
STUB(SLpGetGenuineLocal)
STUB(SLpGetLicenseAcquisitionInfo)
STUB(SLpGetMachineUGUID)
STUB(SLpGetMSPidInformation)
STUB(SLpGetTokenActivationGrantInfo)
STUB(SLpIAActivateProduct)
STUB(SLpIsCurrentInstalledProductKeyDefaultKey)
STUB(SLpProcessVMPipeMessage)
STUB(SLpSetActivationInProgress)
STUB(SLpTriggerServiceWorker)
STUB(SLpVLActivateProduct)
STUB(SLReArm)
STUB(SLRegisterEvent)
STUB(SLRegisterPlugin)
STUB(SLSetAuthenticationData)
STUB(SLSetCurrentProductKey)
STUB(SLSetGenuineInformation)
STUB(SLUninstallLicense)
STUB(SLUninstallProofOfPurchase)
STUB(SLUnloadApplicationPolicies)
STUB(SLUnregisterEvent)
STUB(SLUnregisterPlugin)

HRESULT WINAPI SLOpen(HANDLE *out) {
  Log("SLOpen");
  if (!out) return E_INVALIDARG;
  *out = (HANDLE)(ULONG_PTR)1;
  return S_OK;
}

HRESULT WINAPI SLClose(HANDLE h) {
  (void)h;
  Log("SLClose");
  return S_OK;
}

/* Accept the license file Office hands over and report an id for it.
 * Nothing is stored or enforced here. */
HRESULT WINAPI SLInstallLicense(HANDLE h, UINT size, const BYTE *blob, GUID *id) {
  (void)h; (void)blob;
  Log("SLInstallLicense");
  if (id) {
    ZeroMemory(id, sizeof *id);
    id->Data1 = size;
  }
  return S_OK;
}

/* The calls below answer "no license on this machine" the way the real
 * service does when nothing is installed, so Office moves on to its own
 * subscription licensing instead of treating the service as broken. */
HRESULT WINAPI SLGetSLIDList(HANDLE h, int query_type, const GUID *query, int return_type, UINT *count, GUID **ids) {
  (void)h; (void)query_type; (void)query; (void)return_type;
  Log("SLGetSLIDList");
  if (count) *count = 0;
  if (ids) *ids = NULL;
  return SL_E_VALUE_NOT_FOUND;
}

HRESULT WINAPI SLGetLicensingStatusInformation(HANDLE h, const GUID *app, const GUID *sku, const WCHAR *right, UINT *count, void **status) {
  (void)h; (void)app; (void)sku; (void)right;
  Log("SLGetLicensingStatusInformation");
  if (count) *count = 0;
  if (status) *status = NULL;
  return S_OK;
}

HRESULT WINAPI SLConsumeRight(HANDLE h, const GUID *app, const GUID *sku, const WCHAR *right, void *reserved) {
  (void)h; (void)app; (void)sku; (void)right; (void)reserved;
  Log("SLConsumeRight");
  return SL_E_PRODUCT_SKU_NOT_INSTALLED;
}

HRESULT WINAPI SLLoadApplicationPolicies(const GUID *app, const GUID *sku, DWORD flags, HANDLE *context) {
  (void)app; (void)sku; (void)flags;
  Log("SLLoadApplicationPolicies");
  if (context) *context = NULL;
  return SL_E_PRODUCT_SKU_NOT_INSTALLED;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
  return TRUE;
}
