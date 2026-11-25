// VramReplay - instant replay recorder that keeps everything on the GPU.
// Capture (DXGI Desktop Duplication) -> AMF hardware encode -> encoded ring buffer in VRAM.
// Raw frames never touch system memory; only small encoded packets pass through it briefly.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

struct Config {
    int seconds = 30;           // length of the replay that is kept
    int fps = 60;
    int monitor = 0;
    std::string codec = "h264_amf";
    std::string capture = "auto";   // "auto": AMD driver capture, falling back to Desktop Duplication; "dda": always DDA
    int64_t bitrate = 25'000'000;
    int64_t maxrate = 40'000'000;
    double micGain = 1.0;
    double desktopGain = 1.0;
    bool captureMic = true;
    bool saveSound = true;      // chime after a save
    bool gameNames = true;      // name clips after the foreground app
    bool notify = true;         // on-screen notifications (also shown over games)
    std::wstring micDevice;     // endpoint ID of the chosen microphone; empty: Windows default
    std::wstring outputDir;     // default: %USERPROFILE%\Videos\VramReplay
};

extern Config g_config;
extern std::atomic<bool> g_running;
extern std::atomic<bool> g_restart;     // relaunch after shutting down (settings changed)

// tray.cpp
void tray_run();
bool autostart_enabled();
void set_autostart(bool on);
std::wstring foreground_app_name();     // last app the user was in, for clip names
void play_save_sound();

// overlay.cpp: small on-screen notification, safe to call from any thread
enum ToastKind : int { TOAST_RECORD, TOAST_SAVED, TOAST_SHOT, TOAST_ERROR };
void toast(int kind, const std::wstring& title, const std::wstring& sub);
void overlay_init();      // tray thread
void overlay_shutdown();

// mux.cpp
std::wstring new_clip_path(const wchar_t* ext);         // "<App>_YYYY-MM-DD_HH-MM-SS<ext>" in the output folder
std::wstring clip_app_name(const std::wstring& path);  // "Overwatch", or empty for "Replay_..."

// video.cpp
extern std::atomic<bool> g_forceIdr;    // next frame becomes a keyframe (manual recording start)
void request_screenshot(int delayMs = 0);

// recorder.cpp: manual recording from start to stop, written to disk as it goes
bool record_start(std::string* error);
void record_stop(const wchar_t* reason = nullptr);
void record_toggle();
void record_shutdown();     // stop and wait for the file to be finalised
bool record_active();
int record_seconds();
void record_feed(const AVPacket* pkt, int stream, int64_t wallUs);

// Microseconds on a monotonic clock shared by audio and video
int64_t now_us();

void log_line(const char* fmt, ...);
std::string wide_to_utf8(const std::wstring& w);

// Shared D3D11 device (created by FFmpeg's hwcontext, multithread protected)
struct Gpu {
    AVBufferRef* deviceRef = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
};
extern Gpu g_gpu;
bool gpu_init();

// Stream ids used inside the ring and the muxer
enum StreamId : int { STREAM_VIDEO = 0, STREAM_AUDIO = 1 };

// Encoder parameters the muxer needs (filled once the encoders are open)
struct StreamInfo {
    AVCodecParameters* par = nullptr;
    AVRational timeBase{ 0, 1 };
    std::atomic<bool> ready{ false };
};
extern StreamInfo g_videoInfo, g_audioInfo;

bool video_start();
void video_stop();
bool audio_start();
// Peak level of the last ~0.3 s after gain (0..1), and the microphone in use; for the settings meter
extern std::atomic<float> g_levels[2][2];     // [desktop, mic][left, right]
void audio_set_gain(double desktop, double mic); // takes effect on the next audio frame
std::string audio_mic_name();
void audio_stop();

// Returns the saved file path, or empty on failure
std::wstring save_replay(int seconds, std::string* error);
