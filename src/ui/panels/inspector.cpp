#include "ui/panels/inspector.h"

#include "media/audio_player.h"
#include "media/mf_player.h"
#include "ui/main_layout.h"
#include "ui/theme.h"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace volchay::ui::panels {

void draw_inspector(EditorContext& ctx) {
    ImGui::Begin("Inspector");

    if (!ctx.project || ctx.project->clips().empty()) {
        ImGui::TextColored(theme().text_dim, "No clip selected.");
        ImGui::End();
        return;
    }

    // Inspector edits the SELECTED clip — and only the selected one.
    // Falling back to "first clip on the timeline" when nothing is
    // selected meant that toggling Mute / Volume / Speed silently
    // patched a different clip than the user thought they were
    // editing — which is what produced the "I muted one and it muted
    // everything" report. Now we only act on an explicit selection;
    // when nothing is selected we show a helpful empty state.
    auto& clips = ctx.project->clips_mut();
    core::Clip* clip_ptr = nullptr;
    int n_selected = 0;
    for (auto& c : clips) {
        if (c.selected) {
            ++n_selected;
            if (!clip_ptr) clip_ptr = &c;
        }
    }
    if (!clip_ptr) {
        ImGui::TextColored(theme().text_dim,
            "No clip selected.\n"
            "Click a clip on the timeline to edit its trim, speed, "
            "volume, mute, position and transform here.");
        ImGui::End();
        return;
    }
    if (n_selected > 1) {
        ImGui::TextColored(theme().text_dim,
            "%d clips selected — showing the first one. "
            "Click a single clip on the timeline to edit it on its own.",
            n_selected);
        ImGui::Spacing();
    }
    auto& clip = *clip_ptr;

    // Clip header — make it obvious which clip's properties are being
    // edited. The name shown is the source basename plus the track
    // identifier (V1 / V2 / A1 / ...).
    {
        std::string label = clip.media_id;
        auto pos = label.find_last_of("/\\");
        if (pos != std::string::npos) label = label.substr(pos + 1);
        char tlbl[8];
        if (clip.track >= 0) {
            std::snprintf(tlbl, sizeof(tlbl), "V%d", clip.track + 1);
        } else {
            std::snprintf(tlbl, sizeof(tlbl), "A%d", -clip.track);
        }
        ImGui::TextColored(theme().accent, "%s", tlbl);
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        ImGui::Separator();
    }

    ImGui::TextColored(theme().text_dim, "Source");
    ImGui::Separator();
    ImGui::Text("%s", clip.media_id.c_str());

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Trim");
    ImGui::Separator();

    float in_s  = float(core::to_seconds(clip.src_in));
    float out_s = float(core::to_seconds(clip.src_out));

    auto resync_to_playhead = [&]() {
        // After src_in/src_out change, snap the audio + video back to the
        // file PTS that corresponds to the current timeline playhead.
        // Otherwise the audio engine keeps playing from wherever it was
        // before the trim, which the user perceives as "audio playing
        // from the beginning even though I trimmed the clip".
        if (!ctx.project) return;
        const core::TimeUs ph = ctx.project->playhead();
        core::TimeUs file_t = ctx.project->source_time_at(ph);
        if (file_t < 0) file_t = clip.src_in;
        if (ctx.player) ctx.player->seek(file_t);
        if (ctx.audio)  ctx.audio->seek(file_t);
    };

    if (ImGui::DragFloat("In",  &in_s,  0.05f, 0.0f, out_s, "%.3f s")) {
        clip.src_in = core::from_seconds(in_s);
        ctx.project->mark_dirty();
        resync_to_playhead();
    }
    if (ImGui::DragFloat("Out", &out_s, 0.05f, in_s, 1e6f, "%.3f s")) {
        clip.src_out = core::from_seconds(out_s);
        ctx.project->mark_dirty();
        resync_to_playhead();
    }

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Speed / volume");
    ImGui::Separator();

    // Note on slider IDs: ImGui scopes IDs by parent window, but we still
    // give each control an explicit "##insp_<name>" id-tail. That makes
    // the IDs unambiguous against any other "Speed" / "Volume" sliders
    // sitting in Settings or Timeline panels (a docked NLE typically has
    // a few). Without this, tabbed-dock layouts where Inspector and
    // Timeline share a node have surprised users with what looks like
    // crosstalk between Speed and Zoom sliders.
    float speed = float(clip.speed);
    if (ImGui::SliderFloat("Speed##insp_speed", &speed, 0.25f, 4.0f, "%.2fx")) {
        clip.speed = speed;
        ctx.project->mark_dirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Playback rate of this clip.\n"
            "Higher = clip plays faster (and is shorter on the timeline).\n"
            "1.0x means real time.");
    }
    if (ImGui::SliderFloat("Volume##insp_vol", &clip.volume,
                           0.0f, 2.0f, "%.2fx")) {
        ctx.project->mark_dirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Per-clip linear gain. Multiplied with the global volume "
            "slider in Settings.");
    }
    if (ImGui::Checkbox("Mute##insp_mute", &clip.muted)) {
        ctx.project->mark_dirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Mute the audio of this clip. OR'd with the global Mute "
            "in Settings.");
    }

    ImGui::Spacing();
    ImGui::TextColored(theme().text_dim, "Position");
    ImGui::Separator();
    float t_in_s = float(core::to_seconds(clip.t_in));
    if (ImGui::DragFloat("Timeline in", &t_in_s, 0.05f, 0.0f, 1e6f, "%.3f s")) {
        clip.t_in = core::from_seconds(t_in_s);
        ctx.project->mark_dirty();
    }

    int track_idx = clip.track;
    if (ImGui::InputInt("Track##insp_track", &track_idx, 1, 1)) {
        if (track_idx < 0) track_idx = 0;
        if (track_idx > 8) track_idx = 8;
        clip.track = track_idx;
        ctx.project->mark_dirty();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Track index. 0 = main video, higher numbers stack on top "
            "(V2, V3, ...). CapCut-style overlays.");
    }

    if (clip.track > 0) {
        ImGui::SliderFloat("Opacity##insp_op",
                           &clip.opacity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Per-clip opacity. Only visible on overlay "
                              "tracks (V2, V3, ...).");
        }
    }

    ImGui::TextColored(theme().text_dim,
                       "Duration on timeline: %.2fs",
                       core::to_seconds(clip.duration_on_timeline()));

    ImGui::End();
}

}  // namespace volchay::ui::panels
