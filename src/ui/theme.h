// Visual theme. A near-black workspace inspired by Adobe Lightroom's
// editing surface, paired with the warm Claude-style orange accent
// (#D97757) the user requested. We keep the palette in one place so
// every panel can pull from the same tokens instead of hard-coding
// ImVec4s.
#pragma once

#include <imgui.h>

namespace volchay::ui {

struct Theme {
    // Backgrounds (darkest to lightest).
    ImVec4 bg_window         {0.080f, 0.080f, 0.085f, 1.0f};
    ImVec4 bg_panel          {0.115f, 0.115f, 0.122f, 1.0f};
    ImVec4 bg_panel_alt      {0.145f, 0.145f, 0.152f, 1.0f};
    ImVec4 bg_header         {0.180f, 0.180f, 0.190f, 1.0f};
    ImVec4 bg_input          {0.060f, 0.060f, 0.066f, 1.0f};
    ImVec4 bg_button         {0.205f, 0.205f, 0.215f, 1.0f};
    ImVec4 bg_button_hover   {0.260f, 0.260f, 0.275f, 1.0f};
    ImVec4 bg_button_active  {0.310f, 0.310f, 0.325f, 1.0f};
    ImVec4 separator         {0.030f, 0.030f, 0.035f, 1.0f};
    ImVec4 separator_hover   {0.851f, 0.467f, 0.341f, 1.0f};

    // Text.
    ImVec4 text              {0.870f, 0.870f, 0.870f, 1.0f};
    ImVec4 text_dim          {0.560f, 0.560f, 0.570f, 1.0f};
    ImVec4 text_disabled     {0.380f, 0.380f, 0.390f, 1.0f};

    // Accent — Claude-style warm orange (#D97757).
    ImVec4 accent            {0.851f, 0.467f, 0.341f, 1.0f};
    ImVec4 accent_hover      {0.910f, 0.540f, 0.420f, 1.0f};
    ImVec4 accent_active     {0.760f, 0.395f, 0.275f, 1.0f};

    // Timeline-specific.
    ImVec4 timeline_bg       {0.085f, 0.085f, 0.090f, 1.0f};
    ImVec4 timeline_grid     {0.190f, 0.190f, 0.205f, 1.0f};
    ImVec4 timeline_grid_sub {0.140f, 0.140f, 0.150f, 1.0f};
    ImVec4 timeline_clip     {0.330f, 0.260f, 0.225f, 1.0f};   // muted brown-orange
    ImVec4 timeline_clip_sel {0.851f, 0.467f, 0.341f, 1.0f};   // accent orange
    ImVec4 timeline_playhead {0.965f, 0.620f, 0.250f, 1.0f};   // bright orange

    float corner_radius   = 3.0f;
    float frame_padding_y = 6.0f;
    float item_spacing_y  = 6.0f;
};

const Theme& theme();

// Apply the theme to ImGui's current style.
void apply_theme();

}  // namespace volchay::ui
