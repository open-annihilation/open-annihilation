// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The position table and the named keys. Position numbers are USB keyboard
// usages. A consumer-page key uses the position number a report carries for
// it, which is not the usage itself.

#include "oa/ui/key_bindings/key_codes.hpp"

#include <cstddef>
#include <iterator>

namespace oa::ui::key_bindings {
namespace {

/// The key code of a key that types no character: its position, with the position bit set.
///
/// @param scancode the position number
/// @return the key code
constexpr uint32_t at(uint16_t scancode) {
    return uint32_t{scancode} | position_key_code_bit;
}

constexpr KeyPosition kPositions[] = {
    // Writing-system keys.
    {"Backquote", 53, true},
    {"Backslash", 49, true},
    {"BracketLeft", 47, true},
    {"BracketRight", 48, true},
    {"Comma", 54, true},
    {"Digit0", 39, true},
    {"Digit1", 30, true},
    {"Digit2", 31, true},
    {"Digit3", 32, true},
    {"Digit4", 33, true},
    {"Digit5", 34, true},
    {"Digit6", 35, true},
    {"Digit7", 36, true},
    {"Digit8", 37, true},
    {"Digit9", 38, true},
    {"Equal", 46, true},
    {"IntlBackslash", 100, true},
    {"IntlRo", 135, true},
    {"IntlYen", 137, true},
    {"KeyA", 4, true},
    {"KeyB", 5, true},
    {"KeyC", 6, true},
    {"KeyD", 7, true},
    {"KeyE", 8, true},
    {"KeyF", 9, true},
    {"KeyG", 10, true},
    {"KeyH", 11, true},
    {"KeyI", 12, true},
    {"KeyJ", 13, true},
    {"KeyK", 14, true},
    {"KeyL", 15, true},
    {"KeyM", 16, true},
    {"KeyN", 17, true},
    {"KeyO", 18, true},
    {"KeyP", 19, true},
    {"KeyQ", 20, true},
    {"KeyR", 21, true},
    {"KeyS", 22, true},
    {"KeyT", 23, true},
    {"KeyU", 24, true},
    {"KeyV", 25, true},
    {"KeyW", 26, true},
    {"KeyX", 27, true},
    {"KeyY", 28, true},
    {"KeyZ", 29, true},
    {"Minus", 45, true},
    {"Period", 55, true},
    {"Quote", 52, true},
    {"Semicolon", 51, true},
    {"Slash", 56, true},
    // Functional keys, including the locks and the modifier positions.
    {"AltLeft", 226, false},
    {"AltRight", 230, false},
    {"Backspace", 42, false},
    {"CapsLock", 57, false},
    {"ContextMenu", 101, false},
    {"ControlLeft", 224, false},
    {"ControlRight", 228, false},
    {"Convert", 138, false},
    {"Enter", 40, false},
    {"KanaMode", 136, false},
    {"Lang1", 144, false},
    {"Lang2", 145, false},
    {"Lang3", 146, false},
    {"Lang4", 147, false},
    {"Lang5", 148, false},
    {"MetaLeft", 227, false},
    {"MetaRight", 231, false},
    {"NonConvert", 139, false},
    {"ShiftLeft", 225, false},
    {"ShiftRight", 229, false},
    {"Space", 44, false},
    {"Tab", 43, false},
    // Control pad.
    {"Delete", 76, false},
    {"End", 77, false},
    {"Help", 117, false},
    {"Home", 74, false},
    {"Insert", 73, false},
    {"PageDown", 78, false},
    {"PageUp", 75, false},
    // Arrows.
    {"ArrowDown", 81, false},
    {"ArrowLeft", 80, false},
    {"ArrowRight", 79, false},
    {"ArrowUp", 82, false},
    // Numpad.
    {"NumLock", 83, false},
    {"Numpad0", 98, false},
    {"Numpad1", 89, false},
    {"Numpad2", 90, false},
    {"Numpad3", 91, false},
    {"Numpad4", 92, false},
    {"Numpad5", 93, false},
    {"Numpad6", 94, false},
    {"Numpad7", 95, false},
    {"Numpad8", 96, false},
    {"Numpad9", 97, false},
    {"NumpadAdd", 87, false},
    {"NumpadBackspace", 187, false},
    {"NumpadClear", 216, false},
    {"NumpadClearEntry", 217, false},
    {"NumpadComma", 133, false},
    {"NumpadDecimal", 99, false},
    {"NumpadDivide", 84, false},
    {"NumpadEnter", 88, false},
    {"NumpadEqual", 103, false},
    {"NumpadHash", 204, false},
    {"NumpadMemoryAdd", 211, false},
    {"NumpadMemoryClear", 210, false},
    {"NumpadMemoryRecall", 209, false},
    {"NumpadMemoryStore", 208, false},
    {"NumpadMemorySubtract", 212, false},
    {"NumpadMultiply", 85, false},
    {"NumpadParenLeft", 182, false},
    {"NumpadParenRight", 183, false},
    {"NumpadSubtract", 86, false},
    // Function row.
    {"Escape", 41, false},
    {"F1", 58, false},
    {"F2", 59, false},
    {"F3", 60, false},
    {"F4", 61, false},
    {"F5", 62, false},
    {"F6", 63, false},
    {"F7", 64, false},
    {"F8", 65, false},
    {"F9", 66, false},
    {"F10", 67, false},
    {"F11", 68, false},
    {"F12", 69, false},
    {"F13", 104, false},
    {"F14", 105, false},
    {"F15", 106, false},
    {"F16", 107, false},
    {"F17", 108, false},
    {"F18", 109, false},
    {"F19", 110, false},
    {"F20", 111, false},
    {"F21", 112, false},
    {"F22", 113, false},
    {"F23", 114, false},
    {"F24", 115, false},
    {"Pause", 72, false},
    {"PrintScreen", 70, false},
    {"ScrollLock", 71, false},
    // Media keys and the editing keys that share the keyboard page.
    {"Again", 121, false},
    {"AudioVolumeDown", 129, false},
    {"AudioVolumeMute", 127, false},
    {"AudioVolumeUp", 128, false},
    {"BrowserBack", 282, false},
    {"BrowserFavorites", 286, false},
    {"BrowserForward", 283, false},
    {"BrowserHome", 281, false},
    {"BrowserRefresh", 285, false},
    {"BrowserSearch", 280, false},
    {"BrowserStop", 284, false},
    {"Copy", 124, false},
    {"Cut", 123, false},
    {"Eject", 270, false},
    {"Find", 126, false},
    {"MediaPlayPause", 271, false},
    {"MediaSelect", 272, false},
    {"MediaStop", 269, false},
    {"MediaTrackNext", 267, false},
    {"MediaTrackPrevious", 268, false},
    {"Paste", 125, false},
    {"Power", 102, false},
    {"Select", 119, false},
    {"Sleep", 258, false},
    {"Undo", 122, false},
    {"WakeUp", 259, false},
};

constexpr NamedKeySpec kNamed[] = {
    {"Escape", 0x1B, 41, NamedKey::escape},
    {"Enter", 0x0D, 40, NamedKey::enter},
    {"Tab", 0x09, 43, NamedKey::tab},
    {"Backspace", 0x08, 42, NamedKey::backspace},
    {"Space", 0x20, 44, NamedKey::space},
    {"Delete", 0x7F, 76, NamedKey::delete_key},
    {"F1", at(58), 58, NamedKey::f1},
    {"F2", at(59), 59, NamedKey::f2},
    {"F3", at(60), 60, NamedKey::f3},
    {"F4", at(61), 61, NamedKey::f4},
    {"F5", at(62), 62, NamedKey::f5},
    {"F6", at(63), 63, NamedKey::f6},
    {"F7", at(64), 64, NamedKey::f7},
    {"F8", at(65), 65, NamedKey::f8},
    {"F9", at(66), 66, NamedKey::f9},
    {"F10", at(67), 67, NamedKey::f10},
    {"F11", at(68), 68, NamedKey::f11},
    {"F12", at(69), 69, NamedKey::f12},
    {"F13", at(104), 104, NamedKey::f13},
    {"F14", at(105), 105, NamedKey::f14},
    {"F15", at(106), 106, NamedKey::f15},
    {"F16", at(107), 107, NamedKey::f16},
    {"F17", at(108), 108, NamedKey::f17},
    {"F18", at(109), 109, NamedKey::f18},
    {"F19", at(110), 110, NamedKey::f19},
    {"F20", at(111), 111, NamedKey::f20},
    {"F21", at(112), 112, NamedKey::f21},
    {"F22", at(113), 113, NamedKey::f22},
    {"F23", at(114), 114, NamedKey::f23},
    {"F24", at(115), 115, NamedKey::f24},
    {"Insert", at(73), 73, NamedKey::insert},
    {"Home", at(74), 74, NamedKey::home},
    {"End", at(77), 77, NamedKey::end},
    {"PageUp", at(75), 75, NamedKey::page_up},
    {"PageDown", at(78), 78, NamedKey::page_down},
    {"Up", at(82), 82, NamedKey::up},
    {"Down", at(81), 81, NamedKey::down},
    {"Left", at(80), 80, NamedKey::left},
    {"Right", at(79), 79, NamedKey::right},
    {"Numpad0", at(98), 98, NamedKey::numpad_0},
    {"Numpad1", at(89), 89, NamedKey::numpad_1},
    {"Numpad2", at(90), 90, NamedKey::numpad_2},
    {"Numpad3", at(91), 91, NamedKey::numpad_3},
    {"Numpad4", at(92), 92, NamedKey::numpad_4},
    {"Numpad5", at(93), 93, NamedKey::numpad_5},
    {"Numpad6", at(94), 94, NamedKey::numpad_6},
    {"Numpad7", at(95), 95, NamedKey::numpad_7},
    {"Numpad8", at(96), 96, NamedKey::numpad_8},
    {"Numpad9", at(97), 97, NamedKey::numpad_9},
    {"NumpadAdd", at(87), 87, NamedKey::numpad_add},
    {"NumpadSubtract", at(86), 86, NamedKey::numpad_subtract},
    {"NumpadMultiply", at(85), 85, NamedKey::numpad_multiply},
    {"NumpadDivide", at(84), 84, NamedKey::numpad_divide},
    {"NumpadDecimal", at(99), 99, NamedKey::numpad_decimal},
    {"NumpadEnter", at(88), 88, NamedKey::numpad_enter},
    {"PrintScreen", at(70), 70, NamedKey::print_screen},
    {"Pause", at(72), 72, NamedKey::pause},
    {"ContextMenu", at(101), 101, NamedKey::context_menu},
};

// Position numbers a report names that the UI Events code list does not.
constexpr uint16_t kWithout[] = {
    50,  116, 118, 120, 134, 140, 141, 142, 143, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 158, 159, 160, 161, 162, 163, 164, 176, 177, 178, 179, 180, 181, 184,
    185, 186, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201,
    202, 203, 205, 206, 207, 213, 214, 215, 218, 219, 220, 221, 257, 260, 261, 262,
    263, 264, 265, 266, 273, 274, 275, 276, 277, 278, 279, 287, 288, 289, 290,
};

constexpr bool named_ids_follow_the_enum() {
    for (std::size_t index = 0; index < std::size(kNamed); ++index) {
        if (static_cast<std::size_t>(kNamed[index].id) != index)
            return false;
    }
    return true;
}

constexpr bool positions_are_unique() {
    for (std::size_t left = 0; left < std::size(kPositions); ++left) {
        for (std::size_t right = left + 1; right < std::size(kPositions); ++right) {
            if (kPositions[left].scancode == kPositions[right].scancode)
                return false;
            if (kPositions[left].code == kPositions[right].code)
                return false;
        }
    }
    return true;
}

constexpr bool without_is_sorted() {
    for (std::size_t index = 1; index < std::size(kWithout); ++index) {
        if (kWithout[index] <= kWithout[index - 1])
            return false;
    }
    return true;
}

static_assert(named_ids_follow_the_enum(), "named keys stay in enum order");
static_assert(positions_are_unique(), "each position number and code is listed once");
static_assert(without_is_sorted(), "positions without a code stay in order");

constexpr std::string_view kRefused[] = {
    "CapsLock",
    "NumLock",
    "ScrollLock",
    "ShiftLeft",
    "ShiftRight",
    "ControlLeft",
    "ControlRight",
    "AltLeft",
    "AltRight",
    "MetaLeft",
    "MetaRight",
};

} // namespace

std::span<const KeyPosition> key_positions() noexcept {
    return kPositions;
}

std::optional<uint16_t> scancode_of(std::string_view code) noexcept {
    for (const KeyPosition& position : kPositions) {
        if (position.code == code)
            return position.scancode;
    }
    return std::nullopt;
}

std::string_view code_of(uint16_t scancode) noexcept {
    for (const KeyPosition& position : kPositions) {
        if (position.scancode == scancode)
            return position.code;
    }
    return {};
}

std::span<const NamedKeySpec> named_keys() noexcept {
    return kNamed;
}

const NamedKeySpec* find_named_key(std::string_view name) noexcept {
    for (const NamedKeySpec& key : kNamed) {
        if (key.name == name)
            return &key;
    }
    return nullptr;
}

std::span<const uint16_t> scancodes_without_code() noexcept {
    return kWithout;
}

bool position_refused(std::string_view code) noexcept {
    for (const std::string_view refused : kRefused) {
        if (refused == code)
            return true;
    }
    return false;
}

} // namespace oa::ui::key_bindings
