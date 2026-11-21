// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Preset identity, unchanged Off resolution, mod composition and strict versions.
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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
// Versions 8 and 10 retain their original rules and complete canonical profiles.
constexpr std::string_view following_sim_hash =
    "713a68f3b5fe06431b65864aa0bf63ed0d9b87a14ced88b81df968f25c9c4ddc";
constexpr std::string_view following_canonical_hash =
    "0240ac7df34f56ff4361817488a11e957b3f3dba8b81ddd029b2084f5c080075";
constexpr std::string_view cohort_sim_hash =
    "eb041e276e60a34b69b2bb442bb760ecedc20f7bac82a23df5b06854a398653c";
constexpr std::string_view cohort_canonical_hash =
    "149a37363be305c9cbc9d0d503b3aeed8a757ab95dfe1d09a23c54f2fb6f97f3";

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
    OA_CHECK(profiles::digest_text(following.resolution->profile.sim_hash) == following_sim_hash);
    OA_CHECK(
        profiles::digest_text(following.resolution->profile.full_hash) == following_canonical_hash
    );
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
    OA_CHECK(profiles::digest_text(cohort.resolution->profile.sim_hash) == cohort_sim_hash);
    OA_CHECK(profiles::digest_text(cohort.resolution->profile.full_hash) == cohort_canonical_hash);
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
    for (uint32_t version = 0; version <= enhanced::cardinal_queue_escape_version + 1; ++version) {
        OA_CHECK(
            enhanced::uses_distributed_battle_flow(version) ==
            (version == 7 || version == 8 || version == 10 || version == 11 || version == 13)
        );
        OA_CHECK(
            enhanced::uses_following_battle_flow(version) ==
            (version == 8 || version == 10 || version == 11 || version == 13)
        );
        OA_CHECK(
            enhanced::uses_cohort_target_distribution(version) ==
            (version == 10 || version == 11 || version == 13)
        );
        OA_CHECK(enhanced::uses_cohort_target_retention(version) == (version == 11));
        OA_CHECK(enhanced::uses_cardinal_queue_escape(version) == (version == 13));
        OA_CHECK(
            enhanced::uses_enhanced_unit_rendering(version) ==
            (version == 10 || version == 11 || version == 13)
        );
    }
    for (const auto version : {9u, 12u, 14u, std::numeric_limits<uint32_t>::max()}) {
        OA_CHECK(!enhanced::uses_following_battle_flow(version));
        OA_CHECK(!enhanced::uses_cohort_target_distribution(version));
        OA_CHECK(!enhanced::uses_cohort_target_retention(version));
        OA_CHECK(!enhanced::uses_cardinal_queue_escape(version));
        OA_CHECK(!enhanced::uses_enhanced_unit_rendering(version));
    }
    // Raw profiles and match overrides use the same sparse version choices.
    for (const auto version : {9u, 12u, 14u}) {
        const std::string profile =
            std::string{mod_text} + "  setup.enhanced-mode: " + std::to_string(version) + "\n";
        OA_CHECK(!profiles::resolve_profile(bytes(profile), "unsupported", {}).resolution);
    }
}

