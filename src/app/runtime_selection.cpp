// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Match left-click handling, selection, tracking and squads.
#include "oa/app/runtime.hpp"
#include "touch_state.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/sim/selection.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::app {

namespace {

namespace input = oa::sim::gameplay_input;

/// Returns the order-table command an armed button gives.
input::OrderCommand armed_command(MatchCommand command) {
    switch (command) {
    case MatchCommand::attack:
        return input::OrderCommand::attack;
    case MatchCommand::reclaim:
        return input::OrderCommand::reclaim;
    case MatchCommand::capture:
        return input::OrderCommand::capture;
    case MatchCommand::load:
        return input::OrderCommand::load;
    case MatchCommand::unload:
        return input::OrderCommand::unload;
    default:
        return input::OrderCommand::default_order;
    }
}

} // namespace

void Runtime::handle_match_left_click(float x, float y, int32_t clicks) {
    consume_sticky_modifiers();
    note_shared_latch_used(match_command_ == MatchCommand::none ? 0 : 1);
    // The frame's pick at the click: the unit under the pointer is the
    // cursor unit (Game.cursor_unit_id).
    update_pointer(x, y);
    if (click_unit_info(x, y))
        return;
    // A click on the radar is its left press (issue_radar_orders), which
    // never moves the view.
    if (radar_contains(x, y)) {
        std::ignore = issue_radar_orders(x, y);
        return;
    }
    // Off the radar and the battlefield a click gives nothing.
    if (!battlefield_contains(x, y))
        return;
    if (selection_shortcut_double_click(x, y, clicks))
        return;
    const uint16_t target = hovered_match_unit_;
    if (match_command_ == MatchCommand::build) {
        // The profile's line and ring build tools take the click first.
        if (!build_tool_click(x, y))
            place_pending_build(x, y);
        return;
    }
    if (match_command_ == MatchCommand::dgun) {
        // The click ends here whether or not the orders took it.
        if (target != 0)
            std::ignore = issue_pointer_unit_orders(x, y, clicks, queueing());
        else
            issue_pointer_ground_blast(x, y, queueing());
        return;
    }
    // The role replaces the keyboard's Ctrl. The pad's FORCE is not asked here; the click's
    // pulse still carries the Ctrl that pulse holds for this press, as the keyboard read did.
    uint32_t click_pulse = 0;
    if (const auto* touch = touch_state_if_made())
        click_pulse = static_cast<uint32_t>(touch->dispatch.pulse);
    const bool force_attack =
        modifier_role_active("order.force-modifier") || (click_pulse & SDL_KMOD_CTRL) != 0;
    if ((match_command_ == MatchCommand::none && !force_attack) ||
        match_command_ == MatchCommand::move || match_command_ == MatchCommand::repair ||
        match_command_ == MatchCommand::guard) {
        if (issue_pointer_unit_orders(x, y, clicks, queueing()))
            return;
        // An armed unit order over nothing it applies to stays armed.
        if (match_command_ == MatchCommand::repair || match_command_ == MatchCommand::guard)
            return;
        if (match_command_ == MatchCommand::none) {
            // A unit whose cursor takes no click keeps the selection; open
            // ground goes through its cursor too.
            if (target == 0)
                issue_pointer_ground_orders(x, y, queueing());
            return;
        }
    }
    if (match_command_ == MatchCommand::move) {
        issue_match_move(x, y, queueing());
        finish_issued_command();
        return;
    }
    if (match_command_ == MatchCommand::patrol) {
        issue_match_patrol(x, y, queueing());
        finish_issued_command();
        return;
    }
    if (force_attack && selected_match_unit_ != 0) {
        if (issue_force_attack(x, y))
            return;
    }
    // ATTACK, RECLAIM, CAPTURE, LOAD and UNLOAD give each selected unit the
    // order the order table resolves for it over the cursor unit and the
    // ground; a click that gives nothing leaves the command armed.
    if (match_command_ == MatchCommand::attack || match_command_ == MatchCommand::reclaim ||
        match_command_ == MatchCommand::capture || match_command_ == MatchCommand::load ||
        match_command_ == MatchCommand::unload) {
        auto ground = match_world_point(x, y);
        // The profile's click snap may move a reclaim click on bare ground
        // onto a nearby feature.
        reclaim_click_snapped_ = false;
        if (match_command_ == MatchCommand::reclaim && target == 0 && ground)
            if (const auto snapped = snapped_reclaim_point(*ground)) {
                ground = snapped;
                reclaim_click_snapped_ = true;
            }
        const auto issued =
            issue_selection_orders(armed_command(match_command_), target, ground, queueing());
        reclaim_click_snapped_ = false;
        if (!issued.empty()) {
            status_ = std::string(issued);
            finish_issued_command();
        } else if (match_command_ == MatchCommand::reclaim)
            status_ = "Nothing to reclaim.";
        return;
    }
    select_match_unit(x, y, clicks);
}

