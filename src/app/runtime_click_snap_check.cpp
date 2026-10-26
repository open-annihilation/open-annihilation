// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The click snap (ui.click-snap) through SDL input on dummy devices: the
// cursor RECLAIM shows over open ground beside a wreck, a RECLAIM click
// there, plain, queued with Shift and with the snap override key held, one
// beyond the wreck radius and a click there with no order armed, and a
// Metal Extractor's build click beside a metal deposit; with the hack off,
// that nothing snaps.
#include "oa/app/runtime.hpp"
#include "device_state.hpp"
#include "oa/core/map_plot.h"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace input = oa::sim::gameplay_input;
namespace orders = oa::sim::match_runtime;

/// The wreck placed beside the commander, and the nearest and farthest
/// cells from the commander its site is searched in.
constexpr std::string_view kWreckName = "armsolar_dead";
constexpr int32_t kWreckNearest = 6;
constexpr int32_t kSearchCells = 40;
/// Cells of open ground kept around the wreck's footprint, so that the
/// clicks beside it land on ground with no feature.
constexpr int32_t kOpenMargin = 3;
/// The wreck and mex radii the check's profile gives, in cells.
constexpr int32_t kWreckRadius = 1;
constexpr int32_t kMexRadius = 3;
constexpr uint16_t kNoFeature = 0xffff;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("click snap check: " + what);
}

std::string cell_text(int32_t x, int32_t z) {
    return std::to_string(x) + "," + std::to_string(z);
}

} // namespace

