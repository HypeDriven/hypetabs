#pragma once
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
namespace hype {
using Microsoft::WRL::ComPtr;
struct LocatedTab { uint64_t generation{}; HWND window{}; RECT rectangle{}; bool found = false; };
inline bool chrome_process(DWORD id) {
    if (!id) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id); if (!process) return false;
    wchar_t path[32768]{}; DWORD size = 32768;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE; CloseHandle(process);
    if (!ok) return false;
    const wchar_t* base = wcsrchr(path, L'\\');
    return base && _wcsicmp(base + 1, L"chrome.exe") == 0;
}
inline bool chrome_window(HWND window) {
    wchar_t cls[64]{}; GetClassNameW(window, cls, 64);
    if (std::wstring_view(cls) != L"Chrome_WidgetWin_1") return false;
    DWORD id{}; GetWindowThreadProcessId(window, &id);
    return chrome_process(id);
}
// Chrome's accessible tab name is the title followed by " - " and status text
// such as "Pinned" or "Part of group <name>"; accept exactly that shape.
inline bool tab_name_matches(std::wstring_view name, std::wstring_view title) {
    if (title.empty() || !name.starts_with(title)) return false;
    return name.size() == title.size() || name.substr(title.size(), 3) == L" - ";
}
inline LocatedTab locate_tab(HWND window, const std::wstring& title, uint64_t generation, const std::atomic<bool>& stop) {
    LocatedTab result{generation, window};
    if (stop || !chrome_window(window) || GetForegroundWindow() != window || title.empty()) return result;
    ComPtr<IUIAutomation2> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return result;
    automation->put_ConnectionTimeout(250); automation->put_TransactionTimeout(250); automation->put_AutoSetFocus(FALSE);
    ComPtr<IUIAutomationElement> root; ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->ElementFromHandle(window, &root)) || FAILED(automation->get_ControlViewWalker(&walker))) return result;
    std::vector<ComPtr<IUIAutomationElement>> pending; pending.push_back(root);
    unsigned visited = 0, found = 0; bool complete = true; ULONGLONG deadline = GetTickCount64() + 1500;
    while (!pending.empty() && !stop && ++visited <= 512 && GetTickCount64() < deadline) {
        auto element = std::move(pending.back()); pending.pop_back();
        CONTROLTYPEID type{};
        if (FAILED(element->get_CurrentControlType(&type))) { complete = false; continue; }
        // Never traverse document content: page-authored ARIA tabs must not be
        // mistaken for Chrome's own tab headers.
        if (type == UIA_DocumentControlTypeId) continue;
        if (type == UIA_TabItemControlTypeId) {
            BOOL offscreen = TRUE; element->get_CurrentIsOffscreen(&offscreen);
            ComPtr<IUIAutomationSelectionItemPattern> selection;
            BOOL selected = FALSE;
            if (!offscreen && SUCCEEDED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection)))) selection->get_CurrentIsSelected(&selected);
            if (selected) {
                BSTR name{};
                if (SUCCEEDED(element->get_CurrentName(&name))) {
                    bool matches = name && tab_name_matches(std::wstring_view(name, SysStringLen(name)), title); SysFreeString(name);
                    RECT rectangle{};
                    if (matches && SUCCEEDED(element->get_CurrentBoundingRectangle(&rectangle)) && rectangle.right > rectangle.left && rectangle.bottom > rectangle.top) {
                        result.rectangle = rectangle; ++found;
                    }
                }
            }
            continue;
        }
        ComPtr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(element.Get(), &child))) {
            while (child && pending.size() < 512 && !stop && GetTickCount64() < deadline) {
                pending.push_back(child); ComPtr<IUIAutomationElement> next;
                if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) { complete = false; break; }
                child = std::move(next);
            }
            if (child) complete = false;
        } else complete = false;
    }
    result.found = complete && visited <= 512 && GetTickCount64() < deadline && pending.empty() && !stop && found == 1 && GetForegroundWindow() == window && chrome_window(window);
    return result;
}
class TabLocator {
    struct Request { HWND window; std::wstring title; uint64_t generation; };
    HWND target; UINT message;
    std::mutex lock; std::condition_variable wake;
    std::optional<Request> pending;
    std::optional<LocatedTab> completed;
    std::atomic<bool> stopping{false};
    std::thread worker;
    void run() {
        auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) return;
        for (;;) {
            Request request;
            {
                std::unique_lock guard(lock); wake.wait(guard, [&] { return stopping || pending.has_value(); });
                if (stopping) break;
                request = std::move(*pending); pending.reset();
            }
            auto result = locate_tab(request.window, request.title, request.generation, stopping);
            { std::lock_guard guard(lock); if (stopping) break; completed = result; }
            PostMessageW(target, message, 0, 0);
        }
        CoUninitialize();
    }
public:
    TabLocator(HWND window, UINT notification) : target(window), message(notification), worker([this] { run(); }) {}
    ~TabLocator() { stopping = true; wake.notify_one(); worker.join(); }
    void request(HWND window, std::wstring title, uint64_t generation) {
        { std::lock_guard guard(lock); pending = Request{window, std::move(title), generation}; } wake.notify_one();
    }
    std::optional<LocatedTab> take() { std::lock_guard guard(lock); auto result = completed; completed.reset(); return result; }
};
}
