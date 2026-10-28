// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gamepad controls' model: which pad is in use and what it has, the
// Controller section's settings as the pad reads them, the layers and the
// maps from buttons to actions (the left-handed mirror and the fallback map
// for pads without grips among them), the timing pieces (triggers read as
// buttons, holds, menu repeat, double clicks, wedge aim), the right trackpad
// as a pointer, the gyro, the stick cursor, haptics and the button glyphs.
// Pure: no SDL, no clock of its own and no app header (docs/controllers.md).
// Geometry is in canvas pixels unless a function says otherwise.
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdint.h>
#include <string_view>

namespace oa::ui::pad_controls {

/// A point or a vector, in the unit a function names.
struct Vec2 {
    float x{}; ///< across, positive right
    float y{}; ///< down, positive down
};

/// An axis-aligned rectangle, in canvas pixels.
struct Area {
    float x{};      ///< left edge
    float y{};      ///< top edge
    float width{};  ///< across
    float height{}; ///< down
};

// ---- Physical inputs (positions, not labels: `a` is the bottom face button) ----

/// A physical input of a pad, named by its place: `a` is the bottom face button whatever
/// its label says.
enum class PadButton : uint8_t {
    none,              ///< no button
    a,                 ///< the bottom face button
    b,                 ///< the right face button
    x,                 ///< the left face button
    y,                 ///< the top face button
    view,              ///< the left of the two middle buttons (View, Back, Create, −)
    menu,              ///< the right of the two middle buttons (Menu, Start, Options, +)
    l1,                ///< the left bumper
    r1,                ///< the right bumper
    l2,                ///< the left trigger, read as a button
    r2,                ///< the right trigger, read as a button
    l3,                ///< the left stick pressed
    r3,                ///< the right stick pressed
    l4,                ///< the upper left back grip
    l5,                ///< the lower left back grip
    r4,                ///< the upper right back grip
    r5,                ///< the lower right back grip
    dpad_up,           ///< the D-pad's up arm
    dpad_right,        ///< the D-pad's right arm
    dpad_down,         ///< the D-pad's down arm
    dpad_left,         ///< the D-pad's left arm
    left_pad,          ///< the left trackpad pressed
    right_pad,         ///< the right trackpad pressed
    left_stick_touch,  ///< the left stick's capacitive top
    right_stick_touch, ///< the right stick's capacitive top
};
/// How many PadButton values there are, none included.
inline constexpr std::size_t pad_button_count = 25;
static_assert(
    pad_button_count == static_cast<std::size_t>(PadButton::right_stick_touch) + 1,
    "pad_button_count counts every PadButton"
);
/// Which of a pair: sticks, trackpads, triggers.
enum class Side : uint8_t {
    left,  ///< the left one
    right, ///< the right one
};

/// What kind of pad it is, for prompts and the effective scheme.
enum class PadType : uint8_t {
    unknown,     ///< SDL did not say
    standard,    ///< a pad of no family below
    xbox,        ///< an Xbox pad or one laid out like it
    playstation, ///< a PlayStation pad
    nintendo,    ///< a Nintendo pad
    steam_deck,  ///< a Steam Deck's own controls
};
/// The USB vendor number of Valve's devices.
inline constexpr uint16_t valve_vendor = 0x28de;
/// The USB product number of the Steam Deck's controls.
inline constexpr uint16_t steam_deck_product = 0x1205;
/// Returns the pad's type: steam_deck for valve_vendor and steam_deck_product (which Steam
/// Input's virtual pad also reports), else `reported`.
///
/// @param vendor the USB vendor number SDL reports
/// @param product the USB product number SDL reports
/// @param reported the type SDL's own reading gives
/// @return the type the controls use
[[nodiscard]] PadType pad_type_of(uint16_t vendor, uint16_t product, PadType reported) noexcept;

/// What the pad in use has.
struct PadTraits {
    PadType type{PadType::unknown}; ///< its family
    /// Touchpads SDL reports: 2 on the Deck with Steam Input off, 0 through Steam Input.
    uint8_t trackpads{};
    bool grips{};       ///< the four back grips are reported as buttons
    bool gyro{};        ///< a gyro sensor is reported
    bool steam_input{}; ///< the pad reaches the game through Steam Input (a Steam handle)
    uint16_t vendor{};  ///< the USB vendor number
    uint16_t product{}; ///< the USB product number
};

// ---- Settings (stored by oa::ui::engine_settings) ----

/// The Controller section's Scheme.
enum class Scheme : uint8_t {
    trackpads, ///< the right trackpad is the pointer
    sticks,    ///< the right stick moves a stick cursor
};
/// The Controller section's Right trackpad.
enum class RightTrackpad : uint8_t {
    relative, ///< Pointer (relative): the thumb's motion moves the pointer
    absolute, ///< Pointer (absolute): the pad maps onto the screen
};
/// The Controller section's Pointer acceleration.
enum class Acceleration : uint8_t {
    off,  ///< motion maps one to one
    low,  ///< fast motion goes up to low_acceleration_gain times further
    high, ///< fast motion goes up to high_acceleration_gain times further
};
/// The Controller section's Right stick (trackpads scheme).
enum class RightStick : uint8_t {
    zoom_and_pages, ///< up and down zoom, a flick left or right turns build pages
    pointer,        ///< a second pointer, the stick cursor
    nothing,        ///< the stick does nothing
};
/// The Controller section's Gyro pointer.
enum class Gyro : uint8_t {
    off,                 ///< the gyro is not read
    right_pad_touched,   ///< the gyro moves the pointer while the right trackpad is touched
    right_stick_touched, ///< while the right stick's top is touched
    always,              ///< always
};
/// The Controller section's Haptics.
enum class Haptics : uint8_t {
    off,    ///< no feel
    light,  ///< soft ticks and pulses
    strong, ///< firmer ticks and pulses
};
/// The Controller section's Button prompts.
enum class Prompts : uint8_t {
    automatic,   ///< the glyphs of the pad in use
    steam_deck,  ///< Steam Deck glyphs
    xbox,        ///< Xbox glyphs
    playstation, ///< PlayStation glyphs
    nintendo,    ///< Nintendo glyphs
    off,         ///< no prompts
};
/// The slowest Pointer speed, in percent.
inline constexpr uint32_t lowest_pointer_speed = 50;
/// The fastest Pointer speed, in percent.
inline constexpr uint32_t highest_pointer_speed = 300;
/// The Pointer speed's step, in percent.
inline constexpr uint32_t pointer_speed_step = 10;
/// The Pointer speed when the player has chosen none, in percent.
inline constexpr uint32_t default_pointer_speed = 100;
/// The slowest Gyro speed, in percent.
inline constexpr uint32_t lowest_gyro_speed = 50;
/// The fastest Gyro speed, in percent.
inline constexpr uint32_t highest_gyro_speed = 400;
/// The Gyro speed's step, in percent.
inline constexpr uint32_t gyro_speed_step = 10;
/// The Gyro speed when the player has chosen none, in percent.
inline constexpr uint32_t default_gyro_speed = 100;
/// The percent the speed settings count in: a speed of this many percent is ×1.
inline constexpr uint32_t full_speed_percent = 100;
/// The hold delay before the player chooses one, in milliseconds: the Touch section's
/// default (engine_settings::default_touch_hold_ms equals it; the settings check so).
inline constexpr uint32_t default_hold_ms = 350;
/// How long the self-destruct hold lasts, in milliseconds.
inline constexpr uint32_t self_destruct_hold_ms = 1000;

/// The Controller section's settings as the pad reads them, with the shared hold delay.
struct PadSettings {
    Scheme scheme{Scheme::trackpads};                      ///< Scheme
    RightTrackpad right_trackpad{RightTrackpad::relative}; ///< Right trackpad
    uint32_t pointer_speed{default_pointer_speed};         ///< Pointer speed, percent
    Acceleration acceleration{Acceleration::low};          ///< Pointer acceleration
    bool glide{};                                          ///< Trackpad glide
    RightStick right_stick{RightStick::zoom_and_pages};    ///< Right stick
    bool magnetism{true};                                  ///< Magnetism (stick pointer)
    Gyro gyro{Gyro::off};                                  ///< Gyro pointer
    uint32_t gyro_speed{default_gyro_speed};               ///< Gyro speed, percent
    Haptics haptics{Haptics::light};                       ///< Haptics
    Prompts prompts{Prompts::automatic};                   ///< Button prompts
    bool left_handed{};                                    ///< Left-handed
    uint32_t hold_ms{default_hold_ms}; ///< the Touch section's hold delay, shared, milliseconds
};

/// Returns the scheme that plays: `chosen`, except Trackpads on a pad that has no two
/// trackpads and is not a Deck behind Steam Input (whose right pad is Steam's mouse), which
/// plays Sticks.
///
/// @param chosen the Scheme setting
/// @param traits what the pad in use has
/// @return the scheme the controls use
[[nodiscard]] Scheme effective_scheme(Scheme chosen, const PadTraits& traits) noexcept;

// ---- Layers, actions and maps ----

/// Which set of meanings the buttons have now.
enum class Layer : uint8_t {
    base,            ///< nothing held or open on the match
    groups,          ///< the groups layer is held
    game,            ///< the game layer is held
    standing_orders, ///< the standing orders layer is held
    order_ring,      ///< the order ring is open
    build_ring,      ///< the build ring is open
    select_sheet,    ///< SELECT ▾ is open
    menus,           ///< a menu, a dialog or a screen other than the match
};

/// What is held or open, which decides the layer.
struct Held {
    bool in_match{};     ///< the match screen shows with no menu over it; else Layer::menus
    bool groups{};       ///< L5 held (fallback: View held)
    bool game{};         ///< View held (fallback: Menu held)
    bool standing{};     ///< R5 held
    bool order_ring{};   ///< the order ring is open
    bool build_ring{};   ///< the build ring is open
    bool select_sheet{}; ///< SELECT ▾ is open
};

/// Returns the layer: menus off the match; else order ring, build ring, select sheet, groups,
/// game, standing orders, base, the first that applies.
///
/// @param held what is held or open
/// @return the layer the buttons act in
[[nodiscard]] Layer layer_for(const Held& held) noexcept;

/// What a button does.
enum class Action : uint8_t {
    none,           ///< nothing
    left_button,    ///< held like the mouse's left button
    right_button,   ///< held like the mouse's right button
    queue,          ///< the QUEUE latch (a grip)
    add,            ///< the ADD latch (a grip)
    force,          ///< FORCE: Ctrl for orders and selection while held
    groups_layer,   ///< holds the groups layer (no action of its own)
    game_layer,     ///< holds the game layer (no action of its own)
    standing_layer, ///< holds the standing orders layer (no action of its own)
    order_ring,     ///< opens the order ring at the pointer
    build_ring,     ///< opens the build ring at the pointer
    queue_toggle,   ///< fallback: a tap of R1 turns QUEUE on or off
    add_toggle,     ///< fallback: a tap of L1 turns ADD on or off
    centre,         ///< CENTRE
    follow,         ///< FOLLOW
    clear,          ///< CLEAR: takes back an armed order or clears the selection
    stop,           ///< the STOP order
    select_type,    ///< selects every visible unit of the type under the pointer
    commander,      ///< selects and centres the commander
    next_unit,      ///< NEXT
    next_report,    ///< jumps to the latest report
    select_menu,    ///< opens SELECT ▾
    unit_info,      ///< INFO of the unit under the pointer
    game_menu,      ///< the in-game menu
    previous_page,  ///< the previous build page (right stick flick, trackpads scheme)
    next_page,      ///< the next build page (right stick flick, trackpads scheme)
    zoom_in,        ///< zooms in while held (D-pad, sticks scheme)
    zoom_out,       ///< zooms out while held (D-pad, sticks scheme)
    unit_jump,      ///< R3 held and a right stick flick (sticks scheme)
    group,          ///< a group, Binding::index 1..8
    chat,           ///< CHAT (game layer)
    kill_board,     ///< the kill board (game layer)
    pause,          ///< PAUSE (game layer)
    health_bars,    ///< the health bars (game layer)
    faster,         ///< game speed up (game layer)
    slower,         ///< game speed down (game layer)
    clear_messages, ///< clears the message log (game layer)
    team_menu,      ///< the team menu (game layer)
    share_panel,    ///< the share panel (game layer)
    screenshot,     ///< a screenshot (game layer)
    fire_orders,    ///< the fire standing order (standing orders layer)
    move_orders,    ///< the move standing order (standing orders layer)
    cloak,          ///< cloak on or off (standing orders layer)
    on_off,         ///< activation on or off (standing orders layer)
    self_destruct,  ///< self-destruct, held (standing orders layer)
    ring_arm,       ///< arms the aimed wedge (a ring is open)
    ring_close,     ///< closes the ring (a ring is open)
    ring_give,      ///< gives the aimed wedge (a ring is open)
    ring_reduce,    ///< build ring on a factory: −1 (−5 with QUEUE held)
    minimap,        ///< left pad pressed and held: the minimap under the thumb
    sheet_up,       ///< SELECT ▾ open: the mark up
    sheet_down,     ///< SELECT ▾ open: the mark down
    sheet_pick,     ///< SELECT ▾ open: picks the marked item
    sheet_close,    ///< SELECT ▾ open: closes it
    focus_up,       ///< menus: the focus up
    focus_down,     ///< menus: the focus down
    focus_left,     ///< menus: the focus left
    focus_right,    ///< menus: the focus right
    focus_next,     ///< menus: the next control
    focus_previous, ///< menus: the previous control
    press,          ///< menus: presses the focused control
    back,           ///< menus: back, as Escape
    default_button, ///< menus: the dialog's default button, as Enter
    info,           ///< menus: information about the focused item, as F1
    wheel_up,       ///< menus: a wheel step up
    wheel_down,     ///< menus: a wheel step down
    /// menus: focuses the Library's search (Y). On the skirmish setup and the
    /// campaign launch it opens MUTATORS…. On any other screen it does nothing.
    library_search,
    /// menus: the selected Library entry's first action (X), when that action
    /// can be pressed. On any other screen it does nothing.
    library_action,
    /// Toggles the keys overlay. On the default map, hold View and press R1.
    keys_overlay,
};

/// One button's meaning in a layer.
struct Binding {
    Action action{Action::none}; ///< what it does
    /// The group for Action::group, 1..8; 0 for the left trackpad's ring of the nine groups,
    /// whose group is the wedge the thumb rests on.
    uint8_t index{};
    /// It acts once held past the hold delay (self-destruct: self_destruct_hold_ms), not on
    /// press.
    bool on_hold{};
};

/// Which map applies.
struct MapContext {
    Scheme scheme{Scheme::trackpads}; ///< effective_scheme's answer
    /// No grips (neither buttons nor F13–F16 keys seen): the fallback map.
    bool fallback{};
    /// Two trackpads reported (left pad drag and minimap, pad-aimed rings).
    bool trackpads{};
    bool left_handed{}; ///< the roles mirrored
};

/// Returns the button standing in for `button` in the left-handed mirror: L and R swapped for
/// the pads, sticks, stick clicks and touches, triggers, bumpers and grips; the face buttons,
/// D-pad, View and Menu stay.
///
/// @param button the physical button
/// @return the button whose role it takes
[[nodiscard]] PadButton mirrored(PadButton button) noexcept;
/// Returns what `button` does in `layer` (after mirroring when ctx.left_handed). The tables are
/// those of the gamepad controls' reference (docs/controllers.md).
///
/// The grips are modifiers and layer keys on fingers of their own, so with grips they keep
/// their base meaning in every layer of the match (QUEUE and ADD held while a ring or the
/// groups layer is open still apply); without grips (ctx.fallback) they mean nothing. R5's
/// meaning is Action::standing_layer, which also gives FORCE while held. Stick touches mean
/// nothing (the gyro's gate reads them).
///
/// @param ctx which map applies
/// @param layer the layer the buttons act in
/// @param button the physical button
/// @return its binding; Action::none for a button with no meaning there
[[nodiscard]] Binding binding_for(const MapContext& ctx, Layer layer, PadButton button) noexcept;
/// Returns what a tap of a button that also has a hold gives (View: unit info; fallback R1:
/// QUEUE toggle, L1: ADD toggle; fallback Menu: game menu), else Action::none.
///
/// @param ctx which map applies
/// @param layer the layer the buttons act in
/// @param button the physical button
/// @return the tap's binding; Action::none when a tap gives nothing of its own
[[nodiscard]] Binding
tap_binding_for(const MapContext& ctx, Layer layer, PadButton button) noexcept;

/// The buttons that give an action, for prompts: `held` first, then `button`.
struct Chord {
    PadButton held{PadButton::none};   ///< held first; none for a single button
    PadButton button{PadButton::none}; ///< pressed
    bool tap{};                        ///< a tap of `button` (fallback latches)
    bool hold{};                       ///< `button` held (rings in the fallback, View's game layer)
};

/// Returns the chord that gives `action` (group `index`) in the map, or none.
///
/// The base layer is searched first, then the held layers (with their layer key as `held`),
/// the rings, SELECT ▾ and the menus; within a layer R2 comes before A and the right trackpad,
/// so the main button is named. QUEUE and ADD give the fallback's tap of R1 and L1 when the
/// map has no grips, and FORCE gives R5. Buttons the pad lacks are never named: the trackpads
/// without ctx.trackpads, the grips in the fallback. The buttons are the physical ones, so a
/// left-handed map names the mirrored button.
///
/// @param ctx which map applies
/// @param action the action
/// @param index the group for Action::group, 1..8; 0 otherwise
/// @return the chord, or none when no button gives the action in that map
[[nodiscard]] std::optional<Chord>
chord_for(const MapContext& ctx, Action action, uint8_t index = 0) noexcept;

// ---- Timing pieces ----

/// The trigger pull at which a trigger read as a button goes down, 0..1.
inline constexpr float trigger_on_pull = 0.30f;
/// The trigger pull under which a trigger read as a button comes up, 0..1.
inline constexpr float trigger_off_pull = 0.20f;

/// A trigger read as a button: on at trigger_on_pull, off below trigger_off_pull.
class TriggerButton {
  public:

