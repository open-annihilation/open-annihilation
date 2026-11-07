// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "advisory_flow_state.hpp"

#include "advisory_lane_codec.hpp"
#include "enhanced_state.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>

namespace oa::sim::match_runtime {
namespace {

constexpr uint32_t save_magic = 0x3546414f, save_version = 1;
constexpr size_t header_bytes = 96, catalog_metadata_bytes = 320, identity_bytes = 16;
constexpr uint32_t tick_valid_flag = 1;
constexpr size_t validation_ticks_offset = 40, validation_flags_offset = 296;

/// Reads one explicitly little-endian integer from an already bounded field.
uint64_t read_integer(const uint8_t* bytes, unsigned count) noexcept {
    uint64_t value{};
    for (unsigned byte = 0; byte < count; ++byte)
        value |= uint64_t(bytes[byte]) << (byte * 8);
    return value;
}

/// Writes one explicitly little-endian integer into an already bounded field.
void write_integer(uint8_t* bytes, uint64_t value, unsigned count) noexcept {
    for (unsigned byte = 0; byte < count; ++byte)
        bytes[byte] = static_cast<uint8_t>(value >> (byte * 8));
}

/// Reads a four-byte unsigned field.
uint32_t read32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(read_integer(bytes, 4));
}

/// Reads a signed native cell coordinate from its stored sixteen bits.
int16_t read_coordinate(const uint8_t* bytes) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(read_integer(bytes, 2)));
}

/// Checks every reserved byte rather than accepting another record interpretation.
bool zeros(std::span<const uint8_t> bytes) noexcept {
    for (const auto byte : bytes)
        if (byte != 0)
            return false;
    return true;
}

/// Tests a saved cursor over allocated unit slots, excluding the reserved slot.
bool valid_unit_cursor(uint32_t cursor, size_t unit_count) noexcept {
    return cursor >= 1 && (unit_count > 1 ? cursor < unit_count : cursor == 1);
}

/// Checks a pool's complete remaining and consumed finite work allowance.
bool valid_budget(
    const uint8_t* bytes, uint32_t limit, uint32_t override_limit, bool tick_valid
) noexcept {
    const auto remaining = read32(bytes), used = read32(bytes + 4);
    if (!tick_valid)
        return remaining == 0 && used == 0;
    return uint64_t(remaining) + used == std::min(limit, override_limit);
}

/// Reads a pool after all input bytes and budgets have been validated.
ground_orders::CorridorBudget read_budget(const uint8_t* bytes) noexcept {
    return {read32(bytes), read32(bytes + 4)};
}

/// Writes one pool without depending on native field layout.
void write_budget(uint8_t* bytes, const ground_orders::CorridorBudget& budget) noexcept {
    write_integer(bytes, budget.remaining, 4);
    write_integer(bytes + 4, budget.used, 4);
}

/// Tests complete physical extents against this match's map.
bool fits_map(
    ground_orders::CorridorRectangle rectangle,
    ground_orders::CorridorOrigin footprint,
    uint32_t width,
    uint32_t height
) noexcept {
    return rectangle.left >= 0 && rectangle.top >= 0 && rectangle.right >= rectangle.left &&
           rectangle.bottom >= rectangle.top && footprint[0] > 0 && footprint[1] > 0 &&
           uint64_t(rectangle.right) + uint32_t(footprint[0]) <= width &&
           uint64_t(rectangle.bottom) + uint32_t(footprint[1]) <= height;
}

/// Reads the sampled physical rectangle from its fixed metadata fields.
ground_orders::CorridorRectangle sampled_rectangle(const uint8_t* bytes) noexcept {
    return {
        read_coordinate(bytes + 24),
        read_coordinate(bytes + 26),
        read_coordinate(bytes + 28),
        read_coordinate(bytes + 30)
    };
}

/// Reads a stable unit and order identity without retaining an address.
AdvisoryIdentity read_identity(const uint8_t* bytes) noexcept {
    AdvisoryIdentity value;
    value.generation = read_integer(bytes, 8);
    value.order_issue_tick = read32(bytes + 8);
    value.unit_type = static_cast<uint16_t>(read_integer(bytes + 12, 2));
    value.owner = bytes[14];
    value.order_kind = bytes[15];
    return value;
}

/// Writes a stable identity, excluding its transient pointer-rebinding marker.
void write_identity(uint8_t* bytes, const AdvisoryIdentity& identity) noexcept {
    write_integer(bytes, identity.generation, 8);
    write_integer(bytes + 8, identity.order_issue_tick, 4);
    write_integer(bytes + 12, identity.unit_type, 2);
    bytes[14] = identity.owner;
    bytes[15] = identity.order_kind;
}

} // namespace

