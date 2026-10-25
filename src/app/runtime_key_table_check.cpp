// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The headless key table: one skirmish, every row of --check-key-table
// played through dispatch_event, and the effect of each row compared with
// the file. The observed table is written even when a row does not match.

#include "oa/app/runtime.hpp"

#include "device_state.hpp"
#include "engine_settings_state.hpp"
#include "oa/core/unit.h"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/attack_orders.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/selection.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/hud/whiteboard.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace input = oa::sim::gameplay_input;
namespace orders = oa::sim::match_runtime;

constexpr int32_t kSiteNearest = 6;
constexpr int32_t kSiteFarthest = 40;
constexpr int32_t kCameraIgnore = 2;
constexpr int32_t kCameraJump = 40;
constexpr uint64_t kClockStartNs = 1'000'000'000ULL;
constexpr uint64_t kSettleNs = 33'333'333ULL;
constexpr const char* kScreenshotFolder = "\\screenshots";

/// The watcher rows set this before they install the extension state hook.
/// A function pointer cannot capture the player.
bool row_watcher = false;

enum class GestureKind { key, hold, button, drag, wheel };

struct Gesture {
    GestureKind kind = GestureKind::key;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    int hold_ms = 0;
    uint8_t button = SDL_BUTTON_LEFT;
    std::string place;
    int clicks = 1;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool primary = false;
    bool repeat = false;
    bool wheel_up = true;
    SDL_Scancode held = SDL_SCANCODE_UNKNOWN;
};

struct Prepared {
    std::string select = "none";
    std::string interface_name;
    bool switch_alt = false;
    bool wheel_zoom_off = false;
    std::string armed;
    bool chat = false;
    bool menu = false;
    bool unit_info = false;
    bool shared = false;
    bool watcher = false;
    bool developer = false;
    bool debug_keys = false;
    bool marker = false;
    bool macos = false;
    bool escape_menu = false;
};

struct TableRow {
    std::string id;
    std::string setup;
    std::string input;
    std::string effect;
    Prepared prepared;
    Gesture gesture{};
};

struct Shot {
    int selected = 0;
    std::string primary = "none";
    std::string armed = "none";
    int32_t camera_x = 0;
    int32_t camera_z = 0;
    bool paused = false;
    int speed = 0;
    std::string panel;
    bool unit_info = false;
    bool chat = false;
    int messages = 0;
    int build_page = 0;
    int groups[10]{};
    bool megamap = false;
    int lines = 0;
    int dots = 0;
    int markers = 0;
    bool editing = false;
    bool kill_board = false;
    bool health_bars = false;
    int self_destruct = 0;
    std::map<uint16_t, std::vector<uint8_t>> queues;
    int build_queue = 0;
    std::string facing;
    int screenshots = 0;
    bool full_screen = false;
    bool settings = false;
    bool options_dialog = false;
    bool debug_keys = false;
    std::string console;
    std::string site;
};

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("key table: " + what);
}

bool on_macos() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

SDL_Scancode primary_scancode() {
#ifdef SDL_PLATFORM_MACOS
    return SDL_SCANCODE_LGUI;
#else
    return SDL_SCANCODE_LCTRL;
#endif
}

SDL_Keymod primary_mod() {
#ifdef SDL_PLATFORM_MACOS
    return SDL_KMOD_LGUI;
#else
    return SDL_KMOD_LCTRL;
#endif
}

std::string lower_copy(std::string text) {
    for (char& letter : text)
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    return text;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    return text;
}

std::vector<std::string_view> split(std::string_view text, char mark) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t at = 0; at <= text.size(); ++at) {
        if (at == text.size() || text[at] == mark) {
            parts.push_back(text.substr(start, at - start));
            start = at + 1;
        }
    }
    return parts;
}

SDL_Scancode scancode_named(std::string_view name) {
    static const std::pair<std::string_view, SDL_Scancode> known[] = {
        {"A", SDL_SCANCODE_A},
        {"B", SDL_SCANCODE_B},
        {"C", SDL_SCANCODE_C},
        {"D", SDL_SCANCODE_D},
        {"E", SDL_SCANCODE_E},
        {"F", SDL_SCANCODE_F},
        {"G", SDL_SCANCODE_G},
        {"H", SDL_SCANCODE_H},
        {"I", SDL_SCANCODE_I},
        {"K", SDL_SCANCODE_K},
        {"L", SDL_SCANCODE_L},
        {"M", SDL_SCANCODE_M},
        {"N", SDL_SCANCODE_N},
        {"O", SDL_SCANCODE_O},
        {"P", SDL_SCANCODE_P},
        {"R", SDL_SCANCODE_R},
        {"S", SDL_SCANCODE_S},
        {"T", SDL_SCANCODE_T},
        {"U", SDL_SCANCODE_U},
        {"V", SDL_SCANCODE_V},
        {"W", SDL_SCANCODE_W},
        {"X", SDL_SCANCODE_X},
        {"Y", SDL_SCANCODE_Y},
        {"Z", SDL_SCANCODE_Z},
        {"1", SDL_SCANCODE_1},
        {"2", SDL_SCANCODE_2},
        {"3", SDL_SCANCODE_3},
        {"4", SDL_SCANCODE_4},
        {"5", SDL_SCANCODE_5},
        {"6", SDL_SCANCODE_6},
        {"7", SDL_SCANCODE_7},
        {"8", SDL_SCANCODE_8},
        {"9", SDL_SCANCODE_9},
        {"Return", SDL_SCANCODE_RETURN},
        {"Escape", SDL_SCANCODE_ESCAPE},
        {"Left", SDL_SCANCODE_LEFT},
        {"Right", SDL_SCANCODE_RIGHT},
        {"Up", SDL_SCANCODE_UP},
        {"Down", SDL_SCANCODE_DOWN},
        {"Home", SDL_SCANCODE_HOME},
        {"Pause", SDL_SCANCODE_PAUSE},
        {"Equals", SDL_SCANCODE_EQUALS},
        {"Minus", SDL_SCANCODE_MINUS},
        {"Period", SDL_SCANCODE_PERIOD},
        {"Comma", SDL_SCANCODE_COMMA},
        {"Grave", SDL_SCANCODE_GRAVE},
        {"Backslash", SDL_SCANCODE_BACKSLASH},
        {"Tab", SDL_SCANCODE_TAB},
        {"Space", SDL_SCANCODE_SPACE},
        {"F1", SDL_SCANCODE_F1},
        {"F2", SDL_SCANCODE_F2},
        {"F3", SDL_SCANCODE_F3},
        {"F4", SDL_SCANCODE_F4},
        {"F9", SDL_SCANCODE_F9},
        {"F10", SDL_SCANCODE_F10},
        {"F11", SDL_SCANCODE_F11},
        {"F12", SDL_SCANCODE_F12},
        {"KP_1", SDL_SCANCODE_KP_1},
        {"KP_PLUS", SDL_SCANCODE_KP_PLUS},
        {"KP_MINUS", SDL_SCANCODE_KP_MINUS},
        {"PageUp", SDL_SCANCODE_PAGEUP},
        {"PageDown", SDL_SCANCODE_PAGEDOWN},
        {"PrintScreen", SDL_SCANCODE_PRINTSCREEN},
        {"Insert", SDL_SCANCODE_INSERT},
        {"RightBracket", SDL_SCANCODE_RIGHTBRACKET},
        {"Slash", SDL_SCANCODE_SLASH},
    };
    for (const auto& entry : known)
        if (entry.first == name)
            return entry.second;
    const std::string text(name);
    const auto from_name = SDL_GetScancodeFromName(text.c_str());
    if (from_name != SDL_SCANCODE_UNKNOWN)
        return from_name;
    fail("unknown scancode " + text);
}

bool known_menu(std::string_view stem) {
    static const std::string_view stems[] = {
        "armopt",
        "exitmenu",
        "prefs",
        "restart",
        "yesorno",
        "tabmenu",
        "allies",
        "share",
        "savegame",
        "loadgame",
        "quit"
    };
    for (const auto stem_name : stems)
        if (stem_name == stem)
            return true;
    return false;
}

