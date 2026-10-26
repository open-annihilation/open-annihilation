// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turns one input event into the action events its bindings start. The
// caller passes the time; this module does not read a clock.
#pragma once

#include "oa/ui/key_bindings/helpers.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

/// What an action event says happened.
enum class Phase : uint8_t {
    pressed,      ///< the chord went down, or a wheel notch, or a `:release` chord finished
    released,     ///< a chord that went down has gone up
    repeated,     ///< a repeat action's key is still held
    double_press, ///< the chord's `:double` gesture
    held,         ///< the chord's `:hold` gesture, once the hold time has passed
    active,       ///< a modifier role's chord is down
    inactive,     ///< a modifier role's chord has gone up
};

/// One action a binding started.
///
/// `action` points at the id kept by the dispatcher. It stays valid until the
/// next `set_bindings` or `retarget`.
struct ActionEvent {
    std::string_view action{}; ///< the action id
    Phase phase{};             ///< what happened
    Chord chord{};             ///< the chord that matched
    uint8_t context{};         ///< the context, as an index into `contexts()`, that took the event
};

/// The contexts and conditions the game is in, and whether text is being typed.
struct Contexts {
    uint32_t active{};     ///< bits over `contexts()` that are open
    uint32_t conditions{}; ///< bits over `conditions()` that hold
    bool text_entry{};     ///< a chat line, a marker's text or a save name is being typed
};

/// How long a key must be held, how soon a bounce is ignored, and how double
/// presses, long presses and repeating keys are timed. The caller counts the time.
///
/// A zero `double_press_ms`, `repeat_delay_ms` or `repeat_rate` is kept by
/// `clamped`: the caller decides what zero means. The app uses the system's
/// double-click time and the system's key repeat.
struct Timings {
    uint32_t slow_keys_ms{
        0
    }; ///< a key counts only after it has been held this long; 0 waits nothing
    uint32_t bounce_keys_ms{
        0
    }; ///< a second press of the same key this soon is ignored; 0 ignores nothing
    uint32_t double_press_ms =
        500; ///< two downs of one key at most this far apart; 0 is left for the caller
    uint32_t long_press_ms = 500; ///< how long a `:hold` chord waits
    uint32_t repeat_delay_ms{
        0
    }; ///< a repeating key waits this long before its first repeat; 0 uses the system's
    uint32_t repeat_rate{0}; ///< repeats per second after that delay; 0 uses the system's
};

/// The shortest slow-keys wait, in milliseconds.
inline constexpr uint32_t slow_keys_least = 0;
/// The longest slow-keys wait, in milliseconds.
inline constexpr uint32_t slow_keys_most = 1000;
/// The shortest bounce-keys window, in milliseconds.
inline constexpr uint32_t bounce_keys_least = 0;
/// The longest bounce-keys window, in milliseconds.
inline constexpr uint32_t bounce_keys_most = 1000;
/// The shortest double-press window, in milliseconds. Zero is not in this range.
inline constexpr uint32_t double_press_least = 200;
/// The longest double-press window, in milliseconds.
inline constexpr uint32_t double_press_most = 1000;
/// The shortest long-press wait, in milliseconds.
inline constexpr uint32_t long_press_least = 200;
/// The longest long-press wait, in milliseconds.
inline constexpr uint32_t long_press_most = 1000;
/// The shortest key-repeat delay, in milliseconds. Zero is not in this range.
inline constexpr uint32_t repeat_delay_least = 150;
/// The longest key-repeat delay, in milliseconds.
inline constexpr uint32_t repeat_delay_most = 1000;
/// The slowest key-repeat rate, in repeats per second. Zero is not in this range.
inline constexpr uint32_t repeat_rate_least = 2;
/// The fastest key-repeat rate, in repeats per second.
inline constexpr uint32_t repeat_rate_most = 30;
/// The narrowest drag that still counts as a click, in pixels of the 640×480 game.
inline constexpr uint32_t drag_threshold_least = 32;
/// The widest drag that still counts as a click, in pixels of the 640×480 game.
inline constexpr uint32_t drag_threshold_most = 96;

/// Holds one timing inside its range.
///
/// Zero stays zero when `keep_zero` is set, so a stored "use the system's"
/// value is not rewritten as the shortest allowed time.
///
/// @param value the timing
/// @param least the shortest value, used when `value` is below it
/// @param most the longest value, used when `value` is above it
/// @param keep_zero true when zero means the system's own timing
/// @return `value` held to the range
[[nodiscard]] inline uint32_t
bounded_timing(uint32_t value, uint32_t least, uint32_t most, bool keep_zero) noexcept {
    if (keep_zero && value == 0)
        return 0;
    if (value < least)
        return least;
    if (value > most)
        return most;
    return value;
}

