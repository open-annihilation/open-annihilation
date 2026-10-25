// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The planner's inner parts: views in whole numbers, how points are framed
// and how a view is placed to show less water (planner_framing.cpp), what
// the planner reads from a timeline before it plans (planner_analysis.cpp),
// and the subjects it can film, with their scores and framings
// (planner_subjects.cpp). planner.cpp places the shots.
//
// Everything is integer arithmetic; ties are broken by stated orders, never
// by addresses or hashed containers, so the same timeline gives the same
// plan everywhere.
#pragma once

#include "oa/core/player.h"
#include "oa/formats/oascript.hpp"
#include "oa/media/director/planner.hpp"
#include "oa/media/director/timeline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::media::director::planning {

/// A tick that never comes: a unit that does not die, a game without an
/// engagement or a deciding death.
inline constexpr uint32_t never = std::numeric_limits<uint32_t>::max();
/// An index that names nothing.
inline constexpr uint32_t no_index = std::numeric_limits<uint32_t>::max();
/// The players a timeline names: indices 0 to OA_PLAYER_COUNT - 1.
inline constexpr size_t player_slots = OA_PLAYER_COUNT;
/// A health event's kind (TimelineEvent::damage_kind) for paralysis.
inline constexpr uint8_t paralysis_kind = 2;
/// A health event's kind for healing.
inline constexpr uint8_t healing_kind = 10;

/// Returns a tick plus some ticks, held below never.
///
/// @param tick the tick
/// @param ticks the ticks to add
/// @return the sum, at most never - 1
[[nodiscard]] constexpr uint32_t later(uint32_t tick, uint32_t ticks) noexcept {
    const uint64_t sum{uint64_t{tick} + ticks};
    return sum < never ? static_cast<uint32_t>(sum) : never - 1u;
}

/// Returns a tick less some ticks, held at 0.
///
/// @param tick the tick
/// @param ticks the ticks to take away
/// @return the difference, at least 0
[[nodiscard]] constexpr uint32_t earlier(uint32_t tick, uint32_t ticks) noexcept {
    return tick > ticks ? tick - ticks : 0u;
}

/// A point on the ground plane, in map pixels.
struct Point {
    int32_t x{};
    int32_t row{}; ///< the map-image row: ground_row of the world point
};

/// Returns the ground point of a world point.
///
/// @param point the world point
/// @return its x and ground_row
[[nodiscard]] Point ground_of(WorldPoint point) noexcept;

/// Returns a quotient rounded toward negative infinity.
///
/// @param numerator the dividend
/// @param denominator the divisor, above zero
/// @return floor(numerator / denominator)
[[nodiscard]] int64_t floor_div(int64_t numerator, int64_t denominator) noexcept;

/// Returns a quotient rounded toward positive infinity.
///
/// @param numerator the dividend
/// @param denominator the divisor, above zero
/// @return ceil(numerator / denominator)
[[nodiscard]] int64_t ceil_div(int64_t numerator, int64_t denominator) noexcept;

/// Returns the whole square root of a number.
///
/// @param value the number
/// @return the largest r with r * r at most value
[[nodiscard]] uint64_t integer_sqrt(uint64_t value) noexcept;

/// Returns the square of the distance between two points.
///
/// @param a a point
/// @param b another
/// @return dx * dx + drow * drow
[[nodiscard]] int64_t distance_squared(Point a, Point b) noexcept;

/// Returns the distance between two points, rounded down.
///
/// @param a a point
/// @param b another
/// @return integer_sqrt(distance_squared(a, b))
[[nodiscard]] int64_t distance(Point a, Point b) noexcept;

/// The map and the output a plan frames views for.
struct Stage {
    int64_t output_width{1};  ///< pixels, at least 1
    int64_t output_height{1}; ///< pixels, at least 1
    int64_t map_width{};      ///< the map's bounds, map pixels
    int64_t map_height{};
    /// The whole map's view height: fit_height of the bounds and the output,
    /// rounded down, so that it never exceeds fit_height.
    int64_t fit{};
};

/// A view in whole numbers: its centre in half map pixels, so that a map's
/// middle is exact, and its height in map pixels.
struct PlannedView {
    int64_t x2{}; ///< twice the centre's column
    int64_t z2{}; ///< twice the centre's row
    int64_t height{};

    /// Tells whether two views are the same.
    ///
    /// @param other the other view
    /// @return true when every member is equal
    [[nodiscard]] bool operator==(const PlannedView& other) const noexcept = default;
};

