// Entry point. Kept tiny to keep cold start low: we record a startup
// trace, initialise COM, parse the command line, then hand off to App.
#include "app/app.h"
#include "platform/windows.h"
#include "util/log.h"
#include "util/timing.h"

#include <string>
#include <vector>

namespace {

// Parse the Win32 command line into a vector of argv-style strings.
std::vector<std::wstring> split_command_line(LPWSTR cmdline) {
    std::vector<std::wstring> out;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(cmdline ? cmdline : L"", &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    ::LocalFree(argv);
    return out;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdline, int show_cmd) {
    volchay::StartupTrace trace;
    trace.mark("wWinMain entered");

    volchay::log::init();
    trace.mark("log init");

    HRESULT hr = ::CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        volchay::log::err("CoInitializeEx failed 0x%08lx", long(hr));
        return 1;
    }
    trace.mark("co init");

    // Per-monitor DPI v2 for crisp rendering on hi-DPI displays.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    auto args = split_command_line(cmdline);
    std::wstring initial_path;
    for (const auto& a : args) {
        if (!a.empty() && a[0] != L'-' && a[0] != L'/') {
            initial_path = a;
            break;
        }
    }

    volchay::app::App app;
    if (!app.initialize(show_cmd, trace)) {
        volchay::log::err("App::initialize failed");
        return 2;
    }
    if (!initial_path.empty()) {
        app.open_initial_video(initial_path);
    }

    // Lock the cold-start value before entering the main loop so the
    // status bar shows a constant launch time, not the running session
    // wall-clock.
    trace.freeze();

    int exit_code = app.run();

    ::CoUninitialize();
    volchay::log::info("Exit %d", exit_code);
    volchay::log::shutdown();
    return exit_code;
}
