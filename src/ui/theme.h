// Visual theme. "Luna" — a modern editor look, deep slate-blue
// surface with a soft violet accent. Colours are kept in one place
// so every panel pulls from the same tokens instead of hard-coding
// ImVec4s.
#pragma once

#include <imgui.h>

namespace volchay::ui {

struct Theme {
    // Backgrounds (darkest to lightest).
    ImVec4 bg_window         {0.054f, 0.064f, 0.090f, 1.0f};   // #0E1117 ish, slate-blue
    ImVec4 bg_panel          {0.086f, 0.104f, 0.140f, 1.0f};   // #161B23
    ImVec4 bg_panel_alt      {0.110f, 0.131f, 0.171f, 1.0f};   // #1C222C
    ImVec4 bg_header         {0.140f, 0.165f, 0.214f, 1.0f};   // #232936
    ImVec4 bg_input          {0.041f, 0.052f, 0.078f, 1.0f};   // #0B0E14
    ImVec4 bg_button         {0.150f, 0.180f, 0.232f, 1.0f};
    ImVec4 bg_button_hover   {0.198f, 0.235f, 0.299f, 1.0f};
    ImVec4 bg_button_active  {0.250f, 0.293f, 0.366f, 1.0f};
    ImVec4 separator         {0.020f, 0.026f, 0.040f, 1.0f};   // hairline divider
    ImVec4 separator_hover   {0.486f, 0.557f, 0.973f, 1.0f};   // accent

    // Text.
    ImVec4 text              {0.910f, 0.918f, 0.945f, 1.0f};   // #E8EAF1
    ImVec4 text_dim          {0.560f, 0.578f, 0.640f, 1.0f};   // #8E94A4
    ImVec4 text_disabled     {0.380f, 0.396f, 0.448f, 1.0f};

    // Accent — "Luna" soft violet-blue (#7C8EF8).
    ImVec4 accent            {0.486f, 0.557f, 0.973f, 1.0f};
    ImVec4 accent_hover      {0.583f, 0.651f, 0.984f, 1.0f};
    ImVec4 accent_active     {0.388f, 0.451f, 0.847f, 1.0f};

    // Timeline-specific.
    ImVec4 timeline_bg       {0.046f, 0.054f, 0.078f, 1.0f};
    ImVec4 timeline_grid     {0.166f, 0.193f, 0.252f, 1.0f};
    ImVec4 timeline_grid_sub {0.103f, 0.121f, 0.162f, 1.0f};
    ImVec4 timeline_clip     {0.181f, 0.215f, 0.401f, 1.0f};   // muted violet-blue
    ImVec4 timeline_clip_sel {0.486f, 0.557f, 0.973f, 1.0f};   // accent
    ImVec4 timeline_playhead {0.710f, 0.770f, 1.000f, 1.0f};   // bright violet

    float corner_radius   = 6.0f;
    float frame_padding_y = 7.0f;
    float item_spacing_y  = 7.0f;
};

const Theme& theme();

// Apply the theme to ImGui's current style.
void apply_theme();

}  // namespace volchay::ui
