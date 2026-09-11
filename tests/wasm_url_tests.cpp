#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
extern "C" int fits_url(const unsigned short*, unsigned);
void verify(const std::wstring& text) {
    auto actual = fits_url(reinterpret_cast<const unsigned short*>(text.data()), static_cast<unsigned>(text.size())) != 0;
    int bytes = text.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if ((!text.empty() && !bytes) || actual != (bytes <= 8192)) { std::cerr << "URL kernel disagrees with Windows UTF-8 encoding\n"; std::exit(1); }
}
int main() {
    for (auto ch : {L'a', L'\u00e9', L'\u6771', static_cast<wchar_t>(0xd800), static_cast<wchar_t>(0xdc00)}) {
        for (unsigned length : {0u, 1u, 2730u, 2731u, 4096u, 4097u, 8192u, 8193u}) verify(std::wstring(length, ch));
    }
    std::wstring pairs;
    for (int i = 0; i < 2048; ++i) pairs += L"\U0001f680";
    verify(pairs); verify(pairs + L"a");
    std::mt19937 random(92731);
    for (int trial = 0; trial < 1000; ++trial) {
        std::wstring text(random() % 9000, L' ');
        for (auto& ch : text) ch = static_cast<wchar_t>(random() & 0xffff);
        verify(text);
    }
    std::cout << "Experimental URL kernel passes UTF-8 boundaries, isolated surrogates, pairs, and 1,000 randomized comparisons\n";
}
