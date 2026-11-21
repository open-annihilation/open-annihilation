// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/replication/route_batches.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <utility>

namespace oa::netgame {
namespace {

/// Tells whether a batch holds a part.
///
/// @param batch the batch
/// @param index the part's index
/// @return true when the part is held
bool holds_part(const RouteBatchHeld& batch, std::size_t index) noexcept {
    return index < batch.parts.size() && !batch.parts[index].empty();
}

/// Tells whether a batch holds every part, its last included.
///
/// @param batch the batch
/// @return true when whole
bool whole(const RouteBatchHeld& batch) noexcept {
    return batch.last_index >= 0 && batch.held == static_cast<uint32_t>(batch.last_index) + 1u;
}

/// Returns the receiver's ticks since the first part of anything an inbox still holds arrived.
///
/// @param inbox the inbox; not empty
/// @param now the receiver's tick
/// @return the ticks, 0 when the clock reads earlier
uint32_t held_for(const RouteBatchInbox& inbox, uint32_t now) noexcept {
    uint32_t earliest = now;
    for (const auto& [tick, batch] : inbox.pending)
        if (static_cast<int32_t>(batch.first_seen - earliest) < 0)
            earliest = batch.first_seen;
    const auto waited = static_cast<int32_t>(now - earliest);
    return waited > 0 ? static_cast<uint32_t>(waited) : 0u;
}

/// Tells whether one player holds or has applied every part of a kept batch.
///
/// @param kept the batch
/// @param bit the player's slot bit
/// @return true when the player needs nothing more of it
bool held_by_player(const RouteBatchKept& kept, uint16_t bit) noexcept {
    if ((kept.applied_by & bit) != 0)
        return true;
    return std::all_of(kept.held_by.begin(), kept.held_by.end(), [bit](uint16_t held) {
        return (held & bit) != 0;
    });
}

} // namespace

RouteBatchTake route_batch_inbox_take(
    RouteBatchInbox& inbox,
    World* world,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size,
    uint32_t now,
    WireError* error
) noexcept {
    const auto refuse = [error](WireError why) {
        if (error != nullptr)
            *error = why;
        return RouteBatchTake::refused;
    };
    RouteBatchPartHeader header{};
    uint32_t changes = 0;
    const auto checked =
        replication_check_route_batch_part(world, sender_index, bytes, size, &header, &changes);
    if (checked != WireError::ok)
        return refuse(checked);
    if (header.tick <= inbox.applied_through) {
        inbox.ack_due = true;
        return RouteBatchTake::duplicate;
    }
    const std::size_t index = header.part_index;
    const auto found = inbox.pending.find(header.tick);
    if (found == inbox.pending.end()) {
        if (inbox.pending.size() >= route_batch_most_held_batches)
            return refuse(WireError::overflow);
    } else {
        const auto& batch = found->second;
        if (batch.previous_tick != header.previous_tick)
            return refuse(WireError::bad_argument);
        if (holds_part(batch, index)) {
            inbox.ack_due = true;
            return RouteBatchTake::duplicate;
        }
        if (batch.last_index >= 0 &&
            (header.last || index > static_cast<std::size_t>(batch.last_index)))
            return refuse(WireError::bad_argument);
        if (header.last)
            for (std::size_t later = index + 1; later < batch.parts.size(); ++later)
                if (holds_part(batch, later))
                    return refuse(WireError::bad_argument);
    }
    try {
        auto& batch = inbox.pending[header.tick];
        if (batch.held == 0) {
            batch.previous_tick = header.previous_tick;
            batch.first_seen = now;
        }
        if (batch.parts.size() <= index)
            batch.parts.resize(index + 1);
        batch.parts[index].assign(bytes, bytes + size);
        ++batch.held;
        batch.changes += changes;
        if (header.last)
            batch.last_index = static_cast<int32_t>(index);
    } catch (const std::exception&) {
        return refuse(WireError::overflow);
    }
    inbox.ack_due = true;
    return RouteBatchTake::fresh;
}

void route_batch_inbox_apply(
    RouteBatchInbox& inbox,
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    uint32_t now,
    RouteBatchApplied* applied
) noexcept {
    RouteBatchApplied report{};
    std::vector<RouteBatchPartView> views;
    while (!inbox.pending.empty()) {
        const auto next = inbox.pending.begin();
        const auto& batch = next->second;
        const uint32_t waited = held_for(inbox, now);
        const bool ready = whole(batch) && batch.previous_tick <= inbox.applied_through;
        const bool given_up = !ready && waited >= route_batch_hold_limit_ticks;
        if (!ready && !given_up)
            break;
        try {
            views.clear();
            for (const auto& part : batch.parts)
                if (!part.empty())
                    views.push_back({part.data(), part.size()});
        } catch (const std::exception&) {
            break;
        }
        const auto error = replication_apply_route_batch(world, sim, sender_index, views);
        ++report.batches;
        if (given_up)
            ++report.given_up;
        if (error != WireError::ok)
            ++report.errors;
        report.longest_hold = std::max(report.longest_hold, waited);
        if (batch.changes >= report.largest_changes) {
            report.largest_tick = next->first;
            report.largest_changes = batch.changes;
            report.largest_parts = static_cast<uint16_t>(views.size());
            report.largest_applied_at = now;
        }
        inbox.applied_through = next->first;
        inbox.pending.erase(next);
        inbox.ack_due = true;
    }
    if (applied == nullptr)
        return;
    applied->batches += report.batches;
    applied->given_up += report.given_up;
    applied->errors += report.errors;
    applied->longest_hold = std::max(applied->longest_hold, report.longest_hold);
    if (report.batches != 0 && report.largest_changes >= applied->largest_changes) {
        applied->largest_tick = report.largest_tick;
        applied->largest_changes = report.largest_changes;
        applied->largest_parts = report.largest_parts;
        applied->largest_applied_at = report.largest_applied_at;
    }
}

void route_batch_inbox_ack(
    const RouteBatchInbox& inbox, uint32_t owner_id, RouteBatchAck* out
) noexcept {
    if (out == nullptr)
        return;
    *out = RouteBatchAck{};
    out->owner_id = owner_id;
    out->applied_through = inbox.applied_through;
    out->highest_held =
        inbox.pending.empty() ? inbox.applied_through : inbox.pending.rbegin()->first;
    std::size_t bytes = route_batch_ack_header_bytes;
    for (const auto& [tick, batch] : inbox.pending) {
        if (out->entry_count == route_batch_ack_max_entries)
            break;
        const std::size_t parts = batch.last_index >= 0
                                      ? static_cast<std::size_t>(batch.last_index) + 1u
                                      : batch.parts.size();
        const std::size_t entry_bytes = route_batch_ack_entry_header_bytes + (parts + 7u) / 8u;
        if (bytes + entry_bytes > route_batch_record_max_bytes)
            break;
        bytes += entry_bytes;
        auto& entry = out->entries[out->entry_count++];
        entry.tick = tick;
        entry.parts = static_cast<uint16_t>(parts);
        entry.last_held = batch.last_index >= 0;
        for (std::size_t part = 0; part < parts; ++part)
            if (holds_part(batch, part))
                entry.held[part / 8u] =
                    static_cast<uint8_t>(entry.held[part / 8u] | (1u << (part % 8u)));
    }
}

void route_batch_outbox_keep(
    RouteBatchOutbox& outbox, uint32_t tick, std::vector<std::vector<uint8_t>>&& parts
) noexcept {
    try {
        RouteBatchKept kept;
        kept.tick = tick;
        kept.held_by.assign(parts.size(), 0);
        kept.sent_at.assign(parts.size(), tick);
        kept.parts = std::move(parts);
        outbox.kept.push_back(std::move(kept));
    } catch (const std::exception&) {
        // Without storage the batch is not kept; a player that lost one of
        // its parts gives it up at the hold limit.
    }
}

void route_batch_outbox_acknowledge(
    RouteBatchOutbox& outbox, uint8_t acker_slot, const RouteBatchAck& ack
) noexcept {
    if (acker_slot >= OA_PLAYER_COUNT)
        return;
    const auto bit = static_cast<uint16_t>(1u << acker_slot);
    outbox.reported[acker_slot] =
        std::max({outbox.reported[acker_slot], ack.applied_through, ack.highest_held});
    const std::size_t entries = std::min<std::size_t>(ack.entry_count, route_batch_ack_max_entries);
    for (auto& kept : outbox.kept) {
        if (kept.tick <= ack.applied_through) {
            kept.applied_by = static_cast<uint16_t>(kept.applied_by | bit);
            continue;
        }
        for (std::size_t i = 0; i < entries; ++i) {
            const auto& entry = ack.entries[i];
            if (entry.tick != kept.tick)
                continue;
            const std::size_t parts = std::min<std::size_t>(entry.parts, kept.held_by.size());
            for (std::size_t part = 0; part < parts; ++part)
                if ((entry.held[part / 8u] & (1u << (part % 8u))) != 0)
                    kept.held_by[part] = static_cast<uint16_t>(kept.held_by[part] | bit);
        }
    }
}

uint32_t route_batch_outbox_service(
    RouteBatchOutbox& outbox, uint16_t ackers, uint32_t now, const RouteBatchResend& resend
) noexcept {
    // A batch every player holds is done; one past the hold limit is given
    // up by every player still lacking it.
    std::erase_if(outbox.kept, [ackers, now](const RouteBatchKept& kept) {
        if (static_cast<int32_t>(now - kept.tick) >=
            static_cast<int32_t>(route_batch_hold_limit_ticks))
            return true;
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto bit = static_cast<uint16_t>(1u << slot);
            if ((ackers & bit) != 0 && !held_by_player(kept, bit))
                return false;
        }
        return true;
    });
    uint32_t resent = 0;
    for (auto& kept : outbox.kept) {
        for (std::size_t part = 0; part < kept.parts.size(); ++part) {
            const auto lacking =
                static_cast<uint16_t>(ackers & ~kept.applied_by & ~kept.held_by[part]);
            if (lacking == 0)
                continue;
            const auto since = static_cast<int32_t>(now - kept.sent_at[part]);
            bool sent = false;
            for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
                if ((lacking & (1u << slot)) == 0)
                    continue;
                const bool overdue = since >= static_cast<int32_t>(route_batch_resend_ticks);
                const bool overtaken =
                    static_cast<int32_t>(outbox.reported[slot] - kept.sent_at[part]) > 0 &&
                    since >= static_cast<int32_t>(route_batch_fast_resend_ticks);
                if (!overdue && !overtaken)
                    continue;
                if (resend.send != nullptr)
                    resend.send(
                        resend.context, slot, kept.parts[part].data(), kept.parts[part].size()
                    );
                sent = true;
                ++resent;
            }
            if (sent)
                kept.sent_at[part] = now;
        }
    }
    return resent;
}

} // namespace oa::netgame
