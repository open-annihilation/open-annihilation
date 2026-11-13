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
/// The original large-battle capacity and contact-query preset.
inline constexpr uint32_t capacity_version = 1;
/// The preset adding shared routing and local unit flow.
inline constexpr uint32_t flow_version = 2;
/// The preset using regional shared-route construction and retained fields.
inline constexpr uint32_t regional_flow_version = 3;
/// The preset checking each swept ground step against current obstacles.
inline constexpr uint32_t swept_ground_version = 4;
/// The preset reserved for advisory corridor lanes.
inline constexpr uint32_t corridor_flow_version = 5;
/// The preset retaining patrol progress and responding to lost AI strike targets.
inline constexpr uint32_t battle_flow_version = 6;
/// The preset distributing repaired group destinations while retaining battle flow.
inline constexpr uint32_t distributed_battle_flow_version = 7;
/// The experimental preset for following an applied local movement leg.
inline constexpr uint32_t following_flow_version = 8;
/// The experimental preset distributing AI strike targets among spatial cohorts.
inline constexpr uint32_t cohort_target_version = 10;
/// The Enhanced Mode preset supplied by this build.
inline constexpr uint32_t current_version = swept_ground_version;

/// Selects the known presets inheriting distributed battle flow.
///
/// @param version the recorded preset version
/// @return true only for known presets retaining distributed battle flow
[[nodiscard]] inline constexpr bool uses_distributed_battle_flow(uint32_t version) noexcept {
    return version == distributed_battle_flow_version || version == following_flow_version ||
           version == cohort_target_version;
}

/// Selects the known presets following an applied local movement leg.
///
/// @param version the recorded preset version
/// @return true only for the following and cohort-target presets
[[nodiscard]] inline constexpr bool uses_following_battle_flow(uint32_t version) noexcept {
    return version == following_flow_version || version == cohort_target_version;
}

/// Selects deterministic AI strike-target distribution among spatial cohorts.
///
/// @param version the recorded preset version
/// @return true only for the explicit cohort-target preset
[[nodiscard]] inline constexpr bool uses_cohort_target_distribution(uint32_t version) noexcept {
    return version == cohort_target_version;
}

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
/// projectiles. Version 2 adds routing and unit-flow changes while retaining
/// those capacities. Version 3 selects regional shared-route construction.
/// Version 4 adds bounded obstacle checks along each ground step.
/// Version 5 reserves a separate identity for corridor flow when integrated.
/// Version 6 selects persistent patrol goals and bounded AI strike retargeting.
/// Version 7 retains battle flow and distributes repaired group destinations.
/// Version 8 retains version 7's static routing for experimental local following.
/// Version 10 inherits version 8 and distributes AI strike targets among spatial cohorts.
/// Version 9 is unsupported. Rendering remains independent. Errors leave no usable resolution.
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
