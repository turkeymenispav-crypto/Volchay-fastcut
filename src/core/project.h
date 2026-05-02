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

    // Append a clip referencing media_id. If the clip does not yet have a
    // timeline position, it is placed at the current playhead.
    Clip& append_clip(const std::string& media_id);

    // Replace the (sole) clip on the timeline with a single clip spanning
    // the entire media, positioned at 0. Used when opening a video from
    // the context menu / drag&drop.
    Clip& set_single_clip(const std::string& media_id);

    // Remove all clips.
    void clear_clips();

    const std::vector<Media>& media() const { return media_; }
    const std::vector<Clip>&  clips() const { return clips_; }
    std::vector<Clip>&        clips_mut()   { return clips_; }

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

    std::vector<Media> media_;
    std::vector<Clip>  clips_;
    TimeUs             playhead_ = 0;
    bool               playing_  = false;
    bool               dirty_    = false;

    int next_clip_seq_ = 1;

    std::vector<Snapshot> undo_stack_;
    std::vector<Snapshot> redo_stack_;

    Snapshot snapshot() const;
    void     restore(const Snapshot& s);
};

}  // namespace volchay::core
