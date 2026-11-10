// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Ordinary owned ground orders cross a known FP2 throat through the real Match
// caller. Captured local-head save bytes retain the complete subsequent march.
#include "combat_fixture.hpp"
#include "installed_units.hpp"
#include "local_throat_guide.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;
namespace spatial = oa::sim::spatial_state;
constexpr int32_t pixel = 65536;
constexpr uint32_t width = 160, height = 32, stream_size = 8, actor_count = stream_size * 2;
constexpr uint32_t last_tick = 4000, exact_version = 7;
constexpr uint8_t land_height = 60, sea_level = 30;
constexpr int16_t lower = 8, first = 48, last = 86;

/// Returns an FP2 centre in whole world units.
constexpr int16_t centre(int16_t origin) {
    return int16_t(origin * 16 + 16);
}

/// Compares every native Goal geometry field and its primary Order identity.
bool same_goal(const ground::Goal& a, const ground::Goal& b) {
    return a.order == b.order && a.shape == b.shape && a.cell == b.cell &&
           a.tolerance == b.tolerance && a.radius_squared == b.radius_squared &&
           a.inner_range == b.inner_range && a.inner_radius_squared == b.inner_radius_squared &&
           a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

/// Supplies room for both ordinary native waves and complete known terrain.
Options options() {
    Options result;
    result.sight_cells = 161;
    result.per_player_limit = 16;
    return result;
}

struct Actor {
    uint16_t id{};
    ground::RoutePoint destination{};
    ground::Goal* owned_goal{};
    ground::Goal geometry{};
    bool increasing{}, seen_goal{}, contacted{}, rested{}, crossed{};
};

struct Scene {
    Fixture fixture{options()};
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    std::array<Actor, actor_count> actors{};
    test::InstalledUnits* installed{};
    uint32_t version{};

    /// Builds real native movement, terrain and orders without injecting routes or heads.
    explicit Scene(uint32_t version, test::InstalledUnits* installed = nullptr)
        : installed(installed), version(version) {
        fixture.match.reset();
        fixture.map.attribute_width = width;
        fixture.map.attribute_height = height;
        fixture.map.sea_level = sea_level;
        fixture.map.attributes.resize(size_t(width) * height);
        fixture.terrain_values.resize(size_t(width) * height);
        fixture.collision_plots.resize(size_t(width) * height);
        fixture.altitude_cells.resize(size_t(width / 2) * (height / 2));
        for (uint32_t z = 0; z < height; ++z)
            for (uint32_t x = 0; x < width; ++x) {
                const bool high = land(x, z) || (x && land(x - 1, z)) || (z && land(x, z - 1)) ||
                                  (x && z && land(x - 1, z - 1));
                fixture.map.attributes[size_t(z) * width + x].height = high ? land_height : 0;
            }
        for (uint32_t z = 0; z < height; ++z)
            for (uint32_t x = 0; x < width; ++x) {
                auto& plot = fixture.collision_plots[size_t(z) * width + x];
                plot.low_height = plot.high_height =
                    fixture.map.attributes[size_t(z) * width + x].height;
                for (uint32_t dz = 0; dz < 2 && z + dz < height; ++dz)
                    for (uint32_t dx = 0; dx < 2 && x + dx < width; ++dx) {
                        const auto h =
                            fixture.map.attributes[size_t(z + dz) * width + x + dx].height;
                        plot.low_height = std::min(plot.low_height, h);
                        plot.high_height = std::max(plot.high_height, h);
                    }
            }
        fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
        fixture.types[1].simulation.flags &= ~OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        fixture.loaded[1].type = fixture.types[1];
        fixture.fields[1].movement_class = 1;
        fixture.fields[1].yard_mask = yard;
        fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
        fixture.metadata.min_water_depth = -10000;
        fixture.metadata.max_water_depth = 0;
        fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
        fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
        fixture.def.weapon1.clear();
        fixture.def.can_attack = false;
        fixture.def.max_velocity_fixed = 117964;
        fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = 6553;
        fixture.def.turn_rate = 1120;
        const auto loaded = installed
                                ? std::span<const sim::unit_spawn::LoadedType>(installed->loaded)
                                : std::span<const sim::unit_spawn::LoadedType>(fixture.loaded);
        const auto types = installed ? std::span<sim::unit_spawn::Type>(installed->types)
                                     : std::span<sim::unit_spawn::Type>(fixture.types);
        const auto fields = installed
                                ? std::span<const runtime::RuntimeTypeFields>(installed->fields)
                                : std::span<const runtime::RuntimeTypeFields>(fixture.fields);
        runtime::OfflineInputs input{
            fixture.map,
            loaded,
            types,
            fields,
            installed ? installed->weapons : fixture.weapons,
            fixture.terrain_values,
            fixture.masks,
            int32_t(width / 2),
            int32_t(height / 2),
            16,
            2,
            0,
            sea_level,
            1,
            &fixture.scenario,
            {},
            fixture.collision_plots,
            sim::visibility_state::AltitudeSightData{
                int32_t(width / 2),
                int32_t(height / 2),
                fixture.altitude_cells,
                fixture.altitude_patterns
            }
        };
        input.rules.setup.enhanced_mode.enabled = version != 0;
        input.rules.setup.enhanced_mode.version = version;
        input.unit_player_mask = 3;
        if (installed)
            input.feature_defs = installed->features.defs;
        fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
        match().configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t owner = 0; owner < 2; ++owner) {
            match().simulation().players[owner].present = true;
            match().simulation().players[owner].status = 1;
            std::array<uint8_t, OA_PLAYER_COUNT> allies{};
            allies[0] = allies[1] = 1;
            match().configure_player_alliances(owner, allies);
            if (!owner)
                match().configure_outcomes(owner, allies, false);
        }
        for (uint32_t i = 0; i < actor_count; ++i) {
            const bool increasing = i < stream_size;
            const auto position = i % stream_size;
            const auto x = int16_t(increasing ? 36 - position * 3 : 96 + position * 3);
            const auto destination = int16_t(increasing ? 154 - position * 3 : 3 + position * 3);
            const uint8_t owner = increasing ? 0 : 1;
            const auto type = installed ? installed->type(increasing ? "ARMPW" : "CORAK") : 1;
            auto* slot = match().create(
                {owner,
                 uint16_t(type),
                 {uint32_t(centre(x) * pixel),
                  uint32_t(land_height * pixel),
                  uint32_t(centre(lower) * pixel)},
                 true,
                 1,
                 0}
            );
            CHECK(slot && slot->unit);
            slot->unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
            slot->record.heading = increasing ? 49152 : 16384;
            match().place_unit_at(
                slot->unit_index, centre(x) * pixel, land_height * pixel, centre(lower) * pixel, 1
            );
            actors[i] = {
                slot->unit_index, {centre(destination), centre(lower)}, nullptr, {}, increasing
            };
            CHECK(ground(slot->unit_index).geometry.footprint == (std::array<int16_t, 2>{2, 2}));
        }
        fixture.run(60);
        // Complete known terrain is a stated fixture input, independent of current visibility.
        std::fill(
            match().sight_mutable().player_bits.begin(),
            match().sight_mutable().player_bits.end(),
            3
        );
        check_claims();
        for (const auto& actor : actors) {
            CHECK(!match().order_refused(
                match().issue_ground_move(
                    actor.id, {actor.destination[0] * pixel, 0, actor.destination[1] * pixel}, false
                )
            ));
            auto& slot = match().spawn_slot(actor.id);
            const auto goal = ground::make_goal(
                *slot.unit->primary,
                ground(actor.id).geometry,
                {actor.destination[0] * pixel, 0, actor.destination[1] * pixel},
                4
            );
            CHECK(
                ground::static_flow_passable(
                    classifier(actor.id), goal.cell[0], goal.cell[1], match().spatial()
                )
            );
        }
    }

    /// Defines wide rooms joined by five physical rows of flat ground.
    static bool land(uint32_t x, uint32_t z) {
        return ((x >= 1 && x < uint32_t(first)) || (x > uint32_t(last) && x < width - 1))
                   ? z >= 3 && z <= 23
                   : x >= uint32_t(first) && x <= uint32_t(last) && z >= 8 && z <= 12;
    }

    /// Returns the actual match driven by this scene.
    runtime::Match& match() { return *fixture.match; }

    /// Returns one actor's actual owned ground runtime.
    ground::GroundRuntime& ground(uint16_t id) {
        auto* result = match().ground_runtime(id);
        CHECK(result);
        return *result;
    }

    /// Reads the same installed or synthetic movement metadata supplied to the Match.
    spatial::CellClassifier classifier(uint16_t id) {
        const auto type = match().spawn_slot(id).record.type_index;
        const auto& metadata = installed ? installed->metadata.at(type) : fixture.metadata;
        const auto& body = ground(id).geometry;
        return {
            body.footprint[0],
            body.footprint[1],
            int16_t(metadata.max_water_depth),
            int16_t(metadata.min_water_depth),
            metadata.max_slope,
            metadata.bad_slope,
            metadata.max_water_slope,
            metadata.bad_water_slope,
            0
        };
    }

    /// Audits every complete registered footprint against native spatial claims.
    void check_claims() {
        std::array<uint16_t, size_t(width) * height> claims{};
        for (const auto& actor : actors) {
            const auto& record = match().spawn_slot(actor.id).record;
            const auto& body = ground(actor.id).geometry;
            CHECK(body.cell == (std::array<int16_t, 2>{record.cell_x, record.cell_z}));
            for (int16_t z = 0; z < 2; ++z)
                for (int16_t x = 0; x < 2; ++x) {
                    const auto px = body.cell[0] + x, pz = body.cell[1] + z;
                    CHECK(px >= 0 && pz >= 0 && uint32_t(px) < width && uint32_t(pz) < height);
                    const auto index = size_t(pz) * width + size_t(px);
                    const auto& plot = match().spatial().plots[index];
                    CHECK(
                        !plot.blocking_feature && plot.low_height == land_height &&
                        plot.high_height == land_height
                    );
                    CHECK(claims[index] == 0 && plot.ground == actor.id);
                    claims[index] = actor.id;
                }
        }
    }
};

