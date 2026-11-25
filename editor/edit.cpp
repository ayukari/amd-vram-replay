// VramEdit - lightweight clip editor for VramReplay recordings.
// A tiny local HTTP server (127.0.0.1 only) serves the UI to an Edge app window; the browser's hardware
// decoder plays the clips, and exports are done here with FFmpeg: stream copy (instant, keyframe aligned)
// or a frame-accurate GPU re-encode.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "../src/settings.h"

static std::wstring g_clipDir, g_exeDir;
static int g_port = 0;
static std::atomic<int64_t> g_lastPing{ 0 };
static std::atomic<int> g_activeRequests{ 0 };

static int64_t now_ms() { return (int64_t)GetTickCount64(); }

static std::string to_utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring from_utf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out;
}

// Minimal extraction from the small JSON bodies the UI sends
static std::string json_string(const std::string& body, const std::string& key) {
    size_t k = body.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t q = body.find('"', body.find(':', k) + 1);
    if (q == std::string::npos) return "";
    std::string out;
    for (size_t i = q + 1; i < body.size() && body[i] != '"'; i++) {
        if (body[i] == '\\' && i + 1 < body.size()) i++;
        out += body[i];
    }
    return out;
}

// A scalar number or true/false after "key":
static double json_number(const std::string& body, const std::string& key, double def) {
    size_t k = body.find("\"" + key + "\"");
    if (k == std::string::npos) return def;
    size_t i = body.find(':', k);
    if (i == std::string::npos) return def;
    while (++i < body.size() && body[i] == ' ') {}
    if (body.compare(i, 4, "true") == 0) return 1;
    if (body.compare(i, 5, "false") == 0) return 0;
    char* e;
    double v = strtod(body.c_str() + i, &e);
    return e == body.c_str() + i ? def : v;
}

static std::vector<double> json_numbers(const std::string& body, const std::string& key) {
    std::vector<double> out;
    size_t k = body.find("\"" + key + "\"");
    if (k == std::string::npos) return out;
    size_t start = body.find('[', k), end = body.find(']', body.find(']', start) + 1);
    // ranges are [[a,b],[c,d]] - read every number up to the closing outer bracket
    int depth = 0;
    for (size_t i = start; i < body.size(); i++) {
        char c = body[i];
        if (c == '[') depth++;
        else if (c == ']') { if (--depth == 0) break; }
        else if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
            char* e;
            out.push_back(strtod(body.c_str() + i, &e));
            i = e - body.c_str() - 1;
        }
    }
    (void)end;
    return out;
}

// Only bare *.mp4 names inside the clip folder are accepted
static bool valid_name(const std::wstring& name) {
    if (name.empty() || name.size() > 200) return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || name.find(L"..") != std::wstring::npos) return false;
    return name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".mp4") == 0;
}

static std::mutex g_dirMutex;
static std::wstring clip_dir() { std::lock_guard lock(g_dirMutex); return g_clipDir; }
// The clip folder comes from config.ini, so a change on the settings page applies without restarting
static void refresh_clip_dir() {
    std::wstring d = settings::load().outputDir;
    CreateDirectoryW(d.c_str(), nullptr);
    std::lock_guard lock(g_dirMutex);
    g_clipDir = d;
}
static std::wstring clip_path(const std::wstring& name) { return clip_dir() + L"\\" + name; }

static std::wstring unique_output(const std::wstring& base, const std::wstring& suffix) {
    std::wstring stem = base.substr(0, base.size() - 4);
    std::wstring name = stem + suffix + L".mp4";
    for (int i = 2; GetFileAttributesW(clip_path(name).c_str()) != INVALID_FILE_ATTRIBUTES; i++)
        name = stem + suffix + L"_" + std::to_wstring(i) + L".mp4";
    return name;
}

// ---------- clip listing ----------

struct ClipMeta { uint64_t mtime; double duration; int width, height; };
static std::map<std::wstring, ClipMeta> g_metaCache;
static std::mutex g_metaMutex;

