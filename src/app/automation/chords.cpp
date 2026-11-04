// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chords and action ids as key events (include/oa/app/automation/chords.hpp).
#include "oa/app/automation/chords.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace oa::app::automation {
namespace {

namespace keys = oa::ui::key_bindings;

/// How many times a chord may be pressed in one call.
constexpr int kMaxRepeat = 30;

/// The platform this build presses Primary as. The command key on Apple,
/// Control everywhere else, which is how the game shows the bindings.
keys::Platform machine_platform() noexcept {
    keys::Platform machine;
#if defined(__APPLE__)
    machine.primary_is_meta = true;
#else
    machine.primary_is_meta = false;
#endif
    machine.modifier_symbols = false;
    return machine;
}

/// Compares two texts ignoring ASCII case.
///
/// @param left one text
/// @param right the other
/// @return true when they are the same aside from ASCII case
bool same_ascii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        unsigned char a = static_cast<unsigned char>(left[at]);
        unsigned char b = static_cast<unsigned char>(right[at]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/// The chord spelling of one piece, after a place prefix and a lower-case
/// name have been read.
///
/// @param piece one piece, between plus signs
/// @return the spelling parse_chord accepts
std::string canonical_piece(std::string_view piece) {
    if (piece.size() >= 4 && piece[0] == '@' && piece[1] == 'K' && piece[2] == 'e' &&
        piece[3] == 'y')
        piece.remove_prefix(4);
    if (piece.size() == 1)
        return std::string(piece);
    if (same_ascii(piece, "control"))
        return "Ctrl";
    if (same_ascii(piece, "cmd") || same_ascii(piece, "command") || same_ascii(piece, "win"))
        return "Meta";
    if (same_ascii(piece, "return"))
        return "Enter";
    if (same_ascii(piece, "esc"))
        return "Escape";
    if (same_ascii(piece, "pageup") || same_ascii(piece, "page_up"))
        return "PageUp";
    if (same_ascii(piece, "pagedown") || same_ascii(piece, "page_down"))
        return "PageDown";
    if (same_ascii(piece, "del"))
        return "Delete";
    for (const std::string_view modifier : {"Primary", "Ctrl", "Alt", "Shift", "Meta"})
        if (same_ascii(piece, modifier))
            return std::string(modifier);
    for (const keys::NamedKeySpec& named : keys::named_keys())
        if (same_ascii(piece, named.name))
            return std::string(named.name);
    if (!piece.empty() && piece.front() == '@') {
        const std::string_view code = piece.substr(1);
        for (const keys::KeyPosition& position : keys::key_positions())
            if (same_ascii(code, position.code))
                return std::string("@") + std::string(position.code);
    }
    return std::string(piece);
}

/// Joins a chord's pieces in the spelling parse_chord accepts.
///
/// @param text the chord, as it was given
/// @return the spelling
std::string normalize_chord(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    for (std::size_t at = 0; at <= text.size(); ++at) {
        if (at != text.size() && text[at] != '+')
            continue;
        if (!out.empty())
            out.push_back('+');
        out.append(canonical_piece(text.substr(start, at - start)));
        start = at + 1;
    }
    return out;
}

/// Appends one code point as UTF-8.
///
/// @param[out] out the text
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

/// The position a character is typed from, on the US layout.
///
/// @param character the character, a letter in either case
/// @param[out] slot the key's position
/// @return false when no key prints it
bool slot_for_character(char32_t character, uint16_t& slot) {
    const keys::KeyboardMap typed = keys::KeyboardMap::us();
    const char32_t folded =
        character >= U'A' && character <= U'Z' ? character - U'A' + U'a' : character;
    for (uint16_t candidate = 0; candidate < keys::scancode_limit; ++candidate) {
        const char32_t printed = typed.printed(candidate);
        if (printed == 0)
            continue;
        if (printed == character || printed == folded)
            slot = candidate;
        else if (printed >= U'a' && printed <= U'z' && printed - U'a' + U'A' == character)
            slot = candidate;
        else
            continue;
        return true;
    }
    return false;
}

/// One key of a chord: its position, and the characters a tap of it types.
struct ChordKey {
    uint32_t scancode{}; ///< the key's SDL scancode
    bool types{};        ///< a tap carries typed characters
    std::string typed;   ///< those characters
};

/// The keys a parsed chord presses, modifiers first.
///
/// @param chord the chord, parsed
/// @param[out] pressed the keys
/// @param[out] failure why it is not a key
/// @return false when the chord is not a key
bool chord_keys(const keys::Chord& chord, std::vector<ChordKey>& pressed, std::string& failure) {
    const keys::Chord physical = keys::pressed_chord(chord, machine_platform());
    if (physical.kind == keys::TriggerKind::mouse || physical.kind == keys::TriggerKind::wheel ||
        physical.kind == keys::TriggerKind::pad || physical.gesture != keys::Gesture::press) {
        failure = "the binding is not a key";
        return false;
    }
    const bool shift = (physical.modifiers & keys::Modifier::shift) != 0;
    const bool other = (physical.modifiers &
                        (keys::Modifier::ctrl | keys::Modifier::alt | keys::Modifier::meta)) != 0;
    const auto add_modifier = [&pressed](SDL_Scancode scancode) {
        ChordKey key;
        key.scancode = static_cast<uint32_t>(scancode);
        pressed.push_back(std::move(key));
    };
    if ((physical.modifiers & keys::Modifier::ctrl) != 0)
        add_modifier(SDL_SCANCODE_LCTRL);
    if ((physical.modifiers & keys::Modifier::shift) != 0)
        add_modifier(SDL_SCANCODE_LSHIFT);
    if ((physical.modifiers & keys::Modifier::alt) != 0)
        add_modifier(SDL_SCANCODE_LALT);
    if ((physical.modifiers & keys::Modifier::meta) != 0)
        add_modifier(SDL_SCANCODE_LGUI);
    if (physical.kind == keys::TriggerKind::modifier_alone) {
        if (pressed.empty()) {
            failure = "the binding is not a key";
            return false;
        }
        return true;
    }
    ChordKey trigger;
    if (physical.kind == keys::TriggerKind::character) {
        uint16_t slot = 0;
        if (!slot_for_character(physical.character, slot)) {
            failure = "no key types that character";
            return false;
        }
        trigger.scancode = slot;
    } else {
        if (physical.scancode == 0) {
            failure = "the binding is not a key";
            return false;
        }
        trigger.scancode = physical.scancode;
    }
    if (!other) {
        const keys::KeyboardMap typed = keys::KeyboardMap::us();
        char32_t printed = typed.printed(static_cast<uint16_t>(trigger.scancode));
        if (shift && printed >= U'a' && printed <= U'z')
            printed = printed - U'a' + U'A';
        if (printed != 0) {
            trigger.types = true;
            append_utf8(trigger.typed, printed);
        }
    }
    pressed.push_back(std::move(trigger));
    return true;
}

/// Appends one key going down or up.
///
/// @param[out] out the events
/// @param kind key_down or key_up
/// @param scancode the key
void add_key(std::vector<InputEvent>& out, InputKind kind, uint32_t scancode) {
    InputEvent event;
    event.kind = kind;
    event.scancode = scancode;
    out.push_back(std::move(event));
}

/// The five chords the automation client's action request uses when it is
/// given no chord.
///
/// @param action the action id
/// @return the chord, or null when the action is not one of the five
const char* fallback_chord(std::string_view action) noexcept {
    if (action == "order.attack")
        return "A";
    if (action == "order.stop")
        return "S";
    if (action == "select.builders")
        return "Ctrl+B";
    if (action == "select.all")
        return "Ctrl+A";
    if (action == "select.commander")
        return "Ctrl+C";
    return nullptr;
}

} // namespace

std::optional<KeyPhase> key_phase(std::string_view text) noexcept {
    if (text == "tap")
        return KeyPhase::tap;
    if (text == "press")
        return KeyPhase::press;
    if (text == "release")
        return KeyPhase::release;
    return std::nullopt;
}

std::optional<std::vector<InputEvent>>
chord_events(std::string_view chord, KeyPhase phase, int repeat, std::string& failure) {
    if (repeat < 1 || repeat > kMaxRepeat) {
        failure = "repeat is 1 to 30";
        return std::nullopt;
    }
    if (chord.empty()) {
        failure = "chord is missing";
        return std::nullopt;
    }
    const std::string spelling = normalize_chord(chord);
    keys::ChordError chord_error;
    const std::optional<keys::Chord> parsed = keys::parse_chord(spelling, chord_error);
    if (!parsed) {
        failure = std::string(keys::chord_error_text(chord_error.reason));
        if (failure.empty())
            failure = "the chord cannot be read";
        return std::nullopt;
    }
    std::vector<ChordKey> pressed;
    if (!chord_keys(*parsed, pressed, failure))
        return std::nullopt;
    const bool down = phase != KeyPhase::release;
    const bool up = phase != KeyPhase::press;
    std::vector<InputEvent> events;
    for (int time = 0; time < repeat; ++time) {
        if (down)
            for (const ChordKey& key : pressed)
                add_key(events, InputKind::key_down, key.scancode);
        if (down && up && !pressed.empty() && pressed.back().types) {
            InputEvent text;
            text.kind = InputKind::text;
            text.text = pressed.back().typed;
            events.push_back(std::move(text));
        }
        if (up)
            for (std::size_t at = pressed.size(); at-- > 0;)
                add_key(events, InputKind::key_up, pressed[at].scancode);
    }
    if (events.empty()) {
        failure = "the chord has no key";
        return std::nullopt;
    }
    if (events.size() > max_input_events) {
        failure = "the chord is too long";
        return std::nullopt;
    }
    return events;
}

std::optional<std::vector<InputEvent>> action_events(
    std::string_view action,
    KeyPhase phase,
    int repeat,
    std::string& chord_used,
    std::string& failure
) {
    if (const oa::data::input_actions::Action* found = oa::data::input_actions::find(action)) {
        if (!found->defaults.empty()) {
            std::string why;
            for (const std::string_view def : found->defaults) {
                std::optional<std::vector<InputEvent>> events =
                    chord_events(def, phase, repeat, why);
                if (events) {
                    chord_used = std::string(def);
                    return events;
                }
                if (why == "the binding is not a key") {
                    failure = why;
                    return std::nullopt;
                }
            }
            failure = why.empty() ? std::string("chord is missing") : why;
            return std::nullopt;
        }
    }
    const char* fallback = fallback_chord(action);
    if (fallback == nullptr) {
        failure = "chord is missing";
        return std::nullopt;
    }
    chord_used = fallback;
    return chord_events(fallback, phase, repeat, failure);
}

} // namespace oa::app::automation
