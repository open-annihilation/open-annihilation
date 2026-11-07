// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_enhanced_mode.hpp"
#include "oa/core/player.h"

#include <array>
#include <limits>
#include <new>
#include <tuple>

namespace oa::data::persist {

namespace {

/// The bits naming the ten player slots whose unit blocks a save can retain.
constexpr uint16_t all_unit_player_bits = (uint16_t{1} << OA_PLAYER_COUNT) - 1;

/// Lists the mask's player slots in ascending order.
///
/// @param mask the original unit-pool player slots
/// @param[out] slots space for every player slot
/// @return the number of slots written
std::size_t ascending_order(uint16_t mask, std::array<uint8_t, OA_PLAYER_COUNT>& slots) noexcept {
    std::size_t count{};
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if ((mask & (uint16_t{1} << slot)) != 0)
            slots[count++] = slot;
    return count;
}

/// Checks that an order is an exact permutation of the allocation mask.
///
/// @param order the physical unit-block order
/// @param mask the original unit-pool player slots
/// @return true when each mask bit occurs once and no other slot occurs
bool order_matches_mask(std::span<const uint8_t> order, uint16_t mask) noexcept {
    if (mask == 0 || order.empty() || order.size() > OA_PLAYER_COUNT)
        return false;
    uint16_t seen{};
    for (const uint8_t slot : order) {
        if (slot >= OA_PLAYER_COUNT)
            return false;
        const auto bit = static_cast<uint16_t>(uint16_t{1} << slot);
        if ((mask & bit) == 0 || (seen & bit) != 0)
            return false;
        seen |= bit;
    }
    return seen == mask;
}

} // namespace

bool save_write_enhanced_mode(
    Bank* bank, uint32_t rules_version, uint16_t unit_player_mask, std::span<const uint8_t> order
) {
    if (bank == nullptr || bank->accounts == nullptr)
        return false;
    if (rules_version == 0)
        return true;
    if (rules_version > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        unit_player_mask > all_unit_player_bits)
        return false;
    std::array<uint8_t, OA_PLAYER_COUNT> ascending{};
    if (unit_player_mask != 0 && order.empty())
        order = std::span<const uint8_t>{
            ascending.data(), ascending_order(unit_player_mask, ascending)
        };
    if (!order.empty() && !order_matches_mask(order, unit_player_mask))
        return false;
    std::array<char, OA_PLAYER_COUNT + 1> order_text{};
    for (std::size_t at = 0; at < order.size(); ++at)
        order_text[at] = static_cast<char>('0' + order[at]);
    std::ignore = bank_open_account(bank, enhanced_mode_key::account);
    if (!bank_set_int(
            bank, enhanced_mode_key::format_version, enhanced_mode_key::current_format_version
        ) ||
        !bank_set_int(bank, enhanced_mode_key::rules_version, static_cast<int32_t>(rules_version)))
        return false;
    return unit_player_mask == 0 ||
           (bank_set_int(bank, enhanced_mode_key::unit_player_mask, unit_player_mask) &&
            bank_set_text(bank, enhanced_mode_key::unit_player_order, order_text.data()));
}

bool save_read_enhanced_mode(
    Bank* bank, uint32_t& rules_version, uint16_t* unit_player_mask, std::vector<uint8_t>* order
) {
    if (bank == nullptr || bank->accounts == nullptr)
        return false;
    std::ignore = bank_open_account(bank, enhanced_mode_key::account);
    const auto& accounts = *bank->accounts;
    if (accounts.open < 0 || accounts.open >= accounts.count)
        return false;
    const auto& account = accounts.items[accounts.open];
    // An absent account is opened empty; empty accounts are never serialized.
    if (account.field_count == 0 && account.blob_count == 0) {
        rules_version = 0;
        if (unit_player_mask != nullptr)
            *unit_player_mask = 0;
        if (order != nullptr)
            order->clear();
        return true;
    }
    const int32_t format = bank_get_int(bank, enhanced_mode_key::format_version, -1);
    const int32_t version = bank_get_int(bank, enhanced_mode_key::rules_version, -1);
    if (format != enhanced_mode_key::current_format_version || version <= 0)
        return false;
    uint16_t mask{};
    if (bank_has_field(bank, enhanced_mode_key::unit_player_mask)) {
        const int32_t saved_mask = bank_get_int(bank, enhanced_mode_key::unit_player_mask, -1);
        if (saved_mask <= 0 || saved_mask > all_unit_player_bits)
            return false;
        mask = static_cast<uint16_t>(saved_mask);
    }
    std::array<uint8_t, OA_PLAYER_COUNT> slots{};
    std::size_t slot_count{};
    if (bank_has_field(bank, enhanced_mode_key::unit_player_order)) {
        const char* text = bank_get_text(bank, enhanced_mode_key::unit_player_order, nullptr);
        if (mask == 0 || text == nullptr)
            return false;
        while (slot_count < slots.size() && text[slot_count] != '\0') {
            if (text[slot_count] < '0' || text[slot_count] > '9')
                return false;
            slots[slot_count] = static_cast<uint8_t>(text[slot_count] - '0');
            ++slot_count;
        }
        if (text[slot_count] != '\0' ||
            !order_matches_mask(std::span<const uint8_t>{slots.data(), slot_count}, mask))
            return false;
    } else if (mask != 0) {
        slot_count = ascending_order(mask, slots);
    }
    if (order != nullptr) {
        try {
            std::vector<uint8_t> decoded(slots.begin(), slots.begin() + slot_count);
            order->swap(decoded);
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    rules_version = static_cast<uint32_t>(version);
    if (unit_player_mask != nullptr)
        *unit_player_mask = mask;
    return true;
}

} // namespace oa::data::persist