size_t advisory_flow_saved_size(const AdvisoryFlowData& state) noexcept {
    if (state.catalog_count > state.catalogs.size() ||
        state.unit_count > std::numeric_limits<uint32_t>::max())
        return 0;
    constexpr size_t unit_bytes =
        identity_bytes + advisory_lane_record_bytes + unit_movement::local_flow_head_bytes;
    if (state.unit_count > (advisory_storage_limit - header_bytes) / unit_bytes)
        return 0;
    size_t size = header_bytes + state.unit_count * unit_bytes;
    for (uint32_t index = 0; index < state.catalog_count; ++index) {
        const auto& slot = state.catalogs[index];
        if (!slot.catalog)
            return 0;
        const auto catalog_bytes = slot.catalog->saved_size();
        if (catalog_bytes == 0 || catalog_bytes > advisory_storage_limit - size ||
            catalog_metadata_bytes > advisory_storage_limit - size - catalog_bytes)
            return 0;
        size += catalog_metadata_bytes + catalog_bytes;
    }
    return size;
}

std::span<const uint8_t> Match::advisory_state_bytes() noexcept {
    if (!enhanced_ || !enhanced_->advisory)
        return {};
    auto& state = *enhanced_->advisory;
    if (!state.save_bytes || state.save_size < header_bytes ||
        state.save_size != advisory_flow_saved_size(state) ||
        (state.unit_count != 0 && (!state.guides || !state.identities || !state.published_guides)))
        return {};
    std::span<uint8_t> bytes{state.save_bytes.get(), state.save_size};
    std::fill(bytes.begin(), bytes.end(), uint8_t{0});
    auto* header = bytes.data();
    write_integer(header, save_magic, 4);
    write_integer(header + 4, save_version, 4);
    write_integer(header + 8, bytes.size(), 8);
    write_integer(header + 16, state.unit_count, 4);
    write_integer(header + 20, state.catalog_count, 4);
    write_integer(header + 24, state.catalog_cursor, 4);
    write_integer(header + 28, state.tick, 4);
    write_integer(header + 32, state.enrollment_begin, 4);
    write_integer(header + 36, state.enrollment_next, 4);
    write_integer(header + 40, state.budget_override, 4);
    write_integer(header + 44, state.tick_valid ? tick_valid_flag : 0, 4);
    write_budget(header + 48, state.construction);
    write_budget(header + 56, state.query);
    write_budget(header + 64, state.active);
    write_integer(header + 72, advisory_lane_record_bytes, 4);
    write_integer(header + 76, identity_bytes, 4);
    write_integer(header + 80, catalog_metadata_bytes, 4);
    write_integer(header + 84, state.active_cursor, 4);
    write_integer(header + 88, state.active_begin, 4);
    header[92] = state.active_stopped ? 1 : 0;
    size_t at = header_bytes;
    for (uint32_t index = 0; index < state.catalog_count; ++index) {
        const auto& slot = state.catalogs[index];
        const auto catalog_bytes = slot.catalog->state_bytes();
        if (catalog_bytes.size() != slot.catalog->saved_size() ||
            slot.revision != slot.catalog->status().revision)
            return {};
        auto* metadata = bytes.data() + at;
        write_integer(metadata, slot.movement_class, 4);
        write_integer(metadata + 4, slot.player, 4);
        write_integer(metadata + 16, catalog_bytes.size(), 8);
        const std::array<int16_t, 4> sampled{
            slot.sampled.left, slot.sampled.top, slot.sampled.right, slot.sampled.bottom
        };
        for (size_t coordinate = 0; coordinate < sampled.size(); ++coordinate)
            write_integer(metadata + 24 + coordinate * 2, uint16_t(sampled[coordinate]), 2);
        metadata[32] = slot.sampled_valid ? 1 : 0;
        for (size_t corridor = 0; corridor < slot.validated_tick.size(); ++corridor) {
            write_integer(
                metadata + validation_ticks_offset + corridor * 4, slot.validated_tick[corridor], 4
            );
            if (slot.validated[corridor])
                metadata[validation_flags_offset + corridor / 8] |= uint8_t(1u << (corridor % 8));
        }
        at += catalog_metadata_bytes;
        std::copy(catalog_bytes.begin(), catalog_bytes.end(), bytes.begin() + at);
        at += catalog_bytes.size();
    }
    for (size_t unit = 0; unit < state.unit_count; ++unit) {
        write_identity(bytes.data() + at, state.identities[unit]);
        at += identity_bytes;
    }
    const size_t guide_bytes = state.unit_count * advisory_lane_record_bytes;
    if (!write_advisory_lane_states(
            {state.guides.get(), state.unit_count}, bytes.subspan(at, guide_bytes)
        ))
        return {};
    at += guide_bytes;
    if (!unit_movement::write_local_flow_heads(
            {state.published_guides.get(), state.unit_count}, bytes.subspan(at)
        ))
        return {};
    return bytes;
}

