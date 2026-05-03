#include "ui/panels/export_panel.h"

#include "core/clip.h"
#include "core/project.h"
#include "media/audio_player.h"
#include "media/exporter.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
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

// Center-align a single line within the current content region.
void center_text(ImVec4 col, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    float w = ImGui::CalcTextSize(buf).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (w < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
    ImGui::TextColored(col, "%s", buf);
}

// Tiny "dot orbits a faint ring" spinner. A dim background ring is
// drawn at full circumference, with a small bright dot running around
// it. Compact, never looks like a near-full progress arc.
void spinner(float radius, float dot_radius, ImU32 ring_color,
             ImU32 dot_color) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  size = (radius + dot_radius) * 2.0f;
    ImRect bb(pos, ImVec2(pos.x + size, pos.y + size));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, 0)) return;

    ImVec2 center = ImVec2(pos.x + radius + dot_radius,
                           pos.y + radius + dot_radius);
    float t = float(ImGui::GetTime());
    float angle = t * 4.0f;

    ImDrawList* dl = window->DrawList;
    // Faint background ring so the empty path is still visible.
    dl->AddCircle(center, radius, ring_color, 32, 1.0f);
    // Orbiting dot.
    ImVec2 dot_pos = ImVec2(center.x + std::cos(angle) * radius,
                            center.y + std::sin(angle) * radius);
    dl->AddCircleFilled(dot_pos, dot_radius, dot_color, 12);
}

}  // namespace

