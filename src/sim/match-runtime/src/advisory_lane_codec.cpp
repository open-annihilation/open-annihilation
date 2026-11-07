// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "advisory_lane_codec.hpp"

#include <algorithm>
#include <bit>

namespace oa::sim::match_runtime {
namespace {
namespace ground = ground_orders;
constexpr uint8_t record_version = 1;
constexpr uint8_t entered_flag = 1, steering_active_flag = 2;
constexpr int32_t maximum_map_side = 2047, maximum_footprint = 8;
constexpr uint16_t maximum_corridors = 64;
constexpr unsigned reserved_before_route_bytes = 2, reserved_after_route_bytes = 12;
static_assert(ground::navigation_capacity == 20);

/// Writes a fixed record through its explicit byte cursor.
struct Writer {
    std::span<uint8_t> bytes{};
    size_t at{};

    /// Writes the requested low bytes in little-endian order.
    void write(uint64_t value, unsigned count) noexcept {
        for (unsigned byte = 0; byte < count; ++byte)
            bytes[at++] = static_cast<uint8_t>(value >> (byte * 8));
    }

    /// Writes signed coordinates by their sixteen stored bits.
    void coordinate(int16_t value) noexcept { write(static_cast<uint16_t>(value), 2); }

    /// Writes an inclusive native origin rectangle in world-grid order.
    void rectangle(const ground::CorridorRectangle& value) noexcept {
        for (const auto coordinate : {value.left, value.top, value.right, value.bottom})
            this->coordinate(coordinate);
    }
};

/// Reads a structurally complete record through its explicit byte cursor.
struct Reader {
    std::span<const uint8_t> bytes{};
    size_t at{};

    /// Reads the requested bytes in little-endian order.
    uint64_t read(unsigned count) noexcept {
        uint64_t value{};
        for (unsigned byte = 0; byte < count; ++byte)
            value |= uint64_t(bytes[at++]) << (byte * 8);
        return value;
    }

    /// Restores the signed value of sixteen stored coordinate bits.
    int16_t coordinate() noexcept { return std::bit_cast<int16_t>(static_cast<uint16_t>(read(2))); }

    /// Restores one inclusive native origin rectangle in world-grid order.
    ground::CorridorRectangle rectangle() noexcept {
        const auto left = coordinate(), top = coordinate(), right = coordinate(),
                   bottom = coordinate();
        return {left, top, right, bottom};
    }

