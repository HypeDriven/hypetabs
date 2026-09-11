#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
#include <string>
#include <wrl/client.h>
#include <string_view>
#include <vector>
#include <iostream>
#include "../native/taskbar_locator.hpp"
using Microsoft::WRL::ComPtr;
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HWND target = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    if (!IsWindow(target)) return 3;
    wchar_t title[512]{}; GetWindowTextW(target, title, 512);
    if (std::wstring_view(title).find(L"HypeTabs guidance probe") == std::wstring_view::npos) {
        std::cerr << "The isolated fixture title has not loaded; no taskbar inference is valid\n"; return 8;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
    int status = 0;
    {
        std::wstring app_id;
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(target, IID_PPV_ARGS(&properties)))) {
            PROPVARIANT value{};
            if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
                app_id = value.pwszVal;
            PropVariantClear(&value);
        }
        std::cout << "Target explicit AppUserModelID present=" << !app_id.empty() << '\n';
        unsigned app_matches = 0;
        ComPtr<IUIAutomation2> automation;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return 5;
        automation->put_ConnectionTimeout(250); automation->put_TransactionTimeout(250); automation->put_AutoSetFocus(FALSE);
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(automation->get_ControlViewWalker(&walker))) return 6;
        std::vector<HWND> taskbars;
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            wchar_t name[64]{}; GetClassNameW(window, name, 64);
            if (std::wstring_view(name) == L"Shell_TrayWnd" || std::wstring_view(name) == L"Shell_SecondaryTrayWnd")
                reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&taskbars));
        unsigned visited = 0, fixture_names = 0, target_handles = 0, chrome_names = 0;
        bool complete = true; auto deadline = GetTickCount64() + 5000;
        for (HWND taskbar : taskbars) {
            DWORD pid{}; GetWindowThreadProcessId(taskbar, &pid);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            wchar_t path[32768]{}; DWORD length = 32768;
            bool explorer = process && QueryFullProcessImageNameW(process, 0, path, &length);
            if (process) CloseHandle(process);
            auto base = wcsrchr(path, L'\\');
            if (!explorer || !base || _wcsicmp(base + 1, L"explorer.exe")) { complete = false; continue; }
            ComPtr<IUIAutomationElement> root;
            if (FAILED(automation->ElementFromHandle(taskbar, &root))) { complete = false; continue; }
            std::vector<ComPtr<IUIAutomationElement>> pending{root};
            while (!pending.empty() && visited < 512 && GetTickCount64() < deadline) {
                auto element = std::move(pending.back()); pending.pop_back(); ++visited;
                CONTROLTYPEID type{}; BOOL offscreen = TRUE; UIA_HWND handle{};
                BSTR name{};
                if (FAILED(element->get_CurrentControlType(&type)) || FAILED(element->get_CurrentIsOffscreen(&offscreen)) ||
                    FAILED(element->get_CurrentName(&name))) { complete = false; continue; }
                std::wstring_view text(name ? name : L"", name ? SysStringLen(name) : 0);
                bool fixture = text.find(L"HypeTabs guidance probe") != std::wstring_view::npos;
                bool chrome = text.find(L"Chrome") != std::wstring_view::npos;
                SysFreeString(name);
                bool same_handle = SUCCEEDED(element->get_CurrentNativeWindowHandle(&handle)) && reinterpret_cast<HWND>(handle) == target;
                BSTR identifier{}; bool app_match = false;
                if (!app_id.empty() && SUCCEEDED(element->get_CurrentAutomationId(&identifier))) {
                    std::wstring_view id(identifier ? identifier : L"", identifier ? SysStringLen(identifier) : 0);
                    app_match = id == app_id || id == L"Appid: " + app_id;
                    SysFreeString(identifier);
                }
                if (!offscreen) {
                    app_matches += app_match;
                    fixture_names += fixture; target_handles += same_handle; chrome_names += chrome;
                    if (fixture || chrome || same_handle || app_match) {
                        RECT rect{}; element->get_CurrentBoundingRectangle(&rect);
                        std::cout << "Candidate: type=" << type << " fixture_name=" << fixture << " chrome_name=" << chrome
                            << " app_id_match=" << app_match << " target_hwnd=" << same_handle << " rectangle=" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << '\n';
                    }
                }
                if (type == UIA_DocumentControlTypeId) continue;
                ComPtr<IUIAutomationElement> child;
                if (FAILED(walker->GetFirstChildElement(element.Get(), &child))) { complete = false; continue; }
                while (child && pending.size() < 512 && GetTickCount64() < deadline) {
                    pending.push_back(child); ComPtr<IUIAutomationElement> next;
                    if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) { complete = false; break; }
                    child = std::move(next);
                }
                if (child) complete = false;
            }
            if (!pending.empty()) complete = false;
        }
        std::cout << "Taskbars=" << taskbars.size() << " visited=" << visited << " complete=" << complete
            << " app_id_matches=" << app_matches << " fixture_names=" << fixture_names << " target_handles=" << target_handles << " chrome_names=" << chrome_names << '\n';
        if (!complete || taskbars.empty()) status = 7;
        std::atomic<bool> stop{false};
        auto located = hype::locate_taskbar(target, stop);
        std::cout << "Conservative locator found=" << located.found << '\n';
        if (!located.found) status = 9;
        HWND duplicate = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"HypeTabs taskbar ambiguity fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ComPtr<IPropertyStore> duplicate_properties;
        if (!duplicate || FAILED(SHGetPropertyStoreForWindow(duplicate, IID_PPV_ARGS(&duplicate_properties)))) return 10;
        PROPVARIANT duplicate_id{}; duplicate_id.vt = VT_LPWSTR; duplicate_id.pwszVal = app_id.data();
        if (FAILED(duplicate_properties->SetValue(PKEY_AppUserModel_ID, duplicate_id))) return 11;
        ShowWindow(duplicate, SW_SHOWNOACTIVATE);
        auto ambiguous = hype::locate_taskbar(target, stop);
        PROPVARIANT empty{}; duplicate_properties->SetValue(PKEY_AppUserModel_ID, empty);
        DestroyWindow(duplicate);
        if (ambiguous.found) status = 12;
        stop = true;
        if (hype::locate_taskbar(target, stop).found) status = 13;
        std::cout << "Duplicate group rejected=" << !ambiguous.found << '\n';
    }
    CoUninitialize(); return status;
}
