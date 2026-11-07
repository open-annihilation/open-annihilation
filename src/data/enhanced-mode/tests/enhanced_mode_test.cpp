// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Preset identity, unchanged Off resolution, mod composition and strict versions.
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/test/check.hpp"

#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace enhanced = oa::data::enhanced_mode;
namespace profiles = oa::data::mod_profile;

// The base profile keeps the released identity when Enhanced Mode is Off.
constexpr std::string_view off_sim_hash =
    "1502111e3b1f69bd93be99405e3c61e0c6337698a7c8502f9f6320e80600094d";
constexpr std::string_view off_canonical_hash =
    "56b8e36be0dc22beb516a81805d95cf71db2fe600db965a3ad3460baa681b246";
// The version-1 base profile's simulation rules and complete canonical JSON.
// Later presets retain these values so old saves and recordings keep their rules.
constexpr std::string_view capacity_sim_hash =
    "13cff985db3bf81ac26152dadf25e95ff0dfe788f009b78a6d7b278981990e1b";
constexpr std::string_view capacity_canonical_hash =
    "274c2e562b1fb155ac2edf75a483218b1a28c7503f7b55ac8f4b5bda63d0493f";
// The version-2 identity remains independent of the current preset.
constexpr std::string_view flow_sim_hash =
    "2b7bb180e975630800fa1123c8dde85bd76952f644ec5ef74facc6cf4696bee3";
constexpr std::string_view flow_canonical_hash =
    "4289710df599269b47eda5874eb5ae071d21a1097005d1df7e50a2fc93a8fec9";
// Version 3 retains its routing, movement rules and canonical identity.
constexpr std::string_view regional_sim_hash =
    "1f2a8eb19c55aa46b2e620ac237a8d7f08e7307e0140f5b1b40bd8093cfaff69";
constexpr std::string_view regional_canonical_hash =
    "d6e68fa3490dc43f3fcafb02027c9db9f111e54880b56b7275c7659da503bcd6";

/// A chosen mod with its own unit capacity and unrelated gameplay rule.
constexpr std::string_view mod_text =
    "oamod: 1\n"
    "id: enhanced-example\n"
    "name: Chosen Mod\n"
    "version: \"1\"\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: Open Annihilation}\n"
    "packaging: {revision: 1, date: 2026-11-07, packager: Open Annihilation}\n"
    "limits:\n"
    "  units-per-player: {default: 1000, min: 20, max: 1500}\n"
    "hacks:\n"
    "  units.id-reuse-delay: 90\n";

/// Views profile text as the bytes the resolver reads.
std::span<const uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// Checks that Off preserves an existing profile, including both hashes.
void check_off_identity() {
    const profiles::ResolveOptions options{};
    for (const std::string& text : {profiles::base_game_profile_text(), std::string{mod_text}}) {
        const auto normal = profiles::resolve_profile(bytes(text), "chosen", options);
        const auto off =
            enhanced::resolve_profile(bytes(text), "chosen", options, enhanced::off_version);
        OA_CHECK(normal.resolution.has_value());
        OA_CHECK(off.resolution.has_value());
        if (!normal.resolution || !off.resolution)
            continue;
        OA_CHECK(off.resolution->canonical == normal.resolution->canonical);
        OA_CHECK(off.resolution->profile.sim_hash == normal.resolution->profile.sim_hash);
        OA_CHECK(off.resolution->profile.full_hash == normal.resolution->profile.full_hash);
        OA_CHECK(enhanced::rules_version(off.resolution->profile.rules) == enhanced::off_version);
    }
    const auto text = profiles::base_game_profile_text();
    const auto base = enhanced::resolve_profile(bytes(text), "base", {}, enhanced::off_version);
    OA_CHECK(base.resolution);
    if (base.resolution) {
        OA_CHECK(profiles::digest_text(base.resolution->profile.sim_hash) == off_sim_hash);
        OA_CHECK(profiles::digest_text(base.resolution->profile.full_hash) == off_canonical_hash);
    }
}

