// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/corridors.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace oa::sim::ground_orders {
namespace {
constexpr uint32_t maximum_corridors = 64, maximum_pending_seeds = 1024;
constexpr uint32_t maximum_map_cells = 2'097'152, maximum_map_side = 2047;
constexpr int16_t maximum_footprint = 8, minimum_corridor_length = 4;
constexpr uint32_t bucket_side = 8;
constexpr int64_t fixed_one = 65536, half_cell = 8 * fixed_one, cell_step = 16 * fixed_one;

bool spend(CorridorBudget& budget) noexcept {
    if (budget.remaining == 0)
        return false;
    --budget.remaining;
    ++budget.used;
    return true;
}

bool contains(const CorridorRectangle& rectangle, CorridorOrigin origin) noexcept {
    return origin[0] >= rectangle.left && origin[0] <= rectangle.right &&
           origin[1] >= rectangle.top && origin[1] <= rectangle.bottom;
}

bool intersects(const CorridorRectangle& a, const CorridorRectangle& b) noexcept {
    return a.left <= b.right && b.left <= a.right && a.top <= b.bottom && b.top <= a.bottom;
}

CorridorRectangle body(CorridorOrigin origin, CorridorOrigin footprint) noexcept {
    return {
        origin[0],
        origin[1],
        int16_t(origin[0] + footprint[0] - 1),
        int16_t(origin[1] + footprint[1] - 1)
    };
}

CorridorOrigin transpose(CorridorAxis axis, int16_t major, int16_t minor) noexcept {
    return axis == CorridorAxis::x ? CorridorOrigin{major, minor} : CorridorOrigin{minor, major};
}

CorridorRectangle
rectangle(CorridorAxis axis, int16_t first, int16_t last, int16_t lower, int16_t upper) noexcept {
    return axis == CorridorAxis::x ? CorridorRectangle{first, lower, last, upper}
                                   : CorridorRectangle{lower, first, upper, last};
}

bool same(const CorridorGeometry& a, const CorridorGeometry& b) noexcept {
    return a.axis == b.axis && a.origins.left == b.origins.left && a.origins.top == b.origins.top &&
           a.origins.right == b.origins.right && a.origins.bottom == b.origins.bottom;
}

struct RouteSegment {
    std::array<int64_t, 2> from{}, to{};
    CorridorOrigin first{}, last{};
};

bool position_origin(
    int64_t position, int16_t footprint, int64_t& biased, int16_t& origin
) noexcept {
    biased = position - int64_t(footprint) * half_cell + half_cell;
    if (biased < 0 || biased > std::numeric_limits<sim::unit_movement::Fixed>::max())
        return false;
    origin = static_cast<int16_t>(biased / cell_step);
    return true;
}

template <class Visit>
bool visit_route(const RouteSegment& segment, CorridorBudget& budget, Visit&& visit) noexcept {
    const auto emit = [&](int32_t x, int32_t z) {
        return spend(budget) && visit(CorridorOrigin{int16_t(x), int16_t(z)});
    };
    int32_t x = segment.first[0], z = segment.first[1];
    if (!emit(x, z))
        return false;
    const auto dx = segment.to[0] - segment.from[0], dz = segment.to[1] - segment.from[1];
    const int32_t sx = (dx > 0) - (dx < 0), sz = (dz > 0) - (dz < 0);
    const auto ax = dx < 0 ? -dx : dx, az = dz < 0 ? -dz : dz;
    int64_t nx = sx > 0 ? int64_t(x + 1) * cell_step - segment.from[0]
                        : segment.from[0] - int64_t(x) * cell_step;
    int64_t nz = sz > 0 ? int64_t(z + 1) * cell_step - segment.from[1]
                        : segment.from[1] - int64_t(z) * cell_step;
    while (x != segment.last[0] || z != segment.last[1]) {
        if (x == segment.last[0] || (z != segment.last[1] && nz * ax < nx * az)) {
            z += sz;
            nz += cell_step;
            if (!emit(x, z))
                return false;
        } else if (z == segment.last[1] || nx * az < nz * ax) {
            x += sx;
            nx += cell_step;
            if (!emit(x, z))
                return false;
        } else {
            if (!emit(x + sx, z) || !emit(x, z + sz))
                return false;
            x += sx;
            z += sz;
            nx += cell_step;
            nz += cell_step;
            if (!emit(x, z))
                return false;
        }
    }
    return true;
}
} // namespace

