// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order page's FIRE ORDERS, MOVE ORDERS, ON/OFF and CLOAK buttons in a
// live match, clicked through the SDL presenter, and the order overlays over
// the fog.
#include "oa/app/runtime.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/sim/visibility_state.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

// Standing orders and ARMGEN.GUI art frames. The FBI orders of ARMPW
// (StandingFireOrder=2, StandingMoveOrder=1) start the check; ARMSOLAR takes
// neither but is onoffable. ARMCOM.FBI and CORCOM.FBI give the commander
// StandingFireOrder=2 and StandingMoveOrder=0.
constexpr uint32_t hold_fire = 0;
constexpr uint32_t return_fire = 1;
constexpr uint32_t fire_at_will = 2;
constexpr uint32_t mixed_orders = 3; // the "FIRE ORDERS" / "MOVE ORDERS" frame
constexpr uint32_t hold_position = 0;
constexpr uint32_t maneuver = 1;
constexpr uint32_t roam = 2;
// Ticks for a sighting refresh and a weapon sweep to bring a unit in range
// under a fire-at-will unit's guns.
constexpr int kTargetTicks = 240;
// A point of the top bar, in the game's 640x480 HUD coordinates, that no
// button covers: where the pointer rests off the buttons.
constexpr int kAwaySourceX = 300;
constexpr int kAwaySourceY = 8;
// The order overlays over the fog: a scout this far east and north of the
// commander, in map pixels, maps ground for this many ticks, and its sight
// has lapsed this many ticks after it is dismissed.
constexpr int32_t scout_east = 420;
constexpr int32_t scout_north = 40;
constexpr int scout_ticks = 40;
constexpr int sight_lapse_ticks = 240;
// The view's corner this far west and north of the commander, in map pixels:
// the view runs east over the scouted ground into ground never mapped.
constexpr int32_t fog_view_west = 120;
constexpr int32_t fog_view_north = 200;
// Battlefield pixels around a target marker's point that hold its sprite,
// the fewest pixels the marker draws there, and the step of the search for
// fogged ground to queue a move onto.
constexpr int32_t marker_reach = 24;
constexpr std::size_t marker_pixels = 64;
constexpr int32_t fog_search_step = 16;

uint32_t fire_order(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
}

uint32_t move_order(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
}

bool targets(const oa::Unit& shooter, uint16_t target) {
    for (const auto& weapon : shooter.weapons)
        if (weapon.target_b == OA_UNIT_TARGET_IS_UNIT &&
            weapon.target_a == static_cast<int16_t>(target))
            return true;
    return false;
}

void require(bool condition, const std::string& what) {
    if (!condition)
        throw std::runtime_error("match order check: " + what);
}

} // namespace

