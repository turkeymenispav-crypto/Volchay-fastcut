// Editing model for a single clip on the timeline. Time values are stored
// in microseconds (signed 64-bit) which is enough for ~292,000 years and
// matches Media Foundation's MFTIME / 100ns reference time naturally.
#pragma once

#include <cstdint>
#include <string>

namespace volchay::core {

using TimeUs = std::int64_t;

constexpr TimeUs kSecond = 1'000'000;
constexpr TimeUs kMillis = 1'000;

inline constexpr TimeUs from_seconds(double s) {
    return TimeUs(s * 1'000'000.0);
}
inline constexpr double to_seconds(TimeUs t) {
    return double(t) / 1'000'000.0;
}

// Source media file; immutable once registered with the project.
struct Media {
    std::string  id;          // stable id, currently the absolute path
    std::wstring path;        // absolute path on disk
    std::wstring display;     // basename for UI
    TimeUs       duration = 0;
    int          width    = 0;
    int          height   = 0;
    double       fps      = 0.0;
    bool         probed   = false;
};

// A clip places a region of a source media onto the timeline.
//
//   |---- media (source) ----|
//        ^src_in       ^src_out
//                  becomes
//   timeline:  ...|---- clip ----|...
//                ^t_in           ^t_out  (t_out = t_in + (src_out - src_in)/speed)
struct Clip {
    std::string id;            // stable id
    std::string media_id;      // -> Media.id

    TimeUs  src_in   = 0;      // start offset inside source
    TimeUs  src_out  = 0;      // end offset inside source

    TimeUs  t_in     = 0;      // position on the timeline

    double  speed    = 1.0;    // playback rate (1.0 = real time)
    float   volume   = 1.0f;   // linear gain, 1.0 = unchanged
    bool    muted    = false;
    bool    selected = false;  // UI selection state (transient, not saved)

    TimeUs duration_on_timeline() const {
        TimeUs src_dur = src_out - src_in;
        if (src_dur < 0) src_dur = 0;
        if (speed <= 0.0) return src_dur;
        return TimeUs(double(src_dur) / speed);
    }
    TimeUs t_out() const { return t_in + duration_on_timeline(); }
};

}  // namespace volchay::core
