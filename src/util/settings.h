// Persistent user settings. Stored as a tiny key=value text file under
// %LOCALAPPDATA%\Volchay\settings.ini and reloaded on every launch.
//
// The settings struct is a plain POD; the panel UI just mutates it and
// then calls save(). Defaults are tuned for "looks great on Windows 11
// at 100% DPI on a typical workstation".
#pragma once

#include <string>

namespace volchay {

struct Settings {
    // Typography.
    float font_size_px      = 15.0f;     // Base UI font size in pixels.
    bool  font_anti_alias   = true;      // Greyscale AA in the atlas.
    bool  font_subpixel     = false;     // Off by default (heavier per-glyph).
    bool  font_oversample_h = true;      // 2x horizontal oversampling.
    int   font_choice       = 0;         // 0 = Segoe UI Variable, 1 = Segoe UI,
                                         // 2 = Inter (bundled fallback).

    // Rendering.
    bool  vsync             = false;     // Present(1, 0) vs Present(0, 0).
                                         // Off by default for snappy
                                         // timeline scrubbing on a powerful
                                         // GPU; users on weaker hardware
                                         // can re-enable in Settings.
    bool  hardware_decode   = true;      // DXVA via D3D11 device manager.
    bool  smooth_lines      = true;      // ImGui anti-aliased lines/fills.

    // Playback.
    float audio_volume      = 1.0f;      // 0..1 linear gain.
    bool  audio_mute        = false;
    bool  loop_playback     = false;

    // Cold-start optimisation switches (the "low-end mode" the user asked for).
    // When low_end_mode is enabled we force-disable everything that costs
    // GPU/CPU at idle: font AA, smooth lines, oversample, etc.
    bool  low_end_mode      = false;
};

class SettingsStore {
public:
    // Load from %LOCALAPPDATA%\Volchay\settings.ini. If the file doesn't
    // exist, returns built-in defaults.
    static Settings load();

    // Persist atomically (write to settings.ini.tmp, rename).
    static bool save(const Settings& s);

    // Resolved absolute path (UTF-8). Empty if SHGetKnownFolderPath failed.
    static std::string path();
};

// Apply low-end-mode overrides on top of a settings struct without
// rewriting the file.
Settings effective(const Settings& s);

}  // namespace volchay
