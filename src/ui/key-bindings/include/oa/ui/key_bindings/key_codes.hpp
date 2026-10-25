// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Key positions by their UI Events code, and the named keys of a chord.
// A position number is the USB keyboard usage of that key. The module names
// every position a keyboard report can carry that the UI Events code list
// names, and lists the rest separately: a chord cannot name those.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace oa::ui::key_bindings {

/// How many position numbers a keyboard map holds. A report uses those below this.
inline constexpr std::uint16_t scancode_limit = 512;

/// Bit set on a key code that names a position rather than a character.
inline constexpr std::uint32_t position_key_code_bit = 1u << 30;

/// One physical key: its UI Events code, its position number and whether it prints.
struct KeyPosition {
    std::string_view code{};  ///< the UI Events code, such as `KeyA` or `ArrowUp`
    std::uint16_t scancode{}; ///< the key's position number
    bool prints{};            ///< a writing-system key, whose label follows the layout
};

/// A named key of a chord, with the key code an event carries for it.
enum class NamedKey : std::uint8_t {
    escape,
    enter,
    tab,
    backspace,
    space,
    delete_key,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    f13,
    f14,
    f15,
    f16,
    f17,
    f18,
    f19,
    f20,
    f21,
    f22,
    f23,
    f24,
    insert,
    home,
    end,
    page_up,
    page_down,
    up,
    down,
    left,
    right,
    numpad_0,
    numpad_1,
    numpad_2,
    numpad_3,
    numpad_4,
    numpad_5,
    numpad_6,
    numpad_7,
    numpad_8,
    numpad_9,
    numpad_add,
    numpad_subtract,
    numpad_multiply,
    numpad_divide,
    numpad_decimal,
    numpad_enter,
    print_screen,
    pause,
    context_menu,
};

/// One named key: the word a chord writes, its key code and its position.
struct NamedKeySpec {
    std::string_view name{};  ///< the chord's word, such as `Enter` or `F1`
    std::uint32_t key_code{}; ///< the key code an event carries
    std::uint16_t scancode{}; ///< the key's position number
    NamedKey id{};            ///< the same key, as the chord stores it
};

/// Returns every key position, in section order.
///
/// @return the position table
[[nodiscard]] std::span<const KeyPosition> key_positions() noexcept;

/// Finds the position number of a UI Events code.
///
/// @param code the code, such as `Digit1`
/// @return the position number, or nothing when the table has no such code
[[nodiscard]] std::optional<std::uint16_t> scancode_of(std::string_view code) noexcept;

/// Finds the UI Events code of a position number.
///
/// @param scancode the position number
/// @return the code, or empty when the table does not name it
[[nodiscard]] std::string_view code_of(std::uint16_t scancode) noexcept;

/// Returns every named key, in enum order.
///
/// @return the named-key table
[[nodiscard]] std::span<const NamedKeySpec> named_keys() noexcept;

/// Finds a named key by the word a chord writes.
///
/// @param name the word, such as `F1` or `Up`
/// @return the key, or null when it is not a named key
[[nodiscard]] const NamedKeySpec* find_named_key(std::string_view name) noexcept;

/// Returns the position numbers a keyboard report can carry that no chord can name.
///
/// @return those position numbers, in ascending order
[[nodiscard]] std::span<const std::uint16_t> scancodes_without_code() noexcept;

/// Reports whether a UI Events code is a lock or a modifier key's own position.
///
/// A chord cannot name these. The position stays in the table so a report can
/// still say which key it was.
///
/// @param code the UI Events code
/// @return true for CapsLock, NumLock, ScrollLock and the modifier positions
[[nodiscard]] bool position_refused(std::string_view code) noexcept;

} // namespace oa::ui::key_bindings
