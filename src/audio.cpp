// System audio (WASAPI loopback) + microphone, resampled to 48 kHz stereo, mixed and AAC-encoded.
// FFmpeg has no WASAPI input, so capture is written directly against the Windows API.
#include "ring.h"



#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

static std::thread s_thread;
std::atomic<float> g_levels[2][2];             // [desktop, mic][left, right], after gain, before the limiter
std::atomic<float> g_gainLive[2] = { 1.0f, 1.0f };  // [desktop, mic], changed live from the settings page

void audio_set_gain(double desktop, double mic) {
    g_gainLive[0] = (float)desktop;
    g_gainLive[1] = (float)mic;
    g_config.desktopGain = desktop;
    g_config.micGain = mic;
}
static std::mutex s_nameMutex;
static std::string s_micName;

std::string audio_mic_name() {
    std::lock_guard lock(s_nameMutex);
    return s_micName;
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined here to avoid pulling in ksmedia GUID linkage
static const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static constexpr int RATE = 48000;
// Output trails the wall clock so late microphone packets still make it into the mix
static constexpr int64_t MIX_LATENCY_US = 120'000;
static constexpr int MAX_BACKLOG = RATE / 2;

struct Source {
    const char* name;
    bool loopback;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;    // 48 kHz stereo float, interleaved
    int inRate = 0, inChannels = 0;
    double gain = 1.0;
    bool active = false;
    std::wstring deviceId;      // to notice when Windows switches the default device
    // First seconds after opening, for a one-line health report in the log
    int64_t openedUs = 0;
    int packets = 0, silentPackets = 0, converted = 0;
    float rawPeak = 0;
    bool reported = false;
    bool floatIn = false;
};

static void close_source(Source& s) {
    if (s.client) s.client->Stop();
    if (s.capture) s.capture->Release();
    if (s.client) s.client->Release();
    swr_free(&s.swr);
    if (s.fifo) av_audio_fifo_free(s.fifo);
    s.capture = nullptr;
    s.client = nullptr;
    s.fifo = nullptr;
    s.active = false;
}

static std::wstring default_device_id(IMMDeviceEnumerator* enumerator, bool render) {
    IMMDevice* device = nullptr;
    std::wstring id;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(render ? eRender : eCapture, eConsole, &device))) {
        LPWSTR p = nullptr;
        if (SUCCEEDED(device->GetId(&p))) { id = p; CoTaskMemFree(p); }
        device->Release();
    }
    return id;
}

static std::string device_name(IMMDevice* device) {
    // PKEY_Device_FriendlyName, spelled out to avoid the devpkey GUID linkage
    static const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    std::string name = "?";
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = wide_to_utf8(v.pwszVal);
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

// The microphone chosen in the settings while it is plugged in, otherwise the Windows default
static std::wstring wanted_device_id(IMMDeviceEnumerator* enumerator, const Source& s) {
    if (!s.loopback && !g_config.micDevice.empty()) {
        IMMDevice* device = nullptr;
        DWORD state = 0;
        bool present = SUCCEEDED(enumerator->GetDevice(g_config.micDevice.c_str(), &device)) &&
            SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE;
        if (device) device->Release();
        if (present) return g_config.micDevice;
    }
    return default_device_id(enumerator, s.loopback);
}

static bool open_source(Source& s, IMMDeviceEnumerator* enumerator) {
    IMMDevice* device = nullptr;
    std::wstring want = wanted_device_id(enumerator, s);
    HRESULT hr = want.empty() ? E_FAIL : enumerator->GetDevice(want.c_str(), &device);
    if (FAILED(hr)) {
        log_line("audio: no %s device", s.name);
        return false;
    }
    if (!s.loopback && !g_config.micDevice.empty() && want != g_config.micDevice)
        log_line("audio: the chosen microphone is not connected, using the Windows default");
    std::string friendly = device_name(device);
    LPWSTR idp = nullptr;
    s.deviceId.clear();
    if (SUCCEEDED(device->GetId(&idp))) { s.deviceId = idp; CoTaskMemFree(idp); }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client);
    device->Release();
    if (FAILED(hr)) return false;
    log_line("audio: %s device \"%s\"", s.name, friendly.c_str());
    if (!s.loopback) {
        std::lock_guard lock(s_nameMutex);
        s_micName = friendly;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(s.client->GetMixFormat(&mix))) return false;
    s.inRate = (int)mix->nSamplesPerSec;
    s.inChannels = mix->nChannels;
    bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(((WAVEFORMATEXTENSIBLE*)mix)->SubFormat, kSubtypeFloat));
    AVSampleFormat inFmt = isFloat ? AV_SAMPLE_FMT_FLT : mix->wBitsPerSample == 32 ? AV_SAMPLE_FMT_S32 : AV_SAMPLE_FMT_S16;

    hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, s.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 1'000'000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        log_line("audio: %s Initialize failed 0x%08lx", s.name, hr);
        return false;
    }
    if (FAILED(s.client->GetService(__uuidof(IAudioCaptureClient), (void**)&s.capture))) return false;

    AVChannelLayout inLayout, outLayout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_default(&inLayout, s.inChannels);
    if (swr_alloc_set_opts2(&s.swr, &outLayout, AV_SAMPLE_FMT_FLT, RATE, &inLayout, inFmt, s.inRate, 0, nullptr) < 0 || swr_init(s.swr) < 0)
        return false;
    s.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, RATE);
    s.client->Start();
    s.active = true;
    s.openedUs = now_us();
    s.packets = s.silentPackets = s.converted = 0;
    s.rawPeak = 0;
    s.reported = false;
    s.floatIn = inFmt == AV_SAMPLE_FMT_FLT;
    log_line("audio: %s %d Hz, %d ch", s.name, s.inRate, s.inChannels);
    return true;
}

