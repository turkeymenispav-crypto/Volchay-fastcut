// FFmpeg/libav-based exporter. Reads the open file via libavformat,
// re-encodes video (H.264 or HEVC, hardware preferred) and audio (AAC),
// and muxes into .mp4. Replaces the previous IMFSinkWriter pipeline,
// which was choking on AV1 + HEVC sources.
//
// Runs on its own worker thread; UI polls progress() and finished().
#pragma once

#include "core/clip.h"
#include "platform/windows.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace volchay::core { class Project; }

namespace volchay::media {

enum class ExportCodec {
    H264,
    HEVC,
};

// All numeric resolution/fps/bitrate fields default to "source": copy
// from the input. Set them non-zero to override.
struct ExportRequest {
    std::wstring source_path;
    std::wstring output_path;
    ExportCodec  codec         = ExportCodec::H264;
    int          width         = 0;          // 0 = source
    int          height        = 0;          // 0 = source
    int          fps_num       = 0;          // 0 = source
    int          fps_den       = 1;
    int          video_bitrate = 12'000'000; // bps
    int          audio_bitrate = 192'000;    // bps
    bool         hardware      = true;       // try GPU encoder first
    // Aspect-ratio override. When > 0, the source frame is centre-
    // cropped to this ratio before scale + encode (matches the
    // viewer's "AR:" combo). 0 = no crop, keep source aspect.
    double       aspect_ratio  = 0.0;
    core::TimeUs trim_start_us = 0;
    core::TimeUs trim_end_us   = -1;         // -1 = full duration

    // When replace_source is set the exporter writes to output_path
    // (the caller is expected to make this a tmp file alongside the
    // original) and on success the UI is responsible for swapping
    // the temp file in for `replace_final_path` (deleting the
    // original first). The exporter itself never deletes any file.
    bool         replace_source     = false;
    std::wstring replace_final_path;

    // Audio-only export: skip the video stream entirely and write a
    // bare AAC track inside an .m4a container at output_path. All
    // video fields (codec/width/height/fps/bitrate/aspect_ratio) are
    // ignored. Used by File > Extract audio.
    bool         audio_only         = false;

    // Per-clip transform for the (currently single-source) export
    // pipeline. Carries the active clip's transform so the encoded
    // file matches what's shown in the viewer:
    //   xform_scale > 0 zooms the picture (1.0 = no scale).
    //   xform_pos_x / xform_pos_y are in [-1..1], 0 = centred. Outside
    //   the picture rectangle the canvas is filled with black.
    float        xform_scale  = 1.0f;
    float        xform_pos_x  = 0.0f;
    float        xform_pos_y  = 0.0f;

    // Project pointer. When set, the exporter walks the timeline at
    // output FPS and composites every track + clip transform into the
    // canvas (rather than re-encoding a single source). Audio is
    // sourced from the lowest-track clip covering the trim range.
    // Owned by the caller; must outlive the export.
    const core::Project* project = nullptr;
};

// A small label library the UI uses to populate the codec / resolution
// / fps combos.
struct ExportPreset {
    const char* label;
    int         width;
    int         height;
    int         video_bitrate;
    int         fps_num;
    int         fps_den;
    bool        prefer_hevc;
};

extern const ExportPreset kPresets[];
extern const int          kPresetCount;

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

    // Snapshot of the request that just finished (only valid when
    // finished() is true). Used by the UI to know the source/temp/
    // final paths after a replace_source export completes.
    ExportRequest last_request() const;
    void          mark_replaced();    // UI clears the replace flag after
                                      // performing the swap.

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
    ExportRequest              last_;
};

}  // namespace volchay::media
