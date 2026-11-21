// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced Mode is a versioned match preset. Off leaves the chosen game's
// rules and hashes alone; an enabled preset records its version among the
// simulation rules. Graphics choices are not part of a preset. Each preset
// and revision is a set of named features, and the engine asks only whether
// a feature is on.
#pragma once

#include "oa/data/mod_profile.hpp"

#include <cstdint>
#include <optional>
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
/// The experimental preset retaining verified cohort targets near the best weighted cost.
inline constexpr uint32_t cohort_retention_version = 11;
/// The experimental preset trying cardinal exits from a mature local queue.
inline constexpr uint32_t cardinal_queue_escape_version = 13;
/// The Enhanced Mode preset supplied by this build.
inline constexpr uint32_t current_version = swept_ground_version;

/// Selects the known presets inheriting distributed battle flow.
///
/// Restructure only; removed in R09.
///
/// @param version the recorded preset version
/// @return true only for known presets retaining distributed battle flow
[[nodiscard]] inline constexpr bool uses_distributed_battle_flow(uint32_t version) noexcept {
    return version == distributed_battle_flow_version || version == following_flow_version ||
           version == cohort_target_version || version == cohort_retention_version ||
           version == cardinal_queue_escape_version;
}

/// Selects the known presets following an applied local movement leg.
///
/// Restructure only; removed in R09.
///
/// @param version the recorded preset version
/// @return true only for known presets retaining local following
[[nodiscard]] inline constexpr bool uses_following_battle_flow(uint32_t version) noexcept {
    return version == following_flow_version || version == cohort_target_version ||
           version == cohort_retention_version || version == cardinal_queue_escape_version;
}

/// Selects deterministic AI strike-target distribution among spatial cohorts.
///
/// Restructure only; removed in R09.
///
/// @param version the recorded preset version
/// @return true only for known presets retaining cohort-target distribution
[[nodiscard]] inline constexpr bool uses_cohort_target_distribution(uint32_t version) noexcept {
    return version == cohort_target_version || version == cohort_retention_version ||
           version == cardinal_queue_escape_version;
}

/// Selects bounded retention of verified prior targets for spatial cohorts.
///
/// Restructure only; removed in R09.
///
/// @param version the recorded preset version
/// @return true only for the explicit cohort-retention preset
[[nodiscard]] inline constexpr bool uses_cohort_target_retention(uint32_t version) noexcept {
    return version == cohort_retention_version;
}

/// Selects bounded cardinal exits after a mature local queue's existing choices.
///
/// Restructure only; removed in R09.
///
/// @param version the recorded preset version
/// @return true only for the explicit cardinal queue escape preset
[[nodiscard]] inline constexpr bool uses_cardinal_queue_escape(uint32_t version) noexcept {
    return version == cardinal_queue_escape_version;
}

/// Selects the enhanced unit rendering policies shared by known experimental presets.
///
/// Restructure only; removed in R09.
///
/// @param version the recorded preset version
/// @return true only for known presets permitting animated body detail and deferred geometry
[[nodiscard]] inline constexpr bool uses_enhanced_unit_rendering(uint32_t version) noexcept {
    return version == cohort_target_version || version == cohort_retention_version ||
           version == cardinal_queue_escape_version;
}

/// Tells whether this build can resolve a preset, including Off.
///
/// Restructure only; removed in R09.
///
/// @param version the preset version, or off_version
/// @return true for a supported version
[[nodiscard]] bool supported_version(uint32_t version) noexcept;

/// The old version number of a feature set that matches no numbered preset.
///
/// Restructure only; removed in R09. It is neither Off nor a preset this
/// build plays.
inline constexpr uint32_t no_alias_version = 0xffffffffu;

/// Reads the old preset version held by resolved match rules.
///
/// Restructure only; removed in R09. Off is off_version. An enabled match
/// whose features equal an old preset reads as that preset; any other set
/// reads as no_alias_version.
///
/// @param rules the resolved match rules
/// @return the alias of the stored features, or off_version when disabled
[[nodiscard]] uint32_t rules_version(const match_rules::MatchRules& rules) noexcept;

