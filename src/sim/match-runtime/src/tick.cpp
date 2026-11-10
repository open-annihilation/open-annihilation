// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ai.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "tick_internal.hpp"
#include "enhanced_state.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "route_prefix.hpp"
#include "preferred_search_candidate.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::sim::match_runtime {
namespace {
// Ticks between a player slot's deadlines: the local slot's outcome checks,
// the slot's economy settlement and the viewpoint's contact scan.
constexpr uint32_t player_update_period = 30;
constexpr uint32_t distributed_seed_step_limit = 256;
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
    if (!input_.resuming_saved_game) {
        for (std::size_t player = 0; player < stores.size(); ++player) {
            game.players[player].energy = stores[player][0];
            game.players[player].metal = stores[player][1];
        }
        // The stores the pass settled are put back, so the next settlement
        // does not measure from what the pass left.
        for (auto& spending : settled_spending_)
            spending.stores_left_known = false;
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
    if (computer) {
        const SeatHooks hooks = seat_hooks_[player];
        const bool played = hooks.play != nullptr && hooks.play(hooks.context, *this, player);
        auto* retarget =
            enhanced_ != nullptr && (rules().setup.enhanced_mode.version ==
                                         data::enhanced_mode::battle_flow_version ||
                                     rules().setup.enhanced_mode.version ==
                                         data::enhanced_mode::distributed_battle_flow_version)
                ? &enhanced_->ai_retarget
                : nullptr;
        if (retarget != nullptr) {
            const auto bit = static_cast<uint16_t>(uint16_t(1) << player);
            if (played)
                retarget->builtin_players &= static_cast<uint16_t>(~bit);
            else
                retarget->builtin_players |= bit;
        }
        if (!played) {
            if (retarget != nullptr)
                enhanced_->prepare_ai_retarget(*this, player);

            // Attribution ends with this call, including a callback that unwinds it.
            struct RetargetScope {
                AiRetargetState* state;
                bool previous_dispatch;
                uint8_t previous_player;

                ~RetargetScope() {
                    if (state != nullptr) {
                        state->builtin_dispatch = previous_dispatch;
                        state->builtin_player = previous_player;
                    }
                }
            } scope{
                retarget,
                retarget != nullptr && retarget->builtin_dispatch,
                retarget != nullptr ? retarget->builtin_player : uint8_t(0)
            };

            if (retarget != nullptr) {
                retarget->builtin_dispatch = true;
                retarget->builtin_player = player;
            }
            sim::ai::tick_match_computer_orders(*this, player);
            if (retarget != nullptr)
                enhanced_->capture_ai_retarget(*this, player);
        }
    }
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
    if (rules().setup.enhanced_mode.enabled &&
        rules().setup.enhanced_mode.version == data::enhanced_mode::distributed_battle_flow_version)
        begin.seed_step_limit = distributed_seed_step_limit;
    map = &*klass->map;
    return true;
}

void Match::advance_path_search() {
    if (enhanced_ && enhanced_->static_passability_memo)
        enhanced_->static_passability_memo->begin_pass();
    // Static fields ignore temporary mobile occupancy. Unknown plots retain
    // the ordinary search's passable classification, so fields reveal no fog.
    sim::ground_orders::FlowSampler flow_sampler{
        this, [](void* c, uint32_t class_index, uint32_t player, int32_t x, int32_t z) -> uint8_t {
            auto& match = *static_cast<Match*>(c);
            if (class_index >= match.movement_classes_.size() || player >= OA_PLAYER_COUNT)
                return 0;
            const auto& klass = match.movement_classes_[class_index];
            if (!klass.map || x < 0 || z < 0 || static_cast<uint32_t>(x) >= klass.map->width() ||
                static_cast<uint32_t>(z) >= klass.map->height())
                return 0;
            auto classifier = klass.record;
            const int32_t sight_x = (classifier.footprint_x >> 2) + (x >> 1);
            const int32_t sight_z = (classifier.footprint_z >> 2) + (z >> 1);
            const auto sight_width = match.spatial_.terrain_width >> 1;
            const auto sight_height = match.spatial_.terrain_height >> 1;
            if (sight_x < 0 || sight_z < 0 || static_cast<uint32_t>(sight_x) >= sight_width ||
                static_cast<uint32_t>(sight_z) >= sight_height)
                return 0;
            const auto index =
                static_cast<std::size_t>(sight_z) * sight_width + static_cast<uint32_t>(sight_x);
            if (index >= match.sight_.player_bits.size())
                return 0;
            if ((match.sight_.player_bits[index] & (1u << player)) == 0)
                return 2;
            // Structures/features still block. Objects with movement state
            // have object_present set and are ignored at revision zero.
            classifier.occupancy_before_tick = 0;
            auto* memo = match.enhanced_ ? match.enhanced_->static_passability_memo.get() : nullptr;
            uint8_t value{};
            if (memo && memo->find(class_index, x, z, value))
                return value;
            value =
                sim::ground_orders::static_flow_passable(classifier, x, z, match.spatial_) ? 1 : 0;
            if (memo)
                memo->remember(class_index, x, z, value);
            return value;
        }
    };
    const bool shared_routes = rules().setup.enhanced_mode.enabled &&
                               rules().setup.enhanced_mode.version >= 2 && enhanced_ &&
                               enhanced_->shared_routes;
    if (shared_routes)
        enhanced_->shared_routes->advance(
            simulation_.tick, enhanced_->shared_static_revision, flow_sampler
        );
    auto find_slot = [this](sim::simulation_state::Unit& unit) -> sim::unit_spawn::Slot* {
        const auto id = unit.record.id;
        if (id < slots_.size() && slots_[id].unit == &unit)
            return &slots_[id];
        for (auto& s : slots_)
            if (s.unit == &unit)
                return &s;
        return nullptr;
    };

    struct PreferredCandidate {
        sim::simulation_state::Unit* unit{};
        sim::ground_orders::Goal* goal{};
        sim::ground_orders::Goal geometry{};
        uint32_t unit_index{}, generation{}, age{};
        uint16_t stable_id{};
        bool pending{};
    };

    struct Context {
        Match* match;
        decltype(find_slot)* find;
        const sim::ground_orders::FlowSampler* sampler;
        bool shared_routes;
        bool preferred_searches;
        bool collecting{};
        uint32_t polled_index{};
        std::array<PreferredCandidate, OA_PLAYER_COUNT> candidates{};
        sim::simulation_state::Unit* preferred_dispatch{};
        sim::ground_orders::Goal* preferred_goal{};
    } context{
        this,
        &find_slot,
        &flow_sampler,
        shared_routes,
        shared_routes && rules().setup.enhanced_mode.version ==
                             data::enhanced_mode::distributed_battle_flow_version
    };

    sim::ground_orders::SearchUnitAccess access;
    access.context = &context;
    access.navigator = [](void* c,
                          sim::simulation_state::Unit& unit) -> sim::ground_orders::Navigation* {
        auto& ctx = *static_cast<Context*>(c);
        auto* slot = (*ctx.find)(unit);
        auto* runtime = slot ? ctx.match->ground_runtime(slot->unit_index) : nullptr;
        if (!runtime || runtime->mirrored_driver || !runtime->movement_class)
            return nullptr;
        auto& navigation = runtime->navigation;
        if (ctx.preferred_dispatch == &unit) {
            ctx.preferred_dispatch = nullptr;
            const auto* goal = ctx.preferred_goal;
            ctx.preferred_goal = nullptr;
            // This credited pick already has a validated candidate from this
            // tick's poll. Exhausted shared credits must not turn a second
            // query into Pending and suppress the admitted ordinary search.
            if (navigation.goal == goal)
                return &navigation;
        }
        const auto* traffic = ctx.shared_routes && ctx.match->enhanced_->local_flow_heads
                                  ? &ctx.match->enhanced_->local_flow_heads[slot->unit_index]
                                  : nullptr;
        if (ctx.shared_routes && navigation.needs_search() && navigation.goal &&
            ctx.match->simulation_.tick >=
                navigation.last_search_tick + sim::ground_orders::search_retry_ticks) {
            auto* klass = ctx.match->movement_class_map(runtime->movement_class);
            if (klass) {
                const auto class_index =
                    static_cast<uint32_t>(klass - ctx.match->movement_classes_.data());
                const sim::ground_orders::FlowContext flow{
                    class_index,
                    unit.record.owner_index,
                    ctx.match->enhanced_->shared_static_revision,
                    0,
                    static_cast<uint32_t>(klass->record.footprint_x),
                    static_cast<uint32_t>(klass->record.footprint_z)
                };
                const bool mature_collision =
                    traffic && traffic->blocked_ticks >= sim::unit_movement::local_flow_retry_ticks;
                std::array<sim::ground_orders::RoutePoint, sim::ground_orders::navigation_capacity>
                    route{};
                uint32_t count{};
                const auto result = ctx.match->enhanced_->shared_routes->route(
                    flow, unit.record, *navigation.goal, route, count, *ctx.sampler
                );
                if (result == sim::ground_orders::FlowResult::ready) {
                    if (mature_collision) {
                        constexpr int32_t cell_width = 16, half_cell = cell_width / 2;
                        const sim::ground_orders::RoutePoint destination{
                            static_cast<int16_t>(
                                int32_t(navigation.goal->cell[0]) * cell_width +
                                int32_t(flow.footprint_x) * half_cell
                            ),
                            static_cast<int16_t>(
                                int32_t(navigation.goal->cell[1]) * cell_width +
                                int32_t(flow.footprint_z) * half_cell
                            )
                        };
                        const bool direct_goal = count == 2 && route[1] == destination;
                        const bool same_route =
                            sim::ground_orders::route_present(navigation) &&
                            navigation.count <= navigation.points.size() &&
                            is_route_prefix(
                                std::span{navigation.points}.first(navigation.count),
                                std::span{route}.first(count)
                            );
                        // A static guide cannot resolve settled traffic by
                        // reinstalling the same route or its clear goal leg.
                        // A real terrain detour remains useful even when its
                        // first wall is farther away than a local traffic probe.
                        if (direct_goal || same_route)
                            return &navigation;
                    }
                    std::array<uint8_t, 3> flags{};
                    sim::ground_orders::UnitView view{
                        unit,
                        runtime->geometry,
                        runtime->movement,
                        navigation,
                        slot->record.attach_parent != 0,
                        flags
                    };
                    // The same publication seam as full A*: route_changed
                    // is picked up by the network's local-player tick hook.
                    (void)sim::ground_orders::accept_path(view, std::span{route}.first(count));
                    // A delivered path gets the ordinary retry interval.
                    // Pending fields keep the previous stamp so their first
                    // usable route can still be picked up promptly.
                    navigation.last_search_tick = ctx.match->simulation_.tick;
                    ctx.match->enhanced_->local_flow_heads[slot->unit_index].active = false;
                    ctx.match->enhanced_->local_flow_heads[slot->unit_index].count = 0;
                    return nullptr;
                }
                // Returning no job lets the cursor visit other units. Do
                // not consume the ordinary search's 60-tick retry stamp.
                const bool pending = result == sim::ground_orders::FlowResult::pending;
                if (ctx.collecting &&
                    (result == sim::ground_orders::FlowResult::fallback ||
                     (pending && mature_collision)) &&
                    preferred_search_unit_is_live(unit.record) && traffic &&
                    preferred_fallback_search(
                        navigation, runtime->movement, *traffic, ctx.match->simulation_.tick
                    )) {
                    const auto owner = unit.record.owner_index;
                    const auto* owned = ctx.match->runtime_order(navigation.goal->order);
                    if (owner < ctx.candidates.size() && owned && owned->unit == &unit &&
                        (!pending || (unit.primary == navigation.goal->order &&
                                      owned->extra.goal.get() == navigation.goal &&
                                      preferred_pending_search(
                                          navigation, *traffic, ctx.match->simulation_.tick
                                      )))) {
                        const auto age = ctx.match->simulation_.tick - navigation.last_search_tick;
                        auto& candidate = ctx.candidates[owner];
                        if (!candidate.unit || age > candidate.age ||
                            (age == candidate.age && unit.record.id < candidate.stable_id)) {
                            candidate = {
                                &unit,
                                navigation.goal,
                                *navigation.goal,
                                ctx.polled_index,
                                ctx.match->bridge_->runtime(*slot).instance_generation,
                                age,
                                unit.record.id,
                                pending
                            };
                        }
                    }
                }
                if (result == sim::ground_orders::FlowResult::pending && !mature_collision)
                    return nullptr;
            }
        }
        return &navigation;
    };
    access.prepare = [](void* c,
                        sim::simulation_state::Unit& unit,
                        sim::ground_orders::SearchBegin& begin,
                        sim::ground_orders::MovementMap*& map) {
        auto& ctx = *static_cast<Context*>(c);
        auto* slot = (*ctx.find)(unit);
        const bool prepared = slot && ctx.match->prepare_search_job(*slot, begin, map);
        if (prepared && ctx.match->enhanced_)
            ++ctx.match->enhanced_->counters.full_route_searches;
        return prepared;
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
        const bool unreachable = sim::ground_orders::accept_path(view, route);
        // A replacement route supersedes the old traffic detour even when
        // the destination has not changed. Keep its collision retry clock.
        if (ctx.shared_routes) {
            ctx.match->enhanced_->local_flow_heads[slot->unit_index].active = false;
            ctx.match->enhanced_->local_flow_heads[slot->unit_index].count = 0;
        }
        if (!unreachable)
            return;
        const auto* goal = runtime->navigation.goal;
        auto* goal_order = goal != nullptr ? goal->order : nullptr;
        if (goal_order == nullptr)
            return;
        if (auto* record = ctx.match->runtime_order(goal_order)) {
            if (record->path_failure_reported)
                return;
            record->path_failure_reported = true;
        }
        ctx.match->emit_event<&EventHooks::path_failed>(
            ctx.match->state(), slot->unit_index, goal_order->kind
        );
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
    if (context.preferred_searches)
        access.preferred_unit = [](void* c, uint8_t player, uint32_t tick, uint32_t& index) {
            auto& ctx = *static_cast<Context*>(c);
            if (player >= ctx.candidates.size() || player >= ctx.match->simulation_.players.size())
                return false;
            const auto candidate = ctx.candidates[player];
            ctx.candidates[player] = {};
            auto& units = ctx.match->simulation_.players[player].units;
            if (!candidate.unit || candidate.unit_index >= units.size())
                return false;
            auto& unit = units[candidate.unit_index];
            if (&unit != candidate.unit || unit.record.id != candidate.stable_id ||
                unit.record.owner_index != player || !preferred_search_unit_is_live(unit.record))
                return false;
            auto* slot = (*ctx.find)(unit);
            auto* runtime = slot ? ctx.match->ground_runtime(slot->unit_index) : nullptr;
            if (!runtime || runtime->mirrored_driver || !runtime->movement_class ||
                ctx.match->bridge_->runtime(*slot).instance_generation != candidate.generation ||
                runtime->navigation.goal != candidate.goal ||
                !same_preferred_search_goal(*candidate.goal, candidate.geometry))
                return false;
            const auto* owned = ctx.match->runtime_order(candidate.goal->order);
            if (!ctx.match->enhanced_->local_flow_heads)
                return false;
            const auto& traffic = ctx.match->enhanced_->local_flow_heads[slot->unit_index];
            if (!owned || owned->unit != &unit ||
                !preferred_fallback_search(runtime->navigation, runtime->movement, traffic, tick) ||
                (candidate.pending &&
                 (unit.primary != candidate.goal->order ||
                  owned->extra.goal.get() != candidate.goal ||
                  !preferred_pending_search(runtime->navigation, traffic, tick))))
                return false;
            index = candidate.unit_index;
            ctx.preferred_dispatch = &unit;
            ctx.preferred_goal = candidate.goal;
            return true;
        };
    if (shared_routes) {
        auto& scan = enhanced_->shared_route_scan_state;
        uint32_t previous_tick = 0;
        for (size_t byte = 1; byte < scan.size(); ++byte)
            previous_tick |= uint32_t(scan[byte]) << ((byte - 1) * 8);
        if (scan[0] == 0 || previous_tick != simulation_.tick) {
            scan[0] = 1;
            for (size_t byte = 1; byte < scan.size(); ++byte)
                scan[byte] = static_cast<uint8_t>(simulation_.tick >> ((byte - 1) * 8));
            uint32_t active_players = 0;
            std::array<uint8_t, OA_PLAYER_COUNT> active_slots{};
            for (uint8_t player = 0; player < simulation_.players.size(); ++player)
                if (sim::simulation_state::player_slot_active(player, simulation_.players[player]))
                    active_slots[active_players++] = player;
            constexpr uint32_t shared_units_per_tick = 1024;
            const auto per_player = shared_units_per_tick / std::max(active_players, uint32_t{1});
            context.collecting = context.preferred_searches;
            for (uint32_t seat = 0; seat < active_players; ++seat) {
                // Extraction shares a bounded probe budget. Rotate its first
                // claimant so a large army cannot always exhaust it before
                // another player's pending routes are polled.
                const auto player =
                    active_slots[(uint64_t(simulation_.tick) + seat) % active_players];
                auto& record = simulation_.players[player];
                if (record.units.empty())
                    continue;
                // Pending fields must be revisited independently of full A*
                // jobs, whose cursor can take minutes to circle a large army.
                // Tick-derived ranges need no queue or per-unit allocation.
                const auto first = static_cast<size_t>(
                    uint64_t(simulation_.tick) * per_player % record.units.size()
                );
                const auto count = std::min(record.units.size(), size_t(per_player));
                for (size_t offset = 0; offset < count; ++offset) {
                    const auto index = (first + offset) % record.units.size();
                    auto& unit = record.units[index];
                    if (!search_.controller.idle() && search_.controller.active().unit == &unit)
                        continue;
                    if (unit.record.type_index != 0 && unit.record.movement != 0) {
                        context.polled_index = static_cast<uint32_t>(index);
                        (void)access.navigator(access.context, unit);
                    }
                }
            }
            context.collecting = false;
        }
    }
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
    if (const auto* entry = runtime_order(countdown)) {
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
    invalidate_target_query_extras();
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
