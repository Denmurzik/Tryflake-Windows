#include "Overlay.h"

using namespace snow;

namespace {
constexpr wchar_t kOverlayClass[] = L"TryflakeOverlay";
}

void Overlay::registerClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);
}

Overlay::Overlay(GpuContext& gpu, SettingsStore& settings, const MonitorInfo& monitor, const DesktopSnapshot& snapshot)
    : gpu_(gpu),
      settings_(settings),
      monitor_(monitor),
      world_(monitor.px.width / monitor.scale, monitor.px.height / monitor.scale,
             static_cast<uint64_t>(monitor.px.width + std::abs(monitor.px.x) + 1) * 7919) {
    world_.configuration = settings.configuration();
    geometry_ = WindowTracker::surfaces(monitor_, snapshot);
    world_.setSurfaces(geometry_);
    world_.warmStart();

    pixelWidth_ = static_cast<UINT>(monitor.px.width);
    pixelHeight_ = static_cast<UINT>(monitor.px.height);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
                                WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                            kOverlayClass, L"Tryflake Snow Overlay", WS_POPUP, static_cast<int>(monitor.px.x),
                            static_cast<int>(monitor.px.y), static_cast<int>(pixelWidth_),
                            static_cast<int>(pixelHeight_), nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    const BOOL yes = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &yes, sizeof(yes));
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = pixelWidth_;
    desc.Height = pixelHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(gpu_.factory->CreateSwapChainForComposition(gpu_.device.Get(), &desc, nullptr, &swapChain_))) return;
    ComPtr<ID3D11Texture2D> buffer;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer));
    gpu_.device->CreateRenderTargetView(buffer.Get(), nullptr, &rtv_);
    if (FAILED(gpu_.dcomp->CreateTargetForHwnd(hwnd_, TRUE, &target_)) || FAILED(gpu_.dcomp->CreateVisual(&visual_))) {
        swapChain_.Reset();
        return;
    }
    visual_->SetContent(swapChain_.Get());
    target_->SetRoot(visual_.Get());
    gpu_.dcomp->Commit();
    applySettings(false);
}

Overlay::~Overlay() {
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    if (gpu_.context) gpu_.context->ClearState();
    rtv_.Reset();
    visual_.Reset();
    target_.Reset();
    swapChain_.Reset();
}

void Overlay::applySettings(bool sleeping) {
    world_.configuration = settings_.configuration();
    previousTime_ = 0;
    const bool visible = settings_.enabled && !sleeping;
    if (visible != visible_) {
        visible_ = visible;
        if (visible) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else {
            ShowWindow(hwnd_, SW_HIDE);
        }
    }
    if (visible_ && settings_.paused) render();
    if (settings_.prefs.tool != SnowTool::snowball || !visible_ || settings_.paused) {
        endDragIfNeeded();
        setClickThrough(true);
    }
}

void Overlay::updateSnapshot(const DesktopSnapshot& snapshot) {
    auto next = WindowTracker::surfaces(monitor_, snapshot);
    if (next == geometry_) return;
    geometry_ = std::move(next);
    world_.setSurfaces(geometry_);
    if (visible_ && settings_.paused) render();
}

void Overlay::keepOnTop(HWND below) {
    if (!visible_) return;
    // Остаёмся поверх обычных окон, но под панелью настроек.
    SetWindowPos(hwnd_, below ? below : HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

void Overlay::clear() {
    world_.clear();
    endDragIfNeeded();
    if (visible_ && settings_.paused) render();
}

void Overlay::seed() {
    world_.seedSnow();
    if (visible_ && settings_.paused) render();
}

bool Overlay::frame(double now) {
    if (!visible_ || settings_.paused) return true;
    POINT cursor{};
    GetCursorPos(&cursor);
    const Point local((cursor.x - monitor_.px.x) / monitor_.scale, (cursor.y - monitor_.px.y) / monitor_.scale);
    const double movement = local.distance(lastPointer_);
    const double dt = std::min(0.1, std::max(1.0 / 240, now - lastPointerTime_));
    if (movement > 0.75 && movement < 240) {
        const Point velocity((local.x - lastPointer_.x) / dt, (local.y - lastPointer_.y) / dt);
        world_.brush(local, settings_.prefs.tool, velocity);
    }
    lastPointerTime_ = now;
    lastPointer_ = local;
    setClickThrough(settings_.prefs.tool != SnowTool::snowball || !world_.hitTest(local));

    world_.update(previousTime_ == 0 ? 1.0 / 60 : now - previousTime_);
    previousTime_ = now;
    return render();
}

bool Overlay::render() {
    if (!swapChain_) return true;
    SnowMesh::build(world_, settings_.prefs.opacity, vertices_);
    const float clear[4] = {0, 0, 0, 0};
    gpu_.draw(rtv_.Get(), pixelWidth_, pixelHeight_, static_cast<float>(world_.width),
              static_cast<float>(world_.height), vertices_, clear);
    const HRESULT hr = swapChain_->Present(0, 0);
    return !(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET);
}

void Overlay::setClickThrough(bool through) {
    if (through == clickThrough_ || !hwnd_) return;
    clickThrough_ = through;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex = through ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

Point Overlay::toLocal(LPARAM lParam) const {
    return Point(GET_X_LPARAM(lParam) / monitor_.scale, GET_Y_LPARAM(lParam) / monitor_.scale);
}

void Overlay::endDragIfNeeded() {
    if (world_.draggingID()) world_.endDrag();
    if (dragging_) {
        dragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
}

LRESULT CALLBACK Overlay::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->handle(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Overlay::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            if (settings_.prefs.tool == SnowTool::snowball && !clickThrough_) {
                SetCursor(LoadCursorW(nullptr, dragging_ ? IDC_SIZEALL : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            if (settings_.prefs.tool == SnowTool::snowball && !settings_.paused && world_.beginDrag(toLocal(lParam))) {
                dragging_ = true;
                SetCapture(hwnd_);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_) world_.drag(toLocal(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (dragging_) {
                world_.endDrag();
                dragging_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_ && reinterpret_cast<HWND>(lParam) != hwnd_) {
                world_.endDrag();
                dragging_ = false;
            }
            return 0;
        case WM_DPICHANGED:
        case WM_WINDOWPOSCHANGING:
            break;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