std::string panel_stem(std::string_view path) {
    const auto slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    const auto dot = path.find_last_of('.');
    if (dot != std::string_view::npos)
        path = path.substr(0, dot);
    return lower_copy(std::string(path));
}

std::string kind_word(uint8_t kind) {
    namespace ground = oa::sim::ground_orders;
    if (kind == orders::stop_kind)
        return "stop";
    if (kind == orders::self_destruct_kind || kind == orders::self_destruct_fg_kind)
        return "self-destruct";
    if (kind == orders::cloak_on_kind)
        return "cloak-on";
    if (kind == orders::cloak_off_kind)
        return "cloak-off";
    if (kind == orders::activate_kind)
        return "activate";
    if (kind == orders::deactivate_kind)
        return "deactivate";
    if (kind == orders::patrol_kind)
        return "patrol";
    if (kind == orders::capture_kind)
        return "capture";
    if (kind == orders::reclaim_kind)
        return "reclaim";
    if (kind == orders::reclaim_unit_kind)
        return "reclaim-unit";
    if (kind == orders::building_build_kind)
        return "build";
    if (kind == orders::help_build_kind)
        return "help-build";
    if (kind == orders::mobile_build_kind)
        return "mobile-build";
    if (kind == orders::ground_unload_kind)
        return "unload";
    if (kind == ground::move_ground_kind)
        return "move";
    if (kind == orders::qmove_kind)
        return "qmove";
    if (kind == orders::qpatrol_kind)
        return "qpatrol";
    if (kind == orders::air_strike_kind)
        return "air-strike";
    if (kind == orders::air_to_air_kind)
        return "air-to-air";
    if (kind == orders::air_to_ground_kind)
        return "air-to-ground";
    if (kind == orders::attack_chase_kind)
        return "attack";
    if (kind == orders::repair_unit_kind)
        return "repair";
    if (kind == orders::follow_ground_kind)
        return "guard";
    if (kind == orders::standing_fire_order_kind)
        return "standing-fire";
    if (kind == orders::standing_move_order_kind)
        return "standing-move";
    return "kind-" + std::to_string(kind);
}

std::string command_word(MatchCommand command) {
    switch (command) {
    case MatchCommand::none:
        return "none";
    case MatchCommand::move:
        return "move";
    case MatchCommand::attack:
        return "attack";
    case MatchCommand::dgun:
        return "dgun";
    case MatchCommand::build:
        return "build";
    case MatchCommand::patrol:
        return "patrol";
    case MatchCommand::repair:
        return "repair";
    case MatchCommand::reclaim:
        return "reclaim";
    case MatchCommand::capture:
        return "capture";
    case MatchCommand::load:
        return "load";
    case MatchCommand::unload:
        return "unload";
    case MatchCommand::guard:
        return "guard";
    }
    return "none";
}

std::string facing_word(view_rules::BuildFacing facing) {
    switch (facing) {
    case view_rules::BuildFacing::south:
        return "south";
    case view_rules::BuildFacing::east:
        return "east";
    case view_rules::BuildFacing::north:
        return "north";
    case view_rules::BuildFacing::west:
        return "west";
    }
    return "south";
}

bool safe_console(std::string_view line) {
    if (line.empty())
        return false;
    for (unsigned char letter : line) {
        if (std::isalnum(letter) == 0 && letter != ' ' && letter != '.' && letter != '_' &&
            letter != '-')
            return false;
    }
    for (std::size_t at = 0; at + 4 < line.size(); ++at) {
        const bool arm = line[at] == 'A' && line[at + 1] == 'R' && line[at + 2] == 'M';
        const bool core = line[at] == 'C' && line[at + 1] == 'O' && line[at + 2] == 'R';
        if (!arm && !core)
            continue;
        if (std::isupper(static_cast<unsigned char>(line[at + 3])) != 0 &&
            std::isupper(static_cast<unsigned char>(line[at + 4])) != 0)
            return false;
    }
    return true;
}

void take_flag(Gesture& gesture, std::string_view flag) {
    if (flag == "ctrl")
        gesture.ctrl = true;
    else if (flag == "alt")
        gesture.alt = true;
    else if (flag == "shift")
        gesture.shift = true;
    else if (flag == "primary")
        gesture.primary = true;
    else if (flag == "repeat")
        gesture.repeat = true;
    else if (flag.rfind("held=", 0) == 0)
        gesture.held = scancode_named(flag.substr(5));
    else if (flag.rfind("clicks=", 0) == 0)
        gesture.clicks = std::stoi(std::string(flag.substr(7)));
    else
        fail("unknown input word " + std::string(flag));
}

uint8_t button_named(std::string_view name) {
    if (name == "left")
        return SDL_BUTTON_LEFT;
    if (name == "right")
        return SDL_BUTTON_RIGHT;
    if (name == "middle")
        return SDL_BUTTON_MIDDLE;
    fail("unknown button " + std::string(name));
}

Gesture parse_gesture(std::string_view text) {
    const auto parts = split(trim(text), ' ');
    std::vector<std::string_view> words;
    for (const auto part : parts)
        if (!part.empty())
            words.push_back(part);
    if (words.empty())
        fail("a row has no input");
    Gesture gesture;
    const auto word = words[0];
    if (word == "key" || word == "hold") {
        if (words.size() < 2)
            fail("a key row needs a scancode");
        gesture.kind = word == "hold" ? GestureKind::hold : GestureKind::key;
        gesture.scancode = scancode_named(words[1]);
        std::size_t from = 2;
        if (gesture.kind == GestureKind::hold) {
            if (words.size() < 3)
                fail("a hold row needs a duration");
            gesture.hold_ms = std::stoi(std::string(words[2]));
            from = 3;
        }
        for (std::size_t at = from; at < words.size(); ++at)
            take_flag(gesture, words[at]);
        return gesture;
    }
    if (word == "button" || word == "drag") {
        if (words.size() < 3)
            fail("a pointer row needs a button and a place");
        gesture.kind = word == "drag" ? GestureKind::drag : GestureKind::button;
        gesture.button = button_named(words[1]);
        gesture.place = std::string(words[2]);
        for (std::size_t at = 3; at < words.size(); ++at)
            take_flag(gesture, words[at]);
        return gesture;
    }
    if (word == "wheel") {
        if (words.size() < 3)
            fail("a wheel row needs a direction and a place");
        gesture.kind = GestureKind::wheel;
        if (words[1] == "up")
            gesture.wheel_up = true;
        else if (words[1] == "down")
            gesture.wheel_up = false;
        else
            fail("a wheel is up or down");
        gesture.place = std::string(words[2]);
        for (std::size_t at = 3; at < words.size(); ++at)
            take_flag(gesture, words[at]);
        return gesture;
    }
    fail("unknown input " + std::string(word));
}

Prepared parse_setup(std::string_view text) {
    Prepared prepared;
    if (trim(text).empty())
        return prepared;
    for (const auto part : split(text, ';')) {
        const auto word = trim(part);
        if (word.empty())
            continue;
        const auto mark = word.find('=');
        const auto key = mark == std::string_view::npos ? word : word.substr(0, mark);
        const auto value =
            mark == std::string_view::npos ? std::string_view{} : word.substr(mark + 1);
        if (key == "select")
            prepared.select = std::string(value);
        else if (key == "interface")
            prepared.interface_name = std::string(value);
        else if (key == "switch-alt" && value == "on")
            prepared.switch_alt = true;
        else if (key == "wheel-zoom" && value == "off")
            prepared.wheel_zoom_off = true;
        else if (key == "armed")
            prepared.armed = std::string(value);
        else if (key == "chat" && value == "open")
            prepared.chat = true;
        else if (key == "menu" && value == "open")
            prepared.menu = true;
        else if (key == "unit-info" && value == "open")
            prepared.unit_info = true;
        else if (key == "marker" && value == "received")
            prepared.marker = true;
        else if (key == "platform" && value == "macos")
            prepared.macos = true;
        else if (key == "shared" && value.empty())
            prepared.shared = true;
        else if (key == "watcher" && value.empty())
            prepared.watcher = true;
        else if (key == "developer" && value.empty())
            prepared.developer = true;
        else if (key == "debug-keys" && value.empty())
            prepared.debug_keys = true;
        else if (key == "escape-menu" && value.empty())
            prepared.escape_menu = true;
        else
            fail("unknown setup word " + std::string(word));
    }
    return prepared;
}

