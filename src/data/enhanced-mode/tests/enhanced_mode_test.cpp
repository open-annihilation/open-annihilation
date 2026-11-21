// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Preset identity, unchanged Off resolution, mod composition and strict versions.
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/test/check.hpp"

#include "oa/base/sha256.hpp"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace enhanced = oa::data::enhanced_mode;
namespace profiles = oa::data::mod_profile;

// The base profile keeps the released identity when Enhanced Mode is Off.
constexpr std::string_view off_sim_hash =
    "1502111e3b1f69bd93be99405e3c61e0c6337698a7c8502f9f6320e80600094d";
constexpr std::string_view off_canonical_hash =
    "56b8e36be0dc22beb516a81805d95cf71db2fe600db965a3ad3460baa681b246";
// Alias version 1.
constexpr std::string_view capacity_sim_hash =
    "984103bf6f7144fe70a2da98cbcaa7aea16ba560f3f8db21181ddc2df7f553b9";
constexpr std::string_view capacity_canonical_hash =
    "cc3c7b87ac002d41d9c7f14b15bc15cd9ddab833e05812aecff7c83f147ccded";
// Alias version 2.
constexpr std::string_view flow_sim_hash =
    "6c9c8ff02806d858c5e718831b964797746ffb55c11f48b29ff13576d514fc0f";
constexpr std::string_view flow_canonical_hash =
    "b5fa6346061b9ff5388637440d290c7c57b36e0df624450ac7bca23a474be6af";
// Alias version 3.
constexpr std::string_view regional_sim_hash =
    "b68fe61bb8fa9afd92a45fa610b76d53d6870c96e97d54a4cfee51f91d2dd93f";
constexpr std::string_view regional_canonical_hash =
    "1d35b1b69651ab0bc548f2d3b1ca08d75a3ddca217b446562e3d317437095dea";
// Alias version 4 and Revision 1 (provisional).
constexpr std::string_view swept_ground_sim_hash =
    "b32e84b1199bb1ed107045fa6dec7cd01ac18a04d840703aa80010cde04070c3";
constexpr std::string_view swept_ground_canonical_hash =
    "583220b3b1bbfc5885c752d10d19a286e554e7275b0da883f1450cb53621f182";
// Alias version 5.
constexpr std::string_view corridor_sim_hash =
    "b9578976b6a185978cf2f4516c9a61291dadbf161fcac0aca40bd60c1291cc59";
constexpr std::string_view corridor_canonical_hash =
    "26848ff342eb0c983eb434b18bbe42ecf00e77af7a3d1834480a4314c6543080";
// Alias version 6.
constexpr std::string_view battle_sim_hash =
    "4550a81d720330491f497f621ca99434ef42e3d5a2dd9857cbf786f761d58f25";
constexpr std::string_view battle_canonical_hash =
    "baef569fde900789fb7b0f2d36c56fc7cc29f1f124172d62267c8f4787372bdf";
// Alias version 7.
constexpr std::string_view distributed_sim_hash =
    "6aa585cdd1040fb9af5a15fe9e267a73650aff2172d916cf04d1b57a7f374e13";
constexpr std::string_view distributed_canonical_hash =
    "4e330a94107aaeacd5d8cc61ad844aeec0c4f521b3cfae62cc831a60a346e794";
// Alias version 8.
constexpr std::string_view following_sim_hash =
    "acbdd64b83397820b921f0f1538bedabd4a65b32c30a4dce763e127648784200";
constexpr std::string_view following_canonical_hash =
    "11fa9ccacec5129b8fd5587d1cb61f8d726230b344183009906d2968a501c1ff";
// Alias version 10.
constexpr std::string_view cohort_sim_hash =
    "30b03b2398acbe294b4db952f9ba23d96adeee84b7a41ee58546dc32177b1af2";
constexpr std::string_view cohort_canonical_hash =
    "80c0be5b26abc3e7350a2eb780934ca61e13eadfc3c7093e9bc945a6b4fbd859";
// Alias version 11.
constexpr std::string_view retention_sim_hash =
    "b7aef3438ac8de098441ce7da42da04f10c6fa8a60b724adfe755f78c8842f20";
constexpr std::string_view retention_canonical_hash =
    "e3a81e80d5519c5ed8ba3462242f807951b93db671a3df7c14a7b7a91ac23293";
