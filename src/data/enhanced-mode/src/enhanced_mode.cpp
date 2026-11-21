// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/enhanced_mode.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <initializer_list>
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

/// Makes the set holding exactly the given features.
///
/// @param members the features
/// @return their set
constexpr FeatureSet set_of(std::initializer_list<Feature> members) noexcept {
    FeatureSet result;
    for (const Feature member : members)
        result = result.with(member);
    return result;
}

using enum Feature;

/// The feature table, in id order: entry n - 1 describes the feature with
/// id n. Every feature needs Enhanced Mode on, and every chain of needs ends
/// in capacity, the feature that sizes the Enhanced Mode state.
constexpr std::array<FeatureInfo, 27> feature_rows{{
    {capacity, "capacity", FeatureKind::capacity, {}, {}},
    {local_flow, "local_flow", FeatureKind::simulation, set_of({capacity}), {}},
    {routes_original,
     "routes_original",
     FeatureKind::simulation,
     set_of({capacity, local_flow}),
     set_of({routes_large_regions, routes_fine_regions})},
    {routes_large_regions,
     "routes_large_regions",
     FeatureKind::simulation,
     set_of({capacity, local_flow}),
     set_of({routes_original, routes_fine_regions})},
    {routes_fine_regions,
     "routes_fine_regions",
     FeatureKind::simulation,
     set_of({capacity, local_flow}),
     set_of({routes_original, routes_large_regions})},
    {indexed_searches, "indexed_searches", FeatureKind::capacity, set_of({capacity}), {}},
    {route_corner_advance,
     "route_corner_advance",
     FeatureKind::simulation,
     set_of({local_flow}),
     {}},
    {swept_ground_steps, "swept_ground_steps", FeatureKind::simulation, set_of({capacity}), {}},
    {corridor_lanes, "corridor_lanes", FeatureKind::simulation, set_of({local_flow}), {}},
    {patrol_goal_retention,
     "patrol_goal_retention",
     FeatureKind::simulation,
     set_of({capacity}),
     {}},
    // Local traffic flow sizes the state the retarget table saves.
    {ai_strike_retarget,
     "ai_strike_retarget",
     FeatureKind::simulation,
     set_of({capacity, local_flow}),
     {}},
    {squad_position_index, "squad_position_index", FeatureKind::capacity, set_of({capacity}), {}},
    {route_queue, "route_queue", FeatureKind::simulation, set_of({local_flow}), {}},
    {group_destination_repair,
     "group_destination_repair",
     FeatureKind::orders,
     set_of({capacity}),
     {}},
    {distributed_arrivals,
     "distributed_arrivals",
     FeatureKind::orders,
     set_of({group_destination_repair}),
     {}},
    {moving_corner_advance,
     "moving_corner_advance",
     FeatureKind::simulation,
     set_of({route_corner_advance}),
     {}},
    {local_throat_guide, "local_throat_guide", FeatureKind::simulation, set_of({local_flow}), {}},
    // Any route builder would serve; the fine one is the only one it has played with.
    {credited_fallback_search,
     "credited_fallback_search",
     FeatureKind::simulation,
     set_of({local_flow, routes_fine_regions}),
     {}},
    {bounded_strike_recruits,
     "bounded_strike_recruits",
     FeatureKind::simulation,
     set_of({capacity}),
     {}},
    {ai_attack_retention,
     "ai_attack_retention",
     FeatureKind::simulation,
     set_of({ai_strike_retarget}),
     {}},
    {ai_rally_retention,
     "ai_rally_retention",
     FeatureKind::simulation,
     set_of({ai_strike_retarget}),
     {}},
    {local_following,
     "local_following",
     FeatureKind::simulation,
     set_of({local_flow, route_queue}),
     {}},
    {occupancy_free_planning,
     "occupancy_free_planning",
     FeatureKind::simulation,
     set_of({capacity}),
     {}},
    {cohort_targets, "cohort_targets", FeatureKind::simulation, set_of({ai_strike_retarget}), {}},
    {steering_projection_reuse,
     "steering_projection_reuse",
     FeatureKind::capacity,
     set_of({local_flow}),
     {}},
    {cohort_retention, "cohort_retention", FeatureKind::simulation, set_of({cohort_targets}), {}},
    {cardinal_queue_escape,
     "cardinal_queue_escape",
     FeatureKind::simulation,
     set_of({local_flow}),
     {}},
}};

/// Every feature the table describes.
constexpr FeatureSet known_features = [] {
    FeatureSet known;
    for (const FeatureInfo& row : feature_rows)
        known = known.with(row.id);
    return known;
}();