/// Gives cardinal queue escape a distinct identity with version 10's static rules.
void check_cardinal_queue_escape_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto cohort =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::cohort_target_version);
    const auto cardinal =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::cardinal_queue_escape_version);
    const auto repeated =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::cardinal_queue_escape_version);
    OA_CHECK(cohort.resolution && cardinal.resolution && repeated.resolution);
    if (!cohort.resolution || !cardinal.resolution || !repeated.resolution)
        return;
    OA_CHECK(
        enhanced::rules_version(cardinal.resolution->profile.rules) ==
        enhanced::cardinal_queue_escape_version
    );
    OA_CHECK(cardinal.resolution->profile.sim_hash != cohort.resolution->profile.sim_hash);
    OA_CHECK(cardinal.resolution->profile.full_hash != cohort.resolution->profile.full_hash);
    OA_CHECK(cardinal.resolution->profile.limits == cohort.resolution->profile.limits);
    OA_CHECK(cardinal.resolution->canonical == repeated.resolution->canonical);
    OA_CHECK(cardinal.resolution->profile.sim_hash == repeated.resolution->profile.sim_hash);
    auto earlier_rules = cardinal.resolution->profile.rules;
    earlier_rules.setup.enhanced_mode.version = enhanced::cohort_target_version;
    OA_CHECK(earlier_rules == cohort.resolution->profile.rules);
    auto canonical = cardinal.resolution->canonical;
    constexpr std::string_view cardinal_rule = "\"setup.enhanced-mode\":{\"version\":13}";
    constexpr std::string_view cohort_rule = "\"setup.enhanced-mode\":{\"version\":10}";
    const auto at = canonical.find(cardinal_rule);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        canonical.replace(at, cardinal_rule.size(), cohort_rule);
        OA_CHECK(canonical == cohort.resolution->canonical);
    }
    const std::string profile = std::string{mod_text} + "  setup.enhanced-mode: 13\n";
    const auto raw = profiles::resolve_profile(bytes(profile), "cardinal", {});
    OA_CHECK(raw.resolution);
    if (raw.resolution)
        OA_CHECK(
            enhanced::rules_version(raw.resolution->profile.rules) ==
            enhanced::cardinal_queue_escape_version
        );
}

/// Gives cohort retention a distinct identity without changing the inherited static rules.
void check_cohort_retention_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto cohort =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::cohort_target_version);
    const auto retained =
        enhanced::resolve_profile(bytes(text), "base", {}, enhanced::cohort_retention_version);
    OA_CHECK(cohort.resolution && retained.resolution);
    if (!cohort.resolution || !retained.resolution)
        return;
    OA_CHECK(enhanced::rules_version(retained.resolution->profile.rules) == 11);
    OA_CHECK(retained.resolution->profile.sim_hash != cohort.resolution->profile.sim_hash);
    OA_CHECK(retained.resolution->profile.full_hash != cohort.resolution->profile.full_hash);
    OA_CHECK(retained.resolution->profile.limits == cohort.resolution->profile.limits);
    auto earlier_rules = retained.resolution->profile.rules;
    earlier_rules.setup.enhanced_mode.version = enhanced::cohort_target_version;
    OA_CHECK(earlier_rules == cohort.resolution->profile.rules);
    auto canonical = retained.resolution->canonical;
    constexpr std::string_view retained_rule = "\"setup.enhanced-mode\":{\"version\":11}";
    constexpr std::string_view cohort_rule = "\"setup.enhanced-mode\":{\"version\":10}";
    const auto at = canonical.find(retained_rule);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        canonical.replace(at, retained_rule.size(), cohort_rule);
        OA_CHECK(canonical == cohort.resolution->canonical);
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
          enhanced::cohort_retention_version + 1,
          enhanced::cardinal_queue_escape_version + 1,
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

using Feature = enhanced::Feature;
using FeatureKind = enhanced::FeatureKind;
using FeatureSet = enhanced::FeatureSet;

// A set answers from its bits alone, in constant expressions.
static_assert(FeatureSet{}.empty());
static_assert(FeatureSet{}.with(Feature::capacity).has(Feature::capacity));
static_assert(!FeatureSet{}.with(Feature::capacity).has(Feature::local_flow));
static_assert(FeatureSet{}.with(Feature::local_flow).bits() == 0b100);
static_assert(FeatureSet::from_bits(0b110).without(Feature::capacity).bits() == 0b100);

/// Makes a feature set from feature ids, as the feature table writes them.
///
/// @param id_list the features' ids
/// @return their set
FeatureSet ids(std::initializer_list<unsigned> id_list) {
    FeatureSet result;
    for (const unsigned id : id_list)
        result = result.with(static_cast<Feature>(id));
    return result;
}