// Alias version 13.
constexpr std::string_view cardinal_sim_hash =
    "1603020c788b7b4d78d658f101dd5173cd71bb7de18d80ff1a0eb325640da265";
constexpr std::string_view cardinal_canonical_hash =
    "ec2a6e0304f11dd6fe2d18615273775ab731e63060e08544ac7e20caf1102ea4";

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

/// Prints a digest that misses its pin, then checks it.
///
/// @param label the pin's name
/// @param digest the digest the profile produced
/// @param pinned the pinned hex text
void expect_digest(
    std::string_view label, const oa::base::sha256::Digest& digest, std::string_view pinned
) {
    const std::string text = profiles::digest_text(digest);
    if (text != pinned)
        std::cerr << label << ' ' << text << '\n';
    OA_CHECK(text == pinned);
}

/// The `"setup.enhanced-mode":{...}` object, key and value.
///
/// @param canonical a resolved profile's canonical JSON
/// @return that object, or empty when the profile leaves Enhanced Mode out
std::string_view enhanced_mode_object(std::string_view canonical) {
    constexpr std::string_view key = "\"setup.enhanced-mode\":";
    const auto at = canonical.find(key);
    if (at == std::string_view::npos)
        return {};
    const auto open = at + key.size();
    if (open >= canonical.size() || canonical[open] != '{')
        return {};
    int depth = 0;
    for (std::size_t index = open; index < canonical.size(); ++index) {
        if (canonical[index] == '{')
            ++depth;
        else if (canonical[index] == '}') {
            --depth;
            if (depth == 0)
                return canonical.substr(at, index + 1 - at);
        }
    }
    return {};
}

