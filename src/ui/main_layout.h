// Top-level docking layout: assembles the menu bar, status strip, and the
// four primary panels (Library, Viewer, Timeline, Inspector). Lays them
// out in a Lightroom-like arrangement on first run.
#pragma once

#include "core/project.h"
#include "media/mf_player.h"
#include "util/settings.h"
#include "util/timing.h"

#include <functional>
#include <string>

namespace volchay::media { class AudioPlayer; }
namespace volchay::media { class Exporter;    }

namespace volchay::ui {

// Aggregated state passed to every panel each frame.
struct EditorContext {
    core::Project*           project   = nullptr;
    media::MfPlayer*         player    = nullptr;
    media::AudioPlayer*      audio     = nullptr;
    media::Exporter*         exporter  = nullptr;
    StartupTrace*            startup   = nullptr;
    Settings*                settings  = nullptr;

    // Window flags toggled from the View menu, owned by MainLayout.
    bool* show_settings   = nullptr;
    bool* show_export     = nullptr;
    bool* show_diagnostics = nullptr;

    // Callback invoked when the user requests opening a file via the
    // menu / drag&drop. Implemented by App.
    std::function<void(const std::wstring& path)> open_file;
    std::function<void()> exit_app;

    // Wall-clock since app launched, for status bar.
    double session_seconds = 0.0;
    double last_frame_ms   = 0.0;
};

class MainLayout {
public:
    MainLayout();

    // Lay out and draw all panels for this frame.
    void render(EditorContext& ctx);

    // True iff user has requested exit from the menu.
    bool exit_requested() const { return exit_requested_; }

    // Set by the Settings panel when a font/AA option changed and the
    // atlas needs to be rebuilt before the next frame. Consumed by App.
    bool settings_atlas_dirty() const {
        bool v = settings_atlas_dirty_;
        settings_atlas_dirty_ = false;
        return v;
    }

private:
    void draw_menu_bar(EditorContext& ctx);
    void draw_status_strip(EditorContext& ctx);
    void install_dock_layout(unsigned dockspace_id);

    bool first_layout_done_ = false;
    bool exit_requested_    = false;
    bool show_about_        = false;
    bool show_startup_log_  = false;
    mutable bool settings_atlas_dirty_ = false;
};

}  // namespace volchay::ui
