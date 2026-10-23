// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ai.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "tick_internal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::sim::match_runtime {
namespace {
// Ticks between a player slot's deadlines: the local slot's outcome checks,
// the slot's economy settlement and the viewpoint's contact scan.
constexpr uint32_t player_update_period = 30;
} // namespace

void Match::tick() {
    // The load's pass comes first, if the tick's driver has not run it.
    run_load_pass();
    // A running game has no commander to place.
    commander_placement_ = CommanderPlacement::none;
    if (trace_)
        trace_->sample(*this);
    TickHost host(*this);
    tick(host);
    // The game sets the computer players up as it starts. The match has no
    // start hook, so this runs every tick and does its work on the first.
    sim::ai::prepare_match_computer_players(*this);
    update_player_slots();
    mark_profile(OA_PROFILE_LOGIC);
    // The features step after the players' tick; the reproduction pass
    // reads the ground occupant of the plot it visits.
    project_plot_occupants();
    sim::feature_runtime::tick_features(state(), feature_host());
    // The wind is rescheduled after the players' tick, so the economy and
    // the computer players read the wind of the tick before; the effect
    // lists flush last.
    refresh_wind();
    if (strategic_environment_) {
        strategic_environment_->maximum_wind = environment_wind_.maximum_strength;
        strategic_environment_->normalized_wind = environment_wind_.normalized_strength;
        strategic_environment_->tidal_strength = input_.tidal_strength;
    }
    // The meteor storm steps after the wind.
    if (meteor.step != nullptr)
        meteor.step(meteor.context);
    mark_profile(OA_PROFILE_MISC);
    sim::effect_particles::tick_particles(*effects_, state().game, effect_host());
    mark_profile(OA_PROFILE_SFX);
    // The game lets the viewer's remembered sight lapse after its batch of
    // ticks, compacting the memory every time; the match steps one tick.
    auto context = sight_context();
    sim::visibility_state::expire_remembered_sight(
        remembered_sight_, simulation_.tick, true, context
    );
    mark_profile(OA_PROFILE_MISC);
    // Last, each held debris piece starts its puff or spark, once a tick
    // whether the tick is drawn or not, from the debris table the tick leaves.
    sim::effect_particles::start_debris_particles(*effects_, state().game, effect_host());
    mark_profile(OA_PROFILE_SFX);
    // Share-panel structure gifts that waited out their time go across after
    // the tick (sharing.structure-gift-rate-limit).
    give_due_structure_gifts();
}

void Match::finish_load() {
    if (load_finished_)
        return;
    load_finished_ = true;
    load_pass_waiting_ = true;
    load_tick_ = simulation_.tick;
}

void Match::run_load_pass() {
    if (!load_pass_waiting_)
        return;
    load_pass_waiting_ = false;
    // The pass belongs to the load's tick, whatever tick the clock reached.
    const uint32_t clock_tick = simulation_.tick;
    simulation_.tick = load_tick_;
    auto& game = state().game;
    std::array<std::array<float, 2>, OA_PLAYER_COUNT> stores{};
    for (std::size_t player = 0; player < stores.size(); ++player)
        stores[player] = {game.players[player].energy, game.players[player].metal};
    sim::ai::prepare_match_computer_players(*this);
    advance_path_search();
    update_player_slots();
    if (!input_.resuming_saved_game)
        for (std::size_t player = 0; player < stores.size(); ++player) {
            game.players[player].energy = stores[player][0];
            game.players[player].metal = stores[player][1];
        }
    simulation_.tick = clock_tick;
}

void Match::update_player_slots() {
    for (std::size_t player = 0; player < world_.players.size(); ++player) {
        auto& record = match_player(*this, player);
        const auto index = static_cast<uint8_t>(player);
        if (!sim::simulation_state::player_slot_active(index, record))
            continue;
        // The player's controller (Player.controller) runs when it has
        // one, then its knowledge refresh and its units' sight stamps; the
        // economy settles and, in the viewpoint slot (Game.viewpoint_player),
        // contacts are scanned only when the slot's deadline
        // (Player.next_economy_tick) comes due; the deadline advances by the
        // period, not from the current tick.
        if (sim::ai::player_has_controller(record))
            run_player_controller(index);
        refresh_player_knowledge(index);
        update_player_sight(record);
        if (record.next_economy_tick > simulation_.tick)
            continue;
        record.next_economy_tick += player_update_period;
        if (player == state().game.local_player_index)
            advance_local_outcome();
        // Only a local or computer player still in the game settles its
        // economy, and none does once the outcome countdown runs or the game
        // is decided.
        if (sim::simulation_state::player_participating(record) &&
            (record.status == OA_PLAYER_STATUS_LOCAL ||
             record.status == OA_PLAYER_STATUS_COMPUTER) &&
            (outcome_state_.flags & sim::scenario::outcome_flag::finished) == 0 &&
            outcome_state_.countdown < 0)
            update_player_economy(player);
        // Allied vision rebuilds the sight stamps here once an alliance or
        // the viewpoint changed.
        follow_alliances_in_sight();
        if (player == state().game.viewpoint_player)
            scan_contacts();
        if (player_contacts_kept(index))
            scan_player_contacts(index);
    }
    // A multiplayer game ends once no human participant is left.
    if (multiplayer_outcomes_) {
        const auto next = sim::scenario::advance_abandoned_outcome(
            outcome_state_,
            sim::scenario::deathmatch(state().game),
            sim::scenario::connected_participants(state())
        );
        if (next != sim::scenario::Outcome::ongoing)
            outcome_result_ = next;
    }
}

