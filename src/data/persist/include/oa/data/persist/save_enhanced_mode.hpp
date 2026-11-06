// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The EnhancedMode account records the rules version a saved match uses.
// Games with Enhanced Mode off write no account, keeping their save bytes.
#pragma once

#include "oa/data/persist/hapibank.hpp"

#include <cstdint>

namespace oa::data::persist {

namespace enhanced_mode_key {
inline constexpr const char* account = "EnhancedMode";
inline constexpr const char* format_version = "Format Version";
inline constexpr const char* rules_version = "Rules Version";
inline constexpr int32_t current_format_version = 1;
} // namespace enhanced_mode_key

/// Writes the EnhancedMode account for a nonzero rules version.
///
/// Version zero succeeds without changing the bank. The caller validates
/// whether the engine supports the rules version before writing it.
///
/// @param[in,out] bank the save being written
/// @param rules_version the match's rules version, zero when off
/// @return false for a missing bank, an unrepresentable version or a failed write
bool save_write_enhanced_mode(Bank* bank, uint32_t rules_version);

/// Reads the EnhancedMode account, rejecting malformed or unknown account formats.
///
/// A missing account gives version zero. A positive rules version is kept
/// for the caller to check against the engine's supported rules versions.
///
/// @param[in,out] bank a save read whole; the account is left open
/// @param[out] rules_version the recorded version, unchanged on failure
/// @return false when the account is malformed or its format is unsupported
[[nodiscard]] bool save_read_enhanced_mode(Bank* bank, uint32_t& rules_version);

} // namespace oa::data::persist
