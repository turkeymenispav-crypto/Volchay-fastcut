// Tiny logging facility. Writes to %LOCALAPPDATA%\Volchay\fastcut.log and,
// when a debugger is attached, to OutputDebugString. Designed to be cheap
// when no log file is open (no string formatting on the hot path).
#pragma once

#include "platform/windows.h"

#include <cstdarg>
#include <string>

namespace volchay::log {

enum class Level : int { Trace, Info, Warn, Error };

// Open the log file (creates %LOCALAPPDATA%\Volchay if needed).
// Safe to call multiple times; subsequent calls are no-ops.
void init();
void shutdown();

void write(Level lvl, const char* fmt, ...);

// Convenience wrappers.
inline void info (const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024];
    ::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(Level::Info, "%s", buf);
}
inline void warn (const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024];
    ::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(Level::Warn, "%s", buf);
}
inline void err (const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024];
    ::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(Level::Error, "%s", buf);
}

// Path to the current log file (UTF-8). Empty until init() succeeds.
const std::string& path();

}  // namespace volchay::log