/// Tells whether the table's rows are in id order with no gaps, every
/// feature needs only features with lower ids, and every exclusion is
/// mutual. close_features relies on the order of needs to close a set in one
/// pass.
///
/// @return true when the table keeps all three
constexpr bool table_is_ordered() noexcept {
    for (std::size_t index = 0; index < feature_rows.size(); ++index) {
        const FeatureInfo& row = feature_rows[index];
        const auto id = static_cast<std::size_t>(row.id);
        if (id != index + 1 || id >= 64)
            return false;
        if ((row.needs.bits() >> id) != 0)
            return false;
        for (const FeatureInfo& other : feature_rows)
            if (row.excludes.has(other.id) != other.excludes.has(row.id))
                return false;
    }
    return true;
}

static_assert(table_is_ordered());

/// The longest reason feature_set_problem gives a feature pair, with its NUL.
constexpr std::size_t reason_size = 96;

/// One reason naming a pair of features, NUL-terminated.
using PairReason = std::array<char, reason_size>;

/// Joins a feature's name, a phrase and another feature's name into a reason.
///
/// @param first the first feature's name
/// @param phrase the words between the names
/// @param second the second feature's name
/// @return the reason; a constant expression fails when it does not fit
constexpr PairReason
join_reason(std::string_view first, std::string_view phrase, std::string_view second) noexcept {
    PairReason reason{};
    std::size_t at = 0;
    for (const std::string_view part : {first, phrase, second})
        for (const char letter : part)
            reason[at++] = letter;
    reason[at] = '\0';
    return reason;
}

/// Returns the features a feature is linked to: those it needs and those it
/// excludes.
///
/// @param row the feature's entry
/// @return the linked features
constexpr FeatureSet links_of(const FeatureInfo& row) noexcept {
    return row.needs | row.excludes;
}

/// Counts every feature's links.
///
/// @return the number of reasons pair_reasons holds
constexpr std::size_t link_count() noexcept {
    std::size_t count = 0;
    for (const FeatureInfo& row : feature_rows)
        count += static_cast<std::size_t>(std::popcount(links_of(row).bits()));
    return count;
}

/// Where each feature's reasons start in pair_reasons, by id - 1.
constexpr std::array<std::size_t, feature_rows.size()> first_reason = [] {
    std::array<std::size_t, feature_rows.size()> starts{};
    std::size_t at = 0;
    for (std::size_t index = 0; index < feature_rows.size(); ++index) {
        starts[index] = at;
        at += static_cast<std::size_t>(std::popcount(links_of(feature_rows[index]).bits()));
    }
    return starts;
}();

/// The reasons for every link: feature by feature in id order, then by the
/// linked feature's id.
constexpr std::array<PairReason, link_count()> pair_reasons = [] {
    std::array<PairReason, link_count()> reasons{};
    std::size_t at = 0;
    for (const FeatureInfo& row : feature_rows)
        for (const FeatureInfo& other : feature_rows) {
            if (row.needs.has(other.id))
                reasons[at++] = join_reason(row.name, " needs ", other.name);
            else if (row.excludes.has(other.id))
                reasons[at++] = join_reason(row.name, " cannot share a match with ", other.name);
        }
    return reasons;
}();

/// Returns the reason naming a feature and one it is linked to.
///
/// @param row the feature's entry
/// @param other the linked feature, one of links_of(row)
/// @return the reason, NUL-terminated
const char* pair_reason(const FeatureInfo& row, Feature other) noexcept {
    const uint64_t lower = (uint64_t{1} << static_cast<uint8_t>(other)) - 1;
    const auto rank = static_cast<std::size_t>(std::popcount(links_of(row).bits() & lower));
    return pair_reasons[first_reason[static_cast<std::size_t>(row.id) - 1] + rank].data();
}

// The old numbered presets' features. Restructure only; removed before the
// version numbers leave the engine.

/// Old version 1: capacity alone.
constexpr FeatureSet version_1_features = set_of({capacity});
/// Old version 2: local flow over the original shared routes.
constexpr FeatureSet version_2_features =
    set_of({capacity, local_flow, routes_original, indexed_searches});
/// Old version 3: regional routes and corner advances.
constexpr FeatureSet version_3_features =
    set_of({capacity, local_flow, routes_large_regions, indexed_searches, route_corner_advance});
