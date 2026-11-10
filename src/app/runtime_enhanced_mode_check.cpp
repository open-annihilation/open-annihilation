// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The opt-in preset, graphics independence and recorded rules of loaded games.
#include "oa/app/runtime.hpp"
#include "oa/app/enhanced_mode.hpp"
#include "oa/app/play_host.hpp"
#include "engine_settings_state.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/files.hpp"
#include "oa/sim/unit_spawn/restore_ranges.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// Stops a native check when its expected behavior is absent.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(std::string("Enhanced Mode check: ") + message);
}

/// The owner-local tables a movement save must retain without shared digesting.
constexpr std::array<std::string_view, 4> flow_table_names{
    "shared-routes", "local-flow", "flow-map-version", "flow-progress"
};
using SavedFlowTables = std::array<std::vector<uint8_t>, flow_table_names.size()>;

/// Copies canonical flow bytes before saving or comparing a resumed match.
SavedFlowTables flow_tables(const sim::match_runtime::Match& match) {
    SavedFlowTables saved;
    for (std::size_t at = 0; at < flow_table_names.size(); ++at) {
        const auto* table =
            sim::match_runtime::find_rule_state(match.rule_state(), flow_table_names[at]);
        require(
            table != nullptr && table->bytes != nullptr && table->restore != nullptr &&
                !table->digest,
            "a flow table was absent or included in shared digests"
        );
        const auto bytes = table->bytes(table->context);
        require(!bytes.empty(), "a flow table did not expose canonical save bytes");
        saved[at].assign(bytes.begin(), bytes.end());
    }
    return saved;
}

} // namespace