std::vector<TableRow> read_table(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        fail("cannot read " + path);
    std::vector<TableRow> rows;
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (trim(line).empty())
            continue;
        const auto columns = split(line, '\t');
        if (!header) {
            if (columns.size() != 4 || columns[0] != "row" || columns[1] != "setup" ||
                columns[2] != "input" || columns[3] != "effect")
                fail("the header must be row, setup, input, effect");
            header = true;
            continue;
        }
        if (columns.size() != 4)
            fail("a row needs four columns: " + line);
        TableRow row;
        row.id = std::string(columns[0]);
        row.setup = std::string(columns[1]);
        row.input = std::string(columns[2]);
        row.effect = std::string(columns[3]);
        if (row.id.empty() || row.effect.empty())
            fail("a row needs an id and an effect");
        for (const auto& earlier : rows)
            if (earlier.id == row.id)
                fail("duplicate row " + row.id);
        row.prepared = parse_setup(row.setup);
        row.gesture = parse_gesture(row.input);
        if (row.id.rfind("pointer.", 0) == 0 && row.prepared.interface_name.empty())
            fail(row.id + " does not name its interface");
        rows.push_back(std::move(row));
    }
    if (!header)
        fail("the file has no header");
    return rows;
}

std::string camera_word(int32_t dx, int32_t dz) {
    const int32_t ax = dx < 0 ? -dx : dx;
    const int32_t az = dz < 0 ? -dz : dz;
    if (ax <= kCameraIgnore && az <= kCameraIgnore)
        return {};
    if (ax > kCameraJump && az > kCameraJump)
        return "centred";
    if (ax > kCameraIgnore && ax >= az * 2)
        return dx < 0 ? "left" : "right";
    if (az > kCameraIgnore && az >= ax * 2)
        return dz < 0 ? "up" : "down";
    return "moved";
}

std::string effect_of(const Shot& before, const Shot& after) {
    std::map<std::string, std::string> tokens;
    const auto put = [&](const std::string& name, const std::string& value) {
        tokens.emplace(name, value);
    };
    if (before.selected != after.selected)
        put("selected", std::to_string(after.selected));
    if (before.primary != after.primary)
        put("primary", after.primary);
    if (before.armed != after.armed)
        put("armed", after.armed);
    if (const auto moved =
            camera_word(after.camera_x - before.camera_x, after.camera_z - before.camera_z);
        !moved.empty())
        put("camera", moved);
    if (before.paused != after.paused)
        put("paused", after.paused ? "on" : "off");
    if (before.speed != after.speed)
        put("speed", after.speed > before.speed ? "+1" : "-1");
    if (before.panel != after.panel) {
        if (known_menu(after.panel))
            put("panel", after.panel);
        else if (known_menu(before.panel))
            put("panel", "closed");
    }
    if (before.unit_info != after.unit_info)
        put("unit-info", after.unit_info ? "open" : "closed");
    if (before.chat != after.chat) {
        if (!before.chat && after.chat)
            put("chat", "open");
        else if (before.chat && !after.chat)
            put("chat", after.messages > before.messages ? "sent" : "closed");
    }
    if (before.messages != after.messages) {
        if (before.messages > 0 && after.messages == 0)
            put("messages", "cleared");
        else
            put("messages", std::to_string(after.messages));
    }
    if (before.build_page != after.build_page)
        put("build-page", std::to_string(after.build_page));
    for (int squad = 1; squad <= 9; ++squad)
        if (before.groups[squad] != after.groups[squad])
            put("group-" + std::to_string(squad), std::to_string(after.groups[squad]));
    if (before.megamap != after.megamap)
        put("megamap", after.megamap ? "open" : "closed");
    if (!before.editing && after.editing)
        put("whiteboard", "editing");
    else if (after.lines > before.lines)
        put("whiteboard", "lines");
    else if (after.dots > before.dots)
        put("whiteboard", "dots");
    else if (after.lines < before.lines || after.markers < before.markers)
        put("whiteboard", "wiped");
    if (before.kill_board != after.kill_board)
        put("kill-board", after.kill_board ? "on" : "off");
    if (before.health_bars != after.health_bars)
        put("health-bars", after.health_bars ? "on" : "off");
    if (before.self_destruct != after.self_destruct)
        put("self-destruct", std::to_string(after.self_destruct));
    if (before.build_queue != after.build_queue)
        put("build-queue", std::to_string(after.build_queue));
    if (!after.facing.empty() && before.facing != after.facing)
        put("facing", after.facing);
    if (!after.site.empty())
        put("site", after.site);
    if (after.screenshots > before.screenshots)
        put("screenshot", "written");
    if (before.full_screen != after.full_screen)
        put("full-screen", after.full_screen ? "on" : "off");
    if (before.settings != after.settings)
        put("settings", after.settings ? "open" : "closed");
    if (before.options_dialog != after.options_dialog)
        put("options-dialog", after.options_dialog ? "open" : "closed");
    if (before.debug_keys != after.debug_keys)
        put("debug-keys", after.debug_keys ? "on" : "off");
    if (before.console != after.console && safe_console(after.console))
        put("console", after.console);
    std::string order;
    const auto changed = [&](uint16_t id) {
        const auto before_queue = before.queues.find(id);
        const auto after_queue = after.queues.find(id);
        const std::vector<uint8_t> empty;
        const auto& earlier = before_queue == before.queues.end() ? empty : before_queue->second;
        const auto& later = after_queue == after.queues.end() ? empty : after_queue->second;
        return later.size() >= earlier.size() && later != earlier && !later.empty();
    };
    const auto word_for = [&](uint16_t id, const std::string& role) {
        const auto found = after.queues.find(id);
        if (found == after.queues.end() || found->second.empty())
            return;
        order = kind_word(found->second.back()) + ":" + role;
    };
    if (order.empty()) {
        for (const auto& entry : after.queues) {
            if (!changed(entry.first))
                continue;
            // The role is filled by the caller via the queue key's unit. The
            // effect builder below replaces this pass.
            (void)entry;
        }
    }
    (void)word_for;
    (void)changed;
    // Order tokens are added by the player, which knows each unit's role.
    // This function receives them already stored in after.site's sibling by
    // leaving queues for the player to describe. See describe_orders.
    if (tokens.empty())
        return "none";
    std::string joined;
    for (const auto& token : tokens) {
        if (!joined.empty())
            joined += ';';
        joined += token.first + "=" + token.second;
    }
    return joined;
}

} // namespace

