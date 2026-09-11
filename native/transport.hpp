#pragma once
#include <windows.h>
#include <sddl.h>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "protocol.hpp"

namespace hype {
class Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
inline std::wstring user_sid() {
    HANDLE raw{}; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw wire::Error();
    Handle token(raw); DWORD size{}; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) throw wire::Error();
    LPWSTR text{}; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) throw wire::Error();
    std::wstring result(text); LocalFree(text); return result;
}
inline std::wstring pipe_name() { return L"\\\\.\\pipe\\HypeTabs.v1." + user_sid(); }
// Every pending operation is completed/cancelled before its stack OVERLAPPED is destroyed.
inline bool transfer(HANDLE pipe, HANDLE stop, void* buffer, DWORD size, bool writing, DWORD timeout = INFINITE) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    while (size) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!event.valid()) return false;
        OVERLAPPED operation{}; operation.hEvent = event;
        DWORD count{};
        BOOL ok = writing ? WriteFile(pipe, bytes, size, &count, &operation) : ReadFile(pipe, bytes, size, &count, &operation);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE waits[]{stop, event};
            if (WaitForMultipleObjects(2, waits, FALSE, timeout) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE); return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) return false;
        }
        if (!count) return false;
        bytes += count; size -= count;
    }
    return true;
}
inline bool read_frame(HANDLE pipe, HANDLE stop, std::string& data) {
    uint32_t size{};
    if (!transfer(pipe, stop, &size, 4, false) || !size || size > wire::max_frame) return false;
    data.resize(size); return transfer(pipe, stop, data.data(), size, false, 5000);
}
inline bool write_frame(HANDLE pipe, HANDLE stop, const std::string& data) {
    if (data.empty() || data.size() > wire::max_frame) return false;
    uint32_t size = static_cast<uint32_t>(data.size());
    return transfer(pipe, stop, &size, 4, true, 2000) && transfer(pipe, stop, const_cast<char*>(data.data()), size, true, 2000);
}
struct Packet { uint64_t connection; std::string data; }; // Empty data means disconnect.
class Transport {
    struct Slot { std::mutex lock; HANDLE pipe = INVALID_HANDLE_VALUE; uint64_t generation = 0; DWORD client = 0; };
    std::array<Slot, 16> slots;
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle outgoing_event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Handle incoming_space{CreateEventW(nullptr, TRUE, TRUE, nullptr)};
    std::mutex queue_lock;
    std::deque<Packet> incoming, outgoing;
    std::vector<std::thread> readers;
    std::thread writer;
    std::atomic<uint64_t> next{1};
    HWND target; UINT notification;
    std::wstring name;
    PSECURITY_DESCRIPTOR descriptor{};
    bool enqueue(Packet packet) {
        for (;;) {
            {
                std::lock_guard guard(queue_lock);
                if (incoming.size() < 256) {
                    bool notify = incoming.empty(); incoming.push_back(std::move(packet));
                    if (incoming.size() == 256) ResetEvent(incoming_space);
                    if (notify) PostMessageW(target, notification, 0, 0);
                    return true;
                }
            }
            HANDLE waits[]{stop, incoming_space};
            if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0 + 1) return false;
        }
    }
    void listen(Slot& slot) {
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                static_cast<DWORD>(slots.size()), wire::max_frame, wire::max_frame, 0, &security));
            if (!pipe.valid()) return;
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED pending{}; pending.hEvent = event;
            bool connected = ConnectNamedPipe(pipe, &pending) != FALSE;
            if (!connected) {
                auto error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) connected = true;
                else if (error == ERROR_IO_PENDING) {
                    HANDLE waits[]{stop, event}; DWORD count{};
                    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
                        connected = GetOverlappedResult(pipe, &pending, &count, FALSE) != FALSE;
                    else { CancelIoEx(pipe, &pending); GetOverlappedResult(pipe, &pending, &count, TRUE); }
                }
            }
            if (!connected) continue;
            auto id = next.fetch_add(1);
            ULONG client{}; if (!GetNamedPipeClientProcessId(pipe, &client)) client = 0;
            { std::lock_guard guard(slot.lock); slot.pipe = pipe; slot.generation = id; slot.client = client; }
            unsigned messages = 0; ULONGLONG period = GetTickCount64();
            std::string data;
            while (read_frame(pipe, stop, data)) {
                auto now = GetTickCount64();
                if (now - period >= 1000) { period = now; messages = 0; }
                if (++messages > 5000) break;
                if (!enqueue({id, std::move(data)})) break;
            }
            { std::lock_guard guard(slot.lock); slot.pipe = INVALID_HANDLE_VALUE; slot.generation = 0; slot.client = 0; }
            DisconnectNamedPipe(pipe);
            if (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) enqueue({id, {}});
        }
    }
    void send_loop() {
        HANDLE waits[]{stop, outgoing_event};
        while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            for (;;) {
                Packet packet;
                { std::lock_guard guard(queue_lock); if (outgoing.empty()) break; packet = std::move(outgoing.front()); outgoing.pop_front(); }
                for (auto& slot : slots) {
                    std::lock_guard guard(slot.lock);
                    if (slot.generation == packet.connection && slot.pipe != INVALID_HANDLE_VALUE) {
                        if (packet.data.empty() || !write_frame(slot.pipe, stop, packet.data)) { CancelIoEx(slot.pipe, nullptr); DisconnectNamedPipe(slot.pipe); }
                        break;
                    }
                }
            }
        }
    }
public:
    Transport(HWND window, UINT message) : target(window), notification(message), name(pipe_name()) {
        auto acl = L"D:P(A;;GA;;;" + user_sid() + L")";
        if (!stop.valid() || !outgoing_event.valid() || !incoming_space.valid() ||
            !ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw wire::Error();
        try {
            for (auto& slot : slots) readers.emplace_back([this, &slot] { listen(slot); });
            writer = std::thread([this] { send_loop(); });
        } catch (...) { shutdown(); LocalFree(descriptor); throw; }
    }
    ~Transport() { shutdown(); LocalFree(descriptor); }
    void shutdown() {
        SetEvent(stop);
        for (auto& reader : readers) if (reader.joinable()) reader.join();
        if (writer.joinable()) writer.join();
    }
    std::deque<Packet> receive() {
        std::lock_guard guard(queue_lock); std::deque<Packet> result; result.swap(incoming); SetEvent(incoming_space); return result;
    }
    // Process ID of the bridge connected as this connection, or 0.
    DWORD client_process(uint64_t connection) {
        for (auto& slot : slots) { std::lock_guard guard(slot.lock); if (slot.generation == connection && slot.pipe != INVALID_HANDLE_VALUE) return slot.client; }
        return 0;
    }
    bool send(uint64_t connection, std::string data) {
        if (data.size() > wire::max_frame) return false;
        std::lock_guard guard(queue_lock); if (outgoing.size() >= 128) return false;
        outgoing.push_back({connection, std::move(data)}); SetEvent(outgoing_event); return true;
    }
};
}
