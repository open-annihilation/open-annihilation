// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A factory's movement orders given through synthetic SDL input and carried
// out by the units it builds.
#include "oa/app/runtime.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

// Canvas pixels from the factory to the points it is sent to.
constexpr int kRallyOffset = 160;
// Ticks the lab gets to build one Peewee at full resources (about 440).
constexpr int kBuildTickLimit = 1200;
// Ticks the new Peewee walks before it must have closed on the point.
constexpr int kWalkTicks = 150;

int64_t
squared_distance(const std::array<uint32_t, 3>& position, const oa::sim::ground_orders::Point& to) {
    const auto x = static_cast<int64_t>(std::bit_cast<int32_t>(position[0]) >> 16);
    const auto z = static_cast<int64_t>(std::bit_cast<int32_t>(position[2]) >> 16);
    const auto dx = x - (to[0] >> 16);
    const auto dz = z - (to[2] >> 16);
    return dx * dx + dz * dz;
}

[[noreturn]] void fail(std::string_view what, std::string_view how = {}) {
    throw std::runtime_error(
        "factory order check: " + std::string(what) + (how.empty() ? "" : " ") + std::string(how)
    );
}

} // namespace

void Runtime::check_factory_orders() {
    namespace orders = oa::sim::match_runtime;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    start_benchmark_skirmish();
    apply_output_mode();
    const auto lab = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMLAB");
    const auto peewee = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW");
    if (lab == 0 || peewee == 0 || lab >= spawn_types_.size())
        fail("lacks ARMLAB or ARMPW");
    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    if (commander == 0)
        fail("found no local commander");

    auto* placed = place_finished_structure(lab, commander);
    if (placed == nullptr)
        fail("could not place ARMLAB near the commander");
    const auto factory = placed->unit_index;

    auto& player = match_->world().players[match_local_player_];
    const auto tick = [&] {
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
        step_match_simulation();
    };
    bool running = true;
    const auto send = [&](SDL_EventType type, uint8_t button, float x, float y) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto click = [&](float x, float y, uint8_t button) {
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, button, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, button, x, y);
    };
    const auto click_gadget = [&](std::string_view name) {
        if (!match_hud_)
            fail("has no order panel");
        for (const auto& gadget : match_hud_->layout.gadgets) {
            const auto& common = gadget.common;
            if (common.name != name)
                continue;
            const auto point = oa::ui::display_layout::source_to_canvas(
                match_layout_, common.x + common.width / 2, common.y + common.height / 2
            );
            click(static_cast<float>(point.x), static_cast<float>(point.y), SDL_BUTTON_LEFT);
            return;
        }
        fail("found no lab button", name);
    };
    const auto primary_queue = [&](uint16_t id) {
        std::vector<orders::Match::QueuedCommandView> queue;
        match_->visit_primary_queue(id, [&](const auto& view) { queue.push_back(view); });
        return queue;
    };

    center_camera_on_unit(factory);
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const auto on_screen = project_match_point(viewport, slots[factory].unit->position);
    const auto factory_x = static_cast<float>(on_screen.x);
    const auto factory_y = static_cast<float>(on_screen.y);
    click(factory_x, factory_y, SDL_BUTTON_LEFT);
    if (selected_match_unit_ != factory ||
        (slots[factory].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        fail("did not select the lab with a click");

    std::vector<uint16_t> built;
    // The lab is sent with `button` armed and a left click; it must hold one
    // `held` order at the point, and the Peewee's first order must be `taken`
    // to the point.
    const auto send_lab = [&](float x,
                              float y,
                              std::string_view button,
                              MatchCommand armed,
                              uint8_t held,
                              uint8_t taken,
                              std::string_view how) {
        const auto point = match_world_point(x, y);
        update_pointer(x, y);
        if (!point || hovered_match_unit_ != 0)
            fail("found no open ground for the point", how);
        click_gadget(button);
        if (match_command_ != armed)
            fail("did not arm the command of", button);
        click(x, y, SDL_BUTTON_LEFT);
        if (match_command_ != MatchCommand::none)
            fail("kept the command armed after the click of", button);
        std::size_t kept = 0;
        bool holds = false;
        for (const auto& view : primary_queue(factory)) {
            if (view.kind == orders::qmove_kind || view.kind == orders::qpatrol_kind)
                ++kept;
            if (view.kind == held)
                holds = view.destination == *point;
        }
        if (kept != 1 || !holds)
            fail("did not leave the lab one order at the point", how);
        click_gadget("ARMPW");
        if (match_->queued_build_count(factory, peewee) != 1)
            fail("queued no ARMPW with the build button");
        uint16_t product = 0;
        for (int step = 0; step < kBuildTickLimit && product == 0; ++step) {
            tick();
            for (const auto& slot : slots) {
                if (slot.unit == nullptr || slot.record.type_index != peewee ||
                    slot.record.owner_index != match_local_player_ ||
                    slot.record.build_remaining != 0.0F ||
                    std::find(built.begin(), built.end(), slot.unit_index) != built.end())
                    continue;
                const auto queue = primary_queue(slot.unit_index);
                if (!queue.empty() && queue.front().kind != orders::get_built_kind)
                    product = slot.unit_index;
            }
        }
        if (product == 0)
            fail("saw no ARMPW leave the lab sent to the point", how);
        const auto queue = primary_queue(product);
        if (queue.front().kind != taken || queue.front().destination != *point ||
            (taken == oa::sim::ground_orders::move_ground_kind && queue.size() != 1))
            fail("found the ARMPW without the lab's order to the point", how);
        const auto start = squared_distance(slots[product].unit->position, *point);
        for (int step = 0; step < kWalkTicks; ++step)
            tick();
        if (squared_distance(slots[product].unit->position, *point) >= start)
            fail("saw the ARMPW stay away from the point", how);
        built.push_back(product);
    };
    const auto offset = static_cast<float>(kRallyOffset);
    const auto move_ground = oa::sim::ground_orders::move_ground_kind;
    send_lab(
        factory_x + offset,
        factory_y,
        "ARMMOVE",
        MatchCommand::move,
        orders::qmove_kind,
        move_ground,
        "given with MOVE"
    );
    send_lab(
        factory_x - offset,
        factory_y,
        "ARMPATROL",
        MatchCommand::patrol,
        orders::qpatrol_kind,
        orders::patrol_kind,
        "given with PATROL"
    );
    std::cout << "factory order check: the lab's MOVE and PATROL points were taken by the "
                 "Peewees it built\n";
}

} // namespace oa::app
