#include "App.h"

#include <wtsapi32.h>

#include "Icons.h"

App* App::instance = nullptr;

namespace {

constexpr wchar_t kMainClass[] = L"TryflakeMain";
constexpr UINT_PTR kTimerTrack = 1, kTimerSave = 2, kTimerRebuild = 3, kTimerShow = 4, kTimerQuit = 5;
constexpr UINT kTrayID = 1;
enum MenuCommand : UINT { kMenuPause = 100, kMenuClear, kMenuSettings, kMenuQuit };

// Тёмные контекстные меню Windows 10/11 (uxtheme, без публичного заголовка).
void enableDarkMenus(HWND hwnd) {
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppMode = int(WINAPI*)(int);
    using AllowDarkModeForWindow = bool(WINAPI*)(HWND, bool);
    using FlushMenuThemes = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    auto allow = reinterpret_cast<AllowDarkModeForWindow>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
    auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(1);  // AllowDark: следовать теме приложений Windows
    if (allow) allow(hwnd, true);
    if (flush) flush();
}

bool lightTaskbar() {
    DWORD value = 0, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

UINT primaryDpi() {
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    return dpiX;
}

using WaitForCompositorClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

}  // namespace

// ================================================================= запуск

int App::run(bool showSettings, bool smokeTest) {
    instance = this;
    instance_ = GetModuleHandleW(nullptr);
    store_.load();
    savedPrefs_ = store_.prefs;
    lastAllScreens_ = store_.prefs.allScreens;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    Overlay::registerClass(instance_);
    SettingsPanel::registerClass(instance_);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_,
                            nullptr);
    if (!hwnd_) return 1;
    enableDarkMenus(hwnd_);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    std::wstring error;
    if (!createDevices()) {
        MessageBoxW(nullptr, store_.errorMessage.c_str(), kAppName, MB_ICONERROR);
        return 1;
    }
    store_.updateEnergyState();
    addTrayIcon();

    powerNotifications_.push_back(RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE));
    powerNotifications_.push_back(RegisterPowerSettingNotification(hwnd_, &GUID_ENERGY_SAVER_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE));
    WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);

    // Мгновенная реакция на перемещение окон; таймер 4 Гц — страховка.
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const std::pair<DWORD, DWORD> ranges[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER},
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (const auto& [first, last] : ranges) {
        if (HWINEVENTHOOK hook = SetWinEventHook(first, last, nullptr, winEvent, 0, 0, flags)) hooks_.push_back(hook);
    }

    rebuildOverlays();
    SetTimer(hwnd_, kTimerTrack, 250, nullptr);
    if (store_.firstLaunch() || showSettings) SetTimer(hwnd_, kTimerShow, 500, nullptr);
    if (smokeTest) SetTimer(hwnd_, kTimerQuit, 8000, nullptr);

    tickEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    activeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pacer_ = std::thread([this] { pacingLoop(); });
    updatePacing();

    MSG msg{};
    bool quit = false;
    while (!quit) {
        const DWORD result = MsgWaitForMultipleObjectsEx(1, &tickEvent_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0) frame();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    running_ = false;
    SetEvent(activeEvent_);
    if (pacer_.joinable()) pacer_.join();
    CloseHandle(tickEvent_);
    CloseHandle(activeEvent_);
    gpu_.destroy();  // все COM-объекты освобождаются до CoUninitialize
    return static_cast<int>(msg.wParam);
}

bool App::createDevices() {
    std::wstring error;
    if (!gpu_.create(error)) {
        store_.errorMessage = error;
        return false;
    }
    createPanel();
    return true;
}

void App::createPanel() {
    PanelActions actions;
    actions.changed = [this] { applySettings(); };
    actions.clear = [this] {
        for (auto& o : overlays_) o->clear();
    };
    actions.seed = [this] {
        for (auto& o : overlays_) o->seed();
    };
    // Закрытие через очередь: панель не должна уничтожаться внутри собственного обработчика.
    actions.quit = [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); };
    actions.setLogin = [this](bool enable) { store_.setLoginEnabled(enable); };
    panel_ = std::make_unique<SettingsPanel>(gpu_, store_, std::move(actions));
}

