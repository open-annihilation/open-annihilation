// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch dispatcher's own state, kept in Runtime::TouchState, and its
// access to the runtime (docs/touch-controls.md).
#pragma once

#include "oa/ui/engine_settings.hpp"
#include "oa/ui/touch_gestures.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <optional>
#include <stdint.h>
#include <string>

namespace oa::app {

class Runtime;

/// What a finger was claimed by when it landed.
enum class TouchTarget : uint8_t {
    none,        ///< the entry is unused
    control,     ///< a touch control (or an open sheet or radial)
    hud_gadget,  ///< a gadget of the 3.1c panel or of a placed region
    minimap,     ///< the minimap
    battlefield, ///< the battlefield: fed to the battlefield recogniser
    frontend,    ///< a menu, dialog or settings overlay: synthetic mouse events
};

/// One finger the dispatcher has claimed, from landing to lift or cancel.
struct ClaimedFinger {
    oa::ui::touch_gestures::FingerId id{};           ///< the finger
    TouchTarget target{TouchTarget::none};           ///< what claimed it
    oa::ui::touch_hud::ControlRect control{};        ///< the control, for TouchTarget::control
    int16_t gadget{-1};                              ///< the HUD or menu gadget, -1 for none
    oa::ui::touch_gestures::Recogniser recogniser{}; ///< tap and hold of a non-battlefield finger
    float x{};                                       ///< last canvas point
    float y{};                                       ///< last canvas point
    float start_x{};                                 ///< canvas point it landed at
    float start_y{};                                 ///< canvas point it landed at
    float press_x{};      ///< where its synthetic press went (snapped onto the gadget)
    float press_y{};      ///< where its synthetic press went (snapped onto the gadget)
    float sent_x{};       ///< where its last synthetic pointer event went
    float sent_y{};       ///< where its last synthetic pointer event went
    uint64_t down_ns{};   ///< when it landed, nanoseconds
    bool moved{};         ///< it travelled past the slop since landing
    bool slid_off{};      ///< it left its control or gadget: the lift does nothing
    bool held{};          ///< a hold acted: the lift does nothing more
    bool fired{};         ///< a self-destruct hold gave its click or key
    bool self_destruct{}; ///< a self-destruct gadget: nothing at landing, the click after a hold
    bool pressed{};       ///< a synthetic left press was sent and not yet released
    bool build_button{};  ///< the gadget is a unit or weapon build button
};

/// The most fingers the dispatcher keeps claimed at once.
inline constexpr std::size_t max_claimed_fingers = 10;

/// The Touch settings as the dispatcher last read them (at each finger event and frame).
struct TouchSettingsSeen {
    /// One-finger drag on the battlefield.
    oa::ui::engine_settings::TouchDrag drag{oa::ui::engine_settings::TouchDrag::automatic};
    uint32_t hold_ms{oa::ui::engine_settings::default_touch_hold_ms}; ///< hold delay
    bool one_action{};  ///< QUEUE, ADD and x5 turn off after one action
    bool haptics{true}; ///< haptics are played
    bool left_handed{}; ///< the controls are mirrored
};

/// The dispatcher's own state.
struct TouchDispatch {
    /// The battlefield's fingers' recogniser.
    oa::ui::touch_gestures::Recogniser battlefield{};
    /// Every claimed finger; entries with TouchTarget::none are unused.
    std::array<ClaimedFinger, max_claimed_fingers> fingers{};
    uint8_t finger_count{};           ///< claimed fingers down (Runtime::touch_finger_count)
    bool forced{};                    ///< the check or --touch-controls turned the controls on
    bool seen_direct_finger{};        ///< a direct-touch finger arrived in this run
    bool accept_unregistered_touch{}; ///< the check's unregistered touch device is accepted
    /// The check's clock in nanoseconds; finger times come from it while set.
    std::optional<uint64_t> check_clock_ns{};
    SDL_Keymod pulse{};    ///< modifiers a synthetic key or radial pick holds for one call
    float inertia_x{};     ///< pan inertia, canvas pixels per second
    float inertia_y{};     ///< pan inertia, canvas pixels per second
    bool auto_scrolling{}; ///< a box or ghost drag is in the edge band
    float auto_scroll_x{}; ///< the dragging finger's canvas point
    float auto_scroll_y{}; ///< the dragging finger's canvas point
    /// The battlefield finger the cursor follows, canvas pixels; none without one.
    std::optional<std::array<float, 2>> finger_point{};
    bool hints_set{}; ///< set_input_hints ran