/// Checks the union, difference and containment of sets.
void check_feature_set_operations() {
    const FeatureSet routes = ids({1, 2, 3});
    const FeatureSet flow = ids({1, 2});
    OA_CHECK((routes | ids({6})) == ids({1, 2, 3, 6}));
    OA_CHECK((routes - flow) == ids({3}));
    OA_CHECK((flow - routes).empty());
    OA_CHECK(routes.contains(flow));
    OA_CHECK(!flow.contains(routes));
    OA_CHECK(routes.contains(FeatureSet{}));
    OA_CHECK(FeatureSet::from_bits(routes.bits()) == routes);
    OA_CHECK(!(routes == flow));
}

/// Pins the feature table: ids, names, kinds, needs and exclusions never change.
void check_feature_table() {
    struct Row {
        unsigned id;
        std::string_view name;
        FeatureKind kind;
        FeatureSet needs;
        FeatureSet excludes;
    };

    constexpr FeatureKind capacity = FeatureKind::capacity;
    constexpr FeatureKind simulation = FeatureKind::simulation;
    constexpr FeatureKind orders = FeatureKind::orders;
    const std::array<Row, 27> expected{{
        {1, "capacity", capacity, {}, {}},
        {2, "local_flow", simulation, ids({1}), {}},
        {3, "routes_original", simulation, ids({1, 2}), ids({4, 5})},
        {4, "routes_large_regions", simulation, ids({1, 2}), ids({3, 5})},
        {5, "routes_fine_regions", simulation, ids({1, 2}), ids({3, 4})},
        {6, "indexed_searches", capacity, ids({1}), {}},
        {7, "route_corner_advance", simulation, ids({2}), {}},
        {8, "swept_ground_steps", simulation, ids({1}), {}},
        {9, "corridor_lanes", simulation, ids({2}), {}},
        {10, "patrol_goal_retention", simulation, ids({1}), {}},
        {11, "ai_strike_retarget", simulation, ids({1, 2}), {}},
        {12, "squad_position_index", capacity, ids({1}), {}},
        {13, "route_queue", simulation, ids({2}), {}},
        {14, "group_destination_repair", orders, ids({1}), {}},
        {15, "distributed_arrivals", orders, ids({14}), {}},
        {16, "moving_corner_advance", simulation, ids({7}), {}},
        {17, "local_throat_guide", simulation, ids({2}), {}},
        {18, "credited_fallback_search", simulation, ids({2, 5}), {}},
        {19, "bounded_strike_recruits", simulation, ids({1}), {}},
        {20, "ai_attack_retention", simulation, ids({11}), {}},
        {21, "ai_rally_retention", simulation, ids({11}), {}},
        {22, "local_following", simulation, ids({2, 13}), {}},
        {23, "occupancy_free_planning", simulation, ids({1}), {}},
        {24, "cohort_targets", simulation, ids({11}), {}},
        {25, "steering_projection_reuse", capacity, ids({2}), {}},
        {26, "cohort_retention", simulation, ids({24}), {}},
        {27, "cardinal_queue_escape", simulation, ids({2}), {}},
    }};
    const auto table = enhanced::feature_table();
    OA_CHECK(table.size() == expected.size());
    if (table.size() != expected.size())
        return;
    for (std::size_t index = 0; index < table.size(); ++index) {
        const enhanced::FeatureInfo& row = table[index];
        OA_CHECK(static_cast<unsigned>(row.id) == index + 1);
        OA_CHECK(static_cast<unsigned>(row.id) == expected[index].id);
        OA_CHECK(row.name == expected[index].name);
        OA_CHECK(row.kind == expected[index].kind);
        OA_CHECK(row.needs == expected[index].needs);
        OA_CHECK(row.excludes == expected[index].excludes);
        for (std::size_t later = index + 1; later < table.size(); ++later)
            OA_CHECK(row.name != table[later].name);
        // Every need has a lower id, so needs never form a loop.
        OA_CHECK((row.needs.bits() >> static_cast<unsigned>(row.id)) == 0);
        OA_CHECK(enhanced::feature_name(row.id) == row.name);
        OA_CHECK(enhanced::feature_by_name(enhanced::feature_name(row.id)) == row.id);
    }
    for (const std::string_view unknown :
         {"", "Capacity", "capacity ", "local-flow", "routes", "enhanced_mode"})
        OA_CHECK(!enhanced::feature_by_name(unknown));
    OA_CHECK(enhanced::feature_name(static_cast<Feature>(0)).empty());
    OA_CHECK(enhanced::feature_name(static_cast<Feature>(28)).empty());
    OA_CHECK(enhanced::feature_name(static_cast<Feature>(255)).empty());
}

