// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every side's order page in a live match, its buttons clicked through the
// SDL presenter, whatever prefix the side's page gives its controls (ARMCLOAK,
// or a mod side's GOKFIREORD and TALONOFF).
#include "oa/app/runtime.hpp"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/data/defs/layout.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

// Ticks after a click for its order to reach the units.
constexpr int kOrderTicks = 1;
// Ticks the commander is given to set off toward a point it is sent to.
constexpr int kMoveTicks = 90;
// Map pixels east of the commander that MOVE sends it.
constexpr int32_t kMoveEast = 128;
// Map pixels from the commander, east and south, where a unit that takes
// ON/OFF or CLOAK is placed, tried in turn until one place takes it.
constexpr std::array<std::pair<int32_t, int32_t>, 5> kPlaceOffsets{{
    {96, 96},
    {-96, 96},
    {96, -96},
    {-96, -96},
    {160, 0},
}};

uint32_t fire_order(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
}

uint32_t move_order(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
}

bool cloak_running(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_CLOAK_RUNNING) != 0;
}

bool switched_on(const oa::Unit& unit) {
    return (unit.state_flags & OA_UNIT_STATE_ACTIVE) != 0;
}

std::string field_text(const char* field, std::size_t size) {
    return {field, strnlen(field, size)};
}

bool same_name(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::toupper(static_cast<unsigned char>(a)) ==
                      std::toupper(static_cast<unsigned char>(b));
           });
}

int64_t squared_distance(const std::array<uint32_t, 3>& position, int32_t x, int32_t z) {
    const int64_t dx = static_cast<int64_t>(position[0] >> 16) - x;
    const int64_t dz = static_cast<int64_t>(position[2] >> 16) - z;
    return dx * dx + dz * dz;
}

} // namespace

