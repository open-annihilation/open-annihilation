// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/enhanced_mode.hpp"

#include <string>
#include <utility>

namespace oa::data::enhanced_mode {
namespace {

/// The preset's required rule, which enters the canonical simulation hash.
constexpr std::string_view version_path = "setup.enhanced-mode.version";

/// Refuses a preset without handing partially resolved rules to the caller.
///
/// @param[in,out] result the result to refuse
/// @param source the profile name
/// @param message the reason the preset cannot be used
void refuse(mod_profile::ResolveResult& result, std::string_view source, std::string message) {
    result.resolution.reset();
    result.errors.push_back(
        {std::string{source}, {}, std::string{version_path}, std::move(message)}
    );
}

} // namespace

bool supported_version(uint32_t version) noexcept {
    return version == off_version || version == capacity_version || version == flow_version ||
           version == regional_flow_version || version == swept_ground_version ||
           version == corridor_flow_version || version == battle_flow_version ||
           uses_distributed_battle_flow(version);
}

uint32_t rules_version(const match_rules::MatchRules& rules) noexcept {
    return rules.setup.enhanced_mode.enabled
               ? static_cast<uint32_t>(rules.setup.enhanced_mode.version)
               : off_version;
}

mod_profile::ResolveResult resolve_profile(
    std::span<const uint8_t> text,
    std::string_view source,
    const mod_profile::ResolveOptions& options,
    uint32_t version
) {
    mod_profile::ResolveResult result;
    if (!supported_version(version)) {
        refuse(
            result, source, "unsupported Enhanced Mode rules version " + std::to_string(version)
        );
        return result;
    }
    if (version == off_version) {
        result = mod_profile::resolve_profile(text, source, options);
    } else {
        mod_profile::ResolveOptions enhanced = options;
        enhanced.match.push_back(
            {std::string{version_path}, std::to_string(version), "Enhanced Mode", true}
        );
        result = mod_profile::resolve_profile(text, source, enhanced);
    }
    if (!result.resolution)
        return result;
    const uint32_t resolved_version = rules_version(result.resolution->profile.rules);
    if (resolved_version != version) {
        refuse(
            result,
            source,
            version == off_version
                ? "the profile's enhanced rules require Enhanced Mode"
                : "the Enhanced Mode preset did not resolve at the required version"
        );
    }
    return result;
}

} // namespace oa::data::enhanced_mode