// ================================================================= кадры

void App::pacingLoop() {
    // Такт композитора Windows 11 (DCompositionWaitForCompositorClock), иначе DwmFlush.
    HMODULE dcomp = GetModuleHandleW(L"dcomp.dll");
    auto waitClock =
        dcomp ? reinterpret_cast<WaitForCompositorClock>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"))
              : nullptr;
    while (running_) {
        WaitForSingleObject(activeEvent_, INFINITE);
        if (!running_) break;
        const double start = nowSeconds();
        if (waitClock)
            waitClock(0, nullptr, 100);
        else
            DwmFlush();
        // Если система не дала такт (нет композиции), не крутимся вхолостую.
        if (nowSeconds() - start < 0.002) Sleep(8);
        SetEvent(tickEvent_);
    }
}

void App::updatePacing() {
    bool active = panel_ && panel_->animating();
    for (const auto& o : overlays_) active = active || o->animating();
    if (active) {
        SetEvent(activeEvent_);
    } else {
        ResetEvent(activeEvent_);
        nextFrame_ = 0;
    }
}

void App::frame() {
    const double now = nowSeconds();
    const double interval = (store_.economyActive || gpu_.warp) ? 1.0 / 30 : 1.0 / 60;
    // Накопитель дедлайна: на 75/90/120+ Гц кадры не теряются через один.
    if (nextFrame_ == 0) nextFrame_ = now;
    if (now + 0.003 < nextFrame_) return;
    nextFrame_ += interval;
    if (now - nextFrame_ > interval) nextFrame_ = now;
    if (windowsDirty_) refreshWindows();
    bool ok = true;
    for (auto& o : overlays_) ok = o->frame(now) && ok;
    if (panel_) panel_->frame(now);
    if (!ok || gpu_.deviceLost()) handleDeviceLost();
}

void App::handleDeviceLost() {
    overlays_.clear();
    panel_.reset();
    gpu_.destroy();
    if (!createDevices()) {
        MessageBoxW(nullptr, store_.errorMessage.c_str(), kAppName, MB_ICONERROR);
        DestroyWindow(hwnd_);
        return;
    }
    rebuildOverlays();
}

// ================================================================= состояние

void App::rebuildOverlays() {
    overlays_.clear();
    if (!gpu_.alive()) return;
    monitors_ = WindowTracker::monitors();
    const DesktopSnapshot snapshot = WindowTracker::snapshot(monitors_);
    const size_t count = store_.prefs.allScreens ? monitors_.size() : std::min<size_t>(1, monitors_.size());
    for (size_t i = 0; i < count; ++i) {
        auto overlay = std::make_unique<Overlay>(gpu_, store_, monitors_[i], snapshot);
        if (!overlay->ok()) {
            store_.errorMessage = L"Не удалось создать слой снега для монитора " + monitors_[i].device + L".";
            continue;
        }
        overlay->applySettings(sleeping());
        overlays_.push_back(std::move(overlay));
    }
    lastAllScreens_ = store_.prefs.allScreens;
    windowsDirty_ = false;
    raisePanel();
    updatePacing();
}

void App::raisePanel() {
    HWND panel = panel_ && panel_->visible() ? panel_->hwnd() : nullptr;
    // Цепочка: каждый оверлей встаёт под предыдущим, иначе при 2+ мониторах они меняются местами.
    HWND below = panel;
    for (auto& o : overlays_) {
        o->keepOnTop(below);
        below = o->hwnd();
    }
}

bool App::monitorsChanged() const {
    const auto now = WindowTracker::monitors();
    if (now.size() != monitors_.size()) return true;
    for (size_t i = 0; i < now.size(); ++i) {
        const auto &a = now[i], &b = monitors_[i];
        if (a.px.x != b.px.x || a.px.y != b.px.y || a.px.width != b.px.width || a.px.height != b.px.height ||
            a.scale != b.scale)
            return true;
    }
    return false;
}

void App::applySettings() {
    store_.updateEnergyState();
    if (lastAllScreens_ != store_.prefs.allScreens) rebuildOverlays();
    for (auto& o : overlays_) o->applySettings(sleeping());
    updateTrayIcon();
    raisePanel();
    if (panel_) panel_->refresh();
    if (!(store_.prefs == savedPrefs_)) SetTimer(hwnd_, kTimerSave, 250, nullptr);
    updatePacing();
}

