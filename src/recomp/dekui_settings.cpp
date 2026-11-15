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

// DekUI's presentation settings (positions/scales of every single-screen element). The defaults
// are DekUI's release presentation.json (resources/dekui/, embedded at build time); a
// presentation.json next to game_data/ (same format as DekUI's) overrides them.
//
// Load() below is a transcription of the single-screen part of DekUI's Settings::Load
// (src/mm3d/settings.cpp, GPL-2.0-or-later), its body unchanged: it runs on a small stand-in for
// the Qt JSON/QString API it uses. InventoryMenu::Options/Apply come from DekUI's
// src/mm3d/inventory_menu.h (the rest of that header is Qt file handling), and Apply() from its
// overlay.cpp ApplyPresentationSettings.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "mm3d/banners.h"
#include "mm3d/dialogue_scale.h"
#include "mm3d/double_time.h"
#include "mm3d/dungeon_map.h"
#include "mm3d/file_select_prompt.h"
#include "mm3d/hud_adjustments.h"
#include "mm3d/hud_buttons.h"
#include "mm3d/item_preview.h"
#include "mm3d/items_map_prompt.h"
#include "mm3d/magic_border.h"
#include "mm3d/menu_help.h"
#include "mm3d/menu_tabs.h"
#include "mm3d/minigame_hud.h"
#include "mm3d/minimap.h"
#include "mm3d/native_file_select.h"
#include "mm3d/native_inventory.h"
#include "mm3d/native_item_present.h"
#include "mm3d/native_minimap.h"
#include "mm3d/native_notebook.h"
#include "mm3d/native_startup.h"
#include "mm3d/ocarina_prompt.h"
#include "mm3d/options_menu.h"
#include "mm3d/pause_capture.h"
#include "mm3d/player_hud.h"
#include "mm3d/single_screen_text.h"
#include "mm3d/song_list.h"
#include "mm3d/song_reset.h"
#include "mm3d/world_map_prompt.h"

extern const char* const kDekuiPresentation;  // resources/dekui/presentation.json (generated)

