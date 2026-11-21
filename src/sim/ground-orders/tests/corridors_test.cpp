// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Native full-footprint corridor proofs, bounded construction and live route contacts.
#include "oa/sim/ground_orders/corridors.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>
#include <vector>

namespace {
bool watch_allocations{};
size_t allocation_count{};
} // namespace

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(size_t size) {
    if (watch_allocations)
        ++allocation_count;
    if (void* result = std::malloc(size == 0 ? 1 : size))
        return result;
    throw std::bad_alloc{};
}

void* operator new[](size_t size) {
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
namespace orders = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;
using Cell = orders::CorridorOrigin;
using Kind = orders::CorridorCell;
using Axis = orders::CorridorAxis;
using Direction = orders::CorridorDirection;
using Region = orders::CorridorRegion;
using Request = orders::CorridorRequest;
constexpr Cell footprint{2, 2};
constexpr int32_t fixed_one = 65536;
constexpr uint32_t maximum_passes = 200'000;

struct NativeGrid {
    uint32_t width{}, height{};
    std::vector<Kind> plots;
    uint64_t calls{};

    explicit NativeGrid(uint32_t w = 64, uint32_t h = 80)
        : width(w), height(h), plots(size_t(w) * h, Kind::blocked) {}

    void rectangle(int16_t left, int16_t top, int16_t right, int16_t bottom, Kind kind) {
        for (int32_t z = top; z <= bottom; ++z)
            for (int32_t x = left; x <= right; ++x)
                plots[size_t(z) * width + size_t(x)] = kind;
    }

    void bridge_x(int16_t first = 12, int16_t last = 29, int16_t low = 12, int16_t span = 4) {
        rectangle(first - 10, low - 8, first - 1, low + 11, Kind::clear);
        rectangle(first, low, last, low + span - 1, Kind::clear);
        rectangle(last + 1, low - 8, last + 14, low + 11, Kind::clear);
    }

    void bridge_z() {
        rectangle(4, 2, 23, 11, Kind::clear);
        rectangle(12, 12, 15, 29, Kind::clear);
        rectangle(4, 30, 23, 43, Kind::clear);
    }

    orders::CorridorSampler sampler() {
        return {
            this, [](void* context, Cell origin, Cell size) {
                auto& grid = *static_cast<NativeGrid*>(context);
                ++grid.calls;
                if (origin[0] < 0 || origin[1] < 0 || size[0] <= 0 || size[1] <= 0 ||
                    uint32_t(origin[0]) + uint32_t(size[0]) > grid.width ||
                    uint32_t(origin[1]) + uint32_t(size[1]) > grid.height)
                    return Kind::blocked;
                bool blocked{};
                for (int32_t z = 0; z < size[1]; ++z)
                    for (int32_t x = 0; x < size[0]; ++x) {
                        const auto kind =
                            grid.plots[size_t(origin[1] + z) * grid.width + size_t(origin[0] + x)];
                        if (kind == Kind::unknown)
                            return Kind::unknown;
                        blocked |= kind == Kind::blocked;
                    }
                return blocked ? Kind::blocked : Kind::clear;
            }
        };
    }
};

bool finish(orders::CorridorCatalog& catalog, NativeGrid& grid, uint32_t quantum = 31) {
    for (uint32_t pass = 0; pass < maximum_passes; ++pass) {
        const auto status = catalog.status();
        if (!status.clearing && !status.building && status.pending_seeds == 0)
            return true;
        const auto samples = grid.calls;
        orders::CorridorBudget budget{quantum, 0};
        catalog.advance(budget, grid.sampler());
        OA_CHECK(budget.used <= quantum);
        OA_CHECK(budget.remaining + budget.used == quantum);
        OA_CHECK(grid.calls - samples <= budget.used);
    }
    OA_CHECK(false);
    return false;
}

uint16_t body_id(const orders::CorridorCatalog& catalog, Cell origin) {
    std::array<orders::CorridorContact, 8> contacts{};
    orders::CorridorBudget budget{256, 0};
    const auto result = catalog.at_origin(origin, contacts, budget);
    OA_CHECK(result.complete);
    OA_CHECK(result.count <= contacts.size());
    for (uint32_t i = 0;
         i < std::min<uint32_t>(result.count, static_cast<uint32_t>(contacts.size()));
         ++i)
        if (contacts[i].region == Region::body)
            return contacts[i].id;
    OA_CHECK(false);
    return 0;
}

bool rectangle_equal(const orders::CorridorRectangle& a, const orders::CorridorRectangle& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool geometry_equal(const orders::CorridorGeometry& a, const orders::CorridorGeometry& b) {
    return a.id == b.id && a.axis == b.axis && a.footprint == b.footprint &&
           rectangle_equal(a.origins, b.origins) && rectangle_equal(a.plots, b.plots) &&
           rectangle_equal(a.mouths[0], b.mouths[0]) && rectangle_equal(a.mouths[1], b.mouths[1]) &&
           a.lane_origins == b.lane_origins && a.lane_count == b.lane_count &&
           a.revision == b.revision;
}

movement::Unit unit_at(Cell origin) {
    movement::Unit unit{};
    unit.cell = origin;
    unit.footprint = footprint;
    unit.position = {
        (int32_t(origin[0]) * 16 + 16) * fixed_one, 0, (int32_t(origin[1]) * 16 + 16) * fixed_one
    };
    return unit;
}

orders::RoutePoint point(Cell origin) {
    return {int16_t(int32_t(origin[0]) * 16 + 16), int16_t(int32_t(origin[1]) * 16 + 16)};
}

orders::Navigation route(orders::Goal& goal, std::initializer_list<Cell> origins) {
    orders::Navigation navigation{};
    navigation.goal = &goal;
    navigation.flags = orders::route_present_flag;
    for (const auto origin : origins)
        navigation.points[navigation.count++] = point(origin);
    return navigation;
}

const orders::CorridorApproach*
approach(const std::array<orders::CorridorApproach, 8>& approaches, uint32_t count, uint16_t id) {
    for (uint32_t i = 0; i < std::min<uint32_t>(count, static_cast<uint32_t>(approaches.size()));
         ++i)
        if (approaches[i].id == id)
            return &approaches[i];
    OA_CHECK(false);
    return nullptr;
}

void rotated_geometry_and_right_hand_lanes() {
    for (const auto axis : {Axis::x, Axis::z}) {
        NativeGrid grid;
        if (axis == Axis::x)
            grid.bridge_x();
        else
            grid.bridge_z();
        orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
        catalog.reset(17);
        OA_CHECK(
            catalog.request(axis == Axis::x ? Cell{20, 12} : Cell{12, 20}, axis) == Request::queued
        );
        if (!finish(catalog, grid))
            continue;
        OA_CHECK(catalog.status().corridors == 1);
        const auto id = body_id(catalog, axis == Axis::x ? Cell{20, 12} : Cell{12, 20});
        const auto* geometry = catalog.geometry(id);
        OA_CHECK(geometry != nullptr);
        if (!geometry)
            continue;
        OA_CHECK(geometry->axis == axis && geometry->revision == 17);
        OA_CHECK(geometry->footprint == footprint);
        const orders::CorridorRectangle expected = axis == Axis::x
                                                       ? orders::CorridorRectangle{11, 12, 29, 14}
                                                       : orders::CorridorRectangle{12, 11, 14, 29};
        OA_CHECK(rectangle_equal(geometry->origins, expected));
        OA_CHECK(geometry->plots.left == geometry->origins.left);
        OA_CHECK(geometry->plots.top == geometry->origins.top);
        OA_CHECK(geometry->plots.right == geometry->origins.right + footprint[0] - 1);
        OA_CHECK(geometry->plots.bottom == geometry->origins.bottom + footprint[1] - 1);
        OA_CHECK(geometry->lane_count == 2);
        const auto transverse = axis == Axis::x ? 1u : 0u;
        OA_CHECK(geometry->lane_origins[0] + footprint[transverse] <= geometry->lane_origins[1]);
        int16_t increasing = -1, decreasing = -1, unknown = -7;
        OA_CHECK(orders::corridor_lane(*geometry, Direction::increasing, increasing));
        OA_CHECK(orders::corridor_lane(*geometry, Direction::decreasing, decreasing));
        OA_CHECK(increasing == geometry->lane_origins[axis == Axis::x ? 1 : 0]);
        OA_CHECK(decreasing == geometry->lane_origins[axis == Axis::x ? 0 : 1]);
        OA_CHECK(!orders::corridor_lane(*geometry, Direction::unknown, unknown));
        OA_CHECK(unknown == -7);
        OA_CHECK(!orders::corridor_lane(*geometry, static_cast<Direction>(255), unknown));
        auto invalid_axis = *geometry;
        invalid_axis.axis = static_cast<Axis>(255);
        OA_CHECK(!orders::corridor_lane(invalid_axis, Direction::increasing, unknown));
        OA_CHECK(unknown == -7);
        OA_CHECK(
            catalog.request(axis == Axis::x ? Cell{23, 13} : Cell{13, 23}, axis) == Request::known
        );
    }
}

void physical_width_distinguishes_one_lane_and_wide_rooms() {
    for (const int16_t physical_width :
         {int16_t(2), int16_t(3), int16_t(4), int16_t(5), int16_t(6)}) {
        NativeGrid grid;
        grid.bridge_x(12, 29, 12, physical_width);
        orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
        OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::queued);
        if (!finish(catalog, grid))
            continue;
        if (physical_width == 6) {
            OA_CHECK(catalog.status().corridors == 0);
            continue;
        }
        const auto* geometry = catalog.geometry(body_id(catalog, {20, 12}));
        OA_CHECK(geometry != nullptr);
        if (!geometry)
            continue;
        OA_CHECK(geometry->origins.bottom - geometry->origins.top + 1 == physical_width - 1);
        OA_CHECK(geometry->lane_count == (physical_width >= 4 ? 2 : 1));
        int16_t lane = -9;
        if (physical_width < 4) {
            OA_CHECK(!orders::corridor_lane(*geometry, Direction::increasing, lane));
            OA_CHECK(lane == -9);
        } else {
            OA_CHECK(geometry->lane_origins[0] + footprint[1] <= geometry->lane_origins[1]);
            OA_CHECK(geometry->lane_origins[0] >= geometry->origins.top);
            OA_CHECK(geometry->lane_origins[1] <= geometry->origins.bottom);
        }
    }
}

void minimum_length_covers_two_longitudinal_footprints() {
    for (const auto size : {Cell{2, 2}, Cell{3, 2}}) {
        for (const bool long_enough : {false, true}) {
            NativeGrid grid;
            const int16_t last = int16_t(11 + size[0] + (long_enough ? 1 : 0));
            grid.bridge_x(12, last);
            orders::CorridorCatalog catalog(grid.width, grid.height, size);
            OA_CHECK(catalog.request({12, 12}, Axis::x) == Request::queued);
            if (!finish(catalog, grid))
                continue;
            OA_CHECK(catalog.status().corridors == (long_enough ? 1u : 0u));
            if (long_enough) {
                const auto* geometry = catalog.geometry(1);
                OA_CHECK(geometry != nullptr);
                if (geometry)
                    OA_CHECK(geometry->origins.right - geometry->origins.left + 1 == 2 * size[0]);
            }
        }
    }
}

void separate_corridors_and_overlapping_mouths_keep_every_identifier() {
    NativeGrid grid;
    grid.bridge_x();
    grid.bridge_x(12, 29, 40);
    orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
    OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::queued);
    OA_CHECK(catalog.request({20, 40}, Axis::x) == Request::queued);
    OA_CHECK(catalog.request({21, 13}, Axis::x) == Request::queued);
    if (!finish(catalog, grid))
        return;
    OA_CHECK(catalog.status().corridors == 2);
    OA_CHECK(body_id(catalog, {20, 12}) != body_id(catalog, {20, 40}));

    NativeGrid connected;
    connected.rectangle(2, 4, 11, 23, Kind::clear);
    connected.rectangle(12, 12, 21, 15, Kind::clear);
    connected.rectangle(22, 4, 25, 23, Kind::clear);
    connected.rectangle(26, 12, 35, 15, Kind::clear);
    connected.rectangle(36, 4, 49, 23, Kind::clear);
    orders::CorridorCatalog shared(connected.width, connected.height, footprint);
    OA_CHECK(shared.request({16, 12}, Axis::x) == Request::queued);
    OA_CHECK(shared.request({30, 12}, Axis::x) == Request::queued);
    if (!finish(shared, connected))
        return;
    const auto first = body_id(shared, {16, 12});
    const auto second = body_id(shared, {30, 12});
    OA_CHECK(first != second && shared.status().corridors == 2);
    std::array<orders::CorridorContact, 8> contacts{};
    orders::CorridorBudget budget{256, 0};
    const auto result = shared.at_origin({23, 12}, contacts, budget);
    OA_CHECK(result.complete && result.count == 2);
    bool upper{}, lower{};
    for (uint32_t i = 0;
         i < std::min<uint32_t>(result.count, static_cast<uint32_t>(contacts.size()));
         ++i) {
        upper |= contacts[i].id == first && contacts[i].region == Region::upper_mouth;
        lower |= contacts[i].id == second && contacts[i].region == Region::lower_mouth;
    }
    OA_CHECK(upper && lower);
    budget = {256, 0};
    const auto body_priority = shared.at_origin({10, 12}, contacts, budget);
    OA_CHECK(body_priority.complete && body_priority.count == 1);
    OA_CHECK(contacts[0].id == first && contacts[0].region == Region::body);
    std::array<orders::CorridorContact, 1> short_output{};
    budget = {256, 0};
    const auto truncated = shared.at_origin({23, 12}, short_output, budget);
    OA_CHECK(!truncated.complete && truncated.count <= short_output.size());
}

