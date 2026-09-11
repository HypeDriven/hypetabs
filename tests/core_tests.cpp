#include "../native/core.hpp"
#include "../native/pending_requests.hpp"
#include <cstdlib>
#include <iostream>
#include <chrono>
void check(bool condition, const char* name) {
    if (!condition) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}
int main() {
    using namespace hype;
    PendingRequests pending;
    check(next_request_delay(pending, 1000) == 0, "empty request queue needs no timer");
    pending[1] = {1, L"closed-record", 1, 11000};
    pending[2] = {2, L"", 2, 16000};
    check(next_request_delay(pending, 6000) == 5000, "later request must not extend earlier deadline");
    size_t expiredCount = 0;
    check(expire_requests(pending, 10999, [&](const auto&) { ++expiredCount; }) == 0, "request expired too early");
    check(expire_requests(pending, 11000, [&](const auto& request) { check(request.connection == 1, "wrong request expired"); ++expiredCount; }) == 1, "due request was not expired");
    check(expiredCount == 1 && pending.size() == 1 && pending.contains(2) && next_request_delay(pending, 11000) == 5000, "expiry discarded or delayed later request");
    pending.erase(2);
    check(next_request_delay(pending, 12000) == 0, "last response leaves a timer active");
    check(one_edit(L"chrome", L"chrom"), "deletion");
    check(one_edit(L"chrome", L"chrame"), "substitution");
    check(!one_edit(L"chrome", L"chram"), "two edits rejected");
    std::vector<IndexedTab> tabs;
    Tab a; a.id = 1; a.title = L"Chrome documentation"; a.url = L"https://developer.chrome.com"; a.label = L"Work"; a.used = 10;
    tabs.emplace_back(a);
    a.id = 2; a.label = L"Personal"; a.used = 20; tabs.emplace_back(a);
    a.id = 3; a.closed = 30; a.used = 30; tabs.emplace_back(a);
    auto results = search(tabs, L"CHROME work");
    check(results.size() == 1 && results[0] == 0, "multi-term profile filtering");
    results = search(tabs, L"chrome");
    check(results.size() == 3 && results[0] == 1 && results[2] == 2, "duplicates, activity, open tie break");
    check(search(tabs, L"chrme").size() == 3, "typo search");
    check(search(tabs, L"nonexistent").empty(), "no match");
    check(search(tabs, L"").size() == 3, "empty query");
    check(fold(L"ÉCOLE ПРИВЕТ") == L"école привет", "invariant non-ASCII case mapping");
    std::vector<IndexedTab> international;
    Tab localized; localized.title = L"ÉCOLE 東京 ПРИВЕТ"; localized.label = L"ÉQUIPE";
    international.emplace_back(localized);
    localized.title = L"Unrelated page"; localized.label = L"Personal";
    international.emplace_back(localized);
    for (const auto& query : {L"école", L"東京", L"привет", L"ÉCOLE équipe"})
        check(search(international, query) == std::vector<size_t>{0}, "Unicode case and script matching");
    check(search(international, L"大阪").empty(), "non-Latin query must not become empty search");
    check(words(L"cafe\u0301").size() == 1 && words(L"cafe\u0301")[0] == L"cafe\u0301", "combining mark stays in its word");
    auto supplementary = words(L"\U0001f680");
    check(supplementary.size() == 1 && supplementary[0] == L"\U0001f680", "supplementary character stays intact");
    std::vector<IndexedTab> workload;
    for (int i = 0; i < 2000; ++i) {
        Tab item; item.id = i; item.title = L"Chrome documentation " + std::to_wstring(i);
        item.url = L"https://developer.chrome.com/docs/extensions/reference/api/tabs?item=" + std::to_wstring(i);
        item.label = L"Profile " + std::to_wstring(i % 5); item.closed = i >= 1000 ? 1 : 0;
        workload.emplace_back(std::move(item));
    }
    std::vector<double> timings;
    for (int i = 0; i < 100; ++i) {
        auto start = std::chrono::steady_clock::now();
        auto found = search(workload, i % 2 ? L"chrme documentation" : L"chrome profile");
        check(found.size() == 2000, "benchmark result completeness");
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        timings.push_back(elapsed);
    }
    std::sort(timings.begin(), timings.end());
    std::cout << "Core search checks passed. Synthetic 2,000-tab query p95: " << timings[94] << " ms\n";
}