void Runtime::check_match_orders() {
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    // The buttons are compared with the game's own art, so they wear no key
    // badges.
    {
        auto chosen = engine_settings();
        chosen.show_keys_on_buttons = false;
        apply_engine_settings(chosen);
    }
    start_benchmark_skirmish();
    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    require(commander != 0, "no local commander");
    const auto enemy_player = static_cast<uint8_t>(match_local_player_ == 0 ? 1 : 0);
    const auto spawn = [&](std::string_view name, uint8_t player, int32_t dx, int32_t dz) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        require(type != 0, "no " + std::string(name));
        const auto x = static_cast<int32_t>(slots[commander].unit->position[0] >> 16) + dx;
        const auto z = static_cast<int32_t>(slots[commander].unit->position[2] >> 16) + dz;
        oa::sim::unit_spawn::Request request;
        request.player = player;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* slot = match_->create(request);
        require(slot != nullptr && slot->unit != nullptr, "could not spawn " + std::string(name));
        return slot->unit_index;
    };
    const auto peewee = spawn("ARMPW", match_local_player_, -96, 0);
    const auto wingman = spawn("ARMPW", match_local_player_, -96, 160);
    const auto solar = spawn("ARMSOLAR", match_local_player_, 96, 96);
    center_camera_on_unit(peewee);
    const auto run_ticks = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick)
            step_match_simulation();
    };
    run_ticks(1);

    const auto select = [&](std::initializer_list<uint16_t> ids) {
        clear_local_selection();
        for (const auto id : ids)
            adopt_selection(id);
        selected_match_unit_ = *ids.begin();
        apply_match_hud_for_selection();
    };
    const auto button = [&](std::string_view action) {
        for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_action(match_hud_->layout.gadgets[index].common.name) == action)
                return index;
        throw std::runtime_error("match order check: the order page has no " + std::string(action));
    };
    // The button's rectangle in the HUD layer must hold frame `frame` of its art.
    const auto expect_frame = [&](std::string_view action,
                                  std::size_t frame,
                                  const std::string& what) {
        render_match_surface();
        const auto& gadget = match_hud_->layout.gadgets[button(action)];
        const auto* sequence = gaf_sequence(match_hud_->sprites, gadget.common.name);
        if (sequence == nullptr)
            sequence = gaf_sequence(match_hud_->shared_sprites, gadget.common.name);
        require(
            sequence != nullptr && frame < sequence->frames.size(),
            "no art for " + gadget.common.name
        );
        const auto rendered = oa::formats::gaf::render_normal(sequence->frames[frame]);
        require(rendered.ok(), "cannot render " + gadget.common.name);
        const auto& image = *rendered.frame;
        const auto& palette = match_hud_->background.palette ? *match_hud_->background.palette
                                                             : match_hud_->gui_palette;
        const auto& hud = match_hud_cpu_;
        std::size_t differing = 0;
        for (uint32_t row = 0; row < image.height; ++row)
            for (uint32_t column = 0; column < image.width; ++column) {
                const auto offset = static_cast<std::size_t>(row) * image.width + column;
                if (image.coverage[offset] == 0)
                    continue;
                const auto x = static_cast<std::size_t>(gadget.common.x) + column;
                const auto y = static_cast<std::size_t>(gadget.common.y) + row;
                const auto* shown = hud.rgb.data() + (y * hud.width + x) * 3U;
                const auto* colour =
                    palette.data() +
                    static_cast<std::size_t>(image.pixels[offset]) * oa::palette_entry_bytes;
                differing +=
                    shown[0] != colour[0] || shown[1] != colour[1] || shown[2] != colour[2] ? 1 : 0;
            }
        if (differing != 0) {
            write_ppm(report_directory / "native-match-orders-failed.ppm", hud);
            throw std::runtime_error(
                "match order check: " + what + ": " + gadget.common.name + " does not show frame " +
                std::to_string(frame) + " (" + std::to_string(differing) + " pixels differ)"
            );
        }
    };
    const auto last_frame = [&](std::string_view action) {
        const auto& gadget = match_hud_->layout.gadgets[button(action)];
        const auto* sequence = gaf_sequence(match_hud_->sprites, gadget.common.name);
        if (sequence == nullptr)
            sequence = gaf_sequence(match_hud_->shared_sprites, gadget.common.name);
        require(
            sequence != nullptr && !sequence->frames.empty(), "no art for " + gadget.common.name
        );
        return sequence->frames.size() - 1U;
    };
    // A left click at the button's presented position, as SDL delivers it.
    const auto click = [&](std::string_view action) {
        const auto& common = match_hud_->layout.gadgets[button(action)].common;
        const auto rect = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_, common.x, common.y, common.width, common.height
        );
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(
                sdl_.renderer,
                static_cast<float>(rect.x) + static_cast<float>(rect.width) / 2.0F,
                static_cast<float>(rect.y) + static_cast<float>(rect.height) / 2.0F,
                &window_x,
                &window_y
            ))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        bool running = true;
        SDL_Event motion{};
        motion.motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = SDL_GetWindowID(sdl_.window);
        motion.motion.x = window_x;
        motion.motion.y = window_y;
        dispatch_event(motion, running);
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event press{};
            press.button.type = type;
            press.button.windowID = SDL_GetWindowID(sdl_.window);
            press.button.button = SDL_BUTTON_LEFT;
            press.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            press.button.clicks = 1;
            press.button.x = window_x;
            press.button.y = window_y;
            dispatch_event(press, running);
        }
    };
    const auto unit = [&](uint16_t id) -> const oa::Unit& { return slots[id].record; };
    const auto snapshot = [&](const char* name) {
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(report_directory / name, presented);
    };

    // The commander starts the skirmish with its FBI standing orders: fire at
    // will and hold position. Its panel opens on the build menu; ORDERS shows
    // them.
    require(
        fire_order(unit(commander)) == fire_at_will && move_order(unit(commander)) == hold_position,
        "the commander started with fire order " + std::to_string(fire_order(unit(commander))) +
            " and move order " + std::to_string(move_order(unit(commander))) +
            ", not its FBI standing orders"
    );
    select({commander});
    click("ORDERS");
    snapshot("native-match-orders-commander.ppm");
    expect_frame("FIREORD", fire_at_will, "commander selected");
    expect_frame("MOVEORD", hold_position, "commander selected");

    // The commander stands by the radar's spot; only the peewee may shoot.
    auto& commander_flags = slots[commander].record.flags;
    commander_flags = (commander_flags & ~OA_UNIT_FLAG_FIRE_ORDER_MASK) |
                      (hold_fire << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    require(
        fire_order(unit(peewee)) == fire_at_will && move_order(unit(peewee)) == maneuver,
        "ARMPW did not start with its FBI standing orders"
    );
    select({peewee});
    snapshot("native-match-orders-peewee.ppm");
    expect_frame("FIREORD", fire_at_will, "ARMPW selected");
    expect_frame("MOVEORD", maneuver, "ARMPW selected");
    expect_frame("ONOFF", last_frame("ONOFF"), "ARMPW selected");
    expect_frame("CLOAK", last_frame("CLOAK"), "ARMPW selected");

    // Hold fire, then return fire: the radar spawned in range stays unharmed
    // and untargeted. Fire at will: the peewee shoots it.
    uint16_t target = 0;
    const auto radar_harmed = [&] {
        const auto& radar = slots[target];
        return (radar.record.flags & OA_UNIT_FLAG_LIVE) == 0 ||
               radar.record.health < static_cast<int32_t>(radar.unit->type->maximum_health);
    };
    for (const auto order : {hold_fire, return_fire, fire_at_will}) {
        click("FIREORD");
        expect_frame("FIREORD", order, "after a FIRE ORDERS click");
        run_ticks(1);
        require(
            fire_order(unit(peewee)) == order,
            "FIRE ORDERS gave fire order " + std::to_string(fire_order(unit(peewee))) + ", not " +
                std::to_string(order)
        );
        require(
            fire_order(unit(wingman)) == fire_at_will, "FIRE ORDERS reached an unselected unit"
        );
        if (target == 0)
            target = spawn("CORRAD", enemy_player, -96, -96);
        bool harmed = false;
        for (int tick = 0; tick < kTargetTicks && !harmed; ++tick) {
            run_ticks(1);
            harmed = targets(unit(peewee), target) || radar_harmed();
        }
        require(
            harmed == (order == fire_at_will),
            "fire order " + std::to_string(order) + (harmed ? " shot" : " did not shoot") +
                " the enemy radar in range"
        );
    }
    for (const auto order : {roam, hold_position, maneuver}) {
        click("MOVEORD");
        expect_frame("MOVEORD", order, "after a MOVE ORDERS click");
        run_ticks(1);
        require(
            move_order(unit(peewee)) == order,
            "MOVE ORDERS gave move order " + std::to_string(move_order(unit(peewee)))
        );
    }
    const auto state_before = unit(peewee).state_flags;
    click("ONOFF");
    run_ticks(1);
    expect_frame("ONOFF", last_frame("ONOFF"), "after a click on the grayed ON/OFF");
    require(unit(peewee).state_flags == state_before, "the grayed ON/OFF reached ARMPW");

    // Two peewees that disagree show the mixed frame; a click gives both hold fire.
    select({wingman});
    click("FIREORD");
    run_ticks(1);
    require(fire_order(unit(wingman)) == hold_fire, "FIRE ORDERS did not reach the second ARMPW");
    select({peewee, wingman});
    expect_frame("FIREORD", mixed_orders, "two ARMPW with different fire orders");
    click("FIREORD");
    expect_frame("FIREORD", hold_fire, "after a click on mixed fire orders");
    run_ticks(1);
    require(
        fire_order(unit(peewee)) == hold_fire && fire_order(unit(wingman)) == hold_fire,
        "mixed FIRE ORDERS did not give both ARMPW hold fire"
    );

    // The solar takes on/off orders only.
    select({solar});
    snapshot("native-match-orders-solar.ppm");
    expect_frame("FIREORD", last_frame("FIREORD"), "ARMSOLAR selected");
    expect_frame("MOVEORD", last_frame("MOVEORD"), "ARMSOLAR selected");
    expect_frame("CLOAK", last_frame("CLOAK"), "ARMSOLAR selected");
    const auto active = static_cast<uint32_t>(unit(solar).state_flags & OA_UNIT_STATE_ACTIVE);
    expect_frame("ONOFF", active, "ARMSOLAR selected");
    click("ONOFF");
    expect_frame("ONOFF", active ^ 1u, "after an ON/OFF click");
    run_ticks(1);
    require(
        static_cast<uint32_t>(unit(solar).state_flags & OA_UNIT_STATE_ACTIVE) == (active ^ 1u),
        "ON/OFF did not switch ARMSOLAR"
    );

    // With both selected, fire orders reach the peewee alone.
    const auto solar_fire = fire_order(unit(solar));
    select({peewee, solar});
    expect_frame("FIREORD", hold_fire, "ARMPW and ARMSOLAR selected");
    expect_frame("ONOFF", active ^ 1u, "ARMPW and ARMSOLAR selected");
    click("FIREORD");
    run_ticks(1);
    require(
        fire_order(unit(peewee)) == return_fire, "FIRE ORDERS did not reach ARMPW beside ARMSOLAR"
    );
    require(fire_order(unit(solar)) == solar_fire, "FIRE ORDERS reached ARMSOLAR");

    check_command_buttons(peewee, commander);
    check_unit_damage_bar(peewee);
    check_order_overlays_over_fog(
        commander, spawn("ARMPW", match_local_player_, scout_east, -scout_north)
    );
    check_order_button_states(
        spawn("ARMLAB", match_local_player_, 160, -192),
        commander,
        peewee,
        spawn("ARMAMD", match_local_player_, -192, -192)
    );
    std::cout
        << "match order check: the commander starts with its FBI fire at will and hold "
           "position; FIRE ORDERS, MOVE ORDERS and ON/OFF frames and orders, grayed "
           "buttons blank, hold and return fire leave the enemy radar, fire at will takes it; "
           "command buttons light, go out and play their allsound.tdf entries; the damage bar "
           "draws in UI colours 10 and 4 and hides an enemy commander's damage; a factory's, "
           "a commander's, a non-builder's and a static weapon's order buttons are all drawn, "
           "greyed in gray where the unit cannot give the order, and a build page's BUILD "
           "tab is pressed\n";
}

