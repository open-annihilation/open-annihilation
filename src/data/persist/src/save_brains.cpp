// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_brains.hpp"

#include <cstddef>
#include <cstdint>
#include <tuple>

namespace oa::data::persist {
namespace {

/// The name of one player's state blob, `State0` to `State9`.
///
/// @param player the player, 0 to 9
/// @param[out] name seven bytes, the name and a NUL
void state_name(uint8_t player, char name[7]) noexcept {
    name[0] = 'S';
    name[1] = 't';
    name[2] = 'a';
    name[3] = 't';
    name[4] = 'e';
    name[5] = static_cast<char>('0' + player);
    name[6] = '\0';
}

/// Tells whether the text and the states may be written.
///
/// @param seats_text the seats' text
/// @param states the states
/// @return false when the write must be refused
bool acceptable(std::string_view seats_text, std::span<const SavedBrainState> states) noexcept {
    if (seats_text.empty() || seats_text.size() > save_brain_seats_most_bytes)
        return false;
    bool used[10] = {};
    for (const SavedBrainState& state : states) {
        if (state.player > 9 || state.bytes.size() > save_brain_state_most_bytes)
            return false;
        if (used[state.player])
            return false;
        used[state.player] = true;
    }
    return true;
}

/// Writes one blob, replacing whatever it held.
///
/// @param[in,out] bank the save, with the Brains account open
/// @param name the blob's name, NUL-terminated
/// @param data the bytes
/// @param bytes how many
/// @return false when the blob cannot be written
bool write_blob(Bank* bank, const char* name, const void* data, uint32_t bytes) {
    if (bank_find_blob_name(bank, name, true) < 0)
        return false;
    bank_open_blob_name(bank, name);
    bank_blob_seek(bank, 0);
    return bank_blob_write(bank, data, bytes) == bytes;
}

} // namespace

bool save_write_brains(
    Bank* bank, std::string_view seats_text, std::span<const SavedBrainState> states
) {
    if (bank == nullptr || !acceptable(seats_text, states))
        return false;
    // The names are whole string literals, so data() is NUL-terminated.
    std::ignore = bank_open_account(bank, brains_key::account.data());
    const auto seats_bytes = static_cast<uint32_t>(seats_text.size());
    if (!write_blob(bank, brains_key::seats.data(), seats_text.data(), seats_bytes))
        return false;
    for (const SavedBrainState& state : states) {
        if (state.bytes.empty())
            continue;
        char name[7];
        state_name(state.player, name);
        const auto count = static_cast<uint32_t>(state.bytes.size());
        if (!write_blob(bank, name, state.bytes.data(), count))
            return false;
    }
    return true;
}

std::optional<SavedBrains> save_read_brains(Bank* bank) {
    if (bank == nullptr)
        return std::nullopt;
    // Opening an absent account creates an empty one, which reads as none.
    // An empty account is left out of a save's bytes.
    std::ignore = bank_open_account(bank, brains_key::account.data());
    if (bank_find_blob_name(bank, brains_key::seats.data(), false) < 0)
        return std::nullopt;
    if (!bank_open_blob_name(bank, brains_key::seats.data()))
        return std::nullopt;
    const int32_t seats_bytes = bank_blob_size(bank);
    if (seats_bytes <= 0 || static_cast<std::size_t>(seats_bytes) > save_brain_seats_most_bytes)
        return std::nullopt;
    SavedBrains loaded;
    loaded.seats_text.assign(static_cast<std::size_t>(seats_bytes), '\0');
    bank_blob_seek(bank, 0);
    const auto wanted = static_cast<uint32_t>(seats_bytes);
    if (bank_blob_read(bank, loaded.seats_text.data(), wanted) != wanted)
        return std::nullopt;
    for (uint8_t player = 0; player < 10; ++player) {
        char name[7];
        state_name(player, name);
        if (bank_find_blob_name(bank, name, false) < 0)
            continue;
        if (!bank_open_blob_name(bank, name))
            continue;
        const int32_t blob_bytes = bank_blob_size(bank);
        if (blob_bytes <= 0 || static_cast<std::size_t>(blob_bytes) > save_brain_state_most_bytes)
            continue;
        SavedBrainState state;
        state.player = player;
        state.bytes.resize(static_cast<std::size_t>(blob_bytes));
        bank_blob_seek(bank, 0);
        const auto count = static_cast<uint32_t>(blob_bytes);
        if (bank_blob_read(bank, state.bytes.data(), count) != count)
            continue;
        loaded.states.push_back(std::move(state));
    }
    return loaded;
}

} // namespace oa::data::persist
