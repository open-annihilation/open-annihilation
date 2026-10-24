// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A brain beside the built-in player. The logging host records the built-in
// turn and forwards it. The recording calls record the brain and do not
// apply its orders. The comparison is of those two call logs.
#include "oa/sim/brains/shadow.hpp"

#include "internal.hpp"

#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <new>

namespace oa::sim::brains {

using sim::ai::ComputerHost;
using sim::ai::Squad;
using match_runtime::Match;

constexpr uint32_t squad_count = 10;
constexpr uint32_t marker_limit = 32;
constexpr uint32_t name_limit = 16;

struct SquadList {
    uint16_t* members{};
    uint32_t count{};
    uint32_t capacity{};
};

struct UnitView {
    bool present{};
    int32_t squad{-1};
    bool has_primary{};
    uint8_t preserve{};
    uint8_t command{};
    uint8_t kind{};
    bool has_secondary{};
    bool live_head{};
};

struct RecordDetail {
    SquadList base_squads[squad_count]{};
    SquadList squads[squad_count]{};
    UnitView* base_units{};
    UnitView* units{};
    uint32_t unit_count{};
    char section_name[name_limit]{};
    uint32_t draw_index{};

    ~RecordDetail() {
        for (uint32_t index = 0; index < squad_count; ++index) {
            delete[] base_squads[index].members;
            delete[] squads[index].members;
        }
        delete[] base_units;
        delete[] units;
    }
};

struct TickText {
    uint32_t tick{};
    char* builtin{};
    char* brain{};
};

struct SetupNote {
    uint32_t bound{};
    uint32_t value{};
};

struct ShadowBody {
    Recording recording{};
    LoggingHost logging{};
    CallLog builtin_log{};
    CallLog brain_log{};
    CallLog start_brain{};
    SetupNote* setup_draws{};
    uint32_t setup_count{};
    uint32_t setup_capacity{};
    uint32_t mask{shadow_mask_all};
    bool started{};
    bool in_start{};
    bool setup_tick_known{};
    uint32_t setup_tick{};
    uint32_t ticks{};
    uint32_t differ_count{};
    uint32_t first_tick{};
    char section[name_limit]{};
    bool stopped{};
    uint32_t stopped_tick{};
    char message[256]{};
    bool stop_noted{};
    bool frozen{};
    TickText ring[3]{};
    uint8_t ring_count{};
    TickText frozen_ticks[3]{};
    uint8_t frozen_count{};
    char* start_builtin_text{};
    char* start_brain_text{};
    ShadowReport report{};

