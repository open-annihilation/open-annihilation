// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A chord's keycaps. A word and a character stay on separate keycaps, so
// the words show in the interface's language and the characters as printed.

#include "oa/ui/key_bindings/chord.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

/// Returns a keycap's word as the interface's language shows it. Each word
/// is listed in interface_words.hpp. The Home key is looked up as "Home key",
/// because the catalogue's Home is a folder's place; its English stays Home.
///
/// @param english the word in English
/// @return the word as shown
std::string shown_word(std::string_view english) {
    if (english == "Home") {
        constexpr std::string_view home_key = "Home key";
        const std::string_view shown = oa::data::languages::interface_text(home_key);
        return std::string(shown == home_key ? english : shown);
    }
    return std::string(oa::data::languages::interface_text(english));
}

/// Appends a code point as UTF-8.
///
/// @param out the text
/// @param character the code point
void append_utf8(std::string& out, char32_t character) {
    if (character < 0x80) {
        out.push_back(static_cast<char>(character));
        return;
    }
    if (character < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (character >> 6)));
        out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        return;
    }
    if (character < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (character >> 12)));
        out.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        return;
    }
    out.push_back(static_cast<char>(0xF0 | (character >> 18)));
    out.push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
}

/// Returns one character as a keycap, with a letter in capitals.
///
/// @param character the character
/// @return the keycap
KeyCap character_cap(char32_t character) {
    if (character >= U'a' && character <= U'z')
        character = static_cast<char32_t>(character - U'a' + U'A');
    std::string text;
    append_utf8(text, character);
    return KeyCap{std::move(text), false};
}

/// Adds a modifier keycap unless that label is already there.
///
/// Primary and Ctrl share a label where Primary is Ctrl, and Primary and
/// Meta share one where Primary is the command key.
///
/// @param caps the keycaps so far
/// @param text the label, in English
void add_modifier(std::vector<KeyCap>& caps, const char* text) {
    std::string shown = shown_word(text);
    for (const KeyCap& cap : caps) {
        if (cap.text == shown)
            return;
    }
    caps.push_back(KeyCap{std::move(shown), true});
}

/// Adds the modifier keycaps, in chord order.
///
/// @param caps the keycaps
/// @param modifiers the chord's modifier bits
/// @param platform which key Primary means, and the name of Meta
void add_modifiers(std::vector<KeyCap>& caps, uint8_t modifiers, Platform platform) {
    if ((modifiers & primary) != 0)
        add_modifier(caps, platform.primary_is_meta ? "Cmd" : "Ctrl");
    if ((modifiers & ctrl) != 0)
        add_modifier(caps, "Ctrl");
    if ((modifiers & alt) != 0)
        add_modifier(caps, "Alt");
    if ((modifiers & shift) != 0)
        add_modifier(caps, "Shift");
    if ((modifiers & meta) != 0)
        add_modifier(caps, platform.primary_is_meta ? "Cmd" : "Win");
}

/// Reports whether a code is an F-key label, such as `F1`.
///
/// @param code the code
/// @return true for F and then digits
bool function_label(std::string_view code) {
    if (code.size() < 2 || code.front() != 'F')
        return false;
    for (std::size_t index = 1; index < code.size(); ++index) {
        if (code[index] < '0' || code[index] > '9')
            return false;
    }
    return true;
}

/// One piece of a code: a word, or a run of digits.
struct Piece {
    std::string text{};
    bool word{};
};

/// Splits a code into words and digit runs.
///
/// @param code the UI Events code or a named key
/// @return the pieces, in order
std::vector<Piece> pieces_of(std::string_view code) {
    std::vector<Piece> pieces;
    std::string word;
    std::string digits;
    auto flush_word = [&] {
        if (word.empty())
            return;
        pieces.push_back(Piece{std::move(word), true});
        word.clear();
    };
    auto flush_digits = [&] {
        if (digits.empty())
            return;
        pieces.push_back(Piece{std::move(digits), false});
        digits.clear();
    };
    for (const char letter : code) {
        if (letter >= 'A' && letter <= 'Z') {
            flush_digits();
            flush_word();
            word.push_back(letter);
        } else if (letter >= 'a' && letter <= 'z') {
            flush_digits();
            word.push_back(letter);
        } else if (letter >= '0' && letter <= '9') {
            flush_word();
            digits.push_back(letter);
        }
    }
    flush_word();
    flush_digits();
    return pieces;
}

/// Writes a word with its first letter kept and the rest in lower case.
///
/// @param word the word, as the code spells it
/// @param first whether this is the first word of the name
/// @return the word as a keycap shows it
std::string word_text(std::string_view word, bool first) {
    std::string text;
    text.reserve(word.size());
    for (std::size_t index = 0; index < word.size(); ++index) {
        const char letter = word[index];
        if (!first && letter >= 'A' && letter <= 'Z')
            text.push_back(static_cast<char>(letter - 'A' + 'a'));
        else
            text.push_back(letter);
    }
    return text;
}

/// Adds the English name of a code. An F-key stays one label. A name of
/// words only is one keycap. A digit is its own keycap.
///
/// @param caps the keycaps
/// @param code the code
void add_code_name(std::vector<KeyCap>& caps, std::string_view code) {
    if (function_label(code)) {
        caps.push_back(KeyCap{std::string{code}, false});
        return;
    }
    const std::vector<Piece> pieces = pieces_of(code);
    bool digits = false;
    for (const Piece& piece : pieces) {
        if (!piece.word)
            digits = true;
    }
    if (!digits) {
        std::string phrase;
        for (std::size_t index = 0; index < pieces.size(); ++index) {
            if (index != 0)
                phrase.push_back(' ');
            phrase.append(word_text(pieces[index].text, index == 0));
        }
        if (!phrase.empty())
            caps.push_back(KeyCap{shown_word(phrase), true});
        return;
    }
    bool first = true;
    for (const Piece& piece : pieces) {
        if (piece.word) {
            caps.push_back(KeyCap{shown_word(word_text(piece.text, first)), true});
            first = false;
        } else {
            caps.push_back(KeyCap{piece.text, false});
        }
    }
}

