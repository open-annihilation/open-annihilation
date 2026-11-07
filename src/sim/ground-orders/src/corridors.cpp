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
constexpr uint32_t catalog_state_magic = 0x524f434f, catalog_state_format = 1;
constexpr size_t state_header_bytes = 128, state_builder_bytes = 160;
constexpr size_t state_geometry_bytes = 64, state_seed_bytes = 8;
constexpr uint32_t state_active_flag = 1, state_clearing_flag = 2;

struct StateWriter {
    std::span<uint8_t> bytes;
    size_t cursor{};

    void integer(uint64_t value, size_t width) noexcept {
        for (size_t byte = 0; byte < width; ++byte)
            bytes[cursor++] = static_cast<uint8_t>(value >> (byte * 8));
    }

    void signed16(int16_t value) noexcept { integer(static_cast<uint16_t>(value), 2); }

    void zero(size_t count) noexcept {
        std::fill_n(bytes.begin() + static_cast<ptrdiff_t>(cursor), count, uint8_t{});
        cursor += count;
    }

    void origin(CorridorOrigin value) noexcept {
        signed16(value[0]);
        signed16(value[1]);
    }

    void rectangle(const CorridorRectangle& value) noexcept {
        signed16(value.left);
        signed16(value.top);
        signed16(value.right);
        signed16(value.bottom);
    }

    void geometry(const CorridorGeometry& value) noexcept {
        integer(value.id, 2);
        integer(static_cast<uint8_t>(value.axis), 1);
        integer(value.lane_count, 1);
        origin(value.footprint);
        rectangle(value.origins);
        rectangle(value.plots);
        rectangle(value.mouths[0]);
        rectangle(value.mouths[1]);
        origin(value.lane_origins);
        integer(value.revision, 8);
        zero(12);
    }
};

struct StateReader {
    std::span<const uint8_t> bytes;
    size_t cursor{};
    bool valid{true};

    uint64_t integer(size_t width) noexcept {
        uint64_t value{};
        for (size_t byte = 0; byte < width; ++byte)
            value |= uint64_t(bytes[cursor++]) << (byte * 8);
        return value;
    }

    int16_t signed16() noexcept {
        return std::bit_cast<int16_t>(static_cast<uint16_t>(integer(2)));
    }

    bool boolean() noexcept {
        const auto value = integer(1);
        valid &= value <= 1;
        return value != 0;
    }

    void zero(size_t count) noexcept {
        for (size_t byte = 0; byte < count; ++byte)
            valid &= bytes[cursor++] == 0;
    }

    CorridorOrigin origin() noexcept { return {signed16(), signed16()}; }

    CorridorRectangle rectangle() noexcept {
        return {signed16(), signed16(), signed16(), signed16()};
    }

    CorridorGeometry geometry() noexcept {
        CorridorGeometry value{};
        value.id = static_cast<uint16_t>(integer(2));
        const auto axis = integer(1);
        valid &= axis <= static_cast<uint8_t>(CorridorAxis::z);
        value.axis = static_cast<CorridorAxis>(axis);
        value.lane_count = static_cast<uint8_t>(integer(1));
        value.footprint = origin();
        value.origins = rectangle();
        value.plots = rectangle();
        value.mouths[0] = rectangle();
        value.mouths[1] = rectangle();
        value.lane_origins = origin();
        value.revision = integer(8);
        zero(12);
        return value;
    }
};

bool same_rectangle(const CorridorRectangle& a, const CorridorRectangle& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool empty_geometry(const CorridorGeometry& geometry) noexcept {
    std::array<uint8_t, state_geometry_bytes> bytes{};
    StateWriter writer{bytes};
    writer.geometry(geometry);
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; });
}

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

    size_t wire_size() const noexcept;
    void encode(std::span<uint8_t> output) const noexcept;
    bool decode(std::span<const uint8_t> input) noexcept;
    bool valid_geometry(const CorridorGeometry& geometry) const noexcept;
    bool valid_build() const noexcept;
    bool valid_buckets() const noexcept;
};

