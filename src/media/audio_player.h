// WASAPI shared-mode audio playback driven by a parallel
// IMFSourceReader on the same media file. Lives on its own worker thread:
// the thread owns the COM apartment and the WASAPI client, the UI thread
// only signals open/close/seek/playing/volume through atomics + a
// condvar.
//
// Synchronisation model:
//   - The audio engine is the master clock when audio is available. It
//     runs at the WASAPI device rate (typically 48kHz) and reports its
//     own PTS via current_pts_us(); MfPlayer (video) catches up.
//   - When no audio stream is present, the engine is idle and the video
//     PTS drives the timeline as before.
#pragma once

#include "core/clip.h"
#include "platform/windows.h"
#include "util/com_ptr.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace volchay::media {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&)            = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Open the audio stream of `path`. Returns false if the file has no
    // audio or WASAPI rejects the requested format. Either case is
    // non-fatal; the caller falls back to silent video.
    bool open(const std::wstring& path);
    void close();

    bool has_audio() const   { return has_audio_.load(); }
    bool is_playing() const  { return playing_.load();   }

    void play();
    void pause();
    void seek(core::TimeUs t);

    void set_volume(float v);    // 0..1 linear.
    void set_muted(bool m);

    // Master clock. Returns the PTS of the sample currently mixed by
    // WASAPI, or -1 if audio isn't running.
    core::TimeUs current_pts_us() const { return current_pts_us_.load(); }

    // Total duration of the audio stream (microseconds). 0 if unknown.
    core::TimeUs duration_us() const    { return duration_us_.load();    }

private:
    void worker_main();
    bool worker_open();
    void worker_close();
    bool worker_render_pass();
    bool worker_decode_more();

    // Cross-thread state.
    std::thread             worker_;
    std::atomic<bool>       quit_{false};
    std::atomic<bool>       playing_{false};
    std::atomic<bool>       has_audio_{false};
    std::atomic<bool>       muted_{false};
    std::atomic<float>      volume_{1.0f};
    std::atomic<core::TimeUs> seek_target_{-1};
    std::atomic<core::TimeUs> current_pts_us_{-1};
    std::atomic<core::TimeUs> duration_us_{0};

    std::mutex              mu_;
    std::condition_variable cv_;
    std::wstring            pending_path_;
    bool                    open_requested_ = false;
    bool                    close_requested_ = false;
};

}  // namespace volchay::media
