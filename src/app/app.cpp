#include "app/app.h"

#include "ui/fonts.h"
#include "ui/theme.h"
#include "util/log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>

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

    if (!initialise_imgui()) return false;
    trace.mark("imgui ready");

    return true;
}

void App::open_initial_video(const std::wstring& path) {
    if (!path.empty()) open_video(path);
}

void App::open_video(const std::wstring& path) {
    if (path.empty()) {
        std::wstring picked = Window::pick_video_file(window_.hwnd());
        if (picked.empty()) return;
        return open_video(picked);
    }

    log::info("open_video: %s", volchay::narrow(path).c_str());
    player_.set_prefer_hardware(settings_.hardware_decode);
    if (!player_.open(path, d3d_.device())) {
        log::err("Failed to open video file");
        return;
    }
    audio_.open(path);
    audio_.set_volume(settings_.audio_volume);
    audio_.set_muted(settings_.audio_mute);

    auto& m = project_.add_media(path);
    project_.update_media_probe(m.id,
        player_.duration(), player_.width(), player_.height(), player_.fps());
    project_.set_single_clip(m.id);
    project_.set_playing(true);
    was_playing_ = true;
    audio_.play();
}

void App::advance_playhead(double dt_seconds) {
    if (!project_.is_playing()) return;
    if (project_.duration() <= 0) return;

    // The audio engine's clock is FILE PTS (offset inside the source
    // media), but project_.playhead() is TIMELINE PTS (offset on the
    // edit timeline, after trim / split / multi-clip layout). Translate
    // file -> timeline through the active clip so trimming a clip
    // doesn't desync audio from the visual playhead.
    core::TimeUs ph;
    if (audio_.has_audio() && audio_.current_pts_us() >= 0) {
        const core::TimeUs file_pts = audio_.current_pts_us();
        const auto& clips = project_.clips();
        const core::Clip* active = nullptr;
        for (const auto& c : clips) {
            if (file_pts >= c.src_in && file_pts < c.src_out) {
                active = &c;
                break;
            }
        }
        if (active) {
            const double dur_src = double(file_pts - active->src_in);
            const double dur_tl  = (active->speed > 0.0)
                                 ? dur_src / active->speed
                                 : dur_src;
            ph = active->t_in + core::TimeUs(dur_tl);
        } else if (!clips.empty()) {
            // Audio is past the trimmed-out tail (or before src_in).
            // Treat as "reached end" for the active clip.
            const auto& c = clips.front();
            ph = (file_pts < c.src_in) ? c.t_in : c.t_out();
        } else {
            ph = file_pts;
        }
    } else {
        ph = project_.playhead();
        ph += core::TimeUs(dt_seconds * 1'000'000.0);
    }
    if (ph >= project_.duration()) {
        if (settings_.loop_playback) {
            ph = 0;
            // Seek audio to the FILE PTS at timeline 0, which is the
            // first clip's src_in (not necessarily 0 in the file).
            core::TimeUs file_t = project_.source_time_at(0);
            if (file_t < 0) file_t = 0;
            audio_.seek(file_t);
            player_.seek(file_t);
        } else {
            ph = project_.duration();
            project_.set_playing(false);
            audio_.pause();
        }
    }
    project_.set_playhead(ph);
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

    if (player_.is_open()) {
        // pump() wants FILE PTS, not timeline PTS — translate through
        // the active clip so trimmed/split layouts request the right
        // source frame.
        const core::TimeUs tl = project_.playhead();
        core::TimeUs file_t = project_.source_time_at(tl);
        if (file_t < 0) file_t = tl;
        player_.pump(file_t);
    }

    // Apply audio settings every frame (cheap atomics).
    audio_.set_volume(settings_.audio_volume);
    audio_.set_muted(settings_.audio_mute);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ui::EditorContext ctx;
    ctx.project   = &project_;
    ctx.player    = &player_;
    ctx.audio     = &audio_;
    ctx.exporter  = &exporter_;
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
