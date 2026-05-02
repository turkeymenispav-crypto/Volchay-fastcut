#include "ui/theme.h"

namespace volchay::ui {
namespace {
Theme g_theme;
}

const Theme& theme() { return g_theme; }

void apply_theme() {
    const Theme& t = g_theme;
    ImGuiStyle& s = ImGui::GetStyle();

    // Layout / shape.
    s.WindowRounding    = 0.0f;          // panels feel anchored, not floating
    s.ChildRounding     = t.corner_radius;
    s.FrameRounding     = t.corner_radius;
    s.PopupRounding     = t.corner_radius;
    s.ScrollbarRounding = t.corner_radius;
    s.GrabRounding      = t.corner_radius;
    s.TabRounding       = 4.0f;

    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.PopupBorderSize   = 1.0f;

    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, t.frame_padding_y);
    s.ItemSpacing       = ImVec2(8, t.item_spacing_y);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.IndentSpacing     = 16;
    s.ScrollbarSize     = 12;
    s.GrabMinSize       = 10;

    // Colors.
    auto C = [&](ImGuiCol idx, ImVec4 c) { s.Colors[idx] = c; };

    C(ImGuiCol_Text,                  t.text);
    C(ImGuiCol_TextDisabled,          t.text_disabled);
    C(ImGuiCol_WindowBg,              t.bg_window);
    C(ImGuiCol_ChildBg,               t.bg_panel);
    C(ImGuiCol_PopupBg,               t.bg_panel_alt);

    C(ImGuiCol_Border,                t.separator);
    C(ImGuiCol_BorderShadow,          ImVec4(0,0,0,0));

    C(ImGuiCol_FrameBg,               t.bg_input);
    C(ImGuiCol_FrameBgHovered,        ImVec4(t.bg_input.x*1.4f, t.bg_input.y*1.4f, t.bg_input.z*1.4f, 1.0f));
    C(ImGuiCol_FrameBgActive,         ImVec4(t.bg_input.x*1.8f, t.bg_input.y*1.8f, t.bg_input.z*1.8f, 1.0f));

    C(ImGuiCol_TitleBg,               t.bg_header);
    C(ImGuiCol_TitleBgActive,         t.bg_header);
    C(ImGuiCol_TitleBgCollapsed,      t.bg_header);

    C(ImGuiCol_MenuBarBg,             t.bg_panel_alt);

    C(ImGuiCol_ScrollbarBg,           t.bg_panel);
    C(ImGuiCol_ScrollbarGrab,         t.bg_button);
    C(ImGuiCol_ScrollbarGrabHovered,  t.bg_button_hover);
    C(ImGuiCol_ScrollbarGrabActive,   t.bg_button_active);

    C(ImGuiCol_CheckMark,             t.accent);
    C(ImGuiCol_SliderGrab,            t.accent);
    C(ImGuiCol_SliderGrabActive,      t.accent_active);

    C(ImGuiCol_Button,                t.bg_button);
    C(ImGuiCol_ButtonHovered,         t.bg_button_hover);
    C(ImGuiCol_ButtonActive,          t.bg_button_active);

    C(ImGuiCol_Header,                t.bg_button);
    C(ImGuiCol_HeaderHovered,         t.bg_button_hover);
    C(ImGuiCol_HeaderActive,          t.bg_button_active);

    C(ImGuiCol_Separator,             t.separator);
    C(ImGuiCol_SeparatorHovered,      t.separator_hover);
    C(ImGuiCol_SeparatorActive,       t.accent);

    C(ImGuiCol_ResizeGrip,            ImVec4(0,0,0,0));
    C(ImGuiCol_ResizeGripHovered,     t.accent);
    C(ImGuiCol_ResizeGripActive,      t.accent_active);

    C(ImGuiCol_Tab,                   t.bg_panel_alt);
    C(ImGuiCol_TabHovered,            t.bg_button_hover);
    C(ImGuiCol_TabSelected,           t.bg_panel);
    C(ImGuiCol_TabDimmed,             t.bg_panel_alt);
    C(ImGuiCol_TabDimmedSelected,     t.bg_panel);

    C(ImGuiCol_DockingPreview,        ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.45f));
    C(ImGuiCol_DockingEmptyBg,        t.bg_window);

    C(ImGuiCol_TextSelectedBg,        ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.35f));
    C(ImGuiCol_DragDropTarget,        t.accent);

    C(ImGuiCol_NavCursor,             t.accent);
    C(ImGuiCol_NavWindowingHighlight, ImVec4(t.text.x, t.text.y, t.text.z, 0.55f));
    C(ImGuiCol_NavWindowingDimBg,     ImVec4(0, 0, 0, 0.55f));
    C(ImGuiCol_ModalWindowDimBg,      ImVec4(0, 0, 0, 0.65f));

    C(ImGuiCol_PlotLines,             t.text_dim);
    C(ImGuiCol_PlotLinesHovered,      t.accent);
    C(ImGuiCol_PlotHistogram,         t.accent);
    C(ImGuiCol_PlotHistogramHovered,  t.accent_hover);
}

}  // namespace volchay::ui
