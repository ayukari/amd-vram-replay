// Reads the requested window back from VRAM and muxes it into an MP4 (stream copy, no re-encode).
#include "mux.h"
#include "ring.h"

#include <mutex>

static std::mutex s_saveMutex;

std::wstring save_replay(int seconds, std::string* error) {
    std::lock_guard lock(s_saveMutex);
    auto fail = [&](const char* msg, const wchar_t* why) {
        if (error) *error = msg;
        log_line("save: %s", msg);
        toast(TOAST_ERROR, L"リプレイを保存できませんでした", why);
        return std::wstring();
    };

    if (!g_videoInfo.ready) return fail("video encoder is not running yet", L"録画の準備がまだできていません");
    std::vector<RingEntry> entries = g_ring.snapshot(seconds);
    if (entries.empty()) return fail("nothing recorded yet", L"まだ何も録画されていません");

    // Every timestamp is shifted so the first video frame starts at zero
    int64_t originUs = entries.front().wallUs;
    for (const RingEntry& e : entries)
        if (e.stream == STREAM_VIDEO) { originUs = e.wallUs; break; }

    std::wstring path = new_clip_path(L".mp4");
    ClipWriter writer;
    std::string err;
    if (!writer.open(path, originUs, "+faststart", &err)) {
        DeleteFileW(path.c_str());
        return fail(err.c_str(), L"保存先に書き込めません");
    }

    AVPacket* pkt = av_packet_alloc();
    bool ok = true;
    g_ring.read(entries, [&](const RingEntry& e, const uint8_t* data) {
        if (!ok || av_new_packet(pkt, (int)e.size) < 0) return;
        memcpy(pkt->data, data, e.size);
        pkt->pts = e.pts;
        pkt->dts = e.dts;
        pkt->duration = e.duration;
        pkt->flags = e.flags;
        ok = writer.write(pkt, e.stream);
    });
    av_packet_free(&pkt);
    writer.close();

    if (!ok || writer.written() == 0) {
        DeleteFileW(path.c_str());
        return fail(ok ? "no packets could be read back" : "write failed", ok ? L"リプレイを読み出せませんでした" : L"保存先に書き込めません");
    }
    log_line("save: %lld packets, %.1f s -> %s", (long long)writer.written(), writer.duration(), wide_to_utf8(path).c_str());
    if (g_config.saveSound) play_save_sound();
    std::wstring app = clip_app_name(path);
    wchar_t sub[128];
    swprintf_s(sub, L"%s%s%d 秒", app.c_str(), app.empty() ? L"" : L" · ", (int)(writer.duration() + 0.5));
    toast(TOAST_SAVED, L"リプレイを保存しました", sub);
    return path;
}
