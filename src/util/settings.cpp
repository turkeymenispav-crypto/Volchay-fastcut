#include "util/settings.h"

#include "platform/windows.h"
#include "util/log.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace volchay {
namespace {

std::string resolve_dir() {
    PWSTR roaming = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0,
                                      nullptr, &roaming)) || !roaming) {
        return {};
    }
    std::wstring dir = roaming;
    ::CoTaskMemFree(roaming);
    dir += L"\\Volchay";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return narrow(dir);
}

}  // namespace

std::string SettingsStore::path() {
    auto d = resolve_dir();
    if (d.empty()) return {};
    return d + "\\settings.ini";
}

Settings SettingsStore::load() {
    Settings s;
    auto p = path();
    if (p.empty()) return s;

    std::ifstream in(p);
    if (!in) return s;

    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim trailing CR (Windows line endings).
        while (!val.empty() && (val.back() == '\r' || val.back() == ' '))
            val.pop_back();
        while (!key.empty() && key.back() == ' ') key.pop_back();

        auto as_bool = [&]() { return val == "1" || val == "true"; };
        auto as_float = [&]() { return float(std::strtod(val.c_str(), nullptr)); };
        auto as_int   = [&]() { return int(std::strtol(val.c_str(), nullptr, 10)); };

        if      (key == "font_size_px")      s.font_size_px      = as_float();
        else if (key == "font_anti_alias")   s.font_anti_alias   = as_bool();
        else if (key == "font_subpixel")     s.font_subpixel     = as_bool();
        else if (key == "font_oversample_h") s.font_oversample_h = as_bool();
        else if (key == "font_choice")       s.font_choice       = as_int();
        else if (key == "vsync")             s.vsync             = as_bool();
        else if (key == "hardware_decode")   s.hardware_decode   = as_bool();
        else if (key == "smooth_lines")      s.smooth_lines      = as_bool();
        else if (key == "audio_volume")      s.audio_volume      = as_float();
        else if (key == "audio_mute")        s.audio_mute        = as_bool();
        else if (key == "loop_playback")     s.loop_playback     = as_bool();
        else if (key == "low_end_mode")      s.low_end_mode      = as_bool();
    }
    return s;
}

bool SettingsStore::save(const Settings& s) {
    auto p = path();
    if (p.empty()) return false;

    std::ostringstream out;
    out << "# Volchay-fastcut user settings. Edited by Settings panel.\n";
    out << "font_size_px="      << s.font_size_px      << "\n";
    out << "font_anti_alias="   << (s.font_anti_alias   ? 1 : 0) << "\n";
    out << "font_subpixel="     << (s.font_subpixel     ? 1 : 0) << "\n";
    out << "font_oversample_h=" << (s.font_oversample_h ? 1 : 0) << "\n";
    out << "font_choice="       << s.font_choice       << "\n";
    out << "vsync="             << (s.vsync             ? 1 : 0) << "\n";
    out << "hardware_decode="   << (s.hardware_decode   ? 1 : 0) << "\n";
    out << "smooth_lines="      << (s.smooth_lines      ? 1 : 0) << "\n";
    out << "audio_volume="      << s.audio_volume      << "\n";
    out << "audio_mute="        << (s.audio_mute        ? 1 : 0) << "\n";
    out << "loop_playback="     << (s.loop_playback     ? 1 : 0) << "\n";
    out << "low_end_mode="      << (s.low_end_mode      ? 1 : 0) << "\n";

    std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            log::err("Settings: open '%s' for write failed", tmp.c_str());
            return false;
        }
        f << out.str();
    }
    // Replace atomically. ::MoveFileExW with MOVEFILE_REPLACE_EXISTING handles
    // both the "no destination" and "destination exists" cases.
    auto wp   = widen(p);
    auto wtmp = widen(tmp);
    if (!::MoveFileExW(wtmp.c_str(), wp.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log::err("Settings: MoveFileExW failed: %lu", ::GetLastError());
        return false;
    }
    return true;
}

Settings effective(const Settings& s) {
    if (!s.low_end_mode) return s;
    Settings out = s;
    // Cheapest possible UI: disable every per-frame antialiasing knob.
    out.font_anti_alias   = false;
    out.font_subpixel     = false;
    out.font_oversample_h = false;
    out.smooth_lines      = false;
    return out;
}

}  // namespace volchay
