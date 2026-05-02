// Loads UI fonts from the local Windows Fonts directory and configures
// ImGui's font atlas accordingly. Settings (size, AA, oversampling)
// come from the user's Settings.
#pragma once

#include "util/settings.h"

namespace volchay::ui {

// Build / rebuild the ImGui font atlas to match `s`. Safe to call any
// time before NewFrame(). Returns true on success.
//
// Font search order tries Segoe UI Variable first (modern Win11 default),
// then Segoe UI, then Tahoma. If none can be opened the built-in
// ProggyClean font is used and a warning is logged.
bool rebuild_fonts(const Settings& s, float dpi_scale);

// Recreate the device-side font texture after a rebuild. Must be called
// before the next ImGui NewFrame().
void invalidate_font_texture();

}  // namespace volchay::ui