void Runtime::check_key_table() {
    // A row can save a toggle (the health bars write damagebars). Put the
    // preferences back before leaving, so the next run starts from the same
    // file. The map is what the following flush writes.
    const auto saved_preferences = preference_values_;
    const auto put_preferences_back = [&] {
        preference_values_ = saved_preferences;
        preferences_dirty_ = true;
        flush_preferences();
    };

    struct Player {
        Runtime& self;
        bool running = true;
        uint16_t commander = 0;
        uint16_t builder = 0;
        uint16_t factory = 0;
        uint16_t aircraft = 0;
        uint16_t combat = 0;
        uint16_t combat_two = 0;
        int32_t origin_x = 0;
        int32_t origin_z = 0;
        float origin_zoom = 0;
        float origin_zoom_target = 0;
        uint16_t origin_speed = 0;
        uint16_t origin_graphics = 0;
        uint16_t origin_console = 0;
        uint16_t origin_outcome = 0;
        uint16_t origin_sim = 0;
        uint16_t origin_frame = 0;
        int32_t origin_interface = 0;
        bool origin_wheel = true;
        bool origin_unlocked = false;
        bool origin_escape_menu = false;
        bool origin_full_screen = false;
        int origin_width = 0;
        int origin_height = 0;
        Extension origin_extension{};
        std::vector<std::pair<uint16_t, uint32_t>> origin_unit_flags;
        std::string site_word;

        explicit Player(Runtime& runtime) : self(runtime) {}

        [[nodiscard]] SDL_WindowID window_id() const { return SDL_GetWindowID(self.sdl_.window); }

        void require_running() const {
            if (!running)
                fail("a key ended the run");
        }

        void prime_clock() {
            self.frame_run_clock_ns_ = kClockStartNs;
            self.frame_time_ns_ = kClockStartNs;
            self.scroll_clock_ = kClockStartNs;
            self.frame_run_scroll_ = 0;
        }

        void settle() {
            self.frame_run_clock_ns_ = *self.frame_run_clock_ns_ + kSettleNs;
            self.run_frame(running);
            require_running();
            self.scroll_clock_ = self.frame_time_ns_;
        }

        void dispatch(SDL_Event& event) {
            self.dispatch_event(event, running);
            require_running();
        }

        void release_keys() {
            for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
                device_state::hold_key(static_cast<SDL_Scancode>(code), false);
            SDL_SetModState(SDL_KMOD_NONE);
            self.shift_held_by_check_ = false;
            self.space_held_by_check_ = false;
        }

        SDL_Keymod mods_of(const Gesture& gesture) const {
            auto mods = SDL_KMOD_NONE;
            if (gesture.ctrl)
                mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_LCTRL);
            if (gesture.alt)
                mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_LALT);
            if (gesture.shift)
                mods = static_cast<SDL_Keymod>(mods | SDL_KMOD_LSHIFT);
            if (gesture.primary)
                mods = static_cast<SDL_Keymod>(mods | primary_mod());
            return mods;
        }

        void hold_mods(const Gesture& gesture, bool down) {
            if (gesture.ctrl)
                device_state::hold_key(SDL_SCANCODE_LCTRL, down);
            if (gesture.alt)
                device_state::hold_key(SDL_SCANCODE_LALT, down);
            if (gesture.shift)
                device_state::hold_key(SDL_SCANCODE_LSHIFT, down);
            if (gesture.primary)
                device_state::hold_key(primary_scancode(), down);
            if (gesture.held != SDL_SCANCODE_UNKNOWN)
                device_state::hold_key(gesture.held, down);
            self.shift_held_by_check_ = down && gesture.shift;
            SDL_SetModState(down ? mods_of(gesture) : SDL_KMOD_NONE);
        }

        void tap(SDL_Scancode scancode, SDL_Keymod mods, bool repeat) {
            SDL_Event event{};
            event.key.windowID = window_id();
            event.key.scancode = scancode;
            // true: the key code a real key event would carry. Shift stays in
            // the modifier word, so Ctrl+Shift+C is still C.
            event.key.key = SDL_GetKeyFromScancode(scancode, mods, true);
            event.key.mod = mods;
            event.key.repeat = repeat;
            for (const bool down : {true, false}) {
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.down = down;
                event.key.repeat = down && repeat;
                dispatch(event);
            }
        }

        void move_window(float x, float y) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = window_id();
            event.motion.x = x;
            event.motion.y = y;
            dispatch(event);
        }

        void move_canvas(float x, float y) {
            float window_x = 0;
            float window_y = 0;
            if (!frame_to_window(self.sdl_.renderer, x, y, &window_x, &window_y))
                fail(SDL_GetError());
            move_window(window_x, window_y);
        }

        void pointer_button(uint8_t button, float x, float y, bool down, int clicks) {
            float window_x = 0;
            float window_y = 0;
            if (!frame_to_window(self.sdl_.renderer, x, y, &window_x, &window_y))
                fail(SDL_GetError());
            SDL_Event event{};
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.windowID = window_id();
            event.button.button = button;
            event.button.down = down;
            event.button.clicks = static_cast<Uint8>(clicks);
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch(event);
        }

        void wheel_at(float x, float y, bool up) {
            float window_x = 0;
            float window_y = 0;
            if (!frame_to_window(self.sdl_.renderer, x, y, &window_x, &window_y))
                fail(SDL_GetError());
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_WHEEL;
            event.wheel.windowID = window_id();
            event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            event.wheel.y = up ? 1.0F : -1.0F;
            event.wheel.mouse_x = window_x;
            event.wheel.mouse_y = window_y;
            dispatch(event);
        }

        void recenter() {
            int width = 0;
            int height = 0;
            if (!SDL_GetWindowSize(self.sdl_.window, &width, &height))
                fail(SDL_GetError());
            move_window(static_cast<float>(width) / 2.0F, static_cast<float>(height) / 2.0F);
        }

        [[nodiscard]] std::pair<float, float> battlefield_centre() const {
            return {
                static_cast<float>(self.match_layout_.left) +
                    static_cast<float>(self.match_layout_.battlefield_width()) * 0.5F,
                static_cast<float>(self.match_layout_.top) +
                    static_cast<float>(self.match_layout_.battlefield_height()) * 0.5F
            };
        }

        void clamp_battlefield(float& x, float& y) const {
            const float left = static_cast<float>(self.match_layout_.left) + 8.0F;
            const float top = static_cast<float>(self.match_layout_.top) + 8.0F;
            const float right = static_cast<float>(self.match_layout_.left) +
                                static_cast<float>(self.match_layout_.battlefield_width()) - 9.0F;
            const float bottom = static_cast<float>(self.match_layout_.top) +
                                 static_cast<float>(self.match_layout_.battlefield_height()) - 9.0F;
            x = std::clamp(x, left, std::max(left, right));
            y = std::clamp(y, top, std::max(top, bottom));
        }

        [[nodiscard]] std::pair<float, float> unit_point(uint16_t id) const {
            const auto viewport = self.live_viewport(self.match_camera_x_, self.match_camera_z_);
            const auto point =
                self.project_match_point(viewport, self.match_->world().slots[id].unit->position);
            return {static_cast<float>(point.x), static_cast<float>(point.y)};
        }

        [[nodiscard]] std::pair<float, float> open_ground() const {
            uint16_t id = commander;
            if (combat != 0)
                id = combat;
            const auto at = unit_point(id);
            float x = at.first + 120.0F;
            float y = at.second + 40.0F;
            clamp_battlefield(x, y);
            return {x, y};
        }

        [[nodiscard]] std::string role_of(uint16_t id) const {
            if (id == 0)
                return "none";
            if (id == commander)
                return "commander";
            if (id == builder)
                return "builder";
            if (id == factory)
                return "factory";
            if (id == aircraft)
                return "aircraft";
            if (id == combat || id == combat_two)
                return "combat";
            return "none";
        }

        uint16_t id_of(std::string_view role) const {
            if (role == "commander")
                return commander;
            if (role == "builder")
                return builder;
            if (role == "factory")
                return factory;
            if (role == "aircraft")
                return aircraft;
            if (role == "combat")
                return combat;
            return 0;
        }

        void select_role(std::string_view role) {
            if (role == "none" || role.empty()) {
                self.clear_local_selection();
                return;
            }
            if (role == "all") {
                self.select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
                    const auto id = slot.unit_index;
                    return id == commander || id == builder || id == factory || id == aircraft ||
                           id == combat || id == combat_two;
                });
                return;
            }
            const auto id = id_of(role);
            if (id == 0)
                fail(std::string(role) + " was not spawned");
            self.select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
                return slot.unit_index == id;
            });
        }

        void set_interface(std::string_view name) {
            auto& world = self.match_->state();
            if (name.empty() || name == "right-click")
                world.game.interface_type = input::interface_right_click;
            else if (name == "left-click")
                world.game.interface_type = input::interface_left_click;
            else
                fail("unknown interface " + std::string(name));
        }

        void set_switch_alt(bool on) {
            auto& flags = self.match_->state().game.graphics_flags;
            if (on)
                flags = static_cast<uint16_t>(flags | oa::ui::console::graphics_flag::switch_alt);
            else
                flags = static_cast<uint16_t>(flags & ~oa::ui::console::graphics_flag::switch_alt);
        }

        void set_wheel_zoom(bool on) {
            self.engine_settings_state().current.wheel_zoom = on;
            self.megamap_wheel_zoom_changed();
        }

        void set_flag_bits(bool developer, bool debug_keys) {
            auto& game = self.match_->state().game;
            if (developer)
                game.console_flags = static_cast<uint16_t>(
                    game.console_flags | oa::ui::console::console_flag::developer
                );
            else
                game.console_flags = static_cast<uint16_t>(
                    game.console_flags & ~oa::ui::console::console_flag::developer
                );
            if (debug_keys)
                game.outcome_flags = static_cast<uint16_t>(
                    game.outcome_flags | oa::ui::console::outcome_flag::debug_keys
                );
            else
                game.outcome_flags = static_cast<uint16_t>(
                    game.outcome_flags & ~oa::ui::console::outcome_flag::debug_keys
                );
        }

        uint16_t spawn_named(std::string_view name, int32_t dx, int32_t dz) {
            const auto type = oa::sim::unit_spawn::find_type_index(self.spawn_type_names_, name);
            if (type == 0)
                fail("lacks " + std::string(name));
            auto& slots = self.match_->world().slots;
            const auto x = static_cast<int32_t>(slots[commander].unit->position[0] >> 16) + dx;
            const auto z = static_cast<int32_t>(slots[commander].unit->position[2] >> 16) + dz;
            oa::sim::unit_spawn::Request request;
            request.player = self.match_local_player_;
            request.type = type;
            request.finished = true;
            request.state = kGroundOccupancyState;
            request.position = {
                static_cast<uint32_t>(x) << 16,
                static_cast<uint32_t>(self.match_->map_height(
                    static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
                )) << 16,
                static_cast<uint32_t>(z) << 16
            };
            auto* slot = self.match_->create(request);
            if (slot == nullptr || slot->unit == nullptr)
                fail("could not spawn " + std::string(name));
            slot->unit->object_present = true;
            slot->unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
            return slot->unit_index;
        }

        void spawn_lab() {
            const auto type =
                oa::sim::unit_spawn::find_type_index(self.spawn_type_names_, "ARMLAB");
            if (type == 0)
                fail("lacks ARMLAB");
            auto& slots = self.match_->world().slots;
            const auto cell_x = static_cast<int32_t>(slots[commander].unit->position[0] >> 20);
            const auto cell_z = static_cast<int32_t>(slots[commander].unit->position[2] >> 20);
            std::optional<std::pair<int32_t, int32_t>> site;
            for (int32_t ring = kSiteNearest; ring < kSiteFarthest && !site; ++ring)
                for (int32_t dz = -ring; dz <= ring && !site; dz += 2)
                    for (int32_t dx = -ring; dx <= ring && !site; dx += 2)
                        if ((dx == -ring || dx == ring || dz == -ring || dz == ring) &&
                            self.match_->building_site(type, cell_x + dx, cell_z + dz, 0))
                            site = std::pair{cell_x + dx, cell_z + dz};
            if (!site)
                fail("found no site for the lab");
            const auto& lab_type = self.spawn_types_[type];
            const auto origin = static_cast<int32_t>(slots[commander].unit->position[0] >> 16);
            const auto origin_depth =
                static_cast<int32_t>(slots[commander].unit->position[2] >> 16);
            factory = spawn_named(
                "ARMLAB",
                (site->first * 2 + lab_type.footprint_x) * 8 - origin,
                (site->second * 2 + lab_type.footprint_z) * 8 - origin_depth
            );
        }

        void spawn_all() {
            for (const auto& slot : self.match_->world().slots)
                if (slot.unit != nullptr && slot.record.type_index != 0 &&
                    slot.record.owner_index == self.match_local_player_) {
                    commander = slot.unit_index;
                    break;
                }
            if (commander == 0)
                fail("found no local commander");
            self.match_->world().slots[commander].unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
            builder = spawn_named("ARMCK", 96, 0);
            combat = spawn_named("ARMPW", 96, 64);
            combat_two = spawn_named("ARMPW", 96, -64);
            const char* flyer = nullptr;
            for (const char* name : {"ARMPEEP", "ARMFIG", "ARMCA"}) {
                if (oa::sim::unit_spawn::find_type_index(self.spawn_type_names_, name) != 0) {
                    flyer = name;
                    break;
                }
            }
            if (flyer == nullptr)
                fail("lacks an aircraft");
            aircraft = spawn_named(flyer, 160, 32);
            spawn_lab();
            const auto* builder_def = self.definition_for(builder);
            const auto* lab_def = self.definition_for(factory);
            const auto* flyer_def = self.definition_for(aircraft);
            if (builder_def == nullptr || !builder_def->builder || builder_def->bm_code == 0)
                fail("the construction kbot is not a mobile builder");
            if (lab_def == nullptr || !lab_def->builder || lab_def->bm_code != 0)
                fail("the lab is not a factory");
            if (flyer_def == nullptr || !flyer_def->can_fly)
                fail("the aircraft cannot fly");
        }

        void capture_origin() {
            origin_x = self.match_camera_x_;
            origin_z = self.match_camera_z_;
            origin_zoom = self.match_zoom_;
            origin_zoom_target = self.match_zoom_target_;
            auto& game = self.match_->state().game;
            origin_speed = game.requested_speed;
            origin_graphics = game.graphics_flags;
            origin_console = game.console_flags;
            origin_outcome = game.outcome_flags;
            origin_sim = game.sim_run_flags;
            origin_frame = game.frame_flags;
            origin_interface = game.interface_type;
            origin_wheel = self.engine_settings_state().current.wheel_zoom;
            origin_unlocked = self.engine_settings_state().current.unlocked_camera;
            origin_escape_menu = self.engine_settings_state().current.escape_opens_menu;
            origin_extension = self.extension_;
            origin_full_screen =
                (SDL_GetWindowFlags(self.sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (!SDL_GetWindowSize(self.sdl_.window, &origin_width, &origin_height))
                fail(SDL_GetError());
            origin_unit_flags.clear();
            for (const auto id : {commander, builder, factory, aircraft, combat, combat_two}) {
                if (id == 0 || self.match_->world().slots[id].unit == nullptr)
                    continue;
                origin_unit_flags.emplace_back(id, self.match_->world().slots[id].unit->flags);
            }
        }

        void restore_words() {
            auto& game = self.match_->state().game;
            game.requested_speed = origin_speed;
            game.graphics_flags = origin_graphics;
            game.console_flags = origin_console;
            game.outcome_flags = origin_outcome;
            game.sim_run_flags = origin_sim;
            game.frame_flags = origin_frame;
            game.interface_type = origin_interface;
            self.engine_settings_state().current.escape_opens_menu = origin_escape_menu;
            self.extension_ = origin_extension;
            set_wheel_zoom(origin_wheel);
            if (self.engine_settings_state().current.unlocked_camera != origin_unlocked) {
                auto chosen = self.engine_settings();
                chosen.unlocked_camera = origin_unlocked;
                self.apply_engine_settings(chosen);
            }
            self.match_camera_x_ = origin_x;
            self.match_camera_z_ = origin_z;
            self.match_zoom_ = origin_zoom;
            self.match_zoom_target_ = origin_zoom_target;
            self.match_build_page_ = 0;
        }

        void clear_queues() {
            for (const auto id : {commander, builder, factory, aircraft, combat, combat_two}) {
                if (id == 0)
                    continue;
                for (int pass = 0; pass < 8; ++pass) {
                    std::vector<uint8_t> kinds;
                    self.match_->visit_primary_queue(id, [&](const auto& queued) {
                        kinds.push_back(queued.kind);
                    });
                    if (kinds.empty())
                        break;
                    for (const auto kind : kinds)
                        self.match_->cancel_queued_order(id, kind, 0, nullptr);
                }
                const auto type_total = static_cast<uint16_t>(self.spawn_types_.size());
                for (uint16_t type = 1; type < type_total; ++type) {
                    const auto queued = self.match_->queued_build_count(id, type);
                    if (queued > 0)
                        self.match_->change_queued_count(
                            id, orders::building_build_kind, type, -queued
                        );
                }
                if (self.match_->self_destruct_remaining(id) > 0) {
                    const uint16_t one = id;
                    self.match_->toggle_self_destruct(std::span<const uint16_t>(&one, 1));
                }
                self.match_->set_unit_squad(id, 0);
            }
        }

        void clear_board() {
            self.megamap_open_ = false;
            self.set_megamap_open(false);
            self.whiteboard_.lines.clear();
            self.whiteboard_.markers.clear();
            self.whiteboard_.outgoing.clear();
            self.whiteboard_.received_marker = false;
            self.whiteboard_input_ = {};
            oa::sim::messages::clear_messages(self.match_->state().game);
        }

        void restore_window() {
            const bool full = (SDL_GetWindowFlags(self.sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0;
            if (full != origin_full_screen) {
                tap(SDL_SCANCODE_RETURN, SDL_KMOD_LALT, false);
                device_state::hold_key(SDL_SCANCODE_LALT, false);
                SDL_SetModState(SDL_KMOD_NONE);
            }
            int width = 0;
            int height = 0;
            if (SDL_GetWindowSize(self.sdl_.window, &width, &height) &&
                (width != origin_width || height != origin_height))
                SDL_SetWindowSize(self.sdl_.window, origin_width, origin_height);
        }

        void reset() {
            site_word.clear();
            row_watcher = false;
            if (self.engine_settings_dialog() != nullptr)
                tap(SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, false);
            if (self.match_paused_)
                self.resume_match_pause();
            if (self.chat_composing_)
                self.close_chat_line();
            if (self.unit_info_panel_)
                self.press_unit_info_done();
            self.clear_local_selection();
            for (const auto& saved : origin_unit_flags) {
                auto* unit = self.match_->world().slots[saved.first].unit;
                if (unit != nullptr)
                    unit->flags = saved.second & ~OA_UNIT_FLAG_SELECTED;
            }
            self.clear_local_selection();
            self.reset_match_command();
            self.pending_build_type_ = 0;
            self.stop_match_tracking();
            self.match_tracking_ = false;
            oa::sim::selection::clear_cycle_marks(self.match_->state());
            restore_words();
            release_keys();
            self.middle_pan_ = {};
            self.build_queue_drag_.reset();
            self.match_drag_.reset();
            self.command_queued_with_shift_ = false;
            clear_queues();
            clear_board();
            // Leave full screen before placing the pointer, so the point is
            // the window's centre and the next row does not scroll an edge.
            restore_window();
            recenter();
            self.scroll_clock_ = self.frame_time_ns_;
            self.frame_run_scroll_ = 0;
        }

        void hover(uint16_t id) {
            if (id == 0)
                return;
            self.center_camera_on_unit(id);
            const auto at = unit_point(id);
            move_canvas(at.first, at.second);
            settle();
        }

        bool ends_with(std::string_view text, std::string_view suffix) const {
            return text.size() >= suffix.size() &&
                   text.substr(text.size() - suffix.size()) == suffix;
        }

        bool click_build_button(bool mex) {
            if (!self.match_hud_)
                return false;
            for (const auto& gadget : self.match_hud_->layout.gadgets) {
                if (gadget.common.active == 0)
                    continue;
                if ((static_cast<uint8_t>(gadget.common.common_attributes) &
                     oa::ui::hud::kCommonUnitButton) == 0)
                    continue;
                const std::string_view name = gadget.common.name;
                if (mex && !ends_with(name, "MEX"))
                    continue;
                const auto point = oa::ui::display_layout::source_to_canvas(
                    self.match_layout_,
                    gadget.common.x + gadget.common.width / 2,
                    gadget.common.y + gadget.common.height / 2
                );
                pointer_button(
                    SDL_BUTTON_LEFT,
                    static_cast<float>(point.x),
                    static_cast<float>(point.y),
                    true,
                    1
                );
                pointer_button(
                    SDL_BUTTON_LEFT,
                    static_cast<float>(point.x),
                    static_cast<float>(point.y),
                    false,
                    1
                );
                return self.pending_build_type_ != 0 || self.match_command_ == MatchCommand::build;
            }
            return false;
        }

        void queue_build_at_site() {
            if (self.pending_build_type_ == 0)
                arm_build(false);
            Gesture site_gesture{};
            site_gesture.shift = true;
            site_gesture.place = "queued-site";
            site_gesture.button = SDL_BUTTON_LEFT;
            const auto point = place_point(site_gesture);
            hold_mods(site_gesture, true);
            pointer_button(site_gesture.button, point.first, point.second, true, 1);
            pointer_button(site_gesture.button, point.first, point.second, false, 1);
            hold_mods(site_gesture, false);
            settle();
        }

        void arm_build(bool mex) {
            if (!click_build_button(mex) && mex)
                click_build_button(false);
            if (self.pending_build_type_ == 0) {
                const char* name = mex ? "ARMMEX" : "ARMSOLAR";
                auto type = oa::sim::unit_spawn::find_type_index(self.spawn_type_names_, name);
                if (type == 0)
                    type = oa::sim::unit_spawn::find_type_index(self.spawn_type_names_, "ARMSOLAR");
                self.match_command_ = MatchCommand::build;
                self.pending_build_type_ = type;
            }
        }

        // Squad 2's select rows are the empty-squad case, so they are not planted.
        // Every other select or add digit recalls the commander, as digit 1 does.
        int planted_squad(const std::string& id) const {
            const auto number_of = [&](std::string_view stem) -> int {
                if (id.rfind(stem, 0) != 0)
                    return 0;
                const auto rest = id.substr(stem.size());
                const auto colon = rest.find(':');
                const auto digits = colon == std::string::npos ? rest : rest.substr(0, colon);
                if (digits.size() != 1 || digits[0] < '1' || digits[0] > '9')
                    return 0;
                return digits[0] - '0';
            };
            if (id.rfind("group.add-", 0) == 0)
                return number_of("group.add-");
            if (id.rfind("group.select-2", 0) == 0)
                return 0;
            return number_of("group.select-");
        }

        void preassign_group(const std::string& id) {
            const int squad = planted_squad(id);
            if (squad == 0)
                return;
            select_role("commander");
            self.assign_squad(squad);
            self.clear_local_selection();
        }

        void apply(const TableRow& row) {
            const auto& prepared = row.prepared;
            if (prepared.shared || prepared.watcher) {
                row_watcher = prepared.watcher;
                self.extension_.state = [](void*, const Runtime&) -> uint32_t {
                    auto bits = extension_state::multiplayer | extension_state::shared_match;
                    if (row_watcher)
                        bits |= extension_state::local_watcher;
                    return bits;
                };
            }
            set_interface(prepared.interface_name);
            set_switch_alt(prepared.switch_alt);
            if (prepared.wheel_zoom_off)
                set_wheel_zoom(false);
            set_flag_bits(prepared.developer, prepared.debug_keys);
            if (prepared.escape_menu)
                self.engine_settings_state().current.escape_opens_menu = true;
            if (prepared.marker) {
                self.whiteboard_.received_marker = true;
                self.whiteboard_.received_x = self.match_camera_x_ + 400;
                self.whiteboard_.received_y = self.match_camera_z_ + 400;
            }
            preassign_group(row.id);
            select_role(prepared.select);
            if (prepared.armed == "attack")
                tap(SDL_SCANCODE_A, SDL_KMOD_NONE, false);
            else if (prepared.armed == "build")
                arm_build(
                    row.id.find("snap") != std::string::npos ||
                    row.id.find("place") != std::string::npos
                );
            else if (!prepared.armed.empty())
                fail("unknown armed word " + prepared.armed);
            const bool hover_unit = row.gesture.place == "unit" || prepared.unit_info ||
                                    row.id.rfind("game.unit-info", 0) == 0 ||
                                    row.id.rfind("game.pin-unit", 0) == 0 ||
                                    row.id.rfind("debug.kill-under-pointer", 0) == 0;
            if (hover_unit)
                hover(id_of(prepared.select == "all" ? "commander" : prepared.select));
            if (prepared.chat)
                self.open_chat_line();
            if (prepared.menu)
                self.show_match_pause_menu();
            if (prepared.unit_info && !self.unit_info_panel_)
                tap(SDL_SCANCODE_F1, SDL_KMOD_NONE, false);
            if (prepared.unit_info && !self.unit_info_panel_)
                fail(row.id + " did not open the unit info");
            if (row.id == "whiteboard.wipe" || row.id == "whiteboard.move" ||
                row.id == "whiteboard.erase") {
                const auto at = battlefield_centre();
                const auto map = self.battlefield_map_point(at.first, at.second);
                if (row.id == "whiteboard.wipe")
                    oa::ui::hud::whiteboard_draw_line(
                        self.whiteboard_, map[0], map[1], map[0] + 8, map[1], 1
                    );
                else
                    oa::ui::hud::whiteboard_place_marker(self.whiteboard_, map[0], map[1], 1, "");
            }
            if (row.id.rfind("build.move-queued-site", 0) == 0)
                queue_build_at_site();
            if (row.gesture.place == "megamap") {
                set_wheel_zoom(false);
                self.set_megamap_open(true);
                if (!self.megamap_open_)
                    self.megamap_open_ = true;
            }
        }

        [[nodiscard]] int screenshot_total() const {
            const auto* con = self.match_console();
            const std::string output = oa::ui::console::output_directory(
                self.match_->state().game, con != nullptr ? con->host : nullptr
            );
            const auto path = self.game_file_path(
                output + kScreenshotFolder, oa::ui::frontend::SavePathUse::write
            );
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                return 0;
            int total = 0;
            for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
                if (!error && entry.is_regular_file(error))
                    ++total;
            }
            return total;
        }

        [[nodiscard]] std::string console_line() const {
            const auto* con = self.match_console();
            if (con == nullptr)
                return {};
            std::size_t length = 0;
            while (length < sizeof con->last_command && con->last_command[length] != '\0')
                ++length;
            return std::string(con->last_command, length);
        }

        Shot shoot() const {
            Shot shot;
            auto& slots = self.match_->world().slots;
            for (const auto& slot : slots) {
                if (slot.unit == nullptr || slot.owner_index != self.match_local_player_)
                    continue;
                if ((slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
                    ++shot.selected;
                if (slot.unit->squad >= 1 && slot.unit->squad <= 9)
                    ++shot.groups[slot.unit->squad];
                if (self.match_->self_destruct_remaining(slot.unit_index) > 0)
                    ++shot.self_destruct;
                std::vector<uint8_t> kinds;
                self.match_->visit_primary_queue(slot.unit_index, [&](const auto& queued) {
                    kinds.push_back(queued.kind);
                });
                if (!kinds.empty())
                    shot.queues.emplace(slot.unit_index, std::move(kinds));
                const auto type_total = static_cast<uint16_t>(self.spawn_types_.size());
                for (uint16_t type = 1; type < type_total; ++type)
                    shot.build_queue += self.match_->queued_build_count(slot.unit_index, type);
            }
            shot.primary = role_of(self.selected_match_unit_);
            shot.armed =
                self.pending_build_type_ != 0 ? "build" : command_word(self.match_command_);
            shot.camera_x = self.match_camera_x_;
            shot.camera_z = self.match_camera_z_;
            const auto& game = self.match_->state().game;
            shot.paused =
                self.match_paused_ || (game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
            shot.speed = game.requested_speed;
            shot.panel = known_menu(panel_stem(self.match_hud_panel_))
                             ? panel_stem(self.match_hud_panel_)
                             : std::string{};
            shot.unit_info = self.unit_info_panel_.has_value();
            shot.chat = self.chat_composing_;
            shot.messages = static_cast<int>(self.match_message_lines().size());
            shot.build_page = self.match_build_page_;
            shot.megamap = self.megamap_open_;
            shot.lines = static_cast<int>(self.whiteboard_.lines.size());
            shot.markers = static_cast<int>(self.whiteboard_.markers.size());
            for (const auto& marker : self.whiteboard_.markers)
                if (marker.text.empty())
                    ++shot.dots;
            shot.editing = self.whiteboard_input_.editing;
            shot.kill_board =
                (game.graphics_flags & oa::ui::console::graphics_flag::kill_board) != 0;
            shot.health_bars =
                (game.graphics_flags & oa::ui::console::graphics_flag::damage_bars) != 0;
            shot.screenshots = screenshot_total();
            shot.full_screen = (SDL_GetWindowFlags(self.sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0;
            shot.settings = self.engine_settings_dialog() != nullptr;
            shot.options_dialog = (game.frame_flags & oa::ui::console::kFrameFlagOptionsOpen) != 0;
            shot.debug_keys = (game.outcome_flags & oa::ui::console::outcome_flag::debug_keys) != 0;
            shot.console = console_line();
            if (self.pending_build_type_ != 0)
                shot.facing = facing_word(self.pending_build_facing());
            shot.site = site_word;
            return shot;
        }

        std::string describe(const Shot& before, const Shot& after) const {
            auto text = effect_of(before, after);
            const uint16_t ids[] = {commander, builder, factory, aircraft, combat, combat_two};
            std::string order;
            const auto consider = [&](uint16_t id) {
                if (id == 0 || !order.empty())
                    return;
                const auto earlier = before.queues.find(id);
                const auto later = after.queues.find(id);
                const std::vector<uint8_t> empty;
                const auto& was = earlier == before.queues.end() ? empty : earlier->second;
                const auto& now = later == after.queues.end() ? empty : later->second;
                if (now.size() >= was.size() && now != was && !now.empty())
                    order = kind_word(now.back()) + ":" + role_of(id);
            };
            if (self.selected_match_unit_ != 0)
                consider(self.selected_match_unit_);
            for (const auto id : ids)
                consider(id);
            if (order.empty())
                return text;
            if (text == "none")
                return "order=" + order;
            return text + ";order=" + order;
        }

        [[nodiscard]] std::pair<float, float> place_point(const Gesture& gesture) {
            if (gesture.place == "battlefield" || gesture.place == "queued-site") {
                // A whiteboard stroke has to start on the battlefield. Open
                // ground beside a unit can fall outside the overlay.
                auto point =
                    gesture.place == "queued-site" || gesture.held == SDL_SCANCODE_BACKSLASH
                        ? battlefield_centre()
                        : open_ground();
                if (gesture.place == "queued-site") {
                    point.first += 40.0F;
                    point.second += 40.0F;
                    clamp_battlefield(point.first, point.second);
                }
                return point;
            }
            if (gesture.place == "unit") {
                const auto id = combat != 0 ? combat : commander;
                return unit_point(id);
            }
            if (gesture.place == "minimap") {
                if (self.radar_picture_.width <= 0 || self.radar_picture_.height <= 0)
                    fail("the minimap has no picture");
                return {
                    static_cast<float>(self.radar_picture_.x + self.radar_picture_.width / 2),
                    static_cast<float>(self.radar_picture_.y + self.radar_picture_.height / 2)
                };
            }
            if (gesture.place == "build-button") {
                if (!self.match_hud_)
                    fail("has no build panel");
                for (const auto& gadget : self.match_hud_->layout.gadgets) {
                    if (gadget.common.active == 0)
                        continue;
                    if ((static_cast<uint8_t>(gadget.common.common_attributes) &
                         oa::ui::hud::kCommonUnitButton) == 0)
                        continue;
                    const auto point = oa::ui::display_layout::source_to_canvas(
                        self.match_layout_,
                        gadget.common.x + gadget.common.width / 2,
                        gadget.common.y + gadget.common.height / 2
                    );
                    return {static_cast<float>(point.x), static_cast<float>(point.y)};
                }
                fail("found no build button");
            }
            if (gesture.place == "megamap")
                return battlefield_centre();
            fail("unknown place " + gesture.place);
        }

        void note_site(const Gesture& gesture, float x, float y) {
            if (gesture.kind != GestureKind::button || gesture.place != "battlefield")
                return;
            if (self.pending_build_type_ == 0)
                return;
            site_word = self.snapped_build_site(x, y) ? "snapped" : "free";
        }

        void pan_for(uint64_t elapsed_ns) {
            self.scroll_clock_ = kClockStartNs;
            self.frame_time_ns_ = kClockStartNs + elapsed_ns;
            self.pan_match_camera();
        }

        void edge_point(std::string_view place, float& x, float& y) const {
            int width = 0;
            int height = 0;
            if (!SDL_GetWindowSize(self.sdl_.window, &width, &height))
                fail(SDL_GetError());
            const auto mid_x = static_cast<float>(width) / 2.0F;
            const auto mid_y = static_cast<float>(height) / 2.0F;
            x = mid_x;
            y = mid_y;
            if (place == "edge-left")
                x = 0;
            else if (place == "edge-right")
                x = static_cast<float>(width - 1);
            else if (place == "edge-top")
                y = 0;
            else if (place == "edge-bottom")
                y = static_cast<float>(height - 1);
            else
                fail("unknown edge " + std::string(place));
        }

        bool edge_place(std::string_view place) const { return place.rfind("edge-", 0) == 0; }

        void play_gesture(const TableRow& row) {
            const auto& gesture = row.gesture;
            hold_mods(gesture, true);
            if (row.id == "chat.send") {
                const char* typed = "a";
                SDL_Event text{};
                text.type = SDL_EVENT_TEXT_INPUT;
                text.text.text = typed;
                dispatch(text);
            }
            if (gesture.kind == GestureKind::key) {
                if (gesture.scancode == SDL_SCANCODE_SPACE)
                    self.space_held_by_check_ = true;
                tap(gesture.scancode, mods_of(gesture), gesture.repeat);
                hold_mods(gesture, false);
                self.space_held_by_check_ = false;
                // A key that resizes the window (Alt+Enter) leaves the pointer
                // on the new edge. Put it back before the settling frame.
                recenter();
                settle();
                return;
            }
            if (gesture.kind == GestureKind::hold) {
                device_state::hold_key(gesture.scancode, true);
                if (gesture.scancode == SDL_SCANCODE_SPACE)
                    self.space_held_by_check_ = true;
                const bool arrow = gesture.scancode == SDL_SCANCODE_LEFT ||
                                   gesture.scancode == SDL_SCANCODE_RIGHT ||
                                   gesture.scancode == SDL_SCANCODE_UP ||
                                   gesture.scancode == SDL_SCANCODE_DOWN;
                if (arrow)
                    pan_for(static_cast<uint64_t>(gesture.hold_ms) * 1'000'000ULL);
                device_state::hold_key(gesture.scancode, false);
                hold_mods(gesture, false);
                self.space_held_by_check_ = false;
                recenter();
                settle();
                return;
            }
            if (edge_place(gesture.place)) {
                float x = 0;
                float y = 0;
                edge_point(gesture.place, x, y);
                move_window(x, y);
                pan_for(300'000'000ULL);
                hold_mods(gesture, false);
                recenter();
                settle();
                return;
            }
            const auto point = place_point(gesture);
            if (gesture.kind == GestureKind::wheel) {
                wheel_at(point.first, point.second, gesture.wheel_up);
                hold_mods(gesture, false);
                return;
            }
            note_site(gesture, point.first, point.second);
            if (gesture.kind == GestureKind::button) {
                pointer_button(gesture.button, point.first, point.second, true, gesture.clicks);
                pointer_button(gesture.button, point.first, point.second, false, gesture.clicks);
            } else {
                const auto end_x = point.first + 48.0F;
                const auto end_y = point.second + 8.0F;
                pointer_button(gesture.button, point.first, point.second, true, 1);
                move_canvas(end_x, end_y);
                pointer_button(gesture.button, end_x, end_y, false, 1);
            }
            hold_mods(gesture, false);
            settle();
        }

        std::string play_row(const TableRow& row) {
            reset();
            apply(row);
            site_word.clear();
            const Shot before = shoot();
            play_gesture(row);
            // A narrower battlefield clamps the same view to a new corner.
            // Full screen and the settings column do that, and the corner
            // depends on the display, so the row records a pan only past it.
            const int32_t played_x = self.match_camera_x_;
            const int32_t played_z = self.match_camera_z_;
            self.match_camera_x_ = before.camera_x;
            self.match_camera_z_ = before.camera_z;
            self.refresh_on_screen_view();
            const int32_t reseat_x = self.match_camera_x_;
            const int32_t reseat_z = self.match_camera_z_;
            self.match_camera_x_ = played_x;
            self.match_camera_z_ = played_z;
            self.refresh_on_screen_view();
            Shot after = shoot();
            after.camera_x = before.camera_x + (self.match_camera_x_ - reseat_x);
            after.camera_z = before.camera_z + (self.match_camera_z_ - reseat_z);
            if (!site_word.empty())
                after.site = site_word;
            auto text = describe(before, after);
            // describe appends order at the end; sort by rebuilding when needed.
            if (text.find(";order=") != std::string::npos || text.rfind("order=", 0) == 0) {
                std::map<std::string, std::string> tokens;
                if (text != "none") {
                    for (const auto part : split(text, ';')) {
                        const auto mark = part.find('=');
                        if (mark != std::string_view::npos)
                            tokens.emplace(
                                std::string(part.substr(0, mark)),
                                std::string(part.substr(mark + 1))
                            );
                    }
                }
                text.clear();
                for (const auto& token : tokens) {
                    if (!text.empty())
                        text += ';';
                    text += token.first + "=" + token.second;
                }
                if (text.empty())
                    text = "none";
            }
            return text;
        }

        void run() {
            if (self.sdl_.renderer == nullptr || self.sdl_.window == nullptr)
                fail("needs the SDL window");
            if ((SDL_GetWindowFlags(self.sdl_.window) & SDL_WINDOW_MOUSE_FOCUS) == 0)
                fail("the window does not have the pointer");
            self.start_benchmark_skirmish();
            self.apply_output_mode();
            spawn_all();
            self.center_camera_on_unit(commander);
            self.match_tick_blocked_ = true;
            prime_clock();
            settle();
            capture_origin();
            const auto rows = read_table(self.options_.check_key_table);
            std::vector<std::pair<std::string, std::string>> observed;
            std::vector<std::string> mismatches;
            for (const auto& row : rows) {
                if (row.prepared.macos && !on_macos())
                    continue;
                const auto got = play_row(row);
                observed.emplace_back(row.id, got);
                if (got != row.effect)
                    mismatches.push_back(row.id + ": expected " + row.effect + ", got " + got);
            }
            std::ofstream out("key-table-observed.tsv");
            if (!out)
                fail("cannot write key-table-observed.tsv");
            out << "row\tsetup\tinput\teffect\n";
            for (const auto& row : rows) {
                if (row.prepared.macos && !on_macos())
                    continue;
                std::string got;
                for (const auto& seen : observed)
                    if (seen.first == row.id)
                        got = seen.second;
                out << row.id << '\t' << row.setup << '\t' << row.input << '\t' << got << '\n';
            }
            out.flush();
            if (!mismatches.empty()) {
                for (const auto& line : mismatches)
                    std::cout << line << '\n';
                std::cout.flush();
                fail(mismatches.front());
            }
            std::cout << "key table: " << observed.size() << " rows match "
                      << self.options_.check_key_table << '\n';
        }
    };

    try {
        Player player{*this};
        player.run();
    } catch (...) {
        put_preferences_back();
        throw;
    }
    put_preferences_back();
}

} // namespace oa::app
