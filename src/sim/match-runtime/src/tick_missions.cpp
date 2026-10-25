// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/scenario/commander_rules.hpp"
#include "oa/sim/scenario/conditions.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/sim/gameplay_input/input.hpp"
#include "tick_internal.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

namespace {
constexpr int32_t respawn_site_draws = 9999;
constexpr int32_t respawn_border_divisor = 10; // a tenth of each side stays clear
constexpr int32_t respawn_grid_side = 3;
constexpr int32_t respawn_grid_points = respawn_grid_side * respawn_grid_side;
constexpr uint8_t respawn_occupancy_test = 1;  // the site test also walks mobile occupancy
constexpr float setup_resource_scale = 100.0F; // the setup block's *_hundreds fields
constexpr const char* victory_condition_sound = "Victory Condition";
} // namespace

sim::scenario::ConditionHost Match::scenario_condition_host() {
    return {
        this,
        [](void* context) {
            const auto& sound = static_cast<Match*>(context)->named_sound;
            if (sound.play != nullptr)
                sound.play(sound.context, victory_condition_sound);
        },
        [](void* context, const oa::Unit& unit) {
            auto& owner = *static_cast<Match*>(context);
            return owner.ground_runtime(
                       static_cast<uint16_t>(oa::world_unit_slot(&owner.state(), &unit))
                   ) != nullptr;
        },
        [](void* context, sim::scenario::Condition& condition) {
            return static_cast<Match*>(context)->move_unit_to_radius_met(condition);
        },
    };
}

void Match::scenario_unit_destroyed(sim::unit_spawn::Slot& destroyed) {
    sim::scenario::dispatch(
        scenario_,
        sim::scenario::Event::unit_destroyed,
        state(),
        destroyed.record,
        scenario_condition_host()
    );
}

void Match::scenario_unit_captured(sim::unit_spawn::Slot& captured) {
    sim::scenario::dispatch(
        scenario_,
        sim::scenario::Event::unit_captured,
        state(),
        captured.record,
        scenario_condition_host()
    );
}

void Match::disable_scenario() noexcept {
    sim::scenario::disable(scenario_);
}

void Match::configure_player_alliances(uint8_t player, const std::array<uint8_t, 10>& allies) {
    if (player >= player_alliances_.size()) {
        fault_.note("alliance owner outside player table");
        return;
    }
    player_alliances_[player] = allies;
    std::copy(allies.begin(), allies.end(), state().game.players[player].alliance);
    if (outcome_view_ && outcome_view_->local_player == player)
        outcome_view_->local_allies = allies;
}

void Match::follow_player_alliances(uint8_t player) {
    if (player >= player_alliances_.size() || !player_alliances_[player])
        return;
    auto row = *player_alliances_[player];
    const auto& record = state().game.players[player].alliance;
    for (std::size_t other = 0; other < row.size(); ++other)
        if (other != player)
            row[other] = record[other];
    player_alliances_[player] = row;
    if (outcome_view_ && outcome_view_->local_player == player)
        outcome_view_->local_allies = row;
}

void Match::share_mapped_area(uint8_t from, uint8_t to) {
    if (from >= OA_PLAYER_COUNT || to >= OA_PLAYER_COUNT)
        return;
    sim::visibility_state::share_mapped_cells(sight_, from, to);
}

void Match::configure_outcomes(
    uint8_t local,
    const std::array<uint8_t, 10>& allies,
    bool defeat_allowed,
    bool campaign,
    bool multiplayer_game
) {
    if (outcome_view_) {
        fault_.note("outcome cadence already initialized");
        return;
    }
    if (local >= simulation_.players.size()) {
        fault_.note("local outcome player outside table");
        return;
    }
    outcome_view_.emplace();
    outcome_view_->local_player = local;
    outcome_view_->local_allies = allies;
    configure_player_alliances(local, allies);
    defeat_allowed_ = defeat_allowed;
    campaign_outcomes_ = campaign;
    multiplayer_outcomes_ = multiplayer_game;
    state().game.local_player_index = local;
}

bool Match::move_unit_to_radius_met(sim::scenario::Condition& condition) {
    auto& world = state();
    if (condition.point.y == sim::scenario::unplaced_point_height) {
        const auto point = sim::gameplay_input::terrain_intersection(
            terrain_,
            condition.point.x,
            condition.point.z,
            world.game.map_width_world,
            world.game.map_height_world
        );
        condition.point = {point.x, point.y, point.z};
    }
    const sim::ground_orders::Point centre{condition.point.x, condition.point.y, condition.point.z};

    struct Visit {
        sim::scenario::Condition& condition;
        const oa::World& world;
        sim::scenario::ConditionHost host;
    } visit{condition, world, scenario_condition_host()};

    for_each_unit_in_radius(
        centre,
        condition.radius,
        [](void* context, sim::unit_spawn::Slot& slot) {
            auto& v = *static_cast<Visit*>(context);
            sim::scenario::move_unit_to_radius_unit(v.condition, v.world, slot.record, v.host);
        },
        &visit
    );
    return sim::scenario::condition_result(condition);
}

