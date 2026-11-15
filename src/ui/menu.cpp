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

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gfx/layout.h"
#include "gfx/vulkan/context.h"
#include "gfx/vulkan/renderer.h"
#include "hle/dsp.h"
#include "hle/hid.h"
#include "hle/kernel.h"

namespace ui {

namespace {

SDL_Window* g_window = nullptr;
gfx::vulkan::Context* g_gpu = nullptr;
gfx::vulkan::Renderer* g_renderer = nullptr;
core::Memory* g_mem = nullptr;
core::Config* g_cfg = nullptr;
std::filesystem::path g_file;
bool g_visible = false, g_ready = false;
int g_rebinding = -1;  // key binding waiting for a key press
VkDescriptorSet g_bottom_tex = VK_NULL_HANDLE;  // bottom screen for the N64 HUD

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
    gfx::g_n64 = c.layout == "n64";
    gfx::g_hud_scale = c.hud_scale, gfx::g_hud_opacity = c.hud_opacity, gfx::g_minimap = c.minimap;
    hle::dsp::SetVolume(float(c.volume) / 100.0f);
    if (!prev) return;
    if (prev->resolution != c.resolution) {
        int w = 0, h = 0;
        c.window_size(w, h);
        if (!c.fullscreen) SDL_SetWindowSize(g_window, w, h);  // the swapchain follows the resize
        if (g_renderer && g_mem) g_renderer->set_scale(*g_mem, c.render_scale());
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
            choice("Widescreen", c.widescreen, {{"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"window", "Window aspect"}, {"off", "Off (5:3)"}});
            choice("Screens", c.layout, {{"n64", "N64 unified (HUD in the corners, menus pop up)"},
                                         {"single", "Single screen (bottom as overlay, Tab)"},
                                         {"dual", "Dual screen (stacked)"}});
            if (c.layout == "n64") {
                ImGui::SliderFloat("HUD scale", &c.hud_scale, 0.3f, 1.5f, "%.2f");
                ImGui::SliderFloat("HUD opacity", &c.hud_opacity, 0.1f, 1.0f, "%.2f");
                ImGui::Checkbox("Minimap (Tab)", &c.minimap);
                ImGui::TextDisabled("F3 shows the whole bottom screen (Ocarina, bank, codes...).");
            }
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
    ImGui::TextDisabled("F1/Esc: close   Tab: bottom screen / minimap   F3: bottom screen pop-up");
    ImGui::TextDisabled("F11: fullscreen   F2: mouse camera   F5/F9: save/load state");
    ImGui::End();
    if (!open) set_visible(false);
    if (c.resolution != g_cfg->resolution || c.widescreen != g_cfg->widescreen || c.layout != g_cfg->layout ||
        c.fullscreen != g_cfg->fullscreen || c.vsync != g_cfg->vsync || c.fps != g_cfg->fps ||
        c.volume != g_cfg->volume || c.mouse_sensitivity != g_cfg->mouse_sensitivity || c.hud_scale != g_cfg->hud_scale ||
        c.hud_opacity != g_cfg->hud_opacity || c.minimap != g_cfg->minimap) {
        apply(c, g_cfg);
        *g_cfg = c;
        save();
    }
}

// N64 layout: the bottom-screen pieces in the corners of the top screen, or the whole bottom
// screen over a dimmed top screen while the game shows a menu there. Same rects as
// gfx::screen_layout / hid::window_to_touch, in ImGui's (window) coordinates.
void draw_hud() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    gfx::Rect top, bottom;
    gfx::screen_layout(size.x, size.y, top, bottom);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImTextureID tex = ImTextureID(g_bottom_tex);
    if (gfx::bottom_modal()) {
        dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 150));
        dl->AddImage(tex, ImVec2(bottom.x, bottom.y), ImVec2(bottom.x + bottom.w, bottom.y + bottom.h));
        return;
    }
    const ImU32 tint = IM_COL32(255, 255, 255, int(gfx::g_hud_opacity * 255));
    for (const auto& p : gfx::HUD_PIECES) {
        const gfx::Rect r = gfx::hud_rect(top, p);
        if (r.w <= 0) continue;
        dl->AddImage(tex, ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h), ImVec2(p.src.x / 320, p.src.y / 240),
                     ImVec2((p.src.x + p.src.w) / 320, (p.src.y + p.src.h) / 240), tint);
    }
}

bool imgui_init(SDL_Window* window, gfx::vulkan::Context& gpu) {
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
    // The N64 HUD's view of the bottom screen; present() repoints it at each frame's image.
    g_bottom_tex = ImGui_ImplVulkan_AddTexture(gpu.sampler(), gpu.bottom_view(), VK_IMAGE_LAYOUT_GENERAL);
    gpu.set_bottom_texture(g_bottom_tex);
    return true;
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

    if (!imgui_init(window, gpu)) return false;
    g_ready = true;
    if (std::getenv("DOT_MENU")) set_visible(true);  // opens at start (validating the overlay pass)
    return true;
}

namespace {

// Extraction in a child process (python3 tools/extract_cia.py), its output collected for the screen.
struct Extraction {
    std::mutex lock;
    std::string log;
    std::atomic<bool> done{false};
    bool ok = false;

