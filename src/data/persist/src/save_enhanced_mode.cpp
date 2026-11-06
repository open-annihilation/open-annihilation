// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_enhanced_mode.hpp"
#include "oa/core/player.h"

#include <limits>
#include <tuple>

namespace oa::data::persist {

namespace {

/// The bits naming the ten player slots whose unit blocks a save can retain.
constexpr uint16_t all_unit_player_bits = (uint16_t{1} << OA_PLAYER_COUNT) - 1;

} // namespace

bool save_write_enhanced_mode(Bank* bank, uint32_t rules_version, uint16_t unit_player_mask) {
    if (bank == nullptr || bank->accounts == nullptr)
        return false;
    if (rules_version == 0)
        return true;
    if (rules_version > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        unit_player_mask > all_unit_player_bits)
        return false;
    std::ignore = bank_open_account(bank, enhanced_mode_key::account);
    if (!bank_set_int(
            bank, enhanced_mode_key::format_version, enhanced_mode_key::current_format_version
        ) ||
        !bank_set_int(bank, enhanced_mode_key::rules_version, static_cast<int32_t>(rules_version)))
        return false;
    return unit_player_mask == 0 ||
           bank_set_int(bank, enhanced_mode_key::unit_player_mask, unit_player_mask);
}

bool save_read_enhanced_mode(Bank* bank, uint32_t& rules_version, uint16_t* unit_player_mask) {
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
    rules_version = static_cast<uint32_t>(version);
    if (unit_player_mask != nullptr)
        *unit_player_mask = mask;
    return true;
}

} // namespace oa::data::persist
