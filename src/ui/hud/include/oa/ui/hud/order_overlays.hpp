// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Queued-order overlays: build sites, range circles, target markers and the
// animated path pips drawn over the battlefield for the local player's units.
#pragma once

#include "oa/core/world.h"

#include <cstddef>
#include <cstdint>

namespace oa::ui::hud {

/// Screen-space offset of the battlefield inside the 640x480 HUD frame (the
/// order panel on the left, the resource bar on top).
inline constexpr int32_t kBattlefieldLeft = 0x80;
inline constexpr int32_t kBattlefieldTop = 0x20;

/// Overlay kinds a mission draws (MissionOverlay.mask bits).
inline constexpr uint32_t kOverlayBuildSite = 0x01;
inline constexpr uint32_t kOverlayPath = 0x02;
inline constexpr uint32_t kOverlayTargetRing = 0x04;
inline constexpr uint32_t kOverlayTarget = 0x08;
inline constexpr uint32_t kOverlayRanges = 0x10;
inline constexpr uint32_t kOverlayAll = 0x1f;

/// OrderOverlay.flags bit: the target's position was latched into seen_x/z.
inline constexpr uint32_t kOrderTargetSeen = 0x00200000u;
/// OrderOverlay.state bit: the order builds a stockpiled weapon.
inline constexpr uint8_t kOrderStockpileBuild = 0x08u;

/// Unit.state_flags bit tested before drawing the minimum cloak distance. /* ? cloaked */
inline constexpr uint8_t kUnitStateCloaked = 0x04u;

/// Game.ui_colors slots used by the overlays.
enum UiColor : uint8_t {
    kUiColorSiteOuter = 1,
    kUiColorSiteOuterSelected = 3,
    kUiColorRangeBlink = 4,
    kUiColorSiteInner = 9,
    kUiColorSiteInnerSelected = 10,
    kUiColorRange = 12,
    kUiColorDebugRange = 14,
    kUiColorCloakRange = 15,
};

/// One queued order as the overlays read it.
struct OrderOverlay {
    uint8_t mission{};     // mission kind, the index into kMissionOverlays
    Unit* unit{};          // ordered unit
    Unit* target{};        // target unit, or null
    FixedVec3 position;    // target point
    int16_t seen_x{};      // where the target was last seen, map pixels
    int16_t seen_z{};      // where the target was last seen, map pixels
    uint32_t parameter{};  // build type (low word) or weapon slot
    int32_t progress{};    // stockpile build progress, out of the weapon's reload_time
    uint32_t flags{};      // order flags; kOrderTargetSeen
    uint8_t state{};       // order state; kOrderStockpileBuild
    uint32_t issue_tick{}; // tick the order was issued
    OrderOverlay* next{};  // next order of the queue, or null
};

/// What a mission kind draws: its overlay kinds and target sprite.
struct MissionOverlay {
    uint32_t mask{};     // kOverlay* kinds
    uint8_t indicator{}; // target sprite sequence; 0 none
};

/// Overlay kinds and target sprite (a cursor sequence) of each of the 68
/// mission kinds, which the game numbers in the order of their names. Kinds
/// past the 68 draw nothing.
inline constexpr std::size_t kMissionOverlayCount = 256;
inline constexpr MissionOverlay kMissionOverlays[kMissionOverlayCount] = {
    {0x00, 15}, //  0
    {0x00, 19}, //  1 Activate
    {0x08, 2},  //  2 AirStrike
    {0x08, 1},  //  3 AirToAir
    {0x08, 1},  //  4 AirToGround
    {0x08, 1},  //  5 AirToGroundHover
    {0x08, 1},  //  6 Attack_Chase
    {0x08, 1},  //  7 Attack_Kamikaze
    {0x08, 1},  //  8 Attack_NoMove
    {0x08, 1},  //  9 AttackSpecial
    {0x00, 19}, // 10 AttackUType
    {0x00, 19}, // 11 BeCarried
    {0x00, 19}, // 12 BuildingBuild
    {0x00, 19}, // 13 BuildWeapon
    {0x08, 4},  // 14 Capture
    {0x00, 19}, // 15 Cloak_Off
    {0x00, 19}, // 16 Cloak_On
    {0x00, 19}, // 17 Deactivate
    {0x12, 5},  // 18 Follow_Ground
    {0x00, 19}, // 19 GetBuilt
    {0x08, 12}, // 20 Ground_Pickup
    {0x08, 13}, // 21 Ground_Unload
    {0x00, 19}, // 22 Guard_NoMove
    {0x18, 6},  // 23 HelpBuild
    {0x00, 19}, // 24 MakeSelectable
    {0x13, 0},  // 25 MobileBuild
    {0x12, 14}, // 26 Move_Ground
    {0x00, 19}, // 27 Paralyze
    {0x00, 14}, // 28 Park
    {0x12, 7},  // 29 Patrol
    {0x02, 14}, // 30 QMove
    {0x02, 7},  // 31 QPatrol
    {0x12, 11}, // 32 Reclaim
    {0x12, 11}, // 33 ReclaimUnit
    {0x12, 7},  // 34 RepairPatrol
    {0x12, 6},  // 35 RepairUnit
    {0x18, 6},  // 36 RepairUnitNoMove
    {0x12, 11}, // 37 Resurrect
    {0x00, 19}, // 38 SelfDestruct
    {0x00, 19}, // 39 SelfDestructFG
    {0x00, 19}, // 40 SelfRepair
    {0x10, 15}, // 41 Standby
    {0x10, 15}, // 42 Standby_Mine
    {0x00, 19}, // 43 Standing_FireOrder
    {0x00, 19}, // 44 Standing_MoveOrder
    {0x00, 19}, // 45 Stop
    {0x08, 1},  // 46 Suppress
    {0x08, 9},  // 47 Teleport
    {0x00, 19}, // 48 VTOL_Evade
    {0x02, 5},  // 49 VTOL_Follow
    {0x00, 19}, // 50 VTOL_GetRepaired
    {0x08, 6},  // 51 VTOL_HelpBuild
    {0x00, 19}, // 52 VTOL_LandIfCan
    {0x08, 14}, // 53 VTOL_Landing
    {0x03, 0},  // 54 VTOL_MobileBuild
    {0x02, 14}, // 55 VTOL_Move
    {0x02, 7},  // 56 VTOL_Patrol
    {0x08, 8},  // 57 VTOL_Pickup
    {0x02, 11}, // 58 VTOL_Reclaim
    {0x02, 11}, // 59 VTOL_ReclaimUnit
    {0x02, 7},  // 60 VTOL_RepairPatrol
    {0x02, 6},  // 61 VTOL_RepairUnit
    {0x00, 19}, // 62 VTOL_SeekAttack
    {0x00, 19}, // 63 VTOL_SeekGuard
    {0x00, 15}, // 64 VTOL_Standby
    {0x08, 9},  // 65 VTOL_Unload
    {0x00, 19}, // 66 Wait
    {0x00, 19}, // 67 WaitForAttack
};

/// Frame count and ticks-per-frame of a sprite sequence.
struct SpriteSequence {
    uint16_t frame_count{};
    uint16_t rate{};
};

enum class OverlaySprite : uint8_t { path_pip, indicator };

/// Drawing and terrain services the overlays use.
struct OverlaySink {
    void* user{};
    void (*line)(void* user, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color){};
    /// Label text, drawn unclipped (width -1).
    void (*label)(void* user, const char* text, int32_t x, int32_t y){};
    /// One frame of the path-pip sequence or of indicator sequence `index`.
    void (*sprite)(
        void* user, OverlaySprite sprite, uint8_t index, uint16_t frame, int32_t x, int32_t y
    ){};
    /// Terrain height (integer part) under a point.
    int32_t (*ground_height)(void* user, const FixedVec3& point){};
    /// Whether `viewer` currently sees `unit`.
    bool (*can_see)(void* user, const Player* viewer, const Unit& unit){};
    /// First order of a unit's primary (or secondary) queue.
    OrderOverlay* (*orders)(void* user, const Unit& unit, bool secondary){};
    /// Where a unit shows in the frame drawn, which may lie part of the way
    /// from its place at the tick before (a frame between two ticks); the
    /// overlays start their paths, centre their ranges and find their target
    /// units there. Null takes Unit.position. Whether a target is seen, and
    /// where it was last seen, still follows Unit.position.
    FixedVec3 (*place)(void* user, const Unit& unit){};
};

struct ScreenPoint {
    int32_t x{}, y{};
};

/// The battlefield view the overlays are projected into.
struct OverlayView {
    const Unit* focus_unit{}; // unit the cursor rests on in this view, or null
    int32_t camera_x{};       // map pixels
    int32_t camera_y{};
    /// Optional world projection; null retains the classic HUD coordinates.
    void* projection_user{};
    ScreenPoint (*project)(void* user, const FixedVec3& point){};
};

struct OverlayContext {
    World* world{};
    OverlayView view;
    OverlaySink sink;
    const MissionOverlay* missions{}; // 256 entries, by mission index
    SpriteSequence path_pips;
    const SpriteSequence* indicators{}; // by MissionOverlay.indicator
    bool show_ranges{};                 // debug range display (Game.show_ranges)
    /// ui.interface-fixes range-ring-3: the third weapon's range circle is
    /// gated on its own enable bit, not the first weapon's.
    bool third_ring_own_weapon{};
    /// ui.interface-fixes radius-0: a circle too small for one segment draws
    /// its label alone instead of nothing.
    bool segmentless_circle_label{};
};

/// Projects a world point onto the screen: x across, z up the screen lifted by half the height.
///
/// @param view Camera position of the battlefield view.
/// @param point World point, 16.16 fixed-point per axis.
/// @return Screen pixel position inside the HUD frame.
[[nodiscard]] ScreenPoint overlay_project(const OverlayView& view, const FixedVec3& point) noexcept;

/// The queued building's rectangle in HUD-frame pixels, before its sides close in.
///
/// The same rectangle draw_build_site_overlay starts from: the type's model
/// bounds around the order position, lifted by half the low edge's height.
struct BuildSiteBounds {
    uint32_t left{};
    uint32_t top{};
    uint32_t right{};
    uint32_t bottom{};
};

/// Fills the queued building's rectangle in HUD-frame pixels.
///
/// @param view Camera position of the battlefield view.
/// @param def The building type; its model bounds size the rectangle.
/// @param position The order's site, 16.16 fixed-point per axis.
/// @param[out] bounds The rectangle, including the battlefield's offset in the HUD frame.
void build_site_overlay_bounds(
    const OverlayView& view, const UnitDef& def, const FixedVec3& position, BuildSiteBounds& bounds
) noexcept;

/// Draws the footprint box of a queued building that closes in over ten ticks.
///
/// The box starts at the type's model bounds around the order position and
/// its sides move in by a tenth of the box per tick of order age. Outer lines
/// use UI colour 1 and inner lines 9, or 3 and 10 while the ordered unit is
/// selected. Nothing is drawn for an order without a known build type.
///
/// @param context World, view and drawing services.
/// @param order Queued build order; the low word of parameter is the type.
/// @param[out] point Receives the order position when the box is drawn.
void draw_build_site_overlay(
    const OverlayContext& context, const OrderOverlay& order, FixedVec3& point
);

/// Draws a terrain-following circle, optionally labelled near segment `label_slot * 3`.
///
/// The circle has one segment per 8 pixels of circumference; each point sits
/// on the terrain but never below the centre. A label goes 4 pixels below the
/// end of segment `label_slot * 3`, or below the circle's last point when that
/// segment lands on (0, 0).
///
/// @param context World, view and drawing services.
/// @param center Circle centre, 16.16 fixed-point per axis.
/// @param radius Radius in pixels; under 2 draws no circle, and no label
///        either unless OverlayContext::segmentless_circle_label is set.
/// @param color Palette index of the lines.
/// @param label Label text, or null for none.
/// @param label_slot Which third of the segments the label follows.
void draw_range_circle(
    const OverlayContext& context,
    const FixedVec3& center,
    uint32_t radius,
    uint8_t color,
    const char* label,
    int32_t label_slot
);
/// Draws the ordered unit's range circles.
///
/// In play: the minimum cloak distance (UI colour 15) while the unit is
/// cloaked, and for a kamikaze unit a blast circle pulsing up to half its
/// explosion's area of effect over 60 ticks (at least 8 pixels) plus its reach
/// (sight distance without movement, kamikaze distance otherwise) in UI
/// colour 12. With the debug range display: labelled circles for the minimum
/// cloak, sight, radar, sonar, jammer, build, maneuver and kamikaze distances
/// in UI colour 14 and the weapon ranges blinking between colours 12 and 4.
///
/// @param context World, view, drawing services and the range display switch.
/// @param order Order whose unit's ranges are drawn.
/// @quirk The third weapon's range circle is gated on the first weapon's enable bit.
void draw_unit_ranges(const OverlayContext& context, const OrderOverlay& order);

/// Resolves the order's target point and draws the mission's target marker there.
///
/// A target unit the ordering player sees gives its position, which is
/// latched into seen_x/seen_z (kOrderTargetSeen); one it does not see gives
/// the latched cell once there is one. Without a target unit the order
/// position is used. The marker is the mission's indicator sequence, a frame
/// every two `rate` ticks. With the range display and indicator 1 or 2 the
/// weapon area-of-effect, coverage and attack-run circles surround the target.
///
/// @param context World, view, sprite sequences and drawing services.
/// @param[in,out] order Order drawn; its seen latch is updated.
/// @param[out] point Receives the target point.
void draw_order_target(const OverlayContext& context, OrderOverlay& order, FixedVec3& point);

/// Draws the target marker and, when animating, pips sliding from the previous point to it.
///
/// Pips sit every 48 pixels along the line and slide forward over 30 ticks of
/// order age; their frames advance one per pip. Lines shorter than one pixel
/// get no pips.
///
/// @param context World, view, sprite sequences and drawing services.
/// @param[in,out] order Order drawn; its seen latch is updated.
/// @param[in,out] point In: the previous order's point; out: this order's target point.
/// @param animate Whether to draw the pips.
void draw_order_path(
    const OverlayContext& context, OrderOverlay& order, FixedVec3& point, bool animate
);

/// Draws a 16-segment flattened ring around the target unit, or a 32-pixel ring at the order position.
///
/// The ring's radius is the target type's size_x (integer part), squashed to
/// 0.89 vertically, in UI colour 12.
///
/// @param context World, view and drawing services.
/// @param order Order whose target is ringed.
/// @param[out] point Receives the ring centre.
void draw_target_ring(const OverlayContext& context, const OrderOverlay& order, FixedVec3& point);

/// Draws the overlays in `mask` for every order in the unit's primary queue.
///
/// Each order draws the kinds its mission has in `mask`, starting from the
/// point the previous order ended on (the unit's position for the first).
/// Range circles are drawn once, for the first order that has them.
///
/// @param context World, view, mission table and drawing services.
/// @param unit Unit whose queue is walked.
/// @param mask kOverlay* kinds to draw.
/// @param animate Whether path pips are drawn.
void draw_unit_order_overlays(
    const OverlayContext& context, const Unit& unit, uint32_t mask, bool animate
);

/// Computes how far the unit's stockpile build has come, as a percentage of its weapon's reload time.
///
/// @param context World and the order-queue service.
/// @param unit Unit whose secondary queue is searched for a stockpile build order.
/// @return progress * 100 / reload_time of the first stockpile build order; 0
///         without one, for an out-of-range weapon slot or a zero reload time.
[[nodiscard]] int32_t stockpile_percent(const OverlayContext& context, const Unit& unit);

/// Draws order overlays for the local player's live units.
///
/// The focused, panel and cursor units draw everything with animated pips;
/// other selected units draw everything without pips; the rest draw build
/// sites only while one of the focused, panel or cursor units is a builder.
///
/// @param context World, view, mission table and drawing services.
void draw_selection_overlays(const OverlayContext& context);

} // namespace oa::ui::hud
