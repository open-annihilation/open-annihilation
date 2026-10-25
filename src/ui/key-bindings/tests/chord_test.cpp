// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chords: every registry default round-trips, each trigger form parses and
// writes back, every refusal names its byte, and matching follows the key
// code or the position, including an AZERTY map.
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include "oa/data/input_actions/actions.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;
namespace actions = oa::data::input_actions;

using keys::Chord;
using keys::ChordError;
using keys::ChordErrorReason;
using keys::Gesture;
using keys::InputEvent;
using keys::InputKind;
using keys::KeyCap;
using keys::KeyboardMap;
using keys::Match;
using keys::Platform;
using keys::TriggerKind;

/// Prints both texts when a round trip does not give the original back.
///
/// @param text the chord text
void round_trip(std::string_view text) {
    ChordError error;
    const std::optional<Chord> chord = keys::parse_chord(text, error);
    const std::string written = chord ? keys::chord_text(*chord) : std::string{};
    if (!chord || written != text) {
        std::fprintf(
            stderr,
            "round trip failed: \"%.*s\" reason %u byte %u wrote \"%s\"\n",
            static_cast<int>(text.size()),
            text.data(),
            static_cast<unsigned>(error.reason),
            error.byte,
            written.c_str()
        );
    }
    OA_CHECK(chord.has_value());
    OA_CHECK(written == text);
}

/// Parses a chord the test expects to accept.
///
/// @param text the chord text
/// @return the chord, or a default when it was refused
Chord must_parse(std::string_view text) {
    ChordError error;
    const std::optional<Chord> chord = keys::parse_chord(text, error);
    if (!chord) {
        std::fprintf(
            stderr,
            "parse failed: \"%.*s\" reason %u byte %u\n",
            static_cast<int>(text.size()),
            text.data(),
            static_cast<unsigned>(error.reason),
            error.byte
        );
    }
    OA_CHECK(chord.has_value());
    if (!chord)
        return {};
    return *chord;
}

/// Checks that parsing `text` writes `canonical`, and that parsing that writes the same chord.
///
/// @param text the text as a player might type it
/// @param canonical the one text the chord writes
void writes(std::string_view text, std::string_view canonical) {
    const Chord chord = must_parse(text);
    const std::string written = keys::chord_text(chord);
    if (written != canonical) {
        std::fprintf(
            stderr,
            "wrote \"%s\" for \"%.*s\", want \"%.*s\"\n",
            written.c_str(),
            static_cast<int>(text.size()),
            text.data(),
            static_cast<int>(canonical.size()),
            canonical.data()
        );
    }
    OA_CHECK(written == canonical);
    ChordError error;
    const std::optional<Chord> again = keys::parse_chord(canonical, error);
    OA_CHECK(again.has_value());
    if (again)
        OA_CHECK(*again == chord);
}

/// Checks a refusal and the byte it was found at.
///
/// @param text the chord text
/// @param reason the reason
/// @param byte the byte in the text
void expect_refuse(std::string_view text, ChordErrorReason reason, uint32_t byte) {
    ChordError error;
    const std::optional<Chord> chord = keys::parse_chord(text, error);
    if (chord || error.reason != reason || error.byte != byte) {
        std::fprintf(
            stderr,
            "refusal \"%.*s\": reason %u byte %u, want %u byte %u\n",
            static_cast<int>(text.size()),
            text.data(),
            static_cast<unsigned>(error.reason),
            error.byte,
            static_cast<unsigned>(reason),
            byte
        );
    }
    OA_CHECK(!chord.has_value());
    OA_CHECK(error.reason == reason);
    OA_CHECK(error.byte == byte);
}

/// One key event.
///
/// @param scancode the position number
/// @param key_code the key code the event carries
/// @param modifiers the physical modifiers held
/// @return the event
InputEvent key_event(uint16_t scancode, uint32_t key_code, uint8_t modifiers) {
    InputEvent event;
    event.kind = InputKind::key;
    event.down = true;
    event.scancode = scancode;
    event.key_code = key_code;
    event.modifiers = modifiers;
    return event;
}