static void pull_source(Source& s) {
    if (!s.active) return;
    UINT32 frames = 0;
    while (SUCCEEDED(s.capture->GetNextPacketSize(&frames)) && frames > 0) {
        BYTE* data = nullptr;
        DWORD flags = 0;
        if (FAILED(s.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        s.packets++;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) s.silentPackets++;
        else if (s.floatIn)
            for (UINT32 i = 0; i < frames * (UINT32)s.inChannels; i++) s.rawPeak = std::max(s.rawPeak, std::fabs(((float*)data)[i]));

        int outMax = swr_get_out_samples(s.swr, frames);
        std::vector<float> out((size_t)outMax * 2);
        uint8_t* outPtr = (uint8_t*)out.data();
        int got;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::vector<uint8_t> silence((size_t)frames * s.inChannels * 4, 0);
            const uint8_t* in = silence.data();
            got = swr_convert(s.swr, &outPtr, outMax, &in, frames);
        } else {
            const uint8_t* in = data;
            got = swr_convert(s.swr, &outPtr, outMax, &in, frames);
        }
        s.capture->ReleaseBuffer(frames);
        if (got > 0) {
            s.converted += got;
            void* planes[1] = { out.data() };
            av_audio_fifo_write(s.fifo, planes, got);
        }
    }
    // Clock drift or a stalled consumer: drop the oldest audio rather than growing without bound
    int excess = av_audio_fifo_size(s.fifo) - MAX_BACKLOG;
    if (excess > 0) av_audio_fifo_drain(s.fifo, excess);
}

struct AacEncoder {
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int64_t nextPts = 0;    // in samples on the now_us() timeline
};

static bool open_aac(AacEncoder& a) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    a.ctx = avcodec_alloc_context3(codec);
    a.ctx->sample_rate = RATE;
    a.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&a.ctx->ch_layout, &stereo);
    a.ctx->bit_rate = 160'000;
    a.ctx->time_base = { 1, RATE };
    a.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(a.ctx, codec, nullptr) < 0) return false;

    a.frame = av_frame_alloc();
    a.frame->nb_samples = a.ctx->frame_size;
    a.frame->format = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_copy(&a.frame->ch_layout, &stereo);
    a.frame->sample_rate = RATE;
    if (av_frame_get_buffer(a.frame, 0) < 0) return false;
    a.pkt = av_packet_alloc();

    if (!g_audioInfo.par) g_audioInfo.par = avcodec_parameters_alloc();
    avcodec_parameters_from_context(g_audioInfo.par, a.ctx);
    g_audioInfo.timeBase = a.ctx->time_base;
    g_audioInfo.ready = true;
    return true;
}

static void encode_frame(AacEncoder& a, const float* interleaved) {
    av_frame_make_writable(a.frame);
    float* l = (float*)a.frame->data[0];
    float* r = (float*)a.frame->data[1];
    for (int i = 0; i < a.frame->nb_samples; i++) {
        l[i] = interleaved[i * 2];
        r[i] = interleaved[i * 2 + 1];
    }
    a.frame->pts = a.nextPts;
    a.nextPts += a.frame->nb_samples;
    if (avcodec_send_frame(a.ctx, a.frame) < 0) return;
    while (avcodec_receive_packet(a.ctx, a.pkt) == 0) {
        int64_t wall = av_rescale(a.pkt->pts, 1'000'000, RATE);
        g_ring.push(a.pkt, STREAM_AUDIO, wall);
        record_feed(a.pkt, STREAM_AUDIO, wall);
        av_packet_unref(a.pkt);
    }
}

