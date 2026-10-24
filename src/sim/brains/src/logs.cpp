// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A match's brain log. One tick is collected in a seat buffer and handed
// over whole. The built-in player's lines are the shadow call log's lines.
// A brain's acting calls use that same writer; the three new kinds are
// written here.

#include "internal.hpp"

#include "oa/sim/brains/logs.hpp"
#include "oa/sim/brains/shadow.hpp"

#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

namespace oa::sim::brains {
namespace {

using sim::ai::ComputerHost;
using sim::ai::Squad;
using match_runtime::Match;

constexpr uint32_t line_room = 256;

Brains* brains_of(Match& match) noexcept {
    std::shared_ptr<void>& held = match.seat_state();
    if (!held)
        return nullptr;
    return static_cast<Brains*>(held.get());
}

bool lines_open(const Seat& seat) noexcept {
    if (seat.match == nullptr)
        return false;
    const Brains* brains = brains_of(const_cast<Match&>(*seat.match));
    return brains != nullptr && brains->logs.lines != nullptr;
}

bool log_reserve(Seat& seat, uint32_t extra) noexcept {
    const uint64_t need64 = static_cast<uint64_t>(seat.log_used) + extra + 1u;
    if (need64 > 0xffffffffu)
        return false;
    const auto need = static_cast<uint32_t>(need64);
    if (seat.log_capacity >= need)
        return true;
    uint32_t capacity = seat.log_capacity == 0 ? 256u : seat.log_capacity;
    while (capacity < need) {
        if (capacity > 0x7fffffffu) {
            capacity = need;
            break;
        }
        capacity *= 2u;
    }
    void* fresh = std::realloc(seat.log_text, capacity);
    if (fresh == nullptr)
        return false;
    seat.log_text = static_cast<char*>(fresh);
    seat.log_capacity = capacity;
    return true;
}

void log_append(Seat& seat, const char* text, uint32_t length) noexcept {
    if (!seat.log_open || text == nullptr || length == 0)
        return;
    if (!log_reserve(seat, length))
        return;
    std::memcpy(seat.log_text + seat.log_used, text, length);
    seat.log_used += length;
    seat.log_text[seat.log_used] = '\0';
}

void log_append_written(Seat& seat, const char* storage, int wrote, uint32_t room) noexcept {
    if (wrote <= 0 || static_cast<uint32_t>(wrote) >= room)
        return;
    log_append(seat, storage, static_cast<uint32_t>(wrote));
}

void log_words(
    Seat& seat, const char* kind, const char* name, const int64_t* numbers, uint32_t count
) noexcept {
    if (!seat.log_open || kind == nullptr || name == nullptr)
        return;
    char storage[line_room];
    int wrote = std::snprintf(storage, sizeof storage, "%s %s", kind, name);
    if (wrote < 0 || static_cast<uint32_t>(wrote) >= sizeof storage)
        return;
    for (uint32_t index = 0; index < count; ++index) {
        const int64_t number = numbers != nullptr ? numbers[index] : int64_t{0};
        const int more = std::snprintf(
            storage + wrote,
            sizeof storage - static_cast<std::size_t>(wrote),
            " %lld",
            static_cast<long long>(number)
        );
        if (more < 0 ||
            static_cast<uint32_t>(wrote) + static_cast<uint32_t>(more) >= sizeof storage)
            return;
        wrote += more;
    }
    if (static_cast<uint32_t>(wrote) + 1u >= sizeof storage)
        return;
    storage[wrote] = '\n';
    storage[wrote + 1] = '\0';
    log_append(seat, storage, static_cast<uint32_t>(wrote) + 1u);
}

void log_entry(Seat& seat, const LogEntry& entry) noexcept {
    if (!seat.log_open)
        return;
    char storage[192];
    const int wrote = format_call_log_entry(entry, storage, static_cast<uint32_t>(sizeof storage));
    log_append_written(seat, storage, wrote, static_cast<uint32_t>(sizeof storage));
}

void copy_log_name(char* out, const char* text) noexcept {
    uint32_t index = 0;
    if (text != nullptr) {
        while (index < 15 && text[index] != '\0') {
            out[index] = text[index];
            ++index;
        }
    }
    out[index] = '\0';
}

uint32_t seat_stream(const Seat& seat) noexcept {
    if (seat.match == nullptr)
        return 0;
    return seat.match->random_state();
}

void fill_point(LogEntry& entry, const oa::FixedVec3* at) noexcept {
    if (at == nullptr)
        return;
    entry.x = at->x;
    entry.y = at->y;
    entry.z = at->z;
}

Seat& seat_of_calls(void* context) noexcept {
    return *static_cast<Seat*>(context);
}

Seat& seat_of_host(void* context) noexcept {
    auto* host = static_cast<ComputerHost*>(context);
    return *static_cast<Seat*>(host->context);
}

uint32_t wrap_random(void* context, uint32_t bound) {
    Seat& seat = seat_of_calls(context);
    const uint32_t before = seat_stream(seat);
    uint32_t value = 0;
    if (seat.logged_inner.host.random != nullptr)
        value = seat.logged_inner.host.random(seat.logged_inner.host.context, bound);
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::draw;
        entry.bound = bound;
        entry.value = value;
        entry.state_before = before;
        entry.state_after = seat_stream(seat);
        log_entry(seat, entry);
    }
    return value;
}

uint32_t wrap_squad_size(void* context, uint8_t player, Squad squad) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.squad_size == nullptr)
        return 0;
    return seat.logged_inner.host.squad_size(seat.logged_inner.host.context, player, squad);
}

