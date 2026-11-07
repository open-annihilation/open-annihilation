// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Saved advisory geometry retains native helper state without storing addresses.
#include "advisory_lane_codec.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {
bool watch_allocations{};
size_t allocation_count{};
} // namespace

/// Counts scalar allocations while codec operations are observed.
void* operator new(size_t size) {
    if (watch_allocations)
        ++allocation_count;
    if (void* result = std::malloc(size == 0 ? 1 : size))
        return result;
    throw std::bad_alloc{};
}

/// Routes array allocation through the observed scalar allocator.
void* operator new[](size_t size) {
    return ::operator new(size);
}

/// Releases scalar test storage.
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

/// Releases array test storage.
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

/// Releases scalar test storage when its size is supplied.
void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}

/// Releases array test storage when its size is supplied.
void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

namespace {
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;
constexpr int32_t fixed_one = 65536;
constexpr uint64_t generation = 0x0123456789abcdefull;
constexpr size_t record_bytes = runtime::advisory_lane_record_bytes;
constexpr uint8_t unwritten_byte = 255;

/// Supplies complete native two-cell footprint queries for a four-plot bridge.
struct Grid {
    bool vertical{};

    /// Reads physical terrain while transposing the same proof for the second axis.
    bool clear(int32_t x, int32_t z) const {
        if (vertical)
            std::swap(x, z);
        const bool left = x >= 2 && x <= 11 && z >= 4 && z <= 25;
        const bool bridge = x >= 12 && x <= 29 && z >= 12 && z <= 15;
        const bool right = x >= 30 && x <= 45 && z >= 4 && z <= 25;
        return left || bridge || right;
    }

    /// Checks every physical plot of a requested native footprint.
    ground::CorridorSampler sampler() {
        return {
            this, [](void* context, ground::CorridorOrigin origin, ground::CorridorOrigin foot) {
                const auto& grid = *static_cast<Grid*>(context);
                if (foot[0] <= 0 || foot[1] <= 0)
                    return ground::CorridorCell::blocked;
                for (int32_t z = 0; z < foot[1]; ++z)
                    for (int32_t x = 0; x < foot[0]; ++x)
                        if (!grid.clear(origin[0] + x, origin[1] + z))
                            return ground::CorridorCell::blocked;
                return ground::CorridorCell::clear;
            }
        };
    }
};

/// Builds actual catalog geometry and produces guide states through the live helper.
struct Fixture {
    Grid grid;
    ground::CorridorCatalog catalog{64, 64, {2, 2}};
    oa::sim::simulation_state::Order order{}, other_order{};
    ground::Goal goal{}, other_goal{};
    ground::Navigation navigation{};
    movement::Unit unit{};
    ground::CorridorApproach approach{};
    runtime::AdvisoryLaneState state{};
    runtime::AdvisoryLaneDecision decision{};
    ground::CorridorGeometry corridor{};

    /// Creates a complete route across a proved two-lane corridor.
    Fixture(bool vertical = false, bool reverse = false) {
        grid.vertical = vertical;
        catalog.reset(0);
        const auto seed =
            vertical ? ground::CorridorOrigin{12, 16} : ground::CorridorOrigin{16, 12};
        OA_CHECK(
            catalog.request(seed, vertical ? ground::CorridorAxis::z : ground::CorridorAxis::x) ==
            ground::CorridorRequest::queued
        );
        auto sampler = grid.sampler();
        ground::CorridorBudget budget{100000};
        catalog.advance(budget, sampler);
        OA_CHECK(catalog.status().corridors == 1);
        corridor = *catalog.geometry(1);
        unit.footprint = {2, 2};
        unit.cell = reverse ? ground::CorridorOrigin{40, 13} : ground::CorridorOrigin{5, 12};
        goal.cell = reverse ? ground::CorridorOrigin{5, 12} : ground::CorridorOrigin{40, 13};
        goal.order = &order;
        goal.tolerance = 4;
        navigation.goal = &goal;
        navigation.flags = ground::route_present_flag;
        navigation.count = 3;
        navigation.points[0] = reverse ? ground::RoutePoint{656, 224} : ground::RoutePoint{96, 208};
        navigation.points[1] = {400, 224};
        navigation.points[2] = reverse ? ground::RoutePoint{96, 208} : ground::RoutePoint{656, 224};
        // Unused canonical route storage is retained without requiring zero tails.
        navigation.points.back() = {-1234, 32767};
        if (vertical) {
            std::swap(unit.cell[0], unit.cell[1]);
            std::swap(goal.cell[0], goal.cell[1]);
            for (auto& point : navigation.points)
                std::swap(point[0], point[1]);
        }
        place(unit.cell);
        std::array<ground::CorridorApproach, 4> contacts{};
        const auto query = catalog.upcoming(unit, navigation, contacts, budget);
        OA_CHECK(query.complete && query.count == 1);
        approach = contacts[0];
        OA_CHECK(approach.exits_other_end);
        decision = runtime::choose_advisory_lane(
            unit, navigation, generation, &approach, &corridor, sampler, budget, state
        );
        OA_CHECK(decision.active);
        OA_CHECK(state.phase == runtime::AdvisoryLanePhase::approach);
    }