void Runtime::check_side_orders() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        throw std::runtime_error("side orders check: needs the SDL renderer");
    if (side_table_.count < 2)
        throw std::runtime_error("side orders check: SIDEDATA lists fewer than two sides");
    std::string report;
    const auto hidden_sides = oa::data::defs::skirmish_hidden_sides();
    for (std::size_t side = 0; side < side_table_.count; ++side) {
        if (side < 32 && ((hidden_sides >> side) & 1u) != 0)
            continue;
        const auto side_name =
            field_text(side_table_.sides[side].name, sizeof side_table_.sides[side].name);
        const auto fail = [&side_name](const std::string& what) {
            throw std::runtime_error("side orders check: " + side_name + ": " + what);
        };
        if (match_)
            leave_match();
        load(Screen::main_menu);
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        state_.player_count = 2;
        if (skirmish_settings_.slots.size() < 2)
            fail("the skirmish has fewer than two slots");
        skirmish_settings_.slots[0].side = static_cast<int32_t>(side);
        skirmish_settings_.slots[1].side = static_cast<int32_t>(side == 0 ? 1 : 0);
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        if (screen_ != Screen::match || !match_)
            fail("Start did not enter a match");
        if (match_view_side() != side)
            fail("the local player does not play the side");

        auto& slots = match_->world().slots;
        const oa::World& world = match_->state();
        uint16_t commander = 0;
        for (const auto& slot : slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = slot.unit_index;
                break;
            }
        if (commander == 0)
            fail("no local commander");
        const auto unit = [&](uint16_t id) -> const oa::Unit& { return slots[id].record; };
        const auto def_of = [&](uint16_t id) -> const oa::UnitDef& {
            return world.unit_defs[unit(id).type_index];
        };
        const auto unit_name = [&](uint16_t id) {
            return field_text(def_of(id).unit_name, sizeof def_of(id).unit_name);
        };
        const auto run_ticks = [&](int ticks) {
            for (int tick = 0; tick < ticks; ++tick)
                step_match_simulation();
        };
        const auto select = [&](uint16_t id) {
            clear_local_selection();
            adopt_selection(id);
            selected_match_unit_ = id;
            apply_match_hud_for_selection();
            center_camera_on_unit(id);
            render_match_surface();
        };
        const auto find_button = [&](std::string_view action) -> std::optional<std::size_t> {
            if (!match_hud_)
                return std::nullopt;
            const auto& gadgets = match_hud_->layout.gadgets;
            for (std::size_t index = 1; index < gadgets.size(); ++index)
                if (match_hud_action(gadgets[index].common.name) == action)
                    return index;
            return std::nullopt;
        };
        const auto offered = [&](std::string_view action) {
            const auto index = find_button(action);
            return index && *index < match_hud_states_.size() && !match_hud_states_[*index].grayed;
        };
        const auto press = [&](float x, float y) {
            float window_x = 0;
            float window_y = 0;
            if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
                fail(std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError());
            bool running = true;
            SDL_Event motion{};
            motion.motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = SDL_GetWindowID(sdl_.window);
            motion.motion.x = window_x;
            motion.motion.y = window_y;
            dispatch_event(motion, running);
            for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
                SDL_Event button{};
                button.button.type = type;
                button.button.windowID = SDL_GetWindowID(sdl_.window);
                button.button.button = SDL_BUTTON_LEFT;
                button.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                button.button.clicks = 1;
                button.button.x = window_x;
                button.button.y = window_y;
                dispatch_event(button, running);
            }
        };
        // A left click at the centre of the page's button for `action`, where
        // the side column draws it, as SDL delivers it; the click must not be
        // an operation the game lacks.
        const auto click = [&](std::string_view action) {
            const auto index = find_button(action);
            if (!index)
                fail("the order page has no " + std::string(action));
            const std::string name = match_hud_->layout.gadgets[*index].common.name;
            const auto point = hud_gadget_centre(*index);
            status_.clear();
            press(static_cast<float>(point.x), static_cast<float>(point.y));
            if (status_.find("not implemented") != std::string::npos)
                fail(name + ": " + status_);
            return name;
        };
        // Selects a unit and shows its order page: a unit's page may open on
        // its build menu, and ORDERS shows the order page.
        const auto show_orders = [&](uint16_t id) {
            select(id);
            if (!find_button("FIREORD") && find_button("ORDERS"))
                click("ORDERS");
            return find_button("FIREORD").has_value();
        };
        // A unit of the side whose type takes `ability` and whose order page
        // offers `action` ungreyed, shown: the commander when it is one, or
        // one placed beside it. A unit page may grey a button its type takes,
        // as a mod's own page for a pop-up turret greys ON/OFF.
        const auto subject = [&](uint32_t ability,
                                 std::string_view action) -> std::optional<uint16_t> {
            if ((def_of(commander).abilities & ability) != 0 && show_orders(commander) &&
                offered(action))
                return commander;
            for (uint32_t type = 1; type < world.unit_def_count; ++type) {
                const auto& def = world.unit_defs[type];
                if ((def.abilities & ability) == 0 ||
                    !same_name(field_text(def.side, sizeof def.side), side_name))
                    continue;
                const auto spawn_type = oa::sim::unit_spawn::find_type_index(
                    spawn_type_names_, field_text(def.unit_name, sizeof def.unit_name)
                );
                if (spawn_type == 0)
                    continue;
                for (const auto& [dx, dz] : kPlaceOffsets) {
                    const auto x =
                        static_cast<int32_t>(slots[commander].unit->position[0] >> 16) + dx;
                    const auto z =
                        static_cast<int32_t>(slots[commander].unit->position[2] >> 16) + dz;
                    oa::sim::unit_spawn::Request request;
                    request.player = match_local_player_;
                    request.type = spawn_type;
                    request.finished = true;
                    request.state = kGroundOccupancyState;
                    request.position = {
                        static_cast<uint32_t>(x) << 16,
                        static_cast<uint32_t>(match_->map_height(
                            static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
                        )) << 16,
                        static_cast<uint32_t>(z) << 16
                    };
                    auto* placed = match_->create(request);
                    if (placed == nullptr || placed->unit == nullptr)
                        continue;
                    const auto id = placed->unit_index;
                    run_ticks(1);
                    if (show_orders(id) && offered(action))
                        return id;
                    break;
                }
            }
            return std::nullopt;
        };
        std::vector<std::string> done;

        // FIRE ORDERS and MOVE ORDERS step the commander's standing orders
        // through hold, return and fire at will, and hold, maneuver and roam;
        // MOVE ORDERS, whose name holds MOVE, arms no move.
        if (!show_orders(commander))
            fail("the order page of " + unit_name(commander) + " has no FIREORD");
        const auto& commander_def = def_of(commander);
        if ((commander_def.abilities & OA_UNIT_DEF_ABILITY_FIRE_STAND_ORDERS) != 0) {
            std::string name;
            for (int step = 0; step < 3; ++step) {
                const auto expected = oa::ui::hud::next_standing_order(fire_order(unit(commander)));
                name = click("FIREORD");
                run_ticks(kOrderTicks);
                if (fire_order(unit(commander)) != expected)
                    fail(
                        name + " gave " + unit_name(commander) + " fire order " +
                        std::to_string(fire_order(unit(commander))) + ", not " +
                        std::to_string(expected)
                    );
            }
            done.push_back(name);
        }
        if ((commander_def.abilities & OA_UNIT_DEF_ABILITY_MOBILE_STAND_ORDERS) != 0) {
            std::string name;
            for (int step = 0; step < 3; ++step) {
                const auto expected = oa::ui::hud::next_standing_order(move_order(unit(commander)));
                name = click("MOVEORD");
                run_ticks(kOrderTicks);
                if (move_order(unit(commander)) != expected)
                    fail(
                        name + " gave " + unit_name(commander) + " move order " +
                        std::to_string(move_order(unit(commander))) + ", not " +
                        std::to_string(expected)
                    );
                if (match_command_ == MatchCommand::move)
                    fail(name + " armed MOVE");
            }
            done.push_back(name);
        }

        // CLOAK cloaks a unit that takes it, or uncloaks one its FBI cloaks
        // from the start (init_cloaked), and a second click undoes it.
        if (const auto cloaker = subject(OA_UNIT_DEF_ABILITY_CAN_CLOAK, "CLOAK")) {
            std::string name;
            for (int step = 0; step < 2; ++step) {
                const bool expected = !cloak_running(unit(*cloaker));
                name = click("CLOAK");
                run_ticks(kOrderTicks);
                if (cloak_running(unit(*cloaker)) != expected)
                    fail(
                        name + " did not " + (expected ? "cloak " : "uncloak ") +
                        unit_name(*cloaker)
                    );
            }
            done.push_back(name + " on " + unit_name(*cloaker));
        } else {
            done.emplace_back("(no unit offers CLOAK)");
        }

        // ON/OFF switches a unit that takes it, and a second click switches it
        // back.
        if (const auto switcher = subject(OA_UNIT_DEF_ABILITY_ON_OFFABLE, "ONOFF")) {
            std::string name;
            for (int step = 0; step < 2; ++step) {
                const bool expected = !switched_on(unit(*switcher));
                name = click("ONOFF");
                run_ticks(kOrderTicks);
                if (switched_on(unit(*switcher)) != expected)
                    fail(
                        name + " did not switch " + unit_name(*switcher) +
                        (expected ? " on" : " off")
                    );
            }
            done.push_back(name + " on " + unit_name(*switcher));
        } else {
            done.emplace_back("(no unit offers ONOFF)");
        }

        // MOVE arms a move, and a click on the battlefield sends the
        // commander there.
        if (!show_orders(commander))
            fail("the order page of " + unit_name(commander) + " has no FIREORD");
        const auto move_name = click("MOVE");
        if (match_command_ != MatchCommand::move)
            fail(move_name + " did not arm MOVE");
        const auto& start = slots[commander].unit->position;
        const std::array<uint32_t, 3> goal{
            start[0] + (static_cast<uint32_t>(kMoveEast) << 16), start[1], start[2]
        };
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto on_screen = project_match_point(viewport, goal);
        const auto destination =
            match_world_point(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y));
        if (!destination)
            fail("found no ground east of the commander");
        const auto goal_x = static_cast<int32_t>((*destination)[0] >> 16);
        const auto goal_z = static_cast<int32_t>((*destination)[2] >> 16);
        const auto before = squared_distance(slots[commander].unit->position, goal_x, goal_z);
        press(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y));
        if (match_command_ != MatchCommand::none)
            fail("kept MOVE armed after the battlefield click");
        run_ticks(kMoveTicks);
        const auto after = squared_distance(slots[commander].unit->position, goal_x, goal_z);
        if (after >= before)
            fail(move_name + " did not send " + unit_name(commander) + " toward the clicked point");
        done.push_back(move_name);

        report +=
            (report.empty() ? "" : "; ") + side_name + " (" +
            field_text(
                side_table_.sides[side].name_prefix, sizeof side_table_.sides[side].name_prefix
            ) +
            "):";
        for (const auto& entry : done)
            report += " " + entry;
    }
    std::cout << "side orders check: " << report << '\n';
}

} // namespace oa::app