    /// Takes the trigger's pull and returns whether the button is down after it.
    ///
    /// @param pull the trigger's pull, 0..1
    /// @return whether the button is down
    bool update(float pull) noexcept;
    /// Returns whether the button is down.
    ///
    /// @return whether the button is down
    [[nodiscard]] bool down() const noexcept;

  private:

    bool down_{}; ///< the button is down
};
/// What a hold timer reports.
enum class HoldEvent : uint8_t {
    none,         ///< nothing new
    tap,          ///< a release before the hold delay that nothing used
    hold_started, ///< the press passed the hold delay unused
    hold_ended,   ///< the release after a hold
};

/// A button that has a tap and a hold. A press another action used is neither.
class HoldTimer {
  public:

    /// Starts a press.
    ///
    /// @param now_ms the press's time, milliseconds on the caller's clock
    void press(uint64_t now_ms) noexcept;
    /// Ends the press.
    ///
    /// A release at or past hold_ms that advance() did not report as a hold is still no tap:
    /// it gives hold_ended, and hold_started was never given.
    ///
    /// @param now_ms the release's time, milliseconds on the caller's clock
    /// @param hold_ms the hold delay, milliseconds
    /// @return tap for a release before hold_ms that nothing used, hold_ended after a hold
    ///     (used or not once it started), none for an unused press that was used, or when up
    HoldEvent release(uint64_t now_ms, uint32_t hold_ms) noexcept;
    /// Moves time on while the button is down.
    ///
    /// @param now_ms the time now, milliseconds on the caller's clock
    /// @param hold_ms the hold delay, milliseconds
    /// @return hold_started once, when a press passes hold_ms unused; none otherwise
    HoldEvent advance(uint64_t now_ms, uint32_t hold_ms) noexcept;
    /// Notes that another action used the press, so it gives neither a tap nor a hold.
    void use() noexcept;
    /// Forgets the press.
    void cancel() noexcept;
    /// Returns whether the button is down.
    ///
    /// @return whether a press is under way
    [[nodiscard]] bool down() const noexcept;
    /// Returns whether the press became a hold.
    ///
    /// @return whether hold_started was given for this press
    [[nodiscard]] bool holding() const noexcept;
    /// Returns whether another action used the press.
    ///
    /// @return whether use() was called for this press
    [[nodiscard]] bool used() const noexcept;
    /// Returns how far the press is to the hold delay.
    ///
    /// @param now_ms the time now, milliseconds on the caller's clock
    /// @param hold_ms the hold delay, milliseconds
    /// @return 0..1; 0 when up or when another action used the press
    [[nodiscard]] float progress(uint64_t now_ms, uint32_t hold_ms) const noexcept;

