#include "mux.h"

#include <ctime>

// "<App>_YYYY-MM-DD_HH-MM-SS<ext>" like the vendor recorders name clips after the game; "Replay" when unknown
std::wstring new_clip_path(const wchar_t* ext) {
    CreateDirectoryW(g_config.outputDir.c_str(), nullptr);
    time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    std::wstring app = g_config.gameNames ? foreground_app_name() : L"";
    if (app.empty()) app = L"Replay";
    wchar_t stamp[64];
    wcsftime(stamp, 64, L"_%Y-%m-%d_%H-%M-%S", &local);
    std::wstring stem = g_config.outputDir + L"\\" + app + stamp;
    std::wstring path = stem + ext;
    // Two saves within the same second
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; i++)
        path = stem + L"_" + std::to_wstring(i) + ext;
    return path;
}

// "Overwatch" from "...\Overwatch_2026-11-25_07-46-47.mp4"
std::wstring clip_app_name(const std::wstring& path) {
    std::wstring name = path.substr(path.find_last_of(L'\\') + 1);
    size_t dot = name.find_last_of(L'.');
    size_t cut = dot == std::wstring::npos ? name.size() : dot;
    // The stamp is "_YYYY-MM-DD_HH-MM-SS" (20 chars); search for it from the end, past any "_2" suffix
    for (size_t i = name.size(); i-- > 0;)
        if (name[i] == L'_' && i + 20 <= name.size() && name[i + 5] == L'-' && name[i + 8] == L'-' && name[i + 11] == L'_') { cut = i; break; }
    std::wstring app = name.substr(0, cut);
    return app == L"Replay" ? L"" : app;
}

bool ClipWriter::open(const std::wstring& path, int64_t originUs, const char* movflags, std::string* error) {
    auto fail = [&](const char* msg) { if (error) *error = msg; close(); return false; };
    // The writer is reused by manual recordings; start every file from a clean state
    close();
    originUs_ = originUs;
    lastDts_[0] = lastDts_[1] = INT64_MIN;
    written_ = 0;
    lastVideoUs_ = 0;
    std::string utf8 = wide_to_utf8(path);
    if (avformat_alloc_output_context2(&fmt_, nullptr, "mp4", utf8.c_str()) < 0) return fail("could not create muxer");

    vs_ = avformat_new_stream(fmt_, nullptr);
    avcodec_parameters_copy(vs_->codecpar, g_videoInfo.par);
    videoTb_ = g_videoInfo.timeBase;
    vs_->time_base = videoTb_;
    if (g_audioInfo.ready) {
        as_ = avformat_new_stream(fmt_, nullptr);
        avcodec_parameters_copy(as_->codecpar, g_audioInfo.par);
        audioTb_ = g_audioInfo.timeBase;
        as_->time_base = audioTb_;
    }

    if (avio_open(&fmt_->pb, utf8.c_str(), AVIO_FLAG_WRITE) < 0) return fail("could not open output file");
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", movflags, 0);
    int ret = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (ret < 0) return fail("could not write header");
    header_ = true;
    return true;
}

bool ClipWriter::write(AVPacket* pkt, int stream) {
    AVStream* st = stream == STREAM_VIDEO ? vs_ : as_;
    if (!fmt_ || !st) {
        av_packet_unref(pkt);
        return true;
    }
    AVRational srcTb = stream == STREAM_VIDEO ? videoTb_ : audioTb_;
    int64_t origin = av_rescale_q(originUs_, { 1, 1'000'000 }, srcTb);
    pkt->pts -= origin;
    pkt->dts -= origin;
    pkt->stream_index = st->index;
    if (stream == STREAM_VIDEO) lastVideoUs_ = av_rescale_q(pkt->pts + pkt->duration, srcTb, { 1, 1'000'000 });
    av_packet_rescale_ts(pkt, srcTb, st->time_base);
    if (pkt->dts <= lastDts_[stream]) {
        av_packet_unref(pkt);
        return true;
    }
    lastDts_[stream] = pkt->dts;
    int ret = av_interleaved_write_frame(fmt_, pkt);
    av_packet_unref(pkt);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        log_line("mux: write failed: %s", err);
        return false;
    }
    written_++;
    return true;
}

void ClipWriter::close() {
    if (!fmt_) return;
    if (fmt_->pb) {
        if (header_) av_write_trailer(fmt_);
        avio_closep(&fmt_->pb);
    }
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    vs_ = as_ = nullptr;
    header_ = false;
}
