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
    if (argc < 2) {
        print_usage();
        return 1;
    }
    bool machine = false;
    for (int i = 2; i < argc; ++i) {
        if (::wcscmp(argv[i], L"--machine") == 0) machine = true;
    }
    bool per_user = !machine;

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