/// Checks the keycaps against the ones the test names.
///
/// @param chord the chord
/// @param map the keyboard
/// @param platform the platform
/// @param want the keycaps, in order
void expect_caps(
    const Chord& chord, const KeyboardMap& map, Platform platform, const std::vector<KeyCap>& want
) {
    const std::vector<KeyCap> got = keys::keycaps(chord, map, platform);
    OA_CHECK(got.size() == want.size());
    const std::size_t count = got.size() < want.size() ? got.size() : want.size();
    for (std::size_t index = 0; index < count; ++index) {
        if (got[index].text != want[index].text || got[index].word != want[index].word) {
            std::fprintf(
                stderr,
                "keycap %zu: \"%s\" word %d, want \"%s\" word %d\n",
                index,
                got[index].text.c_str(),
                static_cast<int>(got[index].word),
                want[index].text.c_str(),
                static_cast<int>(want[index].word)
            );
        }
        OA_CHECK(got[index].text == want[index].text);
        OA_CHECK(got[index].word == want[index].word);
    }
}

/// Reports whether a keycap text holds a digit.
///
/// @param text the keycap
/// @return true when it holds a digit
bool has_digit(std::string_view text) {
    for (const char letter : text) {
        if (letter >= '0' && letter <= '9')
            return true;
    }
    return false;
}

/// Reports whether a keycap text holds a letter.
///
/// @param text the keycap
/// @return true when it holds a letter
bool has_letter(std::string_view text) {
    for (const char letter : text) {
        if ((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))
            return true;
    }
    return false;
}

/// Checks that no keycap joins an English word and a character.
///
/// An F-key is one label, and it is not a word. Every other keycap is either
/// words or a character.
///
/// @param caps the keycaps
void expect_words_apart(const std::vector<KeyCap>& caps) {
    for (const KeyCap& cap : caps) {
        const bool digit = has_digit(cap.text);
        const bool letter = has_letter(cap.text);
        if (cap.word)
            OA_CHECK(!digit);
        if (digit && letter) {
            OA_CHECK(!cap.word);
            OA_CHECK(cap.text.size() >= 2);
            OA_CHECK(cap.text.front() == 'F');
        }
    }
}

/// Round-trips every default the registry ships.
void registry_defaults() {
    std::size_t count = 0;
    for (const actions::Action& action : actions::actions()) {
        for (const std::string_view text : action.defaults) {
            round_trip(text);
            ++count;
        }
        if (!action.left_click_default.empty()) {
            round_trip(action.left_click_default);
            ++count;
        }
        for (const actions::HackChange& hack : action.hacks) {
            for (const std::string_view text : hack.defaults) {
                round_trip(text);
                ++count;
            }
        }
    }
    OA_CHECK(count > 0);
}

/// Parse and write cases for each trigger form.
void forms() {
    writes("a", "A");
    writes("Shift+Ctrl+a", "Ctrl+Shift+A");
    writes("Alt+Ctrl+A", "Ctrl+Alt+A");
    writes("Meta+Primary+A", "Primary+Meta+A");
    writes("@F1", "F1");
    writes("F1", "F1");
    writes("@KeyA", "@KeyA");
    writes("@Digit1", "@Digit1");
    writes("@ArrowUp", "@ArrowUp");
    writes("Up", "Up");
    writes("Shift++", "Shift++");
    writes("+", "+");
    writes("]", "]");
    writes("Primary+,", "Primary+,");
    writes("Primary+/", "Primary+/");
    writes("NumpadAdd", "NumpadAdd");
    writes("NumpadEnter", "NumpadEnter");
    writes("ContextMenu", "ContextMenu");
    writes("MouseLeft:double", "MouseLeft:double");
    writes("Ctrl+A:hold", "Ctrl+A:hold");
    writes("Alt+Shift+MouseLeft:release", "Alt+Shift+MouseLeft:release");
    writes("Mouse6", "Mouse6");
    writes("Mouse8", "Mouse8");
    writes("WheelUp", "WheelUp");
    writes("WheelLeft", "WheelLeft");
    writes("Alt+WheelDown", "Alt+WheelDown");
    writes("Pad.South", "Pad.South");
    writes("Pad.Button1", "Pad.Button1");
    writes("Shift", "Shift");
    writes("Primary", "Primary");
    writes("Ctrl", "Ctrl");

    const Chord lower = must_parse("b");
    OA_CHECK(lower.kind == TriggerKind::character);
    OA_CHECK(lower.character == U'B');
    const Chord named = must_parse("@F1");
    OA_CHECK(named.kind == TriggerKind::named);
    OA_CHECK(keys::chord_text(named) == "F1");
    const Chord place = must_parse("@KeyA");
    OA_CHECK(place.kind == TriggerKind::place);
    OA_CHECK(place.scancode == 4);
    const Chord plus = must_parse("Shift++");
    OA_CHECK(plus.kind == TriggerKind::character);
    OA_CHECK(plus.character == U'+');
    OA_CHECK((plus.modifiers & keys::shift) != 0);
}

