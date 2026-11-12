#pragma once

#include "Common.h"
#include "Gpu.h"
#include "Settings.h"
#include "WindowTracker.h"

/// Прозрачный слой со снегом на одном мониторе. Клики проходят насквозь,
/// кроме режима снежков, когда курсор над сугробом или шаром.
class Overlay {
public:
    Overlay(GpuContext& gpu, SettingsStore& settings, const MonitorInfo& monitor, const DesktopSnapshot& snapshot);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool ok() const { return hwnd_ && swapChain_ && rtv_; }
    HWND hwnd() const { return hwnd_; }
    const MonitorInfo& monitor() const { return monitor_; }
    snow::SnowWorld& world() { return world_; }

    void applySettings(bool sleeping);
    void updateSnapshot(const DesktopSnapshot& snapshot);
    /// Шаг симуляции и кадр. false — устройство потеряно.
    bool frame(double now);
    bool animating() const { return visible_ && !settings_.paused; }
    void clear();
    void seed();
    void keepOnTop(HWND below);

    static void registerClass(HINSTANCE instance);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    bool render();
    void setClickThrough(bool through);
    snow::Point toLocal(LPARAM lParam) const;
    void endDragIfNeeded();

    GpuContext& gpu_;
    SettingsStore& settings_;
    MonitorInfo monitor_;
    snow::SnowWorld world_;
    std::vector<snow::SurfaceGeometry> geometry_;
    std::vector<GpuVertex> vertices_;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    UINT pixelWidth_ = 0, pixelHeight_ = 0;
    bool visible_ = false;
    bool clickThrough_ = true;
    bool dragging_ = false;
    double previousTime_ = 0;
    snow::Point lastPointer_{-1000, -1000};
    double lastPointerTime_ = 0;
};