/// Holds every timing inside its range.
///
/// Zero stays zero for the double-press window, the repeat delay and the
/// repeat rate. A long press of zero becomes `long_press_least`.
///
/// @param timings the timings a setting or a caller supplied
/// @return the same timings, each held to its range
[[nodiscard]] inline Timings clamped(Timings timings) noexcept {
    timings.slow_keys_ms =
        bounded_timing(timings.slow_keys_ms, slow_keys_least, slow_keys_most, false);
    timings.bounce_keys_ms =
        bounded_timing(timings.bounce_keys_ms, bounce_keys_least, bounce_keys_most, false);
    timings.double_press_ms =
        bounded_timing(timings.double_press_ms, double_press_least, double_press_most, true);
    timings.long_press_ms =
        bounded_timing(timings.long_press_ms, long_press_least, long_press_most, false);
    timings.repeat_delay_ms =
        bounded_timing(timings.repeat_delay_ms, repeat_delay_least, repeat_delay_most, true);
    timings.repeat_rate =
        bounded_timing(timings.repeat_rate, repeat_rate_least, repeat_rate_most, true);
    return timings;
}

/// The game ticks a drag may last and still be a click, for one pixel threshold.
///
/// The pixel limit is the threshold. The tick limit grows in proportion,
/// `0x19 × threshold / 32`, rounded down, so one setting widens both. At
/// `drag_threshold_least` the result is 0x19, which is 3.1c's.
///
/// @param drag_threshold the pixel limit, in pixels of the 640×480 game
/// @return the tick limit
[[nodiscard]] constexpr uint32_t drag_click_ticks(uint32_t drag_threshold) noexcept {
    return uint32_t{0x19} * drag_threshold / 32U;
}

/// What the app does with the key that `feed` was given, besides the action events.
enum class LegacyGate : uint8_t {
    deliver, ///< the key reaches the rest of the game now
    hold,    ///< the key is held back until `advance` admits it
    drop,    ///< the key is ignored, and so is the rest of the game's reading of it
};

/// What `feed`, `advance` or `release_all` produced.
struct Fed {
    std::vector<ActionEvent> events{};      ///< the action events, in the order they happened
    bool bound{};                           ///< a binding matched the event
    LegacyGate legacy{LegacyGate::deliver}; ///< what the app does with the key that was fed
    std::vector<InputEvent> admitted{};     ///< keys `advance` admitted, each once, at its own time
};

/// Which keys are held, as the app reads them when a modifier role is judged.
///
/// The function is what the app fills from the keys the devices and the
/// automation endpoint are holding. It is not the dispatcher's own memory of
/// key events.
struct HeldKeys {
    const void* context{}; ///< passed to `down`; the dispatcher does not read it
    /// Returns whether that key position is held.
    ///
    /// @param context `HeldKeys::context`
    /// @param scancode the position number
    /// @return true while that key is held
    bool (*down)(const void* context, uint16_t scancode){};
};

/// The dispatcher's held keys, timers and bindings. Defined with the dispatcher.
struct DispatcherState;

/// The held keys and buttons, the open contexts and the double-press and hold
/// timers. One binding is chosen for each event.
class Dispatcher {
  public:

    /// Starts with no bindings and the default timings.
    Dispatcher();

    /// Releases the bindings and the held keys.
    ~Dispatcher();

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    /// Moves the dispatcher.
    Dispatcher(Dispatcher&&) noexcept;

    /// Moves the dispatcher.
    ///
    /// @return this dispatcher
    Dispatcher& operator=(Dispatcher&&) noexcept;

    /// Replaces the bindings, taking ownership of them.
    ///
    /// Held keys, roles and timers are dropped and no action event is made
    /// for them. Call `release_all` first when those events are wanted.
    /// Events returned before this call are no longer valid.
    ///
    /// @param bindings the bindings in use
    void set_bindings(Resolved&& bindings);

    /// The bindings in use.
    ///
    /// The reference stays valid until the next `set_bindings`.
    ///
    /// @return the bindings
    [[nodiscard]] const Resolved& bindings() const;

    /// Replaces the bindings the next event is chosen from, and keeps held keys.
    ///
    /// Roles, timers and keys already down stay. Use this when the action ids
    /// are unchanged and only a context bit moved, so a held page key is not
    /// dropped. Events returned before this call are no longer valid.
    ///
    /// @param bindings the bindings in use
    void retarget(Resolved&& bindings);

    /// Replaces the keyboard map a letter and a place are compared on.
    ///
    /// @param keyboard the map
    void set_keyboard(const KeyboardMap& keyboard);

    /// The keyboard map a letter and a place are compared on.
    ///
    /// @return the map
    [[nodiscard]] const KeyboardMap& keyboard() const;

    /// How many keys and buttons the dispatcher is still remembering.
    ///
    /// A key that has come up is remembered until a later down could no longer
    /// be its double press. A button is forgotten as soon as it comes up.
    /// `release_all` and `set_bindings` forget every one at once.
    ///
    /// @return the number remembered
    [[nodiscard]] std::size_t remembered() const;

    /// Says which key Primary means.
    ///
    /// @param platform the platform
    void set_platform(Platform platform);

    /// Replaces the key timings.
    ///
    /// `clamped` holds each one to its range first. A key already held back
    /// keeps the admit time it was given.
    ///
    /// @param timings the times, in milliseconds
    void set_timings(Timings timings);

