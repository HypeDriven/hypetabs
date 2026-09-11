#pragma once
#include <windows.h>
#include <objbase.h>
#include <oleacc.h>
#include <wrl/client.h>
namespace hype {
// SDK values (oleacc.h, UIAutomationCoreApi.h); defined here so no translation
// unit needs initguid.h ordering.
inline constexpr GUID accprop_services_clsid{0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
inline constexpr GUID accprop_name{0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};
inline constexpr GUID uia_name_property{0xc3a6921b, 0x4a99, 0x44f1, {0xbc, 0xa6, 0x61, 0x18, 0x70, 0x52, 0xc4, 0x31}};
inline constexpr GUID live_setting_property{0xc12bcd8e, 0x2a8e, 0x4950, {0x8a, 0xe7, 0x36, 0x25, 0x11, 0x1d, 0x58, 0xeb}};
// Screen-reader names for controls without a preceding static label, through
// the Windows accessibility property service. Missing COM support degrades to
// the default proxy behavior; nothing here affects sighted interaction.
class AccessibleNames {
    Microsoft::WRL::ComPtr<IAccPropServices> services;
    bool initialized_com = false;
public:
    AccessibleNames() {
        initialized_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        CoCreateInstance(accprop_services_clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&services));
    }
    ~AccessibleNames() { services.Reset(); if (initialized_com) CoUninitialize(); }
    AccessibleNames(const AccessibleNames&) = delete; AccessibleNames& operator=(const AccessibleNames&) = delete;
    bool available() const { return services != nullptr; }
    // Both the MSAA name and the UI Automation Name property, so legacy and
    // UIA-based assistive technology read the same label.
    bool name(HWND window, const wchar_t* text) const {
        if (!services || !window || !text) return false;
        bool msaa = SUCCEEDED(services->SetHwndPropStr(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, accprop_name, text));
        bool uia = SUCCEEDED(services->SetHwndPropStr(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, uia_name_property, text));
        return msaa && uia;
    }
    // Polite live region: status changes are announced without interrupting.
    bool live_status(HWND window) const {
        if (!services || !window) return false;
        VARIANT value{}; value.vt = VT_I4; value.lVal = 1;
        return SUCCEEDED(services->SetHwndProp(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, live_setting_property, value));
    }
    void clear(HWND window) const {
        if (!services || !window) return;
        MSAAPROPID properties[]{accprop_name, uia_name_property, live_setting_property};
        services->ClearHwndProps(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF, properties, 3);
    }
};
}
