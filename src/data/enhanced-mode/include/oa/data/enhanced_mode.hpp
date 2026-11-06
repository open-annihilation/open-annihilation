// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced Mode is a versioned match preset. Off leaves the chosen game's
// rules and hashes alone; an enabled preset records its version among the
// simulation rules. Graphics choices are not part of a preset.
#pragma once

#include "oa/data/mod_profile.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace oa::data::enhanced_mode {

/// The version used when Enhanced Mode is off.
inline constexpr uint32_t off_version = 0;
/// The Enhanced Mode preset supplied by this build.
inline constexpr uint32_t current_version = 1;

/// Tells whether this build can resolve a preset, including Off.
///
/// @param version the preset version, or off_version
/// @return true for a supported version
[[nodiscard]] bool supported_version(uint32_t version) noexcept;

/// Reads the Enhanced Mode version held by resolved match rules.
///
/// @param rules the resolved match rules
/// @return off_version when disabled, otherwise the recorded version
[[nodiscard]] uint32_t rules_version(const match_rules::MatchRules& rules) noexcept;

/// Resolves the chosen game's profile with a required Enhanced Mode preset.
///
/// Off delegates to the normal resolver unchanged, then refuses a profile
/// that enables Enhanced Mode itself. An enabled preset follows existing
/// match choices and must resolve completely at the requested version.
/// Version 1 permits 10000 units per participating player and 32767
/// projectiles. Rendering remains independent. Errors leave no usable resolution.
///
/// @param text the chosen game's profile bytes
/// @param source the name used by profile diagnostics
/// @param options the chosen game's settings, overrides and match choices
/// @param version the required preset version, or off_version
/// @return the resolved rules and hashes, or diagnostics
[[nodiscard]] mod_profile::ResolveResult resolve_profile(
    std::span<const uint8_t> text,
    std::string_view source,
    const mod_profile::ResolveOptions& options,
    uint32_t version
);

} // namespace oa::data::enhanced_mode