/// Every refusal of the grammar, with the byte it was found at.
void refusals() {
    expect_refuse("", ChordErrorReason::empty, 0);
    expect_refuse(std::string(65, 'A'), ChordErrorReason::too_long, 64);
    expect_refuse(std::string(64, 'A'), ChordErrorReason::not_a_trigger, 0);
    expect_refuse("Ctrl+", ChordErrorReason::empty_part, 5);
    expect_refuse("Ctrl++A", ChordErrorReason::empty_part, 5);
    expect_refuse("+Ctrl", ChordErrorReason::empty_part, 0);
    expect_refuse("Ctrl+Ctrl+A", ChordErrorReason::modifier_repeated, 5);
    expect_refuse("Shift+Alt+Shift+A", ChordErrorReason::modifier_repeated, 10);
    expect_refuse("Nope", ChordErrorReason::not_a_trigger, 0);
    expect_refuse("A:", ChordErrorReason::not_a_trigger, 0);
    expect_refuse("@", ChordErrorReason::unknown_position, 0);
    expect_refuse("@NoKey", ChordErrorReason::unknown_position, 1);
    expect_refuse("@Execute", ChordErrorReason::unknown_position, 1);
    expect_refuse("@SoftLeft", ChordErrorReason::unknown_position, 1);
    expect_refuse("CapsLock", ChordErrorReason::lock_key, 0);
    expect_refuse("@CapsLock", ChordErrorReason::lock_key, 1);
    expect_refuse("NumLock", ChordErrorReason::lock_key, 0);
    expect_refuse("@ScrollLock", ChordErrorReason::lock_key, 1);
    expect_refuse("@ShiftLeft", ChordErrorReason::modifier_key, 1);
    expect_refuse("ShiftLeft", ChordErrorReason::modifier_key, 0);
    expect_refuse("@ControlRight", ChordErrorReason::modifier_key, 1);
    expect_refuse("@AltLeft", ChordErrorReason::modifier_key, 1);
    expect_refuse("@MetaRight", ChordErrorReason::modifier_key, 1);
    expect_refuse("Shift:hold", ChordErrorReason::gesture_not_allowed, 5);
    expect_refuse("WheelUp:double", ChordErrorReason::gesture_not_allowed, 7);
    expect_refuse("Alt+WheelDown:hold", ChordErrorReason::gesture_not_allowed, 13);
    expect_refuse("A:tap", ChordErrorReason::unknown_gesture, 1);
    expect_refuse("Ctrl+A:twice", ChordErrorReason::unknown_gesture, 6);
    expect_refuse("Pad.", ChordErrorReason::bad_pad, 4);
    expect_refuse("Pad.1South", ChordErrorReason::bad_pad, 4);
    expect_refuse("Pad.South-1", ChordErrorReason::bad_pad, 4);

    for (int reason = 0; reason <= static_cast<int>(ChordErrorReason::bad_pad); ++reason) {
        const std::string_view text = keys::chord_error_text(static_cast<ChordErrorReason>(reason));
        if (reason == static_cast<int>(ChordErrorReason::ok)) {
            OA_CHECK(text.empty());
        } else {
            OA_CHECK(!text.empty());
            OA_CHECK(text.back() != '.');
        }
    }
}