bool corridor_lane(
    const CorridorGeometry& corridor, CorridorDirection direction, int16_t& transverse
) noexcept {
    if (corridor.lane_count != 2 ||
        (corridor.axis != CorridorAxis::x && corridor.axis != CorridorAxis::z) ||
        direction == CorridorDirection::unknown ||
        (direction != CorridorDirection::increasing && direction != CorridorDirection::decreasing))
        return false;
    const bool upper =
        (corridor.axis == CorridorAxis::x) == (direction == CorridorDirection::increasing);
    transverse = corridor.lane_origins[upper ? 1 : 0];
    return true;
}

struct CorridorCatalog::State {
    struct Seed {
        CorridorOrigin origin{};
        CorridorAxis axis{};
    };
    enum class SectionPhase : uint8_t { centre, lower, upper, done };

    struct Section {
        int16_t major{}, minor{}, lower{}, upper{};
        SectionPhase phase{};
        bool valid{}, wide{};
    };
    enum class BuildPhase : uint8_t { baseline, lower, upper, lower_mouth, upper_mouth, index };

    struct Build {
        Seed seed{};
        Section section{};
        BuildPhase phase{};
        int16_t first{}, last{}, lower{}, upper{}, mouth_row{}, mouth_minor{};
        bool extension{};
        CorridorGeometry result{};
        uint32_t bucket_x{}, bucket_z{}, bucket_first_x{}, bucket_last_x{}, bucket_last_z{};
    };

    uint32_t width{}, height{}, columns{}, rows{}, bucket_count{}, corridor_capacity{},
        seed_capacity{};
    CorridorOrigin footprint{};
    std::unique_ptr<CorridorGeometry[]> corridors{};
    std::unique_ptr<Seed[]> seeds{};
    std::unique_ptr<uint64_t[]> buckets{};
    uint32_t count{}, queue_head{}, queue_count{}, clear_cursor{};
    uint64_t revision{}, work_units{}, samples{}, rejected_seeds{};
    bool clearing{}, active{}, enabled{};
    Build build{};

    State(uint32_t w, uint32_t h, CorridorOrigin foot, CorridorLimits limits) {
        if (w == 0 || h == 0 || w > maximum_map_side || h > maximum_map_side ||
            uint64_t(w) * h > maximum_map_cells || uint64_t(w) * h > limits.total_cells ||
            foot[0] <= 0 || foot[1] <= 0 || foot[0] > maximum_footprint ||
            foot[1] > maximum_footprint || uint32_t(foot[0]) > w || uint32_t(foot[1]) > h ||
            limits.corridors == 0 || limits.corridors > maximum_corridors ||
            limits.pending_seeds == 0 || limits.pending_seeds > maximum_pending_seeds)
            return;
        width = w;
        height = h;
        footprint = foot;
        columns = (w + bucket_side - 1) / bucket_side;
        rows = (h + bucket_side - 1) / bucket_side;
        bucket_count = columns * rows;
        corridor_capacity = limits.corridors;
        seed_capacity = limits.pending_seeds;
        corridors = std::make_unique<CorridorGeometry[]>(corridor_capacity);
        seeds = std::make_unique<Seed[]>(seed_capacity);
        buckets = std::make_unique<uint64_t[]>(bucket_count);
        enabled = true;
    }

    bool valid(CorridorOrigin origin) const noexcept {
        return origin[0] >= 0 && origin[1] >= 0 &&
               uint32_t(origin[0]) + uint32_t(footprint[0]) <= width &&
               uint32_t(origin[1]) + uint32_t(footprint[1]) <= height;
    }

    uint32_t bucket(CorridorOrigin origin) const noexcept {
        return uint32_t(origin[1]) / bucket_side * columns + uint32_t(origin[0]) / bucket_side;
    }

