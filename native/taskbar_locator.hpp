#pragma once
#include "tab_locator.hpp"
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
namespace hype {
inline std::wstring window_app_id(HWND window) {
    ComPtr<IPropertyStore> properties; std::wstring result;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties)))) {
        PROPVARIANT value{};
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            result = value.pwszVal;
        PropVariantClear(&value);
    }
    return result;
}
struct LocatedTaskbar { HWND window{}, taskbar{}; RECT rectangle{}; bool found = false; };
inline LocatedTaskbar locate_taskbar(HWND target, const std::atomic<bool>& stop) {
    LocatedTaskbar result{target};
    if (stop || !IsWindowVisible(target) || !chrome_window(target)) return result;
    auto app_id = window_app_id(target); if (app_id.empty() || app_id.size() > 256) return result;
    auto deadline = GetTickCount64() + 1500;
    struct Windows {
        const std::wstring& id; const std::atomic<bool>& stop; ULONGLONG deadline;
        unsigned count = 0, visited = 0; bool complete = true;
        std::vector<HWND> taskbars;
    } windows{app_id, stop, deadline};
    bool enumerated = EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Windows*>(data);
        if (state.stop || ++state.visited > 512 || GetTickCount64() >= state.deadline) { state.complete = false; return FALSE; }
        if (!IsWindowVisible(window)) return TRUE;
        wchar_t cls[64]{}; GetClassNameW(window, cls, 64);
        if (std::wstring_view(cls) == L"Shell_TrayWnd" || std::wstring_view(cls) == L"Shell_SecondaryTrayWnd") state.taskbars.push_back(window);
        if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        auto id = window_app_id(window);
        // Missing Chrome identities prevent a reliable count of group members.
        if (id.empty() && chrome_window(window)) state.complete = false;
        if (id == state.id) ++state.count;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows)) != FALSE;
    if (!enumerated || !windows.complete || windows.count != 1) return result;
    ComPtr<IUIAutomation2> automation; ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return result;
    automation->put_ConnectionTimeout(250); automation->put_TransactionTimeout(250); automation->put_AutoSetFocus(FALSE);
    if (FAILED(automation->get_ControlViewWalker(&walker))) return result;
    HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    unsigned visited = 0, found = 0; bool complete = true;
    for (HWND taskbar : windows.taskbars) {
        if (MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST) != monitor) continue;
        DWORD pid{}; GetWindowThreadProcessId(taskbar, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        wchar_t path[32768]{}; DWORD length = 32768;
        bool explorer = process && QueryFullProcessImageNameW(process, 0, path, &length);
        if (process) CloseHandle(process);
        auto base = wcsrchr(path, L'\\');
        if (!explorer || !base || _wcsicmp(base + 1, L"explorer.exe")) return result;
        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(taskbar, &root))) return result;
        std::vector<ComPtr<IUIAutomationElement>> pending{root};
        while (!pending.empty() && !stop && visited++ < 512 && GetTickCount64() < deadline) {
            auto element = std::move(pending.back()); pending.pop_back();
            CONTROLTYPEID type{};
            if (FAILED(element->get_CurrentControlType(&type))) { complete = false; break; }
            if (type == UIA_ButtonControlTypeId) {
                BSTR identifier{};
                if (FAILED(element->get_CurrentAutomationId(&identifier))) { complete = false; break; }
                std::wstring_view id(identifier ? identifier : L"", identifier ? SysStringLen(identifier) : 0);
                // Observed Explorer provider shape, never parsed as an HWND.
                // Unknown provider formats deliberately use direct activation.
                bool matches = id == app_id || id == L"Appid: " + app_id;
                SysFreeString(identifier);
                BOOL offscreen = TRUE; RECT rectangle{}, bar{};
                if (matches && SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen &&
                    SUCCEEDED(element->get_CurrentBoundingRectangle(&rectangle)) && GetWindowRect(taskbar, &bar) &&
                    rectangle.left >= bar.left && rectangle.top >= bar.top && rectangle.right <= bar.right && rectangle.bottom <= bar.bottom &&
                    rectangle.right > rectangle.left && rectangle.bottom > rectangle.top) {
                    ++found; result.rectangle = rectangle; result.taskbar = taskbar;
                }
            }
            if (type == UIA_DocumentControlTypeId) continue;
            ComPtr<IUIAutomationElement> child;
            if (FAILED(walker->GetFirstChildElement(element.Get(), &child))) { complete = false; break; }
            while (child && pending.size() < 512 && !stop && GetTickCount64() < deadline) {
                pending.push_back(child); ComPtr<IUIAutomationElement> next;
                if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) { complete = false; break; }
                child = std::move(next);
            }
            if (child) complete = false;
        }
        if (!pending.empty()) complete = false;
    }
    result.found = complete && !stop && found == 1 && GetTickCount64() < deadline &&
        chrome_window(target) && window_app_id(target) == app_id;
    return result;
}
}
