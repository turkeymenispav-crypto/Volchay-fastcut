#include "app/app.h"

#include "ui/fonts.h"
#include "ui/theme.h"
#include "util/log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cwctype>

namespace volchay::app {

App::App()  = default;
App::~App() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

bool App::initialise_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    static std::string ini_path;
    PWSTR roaming = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0,
                                         nullptr, &roaming)) && roaming) {
        std::wstring dir = roaming;
        ::CoTaskMemFree(roaming);
        dir += L"\\Volchay";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        ini_path = volchay::narrow(dir + L"\\imgui.ini");
        io.IniFilename = ini_path.c_str();
    }

    ui::apply_theme();

    if (!ImGui_ImplWin32_Init(window_.hwnd())) return false;
    if (!ImGui_ImplDX11_Init(d3d_.device(), d3d_.context())) return false;

    // Initial font load. We do this AFTER the DX11 backend init so the
    // first NewFrame can already see the atlas.
    UINT dpi = ::GetDpiForWindow(window_.hwnd());
    if (dpi == 0) dpi = 96;
    ui::rebuild_fonts(settings_, float(dpi) / 96.0f);
    fonts_dirty_ = false;
    return true;
}

void App::rebuild_fonts_if_needed() {
    if (!fonts_dirty_) return;
    UINT dpi = ::GetDpiForWindow(window_.hwnd());
    if (dpi == 0) dpi = 96;
    ui::rebuild_fonts(settings_, float(dpi) / 96.0f);
    fonts_dirty_ = false;
}

bool App::initialize(int show_cmd, StartupTrace& trace) {
    trace_ = &trace;

    settings_ = SettingsStore::load();
    trace.mark("settings loaded");

    if (!window_.create(1440, 900, L"Volchay-fastcut")) return false;
    trace.mark("window created");

    window_.set_drop_callback(
        [this](const std::wstring& p) { open_video(p); });
    window_.set_resize_callback(
        [this](int w, int h) {
            (void)w; (void)h;
            d3d_.resize(UINT(w), UINT(h));
        });

    window_.show(show_cmd);
    trace.mark("window shown");

    if (!d3d_.initialize(window_.hwnd())) return false;
    trace.mark("d3d ready");

    thumbs_.set_device(d3d_.device());

    if (!initialise_imgui()) return false;
    trace.mark("imgui ready");

    return true;
}

void App::open_initial_video(const std::wstring& path) {
    if (!path.empty()) open_video(path);
}

namespace {

// Heuristic: still-image extensions (lower-cased). FFmpeg can decode
// these via image2 demuxer just fine, but they have ~0 duration so we
// give the resulting clip a reasonable default time on the timeline.
bool is_still_image_path(const std::wstring& path) {
    auto ext_pos = path.find_last_of(L'.');
    if (ext_pos == std::wstring::npos) return false;
    std::wstring ext = path.substr(ext_pos);
    for (auto& c : ext) c = wchar_t(::towlower(c));
    return ext == L".png" || ext == L".jpg"  || ext == L".jpeg"
        || ext == L".bmp" || ext == L".webp" || ext == L".gif"
        || ext == L".tiff" || ext == L".tif";
}

constexpr core::TimeUs kDefaultStillDuration = 5'000'000;  // 5 s

}  // namespace

