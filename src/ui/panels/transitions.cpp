#include "ui/panels/transitions.h"

#include "core/clip.h"
#include "core/project.h"
#include "ui/main_layout.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <string>

namespace volchay::ui::panels {
namespace {

struct TransitionDef {
    core::TransitionKind kind;
    const char*          label;
    const char*          help;
};

constexpr TransitionDef kCatalog[] = {
    { core::TransitionKind::Cut,         "Cut",
      "Hard cut. No blend; the next clip starts on the exact frame "
      "the previous one ends." },
    { core::TransitionKind::FadeIn,      "Fade In",
      "Fade in from black at the head of the next clip." },
    { core::TransitionKind::FadeOut,     "Fade Out",
      "Fade out to black at the tail of the previous clip." },
    { core::TransitionKind::CrossFade,   "Cross-fade",
      "Dissolve between two clips. Both source frames are blended over "
      "the transition window." },
    { core::TransitionKind::DipToBlack,  "Dip to Black",
      "Fade out then fade in, with a brief moment of solid black "
      "at the boundary." },
    { core::TransitionKind::Wipe,        "Wipe",
      "Horizontal wipe (left -> right). The next clip is revealed "
      "behind a moving edge." },
};

const char* kind_label(core::TransitionKind k) {
    for (const auto& t : kCatalog) {
        if (t.kind == k) return t.label;
    }
    if (k == core::TransitionKind::None) return "None";
    return "?";
}

// Render a small thumbnail-like preview for a transition kind. Pure
// 2D drawlist, no texture upload — keeps the panel cheap.
void draw_thumb(ImDrawList* dl, ImVec2 a, ImVec2 b,
                core::TransitionKind kind) {
    const ImU32 bg   = ImGui::GetColorU32(theme().bg_panel_alt);
    const ImU32 left = ImGui::GetColorU32(ImVec4(0.46f, 0.36f, 0.30f, 1.0f));
    const ImU32 right_= ImGui::GetColorU32(ImVec4(0.30f, 0.36f, 0.46f, 1.0f));
    const ImU32 black= ImGui::GetColorU32(ImVec4(0,0,0,1));
    const ImU32 line = ImGui::GetColorU32(theme().separator);
    const ImU32 acc  = ImGui::GetColorU32(theme().accent);

    dl->AddRectFilled(a, b, bg, 4.f);
    dl->AddRect      (a, b, line, 4.f);

    const float w = b.x - a.x;
    const float mid = a.x + w * 0.5f;

    switch (kind) {
        case core::TransitionKind::Cut: {
            dl->AddRectFilled(a, ImVec2(mid, b.y), left);
            dl->AddRectFilled(ImVec2(mid, a.y), b, right_);
            dl->AddLine(ImVec2(mid, a.y + 2), ImVec2(mid, b.y - 2), acc, 2.f);
        } break;
        case core::TransitionKind::FadeIn: {
            dl->AddRectFilled(a, b, black);
            for (int i = 0; i < 16; ++i) {
                float u = float(i) / 16.f;
                ImU32 c = ImGui::GetColorU32(
                    ImVec4(0.30f * u, 0.36f * u, 0.46f * u, 1.0f));
                ImVec2 p0(a.x + w * u,        a.y);
                ImVec2 p1(a.x + w * (u + 1/16.f), b.y);
                dl->AddRectFilled(p0, p1, c);
            }
        } break;
        case core::TransitionKind::FadeOut: {
            dl->AddRectFilled(a, b, black);
            for (int i = 0; i < 16; ++i) {
                float u = float(i) / 16.f;
                float v = 1.0f - u;
                ImU32 c = ImGui::GetColorU32(
                    ImVec4(0.46f * v, 0.36f * v, 0.30f * v, 1.0f));
                ImVec2 p0(a.x + w * u,        a.y);
                ImVec2 p1(a.x + w * (u + 1/16.f), b.y);
                dl->AddRectFilled(p0, p1, c);
            }
        } break;
        case core::TransitionKind::CrossFade: {
            for (int i = 0; i < 32; ++i) {
                float u = float(i) / 32.f;
                float v = 1.0f - u;
                ImU32 c = ImGui::GetColorU32(
                    ImVec4(0.46f * v + 0.30f * u,
                           0.36f * v + 0.36f * u,
                           0.30f * v + 0.46f * u,
                           1.0f));
                ImVec2 p0(a.x + w * u,        a.y);
                ImVec2 p1(a.x + w * (u + 1/32.f), b.y);
                dl->AddRectFilled(p0, p1, c);
            }
        } break;
        case core::TransitionKind::DipToBlack: {
            for (int i = 0; i < 32; ++i) {
                float u = float(i) / 32.f;
                float dip = 1.0f - std::fabs((u - 0.5f) * 2.0f);
                float k   = 1.0f - dip;
                bool   first = u < 0.5f;
                float r = (first ? 0.46f : 0.30f) * k;
                float g = (first ? 0.36f : 0.36f) * k;
                float bl= (first ? 0.30f : 0.46f) * k;
                ImU32 c = ImGui::GetColorU32(ImVec4(r, g, bl, 1.0f));
                ImVec2 p0(a.x + w * u,        a.y);
                ImVec2 p1(a.x + w * (u + 1/32.f), b.y);
                dl->AddRectFilled(p0, p1, c);
            }
        } break;
        case core::TransitionKind::Wipe: {
            dl->AddRectFilled(a, b, right_);
            float wipe_x = a.x + w * 0.55f;
            dl->AddRectFilled(a, ImVec2(wipe_x, b.y), left);
            dl->AddLine(ImVec2(wipe_x, a.y + 1),
                        ImVec2(wipe_x, b.y - 1), acc, 2.f);
        } break;
        default: {
            dl->AddRectFilled(a, b, bg);
        } break;
    }
}

}  // namespace

void draw_transitions(EditorContext& ctx) {
    ImGui::Begin("Transitions");

    if (!ctx.project) {
        ImGui::TextColored(theme().text_dim, "No project.");
        ImGui::End();
        return;
    }
    auto* project = ctx.project;

    ImGui::TextColored(theme().text_dim,
        "Pick a transition, then choose the clip boundary it should sit "
        "on. Each transition spans the requested duration centred on the "
        "boundary.");
    ImGui::Spacing();

    static int                 sel_idx     = 3;   // Cross-fade default
    static float               dur_seconds = 0.5f;
    static int                 sel_boundary = 0;

    // ---- Transition catalog grid. ----
    const int   cols      = 2;
    const float thumb_w   = 132.0f;
    const float thumb_h   = 60.0f;
    const float pad_x     = 8.0f;
    const float pad_y     = 8.0f;

    ImGui::TextColored(theme().text_dim, "Effect");
    ImGui::Separator();
    for (int i = 0; i < int(sizeof(kCatalog)/sizeof(kCatalog[0])); ++i) {
        const auto& def = kCatalog[i];
        if (i % cols != 0) ImGui::SameLine(0, pad_x);

        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 size(thumb_w, thumb_h + 22.0f);
        ImGui::PushID(int(def.kind));
        bool clicked = ImGui::InvisibleButton("##tr", size);
        ImGui::PopID();
        ImVec2 a = cursor;
        ImVec2 b = ImVec2(cursor.x + thumb_w, cursor.y + thumb_h);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_thumb(dl, a, b, def.kind);

        // Selection ring.
        if (sel_idx == i) {
            dl->AddRect(ImVec2(a.x - 2, a.y - 2),
                        ImVec2(b.x + 2, b.y + 2),
                        ImGui::GetColorU32(theme().accent), 5.f, 0, 2.f);
        }

        // Label below.
        ImVec2 label_pos(a.x + 4.0f, b.y + 4.0f);
        dl->AddText(label_pos,
                    ImGui::GetColorU32(theme().text), def.label);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n\n%s", def.label, def.help);
        }
        if (clicked) sel_idx = i;
        ImGui::Dummy(ImVec2(0, pad_y));
    }

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Duration");
    ImGui::Separator();
    if (ImGui::SliderFloat("##trans_dur",
                           &dur_seconds, 0.1f, 3.0f, "%.2f s")) {
        // intentional empty — sel_idx + dur_seconds applied on Apply.
    }

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Boundary");
    ImGui::Separator();

