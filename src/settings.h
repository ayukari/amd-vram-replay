// Settings shared by VramReplay (recorder) and VramEdit (editor), stored as a UTF-16 INI file at
// %LOCALAPPDATA%\VramReplay\config.ini. Header-only so both executables compile the same rules.
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>

struct Settings {
    int seconds = 30;           // replay length
    int fps = 60;               // 30, 60 or 120
    int bitrateMbps = 25;       // average; peak is 1.6x
    bool mic = true;
    int micGain = 100;          // percent
    int desktopGain = 100;      // percent
    bool saveSound = true;      // short chime when a replay is saved
    bool gameNames = true;      // "Overwatch_2026-..." instead of "Replay_2026-..."
    bool notify = true;         // on-screen notifications for saves, recordings and screenshots
    std::wstring micDevice;     // Windows endpoint ID; empty: the Windows default microphone
    std::wstring outputDir;     // empty: Videos\VramReplay
};

namespace settings {

inline std::wstring dir() {
    wchar_t p[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\VramReplay", p, MAX_PATH);
    return p;
}
inline std::wstring file() { return dir() + L"\\config.ini"; }

inline std::wstring default_output() {
    std::wstring out;
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
        out = std::wstring(videos) + L"\\VramReplay";
        CoTaskMemFree(videos);
    }
    return out;
}

inline int64_t peak_bps(int bitrateMbps) { return (int64_t)bitrateMbps * 1'600'000; }

// The VRAM ring holds (peak + audio) * (seconds + 5) * 2 bytes and must stay one D3D11 buffer (< 2 GB)
inline int max_seconds(int bitrateMbps) {
    const double ringLimit = 1900.0 * 1024 * 1024;
    double perSecond = (peak_bps(bitrateMbps) + 320'000) / 8.0 * 2;
    return std::clamp((int)(ringLimit / perSecond) - 5, 5, 300);
}

inline void clamp(Settings& s) {
    s.fps = s.fps >= 120 ? 120 : s.fps <= 30 ? 30 : 60;
    s.bitrateMbps = std::clamp(s.bitrateMbps, 5, 80);
    s.seconds = std::clamp(s.seconds, 5, max_seconds(s.bitrateMbps));
    s.micGain = std::clamp(s.micGain, 0, 3200);     // up to +30 dB for quiet USB microphones
    s.desktopGain = std::clamp(s.desktopGain, 0, 400);
    if (s.outputDir.empty()) s.outputDir = default_output();
    while (s.outputDir.size() > 3 && (s.outputDir.back() == L'\\' || s.outputDir.back() == L'/')) s.outputDir.pop_back();
}

inline Settings load() {
    Settings s;
    std::wstring f = file();
    auto getInt = [&](const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"VramReplay", key, def, f.c_str()); };
    s.seconds = getInt(L"seconds", s.seconds);
    s.fps = getInt(L"fps", s.fps);
    s.bitrateMbps = getInt(L"bitrate_mbps", s.bitrateMbps);
    s.mic = getInt(L"mic", s.mic) != 0;
    s.micGain = getInt(L"mic_gain", s.micGain);
    s.desktopGain = getInt(L"desktop_gain", s.desktopGain);
    s.saveSound = getInt(L"save_sound", s.saveSound) != 0;
    s.gameNames = getInt(L"game_names", s.gameNames) != 0;
    s.notify = getInt(L"notify", s.notify) != 0;
    wchar_t buf[1024] = L"";
    GetPrivateProfileStringW(L"VramReplay", L"output", L"", buf, 1024, f.c_str());
    s.outputDir = buf;
    GetPrivateProfileStringW(L"VramReplay", L"mic_device", L"", buf, 1024, f.c_str());
    s.micDevice = buf;
    clamp(s);
    return s;
}

inline bool save(Settings s) {
    clamp(s);
    std::wstring d = dir(), f = file();
    CreateDirectoryW(d.c_str(), nullptr);
    // WritePrivateProfileString keeps an existing file's encoding; start it as UTF-16 so any folder name survives
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xFEFF;
            DWORD w;
            WriteFile(h, &bom, sizeof(bom), &w, nullptr);
            CloseHandle(h);
        }
    }
    auto putInt = [&](const wchar_t* key, int v) {
        return WritePrivateProfileStringW(L"VramReplay", key, std::to_wstring(v).c_str(), f.c_str()) != 0;
    };
    bool ok = putInt(L"seconds", s.seconds) && putInt(L"fps", s.fps) && putInt(L"bitrate_mbps", s.bitrateMbps) &&
        putInt(L"mic", s.mic) && putInt(L"mic_gain", s.micGain) && putInt(L"desktop_gain", s.desktopGain) &&
        putInt(L"save_sound", s.saveSound) && putInt(L"game_names", s.gameNames) && putInt(L"notify", s.notify);
    std::wstring out = s.outputDir == default_output() ? L"" : s.outputDir;
    ok = ok && WritePrivateProfileStringW(L"VramReplay", L"output", out.c_str(), f.c_str());
    ok = ok && WritePrivateProfileStringW(L"VramReplay", L"mic_device", s.micDevice.c_str(), f.c_str());
    return ok;
}

}  // namespace settings