    /// Consumes reserved bytes only when every byte is zero.
    bool zeros(unsigned count) noexcept {
        bool valid = true;
        for (unsigned byte = 0; byte < count; ++byte)
            valid &= read(1) == 0;
        return valid;
    }
};

/// Checks exact equality of inclusive origin rectangles.
bool same(ground::CorridorRectangle a, ground::CorridorRectangle b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

/// Checks full-footprint extents against the detector's maximum map size.
bool valid_rectangle(
    ground::CorridorRectangle rectangle, ground::CorridorOrigin footprint
) noexcept {
    return rectangle.left >= 0 && rectangle.top >= 0 && rectangle.right >= rectangle.left &&
           rectangle.bottom >= rectangle.top &&
           int32_t(rectangle.right) + footprint[0] <= maximum_map_side &&
           int32_t(rectangle.bottom) + footprint[1] <= maximum_map_side;
}

/// Constructs an axis-aligned native origin rectangle from major and transverse limits.
ground::CorridorRectangle rectangle(
    ground::CorridorAxis axis, int32_t first, int32_t last, int32_t lower, int32_t upper
) noexcept {
    return axis == ground::CorridorAxis::x
               ? ground::
                     CorridorRectangle{int16_t(first), int16_t(lower), int16_t(last), int16_t(upper)}
               : ground::CorridorRectangle{
                     int16_t(lower), int16_t(first), int16_t(upper), int16_t(last)
                 };
}

/// Checks a two-lane proof's exact detector geometry without sampling a world.
bool valid_corridor(const ground::CorridorGeometry& corridor) noexcept {
    if (corridor.id == 0 || corridor.id > maximum_corridors ||
        (corridor.axis != ground::CorridorAxis::x && corridor.axis != ground::CorridorAxis::z) ||
        corridor.lane_count != 2 || corridor.footprint[0] <= 0 || corridor.footprint[1] <= 0 ||
        corridor.footprint[0] > maximum_footprint || corridor.footprint[1] > maximum_footprint ||
        !valid_rectangle(corridor.origins, corridor.footprint) ||
        !valid_rectangle(corridor.plots, {1, 1}) ||
        !valid_rectangle(corridor.mouths[0], corridor.footprint) ||
        !valid_rectangle(corridor.mouths[1], corridor.footprint))
        return false;
    const bool along_x = corridor.axis == ground::CorridorAxis::x;
    const int32_t first = along_x ? corridor.origins.left : corridor.origins.top;
    const int32_t last = along_x ? corridor.origins.right : corridor.origins.bottom;
    const int32_t lower = along_x ? corridor.origins.top : corridor.origins.left;
    const int32_t upper = along_x ? corridor.origins.bottom : corridor.origins.right;
    const int32_t major_footprint = corridor.footprint[along_x ? 0 : 1];
    const int32_t transverse_footprint = corridor.footprint[along_x ? 1 : 0];
    const int32_t origin_span = upper - lower + 1;
    if (last - first + 1 < std::max(4, 2 * major_footprint) ||
        origin_span < transverse_footprint + 1 || origin_span > 2 * transverse_footprint)
        return false;
    auto plots = corridor.origins;
    plots.right = int16_t(plots.right + corridor.footprint[0] - 1);
    plots.bottom = int16_t(plots.bottom + corridor.footprint[1] - 1);
    if (!same(corridor.plots, plots) ||
        !same(
            corridor.mouths[0],
            rectangle(corridor.axis, first - major_footprint, first - 1, lower, upper)
        ) ||
        !same(
            corridor.mouths[1],
            rectangle(corridor.axis, last + 1, last + major_footprint, lower, upper)
        ))
        return false;
    const auto physical_width = upper - lower + transverse_footprint;
    const auto lower_lane = lower + (physical_width - 2 * transverse_footprint) / 2;
    return corridor.lane_origins[0] == lower_lane &&
           corridor.lane_origins[1] == lower_lane + transverse_footprint;
}

/// Checks that no admitted route or claimed entry and exit indexes are retained.
bool empty_route(const AdvisoryLaneState& state) noexcept {
    if (state.admitted_count != 0 || state.entry_segment != 0 || state.exit_segment != 0)
        return false;
    for (const auto& point : state.admitted_route)
        if (point != ground::RoutePoint{})
            return false;
    return true;
}

/// Checks that an unused guide carries no residual nonpointer state.
bool empty_state(const AdvisoryLaneState& state) noexcept {
    const auto& corridor = state.corridor;
    if (state.generation != 0 || state.goal_geometry != 0 || corridor.id != 0 ||
        corridor.axis != ground::CorridorAxis::x ||
        corridor.footprint != ground::CorridorOrigin{} || !same(corridor.origins, {}) ||
        !same(corridor.plots, {}) || !same(corridor.mouths[0], {}) ||
        !same(corridor.mouths[1], {}) || corridor.lane_origins != ground::CorridorOrigin{} ||
        corridor.lane_count != 0 || corridor.revision != 0 ||
        state.direction != ground::CorridorDirection::unknown || state.lane != 0 || state.entered ||
        state.steering_active || state.admitted_count != 0 || state.entry_segment != 0 ||
        state.exit_segment != 0)
        return false;
    return empty_route(state);
}

/// Checks live helper invariants while allowing budget fallback and pre-contact crossing.
bool valid_state(const AdvisoryLaneState& state) noexcept {
    if (state.phase == AdvisoryLanePhase::none)
        return empty_state(state);
    if ((state.phase != AdvisoryLanePhase::approach &&
         state.phase != AdvisoryLanePhase::crossing) ||
        (state.entered && state.phase != AdvisoryLanePhase::crossing) ||
        !valid_corridor(state.corridor))
        return false;
    if (state.admitted_count == 0) {
        if (!state.entered || state.phase != AdvisoryLanePhase::crossing || !empty_route(state))
            return false;
    } else if (
        state.admitted_count < 2 || state.admitted_count > state.admitted_route.size() ||
        state.entry_segment == 0 || state.entry_segment > state.exit_segment ||
        state.exit_segment >= state.admitted_count
    )
        return false;
    int16_t lane{};
    return ground::corridor_lane(state.corridor, state.direction, lane) && state.lane == lane;
}

/// Writes every nonpointer field after whole-table validation has succeeded.
void write_record(const AdvisoryLaneState& state, std::span<uint8_t> bytes) noexcept {
    Writer writer{bytes};
    writer.write(record_version, 1);
    writer.write(static_cast<uint8_t>(state.phase), 1);
    writer.write(static_cast<uint8_t>(state.direction), 1);
    writer.write(
        (state.entered ? entered_flag : 0) | (state.steering_active ? steering_active_flag : 0), 1
    );
    writer.write(state.generation, 8);
    writer.write(state.goal_geometry, 8);
    const auto& corridor = state.corridor;
    writer.write(corridor.id, 2);
    writer.write(static_cast<uint8_t>(corridor.axis), 1);
    writer.write(corridor.lane_count, 1);
    for (const auto coordinate : corridor.footprint)
        writer.coordinate(coordinate);
    writer.rectangle(corridor.origins);
    writer.rectangle(corridor.plots);
    for (const auto mouth : corridor.mouths)
        writer.rectangle(mouth);
    for (const auto coordinate : corridor.lane_origins)
        writer.coordinate(coordinate);
    writer.write(corridor.revision, 8);
    writer.coordinate(state.lane);
    writer.write(state.entry_segment, 2);
    writer.write(state.exit_segment, 2);
    writer.write(0, reserved_before_route_bytes);
    writer.write(state.admitted_count, 4);
    for (const auto& point : state.admitted_route)
        for (const auto coordinate : point)
            writer.coordinate(coordinate);
    for (unsigned byte = 0; byte < reserved_after_route_bytes; ++byte)
        writer.write(0, 1);
}

/// Decodes one complete versioned record without changing caller state.
bool read_record(std::span<const uint8_t> bytes, AdvisoryLaneState& state) noexcept {
    Reader reader{bytes};
    if (reader.read(1) != record_version)
        return false;
    state = {};
    state.phase = static_cast<AdvisoryLanePhase>(reader.read(1));
    state.direction = static_cast<ground::CorridorDirection>(reader.read(1));
    const auto flags = reader.read(1);
    if (flags > (entered_flag | steering_active_flag))
        return false;
    state.entered = (flags & entered_flag) != 0;
    state.steering_active = (flags & steering_active_flag) != 0;
    state.generation = reader.read(8);
    state.goal_geometry = reader.read(8);
    auto& corridor = state.corridor;
    corridor.id = static_cast<uint16_t>(reader.read(2));
    corridor.axis = static_cast<ground::CorridorAxis>(reader.read(1));
    corridor.lane_count = static_cast<uint8_t>(reader.read(1));
    for (auto& coordinate : corridor.footprint)
        coordinate = reader.coordinate();
    corridor.origins = reader.rectangle();
    corridor.plots = reader.rectangle();
    for (auto& mouth : corridor.mouths)
        mouth = reader.rectangle();
    for (auto& coordinate : corridor.lane_origins)
        coordinate = reader.coordinate();
    corridor.revision = reader.read(8);
    state.lane = reader.coordinate();
    state.entry_segment = static_cast<uint16_t>(reader.read(2));
    state.exit_segment = static_cast<uint16_t>(reader.read(2));
    if (!reader.zeros(reserved_before_route_bytes))
        return false;
    state.admitted_count = static_cast<uint32_t>(reader.read(4));
    for (auto& point : state.admitted_route)
        for (auto& coordinate : point)
            coordinate = reader.coordinate();
    return reader.zeros(reserved_after_route_bytes) && reader.at == advisory_lane_record_bytes &&
           valid_state(state);
}
} // namespace

bool write_advisory_lane_states(
    std::span<const AdvisoryLaneState> states, std::span<uint8_t> bytes
) noexcept {
    if (bytes.size() / advisory_lane_record_bytes != states.size() ||
        bytes.size() % advisory_lane_record_bytes != 0)
        return false;
    for (const auto& state : states)
        if (!valid_state(state))
            return false;
    size_t at{};
    for (const auto& state : states) {
        write_record(state, bytes.subspan(at, advisory_lane_record_bytes));
        at += advisory_lane_record_bytes;
    }
    return true;
}

bool can_read_advisory_lane_states(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() % advisory_lane_record_bytes != 0)
        return false;
    for (size_t at = 0; at < bytes.size(); at += advisory_lane_record_bytes) {
        AdvisoryLaneState state{};
        if (!read_record(bytes.subspan(at, advisory_lane_record_bytes), state))
            return false;
    }
    return true;
}

bool read_advisory_lane_states(
    std::span<AdvisoryLaneState> states, std::span<const uint8_t> bytes
) noexcept {
    if (bytes.size() / advisory_lane_record_bytes != states.size() ||
        !can_read_advisory_lane_states(bytes))
        return false;
    size_t at{};
    for (auto& state : states) {
        (void)read_record(bytes.subspan(at, advisory_lane_record_bytes), state);
        at += advisory_lane_record_bytes;
    }
    return true;
}

} // namespace oa::sim::match_runtime
