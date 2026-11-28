// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The opt-in revision, graphics independence and recorded rules of loaded games.
#include "oa/app/runtime.hpp"
#include "oa/app/enhanced_mode.hpp"
#include "oa/app/play_host.hpp"
#include "engine_settings_state.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/mutators/hacks.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/data/persist/hapibank.hpp"
#include "oa/data/persist/save_enhanced_mode.hpp"
#include "oa/formats/tad.hpp"
#include "oa/netgame/records.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/files.hpp"
#include "net_options.hpp"
#include "network_play.hpp"
#include "oa/sim/unit_spawn/restore_ranges.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
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

/// Copies a save and replaces its Enhanced Mode account.
///
/// @param source a save this build wrote
/// @param destination the future save
/// @param revision the revision the future save names
/// @param feature_ids the feature ids it names
/// @param needs_version the stored engine version, or empty when it stored none
void rewrite_enhanced_account(
    const fs::path& source,
    const fs::path& destination,
    uint32_t revision,
    std::vector<uint32_t> feature_ids,
    std::string needs_version
) {
    namespace persist = oa::data::persist;
    persist::Bank bank{};
    persist::bank_init(&bank);

    struct DropBank {
        persist::Bank* held;

        ~DropBank() { persist::bank_destroy(held); }
    } drop{&bank};

    persist::BankError error{};
    const persist::FileSource reader = persist::stdio_file_source();
    require(
        persist::bank_read_file(
            &bank,
            oa::platform::path_to_utf8(source).c_str(),
            persist::savegame_description,
            nullptr,
            &reader,
            &error
        ),
        "the future-rules fixture could not read its source save"
    );
    persist::SavedEnhancedMode account;
    account.revision = revision;
    account.feature_ids = std::move(feature_ids);
    account.needs_version = std::move(needs_version);
    account.unit_player_mask = 1;
    account.order = {0};
    require(
        persist::save_write_enhanced_mode(&bank, account),
        "the future-rules fixture could not write its account"
    );
    const persist::FileSink writer = persist::stdio_file_sink();
    require(
        persist::bank_write_file(
            &bank,
            oa::platform::path_to_utf8(destination).c_str(),
            persist::savegame_description,
            true,
            false,
            &writer
        ),
        "the future-rules fixture could not write its save"
    );
}

/// A short recording whose Enhanced Mode sector names a future choice.
///
/// @param revision the sector's revision, at least 1
/// @param feature_bits bit (id − 1) for each feature
/// @param needed_version the packed engine version, or 0 when unknown
/// @return the recording's bytes
std::vector<uint8_t>
future_recording(uint32_t revision, uint64_t feature_bits, uint32_t needed_version) {
    namespace tad = oa::formats::tad;
    const auto encoded = tad::encode_enhanced_mode_sector({revision, feature_bits, needed_version});
    const std::vector<uint8_t> sector(encoded.begin(), encoded.end());
    const std::vector<uint8_t> status{0x04};
    const std::vector<uint8_t> payload{tad::payload_marker, 0xff};
    // Playback refuses a recording that names no unit type before it reads the sector.
    oa::netgame::UnitDefHandshakeRecord announced{};
    announced.subtype = static_cast<uint8_t>(oa::netgame::HandshakeSubtype::def_checksum);
    announced.key = 1;
    std::array<uint8_t, tad::layout::unit_check_record_bytes> unit_check{};
    std::size_t unit_check_bytes = 0;
    require(
        oa::netgame::encode_record(
            announced, unit_check.data(), unit_check.size(), &unit_check_bytes
        ) == oa::netgame::WireError::ok &&
            unit_check_bytes == unit_check.size(),
        "the future-rules fixture could not name a unit type"
    );
    tad::Demo demo;
    demo.version = tad::supported_version;
    demo.max_units = 250;
    demo.map_name = "Metal Isles";
    demo.sectors.push_back(
        {static_cast<uint32_t>(tad::SectorType::enhanced_mode), std::span<const uint8_t>{sector}}
    );
    demo.unit_checks = unit_check;
    demo.players.push_back({1, 0, 1, "Pilot"});
    demo.statuses.push_back({1, std::span<const uint8_t>{status}});
    demo.packets.push_back({0, 40, 40, 1, std::span<const uint8_t>{payload}});
    const tad::WriteResult written = tad::write(demo);
    require(written.ok(), "the future-rules fixture could not write its recording");
    return written.bytes;
}

