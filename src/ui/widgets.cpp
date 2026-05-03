#include "ui/widgets.h"

#include "ui/theme.h"

#include <imgui_internal.h>

namespace volchay::ui {
namespace {

inline ImU32 to_u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

inline ImVec4 with_alpha(ImVec4 c, float a) { c.w = a; return c; }

inline ImVec4 scale(ImVec4 c, float k) {
    return ImVec4(c.x * k, c.y * k, c.z * k, c.w);
}

inline ImVec4 lerp(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

struct Pal {
    ImVec4 fill_top;
    ImVec4 fill_bot;
    ImVec4 border_idle;
    ImVec4 border_hover;
    ImVec4 text;
};

Pal palette(ButtonStyle s, bool hovered, bool held, bool enabled) {
    const Theme& t = theme();
    Pal p;
    p.text = t.text;

    switch (s) {
    case ButtonStyle::Primary: {
        ImVec4 a  = t.accent;
        ImVec4 hi = scale(a, 1.18f);
        ImVec4 lo = scale(a, 0.78f);
        p.fill_top = held ? lo : (hovered ? scale(hi, 1.06f) : hi);
        p.fill_bot = held ? scale(hi, 0.85f) : (hovered ? scale(lo, 1.10f) : lo);
        p.border_idle  = with_alpha(a, 0.0f);
        p.border_hover = ImVec4(1, 1, 1, 0.22f);
        p.text = ImVec4(1, 1, 1, 1);
        break;
    }
    case ButtonStyle::Ghost: {
        ImVec4 base = t.bg_button;
        const float a = held ? 0.34f : (hovered ? 0.20f : 0.0f);
        p.fill_top = with_alpha(base, a);
        p.fill_bot = p.fill_top;
        p.border_idle  = with_alpha(t.text_dim, 0.0f);
        p.border_hover = with_alpha(t.accent,   0.55f);
        p.text = hovered ? t.text : t.text_dim;
        break;
    }
    case ButtonStyle::Danger: {
        ImVec4 r    = ImVec4(0.92f, 0.34f, 0.34f, 1.0f);
        ImVec4 r_hi = ImVec4(0.98f, 0.46f, 0.46f, 1.0f);
        ImVec4 r_lo = ImVec4(0.74f, 0.22f, 0.22f, 1.0f);
        p.fill_top = held ? r_lo : (hovered ? scale(r_hi, 1.05f) : r);
        p.fill_bot = held ? scale(r_hi, 0.85f)
                          : (hovered ? r_lo : scale(r, 0.84f));
        p.border_idle  = with_alpha(r, 0.0f);
        p.border_hover = ImVec4(1, 1, 1, 0.20f);
        p.text = ImVec4(1, 1, 1, 1);
        break;
    }
    case ButtonStyle::Normal:
    default: {
        ImVec4 base = t.bg_button;
        ImVec4 hi   = scale(base, 1.32f);
        ImVec4 lo   = scale(base, 0.92f);
        if (hovered && !held) { hi = scale(hi, 1.08f); lo = scale(lo, 1.06f); }
        if (held)             { hi = scale(hi, 0.85f); lo = scale(lo, 0.85f); }
        p.fill_top = hi;
        p.fill_bot = lo;
        p.border_idle  = with_alpha(t.text_dim, 0.10f);
        p.border_hover = with_alpha(t.accent,   0.85f);
        p.text = enabled ? t.text : t.text_disabled;
        break;
    }
    }

    if (!enabled) {
        auto fade = [](ImVec4 c) { c.w *= 0.5f; return c; };
        p.fill_top     = fade(p.fill_top);
        p.fill_bot     = fade(p.fill_bot);
        p.border_idle  = fade(p.border_idle);
        p.border_hover = fade(p.border_hover);
        p.text.w *= 0.55f;
    }
    return p;
}

// Build a rounded-rect path, then fill it with a vertical 2-stop
// gradient by emitting per-vertex colours. This avoids the seams of
// AddRectFilledMultiColor + corner masking and works for any
// background colour.
void fill_rounded_gradient(ImDrawList* dl,
                           const ImVec2& a, const ImVec2& b,
                           ImU32 col_top, ImU32 col_bot,
                           float radius) {
    if (radius < 1.0f) {
        dl->AddRectFilledMultiColor(a, b, col_top, col_top, col_bot, col_bot);
        return;
    }
    dl->PathClear();
    dl->PathRect(a, b, radius, ImDrawFlags_RoundCornersAll);
    const int vtx_start = dl->VtxBuffer.Size;
    dl->PathFillConvex(IM_COL32_WHITE);  // colour gets overwritten below
    const int vtx_end = dl->VtxBuffer.Size;
    const float h = b.y - a.y;
    for (int i = vtx_start; i < vtx_end; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = (v.pos.y - a.y) / (h > 0 ? h : 1.0f);
        const ImVec4 ct = ImGui::ColorConvertU32ToFloat4(col_top);
        const ImVec4 cb = ImGui::ColorConvertU32ToFloat4(col_bot);
        ImVec4 mix(
            ct.x + (cb.x - ct.x) * t,
            ct.y + (cb.y - ct.y) * t,
            ct.z + (cb.z - ct.z) * t,
            ct.w + (cb.w - ct.w) * t);
        v.col = ImGui::ColorConvertFloat4ToU32(mix);
    }
}

}  // namespace

bool pill_button(const char* label, ImVec2 size, ButtonStyle style, bool enabled) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiStyle& style_g = g.Style;
    const ImGuiID id = window->GetID(label);

    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 sz = ImGui::CalcItemSize(size,
        label_size.x + style_g.FramePadding.x * 2.4f,
        label_size.y + style_g.FramePadding.y * 2.0f);

    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + sz.x, pos.y + sz.y));
    ImGui::ItemSize(sz, style_g.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered = false, held = false, pressed = false;
    if (enabled) {
        pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    } else {
        ImGui::ItemHoverable(bb, id, ImGuiItemFlags_None);
    }

    Pal p = palette(style, hovered, held, enabled);

    ImDrawList* dl = window->DrawList;
    const float radius = sz.y * 0.5f;  // pill: full rounding

    fill_rounded_gradient(dl, bb.Min, bb.Max,
                          to_u32(p.fill_top), to_u32(p.fill_bot),
                          radius);

    ImVec4 border_col = (hovered || held) ? p.border_hover : p.border_idle;
    if (border_col.w > 0.005f) {
        dl->AddRect(bb.Min, bb.Max, to_u32(border_col),
                    radius, 0, 1.5f);
    }

    // Hover glow: a soft accent ring just outside the contour.
    if (hovered && (style == ButtonStyle::Primary
                 || style == ButtonStyle::Normal
                 || style == ButtonStyle::Ghost)) {
        ImVec4 glow = with_alpha(theme().accent, 0.18f);
        dl->AddRect(
            ImVec2(bb.Min.x - 1.5f, bb.Min.y - 1.5f),
            ImVec2(bb.Max.x + 1.5f, bb.Max.y + 1.5f),
            to_u32(glow), radius + 1.5f, 0, 1.0f);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::RenderTextClipped(
        ImVec2(bb.Min.x + style_g.FramePadding.x, bb.Min.y),
        ImVec2(bb.Max.x - style_g.FramePadding.x, bb.Max.y),
        label, nullptr, &label_size,
        ImVec2(0.5f, 0.5f), &bb);
    ImGui::PopStyleColor();

    return enabled && pressed;
}

bool icon_button(const char* glyph, float size, ButtonStyle style) {
    return pill_button(glyph, ImVec2(size, size), style, true);
}

}  // namespace volchay::ui
