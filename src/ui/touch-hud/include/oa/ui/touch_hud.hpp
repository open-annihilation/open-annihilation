// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch controls' model: the device class, the tablet and phone layouts
// in points, the QUEUE, ADD and x5 latches, the radial order menu, the sheets,
// the banner, the status line, the tip and the hit tests, and the gamepad's
// part of them: the slim pad HUD, the FORCE chip, the button badges, the
// build and group rings and the pad's hints. Pure: no SDL and no runtime
// (docs/touch-controls.md). Geometry is in canvas pixels, computed from
// points with Viewport::px_per_point.
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include "oa/ui/display_layout.hpp"
#include "oa/ui/pad_controls.hpp"

namespace oa::ui::touch_hud {

using oa::ui::display_layout::Insets;
using oa::ui::display_layout::MatchLayout;
using oa::ui::display_layout::Point;
using oa::ui::display_layout::Rect;

/// Window classes the controls are laid out for.
enum class DeviceClass : uint8_t {
    tablet, ///< the 3.1c HUD with the thumb column, group bar and right rail beside it
    phone,  ///< the compact layout over a full-bleed battlefield
};
/// A window whose shorter side is under this many points is a phone.
inline constexpr int phone_short_side_points = 460;
/// Returns the class of a window of this size in points (either orientation).
///
/// @param width_points window width in points
/// @param height_points window height in points
/// @return phone when the shorter side is under phone_short_side_points, else tablet
[[nodiscard]] DeviceClass classify_device(int width_points, int height_points) noexcept;

/// Distances the touch rules use, in points.
inline constexpr float unit_pick_points = 12.0f;   ///< nearest unit to a tap on the battlefield
inline constexpr float gadget_pick_points = 22.0f; ///< nearest gadget or control in menus and HUD
inline constexpr float edge_scroll_points =
    32.0f; ///< auto-scroll band while dragging a box or ghost
/// How long SELF-DESTRUCT · HOLD must be held, in milliseconds.
inline constexpr uint32_t self_destruct_hold_ms = 1000;

/// The canvas the controls are laid out on.
struct Viewport {
    int width{};                             ///< canvas pixels
    int height{};                            ///< canvas pixels
    float px_per_point{1.0f};                ///< canvas pixels per point
    Insets safe{};                           ///< canvas pixels to keep clear on each side
    DeviceClass device{DeviceClass::tablet}; ///< the layout to use
    bool left_handed{};                      ///< mirror the controls left to right
    /// The match layout the overlays go beside: on a tablet its chrome (left, top, bottom,
    /// hud_width, bottom_bar_y()); on a phone its battlefield is the whole canvas.
    MatchLayout chrome{};
};

/// The latching modifiers.
enum class Latch : uint8_t {
    queue,      ///< QUEUE: Shift for orders
    add,        ///< ADD: Shift for selection
    times_five, ///< x5: Shift for build buttons
};
/// The number of latches.
inline constexpr std::size_t latch_count = 3;
/// Whether a latch stays on after use.
enum class LatchMode : uint8_t {
    stay_on,    ///< a latched latch stays on until tapped again
    one_action, ///< a latched latch turns off after the first action that uses it
};
/// The class of an action, which says which latch is its Shift.
enum class ActionClass : uint8_t {
    selection,    ///< selecting: taps on own units, boxes, group recall
    order,        ///< orders, area orders and placement
    build_button, ///< a build button's +1 or -1
};
/// Returns the latch that gives Shift to a class: ADD, QUEUE or x5.
///
/// @param action the class of the action
/// @return add for selection, queue for order, times_five for build_button
[[nodiscard]] Latch latch_for(ActionClass action) noexcept;

/// QUEUE, ADD and x5: tap to latch, hold for as long as the finger rests.
class Latches {
  public:

    /// A finger pressed the latch's control at now_ms: active at once.
    ///
    /// @param latch the latch pressed
    /// @param now_ms the time in milliseconds
    void press(Latch latch, uint64_t now_ms) noexcept;
    /// The finger lifted. A press shorter than hold_ms during which no action used the latch
    /// toggles the latch; a longer press, or one an action used, only ends the hold.
    ///
    /// @param latch the latch released
    /// @param now_ms the time in milliseconds
    /// @param hold_ms the hold delay in milliseconds
    void release(Latch latch, uint64_t now_ms, uint32_t hold_ms) noexcept;
    /// The finger slid off or was cancelled: ends the hold without toggling.
    ///
    /// @param latch the latch whose hold ends
    void cancel(Latch latch) noexcept;
    /// An action of `action`'s class used its latch: marks a held press as used and, in
    /// one_action mode, turns the latch off (a held press stays active until release).
    ///
    /// @param action the class of the action that used its latch
    /// @param mode whether latches stay on after use
    void used(ActionClass action, LatchMode mode) noexcept;
    /// Turns every latch off and ends every hold.
    void clear() noexcept;
    /// Returns whether the latch gives Shift now (latched or held).
    ///
    /// @param latch the latch
    /// @return whether it is latched or held
    [[nodiscard]] bool active(Latch latch) const noexcept;
    /// Returns whether the latch is latched (drawn lit and marked "latched").
    ///
    /// @param latch the latch
    /// @return whether it is latched
    [[nodiscard]] bool latched(Latch latch) const noexcept;
    /// Returns whether a finger holds the latch.
    ///
    /// @param latch the latch
    /// @return whether it is held
    [[nodiscard]] bool held(Latch latch) const noexcept;

