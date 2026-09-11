#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
namespace hype {
// Dark presentation by default (AGENTS.md); high-contrast themes keep the
// system palette so accessibility settings are never overridden.
class Theme {
    HBRUSH window_brush{}, control_brush{};
    bool dark = false;
public:
    static constexpr COLORREF window_color = RGB(30, 30, 30), control_color = RGB(45, 45, 45),
        text_color = RGB(236, 236, 236), muted_color = RGB(176, 176, 176);
    Theme() {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
        dark = !high_contrast;
        if (dark) { window_brush = CreateSolidBrush(window_color); control_brush = CreateSolidBrush(control_color); }
    }
    ~Theme() { if (window_brush) DeleteObject(window_brush); if (control_brush) DeleteObject(control_brush); }
    Theme(const Theme&) = delete; Theme& operator=(const Theme&) = delete;
    bool active() const { return dark; }
    HBRUSH background() const { return dark ? window_brush : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)); }
    // Top-level: dark title bar and frame through the documented DWM attribute.
    void apply(HWND window) const {
        if (!dark) return;
        BOOL enabled = TRUE; DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
    }
    // Child controls: Explorer's dark visual style for scrollbars, lists,
    // buttons and check boxes.
    void apply_control(HWND control) const {
        if (dark && control) SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    }
    // WM_CTLCOLOR* handling; returns the brush to use or nullptr for defaults.
    HBRUSH color(UINT message, HDC dc, bool muted = false) const {
        if (!dark) return nullptr;
        SetTextColor(dc, muted ? muted_color : text_color);
        bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        SetBkColor(dc, field ? control_color : window_color);
        return field ? control_brush : window_brush;
    }
};
}
