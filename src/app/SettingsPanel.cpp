#include "SettingsPanel.h"

#include "Icons.h"

using namespace snow;
using D2D1::ColorF;
using D2D1::Point2F;
using D2D1::RectF;

namespace {

constexpr wchar_t kPanelClass[] = L"TryflakePanel";
constexpr float kWidth = 380;
constexpr float kPad = 20;

inline D2D1_COLOR_F rgba(float r, float g, float b, float a = 1) { return D2D1_COLOR_F{r, g, b, a}; }
inline D2D1_COLOR_F white(float a) { return rgba(1, 1, 1, a); }
const D2D1_COLOR_F kAccent = rgba(0.61f, 0.80f, 0.73f);
const D2D1_COLOR_F kBackground = rgba(0.10f, 0.12f, 0.13f);
const D2D1_COLOR_F kPrimary = white(0.92f);
const D2D1_COLOR_F kSecondary = white(0.56f);
const D2D1_COLOR_F kTertiary = white(0.33f);
const D2D1_COLOR_F kWarning = rgba(1.0f, 0.62f, 0.25f);
inline D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float a) { return D2D1_COLOR_F{c.r, c.g, c.b, c.a * a}; }

bool contains(const D2D1_RECT_F& r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

std::wstring pickFamily(IDWriteFactory* dwrite, std::initializer_list<const wchar_t*> names) {
    ComPtr<IDWriteFontCollection> fonts;
    if (dwrite && SUCCEEDED(dwrite->GetSystemFontCollection(&fonts))) {
        for (const auto* name : names) {
            UINT32 index;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists) return name;
        }
    }
    return *(names.end() - 1);
}

// Глифы Segoe Fluent Icons / Segoe MDL2 Assets.
constexpr wchar_t kGlyphPlay = 0xE768;
constexpr wchar_t kGlyphPause = 0xE769;
constexpr wchar_t kGlyphDelete = 0xE74D;
constexpr wchar_t kGlyphDown = 0xE70D;
constexpr wchar_t kGlyphUp = 0xE70E;
constexpr wchar_t kGlyphWarning = 0xE7BA;
constexpr wchar_t kGlyphCancel = 0xE711;

}  // namespace

void SettingsPanel::registerClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPanelClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
}

SettingsPanel::SettingsPanel(GpuContext& gpu, SettingsStore& store, PanelActions actions)
    : gpu_(gpu), store_(store), actions_(std::move(actions)) {
    textFamily_ = pickFamily(gpu_.dwrite.Get(), {L"Segoe UI Variable Text", L"Segoe UI"});
    displayFamily_ = pickFamily(gpu_.dwrite.Get(), {L"Segoe UI Variable Display", L"Segoe UI"});
    iconFamily_ = pickFamily(gpu_.dwrite.Get(), {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    previewWorld_ = std::make_unique<SnowWorld>(340, 144, 1926);
    previewWorld_->setSurfaces(SurfaceResolver::resolve(
        {{1, Rect{20, 56, 171, 92}}, {2, Rect{205, 92, 112, 62}}, {3, Rect{116, 129, 108, 16}}}, 340, 144));
    previewWorld_->warmStart();
    previewWorld_->seedSnow(5);
}

SettingsPanel::~SettingsPanel() {
    releaseTargets();
    dc_.Reset();
    swapChain_.Reset();
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

// ================================================================= окно и цели

bool SettingsPanel::ensureWindow() {
    if (hwnd_) return true;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass, L"Tryflake — настройки снега", WS_POPUP, 0,
                            0, 380, 600, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const COLORREF border = RGB(52, 58, 61);
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    return true;
}

void SettingsPanel::releaseTargets() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    previewBitmap_.Reset();
    previewRTV_.Reset();
    previewTexture_.Reset();
}

bool SettingsPanel::ensureTargets() {
    if (!hwnd_ || !gpu_.alive()) return false;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = std::max<LONG>(1, rc.right - rc.left), h = std::max<LONG>(1, rc.bottom - rc.top);
    if (!dc_) {
        if (FAILED(gpu_.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
        dc_->CreateSolidColorBrush(kPrimary, &brush_);
        const float dashes[] = {2, 4};
        gpu_.d2dFactory->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0),
            dashes, 2, &dash_);
        gpu_.d2dFactory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                       D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                           nullptr, 0, &round_);
    }
    if (!swapChain_) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = w;
        desc.Height = h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(gpu_.factory->CreateSwapChainForHwnd(gpu_.device.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_)))
            return false;
        gpu_.factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        pixelWidth_ = w;
        pixelHeight_ = h;
    }
    if (w != pixelWidth_ || h != pixelHeight_) {
        dc_->SetTarget(nullptr);
        target_.Reset();
        if (FAILED(swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return false;
        pixelWidth_ = w;
        pixelHeight_ = h;
    }
    const float dpi = 96.0f * scale_;
    if (!target_) {
        ComPtr<IDXGISurface> surface;
        swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface));
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                   dpi, dpi);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
    }
    if (!previewTexture_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(std::ceil(340 * scale_));
        desc.Height = static_cast<UINT>(std::ceil(144 * scale_));
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(gpu_.device->CreateTexture2D(&desc, nullptr, &previewTexture_))) {
            gpu_.device->CreateRenderTargetView(previewTexture_.Get(), nullptr, &previewRTV_);
            ComPtr<IDXGISurface> surface;
            previewTexture_.As(&surface);
            const auto props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                dpi, dpi);
            dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &previewBitmap_);
        }
    }
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
    return true;
}

