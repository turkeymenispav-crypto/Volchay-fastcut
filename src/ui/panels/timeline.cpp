#include "ui/panels/timeline.h"

#include "media/audio_player.h"
#include "ui/main_layout.h"
#include "ui/theme.h"

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
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            project->set_playing(!playing);
        }
        ImGui::SameLine();
        if (ImGui::Button("|<")) {
            project->set_playhead(0);
            sync_player_seek(0);
        }
        ImGui::SameLine();
        if (ImGui::Button(">|")) {
            const auto d = project->duration();
            project->set_playhead(d);
            sync_player_seek(d);
        }
        ImGui::SameLine(0, 16);

        // Editing buttons.
        if (ImGui::Button("Cut at \xe2\x96\xbc")) {  // "Cut at ▼"
            project->split_at(project->playhead());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Split clip at the playhead (S)");
        }
        ImGui::SameLine();
        const bool has_sel = project->any_selected();
        ImGui::BeginDisabled(!has_sel);
        if (ImGui::Button("Delete")) {
            project->delete_selected(ripple_delete);
        }
        if (ImGui::IsItemHovered() && has_sel) {
            ImGui::SetTooltip("Delete selected clip(s) (Del / Backspace)");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Ripple", &ripple_delete);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "When deleting, slide later clips left to close the gap");
        }
        ImGui::SameLine(0, 12);

        ImGui::BeginDisabled(!project->can_undo());
        if (ImGui::Button("Undo")) project->undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!project->can_redo());
        if (ImGui::Button("Redo")) project->redo();
        ImGui::EndDisabled();

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

        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(140);
        float zf = float(zoom);
        if (ImGui::SliderFloat("zoom", &zf, 0.05f, 8.0f, "%.2fx")) {
            zoom = zf;
        }
    }

    ImGui::Separator();

    const float ruler_h = 22.f;
    const float track_h = 56.f;

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    avail.y       = std::max(track_h + ruler_h + 8.f, avail.y);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().timeline_bg);
    ImGui::BeginChild("##timeline_canvas", avail, false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Total scrollable width: cover the project + 2s slack.
    const double extra_us  = 2'000'000.0;
    const double total_us  = std::max(double(project->duration()) + extra_us,
                                      30'000'000.0);
    const float  total_w   = float(total_us * px_per_us(zoom));
    ImGui::Dummy(ImVec2(total_w, ruler_h + track_h + 8.f));

    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 c = origin;

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

    // Track lane.
    const float track_y0 = c.y + ruler_h + 4.f;
    const float track_y1 = track_y0 + track_h;
    dl->AddRectFilled(ImVec2(c.x, track_y0), ImVec2(c.x + total_w, track_y1),
                      ImGui::GetColorU32(theme().bg_panel));

    const ImGuiIO& io = ImGui::GetIO();

    // Clips: render + per-clip interaction (select, drag-trim).
    auto& clips = project->clips_mut();
    bool any_clip_hovered = false;
    for (auto& clip : clips) {
        const float x0 = c.x + float(double(clip.t_in)    * px_per_us(zoom));
        const float x1 = c.x + float(double(clip.t_out()) * px_per_us(zoom));
        const ImVec2 a(x0, track_y0 + 4);
        const ImVec2 b(x1, track_y1 - 4);

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
                dl->AddText(ImVec2(x0 + 6, track_y0 + (track_h - tsz.y) * 0.5f),
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
                for (auto& cc : clips) {
                    if (cc.id == move_clip_id) {
                        cc.t_in = new_t_in;
                        project->mark_dirty();
                        break;
                    }
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
    dl->AddLine(ImVec2(ph_x, c.y + 2), ImVec2(ph_x, track_y1 + 2),
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
                           ImVec2(total_w, ruler_h + track_h + 8.f));
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
