#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "transport.hpp"

namespace {
bool synchronous(HANDLE file, void* buffer, DWORD size, bool writing) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    while (size) {
        DWORD count{};
        if (!(writing ? WriteFile(file, bytes, size, &count, nullptr) : ReadFile(file, bytes, size, &count, nullptr)) || !count) return false;
        bytes += count; size -= count;
    }
    return true;
}
bool allowed_origin(const wchar_t* caller) {
    if (!caller) return false;
    wchar_t origin[128]{}; DWORD bytes = sizeof(origin);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\HypeTabs", L"ExtensionOrigin", RRF_RT_REG_SZ, nullptr, origin, &bytes) != ERROR_SUCCESS) return false;
    std::wstring_view value(origin);
    if (value.size() != 52 || !value.starts_with(L"chrome-extension://") || value.back() != L'/') return false;
    for (size_t i = 19; i < 51; ++i) if (value[i] < L'a' || value[i] > L'p') return false;
    return value == caller;
}
}
int wmain(int argc, wchar_t** argv) {
    SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    if (argc < 2 || !allowed_origin(argv[1])) return 1;
    try {
        const auto name = hype::pipe_name();
        hype::Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe.valid()) return 2;
        hype::Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop.valid()) return 2;
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
        std::thread reader([&] {
            uint32_t size{};
            while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0 && synchronous(input, &size, 4, false) && size && size <= hype::wire::max_frame) {
                std::string packet(size, '\0');
                if (!synchronous(input, packet.data(), size, false)) break;
                if (!hype::write_frame(pipe, stop, packet)) break;
            }
            SetEvent(stop);
        });
        std::string packet;
        while (hype::read_frame(pipe, stop, packet)) {
            auto size = static_cast<uint32_t>(packet.size());
            if (!synchronous(output, &size, 4, true) || !synchronous(output, packet.data(), size, true)) break;
        }
        SetEvent(stop);
        // Repeat cancellation only during shutdown, covering the race between the
        // stop check and entering the synchronous Chrome stdin read.
        CancelSynchronousIo(reader.native_handle());
        while (WaitForSingleObject(reader.native_handle(), 50) == WAIT_TIMEOUT) CancelSynchronousIo(reader.native_handle());
        reader.join();
        return 0;
    } catch (const std::exception&) { return 3; }
}
