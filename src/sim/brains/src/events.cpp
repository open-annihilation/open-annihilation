// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Records match events for each seated brain. Nothing here runs a script,
// changes the match or draws from a random stream.

#include "internal.hpp"

#include "oa/sim/brains/handles.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/event_hooks.hpp"

#include "oa/core/unit.h"
#include "oa/core/world.h"

#include <cstdint>
#include <memory>
#include <new>
#include <span>

namespace oa::sim::brains {
namespace {

uint32_t generation_of(Brains& brains, uint16_t slot) {
    if (brains.match == nullptr || slot == 0)
        return 0;
    return unit_generation(*brains.match, slot);
}

Event make_event(Brains& brains, const oa::World& world, EventKind kind, uint16_t slot) {
    Event event;
    event.kind = kind;
    event.unit_slot = slot;
    event.tick = world.game.tick;
    event.unit_generation = generation_of(brains, slot);
    return event;
}

const oa::Unit* unit_at(const oa::World& world, uint16_t slot) {
    return oa::world_unit_at(&world, slot);
}

void append(Brains& brains, uint8_t player, const Event& event) {
    if (player >= OA_PLAYER_COUNT)
        return;
    Seat* seat = brains.seats[player].get();
    if (seat == nullptr)
        return;
    EventBuffer& buffer = seat->incoming;
    if (buffer.count >= max_events_per_tick) {
        ++seat->dropped;
        return;
    }
    if (buffer.count == buffer.capacity) {
        uint32_t next = 16;
        if (buffer.capacity != 0) {
            if (buffer.capacity > max_events_per_tick / 2)
                next = max_events_per_tick;
            else
                next = buffer.capacity * 2;
        }
        if (next <= buffer.capacity) {
            ++seat->dropped;
            return;
        }
        Event* grown = new (std::nothrow) Event[next];
        if (grown == nullptr) {
            ++seat->dropped;
            return;
        }
        for (uint32_t index = 0; index < buffer.count; ++index)
            grown[index] = buffer.items[index];
        delete[] buffer.items;
        buffer.items = grown;
        buffer.capacity = next;
    }
    buffer.items[buffer.count] = event;
    ++buffer.count;
}

void append_each(Brains& brains, const Event& event) {
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
        append(brains, player, event);
}

void append_owner(Brains& brains, const oa::World& world, uint16_t slot, const Event& event) {
    const oa::Unit* unit = unit_at(world, slot);
    if (unit == nullptr || unit->type_index == 0)
        return;
    append(brains, unit->owner_index, event);
}

void on_unit_created(void* context, const oa::World& world, uint16_t unit) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    append_owner(*brains, world, unit, make_event(*brains, world, EventKind::created, unit));
}

void on_unit_created_by(void* context, const oa::World& world, uint16_t unit, uint16_t builder) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr || builder == 0)
        return;
    const oa::Unit* created = unit_at(world, unit);
    if (created == nullptr || created->type_index == 0)
        return;
    if (created->owner_index >= OA_PLAYER_COUNT)
        return;
    Seat* seat = brains->seats[created->owner_index].get();
    if (seat == nullptr)
        return;
    const uint32_t builder_generation = generation_of(*brains, builder);
    Event* items = seat->incoming.items;
    const uint32_t count = seat->incoming.count;
    for (uint32_t index = count; index > 0; --index) {
        Event& event = items[index - 1];
        if (event.kind != EventKind::created || event.unit_slot != unit)
            continue;
        event.other_slot = builder;
        event.other_generation = builder_generation;
        return;
    }
}

void on_unit_finished(void* context, const oa::World& world, uint16_t unit, uint16_t builder) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::finished, unit);
    event.other_slot = builder;
    event.other_generation = generation_of(*brains, builder);
    append_owner(*brains, world, unit, event);
}

void on_unit_damaged(
    void* context,
    const oa::World& world,
    uint16_t target,
    uint16_t source,
    int16_t amount,
    uint8_t kind
) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::damaged, target);
    event.detail = kind;
    event.amount = static_cast<int32_t>(amount);
    event.other_slot = source;
    event.other_generation = generation_of(*brains, source);
    const oa::Unit* attacker = unit_at(world, source);
    if (attacker != nullptr && attacker->type_index != 0)
        event.from = attacker->owner_index;
    append_owner(*brains, world, target, event);
}

void on_unit_died(
    void* context,
    const oa::World& world,
    uint16_t unit,
    const match_runtime::KillOutcome& outcome,
    bool
) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    const oa::Unit* dying = unit_at(world, unit);
    if (dying == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::died, unit);
    event.detail = static_cast<uint8_t>(outcome.kind);
    event.from = dying->owner_index;
    event.to = dying->last_attacker_owner;
    if (dying->last_attacker_id != 0 && dying->last_attacker_id <= 0xffffu) {
        event.other_slot = static_cast<uint16_t>(dying->last_attacker_id);
        event.other_generation = generation_of(*brains, event.other_slot);
    }
    append_each(*brains, event);
}