/// Checks that the old preset stays pinned and the new one has its own identity.
void check_versioned_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto capacity =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::capacity_version);
    const auto flow = enhanced::resolve_profile(bytes(text), "base", {}, enhanced::flow_version);
    OA_CHECK(capacity.resolution.has_value());
    OA_CHECK(flow.resolution.has_value());
    if (!capacity.resolution || !flow.resolution)
        return;
    const auto& previous = *capacity.resolution;
    const auto& current = *flow.resolution;
    OA_CHECK(enhanced::rules_version(previous.profile.rules) == enhanced::capacity_version);
    OA_CHECK(profiles::digest_text(previous.profile.sim_hash) == capacity_sim_hash);
    OA_CHECK(profiles::digest_text(previous.profile.full_hash) == capacity_canonical_hash);
    OA_CHECK(
        profiles::digest_text(oa::base::sha256::digest_of(bytes(previous.canonical))) ==
        capacity_canonical_hash
    );
    OA_CHECK(enhanced::rules_version(current.profile.rules) == enhanced::flow_version);
    OA_CHECK(profiles::digest_text(current.profile.sim_hash) == flow_sim_hash);
    OA_CHECK(profiles::digest_text(current.profile.full_hash) == flow_canonical_hash);
    OA_CHECK(current.profile.sim_hash != previous.profile.sim_hash);
    OA_CHECK(current.profile.full_hash != previous.profile.full_hash);
    OA_CHECK(current.profile.limits == previous.profile.limits);
    auto earlier_rules = current.profile.rules;
    earlier_rules.setup.enhanced_mode.version = previous.profile.rules.setup.enhanced_mode.version;
    OA_CHECK(earlier_rules == previous.profile.rules);

    auto earlier_canonical = current.canonical;
    constexpr std::string_view flow_rule = "\"setup.enhanced-mode\":{\"version\":2}";
    constexpr std::string_view capacity_rule = "\"setup.enhanced-mode\":{\"version\":1}";
    const auto version_at = earlier_canonical.find(flow_rule);
    OA_CHECK(version_at != std::string::npos);
    if (version_at != std::string::npos) {
        earlier_canonical.replace(version_at, flow_rule.size(), capacity_rule);
        OA_CHECK(earlier_canonical == previous.canonical);
    }
}

/// Gives regional routing a new identity without changing the earlier preset.
void check_regional_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto flow = enhanced::resolve_profile(bytes(text), "base", {}, enhanced::flow_version);
    const auto regional =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::regional_flow_version);
    OA_CHECK(flow.resolution && regional.resolution);
    if (!flow.resolution || !regional.resolution)
        return;
    OA_CHECK(
        enhanced::rules_version(regional.resolution->profile.rules) ==
        enhanced::regional_flow_version
    );
    OA_CHECK(regional.resolution->profile.sim_hash != flow.resolution->profile.sim_hash);
    OA_CHECK(profiles::digest_text(regional.resolution->profile.sim_hash) == regional_sim_hash);
    OA_CHECK(
        profiles::digest_text(regional.resolution->profile.full_hash) == regional_canonical_hash
    );
    auto prior = regional.resolution->profile.rules;
    prior.setup.enhanced_mode.version = enhanced::flow_version;
    OA_CHECK(prior == flow.resolution->profile.rules);
    auto canonical = regional.resolution->canonical;
    constexpr std::string_view regional_rule = "\"setup.enhanced-mode\":{\"version\":3}";
    constexpr std::string_view flow_rule = "\"setup.enhanced-mode\":{\"version\":2}";
    const auto version_at = canonical.find(regional_rule);
    OA_CHECK(version_at != std::string::npos);
    if (version_at != std::string::npos) {
        canonical.replace(version_at, regional_rule.size(), flow_rule);
        OA_CHECK(canonical == flow.resolution->canonical);
    }
}

