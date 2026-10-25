// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The position numbers and the named keys' key codes, checked against SDL.
// The library itself does not include or link SDL; this program does.
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <string_view>

namespace {

namespace keys = oa::ui::key_bindings;

/// One position and the scancode SDL gives that key.
struct PositionExpect {
    std::string_view code;
    SDL_Scancode scancode;
};

/// One named key and the key code SDL gives it.
struct NamedExpect {
    std::string_view name;
    SDL_Keycode key_code;
};

/// Reports whether a list holds a position number.
///
/// @param scancodes the positions without a code
/// @param scancode the position number
/// @return true when the list holds it
bool listed(std::span<const uint16_t> scancodes, uint16_t scancode) {
    for (const uint16_t listed_code : scancodes) {
        if (listed_code == scancode)
            return true;
    }
    return false;
}

constexpr PositionExpect kPositions[] = {
    {"Backquote", SDL_SCANCODE_GRAVE},
    {"Backslash", SDL_SCANCODE_BACKSLASH},
    {"BracketLeft", SDL_SCANCODE_LEFTBRACKET},
    {"BracketRight", SDL_SCANCODE_RIGHTBRACKET},
    {"Comma", SDL_SCANCODE_COMMA},
    {"Digit0", SDL_SCANCODE_0},
    {"Digit1", SDL_SCANCODE_1},
    {"Digit2", SDL_SCANCODE_2},
    {"Digit3", SDL_SCANCODE_3},
    {"Digit4", SDL_SCANCODE_4},
    {"Digit5", SDL_SCANCODE_5},
    {"Digit6", SDL_SCANCODE_6},
    {"Digit7", SDL_SCANCODE_7},
    {"Digit8", SDL_SCANCODE_8},
    {"Digit9", SDL_SCANCODE_9},
    {"Equal", SDL_SCANCODE_EQUALS},
    {"IntlBackslash", SDL_SCANCODE_NONUSBACKSLASH},
    {"IntlRo", SDL_SCANCODE_INTERNATIONAL1},
    {"IntlYen", SDL_SCANCODE_INTERNATIONAL3},
    {"KeyA", SDL_SCANCODE_A},
    {"KeyB", SDL_SCANCODE_B},
    {"KeyC", SDL_SCANCODE_C},
    {"KeyD", SDL_SCANCODE_D},
    {"KeyE", SDL_SCANCODE_E},
    {"KeyF", SDL_SCANCODE_F},
    {"KeyG", SDL_SCANCODE_G},
    {"KeyH", SDL_SCANCODE_H},
    {"KeyI", SDL_SCANCODE_I},
    {"KeyJ", SDL_SCANCODE_J},
    {"KeyK", SDL_SCANCODE_K},
    {"KeyL", SDL_SCANCODE_L},
    {"KeyM", SDL_SCANCODE_M},
    {"KeyN", SDL_SCANCODE_N},
    {"KeyO", SDL_SCANCODE_O},
    {"KeyP", SDL_SCANCODE_P},
    {"KeyQ", SDL_SCANCODE_Q},
    {"KeyR", SDL_SCANCODE_R},
    {"KeyS", SDL_SCANCODE_S},
    {"KeyT", SDL_SCANCODE_T},
    {"KeyU", SDL_SCANCODE_U},
    {"KeyV", SDL_SCANCODE_V},
    {"KeyW", SDL_SCANCODE_W},
    {"KeyX", SDL_SCANCODE_X},
    {"KeyY", SDL_SCANCODE_Y},
    {"KeyZ", SDL_SCANCODE_Z},
    {"Minus", SDL_SCANCODE_MINUS},
    {"Period", SDL_SCANCODE_PERIOD},
    {"Quote", SDL_SCANCODE_APOSTROPHE},
    {"Semicolon", SDL_SCANCODE_SEMICOLON},
    {"Slash", SDL_SCANCODE_SLASH},
    {"AltLeft", SDL_SCANCODE_LALT},
    {"AltRight", SDL_SCANCODE_RALT},
    {"Backspace", SDL_SCANCODE_BACKSPACE},
    {"CapsLock", SDL_SCANCODE_CAPSLOCK},
    {"ContextMenu", SDL_SCANCODE_APPLICATION},
    {"ControlLeft", SDL_SCANCODE_LCTRL},
    {"ControlRight", SDL_SCANCODE_RCTRL},
    {"Convert", SDL_SCANCODE_INTERNATIONAL4},
    {"Enter", SDL_SCANCODE_RETURN},
    {"KanaMode", SDL_SCANCODE_INTERNATIONAL2},
    {"Lang1", SDL_SCANCODE_LANG1},
    {"Lang2", SDL_SCANCODE_LANG2},
    {"Lang3", SDL_SCANCODE_LANG3},
    {"Lang4", SDL_SCANCODE_LANG4},
    {"Lang5", SDL_SCANCODE_LANG5},
    {"MetaLeft", SDL_SCANCODE_LGUI},
    {"MetaRight", SDL_SCANCODE_RGUI},
    {"NonConvert", SDL_SCANCODE_INTERNATIONAL5},
    {"ShiftLeft", SDL_SCANCODE_LSHIFT},
    {"ShiftRight", SDL_SCANCODE_RSHIFT},
    {"Space", SDL_SCANCODE_SPACE},
    {"Tab", SDL_SCANCODE_TAB},
    {"Delete", SDL_SCANCODE_DELETE},
    {"End", SDL_SCANCODE_END},
    {"Help", SDL_SCANCODE_HELP},
    {"Home", SDL_SCANCODE_HOME},
    {"Insert", SDL_SCANCODE_INSERT},
    {"PageDown", SDL_SCANCODE_PAGEDOWN},
    {"PageUp", SDL_SCANCODE_PAGEUP},
    {"ArrowDown", SDL_SCANCODE_DOWN},
    {"ArrowLeft", SDL_SCANCODE_LEFT},
    {"ArrowRight", SDL_SCANCODE_RIGHT},
    {"ArrowUp", SDL_SCANCODE_UP},
    {"NumLock", SDL_SCANCODE_NUMLOCKCLEAR},
    {"Numpad0", SDL_SCANCODE_KP_0},
    {"Numpad1", SDL_SCANCODE_KP_1},
    {"Numpad2", SDL_SCANCODE_KP_2},
    {"Numpad3", SDL_SCANCODE_KP_3},
    {"Numpad4", SDL_SCANCODE_KP_4},
    {"Numpad5", SDL_SCANCODE_KP_5},
    {"Numpad6", SDL_SCANCODE_KP_6},
    {"Numpad7", SDL_SCANCODE_KP_7},
    {"Numpad8", SDL_SCANCODE_KP_8},
    {"Numpad9", SDL_SCANCODE_KP_9},
    {"NumpadAdd", SDL_SCANCODE_KP_PLUS},
    {"NumpadBackspace", SDL_SCANCODE_KP_BACKSPACE},
    {"NumpadClear", SDL_SCANCODE_KP_CLEAR},
    {"NumpadClearEntry", SDL_SCANCODE_KP_CLEARENTRY},
    {"NumpadComma", SDL_SCANCODE_KP_COMMA},
    {"NumpadDecimal", SDL_SCANCODE_KP_PERIOD},
    {"NumpadDivide", SDL_SCANCODE_KP_DIVIDE},
    {"NumpadEnter", SDL_SCANCODE_KP_ENTER},
    {"NumpadEqual", SDL_SCANCODE_KP_EQUALS},
    {"NumpadHash", SDL_SCANCODE_KP_HASH},
    {"NumpadMemoryAdd", SDL_SCANCODE_KP_MEMADD},
    {"NumpadMemoryClear", SDL_SCANCODE_KP_MEMCLEAR},
    {"NumpadMemoryRecall", SDL_SCANCODE_KP_MEMRECALL},
    {"NumpadMemoryStore", SDL_SCANCODE_KP_MEMSTORE},
    {"NumpadMemorySubtract", SDL_SCANCODE_KP_MEMSUBTRACT},
    {"NumpadMultiply", SDL_SCANCODE_KP_MULTIPLY},
    {"NumpadParenLeft", SDL_SCANCODE_KP_LEFTPAREN},
    {"NumpadParenRight", SDL_SCANCODE_KP_RIGHTPAREN},
    {"NumpadSubtract", SDL_SCANCODE_KP_MINUS},
    {"Escape", SDL_SCANCODE_ESCAPE},
    {"F1", SDL_SCANCODE_F1},
    {"F2", SDL_SCANCODE_F2},
    {"F3", SDL_SCANCODE_F3},
    {"F4", SDL_SCANCODE_F4},
    {"F5", SDL_SCANCODE_F5},
    {"F6", SDL_SCANCODE_F6},
    {"F7", SDL_SCANCODE_F7},
    {"F8", SDL_SCANCODE_F8},
    {"F9", SDL_SCANCODE_F9},
    {"F10", SDL_SCANCODE_F10},
    {"F11", SDL_SCANCODE_F11},
    {"F12", SDL_SCANCODE_F12},
    {"F13", SDL_SCANCODE_F13},
    {"F14", SDL_SCANCODE_F14},
    {"F15", SDL_SCANCODE_F15},
    {"F16", SDL_SCANCODE_F16},
    {"F17", SDL_SCANCODE_F17},
    {"F18", SDL_SCANCODE_F18},
    {"F19", SDL_SCANCODE_F19},
    {"F20", SDL_SCANCODE_F20},
    {"F21", SDL_SCANCODE_F21},
    {"F22", SDL_SCANCODE_F22},
    {"F23", SDL_SCANCODE_F23},
    {"F24", SDL_SCANCODE_F24},
    {"Pause", SDL_SCANCODE_PAUSE},
    {"PrintScreen", SDL_SCANCODE_PRINTSCREEN},
    {"ScrollLock", SDL_SCANCODE_SCROLLLOCK},
    {"Again", SDL_SCANCODE_AGAIN},
    {"AudioVolumeDown", SDL_SCANCODE_VOLUMEDOWN},
    {"AudioVolumeMute", SDL_SCANCODE_MUTE},
    {"AudioVolumeUp", SDL_SCANCODE_VOLUMEUP},
    {"BrowserBack", SDL_SCANCODE_AC_BACK},
    {"BrowserFavorites", SDL_SCANCODE_AC_BOOKMARKS},
    {"BrowserForward", SDL_SCANCODE_AC_FORWARD},
    {"BrowserHome", SDL_SCANCODE_AC_HOME},
    {"BrowserRefresh", SDL_SCANCODE_AC_REFRESH},
    {"BrowserSearch", SDL_SCANCODE_AC_SEARCH},
    {"BrowserStop", SDL_SCANCODE_AC_STOP},
    {"Copy", SDL_SCANCODE_COPY},
    {"Cut", SDL_SCANCODE_CUT},
    {"Eject", SDL_SCANCODE_MEDIA_EJECT},
    {"Find", SDL_SCANCODE_FIND},
    {"MediaPlayPause", SDL_SCANCODE_MEDIA_PLAY_PAUSE},
    {"MediaSelect", SDL_SCANCODE_MEDIA_SELECT},
    {"MediaStop", SDL_SCANCODE_MEDIA_STOP},
    {"MediaTrackNext", SDL_SCANCODE_MEDIA_NEXT_TRACK},
    {"MediaTrackPrevious", SDL_SCANCODE_MEDIA_PREVIOUS_TRACK},
    {"Paste", SDL_SCANCODE_PASTE},
    {"Power", SDL_SCANCODE_POWER},
    {"Select", SDL_SCANCODE_SELECT},
    {"Sleep", SDL_SCANCODE_SLEEP},
    {"Undo", SDL_SCANCODE_UNDO},
    {"WakeUp", SDL_SCANCODE_WAKE},
};

constexpr NamedExpect kNamed[] = {
    {"Escape", SDLK_ESCAPE},
    {"Enter", SDLK_RETURN},
    {"Tab", SDLK_TAB},
    {"Backspace", SDLK_BACKSPACE},
    {"Space", SDLK_SPACE},
    {"Delete", SDLK_DELETE},
    {"F1", SDLK_F1},
    {"F2", SDLK_F2},
    {"F3", SDLK_F3},
    {"F4", SDLK_F4},
    {"F5", SDLK_F5},
    {"F6", SDLK_F6},
    {"F7", SDLK_F7},
    {"F8", SDLK_F8},
    {"F9", SDLK_F9},
    {"F10", SDLK_F10},
    {"F11", SDLK_F11},
    {"F12", SDLK_F12},
    {"F13", SDLK_F13},
    {"F14", SDLK_F14},
    {"F15", SDLK_F15},
    {"F16", SDLK_F16},
    {"F17", SDLK_F17},
    {"F18", SDLK_F18},
    {"F19", SDLK_F19},
    {"F20", SDLK_F20},
    {"F21", SDLK_F21},
    {"F22", SDLK_F22},
    {"F23", SDLK_F23},
    {"F24", SDLK_F24},
    {"Insert", SDLK_INSERT},
    {"Home", SDLK_HOME},
    {"End", SDLK_END},
    {"PageUp", SDLK_PAGEUP},
    {"PageDown", SDLK_PAGEDOWN},
    {"Up", SDLK_UP},
    {"Down", SDLK_DOWN},
    {"Left", SDLK_LEFT},
    {"Right", SDLK_RIGHT},
    {"Numpad0", SDLK_KP_0},
    {"Numpad1", SDLK_KP_1},
    {"Numpad2", SDLK_KP_2},
    {"Numpad3", SDLK_KP_3},
    {"Numpad4", SDLK_KP_4},
    {"Numpad5", SDLK_KP_5},
    {"Numpad6", SDLK_KP_6},
    {"Numpad7", SDLK_KP_7},
    {"Numpad8", SDLK_KP_8},
    {"Numpad9", SDLK_KP_9},
    {"NumpadAdd", SDLK_KP_PLUS},
    {"NumpadSubtract", SDLK_KP_MINUS},
    {"NumpadMultiply", SDLK_KP_MULTIPLY},
    {"NumpadDivide", SDLK_KP_DIVIDE},
    {"NumpadDecimal", SDLK_KP_PERIOD},
    {"NumpadEnter", SDLK_KP_ENTER},
    {"PrintScreen", SDLK_PRINTSCREEN},
    {"Pause", SDLK_PAUSE},
    {"ContextMenu", SDLK_APPLICATION},
};

/// Every scancode SDL names is in the table or on the unnamed list.
void every_named_scancode() {
    const std::span<const uint16_t> without = keys::scancodes_without_code();
    for (int number = 0; number < SDL_SCANCODE_COUNT; ++number) {
        const auto scancode = static_cast<SDL_Scancode>(number);
        const char* name = SDL_GetScancodeName(scancode);
        if (name == nullptr || name[0] == '\0')
            continue;
        const bool in_table = !keys::code_of(static_cast<uint16_t>(number)).empty();
        const bool in_without = listed(without, static_cast<uint16_t>(number));
        OA_CHECK(in_table || in_without);
        OA_CHECK(!(in_table && in_without));
    }
    for (const uint16_t scancode : without) {
        OA_CHECK(scancode < SDL_SCANCODE_COUNT);
        const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
        OA_CHECK(name != nullptr);
        OA_CHECK(name[0] != '\0');
        OA_CHECK(keys::code_of(scancode).empty());
    }
}

/// Every table row's position number is SDL's for that key.
void positions_match() {
    OA_CHECK(std::size(kPositions) == keys::key_positions().size());
    for (const keys::KeyPosition& position : keys::key_positions()) {
        const PositionExpect* expect = nullptr;
        for (const PositionExpect& row : kPositions) {
            if (row.code == position.code)
                expect = &row;
        }
        OA_CHECK(expect != nullptr);
        if (expect != nullptr) {
            OA_CHECK(position.scancode == expect->scancode);
            OA_CHECK(keys::scancode_of(position.code) == position.scancode);
        }
    }
    OA_CHECK(keys::scancode_of("KeyA") == SDL_SCANCODE_A);
    OA_CHECK(keys::scancode_of("NumpadEnter") == SDL_SCANCODE_KP_ENTER);
}

/// Every named key's key code is SDL's.
void named_keys_match() {
    OA_CHECK(std::size(kNamed) == keys::named_keys().size());
    for (const keys::NamedKeySpec& key : keys::named_keys()) {
        const NamedExpect* expect = nullptr;
        for (const NamedExpect& row : kNamed) {
            if (row.name == key.name)
                expect = &row;
        }
        OA_CHECK(expect != nullptr);
        if (expect != nullptr)
            OA_CHECK(key.key_code == static_cast<uint32_t>(expect->key_code));
    }
}

} // namespace

int main() {
    every_named_scancode();
    positions_match();
    named_keys_match();
    return oa::test::check_exit_status();
}
