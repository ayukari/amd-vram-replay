#include "ring.h"

VramRing g_ring;

static constexpr uint64_t STAGING_BYTES = 8ull << 20;

bool VramRing::init(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t capacity) {
    device_ = device;
    context_ = context;
    capacity_ = (capacity + 15) & ~15ull;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = (UINT)capacity_;
    desc.Usage = D3D11_USAGE_DEFAULT;
    HRESULT hr = device_->CreateBuffer(&desc, nullptr, &buffer_);
    if (FAILED(hr)) {
        // Some drivers want a bind flag on default-usage buffers
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        hr = device_->CreateBuffer(&desc, nullptr, &buffer_);
    }
    if (FAILED(hr)) {
        log_line("ring: CreateBuffer(%llu bytes) failed 0x%08lx", (unsigned long long)capacity_, hr);
        return false;
    }

    D3D11_BUFFER_DESC sdesc{};
    sdesc.ByteWidth = (UINT)STAGING_BYTES;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateBuffer(&sdesc, nullptr, &staging_);
    if (FAILED(hr)) {
        log_line("ring: staging CreateBuffer failed 0x%08lx", hr);
        return false;
    }
    log_line("ring: %llu MB in VRAM", (unsigned long long)(capacity_ >> 20));
    return true;
}

void VramRing::shutdown() {
    std::lock_guard lock(mutex_);
    if (staging_) staging_->Release();
    if (buffer_) buffer_->Release();
    staging_ = buffer_ = nullptr;
    entries_.clear();
}

void VramRing::push(const AVPacket* pkt, int stream, int64_t wallUs) {
    if (!buffer_ || pkt->size <= 0 || (uint64_t)pkt->size > capacity_ / 4) return;

    std::lock_guard lock(mutex_);
    uint64_t offset = writePos_ % capacity_;
    if (offset + pkt->size > capacity_) {
        // Packets never wrap, so every packet is one contiguous range in the buffer
        writePos_ += capacity_ - offset;
        offset = 0;
    }

    RingEntry e{ writePos_, (uint32_t)pkt->size, stream, pkt->pts, pkt->dts, pkt->duration, pkt->flags, wallUs };
    writePos_ += pkt->size;
    while (!entries_.empty() && entries_.front().pos < writePos_ - std::min(writePos_, capacity_))
        entries_.pop_front();

    D3D11_BOX box{ (UINT)offset, 0, 0, (UINT)(offset + pkt->size), 1, 1 };
    context_->UpdateSubresource(buffer_, 0, &box, pkt->data, 0, 0);
    entries_.push_back(e);
}

void VramRing::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

uint64_t VramRing::bytesUsed() {
    std::lock_guard lock(mutex_);
    if (entries_.empty()) return 0;
    return writePos_ - entries_.front().pos;
}

std::vector<RingEntry> VramRing::snapshot(int seconds) {
    std::lock_guard lock(mutex_);
    std::vector<RingEntry> out;
    if (entries_.empty()) return out;

    int64_t cutoff = entries_.back().wallUs - (int64_t)seconds * 1'000'000;
    // Start at the last video keyframe at or before the cutoff, so the clip is at least `seconds` long when possible
    size_t start = SIZE_MAX;
    for (size_t i = 0; i < entries_.size(); i++) {
        const RingEntry& e = entries_[i];
        if (e.stream != STREAM_VIDEO || !(e.flags & AV_PKT_FLAG_KEY)) continue;
        if (e.wallUs <= cutoff || start == SIZE_MAX) start = i;
        if (e.wallUs > cutoff) break;
    }
    if (start == SIZE_MAX) return out;

    int64_t startWall = entries_[start].wallUs;
    for (size_t i = 0; i < entries_.size(); i++) {
        const RingEntry& e = entries_[i];
        if (e.stream == STREAM_VIDEO ? i >= start : e.wallUs >= startWall) out.push_back(e);
    }
    return out;
}

bool VramRing::overwritten(uint64_t pos) {
    return writePos_ > capacity_ && pos < writePos_ - capacity_;
}

bool VramRing::read(const std::vector<RingEntry>& entries, const std::function<void(const RingEntry&, const uint8_t*)>& sink) {
    size_t i = 0, delivered = 0;
    while (i < entries.size()) {
        // Group neighbouring packets into one contiguous copy that fits the staging buffer
        uint64_t start = entries[i].pos;
        uint64_t startOffset = start % capacity_;
        size_t j = i;
        uint64_t end = start;
        while (j < entries.size()) {
            const RingEntry& e = entries[j];
            uint64_t newEnd = std::max(end, e.pos + e.size);
            if (e.pos < start || newEnd - start > STAGING_BYTES || startOffset + (newEnd - start) > capacity_) break;
            end = newEnd;
            j++;
        }
        if (j == i) { i++; continue; }

        bool valid;
        {
            // Copies are ordered before any later UpdateSubresource, so checking at issue time is enough
            std::lock_guard lock(mutex_);
            valid = !overwritten(start);
            if (valid) {
                D3D11_BOX box{ (UINT)startOffset, 0, 0, (UINT)(startOffset + (end - start)), 1, 1 };
                context_->CopySubresourceRegion(staging_, 0, 0, 0, 0, buffer_, 0, &box);
            }
        }
        if (valid) {
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &map))) {
                const uint8_t* base = (const uint8_t*)map.pData;
                for (size_t k = i; k < j; k++) {
                    sink(entries[k], base + (entries[k].pos - start));
                    delivered++;
                }
                context_->Unmap(staging_, 0);
            }
        } else {
            log_line("ring: %zu packets were overwritten before they could be saved", j - i);
        }
        i = j;
    }
    return delivered > 0;
}