size_t CorridorCatalog::State::wire_size() const noexcept {
    return enabled ? state_header_bytes + state_builder_bytes +
                         size_t(corridor_capacity) * state_geometry_bytes +
                         size_t(seed_capacity) * state_seed_bytes + size_t(bucket_count) * 8
                   : 0;
}

void CorridorCatalog::State::encode(std::span<uint8_t> output) const noexcept {
    StateWriter writer{output};
    writer.integer(catalog_state_magic, 4);
    writer.integer(catalog_state_format, 4);
    writer.integer(width, 4);
    writer.integer(height, 4);
    writer.origin(footprint);
    for (const auto value :
         {corridor_capacity,
          seed_capacity,
          columns,
          rows,
          count,
          queue_head,
          queue_count,
          clear_cursor})
        writer.integer(value, 4);
    writer.integer((active ? state_active_flag : 0) | (clearing ? state_clearing_flag : 0), 4);
    for (const auto value : {revision, work_units, samples, rejected_seeds, uint64_t(wire_size())})
        writer.integer(value, 8);
    writer.zero(32);
    const auto seed = [&](const Seed& value) {
        writer.origin(value.origin);
        writer.integer(static_cast<uint8_t>(value.axis), 1);
        writer.zero(3);
    };
    seed(build.seed);
    for (const auto value :
         {build.section.major, build.section.minor, build.section.lower, build.section.upper})
        writer.signed16(value);
    writer.integer(static_cast<uint8_t>(build.section.phase), 1);
    writer.integer(build.section.valid, 1);
    writer.integer(build.section.wide, 1);
    writer.zero(5);
    writer.integer(static_cast<uint8_t>(build.phase), 1);
    writer.zero(1);
    for (const auto value :
         {build.first, build.last, build.lower, build.upper, build.mouth_row, build.mouth_minor})
        writer.signed16(value);
    writer.integer(build.extension, 1);
    writer.zero(9);
    writer.geometry(build.result);
    for (const auto value :
         {build.bucket_x,
          build.bucket_z,
          build.bucket_first_x,
          build.bucket_last_x,
          build.bucket_last_z})
        writer.integer(value, 4);
    writer.zero(28);
    for (uint32_t index = 0; index < corridor_capacity; ++index)
        writer.geometry(corridors[index]);
    for (uint32_t index = 0; index < seed_capacity; ++index)
        seed(seeds[index]);
    for (uint32_t index = 0; index < bucket_count; ++index)
        writer.integer(buckets[index], 8);
}

