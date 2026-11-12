#pragma once

#include <map>

#include "Common.h"
#include "Gpu.h"
#include "Settings.h"

struct PanelActions {
    std::function<void()> changed;  // настройки изменились
    std::function<void()> clear;
    std::function<void()> seed;
    std::function<void()> quit;
    std::function<void(bool)> setLogin;
};

/// Всплывающая панель над областью уведомлений, нарисованная Direct2D/DirectWrite.
class SettingsPanel {
public:
    SettingsPanel(GpuContext& gpu, SettingsStore& store, PanelActions actions);
    ~SettingsPanel();

    void show(const RECT& anchor);
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    /// Перерисовать после изменения состояния извне.
    void refresh();
    void frame(double now);
    bool animating() const;
    /// Время последнего скрытия (для клика по значку при открытой панели).
    double hiddenAt() const { return hiddenAt_; }

    static void registerClass(HINSTANCE instance);

private:
    enum Id {
        None, Enable, Pause, Seed, Fall0, Fall1, Fall2, Tool0, Tool1, Tool2, Dial, Calm, Accum, More,
        Opacity, AllScreens, Energy, Login, ErrorClose, Clear, Quit
    };
    struct Region {
        Id id;
        D2D1_RECT_F rect;
        bool enabled;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    bool ensureWindow();
    bool ensureTargets();
    void releaseTargets();
    void releaseDevice();
    void place(bool show);
    bool updateHover();
    void render();
    float layout(bool paint);
    void resizeToContent();
    void activate(Id id);
    void dragTo(Id id, D2D1_POINT_2F p);
    Id hit(D2D1_POINT_2F p) const;
    const Region* region(Id id) const;
    D2D1_POINT_2F toContent(LPARAM lParam) const;
    D2D1_POINT_2F toContent(POINT clientPoint) const;
    void moveFocus(int direction);
    void notify();

    // рисование
    IDWriteTextFormat* format(float size, DWRITE_FONT_WEIGHT weight, bool display = false, bool icons = false);
    float text(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT weight, D2D1_COLOR_F color,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_NEAR, float tracking = 0,
               bool wrap = false, bool display = false);
    void glyph(wchar_t code, D2D1_RECT_F r, float size, D2D1_COLOR_F color);
    void fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1);
    void toggle(Id id, D2D1_RECT_F r, bool on, bool enabled);
    void tile(Id id, D2D1_RECT_F r, int icon, const std::wstring& title, bool selected);
    void button(Id id, D2D1_RECT_F r, wchar_t icon, const std::wstring& title, bool enabled);
    void toggleRow(Id id, float x, float y, float w, const std::wstring& title, bool on, bool enabled = true);
    void focusRing(Id id, D2D1_RECT_F r, float radius);
    void preview(D2D1_RECT_F r);
    void dial(D2D1_RECT_F r);
    void slider(D2D1_RECT_F r);
    void reg(Id id, D2D1_RECT_F r, bool enabled = true);
    float alphaFor(Id id) const;

    GpuContext& gpu_;
    SettingsStore& store_;
    PanelActions actions_;
    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dash_;
    ComPtr<ID2D1StrokeStyle> round_;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring textFamily_, displayFamily_, iconFamily_;
    float scale_ = 1;
    UINT pixelWidth_ = 0, pixelHeight_ = 0;

    // мини-рабочий стол в превью
    std::unique_ptr<snow::SnowWorld> previewWorld_;
    std::vector<GpuVertex> previewVertices_;
    ComPtr<ID3D11Texture2D> previewTexture_;
    ComPtr<ID3D11RenderTargetView> previewRTV_;
    ComPtr<ID2D1Bitmap1> previewBitmap_;
    // кэш объектов превью (пересоздаются вместе с контекстом)
    ComPtr<ID2D1LinearGradientBrush> previewGradient_;
    ComPtr<ID2D1PathGeometry> previewWave_;
    ComPtr<ID2D1RoundedRectangleGeometry> previewClip_;
    float previewX_ = -1, previewY_ = -1;
    double previewTime_ = 0;
    double lastPreviewFrame_ = 0;

    std::vector<Region> regions_;
    std::vector<Id> focusOrder_;
    Id hover_ = None, pressed_ = None, focus_ = None;
    bool focusVisible_ = false;
    bool expanded_ = false;
    bool tracking_ = false;
    bool placing_ = false;
    float scroll_ = 0, contentHeight_ = 0, viewHeight_ = 0, maxHeight_ = 0;
    double hiddenAt_ = 0;
    RECT anchor_{};
};
