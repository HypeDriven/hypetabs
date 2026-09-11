#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <condition_variable>
#include <optional>
#include "transport.hpp"
#include "browser_state.hpp"
namespace hype {
inline std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw wire::Error();
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr); return result;
}
inline std::string saved_record(const Tab& tab) {
    return "{\"v\":1,\"profile\":" + wire::quote(narrow(tab.profile)) + ",\"label\":" + wire::quote(narrow(tab.label)) +
        ",\"title\":" + wire::quote(narrow(tab.title)) + ",\"url\":" + wire::quote(narrow(tab.url)) +
        ",\"session\":" + wire::quote(narrow(tab.session)) + ",\"record\":" + wire::quote(narrow(tab.record)) +
        ",\"closed\":" + std::to_string(tab.closed) + ",\"window\":" + std::to_string(tab.window) +
        ",\"observed\":" + (tab.observed ? "true}" : "false}");
}
inline bool persist_closed(const std::wstring& path, const std::vector<Tab>& tabs) {
    if (tabs.empty()) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    std::string plain;
    if (tabs.size() > 1000) return false;
    for (const auto& tab : tabs) { plain += saved_record(tab); plain += '\n'; }
    if (plain.size() > 16 * 1024 * 1024) return false;
    DATA_BLOB input{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())}, encrypted{};
    if (!CryptProtectData(&input, L"HypeTabs recently closed tabs", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) return false;
    auto acl = L"D:P(A;;GA;;;" + user_sid() + L")";
    PSECURITY_DESCRIPTOR descriptor{};
    bool ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
    auto temporary = path + L".pending";
    if (ok) {
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        {
            Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            DWORD written{};
            ok = file.valid() && WriteFile(file, encrypted.pbData, encrypted.cbData, &written, nullptr) && written == encrypted.cbData && FlushFileBuffers(file);
        }
        if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) DeleteFileW(temporary.c_str());
    if (descriptor) LocalFree(descriptor);
    LocalFree(encrypted.pbData); return ok;
}
inline std::vector<IndexedTab> load_closed(const std::wstring& path, int days, bool& corrupt) {
    corrupt = false; std::vector<IndexedTab> result;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) { corrupt = GetLastError() != ERROR_FILE_NOT_FOUND; return result; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 1 || size.QuadPart > 17 * 1024 * 1024) { corrupt = true; return {}; }
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart)); DWORD read{};
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) { corrupt = true; return {}; }
    DATA_BLOB input{read, bytes.data()}, plain{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) { corrupt = true; return {}; }
    try {
        std::string_view content(reinterpret_cast<char*>(plain.pbData), plain.cbData);
        if (content.size() > 16 * 1024 * 1024) throw wire::Error();
        std::set<std::wstring> records;
        while (!content.empty()) {
            size_t end = content.find('\n'); if (end == std::string_view::npos || result.size() >= 1000) throw wire::Error();
            auto object = wire::Parser(content.substr(0, end)).parse(); content.remove_prefix(end + 1);
            wire::get_int(object, "v", 1, 1); if (object.size() != 10) throw wire::Error();
            Tab tab;
            tab.profile = wide(wire::get_string(object, "profile", 36));
            tab.label = wide(wire::get_string(object, "label", 128));
            tab.title = wide(wire::get_string(object, "title", 4096, true));
            tab.url = wide(wire::get_string(object, "url", 8192));
            tab.session = wide(wire::get_string(object, "session", 256, true));
            tab.record = wide(wire::get_string(object, "record", 38));
            tab.closed = wire::get_int(object, "closed", 1, 9007199254740991LL); tab.used = tab.closed;
            tab.window = static_cast<int>(wire::get_int(object, "window", 0, INT_MAX));
            auto observed = object.find("observed");
            if (observed == object.end() || !std::holds_alternative<bool>(observed->second)) throw wire::Error();
            tab.observed = std::get<bool>(observed->second);
            if (tab.url.empty() || tab.profile.size() != 36 || tab.record.size() != 38 || !records.insert(tab.record).second) throw wire::Error();
            result.emplace_back(std::move(tab));
        }
        expire_history(result, days);
    } catch (const std::exception&) { result.clear(); corrupt = true; }
    LocalFree(plain.pbData); return result;
}
// One sleeping writer, one replaceable pending snapshot; file I/O never runs in
// the tray's message loop. Clear invalidates queued work before deleting files.
class HistoryStore {
    std::wstring path;
    std::mutex queue_mutex, io_mutex;
    std::condition_variable wake;
    std::optional<std::vector<Tab>> pending;
    uint64_t revision = 0;
    bool closing = false;
    std::thread writer;
    void run() {
        for (;;) {
            std::vector<Tab> work; uint64_t version{};
            {
                std::unique_lock lock(queue_mutex); wake.wait(lock, [&] { return closing || pending.has_value(); });
                if (!pending) return;
                work = std::move(*pending); pending.reset(); version = revision;
            }
            std::lock_guard io(io_mutex);
            { std::lock_guard lock(queue_mutex); if (version != revision) continue; }
            try { failed = !persist_closed(path, work); } catch (const std::exception&) { failed = true; }
        }
    }
public:
    std::atomic<bool> failed{false};
    explicit HistoryStore(std::wstring file) : path(std::move(file)), writer([this] { run(); }) {}
    ~HistoryStore() {
        { std::lock_guard lock(queue_mutex); closing = true; } wake.notify_one(); writer.join();
    }
    void submit(const std::vector<IndexedTab>& tabs) {
        std::vector<Tab> snapshot;
        for (const auto& item : tabs) if (item.tab.closed) snapshot.push_back(item.tab);
        { std::lock_guard lock(queue_mutex); ++revision; pending = std::move(snapshot); } wake.notify_one();
    }
    bool clear() {
        { std::lock_guard lock(queue_mutex); ++revision; pending.reset(); }
        std::lock_guard io(io_mutex);
        bool ok = DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        auto temporary = path + L".pending";
        if (!DeleteFileW(temporary.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = false;
        failed = !ok; return ok;
    }
};
}
