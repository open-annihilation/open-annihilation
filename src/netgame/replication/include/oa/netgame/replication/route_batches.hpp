// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Complete route batches (oa/netgame/route_batch.hpp): in an Enhanced Mode
// match each local player's tick travels as one batch of parts in place of
// its 0x2c. The packer writes every unit whose route, traffic head, blocked
// bit or aircraft goal changed in the tick, with no byte budget, and the
// round-robin full record. A receiver keeps each sender's parts until the
// batch is whole and every earlier batch of that sender is applied, then
// applies all of its changes, steps the sender's units once and applies
// the full record, so a copy steers from exactly the heads its owner chose
// in that tick. The sender keeps a batch's parts until every other player
// has acknowledged them and sends a part again while one still lacks it.
// Unlike the rest of replication, the inbox and outbox allocate.

#include "oa/core/player.h"
#include "oa/core/world.h"
#include "oa/netgame/replication.hpp"
#include "oa/netgame/route_batch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <vector>

namespace oa::netgame {

/// Ticks a receiver holds a sender's movement step for a batch it lacks
/// parts of before it gives the batch up and applies what it holds. A
/// sender gives up resending a batch this old. It is the 900-tick lag at
/// which the game loop starts slowing for a player that falls behind.
inline constexpr uint32_t route_batch_hold_limit_ticks = 900;
/// Ticks after its last send that a part some player still lacks is sent again.
inline constexpr uint32_t route_batch_resend_ticks = 30;
/// Ticks after its last send that a part is sent again to a player that
/// already holds a batch sent after it, so lost it.
inline constexpr uint32_t route_batch_fast_resend_ticks = 6;
/// The most batches a receiver holds of one sender; parts of more are refused.
inline constexpr std::size_t route_batch_most_held_batches = 4096;

// ---- sender ----

/// Receives each part as the packer closes it.
struct RouteBatchSink {
    void* context{};

