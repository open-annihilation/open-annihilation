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
// Version 4 retains its swept-ground rules and canonical identity.
constexpr std::string_view swept_ground_sim_hash =
    "eb720332ee25a3e289956f795835d02f5c5f48da5cae2a5855454f6a47120235";
constexpr std::string_view swept_ground_canonical_hash =
    "fb94f560a48e47523c670a225e479db1f5cfc6a8550423c784a852ed8c9b7e0c";
// The explicit experimental identities retain their original canonical rules.
constexpr std::string_view corridor_sim_hash =
    "1d44e799141fb751b4aa9cd8e73d4d01c358dc3eb617a32033e4649cd91ecadf";
constexpr std::string_view corridor_canonical_hash =
    "fe2c864887e71d367381b3e0a38553410f56fe76c6538d8f439af03822ec974b";
constexpr std::string_view battle_sim_hash =
    "7fcd23aa103b68ef1f17fd5d51f038d546dbbd8c4beb361be16e2a52386668b7";
constexpr std::string_view battle_canonical_hash =
    "7b14c3bd16ba36480effbff1f8c494815d609578aaa9853ef5002036d2c6fd76";
constexpr std::string_view distributed_sim_hash =
    "afd8bfcd5bad8e5eb9ae9daee0df8cc007d65635d588c5e97f3bfb9f2b67d4dc";
constexpr std::string_view distributed_canonical_hash =
    "70ff3d27c4a6df5ddf94153fe59e6ecfd8d5fd1f4f9ed90534b4842fdca24ad7";

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
    OA_CHECK(
        enhanced::rules_version(swept.resolution->profile.rules) == enhanced::swept_ground_version
    );
    OA_CHECK(swept.resolution->profile.sim_hash != regional.resolution->profile.sim_hash);
    OA_CHECK(swept.resolution->profile.full_hash != regional.resolution->profile.full_hash);
    OA_CHECK(profiles::digest_text(swept.resolution->profile.sim_hash) == swept_ground_sim_hash);
    OA_CHECK(
        profiles::digest_text(swept.resolution->profile.full_hash) == swept_ground_canonical_hash
    );
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

/// Reserves corridor flow's identity without upgrading earlier ground rules.
void check_corridor_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto swept =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::swept_ground_version);
    const auto corridor =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::corridor_flow_version);
    OA_CHECK(swept.resolution && corridor.resolution);
    if (!swept.resolution || !corridor.resolution)
        return;
    OA_CHECK(enhanced::current_version == enhanced::swept_ground_version);
    OA_CHECK(
        enhanced::rules_version(corridor.resolution->profile.rules) ==
        enhanced::corridor_flow_version
    );
    OA_CHECK(corridor.resolution->profile.sim_hash != swept.resolution->profile.sim_hash);
    OA_CHECK(corridor.resolution->profile.full_hash != swept.resolution->profile.full_hash);
    OA_CHECK(profiles::digest_text(corridor.resolution->profile.sim_hash) == corridor_sim_hash);
    OA_CHECK(
        profiles::digest_text(corridor.resolution->profile.full_hash) == corridor_canonical_hash
    );
    OA_CHECK(corridor.resolution->profile.limits == swept.resolution->profile.limits);
    auto prior = corridor.resolution->profile.rules;
    prior.setup.enhanced_mode.version = enhanced::swept_ground_version;
    OA_CHECK(prior == swept.resolution->profile.rules);
    auto canonical = corridor.resolution->canonical;
    constexpr std::string_view corridor_rule = "\"setup.enhanced-mode\":{\"version\":5}";
    constexpr std::string_view swept_rule = "\"setup.enhanced-mode\":{\"version\":4}";
    const auto version_at = canonical.find(corridor_rule);
    OA_CHECK(version_at != std::string::npos);
    if (version_at != std::string::npos) {
        canonical.replace(version_at, corridor_rule.size(), swept_rule);
        OA_CHECK(canonical == swept.resolution->canonical);
    }
}

