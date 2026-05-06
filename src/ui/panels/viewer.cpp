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

// UV clamp for cropping a texture to match a target AR while
// preserving its source AR. Returns (uv0, uv1).
struct UVRect { ImVec2 uv0, uv1; };
UVRect crop_uv_to_ar(double src_w, double src_h, double target_ar) {
    UVRect r{ ImVec2(0,0), ImVec2(1,1) };
    if (src_w <= 0 || src_h <= 0 || target_ar <= 0) return r;
    const double src_ar = src_w / src_h;
    if (std::abs(src_ar - target_ar) <= 0.001) return r;
    if (target_ar > src_ar) {
        const float keep = float(src_ar / target_ar);
        const float pad  = (1.0f - keep) * 0.5f;
        r.uv0.y = pad;
        r.uv1.y = 1.0f - pad;
    } else {
        const float keep = float(target_ar / src_ar);
        const float pad  = (1.0f - keep) * 0.5f;
        r.uv0.x = pad;
        r.uv1.x = 1.0f - pad;
    }
    return r;
}

// Apply a clip's transform to a base FitRect: pan (pos_x/pos_y in
// [-1..1] of half-width/height) + uniform scale.
FitRect apply_transform(FitRect base, float scale, float px, float py) {
    if (scale <= 0) scale = 0.01f;
    const float w = base.size.x * scale;
    const float h = base.size.y * scale;
    const float cx = base.pos.x + base.size.x * 0.5f
                   + px * base.size.x * 0.5f;
    const float cy = base.pos.y + base.size.y * 0.5f
                   + py * base.size.y * 0.5f;
    FitRect r;
    r.pos  = ImVec2(cx - w * 0.5f, cy - h * 0.5f);
    r.size = ImVec2(w, h);
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
        return 0.0f;
    }
    ImGui::Dummy(ImVec2(0, 2));
    return h + 6.0f;
}

// Find the clip the viewer should consider "active for editing" —
// preference order:
//   1. The currently selected clip (if any) AND it covers the playhead.
//   2. The topmost clip at the playhead.
core::Clip* viewer_active_clip(core::Project& proj, core::TimeUs ph) {
    auto& clips = proj.clips_mut();
    for (auto& c : clips) {
        if (c.selected && ph >= c.t_in && ph < c.t_out()) return &c;
    }
    const core::Clip* top = nullptr;
    proj.clips_at(ph, &top, nullptr);
    if (!top) return nullptr;
    for (auto& c : clips) {
        if (c.id == top->id) return &c;
    }
    return nullptr;
}

}  // namespace

