// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Identity of Off, capacity alone and Revision 1, unchanged Off resolution,
// mod composition, strict choices and the feature table.
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
// A set holding capacity alone, which a bare profile switch selects.
constexpr std::string_view capacity_sim_hash =
    "984103bf6f7144fe70a2da98cbcaa7aea16ba560f3f8db21181ddc2df7f553b9";
constexpr std::string_view capacity_canonical_hash =
    "cc3c7b87ac002d41d9c7f14b15bc15cd9ddab833e05812aecff7c83f147ccded";
// Revision 1.
constexpr std::string_view revision_1_sim_hash =
    "d0b69d572afc0bd51d0861f4ab133e9a70091eff872a038eba3474a080f92a4d";
constexpr std::string_view revision_1_canonical_hash =
    "dc07011ec1a623545c22759b3d53a26886cdbe0fd7f7afa56b2666ab3e4602bd";

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
        const auto off = enhanced::resolve_profile(bytes(text), "chosen", options, {});
        OA_CHECK(normal.resolution.has_value());
        OA_CHECK(off.resolution.has_value());
        if (!normal.resolution || !off.resolution)
            continue;
        OA_CHECK(off.resolution->canonical == normal.resolution->canonical);
        OA_CHECK(off.resolution->profile.sim_hash == normal.resolution->profile.sim_hash);
        OA_CHECK(off.resolution->profile.full_hash == normal.resolution->profile.full_hash);
        OA_CHECK(!off.resolution->profile.rules.setup.enhanced_mode.enabled);
        OA_CHECK(enhanced::features(off.resolution->profile.rules).empty());
    }
    const auto text = profiles::base_game_profile_text();
    const auto base = enhanced::resolve_profile(bytes(text), "base", {}, {});
    OA_CHECK(base.resolution);
    if (base.resolution) {
        OA_CHECK(profiles::digest_text(base.resolution->profile.sim_hash) == off_sim_hash);
        OA_CHECK(profiles::digest_text(base.resolution->profile.full_hash) == off_canonical_hash);
    }
}

/// The choice of capacity alone.
enhanced::Choice capacity_choice() {
    return {1, enhanced::FeatureSet{}.with(enhanced::Feature::capacity)};
}

/// The choice of Revision 1.
enhanced::Choice revision_1_choice() {
    return {1, enhanced::revision_features(1)};
}

/// Gives capacity alone its own pinned identity, apart from Revision 1's, with
/// the same static rules.
void check_capacity_identity() {
    const auto text = profiles::base_game_profile_text();
    const auto capacity = enhanced::resolve_profile(bytes(text), "base", {}, capacity_choice());
    const auto revision = enhanced::resolve_profile(bytes(text), "base", {}, revision_1_choice());
    OA_CHECK(capacity.resolution.has_value());
    OA_CHECK(revision.resolution.has_value());
    if (!capacity.resolution || !revision.resolution)
        return;
    const auto& previous = *capacity.resolution;
    const auto& current = *revision.resolution;
    OA_CHECK(enhanced::features(previous.profile.rules) == capacity_choice().features);
    expect_digest("capacity_sim_hash", previous.profile.sim_hash, capacity_sim_hash);
    expect_digest("capacity_canonical_hash", previous.profile.full_hash, capacity_canonical_hash);
    OA_CHECK(
        profiles::digest_text(oa::base::sha256::digest_of(bytes(previous.canonical))) ==
        capacity_canonical_hash
    );
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
    constexpr std::string_view capacity_rule =
        "\"setup.enhanced-mode\":{\"features\":[\"capacity\"],\"revision\":1}";
    if (capacity_object != capacity_rule)
        std::cerr << "capacity object " << capacity_object << '\n';
    OA_CHECK(capacity_object == capacity_rule);
}

/// Resolves a bare profile switch to capacity alone, and a new game to Revision 1.
void check_profile_default_set() {
    const std::string text = std::string{mod_text} + "  setup.enhanced-mode: true\n";
    const auto profile = profiles::resolve_profile(bytes(text), "chosen", {});
    OA_CHECK(profile.resolution.has_value());
    if (profile.resolution)
        OA_CHECK(
            enhanced::features(profile.resolution->profile.rules) == capacity_choice().features
        );
    const auto current = enhanced::resolve_profile(bytes(text), "chosen", {}, revision_1_choice());
    OA_CHECK(current.resolution.has_value());
    if (current.resolution)
        OA_CHECK(
            enhanced::features(current.resolution->profile.rules) == revision_1_choice().features
        );
}