void on_unit_given(
    void* context, const oa::World& world, uint16_t unit, uint16_t copy, uint8_t from, uint8_t to
) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::given, unit);
    event.other_slot = copy;
    event.other_generation = generation_of(*brains, copy);
    event.from = from;
    event.to = to;
    append(*brains, from, event);
    if (to != from)
        append(*brains, to, event);
}

void on_unit_idle(void* context, const oa::World& world, uint16_t unit) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    append_owner(*brains, world, unit, make_event(*brains, world, EventKind::idle, unit));
}

void on_order_ended(void* context, const oa::World& world, uint16_t unit, uint8_t kind, bool done) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::order_ended, unit);
    event.detail = kind;
    event.to = done ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    append_owner(*brains, world, unit, event);
}

void on_unit_carried(
    void* context, const oa::World& world, uint16_t unit, uint16_t carrier, bool loaded
) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    const EventKind kind = loaded ? EventKind::carried : EventKind::dropped;
    Event event = make_event(*brains, world, kind, unit);
    event.other_slot = carrier;
    event.other_generation = generation_of(*brains, carrier);
    const oa::Unit* carried = unit_at(world, unit);
    const oa::Unit* transport = unit_at(world, carrier);
    uint8_t owners[2] = {OA_PLAYER_COUNT, OA_PLAYER_COUNT};
    if (carried != nullptr && carried->type_index != 0)
        owners[0] = carried->owner_index;
    if (transport != nullptr && transport->type_index != 0)
        owners[1] = transport->owner_index;
    append(*brains, owners[0], event);
    if (owners[1] != owners[0])
        append(*brains, owners[1], event);
}

void on_wreck_placed(
    void* context, const oa::World& world, uint16_t unit, uint16_t feature, uint32_t
) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    const oa::Unit* dying = unit_at(world, unit);
    if (dying == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::wreck, unit);
    event.feature = feature;
    event.x = dying->position.x;
    event.z = dying->position.z;
    append_each(*brains, event);
}

void on_seen_changed(
    void* context,
    const oa::World& world,
    uint8_t player,
    std::span<const sim::detection::SightedUnit> gained,
    std::span<const sim::detection::SightedUnit> lost
) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    const std::span<const sim::detection::SightedUnit> lists[] = {gained, lost};
    const EventKind kinds[] = {EventKind::seen, EventKind::lost};
    for (int list = 0; list < 2; ++list) {
        const std::span<const sim::detection::SightedUnit> entries = lists[list];
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const sim::detection::SightedUnit& sighted = entries[index];
            Event event;
            event.kind = kinds[list];
            event.unit_slot = sighted.unit;
            event.unit_generation = sighted.generation;
            event.tick = world.game.tick;
            append(*brains, player, event);
        }
    }
}

void on_path_failed(void* context, const oa::World& world, uint16_t unit, uint8_t kind) {
    auto* brains = static_cast<Brains*>(context);
    if (brains == nullptr)
        return;
    Event event = make_event(*brains, world, EventKind::path_failed, unit);
    event.detail = kind;
    append_owner(*brains, world, unit, event);
}

} // namespace

void ensure_listener(Brains& brains, match_runtime::Match& match) {
    brains.match = &match;
    if (brains.listener_tried)
        return;
    brains.listener_tried = true;
    match_runtime::EventHooks hooks;
    hooks.context = &brains;
    hooks.unit_created = on_unit_created;
    hooks.unit_created_by = on_unit_created_by;
    hooks.unit_finished = on_unit_finished;
    hooks.unit_damaged = on_unit_damaged;
    hooks.unit_died = on_unit_died;
    hooks.unit_given = on_unit_given;
    hooks.unit_idle = on_unit_idle;
    hooks.order_ended = on_order_ended;
    hooks.unit_carried = on_unit_carried;
    hooks.wreck_placed = on_wreck_placed;
    hooks.seen_changed = on_seen_changed;
    hooks.path_failed = on_path_failed;
    brains.listener_id = match.add_event_listener(hooks);
}

uint32_t dropped_events(const match_runtime::Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return 0;
    // The match exposes the brains only through a non-const accessor. This
    // read does not change the match.
    const std::shared_ptr<void>& held = const_cast<match_runtime::Match&>(match).seat_state();
    if (!held)
        return 0;
    const auto* brains = static_cast<const Brains*>(held.get());
    const Seat* seat = brains->seats[player].get();
    if (seat == nullptr)
        return 0;
    return seat->dropped;
}

} // namespace oa::sim::brains