struct StaticProbe {
    Scene* scene{};
    uint16_t id{};
};

/// Reads current complete knowledge and native static passability for a certificate.
ground::CorridorSampler static_sampler(StaticProbe& probe) {
    return {
        &probe, [](void* pointer, ground::CorridorOrigin origin, ground::CorridorOrigin fp) {
            auto& context = *static_cast<StaticProbe*>(pointer);
            auto& scene = *context.scene;
            const auto player = scene.match().spawn_slot(context.id).record.owner_index;
            if (origin[0] < 0 || origin[1] < 0 || uint32_t(origin[0]) + fp[0] >= width ||
                uint32_t(origin[1]) + fp[1] >= height)
                return ground::CorridorCell::blocked;
            const auto& known = scene.match().sight().player_bits;
            for (int16_t z = 0; z < fp[1]; ++z)
                for (int16_t x = 0; x < fp[0]; ++x) {
                    const auto index =
                        size_t((origin[1] + z) >> 1) * (width / 2) + uint32_t((origin[0] + x) >> 1);
                    if (index >= known.size() || (known[index] & (1u << player)) == 0)
                        return ground::CorridorCell::unknown;
                }
            return ground::static_flow_passable(
                       scene.classifier(context.id), origin[0], origin[1], scene.match().spatial()
                   )
                       ? ground::CorridorCell::clear
                       : ground::CorridorCell::blocked;
        }
    };
}

