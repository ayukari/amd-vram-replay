// On-screen notifications that also show over borderless games, like the ShadowPlay / Adrenalin overlays.
// Windows' own toasts are held back while a game is fullscreen, so this draws a small panel itself:
// a layered, click-through, never-activated topmost window in the top-right corner that fades out after a
// few seconds. It is excluded from capture, so it never ends up in replays, recordings or screenshots.
#include "common.h"

#include <cmath>
#include <mutex>

namespace {

constexpr UINT WM_TOAST = WM_APP + 20;
constexpr UINT_PTR TIMER_ID = 1;
constexpr int FADE_IN_MS = 220, HOLD_MS = 3200, FADE_OUT_MS = 300;

struct Toast {
    int kind = TOAST_SAVED;
    std::wstring title, sub;
};

std::mutex s_mu;
HWND s_wnd = nullptr;
bool s_hasPending = false;
Toast s_pending;            // newest wins; posted from any thread

HDC s_memDc = nullptr;
HBITMAP s_bitmap = nullptr;
HGDIOBJ s_oldBitmap = nullptr;
SIZE s_size{};
POINT s_pos{};
DWORD s_shownAt = 0;
BYTE s_lastAlpha = 0;
int s_lastDx = 0, s_slide = 0;

void free_surface() {
    if (s_memDc) {
        SelectObject(s_memDc, s_oldBitmap);
        DeleteDC(s_memDc);
    }
    if (s_bitmap) DeleteObject(s_bitmap);
    s_memDc = nullptr;
    s_bitmap = nullptr;
}

HFONT make_font(int px, int weight, const wchar_t* face) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

// Overlay-style panel in the spirit of the Adrenalin / ShadowPlay toasts: a dark, slightly translucent
// card pinned to the monitor's top-right corner with a red accent edge, a small product label, title and
// detail line. It slides in from the right edge and fades out sliding back.
void render(const Toast& t) {
    free_surface();
    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForWindow(s_wnd);
    float s = (dpi ? dpi : 96) / 96.0f;
    auto px = [&](float v) { return (int)lroundf(v * s); };
    s_slide = px(28);

    HDC screen = GetDC(nullptr);
    s_memDc = CreateCompatibleDC(screen);
    HFONT labelFont = make_font(px(10), FW_SEMIBOLD, L"Segoe UI");
    HFONT titleFont = make_font(px(15.5f), FW_BOLD, L"Yu Gothic UI");
    HFONT subFont = make_font(px(12.5f), FW_NORMAL, L"Yu Gothic UI");
    HFONT iconFont = make_font(px(22), FW_NORMAL, L"Segoe Fluent Icons");

    SIZE ts{}, ss{};
    HGDIOBJ old = SelectObject(s_memDc, titleFont);
    GetTextExtentPoint32W(s_memDc, t.title.c_str(), (int)t.title.size(), &ts);
    SelectObject(s_memDc, subFont);
    GetTextExtentPoint32W(s_memDc, t.sub.c_str(), (int)t.sub.size(), &ss);
    SelectObject(s_memDc, old);
    const int textX = px(64), padR = px(24);
    int w = std::clamp(textX + (int)std::max(ts.cx, ss.cx) + padR, px(300), px(470));
    int h = px(t.sub.empty() ? 62 : 80);
    s_size = { w, h };
    // Monitor corner rather than work area: in a game the taskbar is hidden anyway
    s_pos = { mi.rcMonitor.right - w - px(24), mi.rcMonitor.top + px(24) };

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    s_bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    s_oldBitmap = SelectObject(s_memDc, s_bitmap);
    auto* pix = (uint32_t*)bits;

    const COLORREF accent = RGB(237, 28, 36);
    COLORREF icon = t.kind == TOAST_SAVED ? RGB(84, 214, 140) : t.kind == TOAST_SHOT ? RGB(235, 236, 240)
        : t.kind == TOAST_ERROR ? RGB(255, 184, 77) : accent;
    const int bar = px(4);
    const float cx = px(34), cy = h / 2.0f;

    // 1) card, accent edge, and for recordings a red dot with a ring
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            // faint vertical sheen so the card is not flat
            float sheen = 1.0f + 0.10f * (1.0f - (float)y / h);
            float r = 22 * sheen, g = 23 * sheen, b = 27 * sheen;
            if (x < bar) { r = GetRValue(accent); g = GetGValue(accent); b = GetBValue(accent); }
            if (t.kind == TOAST_RECORD) {
                float fx = x + 0.5f, fy = y + 0.5f;
                float d = sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
                float dot = std::clamp(px(6.5f) - d + 0.5f, 0.0f, 1.0f);
                float ring = std::clamp(px(11) - d + 0.5f, 0.0f, 1.0f) - std::clamp(px(9.5f) - d + 0.5f, 0.0f, 1.0f);
                float m = std::max(dot, ring * 0.8f);
                r += (GetRValue(accent) - r) * m; g += (GetGValue(accent) - g) * m; b += (GetBValue(accent) - b) * m;
            }
            pix[y * w + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }

    // 2) text
    SetBkMode(s_memDc, TRANSPARENT);
    if (t.kind != TOAST_RECORD) {
        const wchar_t* glyph = t.kind == TOAST_SAVED ? L"\xE73E" : t.kind == TOAST_SHOT ? L"\xE722" : L"\xE7BA";
        SelectObject(s_memDc, iconFont);
        SetTextColor(s_memDc, icon);
        RECT ir{ (int)(cx - px(16)), (int)(cy - px(16)), (int)(cx + px(16)), (int)(cy + px(16)) };
        DrawTextW(s_memDc, glyph, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    int y0 = t.sub.empty() ? px(11) : px(12);
    SelectObject(s_memDc, labelFont);
    SetTextCharacterExtra(s_memDc, px(1.5f));
    SetTextColor(s_memDc, RGB(130, 132, 140));
    RECT lr{ textX, y0, w - padR, y0 + px(14) };
    DrawTextW(s_memDc, L"VRAMREPLAY", -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SetTextCharacterExtra(s_memDc, 0);
    SelectObject(s_memDc, titleFont);
    SetTextColor(s_memDc, RGB(246, 246, 248));
    RECT tr{ textX, y0 + px(15), w - padR, y0 + px(38) };
    DrawTextW(s_memDc, t.title.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (!t.sub.empty()) {
        SelectObject(s_memDc, subFont);
        SetTextColor(s_memDc, RGB(168, 170, 178));
        RECT sr{ textX, y0 + px(39), w - padR, h - px(4) };
        DrawTextW(s_memDc, t.sub.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    GdiFlush();
    SelectObject(s_memDc, old);
    DeleteObject(labelFont);
    DeleteObject(titleFont);
    DeleteObject(subFont);
    DeleteObject(iconFont);

    // 3) alpha: nearly square corners, translucent card, solid accent edge; premultiplied
    const float radius = px(3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = x + 0.5f, fy = y + 0.5f;
            float qx = std::max(std::max(radius - fx, fx - (w - radius)), 0.0f);
            float qy = std::max(std::max(radius - fy, fy - (h - radius)), 0.0f);
            float a = std::clamp(radius - sqrtf(qx * qx + qy * qy) + 0.5f, 0.0f, 1.0f) * (x < bar ? 1.0f : 0.90f);
            uint32_t c = pix[y * w + x];
            uint32_t r = (uint32_t)(((c >> 16) & 255) * a), g = (uint32_t)(((c >> 8) & 255) * a), b = (uint32_t)((c & 255) * a);
            pix[y * w + x] = ((uint32_t)(a * 255) << 24) | (r << 16) | (g << 8) | b;
        }
}

void present(BYTE alpha, int dx) {
    if (!s_memDc) return;
    POINT src{ 0, 0 }, pos{ s_pos.x + dx, s_pos.y };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(s_wnd, screen, &pos, &s_size, s_memDc, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void show_pending() {
    Toast t;
    {
        std::lock_guard lock(s_mu);
        if (!s_hasPending) return;
        t = s_pending;
        s_hasPending = false;
    }
    render(t);
    bool visible = IsWindowVisible(s_wnd);
    // Already showing: swap the content in place and restart the hold instead of sliding in again
    s_shownAt = GetTickCount() - (visible ? FADE_IN_MS : 0);
    s_lastAlpha = visible ? 255 : 0;
    s_lastDx = visible ? 0 : s_slide;
    present(s_lastAlpha, s_lastDx);
    SetWindowPos(s_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(s_wnd, TIMER_ID, 10, nullptr);
}

// Animates only the slide/fade; during the hold the window is left alone so the compositor has nothing to redo
void tick() {
    DWORD t = GetTickCount() - s_shownAt;
    if (t >= (DWORD)(FADE_IN_MS + HOLD_MS + FADE_OUT_MS)) {
        KillTimer(s_wnd, TIMER_ID);
        ShowWindow(s_wnd, SW_HIDE);
        free_surface();
        return;
    }
    if (t >= (DWORD)FADE_IN_MS && t < (DWORD)(FADE_IN_MS + HOLD_MS)) {
        if (s_lastAlpha != 255 || s_lastDx != 0) present(s_lastAlpha = 255, s_lastDx = 0);
        SetTimer(s_wnd, TIMER_ID, FADE_IN_MS + HOLD_MS - t, nullptr);   // sleep until the fade-out
        return;
    }
    float a, dx;
    if (t < (DWORD)FADE_IN_MS) {
        float p = t / (float)FADE_IN_MS, e = 1 - (1 - p) * (1 - p) * (1 - p);   // ease-out cubic
        a = std::min(1.0f, p * 1.6f);
        dx = s_slide * (1 - e);
    } else {
        float p = (t - FADE_IN_MS - HOLD_MS) / (float)FADE_OUT_MS;
        a = 1 - p;
        dx = s_slide * 0.6f * p * p;
    }
    BYTE alpha = (BYTE)(std::clamp(a, 0.0f, 1.0f) * 255);
    int idx = (int)lroundf(dx);
    if (alpha != s_lastAlpha || idx != s_lastDx) present(s_lastAlpha = alpha, s_lastDx = idx);
    SetTimer(s_wnd, TIMER_ID, 10, nullptr);
}

LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TOAST:
        show_pending();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID) tick();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

void toast(int kind, const std::wstring& title, const std::wstring& sub) {
    log_line("toast: %s / %s", wide_to_utf8(title).c_str(), wide_to_utf8(sub).c_str());
    if (!g_config.notify && kind != TOAST_ERROR) return;
    std::lock_guard lock(s_mu);
    s_pending = { kind, title, sub };
    s_hasPending = true;
    // Before the window exists (very early events) the newest one is shown once it is created
    if (s_wnd) PostMessageW(s_wnd, WM_TOAST, 0, 0);
}

void overlay_init() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VramReplayToast";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"VramReplay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    // Keep it out of Desktop Duplication (Windows 10 2004+), so our own captures never contain it
    if (!SetWindowDisplayAffinity(w, 0x11 /* WDA_EXCLUDEFROMCAPTURE */)) log_line("overlay: capture exclusion failed (%lu)", GetLastError());
    std::lock_guard lock(s_mu);
    s_wnd = w;
    if (s_hasPending) PostMessageW(s_wnd, WM_TOAST, 0, 0);
}

void overlay_shutdown() {
    HWND w;
    {
        std::lock_guard lock(s_mu);
        w = s_wnd;
        s_wnd = nullptr;
    }
    if (w) DestroyWindow(w);
    free_surface();
}
