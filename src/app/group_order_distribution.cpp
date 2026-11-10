// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "group_order_distribution.hpp"
#include "group_order_static.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>

namespace oa::app {
namespace {
namespace input = sim::gameplay_input;
namespace ground = sim::ground_orders;
namespace spatial = sim::spatial_state;
namespace geometry = group_order_static_detail;

constexpr uint32_t maximum_units = 32768;
constexpr uint32_t window_side = 256;
constexpr uint32_t window_cells = window_side * window_side;
constexpr uint32_t maximum_classes = 8;
constexpr uint32_t maximum_origins = 262144;
constexpr uint32_t maximum_cells = 1048576;
constexpr int16_t maximum_footprint = 8;
constexpr int32_t halo = maximum_footprint + 1;
constexpr uint32_t reservation_side = window_side + halo * 2;
constexpr uint32_t reservation_size = reservation_side * reservation_side;
constexpr uint32_t bits_per_byte = 8;
constexpr uint32_t index_bits = 16;
constexpr uint32_t index_mask = 65535;
constexpr uint8_t repaired_flag = 128;
constexpr uint8_t assigned_flag = 64;
constexpr uint8_t unsupported_flag = 32;
constexpr uint8_t key_mask = 15;
constexpr int64_t fixed_pixel = 65536;
constexpr int64_t cell_pixels = 16;
constexpr int64_t half_cell_pixels = cell_pixels / 2;
constexpr std::array<std::array<int32_t, 2>, 4> neighbours{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

/// Holds one complete static movement and native arrival policy.
struct ClassKey {
    spatial::CellClassifier movement{};
    input::UnitOrder order{};
};

/// Holds bounded command-local storage, allocated outside the simulation tick.
struct Frontier {
    std::array<uint16_t, window_cells> frontier{};
    std::array<uint8_t, window_cells / bits_per_byte> visited{};
    uint32_t read{};
    uint32_t written{};
    uint32_t candidate{};
    bool seeded{};
};

/// Shares reservations across independently retained movement components.
struct Scratch {
    std::array<uint32_t, maximum_units> slots{};
    std::array<uint8_t, maximum_units> flags{};
    std::array<uint8_t, (reservation_size + bits_per_byte - 1) / bits_per_byte> reserved{};
    std::array<ClassKey, maximum_classes> keys{};
    std::array<std::unique_ptr<Frontier>, maximum_classes> frontiers{};
};

constexpr size_t maximum_scratch_bytes = sizeof(Scratch) + maximum_classes * sizeof(Frontier);
static_assert(maximum_scratch_bytes < 1280 * 1024);

/// Compares every destination bit, including its original height.
bool same_point(const oa::FixedVec3& a, const oa::FixedVec3& b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/// Recognizes the existing ground formation repair domain.
bool ground_actor(const input::SelectionOrder& order) noexcept {
    return order.actor != nullptr &&
           (order.actor->flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == group_order_detail::ground_layer &&
           order.actor->attach_parent == 0 &&
           (order.order == input::UnitOrder::move_ground ||
            order.order == input::UnitOrder::patrol ||
            order.order == input::UnitOrder::repair_patrol);
}

/// Recognizes footprints whose per-origin work is bounded by this allocator.
bool supported_footprint(const oa::Unit& actor) noexcept {
    return actor.footprint_x > 0 && actor.footprint_z > 0 &&
           actor.footprint_x <= maximum_footprint && actor.footprint_z <= maximum_footprint;
}

/// Compares static limits; mobile projection ticks play no part in this plan.
bool same_key(const ClassKey& a, const ClassKey& b) noexcept {
    const auto& x = a.movement;
    const auto& y = b.movement;
    return a.order == b.order && x.footprint_x == y.footprint_x && x.footprint_z == y.footprint_z &&
           x.max_water_depth == y.max_water_depth && x.min_water_depth == y.min_water_depth &&
           x.max_land_slope == y.max_land_slope && x.bad_land_slope == y.bad_land_slope &&
           x.max_water_slope == y.max_water_slope && x.bad_water_slope == y.bad_water_slope;
}

/// Reads or paints one bit in a bounded scratch bitmap.
bool bit(std::span<const uint8_t> bytes, uint32_t index) noexcept {
    return (bytes[index / bits_per_byte] & (1U << (index % bits_per_byte))) != 0;
}

/// Paints one bit whose index has already been bounded by its caller.
void paint(std::span<uint8_t> bytes, uint32_t index) noexcept {
    bytes[index / bits_per_byte] |= static_cast<uint8_t>(1U << (index % bits_per_byte));
}

/// Accounts for every static footprint and reservation-cell inspection.
struct Work {
    GroupDestinationReport& report;
    GroupDestinationLimits limits{};

    /// Reserves work before any corresponding cell inspection is performed.
    bool charge(uint32_t count) noexcept {
        if (count > limits.cells - report.cells) {
            report.exhausted = true;
            return false;
        }
        report.cells += count;
        return true;
    }

    /// Tests one native static footprint, ignoring mobile units as preset 6 does.
    bool passable(
        const spatial::CellClassifier& movement, int32_t x, int32_t z, const spatial::World& world
    ) noexcept {
        const auto area = static_cast<uint32_t>(movement.footprint_x) *
                          static_cast<uint32_t>(movement.footprint_z);
        if (report.origins >= limits.origins || !charge(area)) {
            report.exhausted = true;
            return false;
        }
        ++report.origins;
        return ground::static_flow_passable(movement, x, z, world);
    }

    /// Checks a bounded native arrival region with the same short-circuit order.
    bool arrival(
        const ground::Goal& goal,
        const spatial::CellClassifier& movement,
        const spatial::World& world
    ) noexcept {
        for (const auto& offset : geometry::arrival_offsets) {
            const int32_t x = int32_t(goal.cell[0]) + offset[0];
            const int32_t z = int32_t(goal.cell[1]) + offset[1];
            if (ground::goal_contains(goal, x, z) && passable(movement, x, z, world))
                return true;
            if (report.exhausted)
                return false;
        }
        return false;
    }
};

/// Obtains an actor's complete current static limits without canonical writes.
bool movement_for(
    GroupDestinationHooks hooks, const oa::Unit& actor, spatial::CellClassifier& movement
) noexcept {
    if (hooks.movement == nullptr || !hooks.movement(hooks.context, actor, movement))
        return false;
    movement.footprint_x = actor.footprint_x;
    movement.footprint_z = actor.footprint_z;
    movement.occupancy_before_tick = 0;
    return true;
}

/// Resolves the previous policy exactly, charging supported footprint probes.
oa::FixedVec3 fallback(
    const input::SelectionOrder& order,
    const oa::FixedVec3& click,
    const spatial::World& world,
    int32_t map_width,
    int32_t map_height,
    GroupDestinationHooks hooks,
    Work& work
) noexcept {
    if (!ground_actor(order))
        return order.position;
    const auto& actor = *order.actor;
    const auto bounded =
        bounded_group_order_point(order.position, click, actor, map_width, map_height, true);
    spatial::CellClassifier movement{};
    if (!movement_for(hooks, actor, movement))
        return bounded;
    // Unsupported or abandoned commands retain the exact pre-existing policy;
    // no new distribution is attempted after this work limit is reached.
    if (!supported_footprint(actor) || work.report.exhausted)
        return static_group_order_point(
            bounded, click, actor, order.order, movement, world, map_width, map_height, true
        );
    if (!group_order_detail::axis_fits(click.x, actor.footprint_x, map_width) ||
        !group_order_detail::axis_fits(click.z, actor.footprint_z, map_height))
        return bounded;
    sim::simulation_state::Order native{};
    const auto formation = geometry::arrival_goal(native, actor, order.order, bounded);
    const auto target = geometry::arrival_goal(native, actor, order.order, click);
    if (!formation || !target)
        return bounded;
    if (work.arrival(*formation, movement, world))
        return bounded;
    if (!work.report.exhausted && work.arrival(*target, movement, world))
        return click;
    if (work.report.exhausted)
        return static_group_order_point(
            bounded, click, actor, order.order, movement, world, map_width, map_height, true
        );
    return bounded;
}

/// Maps a checked origin back through native 16.16 footprint quantization.
std::optional<oa::FixedVec3>
point_for(int32_t x, int32_t z, const oa::Unit& actor, int32_t y) noexcept {
    const int64_t px =
        (int64_t(x) * cell_pixels + actor.footprint_x * half_cell_pixels) * fixed_pixel;
    const int64_t pz =
        (int64_t(z) * cell_pixels + actor.footprint_z * half_cell_pixels) * fixed_pixel;
    if (px < std::numeric_limits<int32_t>::min() || px > std::numeric_limits<int32_t>::max() ||
        pz < std::numeric_limits<int32_t>::min() || pz > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    return oa::FixedVec3{static_cast<int32_t>(px), y, static_cast<int32_t>(pz)};
}

/// Tests or reserves a conservative union of all possible native arrival bodies.
bool rectangle(
    const ground::Goal& goal,
    const oa::Unit& actor,
    int32_t left,
    int32_t top,
    Scratch& scratch,
    Work& work,
    bool reserve
) noexcept {
    const int32_t radius = goal.radius_squared == 0 ? 0 : 1;
    const int32_t origin_x = left - halo;
    const int32_t origin_z = top - halo;
    const int32_t x0 = std::max(int32_t(goal.cell[0]) - radius, origin_x);
    const int32_t z0 = std::max(int32_t(goal.cell[1]) - radius, origin_z);
    const int32_t x1 = std::min(
        int32_t(goal.cell[0]) + actor.footprint_x + radius,
        origin_x + static_cast<int32_t>(reservation_side)
    );
    const int32_t z1 = std::min(
        int32_t(goal.cell[1]) + actor.footprint_z + radius,
        origin_z + static_cast<int32_t>(reservation_side)
    );
    for (int32_t z = z0; z < z1; ++z)
        for (int32_t x = x0; x < x1; ++x) {
            if (!work.charge(1))
                return false;
            ++work.report.reservation_cells;
            const auto index = static_cast<uint32_t>(z - origin_z) * reservation_side +
                               static_cast<uint32_t>(x - origin_x);
            if (reserve)
                paint(scratch.reserved, index);
            else if (bit(scratch.reserved, index))
                return false;
        }
    return true;
}

} // namespace

input::SelectionOrder group_order_intent(
    const oa::World& world,
    const input::GroupCentre& centre,
    input::OrderCommand command,
    const oa::Unit& actor,
    const oa::FixedVec3& click,
    const input::OrderCursorHooks& hooks
) noexcept {
    const auto order = input::unit_order(world, command, actor, nullptr, &click, hooks);
    return {
        &actor,
        order,
        input::order_keeps_group_shape(order) ? input::group_order_point(centre, actor, click)
                                              : click
    };
}

GroupDestinationReport distribute_group_order_points(
    std::span<input::SelectionOrder> orders,
    const oa::FixedVec3& click,
    const spatial::World& world,
    int32_t map_width,
    int32_t map_height,
    GroupDestinationHooks hooks,
    GroupDestinationLimits limits
) {
    GroupDestinationReport report{};
    limits.classes = std::min(limits.classes, maximum_classes);
    limits.origins = std::min(limits.origins, maximum_origins);
    limits.cells = std::min(limits.cells, maximum_cells);
    Work work{report, limits};
    const auto scratch = std::make_unique<Scratch>();
    report.scratch_bytes = sizeof(Scratch);
    report.abandoned = orders.size() > maximum_units;
    // Apply the old policy completely before any new destination is assigned.
    for (size_t index = 0; index < orders.size(); ++index) {
        auto& order = orders[index];
        const auto previous = order.position;
        order.position = fallback(order, click, world, map_width, map_height, hooks, work);
        const bool repaired = !same_point(previous, order.position);
        if (repaired)
            ++report.repaired;
        else
            ++report.preserved;
        if (order.actor == nullptr || order.actor->id == 0 || order.actor->id >= maximum_units ||
            index >= maximum_units) {
            report.abandoned = true;
            continue;
        }
        const auto id = order.actor->id;
        if (scratch->slots[id] != 0) {
            report.abandoned = true;
            continue;
        }
        scratch->slots[id] = static_cast<uint32_t>(index + 1) << index_bits;
        if (repaired)
            scratch->flags[id] = repaired_flag;
    }
    if (report.exhausted)
        report.abandoned = true;
    if (report.abandoned || report.repaired == 0)
        return report;
    const int32_t left = static_cast<int32_t>(int64_t(click.x) / (cell_pixels * fixed_pixel)) -
                         static_cast<int32_t>(window_side / 2);
    const int32_t top = static_cast<int32_t>(int64_t(click.z) / (cell_pixels * fixed_pixel)) -
                        static_cast<int32_t>(window_side / 2);
    // A later valid actor must never lose its original arrival region to an
    // earlier repaired one. Rectangles intersecting the halo are included even
    // when their centres lie outside the allocation window.
    for (const auto& order : orders) {
        if (!ground_actor(order) || (scratch->flags[order.actor->id] & repaired_flag) != 0)
            continue;
        sim::simulation_state::Order native{};
        const auto goal = geometry::arrival_goal(native, *order.actor, order.order, order.position);
        if (goal && !rectangle(*goal, *order.actor, left, top, *scratch, work, true)) {
            report.abandoned = true;
            return report;
        }
    }
    for (uint32_t id = 1; id < maximum_units; ++id) {
        if ((scratch->flags[id] & repaired_flag) == 0)
            continue;
        const auto& order = orders[(scratch->slots[id] >> index_bits) - 1];
        ClassKey key{};
        key.order = order.order;
        if (!supported_footprint(*order.actor) ||
            !movement_for(hooks, *order.actor, key.movement)) {
            scratch->flags[id] |= unsupported_flag;
            ++report.unsupported;
            continue;
        }
        uint32_t found = 0;
        while (found < report.classes && !same_key(key, scratch->keys[found]))
            ++found;
        if (found == report.classes) {
            if (report.classes == limits.classes) {
                scratch->flags[id] |= unsupported_flag;
                ++report.unsupported;
                report.exhausted = true;
                continue;
            }
            scratch->keys[report.classes++] = key;
        }
        scratch->flags[id] |= static_cast<uint8_t>(found);
    }
    // Class admission exhaustion does not invalidate already admitted classes.
    const bool class_exhaustion = report.exhausted;
    report.exhausted = false;
    for (uint32_t id = 1; id < maximum_units && !report.exhausted; ++id) {
        if ((scratch->flags[id] & (repaired_flag | unsupported_flag)) != repaired_flag)
            continue;
        const auto& order = orders[(scratch->slots[id] >> index_bits) - 1];
        const auto& actor = *order.actor;
        const auto key_index = scratch->flags[id] & key_mask;
        const auto& key = scratch->keys[key_index];
        auto& storage = scratch->frontiers[key_index];
        if (!storage) {
            storage = std::make_unique<Frontier>();
            report.scratch_bytes += sizeof(Frontier);
            ++report.frontiers;
        }
        auto& frontier = *storage;
        const auto enqueue = [&](int32_t x, int32_t z) {
            const int32_t local_x = x - left;
            const int32_t local_z = z - top;
            if (local_x < 0 || local_z < 0 || local_x >= static_cast<int32_t>(window_side) ||
                local_z >= static_cast<int32_t>(window_side))
                return;
            const auto index =
                static_cast<uint32_t>(local_z) * window_side + static_cast<uint32_t>(local_x);
            if (bit(frontier.visited, index))
                return;
            paint(frontier.visited, index);
            const auto point = point_for(x, z, actor, click.y);
            if (!point || !group_order_detail::axis_fits(point->x, actor.footprint_x, map_width) ||
                !group_order_detail::axis_fits(point->z, actor.footprint_z, map_height))
                return;
            if (work.passable(key.movement, x, z, world))
                frontier.frontier[frontier.written++] = static_cast<uint16_t>(index);
        };
        sim::simulation_state::Order native{};
        if (!frontier.seeded) {
            frontier.seeded = true;
            const auto click_goal = geometry::arrival_goal(native, actor, key.order, click);
            if (!click_goal)
                continue;
            for (const auto& offset : geometry::arrival_offsets) {
                const int32_t x = int32_t(click_goal->cell[0]) + offset[0];
                const int32_t z = int32_t(click_goal->cell[1]) + offset[1];
                if (ground::goal_contains(*click_goal, x, z))
                    enqueue(x, z);
                if (frontier.written != 0 || report.exhausted)
                    break;
            }
        }
        // Visit origins monotonically once per complete class. Reservations
        // affect capacity, not connectivity. Retaining every admitted class's
        // cursor keeps interleaved classes in global actor-ID priority.
        while (frontier.candidate < frontier.written && !report.exhausted) {
            const uint32_t index = frontier.frontier[frontier.candidate++];
            const int32_t x = left + static_cast<int32_t>(index % window_side);
            const int32_t z = top + static_cast<int32_t>(index / window_side);
            // Expanding before testing capacity makes every reached origin
            // available to the next actor without restarting a search.
            while (frontier.read < frontier.candidate && !report.exhausted) {
                const auto expand = frontier.frontier[frontier.read++];
                const int32_t ex = left + static_cast<int32_t>(expand % window_side);
                const int32_t ez = top + static_cast<int32_t>(expand / window_side);
                for (const auto& step : neighbours)
                    enqueue(ex + step[0], ez + step[1]);
            }
            const auto point = point_for(x, z, *order.actor, click.y);
            const auto goal =
                point ? geometry::arrival_goal(native, *order.actor, order.order, *point)
                      : std::nullopt;
            if (!goal || goal->cell[0] != x || goal->cell[1] != z ||
                !rectangle(*goal, *order.actor, left, top, *scratch, work, false))
                continue;
            if (!rectangle(*goal, *order.actor, left, top, *scratch, work, true))
                break;
            scratch->slots[id] |= index;
            scratch->flags[id] |= assigned_flag;
            ++report.assigned;
            break;
        }
    }
    report.exhausted = report.exhausted || class_exhaustion || report.assigned < report.repaired;
    for (auto& order : orders) {
        const auto id = order.actor->id;
        if ((scratch->flags[id] & assigned_flag) == 0)
            continue;
        const auto index = scratch->slots[id] & index_mask;
        const int32_t x = left + static_cast<int32_t>(index % window_side);
        const int32_t z = top + static_cast<int32_t>(index / window_side);
        order.position = *point_for(x, z, *order.actor, click.y);
    }
    return report;
}

} // namespace oa::app