  private:

    bool down_{};           ///< a press is under way
    bool holding_{};        ///< the press became a hold
    bool used_{};           ///< another action used the press
    uint64_t pressed_ms_{}; ///< when the press started, milliseconds
};

/// The delay before a held direction in menus steps again, in milliseconds.
inline constexpr uint32_t menu_repeat_delay_ms = 400;
/// The interval of a held direction's later steps in menus, in milliseconds.
inline constexpr uint32_t menu_repeat_interval_ms = 120;
/// The most steps one MenuRepeat::advance gives, however long since the last call: a stalled
/// frame never sends the focus far down a list.
inline constexpr uint32_t menu_repeat_most_steps = 4;

/// A held direction in menus: one step at once, again after menu_repeat_delay_ms, then every
/// menu_repeat_interval_ms.
class MenuRepeat {
  public:

    /// Starts a held direction.
    ///
    /// @param now_ms the press's time, milliseconds on the caller's clock
    /// @return the steps due now (1)
    uint32_t press(uint64_t now_ms) noexcept;
    /// Moves time on while the direction is held.
    ///
    /// @param now_ms the time now, milliseconds on the caller's clock
    /// @return the steps due since the last call, at most menu_repeat_most_steps (the steps
    ///     past it are dropped); 0 when not held
    uint32_t advance(uint64_t now_ms) noexcept;
    /// Ends the held direction.
    void release() noexcept;