static ClipMeta probe(const std::wstring& name, uint64_t mtime) {
    {
        std::lock_guard lock(g_metaMutex);
        auto it = g_metaCache.find(name);
        if (it != g_metaCache.end() && it->second.mtime == mtime) return it->second;
    }
    ClipMeta m{ mtime, 0, 0, 0 };
    AVFormatContext* fmt = nullptr;
    // The moov atom is at the front (faststart), so opening is enough - no decoding
    if (avformat_open_input(&fmt, to_utf8(clip_path(name)).c_str(), nullptr, nullptr) == 0) {
        if (fmt->duration > 0) m.duration = fmt->duration / (double)AV_TIME_BASE;
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            AVStream* st = fmt->streams[i];
            // Without find_stream_info the container duration may be unset; the per-stream ones come from moov
            if (st->duration > 0) m.duration = std::max(m.duration, st->duration * av_q2d(st->time_base));
        }
        for (unsigned i = 0; i < fmt->nb_streams; i++)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                m.width = fmt->streams[i]->codecpar->width;
                m.height = fmt->streams[i]->codecpar->height;
            }
        avformat_close_input(&fmt);
    }
    std::lock_guard lock(g_metaMutex);
    g_metaCache[name] = m;
    return m;
}

static std::string list_clips() {
    struct Item { std::wstring name; uint64_t size, mtime; };
    std::vector<Item> items;
    WIN32_FIND_DATAW fd;
    refresh_clip_dir();
    std::wstring dir = clip_dir();
    HANDLE h = FindFirstFileW((dir + L"\\*.mp4").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            uint64_t mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            items.push_back({ fd.cFileName, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, mtime });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.mtime > b.mtime; });

    std::string out = "{\"dir\":\"" + json_escape(to_utf8(dir)) + "\",\"clips\":[";
    for (size_t i = 0; i < items.size(); i++) {
        ClipMeta m = probe(items[i].name, items[i].mtime);
        // FILETIME (100ns since 1601) to Unix ms
        int64_t unixMs = (int64_t)(items[i].mtime / 10000) - 11644473600000LL;
        char buf[256];
        snprintf(buf, sizeof(buf), ",\"size\":%llu,\"mtime\":%lld,\"duration\":%.3f,\"width\":%d,\"height\":%d}",
            (unsigned long long)items[i].size, (long long)unixMs, m.duration, m.width, m.height);
        out += (i ? ",{" : "{") + std::string("\"name\":\"") + json_escape(to_utf8(items[i].name)) + "\"" + buf;
    }
    return out + "]}";
}

// ---------- export ----------

