// Notification-area icon: lets the recorder run in the background without a console window.
// Menu: save replay, open the editor, open the clips folder, start with Windows, quit.
#include "common.h"

#include <shellapi.h>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

static constexpr UINT WM_TRAY = WM_APP + 1;
static constexpr UINT TRAY_ID = 1;
static constexpr int HOTKEY_SAVE = 1, HOTKEY_RECORD = 2, HOTKEY_SHOT = 3;
enum : UINT { CMD_SAVE = 100, CMD_RECORD, CMD_SHOT, CMD_EDITOR, CMD_FOLDER, CMD_AUTOSTART, CMD_QUIT };

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"VramReplay";

// ---------- foreground app (for clip names) ----------
// Tracked continuously so a save from the tray menu (which puts us in front) still names the game.

static std::mutex s_fgMutex;
static std::wstring s_fgExe;

static std::wstring exe_of_window(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return L"";
    std::wstring path;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) path = buf;
        CloseHandle(p);
    }
    return path;
}

static void CALLBACK on_foreground(HWINEVENTHOOK, DWORD, HWND w, LONG, LONG, DWORD, DWORD) {
    std::wstring exe = exe_of_window(w);
    if (exe.empty()) return;
    std::lock_guard lock(s_fgMutex);
    s_fgExe = exe;
}

// "Overwatch" from Overwatch.exe's version info, reduced to characters that are safe in any file name
std::wstring foreground_app_name() {
    std::wstring exe;
    {
        std::lock_guard lock(s_fgMutex);
        exe = s_fgExe;
    }
    if (exe.empty()) return L"";
    std::wstring base = exe.substr(exe.find_last_of(L'\\') + 1);
    if (base.size() > 4) base.resize(base.size() - 4);
    for (const wchar_t* shell : { L"explorer", L"ShellExperienceHost", L"StartMenuExperienceHost", L"SearchHost", L"LockApp" })
        if (_wcsicmp(base.c_str(), shell) == 0) return L"Desktop";

    std::wstring name;
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (size) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) {
            struct Lang { WORD lang, cp; } *langs = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) && len >= sizeof(Lang)) {
                // ProductName ("Overwatch") reads better than FileDescription ("Overwatch Application")
                for (const wchar_t* field : { L"ProductName", L"FileDescription" }) {
                    wchar_t key[96];
                    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].cp, field);
                    wchar_t* v = nullptr;
                    UINT vlen = 0;
                    // Windows' own programs all share the ProductName "Microsoft Windows Operating System"
                    if (VerQueryValueW(data.data(), key, (void**)&v, &vlen) && v && wcslen(v) >= 2 && wcslen(v) <= 40 &&
                        !wcsstr(v, L"Operating System")) { name = v; break; }
                }
            }
        }
    }
    auto clean = [](const std::wstring& in) {
        std::wstring out;
        for (wchar_t c : in) {
            if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-') out += c;
            else if (c == L' ' && !out.empty() && out.back() != L' ') out += c;
        }
        while (!out.empty() && out.back() == L' ') out.pop_back();
        if (out.size() > 32) out.resize(32);
        return out;
    };
    std::wstring result = clean(name);
    for (const wchar_t* tail : { L" Application", L" Game Client", L" Client", L" Launcher", L" Game" }) {
        size_t n = wcslen(tail);
        if (result.size() > n + 1 && _wcsicmp(result.c_str() + result.size() - n, tail) == 0) { result.resize(result.size() - n); break; }
    }
    // Descriptions like "Overwatch 2 Game Client" are fine; empty or unhelpful ones fall back to the exe name
    if (result.size() < 2) result = clean(base);
    return result;
}

