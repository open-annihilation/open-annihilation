// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/records.hpp"

#include <cstdint>

namespace oa::netgame {
namespace {

template <class R>
constexpr bool layout_matches_table() {
    return detail::visited_length<R>() == record_length_table[static_cast<uint8_t>(R::type)];
}

static_assert(layout_matches_table<PingRecord>());
static_assert(layout_matches_table<Unused03Record>());
static_assert(layout_matches_table<ChatRecord>());
static_assert(layout_matches_table<ProbeRecord>());
static_assert(layout_matches_table<ProbeReplyRecord>());
static_assert(layout_matches_table<GameStartRecord>());
static_assert(layout_matches_table<UnitCreatedRecord>());
static_assert(layout_matches_table<UnitLinkRecord>());
static_assert(layout_matches_table<UnitDamageRecord>());
static_assert(layout_matches_table<UnitKilledRecord>());
static_assert(layout_matches_table<WeaponFireRecord>());
static_assert(layout_matches_table<ProjectileInterceptedRecord>());
static_assert(layout_matches_table<FeatureEventRecord>());
static_assert(layout_matches_table<CobStartRecord>());
static_assert(layout_matches_table<UnitStateFlagsRecord>());
static_assert(layout_matches_table<BuilderLinkRecord>());
static_assert(layout_matches_table<SoundRecord>());
static_assert(layout_matches_table<UnitTransferRecord>());
static_assert(layout_matches_table<LoadedRecord>());
static_assert(layout_matches_table<ResourceGiveRecord>());
static_assert(layout_matches_table<PlayerValueRequestRecord>());
static_assert(layout_matches_table<PlayerValueReplyRecord>());
static_assert(layout_matches_table<PauseSpeedRecord>());
static_assert(layout_matches_table<UnitDefHandshakeRecord>());
static_assert(layout_matches_table<RejectRecord>());
static_assert(layout_matches_table<DisconnectNoticeRecord>());
static_assert(layout_matches_table<ResendRequestRecord>());
static_assert(layout_matches_table<StartPositionRecord>());
static_assert(layout_matches_table<StartPositionAckRecord>());
static_assert(layout_matches_table<PlayerInfoRecord>());
static_assert(layout_matches_table<MachineGroupRequestRecord>());
static_assert(layout_matches_table<MachineGroupReplyRecord>());
static_assert(layout_matches_table<AllianceRecord>());
static_assert(layout_matches_table<PlayerTeamRecord>());
static_assert(layout_matches_table<Unused25Record>());
static_assert(layout_matches_table<SlotTableRecord>());
static_assert(layout_matches_table<IntegrityNoticeRecord>());
static_assert(layout_matches_table<EconomyRecord>());
static_assert(layout_matches_table<EconomyReplyRecord>());
static_assert(layout_matches_table<LoadProgressRecord>());

static_assert(detail::visited_length<WideWeaponFireRecord>() == wide_record_length_table[0]);
static_assert(
    detail::visited_length<WideProjectileInterceptedRecord>() == wide_record_length_table[1]
);
static_assert(detail::visited_length<WideFeatureEventRecord>() == wide_record_length_table[2]);

/// Calls f with the union member that matches a record type.
///
/// @param any Record whose union member is passed.
/// @param type Record type selecting the member.
/// @param f Callable taking the member.
/// @return False for types without a record (0x00, 0x01, 0x04, 0x2b, >= 0x2d), else true.
template <class Record, class F>
bool with_member(Record& any, RecordType type, F&& f) {
    switch (type) {
    case RecordType::ping:
        f(any.ping);
        return true;
    case RecordType::unused_03:
        f(any.unused_03);
        return true;
    case RecordType::chat:
        f(any.chat);
        return true;
    case RecordType::probe:
        f(any.probe);
        return true;
    case RecordType::probe_reply:
        f(any.probe_reply);
        return true;
    case RecordType::game_start:
        f(any.game_start);
        return true;
    case RecordType::unit_created:
        f(any.unit_created);
        return true;
    case RecordType::unit_link:
        f(any.unit_link);
        return true;
    case RecordType::unit_damage:
        f(any.unit_damage);
        return true;
    case RecordType::unit_killed:
        f(any.unit_killed);
        return true;
    case RecordType::weapon_fire:
        f(any.weapon_fire);
        return true;
    case RecordType::projectile_intercepted:
        f(any.projectile_intercepted);
        return true;
    case RecordType::feature_event:
        f(any.feature_event);
        return true;
    case RecordType::cob_start:
        f(any.cob_start);
        return true;
    case RecordType::unit_state_flags:
        f(any.unit_state_flags);
        return true;
    case RecordType::builder_link:
        f(any.builder_link);
        return true;
    case RecordType::sound:
        f(any.sound);
        return true;
    case RecordType::unit_transfer:
        f(any.unit_transfer);
        return true;
    case RecordType::loaded:
        f(any.loaded);
        return true;
    case RecordType::resource_give:
        f(any.resource_give);
        return true;
    case RecordType::player_value_request:
        f(any.player_value_request);
        return true;
    case RecordType::player_value_reply:
        f(any.player_value_reply);
        return true;
    case RecordType::pause_speed:
        f(any.pause_speed);
        return true;
    case RecordType::unit_def_handshake:
        f(any.unit_def_handshake);
        return true;
    case RecordType::reject:
        f(any.reject);
        return true;
    case RecordType::disconnect_notice:
        f(any.disconnect_notice);
        return true;
    case RecordType::resend_request:
        f(any.resend_request);
        return true;
    case RecordType::start_position:
        f(any.start_position);
        return true;
    case RecordType::start_position_ack:
        f(any.start_position_ack);
        return true;
    case RecordType::player_info:
        f(any.player_info);
        return true;
    case RecordType::machine_group_request:
        f(any.machine_group_request);
        return true;
    case RecordType::machine_group_reply:
        f(any.machine_group_reply);
        return true;
    case RecordType::alliance:
        f(any.alliance);
        return true;
    case RecordType::player_team:
        f(any.player_team);
        return true;
    case RecordType::unused_25:
        f(any.unused_25);
        return true;
    case RecordType::slot_table:
        f(any.slot_table);
        return true;
    case RecordType::integrity_notice:
        f(any.integrity_notice);
        return true;
    case RecordType::economy:
        f(any.economy);
        return true;
    case RecordType::economy_reply:
        f(any.economy_reply);
        return true;
    case RecordType::load_progress:
        f(any.load_progress);
        return true;
    case RecordType::unit_state:
        return false;
    }
    return false;
}

} // namespace

const char* wire_error_name(WireError error) noexcept {
    switch (error) {
    case WireError::ok:
        return "ok";
    case WireError::truncated:
        return "truncated";
    case WireError::invalid_type:
        return "invalid record type";
    case WireError::zero_length_record:
        return "zero-length record";
    case WireError::length_mismatch:
        return "length mismatch";
    case WireError::buffer_too_small:
        return "buffer too small";
    case WireError::overflow:
        return "bounded storage exhausted";
    case WireError::unsupported_delta_layout:
        return "unsupported unit delta layout";
    case WireError::bad_argument:
        return "bad argument";
    case WireError::unknown_peer:
        return "unknown peer";
    }
    return "unknown";
}

WireError
record_wire_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept {
    if (bytes == nullptr || length == nullptr)
        return WireError::bad_argument;
    if (available == 0)
        return WireError::truncated;
    const auto type = bytes[0];
    if (!is_record_type(type))
        return WireError::invalid_type;
    if (type == static_cast<uint8_t>(RecordType::unit_state)) {
        if (available < unit_state_prefix_bytes)
            return WireError::truncated;
        *length = load_u16(bytes + 1);
    } else {
        *length = record_length_table[type];
    }
    // A zero length would never advance past the record.
    return *length == 0 ? WireError::zero_length_record : WireError::ok;
}

WireError
wide_record_wire_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept {
    if (bytes == nullptr || length == nullptr)
        return WireError::bad_argument;
    if (available == 0)
        return WireError::truncated;
    const auto type = bytes[0];
    if (!is_wide_record_type(type))
        return WireError::invalid_type;
    *length = wide_record_length_table[static_cast<std::size_t>(type - first_wide_record_type)];
    return available < *length ? WireError::truncated : WireError::ok;
}

WireError
decode_unit_state_record(const uint8_t* bytes, std::size_t size, UnitStateRecord* out) noexcept {
    if (bytes == nullptr || out == nullptr)
        return WireError::bad_argument;
    if (size < unit_state_header_bytes)
        return WireError::truncated;
    if (bytes[0] != static_cast<uint8_t>(RecordType::unit_state))
        return WireError::invalid_type;
    const auto length = load_u16(bytes + 1);
    if (length < unit_state_header_bytes || length > size)
        return WireError::length_mismatch;
    out->length = length;
    out->sender_tick = load_u32(bytes + 3);
    out->body = bytes + unit_state_header_bytes;
    out->body_size = static_cast<uint16_t>(length - unit_state_header_bytes);
    return WireError::ok;
}

WireError decode_any_record(const uint8_t* bytes, std::size_t size, AnyRecord* out) noexcept {
    if (bytes == nullptr || out == nullptr)
        return WireError::bad_argument;
    if (size == 0)
        return WireError::truncated;
    if (!is_record_type(bytes[0]))
        return WireError::invalid_type;
    const auto type = static_cast<RecordType>(bytes[0]);
    if (type == RecordType::unit_state) {
        UnitStateRecord header{};
        const auto error = decode_unit_state_record(bytes, size, &header);
        if (error != WireError::ok)
            return error;
        if (header.length != size)
            return WireError::length_mismatch;
        out->type = type;
        out->unit_state = header;
        return WireError::ok;
    }
    auto error = WireError::zero_length_record;
    AnyRecord decoded;
    decoded.type = type;
    with_member(decoded, type, [&](auto& member) { error = decode_record(bytes, size, &member); });
    if (error == WireError::ok)
        *out = decoded;
    return error;
}

WireError encode_any_record(
    const AnyRecord& in, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept {
    if (out == nullptr)
        return WireError::bad_argument;
    if (in.type == RecordType::unit_state) {
        const auto& header = in.unit_state;
        const std::size_t total = unit_state_header_bytes + std::size_t{header.body_size};
        if (total > 0xffff || (header.body_size != 0 && header.body == nullptr))
            return WireError::bad_argument;
        if (capacity < total)
            return WireError::buffer_too_small;
        out[0] = static_cast<uint8_t>(RecordType::unit_state);
        store_u16(out + 1, static_cast<uint16_t>(total));
        store_u32(out + 3, header.sender_tick);
        if (header.body_size != 0)
            std::memcpy(out + unit_state_header_bytes, header.body, header.body_size);
        if (written)
            *written = total;
        return WireError::ok;
    }
    auto error = WireError::invalid_type;
    with_member(in, in.type, [&](const auto& member) {
        error = encode_record(member, out, capacity, written);
    });
    return error;
}

} // namespace oa::netgame
