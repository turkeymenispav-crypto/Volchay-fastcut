#include "ui/panels/viewer.h"

#include "media/audio_player.h"
#include "ui/main_layout.h"
#include "ui/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace volchay::ui::panels {

void draw_viewer(EditorContext& ctx) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewer", nullptr,
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    auto* player = ctx.player;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (!player || !player->is_open()) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme().bg_window);
        ImGui::BeginChild("##empty_viewer", avail, false);
        ImVec2 c = ImGui::GetCursorScreenPos();
        const char* msg1 = "No video loaded";
        const char* msg2 = "Drag a video file here, or use File > Open video";
        ImVec2 sz1 = ImGui::CalcTextSize(msg1);
        ImVec2 sz2 = ImGui::CalcTextSize(msg2);
        ImVec2 win = ImGui::GetContentRegionAvail();
        ImGui::SetCursorScreenPos(ImVec2(
            c.x + (win.x - sz1.x) * 0.5f,
            c.y + win.y * 0.5f - sz1.y));
        ImGui::TextColored(theme().text_dim, "%s", msg1);
        ImGui::SetCursorScreenPos(ImVec2(
            c.x + (win.x - sz2.x) * 0.5f,
            c.y + win.y * 0.5f + 4));
        ImGui::TextColored(theme().text_disabled, "%s", msg2);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

    const float vw = float(player->width());
    const float vh = float(player->height());

    // Background fill (so letterbox is visibly black).
    ImVec2 cur = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cur,
                      ImVec2(cur.x + avail.x, cur.y + avail.y),
                      IM_COL32(8, 8, 10, 255));

    if (vw <= 0 || vh <= 0) {
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);
        ImGui::TextColored(theme().text_dim, "(decoding...)");
        ImGui::End();
        return;
    }

    const float ar_video  = vw / vh;
    const float ar_avail  = avail.x / std::max(1.0f, avail.y);

    ImVec2 size = avail;
    if (ar_video > ar_avail) {
        size.y = avail.x / ar_video;
    } else {
        size.x = avail.y * ar_video;
    }
    ImVec2 pos = ImVec2(cur.x + (avail.x - size.x) * 0.5f,
                        cur.y + (avail.y - size.y) * 0.5f);

    auto* srv = player->current_srv();
    if (srv && player->current_pts() >= 0) {
        // ImGui 1.92 expects ImTextureID = ImU64. The DX11 backend casts
        // it back to ID3D11ShaderResourceView*, so we go through intptr_t
        // to avoid -Wpointer-to-int-cast warnings on 64-bit MinGW.
        ImTextureID tex = (ImTextureID)(intptr_t)srv;
        ImGui::SetCursorScreenPos(pos);
        ImGui::Image(tex, size);
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);
    } else {
        // Texture exists but no frame uploaded yet — show a hint so the
        // user knows decode is in flight rather than thinking the player
        // silently failed.
        ImGui::SetCursorScreenPos(cur);
        ImGui::Dummy(avail);
        ImVec2 c2 = ImVec2(cur.x + avail.x * 0.5f - 60,
                           cur.y + avail.y * 0.5f);
        dl->AddText(c2, IM_COL32(180, 180, 180, 220),
                    "decoding first frame...");
    }

    // Compact transport HUD overlayed in the top-left corner.
    if (ctx.project) {
        const double t = core::to_seconds(ctx.project->playhead());
        const double d = core::to_seconds(ctx.project->duration());
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "%02d:%02d:%02d.%03d / %02d:%02d:%02d.%03d   %s",
                      int(t / 3600), int(int(t) / 60) % 60, int(t) % 60,
                      int((t - int(t)) * 1000),
                      int(d / 3600), int(int(d) / 60) % 60, int(d) % 60,
                      int((d - int(d)) * 1000),
                      player->hardware_decode() ? "HW" : "SW");
        dl->AddRectFilled(ImVec2(cur.x + 8, cur.y + 8),
                          ImVec2(cur.x + 8 + ImGui::CalcTextSize(buf).x + 16,
                                 cur.y + 8 + ImGui::GetTextLineHeight() + 8),
                          IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(cur.x + 16, cur.y + 12),
                    IM_COL32(220, 220, 220, 255), buf);
    }

    ImGui::End();
}

}  // namespace volchay::ui::panels