// Stream copy: each range starts at the keyframe at or before its start (VramReplay writes one every second)
static std::string export_fast(const std::wstring& name, const std::vector<std::pair<double, double>>& ranges, std::wstring& outName) {
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, to_utf8(clip_path(name)).c_str(), nullptr, nullptr) < 0) return "could not open clip";
    avformat_find_stream_info(in, nullptr);

    outName = unique_output(name, L"_edit");
    std::string outPath = to_utf8(clip_path(outName));
    AVFormatContext* out = nullptr;
    avformat_alloc_output_context2(&out, nullptr, "mp4", outPath.c_str());
    std::vector<int> map(in->nb_streams, -1);
    int videoIn = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    for (unsigned i = 0; i < in->nb_streams; i++) {
        AVCodecParameters* par = in->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        AVStream* os = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(os->codecpar, par);
        os->codecpar->codec_tag = 0;
        os->time_base = in->streams[i]->time_base;
        map[i] = os->index;
    }
    if (videoIn < 0 || avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_close_input(&in);
        avformat_free_context(out);
        return "could not create output";
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    if (avformat_write_header(out, &opts) < 0) { av_dict_free(&opts); avio_closep(&out->pb); avformat_free_context(out); avformat_close_input(&in); return "could not write header"; }
    av_dict_free(&opts);

    AVPacket* pkt = av_packet_alloc();
    std::vector<int64_t> lastDts(out->nb_streams, INT64_MIN);
    double outOffset = 0;   // seconds already written
    double videoEnd = 0;    // end of the last written video frame, so the next segment never reuses its timestamp
    AVRational fr = in->streams[videoIn]->avg_frame_rate;
    double frameDur = fr.num > 0 ? av_q2d(av_inv_q(fr)) : 1.0 / 60;
    for (auto [a, b] : ranges) {
        if (av_seek_frame(in, -1, (int64_t)(a * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) continue;
        double segStart = -1;   // time of the keyframe this segment really starts at
        double segEnd = b;
        std::vector<AVPacket*> early;   // audio read before the keyframe was found
        std::vector<bool> finished(in->nb_streams, false);

        auto write = [&](AVPacket* p) {
            int si = p->stream_index;
            AVRational tb = in->streams[si]->time_base;
            double t = p->pts * av_q2d(tb);
            if (t < segStart - 0.001) return;
            if (si == videoIn) videoEnd = std::max(videoEnd, t - segStart + outOffset + frameDur);
            int64_t shift = av_rescale_q((int64_t)((outOffset - segStart) * 1'000'000), { 1, 1'000'000 }, tb);
            p->pts += shift;
            p->dts = (p->dts == AV_NOPTS_VALUE ? p->pts : p->dts + shift);
            p->stream_index = map[si];
            av_packet_rescale_ts(p, tb, out->streams[map[si]]->time_base);
            if (p->dts <= lastDts[map[si]]) return;
            lastDts[map[si]] = p->dts;
            av_interleaved_write_frame(out, p);
        };

        while (av_read_frame(in, pkt) >= 0) {
            int si = pkt->stream_index;
            if (map[si] < 0 || pkt->pts == AV_NOPTS_VALUE || finished[si]) { av_packet_unref(pkt); continue; }
            double t = pkt->pts * av_q2d(in->streams[si]->time_base);
            if (t >= segEnd) {
                av_packet_unref(pkt);
                // Streams are interleaved in chunks, so keep reading until every stream has passed the end
                finished[si] = true;
                bool all = true;
                for (unsigned k = 0; k < in->nb_streams; k++) all = all && (map[k] < 0 || finished[k]);
                if (all || t >= segEnd + 3.0) break;
                continue;
            }
            if (segStart < 0) {
                if (si != videoIn) { early.push_back(av_packet_clone(pkt)); av_packet_unref(pkt); continue; }
                if (!(pkt->flags & AV_PKT_FLAG_KEY)) { av_packet_unref(pkt); continue; }
                segStart = t;
                for (AVPacket* e : early) { write(e); av_packet_free(&e); }
                early.clear();
            }
            write(pkt);
            av_packet_unref(pkt);
        }
        for (AVPacket* e : early) av_packet_free(&e);
        if (segStart >= 0) outOffset = std::max(outOffset + segEnd - segStart, videoEnd);
    }
    av_packet_free(&pkt);
    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    return outOffset > 0 ? "" : "nothing to export";
}

static std::wstring find_ffmpeg() {
    wchar_t local[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Packages", local, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(local) + L"\\Gyan.FFmpeg_*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        std::wstring pkg = std::wstring(local) + L"\\" + fd.cFileName;
        FindClose(h);
        h = FindFirstFileW((pkg + L"\\ffmpeg-*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring exe = pkg + L"\\" + fd.cFileName + L"\\bin\\ffmpeg.exe";
            FindClose(h);
            if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return exe;
        }
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return L"";
}

// Frame-accurate: GPU decode, trim/concat, AMF encode
static std::string export_precise(const std::wstring& name, const std::vector<std::pair<double, double>>& ranges, std::wstring& outName) {
    std::wstring ffmpeg = find_ffmpeg();
    if (ffmpeg.empty()) return "ffmpeg.exe not found";
    outName = unique_output(name, L"_edit");

    std::wstring filter;
    for (size_t i = 0; i < ranges.size(); i++) {
        wchar_t seg[256];
        swprintf(seg, 256, L"[0:v]trim=start=%.3f:end=%.3f,setpts=PTS-STARTPTS[v%zu];[0:a]atrim=start=%.3f:end=%.3f,asetpts=PTS-STARTPTS[a%zu];",
            ranges[i].first, ranges[i].second, i, ranges[i].first, ranges[i].second, i);
        filter += seg;
    }
    for (size_t i = 0; i < ranges.size(); i++) filter += L"[v" + std::to_wstring(i) + L"][a" + std::to_wstring(i) + L"]";
    filter += L"concat=n=" + std::to_wstring(ranges.size()) + L":v=1:a=1[v][a]";

    std::wstring cmd = L"\"" + ffmpeg + L"\" -hide_banner -loglevel error -y -hwaccel d3d11va -i \"" + clip_path(name) +
        L"\" -filter_complex \"" + filter + L"\" -map [v] -map [a] -c:v h264_amf -usage transcoding -quality quality -rc cqp -qp_i 18 -qp_p 20" +
        L" -c:a aac -b:a 160k -movflags +faststart \"" + clip_path(outName) + L"\"";

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
        return "could not start ffmpeg";
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        DeleteFileW(clip_path(outName).c_str());
        return "ffmpeg failed (" + std::to_string(code) + ")";
    }
    return "";
}

// ---------- HTTP ----------

struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> headers;
};

static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

static void send_all(SOCKET s, const char* data, size_t len) {
    while (len > 0) {
        int n = send(s, data, (int)std::min<size_t>(len, 1 << 20), 0);
        if (n <= 0) return;
        data += n;
        len -= n;
    }
}

static void respond(SOCKET s, int status, const char* type, const std::string& body, const std::string& extra = "") {
    const char* text = status == 200 ? "OK" : status == 206 ? "Partial Content" : status == 404 ? "Not Found" : status == 403 ? "Forbidden" : "Bad Request";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n";
    send_all(s, head.data(), head.size());
    send_all(s, body.data(), body.size());
}

static void serve_file(SOCKET s, const std::wstring& path, const char* type, const std::string& range) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return respond(s, 404, "text/plain", "not found");
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    int64_t start = 0, end = size.QuadPart - 1;
    bool partial = false;
    // Seeking in the <video> element uses Range requests
    if (range.rfind("bytes=", 0) == 0) {
        partial = true;
        start = _atoi64(range.c_str() + 6);
        size_t dash = range.find('-');
        if (dash != std::string::npos && dash + 1 < range.size()) end = std::min<int64_t>(end, _atoi64(range.c_str() + dash + 1));
        if (start > end) start = end;
    }
    std::string head = std::string("HTTP/1.1 ") + (partial ? "206 Partial Content" : "200 OK") + "\r\nContent-Type: " + type +
        "\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(end - start + 1) + "\r\nConnection: close\r\n";
    if (strncmp(type, "text/html", 9) == 0) head += "Cache-Control: no-store\r\n";   // never keep an old UI after an update
    if (partial) head += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size.QuadPart) + "\r\n";
    head += "\r\n";
    send_all(s, head.data(), head.size());

    LARGE_INTEGER pos;
    pos.QuadPart = start;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::vector<char> buf(256 * 1024);
    int64_t left = end - start + 1;
    while (left > 0) {
        DWORD n = 0;
        if (!ReadFile(f, buf.data(), (DWORD)std::min<int64_t>(left, buf.size()), &n, nullptr) || n == 0) break;
        if (send(s, buf.data(), (int)n, 0) <= 0) break;
        left -= n;
    }
    CloseHandle(f);
}

