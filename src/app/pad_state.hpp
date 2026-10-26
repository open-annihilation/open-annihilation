// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gamepad dispatcher's state for one runtime, the helpers that reach the
// runtime's private members, and the start of SDL's gamepad subsystem
// (docs/controllers.md). Until a gamepad is there none of it is made.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <optional>
#include <stdint.h>
#include <vector>

namespace oa::app {

/// The mouse the pad pointer's synthetic events name: no device registers it, and it is
/// neither SDL_TOUCH_MOUSEID nor SDL_PEN_MOUSEID, so the pointer rests like a mouse's.
inline constexpr SDL_MouseID pad_mouse_id = 0x70ad;
/// The most gamepads kept open at once.
inline constexpr std::size_t max_open_pads = 4;
/// The key Open Annihilation's Steam Input layout sends for the R4 grip, read as the grip
/// while a gamepad is open (docs/controllers.md).
inline constexpr SDL_Keycode grip_key_r4 = SDLK_F13;
/// The key the layout sends for the R5 grip, read as the grip while a gamepad is open.
inline constexpr SDL_Keycode grip_key_r5 = SDLK_F14;
/// The key the layout sends for the L4 grip, read as the grip while a gamepad is open.
inline constexpr SDL_Keycode grip_key_l4 = SDLK_F15;
/// The key the layout sends for the L5 grip, read as the grip while a gamepad is open.
inline constexpr SDL_Keycode grip_key_l5 = SDLK_F16;
/// The trackpads the dispatcher reads, by place: SDL's touchpad 0 is the left, 1 the right.
inline constexpr std::size_t pad_trackpad_count = 2;

/// One open gamepad.
struct OpenPad {
    SDL_JoystickID id{};                      ///< 0 for an unused entry
    SDL_Gamepad* gamepad{};                   ///< closed when SDL removes it
    oa::ui::pad_controls::PadTraits traits{}; ///< what it has
    /// False once the driver refused a trackpad pulse: rumble serves instead.
    bool trackpad_pulses{true};
    bool gyro_on{}; ///< its gyro sends readings (Gyro pointer is not Off)
};

/// Which ring the pad drives.
enum class PadRingKind : uint8_t {
    none,  ///< no ring is open
    order, ///< the order ring: the touch layer's radial
    build, ///< the build ring, or with no builder the standing-orders ring
};

/// The ring the pad drives and how it is held.
struct PadRing {
    PadRingKind kind{PadRingKind::none}; ///< which ring
    /// The physical button that opened it; none for a ring a finger opened.
    oa::ui::pad_controls::PadButton owner{oa::ui::pad_controls::PadButton::none};
    /// The physical button that holds it open now; none once let go.
    oa::ui::pad_controls::PadButton button{oa::ui::pad_controls::PadButton::none};
    bool tapped{};      ///< let go before the hold delay with nothing aimed: it stays open
    bool aimed{};       ///< a wedge was aimed at since it opened or was held again
    bool by_pad{};      ///< the pad opened it (its hint line shows)
    uint64_t held_ms{}; ///< when its button went down, milliseconds on the pad's clock
    float open_x{};     ///< the pointer where it opened, canvas pixels
    float open_y{};     ///< the pointer where it opened, canvas pixels
    oa::ui::pad_controls::WedgeAim aim{}; ///< the wedge aimed at
    oa::ui::pad_controls::Vec2 offset{};  ///< the last aim, -1..1 each way, y down
    uint16_t unit{};                      ///< the selected unit the build ring was made for
    bool factory{};                       ///< the build ring's builder queues units
    /// Since when the aim rests on the standing-orders ring's SELF-DESTRUCT, milliseconds.
    std::optional<uint64_t> self_destruct_ms{};
    bool self_destruct_fired{}; ///< that hold gave its key
};

/// A held direction that repeats: the D-pad's focus steps, the left stick's in menus, the
/// right stick's wheel and the stick cursor's steps over the side panel.
struct PadRepeat {
    oa::ui::pad_controls::MenuRepeat repeat{}; ///< the steps' timing
    /// The D-pad arm that holds it; none for a stick.
    oa::ui::pad_controls::PadButton button{oa::ui::pad_controls::PadButton::none};
    oa::ui::pad_controls::Action action{oa::ui::pad_controls::Action::none}; ///< each step's
    int8_t way_x{}; ///< the stick's direction across: -1, 0 or 1
    int8_t way_y{}; ///< the stick's direction down: -1, 0 or 1
};

/// The gamepad dispatcher's state for one runtime (made on first use, see Runtime::pad_state).
struct Runtime::PadState {
    std::array<OpenPad, max_open_pads> pads{}; ///< the open gamepads
    SDL_JoystickID active{};                   ///< the pad that last sent input; 0 before any
    bool used{};                               ///< a gamepad sent input in this run
    bool forced{};                             ///< the pad check turned the layer on
    bool virtual_pads_only{};                  ///< the pad check opens only its virtual pads
    bool grip_keys_seen{};                     ///< F13–F16 arrived while a pad was open
    bool force{};                              ///< R5 (or its key) is held
    /// The check's clock, nanoseconds; times come from it while set.
    std::optional<uint64_t> check_clock_ns{};
    uint64_t tick_ns{}; ///< when tick_pad last ran, nanoseconds (0 before)

