#include "App.h"

#include <chrono>
#include <sstream>

using namespace snow;

namespace {

bool hasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return true;
    return false;
}

const wchar_t* argValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    return nullptr;
}

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string json(const std::wstring& s) {
    std::string out = "\"";
    for (char c : utf8(s)) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            sprintf_s(buffer, "\\u%04x", c);
            out += buffer;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

void writeOut(const std::string& text, bool error = false) {
    HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            handle = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

/// Проверка без интерфейса: реальное устройство D3D, симуляция, PNG и JSON-отчёт.
int runDiagnostics(int argc, wchar_t** argv) {
    GpuContext gpu;
    std::wstring error;
    if (!gpu.create(error)) {
        writeOut("Tryflake: " + utf8(error) + "\n", true);
        return 1;
    }
    const auto monitors = WindowTracker::monitors();
    const auto snapshot = WindowTracker::snapshot(monitors);
    SnowWorld world(1440, 900);
    world.setSurfaces(SurfaceResolver::resolve({{1, Rect{130, 275, 650, 360}},
                                                {2, Rect{865, 450, 420, 300}},
                                                {3, Rect{440, 815, 560, 74}}},
                                               1440, 900));
    world.warmStart();
    world.seedSnow(15);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1200; ++i) world.update(1.0 / 60);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (hasArg(argc, argv, L"--chunks")) {
        world.brush(Point(450, 269), SnowTool::shovel, Point(700, 80));
        for (int i = 0; i < 24; ++i) world.update(1.0 / 60);
    }
    if (const wchar_t* path = argValue(argc, argv, L"--render-preview")) {
        if (!gpu.renderPNG(world, path, error)) {
            writeOut("Tryflake: " + utf8(error) + "\n", true);
            return 1;
        }
    }
    std::ostringstream out;
    out << "{\n  \"direct3d\": " << json(gpu.adapterName) << ",\n  \"warp\": " << (gpu.warp ? "true" : "false")
        << ",\n  \"screens\": " << monitors.size() << ",\n  \"trackedWindows\": " << snapshot.windows.size()
        << ",\n  \"simulationFrames\": 1200,\n  \"simulationSeconds\": " << seconds
        << ",\n  \"particles\": " << world.particles().size() << ",\n  \"surfaces\": " << world.surfaces().size()
        << ",\n  \"chunks\": " << world.chunks().size() << ",\n  \"vertexStride\": " << sizeof(GpuVertex);
    if (hasArg(argc, argv, L"--windows")) {
        // Только прямоугольники в физических пикселях: без заголовков и содержимого окон.
        out << ",\n  \"windowFrames\": [";
        for (size_t i = 0; i < snapshot.windows.size(); ++i) {
            const auto& r = snapshot.windows[i].rect;
            out << (i ? ", " : "") << "[" << r.x << ", " << r.y << ", " << r.width << ", " << r.height << "]";
        }
        out << "]";
        out << ",\n  \"desktopSurfaces\": [";
        for (size_t d = 0; d < monitors.size(); ++d) {
            const auto& m = monitors[d];
            out << (d ? "," : "") << "\n    {\"display\": " << d << ", \"device\": " << json(m.device)
                << ", \"width\": " << m.px.width / m.scale << ", \"height\": " << m.px.height / m.scale
                << ", \"scale\": " << m.scale << ", \"edges\": [";
            const auto edges = WindowTracker::surfaces(m, snapshot);
            for (size_t e = 0; e < edges.size(); ++e) {
                const auto& s = edges[e];
                const auto exposed = std::count(s.exposed.begin(), s.exposed.end(), uint8_t(1));
                out << (e ? ", " : "") << "{\"id\": " << s.id << ", \"x\": " << s.x << ", \"y\": " << s.y
                    << ", \"width\": " << s.width << ", \"exposedBins\": " << exposed << ", \"totalBins\": " << s.binCount()
                    << "}";
            }
            out << "]}";
        }
        out << "\n  ]";
    }
    out << "\n}\n";
    writeOut(out.str());
    gpu.destroy();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (hasArg(argc, argv, L"--diagnostics")) {
        const int code = runDiagnostics(argc, argv);
        LocalFree(argv);
        CoUninitialize();
        return code;
    }

    // Один экземпляр: повторный запуск открывает настройки уже работающего.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Tryflake.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Первый экземпляр мог только что стартовать и ещё не создать окно: ждём до ~2 с.
        HWND existing = nullptr;
        for (int attempt = 0; attempt < 20 && !existing; ++attempt) {
            existing = FindWindowW(L"TryflakeMain", nullptr);
            if (!existing) Sleep(100);
        }
        if (existing) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_APP_SHOW_SETTINGS, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        LocalFree(argv);
        return 0;
    }

    int code = 0;
    {
        // App (и все COM-объекты внутри) уничтожается до CoUninitialize.
        App app;
        code = app.run(hasArg(argc, argv, L"--show-settings"), hasArg(argc, argv, L"--smoke-test"));
    }
    LocalFree(argv);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    CoUninitialize();
    return code;
}