namespace MM3D {

namespace InventoryMenu {  // DekUI src/mm3d/inventory_menu.h
struct Options {
    MenuHelp::Options help;
    GearMenu::Options gear;
    bool enabled=true; float x=0.f,y=32.f,scale=.8f;
    float masks_x=0.f,masks_y=0.f,masks_scale=1.f;
    bool shared_slide=true;
    bool high_res_pause=true;
    bool bar_enabled=true; float bar_x=0.f,bar_y=-13.6f,bar_scale=.65f,bar_spacing=64.f;
    float description_x=0.f,description_y=0.f,description_scale=1.f;
    float model_x=0.f,model_y=0.f,model_scale=1.f;
    NativeInventory::ButtonPose map_prompt=ItemsMapPrompt::DefaultPose,gear_button{};
    std::array<NativeInventory::ButtonPose,3> map_prompt_elements=ItemsMapPrompt::DefaultElements;
    bool buttons_enabled=true;
    std::array<NativeInventory::ButtonPose,3> buttons{};
    bool background_enabled=true;
    NativeInventory::ButtonPose background{};
    float shadow_width=1.f,shadow_opacity=1.f;
};
inline void Apply(const Options& o) {
    MenuHelp::Apply(o.help);
    GearMenu::Apply(o.gear);
    MenuTabs::gear_pose.x=o.gear_button.x;
    MenuTabs::gear_pose.y=o.gear_button.y;
    MenuTabs::gear_pose.scale=o.gear_button.scale;
    ItemsMapPrompt::pose.x=o.map_prompt.x;
    ItemsMapPrompt::pose.y=o.map_prompt.y;
    ItemsMapPrompt::pose.scale=o.map_prompt.scale;
    for(unsigned i=0;i<o.map_prompt_elements.size();++i) {
        ItemsMapPrompt::elements[i].x=o.map_prompt_elements[i].x;
        ItemsMapPrompt::elements[i].y=o.map_prompt_elements[i].y;
        ItemsMapPrompt::elements[i].scale=o.map_prompt_elements[i].scale;
    }
    NativeInventory::x.store(o.x); NativeInventory::y.store(o.y);
    NativeInventory::scale.store(o.scale); NativeInventory::enabled.store(o.enabled);
    NativeInventory::masks_x.store(o.masks_x); NativeInventory::masks_y.store(o.masks_y);
    NativeInventory::masks_scale.store(o.masks_scale);
    NativeInventory::shared_slide.store(o.shared_slide);
    PauseCapture::enabled.store(o.high_res_pause);
    NativeInventory::bar_enabled.store(o.bar_enabled);
    NativeInventory::bar_x.store(o.bar_x); NativeInventory::bar_y.store(o.bar_y);
    NativeInventory::bar_scale.store(o.bar_scale); NativeInventory::bar_spacing.store(o.bar_spacing);
    DialogueScale::item_x.store(o.description_x); DialogueScale::item_y.store(o.description_y);
    DialogueScale::item_scale.store(o.description_scale);
    ItemPreview::model_x.store(o.model_x); ItemPreview::model_y.store(o.model_y);
    ItemPreview::model_scale.store(o.model_scale);
    NativeInventory::buttons_enabled.store(o.buttons_enabled);
    NativeInventory::shadow_width.store(o.shadow_width);
    NativeInventory::shadow_opacity.store(o.shadow_opacity);
    NativeInventory::background_enabled.store(o.background_enabled);
    NativeInventory::background_pose.x.store(o.background.x);
    NativeInventory::background_pose.y.store(o.background.y);
    NativeInventory::background_pose.scale.store(o.background.scale);
    for(unsigned i=0;i<o.buttons.size();++i) {
        NativeInventory::buttons[i].x.store(o.buttons[i].x);
        NativeInventory::buttons[i].y.store(o.buttons[i].y);
        NativeInventory::buttons[i].scale.store(o.buttons[i].scale);
    }
}
}  // namespace InventoryMenu

namespace {

// ---- Stand-in for the Qt JSON / QString API used by Settings::Load (flat objects only).
struct QJsonValue {
    enum Kind { Undefined, Number, Bool, String } kind = Undefined;
    double number = 0;
    std::string text;
    bool toBool(bool fallback = false) const { return kind == Bool ? number != 0 : fallback; }
    double toDouble(double fallback = 0) const { return kind == Number ? number : fallback; }
    int toInt(int fallback = 0) const {
        return kind == Number && number == std::floor(number) && std::abs(number) < 2147483648.0 ? int(number) : fallback;
    }
};
struct QByteArray {
    std::string bytes;
    const char* constData() const { return bytes.c_str(); }
};
struct QString {
    std::string s;
    QString() = default;
    QString(const char* text) : s(text) {}
    QString(std::string text) : s(std::move(text)) {}
    static QString number(unsigned long long n) { return std::to_string(n); }
    QString arg(const char* a) const { return replace(a); }
    QString arg(unsigned long long n) const { return replace(std::to_string(n)); }
    QString arg(unsigned n) const { return replace(std::to_string(n)); }
    QString arg(int n) const { return replace(std::to_string(n)); }
    QByteArray toUtf8() const { return {s}; }
    QString operator+(const char* t) const { return s + t; }
    QString operator+(const QString& t) const { return s + t.s; }
private:
    QString replace(const std::string& a) const {  // the lowest %N, as Qt
        for (char n = '1'; n <= '9'; ++n)
            if (const auto at = s.find(std::string("%") + n); at != std::string::npos) return s.substr(0, at) + a + s.substr(at + 2);
        return *this;
    }
};
struct QJsonObject {
    std::map<std::string, QJsonValue> values;
    QJsonValue value(const QString& key) const {
        const auto it = values.find(key.s);
        return it == values.end() ? QJsonValue{} : it->second;
    }
    bool contains(const QString& key) const { return values.count(key.s) != 0; }
};

// Flat JSON object reader: string keys with number/bool/string values; arrays, objects and null
// are skipped (none of the single-screen keys uses them). Returns false on malformed input.
bool parse(const std::string& text, QJsonObject& out) {
    size_t i = 0;
    auto ws = [&] { while (i < text.size() && std::isspace((unsigned char)text[i])) ++i; };
    auto str = [&](std::string& s) {
        if (text[i] != '"') return false;
        for (++i; i < text.size() && text[i] != '"'; ++i) s += text[i] == '\\' && i + 1 < text.size() ? text[++i] : text[i];
        return i++ < text.size();
    };
    auto skip = [&] {  // a nested array/object, strings included
        int depth = 0;
        do {
            if (text[i] == '"') { std::string ignored; if (!str(ignored)) return false; continue; }
            depth += text[i] == '[' || text[i] == '{' ? 1 : text[i] == ']' || text[i] == '}' ? -1 : 0;
            ++i;
        } while (depth > 0 && i < text.size());
        return depth == 0;
    };
    ws();
    if (i >= text.size() || text[i++] != '{') return false;
    for (;;) {
        ws();
        if (i < text.size() && text[i] == '}') return true;
        std::string key;
        if (i >= text.size() || !str(key)) return false;
        ws();
        if (i >= text.size() || text[i++] != ':') return false;
        ws();
        if (i >= text.size()) return false;
        QJsonValue v;
        if (text[i] == '"') v.kind = QJsonValue::String, str(v.text);
        else if (text[i] == '[' || text[i] == '{') { if (!skip()) return false; }
        else if (!text.compare(i, 4, "true")) v.kind = QJsonValue::Bool, v.number = 1, i += 4;
        else if (!text.compare(i, 5, "false")) v.kind = QJsonValue::Bool, i += 5;
        else if (!text.compare(i, 4, "null")) i += 4;
        else {
            char* end = nullptr;
            v.number = std::strtod(text.c_str() + i, &end);
            if (end == text.c_str() + i) return false;
            v.kind = QJsonValue::Number, i = end - text.c_str();
        }
        if (v.kind != QJsonValue::Undefined) out.values[key] = v;
        ws();
        if (i < text.size() && text[i] == ',') { ++i; continue; }
        return i < text.size() && text[i] == '}';
    }
}

struct Settings {  // the single-screen members of DekUI's MM3D::Settings (src/mm3d/settings.h)
    InventoryMenu::Options inventory_menu;
    ItemsMapPrompt::ReturnOptions map_items_prompt;
    NativeNotebook::Options notebook_live;
    OcarinaPrompt::Options ocarina_prompt;
    FileSelectPrompt::Options file_select_prompt;
    SongList::Options song_list;
    OptionsMenu::Options options_menu;
    DoubleTime::Options double_time;
    SongReset::Options song_reset;
    HudButtons::Options hud_buttons;
    HudAdjustments::Options player_hud=HudAdjustments::FreshDefaults();
    Banners::Options banners;
    bool banners_migrated=false;
    DungeonMap::Options dungeon_map;
    NativeInventory::ButtonPose world_map_zoom_prompt{0,0,.8f},world_map_native_zoom,
        dungeon_map_zoom_prompt{0,-28,.69f},dungeon_map_move_prompt{0,-44,.69f};
    NativeFileSelect::Options file_select;
    NativeItemPresent::Options item_present;
    NativeItemPresent::Options mask_present;
    NativeStartup::Options startup;
    bool single_screen_text = true;
    float dialogue_scale = 1.f;
    float dialogue_y = 0.f;
    float dialogue_background_width = 1.f;
    float dialogue_indicator_x = 0.f, dialogue_indicator_y = 0.f;
    MagicBorder::Options magic_border;
    MinigameHud::Options minigame_hud;
    NativeInventory::ButtonPose rupee_keys,carrots;
    float magic_bar_y = 0.f;
    float magic_bar_height = 1.f;
    float dpad_y = 0.f;
    float dpad_offset_x = 0.f, dpad_offset_y = 0.f;
    bool minimap = true;
    float minimap_scale = .36f, minimap_opacity = .75f;
    float minimap_right_margin = 8.f, minimap_bottom_margin = 8.f;
    bool Load(const QJsonObject& object);
};

bool Settings::Load(const QJsonObject& object) {
    // ---- transcribed from DekUI's Settings::Load
    if (object.value("version").toInt() != 1) return false;
    const auto value = [&](const char* key, float fallback, float lo, float hi) {
        const double number = object.value(key).toDouble(fallback);
        return std::isfinite(number) ? static_cast<float>(std::clamp(number, double(lo), double(hi))) : fallback;
    };
    if(object.contains("inventory_native")) {
        inventory_menu.enabled=object.value("inventory_native").toBool(true);
        inventory_menu.x=value("inventory_panel_x",0.f,-320.f,320.f);
        inventory_menu.y=value("inventory_panel_y",32.f,-240.f,240.f);
        inventory_menu.scale=value("inventory_panel_scale",.8f,.5f,1.5f);
    }
    inventory_menu.masks_x=value("inventory_masks_x",0.f,-320.f,320.f);
    notebook_live.schedule_prompt=object.value("notebook_schedule_prompt").toBool(true);
    notebook_live.schedule_prompt_y=value("notebook_schedule_prompt_y",-40,-240,240);
    notebook_live.map_prompt=object.value("notebook_map_prompt").toBool(true);
    notebook_live.map_prompt_y=value("notebook_map_prompt_y",-20,-240,240);
    notebook_live.event_prompt=object.value("notebook_event_prompt").toBool(true);
    notebook_live.prompt_x=value("notebook_buttons_x",0,-400,400);
    notebook_live.prompt_y=value("notebook_prompt_y",0,-240,240);
    notebook_live.prompt_scale=value("notebook_buttons_scale",1,.5f,2);
    notebook_live.prompt_icon_scale=value("notebook_buttons_icon_scale",1,.5f,2);
    if(!object.contains("notebook_buttons_x") && notebook_live.map_prompt_y==notebook_live.prompt_y)
        notebook_live.map_prompt_y=std::max(-240.f,notebook_live.prompt_y-20.f);
    notebook_live.popup_width=value("notebook_popup_width",1.f,.5f,3.f);
    notebook_live.popup_height=value("notebook_popup_height",1.f,.5f,3.f);
    notebook_live.expanded_rows=object.value("notebook_expanded_rows").toBool(true);
    notebook_live.enabled=object.value("notebook_native").toBool(true);
    world_map_native_zoom={value("world_map_native_zoom_x",0,-800,800),value("world_map_native_zoom_y",0,-480,480),
        value("world_map_native_zoom_scale",1,.1f,3)};
    world_map_zoom_prompt={value("world_map_zoom_prompt_x",0,-400,400),value("world_map_zoom_prompt_y",0,-240,240),
        value("world_map_zoom_prompt_scale",.8f,.25f,2)};
    dungeon_map_move_prompt={value("dungeon_map_move_prompt_x",0,-400,400),
        value("dungeon_map_move_prompt_y",-44,-240,240),value("dungeon_map_move_prompt_scale",.69f,.25f,2)};
    dungeon_map_zoom_prompt={value("dungeon_map_zoom_prompt_x",0,-400,400),
        value("dungeon_map_zoom_prompt_y",-28,-240,240),value("dungeon_map_zoom_prompt_scale",.69f,.25f,2)};
    // The old dungeon hints borrowed Notebook scale/icon scale and the
    // difference between its two Y values. Notebook X never affected them.
    // Fall back only for missing keys; saving makes the map values independent.
    dungeon_map.prompts={true,value("dungeon_map_prompts_x",0,-400,400),
        value("dungeon_map_prompts_y",0,-240,240),
        value("dungeon_map_prompts_scale",notebook_live.prompt_scale,.5f,2),
        value("dungeon_map_prompts_icon_scale",notebook_live.prompt_icon_scale,.5f,2),
        value("dungeon_map_prompts_spacing",notebook_live.prompt_y-notebook_live.map_prompt_y,-480,480)};
    // New global adjustments start neutral; retain every existing local pose.
    dungeon_map.prompt_group={value("dungeon_map_buttons_x",0,-400,400),
        value("dungeon_map_buttons_y",0,-240,240),value("dungeon_map_buttons_scale",1,.5f,2),
        value("dungeon_map_buttons_icon_scale",1,.5f,2)};
    dungeon_map.enabled=object.value("dungeon_map_top").toBool(true);
    dungeon_map.side_panels=object.value("dungeon_map_side_panels").toBool(true);
    dungeon_map.top_strip=object.value("dungeon_map_top_strip").toBool(true);
    dungeon_map.top_panel={value("dungeon_map_top_panel_x",0,-1024,1024),value("dungeon_map_top_panel_y",0,-480,480),
        value("dungeon_map_top_panel_scale",.3125f,.1f,3)};
    dungeon_map.panel_darkness=value("dungeon_map_panel_darkness",0,0,1);
    dungeon_map.arrow_scale=value("dungeon_map_arrows_scale",1,.1f,3);
    for(unsigned i=0;i<4;++i) {
        const auto key=QString("dungeon_map_arrow_%1_").arg(DungeonMap::ArrowNames[i]);
        dungeon_map.arrow_offsets[i]={value((key+"x").toUtf8().constData(),0,-800,800),
            value((key+"y").toUtf8().constData(),0,-480,480)};
    }
    for(unsigned i=0;i<2;++i) {
        const auto key=QString("dungeon_map_panel_%1_").arg(i);const auto d=DungeonMap::Options{}.panels[i];
        dungeon_map.panels[i]={value((key+"x").toUtf8().constData(),d.x,-512,512),
            value((key+"y").toUtf8().constData(),d.y,-480,480),value((key+"scale").toUtf8().constData(),d.scale,.1f,3)};
    }
    for(unsigned i=0;i<DungeonMap::Count;++i) {
        const auto key=QString("dungeon_map_%1_").arg(i);
        auto& p=dungeon_map.poses[i];
        const bool model=i==DungeonMap::Model,description=i==DungeonMap::Description;
        p={value((key+"x").toUtf8().constData(),model||description?0:40,model?-20:description?-320:-512,model?20:description?320:512),
           value((key+"y").toUtf8().constData(),0,model?-20:-240,model?20:240),
           value((key+"scale").toUtf8().constData(),1,model||description?.25f:.1f,model||description?2.f:3.f)};
    }
    banners_migrated=object.value("banners_native").toBool(false);
    for(unsigned i=0;i<2;++i){const auto key=QString("banner_%1_").arg(i);auto& p=banners.parts[i];
        p.height=value((key+"height").toUtf8().constData(),1,.25f,3);
        p.y=value((key+"y").toUtf8().constData(),0,-240,240);
        p.text_scale=value((key+"text_scale").toUtf8().constData(),1,.25f,3);
        p.text_x=value((key+"text_x").toUtf8().constData(),i?200:40,-400,400);
        p.text_y=value((key+"text_y").toUtf8().constData(),0,-240,240);
        p.brightness=value((key+"brightness").toUtf8().constData(),1,0,1);
    }
    const auto read_diamond=[&](const QString& key,HudButtons::Diamond& d){
        d.enabled=object.value(key+"enabled").toBool(false);
        d.x=value((key+"x").toUtf8().constData(),350,-512,512);d.y=value((key+"y").toUtf8().constData(),55,-240,240);
        d.spacing=value((key+"spacing").toUtf8().constData(),24,0,160);d.rotation=value((key+"rotation").toUtf8().constData(),0,-180,180);};
    read_diamond("hud_diamond_",hud_buttons.diamond);
    for(unsigned i=0;i<3;++i)read_diamond(QString("hud_button_layout_%1_diamond_").arg(i+1),hud_buttons.preset_diamonds[i]);
    const auto read_buttons=[&](const QString& prefix,HudButtons::Layout& layout){
        for(unsigned i=0;i<HudButtons::Count;++i){const auto key=prefix+QString::number(i)+"_";
            layout[i]={value((key+"x").toUtf8().constData(),0,-512,512),value((key+"y").toUtf8().constData(),HudButtons::Default()[i].y,-240,240),value((key+"scale").toUtf8().constData(),1,.1f,3)};}};
    read_buttons("hud_button_",hud_buttons.buttons);
    hud_buttons.flip_peak=value("hud_buttons_flip_peak",1.2f,1,1.2f);
    hud_buttons.scale=value("hud_buttons_scale",hud_buttons.buttons[1].scale,.1f,3);
    for(unsigned i=0;i<3;++i){const auto key=QString("hud_button_layout_%1_").arg(i+1);
        hud_buttons.saved[i]=object.value(key+"saved").toBool(false);read_buttons(key,hud_buttons.presets[i]);
        hud_buttons.preset_flip_peaks[i]=value((key+"flip_peak").toUtf8().constData(),1.2f,1,1.2f);
        hud_buttons.preset_scales[i]=value((key+"scale").toUtf8().constData(),hud_buttons.presets[i][1].scale,.1f,3);}
    song_reset.enabled=object.value("song_reset_top").toBool(true);
    song_reset.background_x=value("song_reset_background_x",40,-512,512);
    for(unsigned i=0;i<SongReset::Count;++i){const auto key=QString("song_reset_%1_").arg(i);
        song_reset.poses[i]={value((key+"x").toUtf8().constData(),SongReset::Options{}.poses[i].x,-512,512),value((key+"y").toUtf8().constData(),0,-240,240),value((key+"scale").toUtf8().constData(),1,.1f,3)};}
    double_time.enabled=object.value("double_time_top").toBool(true);
    double_time.current_day_only=object.value("double_time_current_day").toBool(true);
    for(unsigned i=0;i<DoubleTime::Count;++i){
        const auto key=QString("double_time_%1_").arg(i);
        double_time.poses[i]={value((key+"x").toUtf8().constData(),40,-512,512),value((key+"y").toUtf8().constData(),0,-240,240),value((key+"scale").toUtf8().constData(),1,.1f,3)};
    }
    song_list.enabled=object.value("song_list_top").toBool(true);
    song_list.background_x=value("song_list_background_x",-230.f,-512.f,512.f);
    song_list.spacing=value("song_list_spacing",16.f,0.f,60.f);
    song_list.panel_color={value("song_panel_color_r",1.f,0.f,1.f),value("song_panel_color_g",1.f,0.f,1.f),value("song_panel_color_b",1.f,0.f,1.f)};
    song_list.panel_color_opacity=value("song_panel_color_opacity",0.f,0.f,1.f);
    song_list.panel_darkness=value("song_panel_darkness",0.f,0.f,1.f);
    song_list.stone_darkness=value("song_stone_darkness",0.f,0.f,1.f);
    song_list.column_spacing=value("song_list_column_spacing",62.f,40.f,180.f);
    song_list.songs={value("song_list_x",24.f,-512.f,512.f),value("song_list_y",12.f,-240.f,240.f),value("song_list_scale",.35f,.1f,2.f)};
    // Seed independent banner placement from the old grid-derived position once.
    const float song_banner_height=std::clamp(song_list.songs.y-2.f,10.f,18.f);
    song_list.banner={value("song_banner_x",song_list.songs.x,-512.f,512.f),
        value("song_banner_y",song_list.songs.y-song_banner_height-2.f,-240.f,240.f),
        value("song_banner_scale",1.f,.1f,3.f)};
    song_list.banner_width=value("song_banner_width",(2*song_list.column_spacing+54)*song_list.songs.scale,2.f,600.f);
    song_list.banner_height=value("song_banner_height",song_banner_height,1.f,40.f);
    // Preserve the old fitted bounds on migration, then save independent edges.
    song_list.panel_left=value("song_panel_left_x",std::clamp(std::min(song_list.songs.x,song_list.banner.x)-4.f,-512.f,496.f),-512.f,496.f);
    song_list.panel_right=value("song_panel_right_x",std::clamp(std::max(song_list.songs.x+(2*song_list.column_spacing+54)*song_list.songs.scale,
        song_list.banner.x+song_list.banner_width*song_list.banner.scale)+4.f,song_list.panel_left+16.f,512.f),song_list.panel_left+16.f,512.f);
    song_list.staff={value("song_staff_x",90.f,-512.f,512.f),value("song_staff_y",0.f,-240.f,240.f),value("song_staff_scale",.8f,.1f,2.f)};
    ocarina_prompt.enabled=object.value("ocarina_prompts").toBool(true);
    ocarina_prompt.x=value("ocarina_prompt_x",0.f,-400.f,400.f);
    ocarina_prompt.y=value("ocarina_prompt_y",0.f,-240.f,240.f);
    ocarina_prompt.scale=value("ocarina_prompt_scale",1.f,.5f,2.f);
    ocarina_prompt.spacing=value("ocarina_prompt_spacing",22.f,0.f,60.f);
    ocarina_prompt.icon_scale=value("ocarina_icon_scale",1.f,.5f,2.f);
    file_select_prompt.enabled=object.value("file_select_prompts").toBool(true);
    file_select_prompt.x=value("file_select_prompt_x",0.0f,-400.0f,400.0f);
    file_select_prompt.y=value("file_select_prompt_y",0.0f,-240.0f,240.0f);
    file_select_prompt.scale=value("file_select_prompt_scale",1.0f,0.5f,2.0f);
    file_select_prompt.spacing=value("file_select_prompt_spacing",22.0f,0.0f,60.0f);
    file_select_prompt.icon_scale=value("file_select_prompt_icon_scale",1.0f,0.5f,2.0f);
    file_select.stone_spacing=value("file_select_stone_spacing",0.f,0.f,80.f);
    file_select.entrance_ease=value("file_select_entrance_ease",2.f,1.f,3.f);
    file_select.enabled=object.value("file_select_native").toBool(true);
    file_select.header.x=value("file_select_header_x",0.f,-512.f,512.f);
    file_select.header.y=value("file_select_header_y",0.f,-512.f,512.f);
    file_select.header.scale_x=value("file_select_header_scale_x",1.f,.01f,3.f);
    file_select.header.scale_y=value("file_select_header_scale_y",1.f,.01f,3.f);
    file_select.title.x=value("file_select_title_x",0.f,-512.f,512.f);
    file_select.title.y=value("file_select_title_y",0.f,-512.f,512.f);
    file_select.title.scale=value("file_select_title_scale",1.f,.01f,3.f);
    startup.logo.x=value("startup_logo_x",0,-400,400);
    startup.logo.y=value("startup_logo_y",0,-240,240);
    startup.logo.scale=value("startup_logo_scale",1,.1f,3);
    startup.copyright.x=value("startup_copyright_x",0,-400,400);
    startup.copyright.y=value("startup_copyright_y",0,-240,240);
    startup.copyright.scale=value("startup_copyright_scale",1,.1f,3);
    startup.press_start.x=value("startup_press_start_x",0,-400,400);
    startup.press_start.y=value("startup_press_start_y",0,-240,240);
    startup.press_start.scale=value("startup_press_start_scale",1,.1f,3);
    startup.loading_icon.x=value("startup_loading_icon_x",0,-400,400);
    startup.loading_icon.y=value("startup_loading_icon_y",0,-240,240);
    startup.loading_icon.scale=value("startup_loading_icon_scale",1,.1f,3);
    item_present.enabled=object.value("item_present_native").toBool(true);
    item_present.help_pose={value("item_present_help_x",280,-512,512),value("item_present_help_y",180,-512,512),value("item_present_help_scale",.8f,.1f,2)};
    item_present.help_spacing=value("item_present_help_spacing",6,0,100);
    item_present.horizontal=object.value("item_present_horizontal").toBool(false);
    for(unsigned i=0;i<NativeItemPresent::Count;++i) {
        const QString key=QString("item_present_%1_").arg(i);auto& p=item_present.poses[i];
        p.x=value((key+"x").toUtf8().constData(),p.x,-512.f,512.f);
        if(i==NativeItemPresent::Background)continue;
        p.y=value((key+"y").toUtf8().constData(),p.y,-512.f,512.f);
        p.scale=value((key+"scale").toUtf8().constData(),p.scale,.1f,2.f);
    }
    // Seed the independent moon-mask controls from the former shared placement.
    // Preserve the description's previous global dialogue pose on first load.
    mask_present=item_present;
    mask_present.horizontal=false;
    mask_present.poses[NativeItemPresent::Description]={0,value("dialogue_y",0,-120,120),value("dialogue_scale",1,.5f,1)};
    mask_present.enabled=object.value("mask_present_native").toBool(mask_present.enabled);
    mask_present.help_pose={value("mask_present_help_x",mask_present.help_pose.x,-512,512),
        value("mask_present_help_y",mask_present.help_pose.y,-512,512),value("mask_present_help_scale",mask_present.help_pose.scale,.1f,2)};
    mask_present.help_spacing=value("mask_present_help_spacing",mask_present.help_spacing,0,100);
    for(unsigned i=0;i<NativeItemPresent::Count;++i) {
        if(i==NativeItemPresent::Background||i==NativeItemPresent::Right)continue;
        const QString key=QString("mask_present_%1_").arg(i);auto& p=mask_present.poses[i];
        p.x=value((key+"x").toUtf8().constData(),p.x,-512,512);
        p.y=value((key+"y").toUtf8().constData(),p.y,-512,512);
        p.scale=value((key+"scale").toUtf8().constData(),p.scale,.1f,2);
    }
    for(unsigned i=0;i<NativeFileSelect::Count;++i) {
        const QString key=QString("file_select_%1_").arg(i);auto& p=file_select.poses[i];
        p.x=value((key+"x").toUtf8().constData(),p.x,-512.f,512.f);
        p.y=value((key+"y").toUtf8().constData(),p.y,-512.f,512.f);
        p.scale=value((key+"scale").toUtf8().constData(),p.scale,.1f,2.f);
    }
    for(unsigned i=0;i<NativeNotebook::Count;++i) {
        const QString key=QString("notebook_live_%1_").arg(i);auto& p=notebook_live.poses[i];
        p.x=value((key+"x").toUtf8().constData(),p.x,-512.f,512.f);
        p.y=value((key+"y").toUtf8().constData(),p.y,-512.f,512.f);
        p.scale=value((key+"scale").toUtf8().constData(),p.scale,.1f,2.f);
    }
    inventory_menu.gear.enabled=object.value("gear_enabled").toBool(true);
    inventory_menu.gear.horizontal=object.value("gear_horizontal").toBool(true);
    for(unsigned i=0;i<GearMenu::Count;++i) {
        if(i==GearMenu::Background||i==GearMenu::Model||i==GearMenu::Description)continue; // Shared inventory settings.
        const QString key=QString("gear_%1_").arg(i);
        auto& p=inventory_menu.gear.poses[i];
        const auto def=GearMenu::Options{}.poses[i];
        const bool model=i==GearMenu::Model;
        p.x=value((key+"x").toUtf8().constData(),def.x,model?-20.f:-320.f,model?20.f:320.f);
        if(i==GearMenu::Background)continue;
        p.y=value((key+"y").toUtf8().constData(),def.y,model?-20.f:-240.f,model?20.f:240.f);
        p.scale=value((key+"scale").toUtf8().constData(),def.scale,.25f,2.f);
    }
    inventory_menu.masks_y=value("inventory_masks_y",0.f,-240.f,240.f);
    inventory_menu.masks_scale=value("inventory_masks_scale",1.f,.5f,1.5f);
    inventory_menu.high_res_pause=object.value("inventory_high_res_pause").toBool(true);
    inventory_menu.shared_slide=object.value("inventory_shared_slide").toBool(true);
    inventory_menu.bar_enabled=object.value("inventory_bar_enabled").toBool(true);
    inventory_menu.bar_scale=value("inventory_bar_scale",.65f,.35f,1.5f);
    inventory_menu.bar_spacing=value("inventory_bar_spacing",64.f,40.f,120.f);
    inventory_menu.bar_x=value("inventory_bar_x",inventory_menu.x,-320.f,320.f);
    inventory_menu.bar_y=value("inventory_bar_y",inventory_menu.y-64.f*inventory_menu.bar_scale-4.f,-240.f,240.f);
    inventory_menu.description_x=value("inventory_description_x",0.f,-320.f,320.f);
    inventory_menu.description_y=value("inventory_description_y",0.f,-240.f,240.f);
    inventory_menu.description_scale=value("inventory_description_scale",1.f,.25f,2.f);
    inventory_menu.model_x=value("inventory_model_x",0.f,-20.f,20.f);
    inventory_menu.model_y=value("inventory_model_y",0.f,-20.f,20.f);
    inventory_menu.model_scale=value("inventory_model_scale",1.f,.25f,2.f);
    inventory_menu.gear_button.x=value("inventory_button_gear_x",0.f,-400.f,400.f);
    inventory_menu.gear_button.y=value("inventory_button_gear_y",0.f,-240.f,240.f);
    inventory_menu.gear_button.scale=value("inventory_button_gear_scale",1.f,.25f,2.f);
    inventory_menu.help.enabled=object.value("inventory_help_enabled").toBool(true);
    inventory_menu.help.horizontal=object.value("inventory_help_horizontal").toBool(false);
    inventory_menu.help.spacing=value("inventory_help_spacing",6,0,100);
    inventory_menu.help.button_gap=value("inventory_help_button_gap",4,0,40);
    inventory_menu.help.button_scale=value("inventory_help_button_scale",1,.25f,3);
    inventory_menu.help.text_scale=value("inventory_help_text_scale",1,.25f,3);
    inventory_menu.help.opacity=value("inventory_prompt_opacity",1,0,1);
    inventory_menu.help.pose={value("inventory_help_0_x",280,-512,512),value("inventory_help_0_y",148,-240,240),
        value("inventory_help_0_scale",.8f,.1f,2)};
    inventory_menu.map_prompt.x=value("inventory_map_prompt_x",ItemsMapPrompt::DefaultPose.x,-400.f,400.f);
    inventory_menu.map_prompt.y=value("inventory_map_prompt_y",ItemsMapPrompt::DefaultPose.y,-240.f,240.f);
    inventory_menu.map_prompt.scale=value("inventory_map_prompt_scale",ItemsMapPrompt::DefaultPose.scale,.25f,2.f);
    for(unsigned i=0;i<3;++i) {
        const char* names[]={"r","arrow","text"};
        const auto prefix=QString("inventory_map_prompt_%1_").arg(names[i]);
        auto& p=inventory_menu.map_prompt_elements[i];
        p.x=value((prefix+"x").toUtf8().constData(),ItemsMapPrompt::DefaultElements[i].x,-400.f,400.f);
        p.y=value((prefix+"y").toUtf8().constData(),ItemsMapPrompt::DefaultElements[i].y,-240.f,240.f);
        p.scale=value((prefix+"scale").toUtf8().constData(),ItemsMapPrompt::DefaultElements[i].scale,.25f,4.f);
    }
    // Older settings mirrored the inventory prompt; carry that exact layout
    // forward until the map prompt receives its own saved controls.
    map_items_prompt.pose={value("map_items_prompt_x",-inventory_menu.map_prompt.x,-400,400),
        value("map_items_prompt_y",inventory_menu.map_prompt.y,-240,240),
        value("map_items_prompt_scale",inventory_menu.map_prompt.scale,.25f,2.f)};
    for(unsigned i=0;i<3;++i) {
        const char* names[]={"l","arrow","text"};
        const auto prefix=QString("map_items_prompt_%1_").arg(names[i]);
        const auto& old=inventory_menu.map_prompt_elements[i];auto& p=map_items_prompt.elements[i];
        p.x=value((prefix+"x").toUtf8().constData(),-old.x,-400,400);
        p.y=value((prefix+"y").toUtf8().constData(),old.y,-240,240);
        p.scale=value((prefix+"scale").toUtf8().constData(),old.scale,.25f,4.f);
    }
    inventory_menu.buttons_enabled=object.value("inventory_buttons_enabled").toBool(true);
    inventory_menu.shadow_width=value("brown_shadow_width",value("inventory_shadow_width",1.f,0.f,3.f),0.f,3.f);
    inventory_menu.shadow_opacity=value("brown_shadow_opacity",value("inventory_shadow_opacity",1.f,0.f,1.f),0.f,1.f);
    inventory_menu.background_enabled=object.value("inventory_background_enabled").toBool(true);
    inventory_menu.background.x=value("inventory_background_x",0.f,-400.f,400.f);
    inventory_menu.background.y=value("brown_background_y",value("inventory_background_y",0.f,-240.f,240.f),-240.f,240.f);
    inventory_menu.background.scale=value("brown_background_scale",value("inventory_background_scale",1.f,.25f,2.f),.25f,2.f);
    for(unsigned i=0;i<inventory_menu.buttons.size();++i) {
        const char* names[]={"back","masks","items"};
        const auto prefix=QString("inventory_button_%1_").arg(names[i]);
        auto& b=inventory_menu.buttons[i];
        b.x=value((prefix+"x").toUtf8().constData(),0.f,-400.f,400.f);
        b.y=value((prefix+"y").toUtf8().constData(),0.f,-240.f,240.f);
        b.scale=value((prefix+"scale").toUtf8().constData(),1.f,.25f,2.f);
    }
    const int hud_layout_version=object.value("player_hud_layout_version").toInt(0);
    player_hud.layout_version=hud_layout_version>=1&&hud_layout_version<=2?hud_layout_version:0;
    player_hud.mirror_item_buttons=object.value("player_hud_mirror_item_buttons").toBool(false);
    player_hud.opacity=value("player_hud_opacity",1.f,0.f,1.f);
    player_hud.banner_darkness=value("player_hud_banner_darkness",0.f,0.f,1.f);
    player_hud.banner_y=int(value("player_hud_banner_y",0.f,-240.f,240.f));
    player_hud.hide_minimap=object.value("player_hud_hide_minimap").toBool(false);
    for(unsigned i=0;i<6;++i) {
        const auto key=QString("player_hud_button_%1_").arg(HudAdjustments::ButtonKeys[i]);
        player_hud.button_x[i]=int(value((key+"x").toUtf8().constData(),0,-400,400));
        player_hud.button_y[i]=int(value((key+"y").toUtf8().constData(),0,-240,240));
    }
    player_hud.hide_menu_prompts=object.value("player_hud_hide_menu_prompts").toBool(false);
    player_hud.heart_spacing=int(value("player_hud_heart_spacing",0,
        -HudAdjustments::HeartSpacingLimit,HudAdjustments::HeartSpacingLimit));
    player_hud.magic_height_steps=int(value("player_hud_magic_height",0,-10,10));
    player_hud.magic_width=value("player_hud_magic_width",1.f,.5f,3.f);
    // Existing profiles already encode their chosen size in player_hud_scale.
    // Do not apply the new-install baseline on top of it.
    player_hud.base_scale=int(value("player_hud_base_scale",0,-10,10));
    player_hud.overall=int(value("player_hud_scale",0,-10,10));
    player_hud.top=int(value("player_hud_top_padding",0,-HudAdjustments::VerticalPaddingLimit,HudAdjustments::VerticalPaddingLimit));
    player_hud.bottom=int(value("player_hud_bottom_padding",0,-HudAdjustments::VerticalPaddingLimit,HudAdjustments::VerticalPaddingLimit));
    player_hud.left=int(value("player_hud_left_padding",0,-HudAdjustments::SidePaddingLimit,HudAdjustments::SidePaddingLimit));
    player_hud.right=int(value("player_hud_right_padding",0,-HudAdjustments::SidePaddingLimit,HudAdjustments::SidePaddingLimit));
    for(unsigned i=0;i<HudAdjustments::Count;++i) {
        const auto key=QString("player_hud_%1_scale").arg(HudAdjustments::Keys[i]).toUtf8();
        player_hud.elements[i]=int(value(key.constData(),0,-10,10));
        const auto prefix=QString("player_hud_%1_").arg(HudAdjustments::Keys[i]);
        player_hud.x[i]=int(value((prefix+"x").toUtf8().constData(),0,-400,400));
        player_hud.y[i]=int(value((prefix+"y").toUtf8().constData(),0,-240,240));
    }
    // One-time migration of every older audio preference to Balanced. Keep the
    dialogue_scale=value("dialogue_scale",1.f,.5f,1.f);
    dialogue_y=value("dialogue_y",0.f,-120.f,120.f);
    dialogue_background_width=value("dialogue_background_width",1.f,1.f,2.f);
    dialogue_indicator_x=value("dialogue_indicator_x",0.f,-200.f,200.f);
    dialogue_indicator_y=value("dialogue_indicator_y",0.f,-64.f,64.f);
    single_screen_text = object.value("single_screen_text").toBool(true);
    const OptionsMenu::Options options_defaults;
    options_menu.x=value("options_menu_x",options_defaults.x,-400.f,400.f);
    options_menu.y=value("options_menu_y",options_defaults.y,-240.f,240.f);
    options_menu.scale=value("options_menu_scale",options_defaults.scale,.1f,2.f);
    options_menu.spacing=value("options_menu_spacing",options_defaults.spacing,0.f,120.f);
    options_menu.description.x=value("options_description_x",0.f,-320.f,320.f);
    options_menu.description.y=value("options_description_y",0.f,-240.f,240.f);
    options_menu.description.scale=value("options_description_scale",1.f,.25f,2.f);
    carrots.x=value("carrots_x",0.f,-400.f,400.f);
    carrots.y=value("carrots_y",0.f,-240.f,240.f);
    carrots.scale=value("carrots_scale",1.f,.25f,3.f);
    rupee_keys.x=value("rupee_keys_x",0.f,-400.f,400.f);
    rupee_keys.y=value("rupee_keys_y",0.f,-240.f,240.f);
    rupee_keys.scale=value("rupee_keys_scale",1.f,.25f,3.f);
    minigame_hud.x=value("minigame_hud_x",0.f,-400.f,400.f);
    minigame_hud.y=value("minigame_hud_y",0.f,-240.f,240.f);
    minigame_hud.scale=value("minigame_hud_scale",1.f,.25f,3.f);
    magic_border.enabled=object.value("magic_border_enabled").toBool(true);
    magic_border.shadow_enabled=object.value("magic_shadow_enabled").toBool(true);
    magic_border.shadow_size=value("magic_shadow_size",2.5f,0.f,8.f);
    magic_border.shadow_opacity=value("magic_shadow_opacity",.8f,0.f,1.f);
    magic_border.thickness=value("magic_border_thickness",1.f,1.f,3.f);
    magic_border.color={value("magic_border_r",1.f,0.f,1.f),value("magic_border_g",1.f,0.f,1.f),value("magic_border_b",1.f,0.f,1.f)};
    magic_bar_y = value("magic_bar_y", 0.f, -64.f, 64.f);
    magic_bar_height = value("magic_bar_height", 1.f, .5f, 2.f);
    dpad_y = value("dpad_y", 0.f, -120.f, 120.f);
    dpad_offset_x=value("dpad_offset_x",0.f,-400.f,400.f);
    dpad_offset_y=value("dpad_offset_y",0.f,-240.f,240.f);
    minimap = object.value("minimap").toBool(true);
    minimap_scale = value("minimap_scale", .36f, .2f, .6f);
    minimap_opacity = value("minimap_opacity", .85f, 0.f, 1.f);
    minimap_right_margin = value("minimap_right_margin", 8.f, -64.f, 120.f);
    minimap_bottom_margin = value("minimap_bottom_margin", 8.f, -64.f, 120.f);
    return true;
}

// DekUI's ApplyPresentationSettings (src/mm3d/overlay.cpp), single-screen part.
void Apply(const Settings& settings) {
    HudAdjustments::Apply(settings.player_hud);
    PlayerHud::ApplyRupeeKeys(settings.rupee_keys);
    PlayerHud::ApplyCarrots(settings.carrots);
    PlayerHud::ApplyDpadPosition(settings.dpad_offset_x,settings.dpad_offset_y);
    PlayerHud::ApplyMagicHeight(settings.magic_bar_height);
    SingleScreenText::enabled.store(settings.single_screen_text);
    InventoryMenu::Apply(settings.inventory_menu);
    NativeNotebook::Apply(settings.notebook_live);
    NativeFileSelect::Apply(settings.file_select);
    OcarinaPrompt::Apply(settings.ocarina_prompt);
    FileSelectPrompt::Apply(settings.file_select_prompt);
    MagicBorder::Apply(settings.magic_border);
    SongList::Apply(settings.song_list);
    OptionsMenu::Apply(settings.options_menu);
    DoubleTime::Apply(settings.double_time);
    SongReset::Apply(settings.song_reset);
    HudButtons::Apply(settings.hud_buttons);
    MinigameHud::Apply(settings.minigame_hud);
    Banners::Apply(settings.banners);
    DungeonMap::Apply(settings.dungeon_map);
    ItemsMapPrompt::ApplyReturn(settings.map_items_prompt);
    WorldMapPrompt::Apply(settings.world_map_zoom_prompt);
    WorldMapPrompt::ApplyNative(settings.world_map_native_zoom);
    NativeItemPresent::Apply(settings.item_present);
    NativeItemPresent::ApplyMask(settings.mask_present);
    NativeStartup::Apply(settings.startup);
    DialogueScale::scale.store(settings.dialogue_scale);
    DialogueScale::y_offset.store(settings.dialogue_y);
    DialogueScale::background_width.store(settings.dialogue_background_width);
    DialogueScale::indicator_x.store(settings.dialogue_indicator_x);
    DialogueScale::indicator_y.store(settings.dialogue_indicator_y);
    Minimap::enabled.store(settings.minimap);
    Minimap::scale.store(settings.minimap_scale);
    Minimap::opacity.store(settings.minimap_opacity);
    NativeMinimap::right_margin.store(settings.minimap_right_margin);
    NativeMinimap::bottom_margin.store(settings.minimap_bottom_margin);
}

}  // namespace
}  // namespace MM3D

namespace recomp::hooks {

bool dekui_load_presentation(const char* path) {
    std::string text = kDekuiPresentation;
    if (path) {
        if (std::ifstream f{path}) {
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        } else {
            path = nullptr;
        }
    }
    MM3D::QJsonObject object;
    MM3D::Settings settings;
    if (!MM3D::parse(text, object) || !settings.Load(object)) {
        std::fprintf(stderr, "dekui: %s is not a valid presentation.json (version 1), keeping the defaults\n",
                     path ? path : "the built-in");
        return false;
    }
    MM3D::Apply(settings);
    return true;
}

}  // namespace recomp::hooks