void Runtime::check_order_button_states(
    uint16_t factory, uint16_t commander, uint16_t non_builder, uint16_t static_weapon
) {
    enum class Shown : uint8_t { lit, greyed, hidden };

    struct Expected {
        std::string_view action;
        Shown shown;
    };

    const auto shown_name = [](Shown shown) {
        return shown == Shown::lit ? "lit" : shown == Shown::greyed ? "greyed" : "hidden";
    };
    // Shows the unit's page (its first build page, or its order page for
    // page 0) and checks each order button's state, the art of a greyed one
    // and that it takes no click.
    const auto check_page =
        [&](uint16_t id, int page, std::string_view who, std::initializer_list<Expected> expected) {
            clear_local_selection();
            adopt_selection(id);
            selected_match_unit_ = id;
            apply_match_hud_for_selection();
            if (page > 0)
                show_match_build_page(page);
            reset_match_command();
            render_match_surface();
            const auto& gadgets = match_hud_->layout.gadgets;
            // A build page's BUILD tab is drawn pressed, its ORDERS tab not.
            for (std::size_t at = 1; page > 0 && at < gadgets.size(); ++at) {
                const auto tab = match_hud_action(gadgets[at].common.name);
                if (tab != "BUILD" && tab != "ORDERS")
                    continue;
                const bool on_show = tab == "BUILD";
                require(
                    match_command_lit(at) == on_show,
                    std::string(who) + "'s " + std::string(tab) +
                        (on_show ? " tab is not pressed" : " tab is pressed")
                );
            }
            for (const auto& [action, wanted] : expected) {
                std::size_t index = 0;
                for (std::size_t at = 1; at < gadgets.size() && index == 0; ++at)
                    if (match_hud_action(gadgets[at].common.name) == action)
                        index = at;
                require(index != 0, std::string(who) + "'s page has no " + std::string(action));
                const auto& gadget = gadgets[index];
                auto seen = Shown::hidden;
                if (gadget.common.active != 0) {
                    const auto condition = match_button_condition(index);
                    if (condition == oa::ui::frontend_renderer::ButtonCondition::disabled)
                        seen = Shown::greyed;
                    else if (condition != oa::ui::frontend_renderer::ButtonCondition::hidden)
                        seen = Shown::lit;
                }
                const auto what = std::string(who) + "'s " + std::string(action);
                require(
                    seen == wanted, what + " is " + shown_name(seen) + ", not " + shown_name(wanted)
                );
                // A hidden button is not under the pointer at all.
                require(
                    wanted == Shown::hidden ||
                        gadget_command_available(gadget) == (wanted == Shown::lit),
                    what + (wanted == Shown::lit ? " takes no click" : " takes a click")
                );
                if (wanted != Shown::greyed)
                    continue;
                // Greyed art is gray: through the gray table, then shaded.
                const auto& common = gadget.common;
                for (int row = std::max(0, static_cast<int>(common.y));
                     row <
                     std::min(common.y + common.height, static_cast<int>(match_hud_cpu_.height));
                     ++row)
                    for (int column = std::max(0, static_cast<int>(common.x));
                         column <
                         std::min(common.x + common.width, static_cast<int>(match_hud_cpu_.width));
                         ++column) {
                        const auto* rgb = match_hud_cpu_.rgb.data() +
                                          (static_cast<std::size_t>(row) * match_hud_cpu_.width +
                                           static_cast<std::size_t>(column)) *
                                              3U;
                        const auto [low, high] = std::minmax({rgb[0], rgb[1], rgb[2]});
                        require(high - low <= 4, what + " is greyed in colour, not gray");
                    }
                activate_match_hud(index);
                require(match_command_ == MatchCommand::none, what + " armed an order");
            }
        };
    check_page(
        factory,
        1,
        "ARMLAB",
        {{"MOVE", Shown::lit},
         {"STOP", Shown::lit},
         {"PATROL", Shown::lit},
         {"DEFEND", Shown::greyed},
         {"ATTACK", Shown::greyed},
         {"BLAST", Shown::greyed}}
    );
    check_page(
        commander,
        1,
        "ARMCOM",
        {{"MOVE", Shown::lit},
         {"STOP", Shown::lit},
         {"PATROL", Shown::lit},
         {"DEFEND", Shown::lit},
         {"ATTACK", Shown::lit},
         {"BLAST", Shown::lit}}
    );
    // ARMGEN.GUI puts LOAD and BLAST in one spot, BLAST drawn over LOAD: for
    // a unit that carries none both are greyed.
    check_page(
        non_builder,
        0,
        "ARMPW",
        {{"MOVE", Shown::lit},
         {"STOP", Shown::lit},
         {"PATROL", Shown::lit},
         {"DEFEND", Shown::lit},
         {"ATTACK", Shown::lit},
         {"RECLAIM", Shown::greyed},
         {"REPAIR", Shown::greyed},
         {"CAPTURE", Shown::greyed},
         {"UNLOAD", Shown::greyed},
         {"LOAD", Shown::greyed},
         {"BLAST", Shown::greyed}}
    );
    check_page(
        static_weapon,
        1,
        "ARMAMD",
        {{"MOVE", Shown::greyed},
         {"STOP", Shown::greyed},
         {"PATROL", Shown::greyed},
         {"DEFEND", Shown::greyed},
         {"ATTACK", Shown::greyed},
         {"BLAST", Shown::greyed}}
    );
}