uint16_t wrap_squad_member(void* context, uint8_t player, Squad squad, uint32_t index) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.squad_member == nullptr)
        return 0;
    return seat.logged_inner.host.squad_member(
        seat.logged_inner.host.context, player, squad, index
    );
}

void wrap_set_squad(void* context, uint16_t unit, Squad squad) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.set_squad != nullptr)
        seat.logged_inner.host.set_squad(seat.logged_inner.host.context, unit, squad);
    if (!seat.log_open)
        return;
    LogEntry entry{};
    entry.kind = LogKind::assign;
    entry.slot = unit;
    entry.extra = static_cast<uint16_t>(static_cast<uint32_t>(squad));
    log_entry(seat, entry);
}

bool wrap_allied(void* context, uint8_t player, uint8_t other) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.allied == nullptr)
        return false;
    return seat.logged_inner.host.allied(seat.logged_inner.host.context, player, other);
}

bool wrap_primary(void* context, uint16_t unit, uint8_t* preserve, uint8_t* queue) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.primary_order == nullptr)
        return false;
    return seat.logged_inner.host.primary_order(
        seat.logged_inner.host.context, unit, preserve, queue
    );
}

bool wrap_secondary(void* context, uint16_t unit) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.secondary_order == nullptr)
        return false;
    return seat.logged_inner.host.secondary_order(seat.logged_inner.host.context, unit);
}

bool wrap_visible(void* context, uint8_t player, uint16_t unit) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.unit_visible == nullptr)
        return false;
    return seat.logged_inner.host.unit_visible(seat.logged_inner.host.context, player, unit);
}

const uint8_t* wrap_strengths(void* context, uint8_t player, uint16_t type) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.strengths == nullptr)
        return nullptr;
    return seat.logged_inner.host.strengths(seat.logged_inner.host.context, player, type);
}

bool wrap_site_clear(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.site_clear == nullptr)
        return false;
    return seat.logged_inner.host.site_clear(seat.logged_inner.host.context, type, cell_x, cell_z);
}

bool wrap_building_site(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.building_site == nullptr)
        return false;
    return seat.logged_inner.host.building_site(
        seat.logged_inner.host.context, type, cell_x, cell_z
    );
}