bool Runtime::issue_pointer_unit_orders(float x, float y, int32_t clicks, bool queue) {
    if (!match_)
        return false;
    update_pointer(x, y);
    const auto target = hovered_match_unit_;
    if (target == 0 || !battlefield_contains(x, y))
        return false;
    const auto cursor = static_cast<input::OrderCursor>(pick_match_cursor());
    auto& world = match_->state();
    const auto command = input::pointer_command(world.game);
    switch (input::click_action(world, command, cursor)) {
    case input::ClickAction::none:
        return false;
    case input::ClickAction::select_unit:
        select_match_unit(x, y, clicks);
        return true;
    case input::ClickAction::clear_selection:
        clear_local_selection();
        apply_match_hud_for_selection();
        return true;
    case input::ClickAction::issue_command:
        break;
    }
    const auto issued = issue_selection_orders(command, target, match_world_point(x, y), queue);
    if (!issued.empty())
        status_ = std::string(issued);
    finish_issued_command();
    return true;
}

void Runtime::issue_pointer_ground_blast(float x, float y, bool queue) {
    const auto ground = match_world_point(x, y);
    if (!match_ || !ground)
        return;
    const auto cursor = static_cast<input::OrderCursor>(pick_match_cursor());
    auto& world = match_->state();
    const auto command = input::pointer_command(world.game);
    if (input::click_action(world, command, cursor) != input::ClickAction::issue_command)
        return;
    if (!issue_selection_orders(command, 0, ground, queue).empty())
        status_ = "D-Gun ground";
    finish_issued_command();
}