void App::open_video(const std::wstring& path) {
    if (path.empty()) {
        std::wstring picked = Window::pick_video_file(window_.hwnd());
        if (picked.empty()) return;
        return open_video(picked);
    }

    log::info("open_video: %s", volchay::narrow(path).c_str());

    const bool is_image = is_still_image_path(path);

    if (!is_image) {
        // Video file path — open through the AV decoder + WASAPI audio.
        player_.set_prefer_hardware(settings_.hardware_decode);
        if (!player_.open(path, d3d_.device())) {
            log::err("Failed to open video file");
            return;
        }
        audio_.open(path);
        audio_.set_volume(settings_.audio_volume);
        audio_.set_muted(settings_.audio_mute);
        current_media_path_ = path;
    }

    auto& m = project_.add_media(path);
    if (is_image) {
        // Use the default still duration; libav reports 0 for stills,
        // which would produce a zero-length clip on the timeline.
        project_.update_media_probe(m.id, kDefaultStillDuration,
                                    /*w*/ 0, /*h*/ 0, /*fps*/ 0.0);
    } else {
        project_.update_media_probe(m.id,
            player_.duration(), player_.width(), player_.height(),
            player_.fps());
    }

    // Append behaviour: on the very first import, place the clip at 0
    // and start playback. On subsequent imports, drop the clip onto
    // the end of the existing timeline so the user is "adding" to the
    // project, not replacing it.
    const bool first_clip = project_.clips().empty();
    if (first_clip) {
        project_.set_single_clip(m.id);
        if (!is_image) {
            project_.set_playing(true);
            was_playing_ = true;
            audio_.play();
        }
    } else {
        // Place at the current timeline tail.
        project_.append_clip(m.id);
        // Don't restart playback; user keeps editing where they were.
    }
}

void App::advance_playhead(double dt_seconds) {
    if (!project_.is_playing()) return;
    if (project_.duration() <= 0) return;

    // Multi-clip timeline. The audio engine is only ever loaded with the
    // source file of one clip at a time, so it can't be the master clock
    // across boundaries. We instead drive the playhead from wall-clock
    // dt (which is what every NLE actually does — the audio worker keeps
    // its own internal clock for sample alignment, but the timeline
    // playhead is driven by render time).
    //
    // Within a single clip, however, we still snap to audio PTS to keep
    // A/V in lock-step: any drift between dt-accumulation and the
    // audio worker's actual playback would surface as desync over a
    // long clip. So we use wall-clock dt for "free time" and a small
    // drift correction toward audio PTS when the audio engine has the
    // currently-active clip's media loaded.
    core::TimeUs ph = project_.playhead();
    ph += core::TimeUs(dt_seconds * 1'000'000.0);

    if (audio_.has_audio() && audio_.current_pts_us() >= 0) {
        const core::Clip* active = nullptr;
        (void)project_.source_time_at(ph, &active);
        const core::Media* active_m =
            active ? project_.find_media(active->media_id) : nullptr;
        // The audio engine plays at the source's native sample rate,
        // so its PTS only tracks wall-clock when the active clip is
        // playing at 1.0x. For any other speed we'd snap the playhead
        // to file_pts/speed and effectively cancel the slow-down. Skip
        // drift correction in that case and let wall-clock dt drive
        // the playhead; the per-frame mapping in sync_media_to_playhead
        // takes care of feeding the right FILE PTS to the video player.
        const bool one_x = active && std::abs(active->speed - 1.0) < 1e-6;
        if (one_x && active_m && active_m->path == current_media_path_) {
            const core::TimeUs file_pts = audio_.current_pts_us();
            if (file_pts >= active->src_in && file_pts < active->src_out) {
                const core::TimeUs ph_audio =
                    active->t_in + (file_pts - active->src_in);
                // Snap on big jumps (>50ms drift), low-pass-filter on small.
                core::TimeUs delta = ph_audio - ph;
                if (delta < -50'000 || delta > 50'000) {
                    ph = ph_audio;
                } else {
                    ph += delta / 4;   // 25 % gain LPF
                }
            }
        }
    }

    if (ph >= project_.duration()) {
        if (settings_.loop_playback) {
            ph = 0;
        } else {
            ph = project_.duration();
            project_.set_playing(false);
            audio_.pause();
        }
    }
    project_.set_playhead(ph);
}