void draw_export(EditorContext& ctx) {
    if (!ctx.show_export || !*ctx.show_export) return;
    if (!ctx.exporter) { *ctx.show_export = false; return; }

    static int  preset_index   = 0;
    static int  codec_idx      = 0;     // 0 = H.264, 1 = HEVC
    static int  resolution_idx = 0;     // 0 = source
    static int  fps_idx        = 0;     // 0 = source
    static int  aspect_idx     = 0;     // 0 = source (no AR override)
    static int  custom_w       = 1920;
    static int  custom_h       = 1080;
    static int  custom_fps     = 60;
    static int  vbitrate_mbps  = 12;
    static int  abitrate_idx   = 1;     // 0=128 1=192 2=320 kbps
    static bool hardware       = true;
    static char dest_buf[512]  = "";
    static bool dest_set       = false;
    static bool was_open       = false;

    const char* codec_labels[]      = { "H.264", "HEVC (H.265)" };
    const char* resolution_labels[] = { "Source", "1280x720", "1920x1080",
                                        "2560x1440", "3840x2160", "Custom" };
    const int   resolution_w[]      = { 0, 1280, 1920, 2560, 3840, 0 };
    const int   resolution_h[]      = { 0,  720, 1080, 1440, 2160, 0 };
    const char* fps_labels[]        = { "Source", "24", "30", "60", "Custom" };
    const int   fps_values[]        = {       0,  24,   30,   60,        0 };
    const char* abitrate_labels[]   = { "128 kbps", "192 kbps", "320 kbps" };
    const int   abitrate_values[]   = { 128'000,    192'000,    320'000   };
    const char* aspect_labels[]     = { "Source (no crop)", "1:1", "4:3",
                                        "16:9", "9:16", "21:9" };
    const double aspect_values[]    = { 0.0, 1.0, 4.0/3.0,
                                        16.0/9.0, 9.0/16.0, 21.0/9.0 };
    constexpr int aspect_count = 6;

    // First time the panel comes up in this session, mirror the
    // viewer's AR/quality so the user doesn't have to set it twice.
    if (!was_open) {
        if (ctx.aspect_idx) aspect_idx = std::clamp(*ctx.aspect_idx,
                                                    0, aspect_count - 1);
        was_open = true;
    }

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

    // Bigger panel + chubby paddings inside. The form lives on a
    // single column where every label is centred and every input
    // spans the full content width — closer to a wizard / settings
    // sheet than a debug tool window. We render our own title row
    // (and our own close button) because ImGui's default close X
    // is fixed at one font-size square — far too small to hit
    // comfortably.
    ImGui::SetNextWindowSize(ImVec2(560, 720), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(12, 9));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(10, 10));

    ImGuiWindowFlags export_flags = ImGuiWindowFlags_NoCollapse
                                  | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("Export", nullptr, export_flags)) {
        media::Exporter& ex = *ctx.exporter;
        const bool busy = ex.busy();

        // Custom title row: title text on the left, prominent close
        // button on the right.
        {
            const float row_h = 36.0f;
            const ImVec2 cursor_start = ImGui::GetCursorPos();
            const float row_w = ImGui::GetContentRegionAvail().x;

            // Big pill close button on the right. ~36×36 hit area,
            // turns red on hover so it's obviously the close action.
            const float btn_sz = row_h;
            ImGui::SetCursorPos(ImVec2(
                cursor_start.x + row_w - btn_sz, cursor_start.y));
            if (icon_button("\xC3\x97##export-close", btn_sz,
                            ButtonStyle::Danger)) {
                if (ctx.show_export) *ctx.show_export = false;
            }

            // Title text centred vertically with the close button.
            ImGui::SetCursorPos(cursor_start);
            ImGui::Dummy(ImVec2(0, (row_h - ImGui::GetTextLineHeight()) * 0.5f));
            center_text(theme().text, "Export video");

            // Move cursor to below the row.
            ImGui::SetCursorPos(ImVec2(cursor_start.x,
                cursor_start.y + row_h + 6.0f));
        }
        ImGui::Separator();
        ImGui::Spacing();

        if (!busy) {
            const float full_w = ImGui::GetContentRegionAvail().x;

            auto label = [&](const char* s) {
                ImGui::Spacing();
                center_text(theme().text_dim, "%s", s);
            };

            label("Quick preset");
            const char* labels[16];
            int count = std::min(media::kPresetCount, 16);
            for (int i = 0; i < count; ++i) labels[i] = media::kPresets[i].label;
            ImGui::SetNextItemWidth(full_w);
            if (ImGui::Combo("##preset", &preset_index, labels, count)) {
                apply_preset(preset_index);
            }

            label("Codec");
            ImGui::SetNextItemWidth(full_w);
            ImGui::Combo("##codec", &codec_idx, codec_labels,
                         IM_ARRAYSIZE(codec_labels));

            label("Resolution");
            ImGui::SetNextItemWidth(full_w);
            ImGui::Combo("##resolution", &resolution_idx, resolution_labels,
                         IM_ARRAYSIZE(resolution_labels));
            if (resolution_idx == 5) {  // Custom — two centred fields.
                const float field_w = (full_w - 10.0f) * 0.5f;
                ImGui::SetNextItemWidth(field_w);
                ImGui::InputInt("##cw", &custom_w, 0);
                ImGui::SameLine(0, 10);
                ImGui::SetNextItemWidth(field_w);
                ImGui::InputInt("##ch", &custom_h, 0);
                custom_w = std::clamp(custom_w, 16, 8192);
                custom_h = std::clamp(custom_h, 16, 8192);
            }

            label("Aspect ratio");
            ImGui::SetNextItemWidth(full_w);
            ImGui::Combo("##aspect", &aspect_idx, aspect_labels, aspect_count);
            if (aspect_idx > 0) {
                ImGui::Spacing();
                center_text(theme().text_dim,
                    "Source frame is centre-cropped to %s before encode.",
                    aspect_labels[aspect_idx]);
            }

            label("Frame rate");
            ImGui::SetNextItemWidth(full_w);
            ImGui::Combo("##fps", &fps_idx, fps_labels,
                         IM_ARRAYSIZE(fps_labels));
            if (fps_idx == 4) {
                ImGui::SetNextItemWidth(full_w);
                ImGui::InputInt("##cfps", &custom_fps, 0);
                custom_fps = std::clamp(custom_fps, 1, 480);
            }

            label("Video bitrate (Mbps)");
            ImGui::SetNextItemWidth(full_w);
            ImGui::InputInt("##vbitrate", &vbitrate_mbps, 1, 5);
            vbitrate_mbps = std::clamp(vbitrate_mbps, 1, 500);

            label("Audio bitrate");
            ImGui::SetNextItemWidth(full_w);
            ImGui::Combo("##abitrate", &abitrate_idx, abitrate_labels,
                         IM_ARRAYSIZE(abitrate_labels));

            ImGui::Spacing();
            // Centre the checkbox.
            {
                const char* txt = "Hardware encode (NVENC / Quick Sync / AMF)";
                float w = ImGui::CalcTextSize(txt).x
                        + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (full_w - w) * 0.5f);
                ImGui::Checkbox(txt, &hardware);
            }

            label("Destination");
            // dest field + Browse on the right, full width.
            const float browse_w = 100.0f;
            ImGui::SetNextItemWidth(full_w - browse_w - 10.0f);
            ImGui::InputText("##dest", dest_buf, sizeof(dest_buf));
            ImGui::SameLine(0, 10);
            if (pill_button("Browse...", ImVec2(browse_w, 32),
                            ButtonStyle::Normal)) {
                auto p = pick_save_path(nullptr, L"export.mp4");
                if (!p.empty()) {
                    auto narrow = volchay::narrow(p);
                    ::strncpy(dest_buf, narrow.c_str(), sizeof(dest_buf) - 1);
                    dest_buf[sizeof(dest_buf) - 1] = '\0';
                    dest_set = true;
                }
            }

            const bool can_export = ctx.player && ctx.player->is_open()
                                  && dest_buf[0] != '\0';

            // Honour the timeline trim. We export the source-media
            // range corresponding to the (single) clip on the
            // timeline; if there's no clip yet, fall back to the
            // full source duration.
            core::TimeUs export_src_in  = 0;
            core::TimeUs export_src_out = -1;
            if (ctx.project && !ctx.project->clips().empty()) {
                const core::Clip& c = ctx.project->clips().front();
                export_src_in  = c.src_in;
                export_src_out = c.src_out;
            } else if (ctx.player) {
                export_src_out = ctx.player->duration();
            }

            ImGui::Spacing();
            center_text(theme().text_dim,
                "Trim: %.2fs ... %.2fs (%.2fs)",
                core::to_seconds(export_src_in),
                core::to_seconds(export_src_out),
                core::to_seconds(export_src_out - export_src_in));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Helper that fills an ExportRequest from the current
            // dialog state, except the paths (caller sets those).
            auto build_request = [&](media::ExportRequest& r) {
                r.source_path = ctx.player ? ctx.player->source_path()
                                           : std::wstring{};
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
                r.aspect_ratio  = aspect_values[aspect_idx];
                r.trim_start_us = export_src_in;
                r.trim_end_us   = export_src_out;
            };

            // Centered button row.
            const ImVec2 btn_start (160, 36);
            const ImVec2 btn_replace(200, 36);
            const ImVec2 btn_close (110, 36);
            const float gap = 10.0f;
            const float row_w = btn_start.x + btn_replace.x + btn_close.x
                              + gap * 2.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                + (full_w - row_w) * 0.5f);

            if (pill_button("Start export", btn_start,
                            ButtonStyle::Primary, can_export)) {
                media::ExportRequest r;
                build_request(r);
                r.output_path = volchay::widen(dest_buf);
                ex.start(r);
            }
            ImGui::SameLine(0, gap);
            const bool can_replace = ctx.player && ctx.player->is_open();
            if (pill_button("Replace source video", btn_replace,
                            ButtonStyle::Normal, can_replace)) {
                int yn = ::MessageBoxW(nullptr,
                    L"This will export over the original source file.\n"
                    L"The original will be moved to a .bak next to it,\n"
                    L"then deleted only after the export succeeds.\n\n"
                    L"Continue?",
                    L"Replace source video",
                    MB_ICONWARNING | MB_YESNO);
                if (yn == IDYES) {
                    media::ExportRequest r;
                    build_request(r);
                    // Build temp path "<source>.export.tmp.mp4" and
                    // final path "<source-without-ext>.mp4".
                    std::wstring src = ctx.player->source_path();
                    std::wstring final_path = src;
                    size_t dot = final_path.find_last_of(L'.');
                    if (dot != std::wstring::npos
                        && final_path.find_last_of(L"\\/") < dot) {
                        final_path.resize(dot);
                    }
                    final_path += L".mp4";
                    r.output_path        = src + L".export.tmp.mp4";
                    r.replace_source     = true;
                    r.replace_final_path = final_path;
                    ex.start(r);
                }
            }
            ImGui::SameLine(0, gap);
            if (pill_button("Close", btn_close, ButtonStyle::Ghost))
                *ctx.show_export = false;
        } else {
            // Busy: small "dot orbiting a faint ring" spinner. Centred.
            const float full_w = ImGui::GetContentRegionAvail().x;
            ImGui::Spacing();
            center_text(theme().accent, "Encoding...");
            ImGui::Spacing();

            const float pct = ex.progress() * 100.0f;
            ImVec4 dim = theme().accent;
            dim.w *= 0.25f;
            ImU32 ring_color = ImGui::ColorConvertFloat4ToU32(dim);
            ImU32 dot_color  = ImGui::ColorConvertFloat4ToU32(theme().accent);

            // Centre the spinner + percentage as a unit.
            char pct_buf[16];
            ::snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", pct);
            const float spinner_w = (8.0f + 2.5f) * 2.0f;
            const float pct_w     = ImGui::CalcTextSize(pct_buf).x;
            const float row_w     = spinner_w + 8.0f + pct_w;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                + (full_w - row_w) * 0.5f);
            spinner(8.0f, 2.5f, ring_color, dot_color);
            ImGui::SameLine(0, 8.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
            ImGui::Text("%s", pct_buf);
            ImGui::Spacing();
            center_text(theme().text_dim, "%s", ex.status_text().c_str());
            ImGui::Spacing();

            const ImVec2 btn(140, 34);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                + (full_w - btn.x) * 0.5f);
            if (pill_button("Cancel", btn, ButtonStyle::Danger)) ex.cancel();
        }

        if (ex.finished()) {
            ImGui::Spacing();
            const auto last = ex.last_request();
            if (ex.succeeded()) {
                // If this was a "Replace source" export, perform the
                // file swap on the UI thread (player must be closed
                // before we can delete/rename the source file).
                if (last.replace_source && !last.replace_final_path.empty()) {
                    // Both decoders own their own AVFormatContext on the
                    // source file, so both must release it before we can
                    // delete/move it on disk.
                    if (ctx.audio)  ctx.audio->close();
                    if (ctx.player) ctx.player->close();

                    // Helper: retry a fileop while the OS still has
                    // the file held (Defender scan, Explorer thumbnail
                    // pane, indexing service, etc.). 6 seconds total.
                    auto retry = [](auto&& op) -> DWORD {
                        for (int i = 0; i < 60; ++i) {
                            if (op()) return 0;
                            DWORD e = ::GetLastError();
                            if (e != ERROR_ACCESS_DENIED
                             && e != ERROR_SHARING_VIOLATION
                             && e != ERROR_FILE_NOT_FOUND) return e;
                            ::Sleep(100);
                        }
                        return ::GetLastError();
                    };

                    DWORD err = 0;
                    if (last.source_path == last.replace_final_path) {
                        // Same path: rename source out of the way
                        // first, then move temp into place. If the
                        // second step works, drop the backup.
                        std::wstring bak = last.source_path + L".bak";
                        ::DeleteFileW(bak.c_str());
                        err = retry([&] {
                            return ::MoveFileExW(last.source_path.c_str(),
                                bak.c_str(), MOVEFILE_REPLACE_EXISTING);
                        });
                        if (!err) {
                            err = retry([&] {
                                return ::MoveFileExW(last.output_path.c_str(),
                                    last.replace_final_path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING);
                            });
                            if (err) {
                                // Roll back: restore the original.
                                ::MoveFileExW(bak.c_str(),
                                    last.source_path.c_str(),
                                    MOVEFILE_REPLACE_EXISTING);
                            } else {
                                ::DeleteFileW(bak.c_str());
                            }
                        }
                    } else {
                        // Different paths (e.g. .mov source -> .mp4
                        // final). Move new file into place first, then
                        // delete the original. That way a transient
                        // lock on the source doesn't lose work.
                        err = retry([&] {
                            return ::MoveFileExW(last.output_path.c_str(),
                                last.replace_final_path.c_str(),
                                MOVEFILE_REPLACE_EXISTING);
                        });
                        if (!err) {
                            retry([&] {
                                return ::DeleteFileW(
                                    last.source_path.c_str());
                            });
                        }
                    }
                    ex.mark_replaced();
                    if (!err && ctx.open_file) {
                        ctx.open_file(last.replace_final_path);
                        ImGui::TextColored(theme().accent,
                            "Source video replaced.");
                    } else if (err) {
                        wchar_t buf[256];
                        ::wsprintfW(buf,
                            L"Replace failed: file is locked by "
                            L"another process (Win32 error %lu).\n"
                            L"Close any video previewer / antivirus "
                            L"scan and try again.",
                            err);
                        ::MessageBoxW(nullptr, buf, L"Volchay-fastcut",
                                      MB_ICONERROR | MB_OK);
                        ImGui::TextColored(theme().timeline_playhead,
                            "Export ok, but file swap failed.");
                        // Reopen the original so the user isn't left
                        // with a closed editor session.
                        if (ctx.open_file) ctx.open_file(last.source_path);
                    }
                } else {
                    ImGui::TextColored(theme().accent, "Export finished.");
                }
            } else {
                // On failure of a replace-export, clean up the temp file.
                if (last.replace_source && !last.output_path.empty()) {
                    ::DeleteFileW(last.output_path.c_str());
                    ex.mark_replaced();
                }
                ImGui::TextColored(theme().timeline_playhead,
                                   "Export failed: %s",
                                   ex.status_text().c_str());
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

}  // namespace volchay::ui::panels