    bool known(Seed seed) const noexcept {
        if (clearing)
            return false;
        auto bits = buckets[bucket(seed.origin)];
        while (bits) {
            const auto index = static_cast<uint32_t>(std::countr_zero(bits));
            bits &= bits - 1;
            if (index < count && corridors[index].axis == seed.axis &&
                contains(corridors[index].origins, seed.origin))
                return true;
        }
        return false;
    }

    CorridorCell sample(CorridorOrigin origin, const CorridorSampler& sampler) noexcept {
        if (!valid(origin))
            return CorridorCell::blocked;
        ++samples;
        const auto value = sampler.origin(sampler.context, origin, footprint);
        return value == CorridorCell::blocked || value == CorridorCell::clear
                   ? value
                   : CorridorCell::unknown;
    }

    int16_t major_footprint() const noexcept {
        return footprint[build.seed.axis == CorridorAxis::x ? 0 : 1];
    }

    int16_t transverse_footprint() const noexcept {
        return footprint[build.seed.axis == CorridorAxis::x ? 1 : 0];
    }

    void start_section(int16_t major) noexcept {
        build.section = {};
        build.section.major = major;
        build.section.minor = build.seed.origin[build.seed.axis == CorridorAxis::x ? 1 : 0];
    }

    void section_step(const CorridorSampler& sampler) noexcept {
        auto& section = build.section;
        const auto at = section.phase == SectionPhase::centre  ? section.minor
                        : section.phase == SectionPhase::lower ? int16_t(section.lower - 1)
                                                               : int16_t(section.upper + 1);
        const auto value = sample(transpose(build.seed.axis, section.major, at), sampler);
        if (value == CorridorCell::unknown ||
            (section.phase == SectionPhase::centre && value != CorridorCell::clear)) {
            section.phase = SectionPhase::done;
            return;
        }
        if (section.phase == SectionPhase::centre) {
            section.lower = section.upper = section.minor;
            section.phase = SectionPhase::lower;
        } else if (value == CorridorCell::blocked) {
            if (section.phase == SectionPhase::lower)
                section.phase = SectionPhase::upper;
            else {
                section.valid = true;
                section.phase = SectionPhase::done;
            }
        } else {
            if (section.phase == SectionPhase::lower)
                --section.lower;
            else
                ++section.upper;
            if (int(section.upper) - section.lower + 1 > 2 * transverse_footprint()) {
                section.wide = section.valid = true;
                section.phase = SectionPhase::done;
            }
        }
    }

    void reject() noexcept {
        ++rejected_seeds;
        active = false;
    }

    void begin_mouth(bool lower) noexcept {
        build.phase = lower ? BuildPhase::lower_mouth : BuildPhase::upper_mouth;
        build.mouth_row = 0;
        build.mouth_minor = int16_t(build.lower - 1);
        build.extension = false;
    }

    void completed_section() noexcept {
        const auto& section = build.section;
        if (!section.valid) {
            reject();
            return;
        }
        if (build.phase == BuildPhase::baseline) {
            if (section.wide) {
                reject();
                return;
            }
            build.lower = section.lower;
            build.upper = section.upper;
            build.first = build.last = section.major;
            build.phase = BuildPhase::lower;
            start_section(int16_t(build.first - 1));
            return;
        }
        const bool lower = build.phase == BuildPhase::lower;
        if (!section.wide && section.lower == build.lower && section.upper == build.upper) {
            if (lower) {
                build.first = section.major;
                start_section(int16_t(build.first - 1));
            } else {
                build.last = section.major;
                start_section(int16_t(build.last + 1));
            }
        } else if (
            section.wide || (section.lower <= build.lower && section.upper >= build.upper &&
                             int(section.upper) - section.lower > int(build.upper) - build.lower)
        )
            begin_mouth(lower);
        else
            reject();
    }

