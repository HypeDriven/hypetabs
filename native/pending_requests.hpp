#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
namespace hype {
struct PendingRequest {
    uint64_t connection;
    std::wstring record;
    uint64_t cue_generation{};
    uint64_t deadline{};
    bool preparing = false;
    int64_t tab_id = -1;
    // Set when the verified Chrome window did not reach the Windows foreground.
    bool foreground_failed = false;
};
using PendingRequests = std::map<uint64_t, PendingRequest>;
inline uint32_t next_request_delay(const PendingRequests& requests, uint64_t now) {
    if (requests.empty()) return 0;
    uint64_t earliest = UINT64_MAX;
    for (const auto& [id, request] : requests) { (void)id; earliest = std::min(earliest, request.deadline); }
    return earliest <= now ? 1 : static_cast<uint32_t>(std::min<uint64_t>(10000, earliest - now));
}
template<class Expired>
size_t expire_requests(PendingRequests& requests, uint64_t now, Expired expired) {
    size_t count = 0;
    for (auto entry = requests.begin(); entry != requests.end();) {
        if (entry->second.deadline <= now) {
            expired(entry->second); entry = requests.erase(entry); ++count;
        } else ++entry;
    }
    return count;
}
}