/// Matching on the US map, and Primary on each platform.
void matching() {
    const KeyboardMap us = KeyboardMap::us();
    const Platform windows{};
    const Platform mac{true};
    const Chord letter = must_parse("A");
    const Chord ctrl_a = must_parse("Ctrl+A");
    const Chord digit = must_parse("@Digit1");
    const Chord enter = must_parse("Enter");
    const Chord keypad = must_parse("NumpadEnter");
    const Chord primary = must_parse("Primary+,");

    OA_CHECK(match(letter, key_event(4, 0x61, 0), us, windows) == Match::exact);
    OA_CHECK(
        match(letter, key_event(4, static_cast<uint32_t>('q'), 0), us, windows) == Match::none
    );
    OA_CHECK(
        match(letter, key_event(4, 0x61, keys::shift), us, windows) == Match::with_extra_modifiers
    );
    OA_CHECK(
        match(letter, key_event(4, 0x61, keys::alt), us, windows) == Match::with_extra_modifiers
    );
    OA_CHECK(
        match(letter, key_event(4, 0x61, keys::meta), us, windows) == Match::with_extra_modifiers
    );
    OA_CHECK(match(letter, key_event(4, 0x61, keys::ctrl), us, windows) == Match::none);
    OA_CHECK(
        match(
            letter, key_event(4, 0x61, static_cast<uint8_t>(keys::ctrl | keys::shift)), us, windows
        ) == Match::none
    );

    OA_CHECK(match(ctrl_a, key_event(4, 0x61, keys::ctrl), us, windows) == Match::exact);
    OA_CHECK(
        match(
            ctrl_a, key_event(4, 0x61, static_cast<uint8_t>(keys::ctrl | keys::shift)), us, windows
        ) == Match::with_extra_modifiers
    );
    OA_CHECK(
        match(
            ctrl_a, key_event(4, 0x61, static_cast<uint8_t>(keys::ctrl | keys::alt)), us, windows
        ) == Match::with_extra_modifiers
    );
    OA_CHECK(match(ctrl_a, key_event(4, 0x61, keys::shift), us, windows) == Match::none);

    OA_CHECK(
        match(digit, key_event(30, static_cast<uint32_t>('1'), 0), us, windows) == Match::exact
    );
    OA_CHECK(
        match(digit, key_event(20, static_cast<uint32_t>('1'), 0), us, windows) == Match::none
    );
    OA_CHECK(match(digit, key_event(30, 0x61, 0), us, windows) == Match::exact);

    OA_CHECK(match(enter, key_event(88, 0x0D, 0), us, windows) == Match::exact);
    OA_CHECK(match(keypad, key_event(88, 0x0D, 0), us, windows) == Match::exact);
    OA_CHECK(match(enter, key_event(40, 0x0D, 0), us, windows) == Match::exact);
    OA_CHECK(match(keypad, key_event(40, 0x0D, 0), us, windows) == Match::none);
    const uint32_t keypad_code = uint32_t{88} | keys::position_key_code_bit;
    OA_CHECK(match(enter, key_event(88, keypad_code, 0), us, windows) == Match::none);
    OA_CHECK(match(keypad, key_event(88, keypad_code, 0), us, windows) == Match::exact);
    OA_CHECK(keys::rank(keypad) == keys::rank(enter) + 1);

    const Chord arrow = must_parse("@ArrowUp");
    const Chord up = must_parse("Up");
    const uint32_t up_code = uint32_t{82} | keys::position_key_code_bit;
    OA_CHECK(match(arrow, key_event(82, 0, 0), us, windows) == Match::exact);
    OA_CHECK(match(up, key_event(82, 0, 0), us, windows) == Match::none);
    OA_CHECK(match(up, key_event(0, up_code, 0), us, windows) == Match::exact);
    OA_CHECK(match(arrow, key_event(0, up_code, 0), us, windows) == Match::none);

    OA_CHECK(
        match(primary, key_event(54, static_cast<uint32_t>(','), keys::meta), us, mac) ==
        Match::exact
    );
    OA_CHECK(
        match(primary, key_event(54, static_cast<uint32_t>(','), keys::ctrl), us, mac) ==
        Match::none
    );
    OA_CHECK(
        match(
            primary,
            key_event(
                54, static_cast<uint32_t>(','), static_cast<uint8_t>(keys::meta | keys::ctrl)
            ),
            us,
            mac
        ) == Match::none
    );
    OA_CHECK(
        match(
            primary,
            key_event(
                54, static_cast<uint32_t>(','), static_cast<uint8_t>(keys::meta | keys::shift)
            ),
            us,
            mac
        ) == Match::with_extra_modifiers
    );
    OA_CHECK(
        match(primary, key_event(54, static_cast<uint32_t>(','), keys::ctrl), us, windows) ==
        Match::exact
    );
    OA_CHECK(
        match(primary, key_event(54, static_cast<uint32_t>(','), keys::meta), us, windows) ==
        Match::none
    );
    OA_CHECK(
        match(
            primary,
            key_event(54, static_cast<uint32_t>(','), static_cast<uint8_t>(keys::ctrl | keys::alt)),
            us,
            windows
        ) == Match::with_extra_modifiers
    );

    InputEvent wheel;
    wheel.kind = InputKind::wheel;
    wheel.wheel_y = 1;
    OA_CHECK(keys::match(must_parse("WheelUp"), wheel, us, windows) == Match::exact);
    wheel.wheel_y = -1;
    OA_CHECK(keys::match(must_parse("WheelUp"), wheel, us, windows) == Match::none);
    OA_CHECK(keys::match(must_parse("WheelDown"), wheel, us, windows) == Match::exact);
    wheel.wheel_y = 0;
    wheel.wheel_x = -1;
    OA_CHECK(keys::match(must_parse("WheelLeft"), wheel, us, windows) == Match::exact);

    InputEvent button;
    button.kind = InputKind::button;
    button.button = 1;
    OA_CHECK(keys::match(must_parse("MouseLeft"), button, us, windows) == Match::exact);
    button.button = 6;
    OA_CHECK(keys::match(must_parse("Mouse6"), button, us, windows) == Match::exact);
    OA_CHECK(keys::match(must_parse("Pad.South"), button, us, windows) == Match::none);
    OA_CHECK(
        keys::match(must_parse("Shift"), key_event(4, 0x61, keys::shift), us, windows) ==
        Match::none
    );

    InputEvent repeated = key_event(4, 0x61, 0);
    repeated.repeat = true;
    repeated.down = false;
    OA_CHECK(keys::match(letter, repeated, us, windows) == Match::exact);

    OA_CHECK(keys::rank(letter) == 0);
    OA_CHECK(keys::rank(ctrl_a) == 2);
    OA_CHECK(keys::rank(must_parse("Ctrl+Shift+A")) == 4);
    OA_CHECK(keys::rank(digit) == 1);
    OA_CHECK(keys::rank(must_parse("Ctrl+@Digit1")) == 3);
    OA_CHECK(keys::rank(must_parse("Ctrl+Shift+@Digit1")) == 5);

    OA_CHECK(keys::held(must_parse("Shift"), keys::shift, windows));
    OA_CHECK(!keys::held(must_parse("Shift"), 0, windows));
    OA_CHECK(keys::held(must_parse("Primary"), keys::meta, mac));
    OA_CHECK(!keys::held(must_parse("Primary"), keys::ctrl, mac));
    OA_CHECK(keys::held(must_parse("Primary"), keys::ctrl, windows));
    OA_CHECK(!keys::held(letter, keys::shift, windows));
}