/// Keeps the battle-flow preset distinct without changing its predecessor's rules.
void check_battle_flow_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto swept =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::swept_ground_version);
    const auto battle =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::battle_flow_version);
    OA_CHECK(swept.resolution && battle.resolution);
    if (!swept.resolution || !battle.resolution)
        return;
    OA_CHECK(
        enhanced::rules_version(battle.resolution->profile.rules) == enhanced::battle_flow_version
    );
    OA_CHECK(battle.resolution->profile.sim_hash != swept.resolution->profile.sim_hash);
    OA_CHECK(battle.resolution->profile.full_hash != swept.resolution->profile.full_hash);
    OA_CHECK(profiles::digest_text(battle.resolution->profile.sim_hash) == battle_sim_hash);
    OA_CHECK(profiles::digest_text(battle.resolution->profile.full_hash) == battle_canonical_hash);
    OA_CHECK(battle.resolution->profile.limits == swept.resolution->profile.limits);
    auto prior = battle.resolution->profile.rules;
    prior.setup.enhanced_mode.version = enhanced::swept_ground_version;
    OA_CHECK(prior == swept.resolution->profile.rules);
    auto canonical = battle.resolution->canonical;
    constexpr std::string_view battle_rule = "\"setup.enhanced-mode\":{\"version\":6}";
    constexpr std::string_view swept_rule = "\"setup.enhanced-mode\":{\"version\":4}";
    const auto version_at = canonical.find(battle_rule);
    OA_CHECK(version_at != std::string::npos);
    if (version_at != std::string::npos) {
        canonical.replace(version_at, battle_rule.size(), swept_rule);
        OA_CHECK(canonical == swept.resolution->canonical);
    }
}

/// Keeps repaired-destination distribution separate from retained battle-flow rules.
void check_distributed_battle_flow_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto battle =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::battle_flow_version);
    const auto distributed = enhanced::resolve_profile(
        bytes(text), "base", {}, enhanced::distributed_battle_flow_version
    );
    OA_CHECK(battle.resolution && distributed.resolution);
    if (!battle.resolution || !distributed.resolution)
        return;
    OA_CHECK(
        enhanced::rules_version(distributed.resolution->profile.rules) ==
        enhanced::distributed_battle_flow_version
    );
    OA_CHECK(distributed.resolution->profile.sim_hash != battle.resolution->profile.sim_hash);
    OA_CHECK(distributed.resolution->profile.full_hash != battle.resolution->profile.full_hash);
    OA_CHECK(
        profiles::digest_text(distributed.resolution->profile.sim_hash) == distributed_sim_hash
    );
    OA_CHECK(
        profiles::digest_text(distributed.resolution->profile.full_hash) ==
        distributed_canonical_hash
    );
    OA_CHECK(distributed.resolution->profile.limits == battle.resolution->profile.limits);
    auto prior = distributed.resolution->profile.rules;
    prior.setup.enhanced_mode.version = enhanced::battle_flow_version;
    OA_CHECK(prior == battle.resolution->profile.rules);
    auto canonical = distributed.resolution->canonical;
    constexpr std::string_view distributed_rule = "\"setup.enhanced-mode\":{\"version\":7}";
    constexpr std::string_view battle_rule = "\"setup.enhanced-mode\":{\"version\":6}";
    const auto version_at = canonical.find(distributed_rule);
    OA_CHECK(version_at != std::string::npos);
    if (version_at != std::string::npos) {
        canonical.replace(version_at, distributed_rule.size(), battle_rule);
        OA_CHECK(canonical == battle.resolution->canonical);
    }
}

/// Following has a distinct identity and exactly the preceding static rules.
void check_following_flow_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto distributed = enhanced::resolve_profile(
        bytes(text), "base", {}, enhanced::distributed_battle_flow_version
    );
    const auto following =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::following_flow_version);
    OA_CHECK(distributed.resolution && following.resolution);
    if (!distributed.resolution || !following.resolution)
        return;
    OA_CHECK(
        enhanced::rules_version(following.resolution->profile.rules) ==
        enhanced::following_flow_version
    );
    OA_CHECK(following.resolution->profile.sim_hash != distributed.resolution->profile.sim_hash);
    OA_CHECK(following.resolution->profile.full_hash != distributed.resolution->profile.full_hash);
    OA_CHECK(following.resolution->profile.limits == distributed.resolution->profile.limits);
    auto prior = following.resolution->profile.rules;
    prior.setup.enhanced_mode.version = enhanced::distributed_battle_flow_version;
    OA_CHECK(prior == distributed.resolution->profile.rules);
    auto canonical = following.resolution->canonical;
    constexpr std::string_view following_rule = "\"setup.enhanced-mode\":{\"version\":8}";
    constexpr std::string_view distributed_rule = "\"setup.enhanced-mode\":{\"version\":7}";
    const auto at = canonical.find(following_rule);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        canonical.replace(at, following_rule.size(), distributed_rule);
        OA_CHECK(canonical == distributed.resolution->canonical);
    }
    OA_CHECK(enhanced::uses_distributed_battle_flow(enhanced::distributed_battle_flow_version));
    OA_CHECK(enhanced::uses_distributed_battle_flow(enhanced::following_flow_version));
    OA_CHECK(!enhanced::uses_distributed_battle_flow(enhanced::battle_flow_version));
    OA_CHECK(!enhanced::uses_distributed_battle_flow(enhanced::off_version));
    OA_CHECK(!enhanced::uses_distributed_battle_flow(enhanced::following_flow_version + 1));
    OA_CHECK(!enhanced::uses_distributed_battle_flow(std::numeric_limits<uint32_t>::max()));
}