    // ---- Physical inputs, by oa::ui::pad_controls::PadButton ----
    /// The physical buttons down.
    std::array<bool, oa::ui::pad_controls::pad_button_count> down{};
    /// What each held button's press meant, which its release ends.
    std::array<oa::ui::pad_controls::Binding, oa::ui::pad_controls::pad_button_count> pressed{};
    /// What a tap of each held button gives (a button with a tap and a hold).
    std::array<oa::ui::pad_controls::Binding, oa::ui::pad_controls::pad_button_count> taps{};
    /// The hold timers of buttons with a tap and a hold, of groups and of self-destruct.
    std::array<oa::ui::pad_controls::HoldTimer, oa::ui::pad_controls::pad_button_count> holds{};
    /// The latch each held grip pressed, which its release lets go.
    std::array<std::optional<oa::ui::touch_hud::Latch>, oa::ui::pad_controls::pad_button_count>
        latches{};
    /// The press's action waits for the release (FOLLOW while R3 may jump the pointer).
    std::array<bool, oa::ui::pad_controls::pad_button_count> deferred{};
    oa::ui::pad_controls::TriggerButton left_trigger{};  ///< L2 read as a button
    oa::ui::pad_controls::TriggerButton right_trigger{}; ///< R2 read as a button
    /// The sticks by physical place (Side), -1..1 each way, y down.
    std::array<oa::ui::pad_controls::Vec2, 2> sticks{};

    // ---- The pointer ----
    float pointer_x{};    ///< the pad pointer, canvas pixels
    float pointer_y{};    ///< the pad pointer, canvas pixels
    bool pointer_known{}; ///< the pad pointer has a place on this screen
    /// The pointer pad (the right one; the left when left-handed) as a mouse.
    oa::ui::pad_controls::PadPointer pointer{};
    /// The camera pad (the left one; the right when left-handed): the map dragged with glide.
    oa::ui::pad_controls::PadPointer drag{};
    oa::ui::pad_controls::StickCursor stick_cursor{}; ///< the stick as a pointer
    oa::ui::pad_controls::GyroPointer gyro_pointer{}; ///< the gyro as fine pointer motion
    oa::ui::pad_controls::FlickDetector flick{};      ///< the pointer stick's flicks
    oa::ui::pad_controls::DoubleClick double_click{}; ///< the left button's double clicks
    /// The settings the pointers were last configured with.
    oa::ui::pad_controls::PadSettings configured{};
    oa::ui::pad_controls::Vec2 configured_canvas{}; ///< the canvas they were configured for
    oa::ui::pad_controls::Area configured_area{};   ///< the absolute area they were given
    float configured_px_per_point{};                ///< the stick cursor's scale then
    bool configured_once{};                         ///< configure ran
    uint8_t left_holders{};     ///< pad buttons holding the synthetic left button
    uint8_t right_holders{};    ///< pad buttons holding the synthetic right button
    uint8_t left_clicks{1};     ///< the clicks the held left button's press counted
    bool left_on_build{};       ///< the left press was on a build button
    bool right_on_build{};      ///< the right press was on a build button
    bool left_on_battlefield{}; ///< the left press was a battlefield click
    /// The latch class the left press's click uses (selection or order).
    oa::ui::touch_hud::ActionClass left_class{oa::ui::touch_hud::ActionClass::selection};
    bool box_felt{};       ///< the held left button's box gave its feel
    bool minimap{};        ///< the camera pad is pressed: the minimap under the thumb
    uint64_t moved_ns{};   ///< when the pad last moved the pointer (0 before)
    uint8_t cursor_seen{}; ///< the cursor shown when the pad last looked
    uint16_t unit_seen{};  ///< the unit under the pointer when the pad last looked
    bool seen_once{};      ///< cursor_seen and unit_seen hold a look
    bool jumped{};         ///< R3 jumped the pointer: no FOLLOW at its release
    /// Visible units' screen points, canvas pixels (refreshed each frame on the match).
    std::vector<oa::ui::pad_controls::Vec2> targets{};

