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
// instructions for SVCs in Azahar's dynarmic (src/core/arm/dynarmic/arm_dynarmic.cpp); here the
// same instructions are instruction hooks (tools/recomp/hooks.txt, Restoration + HD HUD section)
// and each hook does what DekUI's SVC handler does, including the displaced instruction.
//
// Ported so far: the Items/Masks/Gear pages drawn on the top screen (NativeInventory: the layouts
// move from the bottom screen's render queue to the top's and are re-posed while drawn), their
// D-pad/stick navigation (InventoryNavigation), tabs (MenuTabs), button legend (MenuHelp), map
// prompt (ItemsMapPrompt), panel shadow (InventoryShadow), item model (ItemPreview) and the item
// description box (DialogueScale).
//
// Several modules draw extra panes by building layout trees on the guest stack below sp: their
// Root() returns the tree to draw in place of r4 + 8, sp moves below it while the layout draws,
// and End() gives sp back (DekUI's SVC 2/3 handlers do the same).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "hle/svc.h"
#include "mm3d/dialogue_scale.h"
#include "mm3d/inventory_navigation.h"
#include "mm3d/inventory_shadow.h"
#include "mm3d/item_preview.h"
#include "mm3d/menu_help.h"
#include "mm3d/menu_tabs.h"
#include "mm3d/native_inventory.h"
#include "recomp/hooks.h"

