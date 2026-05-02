// Thin Win32 window wrapper. Owns the HWND and dispatches input events
// to ImGui via the official Win32 backend. The app gives us a callback
// for file drop / file open events.
#pragma once

#include "platform/windows.h"

#include <functional>
#include <string>

namespace volchay::app {

class Window {
public:
    using FileCallback = std::function<void(const std::wstring&)>;

    Window();
    ~Window();

    bool create(int width, int height, const wchar_t* title);
    void show(int show_cmd);
    void destroy();

    // Pump messages without blocking; returns false if WM_QUIT was seen.
    bool pump_messages();

    // Block in MsgWaitForMultipleObjects until something happens; useful
    // when the timeline is paused (we want to idle, not spin).
    bool wait_for_message(unsigned timeout_ms);

    HWND hwnd() const { return hwnd_; }
    int  width()  const { return width_; }
    int  height() const { return height_; }

    void set_drop_callback(FileCallback cb) { on_drop_ = std::move(cb); }
    void set_resize_callback(std::function<void(int, int)> cb) {
        on_resize_ = std::move(cb);
    }
    void request_close();

    // Show a native "Open file" dialog filtered to common video files.
    // Returns the selected absolute path, or an empty string if cancelled.
    static std::wstring pick_video_file(HWND owner);

private:
    static LRESULT CALLBACK wnd_proc_thunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT wnd_proc(UINT msg, WPARAM wp, LPARAM lp);

    HWND   hwnd_   = nullptr;
    int    width_  = 0;
    int    height_ = 0;

    FileCallback                  on_drop_;
    std::function<void(int, int)> on_resize_;
};

}  // namespace volchay::app
