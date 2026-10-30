// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hold latches and sticky modifiers. The caller passes the time; this
// module does not read a clock and does not link SDL.
#pragma once

#include "oa/ui/key_bindings/layout.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

struct Resolved;

/// What a hold to confirm is doing.
enum class HoldState {
    none,    ///< no hold is running
    waiting, ///< the press is still short of the long-press time
    done,    ///< the long-press time has passed and the press is still held
};

/// A press that must be held before a dangerous action runs.
///
/// The caller passes the time. The hold does not read a clock. The action
/// fires when `tick` first reports `done`, while the press is still held.
/// `release` before that, or `cancel`, leaves it unfired.
class HoldConfirm {
  public:

    /// Where the ring is drawn, in match canvas pixels.
    struct Target {
        bool button{false}; ///< a button's rectangle; otherwise the pointer's point
        int32_t x{0};       ///< the point, or the rectangle's left
        int32_t y{0};       ///< the point, or the rectangle's top
        int32_t width{0};   ///< the rectangle's width; 0 for a point
        int32_t height{0};  ///< the rectangle's height; 0 for a point
    };

    /// A press began. A hold already running is replaced.
    ///
    /// @param id the action id
    /// @param now_ms the time, in milliseconds
    /// @param target where the ring is drawn
    void start(std::string_view id, uint64_t now_ms, Target target);

    /// The press ended before the hold finished. The next tick reports none.
    void release();

    /// Advances the hold.
    ///
    /// @param now_ms the time, in milliseconds
    /// @param long_press_ms the long-press time, in milliseconds
    /// @return none when no hold is running or the press already ended, waiting
    ///     while the press is short of the time, or done once the time has passed
    [[nodiscard]] HoldState tick(uint64_t now_ms, uint32_t long_press_ms);

    /// Ends the hold. It does not fire.
    void cancel();

    /// Reports whether a press is still held for confirmation.
    ///
    /// @return true from start until release or cancel
    [[nodiscard]] bool active() const;

    /// How far the hold has filled, in thousandths of the long-press time.
    ///
    /// @param now_ms the time, in milliseconds
    /// @param long_press_ms the long-press time, in milliseconds
    /// @return 0 when no hold is running, else 0 to 1000
    [[nodiscard]] int32_t progress(uint64_t now_ms, uint32_t long_press_ms) const;

    /// The action id of the hold that is running.
    ///
    /// @return the id; empty when none is running
    [[nodiscard]] const std::string& id() const;

    /// Where the ring is drawn.
    ///
    /// @return the target passed to start
    [[nodiscard]] const Target& target() const;

  private:

    /// Drops the hold.
    void clear();

    std::string id_{};      ///< the action id
    Target target_{};       ///< where the ring is drawn
    uint64_t started_ms_{}; ///< when the press began
    bool started_{};        ///< a hold is running
    bool released_{};       ///< the press ended before the next tick
};

/// Reports whether an action needs a hold before it runs.
///
/// The setting off never needs one. Otherwise the action must be confirmable,
/// and the resolved layout must either name no hold-to-confirm list or name
/// the action in that list. A list that is present and empty names nothing.
/// `gui.yesorno.choice1` is confirmable: it is the Yes of the surrender question.
///
/// @param resolved the bindings in use
/// @param action the action id
/// @param setting_on whether Hold to confirm is on
/// @return true when the press must be held
[[nodiscard]] bool needs_hold(const Resolved& resolved, std::string_view action, bool setting_on);

/// QUEUE, ADD, the status strip and the other latchable holds, one per action id.
///
/// A press is active at once. A release toggles the latch only when the press
/// was shorter than the long-press time and no action used it while it was
/// held. `shown` keeps the latched ids in the order they were turned on.
class Latches {
  public:

    /// A press began at now_ms: the action is active at once.
    ///
    /// @param id the action id
    /// @param now_ms the time, in milliseconds
    void press(std::string_view id, uint64_t now_ms);

    /// The press ended. A press shorter than long_press_ms that no action used
    /// toggles the latch. A longer press, or one an action used, only ends the hold.
    ///
    /// @param id the action id
    /// @param now_ms the time, in milliseconds
    /// @param long_press_ms the long-press time, in milliseconds
    void release(std::string_view id, uint64_t now_ms, uint32_t long_press_ms);

