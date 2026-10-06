/* Stand-in for d2d1.dll: forwards everything to Wine's own Direct2D (d2d1w.dll)
 * and makes ID2D1DeviceContext5::CreateSvgDocument succeed.
 *
 * Wine's CreateSvgDocument is a stub that fails. Excel's ribbon asks for an SVG
 * document while it starts and crashes when it cannot get one. The document made
 * here is empty: it accepts the viewport size and nothing else, so SVG icons are
 * not drawn, but the app keeps running.
 *
 * Better still, the context is made to deny it supports ID2D1DeviceContext5, the
 * interface that carries SVG, so Office never asks.
 *
 * It also makes pixel unit mode work (see ContextSetUnitMode below).
 *
 * Wine's Direct2D objects share their method tables, so the fix swaps table
 * entries in place. It starts from the three exported functions that create
 * objects, follows factory -> device -> device context, and replaces
 * CreateSvgDocument on the context. */
#include <windows.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <dxgi1_2.h>

typedef HRESULT(WINAPI *CreateFactoryFn)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **);
typedef HRESULT(WINAPI *CreateDeviceFn)(IDXGIDevice *, const D2D1_CREATION_PROPERTIES *, ID2D1Device **);
typedef HRESULT(WINAPI *CreateDeviceContextFn)(IDXGISurface *, const D2D1_CREATION_PROPERTIES *, ID2D1DeviceContext **);

typedef HRESULT(STDMETHODCALLTYPE *CreateDeviceFnPtr)(void *, IDXGIDevice *, void **);
typedef HRESULT(STDMETHODCALLTYPE *CreateContextFnPtr)(void *, D2D1_DEVICE_CONTEXT_OPTIONS, void **);
typedef HRESULT(STDMETHODCALLTYPE *CreateSvgFnPtr)(void *, IStream *, D2D1_SIZE_F, ID2D1SvgDocument **);

/* Calls are logged to %D2D1_SHIM_LOG%, default C:\\d2d1-shim.log. */
static void Log(const char *what) {
  char path[MAX_PATH];
  if (!GetEnvironmentVariableA("D2D1_SHIM_LOG", path, sizeof path)) lstrcpyA(path, "C:\\d2d1-shim.log");
  HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char line[160];
  int n = wsprintfA(line, "%lu %s\r\n", GetCurrentProcessId(), what);
  DWORD written;
  WriteFile(f, line, n, &written, nullptr);
  CloseHandle(f);
}