void Runtime::check_builder_orders() {
    namespace input = oa::sim::gameplay_input;
    if (!match_ || !selected_tnt_)
        throw std::runtime_error("builder order check needs a running match");
    auto& slots = match_->world().slots;
    uint16_t builder = 0;
    for (const auto& slot : slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.record.owner_index != match_local_player_ || slot.record.build_remaining != 0.0F)
            continue;
        if (const auto* def = definition_for(slot.unit_index); def != nullptr && def->builder) {
            builder = slot.unit_index;
            break;
        }
    }
    if (builder == 0)
        throw std::runtime_error("builder order check found no finished local builder");
    const auto spawn_local = [&](std::string_view name, int32_t dz, bool finished) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            throw std::runtime_error("builder order check lacks " + std::string(name));
        const auto& anchor = slots[builder];
        const auto x = static_cast<int32_t>(anchor.unit->position[0] >> 16);
        const auto z = static_cast<int32_t>(anchor.unit->position[2] >> 16) + dz;
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = type;
        request.finished = finished;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* slot = match_->create(request);
        if (slot == nullptr || slot->unit == nullptr)
            throw std::runtime_error("builder order check could not spawn " + std::string(name));
        return slot->unit_index;
    };
    const auto unfinished = spawn_local("ARMSOLAR", -96, false);
    const auto damaged = spawn_local("ARMPW", 96, true);
    {
        auto& slot = slots[damaged];
        slot.record.health = static_cast<int16_t>(slot.unit->type->maximum_health / 2U);
    }
    const auto saved_selection = selected_match_unit_;
    const auto saved_command = match_command_;
    if (!cursors_loaded_)
        load_game_cursors();
    clear_local_selection();
    adopt_selection(builder);
    selected_match_unit_ = builder;
    match_command_ = MatchCommand::none;
    const auto point_over = [&](uint16_t id) {
        center_camera_on_unit(id);
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto screen = project_match_point(viewport, slots[id].unit->position);
        return std::pair{static_cast<float>(screen.x), static_cast<float>(screen.y)};
    };
    const auto expect_cursor = [&](uint16_t id, input::OrderCursor cursor, const char* what) {
        const auto [x, y] = point_over(id);
        update_pointer(x, y);
        if (hovered_match_unit_ != id)
            throw std::runtime_error(std::string("builder order check: pointer missed ") + what);
        if (pick_match_cursor() != static_cast<uint8_t>(cursor))
            throw std::runtime_error(
                std::string("builder order check: wrong cursor over ") + what + ": " +
                std::to_string(pick_match_cursor())
            );
        return std::pair{x, y};
    };
    const auto primary_kinds = [&](uint16_t id) {
        std::vector<uint8_t> kinds;
        for (const auto* order = match_->orders(id).primary; order != nullptr; order = order->next)
            kinds.push_back(order->kind);
        return kinds;
    };
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);

    auto [assist_x, assist_y] =
        expect_cursor(unfinished, input::OrderCursor::repair, "the unit under construction");
    rebuild_surface();
    write_ppm(report_directory / "native-match-repair-cursor.ppm", surface_);
    handle_match_left_click(assist_x, assist_y, 1);
    auto kinds = primary_kinds(builder);
    if (kinds.empty() || kinds.front() != oa::sim::match_runtime::help_build_kind)
        throw std::runtime_error("builder order check: click did not issue HelpBuild");
    // A queued click on the same target takes its HelpBuild back
    // instead of queueing a second; the next one queues it again.
    if (!issue_pointer_unit_orders(assist_x, assist_y, 1, true))
        throw std::runtime_error("builder order check: queued click was not consumed");
    if (!primary_kinds(builder).empty())
        throw std::runtime_error("builder order check: shift-click did not take HelpBuild back");
    // The orders it leaves are checked below.
    std::ignore = issue_pointer_unit_orders(assist_x, assist_y, 1, true);
    kinds = primary_kinds(builder);
    if (kinds.size() != 1 || kinds.front() != oa::sim::match_runtime::help_build_kind)
        throw std::runtime_error("builder order check: shift-click did not queue HelpBuild");

    expect_cursor(damaged, input::OrderCursor::select, "the damaged unit");
    match_command_ = MatchCommand::repair;
    auto [repair_x, repair_y] =
        expect_cursor(damaged, input::OrderCursor::repair, "the damaged unit with REPAIR armed");
    rebuild_surface();
    write_ppm(report_directory / "native-match-repair-armed.ppm", surface_);
    handle_match_left_click(repair_x, repair_y, 1);
    kinds = primary_kinds(builder);
    if (kinds.size() != 1 || kinds.front() != oa::sim::match_runtime::repair_unit_kind)
        throw std::runtime_error("builder order check: REPAIR click did not issue RepairUnit");
    if (match_command_ != MatchCommand::none)
        throw std::runtime_error("builder order check: REPAIR stayed armed after the click");
    std::cout << "builder order check: repair cursor, HelpBuild, shift-click took it back and "
                 "queued it again, RepairUnit\n";

    clear_local_selection();
    if (saved_selection != 0)
        adopt_selection(saved_selection);
    selected_match_unit_ = saved_selection;
    match_command_ = saved_command;
}

std::optional<oa::sim::ground_orders::Point> Runtime::match_world_point(float x, float y) {
    if (const auto world = radar_world_point(x, y))
        return world;
    return ground_point_under(x, y);
}

std::optional<oa::sim::ground_orders::Point> Runtime::ground_point_under(float x, float y) const {
    if (live_renderer_active())
        return live_ground_point(x, y);
    if (!selected_tnt_)
        return std::nullopt;
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    // The map pixel drawn under the point, a whole one as orders take.
    auto screen_map =
        map_pixel_drawn_at(viewport, {static_cast<int32_t>(x), static_cast<int32_t>(y)});
    if (!screen_map)
        return std::nullopt;
    // A view past the map's edges shows black there: a point there takes
    // the nearest point of the shown map.
    const auto [map_width, map_height] = shown_map_size();
    if (map_width > 0 && map_height > 0) {
        screen_map->x = std::clamp(screen_map->x, 0, map_width - 1);
        screen_map->y = std::clamp(screen_map->y, 0, map_height - 1);
    }
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    const auto target = oa::sim::gameplay_input::terrain_intersection(
        terrain,
        static_cast<int32_t>(screen_map->x),
        static_cast<int32_t>(screen_map->y),
        static_cast<int32_t>(selected_tnt_->attribute_width * 16U),
        static_cast<int32_t>(selected_tnt_->attribute_height * 16U)
    );
    return oa::sim::ground_orders::Point{target.x, target.y, target.z};
}