    TouchSettingsSeen settings{}; ///< the Touch settings last read
    bool pointer_seen{};          ///< a mouse, trackpad or pen moved since the last finger
    /// The control a mouse button was pressed on, until its release.
    std::optional<oa::ui::touch_hud::ControlRect> pointer_control{};
    uint64_t tick_ns{};        ///< when tick_touch last ran, nanoseconds (0 before)
    uint64_t last_tap_ns{};    ///< lift of the last tap on a gadget or menu
    float last_tap_x{};        ///< where that tap lifted, canvas pixels
    float last_tap_y{};        ///< where that tap lifted, canvas pixels
    uint8_t last_tap_clicks{}; ///< the clicks that tap gave (1 or 2)
    bool box_active{};         ///< a battlefield box drag holds the left button
    bool megamap_drag{};       ///< a one-finger drag on the open megamap holds its left button
    bool megamap_placed{};     ///< the last tap on the open megamap placed a building
    bool scroll_active{};      ///< a one-finger drag moves the camera
    bool ghost_drag{};         ///< a one-finger drag moves the building's ghost
    float drag_last_x{};       ///< the scrolling finger's previous canvas point
    float drag_last_y{};       ///< the scrolling finger's previous canvas point
    bool placement_touch{};    ///< the pending building is placed by touch (ghost anchored)
    /// The pad started the pending building's placement: the ghost follows the pointer until a
    /// finger lands on the battlefield.
    bool placement_by_pad{};
    bool hold_placed{};        ///< the battlefield finger's hold placed a building: no radial
    uint16_t drawer_unit{};    ///< the unit the drawer's page belongs to
    uint64_t look_signature{}; ///< what the controls showed last frame (bumps the revision)
    bool* running{};           ///< the running flag of the event being taken, or null
};

/// The dispatcher's helpers that reach the runtime's private members: static functions that
/// take Runtime&. Defined across runtime_touch.cpp (routing), runtime_touch_actions.cpp
/// (gestures and controls) and runtime_touch_camera.cpp (inertia and auto-scroll).
struct TouchDispatchAccess {
    // ---- routing (runtime_touch.cpp) ---------------------------------------------------