bool CorridorCatalog::State::valid_geometry(const CorridorGeometry& geometry) const noexcept {
    if (empty_geometry(geometry))
        return true;
    if (geometry.id > corridor_capacity || geometry.footprint != footprint ||
        (geometry.axis != CorridorAxis::x && geometry.axis != CorridorAxis::z))
        return false;
    const auto valid_rectangle = [&](const CorridorRectangle& value) {
        return value.left <= value.right && value.top <= value.bottom &&
               valid({value.left, value.top}) && valid({value.right, value.bottom});
    };
    if (!valid_rectangle(geometry.origins) || !valid_rectangle(geometry.mouths[0]) ||
        !valid_rectangle(geometry.mouths[1]))
        return false;
    const auto first =
        geometry.axis == CorridorAxis::x ? geometry.origins.left : geometry.origins.top;
    const auto last =
        geometry.axis == CorridorAxis::x ? geometry.origins.right : geometry.origins.bottom;
    const auto lower =
        geometry.axis == CorridorAxis::x ? geometry.origins.top : geometry.origins.left;
    const auto upper =
        geometry.axis == CorridorAxis::x ? geometry.origins.bottom : geometry.origins.right;
    const auto major_foot = footprint[geometry.axis == CorridorAxis::x ? 0 : 1];
    const auto transverse_foot = footprint[geometry.axis == CorridorAxis::x ? 1 : 0];
    if (int(last) - first + 1 < std::max<int>(minimum_corridor_length, 2 * major_foot) ||
        int(upper) - lower + 1 > 2 * transverse_foot)
        return false;
    auto plots = geometry.origins;
    plots.right = int16_t(plots.right + footprint[0] - 1);
    plots.bottom = int16_t(plots.bottom + footprint[1] - 1);
    if (!same_rectangle(geometry.plots, plots) ||
        !same_rectangle(
            geometry.mouths[0],
            rectangle(geometry.axis, int16_t(first - major_foot), int16_t(first - 1), lower, upper)
        ) ||
        !same_rectangle(
            geometry.mouths[1],
            rectangle(geometry.axis, int16_t(last + 1), int16_t(last + major_foot), lower, upper)
        ))
        return false;
    const int physical_width = int(upper) - lower + transverse_foot;
    if (physical_width >= 2 * transverse_foot) {
        const auto lane = int16_t(lower + (physical_width - 2 * transverse_foot) / 2);
        return geometry.lane_count == 2 && geometry.lane_origins[0] == lane &&
               geometry.lane_origins[1] == lane + transverse_foot;
    }
    return geometry.lane_count == 1 && geometry.lane_origins[0] == (int(lower) + upper) / 2 &&
           geometry.lane_origins[1] == 0;
}

namespace {
CorridorRectangle index_rectangle(
    const CorridorGeometry& geometry, CorridorOrigin footprint, uint32_t width, uint32_t height
) noexcept {
    const int32_t left = std::min<int32_t>(
        geometry.plots.left - footprint[0] + 1,
        std::min(geometry.mouths[0].left, geometry.mouths[1].left)
    );
    const int32_t top = std::min<int32_t>(
        geometry.plots.top - footprint[1] + 1,
        std::min(geometry.mouths[0].top, geometry.mouths[1].top)
    );
    const int32_t right = std::max<int32_t>(
        geometry.plots.right, std::max(geometry.mouths[0].right, geometry.mouths[1].right)
    );
    const int32_t bottom = std::max<int32_t>(
        geometry.plots.bottom, std::max(geometry.mouths[0].bottom, geometry.mouths[1].bottom)
    );
    return {
        int16_t(uint32_t(std::max(left, 0)) / bucket_side),
        int16_t(uint32_t(std::max(top, 0)) / bucket_side),
        int16_t(uint32_t(std::min(right, int32_t(width - 1))) / bucket_side),
        int16_t(uint32_t(std::min(bottom, int32_t(height - 1))) / bucket_side)
    };
}
} // namespace

