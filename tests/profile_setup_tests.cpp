#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../native/profile_setup.hpp"
#include <iostream>
#include <cstdlib>
#include <fstream>
void check(bool ok, const char* name) { if (!ok) { std::cerr << name << '\n'; std::exit(1); } }
void write(const std::wstring& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
int main() {
    using namespace hype::profiles;
    Node node;
    check(Parser(R"({"a":{"b":"x\u00e9\ud83d\ude00","c":[1,2,{"d":null}],"e":true,"f":-1.5e3}})").parse(node), "parse nested document");
    check(node.path({"a", "b"})->text == "x\xc3\xa9\xf0\x9f\x98\x80", "decode escapes and surrogate pairs");
    check(node.path({"a", "c"}) && !node.path({"a", "c"})->is_object && !node.path({"a", "d"}), "arrays and scalars are skipped");
    check(!Parser("{\"a\":}").parse(node) && !Parser("{\"a\":1} x").parse(node) && !Parser("").parse(node), "reject malformed documents");
    check(valid_directory(L"Default") && valid_directory(L"Profile 12") && !valid_directory(L"..\\x") && !valid_directory(L"a\"b") && !valid_directory(L"--flag=x"), "directory names");
    check(!valid_directory(L"a b\\c") && !valid_directory(L"") && !valid_directory(L".hidden"), "reject path separators, empty, dot-leading names");

    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    std::wstring root = std::wstring(temp) + L"HypeTabs-profile-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    for (auto dir : {L"Default", L"Profile 1", L"Profile 2", L"System Profile"}) CreateDirectoryW((root + L"\\" + dir).c_str(), nullptr);
    write(root + L"\\Local State", R"({"profile":{"info_cache":{"Default":{"name":"Work"},"Profile 1":{"name":"Home\u0001"},"Profile 2":{},"System Profile":{"name":"System"},"Bad/Name":{"name":"x"}},"last_used":"Default"}})");
    const std::string id = "abcdefghijklmnopabcdefghijklmnop";
    write(root + L"\\Default\\Secure Preferences", "{\"extensions\":{\"settings\":{\"" + id + "\":{\"path\":\"" + id + "\\\\1.0_0\",\"state\":1}}}}");
    write(root + L"\\Profile 1\\Preferences", R"({"extensions":{"settings":{"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz":{"path":"C:\\Users\\Me\\AppData\\Local\\HypeTabs\\App\\extension\\","location":4}}}})");
    write(root + L"\\Profile 2\\Preferences", R"({"extensions":{"settings":{"other":{"path":"C:\\elsewhere"}}}})");
    auto profiles = scan(root, L"c:/users/me/appdata/local/hypetabs/app/EXTENSION", id);
    check(profiles.size() == 3, "system profile and invalid directory names are excluded");
    check(profiles[0].directory == L"Default" && profiles[0].name == L"Work" && profiles[0].integrated, "registered ID found in Secure Preferences");
    check(profiles[1].directory == L"Profile 1" && profiles[1].name == L"Home" && profiles[1].integrated, "unpacked folder matched case-insensitively with control characters stripped from the name");
    check(profiles[2].directory == L"Profile 2" && profiles[2].name == L"Profile 2" && !profiles[2].integrated, "unrelated extension does not count; name falls back to the directory");
    check(scan(root, L"", "").size() == 3 && !scan(root, L"", "")[0].integrated, "without registration or folder nothing is integrated");
    write(root + L"\\Local State", "{\"profile\":{\"info_cache\":[]}}");
    check(scan(root, L"", id).empty(), "non-object info_cache yields no profiles");
    check(scan(root + L"\\missing", L"", id).empty(), "missing user data yields no profiles");
    for (auto file : {L"\\Local State", L"\\Default\\Secure Preferences", L"\\Profile 1\\Preferences", L"\\Profile 2\\Preferences"}) DeleteFileW((root + file).c_str());
    for (auto dir : {L"Default", L"Profile 1", L"Profile 2", L"System Profile"}) RemoveDirectoryW((root + L"\\" + dir).c_str());
    check(RemoveDirectoryW(root.c_str()) != FALSE, "temporary directory cleanup");
    check(!open_extensions_page(L"C:\\nowhere\\chrome.exe", L"Default") && !open_extensions_page(L"", L"Default"), "missing browser is reported, not launched");
    std::cout << "Profile setup checks passed against a synthetic Chrome user data directory\n";
}