/// Adds a pad button's name, with each run of digits on its own keycap.
///
/// @param caps the keycaps
/// @param name the name after `Pad.`
void add_pad_name(std::vector<KeyCap>& caps, std::string_view name) {
    caps.push_back(KeyCap{shown_word("Pad"), true});
    std::string letters;
    std::string digits;
    auto flush_letters = [&] {
        if (letters.empty())
            return;
        const bool one_letter = letters.size() == 1;
        caps.push_back(KeyCap{std::move(letters), !one_letter});
        letters.clear();
    };
    auto flush_digits = [&] {
        if (digits.empty())
            return;
        caps.push_back(KeyCap{std::move(digits), false});
        digits.clear();
    };
    for (const char letter : name) {
        if (letter >= '0' && letter <= '9') {
            flush_letters();
            digits.push_back(letter);
        } else {
            flush_digits();
            letters.push_back(letter);
        }
    }
    flush_letters();
    flush_digits();
}

/// Adds the trigger's keycaps.
///
/// @param caps the keycaps
/// @param chord the chord
/// @param map what the keys print
void add_trigger(std::vector<KeyCap>& caps, const Chord& chord, const KeyboardMap& map) {
    switch (chord.kind) {
    case TriggerKind::character:
        if (chord.character != 0)
            caps.push_back(character_cap(chord.character));
        break;
    case TriggerKind::place: {
        const char32_t printed = map.printed(chord.scancode);
        if (printed != 0)
            caps.push_back(character_cap(printed));
        else
            add_code_name(caps, code_of(chord.scancode));
        break;
    }
    case TriggerKind::named:
        if (chord.named < named_keys().size())
            add_code_name(caps, named_keys()[chord.named].name);
        break;
    case TriggerKind::mouse:
        // A chord numbers the buttons as it spells them: MouseLeft, MouseRight,
        // MouseMiddle.
        if (chord.button == 1)
            caps.push_back(KeyCap{shown_word("Left button"), true});
        else if (chord.button == 2)
            caps.push_back(KeyCap{shown_word("Right button"), true});
        else if (chord.button == 3)
            caps.push_back(KeyCap{shown_word("Middle button"), true});
        else if (chord.button == 4)
            caps.push_back(KeyCap{shown_word("Back button"), true});
        else if (chord.button == 5)
            caps.push_back(KeyCap{shown_word("Forward button"), true});
        else if (chord.button >= 6 && chord.button <= 8) {
            caps.push_back(KeyCap{shown_word("Button"), true});
            caps.push_back(KeyCap{std::string(1, static_cast<char>('0' + chord.button)), false});
        }
        break;
    case TriggerKind::wheel:
        if (chord.wheel == 1)
            caps.push_back(KeyCap{shown_word("Wheel up"), true});
        else if (chord.wheel == -1)
            caps.push_back(KeyCap{shown_word("Wheel down"), true});
        else if (chord.wheel == 2)
            caps.push_back(KeyCap{shown_word("Wheel right"), true});
        else if (chord.wheel == -2)
            caps.push_back(KeyCap{shown_word("Wheel left"), true});
        break;
    case TriggerKind::pad:
        add_pad_name(caps, chord.pad);
        break;
    case TriggerKind::modifier_alone:
        break;
    }
}

/// Adds the gesture word, other than a plain press.
///
/// @param caps the keycaps
/// @param gesture the gesture
void add_gesture(std::vector<KeyCap>& caps, Gesture gesture) {
    if (gesture == Gesture::double_press)
        caps.push_back(KeyCap{shown_word("double"), true});
    else if (gesture == Gesture::hold)
        caps.push_back(KeyCap{shown_word("hold"), true});
    else if (gesture == Gesture::release)
        caps.push_back(KeyCap{shown_word("release"), true});
}

} // namespace

std::vector<KeyCap> keycaps(const Chord& chord, const KeyboardMap& map, Platform platform) {
    std::vector<KeyCap> caps;
    add_modifiers(caps, chord.modifiers, platform);
    add_trigger(caps, chord, map);
    add_gesture(caps, chord.gesture);
    return caps;
}

std::string display_text(const Chord& chord, const KeyboardMap& map, Platform platform) {
    const std::vector<KeyCap> caps = keycaps(chord, map, platform);
    const bool gestured = chord.gesture != Gesture::press;
    const std::size_t body = gestured && !caps.empty() ? caps.size() - 1 : caps.size();
    std::string text;
    for (std::size_t index = 0; index < body; ++index) {
        if (!text.empty())
            text.push_back('+');
        text.append(caps[index].text);
    }
    if (gestured && caps.size() == body + 1) {
        if (!text.empty())
            text.push_back(' ');
        text.append(caps.back().text);
    }
    return text;
}

bool on_keyboard(const Chord& chord, const KeyboardMap& map) {
    if (chord.kind != TriggerKind::character)
        return true;
    char32_t character = chord.character;
    if (character >= U'A' && character <= U'Z')
        character = static_cast<char32_t>(character - U'A' + U'a');
    if (character == 0)
        return false;
    for (uint16_t scancode = 0; scancode < scancode_limit; ++scancode) {
        if (map.event_key_code(scancode) == static_cast<uint32_t>(character))
            return true;
    }
    return false;
}

} // namespace oa::ui::key_bindings