/// Old version 4, the new-game preset: version 3 with swept ground steps.
constexpr FeatureSet version_4_features = set_of(
    {capacity,
     local_flow,
     routes_large_regions,
     indexed_searches,
     route_corner_advance,
     swept_ground_steps}
);
/// Old version 5: version 4 with corridor lanes.
constexpr FeatureSet version_5_features = set_of(
    {capacity,
     local_flow,
     routes_large_regions,
     indexed_searches,
     route_corner_advance,
     swept_ground_steps,
     corridor_lanes}
);
/// Old version 6: version 4 with battle flow and repaired group destinations.
constexpr FeatureSet version_6_features = set_of(
    {capacity,
     local_flow,
     routes_large_regions,
     indexed_searches,
     route_corner_advance,
     swept_ground_steps,
     patrol_goal_retention,
     ai_strike_retarget,
     squad_position_index,
     route_queue,
     group_destination_repair}
);
/// Old version 7: version 6 over fine routes, with distributed arrivals.
constexpr FeatureSet version_7_features = set_of(
    {capacity,
     local_flow,
     routes_fine_regions,
     indexed_searches,
     route_corner_advance,
     swept_ground_steps,
     patrol_goal_retention,
     ai_strike_retarget,
     squad_position_index,
     route_queue,
     group_destination_repair,
     distributed_arrivals,
     moving_corner_advance,
     local_throat_guide,
     credited_fallback_search,
     bounded_strike_recruits,
     ai_attack_retention,
     ai_rally_retention}
);
/// Old version 8: version 7 with local following.
constexpr FeatureSet version_8_features =
    version_7_features | set_of({local_following, occupancy_free_planning});
/// Old version 10: version 8 with cohort targets.
constexpr FeatureSet version_10_features =
    version_8_features | set_of({cohort_targets, steering_projection_reuse});
/// Old version 11: version 10 with cohort retention.
constexpr FeatureSet version_11_features = version_10_features | set_of({cohort_retention});
/// Old version 13: version 10 with cardinal queue escape.
constexpr FeatureSet version_13_features = version_10_features | set_of({cardinal_queue_escape});

/// The features of each old preset, by version; Off and the withdrawn
/// versions 9 and 12 are empty.
constexpr std::array<FeatureSet, cardinal_queue_escape_version + 1> version_feature_sets{{
    {},
    version_1_features,
    version_2_features,
    version_3_features,
    version_4_features,
    version_5_features,
    version_6_features,
    version_7_features,
    version_8_features,
    {},
    version_10_features,
    version_11_features,
    {},
    version_13_features,
}};

/// Revision 1's features. Provisional until they are decided: old
/// version 4's set, today's new-game preset.
constexpr FeatureSet revision_1_features = version_4_features;

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

std::span<const FeatureInfo> feature_table() noexcept {
    return feature_rows;
}

std::optional<Feature> feature_by_name(std::string_view name) noexcept {
    for (const FeatureInfo& row : feature_rows)
        if (row.name == name)
            return row.id;
    return std::nullopt;
}

std::string_view feature_name(Feature feature) noexcept {
    const auto id = static_cast<std::size_t>(feature);
    if (id == 0 || id > feature_rows.size())
        return {};
    return feature_rows[id - 1].name;
}

FeatureSet close_features(FeatureSet set) noexcept {
    // Needs point only at lower ids, so one pass from the highest id down
    // reaches every feature a later one added.
    FeatureSet closed = set;
    for (auto row = feature_rows.rbegin(); row != feature_rows.rend(); ++row)
        if (closed.has(row->id))
            closed = closed | row->needs;
    return closed;
}

const char* feature_set_problem(FeatureSet set) noexcept {
    if (!(set - known_features).empty())
        return "the set holds a feature this build does not know";
    for (const FeatureInfo& row : feature_rows) {
        if (!set.has(row.id))
            continue;
        for (const FeatureInfo& other : feature_rows)
            if (row.excludes.has(other.id) && set.has(other.id))
                return pair_reason(row, other.id);
    }
    if (!set.empty() && !set.has(Feature::capacity))
        return "a set with any feature needs capacity";
    for (const FeatureInfo& row : feature_rows) {
        if (!set.has(row.id))
            continue;
        for (const FeatureInfo& other : feature_rows)
            if (row.needs.has(other.id) && !set.has(other.id))
                return pair_reason(row, other.id);
    }
    return nullptr;
}

FeatureSet version_features(uint32_t version) noexcept {
    return version < version_feature_sets.size() ? version_feature_sets[version] : FeatureSet{};
}

FeatureSet features(const match_rules::MatchRules& rules) noexcept {
    return version_features(rules_version(rules));
}

FeatureSet revision_features(uint32_t revision) noexcept {
    return revision == 1 ? revision_1_features : FeatureSet{};
}

} // namespace oa::data::enhanced_mode
