#pragma once
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <thread>

namespace hype {
// Windows reserves several Win+key combinations for the shell (Win+W opens the
// Widgets board on Windows 11), so RegisterHotKey refuses them. For those, a
// low-level keyboard hook on its own above-normal thread catches the exact
// combination first, swallows it, and posts WM_HOTKEY to the search window.
// The thread does nothing else, so the hook never approaches the system's
// low-level hook timeout even though the process runs at idle priority.
class ShortcutHook {
public:
    ShortcutHook(HWND target, WORD shortcut, int id) : target_(target), shortcut_(shortcut), id_(id) {
        instance_ = this;
        thread_ = std::thread([this] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, procedure, GetModuleHandleW(nullptr), 0);
            thread_id_ = GetCurrentThreadId();
            ready_.store(true);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
            if (hook_) UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
        });
        while (!ready_.load()) Sleep(1);
    }
    ~ShortcutHook() {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        if (thread_.joinable()) thread_.join();
        instance_ = nullptr;
    }
    bool installed() const { return hook_ != nullptr; }
    ShortcutHook(const ShortcutHook&) = delete;
    ShortcutHook& operator=(const ShortcutHook&) = delete;

private:
    static bool held(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }
    static LRESULT CALLBACK procedure(int code, WPARAM w, LPARAM l) {
        auto self = instance_;
        if (code == HC_ACTION && self) {
            const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
            bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
            if (key.vkCode == LOBYTE(self->shortcut_)) {
                if (!down) { self->pressed_ = false; }
                else if (self->pressed_) return 1; // autorepeat while held: swallow, do not toggle again
                else if (self->modifiers_match()) {
                    self->pressed_ = true;
                    // The shell opens Start when Win is released with no other key seen; an
                    // injected unassigned key (never delivered to any window) marks the chord.
                    if (HIBYTE(self->shortcut_) & 0x10) {
                        INPUT dummy[2]{}; dummy[0].type = dummy[1].type = INPUT_KEYBOARD;
                        dummy[0].ki.wVk = dummy[1].ki.wVk = 0xE8; dummy[1].ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(2, dummy, sizeof(INPUT));
                    }
                    PostMessageW(self->target_, WM_HOTKEY, static_cast<WPARAM>(self->id_), 0);
                    return 1;
                }
            }
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    bool modifiers_match() const {
        BYTE flags = HIBYTE(shortcut_);
        return held(VK_CONTROL) == ((flags & HOTKEYF_CONTROL) != 0) && held(VK_MENU) == ((flags & HOTKEYF_ALT) != 0) &&
            held(VK_SHIFT) == ((flags & HOTKEYF_SHIFT) != 0) && (held(VK_LWIN) || held(VK_RWIN)) == ((flags & 0x10) != 0);
    }
    HWND target_; WORD shortcut_; int id_;
    HHOOK hook_{}; DWORD thread_id_{}; std::atomic<bool> ready_{false}; bool pressed_ = false;
    std::thread thread_;
    static inline ShortcutHook* instance_ = nullptr;
};
}
