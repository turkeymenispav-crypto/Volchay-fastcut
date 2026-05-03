// Custom pill-shaped buttons with a subtle vertical gradient and an
// accent border that glows on hover. Built on top of ImGui's
// invisible-button hit testing so the rest of the codebase can keep
// using these like ImGui::Button.
#pragma once

#include <imgui.h>

namespace volchay::ui {

enum class ButtonStyle {
    Normal,    // raised panel — the default everywhere a normal Button would go
    Primary,   // accent-coloured (call-to-action: Start export, Export top-right)
    Ghost,     // transparent until hover (toolbar / icon-style)
    Danger,    // red-tinted (destructive: Cancel, Close, Replace)
};

// Drop-in replacement for ImGui::Button. Returns true on click.
// size = {0,0} → auto-size from label + frame padding.
bool pill_button(const char* label,
                 ImVec2 size = ImVec2(0, 0),
                 ButtonStyle style = ButtonStyle::Normal,
                 bool enabled = true);

// Convenience: a square icon-only ghost button (e.g. close X, fullscreen
// arrows). Same hit-area logic as pill_button.
bool icon_button(const char* glyph,
                 float size,
                 ButtonStyle style = ButtonStyle::Ghost);

}  // namespace volchay::ui