void draw_viewer(EditorContext& ctx) {
    const bool fullscreen = ctx.fullscreen && *ctx.fullscreen;

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

    auto* player_top = ctx.player;
    auto* player_bot = ctx.player_bot;
    ImVec2 avail_total = ImGui::GetContentRegionAvail();

    float toolbar_h = 0.0f;
    if (!fullscreen) {
        toolbar_h = draw_toolbar(ctx, avail_total.x, /*floating=*/false);
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!player_top || !player_top->is_open()) {
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

    const float vw_top = float(player_top->width());
    const float vh_top = float(player_top->height());

    // Background fill.
    ImVec2 cur = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cur,
                      ImVec2(cur.x + avail.x, cur.y + avail.y),
                      IM_COL32(0, 0, 0, 255));

    if (vw_top <= 0 || vh_top <= 0) {
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);
        ImGui::TextColored(theme().text_dim, "(decoding...)");
        ImGui::End();
        return;
    }

    // Resolve the active aspect ratio for the OUTPUT frame.
    int ar_idx = ctx.aspect_idx ? *ctx.aspect_idx : 0;
    if (ar_idx < 0 || ar_idx >= kAspectCount) ar_idx = 0;
    const double target_ar = (ar_idx == 0)
        ? double(vw_top) / double(vh_top)
        : kAspect[ar_idx].ratio;
    const FitRect frame_fit = fit_aspect(cur, avail, target_ar);

    // Resolve the clip stack at the playhead.
    const core::Clip* top_clip = nullptr;
    const core::Clip* bot_clip = nullptr;
    if (ctx.project) {
        ctx.project->clips_at(ctx.project->playhead(),
                              &top_clip, &bot_clip);
    }

    // ---- Bottom layer (drawn directly through the window draw list,
    //      so the top layer can stack on the same z order without
    //      involving ImGui::Image's per-widget cursor advance). ----
    if (bot_clip && player_bot && player_bot->is_open() &&
        player_bot->current_srv() && player_bot->current_pts() >= 0) {
        const double bw = double(player_bot->width());
        const double bh = double(player_bot->height());
        UVRect uv = crop_uv_to_ar(bw, bh, target_ar);
        FitRect r = apply_transform(frame_fit,
                                    bot_clip->scale,
                                    bot_clip->pos_x,
                                    bot_clip->pos_y);
        ImTextureID tex = (ImTextureID)(intptr_t)player_bot->current_srv();
        dl->AddImage(tex,
                     r.pos,
                     ImVec2(r.pos.x + r.size.x, r.pos.y + r.size.y),
                     uv.uv0, uv.uv1);
    }

    // ---- Top layer ----
    FitRect top_rect = frame_fit;
    bool top_drawn = false;
    if (player_top->current_srv() && player_top->current_pts() >= 0) {
        const float scale = top_clip ? top_clip->scale : 1.0f;
        const float pxn   = top_clip ? top_clip->pos_x : 0.0f;
        const float pyn   = top_clip ? top_clip->pos_y : 0.0f;
        top_rect = apply_transform(frame_fit, scale, pxn, pyn);

        UVRect uv = crop_uv_to_ar(double(vw_top), double(vh_top),
                                  target_ar);
        ImTextureID tex = (ImTextureID)(intptr_t)player_top->current_srv();

        // Top layer opacity (overlay tracks honour clip.opacity).
        const float op = top_clip ? top_clip->opacity : 1.0f;
        const ImU32 tint = IM_COL32(255, 255, 255,
                                    int(std::clamp(op, 0.0f, 1.0f) * 255));
        dl->AddImage(tex,
                     top_rect.pos,
                     ImVec2(top_rect.pos.x + top_rect.size.x,
                            top_rect.pos.y + top_rect.size.y),
                     uv.uv0, uv.uv1, tint);
        top_drawn = true;
    }
    ImGui::SetCursorScreenPos(cur);
    ImGui::Dummy(avail);

    // ---- Transition overlays (preview approximation) ----
    if (ctx.project && top_drawn) {
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
                    alpha = float(1.0 - u);
                    break;
                case core::TransitionKind::FadeOut:
                    alpha = float(u);
                    break;
                case core::TransitionKind::DipToBlack:
                    alpha = u < 0.5
                            ? float(u * 2.0)
                            : float((1.0 - u) * 2.0);
                    break;
                case core::TransitionKind::CrossFade:
                    alpha = float(0.5 *
                                  (1.0 - std::cos(u * 3.14159265)));
                    alpha = alpha * 0.6f;
                    break;
                case core::TransitionKind::Wipe: {
                    const float xw =
                        frame_fit.pos.x + frame_fit.size.x * float(u);
                    dl->AddRectFilled(
                        ImVec2(frame_fit.pos.x, frame_fit.pos.y),
                        ImVec2(xw, frame_fit.pos.y + frame_fit.size.y),
                        IM_COL32(0, 0, 0, 90));
                    dl->AddLine(
                        ImVec2(xw, frame_fit.pos.y),
                        ImVec2(xw, frame_fit.pos.y + frame_fit.size.y),
                        IM_COL32(217, 119, 87, 255), 2.f);
                    continue;
                }
                default: break;
            }
            if (alpha > 0.0f) {
                const ImU32 col = IM_COL32(0, 0, 0, int(alpha * 255.f));
                dl->AddRectFilled(
                    frame_fit.pos,
                    ImVec2(frame_fit.pos.x + frame_fit.size.x,
                           frame_fit.pos.y + frame_fit.size.y),
                    col);
            }
        }
    }

    // ---- Transform handles for the active editing clip ----
    // Persistent drag state.
    static std::string drag_clip_id;
    static int         drag_handle = -1;     // -1 = none, 0..3 corners, 4 = body
    static ImVec2      drag_start_mouse;
    static float       drag_start_scale = 1.0f;
    static float       drag_start_pos_x = 0.0f;
    static float       drag_start_pos_y = 0.0f;

    if (ctx.project && top_drawn) {
        core::Clip* edit_clip = viewer_active_clip(*ctx.project,
                                                   ctx.project->playhead());
        if (edit_clip) {
            const ImU32 c_idle = IM_COL32(255, 255, 255, 220);
            const ImU32 c_hot  = ImGui::GetColorU32(theme().accent);
            const float r = 8.0f;

            // Draw selection frame around the transformed top-layer
            // rectangle (the picture the user actually sees).
            dl->AddRect(top_rect.pos,
                        ImVec2(top_rect.pos.x + top_rect.size.x,
                               top_rect.pos.y + top_rect.size.y),
                        c_idle, 2.f, 0, 2.f);

            ImVec2 corners[4] = {
                ImVec2(top_rect.pos.x, top_rect.pos.y),
                ImVec2(top_rect.pos.x + top_rect.size.x, top_rect.pos.y),
                ImVec2(top_rect.pos.x + top_rect.size.x,
                       top_rect.pos.y + top_rect.size.y),
                ImVec2(top_rect.pos.x, top_rect.pos.y + top_rect.size.y),
            };

            ImVec2 mouse = ImGui::GetIO().MousePos;
            int hover_h = -1;
            for (int i = 0; i < 4; ++i) {
                const float dx = mouse.x - corners[i].x;
                const float dy = mouse.y - corners[i].y;
                if (dx*dx + dy*dy <= r * r) hover_h = i;
            }
            const bool inside_body =
                mouse.x >= top_rect.pos.x &&
                mouse.x <= top_rect.pos.x + top_rect.size.x &&
                mouse.y >= top_rect.pos.y &&
                mouse.y <= top_rect.pos.y + top_rect.size.y;

            if (drag_clip_id.empty() && hover_h >= 0) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            } else if (drag_clip_id.empty() && inside_body) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (drag_clip_id.empty() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::GetIO().WantCaptureMouse == false) {
                if (hover_h >= 0 || inside_body) {
                    drag_clip_id     = edit_clip->id;
                    drag_handle      = (hover_h >= 0) ? hover_h : 4;
                    drag_start_mouse = mouse;
                    drag_start_scale = edit_clip->scale;
                    drag_start_pos_x = edit_clip->pos_x;
                    drag_start_pos_y = edit_clip->pos_y;
                }
            }
            if (!drag_clip_id.empty() && drag_clip_id == edit_clip->id) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const float dxp = mouse.x - drag_start_mouse.x;
                    const float dyp = mouse.y - drag_start_mouse.y;
                    if (drag_handle == 4) {
                        // Body drag: pan in normalised units of the
                        // base frame (so dragging across the full
                        // frame moves pos by ~2 in the relevant axis).
                        const float nx = (frame_fit.size.x > 0)
                            ? (dxp / (frame_fit.size.x * 0.5f)) : 0.f;
                        const float ny = (frame_fit.size.y > 0)
                            ? (dyp / (frame_fit.size.y * 0.5f)) : 0.f;
                        edit_clip->pos_x = drag_start_pos_x + nx;
                        edit_clip->pos_y = drag_start_pos_y + ny;
                        // Clamp to a sensible range.
                        edit_clip->pos_x = std::clamp(edit_clip->pos_x, -2.f, 2.f);
                        edit_clip->pos_y = std::clamp(edit_clip->pos_y, -2.f, 2.f);
                    } else {
                        // Corner drag: uniform scale, dragging
                        // outward grows, inward shrinks. Use the
                        // diagonal distance of the cursor from the
                        // opposite corner relative to the start
                        // distance to avoid weird flips.
                        const ImVec2 anchor = corners[(drag_handle + 2) & 3];
                        auto dist = [](ImVec2 a, ImVec2 b) {
                            const float dx = a.x - b.x, dy = a.y - b.y;
                            return std::sqrt(dx*dx + dy*dy);
                        };
                        const float d_now = dist(mouse, anchor);
                        const float d_start = dist(drag_start_mouse, anchor);
                        if (d_start > 1.0f) {
                            float ns = drag_start_scale * (d_now / d_start);
                            ns = std::clamp(ns, 0.1f, 5.0f);
                            edit_clip->scale = ns;
                        }
                    }
                    ctx.project->mark_dirty();
                } else {
                    drag_clip_id.clear();
                    drag_handle = -1;
                }
            }

            // Draw the corner dots.
            for (int i = 0; i < 4; ++i) {
                const bool hot = (hover_h == i) ||
                                 (!drag_clip_id.empty() &&
                                  drag_handle == i &&
                                  drag_clip_id == edit_clip->id);
                dl->AddCircleFilled(corners[i], r,
                                    hot ? c_hot : c_idle, 16);
                dl->AddCircle      (corners[i], r,
                                    IM_COL32(0, 0, 0, 220), 16, 1.5f);
            }
        }
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
                      player_top->hardware_decode() ? "HW" : "SW");
        const float hud_x = frame_fit.pos.x + 8;
        const float hud_y = frame_fit.pos.y + 8;
        dl->AddRectFilled(ImVec2(hud_x, hud_y),
                          ImVec2(hud_x + ImGui::CalcTextSize(buf).x + 16,
                                 hud_y + ImGui::GetTextLineHeight() + 8),
                          IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(hud_x + 8, hud_y + 4),
                    IM_COL32(220, 220, 220, 255), buf);
    }

    if (fullscreen) {
        ImGui::SetCursorScreenPos(cur);
        draw_toolbar(ctx, avail.x, /*floating=*/true);
        (void)toolbar_h;
    }

    ImGui::End();
}

}  // namespace volchay::ui::panels