void unknown_proof_cells_dead_ends_and_disconnections_are_rejected() {
    constexpr std::array<Cell, 5> unknown_cells{{{20, 12}, {20, 11}, {10, 12}, {9, 11}, {31, 12}}};
    for (const auto cell : unknown_cells) {
        NativeGrid grid;
        grid.bridge_x();
        grid.rectangle(cell[0], cell[1], cell[0], cell[1], Kind::unknown);
        orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
        OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::queued);
        if (finish(catalog, grid)) {
            OA_CHECK(catalog.status().corridors == 0);
            OA_CHECK(catalog.status().rejected_seeds != 0);
        }
    }
    for (const bool disconnected : {false, true}) {
        NativeGrid grid;
        grid.bridge_x();
        if (disconnected)
            grid.rectangle(20, 12, 20, 15, Kind::blocked);
        else
            grid.rectangle(30, 4, 43, 23, Kind::blocked);
        orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
        OA_CHECK(catalog.request({15, 12}, Axis::x) == Request::queued);
        if (finish(catalog, grid))
            OA_CHECK(catalog.status().corridors == 0);
    }
    NativeGrid unrelated;
    unrelated.bridge_x();
    unrelated.rectangle(50, 60, 50, 60, Kind::unknown);
    orders::CorridorCatalog local(unrelated.width, unrelated.height, footprint);
    OA_CHECK(local.request({20, 12}, Axis::x) == Request::queued);
    if (finish(local, unrelated))
        OA_CHECK(local.status().corridors == 1);
}

