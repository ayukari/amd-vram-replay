// MP4 writer shared by replay saves (from the VRAM ring) and manual recordings (live packets).
// Shifts every timestamp so the first video frame starts at zero and drops packets that would go backwards.
#pragma once
#include "common.h"

class ClipWriter {
public:
    // movflags: "+faststart" for short replays; fragmented for long recordings so a crash leaves a playable file
    bool open(const std::wstring& path, int64_t originUs, const char* movflags, std::string* error);
    // Takes the packet's timestamps in the encoder time base; the packet is unreferenced afterwards.
    // Returns false only on a write error (disk full, file gone).
    bool write(AVPacket* pkt, int stream);
    void close();
    void set_origin(int64_t us) { originUs_ = us; }

    int64_t written() const { return written_; }
    // Seconds of video written so far
    double duration() const { return lastVideoUs_ / 1e6; }

private:
    AVFormatContext* fmt_ = nullptr;
    AVStream* vs_ = nullptr;
    AVStream* as_ = nullptr;
    AVRational videoTb_{ 0, 1 }, audioTb_{ 0, 1 };
    int64_t originUs_ = 0;
    int64_t lastDts_[2] = { INT64_MIN, INT64_MIN };
    int64_t written_ = 0;
    int64_t lastVideoUs_ = 0;
    bool header_ = false;
};
