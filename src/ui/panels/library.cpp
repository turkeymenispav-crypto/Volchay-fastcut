#include "ui/panels/library.h"

#include "platform/windows.h"
#include "ui/main_layout.h"
#include "ui/theme.h"

#include <imgui.h>

namespace volchay::ui::panels {

void draw_library(EditorContext& ctx) {
    ImGui::Begin("Library");

    if (ImGui::Button("+ Import video")) {
        if (ctx.open_file) ctx.open_file(L"");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(or drag&drop into the window)");

    ImGui::Separator();

    if (!ctx.project) { ImGui::End(); return; }
    const auto& media = ctx.project->media();
    if (media.empty()) {
        ImGui::TextColored(theme().text_dim, "No media in this project yet.");
    } else {
        if (ImGui::BeginTable("##media", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable
              | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed,  80.f);
            ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 90.f);
            ImGui::TableHeadersRow();

            for (const auto& m : media) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string name = volchay::narrow(m.display);
                ImGui::TextUnformatted(name.c_str());

                ImGui::TableSetColumnIndex(1);
                if (m.duration > 0) {
                    ImGui::Text("%.2fs", core::to_seconds(m.duration));
                } else {
                    ImGui::TextColored(theme().text_dim, "--");
                }

                ImGui::TableSetColumnIndex(2);
                if (m.width > 0 && m.height > 0) {
                    ImGui::Text("%dx%d", m.width, m.height);
                } else {
                    ImGui::TextColored(theme().text_dim, "--");
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

}  // namespace volchay::ui::panels
