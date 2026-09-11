#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/cue.hpp"
#include <iostream>
#include <cstdlib>
void check(bool ok, const char* name) { if (!ok) { std::cerr << name << '\n'; std::exit(1); } }
void pump(DWORD duration) {
    auto end = GetTickCount64() + duration;
    do {
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < end);
}
int clicks = 0;
hype::CueOverlay* show_on_click = nullptr;
RECT click_target{};
HWND taskbar_fixture{}, continuation_target{};
unsigned continuations = 0; int ignore_releases = 0;
uint64_t continued_generation = 0;
LRESULT CALLBACK test_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_APP + 10) { ++continuations; continued_generation = static_cast<uint64_t>(w); return 0; }
    if (message == WM_LBUTTONUP && window == taskbar_fixture) {
        // Explorer occasionally ignores a click; the fixture can drop the first release.
        if (ignore_releases > 0) { --ignore_releases; return DefWindowProcW(window, message, w, l); }
        // Like Explorer restoring a minimized window: foreground first, placement afterwards.
        SetForegroundWindow(continuation_target);
        if (IsIconic(continuation_target)) PostMessageW(window, WM_APP + 11, 0, 0);
    }
    if (message == WM_APP + 11) { ShowWindow(continuation_target, SW_RESTORE); return 0; }
    if (message == WM_LBUTTONDOWN) {
        ++clicks;
        if (show_on_click) { auto cue = show_on_click; show_on_click = nullptr; cue->show(click_target, nullptr); }
    }
    return DefWindowProcW(window, message, w, l);
}
void click_at(POINT point) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input[3]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dx = MulDiv(point.x - x, 65535, width - 1); input[0].mi.dy = MulDiv(point.y - y, 65535, height - 1);
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input[1].type = INPUT_MOUSE; input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[2].type = INPUT_MOUSE; input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    check(SendInput(3, input, sizeof(INPUT)) == 3, "inject test click"); pump(60);
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto original = GetForegroundWindow();
    WNDCLASSW cls{}; cls.lpfnWndProc = test_proc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"HypeTabsCueTest";
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); RegisterClassW(&cls);
    HWND own = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"HypeTabs cue test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120, 120, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetForegroundWindow(own); pump(50);
    auto before = GetForegroundWindow(); check(before == own, "test must own the foreground before injecting input");
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(before, MONITOR_DEFAULTTOPRIMARY), &monitor);
    RECT target{monitor.rcWork.left + 100, monitor.rcWork.top + 150, monitor.rcWork.left + 260, monitor.rcWork.top + 180};
    hype::CueOverlay cue;
    check(cue.show(target, nullptr, 5000, true), "show reduced-motion outline");
    RECT outline_rect{}; GetWindowRect(cue.handle(), &outline_rect);
    check(outline_rect.left == target.left - 3 && outline_rect.top == target.top - 3 &&
        outline_rect.right == target.right + 3 && outline_rect.bottom == target.bottom + 3,
        "reduced-motion cue must surround the target instead of placing an arrow above it");
    check(GetForegroundWindow() == before, "outline cue stole focus");
    cue.hide(); check(!cue.monitoring(), "outline cleanup left hooks installed");
    check(cue.show(target, nullptr, 5000, false, true), "show arrow below target");
    RECT below_rect{}; GetWindowRect(cue.handle(), &below_rect);
    BOOL animations = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    bool below_arrow = below_rect.top == target.bottom && (below_rect.left + below_rect.right) / 2 >= target.left && (below_rect.left + below_rect.right) / 2 <= target.right;
    bool below_outline = below_rect.left == target.left - 3 && below_rect.top == target.top - 3 && below_rect.right == target.right + 3 && below_rect.bottom == target.bottom + 3;
    // Windows' animation policy selects the outline even for arrow requests.
    check(animations ? below_arrow : below_outline, "below arrow must start at the target's bottom edge and stay centered (or outline when animations are off)");
    cue.set_color(RGB(200, 30, 30)); cue.hide();
    check(cue.show(target, nullptr, 200), "show cue"); pump(20);
    check(cue.visible() && cue.monitoring(), "cue not visible or monitoring");
    check(GetForegroundWindow() == before, "cue stole focus");
    auto style = GetWindowLongPtrW(cue.handle(), GWL_EXSTYLE);
    check((style & WS_EX_TRANSPARENT) && (style & WS_EX_NOACTIVATE), "cue must be click-through and nonactivating");
    pump(250); check(!cue.visible() && !cue.monitoring(), "timeout must remove cue and hooks");
    check(cue.show(target, nullptr), "show for Escape");
    INPUT keys[2]{}; keys[0].type = INPUT_KEYBOARD; keys[0].ki.wVk = VK_ESCAPE; keys[1] = keys[0]; keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    check(SendInput(2, keys, sizeof(INPUT)) == 2, "inject Escape"); pump(50);
    check(!cue.visible() && !cue.monitoring(), "Escape must remove cue and hooks");
    check(cue.arm(5000), "arm pending guidance");
    auto generation = cue.generation();
    check(SendInput(2, keys, sizeof(INPUT)) == 2, "cancel pending guidance"); pump(50);
    check(!cue.monitoring() && cue.generation() != generation, "cancelled lookup must invalidate its generation");
    POINT saved_cursor{}; GetCursorPos(&saved_cursor);
    RECT own_rect{}; GetWindowRect(own, &own_rect);
    click_target = {own_rect.left + 60, own_rect.top + 180, own_rect.left + 220, own_rect.top + 210};
    POINT selection{own_rect.left + 140, own_rect.top + 200};
    check(GetAncestor(WindowFromPoint(selection), GA_ROOT) == own, "selection click must land in the owned test window");
    cue.hide(); show_on_click = &cue;
    click_at(selection);
    bool survived_selecting_click = cue.visible();
    // A second click directly through the cue must both dismiss it and reach the
    // underlying control; no overlay consumes the user's click.
    RECT cue_rect{}; GetWindowRect(cue.handle(), &cue_rect);
    int previous_clicks = clicks;
    click_at({(cue_rect.left + cue_rect.right)/2, (cue_rect.top + cue_rect.bottom)/2});
    bool passed_through = clicks == previous_clicks + 1 && !cue.visible() && !cue.monitoring();
    check(cue.show(click_target, nullptr), "show for unrelated window click");
    HWND other = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"HypeTabs unrelated click test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        own_rect.left + 520, own_rect.top, 300, 250, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetForegroundWindow(other); pump(30);
    RECT other_rect{}; GetWindowRect(other, &other_rect); POINT elsewhere{other_rect.left + 80, other_rect.top + 120};
    check(GetAncestor(WindowFromPoint(elsewhere), GA_ROOT) == other, "outside click must land in the second owned window");
    click_at(elsewhere);
    bool dismissed_elsewhere = !cue.visible() && !cue.monitoring();
    SetCursorPos(saved_cursor.x, saved_cursor.y); DestroyWindow(other);
    check(survived_selecting_click, "selecting click must not dismiss its new cue");
    check(passed_through, "cue intercepted the click or failed to dismiss");
    check(dismissed_elsewhere, "click in another window did not dismiss the cue");
    check(!cue.show({0,0,0,0}, nullptr) && !cue.monitoring(), "invalid target must not install hooks");
    taskbar_fixture = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"HypeTabs taskbar click fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        own_rect.left + 520, own_rect.top, 300, 250, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    continuation_target = own;
    SetForegroundWindow(taskbar_fixture); pump(50);
    RECT bar{}; GetWindowRect(taskbar_fixture, &bar);
    RECT button{bar.left + 50, bar.top + 90, bar.left + 150, bar.top + 130};
    POINT center{(button.left + button.right) / 2, (button.top + button.bottom) / 2};
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10), "show taskbar stage");
    auto taskbar_generation = cue.generation();
    SetCursorPos(center.x, center.y);
    INPUT down{}; down.type = INPUT_MOUSE; down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    check(SendInput(1, &down, sizeof(INPUT)) == 1, "taskbar mouse down"); pump(60);
    check(!cue.visible() && cue.monitoring() && continuations == 0, "taskbar down must hide immediately without advancing");
    INPUT up = down; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    check(SendInput(1, &up, sizeof(INPUT)) == 1, "taskbar mouse up"); pump(60);
    for (unsigned attempt = 0; attempt < 40 && continuations == 0 && cue.monitoring(); ++attempt) pump(50);
    if (continuations != 1 || !cue.continuation_ready(taskbar_generation))
        std::cerr << "continuations=" << continuations << " generation=" << cue.generation() << " expected=" << taskbar_generation
            << " foreground_target=" << (GetForegroundWindow() == own) << " monitoring=" << cue.monitoring() << '\n';
    check(continuations == 1 && continued_generation == taskbar_generation && cue.continuation_ready(taskbar_generation),
        "taskbar release and target foreground must permit exactly one continuation");
    check(!cue.visible(), "first cue reappeared before second-stage lookup");
    NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, own, OBJID_WINDOW, CHILDID_SELF); pump(50);
    check(cue.continuation_ready(taskbar_generation), "duplicate restore-location event cancelled a valid continuation");
    click_at(selection);
    check(!cue.continuation_ready(taskbar_generation) && !cue.monitoring(), "later click must invalidate pending continuation");
    // Ignored first click: a second press on the same button must keep the stage alive and then continue.
    SetForegroundWindow(taskbar_fixture); pump(30); ignore_releases = 1;
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10), "show taskbar stage for repeated click");
    auto repeat_generation = cue.generation();
    click_at(center); pump(100);
    check(cue.monitoring() && !cue.visible() && !cue.continuation_ready(repeat_generation) && GetForegroundWindow() != own, "ignored click should leave the taskbar stage pending");
    click_at(center);
    for (unsigned attempt = 0; attempt < 40 && continuations < 2 && cue.monitoring(); ++attempt) pump(50);
    check(continuations == 2 && cue.continuation_ready(repeat_generation), "repeated taskbar click did not continue guidance");
    click_at(selection); pump(50);
    check(!cue.monitoring(), "click after repeated continuation did not clear");
    ShowWindow(own, SW_MINIMIZE); SetForegroundWindow(taskbar_fixture); pump(100);
    check(IsIconic(own) && GetForegroundWindow() != own, "could not minimize continuation target");
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10), "show taskbar stage for minimized target");
    auto minimized_generation = cue.generation();
    SetCursorPos(center.x, center.y);
    check(SendInput(1, &down, sizeof(INPUT)) == 1, "minimized taskbar mouse down"); pump(60);
    check(SendInput(1, &up, sizeof(INPUT)) == 1, "minimized taskbar mouse up");
    for (unsigned attempt = 0; attempt < 40 && (IsIconic(own) || continuations < 3) && cue.monitoring(); ++attempt) pump(50);
    pump(100);
    if (continuations != 2 || IsIconic(own) || !cue.continuation_ready(minimized_generation))
        std::cerr << "continuations=" << continuations << " iconic=" << IsIconic(own) << " ready=" << cue.continuation_ready(minimized_generation)
            << " foreground_target=" << (GetForegroundWindow() == own) << " monitoring=" << cue.monitoring() << '\n';
    check(continuations == 3 && !IsIconic(own) && cue.continuation_ready(minimized_generation),
        "restore placement change after foreground must keep the minimized continuation valid");
    RECT restored{}; GetWindowRect(own, &restored);
    MoveWindow(own, restored.left + 20, restored.top, restored.right - restored.left, restored.bottom - restored.top, TRUE); pump(100);
    check(!cue.continuation_ready(minimized_generation) && !cue.monitoring(), "movement after restore must still cancel the continuation");
    MoveWindow(own, restored.left, restored.top, restored.right - restored.left, restored.bottom - restored.top, TRUE); pump(50);
    SetForegroundWindow(taskbar_fixture); pump(30);
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10), "show taskbar stage for outside click");
    click_at({bar.left + 200, bar.top + 170});
    check(!cue.monitoring() && continuations == 3, "outside taskbar-button click must cancel rather than advance");
    SetForegroundWindow(taskbar_fixture); pump(30);
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10, 100), "show taskbar timeout");
    pump(150); check(!cue.monitoring() && !cue.visible(), "taskbar timeout leaked cue or hooks");
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10), "show taskbar Escape");
    check(SendInput(2, keys, sizeof(INPUT)) == 2, "taskbar Escape"); pump(50);
    check(!cue.monitoring() && continuations == 3, "Escape left taskbar continuation armed");
    check(cue.show_taskbar(button, own, taskbar_fixture, own, WM_APP + 10), "show taskbar drag cancellation");
    SetCursorPos(center.x, center.y);
    check(SendInput(1, &down, sizeof(INPUT)) == 1, "taskbar drag down"); pump(50);
    SetCursorPos(bar.left + 200, bar.top + 170);
    check(SendInput(1, &up, sizeof(INPUT)) == 1, "taskbar drag release"); pump(50);
    check(!cue.monitoring() && continuations == 3, "release outside target must cancel taskbar continuation");
    DestroyWindow(taskbar_fixture); taskbar_fixture = nullptr;
    SetCursorPos(saved_cursor.x, saved_cursor.y);
    DestroyWindow(own); if (IsWindow(original)) SetForegroundWindow(original);
    std::cout << "Cue click-through, below-target arrow, dismissal, taskbar release/foreground continuation, repeated click, minimized restore, cancellation, timeout and cleanup checks passed\n";
}