void Runtime::check_command_buttons(uint16_t peewee, uint16_t commander) {
    std::vector<std::string> heard;
    heard_interface_sounds_ = &heard;

    struct StopListening {
        Runtime& runtime;

        ~StopListening() { runtime.heard_interface_sounds_ = nullptr; }
    } stop_listening{*this};

    const auto index_of = [&](std::string_view action) {
        for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_action(match_hud_->layout.gadgets[index].common.name) == action)
                return index;
        throw std::runtime_error(
            "command button check: the order page has no " + std::string(action)
        );
    };
    bool running = true;
    const auto send_pointer = [&](float canvas_x, float canvas_y, bool press) {
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(sdl_.renderer, canvas_x, canvas_y, &window_x, &window_y))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        SDL_Event motion{};
        motion.motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = SDL_GetWindowID(sdl_.window);
        motion.motion.x = window_x;
        motion.motion.y = window_y;
        dispatch_event(motion, running);
        if (!press)
            return;
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.button.type = type;
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch_event(event, running);
        }
    };
    // A click at the button, then the pointer over the top bar, off the
    // buttons.
    const auto click = [&](std::string_view action) {
        const auto& common = match_hud_->layout.gadgets[index_of(action)].common;
        const auto rect = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_, common.x, common.y, common.width, common.height
        );
        send_pointer(
            static_cast<float>(rect.x) + static_cast<float>(rect.width) / 2.0F,
            static_cast<float>(rect.y) + static_cast<float>(rect.height) / 2.0F,
            true
        );
        const auto away = oa::ui::display_layout::source_to_canvas(match_layout_, 300, 8);
        send_pointer(static_cast<float>(away.x), static_cast<float>(away.y), false);
    };
    // Whether the HUD layer shows frame `frame` of the button's art.
    const auto shows_frame = [&](std::size_t index, std::size_t frame) {
        const auto& gadget = match_hud_->layout.gadgets[index];
        const auto* sequence = gaf_sequence(match_hud_->sprites, gadget.common.name);
        if (sequence == nullptr)
            sequence = gaf_sequence(match_hud_->shared_sprites, gadget.common.name);
        require(
            sequence != nullptr && frame < sequence->frames.size(),
            "no art for " + gadget.common.name
        );
        const auto rendered = oa::formats::gaf::render_normal(sequence->frames[frame]);
        require(rendered.ok(), "cannot render " + gadget.common.name);
        const auto& image = *rendered.frame;
        const auto& palette = match_hud_->background.palette ? *match_hud_->background.palette
                                                             : match_hud_->gui_palette;
        for (uint32_t row = 0; row < image.height; ++row)
            for (uint32_t column = 0; column < image.width; ++column) {
                const auto offset = static_cast<std::size_t>(row) * image.width + column;
                if (image.coverage[offset] == 0)
                    continue;
                const auto x = static_cast<std::size_t>(gadget.common.x) + column;
                const auto y = static_cast<std::size_t>(gadget.common.y) + row;
                const auto* shown = match_hud_cpu_.rgb.data() + (y * match_hud_cpu_.width + x) * 3U;
                const auto entry = static_cast<std::size_t>(image.pixels[offset]);
                const auto* colour = palette.data() + entry * oa::palette_entry_bytes;
                if (shown[0] != colour[0] || shown[1] != colour[1] || shown[2] != colour[2])
                    return false;
            }
        return true;
    };
    // A lit button draws the second frame of its art, an unlit one the first.
    const auto lit = [&](std::string_view action) {
        render_match_surface();
        const auto index = index_of(action);
        const bool on = match_command_lit(index);
        if (!shows_frame(index, on ? 1U : 0U)) {
            write_ppm(
                fs::path("local/reports") / "native-match-orders-command-failed.ppm", match_hud_cpu_
            );
            require(
                false, std::string(action) + (on ? " is lit" : " is out") + " but not drawn so"
            );
        }
        return on;
    };
    const auto expect = [&](std::string_view action,
                            MatchCommand command,
                            std::string_view lit_action,
                            std::string_view sound,
                            const char* what) {
        click(action);
        require(match_command_ == command, std::string(what) + ": wrong order armed");
        require(
            !heard.empty() && heard.back() == sound,
            std::string(what) + ": did not play " + std::string(sound)
        );
        for (const std::string_view name :
             {"MOVE", "ATTACK", "PATROL", "DEFEND", "REPAIR", "RECLAIM", "CAPTURE"}) {
            bool shown_on_page = false;
            for (const auto& gadget : match_hud_->layout.gadgets)
                shown_on_page = shown_on_page || (match_hud_action(gadget.common.name) == name &&
                                                  gadget_command_available(gadget));
            if (!shown_on_page)
                continue;
            const bool shown = lit(name);
            require(
                shown == (name == lit_action),
                std::string(what) + ": " + std::string(name) + (shown ? " is lit" : " is not lit")
            );
        }
    };

    check_hud_buttons_under_pointer(peewee, commander);

    clear_local_selection();
    adopt_selection(peewee);
    selected_match_unit_ = peewee;
    apply_match_hud_for_selection();
    reset_match_command();
    expect("ATTACK", MatchCommand::attack, "ATTACK", "immediateorders", "ATTACK");
    expect("MOVE", MatchCommand::move, "MOVE", "immediateorders", "MOVE after ATTACK");
    expect("MOVE", MatchCommand::none, "", "immediateorders", "MOVE clicked again");
    expect("PATROL", MatchCommand::patrol, "PATROL", "immediateorders", "PATROL");
    expect("STOP", MatchCommand::none, "", "immediateorders", "STOP");
    // A held order key presses its button once: MOVE, armed by its quick
    // key's press, stays armed through the repeats that follow, where a
    // second press takes it back.
    const auto* move = std::get_if<oa::ui::gui_layout::ButtonFields>(
        &match_hud_->layout.gadgets[index_of("MOVE")].fields
    );
    require(move != nullptr && move->quick_key != 0, "MOVE has no quick key");
    const auto move_key =
        static_cast<SDL_Keycode>(std::tolower(static_cast<unsigned char>(move->quick_key)));
    const auto press_move_key = [&](bool repeat) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = move_key;
        event.key.down = true;
        event.key.repeat = repeat;
        dispatch_event(event, running);
    };
    constexpr int kMoveKeyRepeats = 3;
    press_move_key(false);
    for (int repeat = 0; repeat < kMoveKeyRepeats; ++repeat)
        press_move_key(true);
    require(match_command_ == MatchCommand::move, "a held MOVE key pressed MOVE again");
    press_move_key(false);
    require(match_command_ == MatchCommand::none, "MOVE's key pressed again left MOVE armed");

    clear_local_selection();
    adopt_selection(commander);
    selected_match_unit_ = commander;
    apply_match_hud_for_selection();
    show_match_orders_page();
    expect("REPAIR", MatchCommand::repair, "REPAIR", "specialorders", "REPAIR");
    expect("RECLAIM", MatchCommand::reclaim, "RECLAIM", "specialorders", "RECLAIM");
    expect("CAPTURE", MatchCommand::capture, "CAPTURE", "specialorders", "CAPTURE");
    expect("CAPTURE", MatchCommand::none, "", "specialorders", "CAPTURE clicked again");
    expect("DEFEND", MatchCommand::guard, "DEFEND", "immediateorders", "DEFEND");
    reset_match_command();
    require(!lit("DEFEND"), "the reset left DEFEND lit");

    // The names resolve through allsound.tdf to BUTTON5.
    for (const auto* name : {"immediateorders", "specialorders"}) {
        const auto* sound = audio_registry_.get(audio_registry_.find(name));
        require(
            sound != nullptr && sound->resource == "sounds/button5.wav",
            std::string(name) + " is not BUTTON5"
        );
    }
    clear_local_selection();
}

