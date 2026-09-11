#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../native/tab_locator.hpp"
#include <iostream>
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    wchar_t* end{}; auto raw = wcstoull(argv[1], &end, 10); if (!raw || !end || *end) return 2;
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw));
    if (!hype::chrome_window(window)) { std::cerr << "Target is not a Chrome window\n"; return 2; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    std::atomic<bool> stop{false};
    auto found = hype::locate_tab(window, L"HypeTabs guidance probe", 1, stop);
    CoUninitialize();
    if (!found.found) { std::cout << "No unique selected Chrome tab header was located\n"; return 1; }
    std::cout << "Located unique selected Chrome tab outside document content; rectangle " <<
        found.rectangle.left << ',' << found.rectangle.top << ',' << found.rectangle.right << ',' << found.rectangle.bottom << '\n';
    return 0;
}