/// One named Enhanced Mode engine change.
///
/// Each value is the feature's id, from 1 to 63. An id is stable forever:
/// it is never renumbered or given to another feature, because saves,
/// recordings and battle rooms name features by id. Changing what a feature
/// does in a shipped revision means a new feature with a new id.
enum class Feature : uint8_t {
    /// 10,000 units per participating player, at most 32,767 units and 32,767
    /// shots in a match, and contact passes that reuse spatial projections.
    capacity = 1,
    /// Temporary local traffic heads that steer ground units around moving
    /// units, with short deferred replans and staggered recovery.
    local_flow = 2,
    /// Coarse routes shared by nearby destinations, each edge rechecked, built
    /// at the original 4-cell spacing.
    routes_original = 3,
    /// Shared coarse routes built over large regions, with visible bends pruned
    /// and blocked fields retained.
    routes_large_regions = 4,
    /// Shared coarse routes built over fine regions at a 2-cell spacing, with
    /// more fields.
    routes_fine_regions = 5,
    /// Indexed target, sighting and order searches that find what a full scan
    /// finds, and air-goal sweeps skipped until an air goal exists.
    indexed_searches = 6,
    /// A blocked ground unit passes the next route corner when its footprint
    /// has a clear, fully known line beyond it.
    route_corner_advance = 7,
    /// Each ground step checks the unit's footprint along the whole step.
    swept_ground_steps = 8,
    /// Advisory one-way lanes through fully known narrow corridors.
    corridor_lanes = 9,
    /// A patrolling unit keeps an unchanged leg's goal and search while it
    /// travels.
    patrol_goal_retention = 10,
    /// Computer strikes replace a lost or dying target early.
    ai_strike_retarget = 11,
    /// Squad removals find a member by an index, keeping the original order.
    squad_position_index = 12,
    /// A briefly blocked unit queues behind a same-owner leader on the same
    /// route instead of passing it.
    route_queue = 13,
    /// Group Move and Patrol orders send a unit whose formation point is
    /// unusable to the clicked point instead.
    group_destination_repair = 14,
    /// Each unit of a repaired group order gets its own arrival point around
    /// the clicked point.
    distributed_arrivals = 15,
    /// A moving ground unit may skip a nearby route anchor when its next leg is
    /// known and clear.
    moving_corner_advance = 16,
    /// Two-by-two ground units take a right-hand lane through a known two-lane
    /// throat.
    local_throat_guide = 17,
    /// When a shared route falls back, each player's longest-waiting unit
    /// searches first, and every ordinary search start is bounded.
    credited_fallback_search = 18,
    /// A computer strike recruits at most 256 new members at a time.
    bounded_strike_recruits = 19,
    /// A computer strike's refresh keeps a live chase instead of restarting it.
    ai_attack_retention = 20,
    /// A computer rally's refresh keeps a unit's march and patrol queue.
    ai_rally_retention = 21,
    /// A queue follows its leader only while the leader's current step still
    /// advances it.
    local_following = 22,
    /// Ground routes are planned through the places moving units occupy.
    occupancy_free_planning = 23,
    /// Computer strikes split into spatial cohorts with their own nearby
    /// targets.
    cohort_targets = 24,
    /// Movement reuses one steering projection while its inputs are unchanged.
    steering_projection_reuse = 25,
    /// A cohort keeps its verified target while it costs within 12.5% of the
    /// best.
    cohort_retention = 26,
    /// A long-blocked queue may step into a clear neighbouring cell in one of
    /// the four main directions.
    cardinal_queue_escape = 27,
    /// Indexed searches as indexed_searches, whose nearby target search also
    /// reaches a unit that has moved within its cell out of the bucket it was
    /// filed in, so it finds exactly what a full scan finds.
    exact_indexed_searches = 28,
};

/// What a feature changes. The machines of a match agree on every kind.
enum class FeatureKind : uint8_t {
    /// Pool sizes, and searches meant to find exactly what the original
    /// searches find.
    capacity,
    /// What the simulation computes.
    simulation,
    /// The orders a player's clicks issue.
    orders,
};