/* Empty SVG document. */
class EmptySvgDocument final : public ID2D1SvgDocument {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1Resource) || riid == __uuidof(ID2D1SvgDocument)) {
      *out = static_cast<ID2D1SvgDocument *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG n = InterlockedDecrement(&refs_);
    if (!n) delete this;
    return n;
  }
  void STDMETHODCALLTYPE GetFactory(ID2D1Factory **factory) const override { Log("svg::GetFactory"); *factory = nullptr; }
  HRESULT STDMETHODCALLTYPE SetViewportSize(D2D1_SIZE_F size) override { Log("svg::SetViewportSize");
    size_ = size;
    return S_OK;
  }
  D2D1_SIZE_F *STDMETHODCALLTYPE GetViewportSize(D2D1_SIZE_F *ret) const override { Log("svg::GetViewportSize");
    *ret = size_;
    return ret;
  }
  HRESULT STDMETHODCALLTYPE SetRoot(ID2D1SvgElement *) override { Log("svg::SetRoot"); return E_NOTIMPL; }
  void STDMETHODCALLTYPE GetRoot(ID2D1SvgElement **root) override { Log("svg::GetRoot"); *root = nullptr; }
  HRESULT STDMETHODCALLTYPE FindElementById(PCWSTR, ID2D1SvgElement **element) override { Log("svg::FindElementById");
    *element = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Serialize(IStream *, ID2D1SvgElement *) override { Log("svg::Serialize"); return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Deserialize(IStream *, ID2D1SvgElement **element) override { Log("svg::Deserialize");
    *element = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreatePaint(D2D1_SVG_PAINT_TYPE, const D2D1_COLOR_F *, PCWSTR, ID2D1SvgPaint **paint) override { Log("svg::CreatePaint");
    *paint = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreateStrokeDashArray(const D2D1_SVG_LENGTH *, UINT32, ID2D1SvgStrokeDashArray **array) override { Log("svg::CreateStrokeDashArray");
    *array = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreatePointCollection(const D2D1_POINT_2F *, UINT32, ID2D1SvgPointCollection **points) override { Log("svg::CreatePointCollection");
    *points = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE CreatePathData(const FLOAT *, UINT32, const D2D1_SVG_PATH_COMMAND *, UINT32, ID2D1SvgPathData **data) override { Log("svg::CreatePathData");
    *data = nullptr;
    return E_NOTIMPL;
  }

 private:
  LONG refs_ = 1;
  D2D1_SIZE_F size_ = {0, 0};
};

static HRESULT STDMETHODCALLTYPE EmptyCreateSvgDocument(void *, IStream *, D2D1_SIZE_F size, ID2D1SvgDocument **doc) {
  Log("CreateSvgDocument");
  if (!doc) return E_POINTER;
  auto *made = new EmptySvgDocument();
  made->SetViewportSize(size);
  *doc = made;
  return S_OK;
}

/* Table patching. */
static void **SwapSlot(void **table, size_t index, void *replacement, void **original) {
  if (table[index] == replacement) return nullptr;
  DWORD old;
  if (!VirtualProtect(&table[index], sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
  if (original) *original = table[index];
  table[index] = replacement;
  VirtualProtect(&table[index], sizeof(void *), old, &old);
  return &table[index];
}

/* Index of a virtual method, from a pointer to it (Itanium C++ ABI layout). */
template <typename Fn>
static size_t SlotOf(Fn method) {
  union {
    Fn fn;
    struct {
      size_t ptr;
      ptrdiff_t adj;
    } raw;
  } u = {method};
  return (u.raw.ptr - 1) / sizeof(void *);
}

/* Hide the SVG-capable interfaces, so Office draws its icons without SVG. */
static HRESULT(STDMETHODCALLTYPE *g_orig_query)(void *, REFIID, void **);

static HRESULT STDMETHODCALLTYPE ContextQueryInterface(void *self, REFIID riid, void **out) {
  if (riid == __uuidof(ID2D1DeviceContext5) || riid == __uuidof(ID2D1DeviceContext6)) {
    Log("QueryInterface hidden: ID2D1DeviceContext5/6");
    if (out) *out = nullptr;
    return E_NOINTERFACE;
  }
  return g_orig_query(self, riid, out);
}

/* Pixel unit mode. Wine stores D2D1_UNIT_MODE_PIXELS but keeps scaling
 * everything by dpi / 96, so at any DPI above 96 Office's symbol-font icons
 * (formula bar, scroll bars, sheet-tab splitter) land outside their boxes and
 * the empty rest shows black. While a context is in pixel mode, Wine's DPI is
 * held at 96; GetDpi still reports the DPI Office set. */
static void(STDMETHODCALLTYPE *g_orig_set_dpi)(void *, FLOAT, FLOAT);
static void(STDMETHODCALLTYPE *g_orig_get_dpi)(void *, FLOAT *, FLOAT *);
static void(STDMETHODCALLTYPE *g_orig_set_unit_mode)(void *, D2D1_UNIT_MODE);
static D2D1_UNIT_MODE(STDMETHODCALLTYPE *g_orig_get_unit_mode)(void *);
static void(STDMETHODCALLTYPE *g_orig_restore_state)(void *, ID2D1DrawingStateBlock *);

struct PixelState {
  void *context;
  FLOAT dpi_x, dpi_y;
  bool pixels;
};
static PixelState g_states[64];
static LONG g_next_state;
static CRITICAL_SECTION g_states_lock;

/* Caller holds g_states_lock. */
static PixelState *StateOf(void *context, bool create) {
  for (auto &s : g_states)
    if (s.context == context) return &s;
  if (!create) return nullptr;
  PixelState *s = &g_states[(g_next_state++) % 64];
  s->context = context;
  s->pixels = false;
  g_orig_get_dpi(context, &s->dpi_x, &s->dpi_y);
  return s;
}

static void ApplyUnitMode(void *context, D2D1_UNIT_MODE mode) {
  EnterCriticalSection(&g_states_lock);
  PixelState *s = StateOf(context, true);
  if (mode == D2D1_UNIT_MODE_PIXELS && !s->pixels) {
    g_orig_get_dpi(context, &s->dpi_x, &s->dpi_y);
    s->pixels = true;
    g_orig_set_dpi(context, 96.0f, 96.0f);
  } else if (mode == D2D1_UNIT_MODE_DIPS && s->pixels) {
    s->pixels = false;
    g_orig_set_dpi(context, s->dpi_x, s->dpi_y);
  }
  LeaveCriticalSection(&g_states_lock);
}

static void STDMETHODCALLTYPE ContextSetDpi(void *context, FLOAT x, FLOAT y) {
  EnterCriticalSection(&g_states_lock);
  PixelState *s = StateOf(context, false);
  if (s && s->pixels) {
    s->dpi_x = x;
    s->dpi_y = y;
  } else {
    if (s) {
      s->dpi_x = x;
      s->dpi_y = y;
    }
    g_orig_set_dpi(context, x, y);
  }
  LeaveCriticalSection(&g_states_lock);
}

static void STDMETHODCALLTYPE ContextGetDpi(void *context, FLOAT *x, FLOAT *y) {
  EnterCriticalSection(&g_states_lock);
  PixelState *s = StateOf(context, false);
  if (s && s->pixels) {
    if (x) *x = s->dpi_x;
    if (y) *y = s->dpi_y;
  } else {
    g_orig_get_dpi(context, x, y);
  }
  LeaveCriticalSection(&g_states_lock);
}

static void STDMETHODCALLTYPE ContextSetUnitMode(void *context, D2D1_UNIT_MODE mode) {
  g_orig_set_unit_mode(context, mode);
  ApplyUnitMode(context, mode);
}

static void STDMETHODCALLTYPE ContextRestoreState(void *context, ID2D1DrawingStateBlock *block) {
  g_orig_restore_state(context, block);
  ApplyUnitMode(context, g_orig_get_unit_mode(context));
}

/* Axis-aligned clips. Wine turns the clip rectangle into pixels as
 * transform(rect * dpi / 96), leaving the transform's translation unscaled, so
 * at any DPI other than 96 a clip under a translated transform lands in the
 * wrong place and clips away what is drawn (Excel's scroll bars). Hand Wine a
 * transform whose translation is already scaled while it computes the clip. */
static void(STDMETHODCALLTYPE *g_orig_push_clip)(void *, const D2D1_RECT_F *, D2D1_ANTIALIAS_MODE);
static void(STDMETHODCALLTYPE *g_orig_set_transform)(void *, const D2D1_MATRIX_3X2_F *);
static void(STDMETHODCALLTYPE *g_orig_get_transform)(void *, D2D1_MATRIX_3X2_F *);

static void STDMETHODCALLTYPE ContextPushClip(void *context, const D2D1_RECT_F *rect, D2D1_ANTIALIAS_MODE mode) {
  FLOAT dpi_x, dpi_y;
  g_orig_get_dpi(context, &dpi_x, &dpi_y);
  if (dpi_x == 96.0f && dpi_y == 96.0f) return g_orig_push_clip(context, rect, mode);
  D2D1_MATRIX_3X2_F saved, scaled;
  g_orig_get_transform(context, &saved);
  scaled = saved;
  scaled._31 *= dpi_x / 96.0f;
  scaled._32 *= dpi_y / 96.0f;
  g_orig_set_transform(context, &scaled);
  g_orig_push_clip(context, rect, mode);
  g_orig_set_transform(context, &saved);
}

/* Command lists. Office records some drawing into an ID2D1CommandList and
 * then draws the list onto the real target. Wine records the commands but
 * fails EndDraw on a command-list target, and its DrawImage only draws
 * bitmaps, so the recording is lost and the area stays empty. EndDraw on a
 * command list succeeds here, and DrawImage of a command list replays it
 * through ID2D1CommandList::Stream into a sink that forwards every command to
 * the target, offset by the image position and clipped to the image rect. */
class ReplaySink final : public ID2D1CommandSink {
 public:
  ReplaySink(ID2D1DeviceContext *target, const D2D1_MATRIX_3X2_F &base) : target_(target), base_(base) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1CommandSink)) {
      *out = static_cast<ID2D1CommandSink *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE BeginDraw() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE EndDraw() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetAntialiasMode(D2D1_ANTIALIAS_MODE mode) override { target_->SetAntialiasMode(mode); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetTags(D2D1_TAG tag1, D2D1_TAG tag2) override { target_->SetTags(tag1, tag2); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE mode) override { target_->SetTextAntialiasMode(mode); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetTextRenderingParams(IDWriteRenderingParams *params) override { target_->SetTextRenderingParams(params); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetTransform(const D2D1_MATRIX_3X2_F *transform) override {
    D2D1::Matrix3x2F combined = *D2D1::Matrix3x2F::ReinterpretBaseType(transform) * *D2D1::Matrix3x2F::ReinterpretBaseType(&base_);
    target_->SetTransform(combined);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND blend) override { target_->SetPrimitiveBlend(blend); return S_OK; }
  HRESULT STDMETHODCALLTYPE SetUnitMode(D2D1_UNIT_MODE mode) override { target_->SetUnitMode(mode); return S_OK; }
  HRESULT STDMETHODCALLTYPE Clear(const D2D1_COLOR_F *color) override {
    (void)color; /* clearing the whole target would wipe more than the image; recordings start transparent */
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(D2D1_POINT_2F origin, const DWRITE_GLYPH_RUN *run, const DWRITE_GLYPH_RUN_DESCRIPTION *desc,
                                         ID2D1Brush *brush, DWRITE_MEASURING_MODE mode) override {
    target_->DrawGlyphRun(origin, run, desc, brush, mode);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawLine(D2D1_POINT_2F p0, D2D1_POINT_2F p1, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style) override {
    target_->DrawLine(p0, p1, brush, width, style);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGeometry(ID2D1Geometry *geometry, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style) override {
    target_->DrawGeometry(geometry, brush, width, style);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawRectangle(const D2D1_RECT_F *rect, ID2D1Brush *brush, FLOAT width, ID2D1StrokeStyle *style) override {
    target_->DrawRectangle(rect, brush, width, style);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawBitmap(ID2D1Bitmap *bitmap, const D2D1_RECT_F *dst, FLOAT opacity, D2D1_INTERPOLATION_MODE mode,
                                       const D2D1_RECT_F *src, const D2D1_MATRIX_4X4_F *perspective) override {
    target_->DrawBitmap(bitmap, dst, opacity, mode, src, perspective);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawImage(ID2D1Image *image, const D2D1_POINT_2F *offset, const D2D1_RECT_F *rect,
                                      D2D1_INTERPOLATION_MODE mode, D2D1_COMPOSITE_MODE composite) override {
    target_->DrawImage(image, offset, rect, mode, composite);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGdiMetafile(ID2D1GdiMetafile *metafile, const D2D1_POINT_2F *offset) override {
    target_->DrawGdiMetafile(metafile, offset);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FillMesh(ID2D1Mesh *mesh, ID2D1Brush *brush) override { target_->FillMesh(mesh, brush); return S_OK; }
  HRESULT STDMETHODCALLTYPE FillOpacityMask(ID2D1Bitmap *mask, ID2D1Brush *brush, const D2D1_RECT_F *dst, const D2D1_RECT_F *src) override {
    target_->FillOpacityMask(mask, brush, dst, src);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FillGeometry(ID2D1Geometry *geometry, ID2D1Brush *brush, ID2D1Brush *opacity) override {
    target_->FillGeometry(geometry, brush, opacity);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE FillRectangle(const D2D1_RECT_F *rect, ID2D1Brush *brush) override { target_->FillRectangle(rect, brush); return S_OK; }
  HRESULT STDMETHODCALLTYPE PushAxisAlignedClip(const D2D1_RECT_F *rect, D2D1_ANTIALIAS_MODE mode) override {
    target_->PushAxisAlignedClip(rect, mode);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PushLayer(const D2D1_LAYER_PARAMETERS1 *params, ID2D1Layer *layer) override {
    target_->PushLayer(params, layer);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE PopAxisAlignedClip() override { target_->PopAxisAlignedClip(); return S_OK; }
  HRESULT STDMETHODCALLTYPE PopLayer() override { target_->PopLayer(); return S_OK; }

 private:
  ID2D1DeviceContext *target_;
  D2D1_MATRIX_3X2_F base_;
};

static HRESULT(STDMETHODCALLTYPE *g_orig_end_draw)(void *, D2D1_TAG *, D2D1_TAG *);
static void(STDMETHODCALLTYPE *g_orig_draw_image)(void *, ID2D1Image *, const D2D1_POINT_2F *, const D2D1_RECT_F *, D2D1_INTERPOLATION_MODE,
                                                  D2D1_COMPOSITE_MODE);

static bool TargetIsCommandList(ID2D1DeviceContext *context) {
  ID2D1Image *target = nullptr;
  context->GetTarget(&target);
  if (!target) return false;
  ID2D1CommandList *list = nullptr;
  bool is_list = SUCCEEDED(target->QueryInterface(__uuidof(ID2D1CommandList), reinterpret_cast<void **>(&list)));
  if (list) list->Release();
  target->Release();
  return is_list;
}

static HRESULT STDMETHODCALLTYPE ContextEndDraw(void *self, D2D1_TAG *tag1, D2D1_TAG *tag2) {
  if (TargetIsCommandList(static_cast<ID2D1DeviceContext *>(self))) {
    if (tag1) *tag1 = 0;
    if (tag2) *tag2 = 0;
    return S_OK;
  }
  return g_orig_end_draw(self, tag1, tag2);
}

static void STDMETHODCALLTYPE ContextDrawImage(void *self, ID2D1Image *image, const D2D1_POINT_2F *offset, const D2D1_RECT_F *rect,
                                               D2D1_INTERPOLATION_MODE mode, D2D1_COMPOSITE_MODE composite) {
  auto *context = static_cast<ID2D1DeviceContext *>(self);
  ID2D1CommandList *list = nullptr;
  if (!image || TargetIsCommandList(context) ||
      FAILED(image->QueryInterface(__uuidof(ID2D1CommandList), reinterpret_cast<void **>(&list))))
    return g_orig_draw_image(self, image, offset, rect, mode, composite);

  D2D1_MATRIX_3X2_F saved;
  context->GetTransform(&saved);
  D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Translation(offset ? offset->x : 0.0f, offset ? offset->y : 0.0f) *
                          *D2D1::Matrix3x2F::ReinterpretBaseType(&saved);
  context->SetTransform(base);
  if (rect) context->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
  ReplaySink sink(context, base);
  list->Stream(&sink);
  if (rect) context->PopAxisAlignedClip();
  context->SetTransform(saved);
  list->Release();
}

static void PatchContext(IUnknown *context) {
  if (!context) return;
  void **table = *reinterpret_cast<void ***>(context);
  SwapSlot(table, 0, reinterpret_cast<void *>(ContextQueryInterface), reinterpret_cast<void **>(&g_orig_query));
  SwapSlot(table, SlotOf(&ID2D1DeviceContext5::CreateSvgDocument), reinterpret_cast<void *>(EmptyCreateSvgDocument), nullptr);

  g_orig_get_unit_mode = reinterpret_cast<decltype(g_orig_get_unit_mode)>(table[SlotOf(&ID2D1DeviceContext::GetUnitMode)]);
  SwapSlot(table, SlotOf(&ID2D1RenderTarget::SetDpi), reinterpret_cast<void *>(ContextSetDpi), reinterpret_cast<void **>(&g_orig_set_dpi));
  SwapSlot(table, SlotOf(&ID2D1RenderTarget::GetDpi), reinterpret_cast<void *>(ContextGetDpi), reinterpret_cast<void **>(&g_orig_get_dpi));
  SwapSlot(table, SlotOf(&ID2D1DeviceContext::SetUnitMode), reinterpret_cast<void *>(ContextSetUnitMode), reinterpret_cast<void **>(&g_orig_set_unit_mode));
  SwapSlot(table, SlotOf(&ID2D1RenderTarget::RestoreDrawingState), reinterpret_cast<void *>(ContextRestoreState), reinterpret_cast<void **>(&g_orig_restore_state));

  g_orig_set_transform = reinterpret_cast<decltype(g_orig_set_transform)>(
      table[SlotOf(static_cast<void (STDMETHODCALLTYPE ID2D1RenderTarget::*)(const D2D1_MATRIX_3X2_F *)>(&ID2D1RenderTarget::SetTransform))]);
  g_orig_get_transform = reinterpret_cast<decltype(g_orig_get_transform)>(
      table[SlotOf(static_cast<void (STDMETHODCALLTYPE ID2D1RenderTarget::*)(D2D1_MATRIX_3X2_F *) const>(&ID2D1RenderTarget::GetTransform))]);
  SwapSlot(table, SlotOf(static_cast<void (STDMETHODCALLTYPE ID2D1RenderTarget::*)(const D2D1_RECT_F *, D2D1_ANTIALIAS_MODE)>(&ID2D1RenderTarget::PushAxisAlignedClip)),
           reinterpret_cast<void *>(ContextPushClip), reinterpret_cast<void **>(&g_orig_push_clip));

  SwapSlot(table, SlotOf(static_cast<HRESULT (STDMETHODCALLTYPE ID2D1RenderTarget::*)(D2D1_TAG *, D2D1_TAG *)>(&ID2D1RenderTarget::EndDraw)),
           reinterpret_cast<void *>(ContextEndDraw), reinterpret_cast<void **>(&g_orig_end_draw));
  SwapSlot(table,
           SlotOf(static_cast<void (STDMETHODCALLTYPE ID2D1DeviceContext::*)(ID2D1Image *, const D2D1_POINT_2F *, const D2D1_RECT_F *,
                                                                              D2D1_INTERPOLATION_MODE, D2D1_COMPOSITE_MODE)>(&ID2D1DeviceContext::DrawImage)),
           reinterpret_cast<void *>(ContextDrawImage), reinterpret_cast<void **>(&g_orig_draw_image));

  /* A new context may reuse the address of a released one. */
  EnterCriticalSection(&g_states_lock);
  if (PixelState *s = StateOf(context, false)) s->context = nullptr;
  LeaveCriticalSection(&g_states_lock);
}

/* One hook per overload of Device::CreateDeviceContext; N picks the slot and
 * the saved original. */
static const int kLevels = 7;
static CreateContextFnPtr g_orig_context[kLevels];
static CreateDeviceFnPtr g_orig_device[kLevels];

template <int N>
struct ContextHook {
  static HRESULT STDMETHODCALLTYPE Call(void *self, D2D1_DEVICE_CONTEXT_OPTIONS options, void **out) {
    HRESULT hr = g_orig_context[N](self, options, out);
    if (SUCCEEDED(hr) && out && *out) PatchContext(static_cast<IUnknown *>(*out));
    return hr;
  }
};

static void PatchDevice(IUnknown *device);

template <int N>
struct DeviceHook {
  static HRESULT STDMETHODCALLTYPE Call(void *self, IDXGIDevice *dxgi, void **out) {
    HRESULT hr = g_orig_device[N](self, dxgi, out);
    if (SUCCEEDED(hr) && out && *out) PatchDevice(static_cast<IUnknown *>(*out));
    return hr;
  }
};

#define HOOK_CONTEXT(N, DEVICE, CONTEXT)                                                                                          \
  {                                                                                                                               \
    auto pm = static_cast<HRESULT (STDMETHODCALLTYPE DEVICE::*)(D2D1_DEVICE_CONTEXT_OPTIONS, CONTEXT **)>(&DEVICE::CreateDeviceContext); \
    SwapSlot(table, SlotOf(pm), reinterpret_cast<void *>(ContextHook<N>::Call), reinterpret_cast<void **>(&g_orig_context[N]));  \
  }

static void PatchDevice(IUnknown *device) {
  if (!device) return;
  void **table = *reinterpret_cast<void ***>(device);
  if (g_orig_context[0] && table[SlotOf(static_cast<HRESULT (STDMETHODCALLTYPE ID2D1Device::*)(D2D1_DEVICE_CONTEXT_OPTIONS, ID2D1DeviceContext **)>(&ID2D1Device::CreateDeviceContext))] ==
                               reinterpret_cast<void *>(ContextHook<0>::Call))
    return;
  HOOK_CONTEXT(0, ID2D1Device, ID2D1DeviceContext)
  HOOK_CONTEXT(1, ID2D1Device1, ID2D1DeviceContext1)
  HOOK_CONTEXT(2, ID2D1Device2, ID2D1DeviceContext2)
  HOOK_CONTEXT(3, ID2D1Device3, ID2D1DeviceContext3)
  HOOK_CONTEXT(4, ID2D1Device4, ID2D1DeviceContext4)
  HOOK_CONTEXT(5, ID2D1Device5, ID2D1DeviceContext5)
  HOOK_CONTEXT(6, ID2D1Device6, ID2D1DeviceContext6)
}

/* Rectangle CombineWithGeometry. Wine's is a stub that fails, and Office works
 * out the area it repaints as a rectangle minus another shape, so parts of the
 * window stay unpainted (black) until hovering redraws them another way.
 * Rectangle against an axis-aligned rectangle is computed exactly; any other
 * shape is written as an even-odd pair of outlines, which is exact when the
 * shape lies inside the rectangle, the case Office uses. */
static HRESULT(STDMETHODCALLTYPE *g_orig_create_rect)(void *, const D2D1_RECT_F *, ID2D1RectangleGeometry **);

static FLOAT Lo(FLOAT a, FLOAT b) { return a < b ? a : b; }
static FLOAT Hi(FLOAT a, FLOAT b) { return a > b ? a : b; }

static void AddRect(ID2D1SimplifiedGeometrySink *sink, const D2D1_RECT_F &r) {
  if (r.right <= r.left || r.bottom <= r.top) return;
  D2D1_POINT_2F points[3] = {{r.right, r.top}, {r.right, r.bottom}, {r.left, r.bottom}};
  sink->BeginFigure(D2D1::Point2F(r.left, r.top), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLines(points, 3);
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

/* a minus b, as up to four rectangles. */
static void AddDifference(ID2D1SimplifiedGeometrySink *sink, const D2D1_RECT_F &a, const D2D1_RECT_F &b) {
  D2D1_RECT_F i = D2D1::RectF(Hi(a.left, b.left), Hi(a.top, b.top), Lo(a.right, b.right), Lo(a.bottom, b.bottom));
  if (i.right <= i.left || i.bottom <= i.top) return AddRect(sink, a);
  AddRect(sink, D2D1::RectF(a.left, a.top, a.right, i.top));
  AddRect(sink, D2D1::RectF(a.left, i.bottom, a.right, a.bottom));
  AddRect(sink, D2D1::RectF(a.left, i.top, i.left, i.bottom));
  AddRect(sink, D2D1::RectF(i.right, i.top, a.right, i.bottom));
}

static HRESULT STDMETHODCALLTYPE RectCombine(ID2D1RectangleGeometry *self, ID2D1Geometry *other, D2D1_COMBINE_MODE mode,
                                             const D2D1_MATRIX_3X2_F *transform, FLOAT tolerance,
                                             ID2D1SimplifiedGeometrySink *sink) {
  if (!other || !sink) return E_INVALIDARG;
  D2D1_RECT_F a;
  self->GetRect(&a);

  ID2D1RectangleGeometry *other_rect = nullptr;
  bool axis_aligned = !transform || (transform->_12 == 0.0f && transform->_21 == 0.0f);
  if (axis_aligned && SUCCEEDED(other->QueryInterface(__uuidof(ID2D1RectangleGeometry), reinterpret_cast<void **>(&other_rect)))) {
    D2D1_RECT_F b;
    other_rect->GetRect(&b);
    other_rect->Release();
    if (transform) {
      FLOAT x0 = b.left * transform->_11 + transform->_31, x1 = b.right * transform->_11 + transform->_31;
      FLOAT y0 = b.top * transform->_22 + transform->_32, y1 = b.bottom * transform->_22 + transform->_32;
      b = D2D1::RectF(Lo(x0, x1), Lo(y0, y1), Hi(x0, x1), Hi(y0, y1));
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    switch (mode) {
      case D2D1_COMBINE_MODE_UNION:
        AddRect(sink, a);
        AddDifference(sink, b, a);
        break;
      case D2D1_COMBINE_MODE_INTERSECT:
        AddRect(sink, D2D1::RectF(Hi(a.left, b.left), Hi(a.top, b.top), Lo(a.right, b.right), Lo(a.bottom, b.bottom)));
        break;
      case D2D1_COMBINE_MODE_XOR:
        AddDifference(sink, a, b);
        AddDifference(sink, b, a);
        break;
      default:
        AddDifference(sink, a, b);
        break;
    }
    return S_OK;
  }

  if (mode == D2D1_COMBINE_MODE_INTERSECT)
    return other->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, transform, tolerance, sink);
  HRESULT hr = other->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, transform, tolerance, sink);
  sink->SetFillMode(mode == D2D1_COMBINE_MODE_UNION ? D2D1_FILL_MODE_WINDING : D2D1_FILL_MODE_ALTERNATE);
  AddRect(sink, a);
  return hr;
}

static HRESULT STDMETHODCALLTYPE CreateRect(void *factory, const D2D1_RECT_F *rect, ID2D1RectangleGeometry **out) {
  HRESULT hr = g_orig_create_rect(factory, rect, out);
  if (SUCCEEDED(hr) && out && *out) {
    void **table = *reinterpret_cast<void ***>(*out);
    SwapSlot(table,
             SlotOf(static_cast<HRESULT (STDMETHODCALLTYPE ID2D1Geometry::*)(ID2D1Geometry *, D2D1_COMBINE_MODE, const D2D1_MATRIX_3X2_F *, FLOAT, ID2D1SimplifiedGeometrySink *) const>(
                 &ID2D1Geometry::CombineWithGeometry)),
             reinterpret_cast<void *>(RectCombine), nullptr);
  }
  return hr;
}

/* Geometry groups. Wine merges a group's members into one path and works out
 * where its outlines cross. Two members with an identical curve, as in
 * Office's tab and button borders (an outline and the same outline 1 px
 * shorter), make that search split the curves without end: one group took
 * 40 seconds and froze the window. Lines do not have this problem, so the
 * members' curves are flattened to lines first. */
static HRESULT(STDMETHODCALLTYPE *g_orig_create_group)(void *, D2D1_FILL_MODE, ID2D1Geometry **, UINT32, ID2D1GeometryGroup **);
static HRESULT(STDMETHODCALLTYPE *g_orig_create_path)(void *, ID2D1PathGeometry **);

/* Writes a shape into a path geometry with every curve replaced by lines. */
class FlattenSink final : public ID2D1SimplifiedGeometrySink {
 public:
  explicit FlattenSink(ID2D1GeometrySink *out) : out_(out) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
      *out = static_cast<ID2D1SimplifiedGeometrySink *>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE mode) override { out_->SetFillMode(mode); }
  void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT flags) override { out_->SetSegmentFlags(flags); }
  void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN begin) override {
    out_->BeginFigure(p, begin);
    at_ = p;
  }
  void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F *p, UINT32 n) override {
    if (!n) return;
    out_->AddLines(p, n);
    at_ = p[n - 1];
  }
  void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT *b, UINT32 n) override {
    for (UINT32 i = 0; i < n; i++) Flatten(b[i]);
  }
  void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) override { out_->EndFigure(end); }
  HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

 private:
  /* Enough lines to stay within kTolerance of the curve (Wang's formula). */
  void Flatten(const D2D1_BEZIER_SEGMENT &b) {
    const FLOAT kTolerance = 0.05f;
    D2D1_POINT_2F p0 = at_, p1 = b.point1, p2 = b.point2, p3 = b.point3;
    FLOAT ax = p0.x - 2 * p1.x + p2.x, ay = p0.y - 2 * p1.y + p2.y;
    FLOAT bx = p1.x - 2 * p2.x + p3.x, by = p1.y - 2 * p2.y + p3.y;
    FLOAT m = Hi(ax * ax + ay * ay, bx * bx + by * by);
    int n = 1;
    while (n < 64 && (FLOAT)n * n * n * n * kTolerance * kTolerance < 0.5625f * m) n++;
    D2D1_POINT_2F points[64];
    for (int i = 1; i <= n; i++) {
      FLOAT t = (FLOAT)i / n, u = 1 - t;
      FLOAT c0 = u * u * u, c1 = 3 * u * u * t, c2 = 3 * u * t * t, c3 = t * t * t;
      points[i - 1] = D2D1::Point2F(c0 * p0.x + c1 * p1.x + c2 * p2.x + c3 * p3.x, c0 * p0.y + c1 * p1.y + c2 * p2.y + c3 * p3.y);
    }
    points[n - 1] = p3;
    out_->AddLines(points, n);
    at_ = p3;
  }

  ID2D1GeometrySink *out_;
  D2D1_POINT_2F at_ = {0, 0};
};

/* A path holding `geometry` with its curves as lines, or null. */
static ID2D1Geometry *Flattened(void *factory, ID2D1Geometry *geometry) {
  ID2D1PathGeometry *path = nullptr;
  if (FAILED(g_orig_create_path(factory, &path))) return nullptr;
  ID2D1GeometrySink *sink = nullptr;
  HRESULT hr = path->Open(&sink);
  if (SUCCEEDED(hr)) {
    FlattenSink flatten(sink);
    hr = geometry->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, nullptr, 0.05f, &flatten);
    HRESULT closed = sink->Close();
    if (SUCCEEDED(hr)) hr = closed;
    sink->Release();
  }
  if (FAILED(hr)) {
    path->Release();
    return nullptr;
  }
  return path;
}

static HRESULT STDMETHODCALLTYPE CreateGroup(void *factory, D2D1_FILL_MODE mode, ID2D1Geometry **geometries, UINT32 count,
                                             ID2D1GeometryGroup **out) {
  if (!geometries || !count || count > 256 || !g_orig_create_path) return g_orig_create_group(factory, mode, geometries, count, out);
  ID2D1Geometry *flat[256];
  UINT32 made = 0;
  for (; made < count; made++)
    if (!(flat[made] = Flattened(factory, geometries[made]))) break;
  HRESULT hr = made == count ? g_orig_create_group(factory, mode, flat, count, out)
                             : g_orig_create_group(factory, mode, geometries, count, out);
  for (UINT32 i = 0; i < made; i++) flat[i]->Release();
  return hr;
}

#define HOOK_DEVICE(N, FACTORY, DEVICE)                                                                                        \
  {                                                                                                                               \
    auto pm = static_cast<HRESULT (STDMETHODCALLTYPE FACTORY::*)(IDXGIDevice *, DEVICE **)>(&FACTORY::CreateDevice);               \
    SwapSlot(table, SlotOf(pm), reinterpret_cast<void *>(DeviceHook<N>::Call), reinterpret_cast<void **>(&g_orig_device[N]));      \
  }

static void PatchFactory(IUnknown *factory) {
  if (!factory) return;
  void **table = *reinterpret_cast<void ***>(factory);
  HOOK_DEVICE(0, ID2D1Factory1, ID2D1Device)
  HOOK_DEVICE(1, ID2D1Factory2, ID2D1Device1)
  HOOK_DEVICE(2, ID2D1Factory3, ID2D1Device2)
  HOOK_DEVICE(3, ID2D1Factory4, ID2D1Device3)
  HOOK_DEVICE(4, ID2D1Factory5, ID2D1Device4)
  HOOK_DEVICE(5, ID2D1Factory6, ID2D1Device5)
  HOOK_DEVICE(6, ID2D1Factory7, ID2D1Device6)
  SwapSlot(table, SlotOf(static_cast<HRESULT (STDMETHODCALLTYPE ID2D1Factory::*)(const D2D1_RECT_F *, ID2D1RectangleGeometry **)>(&ID2D1Factory::CreateRectangleGeometry)),
           reinterpret_cast<void *>(CreateRect), reinterpret_cast<void **>(&g_orig_create_rect));
  g_orig_create_path = reinterpret_cast<decltype(g_orig_create_path)>(table[SlotOf(&ID2D1Factory::CreatePathGeometry)]);
  SwapSlot(table, SlotOf(&ID2D1Factory::CreateGeometryGroup), reinterpret_cast<void *>(CreateGroup),
           reinterpret_cast<void **>(&g_orig_create_group));
}

/* Rectangle geometries share one method table, patched when Office first
 * creates one through the factory. Create one here so the CombineWithGeometry
 * fix is in place before Office draws anything. */
static void PatchEarly(IUnknown *factory) {
  static LONG done;
  if (InterlockedCompareExchange(&done, 1, 0)) return;
  ID2D1Factory *base = nullptr;
  if (FAILED(factory->QueryInterface(__uuidof(ID2D1Factory), reinterpret_cast<void **>(&base)))) return;
  ID2D1RectangleGeometry *rect = nullptr;
  D2D1_RECT_F r = D2D1::RectF(0, 0, 1, 1);
  if (SUCCEEDED(base->CreateRectangleGeometry(&r, &rect))) rect->Release();
  base->Release();
}

/* Office draws each panel (grid, ribbon, status bar) into a swap chain on its
 * own child window. Wine renders those off screen and copies them in at each
 * present, and its next repaint of the parent paints over them with black
 * until the panel presents again. So each present goes through GDI into the
 * window itself instead, where a repaint keeps it. */
struct Staging {
  ID3D11Device *device;
  UINT width, height;
  DXGI_FORMAT format;
  ID3D11Texture2D *texture;
};
static Staging g_staging[16];
static UINT g_staging_next;

static ID3D11Texture2D *StagingFor(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &desc) {
  for (Staging &s : g_staging) {
    if (s.texture && s.device == device && s.width == desc.Width && s.height == desc.Height && s.format == desc.Format) {
      return s.texture;
    }
  }
  D3D11_TEXTURE2D_DESC staging = {};
  staging.Width = desc.Width;
  staging.Height = desc.Height;
  staging.MipLevels = 1;
  staging.ArraySize = 1;
  staging.Format = desc.Format;
  staging.SampleDesc.Count = 1;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *texture = nullptr;
  if (FAILED(device->CreateTexture2D(&staging, nullptr, &texture))) return nullptr;
  Staging &slot = g_staging[g_staging_next++ % 16];
  if (slot.texture) slot.texture->Release();
  slot = {device, desc.Width, desc.Height, desc.Format, texture};
  return texture;
}

static void PresentThroughGdi(IDXGISwapChain *chain) {
  DXGI_SWAP_CHAIN_DESC desc;
  if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow) return;
  ID3D11Texture2D *back = nullptr;
  if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back)))) return;
  D3D11_TEXTURE2D_DESC td;
  back->GetDesc(&td);
  if ((td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) && td.SampleDesc.Count == 1) {
    ID3D11Device *device = nullptr;
    back->GetDevice(&device);
    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);
    if (ID3D11Texture2D *staging = StagingFor(device, td)) {
      context->CopyResource(staging, back);
      D3D11_MAPPED_SUBRESOURCE map;
      if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
        bmi.bmiHeader.biWidth = static_cast<LONG>(map.RowPitch / 4);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(td.Height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        if (HDC dc = GetDC(desc.OutputWindow)) {
          SetDIBitsToDevice(dc, 0, 0, td.Width, td.Height, 0, 0, 0, td.Height, map.pData, &bmi, DIB_RGB_COLORS);
          ReleaseDC(desc.OutputWindow, dc);
        }
        context->Unmap(staging, 0);
      }
    }
    context->Release();
    device->Release();
  }
  back->Release();
}

static HRESULT(STDMETHODCALLTYPE *g_orig_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *g_orig_present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static HRESULT(STDMETHODCALLTYPE *g_orig_create_for_hwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
                                                          IDXGISwapChain1 **);

static HRESULT STDMETHODCALLTYPE ChainPresent(IDXGISwapChain *self, UINT interval, UINT flags) {
  if (flags & DXGI_PRESENT_TEST) return g_orig_present(self, interval, flags);
  PresentThroughGdi(self);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE ChainPresent1(IDXGISwapChain1 *self, UINT interval, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *params) {
  if (flags & DXGI_PRESENT_TEST) return g_orig_present1(self, interval, flags, params);
  PresentThroughGdi(self);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE CreateForHwnd(IDXGIFactory2 *self, IUnknown *device, HWND hwnd,
                                               const DXGI_SWAP_CHAIN_DESC1 *desc,
                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGIOutput *output,
                                               IDXGISwapChain1 **out) {
  HRESULT hr = g_orig_create_for_hwnd(self, device, hwnd, desc, fullscreen, output, out);
  if (SUCCEEDED(hr) && out && *out) {
    void **table = *reinterpret_cast<void ***>(*out);
    SwapSlot(table, SlotOf(&IDXGISwapChain::Present), reinterpret_cast<void *>(ChainPresent),
             reinterpret_cast<void **>(&g_orig_present));
    SwapSlot(table, SlotOf(&IDXGISwapChain1::Present1), reinterpret_cast<void *>(ChainPresent1),
             reinterpret_cast<void **>(&g_orig_present1));
  }
  return hr;
}

static void PatchDxgi() {
  static bool done;
  if (done) return;
  done = true;
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  auto create = dxgi ? reinterpret_cast<HRESULT(WINAPI *)(REFIID, void **)>(GetProcAddress(dxgi, "CreateDXGIFactory1"))
                     : nullptr;
  IDXGIFactory2 *factory = nullptr;
  if (!create || FAILED(create(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory)))) return;
  SwapSlot(*reinterpret_cast<void ***>(factory), SlotOf(&IDXGIFactory2::CreateSwapChainForHwnd),
           reinterpret_cast<void *>(CreateForHwnd), reinterpret_cast<void **>(&g_orig_create_for_hwnd));
  factory->Release();
}

static FARPROC RealExport(const char *name) {
  static HMODULE real;
  if (!real) real = LoadLibraryW(L"d2d1w.dll");
  return real ? GetProcAddress(real, name) : nullptr;
}

extern "C" {

HRESULT WINAPI D2D1CreateFactory(D2D1_FACTORY_TYPE type, REFIID riid, const D2D1_FACTORY_OPTIONS *options, void **out) {
  auto fn = reinterpret_cast<CreateFactoryFn>(RealExport("D2D1CreateFactory"));
  if (!fn) return E_FAIL;
  HRESULT hr = fn(type, riid, options, out);
  if (SUCCEEDED(hr) && out && *out) {
    PatchFactory(static_cast<IUnknown *>(*out));
    PatchEarly(static_cast<IUnknown *>(*out));
    PatchDxgi();
  }
  return hr;
}

HRESULT WINAPI D2D1CreateDevice(IDXGIDevice *dxgi, const D2D1_CREATION_PROPERTIES *props, ID2D1Device **out) {
  auto fn = reinterpret_cast<CreateDeviceFn>(RealExport("D2D1CreateDevice"));
  if (!fn) return E_FAIL;
  HRESULT hr = fn(dxgi, props, out);
  if (SUCCEEDED(hr) && out && *out) PatchDevice(*out);
  return hr;
}

HRESULT WINAPI D2D1CreateDeviceContext(IDXGISurface *surface, const D2D1_CREATION_PROPERTIES *props, ID2D1DeviceContext **out) {
  auto fn = reinterpret_cast<CreateDeviceContextFn>(RealExport("D2D1CreateDeviceContext"));
  if (!fn) return E_FAIL;
  HRESULT hr = fn(surface, props, out);
  if (SUCCEEDED(hr) && out && *out) PatchContext(*out);
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_states_lock);
  }
  return TRUE;
}

}  // extern "C"
