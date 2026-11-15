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

#include "core/config.h"

#include "gfx/layout.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace core {

void Config::window_size(int& w, int& h) const {
    if (resolution == "720p") w = 1280, h = 720;
    else if (resolution == "native") w = 800, h = 480;
    else w = 1920, h = 1080;
}

// From the preset, not the real window: in fullscreen that is the monitor, and a 1080p
// monitor turned "720p" into a 4x (1080p) internal resolution.
unsigned Config::render_scale() const {
    if (resolution == "native") return 1;
    int w = 0, h = 0;
    window_size(w, h);
    return gfx::native_scale(unsigned(w), unsigned(h), widescreen != "off");
}

static std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

Config parse_config(const std::string& text) {
    Config c;
    std::istringstream in(text);
    std::string section, line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!v.empty() && v[0] == '"') v = v.substr(1, v.find('"', 1) - 1);  // quoted: no escapes needed
        else v = trim(v.substr(0, v.find('#')));
        const std::string k = section + "." + key;
        if (k == "display.resolution") c.resolution = v;
        else if (k == "display.widescreen") c.widescreen = v;
        else if (k == "display.layout") c.layout = v;
        else if (k == "display.fullscreen") c.fullscreen = v == "true";
        else if (k == "display.vsync") c.vsync = v == "true";
        else if (k == "display.fps") c.fps = std::clamp(std::atoi(v.c_str()) / 30, 1, 4) * 30;
        else if (k == "hud.scale") c.hud_scale = std::clamp(std::strtof(v.c_str(), nullptr), 0.3f, 1.5f);
        else if (k == "hud.opacity") c.hud_opacity = std::clamp(std::strtof(v.c_str(), nullptr), 0.1f, 1.0f);
        else if (k == "hud.minimap") c.minimap = v == "true";
        else if (k == "performance.mode") c.perf = v == "fps" || v == "full" ? v : "off";
        else if (k == "performance.opacity") c.perf_opacity = std::clamp(std::strtof(v.c_str(), nullptr), 0.1f, 1.0f);
        else if (k == "camera.fov") c.fov = std::clamp(std::strtof(v.c_str(), nullptr), 45.0f, 70.0f);
        else if (k == "camera.cutscene_fov") c.cutscene_fov = std::clamp(std::strtof(v.c_str(), nullptr), 45.0f, 70.0f);
        else if (k == "audio.volume") c.volume = std::clamp(std::atoi(v.c_str()), 0, 100);
        else if (k == "input.mouse_sensitivity") c.mouse_sensitivity = std::strtof(v.c_str(), nullptr);
        else if (section == "keys") c.keys[key] = v;
    }
    return c;
}

std::string format_config(const Config& c) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    std::ostringstream o;
    o << "# DawnOfTermina settings (also editable in game: F1). DOT_* environment variables override.\n"
      << "[display]\n"
      << "resolution = \"" << c.resolution << "\"  # 1080p | 720p | native\n"
      << "widescreen = \"" << c.widescreen << "\"  # off | 16:9 | 16:10 | 21:9 | window\n"
      << "layout = \"" << c.layout << "\"  # n64 | single | dual\n"
      << "fullscreen = " << b(c.fullscreen) << "\n"
      << "vsync = " << b(c.vsync) << "\n"
      << "fps = " << c.fps << "  # 30 | 60 | 90 | 120 (interpolated)\n\n"
      << "[hud]  # n64 layout\nscale = " << c.hud_scale << "\nopacity = " << c.hud_opacity
      << "\nminimap = " << b(c.minimap) << "\n\n"
      << "[performance]\nmode = \"" << c.perf << "\"  # off | fps | full\nopacity = " << c.perf_opacity << "\n\n"
      << "[camera]  # vertical FOV in degrees: 45 (original) to 70\nfov = " << c.fov
      << "\ncutscene_fov = " << c.cutscene_fov << "\n\n"
      << "[audio]\nvolume = " << c.volume << "\n\n"
      << "[input]\nmouse_sensitivity = " << c.mouse_sensitivity << "\n\n[keys]\n";
    for (const auto& [k, v] : c.keys) o << k << " = \"" << v << "\"\n";
    return o.str();
}

Config load_config(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) {
        Config c;
        save_config(file, c);
        return c;
    }
    std::ostringstream text;
    text << f.rdbuf();
    return parse_config(text.str());
}

bool save_config(const std::filesystem::path& file, const Config& c) {
    std::ofstream f(file);
    f << format_config(c);
    return bool(f);
}

void apply_env_defaults(const Config& c) {
    int w = 0, h = 0;
    c.window_size(w, h);
    const std::string win = std::to_string(w) + "x" + std::to_string(h);
    setenv("DOT_WINDOW", win.c_str(), 0);
    setenv("DOT_RES_SCALE", std::to_string(c.render_scale()).c_str(), 0);
    if (c.widescreen != "off") setenv("DOT_WIDESCREEN", c.widescreen.c_str(), 0);
    setenv("DOT_FPS", std::to_string(c.fps).c_str(), 0);
}

}  // namespace core