void Match::run_player_controller(uint8_t player) {
    const auto& record = match_player(*this, player);
    const bool computer = record.in_use != 0 && record.status == OA_PLAYER_STATUS_COMPUTER;
    if (computer)
        sim::ai::tick_match_computer_orders(*this, player);
    sweep_weapon_targets(player, computer);
}

bool Match::prepare_search_job(
    sim::unit_spawn::Slot& slot,
    sim::ground_orders::SearchBegin& begin,
    sim::ground_orders::MovementMap*& map
) {
    auto* runtime = ground_runtime(slot.unit_index);
    auto* klass = runtime ? movement_class_map(runtime->movement_class) : nullptr;
    if (!klass)
        return false;
    // The search walks every other unit that has a movement object; the
    // searching unit's own tick reads as the current one there. It walks
    // them only when its projection moves, on its class's first search of a
    // tick, so the tick's later searches of the class gather none.
    occupancy_feed_.clear();
    if (klass->map->projection_moves(begin.current_tick))
        for (std::size_t i = 1; i < slots_.size(); ++i) {
            if (i == slot.unit_index || !movement_[i])
                continue;
            const auto& unit = state().units[i];
            occupancy_feed_.push_back(
                {sim::ground_orders::occupancy_rectangle(unit),
                 spatial_units_[i].object_tick,
                 (unit.flags & OA_UNIT_FLAG_LIVE) != 0}
            );
        }
    begin.unit_changed_tick = &spatial_units_[slot.unit_index].object_tick;
    begin.occupancy = occupancy_feed_;
    begin.world_width = spatial_.terrain_width;
    begin.world_height = spatial_.terrain_height;
    map = &*klass->map;
    return true;
}

void Match::advance_path_search() {
    auto find_slot = [this](sim::simulation_state::Unit& unit) -> sim::unit_spawn::Slot* {
        const auto id = unit.record.id;
        if (id < slots_.size() && slots_[id].unit == &unit)
            return &slots_[id];
        for (auto& s : slots_)
            if (s.unit == &unit)
                return &s;
        return nullptr;
    };

    struct Context {
        Match* match;
        decltype(find_slot)* find;
    } context{this, &find_slot};

    sim::ground_orders::SearchUnitAccess access;
    access.context = &context;
    access.navigator = [](void* c,
                          sim::simulation_state::Unit& unit) -> sim::ground_orders::Navigation* {
        auto& ctx = *static_cast<Context*>(c);
        auto* slot = (*ctx.find)(unit);
        auto* runtime = slot ? ctx.match->ground_runtime(slot->unit_index) : nullptr;
        if (!runtime || runtime->mirrored_driver || !runtime->movement_class)
            return nullptr;
        return &runtime->navigation;
    };
    access.prepare = [](void* c,
                        sim::simulation_state::Unit& unit,
                        sim::ground_orders::SearchBegin& begin,
                        sim::ground_orders::MovementMap*& map) {
        auto& ctx = *static_cast<Context*>(c);
        auto* slot = (*ctx.find)(unit);
        return slot && ctx.match->prepare_search_job(*slot, begin, map);
    };
    access.publish = [](void* c,
                        sim::simulation_state::Unit& unit,
                        sim::ground_orders::Navigation&,
                        std::span<const sim::ground_orders::RoutePoint> route) {
        auto& ctx = *static_cast<Context*>(c);
        auto* slot = (*ctx.find)(unit);
        auto* runtime = slot ? ctx.match->ground_runtime(slot->unit_index) : nullptr;
        if (!runtime)
            return;
        std::array<uint8_t, 3> flags{};
        sim::ground_orders::UnitView view{
            unit,
            runtime->geometry,
            runtime->movement,
            runtime->navigation,
            slot->record.attach_parent != 0,
            flags
        };
        sim::ground_orders::accept_path(view, route);
    };
    access.occupancy = [](void* c,
                          sim::simulation_state::Unit& unit,
                          sim::ground_orders::OccupancyRectangle& rectangle,
                          uint32_t& changed_tick) {
        auto& ctx = *static_cast<Context*>(c);
        rectangle = sim::ground_orders::occupancy_rectangle(unit.record);
        if (auto* slot = (*ctx.find)(unit))
            changed_tick = ctx.match->spatial_units_[slot->unit_index].object_tick;
    };
    auto& game = simulation_.record.game;
    const auto player_count = participant_count();
    uint16_t units_per_player = game.pool_units_per_player;
    if (units_per_player == 0)
        units_per_player = game.units_per_player ? game.units_per_player : 1;
    (void)sim::ground_orders::scan_player_jobs(
        search_,
        simulation_.players,
        player_count,
        units_per_player,
        simulation_.tick,
        sight_.player_bits,
        access
    );
}

