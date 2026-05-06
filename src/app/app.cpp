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
        if (active_m && active_m->path == current_media_path_) {
            // We're playing inside a clip whose media matches the audio
            // engine's currently-loaded file — apply a soft drift
            // correction toward the audio master clock.
            const core::TimeUs file_pts = audio_.current_pts_us();
            if (file_pts >= active->src_in && file_pts < active->src_out) {
                const double dur_src = double(file_pts - active->src_in);
                const double dur_tl  = (active->speed > 0.0)
                                     ? dur_src / active->speed
                                     : dur_src;
                const core::TimeUs ph_audio =
                    active->t_in + core::TimeUs(dur_tl);
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

void App::sync_media_to_playhead() {
    if (project_.clips().empty()) return;

    const core::Clip* active = nullptr;
    (void)project_.source_time_at(project_.playhead(), &active);
    if (!active) return;

    const core::Media* m = project_.find_media(active->media_id);
    if (!m) return;

    if (m->path == current_media_path_) return;
    const std::wstring wanted = m->path;

    // Crossed a clip boundary into a clip whose source media is not
    // loaded in the AV decoder + audio engine yet. Re-open both, then
    // seek inside the new file to the FILE PTS that corresponds to the
    // current playhead position inside the new clip.
    log::info("sync_media_to_playhead: switching to %s",
              volchay::narrow(wanted).c_str());

    player_.set_prefer_hardware(settings_.hardware_decode);
    if (player_.open(wanted, d3d_.device())) {
        current_media_path_ = wanted;
    } else {
        log::err("sync_media_to_playhead: failed to open %s",
                 volchay::narrow(wanted).c_str());
        return;
    }

    audio_.open(wanted);
    audio_.set_volume(settings_.audio_volume);
    audio_.set_muted(settings_.audio_mute);

    const core::TimeUs file_t = project_.source_time_at(project_.playhead());
    if (file_t >= 0) {
        player_.seek(file_t);
        audio_.seek(file_t);
    }
    if (project_.is_playing()) audio_.play();
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

    if (player_.is_open()) {
        // pump() wants FILE PTS, not timeline PTS — translate through
        // the active clip so trimmed/split layouts request the right
        // source frame.
        const core::TimeUs tl = project_.playhead();
        core::TimeUs file_t = project_.source_time_at(tl);
        if (file_t < 0) file_t = tl;
        player_.pump(file_t);
    }

    // Apply audio settings every frame (cheap atomics). The effective
    // volume/mute is the *combined* state of the global Settings panel
    // sliders AND the per-clip controls in the Inspector for the clip
    // currently under the playhead, so both UIs visibly affect playback.
    {
        float       gain = settings_.audio_volume;
        bool        mute = settings_.audio_mute;
        const auto& cs   = project_.clips();
        const core::TimeUs ph = project_.playhead();
        for (const auto& c : cs) {
            if (ph >= c.t_in && ph < c.t_out()) {
                gain *= c.volume;
                if (c.muted) mute = true;
                break;
            }
        }
        audio_.set_volume(gain);
        audio_.set_muted(mute);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ui::EditorContext ctx;
    ctx.project   = &project_;
    ctx.player    = &player_;
    ctx.audio     = &audio_;
    ctx.exporter  = &exporter_;
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
