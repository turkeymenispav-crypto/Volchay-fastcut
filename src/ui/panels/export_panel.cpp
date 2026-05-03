#include "ui/panels/export_panel.h"

#include "media/exporter.h"
#include "ui/theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
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

// Indeterminate circular spinner. Uses ImDrawList to render an arc
// that sweeps clockwise. Replaces ImGui::ProgressBar for the export
// dialog (the user wanted a small spinning circle instead of a bar).
void spinner(float radius, float thickness, ImU32 color, int segments = 24) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  size = (radius + thickness) * 2.0f;
    ImRect bb(pos, ImVec2(pos.x + size, pos.y + size));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, 0)) return;

    ImVec2 center = ImVec2(pos.x + radius + thickness,
                           pos.y + radius + thickness);
    float t = float(ImGui::GetTime());
    float start_angle = t * 3.0f;
    float arc = float(IM_PI) * 1.4f;

    ImDrawList* dl = window->DrawList;
    dl->PathClear();
    for (int i = 0; i <= segments; ++i) {
        float a = start_angle + arc * (float(i) / float(segments));
        dl->PathLineTo(ImVec2(center.x + std::cos(a) * radius,
                              center.y + std::sin(a) * radius));
    }
    dl->PathStroke(color, ImDrawFlags_None, thickness);
}

}  // namespace

void draw_export(EditorContext& ctx) {
    if (!ctx.show_export || !*ctx.show_export) return;
    if (!ctx.exporter) { *ctx.show_export = false; return; }

    static int  preset_index   = 0;
    static int  codec_idx      = 0;     // 0 = H.264, 1 = HEVC
    static int  resolution_idx = 0;     // 0 = source
    static int  fps_idx        = 0;     // 0 = source
    static int  custom_w       = 1920;
    static int  custom_h       = 1080;
    static int  custom_fps     = 60;
    static int  vbitrate_mbps  = 12;
    static int  abitrate_idx   = 1;     // 0=128 1=192 2=320 kbps
    static bool hardware       = true;
    static char dest_buf[512]  = "";
    static bool dest_set       = false;

    const char* codec_labels[]      = { "H.264", "HEVC (H.265)" };
    const char* resolution_labels[] = { "Source", "1280x720", "1920x1080",
                                        "2560x1440", "3840x2160", "Custom" };
    const int   resolution_w[]      = { 0, 1280, 1920, 2560, 3840, 0 };
    const int   resolution_h[]      = { 0,  720, 1080, 1440, 2160, 0 };
    const char* fps_labels[]        = { "Source", "24", "30", "60", "Custom" };
    const int   fps_values[]        = {       0,  24,   30,   60,        0 };
    const char* abitrate_labels[]   = { "128 kbps", "192 kbps", "320 kbps" };
    const int   abitrate_values[]   = { 128'000,    192'000,    320'000   };

    auto apply_preset = [&](int idx) {
        if (idx < 0 || idx >= media::kPresetCount) return;
        const media::ExportPreset& p = media::kPresets[idx];
        codec_idx     = p.prefer_hevc ? 1 : 0;
        // Map resolution to dropdown.
        resolution_idx = 5;          // default to Custom
        for (int i = 0; i < 5; ++i) {
            if (p.width == resolution_w[i] && p.height == resolution_h[i]) {
                resolution_idx = i;
                break;
            }
        }
        if (resolution_idx == 5) {
            custom_w = p.width;
            custom_h = p.height;
        }
        // Map fps.
        if (p.fps_num == 0) {
            fps_idx = 0;
        } else {
            fps_idx = 4;
            for (int i = 1; i < 4; ++i) {
                if (fps_values[i] == p.fps_num) { fps_idx = i; break; }
            }
            if (fps_idx == 4) custom_fps = p.fps_num;
        }
        vbitrate_mbps = std::max(1, p.video_bitrate / 1'000'000);
    };

    ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Export", ctx.show_export, ImGuiWindowFlags_NoCollapse)) {
        media::Exporter& ex = *ctx.exporter;
        const bool busy = ex.busy();

        if (!busy) {
            ImGui::TextColored(theme().text_dim, "Quick preset:");
            const char* labels[16];
            int count = std::min(media::kPresetCount, 16);
            for (int i = 0; i < count; ++i) labels[i] = media::kPresets[i].label;
            if (ImGui::Combo("##preset", &preset_index, labels, count)) {
                apply_preset(preset_index);
            }
            ImGui::Spacing();

            ImGui::TextColored(theme().text_dim, "Codec");
            ImGui::Combo("##codec", &codec_idx, codec_labels,
                         IM_ARRAYSIZE(codec_labels));

            ImGui::TextColored(theme().text_dim, "Resolution");
            ImGui::Combo("##resolution", &resolution_idx, resolution_labels,
                         IM_ARRAYSIZE(resolution_labels));
            if (resolution_idx == 5) {  // Custom
                ImGui::PushItemWidth(80);
                ImGui::InputInt("W", &custom_w, 0);
                ImGui::SameLine();
                ImGui::InputInt("H", &custom_h, 0);
                ImGui::PopItemWidth();
                custom_w = std::clamp(custom_w, 16, 8192);
                custom_h = std::clamp(custom_h, 16, 8192);
            }

            ImGui::TextColored(theme().text_dim, "Frame rate");
            ImGui::Combo("##fps", &fps_idx, fps_labels,
                         IM_ARRAYSIZE(fps_labels));
            if (fps_idx == 4) {
                ImGui::PushItemWidth(120);
                ImGui::InputInt("fps", &custom_fps, 0);
                ImGui::PopItemWidth();
                custom_fps = std::clamp(custom_fps, 1, 480);
            }

            ImGui::TextColored(theme().text_dim, "Video bitrate (Mbps)");
            ImGui::PushItemWidth(180);
            ImGui::InputInt("##vbitrate", &vbitrate_mbps, 1, 5);
            ImGui::PopItemWidth();
            vbitrate_mbps = std::clamp(vbitrate_mbps, 1, 500);

            ImGui::TextColored(theme().text_dim, "Audio bitrate");
            ImGui::Combo("##abitrate", &abitrate_idx, abitrate_labels,
                         IM_ARRAYSIZE(abitrate_labels));

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
                r.codec = (codec_idx == 1) ? media::ExportCodec::HEVC
                                           : media::ExportCodec::H264;
                if (resolution_idx == 5) {
                    r.width  = custom_w;
                    r.height = custom_h;
                } else {
                    r.width  = resolution_w[resolution_idx];
                    r.height = resolution_h[resolution_idx];
                }
                if (fps_idx == 4) {
                    r.fps_num = custom_fps;
                    r.fps_den = 1;
                } else {
                    r.fps_num = fps_values[fps_idx];
                    r.fps_den = 1;
                }
                r.video_bitrate = vbitrate_mbps * 1'000'000;
                r.audio_bitrate = abitrate_values[abitrate_idx];
                r.hardware      = hardware;
                r.trim_start_us = 0;
                r.trim_end_us   = ctx.player ? ctx.player->duration() : -1;
                ex.start(r);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100, 0))) *ctx.show_export = false;
        } else {
            // Busy: small circular spinner instead of progress bar.
            ImGui::TextColored(theme().accent, "Encoding...");
            ImGui::Spacing();
            const float pct = ex.progress() * 100.0f;
            ImU32 spin_color = ImGui::ColorConvertFloat4ToU32(theme().accent);
            spinner(14.0f, 3.0f, spin_color);
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
            ImGui::Text("%.0f%%", pct);
            ImGui::Spacing();
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