/// An AZERTY map: the number row's events carry digits, and A sits on the Q key.
void azerty() {
    KeyboardMap map = KeyboardMap::us();
    map.set(20, static_cast<uint32_t>('a'), U'a');
    map.set(4, static_cast<uint32_t>('q'), U'q');
    map.set(30, static_cast<uint32_t>('1'), U'&');
    const Platform windows{};
    const Chord letter = must_parse("A");
    const Chord place = must_parse("@KeyA");
    const Chord digit = must_parse("@Digit1");

    OA_CHECK(
        keys::match(letter, key_event(20, static_cast<uint32_t>('a'), 0), map, windows) ==
        Match::exact
    );
    OA_CHECK(
        keys::match(letter, key_event(4, static_cast<uint32_t>('q'), 0), map, windows) ==
        Match::none
    );
    OA_CHECK(
        keys::match(place, key_event(4, static_cast<uint32_t>('q'), 0), map, windows) ==
        Match::exact
    );
    OA_CHECK(
        keys::match(place, key_event(20, static_cast<uint32_t>('a'), 0), map, windows) ==
        Match::none
    );

    const std::vector<KeyCap> caps = keys::keycaps(digit, map, windows);
    OA_CHECK(caps.size() == 1);
    OA_CHECK(caps[0].text == "&");
    OA_CHECK(!caps[0].word);

    const KeyboardMap us = KeyboardMap::us();
    OA_CHECK(keys::on_keyboard(must_parse("`"), us));
    KeyboardMap without_grave = us;
    without_grave.set(53, 0, 0);
    OA_CHECK(!keys::on_keyboard(must_parse("`"), without_grave));
    OA_CHECK(keys::on_keyboard(must_parse("Enter"), without_grave));
    OA_CHECK(keys::on_keyboard(digit, without_grave));
    OA_CHECK(keys::on_keyboard(letter, map));
}