/// Assigns swept ground checks their own identity without upgrading older rules.
void check_swept_ground_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto regional =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::regional_flow_version);
    const auto swept =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::swept_ground_version);
    OA_CHECK(regional.resolution && swept.resolution);
    if (!regional.resolution || !swept.resolution)
        return;
    OA_CHECK(enhanced::current_version == enhanced::swept_ground_version);
    OA_CHECK(
        enhanced::rules_version(swept.resolution->profile.rules) == enhanced::swept_ground_version
    );
    OA_CHECK(swept.resolution->profile.sim_hash != regional.resolution->profile.sim_hash);
    OA_CHECK(swept.resolution->profile.full_hash != regional.resolution->profile.full_hash);
    OA_CHECK(swept.resolution->profile.limits == regional.resolution->profile.limits);
    auto prior = swept.resolution->profile.rules;
    prior.setup.enhanced_mode.version = enhanced::regional_flow_version;
    OA_CHECK(prior == regional.resolution->profile.rules);
    auto canonical = swept.resolution->canonical;
    constexpr std::string_view swept_rule = "\"setup.enhanced-mode\":{\"version\":4}";
    constexpr std::string_view regional_rule = "\"setup.enhanced-mode\":{\"version\":3}";
    const auto version_at = canonical.find(swept_rule);
    OA_CHECK(version_at != std::string::npos);
    if (version_at != std::string::npos) {
        canonical.replace(version_at, swept_rule.size(), regional_rule);
        OA_CHECK(canonical == regional.resolution->canonical);
    }
}

/// Preserves the original version selected by a bare profile switch.
void check_profile_default_version() {
    const std::string text = std::string{mod_text} + "  setup.enhanced-mode: true\n";
    const auto profile = profiles::resolve_profile(bytes(text), "chosen", {});
    OA_CHECK(profile.resolution.has_value());
    if (profile.resolution)
        OA_CHECK(
            enhanced::rules_version(profile.resolution->profile.rules) == enhanced::capacity_version
        );
    const auto current =
        enhanced::resolve_profile(bytes(text), "chosen", {}, enhanced::current_version);
    OA_CHECK(current.resolution.has_value());
    if (current.resolution)
        OA_CHECK(
            enhanced::rules_version(current.resolution->profile.rules) == enhanced::current_version
        );
}

/// Checks that the preset preserves the mod and other host match choices.
void check_enabled_composition() {
    profiles::ResolveOptions options{};
    options.match.push_back({"limits.units-per-player.default", "1100", "host", false});
    const auto normal = profiles::resolve_profile(bytes(mod_text), "chosen", options);
    const auto first =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, enhanced::current_version);
    const auto second =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, enhanced::current_version);
    OA_CHECK(normal.resolution.has_value());
    OA_CHECK(first.resolution.has_value());
    OA_CHECK(second.resolution.has_value());
    if (!normal.resolution || !first.resolution || !second.resolution)
        return;
    const auto& profile = first.resolution->profile;
    OA_CHECK(profile.id == normal.resolution->profile.id);
    OA_CHECK(profile.name == "Chosen Mod");
    auto expected_limits = normal.resolution->profile.limits;
    expected_limits.units_per_player.maximum = oa::data::limits::highest_enhanced_units_per_player;
    OA_CHECK(profile.limits == expected_limits);
    OA_CHECK(profile.limits.units_per_player.default_limit == 1100);
    OA_CHECK(
        profile.rules.units.id_reuse_delay == normal.resolution->profile.rules.units.id_reuse_delay
    );
    OA_CHECK(enhanced::rules_version(profile.rules) == enhanced::current_version);
    OA_CHECK(profile.sim_hash != normal.resolution->profile.sim_hash);
    OA_CHECK(profile.sim_hash == second.resolution->profile.sim_hash);
    OA_CHECK(first.resolution->canonical == second.resolution->canonical);
    OA_CHECK(first.resolution->canonical.find("setup.enhanced-mode") != std::string::npos);
}