/// Tells whether two canonical profiles differ only in the Enhanced Mode object.
///
/// @param later the profile whose object is replaced
/// @param earlier the profile whose object is kept
/// @return true when that replacement makes the profiles identical
bool same_except_enhanced_mode(std::string later, std::string_view earlier) {
    const std::string_view later_object = enhanced_mode_object(later);
    const std::string_view earlier_object = enhanced_mode_object(earlier);
    if (later_object.empty() || earlier_object.empty()) {
        std::cerr << "enhanced-mode object missing\n";
        return false;
    }
    const auto at = static_cast<std::size_t>(later_object.data() - later.data());
    later.replace(at, later_object.size(), earlier_object);
    return later == earlier;
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
    expect_digest("capacity_sim_hash", previous.profile.sim_hash, capacity_sim_hash);
    expect_digest("capacity_canonical_hash", previous.profile.full_hash, capacity_canonical_hash);
    OA_CHECK(
        profiles::digest_text(oa::base::sha256::digest_of(bytes(previous.canonical))) ==
        capacity_canonical_hash
    );
    OA_CHECK(enhanced::rules_version(current.profile.rules) == enhanced::flow_version);
    expect_digest("flow_sim_hash", current.profile.sim_hash, flow_sim_hash);
    expect_digest("flow_canonical_hash", current.profile.full_hash, flow_canonical_hash);
    OA_CHECK(current.profile.sim_hash != previous.profile.sim_hash);
    OA_CHECK(current.profile.full_hash != previous.profile.full_hash);
    OA_CHECK(current.profile.limits == previous.profile.limits);
    auto earlier_rules = current.profile.rules;
    earlier_rules.setup.enhanced_mode.features =
        previous.profile.rules.setup.enhanced_mode.features;
    OA_CHECK(earlier_rules == previous.profile.rules);

    OA_CHECK(same_except_enhanced_mode(current.canonical, previous.canonical));
    const auto capacity_object = enhanced_mode_object(previous.canonical);
    // Names, in registry order: the hash follows the names, not the indexes.
    // Alias version 1.
    constexpr std::string_view capacity_rule =
        "\"setup.enhanced-mode\":{\"features\":[\"capacity\"],\"revision\":1}";
    if (capacity_object != capacity_rule)
        std::cerr << "capacity object " << capacity_object << '\n';
    OA_CHECK(capacity_object == capacity_rule);
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
    expect_digest("regional_sim_hash", regional.resolution->profile.sim_hash, regional_sim_hash);
    expect_digest(
        "regional_canonical_hash", regional.resolution->profile.full_hash, regional_canonical_hash
    );
    auto prior = regional.resolution->profile.rules;
    prior.setup.enhanced_mode.features =
        flow.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(prior == flow.resolution->profile.rules);
    OA_CHECK(same_except_enhanced_mode(regional.resolution->canonical, flow.resolution->canonical));
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
    expect_digest(
        "swept_ground_sim_hash", swept.resolution->profile.sim_hash, swept_ground_sim_hash
    );
    expect_digest(
        "swept_ground_canonical_hash",
        swept.resolution->profile.full_hash,
        swept_ground_canonical_hash
    );
    OA_CHECK(swept.resolution->profile.limits == regional.resolution->profile.limits);
    auto prior = swept.resolution->profile.rules;
    prior.setup.enhanced_mode.features =
        regional.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(prior == regional.resolution->profile.rules);
    OA_CHECK(
        same_except_enhanced_mode(swept.resolution->canonical, regional.resolution->canonical)
    );
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
    expect_digest("corridor_sim_hash", corridor.resolution->profile.sim_hash, corridor_sim_hash);
    expect_digest(
        "corridor_canonical_hash", corridor.resolution->profile.full_hash, corridor_canonical_hash
    );
    OA_CHECK(corridor.resolution->profile.limits == swept.resolution->profile.limits);
    auto prior = corridor.resolution->profile.rules;
    prior.setup.enhanced_mode.features =
        swept.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(prior == swept.resolution->profile.rules);
    OA_CHECK(
        same_except_enhanced_mode(corridor.resolution->canonical, swept.resolution->canonical)
    );
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
    expect_digest("battle_sim_hash", battle.resolution->profile.sim_hash, battle_sim_hash);
    expect_digest(
        "battle_canonical_hash", battle.resolution->profile.full_hash, battle_canonical_hash
    );
    OA_CHECK(battle.resolution->profile.limits == swept.resolution->profile.limits);
    auto prior = battle.resolution->profile.rules;
    prior.setup.enhanced_mode.features =
        swept.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(prior == swept.resolution->profile.rules);
    OA_CHECK(same_except_enhanced_mode(battle.resolution->canonical, swept.resolution->canonical));
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
    expect_digest(
        "distributed_sim_hash", distributed.resolution->profile.sim_hash, distributed_sim_hash
    );
    expect_digest(
        "distributed_canonical_hash",
        distributed.resolution->profile.full_hash,
        distributed_canonical_hash
    );
    OA_CHECK(distributed.resolution->profile.limits == battle.resolution->profile.limits);
    auto prior = distributed.resolution->profile.rules;
    prior.setup.enhanced_mode.features =
        battle.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(prior == battle.resolution->profile.rules);
    OA_CHECK(
        same_except_enhanced_mode(distributed.resolution->canonical, battle.resolution->canonical)
    );
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
    expect_digest("following_sim_hash", following.resolution->profile.sim_hash, following_sim_hash);
    expect_digest(
        "following_canonical_hash",
        following.resolution->profile.full_hash,
        following_canonical_hash
    );
    OA_CHECK(
        enhanced::rules_version(following.resolution->profile.rules) ==
        enhanced::following_flow_version
    );
    OA_CHECK(following.resolution->profile.sim_hash != distributed.resolution->profile.sim_hash);
    OA_CHECK(following.resolution->profile.full_hash != distributed.resolution->profile.full_hash);
    OA_CHECK(following.resolution->profile.limits == distributed.resolution->profile.limits);
    auto prior = following.resolution->profile.rules;
    prior.setup.enhanced_mode.features =
        distributed.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(prior == distributed.resolution->profile.rules);
    OA_CHECK(same_except_enhanced_mode(
        following.resolution->canonical, distributed.resolution->canonical
    ));
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
    expect_digest("cohort_sim_hash", cohort.resolution->profile.sim_hash, cohort_sim_hash);
    expect_digest(
        "cohort_canonical_hash", cohort.resolution->profile.full_hash, cohort_canonical_hash
    );
    OA_CHECK(
        enhanced::rules_version(cohort.resolution->profile.rules) == enhanced::cohort_target_version
    );
    OA_CHECK(cohort.resolution->profile.sim_hash != following.resolution->profile.sim_hash);
    OA_CHECK(cohort.resolution->profile.full_hash != following.resolution->profile.full_hash);
    OA_CHECK(cohort.resolution->profile.limits == following.resolution->profile.limits);
    auto earlier_rules = cohort.resolution->profile.rules;
    earlier_rules.setup.enhanced_mode.features =
        following.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(earlier_rules == following.resolution->profile.rules);
    OA_CHECK(
        same_except_enhanced_mode(cohort.resolution->canonical, following.resolution->canonical)
    );
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
    earlier_rules.setup.enhanced_mode.features =
        cohort.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(earlier_rules == cohort.resolution->profile.rules);
    expect_digest("cardinal_sim_hash", cardinal.resolution->profile.sim_hash, cardinal_sim_hash);
    expect_digest(
        "cardinal_canonical_hash", cardinal.resolution->profile.full_hash, cardinal_canonical_hash
    );
    OA_CHECK(
        same_except_enhanced_mode(cardinal.resolution->canonical, cohort.resolution->canonical)
    );
    const std::string profile =
        std::string{mod_text} +
        "  setup.enhanced-mode: {revision: 1, features: [capacity, local-flow, "
        "routes-fine-regions, indexed-searches, route-corner-advance, swept-ground-steps, "
        "patrol-goal-retention, ai-strike-retarget, squad-position-index, route-queue, "
        "group-destination-repair, distributed-arrivals, moving-corner-advance, "
        "local-throat-guide, credited-fallback-search, bounded-strike-recruits, "
        "ai-attack-retention, ai-rally-retention, local-following, occupancy-free-planning, "
        "cohort-targets, steering-projection-reuse, cardinal-queue-escape]}\n";
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
    earlier_rules.setup.enhanced_mode.features =
        cohort.resolution->profile.rules.setup.enhanced_mode.features;
    OA_CHECK(earlier_rules == cohort.resolution->profile.rules);
    expect_digest("retention_sim_hash", retained.resolution->profile.sim_hash, retention_sim_hash);
    expect_digest(
        "retention_canonical_hash", retained.resolution->profile.full_hash, retention_canonical_hash
    );
    OA_CHECK(
        same_except_enhanced_mode(retained.resolution->canonical, cohort.resolution->canonical)
    );
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
    const std::array<Row, 29> expected{{
        {1, "capacity", capacity, {}, {}},
        {2, "local_flow", simulation, ids({1}), {}},
        {3, "routes_original", simulation, ids({1, 2}), ids({4, 5})},
        {4, "routes_large_regions", simulation, ids({1, 2}), ids({3, 5})},
        {5, "routes_fine_regions", simulation, ids({1, 2}), ids({3, 4})},
        {6, "indexed_searches", capacity, ids({1}), ids({28})},
        {7, "route_corner_advance", simulation, ids({2}), {}},
        {8, "swept_ground_steps", simulation, ids({1}), {}},
        {9, "corridor_lanes", simulation, ids({2}), {}},
        {10, "patrol_goal_retention", simulation, ids({1}), {}},
        {11, "ai_strike_retarget", simulation, ids({1}), {}},
        {12, "squad_position_index", capacity, ids({1}), {}},
        {13, "route_queue", simulation, ids({2}), {}},
        {14, "group_destination_repair", orders, ids({1}), {}},
        {15, "distributed_arrivals", orders, ids({14}), {}},
        {16, "moving_corner_advance", simulation, ids({7}), {}},
        {17, "local_throat_guide", simulation, ids({2}), {}},
        {18, "credited_fallback_search", simulation, ids({2, 5}), {}},
        {19, "bounded_strike_recruits", simulation, ids({1}), ids({29})},
        {20, "ai_attack_retention", simulation, ids({11}), {}},
        {21, "ai_rally_retention", simulation, ids({11}), {}},
        {22, "local_following", simulation, ids({2, 13}), {}},
        {23, "occupancy_free_planning", simulation, ids({1}), {}},
        {24, "cohort_targets", simulation, ids({11}), {}},
        {25, "steering_projection_reuse", capacity, ids({2}), {}},
        {26, "cohort_retention", simulation, ids({24}), {}},
        {27, "cardinal_queue_escape", simulation, ids({2}), {}},
        {28, "exact_indexed_searches", capacity, ids({1}), ids({6})},
        {29, "wide_strike_recruits", simulation, ids({1}), ids({19})},
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
    OA_CHECK(enhanced::feature_name(static_cast<Feature>(30)).empty());
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
    OA_CHECK(enhanced::close_features(ids({26})) == ids({1, 11, 24, 26}));
    OA_CHECK(enhanced::close_features(ids({16, 22})) == ids({1, 2, 7, 13, 16, 22}));
    OA_CHECK(enhanced::close_features(ids({18})) == ids({1, 2, 5, 18}));
    OA_CHECK(enhanced::close_features(ids({15, 23})) == ids({1, 14, 15, 23}));

    // The three route builders exclude each other, both ways, the two
    // indexed searches exclude each other, so do the two strike recruit
    // bounds, and nothing else excludes.
    const FeatureSet builders = ids({3, 4, 5});
    const FeatureSet searches = ids({6, 28});
    const FeatureSet recruits = ids({19, 29});
    for (const enhanced::FeatureInfo& row : table) {
        if (builders.has(row.id)) {
            OA_CHECK(row.excludes == builders.without(row.id));
        } else if (searches.has(row.id)) {
            OA_CHECK(row.excludes == searches.without(row.id));
        } else if (recruits.has(row.id)) {
            OA_CHECK(row.excludes == recruits.without(row.id));
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
    OA_CHECK(
        reason(ids({1, 6, 28})) ==
        "indexed_searches cannot share a match with exact_indexed_searches"
    );
    OA_CHECK(
        reason(ids({1, 19, 29})) ==
        "bounded_strike_recruits cannot share a match with wide_strike_recruits"
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
        reason(ids({1}) | FeatureSet::from_bits(uint64_t{1} << 30)) ==
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

/// Checks that resolved rules play their version's features, and that the hashes differ.
void check_rules_features() {
    OA_CHECK(enhanced::features({}).empty());
    const auto text = profiles::base_game_profile_text();

    struct Held {
        uint32_t version;
        oa::base::sha256::Digest sim;
        oa::base::sha256::Digest canonical;
    };

    std::vector<Held> held;
    for (const uint32_t version : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u, 13u}) {
        const auto resolved = enhanced::resolve_profile(bytes(text), "base", {}, version);
        OA_CHECK(resolved.resolution.has_value());
        if (!resolved.resolution)
            continue;
        OA_CHECK(
            enhanced::features(resolved.resolution->profile.rules) ==
            enhanced::version_features(version)
        );
        held.push_back(
            Held{
                version,
                resolved.resolution->profile.sim_hash,
                resolved.resolution->profile.full_hash
            }
        );
    }
    for (std::size_t left = 0; left < held.size(); ++left) {
        for (std::size_t right = left + 1; right < held.size(); ++right) {
            if (held[left].sim == held[right].sim)
                std::cerr << "sim collision " << held[left].version << ' ' << held[right].version
                          << '\n';
            if (held[left].canonical == held[right].canonical)
                std::cerr << "canonical collision " << held[left].version << ' '
                          << held[right].version << '\n';
            OA_CHECK(held[left].sim != held[right].sim);
            OA_CHECK(held[left].canonical != held[right].canonical);
        }
    }
    // Revision 1 (provisional) is alias 4's set, so the two hashes are the same.
    const auto revision = enhanced::resolve_profile(
        bytes(text), "base", {}, enhanced::Choice{1, enhanced::revision_features(1)}
    );
    OA_CHECK(revision.resolution.has_value());
    const Held* alias_four = nullptr;
    for (const Held& pin : held)
        if (pin.version == enhanced::swept_ground_version)
            alias_four = &pin;
    OA_CHECK(alias_four != nullptr);
    if (revision.resolution && alias_four != nullptr) {
        const std::string revision_sim =
            profiles::digest_text(revision.resolution->profile.sim_hash);
        const std::string revision_canonical =
            profiles::digest_text(revision.resolution->profile.full_hash);
        const std::string alias_sim = profiles::digest_text(alias_four->sim);
        const std::string alias_canonical = profiles::digest_text(alias_four->canonical);
        if (revision_sim != alias_sim)
            std::cerr << "revision 1 sim " << revision_sim << '\n';
        if (revision_canonical != alias_canonical)
            std::cerr << "revision 1 canonical " << revision_canonical << '\n';
        OA_CHECK(revision_sim == alias_sim);
        OA_CHECK(revision_canonical == alias_canonical);
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
