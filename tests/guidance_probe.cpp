#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/taskbar_locator.hpp"
#include "../native/window_identity.hpp"
#include <stdexcept>
#include <iostream>
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void pump(unsigned ms) {
    auto end = GetTickCount64() + ms;
    do { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() < end);
}
HWND find(DWORD pid, const wchar_t* name, bool visible = false) {
    struct State { DWORD pid; const wchar_t* name; bool visible; HWND found{}; } state{pid,name,visible};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& s = *reinterpret_cast<State*>(data); DWORD pid{}; GetWindowThreadProcessId(window, &pid);
        wchar_t cls[64]{}; GetClassNameW(window, cls, 64);
        if (pid == s.pid && (!s.visible || IsWindowVisible(window)) && std::wstring_view(cls) == s.name) { s.found = window; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state)); return state.found;
}
LRESULT send(HWND window, UINT message, WPARAM w = 0, LPARAM l = 0) {
    DWORD_PTR value{};
    require(SendMessageTimeoutW(window, message, w, l, SMTO_ABORTIFHUNG, 2000, &value) != 0, "window message failed"); return static_cast<LRESULT>(value);
}
bool expect_outline = false; // reduced motion: every cue must be the static outline, never the arrow
bool cue_near(RECT cue, RECT target) {
    bool outline = cue.left == target.left - 3 && cue.top == target.top - 3 && cue.right == target.right + 3 && cue.bottom == target.bottom + 3;
    bool arrow = cue.bottom == target.top && (cue.left + cue.right) / 2 >= target.left && (cue.left + cue.right) / 2 <= target.right;
    return expect_outline ? outline : (outline || arrow);
}
bool injected_down = false;
HWND trace_browser{}, trace_taskbar{};
DWORD trace_host{};
struct Trace { DWORD event; LONG object; int target; ULONGLONG time; };
std::vector<Trace> trace;
void CALLBACK trace_event(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND && event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE && event != EVENT_OBJECT_DESTROY) return;
    if (object != OBJID_WINDOW) return;
    DWORD pid{}; GetWindowThreadProcessId(window, &pid);
    int target = window == trace_browser ? 1 : window == trace_taskbar ? 2 : pid == trace_host ? 3 : 0;
    if (trace.size() < 128) trace.push_back({event, object, target, GetTickCount64()});
}
// Move like a pointer would: Explorer's taskbar tracks hover before it accepts a press.
void move_to(POINT point) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    event.mi.dx = MulDiv(point.x - x, 65535, width - 1); event.mi.dy = MulDiv(point.y - y, 65535, height - 1);
    require(SendInput(1, &event, sizeof(event)) == 1, "mouse move injection failed");
    SetCursorPos(point.x, point.y); pump(120);
}
void input(DWORD flags) { INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = flags; require(SendInput(1, &event, sizeof(event)) == 1, "mouse injection failed"); injected_down = flags == MOUSEEVENTF_LEFTDOWN; }
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    // fallback: another visible window shares the target's taskbar identity, so
    // the host must skip the taskbar cue and directly activate with a tab cue.
    // direct: geometry cannot be verified (scaled monitor); the host must still
    // bring Chrome forward and must not draw any cue.
    // outline: the host runs with the reduced-motion preference.
    bool direct = false, fallback = false;
    for (int i = 3; i < argc; ++i) {
        std::wstring_view flag = argv[i];
        if (flag == L"fallback") fallback = true; else if (flag == L"direct") direct = true; else if (flag == L"outline") expect_outline = true; else return 2;
    }
    if (direct && fallback) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(initialized)) return 3;
    DWORD host_pid = wcstoul(argv[1], nullptr, 10), chrome_pid = wcstoul(argv[2], nullptr, 10);
    HWND prior = GetForegroundWindow(), fixture{}; POINT cursor{}; GetCursorPos(&cursor);
    int result = 0; HWINEVENTHOOK trace_hook{};
    try {
        HWND host = find(host_pid, L"HypeTabsSearch"), browser = find(chrome_pid, L"Chrome_WidgetWin_1", true);
        // With several visible windows, the fixture is the one at the harness bounds.
        if (fallback) browser = hype::unique_window_at({120, 140, 900, 600});
        DWORD browser_pid{}; if (browser) GetWindowThreadProcessId(browser, &browser_pid);
        require(host && browser && browser_pid == chrome_pid, "missing owned host or browser window");
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(browser, MONITOR_DEFAULTTONEAREST), &monitor);
        fixture = CreateWindowExW(0, L"STATIC", L"HypeTabs foreground fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            monitor.rcWork.right - 400, monitor.rcWork.top + 100, 350, 250, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(fixture && SetForegroundWindow(fixture), "could not foreground owned fixture"); pump(100);
        require(GetForegroundWindow() == fixture, "fixture did not own foreground");
        std::atomic<bool> stop{false};
        auto button = direct ? hype::LocatedTaskbar{} : hype::locate_taskbar(browser, stop);
        require(direct || (fallback ? !button.found : button.found), fallback ? "grouped taskbar identity was treated as unique" : "taskbar association is unavailable on this layout");
        if (!IsWindowVisible(host)) PostMessageW(host, WM_HOTKEY, 1, 0);
        for (int i = 0; i < 50 && !IsWindowVisible(host); ++i) pump(50);
        require(IsWindowVisible(host), "search did not open");
        HWND query = GetDlgItem(host, 301), list = GetDlgItem(host, 302);
        send(query, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"HypeTabs live browser tab"));
        for (int i = 0; i < 50 && send(list, LB_GETCOUNT) != 1; ++i) pump(50);
        require(send(list, LB_GETCOUNT) == 1, "guidance fixture was not a unique search result");
        if (fallback || direct) {
            // A real hotkey makes search the foreground window; the posted hotkey
            // cannot, so the foreground-owning fixture hands it over explicitly.
            SetForegroundWindow(host); pump(100);
            require(GetForegroundWindow() == host, "search window did not take foreground before Enter");
        }
        PostMessageW(query, WM_KEYDOWN, VK_RETURN, 0);
        HWND cue{};
        if (direct) {
            for (int i = 0; i < 70 && GetForegroundWindow() != browser; ++i) pump(50);
            pump(500); cue = find(host_pid, L"HypeTabsCue", true);
            if (GetForegroundWindow() != browser || cue) {
                wchar_t cls[64]{}; GetClassNameW(GetForegroundWindow(), cls, 64); DWORD fg_pid{}; GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
                std::wcerr << L"direct: target_foreground=" << (GetForegroundWindow() == browser) << L" foreground_class=" << cls << L" foreground_pid=" << fg_pid
                    << L" chrome_pid=" << chrome_pid << L" fixture=" << (GetForegroundWindow() == fixture) << L" cue_visible=" << (cue != nullptr) << L'\n';
            }
            require(GetForegroundWindow() == browser, "direct activation did not bring Chrome to the Windows foreground");
            require(!cue, "a cue appeared although window geometry could not be verified");
            std::cout << "PASS: direct activation brought the unverifiable Chrome window to the foreground without drawing a cue\n";
            throw 0;
        }
        if (fallback) {
            for (int i = 0; i < 70; ++i) { pump(50); cue = find(host_pid, L"HypeTabsCue", true); if (GetForegroundWindow() == browser && cue) break; }
            if (GetForegroundWindow() != browser || !cue) {
                wchar_t cls[64]{}; GetClassNameW(GetForegroundWindow(), cls, 64); DWORD fg_pid{}; GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
                std::wcerr << L"fallback: target_foreground=" << (GetForegroundWindow() == browser) << L" foreground_class=" << cls << L" foreground_pid=" << fg_pid
                    << L" chrome_pid=" << chrome_pid << L" fixture=" << (GetForegroundWindow() == fixture) << L" cue_visible=" << (cue != nullptr) << L'\n';
            }
            require(GetForegroundWindow() == browser && cue, "grouped-identity fallback did not directly activate Chrome with a tab cue");
            auto tab = hype::locate_tab(browser, L"HypeTabs live browser tab", 1, stop);
            RECT only{}; GetWindowRect(cue, &only);
            require(tab.found && cue_near(only, tab.rectangle), "fallback cue did not identify the actual selected tab header");
            RECT area{}; GetWindowRect(browser, &area); POINT content{area.right - 60, area.bottom - 70};
            require(GetAncestor(WindowFromPoint(content), GA_ROOT) == browser, "fallback dismissal click would miss owned Chrome window");
            move_to(content); input(MOUSEEVENTF_LEFTDOWN); pump(50);
            require(!find(host_pid, L"HypeTabsCue", true), "fallback click did not dismiss tab cue");
            input(MOUSEEVENTF_LEFTUP); pump(300);
            std::cout << "PASS: a grouped taskbar identity skips the taskbar cue and directly activates Chrome with a tab-header cue\n";
            throw 0;
        }
        for (int i = 0; i < 70 && !(cue = find(host_pid, L"HypeTabsCue", true)); ++i) pump(50);
        require(cue != nullptr, "no taskbar-stage cue appeared");
        RECT first{}; GetWindowRect(cue, &first);
        require(GetForegroundWindow() != browser && cue_near(first, button.rectangle), "first cue was not at the taskbar with Chrome still in background");
        auto rechecked = hype::locate_taskbar(browser, stop);
        require(rechecked.found && EqualRect(&button.rectangle, &rechecked.rectangle), "taskbar target changed before test click");
        trace_browser = browser; trace_taskbar = button.taskbar; trace_host = host_pid;
        trace_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_LOCATIONCHANGE, nullptr, trace_event, 0, 0, WINEVENT_OUTOFCONTEXT);
        POINT center{(button.rectangle.left + button.rectangle.right)/2, (button.rectangle.top + button.rectangle.bottom)/2};
        require(GetAncestor(WindowFromPoint(center), GA_ROOT) == button.taskbar, "test click would miss the identified taskbar");
        move_to(center); require(GetAncestor(WindowFromPoint(center), GA_ROOT) == button.taskbar, "taskbar target moved under the pointer"); input(MOUSEEVENTF_LEFTDOWN); pump(60);
        require(!find(host_pid, L"HypeTabsCue", true), "taskbar mouse-down did not hide cue immediately");
        input(MOUSEEVENTF_LEFTUP);
        for (int i = 0; i < 70; ++i) { pump(50); cue = find(host_pid, L"HypeTabsCue", true); if (GetForegroundWindow() == browser && cue) break; }
        if (GetForegroundWindow() != browser || !cue) std::cerr << "after click: target_foreground=" << (GetForegroundWindow() == browser) << " target_minimized=" << IsIconic(browser) << " cue_visible=" << (cue != nullptr) << '\n';
        require(GetForegroundWindow() == browser && cue, "taskbar click did not produce foreground Chrome and a second cue");
        auto tab = hype::locate_tab(browser, L"HypeTabs live browser tab", 1, stop);
        RECT second{}; GetWindowRect(cue, &second);
        require(tab.found && cue_near(second, tab.rectangle), "second cue did not identify the actual selected tab header");
        RECT area{}; GetWindowRect(browser, &area); POINT content{area.right - 60, area.bottom - 70};
        require(GetAncestor(WindowFromPoint(content), GA_ROOT) == browser, "final test click would miss owned Chrome window");
        move_to(content); input(MOUSEEVENTF_LEFTDOWN); pump(50);
        require(!find(host_pid, L"HypeTabsCue", true), "final click did not dismiss tab cue");
        input(MOUSEEVENTF_LEFTUP); pump(300);
        require(!find(host_pid, L"HypeTabsCue", true), "cue reappeared after dismissal");
        std::cout << "PASS: physical taskbar click advances from background Chrome taskbar cue to actual tab-header cue; both disappear on mouse-down\n";
    } catch (int) {
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; result = 1;
        for (const auto& event : trace) std::cerr << "event=" << std::hex << event.event << std::dec << " object=" << event.object << " target=" << event.target << " time=" << event.time << '\n';
    }
    if (trace_hook) UnhookWinEvent(trace_hook);
    // Release any injected held button even when an assertion failed.
    INPUT release{}; release.type = INPUT_MOUSE; release.mi.dwFlags = MOUSEEVENTF_LEFTUP; if (injected_down) SendInput(1, &release, sizeof(release));
    if (fixture) DestroyWindow(fixture);
    SetCursorPos(cursor.x, cursor.y); if (IsWindow(prior)) SetForegroundWindow(prior);
    CoUninitialize(); return result;
}