void Match::toggle_activation(uint16_t index, uint8_t mask) {
    auto& slot = slots_.at(index);
    const bool on = (slot.record.state_flags & mask) == 0;
    set_activation(slot, mask, on);
}

uint16_t Match::self_destruct_remaining(uint16_t index) const {
    const auto& unit = units_.at(index);
    const auto* countdown = sim::simulation_state::find_order(unit.orders, self_destruct_kind);
    if (!countdown)
        return 0;
    for (const auto& entry : orders_) {
        if (&entry->order != countdown)
            continue;
        // The second parameter holds the steps left with a marker once
        // counting started; the first is set with the last count.
        const auto steps = static_cast<uint32_t>(entry->attack.retry);
        if (entry->extra.tolerance != 0)
            return 0;
        if (!(steps & self_destruct_countdown_marker))
            return static_cast<uint16_t>(
                (match_unit_def(*this, unit.record).abilities &
                 OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) >>
                OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT
            );
        return static_cast<uint16_t>((steps & ~self_destruct_countdown_marker) + 1u);
    }
    return 0;
}

void Match::toggle_self_destruct(std::span<const uint16_t> units) {
    TickHost host(*this);
    bool cancelled = false;
    for (const auto index : units) {
        auto& unit = units_.at(index);
        if (auto* countdown = sim::simulation_state::find_order(unit.orders, self_destruct_kind)) {
            note_step(sim::simulation_state::remove_order(unit, *countdown, host));
            cancelled = true;
        }
    }
    if (cancelled)
        return;
    for (const auto index : units)
        if (units_.at(index).record.type_index)
            (void)insert_ground_order(index, self_destruct_kind);
}

bool Match::selectable(uint16_t index) const {
    return sim::simulation_state::unit_selectable(state(), match_unit(*this, index));
}

void Match::attachment_local_update(uint16_t child, uint16_t parent) {
    auto& child_slot = slots_.at(child);
    if (!child_slot.unit || !sim::simulation_state::locally_simulated(*child_slot.unit) ||
        parent == 0)
        return;
    if (match_unit_def(*this, match_unit(*this, parent)).flags & OA_UNIT_DEF_FLAG_IS_AIRBASE)
        return;
    install_be_carried(child);
}

void Match::install_be_carried(uint16_t child) {
    auto& carried = *slots_.at(child).unit;
    if (!carried.record.attach_parent)
        return;
    TickHost host(*this);
    note_step(sim::simulation_state::clear_orders(carried, false, host));
    insert_ground_order(child, be_carried_kind);
}

void Match::script_attach_unit(uint16_t carrier, int32_t target, int32_t piece, int32_t mode) {
    const auto child = static_cast<uint16_t>(target);
    if (child == 0 || child >= slots_.size() || !slots_[child].unit)
        return;
    const auto& carried = match_unit(*this, child);
    const auto parent = link_parent(carried);
    if (!(carried.flags & live_unit_flag) || (parent != 0 && parent != carrier))
        return;
    set_carry_link(child, carrier, static_cast<int8_t>(piece), static_cast<uint8_t>(mode));
}

void Match::script_drop_unit(uint16_t carrier, int32_t target) {
    const auto child = static_cast<uint16_t>(target);
    if (child == 0 || child >= slots_.size() || !slots_[child].unit)
        return;
    const auto& carried = match_unit(*this, child);
    if (!(carried.flags & live_unit_flag) || link_parent(carried) != carrier)
        return;
    if (!site_clear_for(carried.type_index, carried.cell_x, carried.cell_z, child, 1))
        return;
    set_carry_link(child, 0, -1, 1);
}

void Match::finalize_attachment(uint16_t index) {
    auto& u = units_.at(index);
    if ((u.flags & OA_UNIT_FLAG_SELECTED) && !selectable(index)) {
        u.flags &= ~OA_UNIT_FLAG_SELECTED;
        selection_.panel_unit_id = 0;
        selection_.frame_flags |= OA_FRAME_FLAG_REFRESH_ORDER_PANEL;
    }
}