uint8_t wrap_cell_metal(void* context, int32_t cell_x, int32_t cell_z) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.cell_metal == nullptr)
        return 0;
    return seat.logged_inner.host.cell_metal(seat.logged_inner.host.context, cell_x, cell_z);
}

bool wrap_metal_feature(void* context, int32_t cell_x, int32_t cell_z) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.metal_feature == nullptr)
        return false;
    return seat.logged_inner.host.metal_feature(seat.logged_inner.host.context, cell_x, cell_z);
}

bool wrap_order_move(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    Seat& seat = seat_of_calls(context);
    bool accepted = false;
    if (seat.logged_inner.host.order_move != nullptr)
        accepted =
            seat.logged_inner.host.order_move(seat.logged_inner.host.context, unit, to, queue);
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::move;
        entry.slot = unit;
        entry.flag = queue ? 1 : 0;
        fill_point(entry, to);
        log_entry(seat, entry);
    }
    return accepted;
}

bool wrap_order_patrol(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    Seat& seat = seat_of_calls(context);
    bool accepted = false;
    if (seat.logged_inner.host.order_patrol != nullptr) {
        accepted =
            seat.logged_inner.host.order_patrol(seat.logged_inner.host.context, unit, to, queue);
    }
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::patrol;
        entry.slot = unit;
        entry.flag = queue ? 1 : 0;
        fill_point(entry, to);
        log_entry(seat, entry);
    }
    return accepted;
}

bool wrap_order_attack(void* context, uint16_t unit, uint16_t target) {
    Seat& seat = seat_of_calls(context);
    bool accepted = false;
    if (seat.logged_inner.host.order_attack != nullptr)
        accepted =
            seat.logged_inner.host.order_attack(seat.logged_inner.host.context, unit, target);
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::attack;
        entry.slot = unit;
        entry.extra = target;
        log_entry(seat, entry);
    }
    return accepted;
}

bool wrap_order_build(void* context, uint16_t unit, uint16_t type, const oa::FixedVec3* at) {
    Seat& seat = seat_of_calls(context);
    bool accepted = false;
    if (seat.logged_inner.host.order_build != nullptr) {
        accepted =
            seat.logged_inner.host.order_build(seat.logged_inner.host.context, unit, type, at);
    }
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::build;
        entry.slot = unit;
        entry.extra = type;
        fill_point(entry, at);
        log_entry(seat, entry);
    }
    return accepted;
}

bool wrap_order_factory(void* context, uint16_t factory, uint16_t type, int32_t count) {
    Seat& seat = seat_of_calls(context);
    bool accepted = false;
    if (seat.logged_inner.host.order_factory != nullptr) {
        accepted = seat.logged_inner.host.order_factory(
            seat.logged_inner.host.context, factory, type, count
        );
    }
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::queue_order;
        entry.slot = factory;
        entry.extra = type;
        entry.count = count;
        log_entry(seat, entry);
    }
    return accepted;
}

void wrap_set_active(void* context, uint16_t unit, bool on) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.set_active != nullptr)
        seat.logged_inner.host.set_active(seat.logged_inner.host.context, unit, on);
    if (!seat.log_open)
        return;
    LogEntry entry{};
    entry.kind = LogKind::on;
    entry.slot = unit;
    entry.flag = on ? 1 : 0;
    log_entry(seat, entry);
}

bool wrap_point_visible(void* context, uint8_t player, const oa::FixedVec3* at) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.point_visible == nullptr)
        return false;
    return seat.logged_inner.host.point_visible(seat.logged_inner.host.context, player, at);
}

const sim::detection::Sightings* wrap_sightings(void* context, uint8_t player) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.sightings == nullptr)
        return nullptr;
    return seat.logged_inner.host.sightings(seat.logged_inner.host.context, player);
}

bool wrap_weapon_reaches(void* context, uint16_t unit, const oa::FixedVec3* at) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.weapon_reaches == nullptr)
        return false;
    return seat.logged_inner.host.weapon_reaches(seat.logged_inner.host.context, unit, at);
}

