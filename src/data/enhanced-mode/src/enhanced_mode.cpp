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

/// The choice's revision, which enters the canonical simulation hash.
constexpr std::string_view revision_path = "setup.enhanced-mode.revision";
/// The choice's features, which enter the canonical simulation hash.
constexpr std::string_view features_path = "setup.enhanced-mode.features";

/// Refuses a choice without handing partially resolved rules to the caller.
///
/// @param[in,out] result the result to refuse
/// @param source the profile name
/// @param message the reason the choice cannot be used
void refuse(mod_profile::ResolveResult& result, std::string_view source, std::string message) {
    result.resolution.reset();
    result.errors.push_back(
        {std::string{source}, {}, std::string{"setup.enhanced-mode"}, std::move(message)}
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

/// Marks a table row withdrawn: no release plays the feature.
///
/// @param row the feature's entry
/// @return the entry, withdrawn
constexpr FeatureInfo withdrawn_row(FeatureInfo row) noexcept {
    row.withdrawn = true;
    return row;
}

/// The feature table, in id order: entry n - 1 describes the feature with
/// id n. Every feature needs Enhanced Mode on, and every chain of needs ends
/// in capacity, the feature that sizes the Enhanced Mode state. The features
/// Revision 1 left out are withdrawn in 0.9.3: their rows stay, so no id
/// moves, and no release plays them.
constexpr std::array<FeatureInfo, 34> feature_rows{{
    {capacity, "capacity", FeatureKind::capacity, {}, {}},
    {local_flow, "local_flow", FeatureKind::simulation, set_of({capacity}), {}},
    withdrawn_row(
        {routes_original,
         "routes_original",
         FeatureKind::simulation,
         set_of({capacity, local_flow}),
         set_of({routes_large_regions, routes_fine_regions})}
    ),
    withdrawn_row(
        {routes_large_regions,
         "routes_large_regions",
         FeatureKind::simulation,
         set_of({capacity, local_flow}),
         set_of({routes_original, routes_fine_regions})}
    ),
    {routes_fine_regions,
     "routes_fine_regions",
     FeatureKind::simulation,
     set_of({capacity, local_flow}),
     set_of({routes_original, routes_large_regions})},
    withdrawn_row(
        {indexed_searches,
         "indexed_searches",
         FeatureKind::capacity,
         set_of({capacity}),
         set_of({exact_indexed_searches})}
    ),
    {route_corner_advance,
     "route_corner_advance",
     FeatureKind::simulation,
     set_of({local_flow}),
     {}},
    withdrawn_row(
        {swept_ground_steps, "swept_ground_steps", FeatureKind::simulation, set_of({capacity}), {}}
    ),
    withdrawn_row(
        {corridor_lanes, "corridor_lanes", FeatureKind::simulation, set_of({local_flow}), {}}
    ),
    withdrawn_row(
        {patrol_goal_retention,
         "patrol_goal_retention",
         FeatureKind::simulation,
         set_of({capacity}),
         {}}
    ),
    // The retarget table keeps its own count, so it does not need local flow.
    {ai_strike_retarget, "ai_strike_retarget", FeatureKind::simulation, set_of({capacity}), {}},
    withdrawn_row(
        {squad_position_index,
         "squad_position_index",
         FeatureKind::capacity,
         set_of({capacity}),
         {}}
    ),
    withdrawn_row({route_queue, "route_queue", FeatureKind::simulation, set_of({local_flow}), {}}),
    withdrawn_row(
        {group_destination_repair,
         "group_destination_repair",
         FeatureKind::orders,
         set_of({capacity}),
         {}}
    ),
    withdrawn_row(
        {distributed_arrivals,
         "distributed_arrivals",
         FeatureKind::orders,
         set_of({group_destination_repair}),
         {}}
    ),
    withdrawn_row(
        {moving_corner_advance,
         "moving_corner_advance",
         FeatureKind::simulation,
         set_of({route_corner_advance}),
         {}}
    ),
    withdrawn_row(
        {local_throat_guide,
         "local_throat_guide",
         FeatureKind::simulation,
         set_of({local_flow}),
         {}}
    ),
    // Any route builder would serve; the fine one is the only one it has played with.
    {credited_fallback_search,
     "credited_fallback_search",
     FeatureKind::simulation,
     set_of({local_flow, routes_fine_regions}),
     {}},
    withdrawn_row(
        {bounded_strike_recruits,
         "bounded_strike_recruits",
         FeatureKind::simulation,
         set_of({capacity}),
         set_of({wide_strike_recruits})}
    ),
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
    withdrawn_row(
        {local_following,
         "local_following",
         FeatureKind::simulation,
         set_of({local_flow, route_queue}),
         {}}
    ),
    {occupancy_free_planning,
     "occupancy_free_planning",
     FeatureKind::simulation,
     set_of({capacity}),
     {}},
    withdrawn_row(
        {cohort_targets,
         "cohort_targets",
         FeatureKind::simulation,
         set_of({ai_strike_retarget}),
         set_of({paced_cohort_targets})}
    ),
    {steering_projection_reuse,
     "steering_projection_reuse",
     FeatureKind::capacity,
     set_of({local_flow}),
     {}},
    withdrawn_row(
        {cohort_retention,
         "cohort_retention",
         FeatureKind::simulation,
         set_of({cohort_targets}),
         set_of({dispatch_cohort_retention})}
    ),
    withdrawn_row(
        {cardinal_queue_escape,
         "cardinal_queue_escape",
         FeatureKind::simulation,
         set_of({local_flow}),
         set_of({counterflow_queue_escape})}
    ),
    // Indexed searches whose target search finds what a full scan finds, in
    // place of the withdrawn indexed_searches.
    {exact_indexed_searches,
     "exact_indexed_searches",
     FeatureKind::capacity,
     set_of({capacity}),
     set_of({indexed_searches})},
    // Strike recruits in larger batches, in place of the withdrawn
    // bounded_strike_recruits.
    {wide_strike_recruits,
     "wide_strike_recruits",
     FeatureKind::simulation,
     set_of({capacity}),
     set_of({bounded_strike_recruits})},
    // Cohort targets whose new chases are spread over a few ticks, in place of
    // the withdrawn cohort_targets.
    {paced_cohort_targets,
     "paced_cohort_targets",
     FeatureKind::simulation,
     set_of({ai_strike_retarget}),
     set_of({cohort_targets})},
    // Cohort retention for paced cohort targets at the periodic dispatch
    // only, in place of the withdrawn cohort_retention.
    {dispatch_cohort_retention,
     "dispatch_cohort_retention",
     FeatureKind::simulation,
     set_of({paced_cohort_targets}),
     set_of({cohort_retention})},
    // Cardinal queue escape only from crossing or oncoming traffic, in place of
    // the withdrawn cardinal_queue_escape.
    {counterflow_queue_escape,
     "counterflow_queue_escape",
     FeatureKind::simulation,
     set_of({local_flow}),
     set_of({cardinal_queue_escape})},
    // A long-blocked traffic head waiting for recovery skips its goal.
    {idle_goal_skip, "idle_goal_skip", FeatureKind::capacity, set_of({local_flow}), {}},
    // Planning that passes through moving units skips their map refreshes
    // clear of the map's right and bottom edges.
    {static_interior_refresh_skip,
     "static_interior_refresh_skip",
     FeatureKind::capacity,
     set_of({occupancy_free_planning}),
     {}},
}};

/// Every feature the table describes.
constexpr FeatureSet known_features = [] {
    FeatureSet known;
    for (const FeatureInfo& row : feature_rows)
        known = known.with(row.id);
    return known;
}();

/// Returns the features two sets both hold.
///
/// @param left one set
/// @param right the other set
/// @return their intersection
constexpr FeatureSet common_features(FeatureSet left, FeatureSet right) noexcept {
    return left - (left - right);
}

/// Every withdrawn feature: known, and played by no release.
constexpr FeatureSet withdrawn_features = [] {
    FeatureSet withdrawn;
    for (const FeatureInfo& row : feature_rows)
        if (row.withdrawn)
            withdrawn = withdrawn.with(row.id);
    return withdrawn;
}();

/// Tells whether no feature that is still played needs a withdrawn one, so
/// closing a set of played features never needs a withdrawn feature.
///
/// @return true when every played feature's needs are played features
constexpr bool played_features_need_no_withdrawn() noexcept {
    for (const FeatureInfo& row : feature_rows)
        if (!row.withdrawn && !common_features(row.needs, withdrawn_features).empty())
            return false;
    return true;
}

static_assert(played_features_need_no_withdrawn());

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

/// The reason feature_set_problem gives for each feature, by id - 1, when the
/// feature is withdrawn.
constexpr std::array<PairReason, feature_rows.size()> withdrawn_reasons = [] {
    std::array<PairReason, feature_rows.size()> reasons{};
    for (std::size_t index = 0; index < feature_rows.size(); ++index)
        reasons[index] =
            join_reason(feature_rows[index].name, " is withdrawn; no release plays it", "");
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

/// Revision 1's features, the set measured in the gate battles and
/// approved. Frozen: every later build plays exactly these.
constexpr FeatureSet revision_1_features = set_of(
    {capacity,
     local_flow,
     routes_fine_regions,
     route_corner_advance,
     ai_strike_retarget,
     credited_fallback_search,
     ai_attack_retention,
     ai_rally_retention,
     occupancy_free_planning,
     steering_projection_reuse,
     exact_indexed_searches,
     wide_strike_recruits,
     paced_cohort_targets,
     dispatch_cohort_retention,
     counterflow_queue_escape,
     idle_goal_skip,
     static_interior_refresh_skip}
);

/// Tells whether a set holds only known features, every feature its
/// features need, no two features that exclude each other, and capacity
/// when it holds anything: the set feature_set_problem accepts and
/// close_features leaves unchanged.
///
/// @param set the features
/// @return true when the set is closed and conflict-free
constexpr bool closed_and_conflict_free(FeatureSet set) noexcept {
    if (!(set - known_features).empty())
        return false;
    for (const FeatureInfo& row : feature_rows) {
        if (!set.has(row.id))
            continue;
        if (!set.contains(row.needs) || row.excludes - set != row.excludes)
            return false;
    }
    return set.empty() || set.has(capacity);
}

static_assert(closed_and_conflict_free(revision_1_features));

/// The features of each revision, by revision. Index 0 is Off.
constexpr std::array<FeatureSet, highest_revision + 1> revision_sets{{
    {},
    revision_1_features,
}};

/// Tells whether no revision holds a withdrawn feature.
///
/// @return true when every revision plays only features a release plays
constexpr bool revisions_hold_no_withdrawn() noexcept {
    for (const FeatureSet& revision : revision_sets)
        if (!common_features(revision, withdrawn_features).empty())
            return false;
    return true;
}

static_assert(revisions_hold_no_withdrawn());

/// The first engine version of each feature, in id order. A new feature
/// names its version here and nowhere else.
constexpr std::array<const char*, feature_rows.size()> feature_engine_versions{{
    "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3",
    "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3",
    "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3",
    "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3", "0.9.3",
}};

/// The first engine version of each revision. Index 0 is Off and has none.
constexpr std::array<const char*, highest_revision + 1> revision_engine_versions{{
    nullptr,
    "0.9.3",
}};

/// Packs major.minor.patch. A part past 255, or any other shape, is 0.
///
/// @param text the version text
/// @return the packed version, or 0 when the text is not three byte-sized parts
constexpr uint32_t pack_version(std::string_view text) noexcept {
    uint32_t parts[3]{};
    std::size_t part = 0;
    uint32_t value = 0;
    bool saw_digit = false;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool end = index == text.size();
        const char letter = end ? '.' : text[index];
        if (letter == '.') {
            if (!saw_digit || part >= 3)
                return 0;
            parts[part++] = value;
            value = 0;
            saw_digit = false;
            continue;
        }
        if (letter < '0' || letter > '9' || part >= 3)
            return 0;
        saw_digit = true;
        const auto digit = static_cast<uint32_t>(letter - '0');
        if (value > (255u - digit) / 10u)
            return 0;
        value = value * 10u + digit;
    }
    if (part != 3)
        return 0;
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

/// Writes a feature set as the registry's YAML list, names in id order.
///
/// @param features the features
/// @return a profile value such as `[capacity, local-flow]`
std::string features_text(FeatureSet features) {
    std::string text = "[";
    bool first = true;
    for (const FeatureInfo& row : feature_rows) {
        if (!features.has(row.id))
            continue;
        if (!first)
            text += ", ";
        first = false;
        for (const char letter : row.name)
            text += letter == '_' ? '-' : letter;
    }
    text += ']';
    return text;
}

/// The registry enumeration, one value per feature, in id order.
using FeatureEnum = match_rules::SetupEnhancedModeFeatures;

static_assert(enum_size(FeatureEnum{}) == feature_rows.size());
static_assert(static_cast<int>(FeatureEnum::capacity) == 0);
static_assert(static_cast<int>(FeatureEnum::static_interior_refresh_skip) == 33);

/// Writes a feature set into the registry's enumeration set.
///
/// @param features the features; bit n is id n
/// @return the same features, member n - 1 for id n
match_rules::EnumSet<FeatureEnum, feature_rows.size()> to_enum(FeatureSet features) noexcept {
    match_rules::EnumSet<FeatureEnum, feature_rows.size()> stored;
    for (std::size_t index = 0; index < feature_rows.size(); ++index)
        if (features.has(static_cast<Feature>(index + 1)))
            stored.members[index] = true;
    return stored;
}

/// Reads a registry enumeration set back into feature bits.
///
/// @param stored member n - 1 for id n
/// @return bit n for id n
FeatureSet
from_enum(const match_rules::EnumSet<FeatureEnum, feature_rows.size()>& stored) noexcept {
    FeatureSet features;
    for (std::size_t index = 0; index < stored.members.size(); ++index)
        if (stored.members[index])
            features = features.with(static_cast<Feature>(index + 1));
    return features;
}

} // namespace

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
            closed = closed | (row->needs - withdrawn_features);
    return closed;
}

const char* feature_set_problem(FeatureSet set) noexcept {
    if (!(set - known_features).empty())
        return "the set holds a feature this build does not know";
    for (std::size_t index = 0; index < feature_rows.size(); ++index)
        if (feature_rows[index].withdrawn && set.has(feature_rows[index].id))
            return withdrawn_reasons[index].data();
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

FeatureSet features(const match_rules::MatchRules& rules) noexcept {
    if (!rules.setup.enhanced_mode.enabled)
        return {};
    return stored_features(rules.setup.enhanced_mode);
}

FeatureSet revision_features(uint32_t revision) noexcept {
    return revision != 0 && revision < revision_sets.size() ? revision_sets[revision]
                                                            : FeatureSet{};
}

FeatureSet stored_features(const match_rules::SetupEnhancedMode& mode) noexcept {
    return from_enum(mode.features);
}

void store_choice(match_rules::SetupEnhancedMode& mode, const Choice& choice) noexcept {
    mode = {};
    if (choice.revision == 0)
        return;
    mode.enabled = true;
    mode.revision = static_cast<int32_t>(choice.revision);
    mode.features = to_enum(choice.features);
}

const char* choice_problem(const Choice& choice) noexcept {
    if (choice.revision != 0 && revision_features(choice.revision).empty())
        return "unknown Enhanced Mode revision";
    if (choice.revision == 0 && !choice.features.empty())
        return "Enhanced Mode is off and still names features";
    return feature_set_problem(choice.features);
}

bool choice_lacks_support(const Choice& choice) noexcept {
    if (choice.revision != 0 && revision_features(choice.revision).empty())
        return true;
    return !(choice.features - known_features).empty();
}

mod_profile::ResolveResult resolve_profile(
    std::span<const uint8_t> text,
    std::string_view source,
    const mod_profile::ResolveOptions& options,
    const Choice& choice
) {
    mod_profile::ResolveResult result;
    if (const char* problem = choice_problem(choice)) {
        refuse(result, source, problem);
        return result;
    }
    if (choice.revision == 0) {
        result = mod_profile::resolve_profile(text, source, options);
        if (result.resolution && result.resolution->profile.rules.setup.enhanced_mode.enabled)
            refuse(result, source, "the profile's enhanced rules require Enhanced Mode");
        return result;
    }
    mod_profile::ResolveOptions enhanced = options;
    enhanced.match.push_back(
        {std::string{revision_path}, std::to_string(choice.revision), "Enhanced Mode", true}
    );
    enhanced.match.push_back(
        {std::string{features_path}, features_text(choice.features), "Enhanced Mode", true}
    );
    result = mod_profile::resolve_profile(text, source, enhanced);
    if (!result.resolution)
        return result;
    const auto& mode = result.resolution->profile.rules.setup.enhanced_mode;
    if (!mode.enabled || static_cast<uint32_t>(mode.revision) != choice.revision ||
        stored_features(mode) != choice.features)
        refuse(result, source, "the Enhanced Mode preset did not resolve at the required revision");
    return result;
}

const char* first_engine_version(uint32_t revision) noexcept {
    if (revision >= revision_engine_versions.size())
        return nullptr;
    return revision_engine_versions[revision];
}

const char* first_engine_version(Feature feature) noexcept {
    const auto id = static_cast<std::size_t>(feature);
    if (id == 0 || id > feature_engine_versions.size())
        return nullptr;
    return feature_engine_versions[id - 1];
}

const char* needed_engine_version(const Choice& choice) noexcept {
    if (choice.revision == 0 && choice.features.empty())
        return nullptr;
    const char* best = first_engine_version(choice.revision);
    if (choice.revision != 0 && best == nullptr)
        return nullptr;
    uint32_t best_packed = best != nullptr ? pack_version(best) : 0;
    for (const FeatureInfo& row : feature_table()) {
        if (!choice.features.has(row.id))
            continue;
        const char* introduced = first_engine_version(row.id);
        if (introduced == nullptr)
            return nullptr;
        const uint32_t packed = pack_version(introduced);
        if (packed > best_packed) {
            best = introduced;
            best_packed = packed;
        }
    }
    return best;
}

uint32_t packed_engine_version(std::string_view text) noexcept {
    return pack_version(text);
}

const char* refusal_needs_version(std::string_view stored) noexcept {
    if (stored.empty())
        return "This game needs a later version of Open Annihilation.";
    return "This game needs Open Annihilation {version} or later.";
}

} // namespace oa::data::enhanced_mode