static bool read_request(SOCKET s, Request& r) {
    std::string data;
    char buf[8192];
    size_t headerEnd;
    while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0 || data.size() > 64 * 1024) return false;
        data.append(buf, n);
    }
    size_t lineEnd = data.find("\r\n");
    std::string line = data.substr(0, lineEnd);
    size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    r.method = line.substr(0, sp1);
    r.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    for (size_t p = lineEnd + 2; p < headerEnd;) {
        size_t e = data.find("\r\n", p);
        std::string h = data.substr(p, e - p);
        size_t c = h.find(':');
        if (c != std::string::npos) {
            std::string key = h.substr(0, c);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            size_t v = h.find_first_not_of(' ', c + 1);
            r.headers[key] = v == std::string::npos ? "" : h.substr(v);
        }
        p = e + 2;
    }
    size_t len = r.headers.count("content-length") ? (size_t)atoll(r.headers["content-length"].c_str()) : 0;
    if (len > 1 << 20) return false;
    r.body = data.substr(headerEnd + 4);
    while (r.body.size() < len) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        r.body.append(buf, n);
    }
    return true;
}

// ---------- microphones for the settings page ----------
// Lists the capture devices with a live level for each, so the user can see which one reacts to their
// voice. While the page polls, every microphone is opened in shared mode (that is what makes Windows
// meter it); the streams are closed again 3 s after the last poll.