void Match::advance_local_outcome() {
    if (local_game_ended_)
        return;
    auto& view = *outcome_view_;
    auto& world = state();
    for (std::size_t i = 0; i < world_.players.size(); ++i)
        view.live_units[i] =
            std::bit_cast<int16_t>(static_cast<uint16_t>(world_.players[i].current_count));
    outcome_unit_status_.clear();
    for (const auto& slot : slots_) {
        if (slot.unit == nullptr || slot.record.owner_index != 0 || !slot.unit->record.type_index)
            continue;
        sim::scenario::UnitStatus status;
        status.type_index = static_cast<uint16_t>(slot.unit->record.type_index);
        status.flags = static_cast<uint8_t>(slot.record.flags);
        status.build_remaining = slot.record.build_remaining;
        status.capture_cooldown = slot.record.capture_cooldown;
        if (const auto* carrier = oa::world_unit(&world, slot.record.attach_parent)) {
            status.has_attachment_parent = true;
            status.carrier_flags = static_cast<uint8_t>(carrier->flags >> 24);
        }
        outcome_unit_status_.push_back(status);
    }
    view.player_zero_units = outcome_unit_status_;

    struct DiscCheckRandom : sim::scenario::DiagnosticRandom {
        explicit DiscCheckRandom(Match& owner) : match(owner) {}

        Match& match;

        uint16_t rand15() override { return static_cast<uint16_t>(effect_lcg_rand(&match)); }
    } random{*this};

    const sim::scenario::QueryContext query{
        world, view, simulation_.tick, scenario_condition_host()
    };
    const auto kind = campaign_outcomes_      ? sim::scenario::GameKind::campaign
                      : multiplayer_outcomes_ ? sim::scenario::GameKind::multiplayer
                                              : sim::scenario::GameKind::skirmish;
    const bool victory = sim::scenario::victory_check(scenario_, kind, query);
    // A campaign's defeat is tested only while its victory is not met.
    const bool defeat =
        (!campaign_outcomes_ || !victory) &&
        sim::scenario::defeat_check(scenario_, kind, query, disc_check_loss_, random);
    // Without the side table no commander can be placed, and a deathmatch
    // defeat ends the game as under the other rules. This fallback is the
    // engine's own, not 3.1c behaviour.
    const bool respawns = sim::scenario::deathmatch(world.game) && respawn_bound();
    // The defeat test is skipped for a watcher, one made so by an earlier
    // defeat included. Without a setup block the player cannot be marked
    // watching and loses instead.
    auto* local_info =
        oa::world_player_info(&world, &world.game.players[world.game.local_player_index]);
    const bool host_stays = host_stays_watching_ && sim::scenario::host_player_index(world) ==
                                                        world.game.local_player_index;
    const bool watches = multiplayer_outcomes_ && local_info != nullptr &&
                         (sim::scenario::defeated_player_watches(world) || host_stays);
    const bool watching =
        local_info != nullptr && (local_info->options & OA_SETUP_OPTION_WATCHER) != 0;
    const auto next = sim::scenario::advance_outcome(
        outcome_state_,
        campaign_outcomes_,
        victory,
        defeat,
        defeat_allowed_ && !watching,
        respawns,
        watches
    );
    if (next == sim::scenario::Outcome::respawn)
        respawn_local_commander();
    else if (next == sim::scenario::Outcome::watch)
        become_watcher(*local_info);
    else if (next != sim::scenario::Outcome::ongoing)
        outcome_result_ = next;
}

void Match::become_watcher(PlayerSetupInfo& local_info) {
    auto& world = state();
    local_info.options = static_cast<uint16_t>(local_info.options | OA_SETUP_OPTION_WATCHER);
    constexpr auto shown_everywhere =
        sim::visibility_state::terrain_mapping | sim::visibility_state::update_sight_grid;
    world.game.visibility_flags =
        static_cast<uint8_t>(world.game.visibility_flags & ~shown_everywhere);
    reset_sight_buffers(true);
    if (multiplayer.player_status_changed != nullptr)
        multiplayer.player_status_changed(multiplayer.context);
    auto notice = WatchNotice::none;
    if (host_stays_watching_ &&
        sim::scenario::host_player_index(world) == world.game.local_player_index) {
        outcome_state_.flags =
            static_cast<uint16_t>(outcome_state_.flags & ~sim::scenario::outcome_flag::won);
        notice = WatchNotice::host_watching;
    } else if (sim::scenario::computer_participants(world) == 0) {
        notice = WatchNotice::continue_prompt;
    } else if (sim::scenario::connected_participants(world) > 0) {
        outcome_state_.flags =
            static_cast<uint16_t>(outcome_state_.flags & ~sim::scenario::outcome_flag::won);
        notice = WatchNotice::hosting_computers;
    }
    if (watch.became_watcher != nullptr)
        watch.became_watcher(watch.context, notice);
}

