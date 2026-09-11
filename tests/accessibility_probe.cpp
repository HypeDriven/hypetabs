#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <iostream>
#include <string>
#include <vector>
// Verifies the search overlay's UI Automation exposure from another process:
// control types, screen-reader names, and the polite live status region.
using Microsoft::WRL::ComPtr;
struct Found { std::wstring name; CONTROLTYPEID type{}; LONG live = -1; bool present = false; };
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(argv[1], nullptr, 10)));
    if (!IsWindow(window)) { std::cerr << "search window handle is invalid\n"; return 2; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    int result = 1;
    {
        ComPtr<IUIAutomation> automation; ComPtr<IUIAutomationElement> root; ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) ||
            FAILED(automation->ElementFromHandle(window, &root)) || FAILED(automation->get_ControlViewWalker(&walker))) { CoUninitialize(); return 2; }
        Found edit, list, status; unsigned visited = 0;
        std::vector<ComPtr<IUIAutomationElement>> pending{root};
        while (!pending.empty() && ++visited <= 256) {
            auto element = std::move(pending.back()); pending.pop_back();
            CONTROLTYPEID type{}; element->get_CurrentControlType(&type);
            BSTR cls{}; element->get_CurrentClassName(&cls); std::wstring_view class_name = cls ? cls : L"";
            Found* target = class_name == L"Edit" ? &edit : class_name == L"ListBox" ? &list : class_name == L"Static" ? &status : nullptr;
            if (target && !target->present) {
                BSTR name{}; element->get_CurrentName(&name); target->name = name ? name : L""; SysFreeString(name);
                VARIANT live{}; if (SUCCEEDED(element->GetCurrentPropertyValue(UIA_LiveSettingPropertyId, &live)) && live.vt == VT_I4) target->live = live.lVal;
                VariantClear(&live); target->type = type; target->present = true;
            }
            SysFreeString(cls);
            ComPtr<IUIAutomationElement> child; walker->GetFirstChildElement(element.Get(), &child);
            while (child && pending.size() < 256) { pending.push_back(child); ComPtr<IUIAutomationElement> next; walker->GetNextSiblingElement(child.Get(), &next); child = std::move(next); }
        }
        bool ok = edit.present && edit.type == UIA_EditControlTypeId && edit.name == L"Find a tab by title, site, or profile" &&
            list.present && list.type == UIA_ListControlTypeId && list.name == L"Matching tabs" &&
            status.present && status.type == UIA_TextControlTypeId && status.name == L"Search status" && status.live == 1;
        if (ok) { std::cout << "PASS: UI Automation exposes named Edit/List controls and a polite live status region for the search overlay\n"; result = 0; }
        else {
            std::wcerr << L"edit present=" << edit.present << L" type=" << edit.type << L" name=[" << edit.name << L"]\n"
                << L"list present=" << list.present << L" type=" << list.type << L" name=[" << list.name << L"]\n"
                << L"status present=" << status.present << L" type=" << status.type << L" name=[" << status.name << L"] live=" << status.live << L'\n';
        }
    }
    CoUninitialize(); return result;
}
