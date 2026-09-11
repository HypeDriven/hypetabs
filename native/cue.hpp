#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
namespace hype {
class CueOverlay {
    inline static CueOverlay* current = nullptr;
    HWND window{}, owner{};
    HHOOK mouse{}, keyboard{};
    HWINEVENTHOOK changes{}, foreground{};
    bool outline = false, points_up = false;
    // CLR_INVALID selects the system highlight color; anything else is the user's choice.
    COLORREF color_override = CLR_INVALID;
    HWND taskbar{}, continuation_window{};
    UINT continuation_message{};
    RECT taskbar_button{};
    RECT continuation_bounds{};
    bool taskbar_stage = false, pressed_target = false, released_target = false, ready = false, restoring = false;
    void advance() {
        if (!taskbar_stage || !released_target || GetForegroundWindow() != owner) return;
        if (!GetWindowRect(owner, &continuation_bounds)) { hide(); return; }
        // A minimized target becomes foreground before Windows restores its
        // placement; accept that one restore move instead of treating it as
        // the target leaving its continuation bounds.
        restoring = IsIconic(owner) != FALSE;
        taskbar_stage = false; ready = true;
        // Retain cancellation hooks until the host consumes this notification.
        // A later click invalidates the generation before an async result can show.
        if (!PostMessageW(continuation_window, continuation_message, static_cast<WPARAM>(version), 0)) hide();
    }
    void pointer(WPARAM message, POINT point) {
        bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
        if (down) {
            // A repeated press on the same button (Explorer sometimes ignores the
            // first click) stays in the taskbar stage instead of cancelling.
            if (taskbar_stage && message == WM_LBUTTONDOWN && PtInRect(&taskbar_button, point) &&
                GetAncestor(WindowFromPoint(point), GA_ROOT) == taskbar) {
                ShowWindow(window, SW_HIDE); pressed_target = true; released_target = false;
            } else hide();
        } else if (taskbar_stage && pressed_target && message == WM_LBUTTONUP) {
            if (!PtInRect(&taskbar_button, point) || GetAncestor(WindowFromPoint(point), GA_ROOT) != taskbar) { hide(); return; }
            released_target = true; advance();
        }
    }
    uint64_t version = 0;
    static constexpr COLORREF transparent = RGB(1, 2, 3);
    static LRESULT CALLBACK mouse_proc(int code, WPARAM message, LPARAM data) {
        if (code >= 0 && current) current->pointer(message, reinterpret_cast<MSLLHOOKSTRUCT*>(data)->pt);
        return CallNextHookEx(nullptr, code, message, data);
    }
    static LRESULT CALLBACK key_proc(int code, WPARAM message, LPARAM data) {
        if (code >= 0 && current && (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && reinterpret_cast<KBDLLHOOKSTRUCT*>(data)->vkCode == VK_ESCAPE) current->hide();
        return CallNextHookEx(nullptr, code, message, data);
    }
    static void CALLBACK event_proc(HWINEVENTHOOK, DWORD event, HWND target, LONG object, LONG, DWORD, DWORD) {
        if (!current || !current->owner) return;
        if (current->ready && target == current->owner && event == EVENT_OBJECT_LOCATIONCHANGE && object == OBJID_WINDOW) {
            RECT bounds{};
            if (GetWindowRect(target, &bounds)) {
                if (EqualRect(&bounds, &current->continuation_bounds)) return;
                if (current->restoring) {
                    if (!IsIconic(target)) { current->continuation_bounds = bounds; current->restoring = false; }
                    return;
                }
            }
        }
        if (current->taskbar_stage) {
            if ((!current->pressed_target && event == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(target, GA_ROOT) == current->taskbar) ||
                (event == EVENT_OBJECT_SHOW && object == OBJID_WINDOW && GetAncestor(target, GA_ROOT) == target && target != current->window &&
                    !(target == current->owner && current->pressed_target))) { current->hide(); return; }
            if (event == EVENT_SYSTEM_FOREGROUND) {
                if (target == current->owner) {
                    if (current->pressed_target) current->advance();
                    else current->hide();
                } else if (target != current->taskbar) current->hide();
            } else if (object == OBJID_WINDOW &&
                ((target == current->taskbar && (!current->pressed_target || event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE)) || (target == current->owner &&
                    (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE || !current->pressed_target)))) current->hide();
            return;
        }
        if ((event == EVENT_SYSTEM_FOREGROUND && target != current->owner) ||
            (target == current->owner && object == OBJID_WINDOW && (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_LOCATIONCHANGE || event == EVENT_OBJECT_HIDE))) current->hide();
    }
    static LRESULT CALLBACK proc(HWND handle, UINT message, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<CueOverlay*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<CueOverlay*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        switch (message) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER: case WM_DISPLAYCHANGE: case WM_CLOSE:
            if (self) self->hide(); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(handle, &paint); RECT area{}; GetClientRect(handle, &area);
            HBRUSH background = CreateSolidBrush(transparent); FillRect(dc, &area, background); DeleteObject(background);
            COLORREF color = self && self->color_override != CLR_INVALID ? self->color_override : GetSysColor(COLOR_HIGHLIGHT);
            HPEN pen = CreatePen(PS_SOLID, 2, color); auto old_pen = SelectObject(dc, pen);
            HBRUSH brush = CreateSolidBrush(color); auto old_brush = SelectObject(dc, brush);
            if (self && self->outline) {
                SelectObject(dc, GetStockObject(HOLLOW_BRUSH)); Rectangle(dc, 1, 1, area.right - 1, area.bottom - 1);
            } else if (self && self->points_up) {
                // Below the target, tip at the top edge.
                LONG x = area.right / 2, y = area.bottom - 2;
                POINT shape[]{{x-4,y},{x+4,y},{x+4,17},{x+12,17},{x,2},{x-12,17},{x-4,17}};
                Polygon(dc, shape, 7);
            } else {
                LONG x = area.right / 2, y = area.bottom - 2;
                POINT shape[]{{x-4,2},{x+4,2},{x+4,y-15},{x+12,y-15},{x,y},{x-12,y-15},{x-4,y-15}};
                Polygon(dc, shape, 7);
            }
            SelectObject(dc, old_brush); SelectObject(dc, old_pen); DeleteObject(brush); DeleteObject(pen); EndPaint(handle, &paint); return 0;
        }
        }
        return DefWindowProcW(handle, message, w, l);
    }
public:
    CueOverlay() {
        WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"HypeTabsCue";
        RegisterClassW(&cls);
        window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            cls.lpszClassName, L"HypeTabs tab location", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, this);
        if (window) SetLayeredWindowAttributes(window, transparent, 255, LWA_COLORKEY);
    }
    ~CueOverlay() { hide(); if (window) DestroyWindow(window); }
    CueOverlay(const CueOverlay&) = delete; CueOverlay& operator=(const CueOverlay&) = delete;
    bool arm(UINT timeout = 5000) {
        hide(); if (current && current != this) current->hide();
        current = this;
        mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
        keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, key_proc, GetModuleHandleW(nullptr), 0);
        if (!window || !mouse || !keyboard || !SetTimer(window, 1, std::clamp(timeout, 100u, 30000u), nullptr)) { hide(); return false; }
        return true;
    }
    uint64_t generation() const { return version; }
    void set_color(COLORREF value) { color_override = value; }
    // below: place the arrow under the target pointing up (tab headers sit at
    // the top of usually maximized windows); default is above, pointing down.
    bool show(RECT target, HWND target_window, UINT timeout = 5000, bool reduced_motion = false, bool below = false) {
        hide(); if (current && current != this) current->hide();
        int64_t width = static_cast<int64_t>(target.right) - target.left, height = static_cast<int64_t>(target.bottom) - target.top;
        if (!window || width < 1 || height < 1 || width > 32768 || height > 32768 ||
            target.left < -100000 || target.top < -100000 || target.right > 100000 || target.bottom > 100000 ||
            (target_window && !IsWindow(target_window))) return false;
        MONITORINFO monitor{sizeof(monitor)};
        auto screen = MonitorFromRect(&target, MONITOR_DEFAULTTONULL);
        if (!screen || !GetMonitorInfoW(screen, &monitor)) return false;
        UINT dpi = target_window ? GetDpiForWindow(target_window) : 96;
        int arrow_height = MulDiv(42, static_cast<int>(dpi), 96), arrow_width = MulDiv(32, static_cast<int>(dpi), 96);
        BOOL animations = TRUE;
        if (!reduced_motion && SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) reduced_motion = !animations;
        outline = reduced_motion || (below ? target.bottom + arrow_height > monitor.rcWork.bottom : target.top - arrow_height < monitor.rcWork.top);
        points_up = below && !outline;
        int x, y, w, h;
        if (outline) { x = target.left - 3; y = target.top - 3; w = static_cast<int>(width) + 6; h = static_cast<int>(height) + 6; }
        else { x = target.left + static_cast<int>(width) / 2 - arrow_width / 2; y = below ? target.bottom : target.top - arrow_height; w = arrow_width; h = arrow_height; }
        owner = target_window; current = this;
        mouse = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
        keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, key_proc, GetModuleHandleW(nullptr), 0);
        if (!mouse || !keyboard) { hide(); return false; }
        if (owner) {
            DWORD process{}; GetWindowThreadProcessId(owner, &process);
            changes = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, event_proc, process, 0, WINEVENT_OUTOFCONTEXT);
            foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
            if (!changes || !foreground) { hide(); return false; }
        }
        SetWindowPos(window, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(window, nullptr, TRUE);
        if (!SetTimer(window, 1, std::clamp(timeout, 100u, 30000u), nullptr)) { hide(); return false; }
        return true;
    }
    bool show_taskbar(RECT button, HWND browser_window, HWND taskbar_window, HWND receiver, UINT message,
        UINT timeout = 5000, bool reduced_motion = false) {
        if (!IsWindow(browser_window) || !IsWindow(taskbar_window) || !IsWindow(receiver) || !message) { hide(); return false; }
        if (!show(button, nullptr, timeout, reduced_motion)) return false;
        owner = browser_window; taskbar = taskbar_window; continuation_window = receiver; continuation_message = message;
        taskbar_button = button; taskbar_stage = true;
        // Both Explorer and Chrome geometry can invalidate the target.
        changes = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
        foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!changes || !foreground) { hide(); return false; }
        return true;
    }
    bool continuation_ready(uint64_t generation) const { return ready && version == generation && owner && GetForegroundWindow() == owner; }
    void hide() {
        ++version;
        if (window) { ShowWindow(window, SW_HIDE); KillTimer(window, 1); }
        if (mouse) { UnhookWindowsHookEx(mouse); mouse = nullptr; }
        if (keyboard) { UnhookWindowsHookEx(keyboard); keyboard = nullptr; }
        if (changes) { UnhookWinEvent(changes); changes = nullptr; }
        if (foreground) { UnhookWinEvent(foreground); foreground = nullptr; }
        taskbar = nullptr; continuation_window = nullptr; continuation_message = 0;
        taskbar_stage = pressed_target = released_target = ready = restoring = false;
        owner = nullptr; if (current == this) current = nullptr;
    }
    bool visible() const { return window && IsWindowVisible(window); }
    bool monitoring() const { return mouse || keyboard || changes || foreground; }
    HWND handle() const { return window; }
};
}
