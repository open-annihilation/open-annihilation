// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "check.hpp"

#include "oa/ui/hud/command_buttons.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::ui::hud;

namespace {

struct Heard {
    std::vector<std::string> sounds;
    std::vector<std::string> orders;
};

HudEvents events_for(Heard& heard) {
    return {
        &heard,
        [](void* user, const char* name) { static_cast<Heard*>(user)->sounds.emplace_back(name); },
        [](void* user, const char* tag, int32_t value) {
            auto& orders = static_cast<Heard*>(user)->orders;
            orders.push_back(std::string(tag) + "=" + std::to_string(value));
        },
    };
}

struct Case {
    const char* name{};
    uint8_t lit_order{}; // Game.pointer_command once the button is lit
    const char* sound{};
};

// The ARMGEN.GUI and CORGEN.GUI command buttons, and those of a mod's own
// side (GOKGEN.GUI, TALGEN.GUI), the order each arms in Game.pointer_command
// while lit (1 once turned off) and its allsound.tdf entry.
constexpr Case kCases[] = {
    {"ARMMOVE", 2, "immediateorders"},
    {"GOKMOVE", 2, "immediateorders"},
    {"TALATTACK", 3, "immediateorders"},
    {"ARMATTACK", 3, "immediateorders"},
    {"CORBLAST", 4, "immediateorders"},
    {"ARMUNLOAD", 5, "specialorders"},
    {"CORLOAD", 6, "immediateorders"},
    {"ARMDEFEND", 7, "immediateorders"},
    {"CORREPAIR", 8, "specialorders"},
    {"ARMPATROL", 9, "immediateorders"},
    {"ARMRECLAIM", 0x0c, "specialorders"},
    {"CORCAPTURE", 0x0d, "specialorders"},
};

void lit_and_unlit() {
    auto game = std::make_unique<Game>();
    for (const auto& test : kCases) {
        Heard heard;
        game->pointer_command = 0xee;
        game->pointer_flags = 0xff;
        CHECK(order_panel_command(*game, test.name, 1, events_for(heard)));
        CHECK(armed_order_of(*game) == test.lit_order);
        CHECK(game->pointer_flags == 0xf7);
        CHECK(heard.sounds.size() == 1 && heard.sounds[0] == test.sound);
        CHECK(heard.orders.empty());
        // Clicking the lit button turns it off: the default order is armed.
        CHECK(order_panel_command(*game, test.name, 0, events_for(heard)));
        CHECK(armed_order_of(*game) == armed_order::default_order);
        CHECK(heard.sounds.size() == 2 && heard.sounds[1] == test.sound);
    }
}

// STOP arms the default order whatever its status and gives the group STOP.
void stop_gives_stop() {
    auto game = std::make_unique<Game>();
    for (const int16_t status : {int16_t{0}, int16_t{1}}) {
        Heard heard;
        game->pointer_command = armed_order::attack;
        CHECK(order_panel_command(*game, "ARMSTOP", status, events_for(heard)));
        CHECK(armed_order_of(*game) == armed_order::default_order);
        CHECK(heard.orders.size() == 1 && heard.orders[0] == "STOP=0");
        CHECK(heard.sounds.size() == 1 && heard.sounds[0] == kImmediateOrdersSound);
    }
}

// Other names are left to the build buttons; the match is case sensitive.
void other_names() {
    auto game = std::make_unique<Game>();
    Heard heard;
    game->pointer_command = armed_order::patrol;
    CHECK(!order_panel_command(*game, "ARMSOLAR", 1, events_for(heard)));
    CHECK(!order_panel_command(*game, "armattack", 1, events_for(heard)));
    CHECK(!order_panel_command(*game, nullptr, 1, events_for(heard)));
    CHECK(armed_order_of(*game) == armed_order::patrol && heard.sounds.empty());
}

// ui.interface-fixes cursor-reset: the panel's unit counts as gone once its
// slot holds no type.
void vanished_panel_unit() {
    auto* world = world_create();
    const WorldCapacity capacity{8, 2, 0};
    CHECK(world != nullptr && world_alloc_tables(world, &capacity) != 0);
    Game& game = world->game;
    CHECK(!panel_unit_vanished(*world)); // no panel unit
    game.panel_unit_id = 3;
    world->units[3].type_index = 2;
    CHECK(!panel_unit_vanished(*world));
    world->units[3].type_index = 0;
    CHECK(panel_unit_vanished(*world));
    game.panel_unit_id = 200; // past the table
    CHECK(!panel_unit_vanished(*world));
    world_destroy(world);
}

PanelButton shown(const char* name, int16_t x, int16_t y, uint8_t attributes = 0) {
    PanelButton button;
    button.name = name;
    button.x = x;
    button.y = y;
    button.width = 64;
    button.height = 20;
    button.attributes = attributes;
    button.active = true;
    return button;
}

// Tab walks the order column, then the build page. Arrows move only once the
// ring shows. Return presses that stop only then, and leaves the focus on it.
void panel_focus_moves_and_returns() {
    std::vector<PanelButton> buttons(7);
    buttons[1] = shown("ARMMOVE", 10, 10);
    buttons[2] = shown("ARMSTOP", 10, 40);
    buttons[3] = shown("ARMBUILD", 10, 70);
    // Far enough right that it is the next column, not the same row as Move.
    buttons[4] = shown("ARMSOLAR", 160, 10, kCommonUnitButton);
    // A state button (fire orders, move orders, on and off, cloak) is a stop
    // whatever it is named; a greyed or inactive button is not.
    buttons[5] = shown("ARMFIREORD", 10, 200);
    buttons[6] = shown("ARMATTACK", 10, 100);
    buttons[6].grayed = true;
    buttons[0] = shown("ARMPAN", 0, 0);
    buttons[0].active = false;

    const std::vector<int32_t> stops = panel_button_stops(buttons);
    CHECK(stops.size() == 5);
    CHECK(stops[0] == 1 && stops[1] == 2 && stops[2] == 3 && stops[3] == 5 && stops[4] == 4);

    const PanelFocusStep tabbed =
        step_panel_button_focus(buttons, stops.front(), PanelFocusMove::tab_next, false);
    CHECK(tabbed.moved && tabbed.index == 2 && !tabbed.press);
    const PanelFocusStep back =
        step_panel_button_focus(buttons, stops.front(), PanelFocusMove::tab_previous, false);
    CHECK(back.moved && back.index == 4);

    const PanelFocusStep hidden = step_panel_button_focus(buttons, 1, PanelFocusMove::down, false);
    CHECK(!hidden.moved && hidden.index == 1 && !hidden.press);
    const PanelFocusStep down = step_panel_button_focus(buttons, 1, PanelFocusMove::down, true);
    CHECK(down.moved && down.index == 2);
    const PanelFocusStep right = step_panel_button_focus(buttons, 1, PanelFocusMove::right, true);
    CHECK(right.moved && right.index == 4);

    const PanelFocusStep pressed = step_panel_button_focus(buttons, 2, PanelFocusMove::press, true);
    CHECK(pressed.press && pressed.index == 2);
    const PanelFocusStep held = step_panel_button_focus(buttons, 2, PanelFocusMove::press, false);
    CHECK(!held.press && held.index == 2);
}

PanelButton placed(const char* name, int16_t x, int16_t y, int16_t width, int16_t height) {
    PanelButton button = shown(name, x, y);
    button.width = width;
    button.height = height;
    return button;
}

// The Arm commander's build page as the match places it, in its gadgets'
// order: the font gadget first, at x 0 with no size, then the page arrows,
// the cells (the left ones at x 0), Orders and Build, Move and Stop. The
// arrows go by place, as on a menu away from the screen's edge.
void panel_at_the_left_edge_steps_by_place() {
    std::vector<PanelButton> buttons(14);
    buttons[0] = placed("HEADER", 0, 128, 128, 352);
    buttons[0].active = false;
    buttons[1] = placed("ARMFONT", 0, 0, 0, 0);
    buttons[2] = placed("ARMPREV", 8, 350, 44, 16);
    buttons[3] = placed("ARMNEXT", 72, 350, 44, 16);
    buttons[4] = placed("ARMSOLAR", 0, 155, 64, 64);
    buttons[5] = placed("ARMWIN", 64, 155, 64, 64);
    buttons[6] = placed("ARMESTOR", 0, 219, 64, 64);
    buttons[7] = placed("ARMMSTOR", 64, 219, 64, 64);
    buttons[8] = placed("ARMMEX", 0, 283, 64, 64);
    buttons[9] = placed("ARMMAKR", 64, 283, 64, 64);
    buttons[10] = placed("ARMORDERS", 3, 132, 59, 19);
    buttons[11] = placed("ARMBUILD", 65, 132, 59, 19);
    buttons[12] = placed("ARMMOVE", 5, 375, 55, 31);
    buttons[13] = placed("ARMSTOP", 64, 375, 55, 31);

    const auto step = [&](int32_t from, PanelFocusMove move) {
        return step_panel_button_focus(buttons, from, move, true).index;
    };
    CHECK(step(10, PanelFocusMove::down) == 4);
    CHECK(step(4, PanelFocusMove::down) == 6);
    CHECK(step(8, PanelFocusMove::down) == 2);
    CHECK(step(2, PanelFocusMove::down) == 12);
    CHECK(step(12, PanelFocusMove::up) == 2);
    CHECK(step(4, PanelFocusMove::up) == 10);
    CHECK(step(11, PanelFocusMove::down) == 5);
    CHECK(step(9, PanelFocusMove::down) == 3);
    CHECK(step(4, PanelFocusMove::right) == 5);
    CHECK(step(5, PanelFocusMove::left) == 4);

    // Tab reads the rows as before: Orders, Build, the cells, the page
    // arrows, then Move and Stop. The font gadget is no stop.
    const std::vector<int32_t> stops = panel_button_stops(buttons);
    const std::vector<int32_t> rows{10, 11, 4, 5, 6, 7, 8, 9, 2, 3, 12, 13};
    CHECK(stops == rows);
}

} // namespace

int main() {
    vanished_panel_unit();
    lit_and_unlit();
    stop_gives_stop();
    other_names();
    panel_focus_moves_and_returns();
    panel_at_the_left_edge_steps_by_place();
    return 0;
}
