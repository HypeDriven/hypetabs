#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/core.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>

// Frozen pre-optimization scorer: a differential oracle and timing baseline.
int baseline_score(const hype::IndexedTab& tab, const std::vector<std::wstring>& query) {
    int sum = 0;
    for (const auto& term : query) {
        int best = 0;
        if (tab.title == term) best = 120;
        else if (tab.title.starts_with(term)) best = 100;
        else if (tab.text.find(term) != std::wstring::npos) best = 60;
        for (const auto& token : tab.tokens) {
            if (token == term) best = std::max(best, 90);
            else if (token.starts_with(term)) best = std::max(best, 75);
            else if (term.size() >= 4 && hype::one_edit(token, term)) best = std::max(best, 20);
        }
        if (!best) return -1;
        sum += best;
    }
    return sum;
}
std::vector<size_t> baseline_search(const std::vector<hype::IndexedTab>& tabs, const std::wstring& query) {
    auto terms = hype::words(hype::fold(query.substr(0, 512)));
    std::vector<std::pair<int, size_t>> matches;
    for (size_t i = 0; i < tabs.size(); ++i) {
        int score = baseline_score(tabs[i], terms);
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
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    require(SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS) != FALSE, "Could not set idle priority");
    require(SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN) != FALSE, "Could not enable background mode");
    std::vector<hype::IndexedTab> tabs;
    const wchar_t* topics[] = {L"Chrome documentation", L"Project planning", L"Travel reservations", L"C++ performance", L"Weekly notes", L"Browser extensions", L"Recipe collection", L"Release checklist"};
    for (int i = 0; i < 2000; ++i) {
        hype::Tab tab;
        tab.id = i; tab.window = i % 10; tab.profile = std::to_wstring(i % 5);
        tab.label = L"Profile " + tab.profile;
        tab.title = std::wstring(topics[i % 8]) + L" " + std::to_wstring(i % 37);
        tab.url = L"https://example.test/" + std::to_wstring(i % 8) + L"/documentation/browser/reference?item=" + std::to_wstring(i);
        tab.closed = i >= 1000 ? 1 : 0; tab.used = i % 31;
        tabs.emplace_back(std::move(tab));
    }
    const std::vector<std::wstring> queries = {L"", L"chrome", L"chrme documentation", L"profile 3", L"reference", L"reservation", L"release check", L"performance profile", L"zzzzzz", L"documentation browser", L"C++", L"e"};
    for (const auto& query : queries)
        require(hype::search(tabs, query) == baseline_search(tabs, query), "Search ranking changed");

    // Randomized token order catches stronger matches following weaker ones.
    std::mt19937 random(7319);
    const std::vector<std::wstring> vocabulary = {L"chrome", L"chrme", L"chromium", L"chroma", L"work", L"workflow", L"worker", L"profile", L"profiles", L"", L"docs"};
    for (int trial = 0; trial < 10000; ++trial) {
        hype::Tab tab;
        tab.title = vocabulary[random() % vocabulary.size()];
        for (int i = 0; i < 8; ++i) tab.url += L"/" + vocabulary[random() % vocabulary.size()];
        tab.label = vocabulary[random() % vocabulary.size()];
        hype::IndexedTab indexed(std::move(tab));
        auto terms = hype::words(vocabulary[random() % vocabulary.size()] + L" " + vocabulary[random() % vocabulary.size()]);
        require(indexed.score(terms) == baseline_score(indexed, terms), "Randomized score changed");
    }
    size_t checksum = 0;
    struct Timing { double milliseconds, cycles; };
    auto measure = [&](bool baseline) {
        ULONG64 firstCycles{}, lastCycles{};
        require(QueryThreadCycleTime(GetCurrentThread(), &firstCycles) != FALSE, "Could not read thread cycles");
        auto start = std::chrono::steady_clock::now();
        for (int repeat = 0; repeat < 200; ++repeat) for (const auto& query : queries) {
            auto result = baseline ? baseline_search(tabs, query) : hype::search(tabs, query);
            checksum += result.size();
            if (!result.empty()) checksum += result.front();
        }
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        require(QueryThreadCycleTime(GetCurrentThread(), &lastCycles) != FALSE, "Could not read thread cycles");
        return Timing{elapsed / (200 * queries.size()), static_cast<double>(lastCycles - firstCycles) / (200 * queries.size())};
    };
    for (int warmup = 0; warmup < 3; ++warmup) { measure(true); measure(false); }
    std::vector<Timing> before, after;
    for (int trial = 0; trial < 9; ++trial) {
        if (trial % 2) { after.push_back(measure(false)); before.push_back(measure(true)); }
        else { before.push_back(measure(true)); after.push_back(measure(false)); }
    }
    auto report = [](const char* name, const std::vector<Timing>& values) {
        std::vector<double> milliseconds, cycles;
        for (auto value : values) { milliseconds.push_back(value.milliseconds); cycles.push_back(value.cycles); }
        std::sort(milliseconds.begin(), milliseconds.end()); std::sort(cycles.begin(), cycles.end());
        std::cout << name << " ms/query median=" << milliseconds[4] << " range=" << milliseconds.front() << ".." << milliseconds.back()
                  << "; cycles/query median=" << cycles[4] << " range=" << cycles.front() << ".." << cycles.back() << '\n';
    };
    report("Baseline", before); report("Optimized", after);
    std::cout << "Passed ranking and 10,000 score comparisons; checksum=" << checksum
              << "; 2,000 tabs, 5 profiles, 10 windows, idle/background priority\n";
}
