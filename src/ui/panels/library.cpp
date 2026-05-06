#include "ui/panels/library.h"

#include "core/project.h"
#include "media/audio_player.h"
#include "media/mf_player.h"
#include "platform/windows.h"
#include "ui/main_layout.h"
#include "ui/theme.h"
#include "ui/thumbnail_cache.h"
#include "ui/widgets.h"

#include <imgui.h>

#include <cstdint>

namespace volchay::ui::panels {
namespace {

// Render width of a thumbnail in the Library row. Height follows from
// the actual decoded aspect.
constexpr float kThumbW = 96.0f;
constexpr float kThumbH = 54.0f;   // 16:9 default; overridden by entry aspect

}  // namespace

void draw_library(EditorContext& ctx) {
    ImGui::Begin("Library");

    if (pill_button("+ Import media", ImVec2(150, 32),
                    ButtonStyle::Primary)) {
        if (ctx.open_file) ctx.open_file(L"");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(or drag&drop into the window)");

    ImGui::Separator();

    if (!ctx.project) { ImGui::End(); return; }
    const auto& media = ctx.project->media();
    if (media.empty()) {
        ImGui::TextColored(theme().text_dim,
            "No media yet. Drag a video / photo here, or use the "
            "import button above.");
        ImGui::End();
        return;
    }

    ImGui::TextColored(theme().text_dim,
        "Click a row to add it to the timeline at the playhead.");
    ImGui::Spacing();

    if (ImGui::BeginTable("##media", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
          | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Preview",
            ImGuiTableColumnFlags_WidthFixed, kThumbW + 8.f);
        ImGui::TableSetupColumn("Name",
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Duration",
            ImGuiTableColumnFlags_WidthFixed,  72.f);
        ImGui::TableSetupColumn("Resolution",
            ImGuiTableColumnFlags_WidthFixed,  84.f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < media.size(); ++i) {
            const auto& m = media[i];
            ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                kThumbH + 6.0f);

            // ---- Preview column (thumbnail). ----
            ImGui::TableSetColumnIndex(0);
            const ThumbnailCache::Entry* entry = nullptr;
            if (ctx.thumbs) entry = ctx.thumbs->get(m.path);

            if (entry && entry->loaded && entry->srv) {
                // Preserve aspect inside the slot.
                float th_w = kThumbW;
                float th_h = kThumbH;
                if (entry->width > 0 && entry->height > 0) {
                    float ar = float(entry->width) / float(entry->height);
                    if (ar > th_w / th_h) th_h = th_w / ar;
                    else                  th_w = th_h * ar;
                }
                ImTextureID tex = (ImTextureID)(std::uintptr_t)entry->srv.get();
                ImGui::Image(tex, ImVec2(th_w, th_h));
            } else if (entry && entry->failed) {
                ImGui::Dummy(ImVec2(kThumbW, kThumbH));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetItemRectMin();
                ImVec2 q = ImGui::GetItemRectMax();
                dl->AddRectFilled(p, q,
                    ImGui::GetColorU32(theme().bg_panel_alt), 4.f);
                dl->AddText(ImVec2(p.x + 6, p.y + 6),
                    ImGui::GetColorU32(theme().text_dim), "no preview");
            } else {
                // Pending: show a soft placeholder while the worker decodes.
                ImGui::Dummy(ImVec2(kThumbW, kThumbH));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 p = ImGui::GetItemRectMin();
                ImVec2 q = ImGui::GetItemRectMax();
                dl->AddRectFilled(p, q,
                    ImGui::GetColorU32(theme().bg_panel_alt), 4.f);
                dl->AddRect(p, q,
                    ImGui::GetColorU32(theme().separator), 4.f);
                dl->AddText(
                    ImVec2(p.x + 6, p.y + (q.y - p.y) * 0.5f - 6),
                    ImGui::GetColorU32(theme().text_dim),
                    "decoding...");
            }

            // ---- Name column with a wide invisible Selectable that
            // covers the whole row, so clicking anywhere on the row
            // adds the media to the timeline.
            ImGui::TableSetColumnIndex(1);
            std::string name = volchay::narrow(m.display);

            // The Selectable spans across all four columns. We compute
            // the selectable height to match the row's image, so the
            // hit-area covers the whole row (including the thumbnail).
            ImGui::PushID(int(i));
            const bool clicked = ImGui::Selectable(
                ("##row_" + std::to_string(i)).c_str(),
                false,
                ImGuiSelectableFlags_SpanAllColumns
                  | ImGuiSelectableFlags_AllowOverlap
                  | ImGuiSelectableFlags_AllowDoubleClick,
                ImVec2(0, kThumbH + 4));
            ImGui::PopID();

            // Draw the name on top of the Selectable (Selectable draws
            // a thin highlight rectangle behind it on hover).
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name.c_str());

            if (clicked) {
                // Append a clip referencing this media to the end of the
                // timeline. If the user double-clicked, also seek to the
                // start of the new clip and start playing.
                core::Clip& c = ctx.project->append_clip(m.id);
                ctx.project->mark_dirty();
                if (ImGui::IsMouseDoubleClicked(0)) {
                    ctx.project->set_playhead(c.t_in);
                    if (ctx.player) ctx.player->seek(c.src_in);
                    if (ctx.audio)  ctx.audio->seek(c.src_in);
                    ctx.project->set_playing(true);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Click: append to timeline\n"
                    "Double-click: append + seek + play");
            }

            ImGui::TableSetColumnIndex(2);
            if (m.duration > 0) {
                ImGui::Text("%.2fs", core::to_seconds(m.duration));
            } else {
                ImGui::TextColored(theme().text_dim, "--");
            }

            ImGui::TableSetColumnIndex(3);
            if (m.width > 0 && m.height > 0) {
                ImGui::Text("%dx%d", m.width, m.height);
            } else {
                ImGui::TextColored(theme().text_dim, "--");
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace volchay::ui::panels
