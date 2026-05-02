// Settings panel: lets the user toggle anti-aliasing, vsync, hardware
// decode, and other knobs at runtime. Writes back to the SettingsStore
// on every change.
#pragma once

#include "ui/main_layout.h"

namespace volchay::ui::panels {

// Returns true if the user changed a setting that requires the font
// atlas / theme to be rebuilt before the next frame.
bool draw_settings(EditorContext& ctx);

}  // namespace volchay::ui::panels