/// Checks that a large host choice survives the preset and is refused while Off.
void check_enhanced_capacity() {
    profiles::ResolveOptions options{};
    options.match.push_back({"limits.units-per-player.default", "10000", "host", true});
    const auto on =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, enhanced::current_version);
    OA_CHECK(on.resolution.has_value());
    if (on.resolution) {
        OA_CHECK(on.resolution->profile.limits.units_per_player.default_limit == 10000);
        OA_CHECK(on.resolution->profile.limits.units_per_player.maximum == 10000);
        OA_CHECK(on.resolution->canonical.find("10000") != std::string::npos);
    }
    std::string large_profile{mod_text};
    const auto maximum = large_profile.find("max: 1500");
    OA_CHECK(maximum != std::string::npos);
    large_profile.replace(maximum, std::string_view{"max: 1500"}.size(), "max: 10000");
    const auto off =
        enhanced::resolve_profile(bytes(large_profile), "chosen", {}, enhanced::off_version);
    OA_CHECK(!off.resolution);
    OA_CHECK(!off.errors.empty());
    const auto large_on =
        enhanced::resolve_profile(bytes(large_profile), "chosen", {}, enhanced::current_version);
    OA_CHECK(large_on.resolution.has_value());
}

/// Checks strict version and opt-in validation at the profile boundary.
void check_refusals() {
    const profiles::ResolveOptions options{};
    for (const uint32_t version :
         {enhanced::current_version + 1, std::numeric_limits<uint32_t>::max()}) {
        const auto result = enhanced::resolve_profile(bytes(mod_text), "chosen", options, version);
        OA_CHECK(!result.resolution);
        OA_CHECK(!result.errors.empty());
    }
    const std::string requires_enhanced = std::string{mod_text} + "  setup.enhanced-mode: true\n";
    const auto off = enhanced::resolve_profile(
        bytes(requires_enhanced), "chosen", options, enhanced::off_version
    );
    OA_CHECK(!off.resolution);
    OA_CHECK(!off.errors.empty());
    const auto on = enhanced::resolve_profile(
        bytes(requires_enhanced), "chosen", options, enhanced::current_version
    );
    OA_CHECK(on.resolution.has_value());
    const std::string unsupported = std::string{mod_text} + "  setup.enhanced-mode: 5\n";
    const auto bad_profile =
        enhanced::resolve_profile(bytes(unsupported), "chosen", options, enhanced::current_version);
    OA_CHECK(!bad_profile.resolution);
    OA_CHECK(!bad_profile.errors.empty());
}

/// Checks that a developer override cannot silently disable the required preset.
void check_required_version() {
    profiles::ResolveOptions options{};
    profiles::HackOverride off;
    off.hack = "setup.enhanced-mode";
    off.on = false;
    options.overrides.push_back(off);
    const auto result =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, enhanced::current_version);
    OA_CHECK(result.resolution.has_value());
    if (result.resolution)
        OA_CHECK(
            enhanced::rules_version(result.resolution->profile.rules) == enhanced::current_version
        );
    OA_CHECK(options.match.empty());
    OA_CHECK(options.overrides.size() == 1);
    OA_CHECK(!options.overrides.front().on);
}

} // namespace

int main() {
    OA_CHECK(enhanced::supported_version(enhanced::off_version));
    OA_CHECK(enhanced::supported_version(enhanced::capacity_version));
    OA_CHECK(enhanced::supported_version(enhanced::flow_version));
    OA_CHECK(enhanced::supported_version(enhanced::regional_flow_version));
    OA_CHECK(enhanced::supported_version(enhanced::swept_ground_version));
    OA_CHECK(enhanced::supported_version(enhanced::current_version));
    OA_CHECK(!enhanced::supported_version(enhanced::current_version + 1));
    OA_CHECK(enhanced::rules_version({}) == enhanced::off_version);
    check_off_identity();
    check_versioned_identity();
    check_regional_identity();
    check_swept_ground_identity();
    check_profile_default_version();
    check_enabled_composition();
    check_enhanced_capacity();
    check_refusals();
    check_required_version();
    return oa::test::check_exit_status();
}