    const auto& clips = project->clips();
    const int   n_boundaries = std::max(0, int(clips.size()) - 1);
    if (n_boundaries == 0) {
        ImGui::TextColored(theme().text_dim,
            "Add at least two clips to the timeline first.");
    } else {
        if (sel_boundary < 0) sel_boundary = 0;
        if (sel_boundary >= n_boundaries) sel_boundary = n_boundaries - 1;

        for (int i = 0; i < n_boundaries; ++i) {
            std::string lbl = "Between clip " + std::to_string(i + 1)
                            + " and clip "    + std::to_string(i + 2);
            if (ImGui::RadioButton(lbl.c_str(), sel_boundary == i)) {
                sel_boundary = i;
            }
            // Annotate if this boundary already has a transition.
            for (const auto& tr : project->transitions()) {
                if (tr.after_clip == i) {
                    ImGui::SameLine();
                    ImGui::TextColored(theme().accent,
                        "[%s, %.2fs]",
                        kind_label(tr.kind),
                        core::to_seconds(tr.duration));
                    break;
                }
            }
        }
    }

    ImGui::Spacing();
    if (n_boundaries > 0) {
        if (pill_button("Apply transition", ImVec2(160, 32),
                        ButtonStyle::Primary)) {
            project->add_or_update_transition(
                sel_boundary,
                kCatalog[sel_idx].kind,
                core::TimeUs(double(dur_seconds) * 1'000'000.0));
        }
        ImGui::SameLine();
        if (pill_button("Remove", ImVec2(100, 32),
                        ButtonStyle::Danger)) {
            project->remove_transition_at(sel_boundary);
        }
    }

    // ---- Existing transitions table. ----
    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Active");
    ImGui::Separator();

    const auto& trans = project->transitions();
    if (trans.empty()) {
        ImGui::TextColored(theme().text_dim,
            "No transitions on the timeline yet.");
    } else {
        if (ImGui::BeginTable("##trans", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Boundary",
                ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Effect",
                ImGuiTableColumnFlags_WidthFixed, 110.f);
            ImGui::TableSetupColumn("Duration",
                ImGuiTableColumnFlags_WidthFixed, 80.f);
            ImGui::TableHeadersRow();
            for (const auto& tr : trans) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("After clip %d", tr.after_clip + 1);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", kind_label(tr.kind));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2fs", core::to_seconds(tr.duration));
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(theme().text_dim,
            "Note: transitions are stored on the project. Real-time "
            "rendering during playback / export will land in a follow-up "
            "(currently the Cut transition is the only one applied on "
            "export — others are visible only as marks on the timeline).");
    }

    ImGui::End();
}

}  // namespace volchay::ui::panels
