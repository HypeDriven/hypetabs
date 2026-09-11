#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/options_layout.hpp"
#include <cstdlib>
#include <iostream>
void check(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
RECT bounds(HWND child, HWND parent) {
    RECT rect{}; GetWindowRect(child, &rect); MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2); return rect;
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND window = CreateWindowExW(0, L"STATIC", L"HypeTabs layout test", WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL,
        50, 50, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create test window");
    HWND first = CreateWindowExW(0, L"BUTTON", L"First", WS_CHILD, 0, 0, 1, 1, window, nullptr, nullptr, nullptr);
    HWND last = CreateWindowExW(0, L"BUTTON", L"Last", WS_CHILD, 0, 0, 1, 1, window, nullptr, nullptr, nullptr);
    {
        hype::OptionsLayout layout(window);
        layout.place(first, 20, 20, 200, 30); layout.place(last, 20, 613, 405, 26);
        for (UINT dpi : {96u, 120u, 192u, 144u, 96u}) {
            layout.set_dpi(dpi); layout.scroll(SB_VERT, SB_TOP); layout.scroll(SB_HORZ, SB_TOP);
            RECT top = bounds(first, window);
            check(top.left == MulDiv(20, dpi, 96) && top.top == MulDiv(20, dpi, 96) &&
                top.right - top.left == MulDiv(200, dpi, 96), "DPI geometry or cumulative rounding error");
            LOGFONTW font{};
            check(GetObjectW(reinterpret_cast<HFONT>(SendMessageW(first, WM_GETFONT, 0, 0)), sizeof(font), &font) != 0, "missing DPI font");
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            check(SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) &&
                font.lfHeight == metrics.lfMessageFont.lfHeight, "font does not match requested DPI");
            layout.reveal(last);
            RECT end = bounds(last, window), client{}; GetClientRect(window, &client);
            check(end.bottom <= client.bottom && end.top >= 0, "keyboard target remains below viewport");
            layout.reveal(first); top = bounds(first, window);
            check(top.top >= 0 && top.bottom <= client.bottom, "returning keyboard target remains above viewport");
        }
        layout.set_dpi(GetDpiForWindow(window)); layout.fit();
        RECT rect{}; GetWindowRect(window, &rect);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        check(rect.left >= monitor.rcWork.left && rect.top >= monitor.rcWork.top && rect.right <= monitor.rcWork.right && rect.bottom <= monitor.rcWork.bottom, "Options exceeds monitor work area");
        DestroyWindow(window);
    }
    std::cout << "Options DPI geometry, fonts, scrolling, keyboard visibility, round trips and work-area fit passed\n";
}
