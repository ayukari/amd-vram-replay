// Desktop capture with DXGI Desktop Duplication, modelled on FFmpeg's vsrc_ddagrab.c,
// fed straight into FFmpeg's AMF encoder as D3D11 textures (zero-copy, like ddagrab -> h264_amf).
#include "ring.h"

#include <dxgi1_6.h>
#include <thread>
#include <vector>
#include <wincodec.h>

extern "C" {
#include <libavutil/opt.h>
}

static std::thread s_thread;
// Capture failures repeat every retry while the desktop is unavailable (lock screen, UAC); log the first only
static bool s_failLogged = false;
static bool s_announced = false;

struct Capture {
    IDXGIOutputDuplication* dupl = nullptr;
    ID3D11Texture2D* last = nullptr;    // latest desktop image, kept on the GPU
    int width = 0, height = 0;
    bool hasFrame = false;
};

static void release_capture(Capture& c) {
    if (c.dupl) c.dupl->Release();
    if (c.last) c.last->Release();
    c = Capture{};
}

static bool open_duplication(Capture& c) {
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    HRESULT hr = g_gpu.device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) return false;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) return false;
    hr = adapter->EnumOutputs(g_config.monitor, &output);
    adapter->Release();
    if (FAILED(hr)) {
        if (!s_failLogged) log_line("video: monitor %d not found on this GPU", g_config.monitor);
        s_failLogged = true;
        return false;
    }

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    c.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    c.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    IDXGIOutput5* output5 = nullptr;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), (void**)&output5))) {
        DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
        hr = output5->DuplicateOutput1(g_gpu.device, 0, 1, formats, &c.dupl);
        output5->Release();
    } else {
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (SUCCEEDED(hr)) {
            hr = output1->DuplicateOutput(g_gpu.device, &c.dupl);
            output1->Release();
        }
    }
    output->Release();
    if (FAILED(hr)) {
        if (!s_failLogged) log_line("video: DuplicateOutput failed 0x%08lx (retrying quietly)", hr);
        s_failLogged = true;
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = c.width;
    td.Height = c.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    hr = g_gpu.device->CreateTexture2D(&td, nullptr, &c.last);
    if (FAILED(hr)) return false;

    log_line("video: capturing monitor %d at %dx%d", g_config.monitor, c.width, c.height);
    s_failLogged = false;
    return true;
}

// Pulls whatever the desktop has presented within `timeoutMs` into c.last. Returns false when access was lost.
static bool pump_desktop(Capture& c, UINT timeoutMs) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* resource = nullptr;
    HRESULT hr = c.dupl->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return true;
    if (FAILED(hr)) {
        log_line("video: AcquireNextFrame failed 0x%08lx, reopening", hr);
        return false;
    }
    // Pointer-only updates carry no new image
    if (info.LastPresentTime.QuadPart && info.AccumulatedFrames) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            if ((int)d.Width == c.width && (int)d.Height == c.height && d.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                g_gpu.context->CopyResource(c.last, tex);
                c.hasFrame = true;
            }
            tex->Release();
        }
    }
    resource->Release();
    c.dupl->ReleaseFrame();
    return true;
}

// ---------- screenshots ----------
// The video thread copies the current desktop image into a staging texture; a worker maps it, converts it to
// 24-bit RGB (Desktop Duplication's alpha is unreliable) and writes a PNG with WIC.

std::atomic<bool> g_forceIdr{ false };
static std::atomic<int64_t> s_shotDueUs{ 0 };     // 0: no screenshot requested

void request_screenshot(int delayMs) {
    // A tray menu click asks for a short delay so the closing menu is not in the picture
    s_shotDueUs = now_us() + (int64_t)delayMs * 1000 + 1;
}