  private:

    bool down_{};        ///< a direction is held
    uint64_t next_ms_{}; ///< when the next step is due, milliseconds
};

/// The longest time between the presses of a double click, in milliseconds.
inline constexpr uint32_t double_click_ms = 300;
/// The farthest apart the presses of a double click may be, in points.
inline constexpr float double_click_points = 12.0f;

/// Two presses within double_click_ms and double_click_points make a double click.
class DoubleClick {
  public:

    /// Counts a press.
    ///
    /// @param now_ms the press's time, milliseconds on the caller's clock
    /// @param at where it pressed, canvas pixels
    /// @param px_per_point canvas pixels per point, > 0
    /// @return the clicks the press counts: 2 for a second press near the first in time and
    ///     place, else 1 (a third quick press counts 1 again)
    uint8_t press(uint64_t now_ms, Vec2 at, float px_per_point) noexcept;
    /// Forgets the last press, so the next one counts 1.
    void reset() noexcept;

  private:

    uint64_t last_ms_{}; ///< the last single press's time, milliseconds
    Vec2 last_at_{};     ///< where it pressed, canvas pixels
    bool armed_{};       ///< a single press waits for its second
};

// ---- Wedge aim (rings) ----

/// The dead zone of a ring aimed with a trackpad, as a fraction of the pad's half width.
inline constexpr float pad_ring_dead_zone = 0.15f;
/// The dead zone of a ring aimed with a stick, as a fraction of full deflection.
inline constexpr float stick_ring_dead_zone = 0.5f;
/// How far past a wedge's edge the aim must go to leave it, in degrees.
inline constexpr float wedge_hysteresis_degrees = 8.0f;

/// Turns an aim vector into a wedge, clockwise from the top, with hysteresis.
class WedgeAim {
  public:

