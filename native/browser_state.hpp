#pragma once
#include <windows.h>
#include <climits>
#include <set>
#include "core.hpp"
#include "history.hpp"
#include "protocol.hpp"
namespace hype {
inline std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) throw wire::Error();
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n); return result;
}
struct BrowserState {
    struct Profile { std::wstring id, label; uint64_t connection{}; bool snapshot = false; std::vector<IndexedTab> staging; };
    std::map<uint64_t, Profile> profiles;
    std::vector<IndexedTab> tabs;
    bool paused = false;
    int retention_days = 7;
    long long history_floor = 0;
    bool history_dirty = false;
    bool expire(long long now = now_ms()) { bool changed = expire_history(tabs, retention_days, now); history_dirty = history_dirty || changed; return changed; }
    bool disconnect(uint64_t connection) { profiles.erase(connection); return true; }
    void rename(Profile& profile, std::wstring label) {
        if (label.empty() || label.size() > 128 || std::any_of(label.begin(), label.end(), [](wchar_t c) { return c < 32 || c == 127; })) throw wire::Error();
        profile.label = std::move(label);
        for (auto& item : tabs) if (item.tab.profile == profile.id && item.tab.label != profile.label) {
            Tab replacement = item.tab; replacement.label = profile.label;
            history_dirty = history_dirty || replacement.closed != 0; item = IndexedTab(std::move(replacement));
        }
        for (auto& item : profile.staging) { Tab replacement = item.tab; replacement.label = profile.label; item = IndexedTab(std::move(replacement)); }
    }
    bool receive(uint64_t connection, const wire::Object& object) {
        wire::get_int(object, "v", 1, 1);
        auto type = wire::get_string(object, "type", 20);
        if (type == "hello") {
            if (profiles.contains(connection) || profiles.size() >= 16 || object.size() != 4) throw wire::Error();
            auto identity = wire::get_string(object, "profile", 36);
            if (identity.size() != 36) throw wire::Error();
            for (size_t i = 0; i < identity.size(); ++i) {
                if (i == 8 || i == 13 || i == 18 || i == 23) { if (identity[i] != '-') throw wire::Error(); }
                else if (!((identity[i] >= '0' && identity[i] <= '9') || (identity[i] >= 'a' && identity[i] <= 'f'))) throw wire::Error();
            }
            auto id = wide(identity);
            for (auto& [key, profile] : profiles) { (void)key; if (profile.id == id) throw wire::Error(); }
            auto label = wide(wire::get_string(object, "label", 128));
            if (std::any_of(label.begin(), label.end(), [](wchar_t c) { return c < 32 || c == 127; })) throw wire::Error();
            auto [inserted, fresh] = profiles.emplace(connection, Profile{id, label, connection, false, {}});
            (void)fresh; rename(inserted->second, std::move(label));
            return true;
        }
        auto found = profiles.find(connection); if (found == profiles.end()) throw wire::Error();
        auto& profile = found->second;
        if (type == "label") { if (object.size() != 3) throw wire::Error(); rename(profile, wide(wire::get_string(object, "label", 128))); return true; }
        if (type == "display") {
            // Validated here; the UI assembles the layout. Booleans are checked by presence and type.
            if (object.size() != 10) throw wire::Error();
            auto count = wire::get_int(object, "count", 1, 16); wire::get_int(object, "index", 0, count - 1);
            wire::get_int(object, "left", -100000, 100000); wire::get_int(object, "top", -100000, 100000);
            wire::get_int(object, "width", 1, 32768); wire::get_int(object, "height", 1, 32768); wire::get_int(object, "dpi", 48, 960);
            auto primary = object.find("primary"); if (primary == object.end() || !std::holds_alternative<bool>(primary->second)) throw wire::Error();
            return false;
        }
        if (type == "labelError") { if (object.size() != 2) throw wire::Error(); return false; }
        if (type == "located" || type == "prepared") {
            if (type == "prepared" && object.size() != 8) throw wire::Error();
            if (object.size() != 4 && object.size() != 8) throw wire::Error();
            if (object.size() == 8) {
                wire::get_int(object, "left", -100000, 100000); wire::get_int(object, "top", -100000, 100000);
                wire::get_int(object, "width", 1, 32768); wire::get_int(object, "height", 1, 32768);
            }
            wire::get_int(object, "request", 1, 9007199254740991LL); wire::get_string(object, "title", 4096, true); return false;
        }
        if (type == "result") {
            if (object.size() != 4) throw wire::Error();
            wire::get_int(object, "request", 1, 9007199254740991LL);
            auto status = wire::get_string(object, "status", 20);
            if (status != "ok" && status != "focus" && status != "unavailable" && status != "restored") throw wire::Error();
            return false; // Request correlation is handled by the UI.
        }
        if (type != "begin" && type != "end" && type != "upsert" && type != "remove" && type != "close" && type != "recent") throw wire::Error();
        if (paused) return false;
        if (type == "recent") {
            if (object.size() != 7) throw wire::Error();
            auto privacy = object.find("incognito");
            if (privacy == object.end() || !std::holds_alternative<bool>(privacy->second) || std::get<bool>(privacy->second)) throw wire::Error();
            Tab tab; tab.profile = profile.id; tab.label = profile.label;
            tab.session = wide(wire::get_string(object, "session", 256));
            tab.title = wide(wire::get_string(object, "title", 4096, true));
            tab.url = wide(wire::get_string(object, "url", 8192));
            tab.closed = wire::get_int(object, "closed", 1, 9007199254740991LL); tab.used = tab.closed;
            if (tab.closed <= history_floor) return false;
            bool changed = retain_closed(tabs, std::move(tab), retention_days);
            history_dirty = history_dirty || changed; return changed;
        }
        if (type == "begin") {
            if (object.size() != 2 || profile.snapshot) throw wire::Error();
            profile.snapshot = true; profile.staging.clear(); return false;
        }
        if (type == "end") {
            if (object.size() != 2 || !profile.snapshot) throw wire::Error();
            auto remaining = static_cast<size_t>(std::count_if(tabs.begin(), tabs.end(), [&](const auto& item) { return !item.tab.closed && item.tab.profile != profile.id; }));
            if (remaining + profile.staging.size() > 10000) throw wire::Error();
            std::erase_if(tabs, [&](const auto& item) { return !item.tab.closed && item.tab.profile == profile.id; });
            for (auto& item : profile.staging) tabs.push_back(std::move(item));
            profile.staging.clear(); profile.snapshot = false; return true;
        }
        auto& destination = profile.snapshot ? profile.staging : tabs;
        int id = static_cast<int>(wire::get_int(object, "id", 0, INT_MAX));
        auto existing = std::find_if(destination.begin(), destination.end(), [&](const auto& item) { return !item.tab.closed && item.tab.profile == profile.id && item.tab.id == id; });
        if (type == "close") {
            if (object.size() != 4 || profile.snapshot) throw wire::Error();
            auto closed = wire::get_int(object, "closed", 1, 9007199254740991LL);
            if (existing == destination.end()) return false;
            Tab tab = existing->tab; destination.erase(existing);
            tab.closed = closed; tab.observed = true; tab.used = closed; tab.connection.clear();
            if (closed > history_floor) history_dirty = retain_closed(tabs, std::move(tab), retention_days) || history_dirty;
            return true;
        }
        if (type == "remove") {
            if (object.size() != 3) throw wire::Error();
            if (existing == destination.end()) return false;
            destination.erase(existing);
        } else if (type == "upsert") {
            if (object.size() != 8) throw wire::Error();
            auto privacy = object.find("incognito");
            if (privacy == object.end() || !std::holds_alternative<bool>(privacy->second) || std::get<bool>(privacy->second)) throw wire::Error();
            Tab tab; tab.id = id; tab.profile = profile.id; tab.label = profile.label;
            tab.connection = std::to_wstring(connection);
            tab.window = static_cast<int>(wire::get_int(object, "window", 0, INT_MAX));
            tab.title = wide(wire::get_string(object, "title", 4096, true));
            tab.url = wide(wire::get_string(object, "url", 8192, true));
            tab.used = wire::get_int(object, "used", 0, 9007199254740991LL);
            if (existing != destination.end()) {
                if (!existing->update(std::move(tab))) return false;
            }
            else {
                if (std::count_if(destination.begin(), destination.end(), [](const auto& item) { return !item.tab.closed; }) >= 10000) throw wire::Error();
                destination.emplace_back(std::move(tab));
            }
        } else throw wire::Error();
        return !profile.snapshot;
    }
    uint64_t connection_for(const Tab& tab) const {
        for (const auto& [key, profile] : profiles)
            if (profile.id == tab.profile && (tab.closed || std::to_wstring(key) == tab.connection)) return key;
        return 0;
    }
    bool connected(const Tab& tab) const { return connection_for(tab) != 0; }
    bool consume(const std::wstring& record) {
        auto before = tabs.size();
        std::erase_if(tabs, [&](const auto& item) { return item.tab.closed && item.tab.record == record; });
        bool changed = before != tabs.size(); history_dirty = history_dirty || changed; return changed;
    }
};
}