bool Runtime::issue_force_attack(float x, float y) {
    if (!match_ || selected_match_unit_ == 0)
        return false;
    update_pointer(x, y);
    if (const auto id = hovered_match_unit_; id != 0 && id != selected_match_unit_) {
        const auto ground = match_world_point(x, y);
        try {
            // A unit no attack resolves for is given no order.
            for_each_selected([&](uint16_t source) {
                std::ignore = match_->issue_attack_command(
                    source, id, queueing(), ground ? &*ground : nullptr
                );
            });
            status_ = "Force attack";
            return true;
        } catch (const std::exception& error) {
            status_ = std::string("force attack: ") + error.what();
            std::cerr << "unsupported operation: " << status_ << '\n';
            return true;
        }
    }
    const auto ground = match_world_point(x, y);
    if (!ground)
        return false;
    try {
        // A unit no attack resolves for is given no order.
        std::ignore = match_->issue_attack_ground(selected_match_unit_, *ground, queueing());
        status_ = "Force attack ground";
        return true;
    } catch (const std::exception& error) {
        status_ = std::string("force attack ground: ") + error.what();
        std::cerr << "unsupported operation: " << status_ << '\n';
        return true;
    }
}

bool Runtime::has_local_selection() const {
    if (!match_)
        return false;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ ||
            (slot.unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
            continue;
        return true;
    }
    return false;
}

[[nodiscard]] bool Runtime::match_unit_present(uint16_t id) const {
    if (!match_ || id == 0 || id >= match_->world().slots.size())
        return false;
    const auto& slot = match_->world().slots[id];
    return slot.unit != nullptr && slot.record.type_index != 0;
}

void Runtime::stop_match_tracking() {
    match_tracking_ = false;
    tracked_match_unit_ = 0;
    if (match_)
        match_->state().game.follow_unit = 0;
}

std::vector<uint16_t> Runtime::selected_local_ids() const {
    std::vector<uint16_t> ids;
    if (!match_)
        return ids;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ ||
            (slot.unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
            continue;
        ids.push_back(slot.unit_index);
    }
    return ids;
}

void Runtime::begin_match_tracking(uint16_t id) {
    tracked_match_unit_ = id;
    match_tracking_ = true;
    if (match_)
        match_->state().game.follow_unit = oa::oa_unit_ref_from_slot(id);
    center_camera_on_unit(id);
    status_ = "Track " + unit_info_name(id);
}

void Runtime::follow_match_camera_unit() {
    const auto ref = match_->state().game.follow_unit;
    if (ref == 0) {
        if (match_tracking_)
            stop_match_tracking();
        return;
    }
    const auto id = static_cast<uint16_t>(oa::oa_unit_slot_from_ref(ref));
    if (!match_tracking_ || id != tracked_match_unit_)
        begin_match_tracking(id);
}

void Runtime::cycle_match_tracking(bool reverse) {
    if (!match_)
        return;
    oa::World& world = match_->state();
    world.game.local_player_index = match_local_player_;
    const bool was_tracking = match_tracking_;
    oa::sim::selection::follow_next_selected(world, reverse);
    if (world.game.follow_unit == 0) {
        if (was_tracking) {
            stop_match_tracking();
            status_ = "Free cam";
        }
        return;
    }
    const auto followed = oa::oa_unit_slot_from_ref(world.game.follow_unit);
    begin_match_tracking(static_cast<uint16_t>(followed));
}

void Runtime::clear_local_selection() {
    if (!match_)
        return;
    oa::sim::selection::Hooks hooks{};
    hooks.context = this;
    hooks.selection_cleared = [](void* context) {
        static_cast<Runtime*>(context)->selected_match_unit_ = 0;
    };
    oa::sim::selection::clear_selection(match_->state(), hooks);
}

