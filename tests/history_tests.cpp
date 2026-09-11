#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../native/persistence.hpp"
#include <iostream>
#include <cstdlib>
void check(bool ok, const char* name) { if (!ok) { std::cerr << name << '\n'; std::exit(1); } }
int main() {
    using namespace hype;
    const auto now = now_ms();
    Tab tab; tab.profile = L"00000000-0000-0000-0000-000000000001"; tab.label = L"Work";
    tab.title = L"Synthetic test tab"; tab.url = L"https://example.test/history"; tab.closed = now; tab.used = now; tab.observed = true;
    std::vector<IndexedTab> history;
    check(retain_closed(history, tab, 7, now), "retain observed closure");
    Tab imported = tab; imported.observed = false; imported.session = L"session-1";
    check(retain_closed(history, imported, 7, now), "associate exact session");
    check(history.size() == 1 && history[0].tab.session == L"session-1", "observed and imported duplication");
    check(!retain_closed(history, imported, 7, now), "repeated session import");
    Tab unsafe = tab; unsafe.url = L"javascript:alert(1)";
    std::vector<IndexedTab> unsupported;
    check(retain_closed(unsupported, unsafe, 7, now) && !http_url(unsupported[0].tab.url), "unsupported scheme must remain searchable but not reopenable");
    check(!http_url(L"https://example.test/\nother") && !http_url(L"https:///missing"), "malformed URL accepted");
    check(!retain_closed(history, tab, 0, now), "disabled retention accepted closure");
    auto old = tab; old.closed = now - 8LL*86400000;
    check(!retain_closed(history, old, 7, now), "expired closure imported");
    std::vector<IndexedTab> capped;
    for (int i = 0; i < 1005; ++i) { auto item = tab; item.closed = now - i; item.record = record_id(); capped.emplace_back(std::move(item)); }
    check(expire_history(capped, 7, now) && capped.size() == 1000, "global retention cap");
    for (const auto& item : capped) check(item.tab.closed >= now - 999, "oldest-first eviction");
    check(expire_history(capped, 0, now) && capped.empty(), "disabled retention clear");
    std::wstring path = L"build\\history-test-" + record_id() + L".dat";
    {
        HistoryStore store(path); store.submit(history);
    }
    bool corrupt{};
    auto loaded = load_closed(path, 7, corrupt);
    check(!corrupt && loaded.size() == 1 && loaded[0].tab.url == tab.url && loaded[0].tab.session == L"session-1", "encrypted roundtrip");
    {
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        LARGE_INTEGER size{}; GetFileSizeEx(file, &size); std::string raw(static_cast<size_t>(size.QuadPart), '\0'); DWORD count{};
        ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &count, nullptr);
        check(raw.find("example.test") == std::string::npos && raw.find("Synthetic test tab") == std::string::npos, "plaintext leaked to disk");
    }
    {
        HistoryStore store(path); for (int i = 0; i < 20; ++i) store.submit(history);
        check(store.clear(), "clear failed");
    }
    check(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES, "queued save resurrected cleared file");
    {
        Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
        DWORD count{}; const char bad[] = "corrupt synthetic data"; WriteFile(file, bad, sizeof(bad), &count, nullptr);
    }
    loaded = load_closed(path, 7, corrupt); check(corrupt && loaded.empty(), "corrupt file not rejected");
    DeleteFileW(path.c_str());
    std::cout << "History retention, DPAPI, corruption, and clear-race checks passed\n";
}
