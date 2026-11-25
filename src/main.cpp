#include "ring.h"
#include "settings.h"

#include <shlobj.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

Config g_config;
std::atomic<bool> g_running{ true };
std::atomic<bool> g_restart{ false };
Gpu g_gpu;
StreamInfo g_videoInfo, g_audioInfo;

static LARGE_INTEGER s_qpcFreq, s_qpcStart;
static FILE* s_log = nullptr;
static std::mutex s_logMutex;
static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\VramReplay";

int64_t now_us() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - s_qpcStart.QuadPart) * 1'000'000 / s_qpcFreq.QuadPart;
}

std::string wide_to_utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void log_line(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard lock(s_logMutex);
    fprintf(stdout, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
    fflush(stdout);
    if (s_log) {
        fprintf(s_log, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fflush(s_log);
    }
}

bool gpu_init() {
    int ret = av_hwdevice_ctx_create(&g_gpu.deviceRef, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (ret < 0) {
        log_line("gpu: could not create D3D11 device (%d)", ret);
        return false;
    }
    auto* dev = (AVD3D11VADeviceContext*)((AVHWDeviceContext*)g_gpu.deviceRef->data)->hwctx;
    g_gpu.device = dev->device;
    g_gpu.context = dev->device_context;

    // Let the game's GPU work always go first: our capture copies and AMF's colour conversion run at
    // below-normal GPU scheduling priority (OBS uses the same API the other way round, to raise itself).
    using SetPrioClass = LONG(APIENTRY*)(HANDLE, int);
    if (auto fn = (SetPrioClass)(void*)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass")) {
        LONG st = fn(GetCurrentProcess(), 1 /* D3DKMT_SCHEDULINGPRIORITYCLASS_BELOW_NORMAL */);
        log_line("gpu: scheduling priority below normal (status 0x%08lx)", st);
    }

    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(g_gpu.device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi))) {
        dxgi->SetGPUThreadPriority(-7);   // lowest; per-device hint on top of the process class
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc{};
            adapter->GetDesc(&desc);
            log_line("gpu: %s, %llu MB VRAM", wide_to_utf8(desc.Description).c_str(), (unsigned long long)(desc.DedicatedVideoMemory >> 20));
            adapter->Release();
        }
        dxgi->Release();
    }
    return true;
}

// config.ini first (written by the editor's settings page), then command-line overrides
static void parse_args(int argc, wchar_t** argv) {
    Settings s = settings::load();
    g_config.seconds = s.seconds;
    g_config.fps = s.fps;
    g_config.bitrate = (int64_t)s.bitrateMbps * 1'000'000;
    g_config.maxrate = settings::peak_bps(s.bitrateMbps);
    g_config.captureMic = s.mic;
    g_config.micGain = s.micGain / 100.0;
    g_config.desktopGain = s.desktopGain / 100.0;
    g_config.saveSound = s.saveSound;
    g_config.gameNames = s.gameNames;
    g_config.notify = s.notify;
    g_config.micDevice = s.micDevice;
    g_config.outputDir = s.outputDir;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        size_t eq = a.find(L'=');
        if (a.rfind(L"--", 0) != 0 || eq == std::wstring::npos) continue;
        std::wstring key = a.substr(2, eq - 2), val = a.substr(eq + 1);
        if (key == L"seconds") g_config.seconds = std::stoi(val);
        else if (key == L"fps") g_config.fps = std::stoi(val);
        else if (key == L"monitor") g_config.monitor = std::stoi(val);
        else if (key == L"codec") g_config.codec = wide_to_utf8(val);
        else if (key == L"bitrate") g_config.bitrate = (int64_t)(std::stod(val) * 1'000'000);
        else if (key == L"maxrate") g_config.maxrate = (int64_t)(std::stod(val) * 1'000'000);
        else if (key == L"mic-gain") g_config.micGain = std::stod(val);
        else if (key == L"desktop-gain") g_config.desktopGain = std::stod(val);
        else if (key == L"mic") g_config.captureMic = val != L"0" && val != L"off";
        else if (key == L"output") g_config.outputDir = val;
    }
    g_config.maxrate = std::max(g_config.maxrate, g_config.bitrate);
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

