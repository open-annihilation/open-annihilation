// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The EnhancedMode account records the revision and feature ids a saved
// match uses, and the player slots its unit pool was allocated for.
// Games with Enhanced Mode off write no account, keeping their save bytes.
#pragma once

#include "oa/data/persist/hapibank.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::data::persist {

namespace enhanced_mode_key {
inline constexpr const char* account = "EnhancedMode";
inline constexpr const char* format_version = "Format Version";
inline constexpr const char* revision = "Revision";
inline constexpr const char* features = "Features";
inline constexpr const char* needs_version = "Needs Version";
inline constexpr const char* unit_player_mask = "Unit Player Mask";
inline constexpr const char* unit_player_order = "Unit Player Order";
inline constexpr int32_t current_format_version = 2;
} // namespace enhanced_mode_key

/// One Enhanced Mode account. Feature ids are decimals, never names.
///
/// A writer lists the ids in id order with no spaces. A reader accepts any
/// order. The persist layer does not know which ids or revisions this build
/// can play; the application refuses those.
struct SavedEnhancedMode {
    uint32_t revision{};                 ///< 0 is Off and writes no account
    std::vector<uint32_t> feature_ids{}; ///< feature ids, in any order when read
    std::string needs_version{};         ///< stored engine version, or empty when the file has none
    uint16_t unit_player_mask{};         ///< original unit-pool slots, bits 0 to 9, or zero
    std::vector<uint8_t> order{};        ///< physical unit-block order
};

/// Writes the EnhancedMode account for a nonzero revision.
///
/// Revision zero succeeds without changing the bank. The caller validates
/// whether the engine can play the revision and the ids before writing them.
/// Feature ids are written in id order, comma-separated, with no spaces.
/// An empty needs version omits that field, including one an earlier write
/// stored. The optional mask records the original allocation, including
/// players that have since left or been defeated. The order gives their
/// physical unit-block order; when empty, the writer uses ascending player
/// slots. A zero mask leaves the allocation fields alone and requires an
/// empty order.
///
/// @param[in,out] bank the save being written
/// @param saved the revision, feature ids, needed version, mask and order
/// @return false for a missing bank, invalid metadata or a failed write
bool save_write_enhanced_mode(Bank* bank, const SavedEnhancedMode& saved);

/// Reads the EnhancedMode account, rejecting malformed or unknown account formats.
///
/// A missing account is Off: revision zero, no ids and no needed version.
/// A format-1 account is unreadable. A positive revision is kept for the
/// caller to check. Missing feature text is an empty id list. A missing or
/// empty needed version is an empty string. A missing mask gives zero for
/// saves written before allocation masks were recorded. A present mask must
/// name at least one of player slots 0 to 9. A mask without an order uses
/// ascending player slots. A recorded order must name each mask bit exactly
/// once. Failure leaves `saved` unchanged.
///
/// @param[in,out] bank a save read whole; the account is left open
/// @param[out] saved the recorded choice and allocation; unchanged on failure
/// @return false when the account is malformed or its format is unsupported
[[nodiscard]] bool save_read_enhanced_mode(Bank* bank, SavedEnhancedMode& saved);

} // namespace oa::data::persist
