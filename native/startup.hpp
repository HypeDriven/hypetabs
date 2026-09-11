#pragma once
#include <windows.h>
#include <string>
#include <vector>
namespace hype {
class StartupRegistration {
    std::wstring key, value;
public:
    enum class Status { Disabled, Enabled, OtherLocation, Unavailable };
    explicit StartupRegistration(std::wstring subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", std::wstring name = L"HypeTabs")
        : key(std::move(subkey)), value(std::move(name)) {}
    static std::wstring command() {
        std::vector<wchar_t> path(32768);
        DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) return {};
        std::wstring result = L"\"" + std::wstring(path.data(), size) + L"\"";
        // Windows Run keys document a 260-character command limit.
        return result.size() <= 260 ? result : std::wstring{};
    }
    Status status() const {
        auto expected = command(); if (expected.empty()) return Status::Unavailable;
        wchar_t data[512]{}; DWORD bytes = sizeof(data);
        auto result = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), RRF_RT_REG_SZ, nullptr, data, &bytes);
        if (result == ERROR_FILE_NOT_FOUND) return Status::Disabled;
        if (result != ERROR_SUCCESS) return Status::Unavailable;
        return expected == data ? Status::Enabled : Status::OtherLocation;
    }
    bool set(bool enabled) const {
        auto current = status();
        if ((enabled && current == Status::Enabled) || (!enabled && current == Status::Disabled)) return true;
        if (current == Status::Unavailable || (!enabled && current == Status::OtherLocation)) return false;
        HKEY handle{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
        auto text = command();
        LSTATUS result = enabled ? RegSetValueExW(handle, value.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
            static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))) : RegDeleteValueW(handle, value.c_str());
        RegCloseKey(handle); return result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND);
    }
};
}