    /// Says which contexts and conditions hold for the next `feed`.
    ///
    /// `advance` does not read this again: a hold already armed keeps the
    /// action it was armed with.
    ///
    /// @param contexts the open contexts, the conditions and text entry
    void set_contexts(Contexts contexts);

    /// Turns one event into action events.
    ///
    /// A plain chord gives `pressed` on the down and `released` on that key's
    /// or button's up, for the actions the down started, whatever the
    /// modifiers did in between. `:double` gives `double_press` on the second
    /// down, and the first down still gives its plain chord's `pressed`.
    /// `:release` gives `pressed` on the up. A repeat action gives `repeated`
    /// on a repeat; any other action ignores repeats. A wheel gives `pressed`
    /// for each notch. A modifier role gives `active` or `inactive` as its
    /// own key goes down or up. An up for a key that never went down gives
    /// nothing. A latchable action named in `set_toggle` stays active after a
    /// tap. With sticky modifiers on, a modifier pressed and released alone
    /// fires nothing and applies to the next chord.
    ///
    /// With slow keys, a key down is held back (`LegacyGate::hold`) until
    /// `advance` has seen it held for `slow_keys_ms`; it then fires once, at
    /// that time. A release before then drops the down and the release. With
    /// bounce keys, a down of the same key within `bounce_keys_ms` of its last
    /// accepted release is dropped, and so is that down's release. Mouse
    /// buttons, the wheel and keys fed while text is being typed are never
    /// held back or dropped. When a repeat delay and a repeat rate are both
    /// set, a platform repeat is not turned into `repeated`.
    ///
    /// @param event the key, button or wheel
    /// @return the action events, and whether a binding matched
    [[nodiscard]] Fed feed(const InputEvent& event);

    /// Admits each held-back key whose time has come, fires each `:hold` chord
    /// that has been down for `long_press_ms`, and fires each repeating key
    /// whose own delay and rate have come.
    ///
    /// A held-back key fires once, from here, at the time it was due, and a
    /// later `advance` does not fire it again. A hold fires once. An earlier
    /// `advance` does not fire it, and a later one does not fire it again.
    /// One call emits every repeat that became due since the previous one.
    ///
    /// @param now_ms the time, in milliseconds, from the same clock as the events
    /// @return the admitted keys, the `held` events and the `repeated` events
    [[nodiscard]] Fed advance(uint64_t now_ms);

    /// Releases every held action and deactivates every modifier role.
    ///
    /// Held keys, buttons and timers are cleared. A hold that had not fired
    /// does not fire. Latches and sticky modifiers let go too.
    ///
    /// @param now_ms the time, in milliseconds; it is not read
    /// @return the `released` and `inactive` events
    [[nodiscard]] Fed release_all(uint64_t now_ms);

    /// Reports whether a role's chord is held now.
    ///
    /// A role bound to a modifier alone is active while that modifier is
    /// held, from `modifiers_now` or from `keys_held`. A role bound to a
    /// plain key is active while that key is held in `keys_held` and the
    /// chord's modifiers are in `modifiers_now`. This is not decided from
    /// the key events `feed` saw. A latched action is active while its latch is on.
    ///
    /// @param action the action id
    /// @param modifiers_now the physical modifiers held, never `primary`
    /// @param keys_held which key positions are held
    /// @return true while the role is active
    [[nodiscard]] bool
    role_active(std::string_view action, uint8_t modifiers_now, const HeldKeys& keys_held) const;

    /// Names the latchable actions that latch.
    ///
    /// An id that is not latchable never latches. An id that is not named
    /// behaves as before. Replacing the list turns every latch off. The
    /// sticky-modifier switch is left as it is.
    ///
    /// @param ids the action ids, in any order
    void set_toggle(std::vector<std::string> ids);

    /// Turns sticky modifiers on or off.
    ///
    /// Turning them off lets every one go. Setting the same value again
    /// changes nothing, and does not let go of one that is already armed.
    ///
    /// @param on true to arm taps
    void set_sticky_modifiers(bool on);

    /// The hold latches.
    ///
    /// @return the latches
    [[nodiscard]] const Latches& latches() const;

    /// The sticky modifiers.
    ///
    /// @return the sticky modifiers
    [[nodiscard]] const StickyModifiers& sticky() const;

    /// The double-press and hold times.
    ///
    /// @return the times
    [[nodiscard]] Timings timings() const;

    /// Lets an armed sticky modifier go after a chord or a click. A locked one stays.
    void consume_sticky();

    /// Reports whether a key of the action's chords is down.
    ///
    /// A modifier never stops it. A chord that is only a modifier is skipped.
    /// This is not decided from the key events `feed` saw.
    ///
    /// @param action the action id
    /// @param keys_held which key positions are held
    /// @return true while one of that action's keys is held
    [[nodiscard]] bool chord_key_held(std::string_view action, const HeldKeys& keys_held) const;

  private:

    std::unique_ptr<DispatcherState> state_;
};

} // namespace oa::ui::key_bindings