void Runtime::check_hud_buttons_under_pointer(uint16_t peewee, uint16_t commander) {
    namespace layout = oa::ui::display_layout;
    const fs::path report_directory = "local/reports";
    bool running = true;
    std::vector<std::string> problems;
    const auto expect = [&](bool condition, std::string what) {
        if (!condition) {
            std::cerr << "hud pointer check: " << what << '\n';
            problems.push_back(std::move(what));
        }
    };
    const auto send = [&](SDL_EventType type, layout::Point canvas) {
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(
                sdl_.renderer,
                static_cast<float>(canvas.x),
                static_cast<float>(canvas.y),
                &window_x,
                &window_y
            ))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto centre = [&](std::size_t index) {
        const auto& common = match_hud_->layout.gadgets[index].common;
        const auto rect = layout::source_rect_to_canvas(
            match_layout_, common.x, common.y, common.width, common.height
        );
        return layout::Point{rect.x + rect.width / 2, rect.y + rect.height / 2};
    };
    const auto away = layout::source_to_canvas(match_layout_, kAwaySourceX, kAwaySourceY);
    // The HUD layer's pixels over a gadget's rectangle, freshly drawn.
    const auto hud_pixels = [&](std::size_t index) {
        render_match_surface();
        const auto& common = match_hud_->layout.gadgets[index].common;
        std::vector<uint8_t> pixels;
        for (int row = std::max(0, static_cast<int>(common.y));
             row < std::min(common.y + common.height, static_cast<int>(match_hud_cpu_.height));
             ++row)
            for (int column = std::max(0, static_cast<int>(common.x));
                 column < std::min(common.x + common.width, static_cast<int>(match_hud_cpu_.width));
                 ++column) {
                const auto* shown = match_hud_cpu_.rgb.data() +
                                    (static_cast<std::size_t>(row) * match_hud_cpu_.width +
                                     static_cast<std::size_t>(column)) *
                                        3U;
                pixels.insert(pixels.end(), shown, shown + 3);
            }
        return pixels;
    };
    // The frame as presented, the pointer drawn.
    const auto snapshot = [&](const char* name) {
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(report_directory / name, presented);
    };
    const auto select = [&](uint16_t id) {
        clear_local_selection();
        adopt_selection(id);
        selected_match_unit_ = id;
        apply_match_hud_for_selection();
        reset_match_command();
    };
    const auto index_of = [&](std::string_view action) {
        for (std::size_t index = 0; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_action(match_hud_->layout.gadgets[index].common.name) == action)
                return index;
        throw std::runtime_error("hud pointer check: the order page has no " + std::string(action));
    };

    // A build button of the commander's build page under the pointer is drawn
    // as it is without it. ORDERS left the commander on its order page,
    // which it keeps, so BUILD opens its build page again.
    select(commander);
    const auto build_tab = centre(index_of("BUILD"));
    send(SDL_EVENT_MOUSE_MOTION, build_tab);
    send(SDL_EVENT_MOUSE_BUTTON_DOWN, build_tab);
    send(SDL_EVENT_MOUSE_BUTTON_UP, build_tab);
    std::optional<std::size_t> build;
    for (std::size_t index = 0; index < match_hud_->layout.gadgets.size() && !build; ++index) {
        const auto& gadget = match_hud_->layout.gadgets[index];
        if ((gadget.common.common_attributes & oa::ui::hud::kCommonUnitButton) != 0 &&
            gadget_command_available(gadget))
            build = index;
    }
    require(build.has_value(), "the commander's build page has no unit button");
    // Copied: selecting another unit below loads another layout, which frees
    // this one's gadgets.
    const std::string build_name = match_hud_->layout.gadgets[*build].common.name;
    send(SDL_EVENT_MOUSE_MOTION, away);
    const auto idle_build = hud_pixels(*build);
    send(SDL_EVENT_MOUSE_MOTION, centre(*build));
    require(hovered_ == *build, "the pointer is not over " + build_name);
    snapshot("native-match-orders-build-hover.ppm");
    expect(hud_pixels(*build) == idle_build, build_name + " changes under the pointer");

    // ATTACK of the ARMPW's order page: drawn as it is under the pointer,
    // pressed while a press is held over it, raised while the pointer is off
    // it, and a release away from it arms nothing.
    select(peewee);
    const auto attack = index_of("ATTACK");
    send(SDL_EVENT_MOUSE_MOTION, away);
    const auto idle_attack = hud_pixels(attack);
    send(SDL_EVENT_MOUSE_MOTION, centre(attack));
    require(hovered_ == attack, "the pointer is not over ATTACK");
    snapshot("native-match-orders-attack-hover.ppm");
    expect(hud_pixels(attack) == idle_attack, "ATTACK changes under the pointer");
    send(SDL_EVENT_MOUSE_BUTTON_DOWN, centre(attack));
    snapshot("native-match-orders-attack-held.ppm");
    expect(hud_pixels(attack) != idle_attack, "ATTACK held under the pointer is not pressed");
    send(SDL_EVENT_MOUSE_MOTION, away);
    expect(hud_pixels(attack) == idle_attack, "ATTACK stays pressed with the pointer off it");
    send(SDL_EVENT_MOUSE_MOTION, centre(attack));
    expect(hud_pixels(attack) != idle_attack, "ATTACK is not pressed when the pointer is back");
    send(SDL_EVENT_MOUSE_MOTION, away);
    send(SDL_EVENT_MOUSE_BUTTON_UP, away);
    expect(match_command_ == MatchCommand::none, "ATTACK released away from it armed an order");
    expect(hud_pixels(attack) == idle_attack, "ATTACK stays pressed after the release");
    // A press begun off ATTACK neither presses it nor, released over it,
    // arms it; a click on it does.
    send(SDL_EVENT_MOUSE_BUTTON_DOWN, away);
    send(SDL_EVENT_MOUSE_MOTION, centre(attack));
    expect(hud_pixels(attack) == idle_attack, "a press begun off ATTACK presses it");
    send(SDL_EVENT_MOUSE_BUTTON_UP, centre(attack));
    expect(
        match_command_ == MatchCommand::none,
        "a press begun off ATTACK and released over it armed an order"
    );
    send(SDL_EVENT_MOUSE_BUTTON_DOWN, centre(attack));
    send(SDL_EVENT_MOUSE_BUTTON_UP, centre(attack));
    expect(match_command_ == MatchCommand::attack, "a click on ATTACK did not arm it");
    reset_match_command();
    send(SDL_EVENT_MOUSE_MOTION, away);
    if (!problems.empty())
        throw std::runtime_error("hud pointer check: " + problems.front());
    std::cout << "hud pointer check: " << build_name
              << " and ATTACK are drawn as they are under the pointer; ATTACK is pressed while "
                 "held over it and raised off it, and only a press begun on it arms it\n";
}

