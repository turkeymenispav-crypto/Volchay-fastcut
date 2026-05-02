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

    Window                   window_;
    render::D3DContext       d3d_;
    core::Project            project_;
    media::MfPlayer          player_;
    media::AudioPlayer       audio_;
    media::Exporter          exporter_;
    ui::MainLayout           layout_;

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
};

}  // namespace volchay::app