/// Keycaps for each form, and the rule that a word and a character stay apart.
void names() {
    const KeyboardMap us = KeyboardMap::us();
    const Platform windows{};
    const Platform mac{true};

    const Chord held_a = must_parse("Ctrl+A:hold");
    expect_caps(
        held_a, us, windows, {KeyCap{"Ctrl", true}, KeyCap{"A", false}, KeyCap{"hold", true}}
    );
    OA_CHECK(keys::display_text(held_a, us, windows) == "Ctrl+A hold");

    expect_caps(must_parse("Primary+A"), us, mac, {KeyCap{"Cmd", true}, KeyCap{"A", false}});
    expect_caps(must_parse("Primary+A"), us, windows, {KeyCap{"Ctrl", true}, KeyCap{"A", false}});
    expect_caps(
        must_parse("Primary+Ctrl+A"), us, windows, {KeyCap{"Ctrl", true}, KeyCap{"A", false}}
    );
    expect_caps(must_parse("Primary+Meta+A"), us, mac, {KeyCap{"Cmd", true}, KeyCap{"A", false}});
    expect_caps(must_parse("Meta+A"), us, windows, {KeyCap{"Win", true}, KeyCap{"A", false}});
    expect_caps(must_parse("Meta+A"), us, mac, {KeyCap{"Cmd", true}, KeyCap{"A", false}});
    OA_CHECK(keys::display_text(must_parse("Primary+Ctrl+A"), us, windows) == "Ctrl+A");

    expect_caps(must_parse("@Digit1"), us, windows, {KeyCap{"1", false}});
    expect_caps(must_parse("PageUp"), us, windows, {KeyCap{"Page up", true}});
    expect_caps(must_parse("F1"), us, windows, {KeyCap{"F1", false}});
    expect_caps(must_parse("Numpad1"), us, windows, {KeyCap{"Numpad", true}, KeyCap{"1", false}});
    expect_caps(must_parse("NumpadAdd"), us, windows, {KeyCap{"Numpad add", true}});
    expect_caps(must_parse("PrintScreen"), us, windows, {KeyCap{"Print screen", true}});
    expect_caps(must_parse("MouseLeft"), us, windows, {KeyCap{"Left button", true}});
    expect_caps(must_parse("MouseBack"), us, windows, {KeyCap{"Back button", true}});
    expect_caps(must_parse("Mouse6"), us, windows, {KeyCap{"Button", true}, KeyCap{"6", false}});
    expect_caps(must_parse("WheelUp"), us, windows, {KeyCap{"Wheel up", true}});
    expect_caps(must_parse("WheelLeft"), us, windows, {KeyCap{"Wheel left", true}});
    expect_caps(must_parse("Pad.South"), us, windows, {KeyCap{"Pad", true}, KeyCap{"South", true}});
    expect_caps(must_parse("Pad.A"), us, windows, {KeyCap{"Pad", true}, KeyCap{"A", false}});
    expect_caps(
        must_parse("Pad.Button1"),
        us,
        windows,
        {KeyCap{"Pad", true}, KeyCap{"Button", true}, KeyCap{"1", false}}
    );
    OA_CHECK(keys::display_text(must_parse("Mouse6"), us, windows) == "Button+6");
    OA_CHECK(keys::display_text(must_parse("Numpad1"), us, windows) == "Numpad+1");
    OA_CHECK(
        keys::display_text(must_parse("MouseLeft:double"), us, windows) == "Left button double"
    );
    OA_CHECK(keys::display_text(must_parse("Shift"), us, windows) == "Shift");

    KeyboardMap unnamed;
    expect_caps(must_parse("@ArrowUp"), unnamed, windows, {KeyCap{"Arrow up", true}});

    for (const keys::KeyPosition& position : keys::key_positions()) {
        Chord chord;
        chord.kind = TriggerKind::place;
        chord.scancode = position.scancode;
        expect_words_apart(keys::keycaps(chord, us, windows));
        if (us.printed(position.scancode) == 0)
            expect_words_apart(keys::keycaps(chord, unnamed, windows));
    }
    for (const keys::NamedKeySpec& key : keys::named_keys()) {
        Chord chord;
        chord.kind = TriggerKind::named;
        chord.named = static_cast<uint8_t>(key.id);
        chord.scancode = key.scancode;
        expect_words_apart(keys::keycaps(chord, us, windows));
    }
    for (uint8_t button = 1; button <= 8; ++button) {
        Chord chord;
        chord.kind = TriggerKind::mouse;
        chord.button = button;
        expect_words_apart(keys::keycaps(chord, us, windows));
    }
    for (const int8_t wheel : {int8_t{1}, int8_t{-1}, int8_t{2}, int8_t{-2}}) {
        Chord chord;
        chord.kind = TriggerKind::wheel;
        chord.wheel = wheel;
        expect_words_apart(keys::keycaps(chord, us, windows));
    }
}

} // namespace

int main() {
    registry_defaults();
    forms();
    refusals();
    matching();
    azerty();
    names();
    return oa::test::check_exit_status();
}
