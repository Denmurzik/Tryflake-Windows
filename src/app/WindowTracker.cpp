#include "WindowTracker.h"

using namespace snow;

namespace {

BOOL CALLBACK monitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    MonitorInfo m;
    m.handle = monitor;
    m.px = Rect{static_cast<double>(info.rcMonitor.left), static_cast<double>(info.rcMonitor.top),
                static_cast<double>(info.rcMonitor.right - info.rcMonitor.left),
                static_cast<double>(info.rcMonitor.bottom - info.rcMonitor.top)};
    m.scale = std::max(0.5, dpiX / 96.0);
    m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.device = info.szDevice;
    list->push_back(m);
    return TRUE;
}

struct EnumState {
    DWORD ownPID;
    std::vector<WindowCandidate> candidates;
    std::vector<Rect> displays;
};

bool classIs(const wchar_t* name, std::initializer_list<const wchar_t*> list) {
    for (const auto* item : list)
        if (wcscmp(name, item) == 0) return true;
    return false;
}

BOOL CALLBACK windowProc(HWND hwnd, LPARAM data) {
    auto* state = reinterpret_cast<EnumState*>(data);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == state->ownPID) return TRUE;
    // Окна других виртуальных рабочих столов и скрытые UWP-окна «замаскированы» DWM.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // Прозрачные для мыши окна — это оверлеи других программ: их не видно, на них нет снега.
    if (ex & WS_EX_TRANSPARENT) return TRUE;

    wchar_t className[128] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    OwnerKind kind = OwnerKind::application;
    if (classIs(className, {L"Progman", L"WorkerW", L"#32768", L"tooltips_class32", L"SysShadow",
                            L"Xaml_WindowedPopupClass", L"Shell_InputSwitchTopLevelWindow", L"ForegroundStaging",
                            L"TaskListThumbnailWnd", L"TaskListOverlayWnd", L"Windows.UI.Input.InputSite.WindowClass",
                            L"IME", L"MSCTFIME UI", L"TryflakeOverlay"})) {
        kind = OwnerKind::ignoredSystem;
    } else if (classIs(className, {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})) {
        kind = OwnerKind::taskbar;
    } else if ((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_TOPMOST)) {
        kind = OwnerKind::toolOverlay;
    }
    if (kind == OwnerKind::ignoredSystem) return TRUE;

    RECT r{};
    // Видимая рамка без невидимых зон изменения размера.
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
    double alpha = 1;
    if (ex & WS_EX_LAYERED) {
        BYTE a = 255;
        DWORD flags = 0;
        COLORREF key = 0;
        if (GetLayeredWindowAttributes(hwnd, &key, &a, &flags) && (flags & LWA_ALPHA)) alpha = a / 255.0;
    }
    WindowCandidate c;
    c.id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    c.processID = pid;
    c.rect = Rect{static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right - r.left),
                  static_cast<double>(r.bottom - r.top)};
    c.alpha = alpha;
    c.owner = kind;
    state->candidates.push_back(c);
    return TRUE;
}

}  // namespace

std::vector<MonitorInfo> WindowTracker::monitors() {
    std::vector<MonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, monitorProc, reinterpret_cast<LPARAM>(&list));
    std::stable_sort(list.begin(), list.end(), [](const MonitorInfo& a, const MonitorInfo& b) { return a.primary > b.primary; });
    return list;
}

DesktopSnapshot WindowTracker::snapshot(const std::vector<MonitorInfo>& monitors) {
    EnumState state;
    state.ownPID = GetCurrentProcessId();
    for (const auto& m : monitors) state.displays.push_back(m.px);
    EnumWindows(windowProc, reinterpret_cast<LPARAM>(&state));
    DesktopSnapshot snapshot;
    snapshot.windows = WindowSelection::select(state.candidates, state.ownPID, state.displays);
    return snapshot;
}

std::vector<SurfaceGeometry> WindowTracker::surfaces(const MonitorInfo& monitor, const DesktopSnapshot& snapshot) {
    std::vector<WindowFrame> local;
    local.reserve(snapshot.windows.size());
    for (const auto& w : snapshot.windows) {
        local.push_back(WindowFrame{w.id, SurfaceResolver::localRect(w.rect, monitor.px, monitor.scale)});
    }
    return SurfaceResolver::resolve(local, monitor.px.width / monitor.scale, monitor.px.height / monitor.scale);
}