static std::string handle_command(const std::string& line) {
    if (line.rfind("save", 0) == 0) {
        int seconds = g_config.seconds;
        if (line.size() > 5) seconds = std::max(1, std::min(g_config.seconds, atoi(line.c_str() + 5)));
        std::string err;
        std::wstring path = save_replay(seconds, &err);
        if (path.empty()) return "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}";
        return "{\"ok\":true,\"path\":\"" + json_escape(wide_to_utf8(path)) + "\",\"seconds\":" + std::to_string(seconds) + "}";
    }
    if (line == "status") {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"video\":%s,\"audio\":%s,\"ringMB\":%llu,\"usedMB\":%llu,\"seconds\":%d,\"fps\":%d,\"bitrateMbps\":%lld,\"autostart\":%s,"
            "\"recording\":%s,\"recordSeconds\":%d}",
            g_videoInfo.ready ? "true" : "false", g_audioInfo.ready ? "true" : "false",
            (unsigned long long)(g_ring.capacity() >> 20), (unsigned long long)(g_ring.bytesUsed() >> 20),
            g_config.seconds, g_config.fps, (long long)(g_config.bitrate / 1'000'000), autostart_enabled() ? "true" : "false",
            record_active() ? "true" : "false", record_seconds());
        return buf;
    }
    if (line == "level") {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"desktopL\":%.4f,\"desktopR\":%.4f,\"micL\":%.4f,\"micR\":%.4f,\"micOn\":%s,\"micName\":\"%s\"}",
            g_levels[0][0].load(), g_levels[0][1].load(), g_levels[1][0].load(), g_levels[1][1].load(),
            g_config.captureMic ? "true" : "false", json_escape(audio_mic_name()).c_str());
        return buf;
    }
    if (line.rfind("gain ", 0) == 0) {
        // "gain <desktop %> <mic %>": live volume from the settings mixer, no restart
        int desktop = 100, mic = 100;
        if (sscanf(line.c_str() + 5, "%d %d", &desktop, &mic) != 2) return "{\"ok\":false}";
        audio_set_gain(std::clamp(desktop, 0, 400) / 100.0, std::clamp(mic, 0, 3200) / 100.0);
        return "{\"ok\":true}";
    }
    if (line == "testtoast") {
        toast(TOAST_SAVED, L"通知のテストです", L"保存したときはこの位置に、この形で出ます");
        return "{\"ok\":true}";
    }
    if (line == "screenshot") {
        request_screenshot(0);
        return "{\"ok\":true}";
    }
    if (line == "record start" || line == "record stop" || line == "record" || line == "record toggle") {
        bool start = line == "record start" || (line != "record stop" && !record_active());
        if (start) {
            std::string err;
            if (!record_active() && !record_start(&err)) return "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}";
        } else {
            record_stop();
        }
        return std::string("{\"ok\":true,\"recording\":") + (start ? "true" : "false") + "}";
    }
    if (line == "autostart 1" || line == "autostart 0") {
        set_autostart(line.back() == '1');
        return std::string("{\"ok\":true,\"autostart\":") + (autostart_enabled() ? "true" : "false") + "}";
    }
    if (line == "restart") {
        // Settings changed: shut the pipeline down cleanly and start a fresh process that reads config.ini
        g_restart = true;
        g_running = false;
        return "{\"ok\":true}";
    }
    if (line == "quit") {
        g_running = false;
        return "{\"ok\":true}";
    }
    return "{\"ok\":false,\"error\":\"unknown command\"}";
}

// Local control channel: one command per connection ("save", "save 15", "status", "quit")
static void pipe_server() {
    while (g_running) {
        HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::thread([pipe] {
                char buf[256];
                DWORD n = 0;
                std::string reply = "{\"ok\":false}";
                if (ReadFile(pipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
                    buf[n] = 0;
                    std::string line(buf);
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                    reply = handle_command(line);
                }
                reply += "\n";
                DWORD w;
                WriteFile(pipe, reply.data(), (DWORD)reply.size(), &w, nullptr);
                FlushFileBuffers(pipe);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
            }).detach();
        } else {
            CloseHandle(pipe);
        }
    }
}

