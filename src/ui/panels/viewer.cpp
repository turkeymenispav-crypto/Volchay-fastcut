#include "ui/panels/viewer.h"

#include "core/clip.h"
#include "core/project.h"
#include "media/audio_player.h"
#include "ui/main_layout.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace volchay::ui::panels {

namespace {

// Quality preset to a max preview height. 0 = source resolution.
constexpr int kQualityMaxH[] = { 480, 720, 0 };
constexpr const char* kQualityLabels[] = { "Low 480p", "Med 720p", "Source" };
constexpr int kQualityCount = 3;

// Aspect ratio override. 0 = "Source" (no override).
struct AspectOption { const char* label; double ratio; };
constexpr AspectOption kAspect[] = {
    {"Source", 0.0},
    {"1:1",    1.0},
    {"4:3",    4.0/3.0},
    {"16:9",   16.0/9.0},
    {"9:16",   9.0/16.0},
    {"21:9",   21.0/9.0},
};
constexpr int kAspectCount = int(sizeof(kAspect) / sizeof(kAspect[0]));

// Layout helper: given an available rectangle and a target aspect
// ratio, return the largest centred sub-rectangle (pos, size) that
// fits inside it and matches the AR. ar=0 falls back to the natural
// AR of the source.
struct FitRect { ImVec2 pos; ImVec2 size; };
FitRect fit_aspect(ImVec2 avail_pos, ImVec2 avail_sz, double ar) {
    FitRect r;
    if (ar <= 0.0) {
        r.pos  = avail_pos;
        r.size = avail_sz;
        return r;
    }
    const double frame_ar = double(avail_sz.x) / std::max(1.0, double(avail_sz.y));
    ImVec2 sz = avail_sz;
    if (ar > frame_ar) {
        sz.y = float(double(avail_sz.x) / ar);
    } else {
        sz.x = float(double(avail_sz.y) * ar);
    }
    r.size = sz;
    r.pos  = ImVec2(avail_pos.x + (avail_sz.x - sz.x) * 0.5f,
                    avail_pos.y + (avail_sz.y - sz.y) * 0.5f);
    return r;
}

// Draw the viewer toolbar (fullscreen / quality / aspect). Returns
// the height it consumed.
float draw_toolbar(EditorContext& ctx, float width, bool floating) {
    const Theme& th = theme();
    const float h = 40.0f;

    if (floating) {
        // Translucent capsule pinned to the top-right of the viewer.
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float pad = 12.0f;
        const float bar_w = 360.0f;
        const float bar_x = origin.x + width - bar_w - pad;
        const float bar_y = origin.y + pad;
        ImVec4 bg = ImVec4(th.bg_panel.x, th.bg_panel.y, th.bg_panel.z, 0.78f);
        dl->AddRectFilled(
            ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + h),
            ImGui::ColorConvertFloat4ToU32(bg), h * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(bar_x + 10, bar_y + 6));
    } else {
        // Inline at the top of the dock-panel.
        ImGui::Dummy(ImVec2(0, 4));
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 6));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        th.bg_button);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, th.bg_button_hover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  th.bg_button_active);

    if (!floating) {
        const float left_pad = 16.0f;
        ImGui::SetCursorPosX(left_pad);
    }

    // Fullscreen toggle (left).
    if (ctx.fullscreen) {
        const bool fs = *ctx.fullscreen;
        const char* lbl = fs ? "Exit fullscreen (F11)" : "Fullscreen (F11)";
        if (pill_button(lbl, ImVec2(0, h - 12),
                        fs ? ButtonStyle::Primary : ButtonStyle::Normal)) {
            *ctx.fullscreen = !fs;
        }
    }

    ImGui::SameLine(0, 8);
    ImGui::TextColored(th.text_dim, "Q:");
    ImGui::SameLine(0, 6);
    if (ctx.preview_quality_idx) {
        ImGui::PushItemWidth(110.0f);
        int q = *ctx.preview_quality_idx;
        if (ImGui::Combo("##quality", &q, kQualityLabels, kQualityCount)) {
            *ctx.preview_quality_idx = q;
            // Apply the new preview cap to the player. open() picks
            // it up; the decoder still runs at source resolution for
            // export accuracy. We also re-open the current file so
            // the change is visible immediately.
            if (ctx.player) {
                ctx.player->set_preview_max_height(kQualityMaxH[q]);
                if (ctx.player->is_open() && ctx.open_file) {
                    std::wstring p = ctx.player->source_path();
                    if (!p.empty()) ctx.open_file(p);
                }
            }
        }
        ImGui::PopItemWidth();
    }

    ImGui::SameLine(0, 12);
    ImGui::TextColored(th.text_dim, "AR:");
    ImGui::SameLine(0, 6);
    if (ctx.aspect_idx) {
        ImGui::PushItemWidth(96.0f);
        const char* ar_labels[kAspectCount];
        for (int i = 0; i < kAspectCount; ++i) ar_labels[i] = kAspect[i].label;
        ImGui::Combo("##aspect", ctx.aspect_idx, ar_labels, kAspectCount);
        ImGui::PopItemWidth();
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    if (floating) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        (void)dl;
        return 0.0f;  // floating bars don't consume layout space
    }
    ImGui::Dummy(ImVec2(0, 2));
    return h + 6.0f;
}

}  // namespace