void Runtime::check_click_snap() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    start_benchmark_skirmish();
    apply_output_mode();
    // Without a profile that turns the hack on, the check shows that
    // nothing snaps.
    const bool snapping = ui_rules().click_snap.enabled;
    if (snapping ? view_settings_.wreck_snap_radius != kWreckRadius ||
                       view_settings_.mex_snap_radius != kMexRadius
                 : view_settings_.wreck_snap_radius != 0 || view_settings_.mex_snap_radius != 0)
        fail(
            "needs ui.click-snap off, or on with wreck radius " + std::to_string(kWreckRadius) +
            " and mex radius " + std::to_string(kMexRadius) + "; plays wreck " +
            std::to_string(view_settings_.wreck_snap_radius) + ", mex " +
            std::to_string(view_settings_.mex_snap_radius)
        );
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
    auto& world = match_->state();
    const auto commander_x = static_cast<int32_t>(slots[commander].unit->position[0] >> 20);
    const auto commander_z = static_cast<int32_t>(slots[commander].unit->position[2] >> 20);

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
    const auto click = [&](std::array<float, 2> point) {
        send(SDL_EVENT_MOUSE_MOTION, 0, point[0], point[1]);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, point[0], point[1]);
        send(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, point[0], point[1]);
    };
    const auto viewport = [&] { return live_viewport(match_camera_x_, match_camera_z_); };
    // The pointer over the middle of a cell, at the ground's height there.
    const auto cell_pointer = [&](int32_t cell_x, int32_t cell_z) {
        const auto x = static_cast<uint32_t>(cell_x * OA_MAP_CELL_PIXELS + OA_MAP_CELL_PIXELS / 2);
        const auto z = static_cast<uint32_t>(cell_z * OA_MAP_CELL_PIXELS + OA_MAP_CELL_PIXELS / 2);
        const auto height = std::max(
            match_->map_height(x << 16U, z << 16U), static_cast<int32_t>(world.game.sea_level)
        );
        const auto screen = project_match_point(
            viewport(), {x << 16U, static_cast<uint32_t>(height) << 16U, z << 16U}
        );
        return std::array<float, 2>{static_cast<float>(screen.x), static_cast<float>(screen.y)};
    };
    const auto look_at_cell = [&](int32_t cell_x, int32_t cell_z) {
        match_camera_x_ = cell_x * OA_MAP_CELL_PIXELS - visible_map_width() / 2;
        match_camera_z_ = cell_z * OA_MAP_CELL_PIXELS - visible_map_height() / 2;
        render_match_surface();
    };
    const auto queue_of = [&](uint16_t unit) {
        std::vector<orders::Match::QueuedCommandView> queue;
        match_->visit_primary_queue(unit, [&](const auto& order) { queue.push_back(order); });
        return queue;
    };

    // The whole map in sight, so that every cell the check clicks is shown.
    chat_buffer_ = "+NowISee";
    submit_chat_line();

    // The commander, selected with a click.
    center_camera_on_unit(commander);
    render_match_surface();
    const auto at_commander = project_match_point(viewport(), slots[commander].unit->position);
    click({static_cast<float>(at_commander.x), static_cast<float>(at_commander.y)});
    if (selected_match_unit_ != commander ||
        (slots[commander].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        fail("did not select the commander with a click");

    // A wreck placed on open land with open ground around it.
    const auto wreck_index =
        oa::sim::map_runtime::find_feature_index(feature_table_, kWreckName.data());
    if (wreck_index >= world.feature_def_count)
        fail("found no " + std::string(kWreckName) + " definition");
    const auto& wreck_def = world.feature_defs[wreck_index];
    const auto sea = static_cast<int32_t>(world.game.sea_level);
    const auto open_land = [&](int32_t x, int32_t z) {
        const auto* plot = oa::world_plot(&world, x, z);
        return plot != nullptr && plot->feature == kNoFeature && plot->ground_unit == 0 &&
               match_->map_height(
                   static_cast<uint32_t>(x * OA_MAP_CELL_PIXELS + 8) << 16U,
                   static_cast<uint32_t>(z * OA_MAP_CELL_PIXELS + 8) << 16U
               ) > sea;
    };
    std::optional<std::array<int32_t, 2>> wreck;
    for (int32_t ring = kWreckNearest; ring < kSearchCells && !wreck; ++ring)
        for (int32_t dz = -ring; dz <= ring && !wreck; ++dz)
            for (int32_t dx = -ring; dx <= ring && !wreck; ++dx) {
                if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                    continue;
                const auto x = commander_x + dx;
                const auto z = commander_z + dz;
                bool open = true;
                for (int32_t row = -kOpenMargin; open && row < wreck_def.footprint_z + kOpenMargin;
                     ++row)
                    for (int32_t column = -kOpenMargin;
                         open && column < wreck_def.footprint_x + kOpenMargin;
                         ++column)
                        open = open_land(x + column, z + row);
                if (open && console_place_feature(kWreckName.data(), x, z))
                    wreck = std::array<int32_t, 2>{x, z};
            }
    if (!wreck)
        fail("found no open land for " + std::string(kWreckName));
    const auto wreck_x = (*wreck)[0];
    const auto wreck_z = (*wreck)[1];
    const auto wreck_text = std::string(kWreckName) + " at " + cell_text(wreck_x, wreck_z);
    const auto on_wreck = [&](const orders::Match::QueuedCommandView& order) {
        const auto column = (order.destination[0] >> 20) - wreck_x;
        const auto row = (order.destination[2] >> 20) - wreck_z;
        return order.kind == orders::reclaim_kind && column >= 0 && row >= 0 &&
               column < wreck_def.footprint_x && row < wreck_def.footprint_z;
    };

    // The cells beside the wreck: one cell west of its footprint, within the
    // wreck radius, and two cells west, beyond it.
    const auto beside_z = wreck_z + wreck_def.footprint_z / 2;
    const std::array<int32_t, 2> near_cell{wreck_x - kWreckRadius, beside_z};
    const std::array<int32_t, 2> far_cell{wreck_x - kWreckRadius - 1, beside_z};
    look_at_cell(wreck_x, wreck_z);
    const auto pointer_on = [&](std::array<int32_t, 2> cell) {
        const auto point = cell_pointer(cell[0], cell[1]);
        send(SDL_EVENT_MOUSE_MOTION, 0, point[0], point[1]);
        const auto ground = match_world_point(point[0], point[1]);
        if (hovered_ || hovered_match_unit_ != 0 || !ground || ((*ground)[0] >> 20) != cell[0] ||
            ((*ground)[2] >> 20) != cell[1])
            fail("the pointer missed the open cell " + cell_text(cell[0], cell[1]));
        if (input::drawn_feature(world, {(*ground)[0], (*ground)[1], (*ground)[2]}))
            fail("a feature is drawn over the open cell " + cell_text(cell[0], cell[1]));
        return point;
    };
    const auto near_point = pointer_on(near_cell);
    const auto far_point = pointer_on(far_cell);

    // With RECLAIM armed, the cursor over open ground beside the wreck.
    const auto expect_cursor =
        [&](std::array<float, 2> point, input::OrderCursor expected, const std::string& where) {
            match_command_ = MatchCommand::reclaim;
            send(SDL_EVENT_MOUSE_MOTION, 0, point[0], point[1]);
            const auto shown = static_cast<input::OrderCursor>(pick_match_cursor());
            if (shown != expected)
                fail(
                    "with RECLAIM armed, the cursor " + where + " is " +
                    std::to_string(static_cast<int>(shown)) + ", not " +
                    std::to_string(static_cast<int>(expected))
                );
        };
    if (!snapping) {
        expect_cursor(
            near_point,
            input::OrderCursor::normal,
            "with ui.click-snap off one cell beside " + wreck_text
        );
        match_->stop_orders(commander);
        click(near_point);
        if (!queue_of(commander).empty())
            fail("with ui.click-snap off, a RECLAIM click beside " + wreck_text + " gave an order");
        std::cout << "click snap check: with ui.click-snap off, RECLAIM one cell beside "
                  << wreck_text << " shows the arrow and a click gives nothing\n";
        reset_match_command();
        return;
    }
    // The Reclaim cursor shows where a click snaps, and the arrow where it
    // does not: with the snap override key held, beyond the radius, or with
    // the player's wreck radius at 0.
    expect_cursor(near_point, input::OrderCursor::reclaim, "one cell beside " + wreck_text);
    device_state::hold_key(SDL_SCANCODE_LALT, true);
    expect_cursor(
        near_point, input::OrderCursor::normal, "with Alt held one cell beside " + wreck_text
    );
    device_state::hold_key(SDL_SCANCODE_LALT, false);
    expect_cursor(far_point, input::OrderCursor::normal, "two cells beside " + wreck_text);
    const auto chosen_radius = view_settings_.wreck_snap_radius;
    view_settings_.wreck_snap_radius = 0;
    expect_cursor(
        near_point,
        input::OrderCursor::normal,
        "with a wreck radius of 0 one cell beside " + wreck_text
    );
    view_settings_.wreck_snap_radius = chosen_radius;
    std::cout << "click snap check: with RECLAIM armed, the cursor one cell beside " << wreck_text
              << " shows Reclaim, and the arrow with Alt held, two cells away and with a wreck "
                 "radius of 0\n";

    // A RECLAIM click one cell beside the wreck reclaims it.
    match_->stop_orders(commander);
    match_command_ = MatchCommand::reclaim;
    click(near_point);
    {
        const auto queue = queue_of(commander);
        if (queue.size() != 1 || !on_wreck(queue.front()))
            fail(
                "a RECLAIM click on open ground one cell beside " + wreck_text +
                " gave the commander no Reclaim of it (status \"" + status_ + "\")"
            );
    }
    std::cout << "click snap check: a RECLAIM click one cell beside " << wreck_text
              << " reclaims it\n";

    // Queued with Shift, the snapped Reclaim follows the commander's Move.
    match_->stop_orders(commander);
    match_->issue_ground_move(
        commander, {(commander_x * 16 + 8) << 16, 0, ((commander_z + 4) * 16 + 8) << 16}, false
    );
    match_command_ = MatchCommand::reclaim;
    device_state::hold_key(SDL_SCANCODE_LSHIFT, true);
    click(near_point);
    device_state::hold_key(SDL_SCANCODE_LSHIFT, false);
    {
        const auto queue = queue_of(commander);
        if (queue.size() != 2 || queue.front().kind != oa::sim::ground_orders::move_ground_kind ||
            !on_wreck(queue.back()))
            fail(
                "a Shift RECLAIM click one cell beside " + wreck_text +
                " did not queue its Reclaim after the Move"
            );
    }
    std::cout << "click snap check: a Shift RECLAIM click one cell beside " << wreck_text
              << " queues its Reclaim\n";
    // RECLAIM, kept armed by Shift, goes as Shift is let go.
    reset_match_command();

    // With the snap override key held the click is placed exactly: no
    // feature there, no Reclaim.
    match_->stop_orders(commander);
    match_command_ = MatchCommand::reclaim;
    device_state::hold_key(SDL_SCANCODE_LALT, true);
    click(near_point);
    device_state::hold_key(SDL_SCANCODE_LALT, false);
    if (const auto queue = queue_of(commander); !queue.empty())
        fail(
            "a RECLAIM click with Alt held one cell beside " + wreck_text + " gave order kind " +
            std::to_string(queue.front().kind) + " at " +
            cell_text(queue.front().destination[0] >> 20, queue.front().destination[2] >> 20)
        );
    // Beyond the radius nothing snaps.
    match_->stop_orders(commander);
    match_command_ = MatchCommand::reclaim;
    click(far_point);
    if (!queue_of(commander).empty())
        fail("a RECLAIM click two cells beside " + wreck_text + " gave an order");
    std::cout << "click snap check: with Alt held, and two cells beside " << wreck_text
              << ", a RECLAIM click gives nothing\n";

    // With no order armed the click is not a reclaim click: the default
    // order over open ground beside the wreck is a Move there.
    match_->stop_orders(commander);
    match_command_ = MatchCommand::none;
    click(near_point);
    if (const auto queue = queue_of(commander);
        queue.size() != 1 || queue.front().kind != oa::sim::ground_orders::move_ground_kind ||
        (queue.front().destination[0] >> 20) != near_cell[0] ||
        (queue.front().destination[2] >> 20) != near_cell[1])
        fail("a click with no order armed one cell beside " + wreck_text + " gave no Move there");
    std::cout << "click snap check: with no order armed, a click one cell beside " << wreck_text
              << " moves there\n";
    match_->stop_orders(commander);

    // A Metal Extractor's build click beside a deposit moves onto it.
    const auto mex = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMMEX");
    if (mex == 0 || mex >= spawn_types_.size())
        fail("the game has no ARMMEX");
    const auto& spatial = match_->spatial();
    const auto map_width = static_cast<int32_t>(spatial.terrain_width);
    const auto map_height = static_cast<int32_t>(spatial.terrain_height);
    const auto rich = [&](int32_t x, int32_t z) {
        return x >= 0 && z >= 0 && x < map_width &&
               z<map_height&& static_cast<int32_t>(spatial
                                                       .plots
                                                           [static_cast<std::size_t>(z) *
                                                                spatial.terrain_width +
                                                            static_cast<std::size_t>(x)]
                                                       .metal)>
                   configured_map_metal_;
    };
    const auto fx = static_cast<int32_t>(spawn_types_[mex].footprint_x);
    const auto fz = static_cast<int32_t>(spawn_types_[mex].footprint_z);
    const auto rich_under = [&](int32_t x, int32_t z) {
        int32_t count = 0;
        for (int32_t row = 0; row < fz; ++row)
            for (int32_t column = 0; column < fx; ++column)
                count += rich(x + column, z + row) ? 1 : 0;
        return count;
    };
    match_command_ = MatchCommand::build;
    pending_build_type_ = mex;
    // The nearest deposit cell to the commander with a clear site whose
    // middle lies two cells west of it, holding no deposit cell.
    std::optional<std::array<float, 2>> mex_pointer;
    std::optional<PendingBuildSite> unsnapped;
    for (int32_t ring = 1; ring < std::max(map_width, map_height) && !mex_pointer; ++ring)
        for (int32_t dz = -ring; dz <= ring && !mex_pointer; ++dz)
            for (int32_t dx = -ring; dx <= ring && !mex_pointer; ++dx) {
                if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                    continue;
                const auto x = commander_x + dx;
                const auto z = commander_z + dz;
                if (!rich(x, z))
                    continue;
                const auto pointer_x = x - 2;
                look_at_cell(pointer_x, z);
                const auto point = cell_pointer(pointer_x, z);
                update_pointer(point[0], point[1]);
                const auto site = build_site_under(point[0], point[1]);
                if (!site || !site->legal || rich_under(site->cell_x, site->cell_z) != 0)
                    continue;
                mex_pointer = point;
                unsnapped = site;
            }
    if (!mex_pointer)
        fail("found no deposit with a clear site beside it");
    const auto snapped = snapped_build_site((*mex_pointer)[0], (*mex_pointer)[1]);
    if (!snapped || rich_under(snapped->cell_x, snapped->cell_z) == 0)
        fail(
            "a Metal Extractor's site beside the deposit at " +
            cell_text(unsnapped->cell_x, unsnapped->cell_z) + " did not snap onto it"
        );
    handle_match_left_click((*mex_pointer)[0], (*mex_pointer)[1], 1);
    {
        const auto queue = queue_of(commander);
        if (queue.empty() || queue.front().kind != orders::mobile_build_kind ||
            queue.front().destination[0] != snapped->world[0] ||
            queue.front().destination[2] != snapped->world[2])
            fail("a Metal Extractor's build click beside a deposit was not placed on it");
    }
    std::cout << "click snap check: a Metal Extractor's build click at site "
              << cell_text(unsnapped->cell_x, unsnapped->cell_z) << " snaps to "
              << cell_text(snapped->cell_x, snapped->cell_z) << ", "
              << rich_under(snapped->cell_x, snapped->cell_z) << " deposit cells\n";
    match_->stop_orders(commander);
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
}

} // namespace oa::app