struct IAudioMeterInformation_ : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) = 0;
};
static const IID kIID_Meter = { 0xC02216F6, 0x8C67, 0x4B5B, { 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64 } };
static const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

struct MicInfo {
    std::wstring id;
    std::string name;
    bool isDefault = false;
    float level = 0;
};

static std::mutex g_micMutex;
static std::vector<MicInfo> g_mics;
static std::atomic<int64_t> g_micPoll{ 0 };
static std::atomic<bool> g_micThread{ false };

static void mic_meter_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    struct Open { IAudioClient* client = nullptr; IAudioMeterInformation_* meter = nullptr; };
    std::vector<Open> open;
    std::vector<MicInfo> list;
    if (en) {
        std::wstring defId;
        IMMDevice* d = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &d))) {
            LPWSTR p = nullptr;
            if (SUCCEEDED(d->GetId(&p))) { defId = p; CoTaskMemFree(p); }
            d->Release();
        }
        IMMDeviceCollection* col = nullptr;
        UINT n = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            if (FAILED(col->Item(i, &d))) continue;
            MicInfo m;
            LPWSTR p = nullptr;
            if (SUCCEEDED(d->GetId(&p))) { m.id = p; CoTaskMemFree(p); }
            m.isDefault = m.id == defId;
            m.name = "?";
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) m.name = to_utf8(v.pwszVal);
                PropVariantClear(&v);
                ps->Release();
            }
            Open o;
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&o.client)) &&
                SUCCEEDED(o.client->GetMixFormat(&mix)) &&
                SUCCEEDED(o.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, mix, nullptr)))
                o.client->Start();
            if (mix) CoTaskMemFree(mix);
            d->Activate(kIID_Meter, CLSCTX_ALL, nullptr, (void**)&o.meter);
            open.push_back(o);
            list.push_back(m);
            d->Release();
        }
        if (col) col->Release();
    }
    {
        std::lock_guard lock(g_micMutex);
        g_mics = list;
    }
    while (now_ms() - g_micPoll < 3000) {
        {
            std::lock_guard lock(g_micMutex);
            for (size_t i = 0; i < open.size() && i < g_mics.size(); i++) {
                float p = 0;
                if (open[i].meter) open[i].meter->GetPeakValue(&p);
                g_mics[i].level = std::max(p, g_mics[i].level * 0.85f);
            }
        }
        Sleep(40);
    }
    for (Open& o : open) {
        if (o.meter) o.meter->Release();
        if (o.client) { o.client->Stop(); o.client->Release(); }
    }
    if (en) en->Release();
    {
        std::lock_guard lock(g_micMutex);
        g_mics.clear();
    }
    CoUninitialize();
    g_micThread = false;
}

static std::string mics_json() {
    g_micPoll = now_ms();
    if (!g_micThread.exchange(true)) std::thread(mic_meter_loop).detach();
    std::lock_guard lock(g_micMutex);
    std::string out = "{\"mics\":[";
    for (size_t i = 0; i < g_mics.size(); i++) {
        char lvl[32];
        snprintf(lvl, sizeof(lvl), "%.3f", g_mics[i].level);
        out += (i ? ",{" : "{") + std::string("\"id\":\"") + json_escape(to_utf8(g_mics[i].id)) + "\",\"name\":\"" +
            json_escape(g_mics[i].name) + "\",\"default\":" + (g_mics[i].isDefault ? "true" : "false") + ",\"level\":" + lvl + "}";
    }
    return out + "]}";
}

// ---------- recorder control and settings ----------