    void prepare_index() noexcept {
        if (int(build.last) - build.first + 1 <
            std::max<int>(minimum_corridor_length, 2 * major_footprint())) {
            reject();
            return;
        }
        auto& result = build.result;
        result.axis = build.seed.axis;
        result.footprint = footprint;
        result.origins = rectangle(result.axis, build.first, build.last, build.lower, build.upper);
        result.plots = result.origins;
        result.plots.right = int16_t(result.plots.right + footprint[0] - 1);
        result.plots.bottom = int16_t(result.plots.bottom + footprint[1] - 1);
        const auto major_foot = major_footprint(), transverse_foot = transverse_footprint();
        result.mouths[0] = rectangle(
            result.axis,
            int16_t(build.first - major_foot),
            int16_t(build.first - 1),
            build.lower,
            build.upper
        );
        result.mouths[1] = rectangle(
            result.axis,
            int16_t(build.last + 1),
            int16_t(build.last + major_foot),
            build.lower,
            build.upper
        );
        const int physical_width = int(build.upper) - build.lower + transverse_foot;
        result.lane_count = physical_width >= 2 * transverse_foot ? 2 : 1;
        if (result.lane_count == 2) {
            result.lane_origins[0] =
                int16_t(build.lower + (physical_width - 2 * transverse_foot) / 2);
            result.lane_origins[1] = int16_t(result.lane_origins[0] + transverse_foot);
        } else
            result.lane_origins[0] = int16_t((int(build.lower) + build.upper) / 2);
        for (uint32_t index = 0; index < count; ++index)
            if (same(result, corridors[index])) {
                active = false;
                return;
            }
        if (count == corridor_capacity) {
            reject();
            return;
        }
        result.id = static_cast<uint16_t>(count + 1);
        result.revision = revision;
        const int32_t left = std::min<int32_t>(
            result.plots.left - footprint[0] + 1,
            std::min(result.mouths[0].left, result.mouths[1].left)
        );
        const int32_t top = std::min<int32_t>(
            result.plots.top - footprint[1] + 1,
            std::min(result.mouths[0].top, result.mouths[1].top)
        );
        const int32_t right = std::max<int32_t>(
            result.plots.right, std::max(result.mouths[0].right, result.mouths[1].right)
        );
        const int32_t bottom = std::max<int32_t>(
            result.plots.bottom, std::max(result.mouths[0].bottom, result.mouths[1].bottom)
        );
        build.bucket_x = build.bucket_first_x = uint32_t(std::max(left, 0)) / bucket_side;
        build.bucket_z = uint32_t(std::max(top, 0)) / bucket_side;
        build.bucket_last_x = uint32_t(std::min(right, int32_t(width - 1))) / bucket_side;
        build.bucket_last_z = uint32_t(std::min(bottom, int32_t(height - 1))) / bucket_side;
        build.phase = BuildPhase::index;
    }

    void mouth_step(const CorridorSampler& sampler) noexcept {
        const bool lower = build.phase == BuildPhase::lower_mouth;
        const auto major =
            int16_t(lower ? build.first - 1 - build.mouth_row : build.last + 1 + build.mouth_row);
        const auto value = sample(transpose(build.seed.axis, major, build.mouth_minor), sampler);
        const bool extension = build.mouth_minor < build.lower || build.mouth_minor > build.upper;
        if (value == CorridorCell::unknown || (!extension && value != CorridorCell::clear)) {
            reject();
            return;
        }
        build.extension |= extension && value == CorridorCell::clear;
        if (build.mouth_minor < build.upper + 1) {
            ++build.mouth_minor;
            return;
        }
        if (!build.extension) {
            reject();
            return;
        }
        if (++build.mouth_row < major_footprint()) {
            build.mouth_minor = int16_t(build.lower - 1);
            build.extension = false;
            return;
        }
        if (lower) {
            build.phase = BuildPhase::upper;
            start_section(int16_t(build.last + 1));
        } else
            prepare_index();
    }