bool wrap_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    Seat& seat = seat_of_calls(context);
    bool accepted = false;
    if (seat.logged_inner.host.order_attack_point != nullptr) {
        accepted =
            seat.logged_inner.host.order_attack_point(seat.logged_inner.host.context, unit, at);
    }
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::attack_point;
        entry.slot = unit;
        fill_point(entry, at);
        log_entry(seat, entry);
    }
    return accepted;
}

void wrap_note_section(void* context, uint8_t player, uint8_t section) {
    Seat& seat = seat_of_calls(context);
    if (seat.logged_inner.host.note_section != nullptr) {
        seat.logged_inner.host.note_section(seat.logged_inner.host.context, player, section);
    }
    if (!seat.log_open)
        return;
    LogEntry entry{};
    entry.kind = LogKind::section;
    copy_log_name(entry.name, shadow_section_name(section));
    log_entry(seat, entry);
}

int32_t wrap_unit_squad(void* context, uint16_t slot) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.unit_squad == nullptr)
        return -1;
    return seat.logged_inner.unit_squad(&seat.logged_inner.host, slot);
}

bool wrap_nearest(
    void* context,
    uint8_t player,
    int32_t x,
    int32_t z,
    bool widened,
    bool skip_submerged,
    uint16_t* out_slot
) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.nearest_enemy == nullptr)
        return false;
    return seat.logged_inner.nearest_enemy(
        &seat.logged_inner.host, player, x, z, widened, skip_submerged, out_slot
    );
}

void stance_from_flags(const oa::Unit& unit, int32_t* move, int32_t* fire) noexcept {
    const uint32_t flags = unit.flags;
    *move = static_cast<int32_t>(
        (flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT
    );
    *fire = static_cast<int32_t>(
        (flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT
    );
}

void wrap_write_stance(void* context, uint16_t slot, int32_t move, int32_t fire) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.write_stance != nullptr)
        seat.logged_inner.write_stance(&seat.logged_inner.host, slot, move, fire);
    if (!seat.log_open)
        return;
    int32_t logged_move = move;
    int32_t logged_fire = fire;
    const oa::World* world = seat.logged_inner.host.world;
    if (world != nullptr) {
        const oa::Unit* unit = oa::world_unit_at(world, slot);
        if (unit != nullptr)
            stance_from_flags(*unit, &logged_move, &logged_fire);
    }
    if (logged_move < 0)
        logged_move = 0;
    if (logged_fire < 0)
        logged_fire = 0;
    LogEntry entry{};
    entry.kind = LogKind::stance;
    entry.slot = slot;
    entry.count = logged_move;
    entry.fire = logged_fire;
    log_entry(seat, entry);
}

void wrap_section(void* context, const char* name) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.section != nullptr)
        seat.logged_inner.section(&seat.logged_inner.host, name);
    if (!seat.log_open)
        return;
    LogEntry entry{};
    entry.kind = LogKind::section;
    copy_log_name(entry.name, name);
    log_entry(seat, entry);
}

uint8_t wrap_primary_kind(void* context, uint16_t slot) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.primary_kind == nullptr)
        return 0;
    return seat.logged_inner.primary_kind(&seat.logged_inner.host, slot);
}

bool wrap_site_facing(
    void* context, uint16_t type, int32_t cell_x, int32_t cell_z, int32_t facing, bool yard
) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.site_facing == nullptr)
        return false;
    return seat.logged_inner.site_facing(
        &seat.logged_inner.host, type, cell_x, cell_z, facing, yard
    );
}

bool wrap_site_fogged(
    void* context,
    uint8_t player,
    uint16_t type,
    int32_t cell_x,
    int32_t cell_z,
    int32_t facing,
    bool yard
) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.site_fogged == nullptr)
        return false;
    return seat.logged_inner.site_fogged(
        &seat.logged_inner.host, player, type, cell_x, cell_z, facing, yard
    );
}

