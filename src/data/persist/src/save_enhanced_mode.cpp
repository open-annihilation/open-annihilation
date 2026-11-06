// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_enhanced_mode.hpp"

#include <limits>
#include <tuple>

namespace oa::data::persist {

bool save_write_enhanced_mode(Bank* bank, uint32_t rules_version) {
    if (bank == nullptr || bank->accounts == nullptr)
        return false;
    if (rules_version == 0)
        return true;
    if (rules_version > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return false;
    std::ignore = bank_open_account(bank, enhanced_mode_key::account);
    return bank_set_int(
               bank, enhanced_mode_key::format_version, enhanced_mode_key::current_format_version
           ) &&
           bank_set_int(
               bank, enhanced_mode_key::rules_version, static_cast<int32_t>(rules_version)
           );
}

bool save_read_enhanced_mode(Bank* bank, uint32_t& rules_version) {
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
        return true;
    }
    const int32_t format = bank_get_int(bank, enhanced_mode_key::format_version, -1);
    const int32_t version = bank_get_int(bank, enhanced_mode_key::rules_version, -1);
    if (format != enhanced_mode_key::current_format_version || version <= 0)
        return false;
    rules_version = static_cast<uint32_t>(version);
    return true;
}

} // namespace oa::data::persist
