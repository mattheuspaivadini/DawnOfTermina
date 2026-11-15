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

// Glue between the recompiled game and DekUI's single-screen modules (src/dekui/mm3d/, copied
// verbatim from https://github.com/tgacquin/mm3d-dekui, GPL-2.0-or-later). DekUI swaps game
// instructions for SVCs in Azahar's dynarmic (src/core/arm/dynarmic/arm_dynarmic.cpp: the
// MemoryReadCode chain picks the instructions, CallSVC runs the handlers). Here each swapped
// instruction is an instruction hook (tools/recomp/hooks.txt, Restoration + HD HUD section) and
// each hook below does what DekUI's handler for that SVC does, in the same order, including the
// displaced instruction. tools/dekui_coverage.py lists which of DekUI's instructions are hooked.
//
// Handlers redirect the guest by writing r15 (and r14 when they call a function): redirect()
// runs one with r15 = the next instruction and returns 0 (fall through) or the new pc, which the
// generated code turns into a local jump, a call or a jump out (see recomp.py).
//
// Several modules draw extra panes by building layout trees on the guest stack below sp: their
// Root() returns the tree to draw in place of r4 + 8, sp moves below it while the layout draws,
// and End() gives sp back (DekUI's shared layout SVCs, FirstSVC + 1..3 of NativeInventory).
//
// Not ported: DekUI's gameplay extras (classic saving, difficulty, prices, draw distance, owls...),
// its 60 FPS interpolation publishers, and the HD pause/photo capture backends (PauseCapture and
// PhotoCapture run without a backend: the native 240p capture stays, see pause_capture_sync).

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "hle/svc.h"
#include "mm3d/banners.h"
#include "mm3d/dialogue_scale.h"
#include "mm3d/double_time.h"
#include "mm3d/dungeon_map.h"
#include "mm3d/file_select_input.h"
#include "mm3d/file_select_prompt.h"
#include "mm3d/hud_buttons.h"
#include "mm3d/hud_item_background.h"
#include "mm3d/inventory_navigation.h"
#include "mm3d/inventory_shadow.h"
#include "mm3d/item_preview.h"
#include "mm3d/items_map_prompt.h"
#include "mm3d/magic_border.h"
#include "mm3d/menu_backdrop.h"
#include "mm3d/menu_help.h"
#include "mm3d/menu_memory.h"
#include "mm3d/menu_page_switch.h"
#include "mm3d/menu_sound.h"
#include "mm3d/menu_tabs.h"
#include "mm3d/minigame_hud.h"
#include "mm3d/minimap.h"
#include "mm3d/minimap_shortcut.h"
#include "mm3d/native_file_select.h"
#include "mm3d/native_inventory.h"
#include "mm3d/native_item_present.h"
#include "mm3d/native_minimap.h"
#include "mm3d/native_notebook.h"
#include "mm3d/native_startup.h"
#include "mm3d/ocarina_input.h"
#include "mm3d/ocarina_prompt.h"
#include "mm3d/options_menu.h"
#include "mm3d/pause_capture.h"
#include "mm3d/photo_capture.h"
#include "mm3d/player_hud.h"
#include "mm3d/single_screen_text.h"
#include "mm3d/song_list.h"
#include "mm3d/song_reset.h"
#include "mm3d/stick_controls.h"
#include "mm3d/world_map_prompt.h"
#include "recomp/hooks.h"