bool CorridorCatalog::State::valid_build() const noexcept {
    const auto& value = build;
    if (!valid(value.seed.origin) ||
        (value.seed.axis != CorridorAxis::x && value.seed.axis != CorridorAxis::z))
        return false;
    const auto major_axis = value.seed.axis == CorridorAxis::x ? 0u : 1u;
    const auto minor_axis = 1u - major_axis;
    const int major_max = int(major_axis == 0 ? width : height) - footprint[major_axis];
    const int minor_max = int(minor_axis == 0 ? width : height) - footprint[minor_axis];
    const auto& section = value.section;
    if (section.major < -1 || section.major > major_max + 1 ||
        section.minor != value.seed.origin[minor_axis] || section.lower < 0 ||
        section.upper < section.lower || section.upper > minor_max ||
        int(section.upper) - section.lower + 1 > 2 * footprint[minor_axis] + 1 ||
        (section.wide && (!section.valid || section.phase != SectionPhase::done)))
        return false;
    if (section.phase == SectionPhase::centre &&
        (section.lower != 0 || section.upper != 0 || section.valid || section.wide))
        return false;
    if (section.phase != SectionPhase::done && (section.valid || section.wide))
        return false;
    const bool contains_minor = section.lower <= section.minor && section.minor <= section.upper;
    if ((section.phase == SectionPhase::lower || section.phase == SectionPhase::upper) &&
        (!contains_minor ||
         (section.phase == SectionPhase::lower && section.upper != section.minor)))
        return false;
    if (!section.wide && int(section.upper) - section.lower + 1 > 2 * footprint[minor_axis])
        return false;
    if (section.valid && (section.minor < section.lower || section.minor > section.upper))
        return false;
    if (section.wide && int(section.upper) - section.lower + 1 != 2 * footprint[minor_axis] + 1)
        return false;
    if (value.first < 0 || value.first > value.last || value.last > major_max || value.lower < 0 ||
        value.lower > value.upper || value.upper > minor_max || value.mouth_row < 0 ||
        value.mouth_row > footprint[major_axis] || value.mouth_minor < -1 ||
        value.mouth_minor > minor_max + 1)
        return false;
    if (value.phase == BuildPhase::baseline) {
        if (value.first != 0 || value.last != 0 || value.lower != 0 || value.upper != 0 ||
            section.major != value.seed.origin[major_axis])
            return false;
    } else if (
        value.first > value.seed.origin[major_axis] || value.last < value.seed.origin[major_axis] ||
        value.lower > value.seed.origin[minor_axis] ||
        value.upper < value.seed.origin[minor_axis] ||
        int(value.upper) - value.lower + 1 > 2 * footprint[minor_axis]
    )
        return false;
    if ((value.phase == BuildPhase::lower || value.phase == BuildPhase::lower_mouth) &&
        section.major != value.first - 1)
        return false;
    if ((value.phase == BuildPhase::upper || value.phase == BuildPhase::upper_mouth ||
         value.phase == BuildPhase::index) &&
        section.major != value.last + 1)
        return false;
    if (active &&
        (value.phase == BuildPhase::baseline || value.phase == BuildPhase::lower ||
         value.phase == BuildPhase::upper) &&
        section.phase == SectionPhase::done)
        return false;
    if ((value.phase == BuildPhase::lower_mouth || value.phase == BuildPhase::upper_mouth ||
         value.phase == BuildPhase::index) &&
        (section.phase != SectionPhase::done || !section.valid))
        return false;
    if (active &&
        (value.phase == BuildPhase::lower_mouth || value.phase == BuildPhase::upper_mouth) &&
        value.mouth_row == footprint[major_axis])
        return false;
    const bool empty = empty_geometry(value.result);
    if (!valid_geometry(value.result))
        return false;
    if (!empty && (value.result.axis != value.seed.axis ||
                   !same_rectangle(
                       value.result.origins,
                       rectangle(value.seed.axis, value.first, value.last, value.lower, value.upper)
                   )))
        return false;
    if (value.phase == BuildPhase::index) {
        if (empty || value.result.id == 0 || value.mouth_row != footprint[major_axis] ||
            value.mouth_minor != value.upper + 1 || !value.extension ||
            (active && (count >= corridor_capacity || value.result.id != count + 1 ||
                        value.result.revision != revision)))
            return false;
        const auto bounds = index_rectangle(value.result, footprint, width, height);
        if (value.bucket_first_x != uint32_t(bounds.left) ||
            value.bucket_last_x != uint32_t(bounds.right) ||
            value.bucket_last_z != uint32_t(bounds.bottom) ||
            value.bucket_x < uint32_t(bounds.left) || value.bucket_x > uint32_t(bounds.right) ||
            value.bucket_z < uint32_t(bounds.top) || value.bucket_z > uint32_t(bounds.bottom))
            return false;
    } else if (
        value.bucket_x || value.bucket_z || value.bucket_first_x || value.bucket_last_x ||
        value.bucket_last_z ||
        (!empty && (active || value.phase != BuildPhase::upper_mouth || value.result.id != 0 ||
                    value.result.revision != 0))
    )
        return false;
    return true;
}

