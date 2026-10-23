// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat a computer seat on this machine reads and sends. The listener, the
// pace and the queue live here; the runtime feeds the three sources and
// flushes the queue after a frame's ticks.
#pragma once

#include "oa/sim/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::seat_chat {

/// Why a computer seat's line was queued or refused.
enum class Said : uint8_t {
    queued,     ///< the line waits for the next flush
    not_a_seat, ///< the slot is not a computer seat on this machine
    empty,      ///< nothing is left after leading spaces
    command,    ///< '+' or '.' first; a seat never runs a command
    too_soon,   ///< this seat said a line fewer than 30 ticks ago
    no_target,  ///< the mode or the chosen player is not one a seat may use
};

/// One listener for every computer seat on this machine.
///
/// `heard` is called with the seat, the sender, who the line was said to
/// and the words without the "<Name>" head. The words are valid only for
/// the call. Null hears nothing.
struct Listener {
    void* context{}; ///< passed to `heard`
    /// Hears one line. Null hears nothing.
    void (*heard)(
        void* context, uint8_t seat, uint8_t from, uint8_t mode, std::string_view words
    ){};
};

/// The listener, the pace and the queue of lines computer seats said.
class SeatChat {
  public:

    /// Empties the queue and forgets each seat's pace. The listener stays.
    void reset() noexcept;

    /// Replaces the listener. A null `heard` hears nothing.
    ///
    /// @param listener the listener
    void set_listener(const Listener& listener) noexcept;

    /// Queues a line for a computer seat, or refuses it.
    ///
    /// The seat must be in use with status computer. Bytes below 0x20 become
    /// spaces. An empty line, a command and a line within 30 ticks of this
    /// seat's last are refused. The mode is everyone, allies, enemies or one
    /// chosen player who is in use and is not the seat. The pace reads
    /// Game.tick.
    ///
    /// @param world the match, for the seats and the tick
    /// @param seat player slot
    /// @param text the words, without a "<Name>" head
    /// @param mode everyone, allies, enemies or chosen
    /// @param target player slot for chosen; ignored for the other modes
    /// @return why the line was queued or refused
    [[nodiscard]] Said
    say(const World& world, uint8_t seat, std::string_view text, uint8_t mode, uint8_t target);

    /// Posts each queued line, in order, and hands it to the other seats.
    ///
    /// Each line is posted with post_seat_chat. The listener then hears the
    /// words for every other local computer seat that hears the line, and
    /// never for the speaker. A say during the call waits for the next flush.
    ///
    /// @param[in,out] world message log
    /// @param hooks sharing, recording, sound and panel services
    void flush(World& world, const oa::sim::messages::Hooks& hooks);

    /// Hands a line the local human typed to every local computer seat that hears it.
    ///
    /// Commands are skipped. The speaker does not hear its own line.
    ///
    /// @param world the match's players
    /// @param speaker slot of the player who typed the line
    /// @param words the words, without a "<Name>" head
    /// @param mode the line's chat mode
    /// @param targets chosen-player slots; null reaches nobody in chosen mode
    void hear_local(
        const World& world,
        uint8_t speaker,
        std::string_view words,
        uint8_t mode,
        const uint8_t* targets
    ) const;

    /// Hands a chat line that arrived from another machine to the seats it reached.
    ///
    /// The head is split off. A broadcast reaches every local computer seat
    /// with mode everyone. A line addressed to a local computer seat reaches
    /// that seat, with the mode the head's target names. A line addressed to
    /// the human reaches no seat. Commands are skipped. The words have no head.
    ///
    /// @param world the match's players
    /// @param from slot of the sender
    /// @param addressee slot the record was addressed to
    /// @param broadcast the record went to every player
    /// @param line the line as this machine shows it, head included
    void hear_remote(
        const World& world, uint8_t from, uint8_t addressee, bool broadcast, std::string_view line
    ) const;

    /// The lines waiting for the next flush.
    ///
    /// @return how many lines are queued
    [[nodiscard]] std::size_t pending() const noexcept;

  private:

    /// One line a seat said, waiting for the flush.
    struct Pending {
        uint8_t seat{};                     ///< slot of the speaking seat
        uint8_t mode{};                     ///< everyone, allies, enemies or chosen
        uint8_t targets[OA_PLAYER_COUNT]{}; ///< chosen-player slots
        std::string words{};                ///< cleaned words, leading spaces kept
    };

    std::vector<Pending> pending_{};
    uint32_t said_at_[OA_PLAYER_COUNT]{}; ///< Game.tick of each seat's last queued line
    bool said_[OA_PLAYER_COUNT]{};        ///< the seat has queued a line since the last reset
    Listener listener_{};
};

} // namespace oa::app::seat_chat