    /// Takes a finger event of an accepted touch device and routes it.
    ///
    /// @param runtime the runtime
    /// @param event a finger event
    /// @param running the run loop's flag, cleared when a synthetic click ends the run
    /// @return whether the event was taken
    static bool take_finger(Runtime& runtime, const SDL_Event& event, bool& running);
    /// Takes the mouse events SDL made from a finger while touch controls are on, and a real
    /// pointer's left button on a touch control.
    ///
    /// @param runtime the runtime
    /// @param event a mouse motion or button event
    /// @return whether the event was taken
    static bool take_pointer(Runtime& runtime, const SDL_Event& event);
    /// Returns the dispatcher's clock: the check's while set, else SDL's.
    ///
    /// @param runtime the runtime
    /// @return nanoseconds
    [[nodiscard]] static uint64_t now_ns(const Runtime& runtime);
    /// Reads the Touch settings into TouchDispatch::settings.
    ///
    /// @param runtime the runtime
    static void read_settings(Runtime& runtime);
    /// Returns the thresholds a recogniser uses now.
    ///
    /// @param runtime the runtime
    /// @param battlefield whether for the battlefield (its drag mode), else for a control
    /// @return the thresholds
    [[nodiscard]] static oa::ui::touch_gestures::Thresholds
    thresholds(const Runtime& runtime, bool battlefield);
    /// Returns canvas pixels for a distance in points on the current screen.
    ///
    /// @param runtime the runtime
    /// @param points the distance, points
    /// @return canvas pixels
    [[nodiscard]] static float points_to_pixels(const Runtime& runtime, float points);
    /// Sends a synthetic mouse event through dispatch_event, marked as a finger's
    /// (which = SDL_TOUCH_MOUSEID) at canvas coordinates (windowID 0).
    ///
    /// @param runtime the runtime
    /// @param type SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN or SDL_EVENT_MOUSE_BUTTON_UP
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param clicks the clicks a button event counts
    /// @param button the button of a button event: the left, or the right for the right press
    ///        a tap gives on the open megamap
    static void send_mouse(
        Runtime& runtime,
        uint32_t type,
        float x,
        float y,
        uint8_t clicks,
        uint8_t button = SDL_BUTTON_LEFT
    );
    /// Sends a whole synthetic left click: motion, press and release at one point.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param clicks 1, or 2 for a double click
    static void send_click(Runtime& runtime, float x, float y, uint8_t clicks);
    /// Returns the loaded HUD's gadget a match canvas point lies on, as update_pointer finds it.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param[out] on_gadget whether any gadget lies there, even one the pointer would skip
    /// @return the gadget the pointer would hover, or none
    [[nodiscard]] static std::optional<std::size_t>
    hud_gadget_at(const Runtime& runtime, float x, float y, bool* on_gadget = nullptr);
    /// Returns the frontend gadget a canvas point lies on, as update_pointer finds it.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param[out] on_gadget whether any gadget lies there, even one that takes no press
    /// @return the gadget a press would select, or none
    [[nodiscard]] static std::optional<std::size_t>
    frontend_gadget_at(const Runtime& runtime, float x, float y, bool* on_gadget = nullptr);
    /// Returns a canvas point in the frontend panel's own pixels, where its
    /// gadgets lie, as update_pointer moves it.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @return the point in the panel's pixels
    [[nodiscard]] static std::array<float, 2>
    frontend_panel_point(const Runtime& runtime, float x, float y);
    /// Returns how far a canvas point lies from a frontend gadget, to the
    /// edge of the rectangle a press finds it in: none inside it.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param gadget the gadget
    /// @return the distance, in canvas pixels; infinite for a gadget that is not loaded
    [[nodiscard]] static float
    frontend_gadget_distance(const Runtime& runtime, float x, float y, std::size_t gadget);
    /// Finds the nearest gadget within a radius of a point by probing rings around it.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param frontend whether frontend gadgets, else the match HUD's
    /// @param[out] gadget the gadget found
    /// @return the probed canvas point on the gadget, or none within gadget_pick_points
    [[nodiscard]] static std::optional<std::array<float, 2>>
    nearest_gadget(const Runtime& runtime, float x, float y, bool frontend, std::size_t* gadget);
    /// Claims a finger that just landed by what lies under it (SPEC 7.1 step 3).
    ///
    /// @param runtime the runtime
    /// @param[in,out] finger the finger; its point is set, its target and snap are written
    static void claim(Runtime& runtime, ClaimedFinger& finger);
    /// Returns a claimed finger if its entry still holds it (an action may have changed the
    /// screen, which drops every finger).
    ///
    /// @param runtime the runtime
    /// @param slot the finger's entry
    /// @param id the finger
    /// @return the finger, or null when its entry no longer holds it
    [[nodiscard]] static ClaimedFinger*
    finger_at(Runtime& runtime, std::size_t slot, const oa::ui::touch_gestures::FingerId& id);
    /// Counts the claimed fingers into TouchDispatch::finger_count.
    ///
    /// @param runtime the runtime
    static void recount(Runtime& runtime);
    /// Drops every finger, recogniser, drag, press and sheet without acting.
    ///
    /// @param runtime the runtime
    static void reset_fingers(Runtime& runtime);
    /// Refreshes the HUD state from the match, lays the controls out and bumps the revision
    /// when anything drawn changed (SPEC 7.5).
    ///
    /// @param runtime the runtime
    /// @param now nanoseconds
    static void refresh_hud(Runtime& runtime, uint64_t now);

    // ---- gestures and controls (runtime_touch_actions.cpp) ------------------------------