/// Keeps cohort targeting separate while retaining every version-8 static rule.
void check_cohort_target_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto following =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::following_flow_version);
    const auto cohort =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::cohort_target_version);
    OA_CHECK(following.resolution && cohort.resolution);
    if (!following.resolution || !cohort.resolution)
        return;
    OA_CHECK(
        enhanced::rules_version(cohort.resolution->profile.rules) == enhanced::cohort_target_version
    );
    OA_CHECK(cohort.resolution->profile.sim_hash != following.resolution->profile.sim_hash);
    OA_CHECK(cohort.resolution->profile.full_hash != following.resolution->profile.full_hash);
    OA_CHECK(cohort.resolution->profile.limits == following.resolution->profile.limits);
    auto earlier_rules = cohort.resolution->profile.rules;
    earlier_rules.setup.enhanced_mode.version = enhanced::following_flow_version;
    OA_CHECK(earlier_rules == following.resolution->profile.rules);
    auto canonical = cohort.resolution->canonical;
    constexpr std::string_view cohort_rule = "\"setup.enhanced-mode\":{\"version\":10}";
    constexpr std::string_view following_rule = "\"setup.enhanced-mode\":{\"version\":8}";
    const auto at = canonical.find(cohort_rule);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        canonical.replace(at, cohort_rule.size(), following_rule);
        OA_CHECK(canonical == following.resolution->canonical);
    }
    for (uint32_t version = 0; version <= enhanced::cohort_target_version + 1; ++version) {
        OA_CHECK(
            enhanced::uses_distributed_battle_flow(version) ==
            (version == 7 || version == 8 || version == 10)
        );
        OA_CHECK(enhanced::uses_following_battle_flow(version) == (version == 8 || version == 10));
        OA_CHECK(enhanced::uses_cohort_target_distribution(version) == (version == 10));
    }
    for (const auto version : {9u, 11u, std::numeric_limits<uint32_t>::max()}) {
        OA_CHECK(!enhanced::uses_following_battle_flow(version));
        OA_CHECK(!enhanced::uses_cohort_target_distribution(version));
    }
    // Raw profiles and match overrides use the same sparse version choices.
    for (const auto version : {9u, 11u}) {
        const std::string profile =
            std::string{mod_text} + "  setup.enhanced-mode: " + std::to_string(version) + "\n";
        OA_CHECK(!profiles::resolve_profile(bytes(profile), "unsupported", {}).resolution);
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
         {enhanced::following_flow_version + 1,
          enhanced::cohort_target_version + 1,
          std::numeric_limits<uint32_t>::max()}) {
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
    const std::string unsupported = std::string{mod_text} + "  setup.enhanced-mode: 9\n";
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
    OA_CHECK(enhanced::supported_version(enhanced::corridor_flow_version));
    OA_CHECK(enhanced::supported_version(enhanced::battle_flow_version));
    OA_CHECK(enhanced::supported_version(enhanced::distributed_battle_flow_version));
    OA_CHECK(enhanced::supported_version(enhanced::following_flow_version));
    OA_CHECK(enhanced::supported_version(enhanced::cohort_target_version));
    OA_CHECK(enhanced::current_version == enhanced::swept_ground_version);
    OA_CHECK(enhanced::supported_version(enhanced::current_version));
    OA_CHECK(!enhanced::supported_version(enhanced::following_flow_version + 1));
    OA_CHECK(enhanced::rules_version({}) == enhanced::off_version);
    check_off_identity();
    check_versioned_identity();
    check_regional_identity();
    check_swept_ground_identity();
    check_corridor_identity();
    check_battle_flow_identity();
    check_distributed_battle_flow_identity();
    check_following_flow_identity();
    check_cohort_target_identity();
    check_profile_default_version();
    check_enabled_composition();
    check_enhanced_capacity();
    check_refusals();
    check_required_version();
    return oa::test::check_exit_status();
}
