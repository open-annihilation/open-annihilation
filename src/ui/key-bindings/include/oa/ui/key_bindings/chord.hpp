// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A chord: the text a layout writes, the event it matches, and the English
// keycaps a screen shows for it. Matching does not time a gesture.
#pragma once

#include "oa/ui/key_bindings/input.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

/// A modifier a chord can name. Values are bits and combine with `|`.
enum Modifier : uint8_t {
    primary = 1u << 0, ///< the platform's primary modifier, Ctrl or the command key
    ctrl = 1u << 1,    ///< the Control key
    alt = 1u << 2,     ///< the Alt key
    shift = 1u << 3,   ///< the Shift key
    meta = 1u << 4,    ///< the platform's other modifier, command or the Windows key
};

/// What a chord waits for, besides its modifiers.
enum class TriggerKind : uint8_t {
    character,      ///< a letter or another printing character
    place,          ///< one key position, written with an at sign
    named,          ///< a named key, such as Enter or F1
    mouse,          ///< a mouse button
    wheel,          ///< one wheel turn
    pad,            ///< a pad button; matching never accepts it
    modifier_alone, ///< a modifier with no other key
};

/// How the player finishes the chord. A plain press is the usual one.
enum class Gesture : uint8_t {
    press,        ///< the chord goes down
    double_press, ///< two presses, written `:double`
    hold,         ///< the chord is held, written `:hold`
    release,      ///< the chord is released, written `:release`
};

/// One binding: modifiers, one trigger and a gesture.
struct Chord {
    uint8_t modifiers{};  ///< bits of `Modifier`, in any combination
    TriggerKind kind{};   ///< what the trigger is
    char32_t character{}; ///< the character, in capitals for a letter
    uint16_t scancode{};  ///< the position, for a place or a named key
    uint8_t named{};      ///< the `NamedKey` value, for a named key
    uint8_t button{};     ///< the mouse button, 1 to 8
    int8_t wheel{};       ///< 1 up, -1 down, 2 right, -2 left
    std::string pad{};    ///< the pad button's name, without the `Pad.` prefix
    Gesture gesture{};    ///< how the chord finishes; a plain press by default

    /// Returns whether two chords name the same binding.
    ///
    /// @return true when every field is equal
    friend bool operator==(const Chord&, const Chord&) = default;
};

/// Why a chord text was refused, and where in the text.
enum class ChordErrorReason : uint8_t {
    ok,                  ///< the text parsed
    empty,               ///< the text is empty
    too_long,            ///< the text is longer than 64 bytes
    empty_part,          ///< a `+` has nothing on one side
    modifier_repeated,   ///< the same modifier is named twice
    not_a_trigger,       ///< the last part is not a trigger
    unknown_position,    ///< an at sign names no key in the position table
    lock_key,            ///< CapsLock, NumLock or ScrollLock
    modifier_key,        ///< a modifier key's own position, such as ShiftLeft
    gesture_not_allowed, ///< a gesture on a wheel turn or a modifier alone
    unknown_gesture,     ///< a `:` suffix that is not a gesture
    bad_pad,             ///< `Pad.` is missing a name, or the name is not one
};

/// A refusal from `parse_chord`, with the byte it was found at.
struct ChordError {
    ChordErrorReason reason{}; ///< why the text was refused; `ok` when it parsed
    uint32_t byte{};           ///< the byte in the text where the reason was found
};

/// The longest chord text, in bytes.
inline constexpr std::size_t chord_text_limit = 64;

/// How well a chord matches one event.
enum class Match : uint8_t {
    none,                 ///< the event is not this chord
    with_extra_modifiers, ///< the trigger matches, with Shift, Alt or Meta held beyond the chord
    exact,                ///< the trigger and the modifiers match
};

/// One piece of a chord as a screen shows it. A word and a character stay apart.
struct KeyCap {
    std::string text{}; ///< the letters or the character on the keycap
    bool word{};        ///< an English word, rather than a character or an F-key
};

