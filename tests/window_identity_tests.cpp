#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../native/window_identity.hpp"
#include <iostream>
bool test_window(HWND window) {
    DWORD process{}; GetWindowThreadProcessId(window, &process);
    wchar_t cls[64]{}; GetClassNameW(window, cls, 64);
    return process == GetCurrentProcessId() && std::wstring_view(cls) == L"HypeTabsIdentityTest";
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto previous = GetForegroundWindow(); bool ok = true;
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"HypeTabsIdentityTest";
    RegisterClassW(&cls);
    auto make = [&] { auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"HypeTabs identity test", WS_POPUP, 120, 140, 600, 400, nullptr, nullptr, cls.hInstance, nullptr); ShowWindow(window, SW_SHOWNOACTIVATE); return window; };
    ok = ok && hype::tab_name_matches(L"Title", L"Title") && hype::tab_name_matches(L"Title - Pinned", L"Title") &&
        hype::tab_name_matches(L"Title - Part of group Work", L"Title") && !hype::tab_name_matches(L"Title2", L"Title") &&
        !hype::tab_name_matches(L"Title -Pinned", L"Title") && !hype::tab_name_matches(L"Other - Title", L"Title") &&
        !hype::tab_name_matches(L"Title", L"") && !hype::tab_name_matches(L"Titl", L"Title");
    if (!ok) { std::cerr << "accessible tab name matching failed\n"; return 1; }
    // Chrome display layout mapping, using the mixed-scaling development desktop as recorded on 2026-09-11.
    std::vector<hype::ChromeDisplay> displays{{0, 0, 3840, 1600, 96, true}, {3840, -1481, 1728, 3073, 120, false}, {-1728, -1488, 1728, 3073, 120, false}};
    std::vector<hype::PhysicalMonitor> monitors{{{0, 0, 3840, 1600}, 96, true}, {{3840, -1851, 6000, 1989}, 120, false}, {{-2160, -1859, 0, 1981}, 120, false}};
    auto pairs = hype::match_displays(displays, monitors);
    ok = ok && pairs.size() == 3 && pairs[1].second.bounds.left == 3840 && pairs[2].second.bounds.left == -2160;
    auto physical = hype::physical_from_dip({3960, -1711, 1125, 752}, displays, monitors);
    ok = ok && physical && physical->left == 3990 && physical->top == -2139 && physical->width == 1406 && physical->height == 940;
    auto on_primary = hype::physical_from_dip({120, 140, 900, 600}, displays, monitors);
    ok = ok && on_primary && on_primary->left == 120 && on_primary->top == 140 && on_primary->width == 900 && on_primary->height == 600;
    ok = ok && !hype::physical_from_dip({9000, 9000, 100, 100}, displays, monitors);
    auto wrong_dpi = displays; wrong_dpi[1].dpi = 96;
    ok = ok && hype::match_displays(wrong_dpi, monitors).empty();
    auto twin_monitors = monitors; twin_monitors[2].bounds = twin_monitors[1].bounds;
    ok = ok && hype::match_displays(displays, twin_monitors).empty();
    ok = ok && hype::match_displays({}, monitors).empty() && hype::match_displays(displays, {}).empty();
    if (!ok) { std::cerr << "display layout mapping failed\n"; return 1; }
    // Cross-check the scaling gate against per-window DPI on every monitor.
    struct Scaling { WNDCLASSW* cls; bool all96 = true; unsigned monitors = 0; } scaling{&cls};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL {
        auto& state = *reinterpret_cast<Scaling*>(value); MONITORINFO info{sizeof(info)}; GetMonitorInfoW(monitor, &info);
        auto probe = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, state.cls->lpszClassName, L"", WS_POPUP,
            info.rcMonitor.left + 10, info.rcMonitor.top + 10, 50, 50, nullptr, nullptr, state.cls->hInstance, nullptr);
        if (probe) { if (GetDpiForWindow(probe) != 96) state.all96 = false; DestroyWindow(probe); ++state.monitors; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scaling));
    ok = ok && scaling.monitors > 0 && hype::unscaled_desktop() == scaling.all96;
    // The primary monitor's own DPI decides availability when the desktop is mixed.
    auto primary = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"", WS_POPUP, 10, 10, 50, 50, nullptr, nullptr, cls.hInstance, nullptr);
    bool primary96 = primary && GetDpiForWindow(primary) == 96;
    ok = ok && primary && hype::guidance_geometry_available() == (scaling.all96 || primary96) && hype::window_geometry_verified(primary) == primary96;
    if (primary) DestroyWindow(primary);
    if (!ok) { std::cerr << "scaling gate disagrees with per-window DPI\n"; return 1; }
    std::cout << "Desktop monitors: " << scaling.monitors << ", all at 96 DPI: " << (scaling.all96 ? "yes" : "no") << '\n';
    auto first = make();
    ok = ok && first && hype::unique_window_at({120,140,600,400}, test_window) == first;
    auto second = make();
    ok = ok && second && !hype::unique_window_at({120,140,600,400}, test_window);
    SetWindowPos(second, nullptr, 121, 140, 600, 400, SWP_NOZORDER | SWP_NOACTIVATE);
    ok = ok && hype::unique_window_at({120,140,600,400}, test_window) == first;
    ShowWindow(first, SW_HIDE);
    ok = ok && !hype::unique_window_at({120,140,600,400}, test_window) && !hype::unique_window_at({120,140,0,400}, test_window);
    DestroyWindow(first); DestroyWindow(second);
    ok = ok && GetForegroundWindow() == previous;
    auto minimized = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, L"HypeTabs minimized identity test", WS_OVERLAPPEDWINDOW,
        120, 140, 600, 400, nullptr, nullptr, cls.hInstance, nullptr);
    ShowWindow(minimized, SW_SHOWNOACTIVATE); ShowWindow(minimized, SW_SHOWMINNOACTIVE);
    ok = ok && IsIconic(minimized) && hype::unique_window_at({120,140,600,400}, test_window) == minimized;
    auto overlapping = make();
    ok = ok && !hype::unique_window_at({120,140,600,400}, test_window);
    DestroyWindow(overlapping);
    WINDOWPLACEMENT placement{sizeof(placement)}; GetWindowPlacement(minimized, &placement);
    placement.flags |= WPF_RESTORETOMAXIMIZED; SetWindowPlacement(minimized, &placement);
    ok = ok && !hype::unique_window_at({120,140,600,400}, test_window);
    DestroyWindow(minimized);
    if (!ok) { std::cerr << "Window identity checks failed\n"; return 1; }
    std::cout << "Display layout mapping, accessible tab names, unique bounds, ambiguity, minimized placement, maximized-restore refusal, hidden windows and no-focus checks passed\n";
}