    ~ShadowBody() {
        delete[] setup_draws;
        delete[] start_builtin_text;
        delete[] start_brain_text;
        for (uint8_t index = 0; index < ring_count; ++index) {
            delete[] ring[index].builtin;
            delete[] ring[index].brain;
        }
        for (uint8_t index = 0; index < frozen_count; ++index) {
            delete[] frozen_ticks[index].builtin;
            delete[] frozen_ticks[index].brain;
        }
    }
};

namespace {

Recording& recording_of(void* context) noexcept {
    return *static_cast<Recording*>(context);
}

Recording& recording_from_host(void* context) noexcept {
    return *static_cast<Recording*>(static_cast<ComputerHost*>(context)->context);
}

LoggingHost& logging_of(void* context) noexcept {
    return *static_cast<LoggingHost*>(context);
}

void copy_name(char* out, const char* text) noexcept {
    uint32_t index = 0;
    if (text != nullptr) {
        while (text[index] != '\0' && index + 1 < name_limit) {
            out[index] = text[index];
            ++index;
        }
    }
    out[index] = '\0';
}

bool same_name(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr)
        return left == right;
    return std::strcmp(left, right) == 0;
}

bool grow_entries(CallLog& log) noexcept {
    const uint32_t next = log.capacity == 0 ? 8u : log.capacity * 2u;
    if (next < log.capacity)
        return false;
    auto* items = new (std::nothrow) LogEntry[next];
    if (items == nullptr)
        return false;
    for (uint32_t index = 0; index < log.count; ++index)
        items[index] = log.items[index];
    delete[] log.items;
    log.items = items;
    log.capacity = next;
    return true;
}

bool list_push(SquadList& list, uint16_t slot) noexcept {
    if (list.count == list.capacity) {
        const uint32_t next = list.capacity == 0 ? 8u : list.capacity * 2u;
        if (next < list.capacity)
            return false;
        auto* members = new (std::nothrow) uint16_t[next];
        if (members == nullptr)
            return false;
        for (uint32_t index = 0; index < list.count; ++index)
            members[index] = list.members[index];
        delete[] list.members;
        list.members = members;
        list.capacity = next;
    }
    list.members[list.count] = slot;
    ++list.count;
    return true;
}

void list_clear(SquadList& list) noexcept {
    list.count = 0;
}

void list_remove(SquadList& list, uint16_t slot) noexcept {
    for (uint32_t index = 0; index < list.count; ++index) {
        if (list.members[index] != slot)
            continue;
        list.members[index] = list.members[list.count - 1];
        --list.count;
        return;
    }
}

void list_assign(SquadList& destination, const SquadList& source) noexcept {
    list_clear(destination);
    for (uint32_t index = 0; index < source.count; ++index)
        (void)list_push(destination, source.members[index]);
}

bool ensure_units(RecordDetail& detail, uint32_t count) noexcept {
    if (count <= detail.unit_count)
        return true;
    uint32_t next = detail.unit_count == 0 ? 8u : detail.unit_count;
    while (next < count) {
        if (next > 0x7fffffffu)
            return false;
        next *= 2u;
    }
    auto* fresh = new (std::nothrow) UnitView[next]{};
    auto* base = new (std::nothrow) UnitView[next]{};
    if (fresh == nullptr || base == nullptr) {
        delete[] fresh;
        delete[] base;
        return false;
    }
    for (uint32_t index = 0; index < detail.unit_count; ++index) {
        fresh[index] = detail.units[index];
        base[index] = detail.base_units[index];
    }
    delete[] detail.units;
    delete[] detail.base_units;
    detail.units = fresh;
    detail.base_units = base;
    detail.unit_count = next;
    return true;
}

UnitView* view_at(RecordDetail& detail, uint16_t slot) noexcept {
    if (!ensure_units(detail, static_cast<uint32_t>(slot) + 1u))
        return nullptr;
    return &detail.units[slot];
}

void read_head(const SeatCalls& live, UnitView& view, uint16_t slot) noexcept {
    uint8_t preserve = 0;
    uint8_t command = 0;
    if (live.host.primary_order != nullptr &&
        live.host.primary_order(live.host.context, slot, &preserve, &command)) {
        view.has_primary = true;
        view.preserve = preserve;
        view.command = command;
    }
    if (live.primary_kind != nullptr)
        view.kind = live.primary_kind(const_cast<ComputerHost*>(&live.host), slot);
    if (live.host.secondary_order != nullptr)
        view.has_secondary = live.host.secondary_order(live.host.context, slot);
}

void snapshot_detail(RecordDetail& detail, const SeatCalls& live, uint8_t player) noexcept {
    for (uint32_t squad = 0; squad < squad_count; ++squad) {
        list_clear(detail.squads[squad]);
        list_clear(detail.base_squads[squad]);
    }
    if (detail.units != nullptr) {
        for (uint32_t index = 0; index < detail.unit_count; ++index)
            detail.units[index] = {};
    }
    oa::World* world = live.host.world;
    if (world != nullptr && player < OA_PLAYER_COUNT) {
        uint32_t span = 0;
        oa::Unit* first = oa::world_player_units(world, &world->game.players[player], &span);
        if (first != nullptr && world->units != nullptr) {
            const auto begin = static_cast<uint32_t>(first - world->units);
            for (uint32_t step = 0; step < span; ++step) {
                const auto slot = static_cast<uint16_t>(begin + step);
                UnitView* view = view_at(detail, slot);
                if (view == nullptr)
                    continue;
                view->present = true;
                if (live.unit_squad != nullptr)
                    view->squad = live.unit_squad(const_cast<ComputerHost*>(&live.host), slot);
                read_head(live, *view, slot);
            }
        }
    }
    if (live.host.squad_size != nullptr && live.host.squad_member != nullptr) {
        for (uint32_t squad = 0; squad < squad_count; ++squad) {
            const auto squad_id = static_cast<Squad>(squad);
            const uint32_t size = live.host.squad_size(live.host.context, player, squad_id);
            for (uint32_t index = 0; index < size; ++index) {
                const uint16_t slot =
                    live.host.squad_member(live.host.context, player, squad_id, index);
                if (!list_push(detail.squads[squad], slot))
                    break;
                UnitView* view = view_at(detail, slot);
                if (view == nullptr)
                    continue;
                view->present = true;
                view->squad = static_cast<int32_t>(squad);
                if (!view->has_primary && !view->has_secondary)
                    read_head(live, *view, slot);
            }
        }
    }
    for (uint32_t squad = 0; squad < squad_count; ++squad)
        list_assign(detail.base_squads[squad], detail.squads[squad]);
    if (detail.base_units != nullptr && detail.units != nullptr) {
        for (uint32_t index = 0; index < detail.unit_count; ++index)
            detail.base_units[index] = detail.units[index];
    }
}

void reset_view(RecordDetail& detail) noexcept {
    for (uint32_t squad = 0; squad < squad_count; ++squad)
        list_assign(detail.squads[squad], detail.base_squads[squad]);
    if (detail.units != nullptr && detail.base_units != nullptr) {
        for (uint32_t index = 0; index < detail.unit_count; ++index)
            detail.units[index] = detail.base_units[index];
    }
}

void view_assign(RecordDetail& detail, uint16_t slot, int32_t squad) noexcept {
    UnitView* view = view_at(detail, slot);
    if (view == nullptr)
        return;
    view->present = true;
    const int32_t old = view->squad;
    if (old >= 0 && static_cast<uint32_t>(old) < squad_count)
        list_remove(detail.squads[static_cast<uint32_t>(old)], slot);
    if (squad >= 0 && static_cast<uint32_t>(squad) < squad_count) {
        (void)list_push(detail.squads[static_cast<uint32_t>(squad)], slot);
        view->squad = squad;
        return;
    }
    view->squad = -1;
}

bool order_kind(LogKind kind) noexcept {
    return kind == LogKind::move || kind == LogKind::patrol || kind == LogKind::attack ||
           kind == LogKind::build || kind == LogKind::queue_order || kind == LogKind::on ||
           kind == LogKind::attack_point;
}

void mark_live(RecordDetail& detail, uint16_t slot) noexcept {
    UnitView* view = view_at(detail, slot);
    if (view != nullptr)
        view->live_head = true;
}

bool head_is_live(const RecordDetail& detail, uint16_t slot) noexcept {
    return slot < detail.unit_count && detail.units != nullptr && detail.units[slot].live_head;
}

void apply_before(RecordDetail& detail, const CallLog* builtin, uint32_t marker) noexcept {
    reset_view(detail);
    if (builtin == nullptr || builtin->items == nullptr)
        return;
    const uint32_t end = marker < builtin->count ? marker : builtin->count;
    for (uint32_t index = 0; index < end; ++index) {
        const LogEntry& entry = builtin->items[index];
        if (entry.kind == LogKind::assign)
            view_assign(detail, entry.slot, static_cast<int32_t>(entry.extra));
        else if (order_kind(entry.kind))
            mark_live(detail, entry.slot);
    }
}

int find_section(const CallLog* log, const char* name) noexcept {
    if (log == nullptr || log->items == nullptr || name == nullptr)
        return -1;
    for (uint32_t index = 0; index < log->count; ++index) {
        if (log->items[index].kind == LogKind::section && same_name(log->items[index].name, name))
            return static_cast<int>(index);
    }
    return -1;
}

const LogEntry* nth_draw(const CallLog& log, uint32_t marker, uint32_t nth) noexcept {
    uint32_t seen = 0;
    for (uint32_t index = marker + 1; index < log.count; ++index) {
        const LogEntry& entry = log.items[index];
        if (entry.kind == LogKind::section)
            return nullptr;
        if (entry.kind != LogKind::draw)
            continue;
        if (seen == nth)
            return &entry;
        ++seen;
    }
    return nullptr;
}

void push_entry(CallLog* log, const LogEntry& entry) noexcept {
    if (log != nullptr)
        (void)log_push(*log, entry);
}

uint32_t stream_now(const LoggingHost& session) noexcept {
    if (session.stream_state == nullptr)
        return 0;
    return session.stream_state(session.stream_context);
}

void log_draw(
    LoggingHost& session, uint32_t bound, uint32_t value, uint32_t before, uint32_t after
) {
    LogEntry entry{};
    entry.kind = LogKind::draw;
    entry.bound = bound;
    entry.value = value;
    entry.state_before = before;
    entry.state_after = after;
    push_entry(session.log, entry);
}

uint32_t log_random(void* context, uint32_t bound) {
    LoggingHost& session = logging_of(context);
    const uint32_t before = stream_now(session);
    uint32_t value = 0;
    if (session.inner.random != nullptr)
        value = session.inner.random(session.inner.context, bound);
    log_draw(session, bound, value, before, stream_now(session));
    return value;
}

uint32_t log_squad_size(void* context, uint8_t player, Squad squad) {
    LoggingHost& session = logging_of(context);
    if (session.inner.squad_size == nullptr)
        return 0;
    return session.inner.squad_size(session.inner.context, player, squad);
}

uint16_t log_squad_member(void* context, uint8_t player, Squad squad, uint32_t index) {
    LoggingHost& session = logging_of(context);
    if (session.inner.squad_member == nullptr)
        return 0;
    return session.inner.squad_member(session.inner.context, player, squad, index);
}

void log_set_squad(void* context, uint16_t unit, Squad squad) {
    LoggingHost& session = logging_of(context);
    if (session.inner.set_squad != nullptr)
        session.inner.set_squad(session.inner.context, unit, squad);
    LogEntry entry{};
    entry.kind = LogKind::assign;
    entry.slot = unit;
    entry.extra = static_cast<uint16_t>(static_cast<uint32_t>(squad));
    push_entry(session.log, entry);
}

bool log_allied(void* context, uint8_t player, uint8_t other) {
    LoggingHost& session = logging_of(context);
    if (session.inner.allied == nullptr)
        return false;
    return session.inner.allied(session.inner.context, player, other);
}

bool log_primary(void* context, uint16_t unit, uint8_t* preserve, uint8_t* queue) {
    LoggingHost& session = logging_of(context);
    if (session.inner.primary_order == nullptr)
        return false;
    return session.inner.primary_order(session.inner.context, unit, preserve, queue);
}

bool log_secondary(void* context, uint16_t unit) {
    LoggingHost& session = logging_of(context);
    if (session.inner.secondary_order == nullptr)
        return false;
    return session.inner.secondary_order(session.inner.context, unit);
}

bool log_visible(void* context, uint8_t player, uint16_t unit) {
    LoggingHost& session = logging_of(context);
    if (session.inner.unit_visible == nullptr)
        return false;
    return session.inner.unit_visible(session.inner.context, player, unit);
}

const uint8_t* log_strengths(void* context, uint8_t player, uint16_t type) {
    LoggingHost& session = logging_of(context);
    if (session.inner.strengths == nullptr)
        return nullptr;
    return session.inner.strengths(session.inner.context, player, type);
}

bool log_site_clear(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    LoggingHost& session = logging_of(context);
    if (session.inner.site_clear == nullptr)
        return false;
    return session.inner.site_clear(session.inner.context, type, cell_x, cell_z);
}

bool log_building_site(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    LoggingHost& session = logging_of(context);
    if (session.inner.building_site == nullptr)
        return false;
    return session.inner.building_site(session.inner.context, type, cell_x, cell_z);
}

uint8_t log_cell_metal(void* context, int32_t cell_x, int32_t cell_z) {
    LoggingHost& session = logging_of(context);
    if (session.inner.cell_metal == nullptr)
        return 0;
    return session.inner.cell_metal(session.inner.context, cell_x, cell_z);
}

bool log_metal_feature(void* context, int32_t cell_x, int32_t cell_z) {
    LoggingHost& session = logging_of(context);
    if (session.inner.metal_feature == nullptr)
        return false;
    return session.inner.metal_feature(session.inner.context, cell_x, cell_z);
}

void fill_point(LogEntry& entry, const oa::FixedVec3* at) noexcept {
    if (at == nullptr)
        return;
    entry.x = at->x;
    entry.y = at->y;
    entry.z = at->z;
}

bool log_order_move(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    LoggingHost& session = logging_of(context);
    bool accepted = false;
    if (session.inner.order_move != nullptr)
        accepted = session.inner.order_move(session.inner.context, unit, to, queue);
    LogEntry entry{};
    entry.kind = LogKind::move;
    entry.slot = unit;
    entry.flag = queue ? 1 : 0;
    fill_point(entry, to);
    push_entry(session.log, entry);
    return accepted;
}

bool log_order_patrol(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    LoggingHost& session = logging_of(context);
    bool accepted = false;
    if (session.inner.order_patrol != nullptr)
        accepted = session.inner.order_patrol(session.inner.context, unit, to, queue);
    LogEntry entry{};
    entry.kind = LogKind::patrol;
    entry.slot = unit;
    entry.flag = queue ? 1 : 0;
    fill_point(entry, to);
    push_entry(session.log, entry);
    return accepted;
}

bool log_order_attack(void* context, uint16_t unit, uint16_t target) {
    LoggingHost& session = logging_of(context);
    bool accepted = false;
    if (session.inner.order_attack != nullptr)
        accepted = session.inner.order_attack(session.inner.context, unit, target);
    LogEntry entry{};
    entry.kind = LogKind::attack;
    entry.slot = unit;
    entry.extra = target;
    push_entry(session.log, entry);
    return accepted;
}

bool log_order_build(void* context, uint16_t unit, uint16_t type, const oa::FixedVec3* at) {
    LoggingHost& session = logging_of(context);
    bool accepted = false;
    if (session.inner.order_build != nullptr)
        accepted = session.inner.order_build(session.inner.context, unit, type, at);
    LogEntry entry{};
    entry.kind = LogKind::build;
    entry.slot = unit;
    entry.extra = type;
    fill_point(entry, at);
    push_entry(session.log, entry);
    return accepted;
}

bool log_order_factory(void* context, uint16_t factory, uint16_t type, int32_t count) {
    LoggingHost& session = logging_of(context);
    bool accepted = false;
    if (session.inner.order_factory != nullptr)
        accepted = session.inner.order_factory(session.inner.context, factory, type, count);
    LogEntry entry{};
    entry.kind = LogKind::queue_order;
    entry.slot = factory;
    entry.extra = type;
    entry.count = count;
    push_entry(session.log, entry);
    return accepted;
}

void log_set_active(void* context, uint16_t unit, bool on) {
    LoggingHost& session = logging_of(context);
    if (session.inner.set_active != nullptr)
        session.inner.set_active(session.inner.context, unit, on);
    LogEntry entry{};
    entry.kind = LogKind::on;
    entry.slot = unit;
    entry.flag = on ? 1 : 0;
    push_entry(session.log, entry);
}

bool log_point_visible(void* context, uint8_t player, const oa::FixedVec3* at) {
    LoggingHost& session = logging_of(context);
    if (session.inner.point_visible == nullptr)
        return false;
    return session.inner.point_visible(session.inner.context, player, at);
}

const sim::detection::Sightings* log_sightings(void* context, uint8_t player) {
    LoggingHost& session = logging_of(context);
    if (session.inner.sightings == nullptr)
        return nullptr;
    return session.inner.sightings(session.inner.context, player);
}

bool log_weapon_reaches(void* context, uint16_t unit, const oa::FixedVec3* at) {
    LoggingHost& session = logging_of(context);
    if (session.inner.weapon_reaches == nullptr)
        return false;
    return session.inner.weapon_reaches(session.inner.context, unit, at);
}

bool log_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    LoggingHost& session = logging_of(context);
    bool accepted = false;
    if (session.inner.order_attack_point != nullptr)
        accepted = session.inner.order_attack_point(session.inner.context, unit, at);
    LogEntry entry{};
    entry.kind = LogKind::attack_point;
    entry.slot = unit;
    fill_point(entry, at);
    push_entry(session.log, entry);
    return accepted;
}