// ---------- save chime ----------
// A soft two-note blip built in memory. It plays on the default device, so it is also faintly in later replays.
void play_save_sound() {
    static std::vector<uint8_t> wav = [] {
        const int rate = 44100;
        std::vector<int16_t> pcm;
        auto tone = [&](double hz, double secs) {
            int n = (int)(rate * secs);
            for (int i = 0; i < n; i++) {
                double env = std::min(1.0, std::min(i / (rate * 0.005), (n - i) / (rate * 0.03)));
                pcm.push_back((int16_t)(sin(2 * 3.14159265358979 * hz * i / rate) * env * 0.18 * 32767));
            }
        };
        tone(880, 0.06);
        tone(1320, 0.09);
        std::vector<uint8_t> out(44 + pcm.size() * 2);
        auto put32 = [&](size_t at, uint32_t v) { memcpy(&out[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { memcpy(&out[at], &v, 2); };
        memcpy(&out[0], "RIFF", 4); put32(4, (uint32_t)out.size() - 8); memcpy(&out[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
        memcpy(&out[36], "data", 4); put32(40, (uint32_t)pcm.size() * 2);
        memcpy(&out[44], pcm.data(), pcm.size() * 2);
        return out;
    }();
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static HWND s_wnd = nullptr;
static NOTIFYICONDATAW s_nid{};
static UINT s_taskbarCreated = 0;

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L'\\'));
}

static std::wstring exe_path() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// Autostart is a Task Scheduler logon task. Explorer's Run key proved unreliable here: with many startup
// apps it waits ~30 s on each one in turn and gave up before reaching ours.
static const wchar_t* TASK_NAME = L"VramReplay";

static DWORD run_schtasks(const std::wstring& args) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\schtasks.exe\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

bool autostart_enabled() {
    return run_schtasks(L"/Query /TN " + std::wstring(TASK_NAME)) == 0;
}

static void remove_run_key() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    RegDeleteValueW(key, RUN_VALUE);
    RegCloseKey(key);
}

void set_autostart(bool on) {
    remove_run_key();   // older builds used the Run key; never start twice
    if (on) {
        // At this user's logon (+20 s), not elevated, no 3-day time limit, keeps running on battery.
        // Built as XML because plain /Create gives tasks a 72-hour execution limit.
        wchar_t user[256];
        DWORD n = 256;
        GetUserNameW(user, &n);
        wchar_t domain[256];
        DWORD dn = 256;
        if (!GetEnvironmentVariableW(L"USERDOMAIN", domain, dn)) domain[0] = 0;
        std::wstring who = domain[0] ? std::wstring(domain) + L"\\" + user : std::wstring(user);
        std::wstring xml =
            L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
            L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
            L"<RegistrationInfo><Description>VramReplay instant replay recorder</Description></RegistrationInfo>\n"
            L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + who + L"</UserId><Delay>PT20S</Delay></LogonTrigger></Triggers>\n"
            L"<Principals><Principal id=\"Author\"><UserId>" + who + L"</UserId><LogonType>InteractiveToken</LogonType>"
            L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\n"
            L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
            L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
            L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><AllowHardTerminate>true</AllowHardTerminate>"
            L"<StartWhenAvailable>false</StartWhenAvailable><Enabled>true</Enabled><Priority>7</Priority></Settings>\n"
            L"<Actions Context=\"Author\"><Exec><Command>" + exe_path() + L"</Command><Arguments>--background</Arguments>"
            L"<WorkingDirectory>" + exe_dir() + L"</WorkingDirectory></Exec></Actions>\n"
            L"</Task>\n";
        wchar_t tmp[MAX_PATH], file[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        GetTempFileNameW(tmp, L"vrt", 0, file);
        HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            const wchar_t bom = 0xFEFF;
            WriteFile(h, &bom, sizeof(bom), &w, nullptr);
            WriteFile(h, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)), &w, nullptr);
            CloseHandle(h);
            DWORD rc = run_schtasks(L"/Create /F /TN " + std::wstring(TASK_NAME) + L" /XML \"" + file + L"\"");
            log_line("autostart: logon task %s", rc == 0 ? "registered" : "could not be registered");
            DeleteFileW(file);
        }
    } else {
        run_schtasks(L"/Delete /F /TN " + std::wstring(TASK_NAME));
    }
}

// A red "recording" dot, drawn at runtime so the tray icon needs no resource
static HICON make_icon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = (uint32_t*)bits;
    float c = (size - 1) / 2.0f, outer = size * 0.48f, ring = size * 0.40f, dot = size * 0.26f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            auto cov = [&](float r) { return std::clamp(r - d + 0.5f, 0.0f, 1.0f); };
            // dark disc, light ring, red centre dot (premultiplied alpha)
            float a = cov(outer);
            float ringMix = cov(ring) - cov(ring - size * 0.07f);
            float dotMix = cov(dot);
            float r = 30 + ringMix * 200 + dotMix * (235 - 30);
            float g = 32 + ringMix * 200 + dotMix * (64 - 32);
            float b = 36 + ringMix * 200 + dotMix * (64 - 36);
            px[y * size + x] = ((uint32_t)(a * 255) << 24) | ((uint32_t)(r * a) << 16) | ((uint32_t)(g * a) << 8) | (uint32_t)(b * a);
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void add_icon() {
    if (!Shell_NotifyIconW(NIM_ADD, &s_nid)) log_line("tray: could not add the notification icon");
    else log_line("tray: icon added");
    s_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &s_nid);
}

