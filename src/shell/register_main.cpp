// Tiny console tool that registers / unregisters Volchay-fastcut's context
// menu entries. Ships alongside the main exe so installer scripts (or the
// user) can wire things up without launching the full editor.
//
// Usage:
//   volchay-register.exe register      (per-user, no admin)
//   volchay-register.exe unregister
//   volchay-register.exe register --machine     (HKLM, requires admin)
#include "platform/windows.h"
#include "shell/registry.h"

#include <cstdio>
#include <cstring>

static void print_usage() {
    std::fwprintf(stderr,
        L"Usage:\n"
        L"  volchay-register.exe register   [--machine]\n"
        L"  volchay-register.exe unregister [--machine]\n");
}

int wmain(int argc, wchar_t** argv) {
    // Default to "register" when invoked with no arguments (e.g. by
    // double-clicking volchay-register.exe in Explorer). Show a small
    // MessageBox in that case so the user gets visual feedback that
    // the registration succeeded.
    bool no_args = (argc < 2);
    bool machine = false;
    for (int i = 2; i < argc; ++i) {
        if (::wcscmp(argv[i], L"--machine") == 0) machine = true;
    }
    bool per_user = !machine;

    if (no_args) {
        LONG e = volchay::shell::register_context_menu(per_user);
        wchar_t msg[256];
        if (e == ERROR_SUCCESS) {
            ::lstrcpyW(msg,
                L"Volchay-fastcut registered.\n\n"
                L"Right-click any video file -> 'Open with Volchay-fastcut'.\n"
                L"To remove: run 'volchay-register.exe unregister' from cmd.");
            ::MessageBoxW(nullptr, msg, L"Volchay-fastcut",
                          MB_ICONINFORMATION | MB_OK);
            return 0;
        } else {
            ::wsprintfW(msg, L"Registration failed (error %ld).", long(e));
            ::MessageBoxW(nullptr, msg, L"Volchay-fastcut",
                          MB_ICONERROR | MB_OK);
            return 2;
        }
    }

    if (::wcscmp(argv[1], L"register") == 0) {
        LONG e = volchay::shell::register_context_menu(per_user);
        if (e != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"register failed: %ld\n", long(e));
            return 2;
        }
        std::fwprintf(stdout,
            L"Registered (%s scope). Right-click any video file to open in "
            L"Volchay-fastcut.\n",
            per_user ? L"per-user" : L"machine");
        return 0;
    }
    if (::wcscmp(argv[1], L"unregister") == 0) {
        LONG e = volchay::shell::unregister_context_menu(per_user);
        if (e != ERROR_SUCCESS) {
            std::fwprintf(stderr, L"unregister failed: %ld\n", long(e));
            return 2;
        }
        std::fwprintf(stdout, L"Unregistered.\n");
        return 0;
    }
    print_usage();
    return 1;
}
