// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hold latches and sticky modifiers. The caller passes the time; this
// module does not read a clock and does not link SDL.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

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

} // namespace oa::ui::key_bindings
