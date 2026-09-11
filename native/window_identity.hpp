#pragma once
#include <windows.h>
#include <shellscalingapi.h>
#include "tab_locator.hpp"
#include <cmath>
#include <optional>
#include <vector>
namespace hype {
struct WindowBounds { LONG left{}, top{}, width{}, height{}; };
// A display as Chrome lays it out: DIP bounds plus the DPI it renders at.
struct ChromeDisplay { LONG left{}, top{}, width{}, height{}; int dpi = 96; bool primary = false; };
// A Windows monitor in physical pixels with its effective DPI.
struct PhysicalMonitor { RECT bounds{}; int dpi = 96; bool primary = false; };
inline std::vector<PhysicalMonitor> physical_monitors() {
    std::vector<PhysicalMonitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL {
        MONITORINFO info{sizeof(info)}; UINT x{}, y{};
        if (GetMonitorInfoW(monitor, &info) && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x == y)
            reinterpret_cast<std::vector<PhysicalMonitor>*>(value)->push_back({info.rcMonitor, static_cast<int>(x), (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}
// Pair each Chrome display with exactly one monitor: same DPI and primary flag,
// physical size equal to the DIP size scaled by that DPI (within rounding), and
// the nearest scaled center when several monitors qualify. Any ambiguity or an
// unmatched display makes the whole layout unusable.
inline std::vector<std::pair<ChromeDisplay, PhysicalMonitor>> match_displays(const std::vector<ChromeDisplay>& displays, const std::vector<PhysicalMonitor>& monitors) {
    std::vector<std::pair<ChromeDisplay, PhysicalMonitor>> result;
    if (displays.empty() || displays.size() != monitors.size() || displays.size() > 16) return {};
    std::vector<bool> used(monitors.size());
    for (const auto& display : displays) {
        double scale = display.dpi / 96.0; size_t best = monitors.size(); double best_distance = 0; bool ambiguous = false;
        for (size_t i = 0; i < monitors.size(); ++i) {
            const auto& monitor = monitors[i];
            if (used[i] || monitor.dpi != display.dpi || monitor.primary != display.primary) continue;
            double width = static_cast<double>(monitor.bounds.right) - monitor.bounds.left, height = static_cast<double>(monitor.bounds.bottom) - monitor.bounds.top;
            if (std::fabs(width - display.width * scale) > 2 || std::fabs(height - display.height * scale) > 2) continue;
            double dx = (monitor.bounds.left + width / 2) / scale - (display.left + display.width / 2.0);
            double dy = (monitor.bounds.top + height / 2) / scale - (display.top + display.height / 2.0);
            double distance = dx * dx + dy * dy;
            if (best == monitors.size() || distance < best_distance) { ambiguous = best != monitors.size() && best_distance - distance < 1; best = i; best_distance = distance; }
            else if (distance - best_distance < 1) ambiguous = true;
        }
        if (best == monitors.size() || ambiguous) return {};
        used[best] = true; result.emplace_back(display, monitors[best]);
    }
    return result;
}
// Physical bounds predicted for a Chrome DIP window rectangle, from the display
// containing its center. Empty when the layout cannot be matched.
inline std::optional<WindowBounds> physical_from_dip(WindowBounds dip, const std::vector<ChromeDisplay>& displays, const std::vector<PhysicalMonitor>& monitors) {
    auto pairs = match_displays(displays, monitors);
    if (pairs.empty() || dip.width <= 0 || dip.height <= 0) return std::nullopt;
    double cx = dip.left + dip.width / 2.0, cy = dip.top + dip.height / 2.0;
    for (const auto& [display, monitor] : pairs) {
        if (cx < display.left || cx >= display.left + display.width || cy < display.top || cy >= display.top + display.height) continue;
        double scale = display.dpi / 96.0;
        return WindowBounds{monitor.bounds.left + static_cast<LONG>(std::lround((dip.left - display.left) * scale)),
            monitor.bounds.top + static_cast<LONG>(std::lround((dip.top - display.top) * scale)),
            static_cast<LONG>(std::lround(dip.width * scale)), static_cast<LONG>(std::lround(dip.height * scale))};
    }
    return std::nullopt;
}
inline bool unscaled_desktop() {
    bool supported = true;
    // GetScaleFactorForMonitor reported 100% for a 125% monitor on a mixed
    // desktop; the effective per-monitor DPI is the reliable signal.
    auto inspect = [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL {
        UINT x{}, y{};
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) || x != 96 || y != 96) {
            *reinterpret_cast<bool*>(value) = false; return FALSE;
        }
        return TRUE;
    };
    return EnumDisplayMonitors(nullptr, nullptr, inspect, reinterpret_cast<LPARAM>(&supported)) && supported;
}
inline bool monitor_unscaled(HMONITOR monitor) {
    UINT x{}, y{};
    return monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x == 96 && y == 96;
}
// Chrome's DIP coordinates equal physical pixels on every monitor of a fully
// unscaled desktop and on the primary monitor at 96 DPI; a 96-DPI secondary
// monitor beside scaled ones may be laid out differently in DIP space.
inline bool guidance_geometry_available() {
    return unscaled_desktop() || monitor_unscaled(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
}
inline bool window_geometry_verified(HWND window) {
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (!monitor_unscaled(monitor)) return false;
    if (unscaled_desktop()) return true;
    MONITORINFO info{sizeof(info)};
    return GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY);
}
using WindowCandidate = bool (*)(HWND);
inline bool matching_window_rectangle(HWND window, RECT& rectangle) {
    if (!IsIconic(window)) return GetWindowRect(window, &rectangle) != FALSE;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement) || (placement.flags & WPF_RESTORETOMAXIMIZED)) return false;
    rectangle = placement.rcNormalPosition;
    if (!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &monitor)) return false;
        // Normal placement uses workspace coordinates for ordinary top-level
        // windows. Compare its screen equivalent; never reposition the window.
        OffsetRect(&rectangle, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
    }
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}
inline bool chromium_window_class(HWND window) {
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    return std::wstring_view(name) == L"Chrome_WidgetWin_1";
}
inline HWND unique_window_at(WindowBounds bounds, WindowCandidate candidate = chromium_window_class, LONG tolerance = 0) {
    if (bounds.width <= 0 || bounds.height <= 0 || bounds.width > 32768 || bounds.height > 32768 || tolerance < 0 || tolerance > 4) return nullptr;
    struct Search { WindowBounds bounds; WindowCandidate candidate; LONG tolerance; HWND found{}; bool ambiguous = false; } search{bounds, candidate, tolerance};
    auto inspect = [](HWND window, LPARAM value) -> BOOL {
        auto& state = *reinterpret_cast<Search*>(value);
        RECT rectangle{};
        if (!IsWindowVisible(window) || !state.candidate(window) || !matching_window_rectangle(window, rectangle)) return TRUE;
        const auto& b = state.bounds; auto within = [&](int64_t actual, int64_t expected) { return std::llabs(actual - expected) <= state.tolerance; };
        if (!within(rectangle.left, b.left) || !within(rectangle.top, b.top) ||
            !within(static_cast<int64_t>(rectangle.right) - rectangle.left, b.width) ||
            !within(static_cast<int64_t>(rectangle.bottom) - rectangle.top, b.height)) return TRUE;
        if (state.found) { state.ambiguous = true; return FALSE; }
        state.found = window; return TRUE;
    };
    bool complete = EnumWindows(inspect, reinterpret_cast<LPARAM>(&search)) != FALSE;
    return !complete || search.ambiguous ? nullptr : search.found;
}
// With Chrome's display layout, DIP bounds convert to physical pixels on any
// monitor; without it, only monitors where DIPs equal pixels are usable.
inline bool guidance_geometry_available(const std::vector<ChromeDisplay>& displays) {
    return (!displays.empty() && !match_displays(displays, physical_monitors()).empty()) || guidance_geometry_available();
}
inline HWND verified_chrome_window(WindowBounds bounds, const std::vector<ChromeDisplay>& displays = {}) {
    // Minimized windows match through saved normal placement; the cue accepts
    // the single restore move that follows Explorer's foreground change.
    if (!displays.empty()) {
        auto monitors = physical_monitors();
        if (auto physical = physical_from_dip(bounds, displays, monitors)) {
            // Scaled conversions round; allow that but require the window's monitor DPI to agree.
            auto window = unique_window_at(*physical, chromium_window_class, 2);
            if (!window || !chrome_window(window)) return nullptr;
            UINT x{}, y{}; HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL); MONITORINFO info{sizeof(info)};
            if (!monitor || !GetMonitorInfoW(monitor, &info) || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) return nullptr;
            for (const auto& [display, matched] : match_displays(displays, monitors))
                if (EqualRect(&matched.bounds, &info.rcMonitor)) return static_cast<int>(x) == display.dpi ? window : nullptr;
            return nullptr;
        }
        // Fall through: a layout that does not cover the window uses the unscaled rules.
    }
    // Chrome DIP/physical-coordinate equivalence is verified only where the
    // window's monitor guarantees it; other windows use direct activation.
    if (!guidance_geometry_available()) return nullptr;
    auto window = unique_window_at(bounds);
    return window && chrome_window(window) && window_geometry_verified(window) ? window : nullptr;
}
}