bool CorridorCatalog::State::valid_buckets() const noexcept {
    const auto allowed = corridor_capacity == maximum_corridors
                             ? std::numeric_limits<uint64_t>::max()
                             : (uint64_t{1} << corridor_capacity) - 1;
    std::array<CorridorRectangle, maximum_corridors> bounds{};
    for (uint32_t index = 0; index < count; ++index)
        bounds[index] = index_rectangle(corridors[index], footprint, width, height);
    CorridorRectangle partial{};
    const bool indexing = active && build.phase == BuildPhase::index;
    if (indexing)
        partial = index_rectangle(build.result, footprint, width, height);
    for (uint32_t index = 0; index < bucket_count; ++index) {
        const auto bits = buckets[index];
        if ((bits & ~allowed) != 0)
            return false;
        if (clearing) {
            if (index < clear_cursor && bits != 0)
                return false;
            continue;
        }
        const CorridorOrigin at{int16_t(index % columns), int16_t(index / columns)};
        uint64_t expected{};
        for (uint32_t corridor = 0; corridor < count; ++corridor)
            if (contains(bounds[corridor], at))
                expected |= uint64_t{1} << corridor;
        if (indexing && contains(partial, at) &&
            (uint32_t(at[1]) < build.bucket_z ||
             (uint32_t(at[1]) == build.bucket_z && uint32_t(at[0]) < build.bucket_x)))
            expected |= uint64_t{1} << count;
        if (bits != expected)
            return false;
    }
    return true;
}

bool CorridorCatalog::State::decode(std::span<const uint8_t> input) noexcept {
    if (!enabled || input.size() != wire_size())
        return false;
    StateReader reader{input};
    if (reader.integer(4) != catalog_state_magic || reader.integer(4) != catalog_state_format ||
        reader.integer(4) != width || reader.integer(4) != height || reader.origin() != footprint ||
        reader.integer(4) != corridor_capacity || reader.integer(4) != seed_capacity ||
        reader.integer(4) != columns || reader.integer(4) != rows)
        return false;
    count = static_cast<uint32_t>(reader.integer(4));
    queue_head = static_cast<uint32_t>(reader.integer(4));
    queue_count = static_cast<uint32_t>(reader.integer(4));
    clear_cursor = static_cast<uint32_t>(reader.integer(4));
    const auto flags = reader.integer(4);
    if ((flags & ~(uint64_t(state_active_flag) | state_clearing_flag)) != 0 ||
        count > corridor_capacity || queue_head >= seed_capacity || queue_count > seed_capacity ||
        clear_cursor > bucket_count)
        return false;
    active = (flags & state_active_flag) != 0;
    clearing = (flags & state_clearing_flag) != 0;
    if ((clearing && (active || count != 0 || clear_cursor == bucket_count)) ||
        (!clearing && clear_cursor != 0 && clear_cursor != bucket_count))
        return false;
    revision = reader.integer(8);
    work_units = reader.integer(8);
    samples = reader.integer(8);
    rejected_seeds = reader.integer(8);
    if (reader.integer(8) != wire_size() || samples > work_units)
        return false;
    reader.zero(32);
    const auto seed = [&](Seed& value) {
        value.origin = reader.origin();
        const auto axis = reader.integer(1);
        reader.valid &= axis <= static_cast<uint8_t>(CorridorAxis::z);
        value.axis = static_cast<CorridorAxis>(axis);
        reader.zero(3);
        reader.valid &= valid(value.origin);
    };
    seed(build.seed);
    build.section.major = reader.signed16();
    build.section.minor = reader.signed16();
    build.section.lower = reader.signed16();
    build.section.upper = reader.signed16();
    const auto section_phase = reader.integer(1);
    reader.valid &= section_phase <= static_cast<uint8_t>(SectionPhase::done);
    build.section.phase = static_cast<SectionPhase>(section_phase);
    build.section.valid = reader.boolean();
    build.section.wide = reader.boolean();
    reader.zero(5);
    const auto phase = reader.integer(1);
    reader.valid &= phase <= static_cast<uint8_t>(BuildPhase::index);
    build.phase = static_cast<BuildPhase>(phase);
    reader.zero(1);
    build.first = reader.signed16();
    build.last = reader.signed16();
    build.lower = reader.signed16();
    build.upper = reader.signed16();
    build.mouth_row = reader.signed16();
    build.mouth_minor = reader.signed16();
    build.extension = reader.boolean();
    reader.zero(9);
    build.result = reader.geometry();
    build.bucket_x = static_cast<uint32_t>(reader.integer(4));
    build.bucket_z = static_cast<uint32_t>(reader.integer(4));
    build.bucket_first_x = static_cast<uint32_t>(reader.integer(4));
    build.bucket_last_x = static_cast<uint32_t>(reader.integer(4));
    build.bucket_last_z = static_cast<uint32_t>(reader.integer(4));
    reader.zero(28);
    for (uint32_t index = 0; index < corridor_capacity; ++index) {
        auto& geometry = corridors[index];
        geometry = reader.geometry();
        const bool empty = empty_geometry(geometry);
        if (!valid_geometry(geometry) || (!empty && geometry.id != index + 1) ||
            (index < count && (empty || geometry.revision != revision)))
            return false;
        if (index < count)
            for (uint32_t prior = 0; prior < index; ++prior)
                if (same(geometry, corridors[prior]))
                    return false;
    }
    for (uint32_t index = 0; index < seed_capacity; ++index)
        seed(seeds[index]);
    for (uint32_t index = 0; index < bucket_count; ++index)
        buckets[index] = reader.integer(8);
    if (!reader.valid || reader.cursor != input.size() || !valid_build() || !valid_buckets())
        return false;
    for (uint32_t offset = 0; offset < queue_count; ++offset) {
        const auto& value = seeds[(queue_head + offset) % seed_capacity];
        if (active && value.origin == build.seed.origin && value.axis == build.seed.axis)
            return false;
        for (uint32_t prior = 0; prior < offset; ++prior) {
            const auto& other = seeds[(queue_head + prior) % seed_capacity];
            if (value.origin == other.origin && value.axis == other.axis)
                return false;
        }
    }
    return true;
}