    // ---- Layers, rings and menus ----
    PadRing ring{};                             ///< the ring the pad drives
    oa::ui::pad_controls::WedgeAim group_aim{}; ///< the left pad's group ring aim
    std::optional<uint8_t> group_aimed{};       ///< the group aimed at, 1..9
    int8_t sheet_focus{-1};                     ///< the SELECT ▾ item the D-pad marks
    PadRepeat dpad{};                           ///< the D-pad's held focus steps
    PadRepeat stick_steps{};                    ///< the left stick's arrows in menus
    PadRepeat wheel{};                          ///< the right stick's wheel in menus
    PadRepeat panel{};                          ///< the stick cursor's steps over the panel
    bool force_seen{};                          ///< pad_force_held() at the last look
    bool* running{}; ///< the running flag of the event being taken, or null
};

/// The dispatcher's helpers that reach the runtime's private members: static functions that
/// take Runtime& (runtime_pad.cpp, runtime_pad_actions.cpp, runtime_pad_pointer.cpp,
/// runtime_pad_haptics.cpp). The pad check uses none of them.
struct PadAccess {
    // ---- routing and state (runtime_pad.cpp) --------------------------------------------

    /// Returns the dispatcher's clock: the check's while set, else SDL's.
    ///
    /// @param runtime the runtime
    /// @return nanoseconds
    [[nodiscard]] static uint64_t now_ns(const Runtime& runtime);
    /// Returns the open pad that last sent input, else the first open pad.
    ///
    /// @param runtime the runtime
    /// @return the pad, or null when none is open
    [[nodiscard]] static const OpenPad* active_pad(const Runtime& runtime);
    /// Returns what the pad in use has; nothing known without one.
    ///
    /// @param runtime the runtime
    /// @return its traits
    [[nodiscard]] static oa::ui::pad_controls::PadTraits traits(const Runtime& runtime);
    /// Returns which map the buttons follow now: the effective scheme, the fallback without
    /// grips, the trackpads and the mirror.
    ///
    /// @param runtime the runtime
    /// @return the map
    [[nodiscard]] static oa::ui::pad_controls::MapContext map_context(const Runtime& runtime);
    /// Returns whether the match takes the pad's buttons: it shows with no menu, dialog or end
    /// over it.
    ///
    /// @param runtime the runtime
    /// @return whether the buttons act on the match
    [[nodiscard]] static bool in_match(Runtime& runtime);
    /// Returns what is held or open, which decides the layer.
    ///
    /// @param runtime the runtime
    /// @return the layer keys held and the overlays open
    [[nodiscard]] static oa::ui::pad_controls::Held held(Runtime& runtime);
    /// Returns the layer the buttons act in now.
    ///
    /// @param runtime the runtime
    /// @return the layer
    [[nodiscard]] static oa::ui::pad_controls::Layer layer(Runtime& runtime);
    /// Returns whether a gamepad is open.
    ///
    /// @param runtime the runtime
    /// @return whether any pad is open
    [[nodiscard]] static bool any_pad_open(const Runtime& runtime);
    /// Opens a gamepad SDL added and reads what it has.
    ///
    /// @param runtime the runtime
    /// @param id the joystick SDL names it by
    static void open_pad(Runtime& runtime, SDL_JoystickID id);
    /// Opens the gamepads SDL has that are not open yet, while none is: a pad whose arrival an
    /// earlier screen's loop read (the folder chooser, a loading pump) is opened all the same.
    ///
    /// @param runtime the runtime
    static void open_present_pads(Runtime& runtime);
    /// Closes a gamepad SDL removed, letting go of what it held.
    ///
    /// @param runtime the runtime
    /// @param id the joystick SDL names it by
    static void close_pad(Runtime& runtime, SDL_JoystickID id);
    /// Reads what an open pad has: its type, trackpads, grips, gyro and Steam handle.
    ///
    /// @param[in,out] pad the open pad
    static void read_traits(OpenPad& pad);
    /// Notes input from a pad: the pad layer is on and that pad is the one in use.
    ///
    /// @param runtime the runtime
    /// @param id the pad's joystick, or 0 for the grips' keys
    static void note_input(Runtime& runtime, SDL_JoystickID id);
    /// Takes a physical button going down or up.
    ///
    /// @param runtime the runtime
    /// @param button the physical button
    /// @param down whether it went down
    static void take_button(Runtime& runtime, oa::ui::pad_controls::PadButton button, bool down);
    /// Turns each open pad's gyro on while Gyro pointer is not Off, and off while it is.
    ///
    /// @param runtime the runtime
    /// @param settings the Controller section's settings
    static void sync_sensors(Runtime& runtime, const oa::ui::pad_controls::PadSettings& settings);
    /// Lets go of every held button, ring, repeat and glide without acting.
    ///
    /// @param runtime the runtime
    /// @param release_engine also lets go of the engine's left button and drag (a pad that
    ///     went away), else leaves the engine as it is (the screen changed)
    static void forget_held(Runtime& runtime, bool release_engine);
    /// Writes the pad's looks into the touch HUD state (HudState::pad, build_ring, sheet_focus).
    ///
    /// @param runtime the runtime
    /// @param settings the Controller section's settings
    static void write_look(Runtime& runtime, const oa::ui::pad_controls::PadSettings& settings);
    /// Returns whether the pad holds an input that needs the full frame rate: a button down, a
    /// stick out of its dead zone, a thumb on a trackpad, a glide or a repeat.
    ///
    /// @param runtime the runtime
    /// @return whether the pad holds input
    [[nodiscard]] static bool input_held(const Runtime& runtime);
    /// Returns the physical button that plays a role: the button itself, or its mirror when
    /// left-handed.
    ///
    /// @param runtime the runtime
    /// @param role the button of the right-handed role
    /// @return the physical button
    [[nodiscard]] static oa::ui::pad_controls::PadButton
    physical(const Runtime& runtime, oa::ui::pad_controls::PadButton role);
    /// Returns the physical side that plays a role's side (sticks and trackpads).
    ///
    /// @param runtime the runtime
    /// @param role the right-handed role's side
    /// @return the physical side
    [[nodiscard]] static oa::ui::pad_controls::Side
    physical_side(const Runtime& runtime, oa::ui::pad_controls::Side role);
    /// Returns whether any held button's press meant an action.
    ///
    /// @param runtime the runtime
    /// @param action the action
    /// @return whether a held button gives it
    [[nodiscard]] static bool
    action_held(const Runtime& runtime, oa::ui::pad_controls::Action action);
    /// Returns the hold delay, the Touch section's, in milliseconds.
    ///
    /// @param runtime the runtime
    /// @return milliseconds
    [[nodiscard]] static uint32_t hold_ms(const Runtime& runtime);
    /// Returns the touch layer's latch mode from the Touch settings.
    ///
    /// @param runtime the runtime
    /// @return stay on, or one action
    [[nodiscard]] static oa::ui::touch_hud::LatchMode latch_mode(Runtime& runtime);

