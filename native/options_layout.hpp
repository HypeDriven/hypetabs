#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>

namespace hype {
// Coordinates are retained in 96-DPI units so repeated monitor changes never
// compound rounding. Scrollbars keep every setting reachable on small screens.
class OptionsLayout {
    struct Item { HWND window; int x, y, width, height; };
    HWND window;
    std::vector<Item> items;
    HFONT font{};
    int dpi = 96, offset_x = 0, offset_y = 0;
    int scale(int value) const { return MulDiv(value, dpi, 96); }
public:
    explicit OptionsLayout(HWND owner) : window(owner) {}
    ~OptionsLayout() { if (font) DeleteObject(font); }
    OptionsLayout(const OptionsLayout&) = delete;
    OptionsLayout& operator=(const OptionsLayout&) = delete;
    void place(HWND child, int x, int y, int width, int height) {
        items.push_back({child, x, y, width, height});
    }
    void arrange() {
        RECT area{}; GetClientRect(window, &area);
        offset_x = std::clamp(offset_x, 0, std::max(0, scale(470) - static_cast<int>(area.right)));
        offset_y = std::clamp(offset_y, 0, std::max(0, scale(659) - static_cast<int>(area.bottom)));
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL,
            0, scale(470) - 1, static_cast<UINT>(area.right), offset_x, 0};
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL,
            0, scale(659) - 1, static_cast<UINT>(area.bottom), offset_y, 0};
        SetScrollInfo(window, SB_HORZ, &horizontal, TRUE);
        SetScrollInfo(window, SB_VERT, &vertical, TRUE);
        for (const auto& item : items)
            MoveWindow(item.window, scale(item.x) - offset_x, scale(item.y) - offset_y,
                scale(item.width), scale(item.height), TRUE);
    }
    void set_dpi(UINT value) {
        int next = static_cast<int>(value ? value : 96);
        offset_x = MulDiv(offset_x, next, dpi); offset_y = MulDiv(offset_y, next, dpi); dpi = next;
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi))) {
            if (auto replacement = CreateFontIndirectW(&metrics.lfMessageFont)) {
                for (const auto& item : items) SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
                if (font) DeleteObject(font);
                font = replacement;
            }
        }
        arrange();
    }
    void fit(const RECT* suggested = nullptr) {
        RECT position{}; GetWindowRect(window, &position);
        if (suggested) position = *suggested;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(&position, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        RECT size{0, 0, scale(470), scale(731)};
        AdjustWindowRectExForDpi(&size, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), static_cast<UINT>(dpi));
        LONG width = std::min(size.right - size.left, monitor.rcWork.right - monitor.rcWork.left);
        LONG height = std::min(size.bottom - size.top, monitor.rcWork.bottom - monitor.rcWork.top);
        LONG x = std::clamp(position.left, monitor.rcWork.left, monitor.rcWork.right - width);
        LONG y = std::clamp(position.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void scroll(int bar, int action) {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(window, bar, &info);
        int& position = bar == SB_VERT ? offset_y : offset_x;
        switch (action) {
        case SB_LINEUP: position -= scale(32); break;
        case SB_LINEDOWN: position += scale(32); break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = info.nMax; break;
        default: return;
        }
        arrange();
    }
    void reveal(HWND child) {
        if (!child || !IsChild(window, child)) return;
        RECT target{}, area{}; GetWindowRect(child, &target);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&target), 2); GetClientRect(window, &area);
        int x = offset_x, y = offset_y;
        if (target.right > area.right) offset_x += target.right - area.right;
        if (target.left < 0) offset_x += target.left;
        if (target.bottom > area.bottom) offset_y += target.bottom - area.bottom;
        if (target.top < 0) offset_y += target.top;
        if (x != offset_x || y != offset_y) arrange();
    }
};
}
