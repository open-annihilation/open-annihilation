// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit selection for the local player: the on-screen visible-unit list, box
// and click selection, select-all / select-matching, squads, the observer
// pulse and the commander finder. Everything works on the canonical oa::World.
#pragma once

#include "oa/core/world.h"
#include "oa/data/defs/categories.hpp"
#include "oa/data/limits.hpp"

#include <cstdint>

namespace oa::sim::selection {

// Game.frame_flags bit requesting an order-panel/selection refresh.
inline constexpr uint16_t frame_flag_selection_changed = OA_FRAME_FLAG_REFRESH_ORDER_PANEL;

// Unit.flags byte 0 bits the "clear selection" path drops: selected (0x10),
// select-next visited (0x40) and 0x80.
inline constexpr uint32_t selection_clear_keep =
    ~(OA_UNIT_FLAG_SELECTED | OA_UNIT_FLAG_CYCLE_VISITED | OA_UNIT_FLAG_CYCLE_SKIP);
// The select-next marks only (0x40, 0x80).
inline constexpr uint32_t cycle_marks = OA_UNIT_FLAG_CYCLE_VISITED | OA_UNIT_FLAG_CYCLE_SKIP;

// Screen-space margins the game adds when projecting world positions into
// the game view (x + 128, z - y/2 + 32).
inline constexpr int32_t view_origin_x = 0x80;
inline constexpr int32_t view_origin_y = 0x20;

// Sentinel best score for the pointer pick.
inline constexpr int32_t pick_no_score = 0x7fff0000;
// Radar blips count as hit when the squared pointer distance is below this.
inline constexpr int32_t radar_pick_radius_sq = 4;
inline constexpr int32_t radar_pick_no_score = 99999;

// Category a squad key leaves out of the selection while the squad also
// holds an armed unit (the factories carry it).
inline constexpr const char* category_squad_key_skip = "CTRL_F";

// Speech category passed when one unit is picked.
inline constexpr uint32_t speech_selected = 1;
inline constexpr const char* sound_select_multiple = "SelectMultipleUnits";

// Unit-type bitmask (bit = UnitDef type id), as built by a category lookup.
using TypeMask = data::defs::CategoryMask;

// Services owned by other systems. Any pointer may be null: a null predicate
// answers "no", a null action does nothing.
struct Hooks {
    void* context{};
    // Line-of-sight footprint probe.
    bool (*player_sees_unit)(
        void* context, const World& world, const Player& player, const Unit& unit
    ){};
    // Pointer against the rotated model box (sim::gameplay_input::hits_root_bounds).
    bool (*pointer_hits_unit)(
        void* context, const World& world, const Unit& unit, int32_t x, int32_t y
    ){};
    // Plays a unit's speech of a category.
    void (*speak)(void* context, const Unit& unit, uint32_t category){};
    // Plays a named interface sound.
    void (*play_sound)(void* context, const char* name){};
    // Returns the armed command to the default order.
    void (*reset_command)(void* context){};
    // Order-panel rebuild after the selection is dropped.
    void (*selection_cleared)(void* context){};
    // present::world_renderer::camera_center_on_position.
    void (*center_camera)(void* context, const FixedVec3& position, bool immediate){};
    // Ends the camera follow.
    void (*stop_follow)(void* context){};
    // Moves the unit into the member list of `squad`.
    void (*set_squad)(void* context, Unit& unit, int32_t squad){};
};

// Caller-owned id buffers that Game.hot_units / hot_radar_units refer to,
// and where the radar the presentation drew sits on screen.
struct VisibleLists {
    uint16_t* units{}; // capacity >= World.unit_slot_count
    uint32_t unit_capacity{};
    const RadarHotUnit* radar{}; // radar_count entries
    uint32_t radar_capacity{};
    int32_t radar_count{};  // blips the radar listed: the entries of `radar` in use
    Rect32 radar_picture{}; // the radar's map picture on screen (Game.radar_picture_rect)
};

/// Returns the game view rectangle in screen pixels (Game.battlefield_rect).
///
/// @param world game record
/// @return inclusive pixel rectangle
[[nodiscard]] Rect32 game_view_rect(const World& world) noexcept;

/// Rebuilds the visible-unit list.
///
/// Lists every live unit whose model box overlaps the game view and that the viewpoint
/// player owns or can see; the count goes to Game.hot_unit_count.
///
/// @param[in,out] world units, players and game fields
/// @param lists caller-owned id buffers
/// @param hooks sight, sound and panel services
void collect_visible_units(World& world, const VisibleLists& lists, const Hooks& hooks);

/// Tests whether a unit is on screen: listed by the last collect_visible_units().
///
/// Only the first Game.hot_unit_count ids are read, and never more than the
/// buffer holds.
///
/// @param world game record holding the list's count
/// @param lists visible-unit list
/// @param unit unit id
/// @return true when the id is among the listed ones
[[nodiscard]] bool
unit_listed(const World& world, const VisibleLists& lists, uint16_t unit) noexcept;

/// Drops the selection and select-next marks of every unit, then lets the order panel rebuild.
///
/// @param[in,out] world units, players and game fields
/// @param hooks panel service
void clear_selection(World& world, const Hooks& hooks);

/// Selects every selectable unit of the local player.
///
/// @param[in,out] world units, players and game fields
/// @param hooks sight, sound and panel services
void select_all(World& world, const Hooks& hooks);

/// Adds every selectable local unit whose type matches an already selected one.
///
/// The selected types are gathered in a type bitset of `type_bits` type ids;
/// a unit whose type id is past it neither adds its type nor is added, as in
/// 3.1c past its 512 type ids.
///
/// @param[in,out] world units, players and game fields
/// @param hooks sight, sound and panel services
/// @param type_bits type ids the bitset holds (data::limits::UnitTypes::bitset_bits)
void select_matching_types(
    World& world, const Hooks& hooks, uint32_t type_bits = data::limits::base_type_bits
);

/// Selects selectable local units by type mask.
///
/// @param[in,out] world units, players and game fields
/// @param mask unit types to select
/// @param additive true adds to the selection; false also deselects the other units
/// @param hooks sight, sound and panel services
void apply_type_mask_selection(
    World& world, const TypeMask& mask, bool additive, const Hooks& hooks
);

/// Selects selectable local units of a type mask that are on screen.
///
/// A unit counts as on screen when `unit_listed` says the last collection
/// listed it. An additive call adds those units. Any other call also clears
/// the local player's other selectable units. Another player's units, and
/// units that cannot be selected, are left as they are.
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
/// @param mask unit types to select
/// @param additive true adds to the selection; false also deselects the other units
/// @param hooks sight, sound and panel services
void select_type_mask_on_screen(
    World& world, const VisibleLists& lists, const TypeMask& mask, bool additive, const Hooks& hooks
);

/// Replaces the selection with the local player's selectable units in the visible list.
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
/// @param hooks sight, sound and panel services
void select_visible_units(World& world, const VisibleLists& lists, const Hooks& hooks);

/// Drops the select-next marks of every unit.
///
/// @param[in,out] world units, players and game fields
void clear_cycle_marks(World& world) noexcept;

/// Returns the local player's next selected unit after another in unit id order.
///
/// Wraps round the player's id range and ends on `from` itself.
///
/// @param world units and players
/// @param from unit to start after; null or a foreign unit starts at the first id
/// @param backward true searches before `from` instead
/// @return the unit, or null
[[nodiscard]] Unit* next_selected_unit(World& world, const Unit* from, bool backward) noexcept;

/// Marks the local player's units of the visible list as visited by select-next.
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
void mark_visible_local(World& world, const VisibleLists& lists) noexcept;

/// Selects the units in the drag box between the two points the pointer tracker keeps (Game.drag_start and
/// Game.drag_end).
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
/// @param toggle true (shift) flips instead of replacing
/// @param hooks sight, sound and panel services
/// @return true when anything ends up selected
bool select_units_in_box(World& world, const VisibleLists& lists, bool toggle, const Hooks& hooks);

/// Selects the units a box test takes, as select_units_in_box selects the
/// units in the drag box: for a view that tests a unit where it drew it,
/// such as the rotated view. The selection is cleared first unless toggled,
/// the local units of the visible list are visited for select-next, and a
/// unit selected alone speaks, more than one play the selection sound.
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
/// @param toggle true (shift) flips instead of replacing
/// @param hooks sight, sound and panel services
/// @param in_box tells whether one of the local player's selectable units is in the box
/// @param context passed back to in_box
/// @return true when anything ends up selected
bool select_units_where(
    World& world,
    const VisibleLists& lists,
    bool toggle,
    const Hooks& hooks,
    bool (*in_box)(void* context, const Unit& unit),
    void* context
);

/// Selects the unit under the cursor (Game.cursor_unit_id).
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
/// @param toggle true (shift) flips instead of replacing
/// @param hooks sight, sound and panel services
void select_cursor_unit(World& world, const VisibleLists& lists, bool toggle, const Hooks& hooks);

/// Returns the id of the unit under the pointer.
///
/// The smallest visible unit whose model box is hit inside the game view, or the nearest
/// radar blip over the minimap, as `lists` places the radar and lists its blips.
///
/// @param world units and pointer position
/// @param lists visible-unit and radar lists
/// @param hooks pointer hit test
/// @return the unit id, or 0
[[nodiscard]] uint16_t
unit_under_pointer(const World& world, const VisibleLists& lists, const Hooks& hooks);

/// Returns the first live local unit without select-next marks.
///
/// When all are marked the marks are dropped and the scan restarts.
///
/// @param[in,out] world units, players and game fields
/// @return the unit, or null when the player has no units
[[nodiscard]] Unit* next_unmarked_unit(World& world);

/// Steps select-next: centres on the next unmarked unit and marks it visited.
///
/// @param[in,out] world units, players and game fields
/// @param lists visible-unit list
/// @param hooks camera service
void cycle_next_unit(World& world, const VisibleLists& lists, const Hooks& hooks);

/// Centres on the viewpoint player's commander.
///
/// @param[in,out] world units, players and game fields
/// @param select true to make it the only selected unit
/// @param hooks camera and sound services
void find_commander(World& world, bool select, const Hooks& hooks);

// Category Ctrl+C selects, and the category whose unit it has the camera follow.
inline constexpr const char* ctrl_c_category = "CTRL_C";
inline constexpr const char* commander_category = "Commander";

/// Sets the camera follow (Game.follow_unit) on the local player's last Commander-category unit.
///
/// The unit may be live or not; the follow is left unchanged when there is none.
///
/// @param[in,out] world units and camera follow
/// @param categories category registry for the Commander lookup
void follow_commander(World& world, data::defs::CategoryRegistry& categories);

/// Moves the camera follow (Game.follow_unit) to the local player's next selected unit.
///
/// @param[in,out] world units and camera follow
/// @param backward true moves to the one before the followed unit; with nothing selected
///        the follow ends
void follow_next_selected(World& world, bool backward) noexcept;

/// Runs the observer pulse.
///
/// Moves the selection from the viewpoint player's first selected unit to its next
/// selectable unit, wrapping to the first; with none selected, selects the first.
///
/// @param[in,out] world units, players and game fields
/// @param hooks sight, sound and panel services
void select_next_viewpoint_unit(World& world, const Hooks& hooks);

/// Handles Ctrl+digit: the local player's selected units join a squad.
///
/// Its unselected members fall back to squad 0.
///
/// @param[in,out] world units, players and game fields
/// @param squad squad number
/// @param hooks squad membership service
void assign_squad(World& world, int32_t squad, const Hooks& hooks);

/// Handles a digit: selects the local player's selectable units of a squad.
///
/// While the squad holds both a `skip` type and an armed unit, its `skip` members are
/// deselected instead. When that result is already exactly the selection, the camera
/// moves to the average of those units' map positions and the follow stops. The
/// average is the signed high words added, then divided by the count. 3.1c leaves
/// the camera alone; this is OA's. A press whose squad is not exactly the selection
/// does not move the camera.
///
/// @param[in,out] world units, players and game fields
/// @param squad squad number
/// @param add true keeps the rest of the selection; false deselects it
/// @param skip unit types left out while the squad has an armed unit
/// @param hooks sight, sound and panel services
/// @return whether any unit was selected
bool select_squad(World& world, int32_t squad, bool add, const TypeMask& skip, const Hooks& hooks);

/// Tests whether a squad has a selectable local unit whose type is in a mask.
///
/// @param world units and players
/// @param squad squad number
/// @param mask unit types
/// @return true when one does
[[nodiscard]] bool squad_has_type(const World& world, int32_t squad, const TypeMask& mask);

/// Tests whether a squad has a selectable local unit with weapons.
///
/// @param world units and players
/// @param squad squad number
/// @return true when one does
[[nodiscard]] bool squad_has_armed_unit(const World& world, int32_t squad);

} // namespace oa::sim::selection
