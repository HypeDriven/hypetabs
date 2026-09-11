#pragma once
#include "taskbar_locator.hpp"
#include "window_identity.hpp"
namespace hype {
struct TaskbarResult { LocatedTaskbar location; WindowBounds bounds; uint64_t generation{}; };
class TaskbarWorker {
    struct Request { WindowBounds bounds; uint64_t generation; std::vector<ChromeDisplay> displays; };
    HWND receiver; UINT message;
    std::mutex lock; std::condition_variable wake;
    std::optional<Request> pending;
    std::optional<TaskbarResult> completed;
    std::atomic<bool> stopping{false};
    std::thread worker;
    void run() {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Request request;
            {
                std::unique_lock guard(lock); wake.wait(guard, [&] { return stopping || pending.has_value(); });
                if (stopping) break;
                request = *pending; pending.reset();
            }
            TaskbarResult result{{}, request.bounds, request.generation};
            if (SUCCEEDED(initialized)) {
                auto target = verified_chrome_window(request.bounds, request.displays);
                if (target && target != GetForegroundWindow()) result.location = locate_taskbar(target, stopping);
                if (result.location.found && verified_chrome_window(request.bounds, request.displays) != target) result.location.found = false;
            }
            { std::lock_guard guard(lock); if (stopping) break; completed = result; }
            PostMessageW(receiver, message, 0, 0);
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
public:
    TaskbarWorker(HWND target, UINT notification) : receiver(target), message(notification), worker([this] { run(); }) {}
    ~TaskbarWorker() { stopping = true; wake.notify_one(); worker.join(); }
    void request(WindowBounds bounds, uint64_t generation, std::vector<ChromeDisplay> displays = {}) {
        { std::lock_guard guard(lock); pending = Request{bounds, generation, std::move(displays)}; } wake.notify_one();
    }
    std::optional<TaskbarResult> take() { std::lock_guard guard(lock); auto result = completed; completed.reset(); return result; }
};
}
