#include "ui/fonts.h"

#include "platform/windows.h"
#include "util/log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <cstring>
#include <vector>

namespace volchay::ui {
namespace {

struct FontCandidate {
    const wchar_t* file;
    const char*    label;
};

const FontCandidate kCandidates[][3] = {
    // Choice 0: Segoe UI Variable (Windows 11)
    {{L"SegoeUIVF.ttf",      "Segoe UI Variable"},
     {L"segoeui.ttf",        "Segoe UI"},
     {nullptr,               nullptr}},
    // Choice 1: Segoe UI
    {{L"segoeui.ttf",        "Segoe UI"},
     {L"SegoeUIVF.ttf",      "Segoe UI Variable"},
     {nullptr,               nullptr}},
    // Choice 2: Tahoma
    {{L"tahoma.ttf",         "Tahoma"},
     {L"segoeui.ttf",        "Segoe UI"},
     {nullptr,               nullptr}},
};

std::wstring fonts_dir() {
    wchar_t buf[MAX_PATH];
    UINT n = ::GetWindowsDirectoryW(buf, MAX_PATH);
    if (n == 0) return L"C:\\Windows";
    return std::wstring(buf, n) + L"\\Fonts";
}

bool load_file(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        ::CloseHandle(h); return false;
    }
    out.resize(size_t(sz.QuadPart));
    DWORD read = 0;
    BOOL ok = ::ReadFile(h, out.data(), DWORD(out.size()), &read, nullptr);
    ::CloseHandle(h);
    return ok && read == out.size();
}

}  // namespace

void invalidate_font_texture() {
    // imgui_impl_dx11 in 1.92 manages textures via the new
    // ImGuiBackendFlags_RendererHasTextures protocol — bumping the atlas
    // version through a rebuild is enough; the backend rebuilds the
    // texture on the next render.
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        // Touch the atlas so the backend re-uploads.
        io.Fonts->TexIsBuilt = false;
    }
}

bool rebuild_fonts(const Settings& s_in, float dpi_scale) {
    Settings s = effective(s_in);

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    if (dpi_scale <= 0.0f) dpi_scale = 1.0f;
    const float size = s.font_size_px * dpi_scale;

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;            // we hand the bytes to ImGui
    cfg.OversampleH          = s.font_oversample_h ? 2 : 1;
    cfg.OversampleV          = 1;
    cfg.PixelSnapH           = !s.font_anti_alias;
    cfg.RasterizerMultiply   = s.font_subpixel ? 1.10f : 1.0f;

    int choice = s.font_choice;
    if (choice < 0 || choice >= int(IM_ARRAYSIZE(kCandidates))) choice = 0;

    bool loaded = false;
    auto base = fonts_dir();
    for (auto& c : kCandidates[choice]) {
        if (!c.file) break;
        std::wstring full = base + L"\\" + c.file;
        std::vector<unsigned char> data;
        if (!load_file(full, data)) continue;

        // ImGui takes ownership of `data` only if FontDataOwnedByAtlas is
        // true; we still need to pass an allocation it can free with
        // ImGui::MemFree, so allocate via the same allocator.
        void* mem = IM_ALLOC(data.size());
        std::memcpy(mem, data.data(), data.size());

        // ImGui 1.92's dynamic font atlas resolves glyphs on demand, so we
        // don't pre-populate Cyrillic ranges here — it would just waste
        // texture space and slow the first build.
        ImFont* f = io.Fonts->AddFontFromMemoryTTF(
            mem, int(data.size()), size, &cfg, nullptr);
        if (f) {
            log::info("Font loaded: %s @ %.1f px", c.label, size);
            loaded = true;
            break;
        }
        IM_FREE(mem);
    }

    if (!loaded) {
        log::warn("No system font found, using ProggyClean fallback");
        io.Fonts->AddFontDefault();
    }

    // Antialiased lines/fills track the same setting.
    ImGuiStyle& style = ImGui::GetStyle();
    style.AntiAliasedLines       = s.smooth_lines;
    style.AntiAliasedLinesUseTex = s.smooth_lines;
    style.AntiAliasedFill        = s.smooth_lines;

    invalidate_font_texture();
    return loaded;
}

}  // namespace volchay::ui
