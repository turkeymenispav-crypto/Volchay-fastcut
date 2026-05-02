// High-resolution timing helpers backed by QueryPerformanceCounter.
// Used to measure cold-start latency.
#pragma once

#include "platform/windows.h"

#include <cstdint>

namespace volchay {

class HiResClock {
public:
    HiResClock() noexcept {
        ::QueryPerformanceFrequency(&freq_);
        ::QueryPerformanceCounter(&start_);
    }

    // Reset the start point.
    void reset() noexcept { ::QueryPerformanceCounter(&start_); }

    // Elapsed seconds since construction or last reset().
    double seconds() const noexcept {
        LARGE_INTEGER now;
        ::QueryPerformanceCounter(&now);
        return double(now.QuadPart - start_.QuadPart) / double(freq_.QuadPart);
    }

    double millis() const noexcept { return seconds() * 1000.0; }

private:
    LARGE_INTEGER freq_{};
    LARGE_INTEGER start_{};
};

// Records milestones during application startup so we can dump a timeline
// to the log and identify slow phases.
class StartupTrace {
public:
    StartupTrace() = default;

    void mark(const char* name) noexcept {
        if (count_ < kMax) {
            entries_[count_].name   = name;
            entries_[count_].millis = clock_.millis();
            ++count_;
        }
    }

    struct Entry {
        const char* name = nullptr;
        double      millis = 0.0;
    };

    int count() const noexcept { return count_; }
    const Entry& at(int i) const noexcept { return entries_[i]; }
    double total_millis() const noexcept { return clock_.millis(); }

private:
    static constexpr int kMax = 32;
    HiResClock clock_;
    Entry      entries_[kMax];
    int        count_ = 0;
};

}  // namespace volchay