    // ---- actions (runtime_pad_actions.cpp) -----------------------------------------------

    /// Acts on a physical button going down: the layer's binding, the hold timer of a button
    /// with a tap and a hold, a ring held again.
    ///
    /// @param runtime the runtime
    /// @param button the physical button
    /// @param now nanoseconds on the pad's clock
    static void press(Runtime& runtime, oa::ui::pad_controls::PadButton button, uint64_t now);
    /// Acts on a physical button coming up: the end of what its press meant, or its tap.
    ///
    /// @param runtime the runtime
    /// @param button the physical button
    /// @param now nanoseconds on the pad's clock
    static void release(Runtime& runtime, oa::ui::pad_controls::PadButton button, uint64_t now);
    /// Starts what a binding means when its button goes down (or its hold starts).
    ///
    /// @param runtime the runtime
    /// @param button the physical button
    /// @param binding what it means
    /// @param now nanoseconds on the pad's clock
    static void act_press(
        Runtime& runtime,
        oa::ui::pad_controls::PadButton button,
        oa::ui::pad_controls::Binding binding,
        uint64_t now
    );
    /// Ends what a binding meant when its button comes up.
    ///
    /// @param runtime the runtime
    /// @param button the physical button
    /// @param binding what its press meant
    /// @param now nanoseconds on the pad's clock
    static void act_release(
        Runtime& runtime,
        oa::ui::pad_controls::PadButton button,
        oa::ui::pad_controls::Binding binding,
        uint64_t now
    );
    /// Gives a tap's action.
    ///
    /// @param runtime the runtime
    /// @param button the physical button
    /// @param binding the tap's binding
    /// @param now nanoseconds on the pad's clock
    static void act_tap(
        Runtime& runtime,
        oa::ui::pad_controls::PadButton button,
        oa::ui::pad_controls::Binding binding,
        uint64_t now
    );
    /// Runs the hold timers and the held repeats: holds that start, D-pad steps.
    ///
    /// @param runtime the runtime
    /// @param now nanoseconds on the pad's clock
    static void advance_holds(Runtime& runtime, uint64_t now);
    /// Presses a latch for a grip: x5 instead of QUEUE when the pointer is on a build button.
    ///
    /// @param runtime the runtime
    /// @param button the physical grip
    /// @param latch the latch its binding names
    /// @param now nanoseconds on the pad's clock
    static void press_latch(
        Runtime& runtime,
        oa::ui::pad_controls::PadButton button,
        oa::ui::touch_hud::Latch latch,
        uint64_t now
    );
    /// Lets go of the latch a grip pressed.
    ///
    /// @param runtime the runtime
    /// @param button the physical grip
    /// @param now nanoseconds on the pad's clock
    static void
    release_latch(Runtime& runtime, oa::ui::pad_controls::PadButton button, uint64_t now);
    /// Turns a latch on or off at once, as a tap of its chip does (the fallback's taps).
    ///
    /// @param runtime the runtime
    /// @param latch the latch the binding names
    /// @param now nanoseconds on the pad's clock
    static void toggle_latch(Runtime& runtime, oa::ui::touch_hud::Latch latch, uint64_t now);
    /// Marks the latch of an action's class used, as the touch actions do.
    ///
    /// @param runtime the runtime
    /// @param action the class of the action that was given
    static void use_latch(Runtime& runtime, oa::ui::touch_hud::ActionClass action);
    /// Returns whether R4 (QUEUE's grip) is held down, by button or key.
    ///
    /// @param runtime the runtime
    /// @return whether a held grip presses QUEUE or x5
    [[nodiscard]] static bool queue_grip_held(const Runtime& runtime);
    /// Sends a key's press and release through dispatch_event, as the keyboard would.
    ///
    /// @param runtime the runtime
    /// @param key the key
    /// @param mods its modifiers
    static void send_key(Runtime& runtime, SDL_Keycode key, SDL_Keymod mods = SDL_KMOD_NONE);
    /// Gives a group button's tap: selects the group (again: centres on it), ADD adding.
    ///
    /// @param runtime the runtime
    /// @param group the group, 1..9
    static void select_group(Runtime& runtime, uint8_t group);
    /// Gives a group button's hold: stores the selection in the group.
    ///
    /// @param runtime the runtime
    /// @param group the group, 1..9
    static void store_group(Runtime& runtime, uint8_t group);
    /// Opens the order ring at the pointer.
    ///
    /// @param runtime the runtime
    /// @param button the physical button that opens it
    /// @param now nanoseconds on the pad's clock
    static void
    open_order_ring(Runtime& runtime, oa::ui::pad_controls::PadButton button, uint64_t now);
    /// Opens the build ring at the pointer: the builder's page, or the standing-orders ring.
    ///
    /// @param runtime the runtime
    /// @param button the physical button that opens it
    /// @param now nanoseconds on the pad's clock
    static void
    open_build_ring(Runtime& runtime, oa::ui::pad_controls::PadButton button, uint64_t now);
    /// Keeps the ring the pad drives in step with the touch layer: takes up a radial a finger
    /// opened, forgets a ring that closed, and lays the open build ring out again from the
    /// selection (counts, page), closing it when its builder is no longer selected.
    ///
    /// @param runtime the runtime
    static void refresh_rings(Runtime& runtime);
    /// Returns what the build ring holds for the selection.
    ///
    /// @param runtime the runtime
    /// @param[out] factory whether the builder queues units rather than placing buildings
    /// @param open_page whether to show the builder's build page when the panel shows another
    /// @return the content; none with nothing selected
    [[nodiscard]] static std::optional<oa::ui::touch_hud::BuildRingContent>
    build_ring_content(Runtime& runtime, bool& factory, bool open_page);
    /// Aims the open ring from the pointer pad's thumb, the pointer stick past half, or (with
    /// no trackpads) the pointer's travel since the ring opened.
    ///
    /// @param runtime the runtime
    /// @param now nanoseconds on the pad's clock
    static void aim_ring(Runtime& runtime, uint64_t now);
    /// Lets go of the ring's button: gives the aimed wedge, keeps a tapped ring open, or closes.
    ///
    /// @param runtime the runtime
    /// @param button the physical button let go
    /// @param now nanoseconds on the pad's clock
    static void
    release_ring(Runtime& runtime, oa::ui::pad_controls::PadButton button, uint64_t now);
    /// Gives the ring's aimed wedge: the order at the ring's point, or the build ring's press.
    ///
    /// @param runtime the runtime
    /// @param arm whether to arm the order rather than give it (A)
    /// @param from_release whether the ring's own button was let go
    static void give_ring(Runtime& runtime, bool arm, bool from_release);
    /// Takes one off the aimed factory wedge's queue (five with QUEUE).
    ///
    /// @param runtime the runtime
    static void reduce_ring(Runtime& runtime);
    /// Closes the ring the pad drives.
    ///
    /// @param runtime the runtime
    static void close_ring(Runtime& runtime);
    /// Moves the SELECT ▾ mark, picks the marked item or closes the menu.
    ///
    /// @param runtime the runtime
    /// @param action sheet_up, sheet_down, sheet_pick or sheet_close
    static void sheet_action(Runtime& runtime, oa::ui::pad_controls::Action action);
    /// Gives one step of a held menu direction or wheel.
    ///
    /// @param runtime the runtime
    /// @param action the step's action
    static void menu_step(Runtime& runtime, oa::ui::pad_controls::Action action);
    /// Returns whether text is being typed: the chat line, a marker's text or a text field.
    ///
    /// @param runtime the runtime
    /// @return whether text input is on
    [[nodiscard]] static bool text_input_on(const Runtime& runtime);
    /// Notes a placement the pad's action started, so the ghost follows the pointer.
    ///
    /// @param runtime the runtime
    /// @param placing_before whether a building was being placed before the action
    static void note_placement(Runtime& runtime, bool placing_before);
    /// Returns whether a building waits to be placed.
    ///
    /// @param runtime the runtime
    /// @return whether placement is armed
    [[nodiscard]] static bool placing(const Runtime& runtime);