// ================================================================= показ

void SettingsPanel::show(const RECT& anchor) {
    if (!ensureWindow()) return;
    anchor_ = anchor;
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float scale = dpiX / 96.0f;
    if (std::abs(scale - scale_) > 0.001f) {
        releaseTargets();
        scale_ = scale;
    }
    const RECT work = mi.rcWork;
    maxHeight_ = (work.bottom - work.top) / scale_ - 24;
    contentHeight_ = layout(false);
    viewHeight_ = std::min(contentHeight_, maxHeight_);
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentHeight_ - viewHeight_));
    const int w = static_cast<int>(std::lround(kWidth * scale_));
    const int h = static_cast<int>(std::lround(viewHeight_ * scale_));
    const int margin = static_cast<int>(std::lround(12 * scale_));
    const int centerX = (anchor.left + anchor.right) / 2;
    const int x = std::clamp(centerX - w / 2, static_cast<int>(work.left) + margin,
                             std::max(static_cast<int>(work.left) + margin, static_cast<int>(work.right) - margin - w));
    // Панель задач сверху — открываемся вниз, иначе над панелью задач (как флайауты Windows 11).
    const bool taskbarTop = anchor.bottom <= work.top + 2;
    const int y = taskbarTop ? work.top + margin : work.bottom - margin - h;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    focus_ = None;
    focusVisible_ = false;
    lastPreviewFrame_ = 0;
    previewTime_ = 0;
    render();
}

void SettingsPanel::hide() {
    if (!visible()) return;
    if (GetCapture() == hwnd_) ReleaseCapture();
    pressed_ = None;
    hover_ = None;
    ShowWindow(hwnd_, SW_HIDE);
    hiddenAt_ = nowSeconds();
}

void SettingsPanel::refresh() {
    if (visible()) render();
}

bool SettingsPanel::animating() const { return visible() && store_.enabled && !store_.paused; }

void SettingsPanel::frame(double now) {
    if (!animating()) return;
    const double interval = store_.economyActive ? 1.0 / 20 : 1.0 / 30;
    if (now - lastPreviewFrame_ < interval - 0.002) return;
    previewWorld_->update(previewTime_ == 0 ? 1.0 / 30 : now - previewTime_);
    previewTime_ = now;
    lastPreviewFrame_ = now;
    render();
}

void SettingsPanel::resizeToContent() {
    if (!visible()) return;
    const float wanted = std::min(contentHeight_, maxHeight_);
    if (std::abs(wanted - viewHeight_) < 0.5f) return;
    RECT rc;
    GetWindowRect(hwnd_, &rc);
    const bool taskbarTop = anchor_.bottom <= rc.top;
    const int h = static_cast<int>(std::lround(wanted * scale_));
    const int y = taskbarTop ? rc.top : rc.bottom - h;
    viewHeight_ = wanted;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentHeight_ - viewHeight_));
    SetWindowPos(hwnd_, nullptr, rc.left, y, rc.right - rc.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ================================================================= отрисовка

void SettingsPanel::render() {
    if (!visible() || !ensureTargets()) return;
    store_.updateEnergyState();
    if (previewWorld_ && previewRTV_) {
        auto config = store_.configuration();
        config.maxDepth = 7;
        previewWorld_->configuration = config;
        SnowMesh::build(*previewWorld_, store_.enabled ? store_.prefs.opacity : 0, previewVertices_);
        D3D11_TEXTURE2D_DESC desc;
        previewTexture_->GetDesc(&desc);
        const float clear[4] = {0, 0, 0, 0};
        gpu_.draw(previewRTV_.Get(), desc.Width, desc.Height, 340, 144, previewVertices_, clear);
    }
    dc_->BeginDraw();
    dc_->Clear(kBackground);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    dc_->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll_));
    const float height = layout(true);
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (contentHeight_ > viewHeight_ + 0.5f) {
        // Тонкий индикатор прокрутки в стиле Windows 11.
        const float track = viewHeight_ - 16;
        const float thumb = std::max(24.0f, track * viewHeight_ / contentHeight_);
        const float pos = 8 + (track - thumb) * (scroll_ / std::max(1.0f, contentHeight_ - viewHeight_));
        fill(RectF(kWidth - 6, pos, kWidth - 3, pos + thumb), 1.5f, white(0.25f));
    }
    const HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        releaseTargets();
        return;
    }
    swapChain_->Present(0, 0);
    if (std::abs(height - contentHeight_) > 0.5f) {
        contentHeight_ = height;
        resizeToContent();
    }
}

IDWriteTextFormat* SettingsPanel::format(float size, DWRITE_FONT_WEIGHT weight, bool display, bool icons) {
    const uint64_t key = static_cast<uint64_t>(size * 100) | (static_cast<uint64_t>(weight) << 20) |
                         (static_cast<uint64_t>(display) << 32) | (static_cast<uint64_t>(icons) << 33);
    auto& slot = formats_[key];
    if (!slot) {
        const std::wstring& family = icons ? iconFamily_ : (display ? displayFamily_ : textFamily_);
        gpu_.dwrite->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", &slot);
    }
    return slot.Get();
}