void incremental_budgets_capacity_and_revisions() {
    NativeGrid grid;
    grid.bridge_x();
    grid.bridge_x(12, 29, 36);
    grid.bridge_x(12, 29, 60);
    orders::CorridorLimits limits{2, 1, grid.width * grid.height};
    orders::CorridorCatalog catalog(grid.width, grid.height, footprint, limits);
    catalog.reset(5);
    OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::queued);
    OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::pending);
    OA_CHECK(catalog.request({20, 36}, Axis::x) == Request::full);
    const auto before = catalog.status();
    const auto samples = grid.calls;
    orders::CorridorBudget zero{0, 9};
    catalog.advance(zero, grid.sampler());
    OA_CHECK(zero.remaining == 0 && zero.used == 9);
    OA_CHECK(catalog.status().work_units == before.work_units && grid.calls == samples);
    if (!finish(catalog, grid, 1))
        return;
    const auto first = body_id(catalog, {20, 12});
    OA_CHECK(catalog.request({20, 36}, Axis::x) == Request::queued);
    if (!finish(catalog, grid, 1))
        return;
    OA_CHECK(catalog.status().corridors == 2);
    OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::known);
    OA_CHECK(catalog.request({20, 36}, Axis::x) == Request::known);
    const auto full_samples = grid.calls;
    OA_CHECK(catalog.request({20, 60}, Axis::x) == Request::full);
    OA_CHECK(grid.calls == full_samples);
    std::array<orders::CorridorContact, 1> output{{{777, Region::upper_mouth}}};
    zero = {0, 0};
    const auto no_work = catalog.at_origin({20, 12}, output, zero);
    OA_CHECK(no_work.count == 0 && !no_work.complete);
    OA_CHECK(output[0].id == 777 && zero.used == 0);
    orders::CorridorBudget one{1, 0};
    const auto one_work = catalog.at_origin({20, 12}, output, one);
    OA_CHECK(one.used <= 1 && one.remaining + one.used == 1);
    OA_CHECK(one_work.count <= output.size());
    catalog.reset(6);
    OA_CHECK(catalog.geometry(first) == nullptr);
    OA_CHECK(catalog.status().corridors == 0 && catalog.status().revision == 6);
    orders::CorridorBudget clearing_budget{256, 0};
    const auto invalidated = catalog.at_origin({20, 12}, output, clearing_budget);
    OA_CHECK(invalidated.count == 0 && !invalidated.complete);
    grid.rectangle(20, 12, 20, 15, Kind::blocked);
    OA_CHECK(catalog.request({15, 12}, Axis::x) == Request::queued);
    if (finish(catalog, grid, 1)) {
        OA_CHECK(catalog.status().corridors == 0);
        OA_CHECK(catalog.geometry(first) == nullptr);
    }
    OA_CHECK(catalog.request({-1, 12}, Axis::x) == Request::invalid);
    OA_CHECK(catalog.request({64, 12}, Axis::x) == Request::invalid);
    OA_CHECK(catalog.request({20, 12}, static_cast<Axis>(255)) == Request::invalid);
}