static bool write_png(const std::wstring& path, const uint8_t* bgr, int width, int height) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    UINT stride = (UINT)width * 3;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) &&
        SUCCEEDED(frame->Initialize(props)) &&
        SUCCEEDED(frame->SetSize(width, height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat24bppBGR &&
        SUCCEEDED(frame->WritePixels(height, stride, stride * height, (BYTE*)bgr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit());
    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

static void take_screenshot(Capture& c) {
    D3D11_TEXTURE2D_DESC td{};
    c.last->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_gpu.device->CreateTexture2D(&td, nullptr, &staging))) {
        toast(TOAST_ERROR, L"スクリーンショットを保存できませんでした", L"画像を用意できませんでした");
        return;
    }
    g_gpu.context->CopyResource(staging, c.last);
    std::wstring path = new_clip_path(L".png");
    int width = c.width, height = c.height;
    std::thread([staging, path, width, height] {
        // Wait for the copy without holding the device lock (a blocking Map would stall the capture thread)
        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT hr;
        for (int i = 0; (hr = g_gpu.context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map)) ==
            DXGI_ERROR_WAS_STILL_DRAWING && i < 500; i++)
            Sleep(2);
        bool ok = false;
        if (SUCCEEDED(hr)) {
            std::vector<uint8_t> bgr((size_t)width * height * 3);
            for (int y = 0; y < height; y++) {
                const uint8_t* src = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
                uint8_t* dst = bgr.data() + (size_t)y * width * 3;
                for (int x = 0; x < width; x++, src += 4, dst += 3) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
            }
            g_gpu.context->Unmap(staging, 0);
            ok = write_png(path, bgr.data(), width, height);
        }
        staging->Release();
        if (!ok) {
            DeleteFileW(path.c_str());
            log_line("screenshot: failed (0x%08lx)", hr);
            toast(TOAST_ERROR, L"スクリーンショットを保存できませんでした", L"保存先に書き込めません");
            return;
        }
        log_line("screenshot: %dx%d -> %s", width, height, wide_to_utf8(path).c_str());
        if (g_config.saveSound) play_save_sound();
        std::wstring app = clip_app_name(path);
        wchar_t sub[128];
        swprintf_s(sub, L"%s%s%d × %d", app.c_str(), app.empty() ? L"" : L" · ", width, height);
        toast(TOAST_SHOT, L"スクリーンショットを保存しました", sub);
    }).detach();
}

struct Encoder {
    AVBufferRef* framesRef = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
};

static void close_encoder(Encoder& e) {
    avcodec_free_context(&e.ctx);
    av_buffer_unref(&e.framesRef);
    av_packet_free(&e.pkt);
}

static bool open_encoder(Encoder& e, int width, int height) {
    e.framesRef = av_hwframe_ctx_alloc(g_gpu.deviceRef);
    auto* frames = (AVHWFramesContext*)e.framesRef->data;
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_BGRA;   // AMF converts RGB to YUV on the GPU
    frames->width = width;
    frames->height = height;
    // Like vsrc_ddagrab.c, leave initial_pool_size at 0 so every frame is its own texture.
    // AMF on this driver encodes garbage from slices of a texture array.
    frames->initial_pool_size = 0;
    int ret = av_hwframe_ctx_init(e.framesRef);
    if (ret < 0) {
        log_line("video: av_hwframe_ctx_init failed %d", ret);
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(g_config.codec.c_str());
    if (!codec) {
        log_line("video: encoder %s not found", g_config.codec.c_str());
        return false;
    }
    e.ctx = avcodec_alloc_context3(codec);
    e.ctx->width = width;
    e.ctx->height = height;
    e.ctx->pix_fmt = AV_PIX_FMT_D3D11;
    e.ctx->hw_frames_ctx = av_buffer_ref(e.framesRef);
    e.ctx->time_base = { 1, 1'000'000 };
    e.ctx->framerate = { g_config.fps, 1 };
    e.ctx->gop_size = g_config.fps;     // a keyframe every second, so a clip can start close to the requested time
    e.ctx->max_b_frames = 0;
    e.ctx->bit_rate = g_config.bitrate;
    e.ctx->rc_max_rate = g_config.maxrate;
    e.ctx->rc_buffer_size = (int)g_config.maxrate;
    e.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    e.ctx->color_range = AVCOL_RANGE_MPEG;
    e.ctx->colorspace = AVCOL_SPC_BT709;
    e.ctx->color_primaries = AVCOL_PRI_BT709;
    e.ctx->color_trc = AVCOL_TRC_BT709;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "usage", "lowlatency", 0);
    av_dict_set(&opts, "quality", "balanced", 0);
    av_dict_set(&opts, "rc", "vbr_peak", 0);
    // AMF's low-latency usage doesn't honour the IDR period, so IDRs are requested per frame (see video_loop)
    av_dict_set(&opts, "forced_idr", "1", 0);
    av_dict_set(&opts, "async_depth", "4", 0);
    ret = avcodec_open2(e.ctx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        log_line("video: avcodec_open2(%s) failed: %s", g_config.codec.c_str(), err);
        return false;
    }

    e.pkt = av_packet_alloc();
    if (!g_videoInfo.par) g_videoInfo.par = avcodec_parameters_alloc();
    avcodec_parameters_from_context(g_videoInfo.par, e.ctx);
    g_videoInfo.timeBase = e.ctx->time_base;
    g_videoInfo.ready = true;
    log_line("video: %s %dx%d@%d, %lld kbps", g_config.codec.c_str(), width, height, g_config.fps, (long long)(g_config.bitrate / 1000));
    return true;
}