bool Match::carry_link_accepted(uint16_t child, uint16_t parent) {
    if (child == 0 || child >= slots_.size() || !slots_[child].unit)
        return false;
    const auto& child_unit = *slots_[child].unit;
    if (!(child_unit.flags & live_unit_flag) || (child_unit.flags & building_unit_flag) ||
        link_first_child(match_unit(*this, child)) != 0)
        return false;
    if (parent != 0) {
        if (parent >= slots_.size() || parent == child || !slots_[parent].unit)
            return false;
        if (!(slots_[parent].unit->flags & live_unit_flag) ||
            link_parent(match_unit(*this, parent)) != 0)
            return false;
    }
    return true;
}

void Match::set_carry_link(uint16_t child, uint16_t parent, int8_t piece, uint8_t mode) {
    if (!carry_link_accepted(child, parent))
        return;
    // Every machine shares the links it makes, whoever simulates the units.
    if (multiplayer.carry_link_changed != nullptr)
        multiplayer.carry_link_changed(multiplayer.context, child, parent, piece, mode);
    link_carried_unit(child, parent, piece, mode);
}

void Match::apply_carry_link(uint16_t child, uint16_t parent, int8_t piece, uint8_t mode) {
    if (carry_link_accepted(child, parent))
        link_carried_unit(child, parent, piece, mode);
}

void Match::link_carried_unit(uint16_t child, uint16_t parent, int8_t piece, uint8_t mode) {
    auto& child_slot = slots_[child];
    auto& child_unit = *child_slot.unit;
    // A bucket outside the world is noted, and its link is left as it is.
    auto bucket_for = [this](sim::spatial_state::Unit& unit) -> sim::spatial_state::Bucket* {
        if (unit.bucket) {
            if (*unit.bucket >= spatial_.buckets.size()) {
                fault_.note("attachment bucket outside world");
                return nullptr;
            }
            return &spatial_.buckets[*unit.bucket];
        }
        return &spatial_.outside_bucket;
    };
    auto& units = state().units;
    const auto slot_count = state().unit_slot_count;
    const auto old = link_parent(units[child]);
    if (old == 0) {
        auto& projected = begin_spatial_change(child_slot);
        auto* bucket = projected.bucket_linked ? bucket_for(projected) : nullptr;
        if (bucket && sim::spatial_state::bucket_unlink(*bucket, projected, spatial_) ==
                          sim::spatial_state::Error::none)
            projected.bucket_linked = false;
        end_spatial_change(true);
    } else if (old < slot_count) {
        // Unlink the child from the old parent's sibling chain.
        uint32_t previous = 0;
        uint32_t current = link_first_child(units[old]);
        std::size_t remaining = slot_count;
        while (current && current != child) {
            if (remaining == 0) {
                fault_.note("attachment sibling cycle");
                return;
            }
            --remaining;
            if (current >= slot_count) {
                fault_.note("attachment sibling index outside pool");
                return;
            }
            previous = current;
            current = link_next(units[current]);
        }
        if (current == child) {
            if (previous)
                units[previous].attach_next = units[child].attach_next;
            else
                units[old].attach_first_child = units[child].attach_next;
        }
    }
    child_slot.record.attach_piece = static_cast<uint8_t>(piece);
    set_link_parent(units[child], parent);
    set_link_next(units[child], 0);
    if (parent) {
        units[child].attach_next = units[parent].attach_first_child;
        set_link_first_child(units[parent], child);
        if (piece == -1)
            child_unit.flags |= attached_without_piece;
        else
            child_unit.flags &= ~attached_without_piece;
    } else {
        child_unit.flags &= ~attached_without_piece;
        auto& projected = begin_spatial_change(child_slot);
        if (auto* bucket = bucket_for(projected)) {
            sim::spatial_state::bucket_push_front(*bucket, projected);
            projected.bucket_linked = true;
        }
        end_spatial_change(true);
    }
    if (auto* movement = ground_runtime(child))
        movement->movement.flags =
            static_cast<uint8_t>((movement->movement.flags & ~3u) | (mode & 3u));
    attachment_local_update(child, parent);
    finalize_attachment(child);
    // The link is in place. Report the carrier left, then the one joined.
    // A piece-only change keeps the same carrier and is not reported.
    if (old != 0 && old != parent)
        emit_event<&EventHooks::unit_carried>(state(), child, old, false);
    if (parent != 0 && parent != old)
        emit_event<&EventHooks::unit_carried>(state(), child, parent, true);
}

} // namespace oa::sim::match_runtime