/// A set of Enhanced Mode features: bit n holds the feature with id n, and
/// bit 0 is never a feature.
///
/// Asking whether a set holds a feature is one bit test, cheap enough for a
/// tick's innermost loops.
class FeatureSet {
  public:

    /// Makes the empty set, which is Off.
    constexpr FeatureSet() noexcept = default;

    /// Makes the set whose bits are given.
    ///
    /// @param mask bit n for the feature with id n; bits of no known feature
    ///     are kept, and feature_set_problem refuses them
    /// @return the set
    [[nodiscard]] static constexpr FeatureSet from_bits(uint64_t mask) noexcept {
        FeatureSet set;
        set.bits_ = mask;
        return set;
    }

    /// Returns the set's bits.
    ///
    /// @return bit n for the feature with id n
    [[nodiscard]] constexpr uint64_t bits() const noexcept { return bits_; }

    /// Tells whether the set holds no feature.
    ///
    /// @return true for the empty set
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

    /// Tells whether the set holds a feature.
    ///
    /// @param feature the feature
    /// @return true when the set holds it
    [[nodiscard]] constexpr bool has(Feature feature) const noexcept {
        return (bits_ & bit(feature)) != 0;
    }

    /// Returns the set with a feature added.
    ///
    /// @param feature the feature to add
    /// @return this set and the feature
    [[nodiscard]] constexpr FeatureSet with(Feature feature) const noexcept {
        return from_bits(bits_ | bit(feature));
    }

    /// Returns the set with a feature taken out.
    ///
    /// @param feature the feature to take out
    /// @return this set without the feature
    [[nodiscard]] constexpr FeatureSet without(Feature feature) const noexcept {
        return from_bits(bits_ & ~bit(feature));
    }

    /// Tells whether the set holds every feature of another.
    ///
    /// @param other the other set
    /// @return true when every feature of `other` is in this set
    [[nodiscard]] constexpr bool contains(FeatureSet other) const noexcept {
        return (other.bits_ & ~bits_) == 0;
    }

    /// Tells whether two sets hold the same features.
    ///
    /// @param other the other set
    /// @return true when both hold the same features
    [[nodiscard]] constexpr bool operator==(const FeatureSet& other) const noexcept = default;

    /// Returns the features either set holds.
    ///
    /// @param left one set
    /// @param right the other set
    /// @return their union
    [[nodiscard]] friend constexpr FeatureSet
    operator|(FeatureSet left, FeatureSet right) noexcept {
        return from_bits(left.bits_ | right.bits_);
    }

    /// Returns the features one set holds and another does not.
    ///
    /// @param left the set to take from
    /// @param right the features to take out
    /// @return the features of `left` that are not in `right`
    [[nodiscard]] friend constexpr FeatureSet
    operator-(FeatureSet left, FeatureSet right) noexcept {
        return from_bits(left.bits_ & ~right.bits_);
    }

  private:

    /// Returns a feature's bit.
    ///
    /// @param feature the feature, whose id is below 64
    /// @return the bit its id names
    [[nodiscard]] static constexpr uint64_t bit(Feature feature) noexcept {
        return uint64_t{1} << static_cast<uint8_t>(feature);
    }

    uint64_t bits_{}; ///< bit n for the feature with id n
};

/// One feature as the feature table describes it.
struct FeatureInfo {
    Feature id{};          ///< the feature's stable id
    std::string_view name; ///< its snake_case name, as flags and reports spell it
    FeatureKind kind{};    ///< what it changes
    FeatureSet needs;      ///< the features it needs directly; close_features adds them
    FeatureSet excludes;   ///< the features that cannot share a match with it
};

/// Lists every feature this build knows.
///
/// @return one entry for each feature, in id order from id 1, with no gaps
[[nodiscard]] std::span<const FeatureInfo> feature_table() noexcept;

/// Finds a feature by its name.
///
/// @param name the feature's snake_case name, matched exactly
/// @return the feature, or nothing for a name no feature has
[[nodiscard]] std::optional<Feature> feature_by_name(std::string_view name) noexcept;