/// Checks that the preset preserves the mod and other host match choices.
void check_enabled_composition() {
    profiles::ResolveOptions options{};
    options.match.push_back({"limits.units-per-player.default", "1100", "host", false});
    const auto normal = profiles::resolve_profile(bytes(mod_text), "chosen", options);
    const auto first =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, revision_1_choice());
    const auto second =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, revision_1_choice());
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
    OA_CHECK(enhanced::features(profile.rules) == revision_1_choice().features);
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
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, revision_1_choice());
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
    const auto off = enhanced::resolve_profile(bytes(large_profile), "chosen", {}, {});
    OA_CHECK(!off.resolution);
    OA_CHECK(!off.errors.empty());
    const auto large_on =
        enhanced::resolve_profile(bytes(large_profile), "chosen", {}, revision_1_choice());
    OA_CHECK(large_on.resolution.has_value());
}

/// Checks strict choices and opt-in validation at the profile boundary.
void check_refusals() {
    const profiles::ResolveOptions options{};
    const enhanced::FeatureSet capacity = capacity_choice().features;
    for (const enhanced::Choice& refused :
         {enhanced::Choice{2, revision_1_choice().features},
          enhanced::Choice{0, capacity},
          enhanced::Choice{1, capacity | enhanced::FeatureSet::from_bits(uint64_t{1} << 40)},
          enhanced::Choice{1, enhanced::FeatureSet{}.with(enhanced::Feature::local_flow)}}) {
        const auto result = enhanced::resolve_profile(bytes(mod_text), "chosen", options, refused);
        OA_CHECK(!result.resolution);
        OA_CHECK(!result.errors.empty());
    }
    const std::string requires_enhanced = std::string{mod_text} + "  setup.enhanced-mode: true\n";
    const auto off = enhanced::resolve_profile(bytes(requires_enhanced), "chosen", options, {});
    OA_CHECK(!off.resolution);
    OA_CHECK(!off.errors.empty());
    const auto on =
        enhanced::resolve_profile(bytes(requires_enhanced), "chosen", options, revision_1_choice());
    OA_CHECK(on.resolution.has_value());
    // A bare number does not choose a set of features in a profile.
    for (const auto number : {4u, 9u}) {
        const std::string numbered =
            std::string{mod_text} + "  setup.enhanced-mode: " + std::to_string(number) + "\n";
        OA_CHECK(!profiles::resolve_profile(bytes(numbered), "numbered", {}).resolution);
        const auto bad_profile =
            enhanced::resolve_profile(bytes(numbered), "chosen", options, revision_1_choice());
        OA_CHECK(!bad_profile.resolution);
        OA_CHECK(!bad_profile.errors.empty());
    }
}

