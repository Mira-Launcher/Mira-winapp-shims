/* Stand-in for xmllite.dll: forwards everything to Wine's own XmlLite
 * (xmllitew.dll) and fixes duplicate namespace declarations in IXmlWriter.
 *
 * When a program starts an element with a prefix and namespace and then
 * declares that same prefix explicitly (WriteStartElement(L"s", ..., ns)
 * followed by WriteAttributeString(L"xmlns", L"s", NULL, ns)), Windows writes
 * xmlns:s once. Wine writes it twice, which is invalid XML. Office builds its
 * OneDrive file requests this way, and the server drops the connection
 * (HTTP 503), so cloud files never open.
 *
 * Wine's writers share one method table, so the fix swaps two table entries
 * the first time a writer is created: WriteStartElement remembers each
 * writer's current element prefix and namespace, and WriteAttributeString
 * skips a declaration that only repeats them. */
#define COBJMACROS
#include <windows.h>
#include <xmllite.h>

typedef HRESULT(WINAPI *CreateWriterFn)(REFIID, void **, IMalloc *);
typedef HRESULT(STDMETHODCALLTYPE *StartElementFn)(IXmlWriter *, LPCWSTR, LPCWSTR, LPCWSTR);
typedef HRESULT(STDMETHODCALLTYPE *AttributeFn)(IXmlWriter *, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);

enum { kAttributeSlot = 7, kStartElementSlot = 27 };

static StartElementFn real_start_element;
static AttributeFn real_attribute;
static LONG patched;

/* The current element of recently used writers, replaced round-robin. */
static struct {
  IXmlWriter *writer;
  WCHAR prefix[64];
  WCHAR uri[512];
} elements[32];
static LONG next_slot;

static const WCHAR kXmlnsUri[] = L"http://www.w3.org/2000/xmlns/";

static BOOL IsEmpty(LPCWSTR s) { return !s || !*s; }

static int FindSlot(IXmlWriter *writer) {
  for (int i = 0; i < (int)(sizeof(elements) / sizeof(elements[0])); i++)
    if (elements[i].writer == writer) return i;
  return -1;
}

static HRESULT STDMETHODCALLTYPE StartElement(IXmlWriter *writer, LPCWSTR prefix, LPCWSTR local, LPCWSTR uri) {
  int i = FindSlot(writer);
  if (i < 0) {
    i = (int)(InterlockedIncrement(&next_slot) % (sizeof(elements) / sizeof(elements[0])));
    elements[i].writer = writer;
  }
  lstrcpynW(elements[i].prefix, prefix ? prefix : L"", sizeof(elements[i].prefix) / sizeof(WCHAR));
  lstrcpynW(elements[i].uri, uri ? uri : L"", sizeof(elements[i].uri) / sizeof(WCHAR));
  return real_start_element(writer, prefix, local, uri);
}

static HRESULT STDMETHODCALLTYPE Attribute(IXmlWriter *writer, LPCWSTR prefix, LPCWSTR local, LPCWSTR uri, LPCWSTR value) {
  /* Which prefix does this attribute declare, if any? "" is the default namespace. */
  LPCWSTR declared = NULL;
  if (prefix && !lstrcmpW(prefix, L"xmlns") && IsEmpty(uri)) declared = local;
  else if (IsEmpty(prefix) && uri && !lstrcmpW(uri, kXmlnsUri)) declared = (local && !lstrcmpW(local, L"xmlns")) ? L"" : local;
  else if (IsEmpty(prefix) && IsEmpty(uri) && local && !lstrcmpW(local, L"xmlns")) declared = L"";

  int i = FindSlot(writer);
  if (declared && value && i >= 0 && elements[i].uri[0] && !lstrcmpW(declared, elements[i].prefix) &&
      !lstrcmpW(value, elements[i].uri))
    return S_OK; /* the element's own declaration is written by the start tag */
  return real_attribute(writer, prefix, local, uri, value);
}

static void PatchWriter(void *obj) {
  if (InterlockedCompareExchange(&patched, 1, 0) != 0) return;
  void **table = *(void ***)obj;
  real_attribute = (AttributeFn)table[kAttributeSlot];
  real_start_element = (StartElementFn)table[kStartElementSlot];
  DWORD old;
  if (!VirtualProtect(table, (kStartElementSlot + 1) * sizeof(void *), PAGE_READWRITE, &old)) return;
  table[kAttributeSlot] = (void *)Attribute;
  table[kStartElementSlot] = (void *)StartElement;
  VirtualProtect(table, (kStartElementSlot + 1) * sizeof(void *), old, &old);
}

HRESULT WINAPI CreateXmlWriter(REFIID riid, void **obj, IMalloc *imalloc) {
  static CreateWriterFn real;
  if (!real) real = (CreateWriterFn)(void *)GetProcAddress(LoadLibraryA("xmllitew.dll"), "CreateXmlWriter");
  if (!real) return E_FAIL;
  HRESULT hr = real(riid, obj, imalloc);
  if (SUCCEEDED(hr) && obj && *obj) PatchWriter(*obj);
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
  return TRUE;
}