    void step(const CorridorSampler& sampler) noexcept {
        if (clearing) {
            buckets[clear_cursor++] = 0;
            if (clear_cursor == bucket_count)
                clearing = false;
            return;
        }
        if (!active) {
            const auto seed = seeds[queue_head];
            queue_head = (queue_head + 1) % seed_capacity;
            --queue_count;
            if (known(seed))
                return;
            if (count == corridor_capacity) {
                ++rejected_seeds;
                return;
            }
            build = {};
            build.seed = seed;
            active = true;
            start_section(seed.origin[seed.axis == CorridorAxis::x ? 0 : 1]);
            return;
        }
        if (build.phase == BuildPhase::index) {
            buckets[build.bucket_z * columns + build.bucket_x] |= uint64_t{1} << count;
            if (build.bucket_x < build.bucket_last_x)
                ++build.bucket_x;
            else if (build.bucket_z < build.bucket_last_z) {
                build.bucket_x = build.bucket_first_x;
                ++build.bucket_z;
            } else {
                corridors[count++] = build.result;
                active = false;
            }
        } else if (build.phase == BuildPhase::lower_mouth || build.phase == BuildPhase::upper_mouth)
            mouth_step(sampler);
        else {
            section_step(sampler);
            if (build.section.phase == SectionPhase::done)
                completed_section();
        }
    }
};

CorridorCatalog::CorridorCatalog(
    uint32_t width, uint32_t height, CorridorOrigin footprint, CorridorLimits limits
)
    : state_(std::make_unique<State>(width, height, footprint, limits)) {
}

CorridorCatalog::~CorridorCatalog() = default;

void CorridorCatalog::reset(uint64_t revision) noexcept {
    auto& state = *state_;
    state.revision = revision;
    state.count = state.queue_count = state.queue_head = state.clear_cursor = 0;
    state.active = false;
    state.clearing = state.enabled;
}

CorridorRequest CorridorCatalog::request(CorridorOrigin seed, CorridorAxis axis) noexcept {
    auto& state = *state_;
    if (!state.enabled || !state.valid(seed) ||
        (axis != CorridorAxis::x && axis != CorridorAxis::z))
        return CorridorRequest::invalid;
    const State::Seed value{seed, axis};
    if (state.known(value))
        return CorridorRequest::known;
    if (state.active && state.build.seed.origin == seed && state.build.seed.axis == axis)
        return CorridorRequest::pending;
    for (uint32_t offset = 0; offset < state.queue_count; ++offset) {
        const auto& queued = state.seeds[(state.queue_head + offset) % state.seed_capacity];
        if (queued.origin == seed && queued.axis == axis)
            return CorridorRequest::pending;
    }
    if (state.queue_count == state.seed_capacity || state.count == state.corridor_capacity)
        return CorridorRequest::full;
    state.seeds[(state.queue_head + state.queue_count++) % state.seed_capacity] = value;
    return CorridorRequest::queued;
}

void CorridorCatalog::advance(CorridorBudget& budget, const CorridorSampler& sampler) noexcept {
    auto& state = *state_;
    if (!state.enabled || !sampler.origin)
        return;
    while ((state.clearing || state.active || state.queue_count != 0) && spend(budget)) {
        ++state.work_units;
        state.step(sampler);
    }
}

const CorridorGeometry* CorridorCatalog::geometry(uint16_t id) const noexcept {
    return id != 0 && id <= state_->count ? &state_->corridors[id - 1] : nullptr;
}

CorridorQuery CorridorCatalog::at_origin(
    CorridorOrigin origin, std::span<CorridorContact> output, CorridorBudget& budget
) const noexcept {
    const auto& state = *state_;
    CorridorQuery result{};
    if (!state.enabled || state.clearing || !state.valid(origin) || !spend(budget))
        return result;
    auto bits = state.buckets[state.bucket(origin)];
    while (bits) {
        if (!spend(budget))
            return result;
        const auto index = static_cast<uint32_t>(std::countr_zero(bits));
        bits &= bits - 1;
        if (index >= state.count)
            continue;
        const auto& corridor = state.corridors[index];
        CorridorRegion region{};
        if (intersects(body(origin, state.footprint), corridor.plots))
            region = CorridorRegion::body;
        else if (contains(corridor.mouths[0], origin))
            region = CorridorRegion::lower_mouth;
        else if (contains(corridor.mouths[1], origin))
            region = CorridorRegion::upper_mouth;
        else
            continue;
        if (result.count == output.size())
            return result;
        output[result.count++] = {corridor.id, region};
    }
    result.complete = true;
    return result;
}