/// Returns the stage of a timeline's map and a plan's output.
///
/// @param header the timeline's header: its map bounds
/// @param settings the plan's output size
/// @return the stage
[[nodiscard]] Stage
make_stage(const TimelineHeader& header, const PlannerSettings& settings) noexcept;

/// Returns the width of a view, rounded down.
///
/// @param stage the stage
/// @param height the view's height
/// @return floor(height * output width / output height)
[[nodiscard]] int64_t view_width(const Stage& stage, int64_t height) noexcept;

/// Returns a view kept on the map exactly as clamp_view would keep it, in
/// whole numbers, so that clamp_view leaves the written view unchanged.
///
/// The height is held within [min_view_height, fit]; an axis on which the
/// view is as large as the map centres on it, and otherwise the centre is
/// held so that the view stays on the map, then moved inward by half a
/// pixel when that makes it a whole pixel.
///
/// @param stage the stage
/// @param view the view
/// @return the kept view
[[nodiscard]] PlannedView clamp_planned(const Stage& stage, PlannedView view) noexcept;

/// Returns the view of the whole map.
///
/// @param stage the stage
/// @return the map's middle at height fit
[[nodiscard]] PlannedView whole_map(const Stage& stage) noexcept;

/// Returns the height a view needs to hold points in its safe area.
///
/// @param stage the stage
/// @param points the points; at least one
/// @return the larger of the rows and the columns the points span, each
///         scaled so that they fill safe_area_percent of the view, rounded up
[[nodiscard]] int64_t framed_height(const Stage& stage, std::span<const Point> points) noexcept;

/// Returns the view that frames points: their bounding box's middle,
/// rounded down to a whole pixel, at the height that holds them in the safe
/// area, held within [min_height, max_height] and then kept on the map.
///
/// @param stage the stage
/// @param points the points
/// @param min_height the smallest height
/// @param max_height the largest height, at least min_height
/// @return the view, or none for no point
[[nodiscard]] std::optional<PlannedView> frame_points(
    const Stage& stage, std::span<const Point> points, int64_t min_height, int64_t max_height
);

/// Returns a view whose height changes at most max_zoom_change_percent a
/// second from another's.
///
/// @param stage the stage
/// @param from the view the camera holds
/// @param to the view it moves to
/// @param ticks the ticks the change takes
/// @return `to` with its height held within the limit and kept on the map
[[nodiscard]] PlannedView limit_zoom(
    const Stage& stage, const PlannedView& from, const PlannedView& to, uint32_t ticks
) noexcept;

/// Tells whether a point lies in a view's safe area.
///
/// @param stage the stage
/// @param view the view
/// @param point the point
/// @return true when the point is within the middle safe_area_percent of
///         the view on both axes
[[nodiscard]] bool in_safe_area(const Stage& stage, const PlannedView& view, Point point) noexcept;

/// Tells whether the camera pans from one view to another rather than cutting.
///
/// @param stage the stage
/// @param from the view the camera holds
/// @param to the new view
/// @return true when the centres are at most pan_reach_percent of from's
///         width apart and the heights within tuning::pan_height_ratio
[[nodiscard]] bool
near_enough_to_pan(const Stage& stage, const PlannedView& from, const PlannedView& to) noexcept;

/// Returns how much two views overlap: the area they share over the area
/// either covers.
///
/// @param stage the stage
/// @param a a view
/// @param b another
/// @return 0 to 100, in percent
[[nodiscard]] int64_t
overlap_percent(const Stage& stage, const PlannedView& a, const PlannedView& b) noexcept;

/// Returns the view that frames points in a part of it other than the safe
/// area: their bounding box's middle, at the height that holds them within
/// `area_percent` of each side, held within [min_height, max_height] and
/// kept on the map.
///
/// @param stage the stage
/// @param points the points
/// @param area_percent the part of the view that holds them, 1 to 100
/// @param min_height the smallest height
/// @param max_height the largest height, at least min_height
/// @return the view, or none for no point
[[nodiscard]] std::optional<PlannedView> frame_points_within(
    const Stage& stage,
    std::span<const Point> points,
    int64_t area_percent,
    int64_t min_height,
    int64_t max_height
);

/// Returns the script camera of a view.
///
/// @param view the view
/// @return its centre and height as decimals: whole pixels, or halves with
///         one decimal place
[[nodiscard]] oa::formats::oascript::CameraState camera_of(const PlannedView& view);