void App::refreshWindows() {
    windowsDirty_ = false;
    if (!store_.enabled || sleeping() || overlays_.empty()) return;
    const DesktopSnapshot snapshot = WindowTracker::snapshot(monitors_);
    for (auto& o : overlays_) o->updateSnapshot(snapshot);
}

void CALLBACK App::winEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!instance || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (event == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    instance->windowsDirty_ = true;
}

// ================================================================= трей и панель

void App::addTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    updateTrayIcon();
    nid.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(instance_, MAKEINTRESOURCEW(1));
    wcscpy_s(nid.szTip, L"Tryflake — снег на рабочем столе");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::updateTrayIcon() {
    const UINT dpi = primaryDpi();
    const bool light = lightTaskbar(), dimmed = !store_.enabled || store_.paused;
    if (trayIcon_ && dpi == trayDpi_ && light == trayLight_ && dimmed == trayDimmed_) return;
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    HICON icon = Icons::makeTrayIcon(gpu_.d2dFactory.Get(), gpu_.wic.Get(), size, light, dimmed);
    if (!icon) return;
    trayDpi_ = dpi;
    trayLight_ = light;
    trayDimmed_ = dimmed;
    if (trayAdded_) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = kTrayID;
        nid.uFlags = NIF_ICON;
        nid.hIcon = icon;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    if (trayIcon_) DestroyIcon(trayIcon_);
    trayIcon_ = icon;
}

RECT App::trayAnchor() const {
    NOTIFYICONIDENTIFIER id{sizeof(id)};
    id.hWnd = hwnd_;
    id.uID = kTrayID;
    RECT r{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &r))) return r;
    POINT p;
    GetCursorPos(&p);
    return RECT{p.x, p.y, p.x + 1, p.y + 1};
}

void App::showPanel() {
    if (!panel_) return;
    panel_->show(trayAnchor());
    raisePanel();
    updatePacing();
}

void App::showMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuPause, store_.paused ? L"Продолжить снег" : L"Пауза");
    AppendMenuW(menu, MF_STRING, kMenuClear, L"Убрать снег");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Настройки…");
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Выйти из Tryflake");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);
    SetForegroundWindow(hwnd_);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command) {
        case kMenuPause:
            store_.paused = !store_.paused;
            applySettings();
            break;
        case kMenuClear:
            for (auto& o : overlays_) o->clear();
            break;
        case kMenuSettings: showPanel(); break;
        case kMenuQuit: DestroyWindow(hwnd_); break;
        default: break;
    }
}