/// Checks that closing a set adds what its features need, and only that.
void check_closing_features() {
    const auto table = enhanced::feature_table();
    for (const enhanced::FeatureInfo& row : table) {
        const FeatureSet closed = enhanced::close_features(FeatureSet{}.with(row.id));
        OA_CHECK(closed.has(row.id));
        OA_CHECK(closed.contains(row.needs));
        for (const enhanced::FeatureInfo& other : table)
            if (closed.has(other.id))
                OA_CHECK(closed.contains(other.needs));
        OA_CHECK(enhanced::close_features(closed) == closed);
        // Every chain of needs ends in capacity, so one feature closes to a playable set.
        OA_CHECK(closed.has(Feature::capacity));
        OA_CHECK(enhanced::feature_set_problem(closed) == nullptr);
    }
    OA_CHECK(enhanced::close_features({}).empty());
    OA_CHECK(enhanced::close_features(ids({26})) == ids({1, 2, 11, 24, 26}));
    OA_CHECK(enhanced::close_features(ids({16, 22})) == ids({1, 2, 7, 13, 16, 22}));
    OA_CHECK(enhanced::close_features(ids({18})) == ids({1, 2, 5, 18}));
    OA_CHECK(enhanced::close_features(ids({15, 23})) == ids({1, 14, 15, 23}));

    // The three route builders exclude each other, both ways, and nothing else excludes.
    const FeatureSet builders = ids({3, 4, 5});
    for (const enhanced::FeatureInfo& row : table) {
        if (builders.has(row.id)) {
            OA_CHECK(row.excludes == builders.without(row.id));
        } else {
            OA_CHECK(row.excludes.empty());
        }
        for (const enhanced::FeatureInfo& other : table)
            OA_CHECK(row.excludes.has(other.id) == other.excludes.has(row.id));
    }
}

/// Checks every old version's features against the alias table, written out.
void check_version_features() {
    const std::array<std::pair<uint32_t, FeatureSet>, 12> aliases{{
        {0, {}},
        {1, ids({1})},
        {2, ids({1, 2, 3, 6})},
        {3, ids({1, 2, 4, 6, 7})},
        {4, ids({1, 2, 4, 6, 7, 8})},
        {5, ids({1, 2, 4, 6, 7, 8, 9})},
        {6, ids({1, 2, 4, 6, 7, 8, 10, 11, 12, 13, 14})},
        {7, ids({1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21})},
        {8, ids({1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23})},
        {10,
         ids({1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25})},
        {11, ids({1,  2,  5,  6,  7,  8,  10, 11, 12, 13, 14, 15,
                  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26})},
        {13, ids({1,  2,  5,  6,  7,  8,  10, 11, 12, 13, 14, 15,
                  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27})},
    }};
    for (const auto& [version, expected] : aliases) {
        OA_CHECK(enhanced::supported_version(version));
        OA_CHECK(enhanced::version_features(version) == expected);
        // Complete and conflict-free.
        OA_CHECK(enhanced::feature_set_problem(expected) == nullptr);
        OA_CHECK(enhanced::close_features(expected) == expected);
    }
    for (const uint32_t version : {9u, 12u, 14u, 99u, std::numeric_limits<uint32_t>::max()}) {
        OA_CHECK(!enhanced::supported_version(version));
        OA_CHECK(enhanced::version_features(version).empty());
    }
    for (uint32_t version = 0; version <= 64; ++version)
        OA_CHECK(
            enhanced::version_features(version).empty() ==
            (version == enhanced::off_version || !enhanced::supported_version(version))
        );
}