    /// Sets a test's initial native footprint origin and matching fixed-point centre.
    void place(ground::CorridorOrigin cell) {
        unit.cell = cell;
        unit.position = {
            (int32_t(cell[0]) * 16 + 16) * fixed_one, 0, (int32_t(cell[1]) * 16 + 16) * fixed_one
        };
    }

    /// Runs the unmodified helper with the supplied finite static allowance.
    void choose(uint32_t allowance) {
        auto sampler = grid.sampler();
        ground::CorridorBudget budget{allowance};
        decision = runtime::choose_advisory_lane(
            unit, navigation, generation, &approach, &corridor, sampler, budget, state
        );
    }
};

/// Compares every nonpointer guide field explicitly.
bool equal_state(const runtime::AdvisoryLaneState& a, const runtime::AdvisoryLaneState& b) {
    const auto equal_rectangle = [](ground::CorridorRectangle x, ground::CorridorRectangle y) {
        return x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom;
    };
    const auto& x = a.corridor;
    const auto& y = b.corridor;
    return a.generation == b.generation && a.goal_geometry == b.goal_geometry && x.id == y.id &&
           x.axis == y.axis && x.footprint == y.footprint &&
           equal_rectangle(x.origins, y.origins) && equal_rectangle(x.plots, y.plots) &&
           equal_rectangle(x.mouths[0], y.mouths[0]) && equal_rectangle(x.mouths[1], y.mouths[1]) &&
           x.lane_origins == y.lane_origins && x.lane_count == y.lane_count &&
           x.revision == y.revision && a.direction == b.direction && a.lane == b.lane &&
           a.phase == b.phase && a.entered == b.entered && a.steering_active == b.steering_active &&
           a.admitted_route == b.admitted_route && a.admitted_count == b.admitted_count &&
           a.entry_segment == b.entry_segment && a.exit_segment == b.exit_segment;
}

/// Round-trips native helper phases on both axes and preserves all route storage.
void native_states_round_trip() {
    for (const bool vertical : {false, true})
        for (const bool reverse : {false, true}) {
            Fixture fixture(vertical, reverse);
            std::array<runtime::AdvisoryLaneState, 6> source{};
            source[1] = fixture.state;
            const auto entrance = fixture.decision.steering.points[1];
            fixture.choose(0);
            OA_CHECK(fixture.state.phase == runtime::AdvisoryLanePhase::approach);
            OA_CHECK(!fixture.state.steering_active);
            source[2] = fixture.state;
            fixture.unit.position[0] = int32_t(entrance[0]) * fixed_one;
            fixture.unit.position[2] = int32_t(entrance[1]) * fixed_one;
            fixture.unit.cell = {
                int16_t((entrance[0] - 16) / 16), int16_t((entrance[1] - 16) / 16)
            };
            fixture.choose(1000);
            OA_CHECK(fixture.state.phase == runtime::AdvisoryLanePhase::crossing);
            OA_CHECK(!fixture.state.entered);
            source[3] = fixture.state;
            fixture.place(
                vertical ? ground::CorridorOrigin{13, 18} : ground::CorridorOrigin{18, 13}
            );
            fixture.choose(1000);
            OA_CHECK(fixture.state.phase == runtime::AdvisoryLanePhase::crossing);
            OA_CHECK(fixture.state.entered && fixture.state.steering_active);
            source[4] = fixture.state;
            fixture.choose(0);
            OA_CHECK(fixture.state.entered && !fixture.state.steering_active);
            source[5] = fixture.state;
            std::array<uint8_t, source.size() * record_bytes> bytes{};
            bytes.fill(unwritten_byte);
            auto restored = source;
            watch_allocations = true;
            const auto before_allocations = allocation_count;
            const bool wrote = runtime::write_advisory_lane_states(source, bytes);
            const bool valid = runtime::can_read_advisory_lane_states(bytes);
            const bool read = runtime::read_advisory_lane_states(restored, bytes);
            watch_allocations = false;
            OA_CHECK(wrote && valid && read && allocation_count == before_allocations);
            for (size_t at = 0; at < source.size(); ++at) {
                OA_CHECK(equal_state(source[at], restored[at]));
                OA_CHECK(restored[at].goal == nullptr && restored[at].order == nullptr);
            }
            OA_CHECK(bytes[0] == 1);
            OA_CHECK(std::all_of(bytes.begin() + 1, bytes.begin() + record_bytes, [](uint8_t byte) {
                return byte == 0;
            }));
            std::array<uint8_t, bytes.size()> rewritten{};
            OA_CHECK(runtime::write_advisory_lane_states(restored, rewritten));
            OA_CHECK(rewritten == bytes);
            auto uninterrupted = source[4];
            auto loaded = restored[4];
            loaded.goal = uninterrupted.goal;
            loaded.order = uninterrupted.order;
            auto sampler = fixture.grid.sampler();
            ground::CorridorBudget first_budget{1000}, second_budget{1000};
            const auto first = runtime::choose_advisory_lane(
                fixture.unit,
                fixture.navigation,
                generation,
                &fixture.approach,
                &fixture.corridor,
                sampler,
                first_budget,
                uninterrupted
            );
            const auto second = runtime::choose_advisory_lane(
                fixture.unit,
                fixture.navigation,
                generation,
                &fixture.approach,
                &fixture.corridor,
                sampler,
                second_budget,
                loaded
            );
            OA_CHECK(equal_state(uninterrupted, loaded));
            OA_CHECK(
                first.active == second.active && first.steering.points == second.steering.points
            );
            OA_CHECK(first_budget.used == second_budget.used);
        }
}

/// Keeps addresses outside the format and pins explicit little-endian integer fields.
void pointers_and_endian() {
    Fixture fixture;
    std::array<runtime::AdvisoryLaneState, 1> source{fixture.state};
    std::array<uint8_t, record_bytes> first{}, second{};
    OA_CHECK(runtime::write_advisory_lane_states(source, first));
    source[0].goal = &fixture.other_goal;
    source[0].order = &fixture.other_order;
    OA_CHECK(runtime::write_advisory_lane_states(source, second));
    OA_CHECK(first == second);
    constexpr std::array<uint8_t, 12> header{
        1, 1, 1, 2, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01
    };
    OA_CHECK(std::equal(header.begin(), header.end(), first.begin()));
    constexpr size_t unused_route_point_byte = 160;
    constexpr std::array<uint8_t, 4> unused_point{0x2e, 0xfb, 0xff, 0x7f};
    OA_CHECK(
        std::equal(
            unused_point.begin(), unused_point.end(), first.begin() + unused_route_point_byte
        )
    );
    source[0].generation = 0;
    source[0].goal_geometry = 0;
    source[0].corridor.revision = 0;
    OA_CHECK(runtime::write_advisory_lane_states(source, first));
    OA_CHECK(runtime::read_advisory_lane_states(source, first));
    OA_CHECK(source[0].generation == 0 && source[0].goal_geometry == 0);
    OA_CHECK(source[0].goal == nullptr && source[0].order == nullptr);
}

/// Retains late first-enrollment state without inventing an observed route prefix.
void late_crossing_without_prefix_history() {
    for (const bool vertical : {false, true})
        for (const bool reverse : {false, true})
            for (const bool far_body : {false, true}) {
                Fixture fixture(vertical, reverse);
                const auto ordinary = fixture.state;
                const auto axis = vertical ? 1u : 0u;
                ground::CorridorOrigin cell{};
                cell[1 - axis] = fixture.state.lane;
                cell[axis] = int16_t(far_body ? (reverse ? 10 : 30) : 18);
                fixture.place(cell);
                fixture.state = {};
                fixture.choose(1000);
                OA_CHECK(fixture.decision.active);
                OA_CHECK(fixture.state.entered);
                OA_CHECK(fixture.state.phase == runtime::AdvisoryLanePhase::crossing);
                OA_CHECK(fixture.state.admitted_count == 0);
                OA_CHECK(fixture.state.entry_segment == 0 && fixture.state.exit_segment == 0);
                OA_CHECK(
                    std::all_of(
                        fixture.state.admitted_route.begin(),
                        fixture.state.admitted_route.end(),
                        [](ground::RoutePoint point) { return point == ground::RoutePoint{}; }
                    )
                );
                const auto late = fixture.state;
                fixture.choose(0);
                OA_CHECK(fixture.state.entered && !fixture.state.steering_active);
                std::array<runtime::AdvisoryLaneState, 3> source{ordinary, late, fixture.state};
                std::array<uint8_t, source.size() * record_bytes> bytes{};
                auto loaded = source;
                watch_allocations = true;
                const auto before_allocations = allocation_count;
                const bool wrote = runtime::write_advisory_lane_states(source, bytes);
                const bool valid = runtime::can_read_advisory_lane_states(bytes);
                const bool read = runtime::read_advisory_lane_states(loaded, bytes);
                watch_allocations = false;
                OA_CHECK(wrote && valid && read && allocation_count == before_allocations);
                for (size_t at = 0; at < source.size(); ++at) {
                    OA_CHECK(equal_state(source[at], loaded[at]));
                    OA_CHECK(loaded[at].goal == nullptr && loaded[at].order == nullptr);
                }
                loaded[1].goal = fixture.navigation.goal;
                loaded[1].order = fixture.navigation.goal->order;
                cell[axis] = int16_t(reverse ? 8 : 32);
                fixture.place(cell);
                auto uninterrupted = late;
                auto sampler = fixture.grid.sampler();
                ground::CorridorBudget first_budget{1000}, second_budget{1000};
                const auto first = runtime::choose_advisory_lane(
                    fixture.unit,
                    fixture.navigation,
                    generation,
                    &fixture.approach,
                    &fixture.corridor,
                    sampler,
                    first_budget,
                    uninterrupted
                );
                const auto second = runtime::choose_advisory_lane(
                    fixture.unit,
                    fixture.navigation,
                    generation,
                    &fixture.approach,
                    &fixture.corridor,
                    sampler,
                    second_budget,
                    loaded[1]
                );
                OA_CHECK(first.released && first.request_search && first.rejoin_count == 0);
                OA_CHECK(second.released && second.request_search && second.rejoin_count == 0);
                OA_CHECK(equal_state(uninterrupted, loaded[1]));
                OA_CHECK(fixture.navigation.goal == &fixture.goal);
                OA_CHECK(fixture.navigation.goal->order == &fixture.order);

                constexpr size_t phase_byte = 1, flags_byte = 3, entry_byte = 74;
                constexpr size_t exit_byte = 76, count_byte = 80, first_point_byte = 84;
                constexpr size_t last_point_byte = 160;
                constexpr std::array<std::array<size_t, 2>, 8> mutations{
                    {{phase_byte, 1},
                     {flags_byte, 2},
                     {entry_byte, 1},
                     {exit_byte, 1},
                     {count_byte, 1},
                     {count_byte, 2},
                     {first_point_byte, 1},
                     {last_point_byte, 1}}
                };
                for (const auto mutation : mutations) {
                    auto malformed = bytes;
                    malformed[record_bytes + mutation[0]] = static_cast<uint8_t>(mutation[1]);
                    auto preserved = source;
                    OA_CHECK(!runtime::can_read_advisory_lane_states(malformed));
                    OA_CHECK(!runtime::read_advisory_lane_states(preserved, malformed));
                    for (size_t at = 0; at < source.size(); ++at) {
                        OA_CHECK(equal_state(source[at], preserved[at]));
                        OA_CHECK(preserved[at].goal == source[at].goal);
                        OA_CHECK(preserved[at].order == source[at].order);
                    }
                }
                auto bad_state = source;
                bad_state[1].admitted_route.back()[0] = 1;
                auto preserved_bytes = bytes;
                OA_CHECK(!runtime::write_advisory_lane_states(bad_state, preserved_bytes));
                OA_CHECK(preserved_bytes == bytes);
            }
}

/// Rejects malformed later records before changing any earlier live state or pointer.
void malformed_state_is_atomic() {
    Fixture fixture;
    const std::array<runtime::AdvisoryLaneState, 2> source{fixture.state, fixture.state};
    std::array<uint8_t, source.size() * record_bytes> valid{};
    OA_CHECK(runtime::write_advisory_lane_states(source, valid));
    constexpr size_t phase_byte = 1, direction_byte = 2, flags_byte = 3;
    constexpr size_t corridor_id_byte = 20, axis_byte = 22, lane_count_byte = 23;
    constexpr size_t footprint_byte = 24, plot_right_byte = 40, mouth_byte = 44;
    constexpr size_t lower_lane_byte = 60, selected_lane_byte = 72, entry_byte = 74;
    constexpr size_t exit_byte = 76, reserved_byte = 78, admitted_count_byte = 80;
    constexpr std::array<std::array<size_t, 2>, 18> mutations{
        {{0, 2},
         {phase_byte, 3},
         {direction_byte, 0},
         {flags_byte, 4},
         {corridor_id_byte, 0},
         {axis_byte, 2},
         {lane_count_byte, 1},
         {footprint_byte, 0},
         {footprint_byte, 9},
         {plot_right_byte, 0},
         {mouth_byte, 0},
         {lower_lane_byte, 0},
         {selected_lane_byte, 0},
         {entry_byte, 0},
         {exit_byte, 0},
         {reserved_byte, 1},
         {admitted_count_byte, 21},
         {record_bytes - 1, 1}}
    };
    for (const auto mutation : mutations) {
        auto bytes = valid;
        bytes[record_bytes + mutation[0]] = static_cast<uint8_t>(mutation[1]);
        auto restored = source;
        OA_CHECK(!runtime::can_read_advisory_lane_states(bytes));
        OA_CHECK(!runtime::read_advisory_lane_states(restored, bytes));
        for (size_t at = 0; at < source.size(); ++at) {
            OA_CHECK(equal_state(source[at], restored[at]));
            OA_CHECK(
                restored[at].goal == source[at].goal && restored[at].order == source[at].order
            );
        }
    }
    auto bad_state = source;
    bad_state[1].lane = 123;
    constexpr uint8_t untouched_byte = 90;
    std::array<uint8_t, valid.size()> output{};
    output.fill(untouched_byte);
    OA_CHECK(!runtime::write_advisory_lane_states(bad_state, output));
    OA_CHECK(std::all_of(output.begin(), output.end(), [](uint8_t byte) {
        return byte == untouched_byte;
    }));
    bad_state[1] = {};
    bad_state[1].generation = 1;
    OA_CHECK(!runtime::write_advisory_lane_states(bad_state, output));
    std::array<runtime::AdvisoryLaneState, 1> unused{};
    std::array<uint8_t, record_bytes> unused_bytes{};
    OA_CHECK(runtime::write_advisory_lane_states(unused, unused_bytes));
    unused_bytes[4] = 1;
    OA_CHECK(!runtime::can_read_advisory_lane_states(unused_bytes));
    OA_CHECK(!runtime::read_advisory_lane_states(unused, unused_bytes));
    std::array<runtime::AdvisoryLaneState, 1> one{fixture.state};
    OA_CHECK(!runtime::read_advisory_lane_states(one, valid));
    OA_CHECK(!runtime::read_advisory_lane_states(one, std::span(valid).first(record_bytes - 1)));
    OA_CHECK(!runtime::can_read_advisory_lane_states(std::span(valid).first(record_bytes - 1)));
    OA_CHECK(!runtime::write_advisory_lane_states(one, std::span(output).first(record_bytes - 1)));
    std::array<uint8_t, record_bytes + 1> extra{};
    OA_CHECK(!runtime::write_advisory_lane_states(one, extra));
    OA_CHECK(!runtime::read_advisory_lane_states(one, extra));
    OA_CHECK(!runtime::can_read_advisory_lane_states(extra));
    OA_CHECK(runtime::write_advisory_lane_states({}, {}));
    OA_CHECK(runtime::read_advisory_lane_states({}, {}));
    OA_CHECK(runtime::can_read_advisory_lane_states({}));
}
} // namespace

/// Runs the bounded advisory-state format checks.
int main() {
    native_states_round_trip();
    pointers_and_endian();
    late_crossing_without_prefix_history();
    malformed_state_is_atomic();
    return 0;
}
