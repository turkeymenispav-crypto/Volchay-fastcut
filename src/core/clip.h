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
//
// The track index is layered: V0 / V1 / V2 / ... — higher indices are
// composited over lower ones (CapCut convention). Negative values mean
// "audio-only" tracks (A0, A1, ...) and use abs(idx) - 1 as the audio
// track index. Most clips with video implicitly carry their audio on
// the same track (two-clip-per-source isn't needed at this stage).
struct Clip {
    std::string id;            // stable id
    std::string media_id;      // -> Media.id

    int     track    = 0;      // 0 = main video; higher = overlay tracks

    TimeUs  src_in   = 0;      // start offset inside source
    TimeUs  src_out  = 0;      // end offset inside source

    TimeUs  t_in     = 0;      // position on the timeline

    double  speed    = 1.0;    // playback rate (1.0 = real time)
    float   volume   = 1.0f;   // linear gain, 1.0 = unchanged
    bool    muted    = false;
    bool    selected = false;  // UI selection state (transient, not saved)
    float   opacity  = 1.0f;   // 0..1, only meaningful on overlay tracks

    // Per-clip 2D transform applied in BOTH preview compositing and
    // export. The transform is described in NORMALISED coordinates
    // relative to the output frame:
    //   scale = 1.0 means the picture fills the output as if it were
    //           a top-track CapCut layer with no transform.
    //   pos_x, pos_y are in [-1..1]; (0,0) keeps the picture centred,
    //           (+1, 0) shifts the picture by one full output width to
    //           the right (i.e. fully off-screen). The viewer's corner
    //           handles edit scale; dragging the body edits pos_*.
    float   scale    = 1.0f;
    float   pos_x    = 0.0f;   // -1..1, 0 = centred
    float   pos_y    = 0.0f;   // -1..1, 0 = centred

    TimeUs duration_on_timeline() const {
        TimeUs src_dur = src_out - src_in;
        if (src_dur < 0) src_dur = 0;
        if (speed <= 0.0) return src_dur;
        return TimeUs(double(src_dur) / speed);
    }
    TimeUs t_out() const { return t_in + duration_on_timeline(); }
};

// Transitions live "on a clip boundary" — i.e. between clip[i] and
// clip[i+1] in timeline order. The model is intentionally minimal for
// V2: kind + duration. Real GPU rendering during playback / export is
// a follow-up; for now the timeline panel draws a bevel at the
// boundary so the editor can author them.
enum class TransitionKind {
    None     = 0,
    Cut      = 1,   // hard cut (no actual blend) — explicit "no transition"
    FadeIn   = 2,   // fade from black at the head of the next clip
    FadeOut  = 3,   // fade to   black at the tail of the previous clip
    CrossFade = 4,  // dissolve between two clips
    DipToBlack = 5, // fade out then in
    Wipe     = 6,   // simple horizontal wipe
};

struct Transition {
    // After-clip index: this transition sits at the boundary between
    // clips()[after_clip] and clips()[after_clip+1]. -1 = no anchor yet
    // (template entry from the Transitions panel before the user drops
    // it onto a boundary).
    int             after_clip = -1;
    TransitionKind  kind       = TransitionKind::CrossFade;
    TimeUs          duration   = 500'000;   // 0.5s default
};

}  // namespace volchay::core