    /// The press was cancelled: the hold ends and the latch is not toggled.
    ///
    /// @param id the action id
    void cancel(std::string_view id);

    /// An action used this hold. A held press will not toggle when it ends.
    /// A latch that is already on stays on.
    ///
    /// @param id the action id
    void used(std::string_view id);

    /// Turns every latch off and ends every hold.
    void clear();

    /// Reports whether the action is latched or held.
    ///
    /// @param id the action id
    /// @return true while it is latched or held
    [[nodiscard]] bool active(std::string_view id) const;

    /// Reports whether the action is latched.
    ///
    /// @param id the action id
    /// @return true while a tap has turned it on
    [[nodiscard]] bool latched(std::string_view id) const;

    /// Reports whether a press is held now.
    ///
    /// @param id the action id
    /// @return true while the press has not been released
    [[nodiscard]] bool held(std::string_view id) const;

    /// The latched action ids, in the order they were turned on.
    ///
    /// Turning one off removes it. Turning it on again puts it at the end.
    /// The list stays valid until the next call that changes a latch.
    ///
    /// @return the ids
    [[nodiscard]] const std::vector<std::string>& shown() const;

  private:

    /// One action's latch.
    struct Entry {
        std::string id{};       ///< the action id
        bool latched{};         ///< on after a tap
        bool held{};            ///< a press is down
        bool used_while_held{}; ///< an action used it during this press
        uint64_t pressed_ms{};  ///< when this press began, in milliseconds
    };

    /// The entry for an id, or null.
    ///
    /// @param id the action id
    /// @return the entry
    [[nodiscard]] Entry* find(std::string_view id);

    /// The entry for an id, or null.
    ///
    /// @param id the action id
    /// @return the entry
    [[nodiscard]] const Entry* find(std::string_view id) const;

    /// The entry for an id, made when it is new.
    ///
    /// @param id the action id
    /// @return the entry
    Entry& ensure(std::string_view id);

    std::vector<Entry> entries_{};     ///< one per action that has been pressed
    std::vector<std::string> shown_{}; ///< latched ids, in the order turned on
};

/// Shift, Ctrl, Alt and Meta tapped so the next chord or click carries them.
///
/// A first tap arms a modifier. A second tap within the double-press time
/// locks it. A tap while it is armed and the window has passed, or while it
/// is locked, lets it go. `consume` lets an armed modifier go and keeps a
/// locked one. The bits are K04's `Modifier` values.
class StickyModifiers {
  public:

    /// A modifier was pressed and released with no other key or button between.
    ///
    /// @param modifier one modifier bit
    /// @param now_ms the time of this tap, in milliseconds
    /// @param double_press_ms how close a second tap must be to lock
    void tap(uint8_t modifier, uint64_t now_ms, uint32_t double_press_ms);

    /// A chord or a click used the armed modifiers. Locked modifiers stay.
    void consume();

    /// The armed and locked modifiers, as bits.
    ///
    /// @return the bits
    [[nodiscard]] uint8_t mods() const;

    /// The modifiers that apply to the next chord or click only.
    ///
    /// @return the bits
    [[nodiscard]] uint8_t armed() const;

    /// The modifiers that stay until tapped again.
    ///
    /// @return the bits
    [[nodiscard]] uint8_t locked() const;

    /// Lets every sticky modifier go.
    void clear();

    /// The armed and locked modifier bits, in the order they were turned on.
    ///
    /// Locking one does not move it. The list stays valid until the next call
    /// that changes a modifier.
    ///
    /// @return the bits
    [[nodiscard]] const std::vector<uint8_t>& shown() const;

  private:

    /// One modifier bit's sticky state.
    struct Arm {
        bool on{};        ///< armed or locked
        bool locked{};    ///< stays until tapped again
        uint64_t at_ms{}; ///< when the tap that armed it happened
    };

    /// Drops one bit from the sticky set.
    ///
    /// @param bit the modifier bit
    void let_go(uint8_t bit);

