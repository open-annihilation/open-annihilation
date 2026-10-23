// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat a seated brain hears at its turn, and the line it may say.
#pragma once

#include <cstdint>
#include <string_view>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

/// How the match sends a line a brain asked to say.
///
/// `say` returns the seat-chat answer as a number: 0 queued, 1 not a seat,
/// 2 empty, 3 a command, 4 too soon, 5 no target. Any other number means
/// chat is not connected. The words last only for the call. A null `say`
/// means the brain cannot say a line.
struct ChatHooks {
    /// Passed to `say`.
    void* context{};
    /// Queues one line. Null when chat is not connected.
    uint8_t (*say)(
        void* context, uint8_t seat, std::string_view text, uint8_t mode, uint8_t target
    ){};
};

/// Remembers how a seated brain says a line.
///
/// Replaces any hooks already stored. The hooks last until the match ends.
/// With no `say`, a brain that says a line is told chat is not connected.
///
/// @param match the match
/// @param hooks the say function; a null `say` leaves chat unconnected
void set_chat_hooks(match_runtime::Match& match, ChatHooks hooks) noexcept;

/// Queues one chat line for a seated brain.
///
/// A player with no brain is ignored. The line waits for that seat's next
/// turn, and a line that arrives while the turn's hooks run waits for the
/// turn after that. Nothing here runs a script. At most 32 lines wait for
/// one turn; a further line is dropped and counted. Words longer than 256
/// bytes are cut between whole characters.
///
/// @param match the match
/// @param seat the player the line reached, 0 to 9
/// @param from the sender's player index
/// @param mode everyone, allies, enemies or chosen
/// @param words the words, without a name head
void hear_chat(
    match_runtime::Match& match, uint8_t seat, uint8_t from, uint8_t mode, std::string_view words
) noexcept;

/// Returns how many chat lines this seat has dropped.
///
/// The count starts at 0 and grows when a turn already holds 32 lines. A
/// player with no brain has dropped none.
///
/// @param match the match
/// @param seat player index, 0 to 9
/// @return the dropped count, or 0 when that seat has no brain
[[nodiscard]] uint32_t dropped_chat_lines(const match_runtime::Match& match, uint8_t seat) noexcept;

} // namespace oa::sim::brains