/// Returns the first feature the table marks withdrawn: one this build knows
/// and will not play.
oa::data::enhanced_mode::Feature first_withdrawn_feature() {
    for (const auto& row : oa::data::enhanced_mode::feature_table())
        if (row.withdrawn)
            return row.id;
    throw std::runtime_error("Enhanced Mode check: the feature table withdraws nothing");
}

} // namespace

void Runtime::check_enhanced_mode() {
    namespace settings = oa::ui::engine_settings;
    namespace mode = oa::data::enhanced_mode;
    namespace input = oa::sim::gameplay_input;
    require(options_.preferences_file.has_value(), "needs an isolated --preferences-file");
    const settings::EngineSettings kept = engine_settings();
    const auto command_line_mode = options_.enhanced_mode;
    const auto command_line_revision = options_.enhanced_revision;
    const auto command_line_features = options_.enhanced_features;
    options_.enhanced_revision.reset();
    options_.enhanced_features.reset();
    // A new game with Enhanced Mode on plays Revision 1.
    const mode::Choice revision_one{1, mode::revision_features(1)};
    const mode::Choice off{};
    // Sets a save, a recording or a launch's feature list can name besides
    // Revision 1: capacity alone, local traffic flow over shared routes, and
    // Revision 1 without one of its features.
    const auto without = [&](mode::Feature feature) {
        return mode::Choice{1, revision_one.features.without(feature)};
    };
    const std::array<mode::Choice, 3> named_sets{
        {mode::Choice{1, mode::FeatureSet{}.with(mode::Feature::capacity)},
         mode::Choice{
             1, mode::close_features(mode::FeatureSet{}.with(mode::Feature::routes_fine_regions))
         },
         without(mode::Feature::idle_goal_skip)}
    };
    // Choices no build plays: an unknown revision, a set without capacity, Off
    // naming a feature, a feature id no build knows, and a withdrawn feature.
    const std::array<mode::Choice, 5> unplayable{
        {mode::Choice{mode::highest_revision + 1, revision_one.features},
         mode::Choice{1, mode::FeatureSet{}.with(mode::Feature::local_flow)},
         mode::Choice{0, mode::FeatureSet{}.with(mode::Feature::capacity)},
         mode::Choice{1, revision_one.features | mode::FeatureSet::from_bits(uint64_t{1} << 60)},
         mode::Choice{1, revision_one.features.with(first_withdrawn_feature())}}
    };
    auto chosen = kept;
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == off, "Off did not keep released rules");
    const std::string released_hash = simulation_hash(*this);

    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == revision_one, "On did not resolve Revision 1");
    const std::string enhanced_hash = simulation_hash(*this);
    require(enhanced_hash != released_hash, "Revision 1 did not enter the simulation hash");
    for (const auto& held : named_sets) {
        require(hold_enhanced_rules_once(held), "a named set was refused");
        require(enhanced_mode_choice(*this) == held, "a named set was replaced");
        require(simulation_hash(*this) != enhanced_hash, "different sets share an identity");
        release_enhanced_rules_once(true);
        require(
            enhanced_mode_choice(*this) == revision_one,
            "releasing a named set did not restore Revision 1"
        );
        require(simulation_hash(*this) == enhanced_hash, "releasing a named set changed identity");
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
    require(enhanced_mode_choice(*this) == revision_one, "the match lost its preset");
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
    const auto check_group_entrypoints = [&](const mode::Choice& wanted) {
        using Point = sim::ground_orders::Point;
        using Record = sim::match_runtime::Match::OrderRecordView;
        require(enhanced_mode_choice(*this) == wanted, "entrypoint fixture changed rules");
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
            // Every unit keeps its place in the group around the click, as
            // with Enhanced Mode off, even when that place is outside the
            // playable area or blocked.
            const std::array<Point, 2>& expected = formations;
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
                        match_->issue_ground_move(actors[index], expected[index], false);
                    else
                        match_->issue_patrol(actors[index], expected[index], false);
                (void)issue_selection_orders(command, 0, click, true);
                expect_orders(kind, expected, 0);
                (void)issue_selection_orders(command, 0, click, true);
                expect_orders(kind, expected, 1);
                for (size_t index = 0; index < actors.size(); ++index)
                    require(
                        cancels_queued_command(actors[index], command, 0, expected[index], true),
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
        if (wanted == mode::Choice{1, mode::revision_features(wanted.revision)})
            std::printf(
                "Enhanced Mode: generic/specialized/play-host group entrypoints agree in "
                "Revision %u\n",
                static_cast<unsigned>(wanted.revision)
            );
        else
            std::printf(
                "Enhanced Mode: generic/specialized/play-host group entrypoints agree in a set "
                "of %d features\n",
                std::popcount(wanted.features.bits())
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
    // The requests first wait for their player's landmass labels, which the
    // shared routes make over the next ticks; then a request admits the
    // field, and a field admitted in a tick is cleared from the next one.
    constexpr uint32_t admission_attempts = 64;
    for (uint32_t attempt = 0;
         attempt < admission_attempts && match_->enhanced_counters().shared_route_fields == 0;
         ++attempt)
        step_match_simulation();
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
    require(enhanced_mode_choice(*this) == revision_one, "a live match changed mode");
    require(simulation_hash(*this) == enhanced_hash, "a live match changed rules identity");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == off, "the next game did not take Off");
    require(simulation_hash(*this) == released_hash, "Off did not restore the original hash");
    require(load_saved_game(enhanced_save), "the enhanced save did not load with Off selected");
    require(enhanced_mode_choice(*this) == revision_one, "the save did not restore its mode");
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
    check_group_entrypoints(revision_one);
    require(hold_enhanced_rules_once(off), "a subsequent load could not hold Off");
    release_enhanced_rules_once(true);
    require(
        engine_settings_->restored_enhanced_choice &&
            *engine_settings_->restored_enhanced_choice == revision_one &&
            engine_settings_->restored_enhanced_started,
        "a failed subsequent load lost the active match's mode hold"
    );
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == off, "leaving kept the recorded mode");
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
    require(enhanced_mode_choice(*this) == off, "replacement kept enhanced rules");
    leave_match();
    load(Screen::main_menu);
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(load_saved_game(released_save), "a released save did not load with On selected");
    require(enhanced_mode_choice(*this) == off, "a released save acquired new rules");
    require(engine_settings().enhanced_mode, "loading changed the menu preference");
    check_group_entrypoints(mode::Choice{});
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
    require(enhanced_mode_choice(*this) == revision_one, "leaving lost the menu's On");
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
    for (std::size_t at = 0; at < named_sets.size(); ++at) {
        const auto& previous = named_sets[at];
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
            oa::platform::path_from_utf8("enhanced-set-" + std::to_string(at) + ".sav");
        require(save_match_game(old_save, "Named Enhanced set", 0), "a named set did not save");
        leave_match();
        load(Screen::main_menu);
        require(
            enhanced_mode_choice(*this) == revision_one,
            "old-save fixture changed the new-game preference"
        );
        require(load_saved_game(old_save), "a named set's save did not load");
        require(enhanced_mode_choice(*this) == previous, "a named set's save was replaced");
        require(simulation_hash(*this) == old_hash, "a named set's save changed rules identity");
        check_group_entrypoints(previous);
        leave_match();
        load(Screen::main_menu);
        require(
            enhanced_mode_choice(*this) == revision_one, "leaving a named set's save kept its rules"
        );
    }
    for (const auto& refused : unplayable)
        require(!hold_enhanced_rules_once(refused), "an unplayable choice was accepted");
    require(enhanced_mode_choice(*this) == revision_one, "a rejected choice changed rules");
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
    require(enhanced_mode_choice(*this) == revision_one, "the campaign lost its mode");
    restart_campaign_mission();
    require(match_ != nullptr, "the loaded campaign did not restart");
    require(
        enhanced_mode_choice(*this) == revision_one, "a campaign restart lost its recorded mode"
    );
    require(!engine_settings().enhanced_mode, "the campaign changed the menu preference");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == off, "leaving the campaign kept its mode");
    // A launch's feature list (--enhanced-features) chooses what new games
    // play over the menu; held and saved games keep their own sets.
    const mode::Choice launched = without(mode::Feature::idle_goal_skip);
    const mode::Choice relaunched = without(mode::Feature::counterflow_queue_escape);
    const mode::Choice retained = without(mode::Feature::dispatch_cohort_retention);
    options_.enhanced_mode = true;
    options_.enhanced_features = "-idle_goal_skip";
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == launched, "the launch's list did not choose its set");
    const std::string launched_hash = simulation_hash(*this);
    require(launched_hash != enhanced_hash, "the launch's list kept Revision 1's identity");
    for (const auto renderer : renderers) {
        chosen.hardware_acceleration = renderer;
        apply_engine_settings(chosen);
        require(enhanced_mode_choice(*this) == launched, "graphics changed the launch's set");
        require(simulation_hash(*this) == launched_hash, "graphics changed the launch's identity");
    }
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == launched, "menu Off replaced the launch's list");
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == launched, "menu On lost the launch's list");
    for (const auto& held : {off, relaunched}) {
        require(hold_enhanced_rules_once(held), "a held set could not override the launch");
        require(enhanced_mode_choice(*this) == held, "the launch replaced held rules");
        release_enhanced_rules_once(true);
        require(enhanced_mode_choice(*this) == launched, "releasing held rules lost the launch");
    }
    for (const auto& refused : unplayable) {
        require(!hold_enhanced_rules_once(refused), "a held unplayable choice was accepted");
        require(enhanced_mode_choice(*this) == launched, "a rejected choice changed the launch");
    }
    chosen.hardware_acceleration = settings::HardwareAcceleration::off;
    apply_engine_settings(chosen);
    start_benchmark_skirmish_as_chosen();
    require(enhanced_mode_choice(*this) == launched, "a new skirmish lost the launch's set");
    const fs::path launched_save =
        options_.preferences_file->parent_path() / "enhanced-launch-list.sav";
    require(
        save_match_game(launched_save, "Launch feature list", 0), "the launch's set did not save"
    );
    // A real launch keeps its Options. This fixture changes the list, so
    // resolve its pending profile explicitly while the live rules stay held.
    options_.enhanced_features = "-counterflow_queue_escape";
    apply_engine_settings(chosen);
    apply_hack_overrides();
    const auto* pending_profile = next_match_profile();
    require(
        pending_profile != nullptr && mode::features(pending_profile->rules) == relaunched.features,
        "the pending profile did not take the changed list"
    );
    require(enhanced_mode_choice(*this) == launched, "a live match changed its launch rules");
    leave_match();
    load(Screen::main_menu);
    require(
        enhanced_mode_choice(*this) == relaunched, "the next game did not take the changed list"
    );
    require(load_saved_game(launched_save), "the launch's save did not load under another list");
    require(enhanced_mode_choice(*this) == launched, "another list replaced a saved set");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == relaunched, "leaving a saved match lost the list");
    options_.enhanced_features = "-idle_goal_skip";
    apply_engine_settings(chosen);
    apply_hack_overrides();
    require(enhanced_mode_choice(*this) == launched, "the fixture did not restore its first list");
    require(load_saved_game(enhanced_save), "the default save did not load under a launch list");
    require(enhanced_mode_choice(*this) == revision_one, "a launch list replaced saved Revision 1");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == launched, "leaving the default save lost the list");
    options_.enhanced_features = "-dispatch_cohort_retention";
    apply_hack_overrides();
    require(enhanced_mode_choice(*this) == retained, "the launch did not take a third list");
    const std::string retained_hash = simulation_hash(*this);
    require(
        retained_hash != launched_hash && retained_hash != enhanced_hash,
        "a third list did not have a distinct rules identity"
    );
    for (const auto renderer : renderers) {
        chosen.hardware_acceleration = renderer;
        apply_engine_settings(chosen);
        require(enhanced_mode_choice(*this) == retained, "graphics changed the third list");
        require(simulation_hash(*this) == retained_hash, "graphics changed the third identity");
    }
    chosen.hardware_acceleration = settings::HardwareAcceleration::off;
    apply_engine_settings(chosen);
    start_benchmark_skirmish_as_chosen();
    require(enhanced_mode_choice(*this) == retained, "a new skirmish lost the third list");
    const fs::path retained_save = options_.preferences_file->parent_path() / "enhanced-third.sav";
    require(save_match_game(retained_save, "Third feature list", 0), "the third list did not save");
    options_.enhanced_features = "-idle_goal_skip";
    apply_hack_overrides();
    const auto* retained_pending = next_match_profile();
    require(
        retained_pending != nullptr && mode::features(retained_pending->rules) == launched.features,
        "the pending profile did not return to the first list"
    );
    require(enhanced_mode_choice(*this) == retained, "a live match took the pending list");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == launched, "leaving did not return to the first list");
    require(load_saved_game(retained_save), "the third list's save did not load");
    require(enhanced_mode_choice(*this) == retained, "the first list replaced a saved third set");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_choice(*this) == launched, "leaving the third save lost the first list");
    options_.enhanced_features.reset();
    options_.enhanced_mode = false;
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == off, "a launch Off did not leave Enhanced Mode Off");
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(
        enhanced_mode_choice(*this) == revision_one,
        "menu On after a launch Off did not select Revision 1"
    );
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
    options_.enhanced_features = "-idle_goal_skip";
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(enhanced_mode_choice(*this) == launched, "the preference fixture lost its list");
    require(
        !save_engine_settings(chosen, chosen, true),
        "Restore defaults could not write an absent preference"
    );
    written = oa::platform::preferences::load(*options_.preferences_file);
    require(!written.contains(mode_key), "a command-line On was stored as a preference");
    options_.enhanced_mode = command_line_mode;
    options_.enhanced_revision = command_line_revision;
    options_.enhanced_features = command_line_features;
    apply_engine_settings(kept);
    // A later game's revision, or a feature id this build does not know, is
    // refused with the stored version. The reader does not guess one.
    const mode::Choice rules_before = enhanced_mode_choice(*this);
    const std::string version_line = [] {
        std::string text = mode::refusal_needs_version("0.9.4");
        const auto token = text.find("{version}");
        require(token != std::string::npos, "the version refusal lost its placeholder");
        text.replace(token, std::string_view{"{version}"}.size(), "0.9.4");
        return text;
    }();
    const std::string later_line = mode::refusal_needs_version("");
    const uint32_t packed_later = mode::packed_engine_version("0.9.4");
    require(packed_later == 0x00000904u, "0.9.4 did not pack as major, minor, patch");

    struct FutureRules {
        const char* name;
        uint32_t revision;
        std::vector<uint32_t> feature_ids;
        uint64_t feature_bits;
        const char* needs_version;
        uint32_t packed_version;
        const std::string* line;
    };

    const FutureRules futures[] = {
        {"future-revision", 2, {}, 0, "0.9.4", packed_later, &version_line},
        {"future-feature", 1, {60}, uint64_t{1} << 59, "0.9.4", packed_later, &version_line},
        {"future-unversioned", 2, {}, 0, "", 0, &later_line},
    };
    const fs::path future_dir = options_.preferences_file->parent_path();
    for (const FutureRules& future : futures) {
        const fs::path save = future_dir / (std::string{future.name} + ".sav");
        rewrite_enhanced_account(
            enhanced_save, save, future.revision, future.feature_ids, future.needs_version
        );
        require(!load_saved_game(save), "a future save was accepted");
        require(status_ == *future.line, "a future save used the wrong refusal");
        require(enhanced_mode_choice(*this) == rules_before, "a future save changed the rules");
        const fs::path recording = future_dir / (std::string{future.name} + ".tad");
        const std::vector<uint8_t> bytes =
            future_recording(future.revision, future.feature_bits, future.packed_version);
        {
            std::ofstream out(recording, std::ios::binary);
            require(static_cast<bool>(out), "a future recording file did not open");
            out.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
            require(static_cast<bool>(out), "a future recording file did not write");
        }
        netgame_context().staged_recording =
            StagedRecording{oa::platform::path_to_utf8(recording.filename()), bytes, false};
        bool refused = false;
        std::string message;
        try {
            NetworkPlay::of(*this).start_demo_playback();
        } catch (const std::runtime_error& playback_error) {
            refused = true;
            message = playback_error.what();
        }
        require(refused, "a future recording was accepted");
        if (message != *future.line)
            std::cerr << "future recording refusal: " << message << '\n';
        require(message == *future.line, "a future recording used the wrong refusal");
        require(
            enhanced_mode_choice(*this) == rules_before, "a future recording changed the rules"
        );
    }

    // A withdrawn feature is one this build knows and will not play: a save or
    // recording naming one is refused as unusable, not as a later game's.
    {
        const mode::FeatureSet withdrawn_set =
            revision_one.features.with(first_withdrawn_feature());
        const std::string problem = mode::feature_set_problem(withdrawn_set);
        std::vector<uint32_t> withdrawn_ids;
        for (const mode::FeatureInfo& row : mode::feature_table())
            if (withdrawn_set.has(row.id))
                withdrawn_ids.push_back(static_cast<uint32_t>(row.id));
        const fs::path save = future_dir / "withdrawn-feature.sav";
        rewrite_enhanced_account(enhanced_save, save, 1, withdrawn_ids, "0.9.3");
        require(!load_saved_game(save), "a save naming a withdrawn feature was accepted");
        if (status_ != "Invalid savegame file: " + problem)
            std::cerr << "withdrawn save refusal: " << status_ << '\n';
        require(
            status_ == "Invalid savegame file: " + problem,
            "a save naming a withdrawn feature used the wrong refusal"
        );
        require(enhanced_mode_choice(*this) == rules_before, "a withdrawn save changed the rules");
        const fs::path recording = future_dir / "withdrawn-feature.tad";
        const std::vector<uint8_t> bytes = future_recording(
            1, mode::sector_feature_bits(withdrawn_set), mode::packed_engine_version("0.9.3")
        );
        {
            std::ofstream out(recording, std::ios::binary);
            require(static_cast<bool>(out), "the withdrawn recording file did not open");
            out.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
            require(static_cast<bool>(out), "the withdrawn recording file did not write");
        }
        netgame_context().staged_recording =
            StagedRecording{oa::platform::path_to_utf8(recording.filename()), bytes, false};
        std::string message;
        try {
            NetworkPlay::of(*this).start_demo_playback();
        } catch (const std::runtime_error& playback_error) {
            message = playback_error.what();
        }
        if (message != "demo playback: " + problem)
            std::cerr << "withdrawn recording refusal: " << message << '\n';
        require(
            message == "demo playback: " + problem,
            "a recording naming a withdrawn feature used the wrong refusal"
        );
        require(
            enhanced_mode_choice(*this) == rules_before, "a withdrawn recording changed the rules"
        );
        std::puts("Enhanced Mode: a save and a recording naming a withdrawn feature are unusable");
    }

    // A mutator's hack check reads the choice new games play. A planned hack
    // that names the feature set cannot replace it.
    if (match_)
        leave_match();
    release_enhanced_rules_once(false);
    const auto kept_mode = options_.enhanced_mode;
    const auto kept_revision = options_.enhanced_revision;
    const auto kept_features = options_.enhanced_features;
    const oa::data::mutators::MutatorHack feature_hack{
        "ridge", "setup.enhanced-mode.features", "[capacity]"
    };
    const auto hacks = std::span<const oa::data::mutators::MutatorHack>(&feature_hack, 1);
    const auto expect_set = [&](const mode::Choice& wanted, const char* what) {
        apply_hack_overrides();
        require(enhanced_mode_choice(*this) == wanted, what);
        oa::data::mutators::PipelineInput input;
        bind_planned_hack_check(input);
        require(static_cast<bool>(input.check_hacks), "the hack check was not bound");
        const std::vector<oa::data::mutators::HackCheckError> faults = input.check_hacks(hacks);
        if (!faults.empty())
            throw std::runtime_error("Enhanced Mode check: " + faults.front().message);
        const std::optional<mode::Choice> probed = probe_planned_hack_choice(hacks);
        require(probed.has_value() && *probed == wanted, "a mutator changed the match's set");
    };
    options_.enhanced_mode = true;
    options_.enhanced_revision = mode::highest_revision;
    options_.enhanced_features.reset();
    {
        oa::ui::engine_settings::EngineSettings playing = engine_settings();
        playing.enhanced_mode = true;
        apply_engine_settings(playing);
    }
    expect_set(
        mode::Choice{mode::highest_revision, mode::revision_features(mode::highest_revision)},
        "Revision 1 was not the mutator check's choice"
    );
    options_.enhanced_revision.reset();
    options_.enhanced_features = "-counterflow_queue_escape";
    expect_set(
        without(mode::Feature::counterflow_queue_escape),
        "a launch's feature list was not the mutator check's choice"
    );
    options_.enhanced_mode = kept_mode;
    options_.enhanced_revision = kept_revision;
    options_.enhanced_features = kept_features;
    apply_hack_overrides();
    std::puts("Enhanced Mode: mutator checks keep Revision 1 and a launch's feature list");
    std::puts(
        "Enhanced Mode: revision, graphics independence, frozen matches and save restoration "
        "passed"
    );
}

} // namespace oa::app