    /// Sets how many wedges the ring has and forgets the aim.
    ///
    /// @param slots 12 (order ring), 8 (build ring) or 9 (group ring)
    void configure(uint8_t slots) noexcept;
    /// Takes an aim and returns the wedge it picks; a wedge is left only
    /// wedge_hysteresis_degrees past its edge.
    ///
    /// @param offset the aim from the centre, -1..1 each way, y down
    /// @param dead_zone the length under which nothing is aimed at
    /// @return the wedge, 0 at the top and clockwise; none inside `dead_zone`
    std::optional<uint8_t> aim(Vec2 offset, float dead_zone) noexcept;
    /// Forgets the aim.
    void reset() noexcept;
    /// Returns the wedge aimed at.
    ///
    /// @return the wedge, or none
    [[nodiscard]] std::optional<uint8_t> slot() const noexcept;

  private:

    uint8_t slots_{12};             ///< wedges in the ring
    std::optional<uint8_t> slot_{}; ///< the wedge aimed at
};

/// Returns the canvas point at mid radius of a wedge (slot 0 at the top, clockwise), so that
/// touch_hud::radial_hit and build_ring_hit pick that wedge unchanged.
///
/// @param centre the ring's centre, canvas pixels
/// @param inner_radius the ring's inner radius, canvas pixels
/// @param outer_radius the ring's outer radius, canvas pixels
/// @param slot the wedge
/// @param slots wedges in the ring, > 0
/// @return the point, canvas pixels
[[nodiscard]] Vec2 wedge_point(
    Vec2 centre, float inner_radius, float outer_radius, uint8_t slot, uint8_t slots
) noexcept;

// ---- The pad pointer (right trackpad) ----

/// How long after landing motion under landing_dead_band is ignored, in milliseconds.
inline constexpr uint32_t landing_ms = 20;
/// The motion ignored just after landing, in pad widths.
inline constexpr float landing_dead_band = 0.004f;
/// How long the pointer holds still around a pad press and release, in milliseconds.
inline constexpr uint32_t click_lock_ms = 40;
/// The pointer travel between two pointer ticks, in canvas pixels.
inline constexpr float tick_travel_px = 32.0f;
/// The time constant a glide decays with, in seconds.
inline constexpr float glide_time_constant_s = 0.25f;
/// The speed at which a glide stops, in canvas pixels a second.
inline constexpr float glide_stop_px_per_s = 20.0f;
/// How long the thumb may rest before it lifts and still glide, in milliseconds: a thumb that
/// stopped first lifts with no speed.
inline constexpr uint32_t glide_rest_ms = 50;
/// The thumb speed under which acceleration gives ×1, in pad widths a second.
inline constexpr float acceleration_floor = 0.5f;
/// The thumb speed at which acceleration gives its greatest gain, in pad widths a second.
inline constexpr float acceleration_top = 3.0f;
/// The greatest gain of Pointer acceleration Low.
inline constexpr float low_acceleration_gain = 2.5f;
/// The greatest gain of Pointer acceleration High.
inline constexpr float high_acceleration_gain = 4.0f;
/// The share of the canvas's width one pad width moves the pointer at Pointer speed 100 % and
/// slow motion.
inline constexpr float pad_width_canvas_share = 0.5f;
/// The share of the canvas's width a radian of gyro turn moves the pointer at Gyro speed 100 %.
inline constexpr float gyro_canvas_share_per_radian = 0.5f;
/// The longest time one pointer step covers, in milliseconds: after a longer gap (a stalled
/// frame, a sensor that paused) the step counts this long, so the pointer never jumps.
inline constexpr uint32_t longest_pointer_step_ms = 100;
/// The shortest time one trackpad sample's speed is measured over, in milliseconds: samples
/// that arrive together are not read as infinitely fast.
inline constexpr uint32_t shortest_speed_sample_ms = 1;

/// One step of a pointer: a relative move, or (absolute) a place.
struct PointerStep {
    Vec2 move{};                 ///< canvas pixels
    std::optional<Vec2> place{}; ///< canvas pixels, absolute modes
    uint8_t ticks{};             ///< tick_travel_px crossings since the last step
};

/// Counts a pointer's travel into ticks, one each tick_travel_px.
class TickCounter {
  public:

    /// Adds travel and returns the ticks it completes.
    ///
    /// @param distance the travel, canvas pixels, >= 0
    /// @return tick_travel_px crossings, at most 255
    uint8_t add(float distance) noexcept;
    /// Forgets the travel since the last tick.
    void reset() noexcept;

  private:

    float travel_{}; ///< travel since the last tick, canvas pixels
};

/// The right trackpad as a mouse: relative with acceleration, landing dead band, click lock and
/// glide, or absolute over an area.
///
/// Relative: a pad width of thumb travel moves the pointer pad_width_canvas_share of the
/// canvas's width times Pointer speed; thumb speeds over acceleration_floor multiply that,
/// rising linearly to the setting's greatest gain at acceleration_top. For landing_ms after the
/// thumb lands, motion within landing_dead_band of the landing point moves nothing; for
/// click_lock_ms after the pad's click goes down or comes up, motion moves nothing and is
/// dropped. With glide on, a thumb that lifts while moving leaves the pointer coasting at its
/// last speed, slowing with glide_time_constant_s until under glide_stop_px_per_s or until a
/// thumb lands. Absolute: each step places the pointer at pad_to_area of the thumb, held still
/// in the same windows.
class PadPointer {
  public:

