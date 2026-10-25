// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turns one input event into the action events its bindings start. The
// caller passes the time; this module does not read a clock.
#pragma once

#include "oa/ui/key_bindings/layout.hpp"

#include <cstdint>
#include <memory>
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
/// next `set_bindings`.
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

/// How long a double press and a long press wait. The caller counts the time.
struct Timings {
    uint32_t double_press_ms = 500; ///< two downs of one key at most this far apart
    uint32_t long_press_ms = 500;   ///< how long a `:hold` chord waits
};

/// What `feed`, `advance` or `release_all` produced.
struct Fed {
    std::vector<ActionEvent> events{}; ///< the action events, in the order they happened
    bool bound{};                      ///< a binding matched the event
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

    /// Replaces the bindings.
    ///
    /// Held keys, roles and timers are dropped and no action event is made
    /// for them. Call `release_all` first when those events are wanted.
    /// Events returned before this call are no longer valid.
    ///
    /// @param bindings the bindings in use
    void set_bindings(const Resolved& bindings);

    /// Replaces the keyboard map a letter and a place are compared on.
    ///
    /// @param keyboard the map
    void set_keyboard(const KeyboardMap& keyboard);

    /// Says which key Primary means.
    ///
    /// @param platform the platform
    void set_platform(Platform platform);

    /// Replaces the double-press and hold times.
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
    /// nothing.
    ///
    /// @param event the key, button or wheel
    /// @return the action events, and whether a binding matched
    [[nodiscard]] Fed feed(const InputEvent& event);

    /// Fires each `:hold` chord that has been down for `long_press_ms`.
    ///
    /// A hold fires once. An earlier `advance` does not fire it, and a later
    /// one does not fire it again.
    ///
    /// @param now_ms the time, in milliseconds, from the same clock as the events
    /// @return the `held` events
    [[nodiscard]] Fed advance(uint64_t now_ms);

    /// Releases every held action and deactivates every modifier role.
    ///
    /// Held keys, buttons and timers are cleared. A hold that had not fired
    /// does not fire.
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
    /// the key events `feed` saw.
    ///
    /// @param action the action id
    /// @param modifiers_now the physical modifiers held, never `primary`
    /// @param keys_held which key positions are held
    /// @return true while the role is active
    [[nodiscard]] bool
    role_active(std::string_view action, uint8_t modifiers_now, const HeldKeys& keys_held) const;

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