/// Checks that a developer override cannot silently disable the required choice.
void check_required_choice() {
    profiles::ResolveOptions options{};
    profiles::HackOverride off;
    off.hack = "setup.enhanced-mode";
    off.on = false;
    options.overrides.push_back(off);
    const auto result =
        enhanced::resolve_profile(bytes(mod_text), "chosen", options, revision_1_choice());
    OA_CHECK(result.resolution.has_value());
    if (result.resolution)
        OA_CHECK(
            enhanced::features(result.resolution->profile.rules) == revision_1_choice().features
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
        bool withdrawn{};
    };

    constexpr FeatureKind capacity = FeatureKind::capacity;
    constexpr FeatureKind simulation = FeatureKind::simulation;
    constexpr FeatureKind orders = FeatureKind::orders;
    const std::array<Row, 34> expected{{
        {1, "capacity", capacity, {}, {}},
        {2, "local_flow", simulation, ids({1}), {}},
        {3, "routes_original", simulation, ids({1, 2}), ids({4, 5}), true},
        {4, "routes_large_regions", simulation, ids({1, 2}), ids({3, 5}), true},
        {5, "routes_fine_regions", simulation, ids({1, 2}), ids({3, 4})},
        {6, "indexed_searches", capacity, ids({1}), ids({28}), true},
        {7, "route_corner_advance", simulation, ids({2}), {}},
        {8, "swept_ground_steps", simulation, ids({1}), {}, true},
        {9, "corridor_lanes", simulation, ids({2}), {}, true},
        {10, "patrol_goal_retention", simulation, ids({1}), {}, true},
        {11, "ai_strike_retarget", simulation, ids({1}), {}},
        {12, "squad_position_index", capacity, ids({1}), {}, true},
        {13, "route_queue", simulation, ids({2}), {}, true},
        {14, "group_destination_repair", orders, ids({1}), {}, true},
        {15, "distributed_arrivals", orders, ids({14}), {}, true},
        {16, "moving_corner_advance", simulation, ids({7}), {}, true},
        {17, "local_throat_guide", simulation, ids({2}), {}, true},
        {18, "credited_fallback_search", simulation, ids({2, 5}), {}},
        {19, "bounded_strike_recruits", simulation, ids({1}), ids({29}), true},
        {20, "ai_attack_retention", simulation, ids({11}), {}},
        {21, "ai_rally_retention", simulation, ids({11}), {}},
        {22, "local_following", simulation, ids({2, 13}), {}, true},
        {23, "occupancy_free_planning", simulation, ids({1}), {}},
        {24, "cohort_targets", simulation, ids({11}), ids({30}), true},
        {25, "steering_projection_reuse", capacity, ids({2}), {}},
        {26, "cohort_retention", simulation, ids({24}), ids({31}), true},
        {27, "cardinal_queue_escape", simulation, ids({2}), ids({32}), true},
        {28, "exact_indexed_searches", capacity, ids({1}), ids({6})},
        {29, "wide_strike_recruits", simulation, ids({1}), ids({19})},
        {30, "paced_cohort_targets", simulation, ids({11}), ids({24})},
        {31, "dispatch_cohort_retention", simulation, ids({30}), ids({26})},
        {32, "counterflow_queue_escape", simulation, ids({2}), ids({27})},
        {33, "idle_goal_skip", capacity, ids({2}), {}},
        {34, "static_interior_refresh_skip", capacity, ids({23}), {}},
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
        OA_CHECK(row.withdrawn == expected[index].withdrawn);
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
    OA_CHECK(enhanced::feature_name(static_cast<Feature>(35)).empty());
    OA_CHECK(enhanced::feature_name(static_cast<Feature>(255)).empty());
}

/// The features no release plays, by id.
FeatureSet withdrawn_ids() {
    return ids({3, 4, 6, 8, 9, 10, 12, 13, 14, 15, 16, 17, 19, 22, 24, 26, 27});
}

/// Checks that closing a set adds what its features need, and only that,
/// and never adds a withdrawn feature.
void check_closing_features() {
    const auto table = enhanced::feature_table();
    for (const enhanced::FeatureInfo& row : table) {
        const FeatureSet closed = enhanced::close_features(FeatureSet{}.with(row.id));
        OA_CHECK(closed.has(row.id));
        OA_CHECK(enhanced::close_features(closed) == closed);
        for (const enhanced::FeatureInfo& other : table)
            if (other.withdrawn && other.id != row.id)
                OA_CHECK(!closed.has(other.id));
        if (row.withdrawn)
            continue;
        OA_CHECK(closed.contains(row.needs));
        for (const enhanced::FeatureInfo& other : table)
            if (closed.has(other.id))
                OA_CHECK(closed.contains(other.needs));
        // Every chain of needs ends in capacity, so one feature closes to a playable set.
        OA_CHECK(closed.has(Feature::capacity));
        OA_CHECK(enhanced::feature_set_problem(closed) == nullptr);
    }
    OA_CHECK(enhanced::close_features({}).empty());
    OA_CHECK(enhanced::close_features(ids({26})) == ids({26}));
    OA_CHECK(enhanced::close_features(ids({30})) == ids({1, 11, 30}));
    OA_CHECK(enhanced::close_features(ids({31})) == ids({1, 11, 30, 31}));
    OA_CHECK(enhanced::close_features(ids({32})) == ids({1, 2, 32}));
    OA_CHECK(
        enhanced::close_features(FeatureSet{}.with(Feature::idle_goal_skip)) ==
        ids({1, 2}).with(Feature::idle_goal_skip)
    );
    OA_CHECK(
        enhanced::close_features(FeatureSet{}.with(Feature::static_interior_refresh_skip)) ==
        ids({1, 23}).with(Feature::static_interior_refresh_skip)
    );
    OA_CHECK(enhanced::close_features(ids({16, 22})) == ids({1, 2, 7, 16, 22}));
    OA_CHECK(enhanced::close_features(ids({18})) == ids({1, 2, 5, 18}));
    OA_CHECK(enhanced::close_features(ids({15, 23})) == ids({1, 15, 23}));

    // The three route builders exclude each other, both ways, the two
    // indexed searches exclude each other, so do the two strike recruit
    // bounds, the two kinds of cohort targets, the two kinds of cohort
    // retention and the two cell escapes, and nothing else excludes.
    const FeatureSet builders = ids({3, 4, 5});
    const FeatureSet searches = ids({6, 28});
    const FeatureSet recruits = ids({19, 29});
    const FeatureSet cohorts = ids({24, 30});
    const FeatureSet retention = ids({26, 31});
    const FeatureSet escapes = ids({27, 32});
    for (const enhanced::FeatureInfo& row : table) {
        if (builders.has(row.id)) {
            OA_CHECK(row.excludes == builders.without(row.id));
        } else if (searches.has(row.id)) {
            OA_CHECK(row.excludes == searches.without(row.id));
        } else if (recruits.has(row.id)) {
            OA_CHECK(row.excludes == recruits.without(row.id));
        } else if (cohorts.has(row.id)) {
            OA_CHECK(row.excludes == cohorts.without(row.id));
        } else if (retention.has(row.id)) {
            OA_CHECK(row.excludes == retention.without(row.id));
        } else if (escapes.has(row.id)) {
            OA_CHECK(row.excludes == escapes.without(row.id));
        } else {
            OA_CHECK(row.excludes.empty());
        }
        for (const enhanced::FeatureInfo& other : table)
            OA_CHECK(row.excludes.has(other.id) == other.excludes.has(row.id));
    }
}

/// Checks the reasons a set cannot be played.
void check_feature_set_problems() {
    OA_CHECK(enhanced::feature_set_problem({}) == nullptr);
    OA_CHECK(enhanced::feature_set_problem(ids({1})) == nullptr);
    const auto reason = [](FeatureSet set) {
        const char* problem = enhanced::feature_set_problem(set);
        return problem == nullptr ? std::string_view{} : std::string_view{problem};
    };
    // A withdrawn feature is named before an exclusion or a missing need,
    // the first in id order.
    OA_CHECK(reason(ids({1, 2, 3, 4})) == "routes_original is withdrawn; no release plays it");
    OA_CHECK(reason(ids({1, 2, 4, 5})) == "routes_large_regions is withdrawn; no release plays it");
    OA_CHECK(reason(ids({1, 6, 28})) == "indexed_searches is withdrawn; no release plays it");
    OA_CHECK(
        reason(ids({1, 19, 29})) == "bounded_strike_recruits is withdrawn; no release plays it"
    );
    OA_CHECK(reason(ids({1, 11, 24, 30})) == "cohort_targets is withdrawn; no release plays it");
    OA_CHECK(reason(ids({1, 30})) == "paced_cohort_targets needs ai_strike_retarget");
    OA_CHECK(
        reason(ids({1, 2, 27, 32})) == "cardinal_queue_escape is withdrawn; no release plays it"
    );
    OA_CHECK(reason(ids({1, 32})) == "counterflow_queue_escape needs local_flow");
    OA_CHECK(reason(ids({1, 26})) == "cohort_retention is withdrawn; no release plays it");
    OA_CHECK(reason(ids({1, 11, 31})) == "dispatch_cohort_retention needs paced_cohort_targets");
    OA_CHECK(reason(ids({1, 11, 30, 31})) == "");
    OA_CHECK(reason(ids({1, 2, 18})) == "credited_fallback_search needs routes_fine_regions");
    OA_CHECK(reason(ids({2})) == "a set with any feature needs capacity");
    OA_CHECK(reason(ids({2, 5, 7})) == "a set with any feature needs capacity");
    OA_CHECK(
        reason(FeatureSet::from_bits(0b1)) == "the set holds a feature this build does not know"
    );
    OA_CHECK(
        reason(ids({1}) | FeatureSet::from_bits(uint64_t{1} << 35)) ==
        "the set holds a feature this build does not know"
    );
}

/// Checks that every feature Revision 1 left out is withdrawn: known, kept in
/// the table, and refused wherever a set is played, a save or a recording
/// included, as unusable rather than as a later game's.
void check_withdrawn_features() {
    FeatureSet withdrawn;
    for (const enhanced::FeatureInfo& row : enhanced::feature_table())
        if (row.withdrawn)
            withdrawn = withdrawn.with(row.id);
    OA_CHECK(withdrawn == withdrawn_ids());
    const FeatureSet revision_1 = enhanced::revision_features(1);
    for (uint32_t revision = 1; revision <= enhanced::highest_revision; ++revision)
        OA_CHECK(
            (enhanced::revision_features(revision) - withdrawn) ==
            enhanced::revision_features(revision)
        );
    const auto text = profiles::base_game_profile_text();
    for (const enhanced::FeatureInfo& row : enhanced::feature_table()) {
        if (!row.withdrawn)
            continue;
        const std::string expected = std::string(row.name) + " is withdrawn; no release plays it";
        OA_CHECK(enhanced::feature_by_name(row.name) == row.id);
        const FeatureSet asked = revision_1.with(row.id);
        const char* problem = enhanced::feature_set_problem(asked);
        OA_CHECK(problem != nullptr && problem == expected);
        const enhanced::Choice choice{1, asked};
        const char* refused = enhanced::choice_problem(choice);
        OA_CHECK(refused != nullptr && refused == expected);
        // Closing never adds it back, and never adds another withdrawn feature.
        const FeatureSet closed = enhanced::close_features(asked);
        OA_CHECK((closed - asked).empty());
        // A save account stores the ids, and a recording sector bit (id - 1).
        // Both read back to a choice this build knows, so neither is refused
        // with the needs-version sentence: both are refused as unusable.
        OA_CHECK(!enhanced::choice_lacks_support(choice));
        const auto sector =
            enhanced::features_from_sector_bits(enhanced::sector_feature_bits(asked));
        OA_CHECK(sector.has_value() && *sector == asked);
        if (sector) {
            const enhanced::Choice recorded{1, *sector};
            OA_CHECK(!enhanced::choice_lacks_support(recorded));
            const char* recording_problem = enhanced::choice_problem(recorded);
            OA_CHECK(recording_problem != nullptr && recording_problem == expected);
        }
        FeatureSet account;
        for (const enhanced::FeatureInfo& stored : enhanced::feature_table())
            if (asked.has(stored.id))
                account = account.with(static_cast<Feature>(static_cast<unsigned>(stored.id)));
        OA_CHECK(enhanced::choice_problem({1, account}) == refused);
        // No profile resolves it either.
        const auto resolved = enhanced::resolve_profile(bytes(text), "base", {}, choice);
        OA_CHECK(!resolved.resolution);
        OA_CHECK(!resolved.errors.empty());
    }
}

/// Checks the revisions: Revision 1 is the approved set of seventeen
/// features.
void check_revision_features() {
    OA_CHECK(enhanced::highest_revision == 1);
    const FeatureSet revision_1 = FeatureSet{}
                                      .with(Feature::capacity)
                                      .with(Feature::local_flow)
                                      .with(Feature::routes_fine_regions)
                                      .with(Feature::route_corner_advance)
                                      .with(Feature::ai_strike_retarget)
                                      .with(Feature::credited_fallback_search)
                                      .with(Feature::ai_attack_retention)
                                      .with(Feature::ai_rally_retention)
                                      .with(Feature::occupancy_free_planning)
                                      .with(Feature::steering_projection_reuse)
                                      .with(Feature::exact_indexed_searches)
                                      .with(Feature::wide_strike_recruits)
                                      .with(Feature::paced_cohort_targets)
                                      .with(Feature::dispatch_cohort_retention)
                                      .with(Feature::counterflow_queue_escape)
                                      .with(Feature::idle_goal_skip)
                                      .with(Feature::static_interior_refresh_skip);
    OA_CHECK(revision_1 == ids({1, 2, 5, 7, 11, 18, 20, 21, 23, 25, 28, 29, 30, 31, 32, 33, 34}));
    OA_CHECK(enhanced::revision_features(1) == revision_1);
    OA_CHECK(enhanced::close_features(revision_1) == revision_1);
    OA_CHECK(enhanced::revision_features(0).empty());
    OA_CHECK(enhanced::revision_features(2).empty());
    OA_CHECK(enhanced::revision_features(enhanced::highest_revision + 1).empty());
    OA_CHECK(enhanced::revision_features(std::numeric_limits<uint32_t>::max()).empty());
    for (uint32_t revision = 1; revision <= enhanced::highest_revision; ++revision) {
        OA_CHECK(!enhanced::revision_features(revision).empty());
        OA_CHECK(enhanced::feature_set_problem(enhanced::revision_features(revision)) == nullptr);
    }
}

/// Checks that a choice written into a rules record reads back, and that Off clears it.
void check_store_choice() {
    oa::data::match_rules::MatchRules rules;
    const enhanced::Choice revision_1{1, enhanced::revision_features(1)};
    enhanced::store_choice(rules.setup.enhanced_mode, revision_1);
    OA_CHECK(rules.setup.enhanced_mode.enabled);
    OA_CHECK(rules.setup.enhanced_mode.revision == 1);
    OA_CHECK(enhanced::features(rules) == revision_1.features);
    OA_CHECK(enhanced::stored_features(rules.setup.enhanced_mode) == revision_1.features);
    enhanced::store_choice(rules.setup.enhanced_mode, {});
    OA_CHECK(rules.setup.enhanced_mode == oa::data::match_rules::SetupEnhancedMode{});
    OA_CHECK(enhanced::features(rules).empty());
}

/// Checks that resolved rules play their choice's features, and that the hashes differ.
void check_rules_features() {
    OA_CHECK(enhanced::features({}).empty());
    const auto text = profiles::base_game_profile_text();
    const auto off = enhanced::resolve_profile(bytes(text), "base", {}, {});
    const auto capacity = enhanced::resolve_profile(bytes(text), "base", {}, capacity_choice());
    const auto revision = enhanced::resolve_profile(bytes(text), "base", {}, revision_1_choice());
    OA_CHECK(off.resolution && capacity.resolution && revision.resolution);
    if (!off.resolution || !capacity.resolution || !revision.resolution)
        return;
    OA_CHECK(enhanced::features(off.resolution->profile.rules).empty());
    OA_CHECK(enhanced::features(capacity.resolution->profile.rules) == capacity_choice().features);
    const auto& profile = revision.resolution->profile;
    OA_CHECK(enhanced::features(profile.rules) == enhanced::revision_features(1));
    expect_digest("revision_1_sim_hash", profile.sim_hash, revision_1_sim_hash);
    expect_digest("revision_1_canonical_hash", profile.full_hash, revision_1_canonical_hash);
    OA_CHECK(
        profiles::digest_text(oa::base::sha256::digest_of(bytes(revision.resolution->canonical))) ==
        revision_1_canonical_hash
    );
    for (const auto* other : {&*off.resolution, &*capacity.resolution}) {
        OA_CHECK(profile.sim_hash != other->profile.sim_hash);
        OA_CHECK(profile.full_hash != other->profile.full_hash);
    }
    OA_CHECK(off.resolution->profile.sim_hash != capacity.resolution->profile.sim_hash);
    // Names, in registry order: the hash follows the names, not the indexes.
    constexpr std::string_view revision_1_rule =
        "\"setup.enhanced-mode\":{\"features\":[\"capacity\",\"local-flow\","
        "\"routes-fine-regions\",\"route-corner-advance\",\"ai-strike-retarget\","
        "\"credited-fallback-search\",\"ai-attack-retention\",\"ai-rally-retention\","
        "\"occupancy-free-planning\",\"steering-projection-reuse\","
        "\"exact-indexed-searches\",\"wide-strike-recruits\",\"paced-cohort-targets\","
        "\"dispatch-cohort-retention\",\"counterflow-queue-escape\",\"idle-goal-skip\","
        "\"static-interior-refresh-skip\"],\"revision\":1}";
    const std::string_view revision_1_object = enhanced_mode_object(revision.resolution->canonical);
    if (revision_1_object != revision_1_rule)
        std::cerr << "revision 1 object " << revision_1_object << '\n';
    OA_CHECK(revision_1_object == revision_1_rule);
}

} // namespace

int main() {
    check_off_identity();
    check_capacity_identity();
    check_profile_default_set();
    check_enabled_composition();
    check_enhanced_capacity();
    check_refusals();
    check_required_choice();
    check_feature_set_operations();
    check_feature_table();
    check_closing_features();
    check_feature_set_problems();
    check_withdrawn_features();
    check_revision_features();
    check_rules_features();
    check_store_choice();
    return oa::test::check_exit_status();
}