/// Returns a feature's name.
///
/// @param feature the feature
/// @return its snake_case name, or an empty view for an id no feature has
[[nodiscard]] std::string_view feature_name(Feature feature) noexcept;

/// Adds every feature a set needs, and every feature those need in turn.
///
/// @param set the features asked for
/// @return the smallest set holding `set` and everything its features need
[[nodiscard]] FeatureSet close_features(FeatureSet set) noexcept;

/// Says why a set of features cannot be played.
///
/// A set can be played when it holds only known features, no two of them
/// exclude each other, every feature it holds has what it needs, and, unless
/// it is empty (Off), it holds capacity, the feature every other one builds on.
///
/// @param set the features to check
/// @return nullptr when the set can be played; otherwise a short English
///     reason naming the features at fault, valid for the program's lifetime
[[nodiscard]] const char* feature_set_problem(FeatureSet set) noexcept;

/// Returns the features an old numbered preset switches on.
///
/// Restructure only; removed in R09. Each old version's set reproduces
/// every check that version made.
///
/// @param version the preset version, or off_version
/// @return the version's features; the empty set for Off and for every
///     version supported_version refuses
[[nodiscard]] FeatureSet version_features(uint32_t version) noexcept;

/// Reads the old preset version whose feature set equals a set.
///
/// Restructure only; removed in R09.
///
/// @param features the features a match plays
/// @return off_version for the empty set, the matching preset version, or
///     no_alias_version when no preset has exactly those features
[[nodiscard]] uint32_t alias_version(FeatureSet features) noexcept;

/// Returns the features resolved match rules play.
///
/// @param rules the resolved match rules
/// @return the stored set; the empty set when Enhanced Mode is off
[[nodiscard]] FeatureSet features(const match_rules::MatchRules& rules) noexcept;

/// The highest Enhanced Mode revision this build plays. A revision is a
/// frozen set of features: every later build plays it exactly as the build
/// that first shipped it.
inline constexpr uint32_t highest_revision = 1;

/// Returns the features of an Enhanced Mode revision.
///
/// Revision 1 is provisional until its features are decided: until then
/// it holds old version 4's features, today's new-game preset.
///
/// @param revision the revision, from 1 to highest_revision
/// @return its features; the empty set for 0 and for a revision this build
///     does not know
[[nodiscard]] FeatureSet revision_features(uint32_t revision) noexcept;

/// The Enhanced Mode identity of one match: a revision and the features it plays.
///
/// Off is revision 0 with the empty set.
struct Choice {
    uint32_t revision{};   ///< 0 is Off; a played revision is at least 1
    FeatureSet features{}; ///< the features the match plays; empty when Off

    /// Compares the revision and the features.
    bool operator==(const Choice&) const = default;
};

/// Says why a choice cannot be played.
///
/// A choice can be played when it is Off, or when its revision is one this
/// build knows and its features can be played together. A revision of 0
/// that still names features cannot.
///
/// @param choice the revision and features
/// @return nullptr when the choice can be played; otherwise a short English
///     reason, valid for the program's lifetime
[[nodiscard]] const char* choice_problem(const Choice& choice) noexcept;

/// Tells whether a choice names a revision or a feature this build does not have.
///
/// A save or recording that does is refused with the needs-version sentence.
/// A set that is not closed, or that conflicts, is a different refusal.
///
/// @param choice the revision and features
/// @return true when the revision or a feature id is unknown here
[[nodiscard]] bool choice_lacks_support(const Choice& choice) noexcept;

/// Resolves the chosen game's profile with a required Enhanced Mode choice.
///
/// Off delegates to the normal resolver unchanged, then refuses a profile
/// that enables Enhanced Mode itself. An enabled choice pushes
/// `setup.enhanced-mode.revision` and `setup.enhanced-mode.features` after
/// every other match choice, so the engine's choice wins. The resolved
/// rules must hold that revision and those features. Errors leave no usable
/// resolution.
///
/// @param text the chosen game's profile bytes
/// @param source the name used by profile diagnostics
/// @param options the chosen game's settings, overrides and match choices
/// @param choice the required revision and features; Off is revision 0
/// @return the resolved rules and hashes, or diagnostics
[[nodiscard]] mod_profile::ResolveResult resolve_profile(
    std::span<const uint8_t> text,
    std::string_view source,
    const mod_profile::ResolveOptions& options,
    const Choice& choice
);