void exhausted_capacity_does_not_sample_queued_novel_seeds() {
    NativeGrid grid;
    grid.bridge_x();
    grid.bridge_x(12, 29, 40);
    const orders::CorridorLimits limits{1, 2, grid.width * grid.height};
    orders::CorridorCatalog one(grid.width, grid.height, footprint, limits);
    orders::CorridorCatalog two(grid.width, grid.height, footprint, limits);
    OA_CHECK(one.request({20, 12}, Axis::x) == Request::queued);
    OA_CHECK(two.request({20, 12}, Axis::x) == Request::queued);
    OA_CHECK(two.request({20, 40}, Axis::x) == Request::queued);
    const auto start = grid.calls;
    OA_CHECK(finish(one, grid, 1));
    const auto first_samples = grid.calls - start;
    const auto middle = grid.calls;
    OA_CHECK(finish(two, grid, 1));
    OA_CHECK(grid.calls - middle == first_samples);
    OA_CHECK(one.status().corridors == 1 && two.status().corridors == 1);
    OA_CHECK(two.status().rejected_seeds != 0);
    const auto* a = one.geometry(body_id(one, {20, 12}));
    const auto* b = two.geometry(body_id(two, {20, 12}));
    OA_CHECK(a && b);
    if (a && b)
        OA_CHECK(geometry_equal(*a, *b));
}