    /// Sets the speed, acceleration, glide and mode: one pad width at speed 100 % and slow
    /// motion moves half `canvas`'s width; absolute maps the pad onto `absolute_area`
    /// aspect-fitted to the middle of the pad.
    ///
    /// @param settings the Controller section's settings
    /// @param canvas the canvas's size, canvas pixels
    /// @param absolute_area where the absolute mode maps the pad, canvas pixels
    void configure(const PadSettings& settings, Vec2 canvas, Area absolute_area) noexcept;
    /// Takes a thumb landing on the pad; stops a glide. In the absolute mode the next
    /// touch_move, with the landing point or another, places the pointer.
    ///
    /// @param pad where it landed, 0..1 each way, y down
    /// @param now_ns the time, nanoseconds on the caller's clock
    void touch_down(Vec2 pad, uint64_t now_ns) noexcept;
    /// Takes the thumb's motion on the pad.
    ///
    /// @param pad where it is, 0..1 each way, y down
    /// @param now_ns the time, nanoseconds on the caller's clock
    /// @return the pointer's step: a move (relative) or a place (absolute); nothing without a
    ///     thumb down
    PointerStep touch_move(Vec2 pad, uint64_t now_ns) noexcept;
    /// Takes the thumb leaving the pad; starts a glide when glide is on and the thumb was fast.
    ///
    /// @param now_ns the time, nanoseconds on the caller's clock
    void touch_up(uint64_t now_ns) noexcept;
    /// Takes the pad's click going down: the pointer holds still for click_lock_ms.
    ///
    /// @param now_ns the time, nanoseconds on the caller's clock
    void press(uint64_t now_ns) noexcept;
    /// Takes the pad's click coming up: the pointer holds still for click_lock_ms.
    ///
    /// @param now_ns the time, nanoseconds on the caller's clock
    void release(uint64_t now_ns) noexcept;
    /// Moves time on: a glide's step; nothing otherwise.
    ///
    /// @param now_ns the time, nanoseconds on the caller's clock
    /// @return the pointer's step
    PointerStep advance(uint64_t now_ns) noexcept;
    /// Ends a glide and forgets the touch.
    void stop() noexcept;
    /// Returns whether a thumb is on the pad.
    ///
    /// @return whether the pad is touched
    [[nodiscard]] bool touched() const noexcept;
    /// Returns the thumb's last point on the pad.
    ///
    /// @return 0..1 each way, y down
    [[nodiscard]] Vec2 touch() const noexcept;
    /// Returns whether the pointer is gliding.
    ///
    /// @return whether a glide runs
    [[nodiscard]] bool gliding() const noexcept;

  private:

    /// Returns a relative step for the thumb moving by a pad delta over a time.
    ///
    /// @param delta the thumb's travel, pad widths
    /// @param elapsed_ns the time it took, nanoseconds
    /// @return the step
    PointerStep relative_step(Vec2 delta, uint64_t elapsed_ns) noexcept;

    float gain_px_per_pad_{};     ///< canvas pixels a pad width at ×1
    float greatest_gain_{1.0f};   ///< the acceleration's top gain
    bool glide_on_{};             ///< Trackpad glide
    bool absolute_{};             ///< Pointer (absolute)
    Area area_{};                 ///< the absolute mode's area, canvas pixels
    bool touched_{};              ///< a thumb is on the pad
    Vec2 touch_{};                ///< the thumb's last point, 0..1 each way
    Vec2 anchor_{};               ///< the pad point the next move counts from
    uint64_t landed_ns_{};        ///< when the thumb landed
    uint64_t last_sample_ns_{};   ///< when the thumb last moved the pointer
    uint64_t lock_until_ns_{};    ///< the click lock ends then
    Vec2 velocity_px_per_s_{};    ///< the pointer's last speed, relative
    std::optional<Vec2> place_{}; ///< the absolute pointer's last place
    bool gliding_{};              ///< a glide runs
    Vec2 glide_px_per_s_{};       ///< the glide's speed now
    uint64_t glide_ns_{};         ///< the glide's last step
    TickCounter ticks_{};         ///< travel into ticks
};

/// Returns a pad point mapped onto an area aspect-fitted to the middle of the pad: the largest
/// rectangle of the area's shape centred in the square pad maps onto the area, and points of
/// the pad outside it map onto the area's nearest edge.
///
/// @param pad the point, 0..1 each way, y down
/// @param area the area, canvas pixels
/// @return the point in the area, canvas pixels
[[nodiscard]] Vec2 pad_to_area(Vec2 pad, Area area) noexcept;

/// Gyro yaw and pitch as fine pointer motion: speed 100 % moves half the canvas width a radian.
/// Turning the pad's far end up moves the pointer up and turning it left moves the pointer
/// left; roll moves nothing.
class GyroPointer {
  public:

    /// Sets the speed and the canvas.
    ///
    /// @param settings the Controller section's settings
    /// @param canvas the canvas's size, canvas pixels
    void configure(const PadSettings& settings, Vec2 canvas) noexcept;
    /// Takes the gyro's angular velocity over the time since the last call.
    ///
    /// @param pitch angular velocity about the pad's x axis, radians a second, positive as the
    ///     far end comes up
    /// @param yaw angular velocity about the pad's y axis, radians a second, positive as the pad
    ///     turns left
    /// @param gate whether the setting's touch is held (or the setting is Always)
    /// @param now_ns the time, nanoseconds on the caller's clock
    /// @return the pointer's step; nothing while `gate` is false or Gyro pointer is Off, and
    ///     nothing on the first call
    PointerStep advance(float pitch, float yaw, bool gate, uint64_t now_ns) noexcept;
    /// Forgets the last call's time.
    void reset() noexcept;

