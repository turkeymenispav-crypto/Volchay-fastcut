#include "ui/panels/settings.h"

#include "ui/theme.h"
#include "util/settings.h"

#include <imgui.h>

#include <cstring>

namespace volchay::ui::panels {

bool draw_settings(EditorContext& ctx) {
    if (!ctx.settings || !ctx.show_settings || !*ctx.show_settings) return false;

    bool atlas_dirty = false;
    Settings& s = *ctx.settings;
    Settings before = s;

    ImGui::SetNextWindowSize(ImVec2(440, 540), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Settings", ctx.show_settings,
                     ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(theme().text_dim,
            "Settings persist to %%LOCALAPPDATA%%\\Volchay\\settings.ini.");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Typography",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* fonts[] = {
                "Segoe UI Variable (Windows 11 default)",
                "Segoe UI",
                "Tahoma (low-DPI-friendly)",
            };
            if (ImGui::Combo("Font", &s.font_choice, fonts, IM_ARRAYSIZE(fonts)))
                atlas_dirty = true;

            if (ImGui::SliderFloat("Size (px)", &s.font_size_px, 11.0f, 24.0f, "%.1f"))
                atlas_dirty = true;

            if (ImGui::Checkbox("Anti-aliased glyphs", &s.font_anti_alias))
                atlas_dirty = true;
            ImGui::SameLine();
            ImGui::TextColored(theme().text_dim, "(greyscale AA in atlas)");

            if (ImGui::Checkbox("Sub-pixel rendering", &s.font_subpixel))
                atlas_dirty = true;
            ImGui::SameLine();
            ImGui::TextColored(theme().text_dim,
                "(LCD-style; needs RGB monitor)");

            if (ImGui::Checkbox("Horizontal oversampling 2x",
                                &s.font_oversample_h))
                atlas_dirty = true;
            ImGui::SameLine();
            ImGui::TextColored(theme().text_dim,
                "(crisper at non-integer scales)");
        }

        if (ImGui::CollapsingHeader("Rendering",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("V-Sync", &s.vsync);
            ImGui::SameLine();
            ImGui::TextColored(theme().text_dim,
                "(Present(1,0) — caps frame rate to display refresh)");

            ImGui::Checkbox("Hardware video decode (DXVA)",
                            &s.hardware_decode);
            ImGui::SameLine();
            ImGui::TextColored(theme().text_dim,
                "(NVENC/QSV/AMF when available)");

            ImGui::Checkbox("Anti-aliased lines & shapes", &s.smooth_lines);
        }

        if (ImGui::CollapsingHeader("Playback",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Volume", &s.audio_volume, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Mute",          &s.audio_mute);
            ImGui::Checkbox("Loop playback", &s.loop_playback);
        }

        if (ImGui::CollapsingHeader("Performance / low-end mode",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Checkbox("Low-end mode (disable all UI antialiasing)",
                                &s.low_end_mode))
                atlas_dirty = true;
            ImGui::TextWrapped(
                "Disables font AA, sub-pixel rendering, oversampling and "
                "anti-aliased lines. Use on weak hardware or when battery "
                "life matters more than crisp text.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Reset to defaults", ImVec2(160, 0))) {
            s = Settings{};
            atlas_dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(theme().text_dim,
            "Path: %s", SettingsStore::path().c_str());
    }
    ImGui::End();

    // Persist on any change (cheap: tiny ini file).
    if (std::memcmp(&s, &before, sizeof(Settings)) != 0) {
        SettingsStore::save(s);
    }
    return atlas_dirty;
}

}  // namespace volchay::ui::panels