    /// Acts on the battlefield recogniser's gestures (SPEC 7.2); pinches after pans.
    ///
    /// @param runtime the runtime
    /// @param batch the gestures, in order
    static void
    battlefield_gestures(Runtime& runtime, const oa::ui::touch_gestures::GestureBatch& batch);
    /// Acts when a claimed finger lands: presses, latches, the pressed look.
    ///
    /// @param runtime the runtime
    /// @param slot the finger's entry in TouchDispatch::fingers
    /// @param now nanoseconds
    static void finger_landed(Runtime& runtime, std::size_t slot, uint64_t now);
    /// Acts when a claimed finger other than a battlefield one moves.
    ///
    /// @param runtime the runtime
    /// @param slot the finger's entry
    /// @param now nanoseconds
    static void finger_moved(Runtime& runtime, std::size_t slot, uint64_t now);
    /// Acts when a claimed finger other than a battlefield one lifts or is cancelled.
    ///
    /// @param runtime the runtime
    /// @param slot the finger's entry
    /// @param cancelled whether the finger was cancelled rather than lifted
    /// @param now nanoseconds
    static void finger_lifted(Runtime& runtime, std::size_t slot, bool cancelled, uint64_t now);
    /// Acts on the gestures of a non-battlefield finger's own recogniser (holds and the
    /// minimap's taps and drags).
    ///
    /// @param runtime the runtime
    /// @param slot the finger's entry
    /// @param batch the gestures
    /// @param now nanoseconds
    static void finger_gestures(
        Runtime& runtime,
        std::size_t slot,
        const oa::ui::touch_gestures::GestureBatch& batch,
        uint64_t now
    );
    /// Runs a touch control's tap action (SPEC 7.3).
    ///
    /// @param runtime the runtime
    /// @param control the control
    /// @param now nanoseconds
    static void
    control_tap(Runtime& runtime, const oa::ui::touch_hud::ControlRect& control, uint64_t now);
    /// Runs a touch control's hold action, or shows its help tip.
    ///
    /// @param runtime the runtime
    /// @param control the control
    /// @param now nanoseconds
    /// @return whether the hold acted, so the lift does nothing more
    static bool
    control_hold(Runtime& runtime, const oa::ui::touch_hud::ControlRect& control, uint64_t now);
    /// Runs a finger's tap on a build ring wedge: a press on its gadget (a building armed, one
    /// more queued, a page turned, a standing order stepped), INFO, or the self-destruct hint.
    ///
    /// @param runtime the runtime
    /// @param control the wedge
    /// @param now nanoseconds
    static void
    build_wedge_tap(Runtime& runtime, const oa::ui::touch_hud::ControlRect& control, uint64_t now);
    /// Runs a finger's hold on a build ring wedge: a build picture's right button.
    ///
    /// @param runtime the runtime
    /// @param control the wedge
    /// @return whether the hold acted, so the lift does nothing more
    static bool build_wedge_hold(Runtime& runtime, const oa::ui::touch_hud::ControlRect& control);
    /// Returns the latch a control is, if it is QUEUE, ADD or x5.
    ///
    /// @param control the control
    /// @return the latch, or none
    [[nodiscard]] static std::optional<oa::ui::touch_hud::Latch>
    control_latch(oa::ui::touch_hud::Control control) noexcept;
    /// Adds a control to the pressed looks.
    ///
    /// @param runtime the runtime
    /// @param control the control
    static void press_look(Runtime& runtime, const oa::ui::touch_hud::ControlRect& control);
    /// Removes a control from the pressed looks.
    ///
    /// @param runtime the runtime
    /// @param control the control
    static void release_look(Runtime& runtime, const oa::ui::touch_hud::ControlRect& control);
    /// Closes the open sheet; the drawer and MORE reload the panel's page when closed
    /// explicitly with no building pending.
    ///
    /// @param runtime the runtime
    /// @param explicit_close whether the player closed it (✕, again, outside, two fingers)
    static void close_sheet(Runtime& runtime, bool explicit_close);
    /// Closes the radial, or else the open sheet (explicitly).
    ///
    /// @param runtime the runtime
    /// @return whether anything was open
    static bool close_overlay(Runtime& runtime);
    /// Shows a help line in the tip bubble for 2.5 s.
    ///
    /// @param runtime the runtime
    /// @param text the line, untranslated
    /// @param x canvas pixels the bubble points at
    /// @param y canvas pixels the bubble points at
    /// @param now nanoseconds
    static void show_tip(Runtime& runtime, std::string text, float x, float y, uint64_t now);
    /// Re-applies the ghost anchor, its legality and its header each frame while a building is
    /// placed by touch, and starts or ends touch placement as the engine arms or ends it. A
    /// building the pad started placing (TouchDispatch::placement_by_pad) is left to the
    /// pointer until a finger lands on the battlefield; an ended placement clears the flag.
    ///
    /// @param runtime the runtime
    static void refresh_placement(Runtime& runtime);
    /// Hands the building the pad started placing to the touch model: the ghost is anchored
    /// where the pointer left it (on the battlefield), and a tap moves it from then on. Clears
    /// TouchDispatch::placement_by_pad.
    ///
    /// @param runtime the runtime
    static void hand_placement_to_touch(Runtime& runtime);
    /// Holds FORCE while a finger or pointer rests on its chip, or lets it go: writes
    /// HudState::force_touch and gives the order modifiers again when it changes.
    ///
    /// @param runtime the runtime
    /// @param held whether the chip is held now
    static void hold_force(Runtime& runtime, bool held);
    /// Runs the hold timers: self-destruct gadgets' clicks, SELF-DESTRUCT · HOLD's progress.
    ///
    /// @param runtime the runtime
    /// @param now nanoseconds
    static void hold_timers(Runtime& runtime, uint64_t now);
    /// Returns what a tap gives for an order cursor.
    ///
    /// @param cursor an oa::sim::gameplay_input::OrderCursor value
    /// @return the tap's action
    [[nodiscard]] static oa::ui::touch_hud::TapAction tap_action_of(uint8_t cursor) noexcept;
    /// Keeps the pointer under a finger resting on the battlefield (the hover replacement).
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    static void battlefield_hover(Runtime& runtime, float x, float y);
    /// Gives a battlefield tap: the unit nearest the finger, then a click (or, in the
    /// right-click interface, the right press an order needs), then the latches are used.
    /// On the open megamap the tap is the mouse's click there, at the finger, a right press
    /// sent as the mouse's, so the megamap takes it (Runtime::megamap_pointer).
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param taps 1, or 2 for a double tap
    static void battlefield_tap(Runtime& runtime, float x, float y, uint8_t taps);
    /// Returns the point a tap acts at: the finger's, or with no unit under it the nearest
    /// point within unit_pick_points where one lies (a ring of probes at 4, 8 and 12 points).
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @return canvas pixels; the pointer is left there
    [[nodiscard]] static std::array<float, 2> fat_finger_point(Runtime& runtime, float x, float y);
    /// Starts a one-finger drag on the battlefield: the ghost, the map or a box; on the open
    /// megamap, the megamap's box.
    ///
    /// @param runtime the runtime
    /// @param gesture the drag_began gesture
    static void drag_began(Runtime& runtime, const oa::ui::touch_gestures::Gesture& gesture);
    /// Follows a one-finger drag on the battlefield.
    ///
    /// @param runtime the runtime
    /// @param gesture the drag_moved gesture
    static void drag_moved(Runtime& runtime, const oa::ui::touch_gestures::Gesture& gesture);
    /// Ends a one-finger drag on the battlefield: a box selects or gives its area order.
    ///
    /// @param runtime the runtime
    /// @param gesture the drag_ended gesture
    static void drag_ended(Runtime& runtime, const oa::ui::touch_gestures::Gesture& gesture);
    /// Drops a running drag without acting (a box gives nothing).
    ///
    /// @param runtime the runtime
    static void cancel_drags(Runtime& runtime);
    /// Moves the map under a scrolling finger to its new point.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    static void scroll_to(Runtime& runtime, float x, float y);
    /// Moves the ghost of the building being placed.
    ///
    /// @param runtime the runtime
    /// @param x the finger's canvas x
    /// @param y the finger's canvas y
    static void move_ghost(Runtime& runtime, float x, float y);

