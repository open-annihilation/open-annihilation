// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The US keyboard map: the key code events carry, and the character each
// writing-system key prints.

#include "oa/ui/key_bindings/input.hpp"

namespace oa::ui::key_bindings {
namespace {

/// The character a US key prints with no modifier held.
///
/// @param scancode the position number
/// @return the character, or 0 when the key prints none
char32_t us_printed(uint16_t scancode) {
    if (scancode >= 4 && scancode <= 29)
        return static_cast<char32_t>('a' + (scancode - 4));
    if (scancode >= 30 && scancode <= 38)
        return static_cast<char32_t>('1' + (scancode - 30));
    if (scancode == 39)
        return '0';
    switch (scancode) {
    case 45:
        return '-';
    case 46:
        return '=';
    case 47:
        return '[';
    case 48:
        return ']';
    case 49:
        return '\\';
    case 51:
        return ';';
    case 52:
        return '\'';
    case 53:
        return '`';
    case 54:
        return ',';
    case 55:
        return '.';
    case 56:
        return '/';
    default:
        return 0;
    }
}

/// The key code a US key's events carry.
///
/// @param scancode the position number
/// @return the key code, or 0 for position 0
uint32_t us_event(uint16_t scancode) {
    const char32_t printed = us_printed(scancode);
    if (printed != 0)
        return static_cast<uint32_t>(printed);
    switch (scancode) {
    case 40:
        return 0x0D;
    case 41:
        return 0x1B;
    case 42:
        return 0x08;
    case 43:
        return 0x09;
    case 44:
        return 0x20;
    case 76:
        return 0x7F;
    default:
        break;
    }
    if (scancode == 0)
        return 0;
    return uint32_t{scancode} | position_key_code_bit;
}

} // namespace

KeyboardMap KeyboardMap::us() {
    KeyboardMap map;
    for (uint16_t scancode = 1; scancode < scancode_limit; ++scancode)
        map.set(scancode, us_event(scancode), us_printed(scancode));
    return map;
}

void KeyboardMap::set(uint16_t scancode, uint32_t event_key_code, char32_t printed) {
    if (scancode >= scancode_limit)
        return;
    slots_[scancode].event_key_code = event_key_code;
    slots_[scancode].printed = printed;
}

uint32_t KeyboardMap::event_key_code(uint16_t scancode) const {
    if (scancode >= scancode_limit)
        return 0;
    return slots_[scancode].event_key_code;
}

char32_t KeyboardMap::printed(uint16_t scancode) const {
    if (scancode >= scancode_limit)
        return 0;
    return slots_[scancode].printed;
}

} // namespace oa::ui::key_bindings