// One command to VramReplay over its local pipe; empty when the recorder isn't running
static std::string recorder_command(const std::string& cmd) {
    HANDLE p = CreateFileW(L"\\\\.\\pipe\\VramReplay", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (p == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(L"\\\\.\\pipe\\VramReplay", 2000)) return "";
        p = CreateFileW(L"\\\\.\\pipe\\VramReplay", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) return "";
    }
    std::string line = cmd + "\n", reply;
    DWORD n;
    WriteFile(p, line.data(), (DWORD)line.size(), &n, nullptr);
    char buf[1024];
    while (ReadFile(p, buf, sizeof(buf), &n, nullptr) && n > 0) {
        reply.append(buf, n);
        if (reply.find('\n') != std::string::npos) break;
    }
    CloseHandle(p);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    return reply;
}

static void start_recorder() {
    std::wstring exe = g_exeDir + L"\\VramReplay.exe";
    ShellExecuteW(nullptr, L"open", exe.c_str(), L"--background", g_exeDir.c_str(), SW_SHOWNORMAL);
}

static std::string settings_json() {
    Settings s = settings::load();
    std::string status = recorder_command("status");
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"seconds\":%d,\"fps\":%d,\"bitrateMbps\":%d,\"mic\":%s,\"micGain\":%d,\"desktopGain\":%d,\"saveSound\":%s,\"gameNames\":%s,\"notify\":%s,",
        s.seconds, s.fps, s.bitrateMbps, s.mic ? "true" : "false", s.micGain, s.desktopGain,
        s.saveSound ? "true" : "false", s.gameNames ? "true" : "false", s.notify ? "true" : "false");
    std::string out = std::string("{\"settings\":") + buf + "\"micDevice\":\"" + json_escape(to_utf8(s.micDevice)) +
        "\",\"outputDir\":\"" + json_escape(to_utf8(s.outputDir)) + "\"}";
    out += ",\"defaultOutput\":\"" + json_escape(to_utf8(settings::default_output())) + "\"";
    out += ",\"maxSeconds\":{";
    const int rates[] = { 15, 25, 40, 60 };
    for (int i = 0; i < 4; i++) out += (i ? ",\"" : "\"") + std::to_string(rates[i]) + "\":" + std::to_string(settings::max_seconds(rates[i]));
    out += "},\"recorder\":" + (status.empty() ? std::string("null") : status) + "}";
    return out;
}