    std::array<Arm, 8> arms_{};    ///< indexed by the bit's trailing zeros
    std::vector<uint8_t> shown_{}; ///< bits on, in the order turned on
};

/// How fast the pointer starts, in points of the 640×480 game a second.
inline constexpr int pointer_keys_start_speed = 60;

/// How fast the pointer moves once a direction has been held for
/// `pointer_keys_ramp_ms`, in points of the 640×480 game a second.
inline constexpr int pointer_keys_top_speed = 480;

/// How long a direction is held before the pointer reaches
/// `pointer_keys_top_speed`, in milliseconds.
inline constexpr int pointer_keys_ramp_ms = 1000;

/// The left button. A click, a double click and a held button use it.
inline constexpr uint8_t pointer_keys_left = 1;

/// The right button.
inline constexpr uint8_t pointer_keys_right = 3;

/// Movement and button edges for one step of the pointer keys.
///
/// `dx` and `dy` are points of the 640×480 game since the previous step.
/// Down is positive. At most one button goes down and one comes up.
struct PointerStep {
    float dx{};                    ///< points to the right
    float dy{};                    ///< points downward
    std::optional<uint8_t> down{}; ///< a button going down, or none
    std::optional<uint8_t> up{};   ///< a button coming up, or none
    uint8_t clicks{};              ///< 1, or 2 when the down is a double click
};

/// The numpad pointer: eight directions that gather speed, clicks, and a held button.
///
/// The caller passes the time. A diagonal moves at the same speed along its
/// line as a cardinal direction. The left button stays down while Hold left
/// is on, and a click shares that button. Let go, and `clear`, release whatever
/// is held. The caller sends the edges; this class does not move a cursor.
class PointerKeys {
  public:

    /// How close a second click must be to count as a double click.
    ///
    /// Zero means a second click is never a double click. The game passes the
    /// player's double-press time, using 500 ms when that preference asks for
    /// the system's own time.
    ///
    /// @param double_press_ms the window, in milliseconds
    void set_double_press_ms(uint32_t double_press_ms);

    /// A pointer-key action went down.
    ///
    /// @param id the action id
    /// @param now_ms the time, in milliseconds
    void press(std::string_view id, uint64_t now_ms);

    /// A pointer-key action came up. Hold left ignores the key coming up.
    ///
    /// @param id the action id
    /// @param now_ms the time, in milliseconds
    void release(std::string_view id, uint64_t now_ms);

    /// Movement and at most one button down and one button up since the last step.
    ///
    /// A second button edge waits for the next step. A step at the same time
    /// as the press moves nothing.
    ///
    /// @param now_ms the time, in milliseconds
    /// @return the movement and the button edges
    [[nodiscard]] PointerStep step(uint64_t now_ms);

    /// Drops every direction and releases any button the caller has been sent.
    ///
    /// The button comes up on the next `step`, which adds no movement.
    void clear();

  private:

    /// One direction key.
    struct Direction {
        std::string_view id{}; ///< the action id
        float x{};             ///< right is positive
        float y{};             ///< down is positive
        bool held{};           ///< the key is down
        uint64_t pressed_ms{}; ///< when this hold began
    };

    /// One mouse button shared by a click and Hold left.
    struct Button {
        int holders{};      ///< clicks and the hold that are keeping it down
        bool toggled{};     ///< Hold left is on
        bool sent{};        ///< the caller was sent a down that has not come up
        uint64_t last_ms{}; ///< when it was last clicked
        bool have_last{};   ///< `last_ms` is a click
    };

    /// A button edge waiting for a step.
    struct Edge {
        bool down{};      ///< going down; otherwise coming up
        uint8_t button{}; ///< which button
        uint8_t clicks{}; ///< the down's click count
    };

    /// The direction for an action, or null.
    ///
    /// @param id the action id
    /// @return the direction
    [[nodiscard]] Direction* find_direction(std::string_view id);

    /// Queues a down when this press takes the button from up to down.
    ///
    /// @param button the button
    /// @param which the button number
    /// @param clicks the click count
    void press_button(Button& button, uint8_t which, uint8_t clicks);