  private:

    float gain_px_per_radian_{}; ///< canvas pixels a radian
    bool on_{};                  ///< Gyro pointer is not Off
    bool started_{};             ///< advance() was called since the last reset
    uint64_t last_ns_{};         ///< the last call's time, nanoseconds
    TickCounter ticks_{};        ///< travel into ticks
};

// ---- The stick cursor (sticks scheme, and Right stick: Pointer) ----

/// The stick deflection under which the stick cursor does not move.
inline constexpr float stick_inner_dead_zone = 0.12f;
/// The stick deflection over which the stick cursor moves at full speed.
inline constexpr float stick_outer_dead_zone = 0.95f;
/// The stick cursor's full speed, in points a second.
inline constexpr float stick_speed_points = 1200.0f;
/// The power of the stick cursor's response curve.
inline constexpr float stick_curve = 2.2f;
/// How long a stick must stay near full deflection before the ramp starts, in milliseconds.
inline constexpr uint32_t stick_ramp_after_ms = 400;
/// How long the ramp takes to reach its gain, in milliseconds.
inline constexpr uint32_t stick_ramp_ms = 300;
/// The deflection over which the ramp runs.
inline constexpr float stick_ramp_deflection = 0.9f;
/// The ramp's greatest gain.
inline constexpr float stick_ramp_gain = 1.8f;
/// How near a target the stick cursor slows, in points.
inline constexpr float friction_points = 24.0f;
/// The speed factor near a target.
inline constexpr float friction_factor = 0.45f;
/// How near a lone target magnetism pulls the cursor onto it, in points.
inline constexpr float magnet_points = 16.0f;
/// A second target this near the first blocks magnetism, in points.
inline constexpr float magnet_rival_points = 8.0f;
/// How long magnetism takes to settle on a target, in milliseconds.
inline constexpr uint32_t magnet_ease_ms = 80;

/// Returns a stick's deflection past the radial dead zones: 0 within stick_inner_dead_zone,
/// rising linearly to 1 at stick_outer_dead_zone and beyond.
///
/// @param stick the stick, -1..1 each way
/// @return 0..1
[[nodiscard]] float stick_deflection(Vec2 stick) noexcept;

/// The stick cursor: a pointer moved by a stick.
///
/// Speed is stick_speed_points times the deflection past the dead zones to the power
/// stick_curve, in points a second; held at or past stick_ramp_deflection for
/// stick_ramp_after_ms it rises linearly to stick_ramp_gain over stick_ramp_ms. Within
/// friction_points of a target it is friction_factor of that. When the stick comes back to rest
/// with a target within magnet_points of the pointer and no other target within
/// magnet_rival_points of that one, the pointer eases onto the target over magnet_ease_ms
/// (Magnetism on, not blocked); moving the stick or a block ends the ease.
class StickCursor {
  public:

    /// Sets the speed and magnetism.
    ///
    /// @param settings the Controller section's settings
    /// @param px_per_point canvas pixels per point, > 0
    void configure(const PadSettings& settings, float px_per_point) noexcept;
    /// Moves the pointer by the stick over the time since the last call: radial dead zone,
    /// curve, ramp, friction near `targets` and, when the stick comes to rest, magnetism onto a
    /// lone target, unless `blocked` or magnetism is off.
    ///
    /// @param stick the stick, -1..1 each way, y down
    /// @param pointer the pointer, canvas pixels
    /// @param targets visible units, canvas pixels
    /// @param blocked R2 held, placing, or the pointer over the HUD: no friction or magnetism
    /// @param now_ns the time, nanoseconds on the caller's clock
    /// @return the pointer's step (nothing on the first call, which starts the clock)
    PointerStep advance(
        Vec2 stick, Vec2 pointer, std::span<const Vec2> targets, bool blocked, uint64_t now_ns
    ) noexcept;
    /// Forgets the stick's history (ramp, magnetism, the last call's time).
    void reset() noexcept;
    /// Returns whether magnetism is easing the pointer onto a target.
    ///
    /// @return whether an ease runs
    [[nodiscard]] bool easing() const noexcept;

  private:

    float px_per_point_{1.0f}; ///< canvas pixels per point
    bool magnetism_{true};     ///< Magnetism (stick pointer)
    bool started_{};           ///< advance() was called since the last reset
    uint64_t last_ns_{};       ///< the last call's time, nanoseconds
    bool moving_{};            ///< the stick was out of its dead zone at the last call
    std::optional<uint64_t> full_since_ns_{}; ///< since when the stick is at ramp deflection
    bool easing_{};                           ///< magnetism is easing onto a target
    Vec2 ease_from_{};                        ///< where the ease started, canvas pixels
    Vec2 ease_to_{};                          ///< the target, canvas pixels
    uint64_t ease_start_ns_{};                ///< when the ease started
    TickCounter ticks_{};                     ///< travel into ticks
};
/// A stick flick's direction.
enum class Flick : uint8_t {
    none,  ///< no flick
    left,  ///< flicked left
    right, ///< flicked right
    up,    ///< flicked up
    down,  ///< flicked down
};

/// The deflection along one axis past which a stick gives a flick.
inline constexpr float flick_out_deflection = 0.7f;
/// The deflection along both axes under which a stick may flick again.
inline constexpr float flick_back_deflection = 0.3f;

/// A stick flicked past 0.7 one way gives that way once, until it returns under 0.3.
class FlickDetector {
  public:

    /// Takes the stick's position.
    ///
    /// @param stick the stick, -1..1 each way, y down
    /// @return the flick this position completes, or none
    Flick update(Vec2 stick) noexcept;
    /// Arms the detector again, as when the stick has come back.
    void reset() noexcept;

  private:

