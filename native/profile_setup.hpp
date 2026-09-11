#pragma once
// Startup check that every Chrome profile has the HypeTabs extension loaded.
// Reads only Chrome's profile list (Local State: profile.info_cache names) and each profile's
// extension install records (extensions.settings in Preferences / Secure Preferences); no
// browsing data is opened. Profiles are launched only through CreateProcess with validated
// directory names, never through a shell command line.
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>
namespace hype::profiles {
constexpr size_t max_prefs_bytes = 16u << 20;
// Minimal tolerant JSON tree: only objects and strings are retained; everything else is skipped.
struct Node {
    std::map<std::string, Node, std::less<>> object; std::string text; bool is_object = false, is_string = false;
    const Node* get(std::string_view key) const { auto it = object.find(key); return it == object.end() ? nullptr : &it->second; }
    const Node* path(std::initializer_list<std::string_view> keys) const {
        const Node* node = this;
        for (auto key : keys) { if (!node || !node->is_object) return nullptr; node = node->get(key); }
        return node;
    }
};
class Parser {
    std::string_view data; size_t pos = 0; int depth = 0;
    bool more() const { return pos < data.size(); }
    void space() { while (more() && (data[pos] == ' ' || data[pos] == '\r' || data[pos] == '\n' || data[pos] == '\t')) ++pos; }
    bool take(char c) { space(); if (!more() || data[pos] != c) return false; ++pos; return true; }
    static void append(std::string& out, uint32_t c) {
        if (c <= 127) out += static_cast<char>(c);
        else if (c <= 2047) { out += static_cast<char>(0xc0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
        else if (c <= 65535) { out += static_cast<char>(0xe0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
        else { out += static_cast<char>(0xf0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 63)); out += static_cast<char>(0x80 | ((c >> 6) & 63)); out += static_cast<char>(0x80 | (c & 63)); }
    }
    bool hex(uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (!more()) return false; char c = data[pos++]; unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0'; else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10; else return false;
            out = out * 16 + digit;
        }
        return true;
    }
    bool string(std::string& out) {
        if (!take('"')) return false;
        for (;;) {
            if (!more()) return false; char c = data[pos++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (!more()) return false; c = data[pos++];
            switch (c) {
            case '"': case '\\': case '/': out += c; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                uint32_t code; if (!hex(code)) return false;
                if (code >= 0xd800 && code <= 0xdbff && data.substr(pos, 2) == "\\u") { pos += 2; uint32_t low; if (!hex(low)) return false; code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00); }
                append(out, code); break;
            }
            default: return false;
            }
        }
    }
    bool value(Node& out) {
        space(); if (!more()) return false;
        char c = data[pos];
        if (c == '"') { out.is_string = true; return string(out.text); }
        if (c == '{') {
            if (++depth > 64) return false;
            ++pos; out.is_object = true;
            if (take('}')) { --depth; return true; }
            for (;;) {
                std::string key; if (!string(key) || !take(':')) return false;
                Node child; if (!value(child)) return false;
                out.object.insert_or_assign(std::move(key), std::move(child));
                if (take(',')) continue;
                if (take('}')) break;
                return false;
            }
            --depth; return true;
        }
        if (c == '[') {
            if (++depth > 64) return false;
            ++pos;
            if (take(']')) { --depth; return true; }
            for (;;) {
                Node child; if (!value(child)) return false;
                if (take(',')) continue;
                if (take(']')) break;
                return false;
            }
            --depth; return true;
        }
        // Scalars (numbers, true, false, null) are consumed without interpretation.
        size_t start = pos;
        while (more() && (std::isalnum(static_cast<unsigned char>(data[pos])) || data[pos] == '-' || data[pos] == '+' || data[pos] == '.')) ++pos;
        return pos > start;
    }
public:
    explicit Parser(std::string_view input) : data(input) {}
    bool parse(Node& out) { if (data.empty() || !value(out)) return false; space(); return !more(); }
};
inline bool read_file(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(max_prefs_bytes);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart)); DWORD read{};
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file); return ok;
}
inline std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}
// Chrome profile directory names as they appear in Local State ("Default", "Profile 3").
// Anything else is refused so the name can never inject a switch or path element.
inline bool valid_directory(std::wstring_view name) {
    if (name.empty() || name.size() > 64 || name.front() == L'.' || name.back() == L' ' || name.front() == L' ') return false;
    return std::all_of(name.begin(), name.end(), [](wchar_t c) { return std::iswalnum(c) || c == L' ' || c == L'_' || c == L'-' || c == L'.'; });
}
inline std::wstring normalize_path(std::wstring path) {
    for (auto& c : path) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(std::towlower(c)); }
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}
// True when a profile's extension records name the HypeTabs extension by ID or by unpacked folder.
inline bool records_extension(const Node& prefs, std::string_view extension_id, const std::wstring& normalized_folder) {
    auto settings = prefs.path({"extensions", "settings"});
    if (!settings || !settings->is_object) return false;
    for (const auto& [id, entry] : settings->object) {
        if (!extension_id.empty() && id == extension_id) return true;
        if (auto path = entry.path({"path"}); path && path->is_string && !normalized_folder.empty() && normalize_path(widen(path->text)) == normalized_folder) return true;
    }
    return false;
}
struct ChromeProfile { std::wstring directory, name; bool integrated = false; };
inline std::wstring default_user_data() {
    PWSTR local{}; std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) { result = std::wstring(local) + L"\\Google\\Chrome\\User Data"; CoTaskMemFree(local); }
    return result;
}
// Lists Chrome's profiles and whether each has the extension installed. Returns an empty list when
// Chrome's profile list is missing or unreadable, which callers treat as "nothing to prompt".
inline std::vector<ChromeProfile> scan(const std::wstring& user_data, const std::wstring& extension_folder, std::string_view extension_id) {
    std::vector<ChromeProfile> result;
    std::string raw; Node state;
    if (!read_file(user_data + L"\\Local State", raw) || !Parser(raw).parse(state)) return result;
    auto cache = state.path({"profile", "info_cache"});
    if (!cache || !cache->is_object) return result;
    auto folder = normalize_path(extension_folder);
    for (const auto& [directory, info] : cache->object) {
        ChromeProfile profile; profile.directory = widen(directory);
        if (!valid_directory(profile.directory) || profile.directory == L"System Profile" || profile.directory == L"Guest Profile") continue;
        if (auto name = info.path({"name"}); name && name->is_string) profile.name = widen(name->text);
        std::erase_if(profile.name, [](wchar_t c) { return c < 32 || c == 127; });
        if (profile.name.size() > 64) profile.name.resize(64);
        if (profile.name.empty()) profile.name = profile.directory;
        for (auto file : {L"\\Secure Preferences", L"\\Preferences"}) {
            std::string text; Node prefs;
            if (read_file(user_data + L"\\" + profile.directory + file, text) && Parser(text).parse(prefs) && records_extension(prefs, extension_id, folder)) { profile.integrated = true; break; }
        }
        result.push_back(std::move(profile));
    }
    return result;
}
// The registered extension origin ("chrome-extension://<id>/") written by tools/install.ps1; empty until registered.
inline std::string registered_extension_id() {
    wchar_t origin[128]{}; DWORD bytes = sizeof(origin);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\HypeTabs", L"ExtensionOrigin", RRF_RT_REG_SZ, nullptr, origin, &bytes) != ERROR_SUCCESS) return {};
    std::wstring_view text(origin); constexpr std::wstring_view prefix = L"chrome-extension://";
    if (!text.starts_with(prefix) || !text.ends_with(L'/')) return {};
    text = text.substr(prefix.size(), text.size() - prefix.size() - 1);
    if (text.size() != 32 || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'a' && c <= L'p'; })) return {};
    std::string id; for (wchar_t c : text) id += static_cast<char>(c); return id;
}
inline std::wstring chrome_executable() {
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t path[MAX_PATH]{}; DWORD bytes = sizeof(path);
        if (RegGetValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\chrome.exe", nullptr, RRF_RT_REG_SZ, nullptr, path, &bytes) == ERROR_SUCCESS && path[0] &&
            GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}
// Brings one profile's window forward (opening one if needed) via structured process creation, no
// shell involved. Chrome drops chrome:// URLs given on the command line, so the user is told to type
// chrome://extensions there; the profile switch itself is honored by the running browser.
inline bool open_profile(const std::wstring& chrome, const std::wstring& directory) {
    if (chrome.empty() || !valid_directory(directory)) return false;
    std::wstring command = L"\"" + chrome + L"\" --profile-directory=\"" + directory + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(chrome.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}
}