void Match::choose_continue_watching(bool keep) {
    if (keep && multiplayer.player_status_changed != nullptr)
        multiplayer.player_status_changed(multiplayer.context);
    outcome_state_.flags =
        static_cast<uint16_t>(outcome_state_.flags & ~sim::scenario::outcome_flag::won);
    if (!keep) {
        outcome_state_.flags |= sim::scenario::outcome_flag::finished;
        outcome_result_ = sim::scenario::Outcome::defeat;
    }
}

bool Match::respawn_bound() const noexcept {
    const auto& world = state();
    if (world.game.local_player_index >= OA_PLAYER_COUNT)
        return false;
    const auto* local =
        oa::world_player_info(&world, &world.game.players[world.game.local_player_index]);
    const oa::Side* side = local != nullptr ? oa::world_side(&world, local->side) : nullptr;
    return local != nullptr && side != nullptr && side->commander[0] != '\0';
}

void Match::respawn_local_commander() {
    auto& world = state();
    const auto& host = *oa::world_player_info(
        &world, oa::world_player_record(&world, sim::scenario::host_player_index(world))
    );
    auto& player = world.game.players[world.game.local_player_index];
    const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
    const oa::Side* side = info != nullptr ? oa::world_side(&world, info->side) : nullptr;
    if (side == nullptr) {
        fault_.note("the deathmatch commander has no side");
        return;
    }
    const auto type =
        oa::data::defs::unit_defs_type_id(world.unit_defs, world.unit_def_count, side->commander);
    if (type == 0) {
        fault_.note("side commander is not in the unit catalog", side->commander);
        return;
    }
    sim::unit_spawn::Request request;
    request.player = world.game.local_player_index;
    request.type = type;
    request.position = respawn_site(type);
    request.finished = true;
    request.state = sim::unit_spawn::ground_occupancy_state;
    auto* commander = create(request);
    if (commander == nullptr || commander->unit == nullptr) {
        fault_.note("the deathmatch commander could not be created");
        return;
    }
    sim::unit_spawn::grant_start_storage(
        player, host.metal_hundreds * 100, host.energy_hundreds * 100
    );
    credit_energy(
        commander->record, static_cast<float>(host.energy_hundreds) * setup_resource_scale
    );
    credit_metal(commander->record, static_cast<float>(host.metal_hundreds) * setup_resource_scale);
    reset_sight_buffers(true);
    if (respawn.sight_rebuilt)
        respawn.sight_rebuilt(respawn.context);
    if (respawn.select_commander)
        respawn.select_commander(respawn.context);
}

std::array<uint32_t, 3> Match::respawn_site(uint16_t type) {
    const auto& game = state().game;
    std::array<uint32_t, 3> site{};
    for (int32_t draws = respawn_site_draws;;) {
        const auto border_x = game.map_width_world / respawn_border_divisor;
        const auto border_z = game.map_height_world / respawn_border_divisor;
        const auto x = random_bounded(static_cast<uint32_t>(game.map_width_world - border_x * 2));
        site[0] = (x + static_cast<uint32_t>(border_x)) << 16;
        site[1] = 0;
        const auto z = random_bounded(static_cast<uint32_t>(game.map_height_world - border_z * 2));
        site[2] = (z + static_cast<uint32_t>(border_z)) << 16;
        if (respawn_site_clear(type, site) || --draws <= 0)
            return site;
    }
}

bool Match::respawn_site_clear(uint16_t type, const std::array<uint32_t, 3>& site) const {
    const auto& game = state().game;
    const auto step_x = static_cast<uint32_t>(game.map_width) << 16;
    const auto step_z = static_cast<uint32_t>(game.map_height) << 16;
    int32_t clear = 0;
    auto z = site[2] - step_z;
    for (int32_t row = 0; row < respawn_grid_side; ++row, z += step_z) {
        auto x = site[0] - step_x;
        for (int32_t column = 0; column < respawn_grid_side; ++column, x += step_x) {
            // Logical shifts: a point past the near edge wraps to a far cell.
            const auto cell_x = static_cast<int16_t>(static_cast<uint16_t>(x >> 20));
            const auto cell_z = static_cast<int16_t>(static_cast<uint16_t>(z >> 20));
            if (site_clear_for(type, cell_x, cell_z, 0, respawn_occupancy_test))
                ++clear;
        }
    }
    if (clear < respawn_grid_points)
        return false;
    const sim::ground_orders::Point point{
        std::bit_cast<int32_t>(site[0]),
        std::bit_cast<int32_t>(site[1]),
        std::bit_cast<int32_t>(site[2])
    };
    if (feature_word_under(point) != sim::spatial_state::no_feature)
        return false;
    if (lava_world_ == 0)
        return true;
    return sim::unit_spawn::average_plot_height(
               site, spatial_.plots, game.map_width, game.map_height
           ) > static_cast<int32_t>(game.sea_level);
}

} // namespace oa::sim::match_runtime
