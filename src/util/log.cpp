#include "util/log.h"

#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <mutex>

namespace volchay::log {
namespace {

std::mutex  g_mu;
HANDLE      g_file = INVALID_HANDLE_VALUE;
std::string g_path_utf8;

const char* level_name(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Info : return "INFO ";
        case Level::Warn : return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?    ";
}

}  // namespace

void init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) return;

    PWSTR roaming = nullptr;
    if (::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &roaming) != S_OK || !roaming) {
        return;
    }
    std::wstring dir = roaming;
    ::CoTaskMemFree(roaming);
    dir += L"\\Volchay";
    ::CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring path = dir + L"\\fastcut.log";
    g_path_utf8 = volchay::narrow(path);

    g_file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;

    // Header banner so log files clearly mark each launch.
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char banner[160];
    int n = ::snprintf(banner, sizeof(banner),
        "\r\n=== Volchay-fastcut " VOLCHAY_VERSION_STRING
        " launched %04u-%02u-%02u %02u:%02u:%02u ===\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD wrote = 0;
    ::WriteFile(g_file, banner, (DWORD)n, &wrote, nullptr);
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

const std::string& path() { return g_path_utf8; }

void write(Level lvl, const char* fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = ::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    SYSTEMTIME st;
    ::GetLocalTime(&st);

    char line[2200];
    int  m = ::snprintf(line, sizeof(line),
        "[%02u:%02u:%02u.%03u] %s %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        level_name(lvl), body);
    if (m < 0) return;

    if (::IsDebuggerPresent()) {
        ::OutputDebugStringA(line);
    }

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        ::WriteFile(g_file, line, (DWORD)m, &wrote, nullptr);
    }
}

}  // namespace volchay::log