/// Proves the live primary order is also exported as this unit's own circular Goal.
void check_owned_goal(Scene& scene, Actor& actor) {
    auto* goal = scene.ground(actor.id).navigation.goal;
    CHECK(goal && goal->order == scene.match().spawn_slot(actor.id).unit->primary);

    struct Walk {
        uint16_t id{};
        ground::Goal* goal{};
        bool found{};
    } walk{scene.match().spawn_slot(actor.id).record.id, goal};

    scene.match().visit_saved_orders(
        actor.id,
        [](void* pointer,
           const data::persist::SavedOrder* order,
           const data::persist::SavedGoal* saved) {
            auto& state = *static_cast<Walk*>(pointer);
            if (order && saved && order->owner_id == state.id && saved->kind == 4 &&
                saved->circle.cell[0] == state.goal->cell[0] &&
                saved->circle.cell[1] == state.goal->cell[1] &&
                saved->circle.tolerance == state.goal->tolerance &&
                saved->circle.radius_squared == state.goal->radius_squared)
                state.found = true;
        },
        &walk
    );
    CHECK(walk.found);
}

struct Result {
    uint32_t arrived{}, rested{}, crossed{}, certified_heads{}, retained_ticks{}, end_tick{};
    bool restored{};
    uint64_t motion_digest{14695981039346656037ull}, world_digest{};
};