namespace recomp::hooks {
namespace {

namespace Inv = MM3D::NativeInventory;

struct Guest {  // DekUI's memory interface: reads of unmapped memory fail instead of faulting
    std::optional<u32> Read(u32 at) { return readable(at, 4) ? std::optional<u32>(r32(at)) : std::nullopt; }
    void Write(u32 at, u32 value) { w32(at, value); }
};

Inv::Hook g_inventory;
MM3D::ItemPreview::Hook g_preview;
MM3D::DialogueScale::Hook g_dialogue;
MM3D::InventoryShadow::Hook g_shadow;
MM3D::ItemsMapPrompt::Hook g_map_prompt;
MM3D::MenuTabs::Hook g_tabs;
MM3D::MenuHelp::Hook g_help;

// DekUI's release defaults (resources/mm3d/defaults/presentation.json).
// shortcut: fixed layout; expose these in the F1 menu / config.toml when someone wants to move them.
bool apply_defaults() {
    Inv::enabled = true, Inv::shared_slide = true, Inv::bar_enabled = true;
    Inv::x = -12.0f, Inv::y = 87.7f, Inv::scale = 0.6f;
    Inv::masks_x = 2.4f, Inv::masks_y = 0.0f, Inv::masks_scale = 0.97f;
    Inv::bar_x = -14.3f, Inv::bar_y = 34.0f, Inv::bar_scale = 0.6f, Inv::bar_spacing = 69.0f;
    Inv::buttons_enabled = true;
    const Inv::ButtonPose buttons[3] = {{-95.5f, 0, 1}, {-31.3f, 21.3f, 0.64f}, {-61.2f, 21.3f, 0.64f}};  // Back, Masks, Items
    for (int i = 0; i < 3; ++i) Inv::buttons[i].x = buttons[i].x, Inv::buttons[i].y = buttons[i].y, Inv::buttons[i].scale = buttons[i].scale;
    Inv::background_enabled = true;
    Inv::background_pose.x = -73.1f, Inv::background_pose.y = 0.0f, Inv::background_pose.scale = 1.0f;
    Inv::shadow_width = 0.99f, Inv::shadow_opacity = 0.75f;
    namespace G = MM3D::GearMenu;
    G::Options gear;
    gear.poses[G::Panel] = {-10.7f, 87.9f, 0.58f};
    gear.poses[G::Right] = {-259.6f, 35.0f, 0.65f};
    gear.poses[G::Background] = {-73.1f, 0.0f, 1.0f};
    gear.poses[G::Buttons] = {-257.9f, 0.0f, 1.0f};
    gear.poses[G::Model] = {4.11f, 0.44f, 0.87f};
    gear.poses[G::Description] = {83.5f, 1.8f, 0.6f};
    gear.poses[G::Photo] = {78.7f, -20.6f, 0.88f};
    G::Apply(gear);
    MM3D::ItemPreview::model_x = 4.11f, MM3D::ItemPreview::model_y = 0.44f, MM3D::ItemPreview::model_scale = 0.87f;
    MM3D::DialogueScale::item_x = 83.5f, MM3D::DialogueScale::item_y = 1.8f, MM3D::DialogueScale::item_scale = 0.6f;
    MM3D::MenuTabs::gear_pose.x = -14.9f, MM3D::MenuTabs::gear_pose.y = 19.7f, MM3D::MenuTabs::gear_pose.scale = 0.62f;
    MM3D::ItemsMapPrompt::elements[2].y = -3.6f;  // the label; pose, R and arrow keep the module's defaults
    MM3D::MenuHelp::Options help;
    help.pose = {157.8f, 230.4f, 0.53f};
    help.horizontal = true, help.spacing = 20.9f, help.button_gap = 4.1f, help.button_scale = 0.91f;
    help.opacity = 0.85f;
    MM3D::MenuHelp::Apply(help);
    // The NPC "give item" menu (NativeItemPresent) is not ported: keep its navigation native.
    MM3D::NativeItemPresent::enabled = false;
    return true;
}
const bool g_defaults = apply_defaults();

// The code DekUI was verified against, checked once (the hooks only exist in the HD HUD section).
bool active() {
    static const bool ok = [] {
        if (const char* e = std::getenv("DOT_DEKUI"); e && *e == '0') return false;  // DOT_DEKUI=0: native menus
        Guest g;
        return Inv::CodeMatches(g) && MM3D::InventoryNavigation::CodeMatches(g) &&
               MM3D::ItemPreview::CodeMatches(g) && MM3D::DialogueScale::CodeMatches(g);
    }();
    return ok;
}

}  // namespace

bool dekui_inventory_routed() { return Inv::status.load() == 2; }

// mov r4, r0 at the Items/Masks page entry (0x59E3E8).
void dekui_capture_page(Context* c) {
    Guest g;
    if (active()) g_inventory.Capture(g, c->r[0]);
    c->r[4] = c->r[0];
}

// mov r4, r0 where the Items/Masks page has built its icons (0x5C250C; r1 = controller).
void dekui_enter_page(Context* c) {
    Guest g;
    if (active()) g_preview.Enter(g, c->r[0], c->r[1]), g_inventory.Capture(g, c->r[0]);
    c->r[4] = c->r[0];
}

// mov r4, r0 when the Items/Masks page leaves (0x5C27DC).
void dekui_leave_page(Context* c) {
    if (active()) g_preview.Leave(c->r[0]);
    c->r[4] = c->r[0];
}

// mov r4, r0 at the Gear page entry (0x5A6B44).
void dekui_capture_gear(Context* c) {
    Guest g;
    if (active()) g_preview.EnterGear(g, c->r[0], c->r[1]), g_inventory.CaptureGear(g, c->r[0]);
    c->r[4] = c->r[0];
}

// mov r5, r0 when the Gear page closes (0x5A5FC8).
void dekui_close_gear(Context* c) {
    Guest g;
    if (active()) {
        g_preview.Leave(c->r[0]);
        if (g_inventory.gear_page == c->r[0]) g_inventory.Capture(g, 0);
    }
    c->r[5] = c->r[0];
}

// The item model of the menu page (ItemPreview): mov r4, r0 at the model frame (0x111C6C),
// mov r1, r6 before its matrix is copied (0x1122E4, matrix at r6), nop after (0x1122EC).
void dekui_model_frame(Context* c) {
    if (active()) g_preview.ModelFrame(c->r[0]);
    c->r[4] = c->r[0];
}
void dekui_model_begin(Context* c) {
    Guest g;
    if (active()) g_preview.ModelBegin(g, c->r[4], c->r[6]);
    c->r[1] = c->r[6];
}
void dekui_model_end(Context*) {
    Guest g;
    if (active()) g_preview.ModelEnd(g);
}

// The item description MessageWindow (DialogueScale): add r0, r4, #8 before it is posed
// (0x161C9C), add sp, sp, #8 after (0x161CA4).
void dekui_dialogue_begin(Context* c) {
    Guest g;
    if (active()) {
        const bool description = g_preview.Description(g, c->r[4]);
        g_dialogue.Begin(g, c->r[4], description, description && g_preview.gear, nullptr);
    }
    c->r[0] = c->r[4] + 8;
}
void dekui_dialogue_end(Context* c) {
    Guest g;
    if (active()) g_dialogue.End(g, c->r[4]);
    c->r[13] += 8;
}

// mov r4, r0 before the layout manager draws one screen's queue (0x126814): r0 manager, r1 screen.
void dekui_layout_pass(Context* c) {
    Guest g;
    if (active()) g_inventory.Pass(g, c->r[0], c->r[1]);
    c->r[4] = c->r[0];
    // DOT_TRACE_DEKUI=1: routing status changes (1 no page, 2 routed, 3/4 rejected).
    static const bool trace = std::getenv("DOT_TRACE_DEKUI");
    static unsigned last = 0;
    if (trace && c->r[1] == 5 && Inv::status.load() != last) {
        last = Inv::status.load();
        std::fprintf(stderr, "dekui: inventory status %u panel %08X gear %d icons %u\n", last, g_inventory.panel,
                     int(g_inventory.gear), Inv::routed_icons.load());
    }
}

// add r2, r4, #8 before one layout (r4) is drawn (0x1262F0).
void dekui_layout_begin(Context* c) {
    Guest g;
    const u32 layout = c->r[4], native = layout + 8;
    c->r[2] = native;
    if (!active()) return;
    g_inventory.Begin(g, layout, false);  // our widescreen keeps 2D layouts at 5:3
    // Same chain and order as DekUI's SVC 2 (only the modules ported here).
    auto below = [&] { return c->r[2] != native ? c->r[2] : c->r[13]; };  // free stack for the next tree
    if (const u32 root = g_shadow.Root(g, g_inventory, layout, c->r[13], nullptr); root != native) c->r[2] = root;
    g_map_prompt.ResolveDescription(g, g_preview.owner, g_inventory.panel);
    const std::int64_t us = std::int64_t(hle::svc::GetSystemTick() / 268.111856);
    if (const u32 root = g_map_prompt.Root(g, g_inventory, layout, below(), c->r[2], false, us); root != native)
        c->r[2] = root;
    c->r[2] = g_tabs.Root(g, g_inventory, layout, below(), c->r[2]);
    g_help.source.ResolveDescription(g, g_preview.owner, g_inventory.panel);
    c->r[2] = g_help.Root(g, g_inventory, layout, below(), c->r[2], nullptr);
    if (c->r[2] != native) c->r[13] = c->r[2];  // the trees live below the new sp while it draws
}

// str r0, [r6] after it was drawn (0x1262FC): the poses written by Begin are restored.
void dekui_layout_end(Context* c) {
    Guest g;
    if (active()) {
        if (const u32 sp = g_help.scratch.End(g)) c->r[13] = sp;
        if (const u32 sp = g_tabs.scratch.End(g)) c->r[13] = sp;
        if (const u32 sp = g_map_prompt.prompt.End(g)) c->r[13] = sp;
        if (const u32 sp = g_shadow.scratch.End(g)) c->r[13] = sp;
        g_inventory.End(g);
    }
    w32(c->r[6], c->r[0]);
}

// ldrb r1, [r4, #0x19|#0x18] in the Items/Masks cursor's vertical/horizontal move (0x5A0E88,
// 0x5A1498): the relocated equip row changes which slot is up/down/left/right.
static u32 nav(Context* c, bool vertical, u32 done) {
    Guest g;
    if (active() && MM3D::InventoryNavigation::Move(g, c->r[4], c->r[13], vertical, c->r[5])) return done;
    c->r[1] = r8(c->r[4] + (vertical ? 0x19 : 0x18));
    return 0;
}
u32 dekui_nav_vertical(Context* c) { return nav(c, true, 0x5A0EB0); }
u32 dekui_nav_horizontal(Context* c) { return nav(c, false, 0x5A14C0); }

// orr r2, r2, r12 in the Gear cursor move (0x5A56C0).
u32 dekui_nav_gear(Context* c) {
    Guest g;
    c->r[2] |= c->r[12];
    if (!active()) return 0;
    const auto next = MM3D::InventoryNavigation::GearMove(g, c->r[0], c->r[2]);
    if (!next) return 0;
    const u32 current = r8(c->r[0] + 0x195);
    c->r[2] = *next;
    return *next == current ? 0x5A56E0 : 0x5A574C;
}

}  // namespace recomp::hooks