// ================================================================= сообщения

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (instance && (instance->hwnd_ == hwnd || instance->hwnd_ == nullptr)) {
        if (!instance->hwnd_) instance->hwnd_ = hwnd;
        return instance->handle(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT App::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated_ && taskbarCreated_ != 0) {
        trayAdded_ = false;
        addTrayIcon();
        return 0;
    }
    switch (message) {
        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT: {
                    // NIN_SELECT и NIN_KEYSELECT иногда приходят парой: второй игнорируем.
                    const double now = nowSeconds();
                    if (now - lastTraySelect_ < 0.3) return 0;
                    lastTraySelect_ = now;
                    if (panel_ && panel_->visible())
                        panel_->hide();
                    else if (!panel_ || now - panel_->hiddenAt() > 0.3)
                        showPanel();
                    return 0;
                }
                case WM_CONTEXTMENU:
                    showMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
                    return 0;
                default: return 0;
            }
        case WM_APP_SHOW_SETTINGS:
            showPanel();
            return 0;
        case WM_TIMER:
            switch (wParam) {
                case kTimerTrack: {
                    const double now = nowSeconds();
                    if (!trayAdded_) addTrayIcon();
                    // Потеря устройства (в т.ч. DComp) и пустые оверлеи после неё.
                    if (gpu_.deviceLost()) {
                        handleDeviceLost();
                        return 0;
                    }
                    // WARP: раз в минуту пробуем вернуться на аппаратный адаптер.
                    if (gpu_.warp) {
                        if (lastWarpRetry_ == 0) lastWarpRetry_ = now;
                        if (now - lastWarpRetry_ > 60) {
                            lastWarpRetry_ = now;
                            handleDeviceLost();
                            return 0;
                        }
                    } else {
                        lastWarpRetry_ = 0;
                    }
                    if (store_.enabled && !sleeping() && overlays_.empty() && gpu_.alive()) rebuildOverlays();
                    // Полный экран D3D / режим презентации: снег скрыт, поверх ничего не поднимаем.
                    QUERY_USER_NOTIFICATION_STATE qstate = QUNS_ACCEPTS_NOTIFICATIONS;
                    if (SUCCEEDED(SHQueryUserNotificationState(&qstate))) {
                        const bool fs = qstate == QUNS_RUNNING_D3D_FULL_SCREEN || qstate == QUNS_PRESENTATION_MODE;
                        if (fs != fullscreen_) {
                            fullscreen_ = fs;
                            applySettings();
                        }
                    }
                    // Смена мониторов или DPI (у скрытого окна WM_DPICHANGED не приходит).
                    if (!rebuildPending_ && monitorsChanged()) {
                        rebuildPending_ = true;
                        SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
                    }
                    updateTrayIcon();  // кэш: пересоздаёт значок только при смене DPI/темы
                    refreshWindows();
                    if (!sleeping()) raisePanel();
                    if (store_.updateEnergyState()) applySettings();
                    updatePacing();
                    return 0;
                }
                case kTimerSave:
                    KillTimer(hwnd_, kTimerSave);
                    store_.save();
                    savedPrefs_ = store_.prefs;
                    return 0;
                case kTimerRebuild:
                    KillTimer(hwnd_, kTimerRebuild);
                    rebuildPending_ = false;
                    rebuildOverlays();
                    return 0;
                case kTimerShow:
                    KillTimer(hwnd_, kTimerShow);
                    showPanel();
                    return 0;
                case kTimerQuit:
                    DestroyWindow(hwnd_);
                    return 0;
                default: return 0;
            }
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
            return 0;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA) SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) updateTrayIcon();
            return 0;
        case WM_POWERBROADCAST:
            switch (wParam) {
                case PBT_APMSUSPEND:
                    suspended_ = true;
                    applySettings();
                    break;
                case PBT_APMRESUMEAUTOMATIC:
                case PBT_APMRESUMESUSPEND:
                    if (suspended_) {
                        suspended_ = false;
                        SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
                        applySettings();
                    }
                    break;
                case PBT_APMPOWERSTATUSCHANGE:
                    applySettings();
                    break;
                case PBT_POWERSETTINGCHANGE: {
                    const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                    if (!setting || setting->DataLength < sizeof(DWORD)) break;
                    const DWORD value = *reinterpret_cast<const DWORD*>(setting->Data);
                    if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)) {
                        const bool off = value == 0;
                        if (off != displayOff_) {
                            displayOff_ = off;
                            if (!off) SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
                            applySettings();
                        }
                    } else if (IsEqualGUID(setting->PowerSetting, GUID_ENERGY_SAVER_STATUS)) {
                        store_.energySaverOn = value != 0;
                        applySettings();
                    }
                    break;
                }
                default: break;
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT) {
                locked_ = true;
                applySettings();
            } else if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT) {
                if (locked_) {
                    locked_ = false;
                    SetTimer(hwnd_, kTimerRebuild, 400, nullptr);
                    applySettings();
                }
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) store_.save();
            return 0;
        case WM_DESTROY: {
            if (!(store_.prefs == savedPrefs_)) store_.save();
            for (auto hook : hooks_) UnhookWinEvent(hook);
            hooks_.clear();
            for (auto n : powerNotifications_)
                if (n) UnregisterPowerSettingNotification(n);
            powerNotifications_.clear();
            WTSUnRegisterSessionNotification(hwnd_);
            NOTIFYICONDATAW nid{sizeof(nid)};
            nid.hWnd = hwnd_;
            nid.uID = kTrayID;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            overlays_.clear();
            panel_.reset();
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