void Runtime::adopt_selection(uint16_t id) {
    if (!match_ || id == 0)
        return;
    auto& slot = match_->world().slots[id];
    if (slot.unit == nullptr)
        return;
    slot.unit->flags |= OA_UNIT_FLAG_SELECTED;
    match_->selection().panel_unit_id = 0;
    match_->selection().frame_flags |= oa::sim::selection::frame_flag_selection_changed;
    offline_services_.refresh_selected_unit(slot);
    if (selected_match_unit_ == 0)
        selected_match_unit_ = id;
}

void Runtime::assign_squad(int squad) {
    if (!match_ || squad < 1 || squad > 9)
        return;
    oa::World& world = match_->state();
    world.game.local_player_index = match_local_player_;
    oa::sim::selection::Hooks hooks{};
    hooks.context = match_.get();
    hooks.set_squad = [](void* context, oa::Unit& unit, int32_t squad) {
        static_cast<oa::sim::match_runtime::Match*>(context)->set_unit_squad(
            unit.id, static_cast<uint32_t>(squad)
        );
    };
    oa::sim::selection::assign_squad(world, squad, hooks);
    play_match_interface_sound("CreateSquad");
    status_ = "Squad " + std::to_string(squad) + " assigned";
}

void Runtime::select_squad(int squad, bool add) {
    if (!match_ || squad < 1 || squad > 9)
        return;
    namespace selection = oa::sim::selection;
    oa::World& world = match_->state();
    world.game.local_player_index = match_local_player_;
    const selection::TypeMask* skip = oa::data::defs::category_registry_find(
        &unit_table_.tables.categories, selection::category_squad_key_skip
    );
    const selection::TypeMask no_skip{};
    // The shared hooks centre the view and stop tracking when the squad is
    // already the selection. The command reset stays the one a squad key has
    // always used: it does not also drop a building waiting to be placed.
    selection::Hooks hooks = selection_hooks();
    hooks.reset_command = [](void* context) {
        static_cast<Runtime*>(context)->reset_match_command();
    };
    const bool any =
        selection::select_squad(world, squad, add, skip != nullptr ? *skip : no_skip, hooks);
    play_match_interface_sound("SelectSquad");
    selected_match_unit_ = 0;
    uint16_t first = 0;
    int count = 0;
    for (auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.owner_index != match_local_player_ ||
            (slot.unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
            continue;
        adopt_selection(slot.unit_index);
        if (slot.unit->squad != squad)
            continue;
        if (first == 0)
            first = slot.unit_index;
        ++count;
    }
    if (!any) {
        status_ = "Squad " + std::to_string(squad) + " empty";
        apply_match_hud_for_selection();
        return;
    }
    selected_match_unit_ = first;
    apply_match_hud_for_selection();
    status_ = "Squad " + std::to_string(squad) + " (" + std::to_string(count) + ")";
}

void Runtime::select_units_matching(
    const std::function<bool(const oa::sim::unit_spawn::Slot&)>& pred
) {
    if (!match_)
        return;
    clear_local_selection();
    uint16_t first = 0;
    int count = 0;
    for (auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ || !pred(slot))
            continue;
        adopt_selection(slot.unit_index);
        if (first == 0)
            first = slot.unit_index;
        ++count;
    }
    selected_match_unit_ = first;
    apply_match_hud_for_selection();
    status_ = count == 0 ? "No units" : std::to_string(count) + " selected";
}

int Runtime::squad_from_key(const SDL_KeyboardEvent& key) const {
    if (key.scancode >= SDL_SCANCODE_1 && key.scancode <= SDL_SCANCODE_9)
        return static_cast<int>(key.scancode - SDL_SCANCODE_1) + 1;
    if (key.scancode >= SDL_SCANCODE_KP_1 && key.scancode <= SDL_SCANCODE_KP_9)
        return static_cast<int>(key.scancode - SDL_SCANCODE_KP_1) + 1;
    if (key.key >= SDLK_1 && key.key <= SDLK_9)
        return static_cast<int>(key.key - SDLK_1) + 1;
    return 0;
}

} // namespace oa::app
