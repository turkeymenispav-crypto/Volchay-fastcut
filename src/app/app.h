// Top-level application object. Owns the window, the renderer, the editing
// model, and the playback session. Drives the main loop.
#pragma once

#include "app/window.h"
#include "core/project.h"
#include "media/audio_player.h"
#include "media/exporter.h"
#include "media/mf_player.h"
#include "render/d3d_context.h"
#include "ui/main_layout.h"
#include "ui/thumbnail_cache.h"
#include "util/settings.h"
#include "util/timing.h"

#include <memory>
#include <string>

namespace volchay::app {

class App {
public:
    App();
    ~App();

    // Initialise the window + renderer + ImGui. Returns false on error.
    bool initialize(int show_cmd, StartupTrace& trace);

    // Optionally load a video given on the command line (e.g. when the
    // user invoked us via the Explorer context menu).
    void open_initial_video(const std::wstring& path);

    // Run the message + render loop until the window is closed.
    int run();

private:
    bool initialise_imgui();
    void render_one_frame();
    void advance_playhead(double dt_seconds);
    void open_video(const std::wstring& path);
    void rebuild_fonts_if_needed();

    // Multi-clip playback: ensure the AV decoder + audio are loaded with
    // the source media of the clip currently under the playhead. If the
    // playhead crossed a boundary into a clip with a different media,
    // re-open both with the new path and seek to the correct FILE PTS.
    void sync_media_to_playhead();

    // File > Extract audio. Writes the currently-open source's audio
    // to <basename>.m4a alongside it via media::Exporter (audio_only
    // request). Surfaces results to the user via a MessageBox.
    void extract_audio_to_sidecar();

    Window                   window_;
    render::D3DContext       d3d_;
    core::Project            project_;
    // Two video decoders: player_ (top track) + player_bot_ (the layer
    // immediately below the topmost at the playhead). Both run in
    // parallel so the viewer can composite V_n over V_{n-1}.
    media::MfPlayer          player_;
    media::MfPlayer          player_bot_;
    media::AudioPlayer       audio_;
    media::Exporter          exporter_;
    ui::MainLayout           layout_;
    ui::ThumbnailCache       thumbs_;

    Settings                 settings_;
    bool                     fonts_dirty_   = true;
    bool                     show_settings_ = false;
    bool                     show_export_   = false;
    bool                     show_diagnostics_ = false;

    StartupTrace*            trace_ = nullptr;
    HiResClock               wall_clock_;
    double                   last_frame_time_s_ = 0.0;
    double                   last_frame_ms_     = 0.0;
    bool                     was_playing_       = false;  // change-detector for audio play/pause

    // Media currently loaded into player_/audio_. We track this so the
    // multi-clip playback path knows when it has to re-open with a
    // different file — e.g. as the playhead crosses from a clip
    // referencing video A into a clip referencing video B.
    std::wstring             current_media_path_;
    std::wstring             current_media_path_bot_;
};

}  // namespace volchay::app