namespace recomp::hooks {
namespace {

using namespace MM3D;

struct Guest {  // DekUI's memory interface: reads of unmapped memory fail instead of faulting
    std::optional<u32> Read(u32 at) { return readable(at, 4) ? std::optional<u32>(r32(at)) : std::nullopt; }
    void Write(u32 at, u32 value) { w32(at, value); }
};

constexpr bool kWide = false;  // our widescreen keeps 2D layouts centred at 5:3 (DekUI's Widescreen::Active())

// DekUI's DynarmicUserCallbacks members, same names.
struct Hooks {
    NativeMinimap::Hook minimap_hook;
    DungeonMap::Hook dungeon_map_hook;
    InventoryShadow::Hook inventory_shadow_hook;
    ItemsMapPrompt::Hook items_map_prompt_hook, map_items_prompt_hook;
    MenuHelp::Hook menu_help_hook, select_help_hook;
    WorldMapPrompt::Hook world_map_prompt_hook;
    MenuTabs::Hook menu_tabs_hook;
    NativeInventory::Hook inventory_hook;
    OptionsMenu::Hook options_hook;
    MenuPageSwitch::Hook menu_page_switch;
    MenuMemory::Hook menu_memory;
    MenuBackdrop::Hook menu_backdrop;
    u32 menu_sound_context = 0;
    PauseCapture::Hook pause_capture_hook;
    PhotoCapture::Hook photo_capture_hook;
    NativeNotebook::Hook notebook_hook;
    NativeFileSelect::Hook file_select_hook;
    FileSelectPrompt::Hook file_select_prompt_hook;
    OcarinaPrompt::Hook ocarina_hook;
    MagicBorder::Hook magic_hook;
    MinigameHud::Hook minigame_hook;
    PlayerHud::Hook player_hud_hook;
    SongList::Hook song_hook;
    DoubleTime::Hook double_time_hook;
    SongReset::Hook song_reset_hook;
    HudButtons::Hook hud_buttons_hook;
    HudItemBackground::Hook hud_item_background_hook;
    Banners::Hook banner_hook;
    NativeItemPresent::Hook item_present_hook;
    NativeStartup::Hook startup_hook;
    NativeStartup::PressHook press_hook;
    NativeStartup::LoadingIconHook loading_icon_hook;
    ItemPreview::Hook item_preview_hook;
    StickControls::Hook stick_controls_hook;
    DialogueScale::Hook dialogue_hook;
};
Hooks* g_hooks = new Hooks;  // never freed: hooks may run while the process exits
Hooks& H = *g_hooks;

// The code DekUI was verified against, checked once (the hooks only exist in the HD HUD section).
bool active() {
    static const bool ok = [] {
        if (const char* e = std::getenv("DOT_DEKUI"); e && *e == '0') return false;  // DOT_DEKUI=0: native menus
        Guest g;
        const struct { const char* name; bool ok; } checks[] = {
            {"NativeInventory", NativeInventory::CodeMatches(g)},
            {"InventoryNavigation", InventoryNavigation::CodeMatches(g)},
            {"ItemPreview", ItemPreview::CodeMatches(g)},
            {"DialogueScale", DialogueScale::CodeMatches(g)},
            {"DungeonMap", DungeonMap::CodeMatches(g)},
            {"MenuBackdrop", MenuBackdrop::CodeMatches(g)},
            {"MenuMemory", MenuMemory::CodeMatches(g)},
            {"MenuPageSwitch", MenuPageSwitch::CodeMatches(g)},
            {"NativeMinimap", NativeMinimap::CodeMatches(g)},
            {"NativeNotebook", NativeNotebook::CodeMatches(g) && NativeNotebook::MapCodeMatches(g)},
            {"NativeStartup", NativeStartup::CodeMatches(g)},
            {"NativeItemPresent", NativeItemPresent::IntroCodeMatches(g)},
            {"OptionsMenu", OptionsMenu::CodeMatches(g)},
            {"PauseCapture", PauseCapture::CodeMatches(g)},
            {"PhotoCapture", PhotoCapture::CodeMatches(g)},
            {"SingleScreenText", SingleScreenText::CodeMatches(g)},
            {"SongList", SongList::CodeMatches(g)},
            {"MenuSound", MenuSound::CodeMatches(g)},
            {"MinimapShortcut", MinimapShortcut::CodeMatches(g)},
            {"HudButtons", HudButtons::VisibilityCodeMatches(g)},
        };
        unsigned matched = 0;
        for (const auto& k : checks) matched += k.ok;
        if (matched == std::size(checks)) return true;
        // Only builds from DekUI's code.bin have the hooks: on others (v1.0 shares most of the game
        // code) a partial match is expected, so the details are a trace.
        if (std::getenv("DOT_TRACE_DEKUI"))
            for (const auto& k : checks)
                if (!k.ok) std::fprintf(stderr, "dekui: %s does not match this code.bin, single-screen menus off\n", k.name);
        return false;
    }();
    return ok;
}

// Runs a DekUI handler as if its SVC executed at pc: r15 = the next instruction. Returns 0 to
// fall through, else the pc the handler chose (a call when it also set r14).
template <class F>
u32 redirect(Context* c, u32 pc, F&& handler) {
    c->r[15] = pc + 4;
    handler();
    return c->r[15] == pc + 4 ? 0 : c->r[15];
}

u32 cpsr(const Context* c) { return c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28; }
void set_flags(Context* c, u32 p) { c->n = p >> 31 & 1, c->z = p >> 30 & 1, c->c = p >> 29 & 1, c->v = p >> 28 & 1; }

}  // namespace

bool dekui_active() { return active(); }

// ---- NativeInventory (FirstSVC + 0) and the shared layout passes (FirstSVC + 1..3)

// mov r4, r0 at the Items/Masks page entry (0x59E3E8).
void dekui_capture_page(Context* c) {
    Guest g;
    if (active()) H.inventory_hook.Capture(g, c->r[0]);
    c->r[4] = c->r[0];
}

// mov r4, r0 before the layout manager draws one screen's queue (0x126814): r0 manager, r1 screen.
void dekui_layout_pass(Context* c) {
    Guest g;
    const u32 m = c->r[0], s = c->r[1];
    c->r[4] = m;
    if (!active()) return;
    if (MenuBackdrop::CodeMatches(g)) H.menu_backdrop.Pass(g, m, s);
    H.options_hook.Pass(g, m, s), H.song_reset_hook.Pass(g, m, s), H.double_time_hook.Pass(g, m, s);
    H.dungeon_map_hook.Pass(g, m, s), H.song_hook.Pass(g, m, s), H.ocarina_hook.Pass(g, m, s);
    H.item_present_hook.Pass(g, m, s);
    H.press_hook.Pass(g, m, s), H.inventory_hook.Pass(g, m, s), H.notebook_hook.Pass(g, m, s);
    H.file_select_hook.Pass(g, m, s);
    if (s == 5) FileSelectInput::Observe(H.file_select_hook.active && H.file_select_hook.entrance_pending);
    H.banner_hook.Pass(g, m, s);
    // DOT_TRACE_DEKUI=1: routing status changes (1 no page, 2 routed, 3/4 rejected).
    static const bool trace = std::getenv("DOT_TRACE_DEKUI");
    static unsigned last = 0;
    if (trace && s == 5 && NativeInventory::status.load() != last) {
        last = NativeInventory::status.load();
        std::fprintf(stderr, "dekui: inventory status %u panel %08X gear %d icons %u\n", last, H.inventory_hook.panel,
                     int(H.inventory_hook.gear), NativeInventory::routed_icons.load());
    }
}

// add r2, r4, #8 before one layout (r4) is drawn (0x1262F0).
void dekui_layout_begin(Context* c) {
    Guest g;
    auto& r = c->r;
    const u32 layout = r[4], native = layout + 8;
    r[2] = native;
    if (!active()) return;
    H.minigame_hook.Begin(g, layout), H.hud_buttons_hook.Begin(g, layout), H.song_reset_hook.Begin(g, layout);
    H.banner_hook.Begin(g, layout, kWide), H.double_time_hook.Begin(g, layout), H.dungeon_map_hook.Begin(g, layout);
    H.song_hook.Begin(g, layout), H.item_present_hook.Begin(g, layout), H.loading_icon_hook.Begin(g, layout);
    H.press_hook.Begin(g, layout), H.inventory_hook.Begin(g, layout, kWide), H.notebook_hook.Begin(g, layout, kWide);
    H.file_select_hook.Begin(g, layout, kWide);
    H.options_hook.Begin(g, layout);
    H.player_hud_hook.Begin(g, layout);
    const std::int64_t us = std::int64_t(hle::svc::GetSystemTick() / 268.111856);
    auto below = [&] { return r[2] != native ? r[2] : r[13]; };  // free stack for the next tree
    r[2] = H.notebook_hook.PromptRoot(g, layout, r[13]);
    if (const u32 root = H.ocarina_hook.Root(g, layout, r[13], kWide); root != native) r[2] = root;
    if (const u32 root = H.magic_hook.Root(g, layout, r[13]); root != native) r[2] = root;
    if (const u32 root = H.song_hook.ShadowRoot(g, layout, r[13]); root != native) r[2] = root;
    r[2] = H.song_hook.HeaderRoot(g, layout, r[2], below());
    r[2] = H.song_hook.PanelRoot(g, layout, r[2], below());
    if (const u32 root = H.inventory_shadow_hook.Root(g, H.inventory_hook, layout, r[13], &H.item_present_hook); root != native)
        r[2] = root;
    H.items_map_prompt_hook.ResolveDescription(g, H.item_preview_hook.owner, H.inventory_hook.panel);
    if (const u32 root = H.items_map_prompt_hook.Root(g, H.inventory_hook, layout, below(), r[2], kWide, us); root != native)
        r[2] = root;
    if (const u32 root = H.dungeon_map_hook.PromptRoot(g, layout, r[13], kWide); root != native) r[2] = root;
    r[2] = H.dungeon_map_hook.PanelRoot(g, layout, below(), r[2]);
    r[2] = H.dungeon_map_hook.ShadowRoot(g, layout, below(), r[2]);
    r[2] = H.map_items_prompt_hook.MapRoot(g, H.dungeon_map_hook, layout, below(), r[2], kWide, us);
    r[2] = H.world_map_prompt_hook.Root(g, H.dungeon_map_hook, layout, below(), r[2], kWide, us);
    r[2] = H.menu_tabs_hook.Root(g, H.inventory_hook, layout, below(), r[2]);
    H.menu_help_hook.source.ResolveDescription(g, H.item_preview_hook.owner, H.inventory_hook.panel);
    r[2] = H.menu_help_hook.Root(g, H.inventory_hook, layout, below(), r[2]);
    if (!H.item_present_hook.IsDescription(g, H.select_help_hook.source.description)) H.select_help_hook.source.description = 0;
    r[2] = H.select_help_hook.Root(g, H.inventory_hook, layout, below(), r[2], &H.item_present_hook);
    r[2] = H.file_select_prompt_hook.Root(g, H.file_select_hook, H.options_hook, layout, below(), r[2], kWide);
    r[2] = H.double_time_hook.ShadeRoot(g, layout, r[2], below());
    r[2] = H.hud_item_background_hook.Root(g, layout, r[2], below());
    if (r[2] != native) r[13] = r[2];  // the trees live below the new sp while it draws
}

// str r0, [r6] after it was drawn (0x1262FC): the poses written by Begin are restored.
void dekui_layout_end(Context* c) {
    Guest g;
    auto& r = c->r;
    if (active()) {
        auto give_back = [&](u32 sp) { if (sp) r[13] = sp; };
        give_back(H.hud_item_background_hook.scratch.End(g));
        give_back(H.double_time_hook.shade_scratch.End(g));
        give_back(H.file_select_prompt_hook.End(g));
        give_back(H.select_help_hook.scratch.End(g));
        give_back(H.menu_help_hook.scratch.End(g));
        give_back(H.menu_tabs_hook.scratch.End(g));
        give_back(H.world_map_prompt_hook.End(g));
        give_back(H.map_items_prompt_hook.prompt.End(g));
        give_back(H.dungeon_map_hook.shadow_scratch.End(g));
        give_back(H.dungeon_map_hook.panel_scratch.End(g));
        give_back(H.dungeon_map_hook.map_prompts.scratch.End(g));
        give_back(H.items_map_prompt_hook.prompt.End(g));
        give_back(H.inventory_shadow_hook.scratch.End(g));
        give_back(H.song_hook.backdrop.scratch.End(g));
        give_back(H.song_hook.header.scratch.End(g));
        give_back(H.song_hook.shadow_scratch.End(g));
        give_back(H.magic_hook.End(g));
        give_back(H.ocarina_hook.End(g));
        give_back(H.notebook_hook.prompt_hook.End(g));
        H.player_hud_hook.End(g);
        H.options_hook.End(g);
        H.song_hook.End(g), H.item_present_hook.End(g), H.file_select_hook.End(g), H.notebook_hook.End(g);
        H.inventory_hook.End(g), H.press_hook.End(g), H.loading_icon_hook.End(g), H.dungeon_map_hook.End(g);
        H.double_time_hook.End(g), H.banner_hook.End(g), H.song_reset_hook.End(g), H.hud_buttons_hook.End(g);
        H.minigame_hook.End(g);
    }
    w32(r[6], r[0]);
}

// ---- ItemPreview (the Items/Masks/Gear pages and their 3D item model)

// mov r4, r0 where the Items/Masks page has built its icons (0x5C250C; r1 = controller).
void dekui_enter_page(Context* c) {
    Guest g;
    if (active()) H.item_preview_hook.Enter(g, c->r[0], c->r[1]), H.inventory_hook.Capture(g, c->r[0]);
    c->r[4] = c->r[0];
}
// mov r4, r0 when the Items/Masks page leaves (0x5C27DC).
void dekui_leave_page(Context* c) {
    if (active()) H.item_preview_hook.Leave(c->r[0]);
    c->r[4] = c->r[0];
}
// mov r4, r0 at the model frame (0x111C6C), mov r1, r6 before its matrix (r6) is copied
// (0x1122E4), nop after (0x1122EC).
void dekui_model_frame(Context* c) {
    Guest g;
    if (active()) H.item_preview_hook.ModelFrame(c->r[0]), H.dungeon_map_hook.ModelFrame(g, c->r[0]);
    c->r[4] = c->r[0];
}
void dekui_model_begin(Context* c) {
    Guest g;
    if (active()) {
        H.item_preview_hook.ModelBegin(g, c->r[4], c->r[6]);
        H.dungeon_map_hook.preview.ModelBegin(g, c->r[4], c->r[6]);
    }
    c->r[1] = c->r[6];
}
void dekui_model_end(Context*) {
    Guest g;
    if (active()) H.dungeon_map_hook.preview.ModelEnd(g), H.item_preview_hook.ModelEnd(g);
}
// mov r4, r0 at the Gear page entry (0x5A6B44).
void dekui_capture_gear(Context* c) {
    Guest g;
    if (active()) H.item_preview_hook.EnterGear(g, c->r[0], c->r[1]), H.inventory_hook.CaptureGear(g, c->r[0]);
    c->r[4] = c->r[0];
}
// mov r5, r0 when the Gear page closes (0x5A5FC8; also MenuBackdrop's release).
void dekui_close_gear(Context* c) {
    Guest g;
    if (active()) {
        H.menu_backdrop.Release(g);
        H.item_preview_hook.Leave(c->r[0]);
        if (H.inventory_hook.gear_page == c->r[0]) H.inventory_hook.Capture(g, 0);
    }
    c->r[5] = c->r[0];
}

// ---- DialogueScale: add r0, r4, #8 before a MessageWindow (r4) is posed (0x161C9C), add sp, sp, #8 after (0x161CA4)

void dekui_dialogue_begin(Context* c) {
    Guest g;
    const u32 layout = c->r[4];
    if (active()) {
        const bool description = H.item_preview_hook.Description(g, layout);
        const bool present_description = H.item_present_hook.IsDescription(g, layout);
        if (present_description) {
            H.select_help_hook.source.description = layout;
            H.select_help_hook.source.description_panel = H.item_present_hook.panel;
        }
        const auto& setting = (H.item_present_hook.mask_menu ? NativeItemPresent::mask_poses
                                                              : NativeItemPresent::poses)[NativeItemPresent::Description];
        const GearMenu::Pose description_pose{setting.x.load(), setting.y.load(), setting.scale.load()};
        const bool map_description = H.dungeon_map_hook.IsDescription(g, layout);
        const auto& map_setting = DungeonMap::poses[DungeonMap::Description];
        const GearMenu::Pose map_pose{map_setting.x.load(), map_setting.y.load(), map_setting.scale.load()};
        const bool reset_description = H.song_reset_hook.IsDescription(g, layout);
        const auto& reset_setting = SongReset::poses[SongReset::Description];
        const GearMenu::Pose reset_pose{reset_setting.x.load(), reset_setting.y.load(), reset_setting.scale.load()};
        const bool options_description = H.options_hook.IsDescription(g, layout);
        H.dialogue_hook.Begin(g, layout, description, description && H.item_preview_hook.gear,
                              options_description ? &H.options_hook.frame.description
                              : reset_description ? &reset_pose
                              : map_description   ? &map_pose
                              : present_description ? &description_pose
                                                    : nullptr);
        H.item_present_hook.BeginPromptPose(g, layout);
    }
    c->r[0] = layout + 8;
}
void dekui_dialogue_end(Context* c) {
    Guest g;
    if (active()) {
        H.dungeon_map_hook.RememberDescription(g, c->r[4]);
        H.item_present_hook.EndPromptPose(g);
        H.dialogue_hook.End(g, c->r[4]);
    }
    c->r[13] += 8;
}

// ---- InventoryNavigation: ldrb r1, [r4, #0x19|#0x18] in the Items/Masks cursor's vertical/horizontal
// move (0x5A0E88, 0x5A1498), orr r2, r2, r12 in the Gear cursor move (0x5A56C0).

static u32 nav(Context* c, bool vertical, u32 done) {
    Guest g;
    if (active() && (InventoryNavigation::PresentMove(g, c->r[4], c->r[13], vertical, c->r[5]) ||
                     InventoryNavigation::Move(g, c->r[4], c->r[13], vertical, c->r[5])))
        return done;
    c->r[1] = r8(c->r[4] + (vertical ? 0x19 : 0x18));
    return 0;
}
u32 dekui_nav_vertical(Context* c) { return nav(c, true, 0x5A0EB0); }
u32 dekui_nav_horizontal(Context* c) { return nav(c, false, 0x5A14C0); }
u32 dekui_nav_gear(Context* c) {
    Guest g;
    c->r[2] |= c->r[12];
    if (!active()) return 0;
    const auto next = InventoryNavigation::GearMove(g, c->r[0], c->r[2]);
    if (!next) return 0;
    const u32 current = r8(c->r[0] + 0x195);
    c->r[2] = *next;
    return *next == current ? 0x5A56E0 : 0x5A574C;
}

// ---- DungeonMap (Map page: world map, dungeon maps), FirstSVC + 0..7 at DungeonMap::PCs

u32 dekui_map(Context* c, unsigned i) {
    Guest g;
    auto& r = c->r;
    const u32 pc = DungeonMap::PCs[i];
    if (!active()) {  // the displaced instructions
        static constexpr u32 calls[] = {0, 0x2a326c, 0, 0, 0, DungeonMap::SetZoom, 0, 0};
        switch (i) {
        case 0: r[4] = r[0]; return 0;
        case 2: r[0] = r8(r[4] + 0x74c); return 0;
        case 3: r[0] = r8(r[6] + 0xc); return 0;
        case 4: r[0] = r32(r[4] + 0x1c); return 0;
        case 6: r[0] = 1; return 0;
        case 7: r[0] = r[5] + 0x35c; return 0;
        default: c->r[14] = pc + 4; return calls[i];
        }
    }
    return redirect(c, pc, [&] {
        switch (i) {
        case 0: H.dungeon_map_hook.Capture(g, r[0], r[1]), r[4] = r[0]; break;
        case 1: H.dungeon_map_hook.BeforeMap(g), r[14] = pc + 4, r[15] = 0x2a326c; break;
        case 2: H.dungeon_map_hook.AfterMap(g), r[0] = r8(r[4] + 0x74c); break;
        case 3: r[0] = DungeonMap::FilterA(g, r[4], r8(r[6] + 0xc)); break;
        case 4:
            r[0] = r32(r[4] + 0x1c);
            if (H.dungeon_map_hook.SkipWorld(g, r[4])) r[15] = DungeonMap::WorldEnd;
            break;
        case 5: H.dungeon_map_hook.RememberZoom(g, r[4], r[9], r[0]), r[14] = pc + 4, r[15] = DungeonMap::SetZoom; break;
        case 6: r[0] = H.dungeon_map_hook.EntryZoom(g, r[5]); break;
        case 7: r[0] = r[5] + 0x35c, r[15] = DungeonMap::OverviewCursorNext(g, r[4]); break;
        }
    });
}
#define MAP(i) u32 dekui_map_##i(Context* c) { return dekui_map(c, i); }
MAP(0) MAP(1) MAP(2) MAP(3) MAP(4) MAP(5) MAP(6) MAP(7)
#undef MAP

// ---- MenuBackdrop (pause backdrop kept across page switches), FirstSVC + 0..6 at MenuBackdrop::PCs
// (+3 = the Gear close, owned by ItemPreview's dekui_close_gear)

u32 dekui_backdrop(Context* c, unsigned i) {
    Guest g;
    auto& r = c->r;
    const u32 pc = MenuBackdrop::PCs[i];
    if (!active()) {  // the displaced instructions
        switch (i) {
        case 0: w16(r[1] + r[7], r[6]); return 0;            // strh r6, [r1, r7]
        case 1: case 2: w16(r[1] + r[5], r[0]); return 0;    // strh r0, [r1, r5]
        case 4: r[4] = r[0]; return 0;
        case 5: r[14] = pc + 4; return 0x609fa8;             // bl
        case 6: r[0] = r32(r[4] + 0x100); return 0;
        }
        return 0;
    }
    return redirect(c, pc, [&] {  // each handler also does the displaced instruction
        switch (i) {
        case 0: H.menu_backdrop.Pause(g, r[7], r[4], r[1], r[6]); break;
        case 1: case 2: H.menu_backdrop.Pause(g, r[5], r[4], r[1], r[0]); break;
        case 4: H.menu_backdrop.CloseMap(g, r, H.menu_page_switch.KeepCapture(g, MenuBackdrop::Hook::Game(g, r[1]) + 0x11010)); break;
        case 5: H.menu_backdrop.GearImageImport(g, r); break;
        case 6: H.menu_backdrop.FinishGearPhoto(g, r); break;
        }
    });
}
#define BACKDROP(i) u32 dekui_backdrop_##i(Context* c) { return dekui_backdrop(c, i); }
BACKDROP(0) BACKDROP(1) BACKDROP(2) BACKDROP(4) BACKDROP(5) BACKDROP(6)
#undef BACKDROP

// ---- MenuMemory (reopen the last page), FirstSVC + 0..2 at MenuMemory::PCs

u32 dekui_menu_memory(Context* c, unsigned i) {
    Guest g;
    auto& r = c->r;
    const u32 pc = MenuMemory::PCs[i];
    if (!active()) {
        if (i == 0) return r[14] = pc + 4, r[2];  // blx r2
        if (i == 1) w8(r[4] + 0x10, r[0]);         // strb r0, [r4, #0x10]
        else w8(r[0] + 0x186, r[6]);               // strb r6, [r0, #0x186]
        return 0;
    }
    u32 to = redirect(c, pc, [&] {
        if (i == 0) {
            if (!H.menu_memory.Prepare(g, r) && !(MenuBackdrop::CodeMatches(g) && H.menu_backdrop.Prepare(g, r)))
                r[15] = r[2] & ~1u;
        } else if (i == 1) {
            H.menu_memory.Ready(g, r[4], H.menu_backdrop.PreparedReady(g, r[4], r[0]));
        } else if (!(MenuBackdrop::CodeMatches(g) && H.menu_backdrop.KeepGearCapture(g, r[0], r[4], r[7]))) {
            H.menu_memory.GearOpenCapture(g, r[0], r[6], r[4], r[7]);
        }
    });
    if (i == 0 && to == (r[2] & ~1u)) to = r[2], r[14] = pc + 4;  // keep the Thumb bit; blx sets lr
    return to;
}
u32 dekui_menu_memory_0(Context* c) { return dekui_menu_memory(c, 0); }
u32 dekui_menu_memory_1(Context* c) { return dekui_menu_memory(c, 1); }
u32 dekui_menu_memory_2(Context* c) { return dekui_menu_memory(c, 2); }

// ---- MenuPageSwitch (L/R between pages, backdrop kept), FirstSVC + 0..7 at MenuPageSwitch::PCs

u32 dekui_page_switch(Context* c, unsigned i) {
    Guest g;
    auto& r = c->r;
    const u32 pc = MenuPageSwitch::PCs[i];
    if (!active()) {
        switch (i) {
        case 0: case 2: r[4] = r[0]; return 0;
        case 1: r[14] = pc + 4; return r[2];  // blx r2
        case 3: return 0x1bb320;              // b (tail call)
        case 6: c->s[1] = r32(0x5a5ef0 + 8 + 0x32 * 4); return 0;  // vldr s1, [pc, #0xc8]
        default: r[14] = pc + 4; return i == 7 ? 0x2006e4 : 0x198864;  // bl
        }
    }
    u32 to = redirect(c, pc, [&] {
        switch (i) {
        case 0:
            H.menu_sound_context = r[0];
            H.menu_backdrop.Tick(g);
            if (MenuMemory::CodeMatches(g)) H.menu_memory.Tick(g, r[0]);
            H.menu_page_switch.Tick(g, r[0], H.menu_backdrop.HasRetainedCapture() && H.menu_backdrop.Active(g));
            r[4] = r[0];
            break;
        case 1:
            r[14] = MenuPageSwitch::PCs[1] + 4;
            if (H.menu_page_switch.PairPending(g, r[1])) r[0] = 1;
            else r[15] = r[2] & ~1u;
            break;
        case 2:
            if (H.menu_page_switch.FinishLayout(g, r[0])) c->s[0] = std::bit_cast<u32>(10.f);
            r[4] = r[0];
            break;
        case 6:
            c->s[1] = MenuBackdrop::CodeMatches(g) && H.menu_backdrop.Active(g) ? 0 : MenuPageSwitch::GearBackdropAlpha();
            break;
        case 7: H.menu_page_switch.MapCloseSound(g, r); break;
        default:
            if (i == 5 && H.menu_backdrop.CloseGearPhoto(g, r, H.menu_page_switch.KeepCapture(g, r[0]))) break;
            H.menu_page_switch.CaptureCall(g, r, i);
            break;
        }
    });
    if (i == 1 && to == (r[2] & ~1u)) to = r[2];  // keep the Thumb bit
    return to;
}
#define PAGE(i) u32 dekui_page_switch_##i(Context* c) { return dekui_page_switch(c, i); }
PAGE(0) PAGE(1) PAGE(2) PAGE(3) PAGE(4) PAGE(5) PAGE(6) PAGE(7)
#undef PAGE

// ---- MenuSound: strbls r0, [r4] (0x13921C) stores the menu sound request
void dekui_menu_sound(Context* c) {
    Guest g;
    if (active()) MenuSound::Store(g, H.menu_sound_context, c->r[4], c->r[0]);
    else w8(c->r[4], c->r[0]);
}

// ---- NativeItemPresent: bl at 0x5BC790 opens the "give an item" menu's intro message
u32 dekui_present_intro(Context* c) {
    Guest g;
    auto& r = c->r;
    r[14] = NativeItemPresent::IntroPC + 4;
    if (active() && NativeItemPresent::SkipIntro(g, r[5])) return r[0] = 1, 0;  // the caller closes it and moves on
    return 0x1adb90;  // the original call
}

// ---- PhotoCapture / PauseCapture (no HD backend: bookkeeping and the displaced instructions)
void dekui_photo_0(Context* c) { if (active()) H.photo_capture_hook.Begin(c->r[6]); c->r[3] = r32(c->r[13] + 4); }
void dekui_photo_1(Context* c) { Guest g; if (active()) H.photo_capture_hook.Keep(g); c->r[0] = r32(c->r[13] + 12); }
void dekui_photo_2(Context* c) {
    Guest g;
    if (active()) H.photo_capture_hook.Import(g, c->r[4], c->r[5], c->r[6]);
    c->r[2] = c->r[4] + 12;
}
void dekui_photo_3(Context* c) {
    Guest g;
    if (active()) H.photo_capture_hook.Bind(g, c->r[0], c->r[1], c->r[2]);
    c->r[5] = c->r[1];
}
void dekui_pause_1(Context* c) {
    Guest g;
    if (active() && !H.menu_backdrop.HasRetainedCapture()) H.pause_capture_hook.Bind(g, c->r[3], c->r[2]);
    c->r[12] = c->r[0] + 0x1c;
}
void dekui_pause_2(Context* c) {
    if (active()) {
        H.photo_capture_hook.Destroy(c->r[0]);
        if (!H.menu_backdrop.HasRetainedCapture()) H.pause_capture_hook.Reset();
    }
    c->r[4] = c->r[0];
}

// ---- NativeStartup (title "Press Start", loading icon): mov r5, r0 (0x5EDA9C), ldr r0, [r5, #0xc] (0x5EDAD8)
void dekui_startup_enter(Context* c) {
    Guest g;
    if (active()) H.startup_hook.Enter(g, c->r[0]);
    c->r[5] = c->r[0];
}
void dekui_startup_matrix(Context* c) {
    Guest g;
    if (active()) H.startup_hook.Matrix(g, c->r[5], c->r[4]);
    c->r[0] = r32(c->r[5] + 0xc);
}

// ---- Bomber's Notebook: schedule rows, row window, notebook capture, map/schedule input
void dekui_schedule_enter(Context* c) {
    Guest g;
    if (active()) H.notebook_hook.schedule_rows.Enter(g, c->r[0]);
    c->r[4] = c->r[0];
}
void dekui_schedule_stop(Context* c) {
    if (active()) c->r[1] = 0;  // the native stop/reset path for vertical scrolling
    c->r[2] = 0;                // mov r2, #0
}
void dekui_notebook_sync(Context* c) {
    Guest g;
    auto& r = c->r;
    if (active() && H.notebook_hook.row_window.ImmediateScroll(g, r[0], r[2], r[3]))
        r[2] |= 8, r[3] = 0;  // populate the changed window now, without the deferred scroll
    r[4] = r[0];
}
void dekui_notebook_rows_enter(Context* c) {
    Guest g;
    if (active()) H.notebook_hook.row_window.Enter(g, c->r[0]);
    c->r[4] = c->r[0];
}
void dekui_notebook_rows_leave(Context* c) {
    Guest g;
    if (active()) H.notebook_hook.row_window.Leave(g);
    c->r[0] = c->r[4];
}
// tst r3, #4 (0x7D04C4) / tst r3, #0x800 (0x7D04B0): flags of the notebook's map/schedule input
void dekui_notebook_map_input(Context* c) {
    Guest g;
    if (active()) set_flags(c, NativeNotebook::MapInputFlags(g, c->r[4], c->r[3], cpsr(c)));
    else c->n = (c->r[3] & 4) >> 31, c->z = (c->r[3] & 4) == 0;
}
void dekui_notebook_schedule_input(Context* c) {
    Guest g;
    if (active()) set_flags(c, NativeNotebook::ScheduleInputFlags(g, c->r[4], c->r[3], cpsr(c)));
    else c->n = 0, c->z = (c->r[3] & 0x800) == 0;
}
void dekui_notebook(Context* c) {
    Guest g;
    if (active()) H.notebook_hook.Capture(g, c->r[0]);
    c->r[4] = c->r[0];
}

// ---- SongList: cmp r1, r2 in the song list's cursor move (0x5A745C)
void dekui_song_nav(Context* c) {
    auto& r = c->r;
    if (active() && SongList::enabled && H.song_hook.panel && r32(r[4] + 0x14) == H.song_hook.panel) {
        const u32 context = r32(r[7] + 4), input = context ? r32(context + 0x16c) : 0;
        if (input) r[2] = SongList::Selection(r[1], r32(input + 0x34) | r32(input + 0x8c),
                                              (r32(r[5] + 0x1f8) & r32(r[6] + 0x1c)) != 0);
    }
    const u32 a = r[1], b = r[2], d = a - b;
    c->n = d >> 31, c->z = d == 0, c->c = a >= b, c->v = ((a ^ b) & (a ^ d)) >> 31;
}

// ---- SingleScreenText: ldr r3, [r0, #4] (0x7D1498) when message r1 is loaded. Only while enabled
// and never for 0xC01 (Classic Saving's owl): those paths write the original text, which the
// stripped data headers do not carry (tools/dekui_strip_text.py).
void dekui_text(Context* c) {
    Guest g;
    if (active() && SingleScreenText::enabled.load() && c->r[1] != 0xc01) SingleScreenText::Apply(g, c->r[0], c->r[1]);
    c->r[3] = r32(c->r[0] + 4);
}

// ---- OptionsMenu: mov r4, r0 at its draw (0x5A31C0) and selection (0x5A1E30)
void dekui_options_draw(Context* c) {
    Guest g;
    if (active()) H.options_hook.Capture(g, c->r[0]);
    c->r[4] = c->r[0];
}
void dekui_options_select(Context* c) {
    Guest g;
    if (active()) c->r[2] = OptionsMenu::Selection(g, c->r[0], c->r[2]);
    c->r[4] = c->r[0];
}

// ---- NativeMinimap: nop before (0x59B344) and ldr r0, [r4, #0x38] after (0x59B34C) the gameplay map
void dekui_minimap_before(Context* c) {
    Guest g;
    if (active()) H.minimap_hook.Begin(g, c->r[4], Minimap::enabled.load(), c->r[0], MenuBackdrop::Hook::HideGameplayMap(g, c->r[0]));
}
void dekui_minimap_after(Context* c) {
    Guest g;
    if (active())
        H.minimap_hook.End(g, c->r[4], Minimap::enabled.load(), Minimap::scale.load(), Minimap::opacity.load(), 400.f);
    c->r[0] = r32(c->r[4] + 0x38);
}

// ---- StickControls / MinimapShortcut (Restoration's gameplay input: stick clicks, ZR+Select map)
u32 dekui_stick_pad(Context* c) {
    Guest g;
    if (!active()) return w16(c->r[5], c->r[2]), 0;  // strh r2, [r5]
    return redirect(c, StickControls::PadPC, [&] { H.stick_controls_hook.Pad(g, c->r); });
}
u32 dekui_stick_map(Context* c) {
    Guest g;
    if (!active()) { const u32 t = c->r[1] & c->r[5]; c->n = t >> 31, c->z = t == 0; return 0; }  // tst r1, r5
    return redirect(c, StickControls::MapPC, [&] { H.stick_controls_hook.Map(g, c->r); });
}
u32 dekui_minimap_shortcut(Context* c) {
    Guest g;
    if (!active()) return c->r[0] = 5, 0;  // mov r0, #5
    return redirect(c, MinimapShortcut::PC, [&] { MinimapShortcut::Handle(g, c->r); });
}

// ---- Host input (hid:USER), as DekUI's hid.cpp: Ocarina and file-select filters. StickControls
// stays off (its default): our L3/R3 already touch items I/II (hle/hid.cpp).
void dekui_filter_pad(u32& buttons, s16& x, s16& y) {
    if (!active()) return;
    OcarinaInput::Filter(buttons);
    FileSelectInput::Filter(buttons, x, y);
}
bool dekui_touch_blocked() { return active() && FileSelectInput::Blocked(); }

}  // namespace recomp::hooks
