#pragma once
#include <windows.h>
#include <objbase.h>
#include <chrono>
#include "core.hpp"
#include "protocol.hpp"
namespace hype {
inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
inline bool http_url(std::wstring_view url) {
    auto lower = fold(std::wstring(url.substr(0, 8)));
    size_t start = lower.starts_with(L"https://") ? 8 : lower.starts_with(L"http://") ? 7 : 0;
    if (!start || url.size() <= start || url[start] == L'/' || url[start] == L'\\' || url[start] == L'?' || url[start] == L'#') return false;
    for (wchar_t c : url) if (c <= 32 || c == L'\\') return false;
    return true;
}
inline std::wstring record_id() {
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw wire::Error();
    wchar_t value[40]{}; if (!StringFromGUID2(guid, value, 40)) throw wire::Error(); return value;
}
inline bool expire_history(std::vector<IndexedTab>& tabs, int days, long long now = now_ms()) {
    days = std::clamp(days, 0, 7);
    auto before = tabs.size();
    std::erase_if(tabs, [&](const auto& item) {
        return item.tab.closed && (!days || item.tab.closed <= now - static_cast<long long>(days) * 86400000 || item.tab.closed > now + 60000);
    });
    std::vector<long long> times;
    for (const auto& item : tabs) if (item.tab.closed) times.push_back(item.tab.closed);
    if (times.size() > 1000) {
        std::sort(times.begin(), times.end(), std::greater<>());
        auto cutoff = times[999]; size_t retained = 0;
        auto equals_allowed = static_cast<size_t>(std::count(times.begin(), times.begin() + 1000, cutoff));
        std::erase_if(tabs, [&](const auto& item) {
            if (!item.tab.closed) return false;
            if (item.tab.closed < cutoff) return true;
            if (item.tab.closed > cutoff) return false;
            return ++retained > equals_allowed;
        });
    }
    return before != tabs.size();
}
// Only one unambiguous counterpart is associated. Equal URLs alone do not prove
// which of several browser restoration records belongs to an observed closure.
inline bool retain_closed(std::vector<IndexedTab>& tabs, Tab tab, int days, long long now = now_ms()) {
    if (!days || tab.url.empty() || !tab.closed || tab.closed <= now - static_cast<long long>(days) * 86400000 || tab.closed > now + 60000) return false;
    if (!tab.session.empty()) {
        for (auto& existing : tabs) if (existing.tab.closed && existing.tab.profile == tab.profile && existing.tab.session == tab.session) return false;
    }
    std::vector<size_t> candidates;
    for (size_t i = 0; i < tabs.size(); ++i) {
        const auto& existing = tabs[i].tab;
        if (existing.closed && existing.profile == tab.profile && existing.url == tab.url && existing.title == tab.title &&
            std::llabs(existing.closed - tab.closed) <= 2000 && existing.observed != tab.observed) candidates.push_back(i);
    }
    if (candidates.size() == 1) {
        auto& existing = tabs[candidates[0]].tab;
        if (tab.observed || existing.session.empty()) {
            existing.observed = existing.observed || tab.observed;
            if (!tab.session.empty()) existing.session = tab.session;
            return true;
        }
    }
    if (!tab.observed && candidates.size() > 1) return false;
    if (tab.record.empty()) tab.record = record_id();
    tabs.emplace_back(std::move(tab)); expire_history(tabs, days, now); return true;
}
}