bool wrap_attack_queued(void* context, uint16_t slot, uint16_t target) {
    Seat& seat = seat_of_host(context);
    bool accepted = false;
    if (seat.logged_inner.attack_queued != nullptr)
        accepted = seat.logged_inner.attack_queued(&seat.logged_inner.host, slot, target);
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::attack;
        entry.slot = slot;
        entry.extra = target;
        log_entry(seat, entry);
    }
    return accepted;
}

bool wrap_build_shaped(
    void* context,
    uint16_t slot,
    uint16_t type,
    const oa::FixedVec3* at,
    bool queued,
    int32_t facing
) {
    Seat& seat = seat_of_host(context);
    bool accepted = false;
    if (seat.logged_inner.build_shaped != nullptr) {
        accepted =
            seat.logged_inner.build_shaped(&seat.logged_inner.host, slot, type, at, queued, facing);
    }
    if (seat.log_open) {
        LogEntry entry{};
        entry.kind = LogKind::build;
        entry.slot = slot;
        entry.extra = type;
        fill_point(entry, at);
        log_entry(seat, entry);
    }
    return accepted;
}

bool wrap_weapon_slot(void* context, uint16_t unit, const oa::FixedVec3* at, uint8_t weapon) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.weapon_reaches_slot == nullptr)
        return false;
    return seat.logged_inner.weapon_reaches_slot(&seat.logged_inner.host, unit, at, weapon);
}

const sim::ai::ComputerKnowledge*
wrap_knowledge(void* context, uint8_t player, uint32_t* refreshed_tick) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.knowledge == nullptr)
        return nullptr;
    return seat.logged_inner.knowledge(&seat.logged_inner.host, player, refreshed_tick);
}

uint32_t wrap_builds_held_until(void* context, uint8_t player) {
    Seat& seat = seat_of_host(context);
    if (seat.logged_inner.builds_held_until == nullptr)
        return 0;
    return seat.logged_inner.builds_held_until(&seat.logged_inner.host, player);
}

void arm_logged_calls(Seat& seat) noexcept {
    seat.logged_inner = seat.calls;
    ComputerHost& host = seat.calls.host;
    const ComputerHost& inner = seat.logged_inner.host;
    host.context = &seat;
    if (inner.random != nullptr)
        host.random = wrap_random;
    if (inner.squad_size != nullptr)
        host.squad_size = wrap_squad_size;
    if (inner.squad_member != nullptr)
        host.squad_member = wrap_squad_member;
    if (inner.set_squad != nullptr)
        host.set_squad = wrap_set_squad;
    if (inner.allied != nullptr)
        host.allied = wrap_allied;
    if (inner.primary_order != nullptr)
        host.primary_order = wrap_primary;
    if (inner.secondary_order != nullptr)
        host.secondary_order = wrap_secondary;
    if (inner.unit_visible != nullptr)
        host.unit_visible = wrap_visible;
    if (inner.strengths != nullptr)
        host.strengths = wrap_strengths;
    if (inner.site_clear != nullptr)
        host.site_clear = wrap_site_clear;
    if (inner.building_site != nullptr)
        host.building_site = wrap_building_site;
    if (inner.cell_metal != nullptr)
        host.cell_metal = wrap_cell_metal;
    if (inner.metal_feature != nullptr)
        host.metal_feature = wrap_metal_feature;
    if (inner.order_move != nullptr)
        host.order_move = wrap_order_move;
    if (inner.order_patrol != nullptr)
        host.order_patrol = wrap_order_patrol;
    if (inner.order_attack != nullptr)
        host.order_attack = wrap_order_attack;
    if (inner.order_build != nullptr)
        host.order_build = wrap_order_build;
    if (inner.order_factory != nullptr)
        host.order_factory = wrap_order_factory;
    if (inner.set_active != nullptr)
        host.set_active = wrap_set_active;
    if (inner.point_visible != nullptr)
        host.point_visible = wrap_point_visible;
    if (inner.sightings != nullptr)
        host.sightings = wrap_sightings;
    if (inner.weapon_reaches != nullptr)
        host.weapon_reaches = wrap_weapon_reaches;
    if (inner.order_attack_point != nullptr)
        host.order_attack_point = wrap_attack_point;
    if (inner.note_section != nullptr)
        host.note_section = wrap_note_section;
    if (seat.logged_inner.unit_squad != nullptr)
        seat.calls.unit_squad = wrap_unit_squad;
    if (seat.logged_inner.nearest_enemy != nullptr)
        seat.calls.nearest_enemy = wrap_nearest;
    if (seat.logged_inner.write_stance != nullptr)
        seat.calls.write_stance = wrap_write_stance;
    if (seat.logged_inner.section != nullptr)
        seat.calls.section = wrap_section;
    if (seat.logged_inner.primary_kind != nullptr)
        seat.calls.primary_kind = wrap_primary_kind;
    if (seat.logged_inner.site_facing != nullptr)
        seat.calls.site_facing = wrap_site_facing;
    if (seat.logged_inner.site_fogged != nullptr)
        seat.calls.site_fogged = wrap_site_fogged;
    if (seat.logged_inner.attack_queued != nullptr)
        seat.calls.attack_queued = wrap_attack_queued;
    if (seat.logged_inner.build_shaped != nullptr)
        seat.calls.build_shaped = wrap_build_shaped;
    if (seat.logged_inner.weapon_reaches_slot != nullptr)
        seat.calls.weapon_reaches_slot = wrap_weapon_slot;
    // These read the match through the host's context, which the log now
    // holds, so they are passed the inner host as the others are.
    if (seat.logged_inner.knowledge != nullptr)
        seat.calls.knowledge = wrap_knowledge;
    if (seat.logged_inner.builds_held_until != nullptr)
        seat.calls.builds_held_until = wrap_builds_held_until;
}

