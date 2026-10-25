// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/replication.hpp"

#include <cstdint>

namespace oa::netgame {
namespace {

constexpr uint16_t no_unit_index = 0xffff;   // (s16)-1 ends the delta list
constexpr uint8_t carrier_none_piece = 0xff; // piece byte of a detach link

bool unit_live(const Unit* unit) noexcept {
    return unit != nullptr && (unit->flags & OA_UNIT_FLAG_LIVE) != 0;
}

MovementRecord* movement_of(World* world, const ReplicationSim* sim, Unit* unit) noexcept {
    if (unit->movement == 0 || sim == nullptr || sim->movement == nullptr)
        return nullptr;
    return sim->movement(sim->context, world, unit);
}

void toggle_state_flags(
    World* world, const ReplicationSim* sim, Unit* unit, uint8_t mask, bool on
) noexcept {
    if (sim != nullptr && sim->toggle_state_flags != nullptr) {
        sim->toggle_state_flags(sim->context, world, unit, mask, on);
        return;
    }
    unit->state_flags =
        static_cast<uint8_t>(on ? unit->state_flags | mask : unit->state_flags & ~mask);
}

// The 0x09 record the receiver synthesises when a unit's def index changed.
void create_from_stream(
    World* world, const ReplicationSim* sim, Unit* unit, uint16_t def_index
) noexcept {
    if (sim == nullptr || sim->create_unit == nullptr)
        return;
    UnitCreatedRecord record;
    record.unit_def_index = def_index;
    record.unit_index = unit->id;
    record.position[0] = unit->position.x;
    record.position[1] = unit->position.y;
    record.position[2] = unit->position.z;
    record.bank_heading =
        static_cast<uint16_t>(unit->bank) | (static_cast<uint32_t>(unit->heading) << 16);
    record.pitch = static_cast<uint16_t>(unit->pitch);
    sim->create_unit(sim->context, world, unit->owner_index, record);
}

void link_unit(World* world, const ReplicationSim* sim, const UnitLinkRecord& record) noexcept {
    if (sim != nullptr && sim->link_unit != nullptr)
        sim->link_unit(sim->context, world, record);
}

// Delta layout of a receiver's unit: its driver when it has one, else the
// driver the create handler would install for the def.
MovementClass
delta_layout(World* world, const MovementRecord* movement, uint16_t def_index) noexcept {
    if (movement != nullptr && movement->driver != MovementClass::none)
        return movement->driver;
    if (world->unit_defs == nullptr || def_index >= world->unit_def_count)
        return MovementClass::none;
    return movement_class_for_def(world->unit_defs[def_index]);
}

/// Tells whether a unit of a def has a movement object once it is created:
/// only a mobile def's units have one.
///
/// @param world the world whose unit table holds the def
/// @param def_index the def's index
/// @return true for a mobile def in the table
bool def_has_movement(const World* world, uint16_t def_index) noexcept {
    return world->unit_defs != nullptr && def_index < world->unit_def_count &&
           world->unit_defs[def_index].bm_code == unit_def_bm_code_mobile;
}

Player* player_by_id(World* world, uint32_t net_id) noexcept {
    if (net_id == 0xffffffffu)
        return nullptr;
    for (auto& player : world->game.players)
        if (player.in_use != 0 && player.player_id == net_id)
            return &player;
    return nullptr;
}

} // namespace

bool ground_driver_has_delta(const MovementRecord& m) noexcept {
    return (m.ground.flags & ground_driver_resend) != 0 ||
           ((m.flags ^ m.ground.flags) & movement_blocked) != 0;
}

void ground_driver_take_delta(MovementRecord* m, WaypointDelta* out) noexcept {
    int32_t count = 0;
    if (m->ground.flags & ground_driver_path_set)
        count =
            m->ground.path_count > 2 ? 3 : (m->ground.path_count < 0 ? 0 : m->ground.path_count);
    WaypointDelta delta{};
    delta.flag = (m->flags & movement_blocked) != 0;
    delta.count = static_cast<uint8_t>(count);
    for (int32_t i = 0; i < count; ++i) {
        delta.points[i][0] = m->ground.path[i][0];
        delta.points[i][1] = m->ground.path[i][1];
    }
    m->ground.flags = static_cast<uint8_t>(
        ((m->ground.flags & ~movement_blocked) | (m->flags & movement_blocked)) &
        ~ground_driver_resend
    );
    *out = delta;
}

bool air_driver_has_delta(const MovementRecord& m) noexcept {
    return (m.air.flags & air_driver_resend) != 0;
}

void air_driver_take_delta(MovementRecord* m, AirDelta* out) noexcept {
    AirDelta delta{};
    switch (m->air.goal_kind) {
    case air_goal_kind_none:
        delta.goal_tag = air_goal_tag_none;
        break;
    case air_goal_kind_target:
        delta.goal_tag = air_goal_tag_target;
        delta.target = m->air.target;
        break;
    case air_goal_kind_seek:
        delta.goal_tag = air_goal_tag_seek;
        delta.seek = m->air.seek;
        break;
    default:
        delta.goal_tag_omitted = true;
        break;
    }
    delta.substate = m->flags & movement_substate_mask;
    m->air.flags = static_cast<uint8_t>(
        (m->air.flags & ~(air_driver_resend | air_driver_sent_substate_mask)) |
        ((m->flags & movement_substate_mask) << air_driver_sent_substate_shift)
    );
    *out = delta;
}

void air_driver_note_substate(MovementRecord* m) noexcept {
    const auto sent =
        (m->air.flags & air_driver_sent_substate_mask) >> air_driver_sent_substate_shift;
    if ((m->flags & movement_substate_mask) != sent)
        m->air.flags |= air_driver_resend;
}

FullUnitRecord pack_full_unit_record(World* world, const ReplicationSim* sim, Unit* unit) noexcept {
    FullUnitRecord r{};
    r.unit_def_index = unit->type_index;
    if (r.unit_def_index == 0)
        return r;
    r.health = unit->health;
    r.build_byte = unit_state_build_byte(unit->build_remaining);
    r.state_flags = unit->state_flags;
    r.occupancy = static_cast<uint8_t>(unit->flags & OA_UNIT_FLAG_OCCUPANCY_MASK);
    r.attached = unit->attach_parent != 0;
    if (!r.attached) {
        r.position[0] = unit->position.x;
        r.position[1] = unit->position.y;
        r.position[2] = unit->position.z;
        r.heading = unit->heading;
        r.pitch = static_cast<uint16_t>(unit->pitch);
        r.bank = static_cast<uint16_t>(unit->bank);
        r.has_object_word = unit->movement != 0;
        if (const auto* m = movement_of(world, sim, unit))
            r.object_word = m->speed;
        return r;
    }
    const Unit* carrier = world_unit(world, unit->attach_parent);
    r.attached_unit_index = carrier != nullptr ? carrier->id : 0;
    r.attach_piece = static_cast<int8_t>(unit->attach_piece);
    return r;
}

WireError replication_pack_player(
    World* world,
    const ReplicationSim* sim,
    uint8_t player_index,
    BitWriter* writer,
    uint16_t* length
) noexcept {
    Player* player = world_player(world, player_index);
    if (player == nullptr || writer == nullptr)
        return WireError::bad_argument;
    const auto def_bits = static_cast<unsigned>(world->game.unit_def_id_bits);
    const auto tick = world->game.tick;
    unit_state_begin(writer, tick);
    player->last_sim_tick = static_cast<int32_t>(tick);
    uint32_t count = 0;
    Unit* first = world_player_units(world, player, &count);
    for (uint32_t i = 0; i < count; ++i) {
        Unit* unit = &first[i];
        if (!unit_live(unit) || unit->movement == 0)
            continue;
        MovementRecord* m = movement_of(world, sim, unit);
        if (m == nullptr)
            continue;
        UnitDelta delta{};
        delta.movement = m->driver;
        if (m->driver == MovementClass::ground && ground_driver_has_delta(*m))
            ground_driver_take_delta(m, &delta.ground);
        else if (m->driver == MovementClass::air && air_driver_has_delta(*m))
            air_driver_take_delta(m, &delta.air);
        else
            continue;
        const auto index = static_cast<uint16_t>(unit->id - player->base_unit_id);
        unit_state_write_entry_header(writer, index, unit->type_index, def_bits);
        write_unit_delta(writer, delta);
        if (unit_state_full(writer))
            break;
    }
    FullUnitRecord full{};
    const auto slot = unit_state_full_record_slot(tick, world->game.units_per_player);
    if (first != nullptr && slot >= 0 && static_cast<uint32_t>(slot) < count)
        full = pack_full_unit_record(world, sim, &first[slot]);
    return unit_state_finish(writer, full, def_bits, length);
}

void movement_apply_delta(
    World* world, const ReplicationSim* sim, Unit* unit, MovementRecord* m, const UnitDelta& delta
) noexcept {
    if (m == nullptr)
        return;
    if (delta.movement == MovementClass::ground) {
        m->flags = static_cast<uint8_t>(
            (m->flags & ~movement_blocked) | (delta.ground.flag ? movement_blocked : 0)
        );
        m->ground.path_count = delta.ground.count;
        for (uint8_t i = 0; i < delta.ground.count && i < ground_delta_max_points; ++i) {
            m->ground.path[i][0] = delta.ground.points[i][0];
            m->ground.path[i][1] = delta.ground.points[i][1];
        }
        if (sim != nullptr && sim->apply_ground_delta != nullptr)
            sim->apply_ground_delta(sim->context, world, unit, delta.ground);
        return;
    }
    if (delta.movement != MovementClass::air)
        return;
    // The previous goal object is destroyed; tags other than 1 and 2 leave none.
    m->air.goal_kind = air_goal_kind_none;
    m->air.target = AirTargetGoal{};
    m->air.seek = AirSeekGoal{};
    if (delta.air.goal_tag == air_goal_tag_target) {
        m->air.goal_kind = air_goal_kind_target;
        m->air.target = delta.air.target;
    } else if (delta.air.goal_tag == air_goal_tag_seek) {
        m->air.goal_kind = air_goal_kind_seek;
        m->air.seek = delta.air.seek;
    }
    if (sim != nullptr && sim->set_air_goal != nullptr)
        sim->set_air_goal(sim->context, world, unit, delta.air);
    const auto substate = static_cast<uint8_t>(delta.air.substate & movement_substate_mask);
    if (sim != nullptr && sim->set_movement_substate != nullptr)
        sim->set_movement_substate(sim->context, world, unit, substate);
    else
        m->flags = static_cast<uint8_t>((m->flags & ~movement_substate_mask) | substate);
}

WireError replication_apply_full_record(
    World* world, const ReplicationSim* sim, Unit* unit, BitReader* reader
) noexcept {
    const auto def_bits = static_cast<unsigned>(world->game.unit_def_id_bits);
    if (def_bits < 1 || def_bits > unit_state_max_def_index_bits)
        return WireError::bad_argument;
    // The record is read whole before any of it applies, so a truncated one
    // leaves the unit as it was. Only the speed word waits: whether it is
    // there depends on the movement the unit has once recreated.
    BitReader record_reader = *reader;
    FullUnitRecord record{};
    if (read_full_unit_record(&record_reader, def_bits, false, &record) != WireError::ok)
        return WireError::truncated;
    if (sim != nullptr && sim->full_record_read != nullptr)
        sim->full_record_read(sim->context, world, unit, record);
    if (record.unit_def_index == 0) {
        if (unit->type_index != 0)
            unit->flags |= OA_UNIT_FLAG_DEATH_PENDING;
        *reader = record_reader;
        return WireError::ok;
    }
    if (unit->type_index != record.unit_def_index) {
        // A recreated unit's speed word, there when its def has movement, is
        // checked to be in the record before the unit is recreated, so a
        // record cut short leaves the slot's unit as it was.
        if (!record.attached && def_has_movement(world, record.unit_def_index)) {
            BitReader ahead = record_reader;
            (void)bit_reader_read(&ahead, 32);
            if (bit_reader_overrun(&ahead))
                return WireError::truncated;
        }
        create_from_stream(world, sim, unit, record.unit_def_index);
    }
    const bool has_speed_word = !record.attached && unit->movement != 0;
    uint32_t speed_word = 0;
    if (has_speed_word) {
        speed_word = bit_reader_read(&record_reader, 32);
        if (bit_reader_overrun(&record_reader))
            return WireError::truncated;
    }
    *reader = record_reader;
    if (sim != nullptr && sim->clear_linked_object_word != nullptr)
        sim->clear_linked_object_word(sim->context, world, unit);
    unit->health = record.health;
    if (unit_state_build_fraction_differs(record.build_byte, unit->build_remaining)) {
        unit->build_remaining = unit_state_build_fraction(record.build_byte);
        unit->flags |= OA_UNIT_FLAG_CONSTRUCTION_DIRTY;
    }
    toggle_state_flags(world, sim, unit, record.state_flags, true);
    toggle_state_flags(world, sim, unit, static_cast<uint8_t>(~record.state_flags), false);
    if (record.attached) {
        UnitLinkRecord link;
        link.unit_index = unit->id;
        link.linked_unit_index = record.attached_unit_index;
        link.attach_piece = record.attach_piece;
        link.occupancy = record.occupancy;
        link_unit(world, sim, link);
        return WireError::ok;
    }
    if (unit->attach_parent != 0) {
        UnitLinkRecord detach;
        detach.unit_index = unit->id;
        detach.linked_unit_index = 0;
        detach.attach_piece = static_cast<int8_t>(carrier_none_piece);
        detach.occupancy = record.occupancy;
        link_unit(world, sim, detach);
    }
    const FixedVec3 to{record.position[0], record.position[1], record.position[2]};
    if (sim != nullptr && sim->place_unit != nullptr)
        sim->place_unit(sim->context, world, unit, to, record.occupancy);
    else
        unit->position = to;
    unit->bank = static_cast<int16_t>(record.bank);
    unit->heading = record.heading;
    unit->pitch = static_cast<int16_t>(record.pitch);
    if (has_speed_word) {
        if (MovementRecord* m = movement_of(world, sim, unit))
            m->speed = speed_word;
        if (sim != nullptr && sim->set_movement_speed != nullptr)
            sim->set_movement_speed(sim->context, world, unit, speed_word);
    }
    return WireError::ok;
}

WireError replication_apply_unit_state(
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size
) noexcept {
    Player* sender = world != nullptr ? world_player(world, sender_index) : nullptr;
    if (sender == nullptr || bytes == nullptr)
        return WireError::bad_argument;
    const auto def_bits = static_cast<unsigned>(world->game.unit_def_id_bits);
    if (def_bits < 1 || def_bits > unit_state_max_def_index_bits)
        return WireError::bad_argument;
    if (size < unit_state_header_bytes)
        return WireError::truncated;
    if (bytes[0] != static_cast<uint8_t>(RecordType::unit_state))
        return WireError::invalid_type;
    const auto length = load_u16(bytes + 1);
    if (length < unit_state_header_bytes || length > size)
        return WireError::length_mismatch;

    BitReader reader;
    bit_reader_init(&reader, bytes, length);
    (void)bit_reader_read(&reader, 8);
    (void)bit_reader_read(&reader, 16);
    const auto tick = bit_reader_read(&reader, 32);
    sender->last_sim_tick = static_cast<int32_t>(tick);
    uint32_t count = 0;
    Unit* first = world_player_units(world, sender, &count);
    if (first == nullptr)
        return WireError::ok;

    auto index = static_cast<uint16_t>(bit_reader_read(&reader, unit_state_unit_index_bits));
    while (index != no_unit_index) {
        if (bit_reader_overrun(&reader))
            return WireError::truncated;
        const auto slot = static_cast<int16_t>(index);
        if (slot < 0 || static_cast<uint32_t>(slot) >= count)
            return WireError::bad_argument;
        Unit* unit = &first[slot];
        const auto def_index = static_cast<uint16_t>(bit_reader_read(&reader, def_bits));
        if (bit_reader_overrun(&reader))
            return WireError::truncated;
        if (unit->type_index != def_index) {
            // The delta is read to its end, in the layout the recreated
            // unit's driver takes, before the unit is recreated, so a body
            // cut short leaves the slot's unit as it was.
            BitReader ahead = reader;
            UnitDelta read_ahead{};
            const auto error =
                read_unit_delta(&ahead, delta_layout(world, nullptr, def_index), &read_ahead);
            if (error != WireError::ok)
                return error;
            create_from_stream(world, sim, unit, def_index);
        }
        MovementRecord* m = movement_of(world, sim, unit);
        const auto layout = delta_layout(world, m, def_index);
        if (layout == MovementClass::none)
            return WireError::unsupported_delta_layout;
        UnitDelta delta{};
        const auto error = read_unit_delta(&reader, layout, &delta);
        if (error != WireError::ok)
            return error;
        movement_apply_delta(world, sim, unit, m, delta);
        index = static_cast<uint16_t>(bit_reader_read(&reader, unit_state_unit_index_bits));
    }
    if (bit_reader_overrun(&reader))
        return WireError::truncated;
    for (uint32_t i = 0; i < count; ++i) {
        Unit* unit = &first[i];
        if (!unit_live(unit) || unit->movement == 0 || sim == nullptr)
            continue;
        if (sim->movement_tick != nullptr)
            sim->movement_tick(sim->context, world, unit);
        if (sim->sight_update != nullptr)
            sim->sight_update(sim->context, world, unit);
    }
    if (!bit_reader_read_bit(&reader))
        return bit_reader_overrun(&reader) ? WireError::truncated : WireError::ok;
    const auto slot = unit_state_full_record_slot(tick, world->game.units_per_player);
    if (slot < 0 || static_cast<uint32_t>(slot) >= count)
        return WireError::bad_argument;
    return replication_apply_full_record(world, sim, &first[slot], &reader);
}

namespace {

/// Applies one wide feature record. Actions other than a hit, a queue or a burn are not handled.
bool apply_wide_feature(
    World* world, const ReplicationSim* sim, const WideFeatureEventRecord& record
) noexcept {
    const auto action = record.action;
    if (action == static_cast<uint8_t>(WideFeatureAction::tree_burn)) {
        if (sim != nullptr && sim->tree_burn != nullptr)
            sim->tree_burn(sim->context, world, record.x, record.y);
        return true;
    }
    if (action == static_cast<uint8_t>(WideFeatureAction::queue_event) ||
        action == static_cast<uint8_t>(WideFeatureAction::queue_event_flagged)) {
        if (sim != nullptr && sim->queue_feature_event != nullptr)
            sim->queue_feature_event(
                sim->context,
                world,
                record.x,
                record.y,
                action == static_cast<uint8_t>(WideFeatureAction::queue_event_flagged)
            );
        return true;
    }
    if (action == static_cast<uint8_t>(WideFeatureAction::weapon_hit)) {
        if (sim != nullptr && sim->feature_event != nullptr)
            sim->feature_event(sim->context, world, record.weapon_id, record.x, record.y);
        return true;
    }
    return false;
}

} // namespace

WireError replication_apply_record(
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size,
    bool* handled
) noexcept {
    if (handled != nullptr)
        *handled = false;
    if (world == nullptr || bytes == nullptr || size == 0)
        return WireError::bad_argument;
    if (is_wide_record_type(bytes[0])) {
        void* ctx = sim != nullptr ? sim->context : nullptr;
        bool known = false;
        if (bytes[0] == static_cast<uint8_t>(WideRecordType::weapon_fire)) {
            WideWeaponFireRecord record{};
            const auto error = decode_record(bytes, size, &record);
            if (error != WireError::ok)
                return error;
            known = true;
            if (sim != nullptr && sim->weapon_fire != nullptr)
                sim->weapon_fire(ctx, world, world_player(world, sender_index), record);
        } else if (bytes[0] == static_cast<uint8_t>(WideRecordType::projectile_intercepted)) {
            WideProjectileInterceptedRecord record{};
            const auto error = decode_record(bytes, size, &record);
            if (error != WireError::ok)
                return error;
            known = true;
            if (sim != nullptr && sim->detonate_projectile != nullptr)
                sim->detonate_projectile(ctx, world, record);
        } else {
            WideFeatureEventRecord record{};
            const auto error = decode_record(bytes, size, &record);
            if (error != WireError::ok)
                return error;
            known = apply_wide_feature(world, sim, record);
        }
        if (handled != nullptr)
            *handled = known;
        return WireError::ok;
    }
    if (bytes[0] == static_cast<uint8_t>(RecordType::unit_state)) {
        if (handled != nullptr)
            *handled = true;
        return replication_apply_unit_state(world, sim, sender_index, bytes, size);
    }
    Player* sender = world_player(world, sender_index);
    AnyRecord any;
    const auto error = decode_any_record(bytes, size, &any);
    if (error != WireError::ok)
        return error;
    void* ctx = sim != nullptr ? sim->context : nullptr;
    const auto hook = [sim](auto member) { return sim != nullptr && sim->*member != nullptr; };
    bool known = true;
    switch (any.type) {
    case RecordType::unit_created:
        if (hook(&ReplicationSim::create_unit))
            sim->create_unit(ctx, world, sender_index, any.unit_created);
        break;
    case RecordType::unit_link:
        link_unit(world, sim, any.unit_link);
        break;
    case RecordType::unit_damage:
        if (hook(&ReplicationSim::apply_damage))
            sim->apply_damage(ctx, world, any.unit_damage);
        break;
    case RecordType::unit_killed:
        if (hook(&ReplicationSim::apply_kill))
            sim->apply_kill(ctx, world, any.unit_killed);
        break;
    case RecordType::weapon_fire:
        if (hook(&ReplicationSim::weapon_fire))
            sim->weapon_fire(ctx, world, sender, widen(any.weapon_fire));
        break;
    case RecordType::projectile_intercepted:
        if (hook(&ReplicationSim::detonate_projectile))
            sim->detonate_projectile(ctx, world, widen(any.projectile_intercepted));
        break;
    case RecordType::feature_event:
        (void)apply_wide_feature(world, sim, widen(any.feature_event));
        break;
    case RecordType::cob_start: {
        // Unit index 0 names no unit: the record starts no script.
        Unit* unit = any.cob_start.unit_index != 0 ? world_unit_at(world, any.cob_start.unit_index)
                                                   : nullptr;
        if (unit_live(unit) && hook(&ReplicationSim::cob_start))
            sim->cob_start(ctx, world, unit, any.cob_start);
        break;
    }
    case RecordType::unit_state_flags: {
        const auto& r = any.unit_state_flags;
        Unit* unit = r.unit_index != 0 ? world_unit_at(world, r.unit_index) : nullptr;
        if (unit_live(unit)) {
            toggle_state_flags(world, sim, unit, r.state_mask, true);
            toggle_state_flags(world, sim, unit, static_cast<uint8_t>(~r.state_mask), false);
        }
        break;
    }
    case RecordType::builder_link: {
        const auto& r = any.builder_link;
        Unit* subject =
            r.subject_unit_index != 0 ? world_unit_at(world, r.subject_unit_index) : nullptr;
        Unit* source =
            r.source_unit_index != 0 ? world_unit_at(world, r.source_unit_index) : nullptr;
        if (hook(&ReplicationSim::link_builder))
            sim->link_builder(ctx, world, source, subject);
        break;
    }
    case RecordType::sound:
        if (hook(&ReplicationSim::play_sound))
            sim->play_sound(ctx, world, any.sound);
        break;
    case RecordType::unit_transfer: {
        const auto& r = any.unit_transfer;
        Unit* unit = r.unit_index != 0 ? world_unit_at(world, r.unit_index) : nullptr;
        Player* owner = player_by_id(world, r.new_owner_id);
        if (unit_live(unit) && owner != nullptr &&
            (owner->status == OA_PLAYER_STATUS_LOCAL ||
             owner->status == OA_PLAYER_STATUS_COMPUTER) &&
            hook(&ReplicationSim::transfer_unit))
            sim->transfer_unit(ctx, world, unit, owner, r);
        break;
    }
    default:
        known = false;
        break;
    }
    if (handled != nullptr)
        *handled = known;
    return WireError::ok;
}

} // namespace oa::netgame