void log_note_section(void* context, uint8_t, uint8_t section) {
    LoggingHost& session = logging_of(context);
    if (section == sim::ai::section_sort)
        session.saw_sort = true;
    LogEntry entry{};
    entry.kind = LogKind::section;
    copy_name(entry.name, shadow_section_name(section));
    entry.state_before = stream_now(session);
    push_entry(session.log, entry);
}

void arm_logging_host(LoggingHost& session) noexcept {
    ComputerHost& host = session.host;
    host.context = &session;
    host.world = session.inner.world;
    host.difficulty = session.inner.difficulty;
    host.map_cells_x = session.inner.map_cells_x;
    host.map_cells_z = session.inner.map_cells_z;
    host.surface_metal = session.inner.surface_metal;
    host.random = log_random;
    host.squad_size = log_squad_size;
    host.squad_member = log_squad_member;
    host.set_squad = log_set_squad;
    host.allied = log_allied;
    host.primary_order = log_primary;
    host.secondary_order = log_secondary;
    host.unit_visible = log_visible;
    host.strengths = log_strengths;
    host.site_clear = log_site_clear;
    host.building_site = log_building_site;
    host.cell_metal = log_cell_metal;
    host.metal_feature = log_metal_feature;
    host.order_move = log_order_move;
    host.order_patrol = log_order_patrol;
    host.order_attack = log_order_attack;
    host.order_build = log_order_build;
    host.order_factory = log_order_factory;
    host.set_active = log_set_active;
    host.point_visible = log_point_visible;
    host.sightings = log_sightings;
    host.weapon_reaches = log_weapon_reaches;
    host.order_attack_point = log_attack_point;
    host.note_section = log_note_section;
}

uint32_t rec_random(void* context, uint32_t bound) {
    Recording& recorded = recording_of(context);
    RecordDetail* detail = recorded.detail;
    if (detail != nullptr && recorded.builtin != nullptr) {
        const int marker = find_section(recorded.builtin, detail->section_name);
        if (marker >= 0) {
            const LogEntry* draw =
                nth_draw(*recorded.builtin, static_cast<uint32_t>(marker), detail->draw_index);
            if (draw != nullptr) {
                uint32_t previous =
                    recorded.builtin->items[static_cast<uint32_t>(marker)].state_before;
                if (detail->draw_index > 0) {
                    const LogEntry* earlier = nth_draw(
                        *recorded.builtin, static_cast<uint32_t>(marker), detail->draw_index - 1
                    );
                    if (earlier != nullptr)
                        previous = earlier->state_after;
                }
                if (draw->state_before != previous) {
                    recorded.copy.restore(draw->state_before);
                    LogEntry resync{};
                    resync.kind = LogKind::resync;
                    resync.state_before = draw->state_before;
                    push_entry(recorded.brain, resync);
                }
            }
        }
    }
    const uint32_t before = recorded.copy.state();
    const uint32_t value = recorded.copy.bounded(bound);
    LogEntry entry{};
    entry.kind = LogKind::draw;
    entry.bound = bound;
    entry.value = value;
    entry.state_before = before;
    entry.state_after = recorded.copy.state();
    push_entry(recorded.brain, entry);
    if (detail != nullptr)
        ++detail->draw_index;
    return value;
}

uint32_t rec_squad_size(void* context, uint8_t, Squad squad) {
    Recording& recorded = recording_of(context);
    const auto index = static_cast<uint32_t>(squad);
    if (recorded.detail == nullptr || index >= squad_count)
        return 0;
    return recorded.detail->squads[index].count;
}

uint16_t rec_squad_member(void* context, uint8_t, Squad squad, uint32_t index) {
    Recording& recorded = recording_of(context);
    const auto squad_index = static_cast<uint32_t>(squad);
    if (recorded.detail == nullptr || squad_index >= squad_count)
        return 0;
    const SquadList& list = recorded.detail->squads[squad_index];
    if (index >= list.count || list.members == nullptr)
        return 0;
    return list.members[index];
}

void rec_set_squad(void* context, uint16_t unit, Squad squad) {
    Recording& recorded = recording_of(context);
    if (recorded.detail != nullptr)
        view_assign(*recorded.detail, unit, static_cast<int32_t>(static_cast<uint32_t>(squad)));
    LogEntry entry{};
    entry.kind = LogKind::assign;
    entry.slot = unit;
    entry.extra = static_cast<uint16_t>(static_cast<uint32_t>(squad));
    push_entry(recorded.brain, entry);
}

bool rec_allied(void* context, uint8_t player, uint8_t other) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.allied == nullptr)
        return false;
    return recorded.live.host.allied(recorded.live.host.context, player, other);
}

bool rec_primary(void* context, uint16_t unit, uint8_t* preserve, uint8_t* queue) {
    Recording& recorded = recording_of(context);
    if (recorded.detail != nullptr && head_is_live(*recorded.detail, unit)) {
        if (recorded.live.host.primary_order == nullptr)
            return false;
        return recorded.live.host.primary_order(recorded.live.host.context, unit, preserve, queue);
    }
    if (recorded.detail == nullptr || unit >= recorded.detail->unit_count ||
        recorded.detail->units == nullptr || !recorded.detail->units[unit].has_primary)
        return false;
    const UnitView& view = recorded.detail->units[unit];
    if (preserve != nullptr)
        *preserve = view.preserve;
    if (queue != nullptr)
        *queue = view.command;
    return true;
}

bool rec_secondary(void* context, uint16_t unit) {
    Recording& recorded = recording_of(context);
    if (recorded.detail != nullptr && head_is_live(*recorded.detail, unit)) {
        if (recorded.live.host.secondary_order == nullptr)
            return false;
        return recorded.live.host.secondary_order(recorded.live.host.context, unit);
    }
    if (recorded.detail == nullptr || unit >= recorded.detail->unit_count ||
        recorded.detail->units == nullptr)
        return false;
    return recorded.detail->units[unit].has_secondary;
}

bool rec_visible(void* context, uint8_t player, uint16_t unit) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.unit_visible == nullptr)
        return false;
    return recorded.live.host.unit_visible(recorded.live.host.context, player, unit);
}

const uint8_t* rec_strengths(void* context, uint8_t player, uint16_t type) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.strengths == nullptr)
        return nullptr;
    return recorded.live.host.strengths(recorded.live.host.context, player, type);
}

