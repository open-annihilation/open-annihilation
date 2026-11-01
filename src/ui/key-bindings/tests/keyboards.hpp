// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Keyboard maps the binding tests share: French AZERTY as a Mac and as a PC
// lay it out, German QWERTZ and US Dvorak. Each key records what its events
// carry and what it prints, as the game reads them from the system: a French
// number row's events carry the digits while it prints & é " and the rest.
#pragma once

#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace keyboards {

namespace keys = oa::ui::key_bindings;

/// One key of a map that is not where the US map has it.
struct Key {
    std::string_view code{}; ///< the UI Events code of the place
    char32_t carried{};      ///< the character its events carry
    char32_t printed{};      ///< the character it prints with no modifier
};

/// Changes some keys of a map.
///
/// @param[in,out] map the map
/// @param changed the keys
inline void set_keys(keys::KeyboardMap& map, std::initializer_list<Key> changed) {
    for (const Key& key : changed) {
        const std::optional<uint16_t> scancode = keys::scancode_of(key.code);
        if (scancode)
            map.set(*scancode, static_cast<uint32_t>(key.carried), key.printed);
    }
}

/// The keys both French layouts share, on the US map: A, Z, Q, W and M move,
/// and the number row's events carry its digits.
///
/// @return the map
inline keys::KeyboardMap french() {
    keys::KeyboardMap map = keys::KeyboardMap::us();
    set_keys(map, {Key{"KeyQ", U'a', U'a'},         Key{"KeyA", U'q', U'q'},
                   Key{"KeyW", U'z', U'z'},         Key{"KeyZ", U'w', U'w'},
                   Key{"Semicolon", U'm', U'm'},    Key{"KeyM", U',', U','},
                   Key{"Comma", U';', U';'},        Key{"Period", U':', U':'},
                   Key{"Digit1", U'1', U'&'},       Key{"Digit2", U'2', U'é'},
                   Key{"Digit3", U'3', U'"'},       Key{"Digit4", U'4', U'\''},
                   Key{"Digit5", U'5', U'('},       Key{"Digit7", U'7', U'è'},
                   Key{"Digit9", U'9', U'ç'},       Key{"Digit0", U'0', U'à'},
                   Key{"Minus", U')', U')'},        Key{"BracketLeft", U'^', U'^'},
                   Key{"BracketRight", U'$', U'$'}, Key{"Quote", U'ù', U'ù'}});
    return map;
}

/// French AZERTY on a Mac: - is where a US keyboard has =, and = and + share
/// the key where it has /.
///
/// @return the map
inline keys::KeyboardMap mac_azerty() {
    keys::KeyboardMap map = french();
    set_keys(
        map,
        {Key{"Digit6", U'6', U'§'},
         Key{"Digit8", U'8', U'!'},
         Key{"Equal", U'-', U'-'},
         Key{"Slash", U'=', U'='},
         Key{"Backslash", U'`', U'`'},
         Key{"Backquote", U'@', U'@'}}
    );
    return map;
}

/// French AZERTY on a PC: = and + share the key where a US keyboard has =,
/// and - is on the 6 key, whose events carry 6.
///
/// @return the map
inline keys::KeyboardMap pc_azerty() {
    keys::KeyboardMap map = french();
    set_keys(
        map,
        {Key{"Digit6", U'6', U'-'},
         Key{"Digit8", U'8', U'_'},
         Key{"Equal", U'=', U'='},
         Key{"Slash", U'!', U'!'},
         Key{"Backslash", U'*', U'*'},
         Key{"Backquote", U'²', U'²'}}
    );
    return map;
}

/// German QWERTZ: + has a key of its own where a US keyboard has ], - is
/// where it has /, and = is Shift and 0.
///
/// @return the map
inline keys::KeyboardMap qwertz() {
    keys::KeyboardMap map = keys::KeyboardMap::us();
    set_keys(
        map,
        {Key{"KeyY", U'z', U'z'},
         Key{"KeyZ", U'y', U'y'},
         Key{"Minus", U'ß', U'ß'},
         Key{"Equal", U'´', U'´'},
         Key{"BracketLeft", U'ü', U'ü'},
         Key{"BracketRight", U'+', U'+'},
         Key{"Semicolon", U'ö', U'ö'},
         Key{"Quote", U'ä', U'ä'},
         Key{"Backslash", U'#', U'#'},
         Key{"Backquote", U'^', U'^'},
         Key{"Slash", U'-', U'-'}}
    );
    return map;
}

/// US Dvorak: [ and ] are where a US keyboard has - and =, = is where it has
/// ], and - is where it has '.
///
/// @return the map
inline keys::KeyboardMap dvorak() {
    keys::KeyboardMap map = keys::KeyboardMap::us();
    set_keys(
        map,
        {Key{"Minus", U'[', U'['},       Key{"Equal", U']', U']'},        Key{"KeyQ", U'\'', U'\''},
         Key{"KeyW", U',', U','},        Key{"KeyE", U'.', U'.'},         Key{"KeyR", U'p', U'p'},
         Key{"KeyT", U'y', U'y'},        Key{"KeyY", U'f', U'f'},         Key{"KeyU", U'g', U'g'},
         Key{"KeyI", U'c', U'c'},        Key{"KeyO", U'r', U'r'},         Key{"KeyP", U'l', U'l'},
         Key{"BracketLeft", U'/', U'/'}, Key{"BracketRight", U'=', U'='}, Key{"KeyS", U'o', U'o'},
         Key{"KeyD", U'e', U'e'},        Key{"KeyF", U'u', U'u'},         Key{"KeyG", U'i', U'i'},
         Key{"KeyH", U'd', U'd'},        Key{"KeyJ", U'h', U'h'},         Key{"KeyK", U't', U't'},
         Key{"KeyL", U'n', U'n'},        Key{"Semicolon", U's', U's'},    Key{"Quote", U'-', U'-'},
         Key{"KeyZ", U';', U';'},        Key{"KeyX", U'q', U'q'},         Key{"KeyC", U'j', U'j'},
         Key{"KeyV", U'k', U'k'},        Key{"KeyB", U'x', U'x'},         Key{"KeyN", U'b', U'b'},
         Key{"Comma", U'w', U'w'},       Key{"Period", U'v', U'v'},       Key{"Slash", U'z', U'z'}}
    );
    return map;
}

} // namespace keyboards
