// Export pipeline. Wraps IMFSinkWriter to transcode the open file (or
// just its trim range) to an .mp4 using H.264, with hardware encoding
// preferred (NVENC / Quick Sync / AMF) via the MF_TRANSCODE_TOPOLOGY_*
// switches.
//
// V1 implementation: single-track passthrough — copies the input video's
// resolution & frame rate, re-encodes to H.264 at the chosen bitrate.
// Audio gets encoded with AAC LC at 192 kbps stereo if present.
//
// Runs on its own worker thread; UI polls progress() and finished().
#pragma once

#include "core/clip.h"
#include "platform/windows.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace volchay::media {

struct ExportPreset {
    const char* label;
    int         width;
    int         height;
    int         video_bitrate;     // bits per second.
    int         fps_num;
    int         fps_den;
    bool        prefer_hevc;
};

extern const ExportPreset kPresets[];
extern const int          kPresetCount;

struct ExportRequest {
    std::wstring source_path;
    std::wstring output_path;
    int          preset_index = 0;
    bool         hardware     = true;
    core::TimeUs trim_start_us = 0;
    core::TimeUs trim_end_us   = -1;     // -1 = full duration
};

class Exporter {
public:
    Exporter();
    ~Exporter();

    Exporter(const Exporter&)            = delete;
    Exporter& operator=(const Exporter&) = delete;

    bool start(const ExportRequest& req);
    void cancel();

    bool   busy() const     { return busy_.load();     }
    float  progress() const { return progress_.load(); }
    bool   finished() const { return finished_.load(); }
    bool   succeeded() const { return success_.load(); }
    std::string status_text() const;

private:
    void worker_main();
    void run_one_export(ExportRequest req);

    std::thread        worker_;
    std::atomic<bool>  quit_{false};
    std::atomic<bool>  busy_{false};
    std::atomic<bool>  cancel_{false};
    std::atomic<bool>  finished_{false};
    std::atomic<bool>  success_{false};
    std::atomic<float> progress_{0.0f};

    std::mutex                 mu_;
    std::string                status_;
    ExportRequest              pending_;
    bool                       has_pending_ = false;
};

}  // namespace volchay::media
