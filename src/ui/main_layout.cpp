#include "ui/main_layout.h"

#include "shell/registry.h"
#include "ui/panels/export_panel.h"
#include "ui/panels/inspector.h"
#include "ui/panels/library.h"
#include "ui/panels/settings.h"
#include "ui/panels/timeline.h"
#include "ui/panels/viewer.h"
#include "ui/theme.h"
#include "util/log.h"

#include <imgui.h>
#include <imgui_internal.h>   // for DockBuilder*

namespace volchay::ui {

MainLayout::MainLayout() = default;

void MainLayout::install_dock_layout(unsigned dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id,
        ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspace_id,
        ImGui::GetMainViewport()->WorkSize);

    // Lightroom-like layout:
    //  +--------------------------------------------------+
    //  | Menu bar                                         |
    //  +---------+----------------------------+-----------+
    //  | Library | Viewer (centre)            | Inspector |
    //  |         |                            |           |
    //  +---------+----------------------------+-----------+
    //  | Timeline (full width, bottom)                    |
    //  +--------------------------------------------------+
    //  | Status strip                                     |
    //  +--------------------------------------------------+

    ImGuiID dock_main = dockspace_id;
    ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Down, 0.30f, nullptr, &dock_main);
    ImGuiID dock_left = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Left, 0.20f, nullptr, &dock_main);
    ImGuiID dock_right = ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Right, 0.22f, nullptr, &dock_main);

    ImGui::DockBuilderDockWindow("Library",   dock_left);
    ImGui::DockBuilderDockWindow("Viewer",    dock_main);
    ImGui::DockBuilderDockWindow("Inspector", dock_right);
    ImGui::DockBuilderDockWindow("Timeline",  dock_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}

void MainLayout::draw_menu_bar(EditorContext& ctx) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open video...", "Ctrl+O")) {
            if (ctx.open_file) ctx.open_file(L"");
        }
        ImGui::Separator();
        const bool can_export = ctx.player && ctx.player->is_open();
        if (ImGui::MenuItem("Export...", "Ctrl+E", false, can_export)) {
            if (ctx.show_export) *ctx.show_export = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Register context menu...")) {
            LONG e = volchay::shell::register_context_menu(true);
            wchar_t msg[256];
            if (e == ERROR_SUCCESS) {
                ::lstrcpyW(msg,
                    L"Registered. Right-click any video file -> "
                    L"'Open with Volchay-fastcut'.");
                ::MessageBoxW(nullptr, msg, L"Volchay-fastcut",
                              MB_ICONINFORMATION | MB_OK);
            } else {
                ::wsprintfW(msg, L"Failed (error %ld).", long(e));
                ::MessageBoxW(nullptr, msg, L"Volchay-fastcut",
                              MB_ICONERROR | MB_OK);
            }
        }
        if (ImGui::MenuItem("Unregister context menu")) {
            LONG e = volchay::shell::unregister_context_menu(true);
            wchar_t msg[128];
            if (e == ERROR_SUCCESS) {
                ::MessageBoxW(nullptr, L"Unregistered.", L"Volchay-fastcut",
                              MB_ICONINFORMATION | MB_OK);
            } else {
                ::wsprintfW(msg, L"Failed (error %ld).", long(e));
                ::MessageBoxW(nullptr, msg, L"Volchay-fastcut",
                              MB_ICONERROR | MB_OK);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) {
            exit_requested_ = true;
            if (ctx.exit_app) ctx.exit_app();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        ImGui::MenuItem("Undo (todo)", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo (todo)", "Ctrl+Y", false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...", "Ctrl+,")) {
            if (ctx.show_settings) *ctx.show_settings = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Show startup trace", nullptr, &show_startup_log_);
        if (ctx.show_settings) {
            ImGui::MenuItem("Settings panel", nullptr, ctx.show_settings);
        }
        if (ctx.show_export) {
            ImGui::MenuItem("Export panel", nullptr, ctx.show_export);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Volchay-fastcut")) show_about_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void MainLayout::draw_status_strip(EditorContext& ctx) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos (ImVec2(vp->WorkPos.x,
                                    vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme().bg_panel_alt);
    if (ImGui::Begin("##StatusStrip", nullptr, flags)) {
        ImGui::AlignTextToFramePadding();

        // Project state.
        if (ctx.project) {
            ImGui::TextColored(theme().text_dim, "Clips:");
            ImGui::SameLine();
            ImGui::Text("%zu", ctx.project->clips().size());
            ImGui::SameLine(0, 16);
            ImGui::TextColored(theme().text_dim, "Duration:");
            ImGui::SameLine();
            const double s = core::to_seconds(ctx.project->duration());
            ImGui::Text("%.2fs", s);
        }

        // Performance.
        ImGui::SameLine(0, 24);
        ImGui::TextColored(theme().text_dim, "Frame:");
        ImGui::SameLine();
        ImGui::Text("%.2f ms", ctx.last_frame_ms);

        ImGui::SameLine(0, 24);
        ImGui::TextColored(theme().text_dim, "Up:");
        ImGui::SameLine();
        ImGui::Text("%.1fs", ctx.session_seconds);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void MainLayout::render(EditorContext& ctx) {
    // Host window covers the entire viewport with a dock space, leaving
    // room above for the menu bar and below for the status strip.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float status_h = ImGui::GetFrameHeight();

    ImGui::SetNextWindowPos (ImVec2(vp->WorkPos.x,  vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - status_h));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags host_flags =
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::Begin("##HostWindow", nullptr, host_flags);
    ImGui::PopStyleVar();

    draw_menu_bar(ctx);

    ImGuiID dockspace_id = ImGui::GetID("VolchayDockSpace");
    if (!first_layout_done_) {
        install_dock_layout(dockspace_id);
        first_layout_done_ = true;
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0, 0),
                     ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();

    panels::draw_library  (ctx);
    panels::draw_viewer   (ctx);
    panels::draw_timeline (ctx);
    panels::draw_inspector(ctx);

    if (panels::draw_settings(ctx)) settings_atlas_dirty_ = true;
    panels::draw_export(ctx);

    draw_status_strip(ctx);

    if (show_about_) {
        ImGui::OpenPopup("About Volchay-fastcut");
        show_about_ = false;
    }
    if (ImGui::BeginPopupModal("About Volchay-fastcut", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Volchay-fastcut " VOLCHAY_VERSION_STRING);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Native Windows 11 video editor focused on instant cold start "
            "and frame-accurate editing. ImGui + Direct3D 11 + Media Foundation.");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (show_startup_log_ && ctx.startup) {
        ImGui::Begin("Startup trace", &show_startup_log_,
                     ImGuiWindowFlags_AlwaysAutoResize);
        for (int i = 0; i < ctx.startup->count(); ++i) {
            const auto& e = ctx.startup->at(i);
            ImGui::Text("%6.1f ms  %s", e.millis, e.name);
        }
        ImGui::Separator();
        ImGui::Text("Total: %.1f ms", ctx.startup->total_millis());
        ImGui::Separator();
        ImGui::TextDisabled("Log file: %s", log::path().c_str());
        ImGui::End();
    }
}

}  // namespace volchay::ui
