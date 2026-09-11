#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace hype {
struct Tab {
    std::wstring profile, connection, title, url, label;
    int id = 0, window = 0;
    long long used = 0, closed = 0;
    std::wstring session, record;
    bool observed = false;
    bool operator==(const Tab&) const = default;
};
inline std::wstring tab_key(const Tab& tab) {
    return tab.profile + (tab.closed ? L":closed:" + tab.record : L":open:" + tab.connection + L":" + std::to_wstring(tab.id));
}
inline std::wstring fold(std::wstring value) {
    bool unicode = false;
    for (auto& ch : value) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        else if (ch > 127) unicode = true;
    }
    if (unicode) {
        int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
        if (length > 0) {
            std::wstring mapped(static_cast<size_t>(length), L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), mapped.data(), length, nullptr, nullptr, 0)) return mapped;
        }
    }
    return value;
}
inline bool word_character(wchar_t ch) {
    if (ch <= 127) return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9');
    // Keep supplementary Unicode characters intact as searchable UTF-16 pairs.
    if (ch >= 0xd800 && ch <= 0xdfff) return true;
    WORD kind{};
    if (GetStringTypeW(CT_CTYPE1, &ch, 1, &kind) && (kind & (C1_ALPHA | C1_DIGIT))) return true;
    return GetStringTypeW(CT_CTYPE3, &ch, 1, &kind) && (kind & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK));
}
inline bool one_edit(std::wstring_view a, std::wstring_view b) {
    if (a.size() > b.size() + 1 || b.size() > a.size() + 1) return false;
    size_t i = 0, j = 0; unsigned edits = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) { ++i; ++j; continue; }
        if (++edits > 1) return false;
        if (a.size() >= b.size()) ++i;
        if (b.size() >= a.size()) ++j;
    }
    return edits + (i < a.size() || j < b.size() ? 1 : 0) <= 1;
}
inline std::vector<std::wstring> words(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && !word_character(text[start])) ++start;
        auto end = start;
        while (end < text.size() && word_character(text[end])) ++end;
        if (end != start) out.emplace_back(text.substr(start, end - start));
        start = end;
    }
    return out;
}
struct IndexedTab {
    Tab tab;
    std::wstring title, text;
    std::vector<std::wstring> tokens;
    explicit IndexedTab(Tab value) : tab(std::move(value)), title(fold(tab.title)),
        text(fold(tab.title + L" " + tab.url + L" " + tab.label)), tokens(words(text)) {}
    bool update(Tab value) {
        if (tab == value) return false;
        if (tab.title == value.title && tab.url == value.url && tab.label == value.label)
            tab = std::move(value);
        else *this = IndexedTab(std::move(value));
        return true;
    }
    int score(const std::vector<std::wstring>& query) const {
        int sum = 0;
        for (const auto& term : query) {
            int best = 0;
            if (title == term) best = 120;
            else if (title.starts_with(term)) best = 100;
            else if (text.find(term) != std::wstring::npos) best = 60;
            // Token matches cannot improve a title match or an exact token.
            if (best < 90) for (const auto& token : tokens) {
                if (token == term) { best = 90; break; }
                if (best < 75 && token.starts_with(term)) best = 75;
                else if (best < 20 && term.size() >= 4 && one_edit(token, term)) best = 20;
            }
            if (!best) return -1;
            sum += best;
        }
        return sum;
    }
};
inline std::vector<size_t> search(const std::vector<IndexedTab>& tabs, const std::wstring& query) {
    auto terms = words(fold(query.substr(0, 512)));
    std::vector<std::pair<int, size_t>> matches;
    for (size_t i = 0; i < tabs.size(); ++i) {
        int score = tabs[i].score(terms);
        if (score >= 0) matches.emplace_back(score, i);
    }
    std::stable_sort(matches.begin(), matches.end(), [&](auto a, auto b) {
        if (a.first != b.first) return a.first > b.first;
        const auto& x = tabs[a.second].tab; const auto& y = tabs[b.second].tab;
        if (bool(x.closed) != bool(y.closed)) return !x.closed;
        return x.used > y.used;
    });
    std::vector<size_t> result;
    for (auto [score, index] : matches) { (void)score; result.push_back(index); }
    return result;
}
}