  private:

    /// One latch's state.
    struct State {
        bool latched{};         ///< on after a tap
        bool held{};            ///< a finger rests on its control
        bool used_while_held{}; ///< an action used it during the current press
        uint64_t pressed_ms{};  ///< when the current press began, in milliseconds
    };

    std::array<State, latch_count> states_{}; ///< by Latch
};

/// The orders the rail, the radial and the MORE sheet offer.
enum class Order : uint8_t {
    move,    ///< MOVE
    attack,  ///< ATTACK
    patrol,  ///< PATROL
    guard,   ///< GUARD (the order panel's DEFEND)
    stop,    ///< STOP, given at once
    blast,   ///< D-GUN (the order panel's BLAST)
    reclaim, ///< RECLAIM
    repair,  ///< REPAIR (assist)
    capture, ///< CAPTURE
    load,    ///< LOAD
    unload,  ///< UNLOAD
};
/// The number of orders.
inline constexpr std::size_t order_count = 11;
/// Returns the order panel name the engine's arm_match_command and order_command_available take:
/// MOVE, ATTACK, PATROL, DEFEND, STOP, BLAST, RECLAIM, REPAIR, CAPTURE, LOAD, UNLOAD.
///
/// @param order the order
/// @return its order panel name
[[nodiscard]] std::string_view order_name(Order order) noexcept;
/// Returns the label drawn for an order: MOVE, ATTACK, PATROL, GUARD, STOP, D-GUN, ...
///
/// @param order the order
/// @return its label, untranslated
[[nodiscard]] std::string_view order_label(Order order) noexcept;

/// What a tap at a point would do now, from the engine's order cursor.
enum class TapAction : uint8_t {
    none,    ///< nothing
    select,  ///< selects the unit
    move,    ///< moves there
    attack,  ///< attacks
    guard,   ///< guards the unit
    patrol,  ///< patrols there
    repair,  ///< repairs the unit
    assist,  ///< helps build the unit
    reclaim, ///< reclaims the feature or unit
    capture, ///< captures the unit
    load,    ///< loads the unit
    unload,  ///< unloads there
    blast,   ///< fires the D-gun
    build,   ///< builds the armed building
    place,   ///< moves the ghost while placing
};
/// Returns the class of a tap that does `action` (select: selection, the rest: order).
///
/// @param action what the tap does
/// @return its class
[[nodiscard]] ActionClass action_class(TapAction action) noexcept;

/// The radial menu's items, in slot order (blast and load share slot 3).
enum class RadialItem : uint8_t {
    move,    ///< slot 0, at the top
    patrol,  ///< slot 1
    attack,  ///< slot 2
    blast,   ///< slot 3 when the selection has a D-gun
    load,    ///< slot 3 otherwise
    capture, ///< slot 4
    stop,    ///< slot 5
    info,    ///< slot 6: the unit info panel
    type,    ///< slot 7: every unit of that type
    reclaim, ///< slot 8
    repair,  ///< slot 9 (assist)
    unload,  ///< slot 10
    guard,   ///< slot 11
};
/// The number of wedges the radial has.
inline constexpr std::size_t radial_slot_count = 12;
/// Returns the order a radial item arms; none for info and type.
///
/// @param item the radial item
/// @return the order it arms, or none
[[nodiscard]] std::optional<Order> radial_order(RadialItem item) noexcept;

/// One wedge of the open radial.
struct RadialWedge {
    RadialItem item{RadialItem::move}; ///< what the wedge gives
    bool available{};    ///< drawn greyed and ignored when false; keeps its slot either way
    bool default_item{}; ///< ringed: what a plain tap at the point would give
    Rect hit{};          ///< canvas rectangle around the wedge's label, at least 44 pt
    Point label{};       ///< centre of the wedge's icon and label
};

/// The open radial menu.
struct Radial {
    Point anchor{};     ///< the held point, where the order is given
    Point centre{};     ///< the ring's centre: the anchor moved inside the safe area
    int inner_radius{}; ///< canvas pixels (44 pt)
    int outer_radius{}; ///< canvas pixels (128 pt)
    std::array<RadialWedge, radial_slot_count> wedges{}; ///< by slot, clockwise from the top
    bool queue_hub{};       ///< the hub was tapped: the next pick is queued (one-shot Shift)
    uint16_t target_unit{}; ///< the unit under the anchor when it opened, 0 for none
};

/// What each radial item's availability is, set by the app from the selection.
struct RadialAvailability {
    std::array<bool, order_count> orders{};   ///< by Order
    bool info{};                              ///< Info: a unit under the point or one selected
    bool type{};                              ///< Type: an own unit under the point
    bool blast_slot_shows_blast{};            ///< else the shared slot shows Load
    std::optional<RadialItem> default_item{}; ///< what a plain tap would give, ringed
};

/// Lays out the radial at a point, keeping the ring inside the safe area.
///
/// @param anchor the held point, canvas pixels
/// @param availability which items the selection can take
/// @param viewport the canvas
/// @return the radial
[[nodiscard]] Radial make_radial(
    Point anchor, const RadialAvailability& availability, const Viewport& viewport
) noexcept;
/// Returns the wedge under a point (nearest within gadget_pick_points), if any.
///
/// @param radial the open radial
/// @param point canvas pixels
/// @param viewport the canvas
/// @return the item of the wedge under the point, or none
[[nodiscard]] std::optional<RadialItem>
radial_hit(const Radial& radial, Point point, const Viewport& viewport) noexcept;
/// Returns whether a point is on the radial's hub.
///
/// @param radial the open radial
/// @param point canvas pixels
/// @return whether the point lies inside the inner radius
[[nodiscard]] bool radial_hub_hit(const Radial& radial, Point point) noexcept;

/// Sheets and menus that open over the battlefield; one at a time.
enum class Sheet : uint8_t {
    none,        ///< no sheet
    select_menu, ///< SELECT ▾
    speed,       ///< the tablet's speed popover
    phone_menu,  ///< the phone's MENU
    more,        ///< the phone's MORE sheet
    drawer,      ///< the phone's build drawer
};
/// SELECT ▾ items, in order.
enum class SelectItem : uint8_t {
    all,         ///< Ctrl+A
    builders,    ///< Ctrl+B
    factories,   ///< Ctrl+F
    aircraft,    ///< Ctrl+V
    on_screen,   ///< Ctrl+S
    commander,   ///< Ctrl+C
    same_type,   ///< Ctrl+Z
    centre,      ///< Home
    follow,      ///< T
    next_unit,   ///< N
    next_report, ///< F3
};
/// The number of SELECT ▾ items.
inline constexpr std::size_t select_item_count = 11;
/// Speed popover items.
enum class SpeedItem : uint8_t {
    slower, ///< the game speed one step down
    faster, ///< the game speed one step up
};
/// Phone menu items.
enum class PhoneMenuItem : uint8_t {
    game_menu,  ///< the in-game menu, as F2
    slower,     ///< the game speed one step down
    faster,     ///< the game speed one step up
    chat,       ///< the chat line
    photo_mode, ///< photo mode
};
/// MORE sheet touch items, in its last row (the order-page gadgets above are placed regions).
enum class MoreItem : uint8_t {
    info, ///< the unit info panel for the primary selected unit (as the tablet layout's INFO)
    self_destruct, ///< SELF-DESTRUCT · HOLD: toggles the countdown after self_destruct_hold_ms
};
/// Drawer tabs.
enum class DrawerTab : uint8_t {
    build,  ///< the builder's build pages
    orders, ///< the orders page
};

/// Every touch control.
enum class Control : uint8_t {
    none,              ///< no control
    queue,             ///< QUEUE
    add,               ///< ADD
    times_five,        ///< x5
    clear,             ///< CLEAR
    select_menu,       ///< SELECT ▾
    group_store,       ///< STORE (tablet) or + (phone)
    group_chip,        ///< a stored group's chip; index: the group 1..9
    pause,             ///< PAUSE
    speed,             ///< SPEED (tablet)
    chat,              ///< CHAT (shared games)
    centre,            ///< CENTRE
    follow,            ///< FOLLOW
    next_unit,         ///< NEXT
    info,              ///< INFO (tablet)
    menu,              ///< MENU
    build_drawer,      ///< BUILD or ORDERS (phone)
    zoom_out,          ///< zoom − (phone)
    zoom_in,           ///< zoom + (phone)
    order_slot,        ///< a rail slot; index: the slot
    more,              ///< MORE (phone)
    drawer_build_tab,  ///< the drawer's BUILD tab
    drawer_orders_tab, ///< the drawer's ORDERS tab
    drawer_close,      ///< the drawer's ✕
    drawer_prev,       ///< the drawer's PREV
    drawer_next,       ///< the drawer's NEXT
    place_cancel,      ///< ✕ CANCEL
    banner_cancel,     ///< the banner's ✕
    menu_item,         ///< index: SelectItem, SpeedItem or PhoneMenuItem by the open sheet
    more_item,         ///< index: MoreItem
    radial_item,       ///< index: RadialItem
    radial_hub,        ///< the radial's QUEUE hub
    sheet_outside,     ///< anywhere outside an open sheet or radial: closes it, taken
    force,             ///< FORCE (shown with the pad HUD or badges when the pad has grips)
    build_wedge,       ///< a wedge of the build ring; index: its slot 0..7
    group_wedge,       ///< a wedge of the left pad's group ring; index: the group 1..9
};

/// One laid-out control.
struct ControlRect {
    Control control{Control::none}; ///< which control
    uint8_t index{};                ///< group number 1..9, rail slot, menu item, radial item
    Rect rect{};                    ///< canvas pixels
};

/// The most controls a frame lays out: the tablet's with FORCE, a sheet, the radial, the build
/// ring and the group ring at once.
inline constexpr std::size_t max_controls = 96;
/// The most cells the phone's build drawer shows.
inline constexpr std::size_t max_drawer_cells = 12;
/// The most cells the phone's MORE sheet shows for order-page gadgets.
inline constexpr std::size_t max_more_cells = 12;
/// The most slots the phone's order rail has.
inline constexpr std::size_t max_rail_slots = 8;
/// The most controls drawn pressed at once (one per resting finger).
inline constexpr std::size_t max_pressed = 2;

/// A control a finger rests on: drawn with the pressed look.
struct Pressed {
    Control control{Control::none}; ///< none: this entry is unused
    uint8_t index{};                ///< as ControlRect::index
};

/// One phone rail slot: an order, or MORE in the last slot.
struct RailSlot {
    Order order{Order::move}; ///< the order the slot arms, unless more
    bool more{};              ///< the slot is MORE
    bool available{};         ///< the selection can take the order
    bool lit{};               ///< the armed order
};

/// The armed order banner and the placement header. Its title is banner_title's.
struct Banner {
    bool shown{}; ///< the banner shows
    /// the armed order the title names ("PATROL armed"); none while a building is placed, whose
    /// title names Placement::name ("Place Solar Collector")
    std::optional<Order> order{};
    std::string hint; ///< "TAP POINTS · QUEUE KEEPS ADDING", "DOUBLE TAP OR HOLD TO BUILD"
};

/// The tip bubble a long press shows.
struct Tip {
    std::string text;    ///< the help line, untranslated
    Point anchor{};      ///< canvas pixels the bubble points at
    uint64_t until_ms{}; ///< hidden from then; 0 = hidden
};

/// Touch placement of a building.
struct Placement {
    bool active{};    ///< a building is being placed
    bool legal{};     ///< the site at the anchor can be built on
    Point anchor{};   ///< ghost anchor in canvas pixels (the site a hold on the ghost builds at)
    std::string name; ///< the building's name for the header, as the bottom bar names it
};

/// The build ring's wedges, clockwise from the top: six build buttons at N, NE, SE, S, SW, NW,
/// PREV at W and NEXT at E.
inline constexpr std::size_t build_ring_slot_count = 8;
/// The build ring's NEXT wedge (E).
inline constexpr uint8_t build_ring_next_slot = 2;
/// The build ring's PREV wedge (W).
inline constexpr uint8_t build_ring_prev_slot = 6;
/// What a build ring wedge is.
enum class BuildWedgeKind : uint8_t {
    empty,         ///< nothing
    build,         ///< a build button
    prev,          ///< the previous build page
    next,          ///< the next build page
    fire_orders,   ///< the fire standing order (standing-orders ring)
    move_orders,   ///< the move standing order (standing-orders ring)
    on_off,        ///< activation on or off (standing-orders ring)
    cloak,         ///< cloak on or off (standing-orders ring)
    info,          ///< INFO (standing-orders ring)
    self_destruct, ///< self-destruct, held (standing-orders ring)
};

/// One laid-out build ring wedge.
struct BuildWedge {
    BuildWedgeKind kind{BuildWedgeKind::empty}; ///< what it is
    int16_t gadget{-1};                         ///< the loaded HUD gadget it presses, -1 for none
    uint16_t queued{};                          ///< a factory's queue count for it
    bool available{};                           ///< drawn greyed and ignored when false
    Rect hit{};     ///< canvas rectangle around its picture, at least 44 pt
    Rect picture{}; ///< where the gadget's own 3.1c picture is drawn
};

/// What the dispatcher puts in the build ring.
struct BuildRingContent {
    std::array<BuildWedgeKind, build_ring_slot_count> kinds{}; ///< by slot
    std::array<int16_t, build_ring_slot_count> gadgets{};      ///< by slot; -1 for none
    std::array<uint16_t, build_ring_slot_count> queued{};      ///< by slot
    std::array<bool, build_ring_slot_count> available{};       ///< by slot
    bool standing_orders{}; ///< no builder: the standing-orders ring
};

/// The open build ring.
struct BuildRing {
    Point anchor{};     ///< the pointer where it opened, canvas pixels
    Point centre{};     ///< the anchor moved inside the safe area, canvas pixels
    int inner_radius{}; ///< canvas pixels (44 pt)
    int outer_radius{}; ///< canvas pixels (128 pt)
    std::array<BuildWedge, build_ring_slot_count> wedges{}; ///< by slot
    bool standing_orders{};                                 ///< the standing-orders ring
};

/// The groups the left pad's group ring holds.
inline constexpr std::size_t group_ring_slot_count = 9;

/// The left pad's ring of the nine groups, drawn at the battlefield's lower left while the
/// groups layer is held on a pad with trackpads.
struct GroupRing {
    Point centre{};               ///< canvas pixels
    int inner_radius{};           ///< canvas pixels
    int outer_radius{};           ///< canvas pixels
    std::optional<uint8_t> aim{}; ///< the group aimed at, 1..9
};

/// What the pad shows; written by the gamepad dispatcher each frame (tick_pad), read by
/// lay_out (fields marked (layout)) and the drawing. refresh_hud never writes it.
struct PadLook {
    bool hud{};    ///< (layout) the slim pad HUD: a gamepad sent input, touch controls off
    bool badges{}; ///< touch controls show their pad buttons (a pad was used, prompts not Off)
    /// The glyph set the prompts use.
    oa::ui::pad_controls::GlyphStyle glyphs{oa::ui::pad_controls::GlyphStyle::xbox};
    oa::ui::pad_controls::MapContext map{}; ///< which buttons give which roles, for prompts
    bool force_shown{};       ///< (layout) the FORCE chip is laid out (the pad has grips)
    bool force_active{};      ///< FORCE gives Ctrl now: lit
    bool groups_layer{};      ///< the groups layer is held: group chips show their buttons
    bool over_build_button{}; ///< the pointer is on a build button: QUEUE reads x5
    std::optional<uint8_t> radial_aim{}; ///< the order ring wedge the pad aims at
    std::optional<uint8_t> build_aim{};  ///< the build ring wedge the pad aims at
    Point aim_dot{};                     ///< where the aim dot is drawn, canvas pixels
    bool ring_by_pad{}; ///< the open ring was opened by the pad: its hint line shows
    /// (layout) shown while the groups layer is held with trackpads.
    std::optional<GroupRing> group_ring{};
    float hold_progress{}; ///< 0..1 while a timed pad hold runs (self-destruct), else 0
    Point hold_point{};    ///< where that hold's ring fills, canvas pixels
};

/// Everything the controls show. The dispatcher writes it and bumps `revision` on each change;
/// the drawing reads it and redraws its layer when `revision` or the frame changes. lay_out
/// reads only the fields marked (layout); every other field changes looks, not rectangles.
struct HudState {
    uint32_t revision{};      ///< bumped on every change
    bool in_match{};          ///< (layout) the match screen shows
    bool paused{};            ///< the console pause bit is set (Pause key, PAUSE): PAUSE lit
    bool menu_open{};         ///< the in-game menu or another match panel holds the match
    bool watching{};          ///< (layout) a watcher: no speed control
    bool shared_game{};       ///< (layout) multiplayer: CHAT shown
    bool following{};         ///< the camera follows a unit: FOLLOW lit
    bool chat_open{};         ///< the chat line is being written: CHAT lit
    bool has_selection{};     ///< units are selected
    bool builder_selected{};  ///< BUILD is labelled BUILD, else ORDERS
    bool build_page_loaded{}; ///< (layout) the 3.1c panel shows a build page (tablet x5)
    LatchMode latch_mode{LatchMode::stay_on};   ///< the Touch setting QUEUE and ADD
    Latches latches{};                          ///< QUEUE, ADD and x5
    std::array<Pressed, max_pressed> pressed{}; ///< controls under resting fingers
    Sheet sheet{Sheet::none};                   ///< (layout) the open sheet
    DrawerTab drawer_tab{DrawerTab::build};     ///< the drawer's tab
    uint8_t drawer_page{};                      ///< page shown, from 0
    uint8_t drawer_pages{};        ///< (layout) pages the builder has: page dots, PREV/NEXT
    uint8_t drawer_cells_wanted{}; ///< (layout) gadgets the drawer shows, at most max_drawer_cells
    std::string drawer_title;      ///< the drawer's header: the builder's name, or "BUILD"
    uint8_t more_toggle_count{};   ///< (layout) MORE: order-page toggles, laid out 2 across
    uint8_t more_button_count{};   ///< (layout) MORE: order buttons, laid out 3 across
    std::array<uint16_t, 10> group_counts{}; ///< (layout) by group 1..9; 0 = not stored
    uint8_t selected_group{};                ///< the group the selection is (lit chip); 0 for none
    std::array<RailSlot, max_rail_slots> rail{}; ///< the phone's rail, the first rail_count used
    uint8_t rail_count{};           ///< (layout) slots in use, MORE included; <= rail_capacity
    std::optional<Radial> radial{}; ///< (layout) the open radial
    Banner banner{};                ///< (layout: shown)
    Placement placement{};          ///< (layout: active)
    std::string selection_text;     ///< "Commander + 4 Tanks"; empty with no selection
    TapAction tap_action{TapAction::none};   ///< what a tap gives at the hover point
    TapAction enemy_action{TapAction::none}; ///< what a tap on an enemy gives
    Tip tip{};                               ///< (layout: shown while until_ms is not 0)
    float self_destruct_progress{};          ///< 0..1 while the SELF-DESTRUCT hold runs, else 0
    PadLook pad{};                           ///< written by the gamepad dispatcher
    std::optional<BuildRing> build_ring{};   ///< (layout) the open build ring
    int8_t sheet_focus{-1};                  ///< the SELECT ▾ item the D-pad marks, -1 none
    bool force_touch{}; ///< a finger holds the FORCE chip (the touch dispatcher writes it)
    /// An order is armed or a building waits to be placed: the pad's right button takes it back.
    bool armed_or_placing{};
    bool right_click_interface{}; ///< the 3.1c right-click interface is chosen
};

/// What the controls cover, and where the placed regions go on a phone.
struct Frame {
    std::array<ControlRect, max_controls> controls{}; ///< the first control_count are used
    uint8_t control_count{};                          ///< controls laid out
    Rect minimap{};   ///< phone: where the minimap region goes (104 pt square)
    Rect resources{}; ///< phone: the resource strip
    /// The status pill: the phone's, or the pad HUD's at the battlefield's top; empty on a tablet
    /// with touch controls (the 3.1c bottom bar says it).
    Rect status{};
    Rect banner{};                                     ///< empty when no banner
    Rect tip{};                                        ///< empty when no tip
    Rect sheet{};                                      ///< the open sheet's panel, empty when none
    Rect drawer_grid{};                                ///< phone drawer: the area the cells fill
    std::array<Rect, max_drawer_cells> drawer_cells{}; ///< 84 pt cells, 3 across
    uint8_t drawer_cell_count{};                       ///< drawer cells laid out
    Rect more_grid{};                                  ///< MORE: the area the cells fill
    std::array<Rect, max_more_cells> more_cells{};     ///< cells for the order-page gadgets
    uint8_t more_cell_count{};                         ///< MORE cells laid out
    Rect placement_bar{};                              ///< the placement bar: ✕ CANCEL
    Rect panel_sheet{}; ///< phone: where a 3.1c panel (in-game menu, unit info) is centred
    /// The battlefield less the controls: where the message log, the chat line, the kill board
    /// and the other battlefield overlays go while touch controls are on.
    Rect clear{};
};

/// Returns how many phone rail slots fit the viewport, MORE included (at most
/// max_rail_slots − 1); 0 on a tablet.
///
/// @param viewport the canvas
/// @return the slots that fit
[[nodiscard]] uint8_t rail_capacity(const Viewport& viewport) noexcept;
/// Lays out every control the state shows, for the device class and handedness.
///
/// @param viewport the canvas
/// @param state what the controls show (only the fields marked (layout) are read)
/// @return the laid-out frame
[[nodiscard]] Frame lay_out(const Viewport& viewport, const HudState& state) noexcept;
/// Mirrors a rectangle left to right inside the viewport (left-handed layout).
///
/// @param rect canvas pixels
/// @param viewport the canvas
/// @return the mirrored rectangle
[[nodiscard]] Rect mirrored(const Rect& rect, const Viewport& viewport) noexcept;
/// Returns the control under a point: exact first (topmost, last laid out wins), else the
/// nearest whose rectangle is within radius_px. sheet_outside is returned only for points not on
/// any other control while a sheet or radial is open.
///
/// @param frame the laid-out controls
/// @param point canvas pixels
/// @param radius_px how far from a rectangle's edge a point still hits it, canvas pixels
/// @return the control, or none
[[nodiscard]] std::optional<ControlRect>
hit(const Frame& frame, Point point, int radius_px) noexcept;
/// Returns the index of the rectangle nearest a point within radius_px (distance to its edge,
/// 0 inside), or -1. Used for menu gadgets and HUD gadgets too.
///
/// @param rects the rectangles, canvas pixels
/// @param count how many rects holds
/// @param point canvas pixels
/// @param radius_px the largest distance accepted, canvas pixels
/// @return the index of the nearest rectangle, or -1
[[nodiscard]] int
nearest_rect(const Rect* rects, std::size_t count, Point point, int radius_px) noexcept;
/// Returns whether a point lies on any control or open sheet of the frame.
///
/// @param frame the laid-out controls
/// @param point canvas pixels
/// @return whether the controls cover the point
[[nodiscard]] bool covers(const Frame& frame, Point point) noexcept;
/// Returns the help line a long press on a control shows.
///
/// @param control the control
/// @param index as ControlRect::index
/// @return the help line, untranslated
[[nodiscard]] std::string_view control_help(Control control, uint8_t index) noexcept;
/// Returns the help line a long press on a 3.1c order panel gadget shows when the gadget
/// carries no help text of its own (the game's GUI files give the in-game panels none): the
/// order buttons, the standing-order toggles, ORDERS and BUILD, and PREV and NEXT. The name
/// is matched by the word it holds, ignoring case and a side prefix (ARMMOVE, CORFIREORD).
///
/// @param name the gadget's name in its GUI file
/// @return the help line, untranslated; empty for a gadget with none (a build button, the
///     panel's header)
[[nodiscard]] std::string_view gadget_help(std::string_view name) noexcept;
/// Returns the label drawn on a control ("QUEUE", "ADD", "x5", "CLEAR", "SELECT", ...).
///
/// @param control the control
/// @param index as ControlRect::index
/// @param state what the controls show (BUILD or ORDERS, the rail's orders)
/// @return the label, untranslated
[[nodiscard]] std::string_view
control_label(Control control, uint8_t index, const HudState& state) noexcept;
/// Returns the text a control's label is translated by.
///
/// That is the label itself, except where one word stands for two things: the right rail's
/// NEXT, which centres the next unit, is looked up as NEXT UNIT, as SELECT ▾'s item is, and the
/// build pages' NEXT, in the phone's drawer and on the build ring, as NEXT PAGE.
///
/// @param control the control
/// @param index as ControlRect::index
/// @param state what the controls show (BUILD or ORDERS, the rail's orders)
/// @return the text, untranslated; empty for a control with no label
[[nodiscard]] std::string_view
control_label_lookup(Control control, uint8_t index, const HudState& state) noexcept;
/// The field a lookup text holds an action's word in: "ENEMY: {action}".
inline constexpr std::string_view action_field = "{action}";
/// Returns a label as the language shown draws it.
///
/// A label looked up by itself is translated. A label with a lookup text of its own shows that
/// text's translation, or the label in English when the language has none: never the
/// translation of the word alone, which may mean something else. With an action, the
/// translation's action_field is filled with the action's own translation, in one pass, so
/// the words filled in are never read as a field.
///
/// @param label the label, untranslated
/// @param lookup the text it is translated by (control_label_lookup, HintPart::lookup); empty
///     or the label itself for the label alone
/// @param translate turns a text into the language shown, or returns it unchanged
/// @param action the word the lookup's action_field holds, untranslated (HintPart::action);
///     empty for a lookup without one
/// @return the words to draw
[[nodiscard]] std::string shown_label(
    std::string_view label,
    std::string_view lookup,
    const std::function<std::string(std::string_view)>& translate,
    std::string_view action = {}
);
/// Returns the status hint for the actions a tap gives: "TAP: MOVE · ENEMY: ATTACK".
///
/// Each piece is translated whole, as "TAP: {action}" and "ENEMY: {action}" with the action's
/// word filled in (shown_label), so a language with no translation of a piece shows it in
/// English.
///
/// @param tap what a tap gives at the hover point
/// @param enemy what a tap on an enemy gives
/// @param translate turns a text into the language shown, or returns it unchanged
/// @return the hint in the language shown
[[nodiscard]] std::string status_hint(
    TapAction tap, TapAction enemy, const std::function<std::string(std::string_view)>& translate
);
/// The banner's title over an armed order, as it is translated: "PATROL armed".
inline constexpr std::string_view armed_title = "{order} armed";
/// The field armed_title holds the order's word in.
inline constexpr std::string_view order_field = "{order}";
/// The banner's title while a building is placed, as it is translated: "Place Solar Collector".
inline constexpr std::string_view placement_title = "Place {name}";
/// The field placement_title holds the building's name in.
inline constexpr std::string_view name_field = "{name}";
/// Returns the banner's title in the language shown: "PATROL armed", "Place Solar Collector".
///
/// While a building is placed, placement_title's translation, or the phrase in English when
/// the language has none, with Placement::name filled in as it is, since it is in the language
/// shown already. Over an armed order, armed_title's translation with the order's word
/// translated, or the title in English when the language has no translation of the phrase, as
/// shown_label gives a label. The words are filled in in one pass, so they are never read as a
/// field.
///
/// @param state what the controls show: Banner and Placement
/// @param translate turns a text into the language shown, or returns it unchanged
/// @return the title in the language shown; empty when no banner shows
[[nodiscard]] std::string
banner_title(const HudState& state, const std::function<std::string(std::string_view)>& translate);
/// Returns the label of a SELECT ▾, speed or phone menu item.
///
/// @param sheet the open sheet
/// @param index the item: SelectItem, SpeedItem or PhoneMenuItem by sheet
/// @return the label, untranslated; empty for another sheet or index
[[nodiscard]] std::string_view menu_item_label(Sheet sheet, uint8_t index) noexcept;

/// Lays the build ring out at a point, inside the safe area.
///
/// @param anchor the pointer where it opens, canvas pixels
/// @param content what each wedge holds
/// @param viewport the canvas
/// @return the laid-out ring
[[nodiscard]] BuildRing
make_build_ring(Point anchor, const BuildRingContent& content, const Viewport& viewport) noexcept;
/// Returns the build ring wedge under a point (the nearest within gadget_pick_points), if any.
///
/// @param ring the laid-out ring
/// @param point canvas pixels
/// @param viewport the canvas
/// @return the wedge's slot, or none
[[nodiscard]] std::optional<uint8_t>
build_ring_hit(const BuildRing& ring, Point point, const Viewport& viewport) noexcept;
/// Lays the group ring out for a viewport.
///
/// @param viewport the canvas
/// @return the laid-out ring, with no aim
[[nodiscard]] GroupRing make_group_ring(const Viewport& viewport) noexcept;

/// One piece of a pad hint: a button's glyphs with the words after them, or words alone.
struct HintPart {
    std::optional<oa::ui::pad_controls::Chord> chord{}; ///< the glyphs drawn; none for words
    std::string text;                                   ///< the words, untranslated
    /// the text the words are translated by when it is not the words themselves
    /// (shown_label); empty for the words
    std::string lookup;
    /// the word the lookup's action_field holds, untranslated; empty for a lookup without one
    std::string action;
};

/// The most pieces a pad hint has.
inline constexpr std::size_t max_hint_parts = 8;

/// A pad hint: glyphs and words, left to right.
struct PadHint {
    std::array<HintPart, max_hint_parts> parts{}; ///< the first count are used
    uint8_t count{};                              ///< pieces in use
};

/// Returns the pad's status line: what R2 and L2 would do now ("R2 MOVE · ENEMY: ATTACK ·
/// L2 CANCEL"; right-click interface "R2 SELECT · L2 MOVE").
///
/// The enemy's piece is looked up as "ENEMY: {action}" with its action's word (HintPart::action),
/// as status_hint translates it.
///
/// @param tap what a click gives at the pointer
/// @param enemy what a click on an enemy gives
/// @param armed_or_placing an order is armed or a building is being placed
/// @param right_click_interface the right-click interface is chosen
/// @param map which buttons give which roles
/// @return the hint
[[nodiscard]] PadHint pad_status_hint(
    TapAction tap,
    TapAction enemy,
    bool armed_or_placing,
    bool right_click_interface,
    const oa::ui::pad_controls::MapContext& map
);
/// Returns a ring's hint: "release R1: give · A: arm · B: close" (build ring: L1).
///
/// ARM, which arms the aimed wedge, is looked up as ARM ORDER (HintPart::lookup), since the
/// game's own table of words holds ARM as the side's name.
///
/// @param build_ring the build ring, else the order ring
/// @param map which buttons give which roles
/// @return the hint
[[nodiscard]] PadHint ring_hint(bool build_ring, const oa::ui::pad_controls::MapContext& map);
/// Returns the chord a control's badge shows (QUEUE R4, ADD L4, CLEAR B, SELECT ▾ D-pad ←,
/// PAUSE View+X, CHAT View+A, CENTRE L3, FOLLOW R3, NEXT D-pad →, INFO View, FORCE R5), or
/// none.
///
/// @param control the control
/// @param index as ControlRect::index
/// @param map which buttons give which roles
/// @return the chord, or none for a control with no badge
[[nodiscard]] std::optional<oa::ui::pad_controls::Chord>
control_badge(Control control, uint8_t index, const oa::ui::pad_controls::MapContext& map) noexcept;
/// Returns a control's help line naming the pad input too ("QUEUE: hold or tap R4").
///
/// @param control the control
/// @param index as ControlRect::index
/// @param map which buttons give which roles
/// @param glyphs the glyph set the words name the buttons by
/// @return the help line, untranslated
[[nodiscard]] std::string control_help_with_pad(
    Control control,
    uint8_t index,
    const oa::ui::pad_controls::MapContext& map,
    oa::ui::pad_controls::GlyphStyle glyphs
);
/// Returns the words a button is called by in a glyph set, for help lines and for hints shown
/// without glyphs: "R4", "View", "LB", "Cross", "ZR", "D-pad left", "right trackpad".
///
/// @param button the physical button
/// @param glyphs the glyph set
/// @return the words, untranslated; empty for none
[[nodiscard]] std::string_view button_name(
    oa::ui::pad_controls::PadButton button, oa::ui::pad_controls::GlyphStyle glyphs
) noexcept;
/// Returns a chord in words: its buttons' names joined by " + " ("View + X"), after "tap " or
/// "hold " when the chord is a tap or a hold of its button.
///
/// @param chord the buttons
/// @param glyphs the glyph set the buttons are named by
/// @return the words, untranslated
[[nodiscard]] std::string
chord_words(const oa::ui::pad_controls::Chord& chord, oa::ui::pad_controls::GlyphStyle glyphs);
/// Returns a pad hint as one line of words, each chord in chord_words and every piece apart by a
/// space: "R2 MOVE · ENEMY: ATTACK · L2 CANCEL".
///
/// @param hint the hint
/// @param glyphs the glyph set the buttons are named by
/// @return the line, untranslated
[[nodiscard]] std::string hint_words(const PadHint& hint, oa::ui::pad_controls::GlyphStyle glyphs);

} // namespace oa::ui::touch_hud
