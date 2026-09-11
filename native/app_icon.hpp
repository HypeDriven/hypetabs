#pragma once
#include <windows.h>
namespace hype {
// Tray/window icon drawn at runtime: a browser tab header over a page. The
// fill color reports state so activity is visible at a glance:
// grey = no connected profile, blue = connected, amber = collection paused,
// green = a tab activation or guidance is in progress.
enum class IconState { Disconnected, Connected, Paused, Active };
inline COLORREF icon_color(IconState state) {
    switch (state) {
    case IconState::Connected: return RGB(66, 140, 255);
    case IconState::Paused: return RGB(240, 170, 40);
    case IconState::Active: return RGB(70, 200, 120);
    default: return RGB(140, 140, 140);
    }
}
inline HICON create_app_icon(IconState state, int size) {
    if (size < 8) size = 16;
    HDC screen = GetDC(nullptr); HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size), mask = CreateBitmap(size, size, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);
    auto draw = [&](HBITMAP target, COLORREF fill, COLORREF background) {
        auto previous = SelectObject(dc, target);
        RECT all{0, 0, size, size}; HBRUSH back = CreateSolidBrush(background); FillRect(dc, &all, back); DeleteObject(back);
        HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, fill);
        auto old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
        int unit = size / 16; if (unit < 1) unit = 1;
        // Tab header (top left) and the page body below it, joined with a small gap.
        RoundRect(dc, unit, unit, size / 2 + unit, unit * 5, unit * 2, unit * 2);
        RoundRect(dc, unit, unit * 4, size - unit, size - unit, unit * 2, unit * 2);
        // A darker cut-out line hints at a second tab so the glyph reads as "tabs".
        HBRUSH gap = CreateSolidBrush(background); RECT line{size / 2 + unit * 2, unit * 2, size - unit * 2, unit * 3}; FillRect(dc, &line, gap); DeleteObject(gap);
        SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(brush); DeleteObject(pen);
        SelectObject(dc, previous);
    };
    draw(color, icon_color(state), RGB(0, 0, 0));
    draw(mask, RGB(0, 0, 0), RGB(255, 255, 255)); // black = opaque, white = transparent
    DeleteDC(dc);
    ICONINFO info{}; info.fIcon = TRUE; info.hbmColor = color; info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color); DeleteObject(mask);
    return icon;
}
}