/// Folds one stable native value without object addresses into the motion trace.
void fold(Result& result, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        result.motion_digest ^= uint8_t(value >> (byte * 8));
        result.motion_digest *= 1099511628211ull;
    }
}

/// Roundtrips only actually captured heads; no positive test invents a head or route.
void roundtrip_local_heads(Scene& scene) {
    const auto* table = runtime::find_rule_state(scene.match().rule_state(), "local-flow");
    CHECK(table && table->bytes && table->restore);
    const auto span = table->bytes(table->context);
    const std::vector<uint8_t> captured(span.begin(), span.end());
    CHECK(
        captured.size() == scene.match().state().unit_slot_count * movement::local_flow_head_bytes
    );
    CHECK(movement::local_flow_head_bytes == 27);
    std::vector<movement::LocalFlowHead> decoded(scene.match().state().unit_slot_count);
    CHECK(movement::read_local_flow_heads(decoded, captured));
    std::vector<uint8_t> encoded(captured.size());
    CHECK(movement::write_local_flow_heads(decoded, encoded) && encoded == captured);
    CHECK(table->restore(table->context, std::vector<uint8_t>(captured.size())));
    CHECK(table->restore(table->context, captured));
    const auto restored = table->bytes(table->context);
    CHECK(std::equal(restored.begin(), restored.end(), captured.begin(), captured.end()));
}