CorridorQuery CorridorCatalog::upcoming(
    const sim::unit_movement::Unit& unit,
    const Navigation& navigation,
    std::span<CorridorApproach> output,
    CorridorBudget& budget
) const noexcept {
    const auto& state = *state_;
    CorridorQuery result{};
    if (!state.enabled || state.clearing || unit.footprint != state.footprint ||
        !state.valid(unit.cell))
        return result;
    if (!route_present(navigation) || navigation.count < 2) {
        result.complete = true;
        return result;
    }
    if (navigation.count > navigation.points.size())
        return result;
    RouteSegment segment{};
    for (size_t axis = 0; axis < 2; ++axis)
        if (!position_origin(
                unit.position[axis * 2],
                state.footprint[axis],
                segment.from[axis],
                segment.first[axis]
            ) ||
            segment.first[axis] != unit.cell[axis])
            return result;
    std::array<int16_t, maximum_corridors> slots{};
    std::fill(slots.begin(), slots.end(), int16_t{-1});
    const auto actual_body = body(unit.cell, unit.footprint);
    for (uint16_t point = 1; point < navigation.count; ++point) {
        for (size_t axis = 0; axis < 2; ++axis)
            if (!position_origin(
                    int64_t(navigation.points[point][axis]) * fixed_one,
                    state.footprint[axis],
                    segment.to[axis],
                    segment.last[axis]
                ))
                return result;
        if (!state.valid(segment.last))
            return result;
        // Map-bounded nonnegative endpoints keep the boundary products below 2^63.
        const auto visit = [&](CorridorOrigin origin) {
            std::array<CorridorContact, maximum_corridors> contacts{};
            const auto query = at_origin(origin, contacts, budget);
            for (uint32_t contact = 0; contact < query.count; ++contact) {
                const auto& hit = contacts[contact];
                const auto& corridor = state.corridors[hit.id - 1];
                auto& slot = slots[hit.id - 1];
                const auto axis = corridor.axis == CorridorAxis::x ? 0u : 1u;
                const auto delta = segment.to[axis] - segment.from[axis];
                const auto direction = delta > 0   ? CorridorDirection::increasing
                                       : delta < 0 ? CorridorDirection::decreasing
                                                   : CorridorDirection::unknown;
                if (slot < 0) {
                    if (hit.region != CorridorRegion::body || !contains(corridor.origins, origin))
                        continue;
                    if (result.count == output.size())
                        return false;
                    slot = static_cast<int16_t>(result.count);
                    output[result.count++] = {
                        hit.id, direction, point, 0, intersects(actual_body, corridor.plots), false
                    };
                }
                auto& approach = output[static_cast<size_t>(slot)];
                if (approach.direction == CorridorDirection::unknown)
                    approach.direction = direction;
                if ((hit.region == CorridorRegion::upper_mouth &&
                     approach.direction == CorridorDirection::increasing) ||
                    (hit.region == CorridorRegion::lower_mouth &&
                     approach.direction == CorridorDirection::decreasing)) {
                    approach.exits_other_end = true;
                    approach.exit_segment = point;
                }
            }
            return query.complete;
        };
        if (!visit_route(segment, budget, visit))
            return result;
        segment.from = segment.to;
        segment.first = segment.last;
    }
    result.complete = true;
    return result;
}

CorridorStatus CorridorCatalog::status() const noexcept {
    const auto& state = *state_;
    return {
        state.count,
        state.queue_count,
        state.revision,
        state.work_units,
        state.samples,
        state.rejected_seeds,
        state.clearing,
        state.active,
        state.enabled
    };
}

uint64_t CorridorCatalog::allocated_bytes() const noexcept {
    const auto& state = *state_;
    return sizeof(State) + uint64_t(state.corridor_capacity) * sizeof(CorridorGeometry) +
           uint64_t(state.seed_capacity) * sizeof(State::Seed) +
           uint64_t(state.bucket_count) * sizeof(uint64_t);
}

} // namespace oa::sim::ground_orders
