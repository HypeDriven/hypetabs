#pragma once
// Unpacks the embedded bridge and extension into %LOCALAPPDATA%\HypeTabs\App and registers the
// native messaging host for the unpacked extension's ID, so a lone HypeTabs.exe is self-installing.
// Chrome derives an unpacked extension's ID from its folder path exactly as given (only the drive
// letter is upper-cased), hashed as UTF-16LE bytes with SHA-256, first 128 bits mapped to a-p.
// Verified against Chrome for Testing 153 (tests/profile_setup_tests.cpp), so no user step is needed.
#include <windows.h>
#include <wincrypt.h>
#include <string>
#include <string_view>
#include <vector>
namespace hype::deploy {
inline std::string unpacked_extension_id(std::wstring path) {
    if (path.size() >= 2 && path[0] >= L'a' && path[0] <= L'z' && path[1] == L':') path[0] = static_cast<wchar_t>(path[0] - L'a' + L'A');
    HCRYPTPROV provider{}; HCRYPTHASH hash{}; std::string id;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return id;
    if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        BYTE digest[32]{}; DWORD size = sizeof(digest);
        if (CryptHashData(hash, reinterpret_cast<const BYTE*>(path.data()), static_cast<DWORD>(path.size() * sizeof(wchar_t)), 0) && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == 32)
            for (int i = 0; i < 16; ++i) { id += static_cast<char>('a' + (digest[i] >> 4)); id += static_cast<char>('a' + (digest[i] & 15)); }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0); return id;
}
inline bool same_content(const std::wstring& path, const unsigned char* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{}; bool same = GetFileSizeEx(file, &length) && length.QuadPart == static_cast<LONGLONG>(size);
    if (same) {
        std::vector<unsigned char> existing(size); DWORD read{};
        same = ReadFile(file, existing.data(), static_cast<DWORD>(size), &read, nullptr) && read == size && std::equal(existing.begin(), existing.end(), data);
    }
    CloseHandle(file); return same;
}
// Writes through a temporary file and atomic replace; an identical existing file is left untouched
// (a bridge that Chrome is currently running stays valid because its old image is replaced, not truncated).
inline bool write_file(const std::wstring& path, const unsigned char* data, size_t size) {
    if (same_content(path, data, size)) return true;
    auto temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{}; bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
inline std::string narrow_utf8(const std::wstring& text) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}
inline std::string json_escape(const std::string& text) {
    std::string out;
    for (unsigned char c : text) { if (c == '"' || c == '\\') out += '\\'; if (c < 32) { out += ' '; continue; } out += static_cast<char>(c); }
    return out;
}
inline bool set_string(HKEY root, const wchar_t* key, const wchar_t* name, const std::wstring& value) {
    HKEY handle{};
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
    auto result = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle); return result == ERROR_SUCCESS;
}
struct Layout { std::wstring app, extension; std::string extension_id; };
// Unpacks under `root` (the HypeTabs data directory) and registers the bridge for the resulting
// extension ID. Returns false when files or registrations could not be written.
inline bool install(const std::wstring& root, Layout& layout,
    const unsigned char* bridge, size_t bridge_size, const unsigned char* manifest, size_t manifest_size, const unsigned char* worker, size_t worker_size) {
    layout.app = root + L"\\App"; layout.extension = layout.app + L"\\extension";
    // Refuse redirected directories: a junction at the install root could send files elsewhere.
    for (auto dir : {root, layout.app, layout.extension}) {
        CreateDirectoryW(dir.c_str(), nullptr);
        DWORD attributes = GetFileAttributesW(dir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    if (!write_file(layout.app + L"\\HypeTabs.Bridge.exe", bridge, bridge_size) || !write_file(layout.extension + L"\\manifest.json", manifest, manifest_size) ||
        !write_file(layout.extension + L"\\worker.js", worker, worker_size)) return false;
    layout.extension_id = unpacked_extension_id(layout.extension);
    if (layout.extension_id.empty()) return false;
    std::wstring origin = L"chrome-extension://"; for (char c : layout.extension_id) origin += static_cast<wchar_t>(c); origin += L'/';
    std::string host = "{\"name\":\"com.hypetabs.bridge\",\"description\":\"HypeTabs local browser bridge\",\"path\":\"" + json_escape(narrow_utf8(layout.app + L"\\HypeTabs.Bridge.exe")) +
        "\",\"type\":\"stdio\",\"allowed_origins\":[\"" + json_escape(narrow_utf8(origin)) + "\"]}";
    auto host_path = layout.app + L"\\native-host.json";
    if (!write_file(host_path, reinterpret_cast<const unsigned char*>(host.data()), host.size())) return false;
    return set_string(HKEY_CURRENT_USER, L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.hypetabs.bridge", nullptr, host_path) &&
        set_string(HKEY_CURRENT_USER, L"Software\\HypeTabs", L"ExtensionOrigin", origin);
}
}
