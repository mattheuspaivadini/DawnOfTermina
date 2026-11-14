// DawnOfTermina — native static recompilation of Majora's Mask 3D.
// Copyright (C) 2026 DawnOfTermina contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <filesystem>
#include <map>
#include <string>

// User settings persisted in config.toml (next to game_data/, like cache/ and saves/).
// The file uses a flat TOML subset: [section] headers and `key = value` lines with quoted
// strings, booleans and numbers. DOT_* environment variables still win (testing overrides).
namespace core {

struct Config {
    // [display]
    std::string resolution = "1080p";  // 1080p | 720p | native (240p: 1x render scale)
    std::string widescreen = "16:9";   // off | 16:9 | 16:10 | 21:9 | window
    std::string layout = "single";     // single (top screen fills the window) | dual
    bool fullscreen = false;
    bool vsync = true;
    int fps = 30;                      // 30 (native) or 60 (frame interpolation; needs GPU headroom)
    // [audio]
    int volume = 100;                  // percent
    // [input]
    float mouse_sensitivity = 1.0f;    // mouse camera (C-stick) gain
    std::map<std::string, std::string> keys;  // [keys] action -> SDL scancode name

    // Window size and DOT_RES_SCALE for `resolution`.
    void window_size(int& w, int& h) const;
    unsigned render_scale() const;  // internal resolution scale for the preset: 1080p 4x, 720p 3x, native 1x
};

// Missing file or keys keep the defaults. Unknown keys are ignored.
Config parse_config(const std::string& text);
std::string format_config(const Config& c);
Config load_config(const std::filesystem::path& file);  // writes the defaults if missing
bool save_config(const std::filesystem::path& file, const Config& c);

// Exports the settings the renderer reads at startup (DOT_WINDOW, DOT_RES_SCALE,
// DOT_WIDESCREEN, DOT_FPS) without overriding variables already set.
void apply_env_defaults(const Config& c);

}  // namespace core