bool rec_site_clear(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.site_clear == nullptr)
        return false;
    return recorded.live.host.site_clear(recorded.live.host.context, type, cell_x, cell_z);
}

bool rec_building_site(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.building_site == nullptr)
        return false;
    return recorded.live.host.building_site(recorded.live.host.context, type, cell_x, cell_z);
}

uint8_t rec_cell_metal(void* context, int32_t cell_x, int32_t cell_z) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.cell_metal == nullptr)
        return 0;
    return recorded.live.host.cell_metal(recorded.live.host.context, cell_x, cell_z);
}

bool rec_metal_feature(void* context, int32_t cell_x, int32_t cell_z) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.metal_feature == nullptr)
        return false;
    return recorded.live.host.metal_feature(recorded.live.host.context, cell_x, cell_z);
}

void remember_order(Recording& recorded, uint16_t unit) noexcept {
    if (recorded.detail != nullptr)
        mark_live(*recorded.detail, unit);
}

bool rec_order_move(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::move;
    entry.slot = unit;
    entry.flag = queue ? 1 : 0;
    fill_point(entry, to);
    push_entry(recorded.brain, entry);
    remember_order(recorded, unit);
    return true;
}

bool rec_order_patrol(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::patrol;
    entry.slot = unit;
    entry.flag = queue ? 1 : 0;
    fill_point(entry, to);
    push_entry(recorded.brain, entry);
    remember_order(recorded, unit);
    return true;
}

bool rec_order_attack(void* context, uint16_t unit, uint16_t target) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::attack;
    entry.slot = unit;
    entry.extra = target;
    push_entry(recorded.brain, entry);
    remember_order(recorded, unit);
    return true;
}

bool rec_order_build(void* context, uint16_t unit, uint16_t type, const oa::FixedVec3* at) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::build;
    entry.slot = unit;
    entry.extra = type;
    fill_point(entry, at);
    push_entry(recorded.brain, entry);
    remember_order(recorded, unit);
    return true;
}

bool rec_order_factory(void* context, uint16_t factory, uint16_t type, int32_t count) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::queue_order;
    entry.slot = factory;
    entry.extra = type;
    entry.count = count;
    push_entry(recorded.brain, entry);
    remember_order(recorded, factory);
    return true;
}

void rec_set_active(void* context, uint16_t unit, bool on) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::on;
    entry.slot = unit;
    entry.flag = on ? 1 : 0;
    push_entry(recorded.brain, entry);
    remember_order(recorded, unit);
}

bool rec_point_visible(void* context, uint8_t player, const oa::FixedVec3* at) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.point_visible == nullptr)
        return false;
    return recorded.live.host.point_visible(recorded.live.host.context, player, at);
}

const sim::detection::Sightings* rec_sightings(void* context, uint8_t player) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.sightings == nullptr)
        return nullptr;
    return recorded.live.host.sightings(recorded.live.host.context, player);
}

bool rec_weapon_reaches(void* context, uint16_t unit, const oa::FixedVec3* at) {
    Recording& recorded = recording_of(context);
    if (recorded.live.host.weapon_reaches == nullptr)
        return false;
    return recorded.live.host.weapon_reaches(recorded.live.host.context, unit, at);
}

bool rec_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    Recording& recorded = recording_of(context);
    LogEntry entry{};
    entry.kind = LogKind::attack_point;
    entry.slot = unit;
    fill_point(entry, at);
    push_entry(recorded.brain, entry);
    remember_order(recorded, unit);
    return true;
}

int32_t rec_unit_squad(void* context, uint16_t slot) {
    Recording& recorded = recording_from_host(context);
    if (recorded.detail != nullptr && slot < recorded.detail->unit_count &&
        recorded.detail->units != nullptr && recorded.detail->units[slot].present)
        return recorded.detail->units[slot].squad;
    if (recorded.live.unit_squad == nullptr)
        return -1;
    return recorded.live.unit_squad(&recorded.live.host, slot);
}

bool rec_nearest(
    void* context,
    uint8_t player,
    int32_t x,
    int32_t z,
    bool widened,
    bool skip_submerged,
    uint16_t* out_slot
) {
    Recording& recorded = recording_from_host(context);
    if (recorded.live.nearest_enemy == nullptr)
        return false;
    return recorded.live.nearest_enemy(
        &recorded.live.host, player, x, z, widened, skip_submerged, out_slot
    );
}

