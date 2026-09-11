#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <string>
#include <cstddef>
#include <cwctype>
#include <vector>
#include "core.hpp"
#include "transport.hpp"
#include "browser_state.hpp"
#include "persistence.hpp"
#include "startup.hpp"
#include "cue.hpp"
#include "tab_locator.hpp"
#include "window_identity.hpp"
#include "pending_requests.hpp"
#include "options_layout.hpp"
#include "taskbar_worker.hpp"
#include "accessibility.hpp"
#include "theme.hpp"
#include "app_icon.hpp"
#include "shortcut_hook.hpp"
#include "profile_setup.hpp"
#include "deploy.hpp"
#include "../build/embedded_assets.hpp"
#include <memory>

namespace {
constexpr UINT tray_message = WM_APP + 1;
constexpr int search_id = 101, options_id = 102, pause_id = 103, exit_id = 104, about_id = 105;
constexpr wchar_t developer_site[] = L"https://www.hypedriven.com";
// Public repository link for the About window; empty until the project has a known public page.
constexpr wchar_t project_site[] = L"https://github.com/HypeDriven/hypetabs";
HWND about_window{};
HWND main_window{}, query_box{}, results_box{}, status_label{}, options_window{}, shortcut_box{};
HFONT ui_font{};
HFONT search_font{};
std::unique_ptr<hype::OptionsLayout> options_layout;
NOTIFYICONDATAW tray{};
hype::BrowserState browser;
auto& tabs = browser.tabs;
std::unique_ptr<hype::Transport> transport;
std::unique_ptr<hype::AccessibleNames> accessible_names;
std::unique_ptr<hype::Theme> theme;
// Fixed developer link; no user text is ever combined into a launched target.
void open_developer_site() { ShellExecuteW(nullptr, L"open", developer_site, nullptr, nullptr, SW_SHOWNORMAL); }
void open_project_site() { if (project_site[0]) ShellExecuteW(nullptr, L"open", project_site, nullptr, nullptr, SW_SHOWNORMAL); }
// Chrome's display layout as last reported by any connection (all profiles see
// the same monitors); a partial sequence is staged until its final entry.
std::vector<hype::ChromeDisplay> chrome_displays, staged_displays;
uint64_t next_request = 1;
std::unique_ptr<hype::CueOverlay> cue;
std::unique_ptr<hype::TabLocator> locator;
bool guided = true;
bool reduced_motion = false;
UINT cue_seconds = 5;
constexpr UINT cue_ready = WM_APP + 3;
constexpr UINT taskbar_ready = WM_APP + 4, taskbar_clicked = WM_APP + 5;
std::unique_ptr<hype::TaskbarWorker> taskbar_worker;
struct Guidance { uint64_t connection; int64_t tab; uint64_t generation; uint64_t deadline; HWND window{}; };
std::optional<Guidance> guidance;
hype::PendingRequests pending_requests;
std::unique_ptr<hype::HistoryStore> history_store;
bool history_save_armed = false;
// Version 5 appends the cue color (COLORREF; CLR_INVALID = system highlight) to the 24-byte version 4 record.
// Version 6 appends the search widget's last size in 96-DPI units (0 = default) to the 32-byte version 5 record.
struct SavedSettings { uint32_t magic = 0x48545032; uint16_t shortcut{}; uint8_t days = 7; uint8_t version = 7; int64_t history_floor = 0; uint32_t cue_seconds = 5; uint8_t guided = 1; uint8_t reduced_motion = 0; uint8_t skip_profile_check = 0; uint8_t reserved = 0; uint32_t cue_color = CLR_INVALID; uint32_t widget_width = 0; uint32_t widget_height = 0; };
COLORREF cue_color = CLR_INVALID, pending_cue_color = CLR_INVALID;
COLORREF custom_colors[16]{};
static_assert(sizeof(SavedSettings) == 40); // 32-byte version 5 record: size fills its padding and adds 8 bytes
static_assert(offsetof(SavedSettings, cue_color) == 24);
static_assert(offsetof(SavedSettings, widget_width) == 28);
// Search widget geometry (WIDGETS.md): borderless, rounded, dragged by its body and resized
// from its right/bottom edges; only its size persists, position follows the active monitor.
constexpr int widget_radius = 12, widget_edge = 8, widget_min_width = 360, widget_min_height = 160, widget_default_width = 760;
uint32_t widget_width = 0, widget_height = 0; // 96-DPI units; 0 = size from eight result rows
bool widget_sizing = false;
bool widget_foreground = false; // set once the widget truly holds the foreground, so losing it means a click elsewhere
constexpr UINT browser_message = WM_APP + 2;
std::vector<size_t> matches;
std::vector<std::wstring> match_keys;
std::vector<std::wstring> displayed_rows;
bool paused = false;
HWND previous_window{};
bool shortcut_registered = false;
// The hotkey common control cannot capture the Windows key, so it is carried in a
// private modifier bit of the saved WORD and edited through a separate check box.
constexpr BYTE HOTKEYF_WINDOWS = 0x10;
constexpr WORD default_shortcut = MAKEWORD('W', HOTKEYF_WINDOWS);
WORD shortcut = default_shortcut;
// Plain keys and Shift alone would swallow ordinary typing; Ctrl, Alt, or Win must be held.
bool valid_shortcut(WORD value) { return LOBYTE(value) && (HIBYTE(value) & (HOTKEYF_CONTROL | HOTKEYF_ALT | HOTKEYF_WINDOWS)); }
std::unique_ptr<hype::ShortcutHook> shortcut_hook; // only for Win combinations the shell keeps from RegisterHotKey
UINT taskbar_created{};
std::wstring settings_path;
hype::StartupRegistration startup;
bool isolated_data = false;
bool startup_was_enabled = false;
bool first_run = false;
bool profile_check = true; // startup prompt for Chrome profiles that lack the extension
hype::deploy::Layout deployed; // bridge/extension unpacked from this executable (empty when isolated or failed)
struct ProfileChoice { std::wstring id, label; uint64_t connection{}; };
std::vector<ProfileChoice> profile_choices;
void refresh_profiles(HWND window);

std::wstring text(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<size_t>(length)); return value;
}
UINT modifiers(WORD hotkey) {
    BYTE flags = HIBYTE(hotkey);
    return MOD_NOREPEAT | ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) | ((flags & HOTKEYF_ALT) ? MOD_ALT : 0) |
        ((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0) | ((flags & HOTKEYF_WINDOWS) ? MOD_WIN : 0);
}
void release_shortcut() { UnregisterHotKey(main_window, 1); shortcut_hook.reset(); }
// Registers `value` as the live shortcut; Win combinations the shell reserves fall back to the keyboard hook.
bool bind_shortcut(WORD value) {
    if (RegisterHotKey(main_window, 1, modifiers(value), LOBYTE(value))) return true;
    if (!(HIBYTE(value) & HOTKEYF_WINDOWS)) return false;
    shortcut_hook = std::make_unique<hype::ShortcutHook>(main_window, value, 1);
    if (shortcut_hook->installed()) return true;
    shortcut_hook.reset(); return false;
}
bool set_shortcut(WORD value) {
    if (!valid_shortcut(value)) return false;
    if (value == shortcut && shortcut_registered) return true;
    // Probe under a second ID before releasing the working shortcut, unless the hook can take it anyway.
    if (RegisterHotKey(main_window, 2, modifiers(value), LOBYTE(value))) UnregisterHotKey(main_window, 2);
    else if (!(HIBYTE(value) & HOTKEYF_WINDOWS)) return false;
    release_shortcut();
    if (!bind_shortcut(value)) { shortcut_registered = bind_shortcut(shortcut); return false; }
    shortcut = value; shortcut_registered = true; return true;
}
bool save_settings() {
    auto temp = settings_path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    SavedSettings settings; settings.shortcut = shortcut; settings.days = static_cast<uint8_t>(browser.retention_days); settings.history_floor = browser.history_floor; settings.cue_seconds = cue_seconds; settings.guided = guided ? 1 : 0; settings.reduced_motion = reduced_motion ? 1 : 0; settings.skip_profile_check = profile_check ? 0 : 1; settings.cue_color = cue_color; settings.widget_width = widget_width; settings.widget_height = widget_height;
    bool ok = WriteFile(file, &settings, sizeof(settings), &written, nullptr) && written == sizeof(settings);
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temp.c_str(), settings_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    return ok;
}
void refresh(std::wstring selected_key = {}) {
    auto old_selection = SendMessageW(results_box, LB_GETCURSEL, 0, 0);
    if (selected_key.empty() && old_selection >= 0 && static_cast<size_t>(old_selection) < match_keys.size())
        selected_key = match_keys[static_cast<size_t>(old_selection)];
    matches = hype::search(tabs, text(query_box));
    std::vector<std::wstring> keys, rows;
    keys.reserve(matches.size()); rows.reserve(matches.size());
    for (size_t index : matches) {
        const auto& tab = tabs[index].tab;
        keys.push_back(hype::tab_key(tab));
        auto row = tab.title + L"  |  " + tab.label + L" / Window " + std::to_wstring(tab.window) + (tab.closed ? L" / Closed " + std::to_wstring(std::max(0LL, (hype::now_ms() - tab.closed) / 60000)) + (browser.connected(tab) ? L" min ago  |  " : L" min ago / Profile offline  |  ") : browser.connected(tab) ? L" / Open  |  " : L" / Disconnected  |  ") + tab.url;
        rows.push_back(std::move(row));
    }
    const bool rebuild = keys != match_keys || rows != displayed_rows;
    if (rebuild) {
        SendMessageW(results_box, WM_SETREDRAW, FALSE, 0);
        SendMessageW(results_box, LB_RESETCONTENT, 0, 0);
        for (const auto& row : rows) {
            auto added = SendMessageW(results_box, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
            if (added == LB_ERR || added == LB_ERRSPACE) {
                SendMessageW(results_box, LB_RESETCONTENT, 0, 0);
                matches.clear(); keys.clear(); rows.clear();
                SendMessageW(results_box, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(results_box, nullptr, TRUE);
                match_keys.clear(); displayed_rows.clear();
                SetWindowTextW(status_label, L"Not enough memory to show these results. Try a more specific search.");
                return;
            }
        }
        displayed_rows = std::move(rows);
    }
    match_keys = std::move(keys);
    if (!matches.empty()) {
        size_t select = 0;
        for (size_t i = 0; i < matches.size(); ++i) {
            if (match_keys[i] == selected_key) { select = i; break; }
        }
        if (rebuild || old_selection != static_cast<LRESULT>(select)) SendMessageW(results_box, LB_SETCURSEL, select, 0);
    }
    if (rebuild) {
        SendMessageW(results_box, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(results_box, nullptr, TRUE);
    }
    SetWindowTextW(status_label, tabs.empty() ? L"Connect the HypeTabs extension in each Chrome profile to find your tabs." :
        matches.empty() ? L"No matching tabs. Try a title, site, or profile name." : paused ? L"Collection paused. Results may be stale." : L"Enter to open  |  Escape to close");
}
// Chrome launches native messaging hosts through cmd.exe on Windows, so the
// browser process that will request focus is an ancestor of the bridge. Only a
// chrome.exe image within a few levels receives the foreground permission.
DWORD browser_process(uint64_t connection) {
    DWORD process = transport ? transport->client_process(connection) : 0;
    if (!process) return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    DWORD found = 0;
    for (int level = 0; level < 3 && process && !found; ++level) {
        DWORD parent = 0; PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            if (entry.th32ProcessID == process) { parent = entry.th32ParentProcessID; break; }
        }
        if (hype::chrome_process(parent)) found = parent;
        process = parent;
    }
    CloseHandle(snapshot);
    return found;
}
void dismiss(bool restore) {
    widget_foreground = false;
    ShowWindow(main_window, SW_HIDE);
    if (restore && cue) cue->hide();
    if (restore && IsWindow(previous_window)) SetForegroundWindow(previous_window);
}
void update_search_font(UINT dpi) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) return;
    auto replacement = CreateFontIndirectW(&metrics.lfMessageFont);
    if (!replacement) return;
    for (HWND child : {query_box, results_box, status_label})
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
    if (search_font) DeleteObject(search_font);
    search_font = replacement;
}
void toggle_search() {
    if (cue) cue->hide();
    if (IsWindowVisible(main_window)) { dismiss(true); return; }
    browser.expire();
    previous_window = GetForegroundWindow();
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(previous_window, MONITOR_DEFAULTTONEAREST), &monitor);
    // Move the hidden window first so Windows delivers the target monitor DPI.
    SetWindowPos(main_window, nullptr, monitor.rcWork.left, monitor.rcWork.top, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    UINT dpi = GetDpiForWindow(main_window);
    int row_height = static_cast<int>(std::max<LRESULT>(1, SendMessageW(results_box, LB_GETITEMHEIGHT, 0, 0)));
    int width = widget_width ? MulDiv(static_cast<int>(widget_width), static_cast<int>(dpi), 96) : MulDiv(widget_default_width, static_cast<int>(dpi), 96);
    int height = widget_height ? MulDiv(static_cast<int>(widget_height), static_cast<int>(dpi), 96) :
        2 * (MulDiv(16, static_cast<int>(dpi), 96) + MulDiv(32, static_cast<int>(dpi), 96) + MulDiv(8, static_cast<int>(dpi), 96)) + 8 * row_height;
    width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
    SetWindowPos(main_window, HWND_TOPMOST, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
        monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 3, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(main_window); SetFocus(query_box);
    widget_foreground = GetForegroundWindow() == main_window;
    SendMessageW(query_box, EM_SETSEL, 0, -1); refresh();
}
void update_request_timer() {
    auto delay = hype::next_request_delay(pending_requests, GetTickCount64());
    if (delay) SetTimer(main_window, 2, delay, nullptr);
    else KillTimer(main_window, 2);
}
void continue_activation(uint64_t connection, int64_t tab) {
    guidance.reset();
    auto request = next_request++;
    auto generation = cue && cue->monitoring() ? cue->generation() : 0;
    auto command = "{\"v\":1,\"type\":\"activate\",\"request\":" + std::to_string(request) +
        ",\"id\":" + std::to_string(tab) + (generation ? ",\"guided\":true}" : "}");
    if (transport && transport->send(connection, command)) {
        pending_requests[request] = {connection, L"", generation, GetTickCount64() + 10000}; update_request_timer();
    } else {
        if (cue) cue->hide();
        tray.uFlags = NIF_INFO; wcscpy_s(tray.szInfoTitle, L"HypeTabs");
        wcscpy_s(tray.szInfo, L"Chrome disconnected before guidance completed. Click here to search again.");
        tray.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND; Shell_NotifyIconW(NIM_MODIFY, &tray);
    }
}
void cancel_connection_guidance(uint64_t connection) {
    if (guidance && guidance->connection == connection) { guidance.reset(); if (cue) cue->hide(); }
    std::erase_if(pending_requests, [&](const auto& entry) {
        const auto& request = entry.second;
        if (request.connection != connection) return false;
        if (cue && request.cue_generation && request.cue_generation == cue->generation()) cue->hide();
        return true;
    });
    update_request_timer();
}
void activate_selection() {
    auto selection = SendMessageW(results_box, LB_GETCURSEL, 0, 0);
    if (selection < 0 || static_cast<size_t>(selection) >= matches.size()) return;
    if (static_cast<size_t>(selection) >= match_keys.size()) return;
    const auto& key = match_keys[static_cast<size_t>(selection)];
    auto live = std::find_if(tabs.begin(), tabs.end(), [&](const auto& item) { return hype::tab_key(item.tab) == key; });
    if (live == tabs.end()) { refresh(); SetWindowTextW(status_label, L"That result changed. Select it again from the updated list."); return; }
    const auto& tab = live->tab;
    if (!browser.connected(tab)) {
        SetWindowTextW(status_label, L"Open this Chrome profile and reconnect its extension, then try again."); return;
    }
    if (pending_requests.size() >= 16) { SetWindowTextW(status_label, L"Waiting for Chrome. Try again shortly."); return; }
    uint64_t connection = browser.connection_for(tab), request = next_request++;
    auto command = "{\"v\":1,\"type\":\"activate\",\"request\":" + std::to_string(request) + ",\"id\":" + std::to_string(tab.id) + "}";
    if (tab.closed) {
        if (!hype::http_url(tab.url)) { SetWindowTextW(status_label, L"This saved address cannot be reopened safely."); return; }
        for (const auto& [id, pending] : pending_requests) { (void)id; if (pending.record == tab.record) return; }
        command = "{\"v\":1,\"type\":\"restore\",\"request\":" + std::to_string(request) + ",\"session\":" + hype::wire::quote(hype::narrow(tab.session)) +
            ",\"url\":" + hype::wire::quote(hype::narrow(tab.url)) + "}";
    }
    uint64_t cue_generation = 0;
    guidance.reset();
    bool preparing = false;
    if (cue) cue->hide();
    if (guided && !tab.closed && cue && cue->arm(cue_seconds * 1000)) {
        cue_generation = cue->generation();
        preparing = hype::guidance_geometry_available(chrome_displays);
        if (preparing) command = "{\"v\":1,\"type\":\"prepare\",\"request\":" + std::to_string(request) + ",\"id\":" + std::to_string(tab.id) + "}";
        else { command.pop_back(); command += ",\"guided\":true}"; }
    } else if (!tab.closed && hype::guidance_geometry_available(chrome_displays)) {
        // Direct mode still needs the location hint to confirm actual Windows
        // foreground; no cue is armed, so it cannot start guidance.
        command.pop_back(); command += ",\"guided\":true}";
    }
    // Search holds the user's last input; hand that single foreground permission
    // to the browser process before the overlay hides, so Chrome's focus request
    // is honored instead of Windows keeping whichever window it activates next.
    if (auto browser_pid = browser_process(connection)) AllowSetForegroundWindow(browser_pid);
    if (transport && transport->send(connection, command)) {
        pending_requests[request] = {connection, tab.record, cue_generation, GetTickCount64() + 10000, preparing, tab.id}; update_request_timer(); dismiss(false);
    } else { if (cue) cue->hide(); SetWindowTextW(status_label, L"Chrome is busy. Try again."); }
}
LRESULT CALLBACK results_proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    static bool pressed = false;
    if (message == WM_LBUTTONDOWN) pressed = true;
    if (message == WM_CANCELMODE || message == WM_KILLFOCUS || message == WM_CAPTURECHANGED) pressed = false;
    if (message == WM_LBUTTONUP) {
        bool click = pressed; pressed = false;
        auto result = DefSubclassProc(window, message, w, l);
        auto selected = SendMessageW(window, LB_GETCURSEL, 0, 0);
        RECT item{}; POINT point{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        if (click && selected >= 0 && SendMessageW(window, LB_GETITEMRECT, selected, reinterpret_cast<LPARAM>(&item)) != LB_ERR && PtInRect(&item, point))
            activate_selection();
        return result;
    }
    return DefSubclassProc(window, message, w, l);
}
std::string collection_command() {
    return "{\"v\":1,\"type\":\"collect\",\"enabled\":" + std::string(paused ? "false" : "true") +
        ",\"history\":" + (browser.retention_days ? "true" : "false") + ",\"since\":" + std::to_string(browser.history_floor) + "}";
}
void schedule_history_save() {
    if (browser.history_dirty && !history_save_armed && !paused) {
        history_save_armed = true; SetTimer(main_window, 3, 3000, nullptr);
    }
}
void receive_browser() {
    if (!transport) return;
    bool changed = false, profiles_changed = false;
    std::wstring preserved;
    auto selected = SendMessageW(results_box, LB_GETCURSEL, 0, 0);
    if (selected >= 0 && static_cast<size_t>(selected) < match_keys.size()) preserved = match_keys[static_cast<size_t>(selected)];
    for (auto& packet : transport->receive()) {
        try {
            if (packet.data.empty()) {
                cancel_connection_guidance(packet.connection);
                profiles_changed = true; changed = browser.disconnect(packet.connection) || changed; continue; }
            {
                auto message = hype::wire::Parser(packet.data).parse();
                changed = browser.receive(packet.connection, message) || changed;
                auto type = hype::wire::get_string(message, "type", 20);
                if (type == "hello" || type == "label") profiles_changed = true;
                if (type == "label" && options_window) SetWindowTextW(GetDlgItem(options_window, 209), L"Profile name saved.");
                if (type == "labelError" && options_window) SetWindowTextW(GetDlgItem(options_window, 209), L"Chrome could not save the profile name. Try again.");
                if (type == "hello") transport->send(packet.connection, collection_command());
                if (type == "display") {
                    auto index = static_cast<size_t>(hype::wire::get_int(message, "index", 0, 15)), count = static_cast<size_t>(hype::wire::get_int(message, "count", 1, 16));
                    if (index == 0) staged_displays.clear();
                    if (index == staged_displays.size() && count <= 16) {
                        staged_displays.push_back({static_cast<LONG>(hype::wire::get_int(message, "left", -100000, 100000)), static_cast<LONG>(hype::wire::get_int(message, "top", -100000, 100000)),
                            static_cast<LONG>(hype::wire::get_int(message, "width", 1, 32768)), static_cast<LONG>(hype::wire::get_int(message, "height", 1, 32768)),
                            static_cast<int>(hype::wire::get_int(message, "dpi", 48, 960)), std::get<bool>(message.at("primary"))});
                        if (staged_displays.size() == count) { chrome_displays = staged_displays; staged_displays.clear(); }
                    } else staged_displays.clear();
                }
                if (type == "prepared") {
                    auto request = static_cast<uint64_t>(hype::wire::get_int(message, "request", 1, 9007199254740991LL));
                    auto found = pending_requests.find(request);
                    if (found != pending_requests.end() && found->second.preparing && found->second.connection == packet.connection) {
                        auto pending = found->second; pending_requests.erase(found); update_request_timer();
                        if (pending.deadline > GetTickCount64() && cue && cue->monitoring() && pending.cue_generation == cue->generation()) {
                            guidance = Guidance{packet.connection, pending.tab_id, pending.cue_generation, pending.deadline};
                            if (!taskbar_worker) taskbar_worker = std::make_unique<hype::TaskbarWorker>(main_window, taskbar_ready);
                            taskbar_worker->request({static_cast<LONG>(hype::wire::get_int(message, "left", -100000, 100000)),
                                static_cast<LONG>(hype::wire::get_int(message, "top", -100000, 100000)),
                                static_cast<LONG>(hype::wire::get_int(message, "width", 1, 32768)),
                                static_cast<LONG>(hype::wire::get_int(message, "height", 1, 32768))}, pending.cue_generation, chrome_displays);
                        }
                    }
                }
                if (type == "located") {
                    auto request = static_cast<uint64_t>(hype::wire::get_int(message, "request", 1, 9007199254740991LL));
                    auto found = pending_requests.find(request);
                    if (found != pending_requests.end() && !found->second.preparing && found->second.connection == packet.connection && found->second.deadline > GetTickCount64()) {
                        HWND identified{};
                        if (message.size() == 8) {
                            identified = hype::verified_chrome_window({
                                static_cast<LONG>(hype::wire::get_int(message, "left", -100000, 100000)),
                                static_cast<LONG>(hype::wire::get_int(message, "top", -100000, 100000)),
                                static_cast<LONG>(hype::wire::get_int(message, "width", 1, 32768)),
                                static_cast<LONG>(hype::wire::get_int(message, "height", 1, 32768))}, chrome_displays);
                        }
                        // Chrome reports focus from its own state; Windows may have kept
                        // the foreground elsewhere. The host still holds the user's last
                        // input, so it may raise the verified window itself.
                        if (identified && GetForegroundWindow() != identified) SetForegroundWindow(identified);
                        bool foreground_ok = identified && GetForegroundWindow() == identified;
                        if (identified && !foreground_ok) found->second.foreground_failed = true;
                        if (cue && cue->monitoring() && found->second.cue_generation == cue->generation()) {
                            if (foreground_ok) {
                                if (!locator) locator = std::make_unique<hype::TabLocator>(main_window, cue_ready);
                                locator->request(identified, hype::wide(hype::wire::get_string(message, "title", 4096, true)), cue->generation());
                            } else cue->hide();
                        }
                    }
                }
                if (type == "result") {
                    auto request = static_cast<uint64_t>(hype::wire::get_int(message, "request", 1, LLONG_MAX));
                    auto found = pending_requests.find(request);
                    if (found != pending_requests.end() && found->second.connection == packet.connection && found->second.deadline > GetTickCount64()) {
                        auto status = hype::wire::get_string(message, "status", 20);
                        if (status == "ok" && found->second.foreground_failed) status = "focus";
                        if (found->second.preparing) {
                            auto pending = found->second; pending_requests.erase(found); update_request_timer();
                            if (cue && cue->monitoring() && pending.cue_generation == cue->generation()) continue_activation(packet.connection, pending.tab_id);
                            continue;
                        }
                        if (!found->second.record.empty() && (status == "restored" || status == "focus")) changed = browser.consume(found->second.record) || changed;
                        pending_requests.erase(found);
                        update_request_timer();
                        if (status != "ok" && status != "restored") {
                            if (cue) cue->hide();
                            tray.uFlags = NIF_INFO; wcscpy_s(tray.szInfoTitle, L"HypeTabs");
                            wcscpy_s(tray.szInfo, status == "focus"
                                ? L"Chrome selected your tab but did not bring its window forward. Click here to return to search and retry."
                                : L"Chrome could not open the selected tab. Click here to return to search and refresh its location.");
                            tray.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
                            Shell_NotifyIconW(NIM_MODIFY, &tray);
                        }
                    }
                }
            }
        } catch (const hype::wire::Error&) { cancel_connection_guidance(packet.connection); transport->send(packet.connection, {}); browser.disconnect(packet.connection); changed = true; profiles_changed = true; }
    }
    if (profiles_changed && options_window) refresh_profiles(options_window);
    schedule_history_save();
    if (changed && IsWindowVisible(main_window)) refresh(preserved);
}
HWND control(HWND parent, const wchar_t* cls, const wchar_t* label, DWORD style, int id) {
    auto handle = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), TRUE);
    if (theme) theme->apply_control(handle);
    return handle;
}
HICON tray_icon{}, window_icon{};
hype::IconState shown_state = hype::IconState::Disconnected; bool icon_drawn = false;
hype::IconState current_icon_state() {
    if (!pending_requests.empty() || (cue && cue->monitoring())) return hype::IconState::Active;
    if (paused) return hype::IconState::Paused;
    return browser.profiles.empty() ? hype::IconState::Disconnected : hype::IconState::Connected;
}
// Redraws the tray and window icons only when the reported state changes.
void update_icons(bool force = false) {
    auto state = current_icon_state();
    if (icon_drawn && !force && state == shown_state) return;
    shown_state = state; icon_drawn = true;
    HICON icon_small = hype::create_app_icon(state, GetSystemMetrics(SM_CXSMICON)), icon_large = hype::create_app_icon(state, GetSystemMetrics(SM_CXICON));
    if (main_window) { SendMessageW(main_window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small)); SendMessageW(main_window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_large)); }
    if (options_window) { SendMessageW(options_window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small)); SendMessageW(options_window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_large)); }
    if (about_window) { SendMessageW(about_window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small)); SendMessageW(about_window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_large)); }
    tray.hIcon = icon_small;
    if (tray.cbSize) { tray.uFlags = NIF_ICON | NIF_TIP; Shell_NotifyIconW(NIM_MODIFY, &tray); }
    if (tray_icon) DestroyIcon(tray_icon); if (window_icon) DestroyIcon(window_icon);
    tray_icon = icon_small; window_icon = icon_large;
}
void add_tray() {
    tray.cbSize = sizeof(tray); tray.hWnd = main_window; tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = tray_message;
    update_icons(true); tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.hIcon = tray_icon;
    wcscpy_s(tray.szTip, L"HypeTabs — Find your Chrome tabs");
    Shell_NotifyIconW(NIM_ADD, &tray);
}
void select_profile(HWND window) {
    auto selection = SendMessageW(GetDlgItem(window, 206), LB_GETCURSEL, 0, 0);
    bool valid = selection >= 0 && static_cast<size_t>(selection) < profile_choices.size();
    bool connected = valid && profile_choices[static_cast<size_t>(selection)].connection != 0;
    SetWindowTextW(GetDlgItem(window, 207), valid ? profile_choices[static_cast<size_t>(selection)].label.c_str() : L"");
    EnableWindow(GetDlgItem(window, 207), connected); EnableWindow(GetDlgItem(window, 208), connected);
}
void refresh_profiles(HWND window) {
    auto list = GetDlgItem(window, 206); if (!list) return;
    auto old = SendMessageW(list, LB_GETCURSEL, 0, 0);
    std::wstring previous;
    if (old >= 0 && static_cast<size_t>(old) < profile_choices.size()) previous = profile_choices[static_cast<size_t>(old)].id;
    std::map<std::wstring, ProfileChoice> choices;
    for (const auto& item : tabs) choices[item.tab.profile] = {item.tab.profile, item.tab.label, 0};
    for (const auto& [id, profile] : browser.profiles) choices[profile.id] = {profile.id, profile.label, id};
    auto draft = text(GetDlgItem(window, 207));
    bool editing = GetFocus() == GetDlgItem(window, 207);
    profile_choices.clear(); SendMessageW(list, WM_SETREDRAW, FALSE, 0); SendMessageW(list, LB_RESETCONTENT, 0, 0);
    size_t select = 0;
    for (const auto& [id, choice] : choices) {
        if (id == previous) select = profile_choices.size();
        profile_choices.push_back(choice);
        auto label = choice.label + (choice.connection ? L" — Connected" : L" — Open this profile to reconnect");
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!profile_choices.empty()) SendMessageW(list, LB_SETCURSEL, select, 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE); select_profile(window);
    if (editing && select < profile_choices.size() && profile_choices[select].id == previous) SetWindowTextW(GetDlgItem(window, 207), draft.c_str());
}
LRESULT CALLBACK options_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE: {
        options_layout = std::make_unique<hype::OptionsLayout>(window);
        auto label = control(window, L"STATIC", L"Search shortcut (Ctrl, Alt, or the Windows key required)", 0, 0);
        options_layout->place(label, 20, 20, 380, 24);
        shortcut_box = control(window, HOTKEY_CLASSW, L"Search shortcut", WS_TABSTOP | WS_BORDER, 201);
        options_layout->place(shortcut_box, 20, 52, 130, 30);
        SendMessageW(shortcut_box, HKM_SETHOTKEY, static_cast<WPARAM>(shortcut & ~static_cast<WORD>(HOTKEYF_WINDOWS << 8)), 0);
        auto windows_key = control(window, L"BUTTON", L"Windows key", WS_TABSTOP | BS_AUTOCHECKBOX, 225);
        options_layout->place(windows_key, 158, 52, 105, 30);
        SendMessageW(windows_key, BM_SETCHECK, (HIBYTE(shortcut) & HOTKEYF_WINDOWS) ? BST_CHECKED : BST_UNCHECKED, 0);
        auto save = control(window, L"BUTTON", L"Save options", WS_TABSTOP | BS_DEFPUSHBUTTON, 202);
        options_layout->place(save, 275, 52, 125, 30);
        auto reset = control(window, L"BUTTON", L"Reset", WS_TABSTOP, 203);
        options_layout->place(reset, 20, 95, 100, 30);
        auto retention_label = control(window, L"STATIC", L"Keep recently closed tabs", 0, 0);
        options_layout->place(retention_label, 20, 145, 240, 24);
        auto retention = control(window, L"COMBOBOX", L"Retention", WS_TABSTOP | CBS_DROPDOWNLIST, 204);
        options_layout->place(retention, 20, 172, 240, 200);
        for (int day = 0; day <= 7; ++day) {
            auto caption = day ? std::to_wstring(day) + (day == 1 ? L" day" : L" days") : L"Do not retain closed tabs";
            SendMessageW(retention, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(caption.c_str()));
        }
        SendMessageW(retention, CB_SETCURSEL, browser.retention_days, 0);
        auto clear = control(window, L"BUTTON", L"Clear saved data", WS_TABSTOP, 205);
        options_layout->place(clear, 275, 172, 125, 30);
        auto sign_in = control(window, L"BUTTON", L"Start HypeTabs when I sign in", WS_TABSTOP | BS_AUTOCHECKBOX, 210);
        options_layout->place(sign_in, 140, 98, 310, 26);
        startup_was_enabled = startup.status() == hype::StartupRegistration::Status::Enabled;
        SendMessageW(sign_in, BM_SETCHECK, startup_was_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(sign_in, !isolated_data);
        auto profiles_label = control(window, L"STATIC", L"Chrome profiles", 0, 0);
        options_layout->place(profiles_label, 20, 220, 380, 24);
        auto profiles_list = control(window, L"LISTBOX", L"Chrome profiles", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 206);
        options_layout->place(profiles_list, 20, 250, 405, 90);
        auto name_label = control(window, L"STATIC", L"Profile name", 0, 216);
        options_layout->place(name_label, 20, 346, 255, 24);
        auto profile_name = control(window, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 207);
        options_layout->place(profile_name, 20, 373, 255, 28); SendMessageW(profile_name, EM_SETLIMITTEXT, 32, 0);
        auto rename = control(window, L"BUTTON", L"Save name", WS_TABSTOP, 208);
        options_layout->place(rename, 290, 373, 135, 28);
        auto status = control(window, L"STATIC", L"Names are saved in their Chrome profiles.", 0, 209);
        options_layout->place(status, 20, 410, 405, 36);
        auto info = control(window, L"STATIC", L"To connect another profile: open chrome://extensions in that profile, load the installed HypeTabs extension folder, then click its toolbar icon to reconnect.", 0, 0);
        options_layout->place(info, 20, 453, 405, 72);
        auto check = control(window, L"BUTTON", L"Check Chrome profiles for the extension at startup", WS_TABSTOP | BS_AUTOCHECKBOX, 225);
        options_layout->place(check, 20, 527, 405, 26); SendMessageW(check, BM_SETCHECK, profile_check ? BST_CHECKED : BST_UNCHECKED, 0);
        auto guide = control(window, L"BUTTON", L"Show location cues", WS_TABSTOP | BS_AUTOCHECKBOX, 217);
        options_layout->place(guide, 20, 567, 250, 26); SendMessageW(guide, BM_SETCHECK, guided ? BST_CHECKED : BST_UNCHECKED, 0);
        auto duration_label = control(window, L"STATIC", L"Cue duration", 0, 219); options_layout->place(duration_label, 20, 609, 130, 24);
        auto duration = control(window, L"COMBOBOX", L"Cue duration", WS_TABSTOP | CBS_DROPDOWNLIST, 218);
        options_layout->place(duration, 160, 605, 170, 200);
        for (int seconds = 1; seconds <= 30; ++seconds) {
            auto duration_caption = std::to_wstring(seconds) + (seconds == 1 ? L" second" : L" seconds");
            SendMessageW(duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(duration_caption.c_str()));
        }
        SendMessageW(duration, CB_SETCURSEL, cue_seconds - 1, 0);
        auto motion = control(window, L"BUTTON", L"Use outline cues (reduced motion)", WS_TABSTOP | BS_AUTOCHECKBOX, 220);
        options_layout->place(motion, 20, 645, 405, 26);
        SendMessageW(motion, BM_SETCHECK, reduced_motion ? BST_CHECKED : BST_UNCHECKED, 0);
        auto color_label = control(window, L"STATIC", L"Cue colour", 0, 0); options_layout->place(color_label, 20, 683, 130, 24);
        auto swatch = control(window, L"STATIC", L"", SS_OWNERDRAW, 223); options_layout->place(swatch, 160, 681, 40, 26);
        auto pick = control(window, L"BUTTON", L"Choose…", WS_TABSTOP, 222); options_layout->place(pick, 210, 679, 100, 30);
        auto system_color = control(window, L"BUTTON", L"System", WS_TABSTOP, 224); options_layout->place(system_color, 320, 679, 105, 30);
        pending_cue_color = cue_color;
        auto about = control(window, L"STATIC", L"Developed by hypedriven.com", SS_NOTIFY | SS_RIGHT, 221);
        options_layout->place(about, 20, 723, 405, 20);
        if (theme) theme->apply(window);
        options_layout->set_dpi(GetDpiForWindow(window));
        refresh_profiles(window); return 0;
    }
    case WM_SIZE: if (options_layout) options_layout->arrange(); return 0;
    case WM_DPICHANGED:
        if (options_layout) { options_layout->set_dpi(HIWORD(w)); options_layout->fit(reinterpret_cast<RECT*>(l)); }
        return 0;
    case WM_VSCROLL: case WM_HSCROLL:
        if (options_layout && !l) options_layout->scroll(message == WM_VSCROLL ? SB_VERT : SB_HORZ, LOWORD(w));
        return 0;
    case WM_MOUSEWHEEL:
        if (options_layout) options_layout->scroll(SB_VERT, static_cast<short>(HIWORD(w)) > 0 ? SB_LINEUP : SB_LINEDOWN);
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
        if (theme) if (auto brush = theme->color(message, reinterpret_cast<HDC>(w), GetDlgCtrlID(reinterpret_cast<HWND>(l)) == 221)) return reinterpret_cast<LRESULT>(brush);
        break;
    case WM_DRAWITEM:
        if (w == 223) {
            auto item = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            COLORREF shown = pending_cue_color != CLR_INVALID ? pending_cue_color : GetSysColor(COLOR_HIGHLIGHT);
            HBRUSH fill = CreateSolidBrush(shown); FillRect(item->hDC, &item->rcItem, fill); DeleteObject(fill);
            FrameRect(item->hDC, &item->rcItem, reinterpret_cast<HBRUSH>(GetStockObject(theme && theme->active() ? WHITE_BRUSH : BLACK_BRUSH)));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == 222) {
            // Standard color dialog; the choice applies when options are saved.
            CHOOSECOLORW choose{sizeof(choose)}; choose.hwndOwner = window; choose.lpCustColors = custom_colors;
            choose.rgbResult = pending_cue_color != CLR_INVALID ? pending_cue_color : GetSysColor(COLOR_HIGHLIGHT); choose.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&choose)) { pending_cue_color = choose.rgbResult & 0x00ffffff; InvalidateRect(GetDlgItem(window, 223), nullptr, TRUE); }
        }
        if (LOWORD(w) == 224) { pending_cue_color = CLR_INVALID; InvalidateRect(GetDlgItem(window, 223), nullptr, TRUE); }
        if (LOWORD(w) == 221 && HIWORD(w) == STN_CLICKED) open_developer_site();
        if (LOWORD(w) == 206 && HIWORD(w) == LBN_SELCHANGE) select_profile(window);
        if (LOWORD(w) == 208) {
            auto selected = SendMessageW(GetDlgItem(window, 206), LB_GETCURSEL, 0, 0);
            auto label = text(GetDlgItem(window, 207));
            while (!label.empty() && std::iswspace(label.front())) label.erase(label.begin());
            while (!label.empty() && std::iswspace(label.back())) label.pop_back();
            if (selected < 0 || static_cast<size_t>(selected) >= profile_choices.size() || label.empty() || label.size() > 32 ||
                std::any_of(label.begin(), label.end(), [](wchar_t c) { return c < 32 || c == 127; })) {
                SetWindowTextW(GetDlgItem(window, 209), L"Choose a connected profile and enter a name."); return 0;
            }
            auto connection = profile_choices[static_cast<size_t>(selected)].connection;
            bool sent = false;
            try { sent = connection && transport && transport->send(connection, "{\"v\":1,\"type\":\"setLabel\",\"label\":" + hype::wire::quote(hype::narrow(label)) + "}"); }
            catch (const hype::wire::Error&) { SetWindowTextW(GetDlgItem(window, 209), L"Enter a valid profile name."); return 0; }
            SetWindowTextW(GetDlgItem(window, 209), sent ? L"Saving profile name…" : L"This profile disconnected. Open it and try again.");
        }
        if (LOWORD(w) == 203) {
            SendMessageW(shortcut_box, HKM_SETHOTKEY, static_cast<WPARAM>(default_shortcut & ~static_cast<WORD>(HOTKEYF_WINDOWS << 8)), 0);
            SendMessageW(GetDlgItem(window, 225), BM_SETCHECK, (HIBYTE(default_shortcut) & HOTKEYF_WINDOWS) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        if (LOWORD(w) == 205) {
            guidance.reset(); if (cue) cue->hide();
            browser.history_floor = hype::now_ms(); tabs.clear(); browser.history_dirty = false;
            pending_requests.clear(); update_request_timer(); KillTimer(main_window, 3); history_save_armed = false;
            if (history_store && !history_store->clear()) MessageBoxW(window, L"Saved data could not be removed. Close other HypeTabs processes and try again.", L"HypeTabs", MB_OK | MB_ICONERROR);
            if (!save_settings()) MessageBoxW(window, L"The clear-data setting could not be saved. Check access to the application data folder.", L"HypeTabs", MB_OK | MB_ICONERROR);
            refresh_profiles(window);
            for (auto& [id, profile] : browser.profiles) {
                profile.snapshot = false; profile.staging.clear();
                if (transport) transport->send(id, collection_command());
            }
            refresh();
        }
        if (LOWORD(w) == 202) {
            auto value = static_cast<WORD>(SendMessageW(shortcut_box, HKM_GETHOTKEY, 0, 0));
            if (SendMessageW(GetDlgItem(window, 225), BM_GETCHECK, 0, 0) == BST_CHECKED) value = static_cast<WORD>(value | (HOTKEYF_WINDOWS << 8));
            bool sign_in = SendMessageW(GetDlgItem(window, 210), BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!isolated_data && sign_in != startup_was_enabled && !startup.set(sign_in)) {
                MessageBoxW(window, L"Windows could not update sign-in startup. Check access to your user settings and try again.", L"HypeTabs", MB_OK | MB_ICONERROR); return 0;
            }
            startup_was_enabled = sign_in;
            if (set_shortcut(value)) {
                auto days = SendMessageW(GetDlgItem(window, 204), CB_GETCURSEL, 0, 0);
                if (days >= 0 && days <= 7) browser.retention_days = static_cast<int>(days);
                if (!browser.retention_days) {
                    browser.history_floor = hype::now_ms();
                    if (history_store) history_store->clear();
                }
                reduced_motion = SendMessageW(GetDlgItem(window, 220), BM_GETCHECK, 0, 0) == BST_CHECKED;
                cue_color = pending_cue_color; if (cue) cue->set_color(cue_color);
                guided = SendMessageW(GetDlgItem(window, 217), BM_GETCHECK, 0, 0) == BST_CHECKED;
                profile_check = SendMessageW(GetDlgItem(window, 225), BM_GETCHECK, 0, 0) == BST_CHECKED;
                auto duration = SendMessageW(GetDlgItem(window, 218), CB_GETCURSEL, 0, 0);
                if (duration >= 0 && duration < 30) cue_seconds = static_cast<UINT>(duration + 1);
                if (cue) cue->hide();
                browser.expire();
                if (!save_settings()) { MessageBoxW(window, L"Options could not be saved. Check access to the application data folder.", L"HypeTabs", MB_OK | MB_ICONERROR); return 0; }
                schedule_history_save();
                for (auto& [id, profile] : browser.profiles) {
                    profile.snapshot = false; profile.staging.clear();
                    if (transport) transport->send(id, collection_command());
                }
                refresh(); DestroyWindow(window);
            }
            else MessageBoxW(window, L"That shortcut is unavailable. Choose a different combination with Ctrl, Alt, or the Windows key.", L"HypeTabs", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    case WM_DESTROY: options_layout.reset(); options_window = nullptr; return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
// About: version, developer link, and the project page when one is public.
LRESULT CALLBACK about_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE: {
        int dpi = static_cast<int>(GetDpiForWindow(window)); auto px = [&](int v) { return MulDiv(v, dpi, 96); };
        auto title = control(window, L"STATIC", L"HypeTabs 0.1.0", 0, 0); MoveWindow(title, px(20), px(18), px(300), px(24), TRUE);
        auto text = control(window, L"STATIC", L"Find and jump to your open Chrome tabs across profiles.", 0, 0); MoveWindow(text, px(20), px(46), px(320), px(40), TRUE);
        auto developer = control(window, L"STATIC", L"Developed by hypedriven.com", SS_NOTIFY, 231); MoveWindow(developer, px(20), px(96), px(320), px(22), TRUE);
        if (project_site[0]) { auto project = control(window, L"STATIC", L"Project page on GitHub", SS_NOTIFY, 232); MoveWindow(project, px(20), px(122), px(320), px(22), TRUE); }
        if (theme) theme->apply(window);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if (theme) if (auto brush = theme->color(message, reinterpret_cast<HDC>(w), GetDlgCtrlID(reinterpret_cast<HWND>(l)) >= 231)) return reinterpret_cast<LRESULT>(brush);
        break;
    case WM_COMMAND:
        if (HIWORD(w) == STN_CLICKED && LOWORD(w) == 231) open_developer_site();
        if (HIWORD(w) == STN_CLICKED && LOWORD(w) == 232) open_project_site();
        return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: about_window = nullptr; return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void show_about() {
    if (cue) cue->hide();
    int dpi = static_cast<int>(GetDpiForWindow(main_window));
    if (!about_window) about_window = CreateWindowExW(WS_EX_APPWINDOW, L"HypeTabsAbout", L"About HypeTabs", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(370, dpi, 96), MulDiv(project_site[0] ? 200 : 175, dpi, 96), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    update_icons(true);
    ShowWindow(about_window, SW_SHOW); SetForegroundWindow(about_window);
}
// Startup check: list Chrome profiles whose extension records lack HypeTabs and offer to open
// chrome://extensions in each so the user can load the installed extension folder there.
void prompt_missing_profiles() {
    if (deployed.extension.empty()) return; // nothing unpacked, so nothing can be loaded into Chrome
    const auto& folder = deployed.extension;
    auto profiles = hype::profiles::scan(hype::profiles::default_user_data(), folder, deployed.extension_id);
    std::erase_if(profiles, [](const auto& profile) { return profile.integrated; });
    if (profiles.empty()) return;
    auto chrome = hype::profiles::chrome_executable();
    std::wstring message = profiles.size() == 1 ? L"This Chrome profile does not have the HypeTabs extension yet:\n" : L"These Chrome profiles do not have the HypeTabs extension yet:\n";
    size_t listed = 0;
    for (const auto& profile : profiles) { if (listed++ == 8) { message += L"    …\n"; break; } message += L"    " + profile.name + L"\n"; }
    message += L"\nHypeTabs can only find tabs in profiles where the extension is loaded.\n\nIn each profile, type chrome://extensions in the address bar, turn on Developer mode, choose Load unpacked, and paste this folder (copied to the clipboard when you choose Yes):\n" + folder +
        (chrome.empty() ? L"\n\nChrome could not be located, so open those profiles yourself." : L"\n\nOpen a window for each of these profiles now?") + L"\n\nYou can turn this check off in Options.";
    if (MessageBoxW(nullptr, message.c_str(), L"HypeTabs — Chrome profiles", (chrome.empty() ? MB_OK : MB_YESNO) | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDYES) return;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (folder.size() + 1) * sizeof(wchar_t))) {
            if (auto target = static_cast<wchar_t*>(GlobalLock(memory))) { std::copy(folder.begin(), folder.end(), target); target[folder.size()] = L'\0'; GlobalUnlock(memory); }
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
    }
    bool failed = false;
    for (const auto& profile : profiles) failed |= !hype::profiles::open_profile(chrome, profile.directory);
    if (failed) MessageBoxW(nullptr, L"Some Chrome profiles could not be opened. Open them yourself, go to chrome://extensions, and load the extension folder.", L"HypeTabs", MB_OK | MB_ICONINFORMATION);
}
void show_options() {
    if (cue) cue->hide();
    if (!options_window) options_window = CreateWindowExW(WS_EX_APPWINDOW, L"HypeTabsOptions", L"HypeTabs options",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VSCROLL | WS_HSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 475, 832, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (options_layout) options_layout->fit();
    update_icons(true);
    ShowWindow(options_window, SW_SHOW); SetForegroundWindow(options_window);
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (taskbar_created && message == taskbar_created) { add_tray(); return 0; }
    // State-colored icon: cheap comparison per message, redraw only on change.
    if (tray.cbSize && message != WM_DESTROY) update_icons();
    switch (message) {
    case WM_CREATE:
        query_box = control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 301);
        SendMessageW(query_box, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find a tab by title, site, or profile"));
        SendMessageW(query_box, EM_SETLIMITTEXT, 512, 0);
        results_box = control(window, L"LISTBOX", L"Matching tabs", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 302);
        SetWindowSubclass(results_box, results_proc, 1, 0);
        status_label = control(window, L"STATIC", L"", 0, 303);
        if (theme) theme->apply(window);
        if (!accessible_names) accessible_names = std::make_unique<hype::AccessibleNames>();
        accessible_names->name(query_box, L"Find a tab by title, site, or profile");
        accessible_names->name(results_box, L"Matching tabs");
        accessible_names->name(status_label, L"Search status");
        accessible_names->live_status(status_label);
        update_search_font(GetDpiForWindow(window)); return 0;
    case WM_SIZE: {
        RECT area{}; GetClientRect(window, &area);
        int dpi = static_cast<int>(GetDpiForWindow(window));
        int margin = MulDiv(16, dpi, 96), field = MulDiv(32, dpi, 96), gap = MulDiv(8, dpi, 96);
        int width = std::max(0L, area.right - 2 * margin);
        MoveWindow(query_box, margin, margin, width, field, TRUE);
        MoveWindow(results_box, margin, margin + field + gap, width, std::max(0L, area.bottom - 2 * margin - 2 * field - 2 * gap), TRUE);
        MoveWindow(status_label, margin, std::max(0L, area.bottom - margin - field), width, field, TRUE);
        int radius = MulDiv(widget_radius, dpi, 96);
        SetWindowRgn(window, CreateRoundRectRgn(0, 0, area.right + 1, area.bottom + 1, radius, radius), TRUE);
        if (widget_sizing) { widget_width = static_cast<uint32_t>(MulDiv(area.right, 96, dpi)); widget_height = static_cast<uint32_t>(MulDiv(area.bottom, 96, dpi)); }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        int dpi = static_cast<int>(GetDpiForWindow(window)); auto info = reinterpret_cast<MINMAXINFO*>(l);
        info->ptMinTrackSize = {MulDiv(widget_min_width, dpi, 96), MulDiv(widget_min_height, dpi, 96)}; return 0;
    }
    case WM_NCHITTEST: {
        // No frame: the body drags the widget, the right/bottom edge zone resizes it.
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(window, &point);
        RECT area{}; GetClientRect(window, &area);
        int edge = MulDiv(widget_edge, static_cast<int>(GetDpiForWindow(window)), 96);
        bool right = point.x >= area.right - edge, bottom = point.y >= area.bottom - edge;
        return right && bottom ? HTBOTTOMRIGHT : right ? HTRIGHT : bottom ? HTBOTTOM : HTCAPTION;
    }
    case WM_ENTERSIZEMOVE: widget_sizing = true; return 0;
    case WM_EXITSIZEMOVE: widget_sizing = false; save_settings(); return 0;
    case WM_ACTIVATE:
        // Any click outside the widget hides it; the click already chose the next foreground window.
        // A refused foreground request also deactivates, so only a widget that held the foreground hides.
        if (LOWORD(w) == WA_INACTIVE && widget_foreground && IsWindowVisible(window)) dismiss(false);
        break;
    case WM_DPICHANGED: {
        update_search_font(HIWORD(w));
        auto rect = reinterpret_cast<RECT*>(l);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
    }
    case WM_HOTKEY: toggle_search(); return 0;
    case browser_message: receive_browser(); return 0;
    case taskbar_ready:
        if (taskbar_worker) {
            auto result = taskbar_worker->take();
            if (result && guidance && result->generation == guidance->generation && cue && cue->monitoring() &&
                cue->generation() == guidance->generation && guidance->deadline > GetTickCount64()) {
                auto action = *guidance;
                const auto& location = result->location;
                if (location.found && hype::verified_chrome_window(result->bounds, chrome_displays) == location.window &&
                    GetForegroundWindow() != location.window &&
                    cue->show_taskbar(location.rectangle, location.window, location.taskbar, main_window, taskbar_clicked, cue_seconds * 1000, reduced_motion)) {
                    guidance->generation = cue->generation(); guidance->window = location.window;
                    guidance->deadline = GetTickCount64() + cue_seconds * 1000;
                } else continue_activation(action.connection, action.tab);
            }
        }
        return 0;
    case taskbar_clicked:
        if (guidance && cue && guidance->generation == static_cast<uint64_t>(w) &&
            guidance->deadline > GetTickCount64() && cue->continuation_ready(guidance->generation) &&
            GetForegroundWindow() == guidance->window) {
            auto action = *guidance; continue_activation(action.connection, action.tab);
        }
        return 0;
    case cue_ready:
        if (locator) {
            auto result = locator->take();
            if (result && cue && cue->monitoring() && result->generation == cue->generation()) {
                if (result->found && GetForegroundWindow() == result->window) cue->show(result->rectangle, result->window, cue_seconds * 1000, reduced_motion, true);
                else cue->hide();
            }
        }
        return 0;
    case WM_TIMER:
        if (w == 2) {
            auto expired = hype::expire_requests(pending_requests, GetTickCount64(), [](const auto& request) {
                if (cue && request.cue_generation && request.cue_generation == cue->generation()) cue->hide();
            });
            update_request_timer();
            if (expired) {
                tray.uFlags = NIF_INFO; wcscpy_s(tray.szInfoTitle, L"HypeTabs");
                wcscpy_s(tray.szInfo, L"Chrome did not respond in time. Click here to search again and check whether the tab opened.");
                tray.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
                Shell_NotifyIconW(NIM_MODIFY, &tray);
            }
        }
        if (w == 3) {
            KillTimer(window, 3); history_save_armed = false;
            if (history_store && browser.history_dirty && !paused) { history_store->submit(tabs); browser.history_dirty = false; }
        }
        if (w == 4) {
            if (browser.expire()) {
                if (history_store) { history_store->submit(tabs); browser.history_dirty = false; }
                if (IsWindowVisible(window)) refresh();
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if (theme) if (auto brush = theme->color(message, reinterpret_cast<HDC>(w), reinterpret_cast<HWND>(l) == status_label)) return reinterpret_cast<LRESULT>(brush);
        break;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case about_id: show_about(); break;
        case search_id: toggle_search(); break;
        case options_id: show_options(); break;
        case pause_id:
            if (!paused && history_store && browser.history_dirty) { history_store->submit(tabs); browser.history_dirty = false; }
            paused = !paused; browser.paused = paused;
            for (auto& [id, profile] : browser.profiles) {
                profile.snapshot = false; profile.staging.clear();
                if (transport) transport->send(id, collection_command());
            }
            refresh(); break;
        case exit_id: DestroyWindow(window); break;
        case 301: if (HIWORD(w) == EN_CHANGE) refresh(); break;
        case 302: break; // Keyboard selection alone must never open a result.
        } return 0;
    case tray_message:
        if (l == NIN_BALLOONUSERCLICK) {
            if (!IsWindowVisible(window)) toggle_search();
            else { SetForegroundWindow(window); SetFocus(query_box); widget_foreground = GetForegroundWindow() == window; }
            SetWindowTextW(status_label, L"Select the tab and press Enter to retry. If Chrome stays behind, open its window from the taskbar.");
        }
        if (l == WM_LBUTTONUP) toggle_search();
        if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, search_id, L"Search");
            AppendMenuW(menu, MF_STRING, options_id, L"Options");
            AppendMenuW(menu, MF_STRING | (paused ? MF_CHECKED : 0), pause_id, L"Pause collection");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, about_id, L"About HypeTabs");
            AppendMenuW(menu, MF_STRING, exit_id, L"Exit");
            POINT point{}; GetCursorPos(&point); SetForegroundWindow(window);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
            PostMessageW(window, WM_NULL, 0, 0); DestroyMenu(menu);
        } return 0;
    case WM_CLOSE: dismiss(true); return 0;
    case WM_DESTROY:
        guidance.reset(); taskbar_worker.reset(); cue.reset(); locator.reset();
        if (accessible_names) { for (HWND child : {query_box, results_box, status_label}) accessible_names->clear(child); accessible_names.reset(); }
        transport.reset();
        if (history_store && browser.history_dirty && !paused) history_store->submit(tabs);
        history_store.reset();
        Shell_NotifyIconW(NIM_DELETE, &tray); release_shortcut();
        if (options_window) DestroyWindow(options_window);
        if (about_window) DestroyWindow(about_window);
        theme.reset();
        if (tray_icon) { DestroyIcon(tray_icon); tray_icon = nullptr; } if (window_icon) { DestroyIcon(window_icon); window_icon = nullptr; }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\HypeTabs.Desktop");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return 0; }
    std::wstring override_data;
    int argument_count{}; auto arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments) {
        if (argument_count == 3 && std::wstring_view(arguments[1]) == L"--data-dir") override_data = arguments[2];
        LocalFree(arguments);
    }
    isolated_data = !override_data.empty();
    PWSTR local{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        settings_path = override_data.empty() ? std::wstring(local) + L"\\HypeTabs" : override_data; CoTaskMemFree(local);
        CreateDirectoryW(settings_path.c_str(), nullptr); settings_path += L"\\settings.bin";
        HANDLE file = CreateFileW(settings_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        first_run = file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND;
        if (file != INVALID_HANDLE_VALUE) {
            SavedSettings settings{}; DWORD read{};
            if (ReadFile(file, &settings, sizeof(settings), &read, nullptr)) {
                WORD value{};
                if (read == 2) value = static_cast<WORD>(settings.magic & 0xffff);
                else if (((read == 16 && settings.version == 2) || (read == 24 && (settings.version == 3 || settings.version == 4)) || (read == 32 && settings.version == 5) || (read == sizeof(settings) && (settings.version == 6 || settings.version == 7))) &&
                    settings.magic == 0x48545032 && settings.days <= 7 && settings.history_floor >= 0 && settings.history_floor <= 9007199254740991LL) {
                    value = settings.shortcut; browser.retention_days = settings.days; browser.history_floor = settings.history_floor;
                    if (settings.version >= 3 && settings.cue_seconds >= 1 && settings.cue_seconds <= 30 && settings.guided <= 1) {
                        cue_seconds = settings.cue_seconds; guided = settings.guided != 0;
                        if (settings.version >= 4 && settings.reduced_motion <= 1) reduced_motion = settings.reduced_motion != 0;
                        // Only opaque RGB values are accepted; anything else means the system color.
                        if (settings.version >= 5 && (settings.cue_color & 0xff000000) == 0) cue_color = settings.cue_color;
                        if (settings.version >= 6 && settings.widget_width >= widget_min_width && settings.widget_height >= widget_min_height &&
                            settings.widget_width <= 8192 && settings.widget_height <= 8192) { widget_width = settings.widget_width; widget_height = settings.widget_height; }
                        if (settings.version >= 7 && settings.skip_profile_check <= 1) profile_check = settings.skip_profile_check == 0;
                    }
                }
                if (valid_shortcut(value)) shortcut = value;
            }
            CloseHandle(file);
        }
    }
    if (!settings_path.empty()) {
        auto data_path = settings_path.substr(0, settings_path.find_last_of(L'\\') + 1) + L"closed.dat";
        bool corrupt{}; tabs = hype::load_closed(data_path, browser.retention_days, corrupt);
        history_store = std::make_unique<hype::HistoryStore>(data_path);
        std::erase_if(tabs, [&](const auto& item) { return item.tab.closed <= browser.history_floor; });
        if (corrupt) history_store->clear();
        else history_store->submit(tabs);
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES}; InitCommonControlsEx(&controls);
    NONCLIENTMETRICSW metrics{sizeof(metrics)}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    ui_font = CreateFontIndirectW(&metrics.lfMessageFont);
    WNDCLASSW cls{}; cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    theme = std::make_unique<hype::Theme>();
    cls.hbrBackground = theme->background(); cls.lpfnWndProc = window_proc; cls.lpszClassName = L"HypeTabsSearch";
    RegisterClassW(&cls);
    cls.lpfnWndProc = options_proc; cls.lpszClassName = L"HypeTabsOptions"; RegisterClassW(&cls);
    cls.lpfnWndProc = about_proc; cls.lpszClassName = L"HypeTabsAbout"; RegisterClassW(&cls);
    main_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"HypeTabsSearch", L"HypeTabs — Find a tab",
        WS_POPUP, 0, 0, widget_default_width, 410, nullptr, nullptr, instance, nullptr);
    if (!main_window) { DeleteObject(ui_font); CloseHandle(singleton); return 1; }
    try { transport = std::make_unique<hype::Transport>(main_window, browser_message); }
    catch (const std::exception&) { MessageBoxW(main_window, L"The local browser connection could not start. Restart HypeTabs to retry.", L"HypeTabs", MB_OK | MB_ICONERROR); }
    cue = std::make_unique<hype::CueOverlay>(); cue->set_color(cue_color);
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated"); add_tray();
    if (!isolated_data) {
        // First run: offer sign-in startup once, then persist settings so the question is not repeated.
        if (first_run) {
            if (MessageBoxW(nullptr, L"Start HypeTabs automatically when you sign in to Windows?\nYou can change this later in Options.", L"HypeTabs",
                MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES && !startup.set(true))
                MessageBoxW(nullptr, L"The sign-in registration could not be written. You can retry in Options.", L"HypeTabs", MB_OK | MB_ICONINFORMATION);
            save_settings();
        } else if (startup.status() == hype::StartupRegistration::Status::OtherLocation) {
            // The Run entry points at another copy; only this running path is trusted to start at sign-in.
            if (MessageBoxW(nullptr, L"HypeTabs is registered to start from a different location. Update the sign-in registration to this copy?", L"HypeTabs",
                MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES && !startup.set(true))
                MessageBoxW(nullptr, L"The sign-in registration could not be updated. You can retry in Options.", L"HypeTabs", MB_OK | MB_ICONINFORMATION);
        }
    }
    if (!isolated_data && !settings_path.empty()) {
        using namespace hype::embedded;
        auto root = settings_path.substr(0, settings_path.find_last_of(L'\\'));
        if (!hype::deploy::install(root, deployed, bridge_exe, bridge_exe_size, extension_manifest, extension_manifest_size, extension_worker, extension_worker_size)) {
            deployed = {};
            MessageBoxW(nullptr, (L"HypeTabs could not unpack its Chrome bridge and extension into\n" + root + L"\\App\n\nCheck access to that folder and restart HypeTabs; tabs cannot be found until then.").c_str(), L"HypeTabs", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        if (profile_check) prompt_missing_profiles();
    }
    shortcut_registered = bind_shortcut(shortcut);
    if (!shortcut_registered) {
        MessageBoxW(nullptr, L"The search shortcut is already in use. Choose another in Options.", L"HypeTabs", MB_OK | MB_ICONINFORMATION); show_options();
    }
    SetTimer(main_window, 4, 60000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (options_window && IsDialogMessageW(options_window, &message)) {
            if (options_layout) options_layout->reveal(GetFocus());
            continue;
        }
        if (IsWindowVisible(main_window) && (message.hwnd == query_box || message.hwnd == results_box) && message.message == WM_KEYDOWN) {
            if (message.wParam == VK_RETURN) { activate_selection(); continue; }
            if (message.wParam == VK_ESCAPE) { dismiss(true); continue; }
            if (message.wParam == VK_TAB) { SetFocus(message.hwnd == query_box ? results_box : query_box); continue; }
            if (message.wParam == VK_DOWN || message.wParam == VK_UP ||
                (message.hwnd == results_box && (message.wParam == VK_HOME || message.wParam == VK_END || message.wParam == VK_PRIOR || message.wParam == VK_NEXT))) {
                auto selected = SendMessageW(results_box, LB_GETCURSEL, 0, 0);
                if (message.wParam == VK_HOME) selected = 0;
                else if (message.wParam == VK_END) selected = static_cast<LRESULT>(matches.size()) - 1;
                else if (message.wParam == VK_PRIOR || message.wParam == VK_NEXT) {
                    RECT area{}; GetClientRect(results_box, &area);
                    auto row_height = std::max<LRESULT>(1, SendMessageW(results_box, LB_GETITEMHEIGHT, 0, 0));
                    auto page = std::max<LRESULT>(1, area.bottom / row_height);
                    selected += message.wParam == VK_NEXT ? page : -page;
                } else selected += message.wParam == VK_DOWN ? 1 : -1;
                selected = std::clamp<LRESULT>(selected, 0, std::max<LRESULT>(0, static_cast<LRESULT>(matches.size()) - 1));
                SendMessageW(results_box, LB_SETCURSEL, static_cast<WPARAM>(selected), 0); continue;
            }
        }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (search_font) DeleteObject(search_font);
    DeleteObject(ui_font); CloseHandle(singleton); return 0;
}