    // ---- pointer and camera (runtime_pad_pointer.cpp) -----------------------------------

    /// Returns the canvas the current screen is drawn on.
    ///
    /// @param runtime the runtime
    /// @return its size, canvas pixels
    [[nodiscard]] static oa::ui::pad_controls::Vec2 canvas(const Runtime& runtime);
    /// Gives the pad pointer a place on this screen: the engine's pointer when another device
    /// moved it, else where it was, held inside the canvas.
    ///
    /// @param runtime the runtime
    static void sync_pointer(Runtime& runtime);
    /// Configures the pointers when the settings or the canvas changed.
    ///
    /// @param runtime the runtime
    /// @param settings the Controller section's settings
    static void
    configure_pointers(Runtime& runtime, const oa::ui::pad_controls::PadSettings& settings);
    /// Sends a synthetic event through dispatch_event with the run's flag.
    ///
    /// @param runtime the runtime
    /// @param[in,out] event the event
    static void dispatch(Runtime& runtime, SDL_Event& event);
    /// Sends a synthetic mouse event of the pad's mouse at the pad pointer.
    ///
    /// @param runtime the runtime
    /// @param type SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN or SDL_EVENT_MOUSE_BUTTON_UP
    /// @param button SDL_BUTTON_LEFT or SDL_BUTTON_RIGHT for a button event
    /// @param clicks the clicks a button event counts
    static void send_mouse(Runtime& runtime, uint32_t type, uint8_t button, uint8_t clicks);
    /// Moves the pad pointer to a place, sending the motion when it moved.
    ///
    /// @param runtime the runtime
    /// @param x canvas pixels
    /// @param y canvas pixels
    static void place_pointer(Runtime& runtime, float x, float y);
    /// Applies a pointer step: its place or its move, and its ticks as pointer feels.
    ///
    /// @param runtime the runtime
    /// @param step the step
    /// @param ticks whether its ticks play pointer ticks
    static void
    apply_step(Runtime& runtime, const oa::ui::pad_controls::PointerStep& step, bool ticks);
    /// Presses or lets go of the synthetic left or right button for one pad button.
    ///
    /// @param runtime the runtime
    /// @param left the left button, else the right
    /// @param down whether it goes down
    /// @param now nanoseconds on the pad's clock
    static void pointer_button(Runtime& runtime, bool left, bool down, uint64_t now);
    /// Returns whether the pointer is on a build button of the match HUD.
    ///
    /// @param runtime the runtime
    /// @return whether a unit or weapon build button is under it
    [[nodiscard]] static bool pointer_on_build_button(const Runtime& runtime);
    /// Takes a trackpad's thumb landing, moving or lifting.
    ///
    /// @param runtime the runtime
    /// @param event a touchpad event
    static void take_touchpad(Runtime& runtime, const SDL_GamepadTouchpadEvent& event);
    /// Takes a gyro reading: fine pointer motion while the setting's touch is held.
    ///
    /// @param runtime the runtime
    /// @param event a sensor event
    static void take_sensor(Runtime& runtime, const SDL_GamepadSensorEvent& event);
    /// Runs the pointer's and the camera's frame: glides, sticks, the stick cursor, held zoom,
    /// the edge push and the target detent.
    ///
    /// @param runtime the runtime
    /// @param settings the Controller section's settings
    /// @param now nanoseconds on the pad's clock
    /// @param elapsed_ns time since the last frame, nanoseconds
    static void tick_pointer(
        Runtime& runtime,
        const oa::ui::pad_controls::PadSettings& settings,
        uint64_t now,
        uint64_t elapsed_ns
    );
    /// Starts or ends the minimap under the camera pad's thumb.
    ///
    /// @param runtime the runtime
    /// @param on whether the camera pad went down
    static void minimap(Runtime& runtime, bool on);
    /// Refreshes the visible units' screen points the stick cursor and the jump use.
    ///
    /// @param runtime the runtime
    static void refresh_targets(Runtime& runtime);
    /// Returns whether the pointer stick moves the pointer: the sticks scheme, or Right stick
    /// set to Pointer.
    ///
    /// @param runtime the runtime
    /// @param settings the Controller section's settings
    /// @return whether the pointer stick is a pointer
    [[nodiscard]] static bool
    stick_is_pointer(const Runtime& runtime, const oa::ui::pad_controls::PadSettings& settings);
    /// Returns a stick by its role: the left one pans, the right one points or zooms.
    ///
    /// @param runtime the runtime
    /// @param role the right-handed role's side
    /// @return the stick, -1..1 each way, y down
    [[nodiscard]] static oa::ui::pad_controls::Vec2
    stick(const Runtime& runtime, oa::ui::pad_controls::Side role);
    /// Returns the viewport the touch layer lays its rings out on.
    ///
    /// @param runtime the runtime
    /// @return the viewport, filled from the match layout when the touch layer has none yet
    [[nodiscard]] static oa::ui::touch_hud::Viewport ring_viewport(Runtime& runtime);
};

/// Starts SDL's gamepad subsystem after SDL_Init; a failure is written to the error output and
/// the game goes on without gamepads. Calling it again does nothing more.
void start_gamepad_subsystem() noexcept;

} // namespace oa::app