static void audio_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    Source sources[2] = { { "desktop", true }, { "mic", false } };
    g_gainLive[0] = (float)g_config.desktopGain;
    g_gainLive[1] = (float)g_config.micGain;
    AacEncoder aac;
    if (!enumerator || !open_aac(aac)) {
        log_line("audio: init failed");
        return;
    }

    const int frameSize = aac.ctx->frame_size;
    // Sample index 0 corresponds to now_us() == 0, so audio and video share one timeline
    int64_t startUs = now_us();
    aac.nextPts = av_rescale(startUs, RATE, 1'000'000) / frameSize * frameSize;
    std::vector<float> mix((size_t)frameSize * 2), tmp((size_t)frameSize * 2);
    int64_t lastRetry = 0;

    while (g_running) {
        int64_t now = now_us();
        // Reopen devices that are missing or were unplugged / changed
        if (now - lastRetry > 3'000'000) {
            lastRetry = now;
            for (int i = 0; i < 2; i++) {
                if (i == 1 && !g_config.captureMic) continue;
                if (sources[i].active && wanted_device_id(enumerator, sources[i]) != sources[i].deviceId) {
                    // e.g. earbuds connected and became the default, or the chosen mic was plugged back in
                    log_line("audio: %s device to use changed", sources[i].name);
                    close_source(sources[i]);
                }
                if (!sources[i].active) {
                    close_source(sources[i]);
                    open_source(sources[i], enumerator);
                }
            }
        }
        for (Source& s : sources) {
            pull_source(s);
            if (s.active && !s.reported && now - s.openedUs > 5'000'000) {
                s.reported = true;
                log_line("audio: %s first 5 s: %d packets (%d silent), %d samples, peak %.3f", s.name, s.packets, s.silentPackets,
                    s.converted, s.rawPeak);
            }
            if (s.active) {
                UINT32 pad;
                if (s.client->GetCurrentPadding(&pad) == AUDCLNT_E_DEVICE_INVALIDATED) {
                    log_line("audio: %s device changed", s.name);
                    close_source(s);
                }
            }
        }

        // Emit every AAC frame whose time has come; sources that are behind (silent loopback) get zeros
        int64_t due = av_rescale(now - MIX_LATENCY_US, RATE, 1'000'000);
        while (aac.nextPts + frameSize <= due) {
            std::fill(mix.begin(), mix.end(), 0.0f);
            float peaks[2][2] = {};
            for (int k = 0; k < 2; k++) {
                const float gain = g_gainLive[k];
                Source& s = sources[k];
                if (!s.active) continue;
                int n = std::min(av_audio_fifo_size(s.fifo), frameSize);
                if (n <= 0) continue;
                void* planes[1] = { tmp.data() };
                av_audio_fifo_read(s.fifo, planes, n);
                for (int i = 0; i < n * 2; i++) {
                    float v = tmp[i] * gain;
                    mix[i] += v;
                    peaks[k][i & 1] = std::max(peaks[k][i & 1], std::fabs(v));
                }
            }
            // Peak hold with a ~0.3 s fall, read by the settings meter
            for (int k = 0; k < 2; k++)
                for (int c = 0; c < 2; c++) g_levels[k][c] = std::max(peaks[k][c], g_levels[k][c] * 0.93f);
            // Soft limiter instead of hard clipping, so a strongly boosted microphone stays clean on loud peaks
            for (float& v : mix) {
                float a = std::fabs(v);
                if (a > 0.85f) v = std::copysign(0.85f + 0.15f * std::tanh((a - 0.85f) / 0.15f), v);
            }
            encode_frame(aac, mix.data());
        }
        Sleep(5);
    }

    for (Source& s : sources) close_source(s);
    avcodec_free_context(&aac.ctx);
    av_frame_free(&aac.frame);
    av_packet_free(&aac.pkt);
    enumerator->Release();
    CoUninitialize();
}

bool audio_start() {
    s_thread = std::thread(audio_loop);
    return true;
}

void audio_stop() {
    if (s_thread.joinable()) s_thread.join();
}