/// Runs full native ticks, observing owned Goals, physical claims and actual retained heads.
Result march(uint32_t version, bool roundtrip, test::InstalledUnits* installed = nullptr) {
    Scene scene(version, installed);
    Result result;
    for (uint32_t tick = 0; tick < last_tick; ++tick) {
        std::array<movement::LocalFlowHead, actor_count> before_heads{};
        std::array<movement::Unit, actor_count> before_bodies{};
        std::array<ground::Navigation, actor_count> before_routes{}, certificates{};
        std::array<bool, actor_count> proved{};
        const auto next_tick = scene.match().simulation().tick + 1;
        for (size_t i = 0; i < actor_count; ++i) {
            auto& actor = scene.actors[i];
            auto& ground = scene.ground(actor.id);
            before_bodies[i] = ground.geometry;
            before_routes[i] = ground.navigation;
            if (const auto* head = scene.match().local_flow_head(actor.id))
                before_heads[i] = *head;
            const bool recovery =
                movement::local_flow_recovery_due(before_heads[i], next_tick, actor.id);
            if (version == exact_version && !before_heads[i].active &&
                (before_heads[i].blocked_ticks < 30 || recovery) &&
                (next_tick + actor.id) % 12 == 0) {
                certificates[i] = ground.navigation;
                StaticProbe context{&scene, actor.id};
                ground::CorridorBudget budget{512, 0};
                proved[i] = runtime::try_local_throat_guide(
                    ground.geometry,
                    ground.navigation,
                    static_sampler(context),
                    budget,
                    certificates[i]
                );
            }
        }
        scene.fixture.run(1);
        scene.check_claims();
        bool all_resting = true;
        for (size_t i = 0; i < actor_count; ++i) {
            auto& actor = scene.actors[i];
            auto& ground = scene.ground(actor.id);
            auto* goal = ground.navigation.goal;
            if (goal) {
                CHECK(goal->order == scene.match().spawn_slot(actor.id).unit->primary);
                if (!actor.seen_goal) {
                    actor.owned_goal = goal;
                    actor.geometry = *goal;
                    actor.seen_goal = true;
                    check_owned_goal(scene, actor);
                } else if (version == exact_version) {
                    CHECK(goal == actor.owned_goal && same_goal(*goal, actor.geometry));
                }
            } else if (actor.seen_goal && !actor.contacted) {
                const bool contacted =
                    ground::goal_contains_unit(actor.geometry, before_bodies[i]) ||
                    ground::goal_contains_unit(actor.geometry, ground.geometry);
                if (version == exact_version)
                    CHECK(contacted);
                if (contacted) {
                    actor.contacted = true;
                    ++result.arrived;
                }
            }
            const auto* head = scene.match().local_flow_head(actor.id);
            if (head && head->active) {
                CHECK(head->tick == scene.match().simulation().tick);
                const bool reached =
                    movement::local_flow_head_reached(before_bodies[i], before_heads[i]);
                const bool recovery =
                    movement::local_flow_recovery_due(before_heads[i], next_tick, actor.id);
                if (version == exact_version && before_heads[i].active && !reached && !recovery &&
                    goal == before_routes[i].goal &&
                    ground.navigation.points == before_routes[i].points &&
                    ground.navigation.count == before_routes[i].count) {
                    CHECK(head->points == before_heads[i].points);
                    ++result.retained_ticks;
                }
                if (proved[i] && head->count == 3 &&
                    std::equal(
                        head->points.begin(), head->points.end(), certificates[i].points.begin()
                    )) {
                    CHECK(version == exact_version && (next_tick + actor.id) % 12 == 0);
                    ++result.certified_heads;
                    check_owned_goal(scene, actor);
                    if (roundtrip && !result.restored) {
                        roundtrip_local_heads(scene);
                        result.restored = true;
                    }
                }
            }
            const auto x = ground.geometry.cell[0];
            if (!actor.crossed && (actor.increasing ? x > last : x + 1 < first)) {
                actor.crossed = true;
                ++result.crossed;
            }
            if (actor.contacted && !actor.rested && ground.movement.speed == 0 &&
                ground.movement.velocity == ground::Point{}) {
                CHECK(std::abs((ground.geometry.position[0] >> 16) - actor.destination[0]) <= 32);
                CHECK(std::abs((ground.geometry.position[2] >> 16) - actor.destination[1]) <= 32);
                actor.rested = true;
                ++result.rested;
            }
            all_resting = all_resting && actor.rested;
            for (const auto value :
                 {uint32_t(ground.geometry.position[0]),
                  uint32_t(ground.geometry.position[2]),
                  uint32_t(ground.geometry.heading),
                  uint32_t(ground.movement.speed),
                  ground.movement.last_motion_tick,
                  ground.navigation.count,
                  uint32_t(ground.navigation.flags)})
                fold(result, value);
            if (head) {
                for (const auto& point : head->points) {
                    fold(result, uint16_t(point[0]));
                    fold(result, uint16_t(point[1]));
                }
                fold(result, head->goal_key);
                fold(result, head->tick);
                fold(result, head->count);
                fold(result, head->active);
                fold(result, head->blocked_ticks);
            }
        }
        result.end_tick = scene.match().simulation().tick;
        if (all_resting)
            break;
    }
    result.world_digest =
        sim::trace::match_state_hash(scene.match(), base::game_loop::Timing{}, 0, 0, nullptr);
    std::printf(
        "{\"mode\":%u,\"installed\":%s,\"roundtrip\":%s,\"arrived\":%u,\"rested\":%u,"
        "\"crossed\":%u,\"certified_heads\":%u,\"retained_ticks\":%u,\"end_tick\":%u,"
        "\"world\":\"%016llx\",\"motion\":\"%016llx\"}\n",
        version,
        installed ? "true" : "false",
        roundtrip ? "true" : "false",
        result.arrived,
        result.rested,
        result.crossed,
        result.certified_heads,
        result.retained_ticks,
        result.end_tick,
        (unsigned long long)result.world_digest,
        (unsigned long long)result.motion_digest
    );
    return result;
}
} // namespace

/// Runs exact-version caller checks separately from installed-content checks.
int main(int argc, char** argv) {
    try {
        if (test::game_data_requested(argc, argv)) {
            const auto assets = test::require_game_assets("the installed sparse kbot throat");
            test::InstalledUnits units(assets, {"ARMPW", "CORAK"});
            const auto result = march(exact_version, false, &units);
            CHECK(
                result.arrived == actor_count && result.rested == actor_count &&
                result.crossed == actor_count
            );
            CHECK(result.certified_heads && result.retained_ticks);
        } else {
            for (uint32_t version = 0; version < exact_version; ++version)
                march(version, false);
            const auto ordinary = march(exact_version, false);
            CHECK(
                ordinary.arrived == actor_count && ordinary.rested == actor_count &&
                ordinary.crossed == actor_count
            );
            CHECK(ordinary.certified_heads && ordinary.retained_ticks);
            const auto restored = march(exact_version, true);
            CHECK(
                restored.restored && restored.arrived == actor_count &&
                restored.rested == actor_count
            );
            CHECK(
                restored.motion_digest == ordinary.motion_digest &&
                restored.world_digest == ordinary.world_digest
            );
        }
    } catch (const std::exception& error) {
        std::cerr << "sparse throat flow: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