/// One unit from its creation to its death. A slot holds a new life each
/// time a unit is created in it.
struct Life {
    uint16_t slot{};
    uint16_t unit_type{};
    uint8_t owner{no_player};
    bool commander{};
    bool mobile{};
    bool armed{};   ///< its type carries a weapon
    bool builder{}; ///< its type builds or assists
    bool factory{}; ///< a building that builds: it finishes units
    uint32_t born{};
    uint32_t died{never};
    Point created_at{};
    Point died_at{};
    int64_t value{1};        ///< its type's metal cost, at least 1
    int64_t max_damage{1};   ///< its type's full health, at least 1
    int64_t build_reach{};   ///< how far from it what it builds may be, map pixels
    uint32_t first_sample{}; ///< its first entry in Analysis::life_samples
    uint32_t sample_count{}; ///< its entries there
};

/// The lives an event names.
struct EventLives {
    uint32_t unit{no_index};  ///< the life of TimelineEvent::unit
    uint32_t other{no_index}; ///< the life of TimelineEvent::other_unit
};

/// A long-range shot matched with its detonation.
struct LongRangeFire {
    uint32_t shot_tick{};
    uint32_t detonation_tick{};
    Point origin{}; ///< the muzzle
    Point impact{};
    uint16_t shooter{}; ///< the shooter's unit slot
    uint16_t weapon{};
    bool launched{}; ///< the weapon launches vertically or is stockpiled: a missile
    int64_t score{}; ///< its detonation's score
};

/// An event that weighs in a hot spot.
struct HotEvent {
    uint32_t tick{};
    int32_t cell_x{}; ///< hot_cell_pixels squares, held on the map
    int32_t cell_z{};
    int64_t score{};
    Point at{};
    uint32_t life{no_index};     ///< the unit hit or killed
    uint32_t attacker{no_index}; ///< the unit that hit it
    uint32_t fire{no_index};     ///< the long-range fire a detonation ends
};

/// What makes a moment; later kinds take the camera before earlier ones.
enum class MomentKind : uint8_t {
    launch,          ///< long-range fire launched too far from its target to show both
    impact,          ///< a long-range shell landing, after its launch
    big_blast,       ///< a detonation of at least big_blast_area, or one that shakes the view
    commander_death, ///< a commander died
};

/// A big moment, which may interrupt the shot.
struct Moment {
    uint32_t tick{};
    MomentKind kind{MomentKind::big_blast};
    Point at{};
    int64_t score{};
    /// The long-range fire whose launch or landing it is, as an index into
    /// Analysis::fires; no_index for none.
    uint32_t fire{no_index};
};

/// A mobile unit a factory finished.
struct Production {
    uint32_t tick{};    ///< the tick it was finished on
    uint32_t factory{}; ///< the factory's life
    uint32_t unit{};    ///< the unit's life
    uint32_t order{};   ///< the units the factory finished before it
    Point at{};         ///< where it was finished
};

/// What the planner reads from a timeline before it plans.
struct Analysis {
    const Timeline* timeline{};
    Stage stage{};
    uint32_t first_tick{};
    /// The first tick the plan may not show: the usable end, after the first tick.
    uint32_t end_tick{};
    /// Players with units, not watching and not the viewer, in index order.
    std::vector<uint8_t> combatants{};
    /// Each combatant's team: the smallest index of the players it is
    /// mutually allied with, itself included; no_player for the others.
    std::array<uint8_t, player_slots> team{};
    std::array<bool, player_slots> has_start{};
    std::array<Point, player_slots> start{};
    std::vector<const TimelineUnitType*> types{}; ///< by type index
    std::vector<const TimelineWeapon*>
        weapons{};             ///< by weapon id; empty when the header names none
    std::vector<Life> lives{}; ///< in order of birth
    /// The timeline's events before end_tick, as indices, in tick order.
    std::vector<uint32_t> event_order{};
    /// The timeline's samples before end_tick, as indices, by tick then slot.
    std::vector<uint32_t> sample_order{};
    /// The lives each event names, by the event's index in the timeline.
    std::vector<EventLives> event_lives{};
    /// The life of each sample, by the sample's index in the timeline.
    std::vector<uint32_t> sample_lives{};
    /// Sample indices grouped by life, each life's in tick order.
    std::vector<uint32_t> life_samples{};
    /// Each player's created events, as indices into the timeline's events.
    std::array<std::vector<uint32_t>, player_slots> created{};
    /// Each player's commanders, as life indices in order of birth.
    std::array<std::vector<uint32_t>, player_slots> commanders{};
    /// The first damage one combatant deals another's team; never for none.
    uint32_t engagement_tick{never};
    /// The commander death that decides the game; never for none.
    uint32_t deciding_tick{never};
    Point deciding_at{};
    int64_t deciding_score{};
    std::vector<LongRangeFire> fires{};
    std::vector<HotEvent> hot_events{}; ///< in tick order
    std::vector<Moment> moments{};      ///< in tick order
    int32_t cells_x{1};                 ///< hot spot grid columns
    int32_t cells_z{1};                 ///< hot spot grid rows
    /// Each player's buildings, as life indices in order of birth.
    std::array<std::vector<uint32_t>, player_slots> buildings{};
    /// Each player's mobile builders but its commanders, as life indices in
    /// order of birth.
    std::array<std::vector<uint32_t>, player_slots> builders{};
    /// The mobile units factories finished, in tick order.
    std::vector<Production> productions{};
    /// The water map's squares across and down; 0 for no map.
    int64_t water_columns{};
    int64_t water_rows{};
    /// The water map summed: entry (column, row) of (water_columns + 1)
    /// columns holds the percents of every square above and left of it.
    std::vector<int64_t> water_sums{};
};

