// Visual theme. "Luna Sky" — a modern editor look, deep slate-blue
// surface with a soft sky-blue accent. Colours are kept in one
// place so every panel pulls from the same tokens instead of
// hard-coding ImVec4s.
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
    ImVec4 separator_hover   {0.302f, 0.816f, 0.882f, 1.0f};   // accent

    // Text.
    ImVec4 text              {0.910f, 0.918f, 0.945f, 1.0f};   // #E8EAF1
    ImVec4 text_dim          {0.560f, 0.578f, 0.640f, 1.0f};   // #8E94A4
    ImVec4 text_disabled     {0.380f, 0.396f, 0.448f, 1.0f};

    // Accent — Luna Sky aqua-cyan (#4DD0E1). Distinct from the
    // Microsoft system blue, sits between cyan and teal.
    ImVec4 accent            {0.302f, 0.816f, 0.882f, 1.0f};   // #4DD0E1
    ImVec4 accent_hover      {0.420f, 0.870f, 0.918f, 1.0f};   // #6BDDEA
    ImVec4 accent_active     {0.180f, 0.690f, 0.770f, 1.0f};   // #2EB0C4

    // Timeline-specific.
    ImVec4 timeline_bg       {0.046f, 0.054f, 0.078f, 1.0f};
    ImVec4 timeline_grid     {0.166f, 0.193f, 0.252f, 1.0f};
    ImVec4 timeline_grid_sub {0.103f, 0.121f, 0.162f, 1.0f};
    ImVec4 timeline_clip     {0.130f, 0.300f, 0.340f, 1.0f};   // muted teal
    ImVec4 timeline_clip_sel {0.302f, 0.816f, 0.882f, 1.0f};   // accent
    ImVec4 timeline_playhead {0.560f, 0.920f, 0.960f, 1.0f};   // bright aqua

    float corner_radius   = 6.0f;
    float frame_padding_y = 7.0f;
    float item_spacing_y  = 7.0f;
};

const Theme& theme();

// Apply the theme to ImGui's current style.
void apply_theme();

}  // namespace volchay::ui