static std::string save_settings(const std::string& body) {
    Settings s = settings::load();
    const Settings before = s;
    s.seconds = (int)json_number(body, "seconds", s.seconds);
    s.fps = (int)json_number(body, "fps", s.fps);
    s.bitrateMbps = (int)json_number(body, "bitrateMbps", s.bitrateMbps);
    s.mic = json_number(body, "mic", s.mic) != 0;
    s.micGain = (int)json_number(body, "micGain", s.micGain);
    s.desktopGain = (int)json_number(body, "desktopGain", s.desktopGain);
    s.saveSound = json_number(body, "saveSound", s.saveSound) != 0;
    s.gameNames = json_number(body, "gameNames", s.gameNames) != 0;
    s.notify = json_number(body, "notify", s.notify) != 0;
    if (body.find("\"micDevice\"") != std::string::npos) s.micDevice = from_utf8(json_string(body, "micDevice"));
    std::wstring out = from_utf8(json_string(body, "outputDir"));
    if (!out.empty()) {
        // A local absolute folder that exists or can be created
        bool absolute = out.size() >= 3 && iswalpha(out[0]) && out[1] == L':' && (out[2] == L'\\' || out[2] == L'/');
        if (!absolute || out.find(L"..") != std::wstring::npos) return "保存先は C:\\... のような完全なパスで指定してください";
        SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
        DWORD a = GetFileAttributesW(out.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return "保存先のフォルダを作れませんでした";
        s.outputDir = out;
    }
    if (!settings::save(s)) return "設定を保存できませんでした";
    refresh_clip_dir();
    // The recorder reads config.ini at startup; restarting clears its replay buffer. Volumes and the
    // notification/sound switches don't need that: volumes are pushed live, the rest is read when used next.
    Settings a = before, b = s;
    a.micGain = b.micGain = a.desktopGain = b.desktopGain = 0;
    a.saveSound = b.saveSound = a.notify = b.notify = false;
    bool onlyLive = a.seconds == b.seconds && a.fps == b.fps && a.bitrateMbps == b.bitrateMbps && a.mic == b.mic &&
        a.gameNames == b.gameNames && a.outputDir == b.outputDir && a.micDevice == b.micDevice &&
        before.saveSound == s.saveSound && before.notify == s.notify;
    if (!recorder_command("status").empty()) {
        if (onlyLive) recorder_command("gain " + std::to_string(s.desktopGain) + " " + std::to_string(s.micGain));
        else recorder_command("restart");
    }
    return "";
}

static void handle(SOCKET s) {
    g_activeRequests++;
    Request r;
    if (!read_request(s, r)) { g_activeRequests--; closesocket(s); return; }
    g_lastPing = now_ms();

    // Only our own origin: blocks DNS rebinding, and the custom header blocks cross-site POSTs
    std::string host = r.headers["host"];
    std::string expected1 = "127.0.0.1:" + std::to_string(g_port), expected2 = "localhost:" + std::to_string(g_port);
    if (host != expected1 && host != expected2) {
        respond(s, 403, "text/plain", "forbidden");
    } else if (r.method == "POST" && r.headers["x-vramedit"] != "1") {
        respond(s, 403, "text/plain", "forbidden");
    } else if (r.method == "GET" && (r.path == "/" || r.path == "/index.html")) {
        serve_file(s, g_exeDir + L"\\editor.html", "text/html; charset=utf-8", "");
    } else if (r.method == "GET" && r.path == "/api/clips") {
        respond(s, 200, "application/json", list_clips());
    } else if (r.method == "GET" && r.path.rfind("/media/", 0) == 0) {
        std::wstring name = from_utf8(url_decode(r.path.substr(7)));
        if (!valid_name(name)) respond(s, 400, "text/plain", "bad name");
        else serve_file(s, clip_path(name), "video/mp4", r.headers["range"]);
    } else if (r.method == "POST" && r.path == "/api/ping") {
        // "v" changes whenever editor.html is replaced, so open windows reload themselves after an update
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW((g_exeDir + L"\\editor.html").c_str(), GetFileExInfoStandard, &fa);
        unsigned long long v = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
        respond(s, 200, "application/json", "{\"ok\":true,\"v\":\"" + std::to_string(v) + "\"}");
    } else if (r.method == "POST" && r.path == "/api/export") {
        std::wstring name = from_utf8(json_string(r.body, "name"));
        std::string mode = json_string(r.body, "mode");
        std::vector<double> nums = json_numbers(r.body, "ranges");
        std::vector<std::pair<double, double>> ranges;
        for (size_t i = 0; i + 1 < nums.size(); i += 2)
            if (nums[i + 1] - nums[i] > 0.05) ranges.push_back({ std::max(0.0, nums[i]), nums[i + 1] });
        if (!valid_name(name) || ranges.empty() || ranges.size() > 200) {
            respond(s, 400, "application/json", "{\"ok\":false,\"error\":\"bad request\"}");
        } else {
            std::wstring outName;
            std::string err = mode == "precise" ? export_precise(name, ranges, outName) : export_fast(name, ranges, outName);
            if (err.empty()) respond(s, 200, "application/json", "{\"ok\":true,\"name\":\"" + json_escape(to_utf8(outName)) + "\"}");
            else respond(s, 200, "application/json", "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}");
        }
    } else if (r.method == "POST" && r.path == "/api/delete") {
        std::wstring name = from_utf8(json_string(r.body, "name"));
        bool ok = false;
        if (valid_name(name)) {
            // Recycle Bin, not a permanent delete
            std::wstring from = clip_path(name) + L'\0';
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
        }
        respond(s, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (r.method == "POST" && r.path == "/api/rename") {
        std::wstring name = from_utf8(json_string(r.body, "name")), to = from_utf8(json_string(r.body, "to"));
        if (to.size() < 4 || _wcsicmp(to.c_str() + to.size() - 4, L".mp4") != 0) to += L".mp4";
        bool ok = valid_name(name) && valid_name(to) && MoveFileW(clip_path(name).c_str(), clip_path(to).c_str());
        respond(s, 200, "application/json", ok ? "{\"ok\":true,\"name\":\"" + json_escape(to_utf8(to)) + "\"}" : "{\"ok\":false}");
    } else if (r.method == "GET" && r.path == "/api/mics") {
        respond(s, 200, "application/json", mics_json());
    } else if (r.method == "POST" && r.path == "/api/gain") {
        // While a mixer fader is dragged: change the recorder's volume live without saving
        int desktop = (int)json_number(r.body, "desktopGain", 100), mic = (int)json_number(r.body, "micGain", 100);
        std::string rep = recorder_command("gain " + std::to_string(desktop) + " " + std::to_string(mic));
        respond(s, 200, "application/json", rep.empty() ? "{\"ok\":false}" : rep);
    } else if (r.method == "GET" && r.path == "/api/level") {
        std::string lv = recorder_command("level");
        respond(s, 200, "application/json", lv.empty() ? "{\"ok\":false}" : lv);
    } else if (r.method == "GET" && r.path == "/api/settings") {
        respond(s, 200, "application/json", settings_json());
    } else if (r.method == "POST" && r.path == "/api/settings") {
        std::string err = save_settings(r.body);
        respond(s, 200, "application/json", err.empty() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}");
    } else if (r.method == "POST" && r.path == "/api/recorder") {
        std::string action = json_string(r.body, "action");
        std::string reply;
        if (action == "start") { if (recorder_command("status").empty()) start_recorder(); reply = "{\"ok\":true}"; }
        else if (action == "stop") reply = recorder_command("quit");
        else if (action == "testtoast") reply = recorder_command("testtoast");
        else if (action == "autostart_on") reply = recorder_command("autostart 1");
        else if (action == "autostart_off") reply = recorder_command("autostart 0");
        respond(s, 200, "application/json", reply.empty() ? "{\"ok\":false,\"error\":\"録画ツールが動いていません\"}" : reply);
    } else if (r.method == "POST" && r.path == "/api/opendir") {
        std::wstring dir = clip_dir();
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        respond(s, 200, "application/json", "{\"ok\":true}");
    } else if (r.method == "POST" && r.path == "/api/reveal") {
        std::wstring name = from_utf8(json_string(r.body, "name"));
        if (valid_name(name)) {
            std::wstring args = L"/select,\"" + clip_path(name) + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        respond(s, 200, "application/json", "{\"ok\":true}");
    } else {
        respond(s, 404, "text/plain", "not found");
    }
    g_activeRequests--;
    closesocket(s);
}

static void open_window() {
    std::wstring url = L"http://127.0.0.1:" + std::to_wstring(g_port) + L"/";
    std::wstring args = L"--app=" + url + L" --window-size=1280,820";
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"msedge.exe", args.c_str(), nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    // --serve: run the server without opening a window (for testing)
    bool openUi = !(cmdLine && wcsstr(cmdLine, L"--serve"));
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
    refresh_clip_dir();

    wchar_t portFile[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\VramReplay\\editor.port", portFile, MAX_PATH);
    // Already running: just open another window onto it
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\VramEditSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        FILE* f = _wfopen(portFile, L"r");
        if (f) { fscanf(f, "%d", &g_port); fclose(f); }
        if (g_port && openUi) open_window();
        return 0;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (g_port = 47123; g_port < 47150; g_port++) {
        addr.sin_port = htons((u_short)g_port);
        if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == 0) break;
    }
    listen(listener, 16);
    FILE* f = _wfopen(portFile, L"w");
    if (f) { fprintf(f, "%d", g_port); fclose(f); }

    av_log_set_level(AV_LOG_ERROR);
    g_lastPing = now_ms();
    if (openUi) open_window();

    // Quit once the window has been closed for a while. The page pings every 5 s, but a minimised or hidden
    // window gets its timers throttled to once a minute, so allow several missed minutes before giving up.
    std::thread([listener] {
        for (;;) {
            Sleep(5000);
            if (g_activeRequests == 0 && now_ms() - g_lastPing > 300'000) {
                closesocket(listener);
                return;
            }
        }
    }).detach();

    for (;;) {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::thread(handle, c).detach();
    }
    DeleteFileW(portFile);
    ReleaseMutex(single);
    WSACleanup();
    return 0;
}
