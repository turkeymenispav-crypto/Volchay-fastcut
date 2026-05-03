#include "app/window.h"

#include "util/log.h"

#include <imgui.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace volchay::app {

namespace {
constexpr wchar_t kClassName[] = L"VolchayFastcutMain";
}

Window::Window()  = default;
Window::~Window() { destroy(); }

LRESULT CALLBACK Window::wnd_proc_thunk(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (self->hwnd_ == nullptr) self->hwnd_ = hwnd;
        return self->wnd_proc(msg, wp, lp);
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Window::wnd_proc(UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd_, msg, wp, lp)) {
        return 0;
    }

    switch (msg) {
    case WM_SIZE: {
        if (wp != SIZE_MINIMIZED) {
            width_  = LOWORD(lp);
            height_ = HIWORD(lp);
            if (on_resize_) on_resize_(width_, height_);
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count > 0) {
            UINT len = ::DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(len, L'\0');
            ::DragQueryFileW(drop, 0, path.data(), len + 1);
            ::DragFinish(drop);
            if (on_drop_) on_drop_(path);
        } else {
            ::DragFinish(drop);
        }
        return 0;
    }
    case WM_CLOSE:
        ::DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 480;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;  // we paint everything via D3D
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

bool Window::create(int width, int height, const wchar_t* title) {
    HINSTANCE inst = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = wnd_proc_thunk;
    wc.hInstance     = inst;
    wc.hIcon         = ::LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIconSm       = wc.hIcon;
    if (!::RegisterClassExW(&wc)) {
        log::err("RegisterClassExW failed: %lu", ::GetLastError());
        return false;
    }

    DWORD style    = WS_OVERLAPPEDWINDOW;
    DWORD ex_style = WS_EX_ACCEPTFILES;

    RECT rc{0, 0, width, height};
    ::AdjustWindowRectEx(&rc, style, FALSE, ex_style);

    hwnd_ = ::CreateWindowExW(
        ex_style, kClassName, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, inst, this);
    if (!hwnd_) {
        log::err("CreateWindowExW failed: %lu", ::GetLastError());
        return false;
    }
    width_  = width;
    height_ = height;
    return true;
}

void Window::show(int show_cmd) {
    if (!hwnd_) return;
    // Always launch maximised so the dock layout has room for the
    // Library / Viewer / Inspector / Timeline panels at any monitor
    // size. The shell-supplied show_cmd is preserved as a fallback if
    // the user explicitly requested a hidden / minimised launch.
    if (show_cmd != SW_HIDE && show_cmd != SW_MINIMIZE
        && show_cmd != SW_SHOWMINNOACTIVE
        && show_cmd != SW_SHOWMINIMIZED) {
        show_cmd = SW_SHOWMAXIMIZED;
    }
    ::ShowWindow(hwnd_, show_cmd);
    ::UpdateWindow(hwnd_);
}

void Window::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ::UnregisterClassW(kClassName, ::GetModuleHandleW(nullptr));
}

bool Window::pump_messages() {
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return true;
}

bool Window::wait_for_message(unsigned timeout_ms) {
    DWORD r = ::MsgWaitForMultipleObjects(0, nullptr, FALSE,
                                          timeout_ms, QS_ALLINPUT);
    (void)r;
    return pump_messages();
}

void Window::request_close() {
    if (hwnd_) ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

std::wstring Window::pick_video_file(HWND owner) {
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter =
        L"Video files\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv\0"
        L"All files\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle   = L"Open video";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetOpenFileNameW(&ofn)) return {};
    return std::wstring(buf);
}

}  // namespace volchay::app