/// Reads what the planner needs from a timeline.
///
/// @param timeline the timeline; must outlive the analysis
/// @param settings the plan's settings
/// @return the analysis
[[nodiscard]] Analysis analyse(const Timeline& timeline, const PlannerSettings& settings);

/// Tells whether a life is alive on a tick.
///
/// @param life the life
/// @param tick the tick
/// @return true from its birth up to, not including, its death
[[nodiscard]] bool alive(const Life& life, uint32_t tick) noexcept;

/// Returns a life's latest sample at or before a tick.
///
/// @param analysis the analysis
/// @param life the life's index
/// @param tick the tick
/// @return the sample, or null when it has none by then
[[nodiscard]] const UnitSample*
latest_sample(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept;

/// Returns where a life was on a tick: its latest sample, else where it was
/// created, or where it died once it has.
///
/// @param analysis the analysis
/// @param life the life's index
/// @param tick the tick
/// @return the ground point
[[nodiscard]] Point ground_point(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept;

/// Tells whether a unit is drawn on a tick: its latest sample shows it
/// drawn and not cloaked. A unit with no sample yet, a building among them,
/// counts as drawn.
///
/// @param analysis the analysis
/// @param life the life's index
/// @param tick the tick
/// @return true when it is drawn
[[nodiscard]] bool drawn(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept;

/// Tells whether a life may be framed on a tick: it is alive and drawn.
///
/// @param analysis the analysis
/// @param life the life's index
/// @param tick the tick
/// @return true when it may be framed
[[nodiscard]] bool framable(const Analysis& analysis, uint32_t life, uint32_t tick) noexcept;

/// Tells whether two players are combatants of different teams.
///
/// @param analysis the analysis
/// @param a a player index
/// @param b another
/// @return true when they fight each other
[[nodiscard]] bool enemies(const Analysis& analysis, uint8_t a, uint8_t b) noexcept;

/// Returns the team whose start lies nearest a point.
///
/// @param analysis the analysis
/// @param at the point
/// @return the team of the combatant whose start is nearest, the lower
///         player on a tie; no_player without a combatant
[[nodiscard]] uint8_t side_of(const Analysis& analysis, Point at) noexcept;

/// Returns how much of a view shows water.
///
/// @param analysis the analysis; its water map
/// @param view the view
/// @return 0 to 100, the percents of the water squares the view covers,
///         averaged; 0 for no water map
[[nodiscard]] int64_t water_percent(const Analysis& analysis, const PlannedView& view) noexcept;

/// Returns how much of the water square a point lies in is water.
///
/// @param analysis the analysis; its water map
/// @param at the point
/// @return 0 to 100, in percent; 0 off the map or for no water map
[[nodiscard]] int64_t water_at(const Analysis& analysis, Point at) noexcept;

/// Tells whether a point lies within the middle part of a view.
///
/// @param stage the stage
/// @param view the view
/// @param point the point
/// @param area_percent the middle part, in percent of each side
/// @return true when the point is within it on both axes
[[nodiscard]] bool within_part(
    const Stage& stage, const PlannedView& view, Point point, int64_t area_percent
) noexcept;

/// Returns a view moved, keeping its height, to show as little water as it
/// can while every point stays in the middle `keep_percent` of it.
///
/// The centres tried lie on an even grid of tuning::water_search_steps
/// along each axis of the range that keeps the points there; the one with
/// the least water wins, then the one nearest the view's own centre, then
/// the lower row and column. A centre that keeping the view on the map
/// moves so far that a point leaves that part is not tried.
///
/// @param analysis the analysis
/// @param points the points to keep; none keeps the view
/// @param view the view
/// @param keep_percent the middle part of the view the points stay in, at
///        most safe_area_percent
/// @return the view moved and kept on the map
[[nodiscard]] PlannedView settle_on_land(
    const Analysis& analysis,
    std::span<const Point> points,
    const PlannedView& view,
    int64_t keep_percent
);

/// Returns a unit type by its index.
///
/// @param analysis the analysis
/// @param type_index the type's index
/// @return the type, or null when the timeline does not name it
[[nodiscard]] const TimelineUnitType*
type_of(const Analysis& analysis, uint16_t type_index) noexcept;

/// Returns a weapon by its id.
///
/// @param analysis the analysis
/// @param weapon_id the weapon's id
/// @return the weapon, or null when the timeline does not name it
[[nodiscard]] const TimelineWeapon*
weapon_of(const Analysis& analysis, uint16_t weapon_id) noexcept;

/// What a subject is; on equal scores, earlier kinds are preferred.
enum class SubjectKind : uint8_t {
    hot,       ///< a hot spot: a, b its cell
    army,      ///< a player's leading advancing army: a the player
    barrage,   ///< long-range fire, framed wide
    commander, ///< a player's commander and its builds: a the player
    factory,   ///< a factory finishing a unit: a the player, b the factory's life
    builder,   ///< a construction unit and its builds: a the player, b its life
    base,      ///< a player's base, where it is building: a the player
    moment,    ///< a moment: a its index in Analysis::moments
    overview,  ///< the whole map, or the wide drift from one side to the other
    ending,    ///< the deciding commander death
};

/// Tells whether a subject is action: a hot spot, an army or long-range fire.
///
/// @param kind the subject's kind
/// @return true for action
[[nodiscard]] constexpr bool action_kind(SubjectKind kind) noexcept {
    return kind == SubjectKind::hot || kind == SubjectKind::army || kind == SubjectKind::barrage;
}

/// Tells whether a subject belongs to a player's side in the quiet turns.
///
/// @param kind the subject's kind
/// @return true for a commander, a factory, a construction unit or a base
[[nodiscard]] constexpr bool side_kind(SubjectKind kind) noexcept {
    return kind == SubjectKind::commander || kind == SubjectKind::factory ||
           kind == SubjectKind::builder || kind == SubjectKind::base;
}

/// Names a subject.
struct SubjectKey {
    SubjectKind kind{SubjectKind::overview};
    int32_t a{};
    int32_t b{};

    /// Tells whether two keys name the same subject.
    ///
    /// @param other the other key
    /// @return true when every member is equal
    [[nodiscard]] bool operator==(const SubjectKey& other) const noexcept = default;
};

/// Returns a subject's key as the plan's notes write it.
///
/// @param analysis the analysis
/// @param key the key
/// @return such as "commander:1", "army:2", "hot:4,7", "barrage",
///         "factory:0,12", "builder:1,40", "base:0", "moment:1234",
///         "overview" or "ending:5000"
[[nodiscard]] std::string key_text(const Analysis& analysis, const SubjectKey& key);

/// A player's leading army on a tick.
struct ArmyGroup {
    bool found{};
    int64_t score{};
    std::vector<Point> points{}; ///< its units, where they are and where they will be
    Point centre{};              ///< where its units are, on average
    Point heading{};             ///< their average move over army_look_ahead_ticks
};

/// Returns a player's advancing army with the largest score on a tick.
///
/// An army is a group of the player's armed mobile units, not builders or
/// commanders, drawn and free, each moving at least
/// army_min_advance_pixels toward the enemy start nearest it over the next
/// army_look_ahead_ticks, linked within group_link_pixels. Its score is its
/// units' value, weighed from half at its own start to whole at the enemy's.
///
/// @param analysis the analysis
/// @param player the player
/// @param tick the tick
/// @return the group; not found when no group of army_min_units or more
///         advances
[[nodiscard]] ArmyGroup best_army(const Analysis& analysis, uint8_t player, uint32_t tick);

/// Long-range fire landing soon, framed wide.
struct Barrage {
    bool found{};
    int64_t score{};             ///< its blasts' scores
    std::vector<Point> points{}; ///< its launches and impacts
};

/// Returns the long-range fire landing within tuning::barrage_window_ticks of
/// a decision, when there is enough of it.
///
/// @param analysis the analysis
/// @param tick the decision's tick
/// @return the barrage; not found for fewer than tuning::barrage_min_fires shots
[[nodiscard]] Barrage find_barrage(const Analysis& analysis, uint32_t tick);

/// A subject of a side's quiet turn and its score.
struct SideSubject {
    SubjectKey key{};
    int64_t score{};
};

/// Returns the subjects of a player's side on a tick: its commander, its
/// factory finishing the most valued unit soon, its construction unit
/// starting the most, and its base, each while it can be framed.
///
/// @param analysis the analysis
/// @param player the player
/// @param tick the tick
/// @return the subjects, in that order
[[nodiscard]] std::vector<SideSubject>
side_subjects(const Analysis& analysis, uint8_t player, uint32_t tick);

/// The hot spot of a decision.
struct HotSpot {
    bool found{};
    int32_t cell_x{};
    int32_t cell_z{};
    int64_t score{};
    uint32_t first_tick{}; ///< the first event of its neighbourhood in the window
};

/// The hot spots of a decision.
struct HotSpots {
    HotSpot best{}; ///< the cell whose neighbourhood scores highest
    /// The best cell whose neighbourhood does not overlap the avoided
    /// cell's; the best when no cell is avoided.
    HotSpot away{};
};

/// Tells whether two cells' neighbourhoods overlap.
///
/// @param first_x a cell's column
/// @param first_z its row
/// @param second_x another cell's column
/// @param second_z its row
/// @return true when the cells are at most twice hot_neighbourhood_cells
///         apart on both axes
[[nodiscard]] bool neighbourhoods_overlap(
    int32_t first_x, int32_t first_z, int32_t second_x, int32_t second_z
) noexcept;

/// Returns the cells whose neighbourhoods score highest over the look-ahead
/// window of a decision.
///
/// @param analysis the analysis
/// @param tick the decision's tick
/// @param avoid a cell whose overlapping neighbourhoods `away` leaves out;
///        none leaves out nothing
/// @param[in,out] grid scratch space, resized as needed
/// @return the hot spots; ties go to the lower row, then the lower column
[[nodiscard]] HotSpots find_hot_spots(
    const Analysis& analysis,
    uint32_t tick,
    const std::optional<std::array<int32_t, 2>>& avoid,
    std::vector<int64_t>& grid
);

/// Returns the life of a player's commander alive on a tick.
///
/// @param analysis the analysis
/// @param player the player
/// @param tick the tick
/// @return the life's index, or no_index for none
[[nodiscard]] uint32_t
commander_life(const Analysis& analysis, uint8_t player, uint32_t tick) noexcept;

/// Returns a subject's score on a tick.
///
/// @param analysis the analysis
/// @param key the subject
/// @param tick the tick
/// @return the score, 0 when the subject is gone
[[nodiscard]] int64_t subject_score(const Analysis& analysis, const SubjectKey& key, uint32_t tick);

/// A subject's framing: the view and the points it keeps in its safe area.
struct Framing {
    PlannedView view{};
    std::vector<Point> points{}; ///< the subject's points; none for a view of places
};

/// Returns a subject's framing on a tick.
///
/// @param analysis the analysis
/// @param key the subject
/// @param tick the tick
/// @param require_framable a commander must be framable to be framed
/// @param min_height the smallest height; 0 for the subject's own
/// @param show_launches a hot spot is framed from the launches of the
///        long-range fire landing there too, when one view shows both
/// @return the framing, or none when the subject is gone
[[nodiscard]] std::optional<Framing> frame_subject(
    const Analysis& analysis,
    const SubjectKey& key,
    uint32_t tick,
    bool require_framable,
    int64_t min_height = 0,
    bool show_launches = false
);

/// Returns the score of the damage, deaths and blasts landing in a span of
/// ticks, anywhere on the map.
///
/// @param analysis the analysis
/// @param from the first tick
/// @param to the last tick, included
/// @return the scores of the hot events of those ticks, summed
[[nodiscard]] int64_t damage_between(const Analysis& analysis, uint32_t from, uint32_t to);

} // namespace oa::media::director::planning