void App::extract_audio_to_sidecar() {
    if (current_media_path_.empty()) {
        ::MessageBoxW(window_.hwnd(),
                      L"No video is open.",
                      L"Volchay-fastcut",
                      MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (exporter_.busy()) {
        ::MessageBoxW(window_.hwnd(),
                      L"An export is already running. Wait for it to finish.",
                      L"Volchay-fastcut",
                      MB_ICONINFORMATION | MB_OK);
        return;
    }
    // Build sibling .m4a path: <stem>.m4a alongside the source.
    std::wstring out = current_media_path_;
    size_t dot = out.find_last_of(L'.');
    size_t sep = out.find_last_of(L"/\\");
    if (dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep)) {
        out.resize(dot);
    }
    out += L".m4a";

    media::ExportRequest req;
    req.source_path    = current_media_path_;
    req.output_path    = out;
    req.audio_only     = true;
    req.audio_bitrate  = 192000;
    if (!exporter_.start(req)) {
        ::MessageBoxW(window_.hwnd(),
                      L"Failed to start audio extract.",
                      L"Volchay-fastcut",
                      MB_ICONERROR | MB_OK);
        return;
    }
    ::MessageBoxW(window_.hwnd(),
                  L"Extracting audio... Progress is shown in the status bar; "
                  L"the .m4a file will appear next to your source when done.",
                  L"Volchay-fastcut",
                  MB_ICONINFORMATION | MB_OK);
}

void App::sync_media_to_playhead() {
    const core::Clip* top = nullptr;
    const core::Clip* bot = nullptr;
    if (!project_.clips().empty()) {
        project_.clips_at(project_.playhead(), &top, &bot);
    }
    // An A-track clip can outlive the V-track clip whose audio it was
    // detached from (the user may move/extend it). When that happens
    // we still want the audio engine to keep playing this file, so
    // promote the highest-index A-track clip into "top" if no video
    // clip is at the playhead.
    const core::Clip* audio_only_top = nullptr;
    if (!top) {
        const core::TimeUs ph = project_.playhead();
        for (const auto& c : project_.clips()) {
            if (c.track >= 0) continue;
            if (ph < c.t_in || ph >= c.t_out()) continue;
            if (!audio_only_top || c.track > audio_only_top->track) {
                audio_only_top = &c;
            }
        }
    }

    // No video AND no audio clip alive at the current playhead: gap,
    // project emptied, or playhead past duration. The audio engine
    // still has whatever file it last loaded mapped, and its WASAPI
    // worker keeps consuming samples until paused — that's what
    // produced the "I deleted the clip but the sound keeps playing"
    // report. Mute and pause it; the viewer's per-frame "no top
    // frame" branch already drops to black.
    if (!top && !bot && !audio_only_top) {
        audio_.pause();
        audio_.set_muted(true);
        if (!current_media_path_.empty()) {
            player_.close();
            current_media_path_.clear();
        }
        if (!current_media_path_bot_.empty()) {
            player_bot_.close();
            current_media_path_bot_.clear();
        }
        return;
    }

    // ---- Top track / audio master ----
    // For purposes of "what file should the audio engine and the top
    // video player be loaded with", an A-track clip overrides when
    // there's no V-track clip at the playhead. The video stage will
    // see top == nullptr, leave the canvas black, and the audio stage
    // will play this file.
    const core::Clip* audio_master = top ? top : audio_only_top;
    if (audio_master) {
        const core::Media* m = project_.find_media(audio_master->media_id);
        if (m && m->path != current_media_path_) {
            const std::wstring wanted = m->path;
            log::info("sync_media_to_playhead: TOP -> %s",
                      volchay::narrow(wanted).c_str());
            // Only open the video decoder when we actually have a
            // V-track clip at the playhead. For an A-only window we
            // want the canvas to stay black until we hit a V clip.
            if (top) {
                player_.set_prefer_hardware(settings_.hardware_decode);
                if (player_.open(wanted, d3d_.device())) {
                    current_media_path_ = wanted;
                } else {
                    log::err("sync_media_to_playhead: TOP failed to open %s",
                             volchay::narrow(wanted).c_str());
                }
            } else if (!current_media_path_.empty()) {
                player_.close();
                current_media_path_.clear();
            }
            audio_.open(wanted);
            audio_.set_volume(settings_.audio_volume);
            audio_.set_muted(settings_.audio_mute);
            const core::TimeUs offset_on_tl =
                project_.playhead() - audio_master->t_in;
            const core::TimeUs file_t = audio_master->src_in
                + core::TimeUs(double(offset_on_tl) * audio_master->speed);
            if (file_t >= 0) {
                if (top) player_.seek(file_t);
                audio_.seek(file_t);
            }
            if (project_.is_playing()) audio_.play();
        }
    }

    // ---- Bottom layer (decode-only, used by the viewer for layered
    //      compositing). No audio mixing for now. ----
    if (bot) {
        const core::Media* m = project_.find_media(bot->media_id);
        if (m && m->path != current_media_path_bot_) {
            const std::wstring wanted = m->path;
            log::info("sync_media_to_playhead: BOT -> %s",
                      volchay::narrow(wanted).c_str());
            player_bot_.set_prefer_hardware(settings_.hardware_decode);
            if (player_bot_.open(wanted, d3d_.device())) {
                current_media_path_bot_ = wanted;
            } else {
                log::err("sync_media_to_playhead: BOT failed to open %s",
                         volchay::narrow(wanted).c_str());
            }
        }
    } else if (!current_media_path_bot_.empty()) {
        // No bottom layer at this playhead — close the player so the
        // viewer renders a single layer cleanly.
        player_bot_.close();
        current_media_path_bot_.clear();
    }
}

void App::render_one_frame() {
    HiResClock frame_timer;

    rebuild_fonts_if_needed();

    const double now_s = wall_clock_.seconds();
    const double dt    = (last_frame_time_s_ > 0)
                       ? (now_s - last_frame_time_s_) : 0.0;
    last_frame_time_s_ = now_s;

    // Detect play/pause edge and forward to the audio engine. The
    // timeline's "Pause" button toggles project_.set_playing(false), but
    // the WASAPI worker only stops when AudioPlayer::pause() is called.
    const bool now_playing = project_.is_playing();
    if (now_playing != was_playing_) {
        if (now_playing) audio_.play();
        else             audio_.pause();
        was_playing_ = now_playing;
    }

    advance_playhead(dt);

    // Multi-clip playback: if the playhead crossed a clip boundary
    // into a clip whose source media is not currently loaded in the
    // player + audio engine, swap them now (and seek to the right
    // FILE PTS inside the new file).
    sync_media_to_playhead();

    {
        const core::TimeUs tl = project_.playhead();
        const core::Clip* top = nullptr;
        const core::Clip* bot = nullptr;
        project_.clips_at(tl, &top, &bot);

        if (top && player_.is_open()) {
            // pump() wants FILE PTS, not timeline PTS — translate
            // through the active clip so trimmed/split layouts
            // request the right source frame.
            const core::TimeUs offset = tl - top->t_in;
            core::TimeUs file_t =
                top->src_in + core::TimeUs(double(offset) * top->speed);
            if (file_t < 0) file_t = tl;
            player_.pump(file_t);
        }
        if (bot && player_bot_.is_open()) {
            const core::TimeUs offset = tl - bot->t_in;
            core::TimeUs file_t =
                bot->src_in + core::TimeUs(double(offset) * bot->speed);
            if (file_t < 0) file_t = tl;
            player_bot_.pump(file_t);
        }
    }

    // Apply audio settings every frame (cheap atomics). The effective
    // volume/mute is the *combined* state of the global Settings panel
    // sliders AND the per-clip controls in the Inspector for the clip
    // currently under the playhead, so both UIs visibly affect playback.
    {
        float       gain = settings_.audio_volume;
        bool        mute = settings_.audio_mute;
        const core::TimeUs ph = project_.playhead();
        // Resolve the clip that drives audio mute / volume at the
        // playhead. Priority order:
        //   1) An A-track (track < 0) clip overlapping the playhead —
        //      that's where "Extract audio" puts the audio. If
        //      multiple, the highest A-track index (closest to A1)
        //      wins.
        //   2) Otherwise, the topmost video clip — the same one
        //      sync_media_to_playhead loaded into the audio engine.
        // This keeps the per-clip Inspector controls on V0 from
        // overriding a non-muted A1 clip.
        const core::Clip* audio_clip = nullptr;
        for (const auto& c : project_.clips()) {
            if (c.track >= 0) continue;
            if (ph < c.t_in || ph >= c.t_out()) continue;
            if (!audio_clip || c.track > audio_clip->track) audio_clip = &c;
        }
        if (!audio_clip) {
            const core::Clip* top = nullptr;
            const core::Clip* bot = nullptr;
            project_.clips_at(ph, &top, &bot);
            audio_clip = top ? top : bot;
        }
        if (audio_clip) {
            gain *= audio_clip->volume;
            if (audio_clip->muted) mute = true;
            // Slow / fast clips: the audio engine plays at the source's
            // native rate so its output would race ahead (or fall
            // behind) the timeline. Mute it so we don't hear "fast
            // audio for 1s then silence for 3s" at 0.25x. Proper
            // speed-aware audio (atempo / SWR rate change) is on the
            // todo list.
            if (std::abs(audio_clip->speed - 1.0) > 1e-6) mute = true;
        }
        audio_.set_volume(gain);
        audio_.set_muted(mute);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ui::EditorContext ctx;
    ctx.project    = &project_;
    ctx.player     = &player_;
    ctx.player_bot = &player_bot_;
    ctx.audio      = &audio_;
    ctx.exporter   = &exporter_;
    ctx.thumbs    = &thumbs_;
    ctx.startup   = trace_;
    ctx.settings  = &settings_;
    ctx.show_settings    = &show_settings_;
    ctx.show_export      = &show_export_;
    ctx.show_diagnostics = &show_diagnostics_;
    ctx.session_seconds = now_s;
    ctx.last_frame_ms   = last_frame_ms_;
    ctx.open_file       = [this](const std::wstring& p) { open_video(p); };
    ctx.exit_app        = [this]() { window_.request_close(); };
    ctx.extract_audio   = [this]() { extract_audio_to_sidecar(); };

    layout_.render(ctx);

    // Settings panel may flip fonts_dirty_ so the next frame rebuilds.
    if (layout_.settings_atlas_dirty()) {
        fonts_dirty_ = true;
    }

    ImGui::Render();

    const float clear[4] = { 0.06f, 0.06f, 0.065f, 1.0f };
    d3d_.begin_frame(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    d3d_.end_frame(/*vsync=*/settings_.vsync);

    last_frame_ms_ = frame_timer.millis();
}

int App::run() {
    if (trace_) trace_->mark("entering main loop");
    log::info("Main loop starting (cold start %.1f ms)",
              trace_ ? trace_->total_millis() : 0.0);

    while (window_.pump_messages()) {
        if (layout_.exit_requested()) {
            window_.request_close();
        }
        render_one_frame();

        // When the user is not interacting and nothing is playing, sleep
        // briefly so we don't burn 100% of a CPU core. Vsync alone is not
        // enough on minimised / off-screen windows.
        const ImGuiIO& io = ImGui::GetIO();
        const bool busy = project_.is_playing()
                       || io.WantCaptureMouse
                       || io.WantCaptureKeyboard
                       || io.WantTextInput
                       || io.MouseDelta.x != 0.0f
                       || io.MouseDelta.y != 0.0f;
        if (!busy && !settings_.vsync) {
            // Idle frame budget: ~15 ms (= 60-ish Hz cap without vsync).
            window_.wait_for_message(15);
        }
    }
    return 0;
}

}  // namespace volchay::app
