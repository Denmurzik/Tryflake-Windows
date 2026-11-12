#include "Settings.h"

namespace {

constexpr wchar_t kKey[] = L"Software\\Tryflake";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

bool readString(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring& out) {
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return false;
    out = buffer;
    return true;
}

double readDouble(const wchar_t* name, double fallback, double lo, double hi) {
    std::wstring text;
    if (!readString(HKEY_CURRENT_USER, kKey, name, text)) return fallback;
    wchar_t* end = nullptr;
    const double v = wcstod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

DWORD readDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

void writeDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void writeDouble(HKEY key, const wchar_t* name, double value) {
    wchar_t text[64];
    swprintf_s(text, L"%.6f", value);
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text),
                   static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t)));
}

std::wstring exeCommand() {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return L"\"" + std::wstring(path) + L"\"";
}

}  // namespace

void SettingsStore::load() {
    Preferences p;
    p.snowfall = static_cast<snow::Snowfall>(std::min<DWORD>(2, readDword(L"Snowfall", 1)));
    p.tool = static_cast<snow::SnowTool>(std::min<DWORD>(2, readDword(L"Tool", 0)));
    p.wind.x = readDouble(L"WindX", 0, -1, 1);
    p.wind.y = readDouble(L"WindY", 0, -1, 1);
    p.opacity = readDouble(L"Opacity", 0.88, 0.25, 1);
    p.maxDepth = readDouble(L"MaxDepth", 22, 4, 60);
    p.accumulation = readDword(L"Accumulation", 1) != 0;
    p.allScreens = readDword(L"AllScreens", 1) != 0;
    p.energyAware = readDword(L"EnergyAware", 1) != 0;
    prefs = p;
    refreshLogin();
}

void SettingsStore::save() const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    writeDword(key, L"Snowfall", static_cast<DWORD>(prefs.snowfall));
    writeDword(key, L"Tool", static_cast<DWORD>(prefs.tool));
    writeDouble(key, L"WindX", prefs.wind.x);
    writeDouble(key, L"WindY", prefs.wind.y);
    writeDouble(key, L"Opacity", prefs.opacity);
    writeDouble(key, L"MaxDepth", prefs.maxDepth);
    writeDword(key, L"Accumulation", prefs.accumulation);
    writeDword(key, L"AllScreens", prefs.allScreens);
    writeDword(key, L"EnergyAware", prefs.energyAware);
    RegCloseKey(key);
}

bool SettingsStore::firstLaunch() {
    if (readDword(L"HasLaunched", 0) != 0) return false;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        writeDword(key, L"HasLaunched", 1);
        RegCloseKey(key);
    }
    return true;
}

snow::SnowConfiguration SettingsStore::configuration() const {
    snow::SnowConfiguration c;
    c.snowfall = prefs.snowfall;
    c.wind = prefs.wind;
    c.accumulation = prefs.accumulation;
    c.maxDepth = prefs.maxDepth;
    c.quality = economyActive ? 0.55 : 1;
    return c;
}

bool SettingsStore::updateEnergyState() {
    SYSTEM_POWER_STATUS status{};
    bool saver = energySaverOn;
    if (GetSystemPowerStatus(&status) && status.SystemStatusFlag == 1) saver = true;
    const bool active = prefs.energyAware && saver;
    if (active == economyActive) return false;
    economyActive = active;
    return true;
}

void SettingsStore::refreshLogin() {
    std::wstring value;
    loginEnabled = readString(HKEY_CURRENT_USER, kRunKey, kAppName, value);
}

void SettingsStore::setLoginEnabled(bool enable) {
    HKEY key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_SUCCESS) {
        if (enable) {
            const std::wstring command = exeCommand();
            status = RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        } else {
            status = RegDeleteValueW(key, kAppName);
            if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        }
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) errorMessage = L"Автозапуск: " + hrMessage(HRESULT_FROM_WIN32(status));
    refreshLogin();
    if (enable && loginEnabled) {
        // Пользователь мог отключить программу в «Диспетчер задач → Автозагрузка».
        BYTE data[12]{};
        DWORD size = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kAppName, RRF_RT_REG_BINARY, nullptr, data, &size) ==
                ERROR_SUCCESS &&
            size > 0 && (data[0] & 1) == 1) {
            errorMessage = L"Автозапуск отключён в «Параметры → Приложения → Автозагрузка». Включите Tryflake там.";
        }
    }
}

std::wstring snowfallTitle(snow::Snowfall s) {
    switch (s) {
        case snow::Snowfall::light: return L"Лёгкий";
        case snow::Snowfall::steady: return L"Ровный";
        case snow::Snowfall::blizzard: return L"Метель";
    }
    return L"";
}

std::wstring toolTitle(snow::SnowTool t) {
    switch (t) {
        case snow::SnowTool::mitten: return L"Варежка";
        case snow::SnowTool::shovel: return L"Лопата";
        case snow::SnowTool::snowball: return L"Снежки";
    }
    return L"";
}

std::wstring toolHint(snow::SnowTool t) {
    switch (t) {
        case snow::SnowTool::mitten: return L"Сталкивайте снег курсором — сугроб падает кусками.";
        case snow::SnowTool::shovel: return L"Проведите по сугробу, чтобы расчистить широкую полосу.";
        case snow::SnowTool::snowball: return L"Тяните снег вдоль края окна. Сложите три шара в снеговика.";
    }
    return L"";
}