static bool g_paint = false;

float SettingsPanel::text(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT weight,
                          D2D1_COLOR_F color, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para,
                          float tracking, bool wrap, bool display) {
    ComPtr<IDWriteTextLayout> layoutObj;
    if (FAILED(gpu_.dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), format(size, weight, display),
                                             std::max(1.0f, r.right - r.left), std::max(1.0f, r.bottom - r.top),
                                             &layoutObj)))
        return 0;
    layoutObj->SetTextAlignment(align);
    layoutObj->SetParagraphAlignment(para);
    layoutObj->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (tracking != 0) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layoutObj.As(&layout1)))
            layout1->SetCharacterSpacing(0, tracking, 0, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(s.size())});
    }
    DWRITE_TEXT_METRICS metrics{};
    layoutObj->GetMetrics(&metrics);
    if (g_paint) {
        brush_->SetColor(color);
        dc_->DrawTextLayout(Point2F(r.left, r.top), layoutObj.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    return wrap ? metrics.height : metrics.widthIncludingTrailingWhitespace;
}

void SettingsPanel::glyph(wchar_t code, D2D1_RECT_F r, float size, D2D1_COLOR_F color) {
    if (!g_paint) return;
    const wchar_t s[2] = {code, 0};
    brush_->SetColor(color);
    dc_->DrawTextW(s, 1, format(size, DWRITE_FONT_WEIGHT_NORMAL, false, true), r, brush_.Get(),
                   D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void SettingsPanel::fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    if (!g_paint) return;
    brush_->SetColor(c);
    if (radius > 0)
        dc_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    else
        dc_->FillRectangle(r, brush_.Get());
}

void SettingsPanel::stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width) {
    if (!g_paint) return;
    brush_->SetColor(c);
    const float inset = width / 2;
    const D2D1_RECT_F rr = RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(rr, radius, radius), brush_.Get(), width);
}

void SettingsPanel::reg(Id id, D2D1_RECT_F r, bool enabled) {
    regions_.push_back(Region{id, r, enabled});
    if (enabled) focusOrder_.push_back(id);
}

float SettingsPanel::alphaFor(Id id) const {
    if (pressed_ == id && hover_ == id) return 0.05f;
    if (hover_ == id && pressed_ == None) return 0.11f;
    return 0.07f;
}

void SettingsPanel::focusRing(Id id, D2D1_RECT_F r, float radius) {
    if (!focusVisible_ || focus_ != id) return;
    stroke(RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), radius + 3, white(0.9f), 2);
}

void SettingsPanel::toggle(Id id, D2D1_RECT_F r, bool on, bool enabled) {
    reg(id, r, enabled);
    const float a = enabled ? 1.0f : 0.4f;
    const float h = r.bottom - r.top, cy = (r.top + r.bottom) / 2;
    const bool hot = hover_ == id && enabled;
    const float knob = (pressed_ == id && hot) ? 7.0f : (hot ? 6.5f : 5.5f);
    if (on) {
        fill(r, h / 2, withAlpha(kAccent, a));
        if (g_paint) {
            brush_->SetColor(withAlpha(rgba(0.07f, 0.09f, 0.1f), a));
            dc_->FillEllipse(D2D1::Ellipse(Point2F(r.right - h / 2, cy), knob, knob), brush_.Get());
        }
    } else {
        fill(r, h / 2, white(hot ? 0.06f : 0.02f));
        stroke(r, h / 2, white(0.6f * a), 1);
        if (g_paint) {
            brush_->SetColor(white(0.75f * a));
            dc_->FillEllipse(D2D1::Ellipse(Point2F(r.left + h / 2, cy), knob - 1, knob - 1), brush_.Get());
        }
    }
    focusRing(id, r, h / 2);
}