    void run(std::string script, std::string cia, std::string out) {
        int fds[2];
        if (pipe(fds) != 0) return finish(false, "cannot create a pipe\n");
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        char* argv[] = {const_cast<char*>("python3"), const_cast<char*>("-I"), script.data(), cia.data(), out.data(), nullptr};
        pid_t pid = 0;
        const int err = posix_spawnp(&pid, "python3", &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(fds[1]);
        if (err) {
            close(fds[0]);
            return finish(false, "python3 not found: install Python 3 to extract the game.\n");
        }
        char buf[256];
        for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) {
            std::lock_guard lk(lock);
            log.append(buf, size_t(n));
        }
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        finish(WIFEXITED(status) && WEXITSTATUS(status) == 0, "");
    }

    void finish(bool success, const char* msg) {
        {
            std::lock_guard lk(lock);
            log += msg;
        }
        ok = success, done = true;
    }
};

}  // namespace

bool run_setup(const std::filesystem::path& target, const std::filesystem::path& script,
               const std::filesystem::path& suggested) {
    SDL_Window* window = SDL_CreateWindow("DawnOfTermina — setup", 900, 560, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (!window) return false;
    bool result = false;
    {
        gfx::vulkan::Context gpu;
        if (!gpu.init(window) || !imgui_init(window, gpu)) {
            SDL_DestroyWindow(window);
            return false;
        }
        const std::vector<u32> top(gfx::vulkan::TOP_W * gfx::vulkan::SCREEN_H, 0xFF000000);
        const std::vector<u32> bottom(gfx::vulkan::BOTTOM_W * gfx::vulkan::SCREEN_H, 0xFF000000);
        std::mutex picked_lock;
        std::filesystem::path picked;  // set by the file dialog callback
        auto ex = std::make_unique<Extraction>();
        std::thread worker;
        bool running = false, finished = false;
        auto start = [&](const std::filesystem::path& cia) {
            if (running) return;
            ex = std::make_unique<Extraction>();
            running = true;
            worker = std::thread([e = ex.get(), script, cia, target] { e->run(script.string(), cia.string(), target.string()); });
        };
        if (const char* cia = std::getenv("DOT_SETUP_CIA")) start(cia);  // unattended (testing)
        for (bool open = true; open && !finished;) {
            for (SDL_Event e; SDL_PollEvent(&e);) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) open = running;  // not mid-extraction
            }
            {
                std::lock_guard lk(picked_lock);
                if (!picked.empty()) start(picked), picked.clear();
            }
            if (running && ex->done) {
                worker.join();
                running = false;
                finished = ex->ok;
            }
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
            ImGui::TextUnformatted("DawnOfTermina");
            ImGui::Separator();
            ImGui::TextWrapped("The Majora's Mask 3D game data was not found. Select your own copy of the game "
                               "as a decrypted .cia file; it will be extracted to:");
            ImGui::TextDisabled("%s", target.string().c_str());
            ImGui::TextWrapped("This is done once (about 650 MB, a minute or two). Nothing is downloaded.");
            ImGui::Spacing();
            ImGui::BeginDisabled(running);
            if (!suggested.empty() && ImGui::Button(("Use " + suggested.filename().string()).c_str())) start(suggested);
            if (ImGui::Button("Select .cia file...")) {
                static const SDL_DialogFileFilter filter{"Nintendo 3DS CIA (*.cia)", "cia"};
                SDL_ShowOpenFileDialog(
                    [](void* user, const char* const* files, int) {
                        auto* ctx = static_cast<std::pair<std::mutex*, std::filesystem::path*>*>(user);
                        if (files && files[0]) {
                            std::lock_guard lk(*ctx->first);
                            *ctx->second = files[0];
                        }
                    },
                    new std::pair<std::mutex*, std::filesystem::path*>(&picked_lock, &picked),  // shortcut: leaked once per click
                    window, &filter, 1, nullptr, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Quit")) open = false;
            ImGui::EndDisabled();
            if (running) ImGui::TextUnformatted("Extracting...");
            else if (ex->done && !ex->ok) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Extraction failed: see the log below.");
            {
                std::lock_guard lk(ex->lock);
                ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(ex->log.c_str());
                if (running) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
            }
            ImGui::End();
            ImGui::Render();
            gpu.set_overlay([](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
            gfx::g_single_screen = true, gfx::g_show_bottom = false;
            if (!gpu.present({gfx::vulkan::TOP_W, gfx::vulkan::SCREEN_H, top.data()},
                             {gfx::vulkan::BOTTOM_W, gfx::vulkan::SCREEN_H, bottom.data()}))
                break;
        }
        if (worker.joinable()) worker.join();
        result = finished;
        gpu.set_overlay(nullptr);
        gpu.set_bottom_texture(VK_NULL_HANDLE);  // freed with ImGui's descriptor pool
        vkDeviceWaitIdle(gpu.device());
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    gfx::g_show_bottom = true;
    SDL_DestroyWindow(window);
    return result;
}

void set_renderer(gfx::vulkan::Renderer* renderer, core::Memory* mem) { g_renderer = renderer, g_mem = mem; }

void shutdown() {
    if (!g_ready) return;
    g_gpu->set_overlay(nullptr);
    g_gpu->set_bottom_texture(VK_NULL_HANDLE);
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
        if (gfx::g_n64) {
            gfx::g_minimap = g_cfg->minimap = !g_cfg->minimap;
            save();
        } else {
            gfx::g_show_bottom = !gfx::g_show_bottom;
        }
        return false;
    case SDLK_F3:
        gfx::g_force_modal = !gfx::g_force_modal;
        return false;
    case SDLK_F5:  // savestates live next to config.toml (and game_data/, saves/)
        hle::kernel::request_save(g_file.parent_path() / "states" / "slot1.state");
        return false;
    case SDLK_F9:
        hle::kernel::request_load(g_file.parent_path() / "states" / "slot1.state");
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
    static std::string state_status;  // savestate result in the window title
    if (std::string st = hle::kernel::savestate_status(); st != state_status) {
        state_status = st;
        SDL_SetWindowTitle(g_window, ("DawnOfTermina - " + st).c_str());
    }
    if (!g_visible && !gfx::g_n64) {
        g_gpu->set_overlay(nullptr);
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (gfx::g_n64) draw_hud();
    if (g_visible) draw_menu();
    ImGui::Render();
    g_gpu->set_overlay([](VkCommandBuffer cmd) { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd); });
}

}  // namespace ui