CorridorCatalog::CorridorCatalog(
    uint32_t width, uint32_t height, CorridorOrigin footprint, CorridorLimits limits
)
    : state_(std::make_unique<State>(width, height, footprint, limits)),
      restore_state_(std::make_unique<State>(width, height, footprint, limits)) {
    saved_size_ = state_->wire_size();
    if (saved_size_ != 0)
        saved_bytes_ = std::make_unique<uint8_t[]>(saved_size_);
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
    const auto native = sizeof(State) +
                        uint64_t(state.corridor_capacity) * sizeof(CorridorGeometry) +
                        uint64_t(state.seed_capacity) * sizeof(State::Seed) +
                        uint64_t(state.bucket_count) * sizeof(uint64_t);
    return sizeof(CorridorCatalog) + native * 2 + saved_size_;
}

size_t CorridorCatalog::saved_size() const noexcept {
    return saved_size_;
}

bool CorridorCatalog::write_state(std::span<uint8_t> output) const noexcept {
    if (saved_size_ == 0 || output.size() != saved_size_)
        return false;
    state_->encode(output);
    return true;
}

std::span<const uint8_t> CorridorCatalog::state_bytes() const noexcept {
    if (saved_size_ == 0)
        return {};
    const std::span<uint8_t> output{saved_bytes_.get(), saved_size_};
    state_->encode(output);
    return output;
}

bool CorridorCatalog::can_restore(std::span<const uint8_t> input) const noexcept {
    return restore_state_->decode(input);
}

bool CorridorCatalog::restore(std::span<const uint8_t> input) noexcept {
    if (!restore_state_->decode(input))
        return false;
    state_.swap(restore_state_);
    return true;
}

} // namespace oa::sim::ground_orders
