#include "ui/panels/export_panel.h"

#include "media/exporter.h"
#include "ui/theme.h"

#include <imgui.h>

#include <cstdio>

namespace volchay::ui::panels {
namespace {

std::wstring pick_save_path(HWND owner, const wchar_t* default_name) {
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH];
    if (default_name) ::lstrcpyW(buf, default_name);
    else              buf[0] = L'\0';

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter =
        L"MP4 video\0*.mp4\0"
        L"All files\0*.*\0\0";
    ofn.lpstrDefExt = L"mp4";
    ofn.lpstrTitle  = L"Export to...";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!::GetSaveFileNameW(&ofn)) return {};
    return std::wstring(buf);
}

}  // namespace

void draw_export(EditorContext& ctx) {
    if (!ctx.show_export || !*ctx.show_export) return;
    if (!ctx.exporter) { *ctx.show_export = false; return; }

    static int  preset_index = 0;
    static bool hardware     = true;
    static char dest_buf[512] = "";
    static bool dest_set      = false;

    ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Export", ctx.show_export, ImGuiWindowFlags_NoCollapse)) {
        media::Exporter& ex = *ctx.exporter;
        const bool busy = ex.busy();

        if (!busy) {
            ImGui::TextColored(theme().text_dim, "Preset:");
            const char* labels[16];
            int count = std::min(media::kPresetCount, 16);
            for (int i = 0; i < count; ++i) labels[i] = media::kPresets[i].label;
            ImGui::Combo("##preset", &preset_index, labels, count);

            ImGui::Checkbox("Hardware encode (NVENC / Quick Sync / AMF)",
                            &hardware);

            ImGui::Spacing();
            ImGui::TextColored(theme().text_dim, "Destination:");
            ImGui::InputText("##dest", dest_buf, sizeof(dest_buf));
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                auto p = pick_save_path(nullptr, L"export.mp4");
                if (!p.empty()) {
                    auto narrow = volchay::narrow(p);
                    ::strncpy(dest_buf, narrow.c_str(), sizeof(dest_buf) - 1);
                    dest_buf[sizeof(dest_buf) - 1] = '\0';
                    dest_set = true;
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool can_export = ctx.player && ctx.player->is_open()
                                  && dest_buf[0] != '\0';

            ImGui::BeginDisabled(!can_export);
            if (ImGui::Button("Start export", ImVec2(160, 0))) {
                media::ExportRequest r;
                r.source_path = ctx.player ? ctx.player->source_path()
                                           : std::wstring{};
                r.output_path = volchay::widen(dest_buf);
                r.preset_index = preset_index;
                r.hardware     = hardware;
                r.trim_start_us = 0;
                r.trim_end_us   = ctx.player ? ctx.player->duration() : -1;
                ex.start(r);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100, 0))) *ctx.show_export = false;
        } else {
            ImGui::TextColored(theme().accent, "Encoding...");
            ImGui::ProgressBar(ex.progress(), ImVec2(-1, 16));
            ImGui::TextColored(theme().text_dim, "%s", ex.status_text().c_str());
            ImGui::Spacing();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ex.cancel();
        }

        if (ex.finished()) {
            ImGui::Spacing();
            if (ex.succeeded()) {
                ImGui::TextColored(theme().accent, "Export finished.");
            } else {
                ImGui::TextColored(theme().timeline_playhead,
                                   "Export failed: %s",
                                   ex.status_text().c_str());
            }
        }
    }
    ImGui::End();
}

}  // namespace volchay::ui::panels