void rec_write_stance(void* context, uint16_t slot, int32_t move, int32_t fire) {
    Recording& recorded = recording_from_host(context);
    int32_t logged_move = move;
    int32_t logged_fire = fire;
    if ((move < 0 || fire < 0) && recorded.live.host.world != nullptr) {
        const oa::Unit* unit = oa::world_unit_at(recorded.live.host.world, slot);
        if (unit != nullptr) {
            const uint32_t flags = unit->flags;
            if (move < 0)
                logged_move = static_cast<int32_t>(
                    (flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT
                );
            if (fire < 0)
                logged_fire = static_cast<int32_t>(
                    (flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT
                );
        }
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
    push_entry(recorded.brain, entry);
}

void rec_section(void* context, const char* name) {
    Recording& recorded = recording_from_host(context);
    LogEntry entry{};
    entry.kind = LogKind::section;
    copy_name(entry.name, name);
    push_entry(recorded.brain, entry);
    if (recorded.detail == nullptr)
        return;
    copy_name(recorded.detail->section_name, name);
    recorded.detail->draw_index = 0;
    const int marker = find_section(recorded.builtin, recorded.detail->section_name);
    if (marker < 0) {
        reset_view(*recorded.detail);
        return;
    }
    const LogEntry& found = recorded.builtin->items[static_cast<uint32_t>(marker)];
    recorded.copy.restore(found.state_before);
    apply_before(*recorded.detail, recorded.builtin, static_cast<uint32_t>(marker));
}

uint8_t rec_kind(void* context, uint16_t slot) {
    Recording& recorded = recording_from_host(context);
    if (recorded.detail != nullptr && head_is_live(*recorded.detail, slot)) {
        if (recorded.live.primary_kind == nullptr)
            return 0;
        return recorded.live.primary_kind(&recorded.live.host, slot);
    }
    if (recorded.detail == nullptr || slot >= recorded.detail->unit_count ||
        recorded.detail->units == nullptr || !recorded.detail->units[slot].has_primary)
        return 0;
    return recorded.detail->units[slot].kind;
}

bool rec_site_facing(
    void* context, uint16_t type, int32_t cell_x, int32_t cell_z, int32_t facing, bool yard
) {
    Recording& recorded = recording_from_host(context);
    if (recorded.live.site_facing == nullptr)
        return false;
    return recorded.live.site_facing(&recorded.live.host, type, cell_x, cell_z, facing, yard);
}

bool rec_site_fogged(
    void* context,
    uint8_t player,
    uint16_t type,
    int32_t cell_x,
    int32_t cell_z,
    int32_t facing,
    bool yard
) {
    Recording& recorded = recording_from_host(context);
    if (recorded.live.site_fogged == nullptr)
        return false;
    return recorded.live.site_fogged(
        &recorded.live.host, player, type, cell_x, cell_z, facing, yard
    );
}

bool rec_attack_queued(void* context, uint16_t slot, uint16_t) {
    Recording& recorded = recording_from_host(context);
    recording_refuse(recorded, "attack", slot);
    return true;
}

bool rec_build_shaped(void* context, uint16_t slot, uint16_t, const oa::FixedVec3*, bool, int32_t) {
    Recording& recorded = recording_from_host(context);
    recording_refuse(recorded, "build", slot);
    return true;
}

bool rec_weapon_slot(void* context, uint16_t unit, const oa::FixedVec3* at, uint8_t weapon) {
    Recording& recorded = recording_from_host(context);
    if (recorded.live.weapon_reaches_slot == nullptr)
        return false;
    return recorded.live.weapon_reaches_slot(&recorded.live.host, unit, at, weapon);
}

void rec_refuse(void* context, const char* name, uint16_t slot) {
    recording_refuse(recording_from_host(context), name, slot);
}

const sim::ai::ComputerKnowledge*
rec_knowledge(void* context, uint8_t player, uint32_t* refreshed_tick) {
    Recording& recorded = recording_from_host(context);
    if (recorded.live.knowledge == nullptr)
        return nullptr;
    return recorded.live.knowledge(&recorded.live.host, player, refreshed_tick);
}

uint32_t rec_builds_held_until(void* context, uint8_t player) {
    Recording& recorded = recording_from_host(context);
    if (recorded.live.builds_held_until == nullptr)
        return 0;
    return recorded.live.builds_held_until(&recorded.live.host, player);
}

void arm_recording(Recording& recorded) noexcept {
    SeatCalls& calls = recorded.calls;
    calls.host.context = &recorded;
    calls.host.world = recorded.live.host.world;
    calls.host.difficulty = recorded.live.host.difficulty;
    calls.host.map_cells_x = recorded.live.host.map_cells_x;
    calls.host.map_cells_z = recorded.live.host.map_cells_z;
    calls.host.surface_metal = recorded.live.host.surface_metal;
    calls.host.random = rec_random;
    calls.host.squad_size = rec_squad_size;
    calls.host.squad_member = rec_squad_member;
    calls.host.set_squad = rec_set_squad;
    calls.host.allied = rec_allied;
    calls.host.primary_order = rec_primary;
    calls.host.secondary_order = rec_secondary;
    calls.host.unit_visible = rec_visible;
    calls.host.strengths = rec_strengths;
    calls.host.site_clear = rec_site_clear;
    calls.host.building_site = rec_building_site;
    calls.host.cell_metal = rec_cell_metal;
    calls.host.metal_feature = rec_metal_feature;
    calls.host.order_move = rec_order_move;
    calls.host.order_patrol = rec_order_patrol;
    calls.host.order_attack = rec_order_attack;
    calls.host.order_build = rec_order_build;
    calls.host.order_factory = rec_order_factory;
    calls.host.set_active = rec_set_active;
    calls.host.point_visible = rec_point_visible;
    calls.host.sightings = rec_sightings;
    calls.host.weapon_reaches = rec_weapon_reaches;
    calls.host.order_attack_point = rec_attack_point;
    calls.unit_squad = rec_unit_squad;
    calls.nearest_enemy = rec_nearest;
    calls.write_stance = rec_write_stance;
    calls.section = rec_section;
    calls.primary_kind = rec_kind;
    calls.site_facing = rec_site_facing;
    calls.site_fogged = rec_site_fogged;
    calls.attack_queued = rec_attack_queued;
    calls.build_shaped = rec_build_shaped;
    calls.weapon_reaches_slot = rec_weapon_slot;
    calls.refuse = rec_refuse;
    calls.knowledge = recorded.live.knowledge != nullptr ? rec_knowledge : nullptr;
    calls.builds_held_until =
        recorded.live.builds_held_until != nullptr ? rec_builds_held_until : nullptr;
}

uint32_t bit_of(const char* name) noexcept {
    if (name == nullptr)
        return 0;
    if (same_name(name, "start"))
        return shadow_bit_start;
    if (same_name(name, "schedule"))
        return shadow_bit_schedule;
    if (same_name(name, "sort"))
        return shadow_bit_sort;
    if (name[0] == 't' && name[1] == 'a' && name[2] == 's' && name[3] == 'k' && name[4] >= '1' &&
        name[4] <= '9' && name[5] == '\0')
        return shadow_bit_task(static_cast<uint8_t>(name[4] - '0'));
    return 0;
}

bool masked(uint32_t mask, const char* name) noexcept {
    const uint32_t bit = bit_of(name);
    return bit != 0 && (mask & bit) != 0;
}

bool same_entry(const LogEntry& left, const LogEntry& right) noexcept {
    if (left.kind != right.kind || left.slot != right.slot || left.extra != right.extra)
        return false;
    if (left.x != right.x || left.y != right.y || left.z != right.z)
        return false;
    if (left.count != right.count || left.bound != right.bound || left.value != right.value)
        return false;
    if (left.flag != right.flag || left.fire != right.fire)
        return false;
    return same_name(left.name, right.name);
}

bool skip_entry(LogKind kind) noexcept {
    return kind == LogKind::resync || kind == LogKind::stance || kind == LogKind::section;
}

uint32_t next_marker(const CallLog& log, uint32_t after) noexcept {
    for (uint32_t index = after; index < log.count; ++index) {
        if (log.items[index].kind == LogKind::section)
            return index;
    }
    return log.count;
}

bool range_equal(
    const CallLog& left,
    uint32_t left_at,
    uint32_t left_end,
    const CallLog& right,
    uint32_t right_at,
    uint32_t right_end
) noexcept {
    while (true) {
        while (left_at < left_end && skip_entry(left.items[left_at].kind))
            ++left_at;
        while (right_at < right_end && skip_entry(right.items[right_at].kind))
            ++right_at;
        if (left_at >= left_end && right_at >= right_end)
            return true;
        if (left_at >= left_end || right_at >= right_end)
            return false;
        if (!same_entry(left.items[left_at], right.items[right_at]))
            return false;
        ++left_at;
        ++right_at;
    }
}

bool find_nth(const CallLog& log, const char* name, uint32_t nth, uint32_t* index) noexcept {
    uint32_t seen = 0;
    for (uint32_t cursor = 0; cursor < log.count; ++cursor) {
        if (log.items[cursor].kind != LogKind::section || !same_name(log.items[cursor].name, name))
            continue;
        if (seen == nth) {
            *index = cursor;
            return true;
        }
        ++seen;
    }
    return false;
}

bool schedule_equal(const CallLog& builtin, const CallLog& brain, uint32_t mask) noexcept {
    uint32_t left = 0;
    uint32_t right = 0;
    while (true) {
        while (left < builtin.count && (builtin.items[left].kind != LogKind::section ||
                                        !masked(mask, builtin.items[left].name)))
            ++left;
        while (right < brain.count && (brain.items[right].kind != LogKind::section ||
                                       !masked(mask, brain.items[right].name)))
            ++right;
        if (left >= builtin.count && right >= brain.count)
            return true;
        if (left >= builtin.count || right >= brain.count)
            return false;
        if (!same_name(builtin.items[left].name, brain.items[right].name))
            return false;
        ++left;
        ++right;
    }
}

bool stances_equal(const CallLog& builtin, const CallLog& brain) noexcept {
    uint32_t left = 0;
    uint32_t right = 0;
    while (true) {
        while (left < builtin.count && builtin.items[left].kind != LogKind::stance)
            ++left;
        while (right < brain.count && brain.items[right].kind != LogKind::stance)
            ++right;
        if (left >= builtin.count && right >= brain.count)
            return true;
        if (left >= builtin.count || right >= brain.count)
            return false;
        if (!same_entry(builtin.items[left], brain.items[right]))
            return false;
        ++left;
        ++right;
    }
}

struct TextBuf {
    char* data{};
    uint32_t count{};
    uint32_t capacity{};

    ~TextBuf() { delete[] data; }

    TextBuf() = default;
    TextBuf(const TextBuf&) = delete;
    TextBuf& operator=(const TextBuf&) = delete;
};

bool text_reserve(TextBuf& buffer, uint32_t extra) noexcept {
    const uint32_t need = buffer.count + extra + 1u;
    if (need < buffer.count)
        return false;
    if (need <= buffer.capacity)
        return true;
    uint32_t next = buffer.capacity == 0 ? 256u : buffer.capacity;
    while (next < need) {
        if (next > 0x7fffffffu)
            return false;
        next *= 2u;
    }
    auto* fresh = new (std::nothrow) char[next];
    if (fresh == nullptr)
        return false;
    if (buffer.data != nullptr && buffer.count > 0)
        std::memcpy(fresh, buffer.data, buffer.count);
    delete[] buffer.data;
    buffer.data = fresh;
    buffer.capacity = next;
    return true;
}

bool text_add(TextBuf& buffer, const char* text, uint32_t length) noexcept {
    if (!text_reserve(buffer, length))
        return false;
    if (length > 0)
        std::memcpy(buffer.data + buffer.count, text, length);
    buffer.count += length;
    buffer.data[buffer.count] = '\0';
    return true;
}

bool text_line(TextBuf& buffer, const char* line) noexcept {
    char storage[192];
    const int wrote = std::snprintf(storage, sizeof storage, "%s\n", line);
    if (wrote < 0)
        return false;
    return text_add(buffer, storage, static_cast<uint32_t>(wrote));
}

int write_heading(
    char* storage, std::size_t size, uint32_t tick, uint8_t player, bool brain
) noexcept {
    if (storage == nullptr || size == 0)
        return -1;
    return std::snprintf(
        storage,
        size,
        "tick %u seat %u %s\n",
        tick,
        static_cast<unsigned>(player),
        brain ? "brain" : "builtin"
    );
}

bool text_heading(TextBuf& buffer, uint32_t tick, uint8_t player, bool brain) noexcept {
    char storage[128];
    const int wrote = write_heading(storage, sizeof storage, tick, player, brain);
    if (wrote < 0)
        return false;
    return text_add(buffer, storage, static_cast<uint32_t>(wrote));
}

int write_entry_line(char* storage, std::size_t size, const LogEntry& entry) noexcept {
    if (storage == nullptr || size == 0)
        return -1;
    int wrote = 0;
    switch (entry.kind) {
    case LogKind::section:
        wrote = std::snprintf(storage, size, "section %s\n", entry.name);
        break;
    case LogKind::draw:
        wrote = std::snprintf(storage, size, "draw %u %u\n", entry.bound, entry.value);
        break;
    case LogKind::assign:
        wrote = std::snprintf(
            storage,
            size,
            "assign %u %u\n",
            static_cast<unsigned>(entry.slot),
            static_cast<unsigned>(entry.extra)
        );
        break;
    case LogKind::move:
        wrote = std::snprintf(
            storage,
            size,
            "move %u %d %d %d %u\n",
            static_cast<unsigned>(entry.slot),
            entry.x,
            entry.y,
            entry.z,
            static_cast<unsigned>(entry.flag)
        );
        break;
    case LogKind::patrol:
        wrote = std::snprintf(
            storage,
            size,
            "patrol %u %d %d %d %u\n",
            static_cast<unsigned>(entry.slot),
            entry.x,
            entry.y,
            entry.z,
            static_cast<unsigned>(entry.flag)
        );
        break;
    case LogKind::attack:
        wrote = std::snprintf(
            storage,
            size,
            "attack %u %u\n",
            static_cast<unsigned>(entry.slot),
            static_cast<unsigned>(entry.extra)
        );
        break;
    case LogKind::build:
        wrote = std::snprintf(
            storage,
            size,
            "build %u %u %d %d %d\n",
            static_cast<unsigned>(entry.slot),
            static_cast<unsigned>(entry.extra),
            entry.x,
            entry.y,
            entry.z
        );
        break;
    case LogKind::queue_order:
        wrote = std::snprintf(
            storage,
            size,
            "queue %u %u %d\n",
            static_cast<unsigned>(entry.slot),
            static_cast<unsigned>(entry.extra),
            entry.count
        );
        break;
    case LogKind::on:
        wrote = std::snprintf(
            storage,
            size,
            "on %u %u\n",
            static_cast<unsigned>(entry.slot),
            static_cast<unsigned>(entry.flag)
        );
        break;
    case LogKind::attack_point:
        wrote = std::snprintf(
            storage,
            size,
            "attack_point %u %d %d %d\n",
            static_cast<unsigned>(entry.slot),
            entry.x,
            entry.y,
            entry.z
        );
        break;
    case LogKind::stance:
        wrote = std::snprintf(
            storage,
            size,
            "stance %u %d %d\n",
            static_cast<unsigned>(entry.slot),
            entry.count,
            entry.fire
        );
        break;
    case LogKind::refused:
        wrote = std::snprintf(
            storage, size, "refused %s %u\n", entry.name, static_cast<unsigned>(entry.slot)
        );
        break;
    case LogKind::resync:
        wrote = std::snprintf(storage, size, "resync %u\n", entry.state_before);
        break;
    }
    return wrote;
}

bool render_entry(TextBuf& buffer, const LogEntry& entry) noexcept {
    char storage[192];
    const int wrote = write_entry_line(storage, sizeof storage, entry);
    if (wrote < 0)
        return false;
    return text_add(buffer, storage, static_cast<uint32_t>(wrote));
}

char* render_log(const CallLog& log, uint32_t tick, uint8_t player, bool brain) noexcept {
    TextBuf buffer;
    if (!text_heading(buffer, tick, player, brain))
        return nullptr;
    for (uint32_t index = 0; index < log.count; ++index) {
        if (!render_entry(buffer, log.items[index]))
            return nullptr;
    }
    char* taken = buffer.data;
    buffer.data = nullptr;
    buffer.count = 0;
    buffer.capacity = 0;
    return taken;
}

char* render_start_builtin(
    const match_runtime::SetUpRecord& record, uint32_t tick, uint8_t player
) noexcept {
    TextBuf buffer;
    if (!text_heading(buffer, tick, player, false))
        return nullptr;
    if (!text_line(buffer, "section start"))
        return nullptr;
    const uint8_t count = record.draw_count;
    for (uint8_t index = 0; index < count && index < record.draws.size(); ++index) {
        char storage[64];
        const int wrote = std::snprintf(
            storage,
            sizeof storage,
            "draw %u %u\n",
            record.draws[index].bound,
            record.draws[index].value
        );
        if (wrote < 0 || !text_add(buffer, storage, static_cast<uint32_t>(wrote)))
            return nullptr;
    }
    char* taken = buffer.data;
    buffer.data = nullptr;
    return taken;
}

const Seat* seat_at(const Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return nullptr;
    const std::shared_ptr<void>& state = match.seat_state();
    if (!state)
        return nullptr;
    return static_cast<const Brains*>(state.get())->seats[player].get();
}

ShadowBody* body_of(Seat& seat) noexcept {
    return static_cast<ShadowBody*>(seat.shadow_body);
}

void copy_message(char* out, const script_host::ScriptError& error) noexcept {
    const char* data = error.message.data();
    const std::size_t size = error.message.size();
    std::size_t count = size < 255 ? size : 255;
    for (std::size_t index = 0; index < count; ++index)
        out[index] = data[index];
    out[count] = '\0';
}

void report_stop(Seat& seat, uint32_t tick, const script_host::ScriptError& error) noexcept {
    if (seat.match == nullptr)
        return;
    std::shared_ptr<void>& state = seat.match->seat_state();
    if (!state)
        return;
    const ReportHooks hooks = static_cast<Brains*>(state.get())->reports;
    if (hooks.stopped == nullptr)
        return;
    try {
        hooks.stopped(hooks.context, seat.player, tick, error);
    } catch (const std::exception&) {
    }
}

void note_shadow_failure(
    Seat& seat, uint32_t tick, const script_host::ScriptError& error
) noexcept {
    seat.stopped = true;
    ShadowBody* body = body_of(seat);
    if (body == nullptr)
        return;
    body->stopped = true;
    body->stopped_tick = tick;
    copy_message(body->message, error);
    if (body->stop_noted)
        return;
    body->stop_noted = true;
    report_stop(seat, tick, error);
}

const char* difficulty_word(int32_t difficulty) noexcept {
    if (difficulty == OA_DIFFICULTY_EASY)
        return "easy";
    if (difficulty == OA_DIFFICULTY_HARD)
        return "hard";
    return "medium";
}

void copy_word(char* out, std::size_t room, const char* text) noexcept {
    std::size_t index = 0;
    if (text != nullptr && room > 0) {
        while (text[index] != '\0' && index + 1 < room) {
            out[index] = text[index];
            ++index;
        }
    }
    if (room > 0)
        out[index] = '\0';
}

struct StartBox {
    Seat* seat{};
};

int push_shadow_start(void* context, lua_State* state) {
    auto* box = static_cast<StartBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "start");
    lua_remove(state, -2);
    lua_getglobal(state, "game");
    lua_getglobal(state, "me");
    lua_pushnil(state);
    return 3;
}

void run_shadow_start(Seat& seat) noexcept {
    ShadowBody* body = body_of(seat);
    if (body == nullptr || body->started || seat.stopped)
        return;
    body->started = true;
    LogEntry section{};
    section.kind = LogKind::section;
    copy_name(section.name, "start");
    (void)log_push(body->start_brain, section);
    if (!seat.has_start || !seat.sandbox.has_value())
        return;
    CallLog* saved_brain = body->recording.brain;
    const CallLog* saved_builtin = body->recording.builtin;
    const bool saved_ready = body->recording.turn_ready;
    body->recording.brain = &body->start_brain;
    body->recording.builtin = nullptr;
    body->recording.turn_ready = false;
    body->in_start = true;
    StartBox box;
    box.seat = &seat;
    script_host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_shadow_start;
    script_host::ScriptError error;
    const bool called = seat.sandbox->call("start", hooks, error);
    body->in_start = false;
    body->recording.brain = saved_brain;
    body->recording.builtin = saved_builtin;
    body->recording.turn_ready = saved_ready;
    note_call_instructions(seat);
    note_prints(seat);
    if (!called)
        note_shadow_failure(seat, seat.match != nullptr ? seat.match->state().game.tick : 0, error);
}

void remember_setup_tick(ShadowBody& body, uint32_t tick) noexcept {
    if (body.setup_tick_known)
        return;
    body.setup_tick = tick;
    body.setup_tick_known = true;
}

void keep_record(Seat& seat, const match_runtime::SetUpRecord& record, uint32_t tick) noexcept {
    seat.setup = record;
    seat.setup_ready = true;
    seat.setup_copy.restore(record.stream_state);
    seat.handed_over = record.handed_over;
    ShadowBody* body = body_of(seat);
    if (body != nullptr)
        remember_setup_tick(*body, tick);
}

void note_difficulty(Seat& seat, Match& match) noexcept {
    const int32_t difficulty = sim::ai::computer_player_difficulty(
        sim::ai::match_computer_players(match), seat.player, match.difficulty()
    );
    copy_word(seat.difficulty, sizeof seat.difficulty, difficulty_word(difficulty));
    if (seat.sandbox.has_value())
        set_hidden_difficulty(seat.sandbox->state(), seat.difficulty);
}

bool note_setup_draw(ShadowBody& body, uint32_t bound, uint32_t value) noexcept {
    if (body.setup_count == body.setup_capacity) {
        const uint32_t next = body.setup_capacity == 0 ? 8u : body.setup_capacity * 2u;
        if (next < body.setup_capacity)
            return false;
        auto* draws = new (std::nothrow) SetupNote[next];
        if (draws == nullptr)
            return false;
        for (uint32_t index = 0; index < body.setup_count; ++index)
            draws[index] = body.setup_draws[index];
        delete[] body.setup_draws;
        body.setup_draws = draws;
        body.setup_capacity = next;
    }
    body.setup_draws[body.setup_count].bound = bound;
    body.setup_draws[body.setup_count].value = value;
    ++body.setup_count;
    LogEntry entry{};
    entry.kind = LogKind::draw;
    entry.bound = bound;
    entry.value = value;
    return log_push(body.start_brain, entry);
}

void erase_range(CallLog& log, uint32_t begin, uint32_t end) noexcept {
    if (begin >= end || begin >= log.count || log.items == nullptr)
        return;
    if (end > log.count)
        end = log.count;
    const uint32_t tail = log.count - end;
    for (uint32_t index = 0; index < tail; ++index)
        log.items[begin + index] = log.items[end + index];
    log.count -= end - begin;
}

void take_start_record(Seat& seat, const CallLog& log, uint32_t marker, uint32_t end) noexcept {
    match_runtime::SetUpRecord record{};
    record.handed_over = true;
    record.stream_state = log.items[marker].state_before;
    for (uint32_t index = marker + 1; index < end && record.draw_count < 8; ++index) {
        if (log.items[index].kind != LogKind::draw)
            continue;
        record.draws[record.draw_count].bound = log.items[index].bound;
        record.draws[record.draw_count].value = log.items[index].value;
        ++record.draw_count;
    }
    const uint32_t tick = seat.match != nullptr ? seat.match->state().game.tick : 0;
    keep_record(seat, record, tick);
    note_difficulty(seat, *seat.match);
    run_shadow_start(seat);
}

void strip_start(Seat& seat) noexcept {
    ShadowBody* body = body_of(seat);
    if (body == nullptr)
        return;
    CallLog& log = body->builtin_log;
    int marker = -1;
    for (uint32_t index = 0; index < log.count; ++index) {
        if (log.items[index].kind == LogKind::section &&
            same_name(log.items[index].name, "start")) {
            marker = static_cast<int>(index);
            break;
        }
    }
    if (marker < 0)
        return;
    uint32_t end = log.count;
    for (uint32_t index = static_cast<uint32_t>(marker) + 1; index < log.count; ++index) {
        if (log.items[index].kind == LogKind::section) {
            end = index;
            break;
        }
    }
    if (!body->started && !seat.stopped && seat.match != nullptr)
        take_start_record(seat, log, static_cast<uint32_t>(marker), end);
    erase_range(log, static_cast<uint32_t>(marker), end);
}

ShadowStart compare_start(const ShadowBody& body, const Seat& seat) noexcept {
    if ((body.mask & shadow_bit_start) == 0)
        return ShadowStart::not_compared;
    if (body.setup_count != seat.setup.draw_count)
        return ShadowStart::differs;
    const uint32_t count = body.setup_count;
    for (uint32_t index = 0; index < count; ++index) {
        if (body.setup_draws == nullptr)
            return ShadowStart::differs;
        if (body.setup_draws[index].bound != seat.setup.draws[index].bound ||
            body.setup_draws[index].value != seat.setup.draws[index].value)
            return ShadowStart::differs;
    }
    return ShadowStart::same;
}

void drop_oldest(ShadowBody& body) noexcept {
    if (body.ring_count == 0)
        return;
    delete[] body.ring[0].builtin;
    delete[] body.ring[0].brain;
    for (uint8_t index = 1; index < body.ring_count; ++index)
        body.ring[index - 1] = body.ring[index];
    --body.ring_count;
    body.ring[body.ring_count] = {};
}

void remember_tick(ShadowBody& body, uint32_t tick, uint8_t player) noexcept {
    if (body.frozen)
        return;
    if (body.ring_count == 3)
        drop_oldest(body);
    TickText& slot = body.ring[body.ring_count];
    slot.tick = tick;
    slot.builtin = render_log(body.builtin_log, tick, player, false);
    slot.brain = render_log(body.brain_log, tick, player, true);
    ++body.ring_count;
}

void freeze_ring(ShadowBody& body) noexcept {
    body.frozen_count = body.ring_count;
    for (uint8_t index = 0; index < body.ring_count; ++index) {
        body.frozen_ticks[index] = body.ring[index];
        body.ring[index] = {};
    }
    body.ring_count = 0;
    body.frozen = true;
}

void store_difference(ShadowBody& body, uint32_t tick, const TickDifference& difference) noexcept {
    ++body.differ_count;
    if (body.frozen)
        return;
    body.first_tick = tick;
    copy_name(body.section, difference.section);
    freeze_ring(body);
}

void compare_turn(ShadowBody& body, uint32_t tick, uint8_t player) noexcept {
    ++body.ticks;
    remember_tick(body, tick, player);
    const TickDifference difference = compare_logs(body.builtin_log, body.brain_log, body.mask);
    if (difference.different)
        store_difference(body, tick, difference);
}

uint32_t match_stream(void* context) {
    return static_cast<Match*>(context)->random_state();
}

void fill_report(ShadowBody& body, const Seat& seat) noexcept {
    ShadowReport& report = body.report;
    report = {};
    report.start = compare_start(body, seat);
    report.ticks = body.ticks;
    report.differ_count = body.differ_count;
    report.first_tick = body.first_tick;
    copy_name(report.section, body.section);
    report.stopped = body.stopped;
    report.stopped_tick = body.stopped_tick;
    report.message = body.message;
    delete[] body.start_builtin_text;
    delete[] body.start_brain_text;
    const uint32_t tick = body.setup_tick_known ? body.setup_tick : 0;
    body.start_builtin_text = render_start_builtin(seat.setup, tick, seat.player);
    body.start_brain_text = render_log(body.start_brain, tick, seat.player, true);
    report.start_builtin = body.start_builtin_text;
    report.start_brain = body.start_brain_text;
    report.tick_files = body.frozen_count;
    for (uint8_t index = 0; index < body.frozen_count && index < 3; ++index) {
        report.tick_builtin[index] = body.frozen_ticks[index].builtin;
        report.tick_brain[index] = body.frozen_ticks[index].brain;
        report.tick_numbers[index] = body.frozen_ticks[index].tick;
    }
}

} // namespace

int format_call_log_entry(const LogEntry& entry, char* storage, uint32_t storage_size) noexcept {
    return write_entry_line(storage, storage_size, entry);
}

int format_tick_heading(
    uint32_t tick, uint8_t player, bool brain, char* storage, uint32_t storage_size
) noexcept {
    return write_heading(storage, storage_size, tick, player, brain);
}

char* render_call_log(const CallLog& log, uint32_t tick, uint8_t player, bool brain) noexcept {
    return render_log(log, tick, player, brain);
}

CallLog::~CallLog() {
    delete[] items;
}

bool log_push(CallLog& log, const LogEntry& entry) noexcept {
    if (log.count == log.capacity && !grow_entries(log))
        return false;
    log.items[log.count] = entry;
    ++log.count;
    return true;
}

void log_clear(CallLog& log) noexcept {
    log.count = 0;
}

void logging_arm(
    LoggingHost& session,
    const sim::ai::ComputerHost& inner,
    CallLog& log,
    uint8_t player,
    uint32_t (*stream_state)(void* context),
    void* stream_context
) noexcept {
    session.inner = inner;
    session.log = &log;
    session.player = player;
    session.stream_state = stream_state;
    session.stream_context = stream_context;
    session.saw_sort = false;
    arm_logging_host(session);
}

void logging_note_stances(LoggingHost& session) noexcept {
    if (!session.saw_sort)
        return;
    session.saw_sort = false;
    oa::World* world = session.host.world;
    if (world == nullptr || session.player >= OA_PLAYER_COUNT)
        return;
    uint32_t span = 0;
    oa::Unit* first = oa::world_player_units(world, &world->game.players[session.player], &span);
    if (first == nullptr || world->units == nullptr)
        return;
    const auto begin = static_cast<uint32_t>(first - world->units);
    for (uint32_t step = 0; step < span; ++step) {
        const oa::Unit& unit = first[step];
        if ((unit.flags & OA_UNIT_FLAG_SELECTABLE) == 0)
            continue;
        if (unit.type_index < 1 || unit.type_index >= world->unit_def_count)
            continue;
        const uint32_t flags = unit.flags;
        LogEntry entry{};
        entry.kind = LogKind::stance;
        entry.slot = static_cast<uint16_t>(begin + step);
        entry.count = static_cast<int32_t>(
            (flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT
        );
        entry.fire = static_cast<int32_t>(
            (flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT
        );
        push_entry(session.log, entry);
    }
}

Recording::~Recording() {
    delete detail;
}

void recording_bind(Recording& recorded, const SeatCalls& live, uint8_t player) noexcept {
    recorded.live = live;
    recorded.player = player;
    recorded.turn_ready = false;
    recorded.brain = nullptr;
    recorded.builtin = nullptr;
    delete recorded.detail;
    recorded.detail = new (std::nothrow) RecordDetail;
    arm_recording(recorded);
}

void recording_begin(
    Recording& recorded, CallLog& brain, const CallLog* builtin, uint32_t stream
) noexcept {
    recorded.brain = &brain;
    recorded.builtin = builtin;
    recorded.copy.restore(stream);
    recorded.turn_ready = true;
    if (recorded.detail == nullptr)
        return;
    recorded.detail->section_name[0] = '\0';
    recorded.detail->draw_index = 0;
    snapshot_detail(*recorded.detail, recorded.live, recorded.player);
}

void recording_refuse(Recording& recorded, const char* name, uint16_t slot) noexcept {
    LogEntry entry{};
    entry.kind = LogKind::refused;
    copy_name(entry.name, name);
    entry.slot = slot;
    push_entry(recorded.brain, entry);
}

TickDifference compare_logs(const CallLog& builtin, const CallLog& brain, uint32_t mask) noexcept {
    TickDifference difference{};
    if ((mask & shadow_bit_schedule) != 0 && !schedule_equal(builtin, brain, mask)) {
        difference.different = true;
        copy_name(difference.section, "schedule");
        return difference;
    }
    uint32_t seen_names = 0;
    char seen[marker_limit][name_limit]{};
    uint8_t seen_count[marker_limit]{};
    for (uint32_t index = 0; index < builtin.count; ++index) {
        if (builtin.items == nullptr || builtin.items[index].kind != LogKind::section)
            continue;
        const char* name = builtin.items[index].name;
        if (!masked(mask, name) || same_name(name, "start"))
            continue;
        uint32_t slot = seen_names;
        for (uint32_t prior = 0; prior < seen_names; ++prior) {
            if (same_name(seen[prior], name)) {
                slot = prior;
                break;
            }
        }
        if (slot == seen_names && seen_names < marker_limit) {
            copy_name(seen[seen_names], name);
            ++seen_names;
        }
        if (slot >= marker_limit)
            continue;
        const uint32_t nth = seen_count[slot];
        ++seen_count[slot];
        uint32_t brain_at = 0;
        if (!find_nth(brain, name, nth, &brain_at)) {
            difference.different = true;
            copy_name(difference.section, name);
            return difference;
        }
        const uint32_t left_end = next_marker(builtin, index + 1);
        const uint32_t right_end = next_marker(brain, brain_at + 1);
        if (!range_equal(builtin, index + 1, left_end, brain, brain_at + 1, right_end)) {
            difference.different = true;
            copy_name(difference.section, name);
            return difference;
        }
    }
    uint32_t brain_seen = 0;
    char brain_names[marker_limit][name_limit]{};
    uint8_t brain_counts[marker_limit]{};
    for (uint32_t index = 0; index < brain.count; ++index) {
        if (brain.items == nullptr || brain.items[index].kind != LogKind::section)
            continue;
        const char* name = brain.items[index].name;
        if (!masked(mask, name) || same_name(name, "start"))
            continue;
        uint32_t slot = brain_seen;
        for (uint32_t prior = 0; prior < brain_seen; ++prior) {
            if (same_name(brain_names[prior], name)) {
                slot = prior;
                break;
            }
        }
        if (slot == brain_seen && brain_seen < marker_limit) {
            copy_name(brain_names[brain_seen], name);
            ++brain_seen;
        }
        if (slot >= marker_limit)
            continue;
        const uint32_t nth = brain_counts[slot];
        ++brain_counts[slot];
        uint32_t builtin_at = 0;
        if (!find_nth(builtin, name, nth, &builtin_at)) {
            difference.different = true;
            copy_name(difference.section, name);
            return difference;
        }
    }
    if ((mask & shadow_bit_sort) != 0 && !stances_equal(builtin, brain)) {
        difference.different = true;
        copy_name(difference.section, "sort");
        return difference;
    }
    return difference;
}

uint32_t shadow_section_mask(const char* list) noexcept {
    if (list == nullptr || list[0] == '\0')
        return shadow_mask_all;
    uint32_t mask = 0;
    const char* cursor = list;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',')
            ++cursor;
        if (*cursor == '\0')
            break;
        char token[name_limit]{};
        uint32_t length = 0;
        while (*cursor != '\0' && *cursor != ',' && *cursor != ' ' && length + 1 < name_limit) {
            token[length] = *cursor;
            ++length;
            ++cursor;
        }
        token[length] = '\0';
        while (*cursor != '\0' && *cursor != ',')
            ++cursor;
        mask |= bit_of(token);
    }
    return mask;
}

const char* shadow_section_name(uint8_t section) noexcept {
    static const char* names[] = {
        "sort",
        "task1",
        "task2",
        "task3",
        "task4",
        "task5",
        "task6",
        "task7",
        "task8",
        "task9",
        "start",
    };
    if (section < sizeof names / sizeof names[0])
        return names[section];
    return "section";
}

bool brain_is_shadow(const Match& match, uint8_t player) noexcept {
    const Seat* seat = seat_at(match, player);
    return seat != nullptr && seat->shadow;
}

const match_runtime::SetUpRecord* seat_setup_record(const Match& match, uint8_t player) noexcept {
    const Seat* seat = seat_at(match, player);
    if (seat == nullptr || !seat->setup_ready)
        return nullptr;
    return &seat->setup;
}

const ShadowReport* shadow_report(const Match& match, uint8_t player) noexcept {
    const Seat* seat = seat_at(match, player);
    if (seat == nullptr || !seat->shadow || seat->shadow_body == nullptr)
        return nullptr;
    auto* body = static_cast<ShadowBody*>(seat->shadow_body);
    fill_report(*body, *seat);
    return &body->report;
}

bool shadow_play(void* context, Match& match, uint8_t player) {
    auto* seat = static_cast<Seat*>(context);
    if (seat == nullptr || seat->shadow_body == nullptr)
        return true;
    ShadowBody& body = *static_cast<ShadowBody*>(seat->shadow_body);
    const uint32_t tick = match.state().game.tick;
    const uint32_t stream = match.random_state();
    try {
        log_clear(body.builtin_log);
        log_clear(body.brain_log);
        recording_begin(body.recording, body.brain_log, &body.builtin_log, stream);
        body.recording.turn_ready = false;
        logging_arm(
            body.logging,
            sim::ai::match_computer_host(match),
            body.builtin_log,
            player,
            match_stream,
            &match
        );
        sim::ai::computer_players_adopt(
            sim::ai::match_computer_players(match), body.logging.host, player
        );
        sim::ai::computer_player_tick_orders(
            sim::ai::match_computer_players(match), body.logging.host, player
        );
        strip_start(*seat);
        logging_note_stances(body.logging);
        if (!seat->stopped) {
            body.recording.turn_ready = true;
            script_host::ScriptError error;
            if (!deliver_shadow_turn(*seat, error))
                note_shadow_failure(*seat, tick, error);
            else
                compare_turn(body, tick, player);
        }
    } catch (const std::exception&) {
    }
    publish_logged_shadow_turn(*seat, body.builtin_log, body.brain_log, tick);
    body.recording.turn_ready = false;
    return true;
}

void shadow_set_up(
    void* context, Match& match, uint8_t player, const match_runtime::SetUpRecord& record
) {
    auto* seat = static_cast<Seat*>(context);
    if (seat == nullptr)
        return;
    (void)player;
    try {
        keep_record(*seat, record, match.state().game.tick);
        note_difficulty(*seat, match);
        run_shadow_start(*seat);
    } catch (const std::exception&) {
    }
}

void playing_set_up(void* context, Match&, uint8_t, const match_runtime::SetUpRecord& record) {
    auto* seat = static_cast<Seat*>(context);
    if (seat == nullptr)
        return;
    seat->setup = record;
    seat->setup_ready = true;
    seat->setup_copy.restore(record.stream_state);
    seat->handed_over = record.handed_over;
}

void shadow_attach(Seat& seat, uint32_t mask) noexcept {
    shadow_release(seat);
    auto* body = new (std::nothrow) ShadowBody;
    if (body == nullptr)
        return;
    seat.shadow_body = body;
    body->mask = mask;
    SeatCalls live{};
    if (seat.match != nullptr)
        live = seat_calls_for_match(*seat.match);
    recording_bind(body->recording, live, seat.player);
    seat.calls = body->recording.calls;
}

void shadow_release(Seat& seat) noexcept {
    auto* body = static_cast<ShadowBody*>(seat.shadow_body);
    seat.shadow_body = nullptr;
    delete body;
}

bool shadow_stream_ready(const Seat& seat) noexcept {
    const auto* body = static_cast<const ShadowBody*>(seat.shadow_body);
    return body != nullptr && body->recording.turn_ready;
}

void shadow_note_setup_draw(Seat& seat, uint32_t bound, uint32_t value) noexcept {
    ShadowBody* body = body_of(seat);
    if (body == nullptr)
        return;
    (void)note_setup_draw(*body, bound, value);
}

} // namespace oa::sim::brains