void Runtime::check_enhanced_mode() {
    namespace settings = oa::ui::engine_settings;
    namespace mode = oa::data::enhanced_mode;
    namespace input = oa::sim::gameplay_input;
    require(options_.preferences_file.has_value(), "needs an isolated --preferences-file");
    const settings::EngineSettings kept = engine_settings();
    auto chosen = kept;
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_version(*this) == mode::off_version, "Off did not keep released rules");
    const std::string released_hash = simulation_hash(*this);

    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(enhanced_mode_version(*this) == mode::current_version, "On did not resolve the preset");
    const std::string enhanced_hash = simulation_hash(*this);
    require(enhanced_hash != released_hash, "the preset did not enter the simulation hash");
    for (const uint32_t previous :
         {mode::capacity_version,
          mode::flow_version,
          mode::regional_flow_version,
          mode::swept_ground_version,
          mode::corridor_flow_version,
          mode::battle_flow_version,
          mode::distributed_battle_flow_version}) {
        if (previous == mode::current_version)
            continue;
        require(hold_enhanced_rules_once(previous), "an earlier preset was refused");
        require(enhanced_mode_version(*this) == previous, "an earlier preset was upgraded");
        require(simulation_hash(*this) != enhanced_hash, "different presets share an identity");
        release_enhanced_rules_once(true);
        require(
            enhanced_mode_version(*this) == mode::current_version,
            "releasing old rules did not restore the new-game preset"
        );
        require(simulation_hash(*this) == enhanced_hash, "releasing old rules changed identity");
    }
    constexpr std::array renderers{
        settings::HardwareAcceleration::off,
        settings::HardwareAcceleration::basic,
        settings::HardwareAcceleration::full
    };
    for (const auto renderer : renderers) {
        chosen.hardware_acceleration = renderer;
        apply_engine_settings(chosen);
        require(simulation_hash(*this) == enhanced_hash, "graphics changed enhanced rules");
    }
    chosen.hardware_acceleration = settings::HardwareAcceleration::off;
    chosen.unit_limit = oa::data::limits::highest_enhanced_units_per_player;
    apply_engine_settings(chosen);

    start_benchmark_skirmish_as_chosen();
    require(enhanced_mode_version(*this) == mode::current_version, "the match lost its preset");
    require(
        match_->world().slots.size() == 20'001, "the match did not reserve two 10,000-unit ranges"
    );
    require(match_->state().game.units_per_player == 10'000, "the match clipped the unit limit");
    require(
        oa::world_projectile_capacity(&match_->world().record) == 32'767,
        "the match kept the classic projectile capacity"
    );
    // Opposing traffic on a clear patch exercises real retained steering
    // heads. Requests wait together so the save catches shared work pending.
    const auto idle_flow_tables = flow_tables(*match_);
    const auto mover_type = sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW");
    require(mover_type != 0, "the flow fixture lacks its ground unit");
    const auto& mover_definition = spawn_types_[mover_type];
    require(
        mover_definition.footprint_x <= 2 && mover_definition.footprint_z <= 2,
        "the flow fixture unit exceeds its placement spacing"
    );
    const auto check_group_entrypoints = [&](uint32_t version) {
        using Point = sim::ground_orders::Point;
        using Record = sim::match_runtime::Match::OrderRecordView;
        require(enhanced_mode_version(*this) == version, "entrypoint fixture changed rules");
        const auto type = sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW");
        require(type != 0, "entrypoint fixture lacks ARMPW");
        const auto& definition = spawn_types_[type];
        require(
            definition.footprint_x == 2 && definition.footprint_z == 2,
            "entrypoint fixture requires the native two-plot Peewee"
        );
        auto& world = match_->state();
        constexpr int32_t cells_x = 24, cells_z = 8;
        std::array<int32_t, 2> clear_patch{};
        bool found{};
        for (int32_t z = 4; !found && z + cells_z < world.game.map_height; z += 8)
            for (int32_t x = 4; !found && x + cells_x < world.game.map_width; x += 32) {
                bool clear = true;
                for (int32_t dz = 0; clear && dz < cells_z; ++dz)
                    for (int32_t dx = 0; clear && dx < cells_x; ++dx)
                        clear = match_->site_clear_for(type, x + dx, z + dz, 0, 1);
                if (clear) {
                    clear_patch = {x, z};
                    found = true;
                }
            }
        require(found, "entrypoint fixture found no clear patch");
        constexpr int32_t pixel = 65536;
        const auto at = [&](int32_t x, int32_t z) {
            const auto px = static_cast<uint32_t>(x * 16 + definition.footprint_x * 8 - 8) << 16;
            const auto pz = static_cast<uint32_t>(z * 16 + definition.footprint_z * 8 - 8) << 16;
            return Point{
                static_cast<int32_t>(px),
                static_cast<int32_t>(static_cast<uint32_t>(match_->map_height(px, pz)) << 16),
                static_cast<int32_t>(pz)
            };
        };
        std::array<uint16_t, 2> actors{};
        for (size_t index = 0; index < actors.size(); ++index) {
            const auto position =
                at(clear_patch[0] + 2 + static_cast<int32_t>(index) * 3, clear_patch[1] + 3);
            sim::unit_spawn::Request request;
            request.player = match_local_player_;
            request.type = type;
            request.finished = true;
            request.state = kGroundOccupancyState;
            request.position = {
                static_cast<uint32_t>(position[0]),
                static_cast<uint32_t>(position[1]),
                static_cast<uint32_t>(position[2])
            };
            const auto* created = match_->create(request);
            require(created && created->unit, "entrypoint fixture did not spawn");
            actors[index] = created->unit_index;
        }
        clear_local_selection();
        for (const auto id : actors)
            adopt_selection(id);
        const auto centre = local_selection_centre(0);
        require(centre.units == 2, "entrypoint fixture did not select its native pair");
        const Point click = at(clear_patch[0] + 14, clear_patch[1] + 3);
        const oa::FixedVec3 pointer{click[0], click[1], click[2]};
        std::array<Point, 2> formations{};
        for (size_t index = 0; index < actors.size(); ++index) {
            const auto formed =
                input::group_order_point(centre, world.units[actors[index]], pointer);
            formations[index] = {formed.x, formed.y, formed.z};
            require(
                formed.x == click[0] + (index == 0 ? -24 : 24) * pixel,
                "entrypoint fixture did not preserve the native 48-pixel formation"
            );
        }
        const auto host = play_host(*this);
        const uint8_t old_clock_mode = stepped_clock_ ? 1 : 0;
        struct RestoreClockMode {
            PlayHost host;
            uint8_t mode{};
            ~RestoreClockMode() { (void)host.set_clock_mode(host.context, mode); }
        } restore_clock{host, old_clock_mode};
        require(
            host.set_clock_mode(host.context, 0),
            "entrypoint fixture could not use immediate orders"
        );
        const auto clear_orders = [&] {
            for (const auto id : actors)
                match_->stop_orders(id);
        };
        const auto expect_orders =
            [&](uint8_t kind, const std::array<Point, 2>& expected, size_t count) {
                for (size_t index = 0; index < actors.size(); ++index) {
                    std::array<Record, 4> records{};
                    const auto size =
                        match_->queue_records(actors[index], false, records.data(), records.size());
                    require(size == count, "entrypoint fixture changed the native queue length");
                    for (size_t order = 0; order < size; ++order)
                        require(
                            records[order].kind == kind && records[order].point == expected[index],
                            "an order entrypoint used another formation destination"
                        );
                }
            };
        const auto* body = std::as_const(*match_).ground_runtime(actors[1]);
        require(body != nullptr, "entrypoint fixture lost native ground movement");
        sim::simulation_state::Order scratch{};
        const auto click_goal = sim::ground_orders::make_goal(scratch, body->geometry, click, 0);
        const auto right_goal =
            sim::ground_orders::make_goal(scratch, body->geometry, formations[1], 0);
        for (uint32_t scenario = 0; scenario < 3; ++scenario) {
            // These are input fixtures only. Every order is inspected before
            // movement, and the playable extent and spatial plot are restored.
            struct RestoreWidth {
                oa::Game& game;
                int32_t width{};
                ~RestoreWidth() { game.map_pixel_width = width; }
            } restore_width{world.game, world.game.map_pixel_width};
            auto& plot =
                match_->spatial().plots
                    [static_cast<size_t>(right_goal.cell[1]) * match_->spatial().terrain_width +
                     static_cast<size_t>(right_goal.cell[0] + definition.footprint_x - 1)];
            struct RestorePlot {
                sim::spatial_state::Plot& plot;
                bool blocked{};
                ~RestorePlot() { plot.blocking_feature = blocked; }
            } restore_plot{plot, plot.blocking_feature};
            if (scenario == 1) {
                world.game.map_pixel_width = (click_goal.cell[0] + definition.footprint_x) * 16;
                require(
                    click_goal.cell[0] + definition.footprint_x ==
                            world.game.map_pixel_width / 16 &&
                        right_goal.cell[0] + definition.footprint_x >
                            world.game.map_pixel_width / 16,
                    "entrypoint bounds fixture did not isolate the right formation"
                );
            } else if (scenario == 2) {
                require(!plot.blocking_feature, "entrypoint static fixture was not clear");
                plot.blocking_feature = true;
            }
            std::array<Point, 2> expected = formations;
            if (version == mode::battle_flow_version && scenario != 0)
                expected[1] = click;
            for (const auto command : {input::OrderCommand::move, input::OrderCommand::patrol}) {
                const auto kind = data::mission_types::index_for_name(
                    command == input::OrderCommand::move ? "Move_Ground" : "Patrol"
                );
                std::array<Point, 2> specialized{};
                for (size_t index = 0; index < actors.size(); ++index) {
                    specialized[index] =
                        group_order_destination(centre, command, actors[index], 0, click);
                    require(
                        specialized[index] == expected[index],
                        "the specialized group path did not match its native formation policy"
                    );
                }
                clear_orders();
                (void)issue_selection_orders(command, 0, click, false);
                expect_orders(kind, expected, 1);
                clear_orders();
                for (size_t index = 0; index < actors.size(); ++index)
                    if (command == input::OrderCommand::move)
                        match_->issue_ground_move(actors[index], specialized[index], false);
                    else
                        match_->issue_patrol(actors[index], specialized[index], false);
                (void)issue_selection_orders(command, 0, click, true);
                expect_orders(kind, expected, 0);
                (void)issue_selection_orders(command, 0, click, true);
                expect_orders(kind, expected, 1);
                for (size_t index = 0; index < actors.size(); ++index)
                    require(
                        cancels_queued_command(actors[index], command, 0, specialized[index], true),
                        "the specialized path could not cancel a generic queued destination"
                    );
                expect_orders(kind, expected, 0);
                PlayOrderRequest request;
                request.kind = command == input::OrderCommand::move ? PlayOrderKind::move
                                                                    : PlayOrderKind::patrol;
                request.units = actors.data();
                request.unit_count = actors.size();
                request.has_point = true;
                request.point = {click[0] / pixel, click[1] / pixel, click[2] / pixel};
                auto result = host.issue_orders(host.context, request);
                require(result.ok && !result.pending, "the real play host refused its native pair");
                expect_orders(kind, expected, 1);
                request.append = true;
                result = host.issue_orders(host.context, request);
                require(result.ok && !result.pending, "the real play host refused append");
                expect_orders(kind, expected, 2);
                clear_orders();
            }
        }
        clear_local_selection();
        require(match_->fault() == nullptr, "entrypoint regression recorded a native fault");
        std::printf(
            "Enhanced Mode: generic/specialized/play-host group entrypoints agree in v%u\n", version
        );
    };
    constexpr int32_t patch_width = 28;
    constexpr int32_t patch_height = 12;
    std::array<int32_t, 2> patch{};
    bool patch_found{};
    for (int32_t z = 4; !patch_found && z + patch_height < match_->state().game.map_height; z += 8)
        for (int32_t x = 4; !patch_found && x + patch_width < match_->state().game.map_width;
             x += 32) {
            bool clear = true;
            for (int32_t dz = 0; clear && dz < patch_height; ++dz)
                for (int32_t dx = 0; clear && dx < patch_width; ++dx)
                    clear = match_->site_clear_for(mover_type, x + dx, z + dz, 0, 1);
            if (clear) {
                patch = {x, z};
                patch_found = true;
            }
        }
    require(patch_found, "the flow fixture could not find a clear traffic patch");
    constexpr unsigned coordinate_fraction_bits = 16;
    const auto point_at = [&](int32_t cell_x, int32_t cell_z) {
        const auto x = static_cast<uint32_t>(cell_x * 16 + mover_definition.footprint_x * 8 - 8)
                       << coordinate_fraction_bits;
        const auto z = static_cast<uint32_t>(cell_z * 16 + mover_definition.footprint_z * 8 - 8)
                       << coordinate_fraction_bits;
        return sim::ground_orders::Point{
            static_cast<int32_t>(x),
            static_cast<int32_t>(
                static_cast<uint32_t>(match_->map_height(x, z)) << coordinate_fraction_bits
            ),
            static_cast<int32_t>(z)
        };
    };
    std::array<uint16_t, 4> flow_units{};
    std::array<oa::FixedVec3, flow_units.size()> flow_start_positions{};
    for (std::size_t at = 0; at < flow_units.size(); ++at) {
        const auto point = point_at(patch[0] + static_cast<int32_t>(at) * 4, patch[1] + 4);
        sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = mover_type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(point[0]),
            static_cast<uint32_t>(point[1]),
            static_cast<uint32_t>(point[2])
        };
        const auto* placed = match_->create(request);
        require(placed != nullptr && placed->unit != nullptr, "the flow fixture did not spawn");
        flow_units[at] = placed->unit_index;
        flow_start_positions[at] = placed->record.position;
        match_->issue_ground_move(
            placed->unit_index, point_at(patch[0] + (at < 2 ? 24 : 0), patch[1] + 4), false
        );
        require(
            give_state_order(placed->unit_index, "Standing_FireOrder", 0),
            "the flow fixture could not hold fire"
        );
    }
    std::vector<sim::unit_movement::LocalFlowHead> flow_heads(match_->world().slots.size());
    bool traffic_head{};
    bool traffic_progress{};
    constexpr uint32_t traffic_tick_limit = 120;
    for (uint32_t tick = 0; tick < traffic_tick_limit && !(traffic_head && traffic_progress);
         ++tick) {
        for (const auto id : flow_units) {
            auto* runtime = match_->ground_runtime(id);
            require(runtime != nullptr, "the flow fixture lost a movement runtime");
            runtime->navigation.last_search_tick = match_->simulation().tick;
        }
        step_match_simulation();
        const auto heads = flow_tables(*match_)[1];
        require(
            sim::unit_movement::read_local_flow_heads(flow_heads, heads),
            "the flow fixture did not expose valid retained heads"
        );
        traffic_head = false;
        for (std::size_t at = 0; at < flow_units.size(); ++at) {
            traffic_head |= flow_heads[flow_units[at]].active;
            const auto& position = match_->world().slots[flow_units[at]].record.position;
            traffic_progress |= position.x != flow_start_positions[at].x ||
                                position.z != flow_start_positions[at].z;
        }
    }
    require(traffic_head && traffic_progress, "the traffic fixture did not steer and move");
    require(
        match_->enhanced_counters().shared_route_fields == 0,
        "the flow fixture admitted shared work before its pending save point"
    );
    for (const auto id : flow_units) {
        auto* runtime = match_->ground_runtime(id);
        require(runtime != nullptr && runtime->navigation.goal != nullptr, "a move lost its goal");
        runtime->navigation.flags |= sim::ground_orders::search_pending_flag;
        runtime->navigation.last_search_tick =
            match_->simulation().tick - sim::ground_orders::search_retry_ticks;
    }
    // The fixture owns four nearby slots in a 10,000-slot player range.
    // Start the round-robin scan at that cohort instead of timing empty slots.
    const auto base_unit = match_->state().game.players[match_local_player_].base_unit_id;
    require(flow_units.front() > base_unit, "the flow cohort lacks a preceding cursor slot");
    match_->path_search_jobs().unit_cursor[match_local_player_] =
        flow_units.front() - base_unit - 1;
    constexpr uint32_t admission_attempts = 64;
    for (uint32_t attempt = 0;
         attempt < admission_attempts && match_->enhanced_counters().shared_route_fields == 0;
         ++attempt)
        match_->advance_path_search();
    const auto pending_flow_work = match_->enhanced_counters();
    const bool pending_field =
        pending_flow_work.shared_route_fields > 0 && pending_flow_work.shared_route_cells == 0;
    if (!pending_field)
        std::fprintf(
            stderr,
            "Enhanced flow pending: requests %llu fields %llu cells %llu waits %llu at tick %u\n",
            static_cast<unsigned long long>(pending_flow_work.shared_route_requests),
            static_cast<unsigned long long>(pending_flow_work.shared_route_fields),
            static_cast<unsigned long long>(pending_flow_work.shared_route_cells),
            static_cast<unsigned long long>(pending_flow_work.shared_route_waits),
            static_cast<uint32_t>(match_->simulation().tick)
        );
    require(pending_field, "the flow fixture did not save an admitted, unexpanded shared field");
    const SavedFlowTables saved_flow_tables = flow_tables(*match_);
    require(
        saved_flow_tables[0] != idle_flow_tables[0] &&
            saved_flow_tables[1] != idle_flow_tables[1] &&
            saved_flow_tables[3] != idle_flow_tables[3],
        "the save fixture kept empty route, steering or progress state"
    );
    std::array<oa::FixedVec3, flow_units.size()> flow_saved_positions{};
    for (std::size_t at = 0; at < flow_units.size(); ++at)
        flow_saved_positions[at] = match_->world().slots[flow_units[at]].record.position;
    const fs::path enhanced_save = options_.preferences_file->parent_path() / "enhanced.sav";
    require(save_match_game(enhanced_save, "Enhanced Mode", 0), "the enhanced save failed");
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_version(*this) == mode::current_version, "a live match changed mode");
    require(simulation_hash(*this) == enhanced_hash, "a live match changed rules identity");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_version(*this) == mode::off_version, "the next game did not take Off");
    require(simulation_hash(*this) == released_hash, "Off did not restore the original hash");
    require(load_saved_game(enhanced_save), "the enhanced save did not load with Off selected");
    require(
        enhanced_mode_version(*this) == mode::current_version, "the save did not restore its mode"
    );
    require(!engine_settings().enhanced_mode, "loading changed the menu preference");
    require(match_->world().slots.size() == 20'001, "the saved match lost its unit ranges");
    require(match_->unit_player_mask() == 3, "the saved match lost its allocation mask");
    require(flow_tables(*match_) == saved_flow_tables, "loading changed canonical flow save bytes");
    constexpr uint32_t continuation_ticks = 120;
    bool resumed_progress{};
    for (uint32_t tick = 0; tick < continuation_ticks; ++tick) {
        step_match_simulation();
        for (std::size_t at = 0; at < flow_units.size(); ++at) {
            const auto& position = match_->world().slots[flow_units[at]].record.position;
            resumed_progress |= position.x != flow_saved_positions[at].x ||
                                position.z != flow_saved_positions[at].z;
        }
    }
    require(resumed_progress && match_->fault() == nullptr, "restored traffic made no progress");
    require(
        match_->enhanced_counters().shared_route_cells > 0,
        "the restored shared field did not perform pending work"
    );
    const auto continued_flow_tables = flow_tables(*match_);
    require(
        continued_flow_tables[0] != saved_flow_tables[0] &&
            continued_flow_tables[3] != saved_flow_tables[3],
        "the restored shared field or progress clocks did not advance"
    );
    std::puts("Enhanced Mode: populated pending flow tables restored and continued");
    check_group_entrypoints(mode::current_version);
    require(hold_enhanced_rules_once(mode::off_version), "a subsequent load could not hold Off");
    release_enhanced_rules_once(true);
    require(
        engine_settings_->restored_enhanced_version == mode::current_version &&
            engine_settings_->restored_enhanced_started,
        "a failed subsequent load lost the active match's mode hold"
    );
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_version(*this) == mode::off_version, "leaving kept the recorded mode");
    require(
        frontend_game().max_units_setting <= oa::data::limits::highest_units_per_player,
        "the enhanced save left an unsupported classic unit limit"
    );

    chosen.unit_limit = kept.unit_limit;
    apply_engine_settings(chosen);
    start_benchmark_skirmish_as_chosen();
    const fs::path released_save = options_.preferences_file->parent_path() / "released.sav";
    require(save_match_game(released_save, "Released rules", 0), "the released save failed");
    require(
        load_saved_game(enhanced_save), "an enhanced save could not replace a live classic match"
    );
    require(match_->world().slots.size() == 20'001, "replacement clipped the enhanced capacity");
    require(
        load_saved_game(released_save), "a classic save could not replace a live enhanced match"
    );
    require(enhanced_mode_version(*this) == mode::off_version, "replacement kept enhanced rules");
    leave_match();
    load(Screen::main_menu);
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(load_saved_game(released_save), "a released save did not load with On selected");
    require(
        enhanced_mode_version(*this) == mode::off_version, "a released save acquired new rules"
    );
    require(engine_settings().enhanced_mode, "loading changed the menu preference");
    check_group_entrypoints(mode::off_version);
    std::array<sim::unit_spawn::Request, 2> reordered_units{};
    std::array<bool, 2> found_units{};
    for (const auto& slot : match_->world().slots) {
        const auto& unit = slot.record;
        if (unit.type_index == 0 || unit.owner_index >= reordered_units.size())
            continue;
        auto& request = reordered_units[unit.owner_index];
        request.player = unit.owner_index;
        request.type = unit.type_index;
        request.position = {
            static_cast<uint32_t>(unit.position.x),
            static_cast<uint32_t>(unit.position.y),
            static_cast<uint32_t>(unit.position.z)
        };
        request.finished = true;
        request.state = kGroundOccupancyState;
        found_units[unit.owner_index] = true;
    }
    require(found_units[0] && found_units[1], "the allocation-order fixture lacks its units");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_version(*this) == mode::current_version, "leaving lost the menu's On");
    bootstrap_match({.units_per_player = 10'000, .place_commanders = false, .seat_roster = true});
    constexpr std::array<uint8_t, 2> reversed_order{1, 0};
    require(
        sim::unit_spawn::restore_unit_player_order(match_->state(), reversed_order),
        "the allocation-order fixture could not assign reversed ranges"
    );
    match_->rebind_player_ranges();
    for (const auto& request : reordered_units)
        require(match_->create(request) != nullptr, "the allocation-order fixture could not spawn");
    enter_match_view();
    const fs::path ordered_save = options_.preferences_file->parent_path() / "ordered.sav";
    require(
        save_match_game(ordered_save, "Reversed unit ranges", 0), "reversed ranges did not save"
    );
    require(load_saved_game(ordered_save), "reversed ranges did not load");
    require(
        match_->state().game.players[1].base_unit_id == 1 &&
            match_->state().game.players[0].base_unit_id == 10'001 &&
            match_->state().units[1].type_index == reordered_units[1].type &&
            match_->state().units[10'001].type_index == reordered_units[0].type,
        "the saved allocation order changed unit ownership or ids"
    );
    leave_match();
    load(Screen::main_menu);
    for (const uint32_t previous :
         {mode::capacity_version,
          mode::flow_version,
          mode::regional_flow_version,
          mode::swept_ground_version,
          mode::corridor_flow_version,
          mode::battle_flow_version,
          mode::distributed_battle_flow_version}) {
        if (previous == mode::current_version)
            continue;
        require(hold_enhanced_rules_once(previous), "old-save fixture could not select its rules");
        bootstrap_match(
            {.units_per_player = 10'000, .place_commanders = false, .seat_roster = true}
        );
        for (const auto& request : reordered_units)
            require(match_->create(request) != nullptr, "old-save fixture could not spawn");
        enter_match_view();
        const std::string old_hash = simulation_hash(*this);
        const fs::path old_save =
            options_.preferences_file->parent_path() /
            oa::platform::path_from_utf8("enhanced-v" + std::to_string(previous) + ".sav");
        require(save_match_game(old_save, "Earlier Enhanced rules", 0), "old rules did not save");
        leave_match();
        load(Screen::main_menu);
        require(
            enhanced_mode_version(*this) == mode::current_version,
            "old-save fixture changed the new-game preference"
        );
        require(load_saved_game(old_save), "an earlier Enhanced save did not load");
        require(enhanced_mode_version(*this) == previous, "an earlier save was upgraded");
        require(simulation_hash(*this) == old_hash, "an earlier save changed rules identity");
        // The distributed preset has a separate batch-destination contract.
        // This fixture keeps the unchanged raw and repaired-click policies.
        if (previous != mode::distributed_battle_flow_version)
            check_group_entrypoints(previous);
        leave_match();
        load(Screen::main_menu);
        require(
            enhanced_mode_version(*this) == mode::current_version,
            "leaving an earlier save kept its old rules"
        );
    }
    require(
        !hold_enhanced_rules_once(mode::distributed_battle_flow_version + 1),
        "an unknown version was accepted"
    );
    require(
        enhanced_mode_version(*this) == mode::current_version, "a rejected version changed rules"
    );
    show_mission_briefing();
    start_campaign_mission();
    require(match_ != nullptr, "the campaign fixture did not start");
    const fs::path campaign_save = options_.preferences_file->parent_path() / "campaign.sav";
    require(save_match_game(campaign_save, "Enhanced campaign", 0), "the campaign save failed");
    leave_match();
    load(Screen::main_menu);
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(load_saved_game(campaign_save), "the enhanced campaign did not load");
    require(enhanced_mode_version(*this) == mode::current_version, "the campaign lost its mode");
    restart_campaign_mission();
    require(match_ != nullptr, "the loaded campaign did not restart");
    require(
        enhanced_mode_version(*this) == mode::current_version,
        "a campaign restart lost its recorded mode"
    );
    require(!engine_settings().enhanced_mode, "the campaign changed the menu preference");
    leave_match();
    load(Screen::main_menu);
    require(
        enhanced_mode_version(*this) == mode::off_version, "leaving the campaign kept its mode"
    );
    const auto command_line_mode = options_.enhanced_mode;
    const std::string mode_key(settings::key::enhanced_mode);
    preference_values_[mode_key] = "1";
    options_.enhanced_mode = false;
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(
        !save_engine_settings(chosen, chosen, true),
        "Restore defaults could not write the preferences"
    );
    auto written = oa::platform::preferences::load(*options_.preferences_file);
    require(
        written.contains(mode_key) && written.at(mode_key) == "1",
        "a command-line Off erased the stored On preference"
    );
    preference_values_.erase(mode_key);
    options_.enhanced_mode = true;
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(
        !save_engine_settings(chosen, chosen, true),
        "Restore defaults could not write an absent preference"
    );
    written = oa::platform::preferences::load(*options_.preferences_file);
    require(!written.contains(mode_key), "a command-line On was stored as a preference");
    options_.enhanced_mode = command_line_mode;
    apply_engine_settings(kept);
    std::puts(
        "Enhanced Mode: preset, graphics independence, frozen matches and save restoration passed"
    );
}

} // namespace oa::app
