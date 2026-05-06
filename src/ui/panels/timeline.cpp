#include "ui/panels/timeline.h"

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

// One pixel = how many microseconds at 1.0 zoom.
constexpr double kBaseUsPerPx = 100'000.0;     // 100 ms per pixel at zoom=1

double us_per_px(double zoom) { return kBaseUsPerPx / zoom; }
double px_per_us(double zoom) { return 1.0 / us_per_px(zoom); }

void format_time(char* out, int n, core::TimeUs us) {
    double s = core::to_seconds(us);
    int    m = int(s / 60.0);
    double r = s - 60.0 * m;
    ::snprintf(out, n, "%d:%05.2f", m, r);
}

}  // namespace

void draw_timeline(EditorContext& ctx) {
    static double zoom = 1.0;
    static bool   ripple_delete = true;
    // Per-frame drag state for trim handles.
    static std::string  trim_clip_id;
    static int          trim_side       = 0;   // -1 left, +1 right, 0 none
    static core::TimeUs trim_initial_x  = 0;   // initial timeline coordinate
    static core::TimeUs trim_initial_v  = 0;
    // Per-frame drag state for moving clips along the timeline.
    static std::string  move_clip_id;
    static float        move_initial_mouse_x = 0.f;
    static core::TimeUs move_initial_t_in    = 0;
    static bool         move_did_drag        = false;
    static bool         move_pushed_undo     = false;
    // Number of overlay tracks drawn even if no clip occupies them yet.
    // Bumped by the "+ Track" button so the user has a place to drop the
    // next overlay clip onto.
    static int          extra_overlay_rows   = 0;

    ImGui::Begin("Timeline");

    if (!ctx.project) { ImGui::End(); return; }
    auto* project = ctx.project;
    auto* player  = ctx.player;
    auto* audio   = ctx.audio;

    // The audio engine + video player both speak FILE PTS, but `t` here
    // is the project's TIMELINE PTS. Translate via the clip that covers
    // the playhead so that trim_in / split / multi-clip layouts (where
    // timeline_pts != file_pts) seek the source to the right place.
    auto sync_player_seek = [&](core::TimeUs t) {
        core::TimeUs file_t = project->source_time_at(t);
        if (file_t < 0) {
            // Outside any clip — fall back to t. The player will clamp
            // negative values internally.
            file_t = t;
        }
        if (player) player->seek(file_t);
        if (audio)  audio->seek(file_t);
    };

    // Transport bar.
    {
        bool playing = project->is_playing();
        if (pill_button(playing ? "Pause" : "Play", ImVec2(78, 30),
                        playing ? ButtonStyle::Primary
                                : ButtonStyle::Normal)) {
            project->set_playing(!playing);
        }
        ImGui::SameLine();
        if (pill_button("|<", ImVec2(40, 30), ButtonStyle::Normal)) {
            project->set_playhead(0);
            sync_player_seek(0);
        }
        ImGui::SameLine();
        if (pill_button(">|", ImVec2(40, 30), ButtonStyle::Normal)) {
            const auto d = project->duration();
            project->set_playhead(d);
            sync_player_seek(d);
        }
        ImGui::SameLine(0, 16);

        // Editing buttons.
        if (pill_button("Cut at \xe2\x96\xbc", ImVec2(0, 30),  // "Cut at ▼"
                        ButtonStyle::Normal)) {
            project->split_at(project->playhead());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Split clip at the playhead (S)");
        }
        ImGui::SameLine();
        // Trim-left / Trim-right razor cuts. Q / W follow the Premiere
        // and Resolve convention (Edit -> Trim -> Trim Start / Trim End
        // to Play Head).
        if (pill_button("\xe2\x86\xa4 L", ImVec2(40, 30),    // "↤ L"
                        ButtonStyle::Normal,
                        project->playhead() > 0)) {
            project->trim_left_at(project->playhead());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Trim everything to the left of the playhead (Q).\n"
                "All clips before the playhead are dropped, the rest "
                "shift left to start at 0.");
        }
        ImGui::SameLine();
        if (pill_button("R \xe2\x86\xa6", ImVec2(40, 30),    // "R ↦"
                        ButtonStyle::Normal,
                        project->playhead() < project->duration())) {
            project->trim_right_at(project->playhead());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Trim everything to the right of the playhead (W).\n"
                "All clips after the playhead are dropped.");
        }
        ImGui::SameLine();
        const bool has_sel = project->any_selected();
        if (pill_button("Delete", ImVec2(74, 30),
                        has_sel ? ButtonStyle::Danger : ButtonStyle::Normal,
                        has_sel)) {
            project->delete_selected(ripple_delete);
        }
        if (ImGui::IsItemHovered() && has_sel) {
            ImGui::SetTooltip("Delete selected clip(s) (Del / Backspace)");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Ripple", &ripple_delete);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When deleting, slide later clips left to close the gap");
        }
        ImGui::SameLine(0, 12);

        if (pill_button("Undo", ImVec2(64, 30), ButtonStyle::Ghost,
                        project->can_undo()))
            project->undo();
        ImGui::SameLine();
        if (pill_button("Redo", ImVec2(64, 30), ButtonStyle::Ghost,
                        project->can_redo()))
            project->redo();

        ImGui::SameLine(0, 16);

        char tbuf[32];
        format_time(tbuf, sizeof(tbuf), project->playhead());
        ImGui::TextColored(theme().accent, " %s", tbuf);
        ImGui::SameLine();
        ImGui::TextColored(theme().text_dim, "/");
        ImGui::SameLine();
        char dbuf[32];
        format_time(dbuf, sizeof(dbuf), project->duration());
        ImGui::Text("%s", dbuf);

        ImGui::SameLine(0, 16);
        if (pill_button("+ Track", ImVec2(0, 30), ButtonStyle::Normal)) {
            ++extra_overlay_rows;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add an empty overlay video track (V%d).\n"
                              "Drop a clip onto that lane to stack it on top.",
                              std::max(project->video_track_count() + 1,
                                       2 + extra_overlay_rows));
        }

        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(140);
        float zf = float(zoom);
        if (ImGui::SliderFloat("zoom", &zf, 0.05f, 8.0f, "%.2fx")) {
            zoom = zf;
        }
    }

    ImGui::Separator();

    const float ruler_h    = 22.f;
    const float track_h    = 56.f;
    const float track_gap  = 4.f;

    // Multi-track stack. The user-visible "+Track" button in the
    // toolbar adds an empty overlay row; clips on track i sit on row
    // i, and row 0 (the main track) is drawn at the bottom of the
    // stack (CapCut convention — overlays float above the base video).
    int data_tracks = project->video_track_count();
    int n_tracks    = std::max(data_tracks, 1 + extra_overlay_rows);
    const float tracks_h = n_tracks * (track_h + track_gap) - track_gap;

    ImVec2 avail  = ImGui::GetContentRegionAvail();
    avail.y       = std::max(tracks_h + ruler_h + 8.f, avail.y);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().timeline_bg);
    ImGui::BeginChild("##timeline_canvas", avail, false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // origin must be sampled INSIDE the child so it tracks the scroll
    // offset. When the user scrolls right, GetCursorScreenPos() returns
    // a value shifted left by the scroll delta, and clip rectangles
    // (which we draw at `origin + t*px_per_us`) follow naturally.
    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Total scrollable width: cover the project + 2s slack.
    const double extra_us  = 2'000'000.0;
    const double total_us  = std::max(double(project->duration()) + extra_us,
                                      30'000'000.0);
    const float  total_w   = float(total_us * px_per_us(zoom));
    ImGui::Dummy(ImVec2(total_w, ruler_h + tracks_h + 8.f));

    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 c = origin;

    // Mouse-wheel pan + zoom. ImGui only auto-translates wheel into
    // horizontal scroll when there is no vertical-scroll need; the
    // timeline child rarely satisfies that, so we do it explicitly:
    //   * Shift + wheel: horizontal pan
    //   * Ctrl  + wheel: zoom in/out (anchored at mouse position)
    //   * plain wheel  : horizontal pan as well, since vertical scroll
    //                    in this single-row child is meaningless
    //   * middle-drag  : pan horizontally
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            if (ImGui::GetIO().KeyCtrl) {
                // Zoom around the mouse cursor.
                const ImVec2 m = ImGui::GetIO().MousePos;
                const double t_at_cursor =
                    double(m.x - origin.x) * us_per_px(zoom);
                double new_zoom = zoom * std::pow(1.15, double(wheel));
                if (new_zoom < 0.05) new_zoom = 0.05;
                if (new_zoom > 8.0)  new_zoom = 8.0;
                zoom = new_zoom;
                // Recompute scroll so the time under cursor stays put.
                const float new_x =
                    float(t_at_cursor * px_per_us(zoom));
                ImGui::SetScrollX(new_x - (m.x - origin.x - ImGui::GetScrollX()));
            } else {
                // Horizontal pan.
                ImGui::SetScrollX(ImGui::GetScrollX() - wheel * 80.0f);
            }
        }
        // Middle-button drag = pan.
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
        }
    }

    // Ruler.
    ImVec2 ruler0 = ImVec2(c.x, c.y);
    ImVec2 ruler1 = ImVec2(c.x + total_w, c.y + ruler_h);
    dl->AddRectFilled(ruler0, ruler1,
                      ImGui::GetColorU32(theme().bg_panel_alt));

    const double major_us = std::pow(10.0,
        std::ceil(std::log10(150.0 * us_per_px(zoom))));
    const double minor_us = major_us / 5.0;

    double t = 0.0;
    while (t < total_us) {
        const float x = c.x + float(t * px_per_us(zoom));
        dl->AddLine(ImVec2(x, c.y + ruler_h - 6),
                    ImVec2(x, c.y + ruler_h),
                    ImGui::GetColorU32(theme().timeline_grid));
        char buf[24];
        format_time(buf, sizeof(buf), core::TimeUs(t));
        dl->AddText(ImVec2(x + 3, c.y + 2),
                    ImGui::GetColorU32(theme().text_dim),
                    buf);
        for (int i = 1; i < 5; ++i) {
            const float xs = x + float(double(i) * minor_us * px_per_us(zoom));
            dl->AddLine(ImVec2(xs, c.y + ruler_h - 3),
                        ImVec2(xs, c.y + ruler_h),
                        ImGui::GetColorU32(theme().timeline_grid_sub));
        }
        t += major_us;
    }

    // Track lanes. Track 0 is at the BOTTOM of the stack (visually the
    // base layer); higher track indices float above. row_y(i) returns
    // the screen-y of the top of row i.
    const float lanes_top = c.y + ruler_h + 4.f;
    auto row_y = [&](int track) {
        // track == 0 sits at lanes_top + (n-1) * (h+gap)
        const int row_from_top = n_tracks - 1 - track;
        return lanes_top + row_from_top * (track_h + track_gap);
    };

    // Backwards-compat aliases for the existing main-track code paths
    // (most operations only need track-0 geometry).
    const float track_y0 = row_y(0);
    const float track_y1 = track_y0 + track_h;

    // Lane backgrounds + labels.
    for (int t_idx = 0; t_idx < n_tracks; ++t_idx) {
        const float y0 = row_y(t_idx);
        const float y1 = y0 + track_h;
        ImU32 lane_bg = ImGui::GetColorU32(t_idx == 0
            ? theme().bg_panel_alt
            : ImVec4(0.16f, 0.13f, 0.12f, 1.0f));
        dl->AddRectFilled(ImVec2(c.x, y0), ImVec2(c.x + total_w, y1), lane_bg);
        char lbl[8]; std::snprintf(lbl, sizeof(lbl), "V%d", t_idx + 1);
        dl->AddText(ImVec2(c.x + 4, y0 + 4),
                    ImGui::GetColorU32(theme().text_dim), lbl);
    }
    const ImGuiIO& io = ImGui::GetIO();

    // Clips: render + per-clip interaction (select, drag-trim).
    auto& clips = project->clips_mut();
    bool any_clip_hovered = false;
    for (auto& clip : clips) {
        const float x0 = c.x + float(double(clip.t_in)    * px_per_us(zoom));
        const float x1 = c.x + float(double(clip.t_out()) * px_per_us(zoom));
        const float y0 = row_y(clip.track);
        const float y1 = y0 + track_h;
        const ImVec2 a(x0, y0 + 4);
        const ImVec2 b(x1, y1 - 4);

        const ImU32 fill = clip.selected
            ? ImGui::GetColorU32(theme().accent)
            : ImGui::GetColorU32(theme().timeline_clip);
        dl->AddRectFilled(a, b, fill, theme().corner_radius);
        dl->AddRect(a, b,
                    ImGui::GetColorU32(theme().timeline_clip_sel),
                    theme().corner_radius, 0,
                    clip.selected ? 2.0f : 1.0f);

        const float clip_w = x1 - x0;
        if (clip_w > 60.f) {
            std::string label = clip.media_id;
            auto pos = label.find_last_of("/\\");
            if (pos != std::string::npos) label = label.substr(pos + 1);
            ImVec2 tsz = ImGui::CalcTextSize(label.c_str());
            if (tsz.x < clip_w - 10.f) {
                dl->AddText(ImVec2(x0 + 6, y0 + (track_h - tsz.y) * 0.5f),
                            ImGui::GetColorU32(theme().text),
                            label.c_str());
            }
        }

        // Hit regions: 6px on each edge are trim handles, the middle is select.
        const float handle_w = 6.f;
        const ImVec2 mouse = io.MousePos;
        const bool inside = mouse.x >= x0 && mouse.x <= x1 &&
                            mouse.y >= a.y && mouse.y <= b.y;
        const bool over_left  = inside && mouse.x <= x0 + handle_w;
        const bool over_right = inside && mouse.x >= x1 - handle_w;

        if (inside) {
            any_clip_hovered = true;
            // Cursor hint.
            if (over_left || over_right) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            } else {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }

        // Begin drag-trim.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside &&
            (over_left || over_right) && trim_clip_id.empty()) {
            trim_clip_id   = clip.id;
            trim_side      = over_left ? -1 : +1;
            trim_initial_x = core::TimeUs(double(mouse.x - c.x)
                                          * us_per_px(zoom));
            trim_initial_v = (trim_side == -1) ? clip.t_in : clip.t_out();
        }
        // Click on the clip body: select AND arm a move-drag. The actual
        // move only commits if the mouse moves more than a few pixels;
        // a pure click stays a select.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside &&
            !over_left && !over_right && trim_clip_id.empty() &&
            move_clip_id.empty()) {
            if (!io.KeyCtrl) project->clear_selection();
            if (io.KeyCtrl) clip.selected = !clip.selected;
            else            clip.selected = true;

            move_clip_id         = clip.id;
            move_initial_mouse_x = mouse.x;
            move_initial_t_in    = clip.t_in;
            move_did_drag        = false;
            move_pushed_undo     = false;
        }

        // Draw handles for visual feedback.
        if (inside && (over_left || over_right)) {
            dl->AddRectFilled(
                ImVec2(over_left ? x0 : x1 - handle_w, a.y),
                ImVec2(over_left ? x0 + handle_w : x1, b.y),
                IM_COL32(255, 165, 60, 200));
        }
    }

    // Transition badges on clip boundaries. We draw them as a small
    // diamond-with-label hovering on the seam between clip[i] and
    // clip[i+1], so the user sees that a transition has been authored
    // there from the Transitions tab.
    {
        const auto& trans = project->transitions();
        for (const auto& tr : trans) {
            if (tr.after_clip < 0
                || tr.after_clip + 1 >= int(clips.size())) continue;
            const auto& left  = clips[size_t(tr.after_clip)];
            const float boundary_x = c.x +
                float(double(left.t_out()) * px_per_us(zoom));
            const float yc = (track_y0 + track_y1) * 0.5f;
            const float r  = 6.0f;
            ImU32 col = ImGui::GetColorU32(theme().accent);
            dl->AddTriangleFilled(
                ImVec2(boundary_x, yc - r),
                ImVec2(boundary_x + r, yc),
                ImVec2(boundary_x, yc + r), col);
            dl->AddTriangleFilled(
                ImVec2(boundary_x, yc - r),
                ImVec2(boundary_x - r, yc),
                ImVec2(boundary_x, yc + r), col);
            const char* lbl = "";
            switch (tr.kind) {
                case core::TransitionKind::Cut:        lbl = "Cut"; break;
                case core::TransitionKind::FadeIn:     lbl = "FI";  break;
                case core::TransitionKind::FadeOut:    lbl = "FO";  break;
                case core::TransitionKind::CrossFade:  lbl = "X";   break;
                case core::TransitionKind::DipToBlack: lbl = "Dip"; break;
                case core::TransitionKind::Wipe:       lbl = "Wipe";break;
                default:                               lbl = "";    break;
            }
            ImVec2 tsz = ImGui::CalcTextSize(lbl);
            dl->AddText(ImVec2(boundary_x - tsz.x * 0.5f, track_y0 - tsz.y - 1.f),
                        ImGui::GetColorU32(theme().text), lbl);
        }
    }

    // Active drag-trim.
    if (!trim_clip_id.empty()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 m = io.MousePos;
            const core::TimeUs t_now = core::TimeUs(double(m.x - c.x)
                                                    * us_per_px(zoom));
            const core::TimeUs delta = t_now - trim_initial_x;
            if (trim_side == -1) {
                project->trim_in(trim_clip_id, trim_initial_v + delta);
            } else if (trim_side == +1) {
                project->trim_out(trim_clip_id, trim_initial_v + delta);
            }
            // Snap audio/video to the new clip extents so the preview
            // doesn't keep playing the pre-trim region (most visible
            // when the user drags the left handle while paused — the
            // audio engine is otherwise free-running from its old PTS).
            const auto& cs = project->clips();
            for (const auto& cc : cs) {
                if (cc.id == trim_clip_id) {
                    if (trim_side == -1) {
                        project->set_playhead(cc.t_in);
                    }
                    sync_player_seek(project->playhead());
                    break;
                }
            }
        } else {
            trim_clip_id.clear();
            trim_side = 0;
        }
    }

    // Active drag-move (translate clip along the timeline).
    if (!move_clip_id.empty()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float dx_px = io.MousePos.x - move_initial_mouse_x;
            // Use a small threshold so a plain click doesn't shift the
            // clip by accident.
            if (!move_did_drag && std::fabs(dx_px) >= 3.0f) {
                move_did_drag    = true;
                if (!move_pushed_undo) {
                    project->push_undo();
                    move_pushed_undo = true;
                }
            }
            if (move_did_drag) {
                core::TimeUs dt = core::TimeUs(double(dx_px) * us_per_px(zoom));
                core::TimeUs new_t_in = move_initial_t_in + dt;
                if (new_t_in < 0) new_t_in = 0;

                // Snap-to-neighbour. If the drag is within ~8px of any
                // other clip's start or end, glue it on for a clean
                // back-to-back placement (no gap, no overlap). This
                // keeps the timeline tidy when the user drops a second
                // clip near the end of the first one.
                core::Clip* moving = nullptr;
                core::TimeUs moving_dur = 0;
                for (auto& cc : clips) {
                    if (cc.id == move_clip_id) {
                        moving = &cc;
                        moving_dur = cc.duration_on_timeline();
                        break;
                    }
                }
                if (moving) {
                    auto absus = [](core::TimeUs x) -> core::TimeUs {
                        return x < 0 ? -x : x;
                    };
                    const core::TimeUs snap_us =
                        core::TimeUs(8.0 * us_per_px(zoom));
                    core::TimeUs best_delta  = snap_us + 1;
                    core::TimeUs best_target = new_t_in;
                    auto try_snap = [&](core::TimeUs cand) {
                        if (cand < 0) return;
                        const core::TimeUs d = absus(new_t_in - cand);
                        if (d < best_delta) { best_delta = d; best_target = cand; }
                    };
                    for (const auto& cc : clips) {
                        if (cc.id == move_clip_id) continue;
                        try_snap(cc.t_out());                    // L->R
                        try_snap(cc.t_in - moving_dur);          // R->L
                        try_snap(cc.t_in);                       // L->L
                    }
                    if (best_delta <= snap_us) new_t_in = best_target;
                    moving->t_in = new_t_in;

                    // Drop onto a different track lane: the user is
                    // dragging the clip up to V2 / V3 etc. Compute the
                    // lane under the cursor and reassign the clip.
                    const float my = io.MousePos.y;
                    for (int t_idx = 0; t_idx < n_tracks; ++t_idx) {
                        const float ly0 = row_y(t_idx);
                        const float ly1 = ly0 + track_h;
                        if (my >= ly0 && my <= ly1) {
                            moving->track = t_idx;
                            break;
                        }
                    }
                    project->mark_dirty();
                }
            }
        } else {
            move_clip_id.clear();
            move_did_drag    = false;
            move_pushed_undo = false;
        }
    }

    // Playhead.
    const float ph_x = c.x + float(double(project->playhead())
                                   * px_per_us(zoom));
    const float ph_y_bottom = lanes_top + tracks_h + 2.f;
    dl->AddLine(ImVec2(ph_x, c.y + 2), ImVec2(ph_x, ph_y_bottom),
                ImGui::GetColorU32(theme().timeline_playhead),
                2.0f);
    dl->AddTriangleFilled(
        ImVec2(ph_x - 6, c.y + 2),
        ImVec2(ph_x + 6, c.y + 2),
        ImVec2(ph_x,     c.y + ruler_h - 2),
        ImGui::GetColorU32(theme().timeline_playhead));

    // Click-to-seek on the canvas (lowest priority — only fires when no
    // clip handled the click).
    ImGui::SetCursorScreenPos(c);
    ImGui::InvisibleButton("##timeline_seek",
                           ImVec2(total_w, ruler_h + tracks_h + 8.f));
    if (ImGui::IsItemActive() && trim_clip_id.empty() && move_clip_id.empty()) {
        const ImVec2 m = io.MousePos;
        const double t_us = double(m.x - c.x) * us_per_px(zoom);
        core::TimeUs nt = core::TimeUs(std::max(0.0, t_us));
        project->set_playhead(nt);
        sync_player_seek(project->playhead());
    }
    if (ImGui::IsItemClicked() && !any_clip_hovered) {
        project->clear_selection();
    }

    // Keyboard shortcuts (only when timeline window is focused, but ImGui
    // captures keys at top-level so we use the hovered/focused window flag).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            project->split_at(project->playhead());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            project->trim_left_at(project->playhead());
            sync_player_seek(project->playhead());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            project->trim_right_at(project->playhead());
            sync_player_seek(project->playhead());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            project->delete_selected(ripple_delete);
        }
        const bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) project->redo();
            else             project->undo();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            project->redo();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            project->set_playing(!project->is_playing());
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
}

}  // namespace volchay::ui::panels