static void drain_packets(Encoder& e) {
    while (avcodec_receive_packet(e.ctx, e.pkt) == 0) {
        g_ring.push(e.pkt, STREAM_VIDEO, e.pkt->pts);
        record_feed(e.pkt, STREAM_VIDEO, e.pkt->pts);
        av_packet_unref(e.pkt);
    }
}

static void video_loop() {
    // High resolution waitable timer for frame pacing without spinning
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    Capture cap;
    Encoder enc;
    const int64_t frameUs = 1'000'000 / g_config.fps;
    int64_t nextTick = 0;
    uint64_t frames = 0;
    int64_t lastFrameUs = 0;

    while (g_running) {
        if (!cap.dupl) {
            if (!open_duplication(cap)) {
                release_capture(cap);
                Sleep(500);
                continue;
            }
            if (enc.ctx && (enc.ctx->width != cap.width || enc.ctx->height != cap.height)) {
                // Resolution changed: older packets can't be muxed with the new stream
                record_stop(L"解像度が変わったため、ここで区切りました");
                close_encoder(enc);
                g_ring.clear();
            }
            if (!enc.ctx && !open_encoder(enc, cap.width, cap.height)) {
                close_encoder(enc);
                release_capture(cap);
                Sleep(2000);
                continue;
            }
            if (lastFrameUs && now_us() - lastFrameUs > 1'000'000) {
                // Capture was gone for a while (lock screen, UAC, display off). The packets from before the gap
                // would make the next save a frozen clip as long as the outage, so start the replay afresh.
                log_line("video: capture resumed after %.1f s, clearing the replay", (now_us() - lastFrameUs) / 1e6);
                g_ring.clear();
                frames = 0;     // next frame is an IDR
            }
            nextTick = now_us();
        }

        // Collect desktop updates until the next frame is due
        bool ok = true;
        for (;;) {
            int64_t remaining = nextTick - now_us();
            if (remaining <= 0) break;
            UINT ms = (UINT)(remaining / 1000);
            if (ms == 0) {
                LARGE_INTEGER due{};
                due.QuadPart = -remaining * 10;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 5);
                break;
            }
            if (!(ok = pump_desktop(cap, ms))) break;
        }
        if (!ok) {
            release_capture(cap);
            continue;
        }
        if (!cap.hasFrame && !(ok = pump_desktop(cap, 0))) {
            release_capture(cap);
            continue;
        }

        int64_t pts = nextTick;
        nextTick += frameUs;
        // If we fell far behind (e.g. a hitch), skip ahead instead of bursting frames
        if (now_us() - nextTick > 4 * frameUs) nextTick = now_us() + frameUs;
        if (!cap.hasFrame) continue;
        if (int64_t due = s_shotDueUs; due && now_us() >= due) {
            s_shotDueUs = 0;
            // A request made while the desktop was unavailable (lock screen) is stale by the time it comes back
            if (now_us() - due < 3'000'000) take_screenshot(cap);
        }

        AVFrame* frame = av_frame_alloc();
        if (av_hwframe_get_buffer(enc.framesRef, frame, 0) == 0) {
            g_gpu.context->CopySubresourceRegion((ID3D11Resource*)frame->data[0], (UINT)(intptr_t)frame->data[1],
                0, 0, 0, cap.last, 0, nullptr);
            frame->pts = pts;
            frame->pict_type = g_forceIdr.exchange(false) || frames % g_config.fps == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            frame->color_range = AVCOL_RANGE_JPEG;
            frame->colorspace = AVCOL_SPC_RGB;
            frame->color_primaries = AVCOL_PRI_BT709;
            frame->color_trc = AVCOL_TRC_IEC61966_2_1;
            int ret = avcodec_send_frame(enc.ctx, frame);
            if (ret < 0 && ret != AVERROR(EAGAIN)) log_line("video: send_frame failed %d", ret);
            drain_packets(enc);
            if (frames++ == 0 && !s_announced) {
                // First frame of this process: the replay buffer is live
                s_announced = true;
                wchar_t sub[96];
                swprintf_s(sub, L"直近 %d 秒 · Ctrl+Alt+Shift+F10 で保存", g_config.seconds);
                toast(TOAST_RECORD, L"リプレイの録画を開始しました", sub);
            }
            lastFrameUs = now_us();
        }
        av_frame_free(&frame);
    }

    if (enc.ctx) {
        avcodec_send_frame(enc.ctx, nullptr);
        drain_packets(enc);
    }
    close_encoder(enc);
    release_capture(cap);
    CloseHandle(timer);
    log_line("video: stopped after %llu frames", (unsigned long long)frames);
}

bool video_start() {
    s_thread = std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        video_loop();
    });
    return true;
}

void video_stop() {
    if (s_thread.joinable()) s_thread.join();
}
