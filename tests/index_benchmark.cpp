#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../native/core.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>

void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Measurement { double ms, cycles; std::vector<hype::IndexedTab> tabs; };
int main() {
    require(SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS) != FALSE, "Idle priority failed");
    require(SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN) != FALSE, "Background mode failed");
    std::vector<hype::IndexedTab> initial;
    for (int i = 0; i < 1000; ++i) {
        hype::Tab tab; tab.id = i; tab.window = i % 10; tab.profile = std::to_wstring(i % 5);
        tab.label = L"Profile " + tab.profile; tab.connection = L"1";
        tab.title = L"Browser extension documentation and project notes " + std::to_wstring(i);
        tab.url = L"https://example.test/documentation/browser/extensions/reference?record=" + std::to_wstring(i);
        initial.emplace_back(std::move(tab));
    }
    auto measure = [&](bool baseline) {
        auto tabs = initial;
        ULONG64 startCycles{}, endCycles{};
        require(QueryThreadCycleTime(GetCurrentThread(), &startCycles) != FALSE, "Cycle counter failed");
        auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < 100; ++round) for (size_t i = 0; i < tabs.size(); ++i) {
            auto next = tabs[i].tab;
            // 20% duplicates, 70% activity/window changes, 10% navigation.
            if (i % 10 >= 2) { ++next.used; next.window = (next.window + 1) % 10; }
            if (i % 10 == 2) next.title = L"Navigated documentation " + std::to_wstring(round);
            if (baseline) tabs[i] = hype::IndexedTab(std::move(next));
            else tabs[i].update(std::move(next));
        }
        double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        require(QueryThreadCycleTime(GetCurrentThread(), &endCycles) != FALSE, "Cycle counter failed");
        return Measurement{elapsed / 100000, static_cast<double>(endCycles - startCycles) / 100000, std::move(tabs)};
    };
    auto verify = [](const Measurement& before, const Measurement& after) {
        require(before.tabs.size() == after.tabs.size(), "Index size mismatch");
        for (size_t i = 0; i < before.tabs.size(); ++i) {
            const auto& a = before.tabs[i]; const auto& b = after.tabs[i];
            require(a.tab == b.tab && a.title == b.title && a.text == b.text && a.tokens == b.tokens, "Index content mismatch");
        }
    };
    for (int warmup = 0; warmup < 3; ++warmup) { auto before = measure(true); auto after = measure(false); verify(before, after); }
    std::vector<double> beforeMs, afterMs, beforeCycles, afterCycles;
    for (int trial = 0; trial < 9; ++trial) {
        auto first = measure(trial % 2 == 0); auto second = measure(trial % 2 != 0);
        const auto& before = trial % 2 == 0 ? first : second;
        const auto& after = trial % 2 == 0 ? second : first;
        verify(before, after);
        beforeMs.push_back(before.ms); afterMs.push_back(after.ms);
        beforeCycles.push_back(before.cycles); afterCycles.push_back(after.cycles);
    }
    auto report = [](const char* name, std::vector<double> values) {
        std::sort(values.begin(), values.end());
        std::cout << name << " median=" << values[4] << " range=" << values.front() << ".." << values.back() << '\n';
    };
    report("Baseline ms/update", beforeMs); report("Optimized ms/update", afterMs);
    report("Baseline cycles/update", beforeCycles); report("Optimized cycles/update", afterCycles);
    std::cout << "Index contents identical after each 100,000-update trial; idle/background priority\n";
}