/// Resolves the chosen game's profile with an old numbered preset.
///
/// Restructure only; removed in R09. Off is revision 0. Any other supported
/// version is revision 1 with that version's features. An unsupported
/// version is refused and is not turned into a choice.
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

/// Writes an old numbered preset into a rules record.
///
/// Restructure only; removed in R09. Off and every unsupported version
/// clear the record. A supported version enables revision 1 and that
/// version's features.
///
/// @param[out] mode the Enhanced Mode record
/// @param version the preset version, or off_version
void apply_alias(match_rules::SetupEnhancedMode& mode, uint32_t version) noexcept;

/// Reads the features stored on an Enhanced Mode record, ignoring whether it is on.
///
/// @param mode the Enhanced Mode record
/// @return the stored set; bits of no known feature are kept
[[nodiscard]] FeatureSet stored_features(const match_rules::SetupEnhancedMode& mode) noexcept;

/// Names the first engine version that plays a revision.
///
/// @param revision the revision, from 1
/// @return its version text, such as "0.9.3", or nullptr when this build
///     does not know the revision
[[nodiscard]] const char* first_engine_version(uint32_t revision) noexcept;

/// Names the first engine version that plays a feature.
///
/// @param feature the feature
/// @return its version text, such as "0.9.3", or nullptr for an id no feature has
[[nodiscard]] const char* first_engine_version(Feature feature) noexcept;

/// Names the newest first-version among a choice's revision and features.
///
/// A writer stores this text in a save and its packed form in a recording.
/// Off has none. A choice this build cannot name has none: the writer does
/// not guess.
///
/// @param choice the revision and features
/// @return the version text, or nullptr for Off or for a part this build cannot name
[[nodiscard]] const char* needed_engine_version(const Choice& choice) noexcept;

/// Packs an engine version as major << 16 | minor << 8 | patch.
///
/// @param text a version such as "0.9.3"; each part fits in one byte
/// @return the packed version, or 0 when the text is empty or not three numbers
[[nodiscard]] uint32_t packed_engine_version(std::string_view text) noexcept;

/// The English sentence a player sees when a save or recording needs a later game.
///
/// The caller translates the sentence, then replaces `{version}` with
/// `stored` when `stored` is not empty. An empty `stored` is the sentence
/// that names no version.
///
/// @param stored the version the file stored, or empty when it stored none
/// @return the English sentence, with `{version}` still in it when a version was stored
[[nodiscard]] const char* refusal_needs_version(std::string_view stored) noexcept;

/// Reads the old preset version of one Enhanced Mode record.
///
/// Restructure only; removed in R09.
///
/// @param mode the Enhanced Mode record
/// @return off_version when the record is off, otherwise alias_version of its features
[[nodiscard]] inline uint32_t alias_of(const match_rules::SetupEnhancedMode& mode) noexcept {
    return mode.enabled ? alias_version(stored_features(mode)) : off_version;
}

/// The feature bits a recording sector stores: bit (id − 1), so bit 0 is id 1.
///
/// @param features the features a match plays
/// @return those bits; bit 63 is id 64 and is never set by a set this build can hold
[[nodiscard]] inline uint64_t sector_feature_bits(const FeatureSet& features) noexcept {
    return features.bits() >> 1;
}

/// Reads feature bits stored by a recording sector.
///
/// Bit 63 is id 64, which does not fit in a feature set. That bit is refused
/// rather than dropped.
///
/// @param sector_bits bit (id − 1) for each stored feature
/// @return the set, or nothing when bit 63 is set
[[nodiscard]] inline std::optional<FeatureSet>
features_from_sector_bits(uint64_t sector_bits) noexcept {
    if ((sector_bits & (uint64_t{1} << 63)) != 0)
        return std::nullopt;
    return FeatureSet::from_bits(sector_bits << 1);
}

} // namespace oa::data::enhanced_mode
