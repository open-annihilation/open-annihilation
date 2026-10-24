// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Brains account of a savegame: the seats' text and each seat's saved
// state, kept as named blobs. The text and the bytes are stored as given
// and are not read here. A save written without calling for the account has
// no such account, so its bytes are unchanged.
#pragma once

#include "oa/data/persist/hapibank.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::persist {

/// Account and blob names of the Brains account.
namespace brains_key {
/// The account a save uses for the computer players.
inline constexpr std::string_view account = "Brains";
/// The blob that holds the seats' text.
inline constexpr std::string_view seats = "Seats";
/// The start of a state blob's name. The player's digit follows, `State0` to `State9`.
inline constexpr std::string_view state_prefix = "State";
} // namespace brains_key

/// Most bytes the seats' text may hold. A longer text is refused on write and
/// reads as none.
inline constexpr std::size_t save_brain_seats_most_bytes = 16 * 1024;

/// Most bytes one seat's state may hold. A longer state is refused on write.
/// A longer blob is skipped on read.
inline constexpr std::size_t save_brain_state_most_bytes = 1024 * 1024;

/// One seat's saved state. `player` is the engine index, 0 to 9.
struct SavedBrainState {
    uint8_t player{};             ///< the player the state belongs to
    std::vector<uint8_t> bytes{}; ///< the state's bytes
};

/// The Brains account as it was stored.
struct SavedBrains {
    std::string seats_text{};              ///< the seats' text, as stored
    std::vector<SavedBrainState> states{}; ///< one state per player that had one, in player order
};

/// Writes the Brains account: the seats' text and each seat's state.
///
/// An empty text, a text longer than save_brain_seats_most_bytes, a player
/// above 9, two states for one player, or a state longer than
/// save_brain_state_most_bytes is refused, and the bank is left as it was.
/// A state with no bytes is left out and is not a refusal. Each state's blob
/// is named `State` and the player's digit.
///
/// @param[in,out] bank the save being written; the account is left open
/// @param seats_text the seats, as text
/// @param states the states, one per player that has one
/// @return false when the text or a state is refused, or a blob cannot be written
bool save_write_brains(
    Bank* bank, std::string_view seats_text, std::span<const SavedBrainState> states
);

/// Reads the Brains account.
///
/// A save with no such account, or no Seats blob, or a Seats blob that is
/// empty or over save_brain_seats_most_bytes, reads as none. A state blob
/// over save_brain_state_most_bytes, or of a name other than `State0` to
/// `State9`, is skipped. A blob's size is checked before its bytes are copied.
/// States are returned in player order.
///
/// @param[in,out] bank a save read whole; the account is left open when present
/// @return the seats and their states, or nullopt when the save has none
[[nodiscard]] std::optional<SavedBrains> save_read_brains(Bank* bank);

} // namespace oa::data::persist
