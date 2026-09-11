#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include "../native/startup.hpp"
#include <iostream>
#include <cstdlib>
void check(bool ok, const char* name) { if (!ok) { std::cerr << name << '\n'; std::exit(1); } }
int main() {
    GUID guid{}; CoCreateGuid(&guid); wchar_t id[40]{}; StringFromGUID2(guid, id, 40);
    std::wstring key = L"Software\\HypeTabs.Tests\\Startup-" + std::wstring(id);
    hype::StartupRegistration registration(key);
    using Status = hype::StartupRegistration::Status;
    check(registration.status() == Status::Disabled, "default registration should be disabled");
    check(registration.set(true) && registration.status() == Status::Enabled, "enable registration");
    auto command = hype::StartupRegistration::command();
    check(!command.empty() && command.front() == L'"' && command.back() == L'"', "executable path must be quoted");
    check(registration.set(true), "enabling twice");
    check(registration.set(false) && registration.status() == Status::Disabled, "disable registration");
    HKEY handle{}; RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &handle);
    const wchar_t other[] = L"\"C:\\Another installation\\HypeTabs.exe\"";
    RegSetValueExW(handle, L"HypeTabs", 0, REG_SZ, reinterpret_cast<const BYTE*>(other), sizeof(other)); RegCloseKey(handle);
    check(registration.status() == Status::OtherLocation, "recognize another installation");
    check(!registration.set(false) && registration.status() == Status::OtherLocation, "do not remove another installation's registration");
    check(RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS, "test key cleanup");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\HypeTabs.Tests");
    std::cout << "Startup registration checks passed using an isolated test registry key\n";
}