/// Checks the reasons a set cannot be played.
void check_feature_set_problems() {
    OA_CHECK(enhanced::feature_set_problem({}) == nullptr);
    OA_CHECK(enhanced::feature_set_problem(ids({1})) == nullptr);
    const auto reason = [](FeatureSet set) {
        const char* problem = enhanced::feature_set_problem(set);
        return problem == nullptr ? std::string_view{} : std::string_view{problem};
    };
    OA_CHECK(
        reason(ids({1, 2, 3, 4})) ==
        "routes_original cannot share a match with routes_large_regions"
    );
    OA_CHECK(
        reason(ids({1, 2, 4, 5})) ==
        "routes_large_regions cannot share a match with routes_fine_regions"
    );
    OA_CHECK(reason(ids({1, 26})) == "cohort_retention needs cohort_targets");
    OA_CHECK(reason(ids({1, 3})) == "routes_original needs local_flow");
    OA_CHECK(reason(ids({1, 2, 18})) == "credited_fallback_search needs routes_fine_regions");
    OA_CHECK(reason(ids({2})) == "a set with any feature needs capacity");
    OA_CHECK(
        reason(ids({1, 2, 4, 6, 7, 8, 9}).without(Feature::capacity)) ==
        "a set with any feature needs capacity"
    );
    OA_CHECK(
        reason(FeatureSet::from_bits(0b1)) == "the set holds a feature this build does not know"
    );
    OA_CHECK(
        reason(ids({1}) | FeatureSet::from_bits(uint64_t{1} << 28)) ==
        "the set holds a feature this build does not know"
    );
}

/// Checks the revisions: Revision 1 is provisionally version 4's set.
void check_revision_features() {
    OA_CHECK(enhanced::highest_revision == 1);
    OA_CHECK(enhanced::revision_features(1) == enhanced::version_features(4));
    OA_CHECK(enhanced::revision_features(0).empty());
    OA_CHECK(enhanced::revision_features(2).empty());
    OA_CHECK(enhanced::revision_features(enhanced::highest_revision + 1).empty());
    OA_CHECK(enhanced::revision_features(std::numeric_limits<uint32_t>::max()).empty());
    for (uint32_t revision = 1; revision <= enhanced::highest_revision; ++revision) {
        OA_CHECK(!enhanced::revision_features(revision).empty());
        OA_CHECK(enhanced::feature_set_problem(enhanced::revision_features(revision)) == nullptr);
    }
}

/// Checks that resolved rules play their version's features.
void check_rules_features() {
    OA_CHECK(enhanced::features({}).empty());
    const auto text = profiles::base_game_profile_text();
    for (const uint32_t version : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 13u}) {
        const auto resolved = enhanced::resolve_profile(bytes(text), "base", {}, version);
        OA_CHECK(resolved.resolution.has_value());
        if (!resolved.resolution)
            continue;
        OA_CHECK(
            enhanced::features(resolved.resolution->profile.rules) ==
            enhanced::version_features(version)
        );
    }
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
    OA_CHECK(enhanced::supported_version(enhanced::cohort_retention_version));
    OA_CHECK(enhanced::supported_version(enhanced::cardinal_queue_escape_version));
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
    check_cohort_retention_identity();
    check_cardinal_queue_escape_identity();
    check_profile_default_version();
    check_enabled_composition();
    check_enhanced_capacity();
    check_refusals();
    check_required_version();
    check_feature_set_operations();
    check_feature_table();
    check_closing_features();
    check_version_features();
    check_feature_set_problems();
    check_revision_features();
    check_rules_features();
    return oa::test::check_exit_status();
}
