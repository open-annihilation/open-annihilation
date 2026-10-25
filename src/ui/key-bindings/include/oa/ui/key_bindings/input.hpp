// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One input event, the platform's primary modifier, and what a keyboard
// prints. The module reads no clock: the event carries its own time.
#pragma once

#include "oa/ui/key_bindings/key_codes.hpp"

#include <array>
#include <cstdint>

namespace oa::ui::key_bindings {

/// What an input event is.
enum class InputKind : std::uint8_t {
    key,    ///< a keyboard key
    button, ///< a mouse button
    wheel,  ///< a wheel turn
    pad,    ///< a pad button; a chord never matches one
};

/// One key, button or wheel event, already translated for the player.
///
/// `modifiers` holds the physical Ctrl, Alt, Shift and Meta keys that are
/// held, using the bits of `Modifier`. It never carries `primary`: that bit
/// is a chord's, and the platform decides which physical key it means.
struct InputEvent {
    InputKind kind{};         ///< which kind of event
    bool down{};              ///< the control went down, rather than up
    bool repeat{};            ///< the platform repeated a key that is still held
    std::uint16_t scancode{}; ///< the key's position number; 0 for a button or a wheel
    std::uint32_t key_code{}; ///< the character an event carries, or a position key code
    std::uint8_t button{};    ///< a mouse button, 1 to 8
    std::int8_t wheel_x{};    ///< notches across; positive is to the right
    std::int8_t wheel_y{};    ///< notches away from the player; positive is up
    std::uint8_t clicks{};    ///< how many clicks landed together, for a button
    std::uint8_t modifiers{}; ///< the physical modifiers held, never `primary`
    std::uint64_t time_ms{};  ///< when the event happened, in milliseconds
};

/// Which physical key the word Primary means on this platform.
struct Platform {
    bool primary_is_meta{}; ///< true where Primary is the platform's command key
};

/// What each key's events carry, and the character the key prints.
///
/// One slot for every position number below `scancode_limit`. A zero key code
/// means the map does not know that key.
struct KeyboardMap {
    /// Returns the US layout: letters and digits as events carry them, and the
    /// character each writing-system key prints with no modifier held.
    ///
    /// @return the US map
    [[nodiscard]] static KeyboardMap us();

    /// Records one key.
    ///
    /// @param scancode the position number, ignored when it is past the map
    /// @param event_key_code the key code events for that key carry
    /// @param printed the character the key prints, or 0 when it prints none
    void set(std::uint16_t scancode, std::uint32_t event_key_code, char32_t printed);

    /// Returns the key code events carry for a key.
    ///
    /// @param scancode the position number
    /// @return the key code, or 0 when the map does not know it
    [[nodiscard]] std::uint32_t event_key_code(std::uint16_t scancode) const;

    /// Returns the character a key prints.
    ///
    /// @param scancode the position number
    /// @return the character, or 0 when the key prints none
    [[nodiscard]] char32_t printed(std::uint16_t scancode) const;

  private:

    struct Slot {
        std::uint32_t event_key_code{}; ///< the key code events carry, or 0
        char32_t printed{};             ///< the character the key prints, or 0
    };

    std::array<Slot, scancode_limit> slots_{};
};

} // namespace oa::ui::key_bindings