uint32_t logged_match_stream(void* context) noexcept {
    return static_cast<Match*>(context)->random_state();
}

bool logged_builtin_turn(void*, Match& match, uint8_t player, const ComputerHost& host) {
    if (player >= OA_PLAYER_COUNT)
        return false;
    const Brains* brains = brains_of(match);
    if (brains == nullptr || brains->logs.lines == nullptr)
        return false;
    CallLog log;
    LoggingHost session;
    logging_arm(session, host, log, player, logged_match_stream, &match);
    sim::ai::computer_players_adopt(sim::ai::match_computer_players(match), session.host, player);
    sim::ai::computer_player_tick_orders(
        sim::ai::match_computer_players(match), session.host, player
    );
    logging_note_stances(session);
    char* text = render_call_log(log, match.state().game.tick, player, false);
    if (text != nullptr) {
        brains->logs.lines(brains->logs.context, std::string_view(text));
        delete[] text;
    }
    return true;
}

void hand_text(const LogHooks& hooks, char* text) noexcept {
    if (text == nullptr || hooks.lines == nullptr)
        return;
    hooks.lines(hooks.context, std::string_view(text));
    delete[] text;
}

} // namespace

LogHooks seat_log_hooks(const Seat& seat) noexcept {
    if (seat.match == nullptr)
        return {};
    const Brains* brains = brains_of(const_cast<Match&>(*seat.match));
    if (brains == nullptr)
        return {};
    return brains->logs;
}

void log_arm_seat(Seat& seat) noexcept {
    if (seat.shadow || seat.calls_logged || !lines_open(seat))
        return;
    if (seat.calls.host.world == nullptr && seat.calls.host.random == nullptr)
        return;
    arm_logged_calls(seat);
    seat.calls_logged = true;
}

void log_begin_turn(Seat& seat, uint32_t tick, bool brain) noexcept {
    seat.log_used = 0;
    seat.log_open = false;
    if (!lines_open(seat))
        return;
    char storage[128];
    const int wrote = format_tick_heading(
        tick, seat.player, brain, storage, static_cast<uint32_t>(sizeof storage)
    );
    seat.log_open = true;
    log_append_written(seat, storage, wrote, static_cast<uint32_t>(sizeof storage));
}

