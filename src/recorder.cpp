// Manual recording (start/stop, like ShadowPlay's Alt+F9 or ReLive's Ctrl+Shift+E).
// The encoders already produce every packet for the replay ring; while recording, copies of those packets are
// also queued here and a writer thread muxes them to disk, so capture and audio threads never wait on the disk.
#include "mux.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

enum State : int { IDLE, WAIT_KEY, RECORDING, STOPPING };

struct Item {
    AVPacket* pkt;
    int stream;
};

std::mutex s_ctl;           // serialises start/stop/shutdown
std::mutex s_mu;            // guards the queue and the fields below
std::condition_variable s_cv;
std::deque<Item> s_queue;
size_t s_queuedBytes = 0;
std::atomic<int> s_state{ IDLE };
std::thread s_writer;
ClipWriter s_clip;
std::wstring s_path;
std::wstring s_stopReason;
int64_t s_originUs = 0;
std::atomic<int64_t> s_lastVideoUs{ 0 };
bool s_stopRequested = false;

// A disk that can't keep up for this long (at 25 Mbps ~80 s) ends the recording instead of eating memory
constexpr size_t MAX_QUEUED = 256u << 20;

std::wstring format_duration(double secs) {
    int s = (int)(secs + 0.5);
    wchar_t buf[48];
    if (s >= 3600) swprintf_s(buf, L"%d時間%d分%d秒", s / 3600, s / 60 % 60, s % 60);
    else if (s >= 60) swprintf_s(buf, L"%d分%d秒", s / 60, s % 60);
    else swprintf_s(buf, L"%d秒", s);
    return buf;
}

void writer_loop() {
    bool failed = false;
    for (;;) {
        std::unique_lock lock(s_mu);
        s_cv.wait(lock, [] { return !s_queue.empty() || s_stopRequested; });
        if (s_queue.empty()) break;     // stop requested and everything written
        Item it = s_queue.front();
        s_queue.pop_front();
        s_queuedBytes -= it.pkt->size;
        lock.unlock();

        if (!failed && !s_clip.write(it.pkt, it.stream)) {
            failed = true;
            record_stop(L"保存先に書き込めなくなりました");
        }
        av_packet_free(&it.pkt);
    }

    double secs = s_clip.duration();
    int64_t packets = s_clip.written();
    s_clip.close();
    std::wstring reason;
    {
        std::lock_guard lock(s_mu);
        reason = s_stopReason;
        s_state = IDLE;
    }
    std::string utf8 = wide_to_utf8(s_path);
    if (packets == 0) {
        DeleteFileW(s_path.c_str());
        log_line("record: stopped before the first frame");
        if (!reason.empty()) toast(TOAST_ERROR, L"録画できませんでした", reason);
        return;
    }
    log_line("record: %lld packets, %.1f s -> %s", (long long)packets, secs, utf8.c_str());
    std::wstring app = clip_app_name(s_path);
    std::wstring sub = (app.empty() ? L"" : app + L" · ") + format_duration(secs);
    if (!reason.empty()) sub += L"（" + reason + L"）";
    if (g_config.saveSound) play_save_sound();
    toast(reason.empty() ? TOAST_SAVED : TOAST_ERROR, L"録画を保存しました", sub);
}

}  // namespace

bool record_start(std::string* error) {
    std::lock_guard ctl(s_ctl);
    if (s_state != IDLE) {
        if (error) *error = "already recording";
        return false;
    }
    if (s_writer.joinable()) s_writer.join();   // previous recording has finished writing
    if (!g_videoInfo.ready) {
        if (error) *error = "video encoder is not running yet";
        toast(TOAST_ERROR, L"録画を開始できませんでした", L"録画の準備がまだできていません");
        return false;
    }
    s_path = new_clip_path(L".mp4");
    // Fragmented MP4: a fragment per keyframe (every second), so the file stays playable even if the
    // process dies mid-recording, and stopping never rewrites the whole file the way +faststart would.
    // Timestamps are rebased once the first keyframe arrives; the origin is filled in then.
    std::string err;
    if (!s_clip.open(s_path, 0, "+frag_keyframe+empty_moov+default_base_moof", &err)) {
        DeleteFileW(s_path.c_str());
        if (error) *error = err;
        log_line("record: %s", err.c_str());
        toast(TOAST_ERROR, L"録画を開始できませんでした", L"保存先に書き込めません");
        return false;
    }
    {
        std::lock_guard lock(s_mu);
        s_stopRequested = false;
        s_stopReason.clear();
        s_lastVideoUs = 0;
        s_state = WAIT_KEY;
    }
    g_forceIdr = true;
    s_writer = std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        writer_loop();
    });
    log_line("record: starting -> %s", wide_to_utf8(s_path).c_str());
    return true;
}

void record_stop(const wchar_t* reason) {
    std::lock_guard lock(s_mu);
    if (s_state != WAIT_KEY && s_state != RECORDING) return;
    s_state = STOPPING;
    s_stopRequested = true;
    if (reason) s_stopReason = reason;
    s_cv.notify_one();
}

void record_toggle() {
    if (s_state == IDLE) record_start(nullptr);
    else record_stop();
}

void record_shutdown() {
    std::lock_guard ctl(s_ctl);
    record_stop();
    if (s_writer.joinable()) s_writer.join();
}

bool record_active() {
    int st = s_state;
    return st == WAIT_KEY || st == RECORDING;
}

int record_seconds() {
    if (s_state != RECORDING) return 0;
    return (int)((s_lastVideoUs - s_originUs) / 1'000'000);
}

void record_feed(const AVPacket* pkt, int stream, int64_t wallUs) {
    int st = s_state.load(std::memory_order_relaxed);
    if (st != WAIT_KEY && st != RECORDING) return;
    std::lock_guard lock(s_mu);
    if (s_state == WAIT_KEY) {
        // Start on the keyframe requested by record_start; audio from before it is dropped
        if (stream != STREAM_VIDEO || !(pkt->flags & AV_PKT_FLAG_KEY)) return;
        s_originUs = wallUs;
        s_clip.set_origin(wallUs);
        s_state = RECORDING;
        toast(TOAST_RECORD, L"録画を開始しました", L"Ctrl+Alt+Shift+F11 で停止");
    } else if (s_state != RECORDING) {
        return;
    }
    if (wallUs < s_originUs) return;
    if (s_queuedBytes > MAX_QUEUED) {
        // Can't call record_stop here (same mutex); do the same inline
        s_state = STOPPING;
        s_stopRequested = true;
        s_stopReason = L"保存先への書き込みが追いつきませんでした";
        s_cv.notify_one();
        return;
    }
    AVPacket* copy = av_packet_clone(pkt);
    if (!copy) return;
    if (stream == STREAM_VIDEO) s_lastVideoUs = wallUs;
    s_queue.push_back({ copy, stream });
    s_queuedBytes += copy->size;
    s_cv.notify_one();
}