    bool armed_{true}; ///< the stick has returned near the middle since the last flick
};

// ---- Haptics ----

/// The moments the pad gives a feel.
enum class Feel : uint8_t {
    pointer_tick,  ///< the pointer travelled tick_travel_px
    target_detent, ///< the stick cursor settled on a target
    wedge_change,  ///< a ring's aim moved to another wedge
    hold_started,  ///< a hold passed its delay
    box_started,   ///< a selection box started
    site_refused,  ///< a building site was refused
    queue_reduced, ///< a factory's queue went down
};
/// Which trackpad or motor a feel plays on.
enum class PadSide : uint8_t {
    left,  ///< the left
    right, ///< the right
    both,  ///< both
};

/// A rumble: low and high motor speeds, which the Deck plays on its left and right pads.
struct Rumble {
    uint16_t low{};  ///< the low-frequency motor's speed, 0..0xffff
    uint16_t high{}; ///< the high-frequency motor's speed, 0..0xffff
    uint32_t ms{};   ///< how long, milliseconds
};

/// Returns the rumble for a feel at a strength. The Deck plays the low motor on its left pad and
/// the high motor on its right, so a feel of the right thumb (the pointer, the rings, the box, a
/// refused site, a factory's queue) runs the high motor alone and a hold started runs both. The
/// pointer tick is the shortest and lightest, a target detent firmer, a refused site a thump;
/// Strong runs each motor harder than Light.
///
/// @param feel the moment
/// @param strength the Haptics setting
/// @return the rumble; none when Haptics is Off
[[nodiscard]] std::optional<Rumble> rumble_for(Feel feel, Haptics strength) noexcept;
/// A trackpad pulse's kind: the Deck's own tick or click.
enum class PulseKind : uint8_t {
    tick,  ///< a light tick
    click, ///< a click
};

/// A trackpad pulse: the Deck's own tick or click on one pad (where the driver takes it).
struct TrackpadPulse {
    PadSide side{PadSide::right};    ///< which pad
    PulseKind kind{PulseKind::tick}; ///< tick or click
    int8_t gain_db{};                ///< the pulse's gain, decibels
};

/// Returns the trackpad pulse for a feel at a strength: the same sides as rumble_for (a hold
/// started on both pads, every other feel on the right), a tick for the light feels (pointer
/// tick, wedge change, box started, queue reduced) and a click for the firm ones (target
/// detent, hold started, site refused); Strong is louder than Light.
///
/// @param feel the moment
/// @param strength the Haptics setting
/// @return the pulse; none when Haptics is Off
[[nodiscard]] std::optional<TrackpadPulse> trackpad_pulse_for(Feel feel, Haptics strength) noexcept;

// ---- Prompts and glyphs ----

/// A set of button glyphs.
enum class GlyphStyle : uint8_t {
    steam_deck,  ///< Steam Deck's labels
    xbox,        ///< Xbox's labels
    playstation, ///< PlayStation's marks
    nintendo,    ///< Nintendo's labels, by place
};
/// Returns whether prompts show at all.
///
/// @param prompts the Button prompts setting
/// @return false for Off
[[nodiscard]] bool prompts_shown(Prompts prompts) noexcept;
/// Returns the glyph set: the chosen one; Automatic gives steam_deck for a Deck (Steam Input's
/// virtual pad included), playstation and nintendo for their types, else xbox.
///
/// @param prompts the Button prompts setting
/// @param type the pad in use's type
/// @return the glyph set
[[nodiscard]] GlyphStyle glyph_style_for(Prompts prompts, PadType type) noexcept;
/// How a glyph is drawn: project-made shapes, never console or Valve art.
enum class GlyphShape : uint8_t {
    letter_circle, ///< A B X Y in a disc (Deck, Xbox); Nintendo's letters
    pill,          ///< L1 R1 L2 R2 L4 L5 R4 R5, LB RB LT RT, ZL ZR, L R
    cross,         ///< PlayStation's bottom face button, drawn as a mark
    circle,        ///< PlayStation's right face button, drawn as a mark
    square,        ///< PlayStation's left face button, drawn as a mark
    triangle,      ///< PlayStation's top face button, drawn as a mark
    dpad,          ///< a D-pad with one arm lit (GlyphSpec::direction)
    trackpad,      ///< a rounded square with a dot (Side in GlyphSpec::side)
    stick,         ///< a ring with a dot; L3/R3 with a press mark
    view,          ///< two overlapping squares (Deck, Xbox), Create on PlayStation, "−" on Nintendo
    menu,          ///< three bars (Deck, Xbox), Options on PlayStation, "+" on Nintendo
};

/// How one glyph is drawn.
struct GlyphSpec {
    GlyphShape shape{GlyphShape::pill}; ///< the shape
    std::string_view text{};            ///< the letters drawn, empty for marks
    uint8_t direction{};                ///< dpad: 0 up, 1 right, 2 down, 3 left
    Side side{Side::left};              ///< trackpad, stick
    bool pressed{};                     ///< stick click or pad press
};

/// Returns how a button's glyph is drawn in a style (by place: Nintendo's bottom button is
/// labelled B, its right one A).
///
/// Face buttons: A B X Y in discs (Deck, Xbox), PlayStation's four marks, Nintendo's B A Y X
/// for the bottom, right, left and top buttons. Bumpers and triggers: pills L1 R1 L2 R2 (Deck,
/// PlayStation), LB RB LT RT (Xbox), L R ZL ZR (Nintendo). Grips: pills L4 L5 R4 R5, P3 P4 P1
/// P2 in the Xbox set (its paddles' own names). View and Menu: their marks, with "-" and "+"
/// as Nintendo's text. The D-pad's arms, the trackpads (pressed) and the sticks (pressed for
/// L3 and R3, not for their touches) by shape and side.
///
/// @param button the physical button
/// @param style the glyph set
/// @return how its glyph is drawn
[[nodiscard]] GlyphSpec glyph_spec(PadButton button, GlyphStyle style) noexcept;

} // namespace oa::ui::pad_controls
