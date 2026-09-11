#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../native/browser_state.hpp"
#include <iostream>
#include <cstdlib>
void check(bool condition, const char* name) { if (!condition) { std::cerr << name << '\n'; std::exit(1); } }
int main() {
    hype::BrowserState state;
    auto apply = [&](uint64_t id, const std::string& json) { return state.receive(id, hype::wire::Parser(json).parse()); };
    auto invalid = [&](uint64_t id, const std::string& json) { try { apply(id, json); check(false, "accepted invalid state transition"); } catch (const hype::wire::Error&) {} };
    invalid(1, R"({"v":1,"type":"begin"})");
    apply(1, R"({"v":1,"type":"hello","profile":"00000000-0000-0000-0000-000000000001","label":"Work"})");
    apply(1, R"({"v":1,"type":"prepared","request":1,"title":"Example","left":120,"top":140,"width":900,"height":600})");
    invalid(1, R"({"v":1,"type":"prepared","request":1,"title":"Missing bounds"})");
    invalid(1, R"({"v":1,"type":"prepared","request":1,"title":"Example","left":120,"top":140,"width":0,"height":600})");
    apply(1, R"({"v":1,"type":"located","request":1,"title":"Example","left":120,"top":140,"width":900,"height":600})");
    apply(1, R"({"v":1,"type":"located","request":1,"title":"Legacy hint"})");
    invalid(1, R"({"v":1,"type":"located","request":1,"title":"Example","left":120})");
    invalid(1, R"({"v":1,"type":"located","request":1,"title":"Example","left":120,"top":140,"width":0,"height":600})");
    invalid(1, R"({"v":1,"type":"located","request":1,"title":"Example","left":100001,"top":140,"width":900,"height":600})");
    invalid(2, R"({"v":1,"type":"hello","profile":"00000000-0000-0000-0000-000000000001","label":"Duplicate"})");
    apply(1, R"({"v":1,"type":"begin"})");
    invalid(1, R"({"v":1,"type":"begin"})");
    auto update = R"({"v":1,"type":"upsert","id":1,"window":2,"title":"Example","url":"https://example.test","used":100,"incognito":false})";
    apply(1, update); check(state.tabs.empty(), "partial snapshot leaked");
    apply(1, R"({"v":1,"type":"end"})"); check(state.tabs.size() == 1, "snapshot commit");
    check(!apply(1, R"({"v":1,"type":"display","index":0,"count":2,"left":0,"top":0,"width":3840,"height":1600,"dpi":96,"primary":true})"), "display hint requested a refresh");
    invalid(1, R"({"v":1,"type":"display","index":2,"count":2,"left":0,"top":0,"width":3840,"height":1600,"dpi":96,"primary":true})");
    invalid(1, R"({"v":1,"type":"display","index":0,"count":2,"left":0,"top":0,"width":3840,"height":1600,"dpi":20,"primary":true})");
    invalid(1, R"({"v":1,"type":"display","index":0,"count":2,"left":0,"top":0,"width":3840,"height":1600,"dpi":96,"primary":"yes"})");
    invalid(1, R"({"v":1,"type":"display","index":0,"count":2,"left":0,"top":0,"width":0,"height":1600,"dpi":96,"primary":true})");
    invalid(1, R"({"v":1,"type":"display","index":0,"count":2,"left":0,"top":0,"width":3840,"height":1600,"dpi":96})");
    check(!apply(1, update), "duplicate event requested an unnecessary refresh");
    check(!apply(1, R"({"v":1,"type":"remove","id":99})"), "absent removal requested an unnecessary refresh");
    check(apply(1, R"({"v":1,"type":"upsert","id":1,"window":3,"title":"Example","url":"https://example.test","used":200,"incognito":false})"), "activity/window update was ignored");
    check(state.tabs[0].tab.window == 3 && state.tabs[0].tab.used == 200 && hype::search(state.tabs, L"example").size() == 1, "metadata update lost searchable state");
    check(apply(1, R"({"v":1,"type":"upsert","id":1,"window":3,"title":"Revised","url":"https://new.test","used":200,"incognito":false})"), "navigation update was ignored");
    check(hype::search(state.tabs, L"revised new").size() == 1 && hype::search(state.tabs, L"example").empty(), "navigation retained stale search text");
    apply(1, update);
    apply(2, R"({"v":1,"type":"hello","profile":"00000000-0000-0000-0000-000000000002","label":"Personal"})");
    apply(2, R"({"v":1,"type":"begin"})"); apply(2, update); apply(2, R"({"v":1,"type":"end"})");
    check(state.tabs.size() == 2, "duplicate tab ids merged across profiles");
    invalid(2, R"({"v":1,"type":"upsert","id":1,"window":2,"title":"Private","url":"https://private.test","used":100,"incognito":true})");
    check(state.tabs[1].tab.title == L"Example", "invalid update changed state");
    state.paused = true; apply(2, R"({"v":1,"type":"remove","id":1})"); check(state.tabs.size() == 2, "pause indexed data");
    state.paused = false;
    state.disconnect(2); check(!state.connected(state.tabs[1].tab), "disconnected tab marked connected");
    apply(3, R"({"v":1,"type":"hello","profile":"00000000-0000-0000-0000-000000000002","label":"Personal"})");
    check(!state.connected(state.tabs[1].tab), "reconnect revived stale tab ids");
    apply(3, R"({"v":1,"type":"begin"})"); apply(3, R"({"v":1,"type":"end"})");
    check(state.tabs.size() == 1 && state.tabs[0].tab.label == L"Work", "empty snapshot affected another profile");
    apply(1, R"({"v":1,"type":"label","label":"Renamed work"})");
    check(state.tabs[0].tab.label == L"Renamed work" && hype::search(state.tabs, L"renamed").size() == 1, "rename did not update the search index");
    invalid(1, R"({"v":1,"type":"label","label":"bad\nlabel"})");
    check(state.tabs[0].tab.label == L"Renamed work", "invalid label changed state");
    std::cout << "Browser state checks passed\n";
}
