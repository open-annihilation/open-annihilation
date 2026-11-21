// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_enhanced_mode.hpp"
#include "oa/core/player.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace oa::data::persist {

namespace {

/// The bits naming the ten player slots whose unit blocks a save can retain.
constexpr uint16_t all_unit_player_bits = (uint16_t{1} << OA_PLAYER_COUNT) - 1;

/// Most feature ids one account lists. A sector holds 64 bits; the text
/// form can name the same ids and refuses a longer list as malformed.
constexpr std::size_t most_feature_ids = 64;

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

/// Writes feature ids in id order, with no spaces and no duplicates.
///
/// @param ids the ids, in any order
/// @param[out] text comma-separated decimals
/// @return false when an id is zero or repeated
bool features_text(const std::vector<uint32_t>& ids, std::string& text) {
    if (ids.size() > most_feature_ids)
        return false;
    std::vector<uint32_t> ordered = ids;
    std::sort(ordered.begin(), ordered.end());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (ordered[index] == 0)
            return false;
        if (index > 0 && ordered[index] == ordered[index - 1])
            return false;
    }
    text.clear();
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (index != 0)
            text.push_back(',');
        text += std::to_string(ordered[index]);
    }
    return true;
}

/// Reads comma-separated feature ids. Any order is kept. Spaces, names,
/// duplicates, a zero and a trailing comma are refused.
///
/// @param text the stored text; null or empty is an empty list
/// @param[out] ids the ids, in the stored order
/// @return false when the text is malformed
bool parse_features(const char* text, std::vector<uint32_t>& ids) {
    ids.clear();
    if (text == nullptr || text[0] == '\0')
        return true;
    const char* at = text;
    while (*at != '\0') {
        if (*at < '1' || *at > '9')
            return false;
        uint64_t value = 0;
        while (*at >= '0' && *at <= '9') {
            value = value * 10U + static_cast<uint64_t>(*at - '0');
            if (value > std::numeric_limits<uint32_t>::max())
                return false;
            ++at;
        }
        if (*at != '\0' && *at != ',')
            return false;
        if (ids.size() >= most_feature_ids)
            return false;
        ids.push_back(static_cast<uint32_t>(value));
        if (*at == ',') {
            ++at;
            if (*at == '\0')
                return false;
        }
    }
    std::vector<uint32_t> ordered = ids;
    std::sort(ordered.begin(), ordered.end());
    for (std::size_t index = 1; index < ordered.size(); ++index)
        if (ordered[index] == ordered[index - 1])
            return false;
    return true;
}

/// Drops one field of the open account, so an empty needed version is absent.
///
/// @param[in,out] bank bank with an open account
/// @param name field to remove when it exists
void drop_field(Bank* bank, const char* name) {
    const int32_t index = bank_find_field(bank, name, false);
    if (index < 0 || bank->accounts == nullptr)
        return;
    BankAccount& account = bank->accounts->items[bank->accounts->open];
    std::free(account.fields[index].name);
    if (account.fields[index].type == BankFieldType::text)
        std::free(account.fields[index].text);
    const int32_t last = account.field_count - 1;
    if (index < last)
        account.fields[index] = account.fields[last];
    account.field_count = last;
}

} // namespace

bool save_write_enhanced_mode(Bank* bank, const SavedEnhancedMode& saved) {
    if (bank == nullptr || bank->accounts == nullptr)
        return false;
    if (saved.revision == 0)
        return true;
    if (saved.revision > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        saved.unit_player_mask > all_unit_player_bits)
        return false;
    std::string features;
    if (!features_text(saved.feature_ids, features))
        return false;
    std::span<const uint8_t> order = saved.order;
    std::array<uint8_t, OA_PLAYER_COUNT> ascending{};
    if (saved.unit_player_mask != 0 && order.empty())
        order = std::span<const uint8_t>{
            ascending.data(), ascending_order(saved.unit_player_mask, ascending)
        };
    if (!order.empty() && !order_matches_mask(order, saved.unit_player_mask))
        return false;
    std::array<char, OA_PLAYER_COUNT + 1> order_text{};
    for (std::size_t at = 0; at < order.size(); ++at)
        order_text[at] = static_cast<char>('0' + order[at]);
    std::ignore = bank_open_account(bank, enhanced_mode_key::account);
    if (!bank_set_int(
            bank, enhanced_mode_key::format_version, enhanced_mode_key::current_format_version
        ) ||
        !bank_set_int(bank, enhanced_mode_key::revision, static_cast<int32_t>(saved.revision)) ||
        !bank_set_text(bank, enhanced_mode_key::features, features.c_str()))
        return false;
    if (saved.needs_version.empty())
        drop_field(bank, enhanced_mode_key::needs_version);
    else if (!bank_set_text(bank, enhanced_mode_key::needs_version, saved.needs_version.c_str()))
        return false;
    return saved.unit_player_mask == 0 ||
           (bank_set_int(bank, enhanced_mode_key::unit_player_mask, saved.unit_player_mask) &&
            bank_set_text(bank, enhanced_mode_key::unit_player_order, order_text.data()));
}

bool save_read_enhanced_mode(Bank* bank, SavedEnhancedMode& saved) {
    if (bank == nullptr || bank->accounts == nullptr)
        return false;
    std::ignore = bank_open_account(bank, enhanced_mode_key::account);
    const auto& accounts = *bank->accounts;
    if (accounts.open < 0 || accounts.open >= accounts.count)
        return false;
    const auto& account = accounts.items[accounts.open];
    // An absent account is opened empty; empty accounts are never serialized.
    if (account.field_count == 0 && account.blob_count == 0) {
        saved = {};
        return true;
    }
    const int32_t format = bank_get_int(bank, enhanced_mode_key::format_version, -1);
    const int32_t revision = bank_get_int(bank, enhanced_mode_key::revision, -1);
    if (format != enhanced_mode_key::current_format_version || revision <= 0)
        return false;
    std::vector<uint32_t> feature_ids;
    if (bank_has_field(bank, enhanced_mode_key::features)) {
        const char* text = bank_get_text(bank, enhanced_mode_key::features, nullptr);
        if (text == nullptr || !parse_features(text, feature_ids))
            return false;
    }
    std::string needs_version;
    if (bank_has_field(bank, enhanced_mode_key::needs_version)) {
        const char* text = bank_get_text(bank, enhanced_mode_key::needs_version, nullptr);
        if (text == nullptr)
            return false;
        try {
            needs_version = text;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
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
    std::vector<uint8_t> order;
    try {
        order.assign(slots.begin(), slots.begin() + static_cast<std::ptrdiff_t>(slot_count));
    } catch (const std::bad_alloc&) {
        return false;
    }
    saved.revision = static_cast<uint32_t>(revision);
    saved.feature_ids = std::move(feature_ids);
    saved.needs_version = std::move(needs_version);
    saved.unit_player_mask = mask;
    saved.order = std::move(order);
    return true;
}

} // namespace oa::data::persist