    /// Takes one part.
    ///
    /// @param context RouteBatchSink.context.
    /// @param bytes the part, type byte first
    /// @param size its length
    void (*part)(void* context, const uint8_t* bytes, std::size_t size){};
};

/// What the packer wrote for one tick.
struct RouteBatchPacked {
    uint32_t tick{};    ///< the tick packed
    uint32_t changes{}; ///< units listed
    uint16_t parts{};   ///< parts written
    uint32_t bytes{};   ///< bytes of every part together
};

/// Packs a local player's tick as one complete route batch.
///
/// Lists every live mobile unit whose driver has a delta, with no byte
/// budget: a part is closed once it reaches route_batch_part_soft_limit_bytes
/// and the next takes the following units. The last part ends with the full
/// record of the unit that rides on the tick. Each delta taken is recorded as
/// sent, as for a 0x2c. Sets the player's last_sim_tick to Game.tick.
///
/// @param world world holding the player's units
/// @param sim hooks used to reach movement records, or null
/// @param player_index player slot, 0..9
/// @param previous_tick the tick of the player's batch before this one, 0 for its first
/// @param changes false packs no unit and an empty full record, as a player
///        watching while seated sends
/// @param sink receives each part in order; called only once the whole batch is packed
/// @param[out] packed what was packed, when not null
/// @return ok; bad_argument for an invalid player, a null sink, a def index width outside 1..16, a
///         tick of 0 or a previous tick not before it; overflow beyond route_batch_max_parts parts;
///         the writer's error. Nothing reaches the sink unless the result is ok.
[[nodiscard]] WireError replication_pack_route_batch(
    World* world,
    const ReplicationSim* sim,
    uint8_t player_index,
    uint32_t previous_tick,
    bool changes,
    const RouteBatchSink& sink,
    RouteBatchPacked* packed
) noexcept;

// ---- receiver ----

/// Checks one received part against the receiving world without changing anything.
///
/// The header must be well formed. Each change must name a unit within the
/// sender's range, with a def index whose units have a delta layout, and its
/// delta must be whole. The last part's full record, when its flag is set,
/// must name a slot in the range and fill the part but for a speed word and
/// at most seven bits of padding.
///
/// @param world receiving world
/// @param sender_index player slot of the sender
/// @param bytes the part
/// @param size its size; must equal its length
/// @param[out] header the part's header, when not null; written only on success
/// @param[out] changes units the part lists, when not null; written only on success
/// @return ok; the header's errors; bad_argument for an invalid sender, a def index width outside
///         1..16 or a unit outside the sender's range; unsupported_delta_layout; truncated; length_mismatch
///         for more than seven bits left over
[[nodiscard]] WireError replication_check_route_batch_part(
    World* world,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size,
    RouteBatchPartHeader* header,
    uint32_t* changes
) noexcept;

/// One held part of a batch.
struct RouteBatchPartView {
    const uint8_t* bytes{};
    std::size_t size{};
};

/// Applies a batch: every part's changes in part order, one movement step of
/// every live mobile sender unit, then the full record its last part
/// carries.
///
/// Every part is checked before any of it applies, so a batch with a
/// malformed part changes nothing. The parts must share one tick and come in
/// ascending index order; a batch given up may lack parts, its last among
/// them. Sets the sender's last_sim_tick to the batch's tick.
///
/// @param world receiving world
/// @param sim receiver hooks, or null
/// @param sender_index player slot of the sender
/// @param parts the held parts, ascending by index; at least one
/// @return ok; bad_argument for no parts, parts of different ticks or out of order; a part's check
///         error; the full record's error
[[nodiscard]] WireError replication_apply_route_batch(
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    std::span<const RouteBatchPartView> parts
) noexcept;

/// The parts of one batch a receiver holds.
struct RouteBatchHeld {
    uint32_t previous_tick{};                ///< the sender's batch before it
    std::vector<std::vector<uint8_t>> parts; ///< by part index; empty while missing
    uint16_t held{};                         ///< parts held
    int32_t last_index{-1};                  ///< the last part's index once it is held
    uint32_t changes{};                      ///< units the held parts list
    uint32_t first_seen{};                   ///< the receiver's tick when its first part arrived
};

/// One sender's batches as a receiver holds them.
struct RouteBatchInbox {
    uint32_t applied_through{};                 ///< the tick of the last batch applied, 0 for none
    std::map<uint32_t, RouteBatchHeld> pending; ///< by tick, batches not yet applied
    bool ack_due{};      ///< something arrived since the last acknowledgement
    uint32_t last_ack{}; ///< the receiver's tick of the last acknowledgement sent
};

/// What a received part was.
enum class RouteBatchTake : uint8_t {
    fresh,     ///< a part not held before, now kept
    duplicate, ///< a part already held or of a batch already applied; nothing changed
    refused,   ///< malformed, or beyond what the inbox keeps; nothing changed
};

/// Takes one received part into a sender's inbox.
///
/// The part is checked against the world first (replication_check_route_batch_part).
/// A part is refused when its batch already holds a different previous tick
/// or a different last part, when its index lies past the last part's, or
/// when the inbox already holds route_batch_most_held_batches batches. Any
/// part that is not refused makes an acknowledgement due.
///
/// @param[in,out] inbox the sender's inbox
/// @param world receiving world
/// @param sender_index player slot of the sender
/// @param bytes the part
/// @param size its size
/// @param now the receiver's tick
/// @param[out] error why a refused part was refused, when not null
/// @return what the part was
[[nodiscard]] RouteBatchTake route_batch_inbox_take(
    RouteBatchInbox& inbox,
    World* world,
    uint8_t sender_index,
    const uint8_t* bytes,
    std::size_t size,
    uint32_t now,
    WireError* error
) noexcept;

/// What applying a sender's ready batches did.
struct RouteBatchApplied {
    uint32_t batches{};            ///< batches applied, given-up ones among them
    uint32_t given_up{};           ///< batches applied without all their parts after the hold limit
    uint32_t errors{};             ///< batches whose application failed
    uint32_t longest_hold{};       ///< the longest a batch waited, in the receiver's ticks
    uint32_t largest_tick{};       ///< the tick of the batch with the most changes applied
    uint32_t largest_changes{};    ///< its changes
    uint16_t largest_parts{};      ///< its parts
    uint32_t largest_applied_at{}; ///< the receiver's tick when it was applied
};

/// Applies a sender's batches in order while the next one is whole.
///
/// The next batch is the one whose previous tick is the last applied. A
/// batch whose first part arrived route_batch_hold_limit_ticks or more ago,
/// and every batch before it, is applied with the parts held, missing ones
/// skipped; a batch's hold is the receiver's ticks from its first part to
/// its application.
///
/// @param[in,out] inbox the sender's inbox
/// @param world receiving world
/// @param sim receiver hooks, or null
/// @param sender_index player slot of the sender
/// @param now the receiver's tick
/// @param[out] applied what was applied, added to; may be null
void route_batch_inbox_apply(
    RouteBatchInbox& inbox,
    World* world,
    const ReplicationSim* sim,
    uint8_t sender_index,
    uint32_t now,
    RouteBatchApplied* applied
) noexcept;

/// Describes what an inbox holds as an acknowledgement.
///
/// Lists up to route_batch_ack_max_entries batches held but not applied,
/// oldest first.
///
/// @param inbox the sender's inbox
/// @param owner_id the sender's player id
/// @param[out] out the acknowledgement
void route_batch_inbox_ack(
    const RouteBatchInbox& inbox, uint32_t owner_id, RouteBatchAck* out
) noexcept;

// ---- sender's copies ----

/// One batch a sender keeps until every other player holds it.
struct RouteBatchKept {
    uint32_t tick{};
    std::vector<std::vector<uint8_t>> parts;
    std::vector<uint16_t> held_by; ///< per part: bit n set once player slot n holds it
    std::vector<uint32_t> sent_at; ///< per part: the sender's tick of its last send
    uint16_t applied_by{};         ///< bit n set once player slot n applied the batch
};

/// The batches one local player has sent and not yet seen acknowledged by every other player.
struct RouteBatchOutbox {
    uint32_t last_tick{}; ///< the tick of the player's last batch, 0 before its first
    std::deque<RouteBatchKept> kept;
    std::array<uint32_t, OA_PLAYER_COUNT> reported{}; ///< by player slot, the highest tick it holds
};

/// Keeps a batch's parts, just sent, until every other player holds them.
///
/// @param[in,out] outbox the sender's outbox
/// @param tick the batch's tick, also the tick its parts were sent
/// @param parts its parts in index order
void route_batch_outbox_keep(
    RouteBatchOutbox& outbox, uint32_t tick, std::vector<std::vector<uint8_t>>&& parts
) noexcept;

/// Notes what one player's acknowledgement says it holds.
///
/// @param[in,out] outbox the sender's outbox
/// @param acker_slot player slot the acknowledgement came from, 0..9
/// @param ack the acknowledgement
void route_batch_outbox_acknowledge(
    RouteBatchOutbox& outbox, uint8_t acker_slot, const RouteBatchAck& ack
) noexcept;

/// Sends a part again to one player.
struct RouteBatchResend {
    void* context{};

    /// Sends one part to one player.
    ///
    /// @param context RouteBatchResend.context.
    /// @param acker_slot the player slot that lacks it
    /// @param bytes the part
    /// @param size its length
    void (*send)(void* context, uint8_t acker_slot, const uint8_t* bytes, std::size_t size){};
};

/// Sends again the parts players still lack, and forgets the batches every player holds.
///
/// A part goes again to a player that lacks it once route_batch_resend_ticks
/// have passed since its last send, or route_batch_fast_resend_ticks once
/// the player holds a batch sent after it. A batch every player in `ackers`
/// holds or applied, or older than route_batch_hold_limit_ticks, is
/// forgotten.
///
/// @param[in,out] outbox the sender's outbox
/// @param ackers bit n set for each player slot that must hold the batches
/// @param now the sender's tick
/// @param resend sends each part again
/// @return the parts sent again
uint32_t route_batch_outbox_service(
    RouteBatchOutbox& outbox, uint16_t ackers, uint32_t now, const RouteBatchResend& resend
) noexcept;

} // namespace oa::netgame