    /// Queues an up when this release takes the button from down to up.
    ///
    /// @param button the button
    /// @param which the button number
    void release_button(Button& button, uint8_t which);

    /// Drops edges for one button and queues an up when one was sent.
    ///
    /// @param button the button
    /// @param which the button number
    void release_sent(Button& button, uint8_t which);

    std::array<Direction, 8> directions_{{
        {"pointer.move-up", 0.F, -1.F, false, 0},
        {"pointer.move-down", 0.F, 1.F, false, 0},
        {"pointer.move-left", -1.F, 0.F, false, 0},
        {"pointer.move-right", 1.F, 0.F, false, 0},
        {"pointer.move-up-left", -1.F, -1.F, false, 0},
        {"pointer.move-up-right", 1.F, -1.F, false, 0},
        {"pointer.move-down-left", -1.F, 1.F, false, 0},
        {"pointer.move-down-right", 1.F, 1.F, false, 0},
    }};
    Button left_{};
    Button right_{};
    std::vector<Edge> edges_{};
    uint32_t double_press_ms_{500};
    uint64_t last_step_ms_{};
    bool have_step_{};
};

/// Sets aside chords a pointer-key default takes, while pointer keys are on.
///
/// A chord of the base, the chosen layout or the mod is set aside. The
/// player's own chord stays, and so does every other chord of that action
/// (Faster keeps `=`). While pointer keys are off the bindings are unchanged.
/// Nothing is written back into a layout.
///
/// @param resolved the bindings in use
/// @param on true while pointer keys are on
/// @return the bindings the dispatcher should search
[[nodiscard]] Resolved pointer_keys_bindings(const Resolved& resolved, bool on);

/// What a held mouse button has become.
enum class WheelDecision {
    waiting,    ///< still held, inside the drag threshold and short of the long press
    pass_click, ///< let go inside the drag threshold: a click at the press's point
    pass_drag,  ///< moved past the drag threshold: the press, then that motion
    open_wheel, ///< the long-press time passed while it was still inside the threshold
};

/// One mouse button held back until it is a click, a drag or a long press.
///
/// The caller passes the time and the drag threshold. The class reads no
/// clock and does not link SDL. A distance of the threshold, or inside it,
/// is still a click. A move past the threshold is a drag even when the
/// long-press time has also passed. After a decision the press is no longer
/// held, and the point it went down at stays readable until `cancel` or the
/// next `press`. `cancel` drops the held press.
class WheelButton {
  public:

    /// A point, in the same units as the drag threshold.
    struct Point {
        float x{}; ///< across
        float y{}; ///< down
    };

    /// A press began. A press already held is replaced.
    ///
    /// @param point where it went down
    /// @param now_ms the time, in milliseconds
    void press(Point point, uint64_t now_ms);

    /// The pointer moved while the press is held.
    ///
    /// @param point where it is now
    void motion(Point point);

    /// The button came up.
    ///
    /// @param point where it came up
    void release(Point point);

    /// Decides what the press has become.
    ///
    /// @param now_ms the time, in milliseconds
    /// @param long_press_ms the long-press time, in milliseconds
    /// @param drag_threshold how far the pointer may move and still be a click
    /// @return waiting, pass_click, pass_drag or open_wheel
    [[nodiscard]] WheelDecision tick(uint64_t now_ms, uint32_t long_press_ms, float drag_threshold);

    /// Drops the held press and the point it went down at.
    void cancel();

    /// Where the press went down.
    ///
    /// Readable after a decision, until `cancel` or the next `press`.
    ///
    /// @return the press's point; the origin when none is remembered
    [[nodiscard]] Point press_point() const;

    /// Where the pointer last was for this press.
    ///
    /// For a drag, this is the point that crossed the threshold.
    ///
    /// @return the latest point; the origin when none is remembered
    [[nodiscard]] Point point() const;

  private:

    Point origin_{};        ///< where the press went down
    Point current_{};       ///< where the pointer last was
    uint64_t started_ms_{}; ///< when the press began
    bool down_{};           ///< a press is held and not yet decided
    bool released_{};       ///< the button came up before the next tick
    bool remembered_{};     ///< origin_ is a press
};

} // namespace oa::ui::key_bindings