void SettingsPanel::toggleRow(Id id, float x, float y, float w, const std::wstring& title, bool on, bool enabled) {
    const D2D1_RECT_F row = RectF(x, y, x + w, y + 24);
    text(title, RectF(x, y, x + w - 56, y + 24), 13, DWRITE_FONT_WEIGHT_NORMAL, enabled ? kPrimary : kTertiary,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Вся строка кликабельна, как в параметрах Windows.
    const D2D1_RECT_F sw = RectF(x + w - 40, y + 2, x + w, y + 22);
    toggle(id, sw, on, enabled);
    regions_.back().rect = row;
}

void SettingsPanel::tile(Id id, D2D1_RECT_F r, int icon, const std::wstring& title, bool selected) {
    reg(id, r);
    const bool hot = hover_ == id;
    if (selected) {
        fill(r, 9, withAlpha(kAccent, hot ? 0.14f : 0.10f));
        stroke(r, 9, withAlpha(kAccent, 0.28f), 1);
    } else {
        fill(r, 9, white(hot ? (pressed_ == id ? 0.04f : 0.06f) : 0.025f));
    }
    const D2D1_COLOR_F color = selected ? kAccent : white(hot ? 0.8f : 0.58f);
    if (g_paint) {
        brush_->SetColor(color);
        Icons::draw(dc_.Get(), gpu_.d2dFactory.Get(), static_cast<Icons::Kind>(icon),
                    Point2F((r.left + r.right) / 2, r.top + 20), 19, brush_.Get(), 1.3f);
    }
    text(title, RectF(r.left, r.top + 33, r.right, r.bottom - 4), 12,
         selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, color, DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    focusRing(id, r, 9);
}

void SettingsPanel::button(Id id, D2D1_RECT_F r, wchar_t icon, const std::wstring& title, bool enabled) {
    reg(id, r, enabled);
    const float a = enabled ? 1.0f : 0.4f;
    fill(r, 8, white(enabled ? alphaFor(id) : 0.04f));
    const bool painting = g_paint;
    g_paint = false;
    const float labelWidth = text(title, RectF(0, 0, 400, 20), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, kPrimary);
    g_paint = painting;
    const float total = 16 + 8 + labelWidth;
    const float x = (r.left + r.right - total) / 2;
    const float cy = (r.top + r.bottom) / 2;
    if (icon >= 0xE000) {
        glyph(icon, RectF(x, cy - 8, x + 16, cy + 9), 14, withAlpha(kPrimary, a));
    } else if (g_paint) {
        brush_->SetColor(withAlpha(kPrimary, a));
        Icons::draw(dc_.Get(), gpu_.d2dFactory.Get(), static_cast<Icons::Kind>(icon), Point2F(x + 8, cy), 16,
                    brush_.Get(), 1.3f);
    }
    text(title, RectF(x + 24, r.top, r.right, r.bottom), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, withAlpha(kPrimary, a),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    focusRing(id, r, 8);
}

void SettingsPanel::preview(D2D1_RECT_F r) {
    if (!g_paint) return;
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    gpu_.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, 14, 14), &clip);
    dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get()), nullptr);
    // фон «рабочего стола»
    ComPtr<ID2D1GradientStopCollection> stops;
    const D2D1_GRADIENT_STOP gs[2] = {{0, rgba(0.09f, 0.17f, 0.19f)}, {1, rgba(0.14f, 0.23f, 0.23f)}};
    dc_->CreateGradientStopCollection(gs, 2, &stops);
    ComPtr<ID2D1LinearGradientBrush> gradient;
    dc_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(Point2F(r.left, r.top), Point2F(r.right, r.bottom)), stops.Get(), &gradient);
    dc_->FillRectangle(r, gradient.Get());
    const float ox = r.left, oy = r.top;
    {
        ComPtr<ID2D1PathGeometry> wave;
        gpu_.d2dFactory->CreatePathGeometry(&wave);
        ComPtr<ID2D1GeometrySink> sink;
        wave->Open(&sink);
        sink->BeginFigure(Point2F(ox, oy + 130), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier(D2D1::BezierSegment(Point2F(ox + 150, oy + 28), Point2F(ox + 210, oy + 170),
                                            Point2F(ox + 340, oy + 93)));
        sink->AddLine(Point2F(ox + 340, oy + 144));
        sink->AddLine(Point2F(ox, oy + 144));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        brush_->SetColor(white(0.035f));
        dc_->FillGeometry(wave.Get(), brush_.Get());
    }
    const std::wstring label = store_.enabled ? (store_.paused ? L"ЗИМА НА ПАУЗЕ" : L"ВАШ РАБОЧИЙ СТОЛ") : L"СНЕГ ВЫКЛЮЧЕН";
    text(label, RectF(ox + 13, oy + 11, ox + 300, oy + 26), 9, DWRITE_FONT_WEIGHT_SEMI_BOLD, white(0.5f),
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 1.5f);
    auto miniWindow = [&](float x, float y, float w, float h) {
        const D2D1_RECT_F wr = RectF(ox + x, oy + y, ox + x + w, oy + y + h);
        fill(wr, 7, rgba(0.13f, 0.18f, 0.20f));
        fill(RectF(wr.left, wr.top, wr.right, wr.top + 21), 7, white(0.025f));
        for (int i = 0; i < 3; ++i) {
            brush_->SetColor(white(0.22f));
            dc_->FillEllipse(D2D1::Ellipse(Point2F(wr.left + 10 + i * 8.0f, wr.top + 10.5f), 2, 2), brush_.Get());
        }
        fill(RectF(wr.left, wr.top + 21, wr.right, wr.top + 21.5f), 0, white(0.06f));
        fill(RectF(wr.left + 6, wr.top + 27, wr.left + 37, wr.bottom - 6), 2, white(0.035f));
        for (int i = 0; i < 3; ++i) {
            const float lx = wr.left + 45, ly = wr.top + 36 + i * 8.0f;
            fill(RectF(lx, ly, lx + 46 - i * 8.0f, ly + 3), 1.5f, white(0.07f));
        }
        stroke(wr, 7, white(0.1f), 0.5f);
    };
    miniWindow(20, 56, 171, 92);
    miniWindow(205, 92, 112, 62);
    fill(RectF(ox + 116, oy + 129, ox + 224, oy + 145), 6, white(0.10f));
    if (previewBitmap_) {
        dc_->DrawBitmap(previewBitmap_.Get(), RectF(ox, oy, ox + 340, oy + 144), 1.0f,
                        D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
    }
    dc_->PopLayer();
    stroke(r, 14, white(0.09f), 1);
}

void SettingsPanel::dial(D2D1_RECT_F r) {
    reg(Dial, r);
    if (!g_paint) {
        return;
    }
    const float size = r.right - r.left;
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const float reach = size / 2 - 13;
    const Point wind = store_.prefs.wind;
    const D2D1_POINT_2F knob = Point2F(cx + static_cast<float>(wind.x) * reach, cy + static_cast<float>(wind.y) * reach);
    brush_->SetColor(white(hover_ == Dial ? 0.05f : 0.025f));
    dc_->FillEllipse(D2D1::Ellipse(Point2F(cx, cy), size / 2, size / 2), brush_.Get());
    brush_->SetColor(white(0.09f));
    dc_->DrawEllipse(D2D1::Ellipse(Point2F(cx, cy), size / 2 - 0.5f, size / 2 - 0.5f), brush_.Get(), 1);
    brush_->SetColor(white(0.05f));
    dc_->DrawEllipse(D2D1::Ellipse(Point2F(cx, cy), size / 2 - 15, size / 2 - 15), brush_.Get(), 1, dash_.Get());
    brush_->SetColor(white(0.07f));
    dc_->DrawLine(Point2F(cx, r.top + 17), Point2F(cx, r.bottom - 17), brush_.Get(), 1);
    dc_->DrawLine(Point2F(r.left + 17, cy), Point2F(r.right - 17, cy), brush_.Get(), 1);
    brush_->SetColor(withAlpha(kAccent, 0.55f));
    dc_->DrawLine(Point2F(cx, cy), knob, brush_.Get(), 1.5f, round_.Get());
    const auto letter = [&](const wchar_t* s, float x, float y) {
        text(s, RectF(x - 8, y - 7, x + 8, y + 7), 9, DWRITE_FONT_WEIGHT_SEMI_BOLD, kSecondary,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    letter(L"С", cx, r.top + 8);
    letter(L"Ю", cx, r.bottom - 8);
    letter(L"З", r.left + 8, cy);
    letter(L"В", r.right - 8, cy);
    brush_->SetColor(kAccent);
    const float k = (pressed_ == Dial) ? 6 : 4.5f;
    dc_->FillEllipse(D2D1::Ellipse(knob, k, k), brush_.Get());
    if (focusVisible_ && focus_ == Dial) {
        brush_->SetColor(white(0.9f));
        dc_->DrawEllipse(D2D1::Ellipse(Point2F(cx, cy), size / 2 + 3, size / 2 + 3), brush_.Get(), 2);
    }
}

void SettingsPanel::slider(D2D1_RECT_F r) {
    reg(Opacity, r);
    if (!g_paint) return;
    const float cy = (r.top + r.bottom) / 2;
    const float left = r.left + 8, right = r.right - 8;
    const float t = static_cast<float>((store_.prefs.opacity - 0.25) / 0.75);
    const float x = left + (right - left) * std::clamp(t, 0.0f, 1.0f);
    fill(RectF(left, cy - 2, right, cy + 2), 2, white(0.35f));
    fill(RectF(left, cy - 2, x, cy + 2), 2, kAccent);
    brush_->SetColor(rgba(0.27f, 0.29f, 0.30f));
    dc_->FillEllipse(D2D1::Ellipse(Point2F(x, cy), 10, 10), brush_.Get());
    brush_->SetColor(white(0.12f));
    dc_->DrawEllipse(D2D1::Ellipse(Point2F(x, cy), 9.5f, 9.5f), brush_.Get(), 1);
    brush_->SetColor(kAccent);
    const float inner = pressed_ == Opacity ? 4.5f : (hover_ == Opacity ? 7.0f : 5.5f);
    dc_->FillEllipse(D2D1::Ellipse(Point2F(x, cy), inner, inner), brush_.Get());
    if (focusVisible_ && focus_ == Opacity) stroke(RectF(r.left, r.top - 2, r.right, r.bottom + 2), 6, white(0.9f), 2);
}

float SettingsPanel::layout(bool paint) {
    g_paint = paint;
    regions_.clear();
    focusOrder_.clear();
    const float L = kPad, R = kWidth - kPad, CW = R - L;
    float y = kPad;
    const bool enabled = store_.enabled;

    // ---- заголовок
    fill(RectF(L, y, L + 38, y + 38), 11, withAlpha(kAccent, 0.08f));
    if (paint) {
        brush_->SetColor(kAccent);
        Icons::draw(dc_.Get(), gpu_.d2dFactory.Get(), Icons::Kind::snowflake, Point2F(L + 19, y + 19), 25,
                    brush_.Get(), 1.4f);
    }
    text(L"Tryflake", RectF(L + 50, y - 2, R - 56, y + 25), 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, kPrimary,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, -0.4f, false, true);
    text(L"Немного зимы на вашем ПК", RectF(L + 50, y + 23, R - 56, y + 40), 12, DWRITE_FONT_WEIGHT_NORMAL, kSecondary);
    toggle(Enable, RectF(R - 40, y + 9, R, y + 29), enabled, true);
    y += 38 + 12;

    // ---- превью
    preview(RectF(L, y, R, y + 144));
    y += 144 + 12;

    // ---- пауза / насыпать
    const float half = (CW - 8) / 2;
    button(Pause, RectF(L, y, L + half, y + 34), store_.paused ? kGlyphPlay : kGlyphPause,
           store_.paused ? L"Продолжить" : L"Пауза", enabled);
    button(Seed, RectF(L + half + 8, y, R, y + 34), static_cast<wchar_t>(Icons::Kind::cloudSnow), L"Насыпать",
           enabled && store_.prefs.accumulation);
    y += 34 + 14;

    // ---- снегопад
    text(L"СНЕГОПАД", RectF(L, y, R, y + 14), 10, DWRITE_FONT_WEIGHT_SEMI_BOLD, kSecondary,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 1.4f);
    y += 14 + 8;
    const float tw = (CW - 8) / 3;
    for (int i = 0; i < 3; ++i) {
        const auto weather = static_cast<Snowfall>(i);
        const float x = L + i * (tw + 4);
        tile(static_cast<Id>(Fall0 + i), RectF(x, y, x + tw, y + 56),
             static_cast<int>(weather == Snowfall::blizzard ? Icons::Kind::windSnow : Icons::Kind::cloudSnow),
             snowfallTitle(weather), store_.prefs.snowfall == weather);
    }
    y += 56 + 14;

    // ---- инструмент
    text(L"ИНСТРУМЕНТ", RectF(L, y, R, y + 14), 10, DWRITE_FONT_WEIGHT_SEMI_BOLD, kSecondary,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 1.4f);
    y += 14 + 8;
    const Icons::Kind toolIcons[3] = {Icons::Kind::mitten, Icons::Kind::shovel, Icons::Kind::snowball};
    for (int i = 0; i < 3; ++i) {
        const auto tool = static_cast<SnowTool>(i);
        const float x = L + i * (tw + 4);
        tile(static_cast<Id>(Tool0 + i), RectF(x, y, x + tw, y + 56), static_cast<int>(toolIcons[i]), toolTitle(tool),
             store_.prefs.tool == tool);
    }
    y += 56 + 8;
    text(toolHint(store_.prefs.tool), RectF(L, y, R, y + 34), 12, DWRITE_FONT_WEIGHT_NORMAL, kSecondary,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 0, true);
    y += 34 + 10;

    // ---- ветер
    dial(RectF(L, y, L + 92, y + 92));
    {
        const float tx = L + 112;
        text(L"Ветер", RectF(tx, y + 14, R, y + 34), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, kPrimary);
        const Point w = store_.prefs.wind;
        std::wstring description;
        if (std::hypot(w.x, w.y) < 0.06) {
            description = L"Штиль. Снежинки падают мягко.";
        } else {
            const wchar_t* direction = std::abs(w.x) > std::abs(w.y) ? (w.x < 0 ? L"На запад" : L"На восток")
                                                                      : (w.y < 0 ? L"На север" : L"На юг");
            description = std::wstring(direction) + L" · " + (std::hypot(w.x, w.y) > 0.65 ? L"сильный" : L"лёгкий");
        }
        text(description, RectF(tx, y + 36, R, y + 54), 12, DWRITE_FONT_WEIGHT_NORMAL, kSecondary);
        const float linkW = text(L"Вернуть штиль", RectF(tx, y + 60, R, y + 78), 12, DWRITE_FONT_WEIGHT_NORMAL,
                                 hover_ == Calm ? withAlpha(kAccent, 0.75f) : kAccent);
        reg(Calm, RectF(tx - 2, y + 58, tx + linkW + 2, y + 80));
        focusRing(Calm, RectF(tx - 2, y + 58, tx + linkW + 2, y + 80), 4);
    }
    y += 92 + 12;

    fill(RectF(L, y, R, y + 1), 0, white(0.07f));
    y += 1 + 12;

    toggleRow(Accum, L, y, CW, L"Сугробы на окнах и панели задач", store_.prefs.accumulation);
    y += 24 + 10;

    // ---- ещё настройки
    {
        const D2D1_RECT_F r = RectF(L - 4, y, R + 4, y + 26);
        reg(More, r);
        if (hover_ == More) fill(r, 6, white(0.04f));
        text(L"Ещё настройки", RectF(L, y, R - 24, y + 26), 13, DWRITE_FONT_WEIGHT_NORMAL, kSecondary,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        glyph(expanded_ ? kGlyphUp : kGlyphDown, RectF(R - 16, y + 6, R, y + 22), 11, kSecondary);
        focusRing(More, r, 6);
        y += 26;
    }
    if (expanded_) {
        y += 12;
        text(L"Видимость снега", RectF(L, y, R - 60, y + 20), 13, DWRITE_FONT_WEIGHT_NORMAL, kPrimary);
        text(std::to_wstring(static_cast<int>(std::lround(store_.prefs.opacity * 100))) + L"%", RectF(R - 60, y, R, y + 20),
             13, DWRITE_FONT_WEIGHT_NORMAL, kSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 22;
        slider(RectF(L, y, R, y + 22));
        y += 22 + 12;
        toggleRow(AllScreens, L, y, CW, L"На всех мониторах", store_.prefs.allScreens);
        y += 24 + 10;
        toggleRow(Energy, L, y, CW, L"Беречь заряд батареи", store_.prefs.energyAware);
        y += 24 + 10;
        toggleRow(Login, L, y, CW, L"Запускать при входе в Windows", store_.loginEnabled);
        y += 24;
    }

    // ---- ошибка
    if (!store_.errorMessage.empty()) {
        y += 12;
        const float textW = CW - 20 - 22 - 22;
        g_paint = false;
        const float h = text(store_.errorMessage, RectF(0, 0, textW, 400), 12, DWRITE_FONT_WEIGHT_NORMAL, kWarning,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 0, true);
        g_paint = paint;
        const float boxH = std::max(38.0f, h + 20);
        const D2D1_RECT_F box = RectF(L, y, R, y + boxH);
        fill(box, 8, withAlpha(kWarning, 0.08f));
        glyph(kGlyphWarning, RectF(L + 10, y + 11, L + 26, y + 27), 13, kWarning);
        text(store_.errorMessage, RectF(L + 32, y + 10, L + 32 + textW, y + boxH - 8), 12, DWRITE_FONT_WEIGHT_NORMAL,
             kWarning, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 0, true);
        const D2D1_RECT_F close = RectF(R - 28, y + 6, R - 6, y + 28);
        reg(ErrorClose, close);
        if (hover_ == ErrorClose) fill(close, 4, withAlpha(kWarning, 0.12f));
        glyph(kGlyphCancel, RectF(close.left + 5, close.top + 5, close.right - 5, close.bottom - 5), 10, kWarning);
        focusRing(ErrorClose, close, 4);
        y += boxH;
    }

    // ---- подвал
    y += 16;
    {
        const D2D1_COLOR_F clearColor = hover_ == Clear ? kPrimary : kSecondary;
        glyph(kGlyphDelete, RectF(L, y + 3, L + 14, y + 17), 12, clearColor);
        const float w = text(L"Убрать снег", RectF(L + 20, y, R, y + 20), 12, DWRITE_FONT_WEIGHT_NORMAL, clearColor,
                             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        reg(Clear, RectF(L - 4, y - 2, L + 24 + w, y + 22));
        focusRing(Clear, RectF(L - 4, y - 2, L + 24 + w, y + 22), 4);
        g_paint = false;
        const float qw = text(L"Выйти", RectF(0, 0, 200, 20), 12, DWRITE_FONT_WEIGHT_NORMAL, kSecondary);
        g_paint = paint;
        text(L"Выйти", RectF(R - qw, y, R, y + 20), 12, DWRITE_FONT_WEIGHT_NORMAL, hover_ == Quit ? kPrimary : kSecondary,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        reg(Quit, RectF(R - qw - 4, y - 2, R + 4, y + 22));
        focusRing(Quit, RectF(R - qw - 4, y - 2, R + 4, y + 22), 4);
        y += 20 + 12;
    }
    if (paint) {
        brush_->SetColor(withAlpha(kAccent, 0.7f));
        dc_->FillEllipse(D2D1::Ellipse(Point2F(L + 2, y + 7), 2, 2), brush_.Get());
    }
    text(store_.economyActive ? L"Режим экономии энергии" : L"Бесплатно. Без аккаунта. Всё на вашем ПК.",
         RectF(L + 10, y, R - 40, y + 14), 11, DWRITE_FONT_WEIGHT_NORMAL, kTertiary);
    text(kAppVersion, RectF(R - 40, y, R, y + 14), 11, DWRITE_FONT_WEIGHT_NORMAL, kTertiary,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    y += 14 + kPad;
    g_paint = false;
    return y;
}

// ================================================================= ввод

const SettingsPanel::Region* SettingsPanel::region(Id id) const {
    for (const auto& r : regions_)
        if (r.id == id) return &r;
    return nullptr;
}

SettingsPanel::Id SettingsPanel::hit(D2D1_POINT_2F p) const {
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it)
        if (contains(it->rect, p)) return it->enabled ? it->id : None;
    return None;
}

D2D1_POINT_2F SettingsPanel::toContent(LPARAM lParam) const {
    return Point2F(GET_X_LPARAM(lParam) / scale_, GET_Y_LPARAM(lParam) / scale_ + scroll_);
}

void SettingsPanel::notify() {
    if (actions_.changed) actions_.changed();
    render();
}

void SettingsPanel::activate(Id id) {
    auto& p = store_.prefs;
    switch (id) {
        case Enable: store_.enabled = !store_.enabled; break;
        case Pause: store_.paused = !store_.paused; break;
        case Seed:
            if (actions_.seed) actions_.seed();
            break;
        case Fall0: case Fall1: case Fall2: p.snowfall = static_cast<Snowfall>(id - Fall0); break;
        case Tool0: case Tool1: case Tool2: p.tool = static_cast<SnowTool>(id - Tool0); break;
        case Calm: p.wind = Point(); break;
        case Accum: p.accumulation = !p.accumulation; break;
        case More:
            expanded_ = !expanded_;
            contentHeight_ = layout(false);
            resizeToContent();
            break;
        case AllScreens: p.allScreens = !p.allScreens; break;
        case Energy: p.energyAware = !p.energyAware; break;
        case Login:
            if (actions_.setLogin) actions_.setLogin(!store_.loginEnabled);
            break;
        case ErrorClose: store_.errorMessage.clear(); break;
        case Clear:
            if (actions_.clear) actions_.clear();
            break;
        case Quit:
            if (actions_.quit) actions_.quit();
            return;
        default: return;
    }
    notify();
}

void SettingsPanel::dragTo(Id id, D2D1_POINT_2F p) {
    const Region* r = region(id);
    if (!r) return;
    if (id == Dial) {
        const float cx = (r->rect.left + r->rect.right) / 2, cy = (r->rect.top + r->rect.bottom) / 2;
        const float reach = (r->rect.right - r->rect.left) / 2 - 13;
        double x = (p.x - cx) / reach, y = (p.y - cy) / reach;
        const double length = std::max(1.0, std::hypot(x, y));
        store_.prefs.wind = Point(x / length, y / length);
    } else if (id == Opacity) {
        const float left = r->rect.left + 8, right = r->rect.right - 8;
        const double t = std::clamp((p.x - left) / (right - left), 0.0f, 1.0f);
        store_.prefs.opacity = std::round((0.25 + t * 0.75) * 100) / 100;
    }
    notify();
}

void SettingsPanel::moveFocus(int direction) {
    layout(false);
    if (focusOrder_.empty()) return;
    auto it = std::find(focusOrder_.begin(), focusOrder_.end(), focus_);
    int index = it == focusOrder_.end() ? (direction > 0 ? -1 : 0) : static_cast<int>(it - focusOrder_.begin());
    const int n = static_cast<int>(focusOrder_.size());
    index = (index + direction + n) % n;
    focus_ = focusOrder_[index];
    focusVisible_ = true;
    if (const Region* r = region(focus_)) {
        if (r->rect.top - 8 < scroll_) scroll_ = std::max(0.0f, r->rect.top - 8);
        if (r->rect.bottom + 8 > scroll_ + viewHeight_) scroll_ = r->rect.bottom + 8 - viewHeight_;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentHeight_ - viewHeight_));
    }
    render();
}

LRESULT CALLBACK SettingsPanel::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        reinterpret_cast<SettingsPanel*>(create->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<SettingsPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->handle(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT SettingsPanel::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) hide();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (visible()) render();
            return 0;
        case WM_DPICHANGED: {
            releaseTargets();
            scale_ = HIWORD(wParam) / 96.0f;
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            const auto p = toContent(lParam);
            if (pressed_ == Dial || pressed_ == Opacity) {
                dragTo(pressed_, p);
                return 0;
            }
            const Id h = hit(p);
            if (h != hover_) {
                hover_ = h;
                render();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hover_ != None) {
                hover_ = None;
                render();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const auto p = toContent(lParam);
            pressed_ = hit(p);
            hover_ = pressed_;
            focusVisible_ = false;
            if (pressed_ != None) {
                focus_ = pressed_;
                SetCapture(hwnd_);
            }
            if (pressed_ == Dial || pressed_ == Opacity)
                dragTo(pressed_, p);
            else
                render();
            return 0;
        }
        case WM_LBUTTONUP: {
            const Id was = pressed_;
            pressed_ = None;
            if (GetCapture() == hwnd_) ReleaseCapture();
            const Id h = hit(toContent(lParam));
            hover_ = h;
            if (was != None && was == h && was != Dial && was != Opacity)
                activate(was);
            else
                render();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed_ != None && reinterpret_cast<HWND>(lParam) != hwnd_) {
                pressed_ = None;
                render();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            const float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 48;
            const float next = std::clamp(scroll_ - delta, 0.0f, std::max(0.0f, contentHeight_ - viewHeight_));
            if (next != scroll_) {
                scroll_ = next;
                render();
            }
            return 0;
        }
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            switch (wParam) {
                case VK_ESCAPE: hide(); return 0;
                case VK_TAB: moveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0;
                case VK_SPACE:
                case VK_RETURN:
                    if (focus_ == Dial && wParam == VK_SPACE) {
                        store_.prefs.wind = Point();
                        notify();
                    } else if (focus_ != None) {
                        activate(focus_);
                    }
                    return 0;
                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN: {
                    const double sign = (wParam == VK_LEFT || wParam == VK_UP) ? -1 : 1;
                    if (focus_ == Dial) {
                        auto& w = store_.prefs.wind;
                        if (wParam == VK_LEFT || wParam == VK_RIGHT)
                            w.x = std::clamp(w.x + sign * 0.1, -1.0, 1.0);
                        else
                            w.y = std::clamp(w.y + sign * 0.1, -1.0, 1.0);
                        notify();
                    } else if (focus_ == Opacity) {
                        const double s = (wParam == VK_LEFT || wParam == VK_DOWN) ? -0.05 : 0.05;
                        store_.prefs.opacity = std::clamp(std::round((store_.prefs.opacity + s) * 100) / 100, 0.25, 1.0);
                        notify();
                    } else {
                        moveFocus(sign > 0 ? 1 : -1);
                    }
                    return 0;
                }
                case 'Q':
                    if (ctrl && actions_.quit) actions_.quit();
                    return 0;
                default: break;
            }
            break;
        }
        case WM_CLOSE:
            hide();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