static BOOL WINAPI on_console_ctrl(DWORD) {
    g_running = false;
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&s_qpcFreq);
    QueryPerformanceCounter(&s_qpcStart);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\VramReplaySingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // A restart hands over to a new process; give the old one a moment to let go
        for (int i = 1; i < argc; i++)
            if (std::wstring(argv[i]) == L"--restarted") {
                CloseHandle(single);
                for (int t = 0; t < 50; t++) {
                    Sleep(100);
                    single = CreateMutexW(nullptr, TRUE, L"Local\\VramReplaySingleInstance");
                    if (GetLastError() != ERROR_ALREADY_EXISTS) goto acquired;
                    CloseHandle(single);
                }
                return 0;
            }
        // Started by the logon task while already running: stay quiet
        for (int i = 1; i < argc; i++)
            if (std::wstring(argv[i]) == L"--background") return 0;
        MessageBoxW(nullptr, L"VramReplay はすでに動いています。\n通知領域（タスクバー右下）のアイコンから操作できます。",
            L"VramReplay", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
acquired:

    parse_args(argc, argv);
    CreateDirectoryW(g_config.outputDir.c_str(), nullptr);
    std::wstring logDir = settings::dir();
    CreateDirectoryW(logDir.c_str(), nullptr);
    std::wstring logPath = logDir + L"\\log.txt";
    // Keep the log small: start a new one past 1 MB, keeping the previous as log.old.txt
    WIN32_FILE_ATTRIBUTE_DATA la;
    if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &la) && (la.nFileSizeHigh || la.nFileSizeLow > (1u << 20)))
        MoveFileExW(logPath.c_str(), (logDir + L"\\log.old.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    s_log = _wfsopen(logPath.c_str(), L"a", _SH_DENYWR);   // others may read the log while we run

    av_log_set_level(AV_LOG_WARNING);
    log_line("VramReplay starting: %ds, %dfps, %lld Mbps, mic %s", g_config.seconds, g_config.fps,
        (long long)(g_config.bitrate / 1'000'000), g_config.captureMic ? "on" : "off");
    if (!gpu_init()) return 1;

    // Room for the replay at peak bitrate plus slack, so a save in progress is never overwritten
    auto ringBytesFor = [](const Config& c) { return (uint64_t)((c.maxrate + 320'000) / 8.0 * (c.seconds + 5) * 2); };
    if (!g_ring.init(g_gpu.device, g_gpu.context, ringBytesFor(g_config))) {
        // A setting the GPU can't hold must not leave a recorder that never starts: fall back to defaults
        Config def;
        g_config.seconds = def.seconds;
        g_config.bitrate = def.bitrate;
        g_config.maxrate = def.maxrate;
        log_line("ring: falling back to %ds at %lld Mbps", g_config.seconds, (long long)(g_config.bitrate / 1'000'000));
        if (!g_ring.init(g_gpu.device, g_gpu.context, ringBytesFor(g_config))) return 1;
    }

    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    video_start();
    audio_start();
    std::thread(pipe_server).detach();

    // Tray icon + Ctrl+Alt+Shift+F10 hotkey; returns when quitting
    tray_run();

    log_line("shutting down");
    video_stop();
    audio_stop();
    // A manual recording in progress is finalised before quitting or restarting
    record_shutdown();
    g_ring.shutdown();
    av_buffer_unref(&g_gpu.deviceRef);
    timeEndPeriod(1);

    if (g_restart) {
        log_line("restarting with new settings");
        if (s_log) fclose(s_log);
        s_log = nullptr;
        ReleaseMutex(single);
        CloseHandle(single);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --background --restarted";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return 0;
    }
    if (s_log) fclose(s_log);
    return 0;
}
