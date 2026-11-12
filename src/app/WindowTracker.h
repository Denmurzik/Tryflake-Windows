#pragma once

#include "Common.h"
#include "../core/SnowCore.h"

struct MonitorInfo {
    HMONITOR handle = nullptr;
    snow::Rect px;  // физические пиксели виртуального экрана
    double scale = 1;
    bool primary = false;
    std::wstring device;
};

struct DesktopSnapshot {
    std::vector<snow::WindowFrame> windows;  // спереди назад, физические пиксели
};

namespace WindowTracker {
std::vector<MonitorInfo> monitors();
/// Метаданные окон: прямоугольники и порядок. Заголовки и содержимое не читаются.
DesktopSnapshot snapshot(const std::vector<MonitorInfo>& monitors);
std::vector<snow::SurfaceGeometry> surfaces(const MonitorInfo& monitor, const DesktopSnapshot& snapshot);
}
