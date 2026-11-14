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

#include "ui/menu.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include "gfx/layout.h"
#include "gfx/vulkan/context.h"
#include "gfx/vulkan/renderer.h"
#include "hle/dsp.h"
#include "hle/hid.h"

namespace ui {

namespace {

SDL_Window* g_window = nullptr;
gfx::vulkan::Context* g_gpu = nullptr;
gfx::vulkan::Renderer* g_renderer = nullptr;
core::Config* g_cfg = nullptr;
std::filesystem::path g_file;
bool g_visible = false, g_ready = false;
int g_rebinding = -1;  // key binding waiting for a key press

float aspect(const std::string& w) {
    float a = 0, b = 0;
    if (w == "window") return -1;
    return std::sscanf(w.c_str(), "%f:%f", &a, &b) == 2 && a > 0 && b > 0 ? a / b : 0;
}

// Brings the running game in line with `c`; `prev` = what is already in effect (null at
// startup, when the renderer settings came from the environment and stay as they are).
void apply(const core::Config& c, const core::Config* prev) {
    const bool full = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
    if (full != c.fullscreen) SDL_SetWindowFullscreen(g_window, c.fullscreen);
    if (!prev || prev->vsync != c.vsync) g_gpu->set_vsync(c.vsync);
    gfx::g_single_screen = c.layout != "dual";
    hle::dsp::SetVolume(float(c.volume) / 100.0f);
    if (!prev) return;
    if (prev->resolution != c.resolution && !c.fullscreen) {
        int w = 0, h = 0;
        c.window_size(w, h);
        SDL_SetWindowSize(g_window, w, h);
    }
    if (g_renderer && prev->widescreen != c.widescreen) g_renderer->set_widescreen(aspect(c.widescreen));
    if (g_renderer && prev->fps != c.fps) g_renderer->set_interpolate(c.fps >= 60);
}

void save() {
    if (!core::save_config(g_file, *g_cfg)) std::fprintf(stderr, "config: cannot write %s\n", g_file.c_str());
}

void set_visible(bool v) {
    g_visible = v, g_rebinding = -1;
    hle::hid::set_host_input(!v);
    if (v) SDL_SetWindowRelativeMouseMode(g_window, false);
}

// One combo over fixed choices; true when the user picked another one.
bool choice(const char* label, std::string& value, std::initializer_list<std::pair<const char*, const char*>> items) {
    const char* shown = value.c_str();
    for (auto [v, text] : items)
        if (value == v) shown = text;
    bool changed = false;
    if (ImGui::BeginCombo(label, shown)) {
        for (auto [v, text] : items)
            if (ImGui::Selectable(text, value == v) && value != v) value = v, changed = true;
        ImGui::EndCombo();
    }
    return changed;
}

void draw_menu() {
    core::Config c = *g_cfg;
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("DawnOfTermina", &open, ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Video")) {
            choice("Resolution", c.resolution, {{"1080p", "1080p (1920x1080)"}, {"720p", "720p (1280x720)"}, {"native", "240p native (1x)"}});
            ImGui::TextDisabled("Window size changes now; the render scale on the next start.");
            choice("Widescreen", c.widescreen, {{"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"window", "Window aspect"}, {"off", "Off (5:3)"}});
            choice("Screens", c.layout, {{"single", "Single screen (bottom as overlay, Tab)"}, {"dual", "Dual screen (stacked)"}});
            ImGui::Checkbox("Fullscreen (F11)", &c.fullscreen);
            ImGui::Checkbox("VSync", &c.vsync);
            int fps = c.fps >= 60 ? 1 : 0;
            if (ImGui::Combo("Frame rate", &fps, "30 FPS (original)\0" "60 FPS (interpolated)\0")) c.fps = fps ? 60 : 30;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::SliderInt("Volume", &c.volume, 0, 100, "%d%%");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            ImGui::SliderFloat("Mouse camera sensitivity", &c.mouse_sensitivity, 0.1f, 5.0f, "%.2f");
            ImGui::TextDisabled("F2 captures the mouse for the camera (C-stick); Esc releases it.");
            ImGui::TextDisabled("Gamepad: Nintendo layout, right stick = camera, L3/R3 = items I/II,");
            ImGui::TextDisabled("touchpad = map, misc/share = Ocarina.");
            if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                auto keys = hle::hid::key_bindings();
                for (int i = 0; i < int(keys.size()); ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(keys[i].label);
                    ImGui::TableNextColumn();
                    const char* name = SDL_GetScancodeName(SDL_Scancode(keys[i].key));
                    const std::string text = g_rebinding == i ? "press a key..." : (*name ? name : "(none)");
                    if (ImGui::Button((text + "##" + keys[i].action).c_str(), ImVec2(160, 0))) g_rebinding = i;
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    ImGui::TextDisabled("F1/Esc: close   Tab: bottom screen   F11: fullscreen   F2: mouse camera");
    ImGui::End();
    if (!open) set_visible(false);
    if (c.resolution != g_cfg->resolution || c.widescreen != g_cfg->widescreen || c.layout != g_cfg->layout ||
        c.fullscreen != g_cfg->fullscreen || c.vsync != g_cfg->vsync || c.fps != g_cfg->fps ||
        c.volume != g_cfg->volume || c.mouse_sensitivity != g_cfg->mouse_sensitivity) {
        apply(c, g_cfg);
        *g_cfg = c;
        save();
    }
}

}  // namespace

bool init(SDL_Window* window, gfx::vulkan::Context& gpu, core::Config& config, const std::filesystem::path& file) {
    g_window = window, g_gpu = &gpu, g_cfg = &config, g_file = file;
    // Keys: config.toml names override the defaults; the file then lists every binding.
    for (auto& k : hle::hid::key_bindings()) {
        if (auto it = config.keys.find(k.action); it != config.keys.end())
            if (const SDL_Scancode sc = SDL_GetScancodeFromName(it->second.c_str()); sc != SDL_SCANCODE_UNKNOWN) k.key = sc;
        config.keys[k.action] = SDL_GetScancodeName(SDL_Scancode(k.key));
    }
    apply(config, nullptr);
    save();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the binary
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForVulkan(window)) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_1;
    ii.Instance = gpu.instance();
    ii.PhysicalDevice = gpu.gpu();
    ii.Device = gpu.device();
    ii.QueueFamily = gpu.queue_family();
    ii.Queue = gpu.queue();
    ii.RenderPass = gpu.render_pass();
    ii.MinImageCount = 2;
    ii.ImageCount = std::max(2u, gpu.image_count());
    ii.DescriptorPoolSize = 8;
    if (!gpu.render_pass() || !ImGui_ImplVulkan_Init(&ii)) return false;
    {  // uploads through the queue the renderer also submits to
        std::lock_guard lk(gpu.queue_lock());
        ImGui_ImplVulkan_CreateFontsTexture();
    }
    g_ready = true;
    if (std::getenv("DOT_MENU")) set_visible(true);  // opens at start (validating the overlay pass)
    return true;
}

void set_renderer(gfx::vulkan::Renderer* renderer) { g_renderer = renderer; }

void shutdown() {
    if (!g_ready) return;
    g_gpu->set_overlay(nullptr);
    vkDeviceWaitIdle(g_gpu->device());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

bool visible() { return g_visible; }

bool handle_event(const SDL_Event& e) {
    if (g_ready && g_visible) ImGui_ImplSDL3_ProcessEvent(&e);
    if (e.type == SDL_EVENT_MOUSE_MOTION && SDL_GetWindowRelativeMouseMode(g_window))
        hle::hid::add_mouse_motion(e.motion.xrel, e.motion.yrel, g_cfg->mouse_sensitivity);
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) return !g_visible;
    if (g_visible && g_rebinding >= 0) {  // rebinding: this key (Esc cancels)
        if (e.key.scancode != SDL_SCANCODE_ESCAPE) {
            auto& k = hle::hid::key_bindings()[g_rebinding];
            k.key = e.key.scancode;
            g_cfg->keys[k.action] = SDL_GetScancodeName(e.key.scancode);
            save();
        }
        g_rebinding = -1;
        return false;
    }
    switch (e.key.key) {
    case SDLK_F1:
        set_visible(!g_visible);
        return false;
    case SDLK_ESCAPE:
        if (SDL_GetWindowRelativeMouseMode(g_window)) SDL_SetWindowRelativeMouseMode(g_window, false);
        else set_visible(!g_visible);
        return false;
    case SDLK_F2:
        if (!g_visible) SDL_SetWindowRelativeMouseMode(g_window, !SDL_GetWindowRelativeMouseMode(g_window));
        return false;
    case SDLK_TAB:
        gfx::g_show_bottom = !gfx::g_show_bottom;
        return false;
    case SDLK_F11:
        g_cfg->fullscreen = !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
        SDL_SetWindowFullscreen(g_window, g_cfg->fullscreen);
        save();
        return false;
    default:
        return !g_visible;
    }
}

void frame() {
    if (!g_ready) return;
    if (!g_visible) {
        g_gpu->set_overlay(nullptr);
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_menu();
    ImGui::Render();
    g_gpu->set_overlay([](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
}

}  // namespace ui