void draw_viewer(EditorContext& ctx) {
    const bool fullscreen = ctx.fullscreen && *ctx.fullscreen;

    // Window setup. We use a SEPARATE window when fullscreen so the
    // docked "Viewer" panel keeps its place in the dockspace and
    // re-appears cleanly when fullscreen is toggled off (otherwise
    // the NoDocking flag would un-dock the panel for good).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (fullscreen) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos (vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags fs_flags =
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking
            | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("Viewer##fullscreen", nullptr, fs_flags);
    } else {
        ImGui::Begin("Viewer", nullptr,
                     ImGuiWindowFlags_NoScrollbar
                     | ImGuiWindowFlags_NoScrollWithMouse);
    }
    ImGui::PopStyleVar();

    auto* player = ctx.player;
    ImVec2 avail_total = ImGui::GetContentRegionAvail();

    // In docked mode, draw an inline toolbar that consumes layout
    // height. In fullscreen mode it floats over the video.
    float toolbar_h = 0.0f;
    if (!fullscreen) {
        toolbar_h = draw_toolbar(ctx, avail_total.x, /*floating=*/false);
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!player || !player->is_open()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().bg_window);
        ImGui::BeginChild("##empty_viewer", avail, false);
        ImVec2 c = ImGui::GetCursorScreenPos();
        const char* msg1 = "No video loaded";
        const char* msg2 = "Drag a video file here, or use File > Open video";
        ImVec2 sz1 = ImGui::CalcTextSize(msg1);
        ImVec2 sz2 = ImGui::CalcTextSize(msg2);
        ImVec2 win = ImGui::GetContentRegionAvail();
        ImGui::SetCursorScreenPos(ImVec2(
            c.x + (win.x - sz1.x) * 0.5f,
            c.y + win.y * 0.5f - sz1.y));
        ImGui::TextColored(theme().text_dim, "%s", msg1);
        ImGui::SetCursorScreenPos(ImVec2(
            c.x + (win.x - sz2.x) * 0.5f,
            c.y + win.y * 0.5f + 4));
        ImGui::TextColored(theme().text_disabled, "%s", msg2);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    const float vw = float(player->width());
    const float vh = float(player->height());

    // Background fill: full window in fullscreen, available rect
    // otherwise. Letterbox bands look naturally black this way.
    ImVec2 cur = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cur,
                      ImVec2(cur.x + avail.x, cur.y + avail.y),
                      IM_COL32(0, 0, 0, 255));

    if (vw <= 0 || vh <= 0) {
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);
        ImGui::TextColored(theme().text_dim, "(decoding...)");
        ImGui::End();
        return;
    }

    // Resolve the active aspect ratio: 0 = use source video's natural
    // ratio; otherwise the user has selected a fixed AR (1:1, 4:3 ...).
    int ar_idx = ctx.aspect_idx ? *ctx.aspect_idx : 0;
    if (ar_idx < 0 || ar_idx >= kAspectCount) ar_idx = 0;
    const double target_ar = (ar_idx == 0)
        ? double(vw) / double(vh)
        : kAspect[ar_idx].ratio;

    FitRect fit = fit_aspect(cur, avail, target_ar);

    auto* srv = player->current_srv();
    if (srv && player->current_pts() >= 0) {
        ImTextureID tex = (ImTextureID)(intptr_t)srv;

        // When the user picks an aspect that does NOT match the
        // source AR, we must crop the texture so the picture fills
        // `fit` without distortion. We compute UV coordinates that
        // centre-crop the source texture to the chosen AR.
        const double src_ar = double(vw) / double(vh);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ar_idx != 0 && std::abs(src_ar - target_ar) > 0.001) {
            if (target_ar > src_ar) {
                // Output is wider than source: crop top+bottom.
                const float keep = float(src_ar / target_ar);
                const float pad  = (1.0f - keep) * 0.5f;
                uv0.y = pad;
                uv1.y = 1.0f - pad;
            } else {
                // Output is taller: crop left+right.
                const float keep = float(target_ar / src_ar);
                const float pad  = (1.0f - keep) * 0.5f;
                uv0.x = pad;
                uv1.x = 1.0f - pad;
            }
        }

        ImGui::SetCursorScreenPos(fit.pos);
        ImGui::Image(tex, fit.size, uv0, uv1);
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);

        // Transition overlays. We draw them on top of the picture so
        // they're visible at preview time even before a real GPU
        // composite path exists. FadeIn / FadeOut / DipToBlack render
        // as a black rectangle with time-varying alpha. CrossFade and
        // Wipe render as a hint that fades; full second-stream blend
        // is wired in the exporter only.
        if (ctx.project) {
            const auto& clips       = ctx.project->clips();
            const auto& transitions = ctx.project->transitions();
            const core::TimeUs ph   = ctx.project->playhead();
            for (const auto& tr : transitions) {
                if (tr.kind == core::TransitionKind::None ||
                    tr.kind == core::TransitionKind::Cut) continue;
                if (tr.after_clip < 0 ||
                    tr.after_clip >= (int)clips.size() - 1) continue;
                const auto& left  = clips[tr.after_clip];
                const auto& right = clips[tr.after_clip + 1];
                const core::TimeUs half  = tr.duration / 2;
                const core::TimeUs seam  = std::max(left.t_out(), right.t_in);
                const core::TimeUs start = seam - half;
                const core::TimeUs end   = seam + half;
                if (ph < start || ph >= end) continue;
                const double u = double(ph - start) / double(tr.duration);
                float alpha = 0.0f;
                switch (tr.kind) {
                    case core::TransitionKind::FadeIn:
                        alpha = float(1.0 - u);          // black -> picture
                        break;
                    case core::TransitionKind::FadeOut:
                        alpha = float(u);                // picture -> black
                        break;
                    case core::TransitionKind::DipToBlack: {
                        // 0..0.5 fade out, 0.5..1 fade in.
                        alpha = u < 0.5
                                ? float(u * 2.0)
                                : float((1.0 - u) * 2.0);
                    } break;
                    case core::TransitionKind::CrossFade: {
                        // Approximation: dim the current picture in the
                        // middle of the transition. Real two-stream
                        // blend is exporter-side.
                        alpha = float(0.5 *
                                      (1.0 - std::cos(u * 3.14159265)));
                        alpha = alpha * 0.6f;             // never fully black
                    } break;
                    case core::TransitionKind::Wipe: {
                        // Draw a moving vertical band sweeping L->R.
                        const float xw =
                            fit.pos.x + fit.size.x * float(u);
                        dl->AddRectFilled(
                            ImVec2(fit.pos.x, fit.pos.y),
                            ImVec2(xw, fit.pos.y + fit.size.y),
                            IM_COL32(0, 0, 0, 90));
                        dl->AddLine(
                            ImVec2(xw, fit.pos.y),
                            ImVec2(xw, fit.pos.y + fit.size.y),
                            IM_COL32(217, 119, 87, 255), 2.f);
                        continue;
                    } break;
                    default: break;
                }
                if (alpha > 0.0f) {
                    const ImU32 col = IM_COL32(0, 0, 0, int(alpha * 255.f));
                    dl->AddRectFilled(
                        fit.pos,
                        ImVec2(fit.pos.x + fit.size.x,
                               fit.pos.y + fit.size.y),
                        col);
                }
            }
        }
    } else {
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);
        ImVec2 c2 = ImVec2(cur.x + avail.x * 0.5f - 60,
                           cur.y + avail.y * 0.5f);
        dl->AddText(c2, IM_COL32(180, 180, 180, 220),
                    "decoding first frame...");
    }

    // Compact transport HUD overlayed in the top-left corner of
    // the picture (NOT the toolbar area).
    if (ctx.project) {
        const double t = core::to_seconds(ctx.project->playhead());
        const double d = core::to_seconds(ctx.project->duration());
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "%02d:%02d:%02d.%03d / %02d:%02d:%02d.%03d   %s",
                      int(t / 3600), int(int(t) / 60) % 60, int(t) % 60,
                      int((t - int(t)) * 1000),
                      int(d / 3600), int(int(d) / 60) % 60, int(d) % 60,
                      int((d - int(d)) * 1000),
                      player->hardware_decode() ? "HW" : "SW");
        const float hud_x = fit.pos.x + 8;
        const float hud_y = fit.pos.y + 8;
        dl->AddRectFilled(ImVec2(hud_x, hud_y),
                          ImVec2(hud_x + ImGui::CalcTextSize(buf).x + 16,
                                 hud_y + ImGui::GetTextLineHeight() + 8),
                          IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(hud_x + 8, hud_y + 4),
                    IM_COL32(220, 220, 220, 255), buf);
    }

    // Floating toolbar last, so it draws on top of the picture.
    if (fullscreen) {
        ImGui::SetCursorScreenPos(cur);
        draw_toolbar(ctx, avail.x, /*floating=*/true);
        (void)toolbar_h;
    }

    ImGui::End();
}

}  // namespace volchay::ui::panels
