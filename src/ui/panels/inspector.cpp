#include "ui/panels/inspector.h"

#include "ui/main_layout.h"
#include "ui/theme.h"

#include <imgui.h>

namespace volchay::ui::panels {

void draw_inspector(EditorContext& ctx) {
    ImGui::Begin("Inspector");

    if (!ctx.project || ctx.project->clips().empty()) {
        ImGui::TextColored(theme().text_dim, "No clip selected.");
        ImGui::End();
        return;
    }

    auto& clip = ctx.project->clips_mut().front();   // V0: edit only first

    ImGui::TextColored(theme().text_dim, "Source");
    ImGui::Separator();
    ImGui::Text("%s", clip.media_id.c_str());

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Trim");
    ImGui::Separator();

    float in_s  = float(core::to_seconds(clip.src_in));
    float out_s = float(core::to_seconds(clip.src_out));

    if (ImGui::DragFloat("In",  &in_s,  0.05f, 0.0f, out_s, "%.3f s")) {
        clip.src_in = core::from_seconds(in_s);
        ctx.project->mark_dirty();
    }
    if (ImGui::DragFloat("Out", &out_s, 0.05f, in_s, 1e6f, "%.3f s")) {
        clip.src_out = core::from_seconds(out_s);
        ctx.project->mark_dirty();
    }

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Speed / volume");
    ImGui::Separator();

    float speed = float(clip.speed);
    if (ImGui::SliderFloat("Speed", &speed, 0.25f, 4.0f, "%.2fx")) {
        clip.speed = speed;
        ctx.project->mark_dirty();
    }
    ImGui::SliderFloat("Volume", &clip.volume, 0.0f, 2.0f, "%.2fx");
    ImGui::Checkbox("Mute", &clip.muted);

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Position");
    ImGui::Separator();
    float t_in_s = float(core::to_seconds(clip.t_in));
    if (ImGui::DragFloat("Timeline in", &t_in_s, 0.05f, 0.0f, 1e6f, "%.3f s")) {
        clip.t_in = core::from_seconds(t_in_s);
        ctx.project->mark_dirty();
    }
    ImGui::TextColored(theme().text_dim,
                       "Duration on timeline: %.2fs",
                       core::to_seconds(clip.duration_on_timeline()));

    ImGui::End();
}

}  // namespace volchay::ui::panels