void log_end_turn(Seat& seat) noexcept {
    if (!seat.log_open) {
        seat.log_used = 0;
        return;
    }
    seat.log_open = false;
    if (seat.log_text != nullptr && seat.log_used > 0) {
        const LogHooks hooks = seat_log_hooks(seat);
        if (hooks.lines != nullptr)
            hooks.lines(hooks.context, std::string_view(seat.log_text, seat.log_used));
    }
    seat.log_used = 0;
}

void log_call(Seat& seat, const char* name, const int64_t* numbers, uint32_t count) noexcept {
    log_words(seat, "call", name, numbers, count);
}

void log_own_integer(Seat& seat, int64_t low, int64_t high, int64_t value) noexcept {
    if (!seat.log_open)
        return;
    char storage[128];
    const int wrote = std::snprintf(
        storage,
        sizeof storage,
        "own-draw %lld %lld %lld\n",
        static_cast<long long>(low),
        static_cast<long long>(high),
        static_cast<long long>(value)
    );
    log_append_written(seat, storage, wrote, static_cast<uint32_t>(sizeof storage));
}

void log_own_float(Seat& seat, uint64_t bits) noexcept {
    if (!seat.log_open)
        return;
    char storage[64];
    const int wrote = std::snprintf(
        storage, sizeof storage, "own-draw float %016llx\n", static_cast<unsigned long long>(bits)
    );
    log_append_written(seat, storage, wrote, static_cast<uint32_t>(sizeof storage));
}

void log_order(Seat& seat, const char* name, const int64_t* numbers, uint32_t count) noexcept {
    log_words(seat, "order", name, numbers, count);
}

void log_standing_order(Seat& seat, uint16_t slot, bool fire_field, int32_t value) noexcept {
    if (!seat.log_open)
        return;
    int32_t move = 0;
    int32_t fire = 0;
    if (seat.match != nullptr) {
        const oa::Unit* unit = oa::world_unit_at(&seat.match->state(), slot);
        if (unit != nullptr)
            stance_from_flags(*unit, &move, &fire);
    }
    if (fire_field)
        fire = value;
    else
        move = value;
    if (move < 0)
        move = 0;
    if (fire < 0)
        fire = 0;
    LogEntry entry{};
    entry.kind = LogKind::stance;
    entry.slot = slot;
    entry.count = move;
    entry.fire = fire;
    log_entry(seat, entry);
}

void publish_logged_shadow_turn(
    Seat& seat, const CallLog& builtin, const CallLog& brain, uint32_t tick
) noexcept {
    const LogHooks hooks = seat_log_hooks(seat);
    if (hooks.lines == nullptr)
        return;
    hand_text(hooks, render_call_log(builtin, tick, seat.player, false));
    hand_text(hooks, render_call_log(brain, tick, seat.player, true));
}

void set_log_hooks(Match& match, LogHooks hooks) noexcept {
    std::shared_ptr<void>& held = match.seat_state();
    if (!held) {
        auto* created = new (std::nothrow) Brains;
        if (created == nullptr)
            return;
        try {
            held.reset(created, [](void* pointer) { delete static_cast<Brains*>(pointer); });
        } catch (...) {
            // A reset that throws has already passed `created` to the deleter.
            return;
        }
    }
    auto* brains = static_cast<Brains*>(held.get());
    brains->logs = hooks;
    if (hooks.lines != nullptr && !brains->log_banner_sent) {
        hooks.lines(hooks.context, "brain log 1\n");
        brains->log_banner_sent = true;
    }
    sim::ai::set_logged_computer_turn(&logged_builtin_turn, nullptr);
    if (hooks.lines == nullptr)
        return;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        Seat* seat = brains->seats[player].get();
        if (seat != nullptr)
            log_arm_seat(*seat);
    }
}

} // namespace oa::sim::brains
