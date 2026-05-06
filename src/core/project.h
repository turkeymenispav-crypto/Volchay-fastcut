// Project = the editing document. Holds media bin + a single timeline
// (multi-track timelines come in a later iteration).
#pragma once

#include "core/clip.h"

#include <string>
#include <string_view>
#include <vector>

namespace volchay::core {

class Project {
public:
    Project();

    // Resolve a media entry by id, or nullptr if missing.
    const Media* find_media(std::string_view id) const;

    // Add a media file (probing not performed here; callers fill duration
    // etc. once the player has examined the file).
    Media& add_media(const std::wstring& abs_path);

    // Update probed metadata for a media entry.
    void update_media_probe(std::string_view id,
                            TimeUs duration, int w, int h, double fps);

    // Append a clip referencing media_id on track 0 (main track), placed
    // flush against the existing timeline tail of that track.
    Clip& append_clip(const std::string& media_id);

    // Append a clip on a specific track. The new clip is positioned at
    // the current playhead (CapCut-style — when you drop a second media
    // onto V2 above an existing V1 clip, the overlay starts under the
    // playhead). If track is unused so far, it is allocated.
    Clip& append_clip_on_track(const std::string& media_id, int track);

    // Replace the (sole) clip on the timeline with a single clip spanning
    // the entire media, positioned at 0. Used when opening a video from
    // the context menu / drag&drop.
    Clip& set_single_clip(const std::string& media_id);

    // Total number of currently-occupied video tracks (max(track)+1).
    int video_track_count() const;

    // Remove all clips.
    void clear_clips();

    const std::vector<Media>& media() const { return media_; }
    const std::vector<Clip>&  clips() const { return clips_; }
    std::vector<Clip>&        clips_mut()   { return clips_; }

    // Transitions placed between clip boundaries.
    const std::vector<Transition>& transitions() const { return transitions_; }
    std::vector<Transition>&       transitions_mut()   { return transitions_; }
    void add_or_update_transition(int after_clip,
                                  TransitionKind kind,
                                  TimeUs duration);
    void remove_transition_at(int after_clip);

    // Timeline duration: end of last clip.
    TimeUs duration() const;

    // Playhead state lives on the project so panels can stay in sync.
    TimeUs playhead() const           { return playhead_; }
    void   set_playhead(TimeUs t);

    bool   is_playing() const         { return playing_; }
    void   set_playing(bool p)        { playing_ = p; }

    // Dirty flag, cleared after successful save.
    bool   dirty() const              { return dirty_; }
    void   mark_dirty()               { dirty_ = true; }
    void   mark_saved()               { dirty_ = false; }

    // -----------------------------------------------------------------
    // Editing operations.
    // -----------------------------------------------------------------

    // Map a timeline time -> source-media time for the clip that covers it,
    // or returns -1 if no clip covers t.
    TimeUs source_time_at(TimeUs t, const Clip** out_clip = nullptr) const;

    // Layered query for the compositor. Returns the (up to 2) clips
    // covering `t`, sorted top-track first. out[0] is the topmost,
    // out[1] is the second-topmost (the layer immediately below). Each
    // can be nullptr if no clip exists on that level. Used by the
    // preview viewer to render V0 under V1 + by the export pipeline to
    // composite tracks.
    void clips_at(TimeUs t, const Clip** out_top,
                            const Clip** out_bot) const;

    // Selection helpers.
    void clear_selection();
    bool any_selected() const;
    int  selected_count() const;

    // Split every clip at the given timeline time. The clip is divided into
    // two halves: [t_in .. t] and [t .. t_out]. Returns number of clips
    // created (0 if t lies on an existing clip boundary or outside any clip).
    int split_at(TimeUs t);

    // Delete all selected clips. If `ripple` is true, clips after the
    // deleted region shift left by the cumulative deleted span. Returns
    // number of clips removed.
    int delete_selected(bool ripple);

    // Trim handles. Returns true if trim succeeded (clamped to media bounds
    // when known).
    bool trim_in (const std::string& clip_id, TimeUs new_t_in);
    bool trim_out(const std::string& clip_id, TimeUs new_t_out);

    // "Razor" trims. trim_left_at(t)  removes everything before t — clips
    // entirely before t are deleted, the clip under t is split and its
    // left half discarded, then a ripple shift snaps the remainder to 0.
    // trim_right_at(t) removes everything after t (no ripple needed).
    // Returns the number of clips removed (or trimmed).
    int trim_left_at (TimeUs t);
    int trim_right_at(TimeUs t);

    // -----------------------------------------------------------------
    // Undo/Redo. We snapshot the full editing state (clips + playhead)
    // before each mutating operation; the stack is bounded to keep memory
    // sane on large projects.
    // -----------------------------------------------------------------
    void push_undo();
    bool undo();
    bool redo();
    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }

private:
    struct Snapshot {
        std::vector<Clip> clips;
        TimeUs            playhead;
    };

    std::vector<Media>      media_;
    std::vector<Clip>       clips_;
    std::vector<Transition> transitions_;
    TimeUs                  playhead_ = 0;
    bool                    playing_  = false;
    bool                    dirty_    = false;

    int next_clip_seq_ = 1;

    std::vector<Snapshot> undo_stack_;
    std::vector<Snapshot> redo_stack_;

    Snapshot snapshot() const;
    void     restore(const Snapshot& s);
};

}  // namespace volchay::core