void Runtime::check_unit_damage_bar(uint16_t peewee) {
    auto& world = match_->state();
    auto& game = world.game;
    require(
        game.ui_colors[4] == 213 && game.ui_colors[10] == 233 && game.ui_colors[12] == 211 &&
            game.ui_colors[14] == 194 && game.ui_colors[15] == 255,
        "the UI colour table is not guipal on PALETTE.PAL"
    );
    const auto& bar = side_hud_.damage_bar;
    require(bar.width > 1 && bar.height > 0, "SIDEDATA names no DAMAGEBAR");
    const auto colour_at = [&](int x, int y) {
        const auto offset =
            static_cast<std::size_t>(y) * match_hud_cpu_.width + static_cast<std::size_t>(x);
        const auto* pixel = match_hud_cpu_.rgb.data() + offset * 3U;
        return std::array<uint8_t, 3>{pixel[0], pixel[1], pixel[2]};
    };
    const auto palette_colour = [&](uint8_t index) {
        const auto at = static_cast<std::size_t>(index) * 4U;
        return std::array<uint8_t, 3>{
            match_palette_[at], match_palette_[at + 1], match_palette_[at + 2]
        };
    };
    const auto bar_row = [&] {
        std::vector<std::array<uint8_t, 3>> row;
        for (int x = bar.x; x < bar.x + bar.width; ++x)
            row.push_back(colour_at(x, bar.y));
        return row;
    };
    // The unit panel shows the unit under the cursor (Game.cursor_unit_id),
    // which the pointer's pick sets.
    const auto show = [&](uint16_t id) {
        clear_local_selection();
        selected_match_unit_ = 0;
        game.cursor_unit_id = id;
        hovered_match_unit_ = id;
        render_match_surface();
        return bar_row();
    };

    auto& peewee_unit = match_->world().slots[peewee].record;
    const auto* def = oa::world_unit_def_of(&world, &peewee_unit);
    require(def != nullptr && def->max_damage > 1, "ARMPW has no max_damage");
    const auto saved_health = peewee_unit.health;
    peewee_unit.health = static_cast<int16_t>(def->max_damage / 2U);
    const auto own = show(peewee);
    peewee_unit.health = saved_health;
    const auto maximum = static_cast<int>(def->max_damage);
    const auto split = (bar.width - 1) * (maximum / 2) / maximum;
    for (int column = 0; column < bar.width; ++column) {
        const int slot = column <= split ? 10 : 4;
        require(
            own[static_cast<std::size_t>(column)] == palette_colour(game.ui_colors[slot]),
            "damage bar column " + std::to_string(column) + " is not UI colour " +
                std::to_string(slot)
        );
    }

    uint16_t enemy_commander = 0;
    for (const auto& slot : match_->world().slots) {
        const auto* type =
            slot.unit != nullptr ? oa::world_unit_def_of(&world, &slot.record) : nullptr;
        if (type != nullptr && slot.record.owner_index != match_local_player_ &&
            (type->flags & OA_UNIT_DEF_FLAG_HIDE_DAMAGE) != 0 &&
            (slot.record.flags & OA_UNIT_FLAG_LIVE) != 0) {
            enemy_commander = slot.unit_index;
            break;
        }
    }
    require(enemy_commander != 0, "no enemy unit whose type hides damage");
    const auto nothing = show(0);
    const auto hidden = show(enemy_commander);
    require(hidden == nothing, "an enemy commander's damage bar was drawn");
    const auto viewpoint = game.viewpoint_player;
    game.viewpoint_player = match_->world().slots[enemy_commander].record.owner_index;
    const auto owned = show(enemy_commander);
    game.viewpoint_player = viewpoint;
    require(
        owned != nothing && owned.front() == palette_colour(game.ui_colors[10]),
        "the commander's owner does not see its damage bar"
    );
    game.cursor_unit_id = 0;
    hovered_match_unit_ = 0;
}