void invalid_construction_is_disabled_without_sampler_work() {
    NativeGrid grid;
    grid.bridge_x();
    const auto disabled =
        [&](uint32_t width, uint32_t height, Cell size, orders::CorridorLimits limits) {
            orders::CorridorCatalog catalog(width, height, size, limits);
            OA_CHECK(!catalog.status().enabled);
            OA_CHECK(catalog.request({0, 0}, Axis::x) == Request::invalid);
            const auto samples = grid.calls;
            orders::CorridorBudget budget{1, 0};
            catalog.advance(budget, grid.sampler());
            OA_CHECK(budget.used == 0 && budget.remaining == 1 && grid.calls == samples);
            OA_CHECK(catalog.geometry(0) == nullptr && catalog.geometry(1) == nullptr);
        };
    disabled(0, grid.height, footprint, {});
    disabled(grid.width, 0, footprint, {});
    disabled(grid.width, grid.height, {0, 2}, {});
    disabled(grid.width, grid.height, footprint, {0, 1, grid.width * grid.height});
    disabled(grid.width, grid.height, footprint, {1, 0, grid.width * grid.height});
    disabled(grid.width, grid.height, footprint, {1, 1, 1});
}

void construction_is_deterministic_and_does_not_allocate_after_construction() {
    NativeGrid grid;
    grid.bridge_x();
    grid.bridge_x(12, 29, 40);
    orders::CorridorCatalog small(grid.width, grid.height, footprint);
    orders::CorridorCatalog large(grid.width, grid.height, footprint);
    const auto bytes = small.allocated_bytes();
    OA_CHECK(bytes > 0 && bytes == large.allocated_bytes());
    allocation_count = 0;
    watch_allocations = true;
    small.reset(19);
    large.reset(19);
    for (auto* catalog : {&small, &large}) {
        OA_CHECK(catalog->request({20, 12}, Axis::x) == Request::queued);
        OA_CHECK(catalog->request({20, 40}, Axis::x) == Request::queued);
    }
    const bool first_done = finish(small, grid, 1);
    const bool second_done = finish(large, grid, 4096);
    if (first_done && second_done) {
        OA_CHECK(small.status().samples == large.status().samples);
        for (const auto cell : {Cell{20, 12}, Cell{20, 40}}) {
            const auto a = body_id(small, cell);
            const auto b = body_id(large, cell);
            OA_CHECK(a == b);
            const auto* first = small.geometry(a);
            const auto* second = large.geometry(b);
            OA_CHECK(first && second);
            if (first && second)
                OA_CHECK(geometry_equal(*first, *second));
        }
        orders::Goal goal{};
        auto navigation = route(goal, {{2, 70}, {40, 12}});
        auto unit = unit_at({6, 12});
        std::array<orders::CorridorApproach, 8> output{};
        orders::CorridorBudget budget{1024, 0};
        OA_CHECK(small.upcoming(unit, navigation, output, budget).complete);
    }
    small.reset(20);
    OA_CHECK(finish(small, grid, 1));
    watch_allocations = false;
    OA_CHECK(allocation_count == 0);
    OA_CHECK(small.allocated_bytes() == bytes);
}

