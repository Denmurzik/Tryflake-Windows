#pragma once

#include "Common.h"
#include "../core/SnowCore.h"

struct Preferences {
    snow::Snowfall snowfall = snow::Snowfall::steady;
    snow::SnowTool tool = snow::SnowTool::mitten;
    snow::Point wind;
    double opacity = 0.88;
    bool accumulation = true;
    bool allScreens = true;
    bool energyAware = true;
    double maxDepth = 22;
    bool operator==(const Preferences&) const = default;
};

/// Состояние приложения + хранение в HKCU\Software\Tryflake.
class SettingsStore {
public:
    Preferences prefs;
    bool enabled = true;
    bool paused = false;
    bool loginEnabled = false;
    bool economyActive = false;
    std::wstring errorMessage;

    /// Сигналы энергосбережения Windows, выставляет App.
    bool energySaverOn = false;

    void load();
    void save() const;
    snow::SnowConfiguration configuration() const;
    /// true, если режим экономии изменился.
    bool updateEnergyState();
    void setLoginEnabled(bool enable);
    void refreshLogin();
    bool firstLaunch();
};

std::wstring snowfallTitle(snow::Snowfall s);
std::wstring toolTitle(snow::SnowTool t);
std::wstring toolHint(snow::SnowTool t);