void Runtime::check_order_overlays_over_fog(uint16_t commander, uint16_t scout) {
    namespace visibility_flag = oa::ui::console::visibility_flag;
    const fs::path report_directory = "local/reports";
    require(
        match_line_of_sight_on() && match_mapping_on(),
        "the skirmish plays without line of sight or mapping"
    );
    const auto run_ticks = [&](int ticks) {
        for (int tick = 0; tick < ticks; ++tick)
            step_match_simulation();
    };
    run_ticks(scout_ticks);
    match_->kill_unit(scout, static_cast<uint8_t>(oa::sim::match_runtime::DeathKind::dismissed));
    run_ticks(sight_lapse_ticks);
    match_->stop_orders(commander);
    clear_local_selection();
    adopt_selection(commander);
    selected_match_unit_ = commander;
    apply_match_hud_for_selection();
    const auto& position = match_->world().slots[commander].unit->position;
    match_camera_x_ = static_cast<int32_t>(position[0] >> 16) - fog_view_west;
    match_camera_z_ = static_cast<int32_t>(position[2] >> 16) - fog_view_north;
    // The render keeps the camera on the map.
    render_match_surface();
    const int32_t camera_x = match_camera_x_;
    const int32_t camera_z = match_camera_z_;
    const auto width = static_cast<int32_t>(match_world_cpu_.width);
    const auto height = static_cast<int32_t>(match_world_cpu_.height);
    const double zoom = match_zoom();

    // The fog over a battlefield box, from the sight cells the corners of
    // every fog tile over it sit on: clear, gray or black alike, or mixed.
    enum class Fog { clear, gray, black, mixed };
    const auto& sight = match_->sight();
    const auto coverage = match_->player_coverage(match_view_player());
    const auto viewer_bit = static_cast<uint16_t>(1U << sight.viewpoint_player);
    const auto fog_over = [&](int32_t x, int32_t y) {
        const auto map_cell = [&](int32_t camera, int32_t screen) {
            constexpr int32_t cell = oa::present::world_renderer::fog_cell_pixels;
            return (camera + static_cast<int32_t>(std::lround(screen / zoom))) / cell;
        };
        std::optional<Fog> found;
        for (int32_t z = map_cell(camera_z, y - marker_reach) - 1;
             z <= map_cell(camera_z, y + marker_reach) + 1;
             ++z)
            for (int32_t column = map_cell(camera_x, x - marker_reach) - 1;
                 column <= map_cell(camera_x, x + marker_reach) + 1;
                 ++column) {
                if (column < 0 || z < 0 || column >= sight.width || z >= sight.height)
                    return Fog::mixed;
                const auto index =
                    static_cast<std::size_t>(z) * static_cast<std::size_t>(sight.width) +
                    static_cast<std::size_t>(column);
                const bool seen =
                    index < coverage.size() &&
                    oa::sim::visibility_state::count_in_sight(
                        coverage[index], oa::sim::visibility_state::SightReading::player
                    );
                const bool mapped = index < sight.player_bits.size() &&
                                    (sight.player_bits[index] & viewer_bit) != 0;
                const auto here = !mapped ? Fog::black : seen ? Fog::clear : Fog::gray;
                if (found.has_value() && *found != here)
                    return Fog::mixed;
                found = here;
            }
        return found.value_or(Fog::mixed);
    };

    // A move onto the first ground of that fog found in the view: where it
    // goes, and the battlefield point its target marker is drawn at.
    struct Target {
        const char* ground{};
        oa::sim::ground_orders::Point destination{};
        int32_t x{};
        int32_t y{};
    };

    std::vector<Target> targets;
    for (const auto& [fog, ground] :
         {std::pair{Fog::gray, "gray"}, std::pair{Fog::black, "black"}}) {
        std::optional<Target> target;
        for (int32_t y = marker_reach; !target && y < height - marker_reach; y += fog_search_step)
            for (int32_t x = marker_reach; !target && x < width - marker_reach;
                 x += fog_search_step) {
                if (fog_over(x, y) != fog)
                    continue;
                // The marker is drawn half its point's height up the view
                // from the point, so the point lies that far south of the
                // ground found.
                const auto map_x = camera_x + static_cast<int32_t>(std::lround(x / zoom));
                const auto ground_z = camera_z + static_cast<int32_t>(std::lround(y / zoom));
                const auto height_at = [&](int32_t z) {
                    return match_->map_height(
                        static_cast<uint32_t>(map_x) << 16, static_cast<uint32_t>(z) << 16
                    );
                };
                const auto map_z = ground_z + height_at(ground_z) / 2;
                const auto point_height = height_at(map_z);
                const Target placed{
                    ground,
                    {map_x << 16, point_height << 16, map_z << 16},
                    static_cast<int32_t>(std::lround((map_x - camera_x) * zoom)),
                    static_cast<int32_t>(
                        std::lround((map_z - (point_height >> 1) - camera_z) * zoom)
                    )
                };
                if (fog_over(placed.x, placed.y) == fog)
                    target = placed;
            }
        require(target.has_value(), std::string("the view shows no ") + ground + " ground");
        targets.push_back(*target);
    }

    // Each frame with Shift held or not, and with the fog or without it.
    auto& visibility = match_->state().game.visibility_flags;
    const auto kept_visibility = visibility;
    const auto frame = [&](bool shift, bool fog) {
        // Shift is let go and the fog restored however the render ends.
        struct Restore {
            Runtime& runtime;
            uint8_t& visibility;
            uint8_t kept;

            ~Restore() {
                runtime.shift_held_by_check_ = false;
                visibility = kept;
            }
        } restore{*this, visibility, kept_visibility};
        shift_held_by_check_ = shift;
        if (!fog)
            visibility = static_cast<uint8_t>(
                visibility & ~(visibility_flag::line_of_sight | visibility_flag::mapping)
            );
        render_match_surface();
        return match_world_cpu_;
    };
    // The ground before the moves are queued, then the four frames with them.
    const auto unordered_plain = frame(false, false);
    for (const auto& target : targets)
        match_->issue_ground_move(commander, target.destination, true);
    const auto fogged = frame(true, true);
    const auto clear = frame(true, false);
    const auto fogged_plain = frame(false, true);
    const auto clear_plain = frame(false, false);
    const auto pixel = [](const renderer::Surface& surface, int32_t x, int32_t y) {
        const auto* at =
            surface.rgb.data() +
            (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3U;
        return std::array<uint8_t, 3>{at[0], at[1], at[2]};
    };
    // Without Shift the moves change no pixel. With it, a marker's pixels are
    // those the overlays change in the frame without fog: over the fog each
    // must show as it does there, and every other pixel keeps the fog it has
    // without Shift.
    std::string failures;
    const auto fail = [&](const std::string& what) {
        failures += (failures.empty() ? "" : "; ") + what;
    };
    for (const auto& target : targets) {
        const std::string move = std::string("the move onto ") + target.ground + " ground";
        std::size_t unshifted = 0;
        std::size_t drawn = 0;
        std::size_t shown = 0;
        std::size_t fogged_ground = 0;
        std::size_t fog_changed = 0;
        for (int32_t y = std::max(0, target.y - marker_reach);
             y <= std::min(height - 1, target.y + marker_reach);
             ++y)
            for (int32_t x = std::max(0, target.x - marker_reach);
                 x <= std::min(width - 1, target.x + marker_reach);
                 ++x) {
                unshifted += pixel(clear_plain, x, y) != pixel(unordered_plain, x, y);
                if (pixel(clear, x, y) == pixel(clear_plain, x, y)) {
                    fogged_ground += pixel(fogged_plain, x, y) != pixel(clear_plain, x, y);
                    fog_changed += pixel(fogged, x, y) != pixel(fogged_plain, x, y);
                    continue;
                }
                ++drawn;
                shown += pixel(fogged, x, y) == pixel(clear, x, y);
            }
        if (unshifted != 0)
            fail(move + " changes " + std::to_string(unshifted) + " pixels without Shift held");
        else if (drawn < marker_pixels)
            fail(
                move + " changes " + std::to_string(drawn) +
                " pixels with Shift held, too few for its marker"
            );
        if (fogged_ground == 0)
            fail("the fog leaves the ground around " + move + " clear");
        if (shown != drawn)
            fail(
                "the marker of " + move + " shows " + std::to_string(shown) + " of its " +
                std::to_string(drawn) + " pixels over the fog"
            );
        if (fog_changed != 0)
            fail(
                "around the marker of " + move + ", " + std::to_string(fog_changed) +
                " pixels of fog change while Shift is held"
            );
    }
    match_->stop_orders(commander);
    clear_local_selection();
    if (!failures.empty()) {
        write_ppm(report_directory / "native-match-orders-order-fog.ppm", fogged);
        require(false, failures);
    }
    std::cout << "order overlay fog check: with Shift held, the target markers of moves queued "
                 "onto gray and black ground show over the fog as they do without it, and the "
                 "fog around them stays; without Shift the moves draw nothing\n";
}

} // namespace oa::app
