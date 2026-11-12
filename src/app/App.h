#pragma once

#include <atomic>
#include <thread>

#include "Common.h"
#include "Gpu.h"
#include "Overlay.h"
#include "Settings.h"
#include "SettingsPanel.h"
#include "WindowTracker.h"

class App {
public:
    int run(bool showSettings, bool smokeTest);
    static App* instance;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static void CALLBACK winEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);

    bool createDevices();
    void createPanel();
    void rebuildOverlays();
    void applySettings();
    void refreshWindows();
    void frame();
    void updatePacing();
    void handleDeviceLost();
    static bool hardwareAvailable();
    void showPanel();
    void showMenu(int x, int y);
    void addTrayIcon();
    void updateTrayIcon();
    RECT trayAnchor() const;
    bool sleeping() const { return suspended_ || displayOff_ || locked_ || fullscreen_; }
    void raisePanel();
    bool monitorsChanged() const;
    void pacingLoop();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    GpuContext gpu_;
    SettingsStore store_;
    std::unique_ptr<SettingsPanel> panel_;
    std::vector<std::unique_ptr<Overlay>> overlays_;
    std::vector<MonitorInfo> monitors_;
    std::vector<HWINEVENTHOOK> hooks_;
    std::vector<HPOWERNOTIFY> powerNotifications_;
    HICON trayIcon_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool trayAdded_ = false;
    bool lastAllScreens_ = true;
    bool suspended_ = false, displayOff_ = false, locked_ = false, fullscreen_ = false;
    bool rebuildPending_ = false;
    double nextFrame_ = 0;
    double lastWarpRetry_ = 0;
    // Backoff пересоздания устройств, пустых оверлеев и повтора NIM_ADD (секунды монотонного времени).
    bool needRecovery_ = false;
    double nextRecovery_ = 0, recoveryDelay_ = 0.5, lastRecovery_ = -1000;
    double nextRebuild_ = 0, rebuildDelay_ = 0.5;
    double nextTrayAdd_ = 0;
    double lastTraySelect_ = 0;
    // Кэш последнего значка трея: пересоздаём только при реальной смене.
    UINT trayDpi_ = 0;
    bool trayLight_ = false, trayDimmed_ = false;
    bool windowsDirty_ = false;
    Preferences savedPrefs_;

    std::thread pacer_;
    std::atomic<bool> running_{true};
    HANDLE tickEvent_ = nullptr;    // кадр композитора
    HANDLE activeEvent_ = nullptr;  // есть что анимировать
};
