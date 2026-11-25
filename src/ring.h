#pragma once
#include "common.h"

#include <deque>
#include <functional>
#include <mutex>
#include <vector>

// Encoded packets live in one big ID3D11Buffer in VRAM. Only the small index stays in system memory.
struct RingEntry {
    uint64_t pos;       // monotonic byte position; offset in the buffer is pos % capacity
    uint32_t size;
    int stream;
    int64_t pts, dts, duration;
    int flags;          // AV_PKT_FLAG_KEY
    int64_t wallUs;     // capture time on the now_us() clock
};

class VramRing {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t capacity);
    void shutdown();
    void push(const AVPacket* pkt, int stream, int64_t wallUs);
    // Forget everything, e.g. when the encoder restarts with a new resolution
    void clear();

    // Entries covering the last `seconds`, starting at a video keyframe
    std::vector<RingEntry> snapshot(int seconds);

    // Reads entries back from VRAM in chunks and hands each packet to `sink`.
    // Packets overwritten while saving are skipped. Returns false if nothing could be read.
    bool read(const std::vector<RingEntry>& entries, const std::function<void(const RingEntry&, const uint8_t*)>& sink);

    uint64_t capacity() const { return capacity_; }
    uint64_t bytesUsed();

private:
    bool overwritten(uint64_t pos);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Buffer* buffer_ = nullptr;
    ID3D11Buffer* staging_ = nullptr;
    uint64_t capacity_ = 0;
    uint64_t writePos_ = 0;
    std::deque<RingEntry> entries_;
    std::mutex mutex_;
};

extern VramRing g_ring;