bool Match::restore_advisory_state(std::span<const uint8_t> bytes) noexcept {
    if (!enhanced_ || !enhanced_->advisory)
        return false;
    auto& state = *enhanced_->advisory;
    if (bytes.size() < header_bytes || bytes.size() != state.save_size ||
        state.save_size != advisory_flow_saved_size(state) ||
        (state.unit_count != 0 && (!state.guides || !state.identities || !state.published_guides)))
        return false;
    const auto* header = bytes.data();
    const auto flags = read32(header + 44), override_limit = read32(header + 40);
    const bool tick_valid = flags == tick_valid_flag;
    const auto catalog_cursor = read32(header + 24);
    if (read32(header) != save_magic || read32(header + 4) != save_version ||
        read_integer(header + 8, 8) != bytes.size() || read32(header + 16) != state.unit_count ||
        read32(header + 20) != state.catalog_count || flags > tick_valid_flag ||
        (state.catalog_count == 0 ? catalog_cursor != 0 : catalog_cursor >= state.catalog_count) ||
        !valid_unit_cursor(read32(header + 32), state.unit_count) ||
        !valid_unit_cursor(read32(header + 36), state.unit_count) ||
        !valid_budget(header + 48, advisory_construction_work, override_limit, tick_valid) ||
        !valid_budget(header + 56, advisory_query_work, override_limit, tick_valid) ||
        !valid_budget(header + 64, advisory_active_work, override_limit, tick_valid) ||
        read32(header + 72) != advisory_lane_record_bytes ||
        read32(header + 76) != identity_bytes || read32(header + 80) != catalog_metadata_bytes ||
        !valid_unit_cursor(read32(header + 84), state.unit_count) ||
        !valid_unit_cursor(read32(header + 88), state.unit_count) || header[92] > 1 ||
        !zeros(bytes.subspan(93, header_bytes - 93)))
        return false;
    size_t at = header_bytes;
    for (uint32_t index = 0; index < state.catalog_count; ++index) {
        const auto& slot = state.catalogs[index];
        const auto catalog_bytes = slot.catalog->saved_size();
        const auto metadata = bytes.subspan(at, catalog_metadata_bytes);
        if (read32(metadata.data()) != slot.movement_class ||
            read32(metadata.data() + 4) != slot.player || !zeros(metadata.subspan(8, 8)) ||
            read_integer(metadata.data() + 16, 8) != catalog_bytes || metadata[32] > 1 ||
            !fits_map(
                sampled_rectangle(metadata.data()),
                {1, 1},
                spatial_.terrain_width,
                spatial_.terrain_height
            ) ||
            !zeros(metadata.subspan(33, 7)) || !zeros(metadata.subspan(304, 16)))
            return false;
        at += catalog_metadata_bytes;
        if (!slot.catalog->can_restore(bytes.subspan(at, catalog_bytes)))
            return false;
        at += catalog_bytes;
    }
    const size_t identities_at = at;
    const size_t guides_at = identities_at + state.unit_count * identity_bytes;
    const size_t published_at = guides_at + state.unit_count * advisory_lane_record_bytes;
    const auto guide_bytes =
        bytes.subspan(guides_at, state.unit_count * advisory_lane_record_bytes);
    const auto published_bytes = bytes.subspan(published_at);
    if (!can_read_advisory_lane_states(guide_bytes))
        return false;
    for (size_t unit = 0; unit < state.unit_count; ++unit) {
        unit_movement::LocalFlowHead head;
        if (!unit_movement::read_local_flow_heads(
                {&head, 1},
                published_bytes.subspan(
                    unit * unit_movement::local_flow_head_bytes,
                    unit_movement::local_flow_head_bytes
                )
            ))
            return false;
    }
    for (size_t unit = 0; unit < state.unit_count; ++unit) {
        const auto identity = read_identity(bytes.data() + identities_at + unit * identity_bytes);
        if (identity.owner >= OA_PLAYER_COUNT || identity.unit_type >= input_.types.size())
            return false;
        AdvisoryLaneState guide;
        const bool decoded = read_advisory_lane_states(
            {&guide, 1},
            guide_bytes.subspan(unit * advisory_lane_record_bytes, advisory_lane_record_bytes)
        );
        assert(decoded);
        if (!decoded)
            return false;
        if (guide.phase == AdvisoryLanePhase::none)
            continue;
        if (identity.unit_type == 0 || identity.unit_type >= input_.fields.size() ||
            guide.generation != identity.generation ||
            !fits_map(
                guide.corridor.origins,
                guide.corridor.footprint,
                spatial_.terrain_width,
                spatial_.terrain_height
            ) ||
            !fits_map(
                guide.corridor.plots, {1, 1}, spatial_.terrain_width, spatial_.terrain_height
            ) ||
            !fits_map(
                guide.corridor.mouths[0],
                guide.corridor.footprint,
                spatial_.terrain_width,
                spatial_.terrain_height
            ) ||
            !fits_map(
                guide.corridor.mouths[1],
                guide.corridor.footprint,
                spatial_.terrain_width,
                spatial_.terrain_height
            ))
            return false;
        const auto movement_class = input_.fields[identity.unit_type].movement_class;
        bool supported = false;
        for (uint32_t index = 0; index < state.catalog_count; ++index) {
            const auto& slot = state.catalogs[index];
            if (slot.player != identity.owner || slot.movement_class >= movement_classes_.size())
                continue;
            const auto& klass = movement_classes_[slot.movement_class];
            if (movement_class && klass.map && klass.handle == *movement_class &&
                guide.corridor.footprint ==
                    ground_orders::CorridorOrigin{
                        int16_t(klass.record.footprint_x), int16_t(klass.record.footprint_z)
                    }) {
                supported = true;
                break;
            }
        }
        if (!supported)
            return false;
    }
    // Child codecs have all accepted the complete input. The following
    // operations consume those same bytes and cannot allocate or reject them.
    at = header_bytes;
    for (uint32_t index = 0; index < state.catalog_count; ++index) {
        auto& slot = state.catalogs[index];
        const auto* metadata = bytes.data() + at;
        const auto catalog_bytes = slot.catalog->saved_size();
        at += catalog_metadata_bytes;
        const bool restored = slot.catalog->restore(bytes.subspan(at, catalog_bytes));
        assert(restored);
        if (!restored)
            return false;
        at += catalog_bytes;
        // The catalog's saved revision is authoritative. A second saved
        // copy could disagree and permit an epoch to be reused on reset.
        slot.revision = slot.catalog->status().revision;
        slot.sampled = sampled_rectangle(metadata);
        slot.sampled_valid = metadata[32] != 0;
        slot.recording_samples = false;
        for (size_t corridor = 0; corridor < slot.validated_tick.size(); ++corridor) {
            slot.validated_tick[corridor] =
                read32(metadata + validation_ticks_offset + corridor * 4);
            slot.validated[corridor] = (metadata[validation_flags_offset + corridor / 8] &
                                        uint8_t(1u << (corridor % 8))) != 0;
        }
    }
    const bool guides_restored =
        read_advisory_lane_states({state.guides.get(), state.unit_count}, guide_bytes);
    assert(guides_restored);
    if (!guides_restored)
        return false;
    const bool heads_restored = unit_movement::read_local_flow_heads(
        {state.published_guides.get(), state.unit_count}, published_bytes
    );
    assert(heads_restored);
    if (!heads_restored)
        return false;
    for (size_t unit = 0; unit < state.unit_count; ++unit) {
        state.identities[unit] =
            read_identity(bytes.data() + identities_at + unit * identity_bytes);
        state.identities[unit].rebind = state.guides[unit].phase != AdvisoryLanePhase::none;
    }
    state.catalog_cursor = catalog_cursor;
    state.tick = read32(header + 28);
    state.enrollment_begin = read32(header + 32);
    state.enrollment_next = read32(header + 36);
    state.active_cursor = read32(header + 84);
    state.active_begin = read32(header + 88);
    state.active_stopped = header[92] != 0;
    state.budget_override = override_limit;
    state.tick_valid = tick_valid;
    state.construction = read_budget(header + 48);
    state.query = read_budget(header + 56);
    state.active = read_budget(header + 64);
    rebuild_advisory_membership(state);
    return true;
}

} // namespace oa::sim::match_runtime