void upcoming_uses_actual_pose_live_routes_and_both_ends() {
    NativeGrid grid;
    grid.bridge_x();
    orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
    OA_CHECK(catalog.request({20, 12}, Axis::x) == Request::queued);
    if (!finish(catalog, grid))
        return;
    const auto id = body_id(catalog, {20, 12});
    orders::Goal goal{};
    goal.cell = {40, 12};
    auto unit = unit_at({6, 12});
    auto navigation = route(goal, {{55, 60}, {40, 12}});
    const auto original = navigation;
    std::array<orders::CorridorApproach, 8> output{};
    orders::CorridorBudget budget{4096, 0};
    auto query = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(query.complete && query.count == 1);
    const auto* increasing = approach(output, query.count, id);
    if (increasing) {
        OA_CHECK(increasing->direction == Direction::increasing);
        OA_CHECK(increasing->entry_segment == 1 && increasing->exit_segment == 1);
        OA_CHECK(increasing->exits_other_end && !increasing->already_inside);
    }
    OA_CHECK(navigation.points == original.points && navigation.count == original.count);
    OA_CHECK(navigation.goal == &goal && navigation.flags == original.flags);
    navigation = route(goal, {{40, 12}, {2, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(query.complete && query.count == 0);
    unit = unit_at({40, 12});
    navigation = route(goal, {{2, 12}, {6, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(query.complete && query.count == 1);
    const auto* decreasing = approach(output, query.count, id);
    if (decreasing) {
        OA_CHECK(decreasing->direction == Direction::decreasing);
        OA_CHECK(decreasing->exits_other_end && !decreasing->already_inside);
    }
    unit = unit_at({20, 12});
    navigation = route(goal, {{2, 12}, {40, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    const auto* inside = approach(output, query.count, id);
    if (inside) {
        OA_CHECK(inside->already_inside && inside->direction == Direction::increasing);
        OA_CHECK(inside->exits_other_end && inside->exit_segment == 1);
    }
    unit = unit_at({6, 12});
    navigation = route(goal, {{55, 60}, {20, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    const auto* prefix = approach(output, query.count, id);
    if (prefix) {
        OA_CHECK(prefix->direction == Direction::increasing && prefix->entry_segment == 1);
        OA_CHECK(prefix->exit_segment == 0 && !prefix->exits_other_end);
    }
    navigation = route(goal, {{55, 60}, {20, 12}, {40, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    const auto* extended = approach(output, query.count, id);
    if (extended)
        OA_CHECK(
            extended->entry_segment == 1 && extended->exit_segment == 2 && extended->exits_other_end
        );
    unit = unit_at({20, 6});
    navigation = route(goal, {{55, 60}, {20, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    const auto* transverse = approach(output, query.count, id);
    if (transverse)
        OA_CHECK(transverse->direction == Direction::unknown && !transverse->exits_other_end);
    budget = {0, 0};
    const auto empty_budget = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(empty_budget.count == 0 && !empty_budget.complete && budget.used == 0);
    budget = {1, 0};
    const auto tiny = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(budget.used <= 1 && budget.remaining + budget.used == 1);
    OA_CHECK(tiny.count <= output.size());
}

void upcoming_retains_both_corridors_and_rotates_with_the_map() {
    NativeGrid grid;
    grid.rectangle(2, 4, 11, 23, Kind::clear);
    grid.rectangle(12, 12, 21, 15, Kind::clear);
    grid.rectangle(22, 4, 25, 23, Kind::clear);
    grid.rectangle(26, 12, 35, 15, Kind::clear);
    grid.rectangle(36, 4, 49, 23, Kind::clear);
    orders::CorridorCatalog catalog(grid.width, grid.height, footprint);
    OA_CHECK(catalog.request({16, 12}, Axis::x) == Request::queued);
    OA_CHECK(catalog.request({30, 12}, Axis::x) == Request::queued);
    if (!finish(catalog, grid))
        return;
    const auto first = body_id(catalog, {16, 12});
    const auto second = body_id(catalog, {30, 12});
    orders::Goal goal{};
    auto navigation = route(goal, {{50, 60}, {18, 12}, {42, 12}});
    auto unit = unit_at({6, 12});
    std::array<orders::CorridorApproach, 8> output{};
    orders::CorridorBudget budget{4096, 0};
    auto query = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(query.complete && query.count == 2);
    const auto* a = approach(output, query.count, first);
    const auto* b = approach(output, query.count, second);
    if (a && b) {
        OA_CHECK(a->entry_segment == 1 && a->exit_segment == 2);
        OA_CHECK(b->entry_segment == 2 && b->exit_segment == 2);
        OA_CHECK(a->direction == Direction::increasing && b->direction == Direction::increasing);
        OA_CHECK(a->exits_other_end && b->exits_other_end);
    }
    std::array<orders::CorridorApproach, 1> short_output{};
    budget = {4096, 0};
    const auto truncated = catalog.upcoming(unit, navigation, short_output, budget);
    OA_CHECK(!truncated.complete && truncated.count <= short_output.size());
    unit = unit_at({42, 12});
    navigation = route(goal, {{50, 60}, {6, 12}});
    budget = {4096, 0};
    query = catalog.upcoming(unit, navigation, output, budget);
    OA_CHECK(query.complete && query.count == 2);
    for (const auto id : {first, second}) {
        const auto* reverse = approach(output, query.count, id);
        if (reverse)
            OA_CHECK(reverse->direction == Direction::decreasing && reverse->exits_other_end);
    }

    NativeGrid rotated;
    rotated.bridge_z();
    orders::CorridorCatalog vertical(rotated.width, rotated.height, footprint);
    OA_CHECK(vertical.request({12, 20}, Axis::z) == Request::queued);
    if (!finish(vertical, rotated))
        return;
    const auto id = body_id(vertical, {12, 20});
    for (const bool reverse : {false, true}) {
        unit = unit_at({12, int16_t(reverse ? 40 : 6)});
        navigation = route(goal, {{50, 60}, {12, int16_t(reverse ? 6 : 40)}});
        budget = {4096, 0};
        query = vertical.upcoming(unit, navigation, output, budget);
        OA_CHECK(query.complete && query.count == 1);
        const auto* contact = approach(output, query.count, id);
        if (contact) {
            OA_CHECK(
                contact->direction == (reverse ? Direction::decreasing : Direction::increasing)
            );
            OA_CHECK(contact->entry_segment == 1 && contact->exit_segment == 1);
            OA_CHECK(!contact->already_inside && contact->exits_other_end);
        }
    }
}

} // namespace

int main() {
    rotated_geometry_and_right_hand_lanes();
    physical_width_distinguishes_one_lane_and_wide_rooms();
    minimum_length_covers_two_longitudinal_footprints();
    separate_corridors_and_overlapping_mouths_keep_every_identifier();
    unknown_proof_cells_dead_ends_and_disconnections_are_rejected();
    incremental_budgets_capacity_and_revisions();
    exhausted_capacity_does_not_sample_queued_novel_seeds();
    invalid_construction_is_disabled_without_sampler_work();
    construction_is_deterministic_and_does_not_allocate_after_construction();
    upcoming_uses_actual_pose_live_routes_and_both_ends();
    upcoming_retains_both_corridors_and_rotates_with_the_map();
    return oa::test::check_exit_status();
}