/// Reads a chord.
///
/// Modifiers may arrive in any order, each once. A lower-case letter is read
/// as its capital. An at sign and a code names that key's place; an at sign
/// and a named key (at F1) is that named key. A chord is at most 64 bytes. A
/// modifier alone takes no gesture, and neither does a wheel turn.
///
/// @param text the chord, as a layout writes it
/// @param[out] error the reason and the byte; `ok` at byte 0 when the text parses
/// @return the chord, or nothing when the text is refused
[[nodiscard]] std::optional<Chord> parse_chord(std::string_view text, ChordError& error);

/// Returns the English for a refusal. An accepted chord has an empty text.
///
/// @param reason the reason
/// @return the sentence, with no full stop
[[nodiscard]] std::string_view chord_error_text(ChordErrorReason reason) noexcept;

/// Writes a chord in the one canonical text.
///
/// Modifiers come out as Primary, Ctrl, Alt, Shift, then Meta. A letter is a
/// capital. A named key is its name, never with an at sign. A place is an at
/// sign and its code. A gesture other than a plain press follows a colon.
///
/// @param chord the chord
/// @return the text
[[nodiscard]] std::string chord_text(const Chord& chord);

/// Says how well a chord matches one event on a platform.
///
/// A character matches the event's key code (`A` is the code of `a`). A place
/// matches the position number. A named key matches its key code, except
/// NumpadEnter, which matches its position: a keypad Enter whose key code was
/// turned into Return still matches Enter, and NumpadEnter only matches the
/// keypad key. A button matches its number. A wheel turn matches the sign of
/// the event's turn. A pad chord matches nothing.
///
/// Ctrl is held exactly when the chord names it, counting Primary where Primary
/// is Ctrl. Each other modifier the chord names must be held. Shift, Alt or
/// Meta held beyond the chord still matches, as `with_extra_modifiers`. The
/// gesture is not judged.
///
/// @param chord the chord
/// @param event the event
/// @param map the keyboard; matching a key does not read it
/// @param platform which key Primary means
/// @return how well the event matches
[[nodiscard]] Match
match(const Chord& chord, const InputEvent& event, const KeyboardMap& map, Platform platform);

/// Orders the chords that match one event. A higher rank wins.
///
/// The rank is twice the number of modifiers the chord names, plus one when
/// the chord matches by position (a place, or NumpadEnter).
///
/// @param chord the chord
/// @return the rank
[[nodiscard]] int rank(const Chord& chord);

/// Reports whether a modifier-alone chord is held.
///
/// @param modifier_alone a chord whose trigger is a modifier alone
/// @param modifiers the physical modifiers held, as an event records them
/// @param platform which key Primary means
/// @return true when that modifier is among those held
[[nodiscard]] bool held(const Chord& modifier_alone, uint8_t modifiers, Platform platform);

/// Splits a chord into the keycaps a screen draws, in chord order.
///
/// Modifiers are Ctrl, Alt, Shift, and Cmd or Win. Primary is Cmd where it is
/// the command key and Ctrl elsewhere. A character is a capital. A place is
/// the character its key prints, in capitals, or the code's English name when
/// it prints none. A word and a character are separate keycaps. The gesture,
/// other than a plain press, is one more word.
///
/// @param chord the chord
/// @param map what the keys print
/// @param platform which key Primary means, and the name of Meta
/// @return the keycaps, modifiers first and the gesture last
[[nodiscard]] std::vector<KeyCap>
keycaps(const Chord& chord, const KeyboardMap& map, Platform platform);

/// Joins the keycaps with `+` and puts the gesture after a space.
///
/// @param chord the chord
/// @param map what the keys print
/// @param platform which key Primary means
/// @return the text a screen shows, such as `Ctrl+A hold`
[[nodiscard]] std::string
display_text(const Chord& chord, const KeyboardMap& map, Platform platform);

/// Reports whether some key's events carry a character chord's character.
///
/// A chord that is not a character is always on the keyboard.
///
/// @param chord the chord
/// @param map what events carry
/// @return false when the chord is a character no key's events carry
[[nodiscard]] bool on_keyboard(const Chord& chord, const KeyboardMap& map);

} // namespace oa::ui::key_bindings