    /// Places the pending building by a hold or a double tap: at the ghost
    /// when the finger is within ghost_reach_points of it, else at the
    /// finger, the ghost moved there first.
    ///
    /// @param runtime the runtime
    /// @param x the finger's canvas column
    /// @param y the finger's canvas row
    static void place_at(Runtime& runtime, float x, float y);
    /// Builds at the ghost: the refused-site haptic, then place_pending_build.
    ///
    /// @param runtime the runtime
    static void confirm_placement(Runtime& runtime);
    /// Opens the radial order menu at a held point (SPEC 7.4); on the open megamap for the icon
    /// and the ground under it, whose orders are the megamap's click there, and none while a
    /// panel holds the match.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    static void open_radial(Runtime& runtime, float x, float y);
    /// Acts on a tapped radial wedge; a greyed one leaves the menu open.
    ///
    /// @param runtime the runtime
    /// @param item the wedge's item
    static void radial_pick(Runtime& runtime, oa::ui::touch_hud::RadialItem item);
    /// Opens a sheet, closing the radial and any other sheet first.
    ///
    /// @param runtime the runtime
    /// @param sheet the sheet
    static void open_sheet(Runtime& runtime, oa::ui::touch_hud::Sheet sheet);
    /// Opens the phone's build drawer on the builder's page, or on the orders page.
    ///
    /// @param runtime the runtime
    static void open_drawer(Runtime& runtime);
    /// Shows a build page in the drawer, keeping the drawer's tab and page in step.
    ///
    /// @param runtime the runtime
    /// @param page the page, from 1
    static void show_drawer_page(Runtime& runtime, int page);
    /// Runs a SELECT ▾ item through its key and closes the menu.
    ///
    /// @param runtime the runtime
    /// @param index the SelectItem
    static void select_item(Runtime& runtime, uint8_t index);
    /// Opens the unit info panel for the primary selected unit, as F1 does over it.
    ///
    /// @param runtime the runtime
    static void show_unit_info(Runtime& runtime);
    /// Gives a HUD gadget finger's hold: a build button's right button, else its help.
    ///
    /// @param runtime the runtime
    /// @param slot the finger's entry
    /// @param now nanoseconds
    static void hud_hold(Runtime& runtime, std::size_t slot, uint64_t now);
    /// Releases a HUD gadget finger's press: a click where it pressed, or nothing when it slid
    /// off, was cancelled or a hold acted.
    ///
    /// @param runtime the runtime
    /// @param finger the finger as it lifted
    /// @param cancelled whether it was cancelled
    /// @param now nanoseconds
    static void
    hud_release(Runtime& runtime, const ClaimedFinger& finger, bool cancelled, uint64_t now);
    /// Ends a HUD gadget finger's press so that it acts on nothing.
    ///
    /// @param runtime the runtime
    /// @param finger the finger
    static void cancel_hud_press(Runtime& runtime, const ClaimedFinger& finger);
    /// Releases a frontend finger's press where it lies.
    ///
    /// @param runtime the runtime
    /// @param finger the finger as it lifted
    /// @param cancelled whether it was cancelled
    /// @param now nanoseconds
    static void
    frontend_release(Runtime& runtime, const ClaimedFinger& finger, bool cancelled, uint64_t now);
    /// Returns the point on a finger's own gadget nearest its point, if it is still on it.
    ///
    /// @param runtime the runtime
    /// @param finger the finger
    /// @return canvas pixels on the gadget, or none when the finger left it
    [[nodiscard]] static std::optional<std::array<float, 2>>
    on_own_gadget(Runtime& runtime, const ClaimedFinger& finger);
    /// Counts the clicks a lifting finger gives: 2 for a second quick tap near the first.
    ///
    /// @param runtime the runtime
    /// @param finger the finger as it lifted
    /// @param now nanoseconds
    /// @return 1 or 2
    [[nodiscard]] static uint8_t
    tap_clicks(Runtime& runtime, const ClaimedFinger& finger, uint64_t now);

    // ---- camera (runtime_touch_camera.cpp) ----------------------------------------------

    /// Carries a pan's inertia on with no finger down: decays with a 0.25 s time constant and
    /// stops under 20 points a second.
    ///
    /// @param runtime the runtime
    /// @param elapsed_ns time since the previous frame
    static void step_inertia(Runtime& runtime, uint64_t elapsed_ns);
    /// Scrolls toward the battlefield's edge while a box or ghost drag's finger is in the edge
    /// band, at the keyboard's scroll rate, then follows the box or the ghost.
    ///
    /// @param runtime the runtime
    /// @param elapsed_ns time since the previous frame
    static void step_auto_scroll(Runtime& runtime, uint64_t elapsed_ns);
};

} // namespace oa::app