static void notify(const wchar_t* title, const std::wstring& text, DWORD flags) {
    NOTIFYICONDATAW n = s_nid;
    n.uFlags = NIF_INFO;
    // Quiet: no sound, and Windows holds it back during fullscreen games / focus assist
    n.dwInfoFlags = flags | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void start_save() {
    // save_replay shows its own notification (also for saves from the pipe / Stream Dock)
    std::thread([] { save_replay(g_config.seconds, nullptr); }).detach();
}

static void open_editor() {
    std::wstring editor = exe_dir() + L"\\VramEdit.exe";
    ShellExecuteW(nullptr, L"open", editor.c_str(), nullptr, exe_dir().c_str(), SW_SHOWNORMAL);
}

static void show_menu(HWND wnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_SAVE, L"リプレイを保存\tCtrl+Alt+Shift+F10");
    wchar_t rec[64];
    int rs = record_seconds();
    if (record_active()) swprintf_s(rec, L"録画を停止（%d:%02d）\tCtrl+Alt+Shift+F11", rs / 60, rs % 60);
    else wcscpy_s(rec, L"録画を開始\tCtrl+Alt+Shift+F11");
    AppendMenuW(m, MF_STRING, CMD_RECORD, rec);
    AppendMenuW(m, MF_STRING, CMD_SHOT, L"スクリーンショット\tCtrl+Alt+Shift+F1");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EDITOR, L"エディタを開く");
    AppendMenuW(m, MF_STRING, CMD_FOLDER, L"保存フォルダを開く");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), CMD_AUTOSTART, L"Windows 起動時に自動で開始");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_QUIT, L"終了");
    SetMenuDefaultItem(m, CMD_EDITOR, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(wnd);   // required so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, nullptr);
    PostMessageW(wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == s_taskbarCreated && s_taskbarCreated) {
        add_icon();     // Explorer restarted
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            show_menu(wnd);
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            open_editor();
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CMD_SAVE: start_save(); break;
        case CMD_RECORD: record_toggle(); break;
        case CMD_SHOT: request_screenshot(400); break;   // let the menu fade out first
        case CMD_EDITOR: open_editor(); break;
        case CMD_FOLDER: ShellExecuteW(nullptr, L"open", g_config.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case CMD_AUTOSTART: set_autostart(!autostart_enabled()); break;
        case CMD_QUIT: g_running = false; break;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_SAVE) start_save();
        else if (wp == HOTKEY_RECORD) record_toggle();
        else if (wp == HOTKEY_SHOT) request_screenshot(0);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) g_running = false;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Runs the tray icon and hotkey on the calling thread until g_running goes false
void tray_run() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"VramReplayTray";
    RegisterClassW(&wc);
    // A hidden top-level window (not message-only, so it also receives TaskbarCreated and session-end messages)
    s_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"VramReplay", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HWINEVENTHOOK fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, on_foreground, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    on_foreground(nullptr, 0, GetForegroundWindow(), 0, 0, 0, 0);

    // Move an autostart left in the Run key by older builds over to the logon task
    {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            std::thread([] { set_autostart(true); }).detach();
    }

    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_wnd;
    s_nid.uID = TRAY_ID;
    s_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    s_nid.uCallbackMessage = WM_TRAY;
    int sm = GetSystemMetrics(SM_CXSMICON);
    s_nid.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, sm, sm, 0);
    if (!s_nid.hIcon) s_nid.hIcon = make_icon(sm);
    wchar_t tip[128];
    swprintf_s(tip, L"VramReplay — 直近%d秒をリプレイ録画中\nCtrl+Alt+Shift+F10 で保存", g_config.seconds);
    wcsncpy_s(s_nid.szTip, tip, _TRUNCATE);
    add_icon();

    // ShadowPlay-like layout (screenshot F1, replay F10); F9 is taken on this PC, so recording is F11
    struct { int id; UINT vk; const wchar_t* key; const char* name; } hotkeys[] = {
        { HOTKEY_SAVE, VK_F10, L"Ctrl+Alt+Shift+F10", "Ctrl+Alt+Shift+F10" },
        { HOTKEY_RECORD, VK_F11, L"Ctrl+Alt+Shift+F11", "Ctrl+Alt+Shift+F11" },
        { HOTKEY_SHOT, VK_F1, L"Ctrl+Alt+Shift+F1", "Ctrl+Alt+Shift+F1" },
    };
    for (auto& hk : hotkeys)
        if (!RegisterHotKey(s_wnd, hk.id, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, hk.vk)) {
            log_line("hotkey %s is taken by another app", hk.name);
            notify(L"VramReplay", std::wstring(hk.key) + L" が他のアプリに使われています。トレイのメニューから操作できます。", NIIF_WARNING);
        }
    overlay_init();

    MSG msg;
    HICON normalIcon = s_nid.hIcon, recIcon = make_icon(sm);
    bool shownRecording = false;
    while (g_running) {
        // While manually recording, the tray icon turns into a red dot and the tooltip shows the time
        bool rec = record_active();
        if (rec != shownRecording || rec) {
            wchar_t t[128];
            int rs = record_seconds();
            if (rec) swprintf_s(t, L"VramReplay — 録画中 %d:%02d\nCtrl+Alt+Shift+F11 で停止", rs / 60, rs % 60);
            else wcscpy_s(t, tip);
            if (wcscmp(t, s_nid.szTip) != 0 || rec != shownRecording) {
                s_nid.hIcon = rec ? recIcon : normalIcon;
                wcsncpy_s(s_nid.szTip, t, _TRUNCATE);
                NOTIFYICONDATAW n = s_nid;
                n.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
                Shell_NotifyIconW(NIM_MODIFY, &n);
            }
            shownRecording = rec;
        }
        if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLINPUT) != WAIT_OBJECT_0) continue;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (fgHook) UnhookWinEvent(fgHook);
    for (auto& hk : hotkeys) UnregisterHotKey(s_wnd, hk.id);
    overlay_shutdown();
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    DestroyIcon(normalIcon);
    DestroyIcon(recIcon);
    DestroyWindow(s_wnd);
}
