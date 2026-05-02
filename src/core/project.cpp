#include "core/project.h"

#include "platform/windows.h"

#include <algorithm>

namespace volchay::core {

namespace {

std::string make_media_id(const std::wstring& abs_path) {
    // The absolute path is stable enough as an id for V0; later we might
    // hash file content + size for cross-machine stability.
    return volchay::narrow(abs_path);
}

std::wstring basename(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

}  // namespace

Project::Project() = default;

const Media* Project::find_media(std::string_view id) const {
    for (const auto& m : media_) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

Media& Project::add_media(const std::wstring& abs_path) {
    std::string id = make_media_id(abs_path);
    for (auto& m : media_) {
        if (m.id == id) return m;
    }
    Media m;
    m.id      = std::move(id);
    m.path    = abs_path;
    m.display = basename(abs_path);
    media_.push_back(std::move(m));
    mark_dirty();
    return media_.back();
}

void Project::update_media_probe(std::string_view id,
                                 TimeUs duration, int w, int h, double fps) {
    for (auto& m : media_) {
        if (m.id == id) {
            m.duration = duration;
            m.width    = w;
            m.height   = h;
            m.fps      = fps;
            m.probed   = true;
            return;
        }
    }
}

Clip& Project::append_clip(const std::string& media_id) {
    const Media* m = find_media(media_id);
    Clip c;
    c.id        = "clip_" + std::to_string(next_clip_seq_++);
    c.media_id  = media_id;
    c.src_in    = 0;
    c.src_out   = m ? m->duration : 0;
    c.t_in      = duration();
    clips_.push_back(c);
    mark_dirty();
    return clips_.back();
}

Clip& Project::set_single_clip(const std::string& media_id) {
    clips_.clear();
    Clip& c = append_clip(media_id);
    c.t_in = 0;
    set_playhead(0);
    return c;
}

void Project::clear_clips() {
    clips_.clear();
    set_playhead(0);
    mark_dirty();
}

TimeUs Project::duration() const {
    TimeUs end = 0;
    for (const auto& c : clips_) {
        end = std::max(end, c.t_out());
    }
    return end;
}

void Project::set_playhead(TimeUs t) {
    if (t < 0) t = 0;
    TimeUs d = duration();
    if (d > 0 && t > d) t = d;
    playhead_ = t;
}

TimeUs Project::source_time_at(TimeUs t, const Clip** out_clip) const {
    for (const auto& c : clips_) {
        if (t >= c.t_in && t < c.t_out()) {
            if (out_clip) *out_clip = &c;
            const TimeUs offset_on_tl = t - c.t_in;
            return c.src_in + TimeUs(double(offset_on_tl) * c.speed);
        }
    }
    if (out_clip) *out_clip = nullptr;
    return -1;
}

void Project::clear_selection() {
    for (auto& c : clips_) c.selected = false;
}

bool Project::any_selected() const {
    for (const auto& c : clips_) if (c.selected) return true;
    return false;
}

int Project::selected_count() const {
    int n = 0;
    for (const auto& c : clips_) if (c.selected) ++n;
    return n;
}

int Project::split_at(TimeUs t) {
    push_undo();
    int created = 0;
    // Snapshot the indices before mutating to avoid iterator invalidation.
    const size_t initial = clips_.size();
    for (size_t i = 0; i < initial; ++i) {
        Clip& c = clips_[i];
        if (t <= c.t_in || t >= c.t_out()) continue;
        const TimeUs split_offset_tl  = t - c.t_in;
        const TimeUs split_offset_src = TimeUs(double(split_offset_tl) * c.speed);
        Clip right            = c;
        right.id              = "clip_" + std::to_string(next_clip_seq_++);
        right.src_in          = c.src_in + split_offset_src;
        right.t_in            = t;
        right.selected        = false;
        c.src_out             = c.src_in + split_offset_src;
        clips_.push_back(right);
        ++created;
    }
    if (created == 0) {
        // No-op: pop the undo entry we just pushed so the stack stays clean.
        if (!undo_stack_.empty()) undo_stack_.pop_back();
        return 0;
    }
    mark_dirty();
    return created;
}

int Project::delete_selected(bool ripple) {
    if (!any_selected()) return 0;
    push_undo();

    // Compute deleted spans (sorted by t_in) for ripple shift logic.
    struct Span { TimeUs t_in, t_out; };
    std::vector<Span> deleted;
    deleted.reserve(clips_.size());

    int removed = 0;
    for (auto it = clips_.begin(); it != clips_.end(); ) {
        if (it->selected) {
            deleted.push_back({it->t_in, it->t_out()});
            it = clips_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed == 0) {
        if (!undo_stack_.empty()) undo_stack_.pop_back();
        return 0;
    }

    if (ripple && !deleted.empty()) {
        std::sort(deleted.begin(), deleted.end(),
                  [](const Span& a, const Span& b) { return a.t_in < b.t_in; });
        // Walk every remaining clip; for each deleted span before it,
        // subtract the span length from t_in.
        for (auto& c : clips_) {
            TimeUs shift = 0;
            for (const auto& d : deleted) {
                if (d.t_out <= c.t_in) {
                    shift += (d.t_out - d.t_in);
                }
            }
            if (shift > 0) c.t_in -= shift;
        }
        // Snap the playhead similarly.
        TimeUs shift = 0;
        for (const auto& d : deleted) {
            if (d.t_out <= playhead_) shift += (d.t_out - d.t_in);
            else if (d.t_in <= playhead_ && d.t_out > playhead_) {
                shift += (playhead_ - d.t_in);
            }
        }
        if (shift > 0) playhead_ -= shift;
    }

    mark_dirty();
    return removed;
}

bool Project::trim_in(const std::string& clip_id, TimeUs new_t_in) {
    for (auto& c : clips_) {
        if (c.id != clip_id) continue;
        if (new_t_in < 0) new_t_in = 0;
        if (new_t_in >= c.t_out()) return false;
        push_undo();
        const TimeUs delta_tl  = new_t_in - c.t_in;
        const TimeUs delta_src = TimeUs(double(delta_tl) * c.speed);
        c.src_in += delta_src;
        c.t_in    = new_t_in;
        mark_dirty();
        return true;
    }
    return false;
}

bool Project::trim_out(const std::string& clip_id, TimeUs new_t_out) {
    for (auto& c : clips_) {
        if (c.id != clip_id) continue;
        if (new_t_out <= c.t_in) return false;
        push_undo();
        // Adjust src_out so the new clip has the requested length.
        const TimeUs new_dur_tl   = new_t_out - c.t_in;
        const TimeUs new_dur_src  = TimeUs(double(new_dur_tl) * c.speed);
        TimeUs target_src_out     = c.src_in + new_dur_src;
        // Clamp against the source media duration if known.
        if (auto* m = const_cast<Media*>(find_media(c.media_id))) {
            if (m->duration > 0 && target_src_out > m->duration) {
                target_src_out = m->duration;
            }
        }
        c.src_out = target_src_out;
        mark_dirty();
        return true;
    }
    return false;
}

Project::Snapshot Project::snapshot() const {
    return Snapshot{clips_, playhead_};
}

void Project::restore(const Snapshot& s) {
    clips_    = s.clips;
    playhead_ = s.playhead;
}

void Project::push_undo() {
    constexpr size_t kMaxUndo = 100;
    undo_stack_.push_back(snapshot());
    if (undo_stack_.size() > kMaxUndo) {
        undo_stack_.erase(undo_stack_.begin());
    }
    redo_stack_.clear();
}

bool Project::undo() {
    if (undo_stack_.empty()) return false;
    redo_stack_.push_back(snapshot());
    restore(undo_stack_.back());
    undo_stack_.pop_back();
    mark_dirty();
    return true;
}

bool Project::redo() {
    if (redo_stack_.empty()) return false;
    undo_stack_.push_back(snapshot());
    restore(redo_stack_.back());
    redo_stack_.pop_back();
    mark_dirty();
    return true;
}

}  // namespace volchay::core
