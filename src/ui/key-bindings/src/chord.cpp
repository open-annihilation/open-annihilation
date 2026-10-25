// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading a chord, writing it back, and matching it to one event.

#include "oa/ui/key_bindings/chord.hpp"

#include "oa/ui/key_bindings/key_codes.hpp"

#include <iterator>

namespace oa::ui::key_bindings {
namespace {

struct ModifierName {
    std::string_view name;
    std::uint8_t bit;
};

constexpr ModifierName kModifiers[] = {
    {"Primary", primary},
    {"Ctrl", ctrl},
    {"Alt", alt},
    {"Shift", shift},
    {"Meta", meta},
};

constexpr std::string_view kMouse[] = {
    "MouseLeft",
    "MouseRight",
    "MouseMiddle",
    "MouseBack",
    "MouseForward",
    "Mouse6",
    "Mouse7",
    "Mouse8",
};

/// Fills an error and returns no chord.
///
/// @param error the error to fill
/// @param reason why the text was refused
/// @param byte the byte where it was found
/// @return nothing
std::optional<Chord> fail(ChordError& error, ChordErrorReason reason, std::uint32_t byte) {
    error.reason = reason;
    error.byte = byte;
    return std::nullopt;
}

/// Returns the modifier the remainder starts with, or null.
///
/// @param text the remainder of the chord
/// @return the modifier, or nothing
const ModifierName* modifier_at(std::string_view text) {
    for (const ModifierName& modifier : kModifiers) {
        if (text.starts_with(modifier.name))
            return &modifier;
    }
    return nullptr;
}

/// The mouse button a trigger names, or 0.
///
/// @param trigger the trigger text
/// @return the button number, 1 to 8, or 0
std::uint8_t mouse_button(std::string_view trigger) {
    for (std::size_t index = 0; index < std::size(kMouse); ++index) {
        if (kMouse[index] == trigger)
            return static_cast<std::uint8_t>(index + 1);
    }
    return 0;
}

/// The wheel code a trigger names, or 0.
///
/// @param trigger the trigger text
/// @return 1 up, -1 down, 2 right, -2 left, or 0
std::int8_t wheel_code(std::string_view trigger) {
    if (trigger == "WheelUp")
        return 1;
    if (trigger == "WheelDown")
        return -1;
    if (trigger == "WheelRight")
        return 2;
    if (trigger == "WheelLeft")
        return -2;
    return 0;
}

/// Whether a pad name is one capital word of letters and digits.
///
/// @param name the part after `Pad.`
/// @return true when it is a name
bool pad_name_ok(std::string_view name) {
    if (name.empty() || name.front() < 'A' || name.front() > 'Z')
        return false;
    for (const char letter : name) {
        const bool digit = letter >= '0' && letter <= '9';
        const bool capital = letter >= 'A' && letter <= 'Z';
        const bool small = letter >= 'a' && letter <= 'z';
        if (!digit && !capital && !small)
            return false;
    }
    return true;
}

/// Appends one ASCII character.
///
/// @param out the text
/// @param character the character
void append_ascii(std::string& out, char32_t character) {
    out.push_back(static_cast<char>(character));
}

/// Appends a code point as UTF-8.
///
/// @param out the text
/// @param character the code point
void append_utf8(std::string& out, char32_t character) {
    if (character < 0x80) {
        append_ascii(out, character);
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

/// Reads the trigger, which starts at `at` in the whole chord.
///
/// @param trigger the trigger text
/// @param at the trigger's first byte
/// @param[out] error the refusal, left unchanged when the trigger parses
/// @return the chord, or nothing
std::optional<Chord> parse_trigger(std::string_view trigger, std::uint32_t at, ChordError& error) {
    if (trigger.empty())
        return fail(error, ChordErrorReason::empty_part, at);

    if (position_refused(trigger)) {
        const bool lock = trigger == "CapsLock" || trigger == "NumLock" || trigger == "ScrollLock";
        return fail(error, lock ? ChordErrorReason::lock_key : ChordErrorReason::modifier_key, at);
    }

    if (const NamedKeySpec* named = find_named_key(trigger)) {
        Chord chord;
        chord.kind = TriggerKind::named;
        chord.named = static_cast<std::uint8_t>(named->id);
        chord.scancode = named->scancode;
        return chord;
    }

    if (trigger.starts_with('@')) {
        const std::string_view code = trigger.substr(1);
        const std::uint32_t code_at = at + 1;
        if (code.empty())
            return fail(error, ChordErrorReason::unknown_position, at);
        if (position_refused(code)) {
            const bool lock = code == "CapsLock" || code == "NumLock" || code == "ScrollLock";
            const ChordErrorReason reason =
                lock ? ChordErrorReason::lock_key : ChordErrorReason::modifier_key;
            return fail(error, reason, code_at);
        }
        if (const NamedKeySpec* named = find_named_key(code)) {
            Chord chord;
            chord.kind = TriggerKind::named;
            chord.named = static_cast<std::uint8_t>(named->id);
            chord.scancode = named->scancode;
            return chord;
        }
        const std::optional<std::uint16_t> scancode = scancode_of(code);
        if (!scancode)
            return fail(error, ChordErrorReason::unknown_position, code_at);
        Chord chord;
        chord.kind = TriggerKind::place;
        chord.scancode = *scancode;
        return chord;
    }

    if (trigger.starts_with("Pad.")) {
        const std::string_view name = trigger.substr(4);
        if (!pad_name_ok(name))
            return fail(error, ChordErrorReason::bad_pad, at + 4);
        Chord chord;
        chord.kind = TriggerKind::pad;
        chord.pad = std::string{name};
        return chord;
    }

    if (const std::uint8_t button = mouse_button(trigger)) {
        Chord chord;
        chord.kind = TriggerKind::mouse;
        chord.button = button;
        return chord;
    }

    if (const std::int8_t wheel = wheel_code(trigger)) {
        Chord chord;
        chord.kind = TriggerKind::wheel;
        chord.wheel = wheel;
        return chord;
    }

    if (trigger.size() == 1) {
        const unsigned char letter = static_cast<unsigned char>(trigger[0]);
        if (letter >= 0x21 && letter <= 0x7E) {
            Chord chord;
            chord.kind = TriggerKind::character;
            if (letter >= 'a' && letter <= 'z')
                chord.character = static_cast<char32_t>(letter - 'a' + 'A');
            else
                chord.character = letter;
            return chord;
        }
    }

    return fail(error, ChordErrorReason::not_a_trigger, at);
}

/// The physical modifiers a chord requires.
struct Need {
    bool ctrl{};
    bool alt{};
    bool shift{};
    bool meta{};
};

/// Which physical modifiers a chord names, with Primary resolved.
///
/// @param modifiers the chord's modifier bits
/// @param platform which key Primary means
/// @return the physical modifiers that must be held
Need required_modifiers(std::uint8_t modifiers, Platform platform) {
    Need need;
    need.ctrl =
        (modifiers & ctrl) != 0 || ((modifiers & primary) != 0 && !platform.primary_is_meta);
    need.alt = (modifiers & alt) != 0;
    need.shift = (modifiers & shift) != 0;
    need.meta = (modifiers & meta) != 0 || ((modifiers & primary) != 0 && platform.primary_is_meta);
    return need;
}

/// Whether a modifier bit is set.
///
/// @param modifiers the bits
/// @param bit the bit
/// @return true when it is set
bool has_bit(std::uint8_t modifiers, std::uint8_t bit) {
    return (modifiers & bit) != 0;
}

/// Whether the event is the chord's trigger. Modifiers are judged separately.
///
/// @param chord the chord
/// @param event the event
/// @return true when the trigger matches
bool trigger_matches(const Chord& chord, const InputEvent& event) {
    switch (chord.kind) {
    case TriggerKind::character: {
        if (event.kind != InputKind::key)
            return false;
        char32_t expected = chord.character;
        if (expected >= U'A' && expected <= U'Z')
            expected = static_cast<char32_t>(expected - U'A' + U'a');
        return event.key_code == static_cast<std::uint32_t>(expected);
    }
    case TriggerKind::place:
        return event.kind == InputKind::key && event.scancode == chord.scancode;
    case TriggerKind::named: {
        if (event.kind != InputKind::key)
            return false;
        if (chord.named >= named_keys().size())
            return false;
        const NamedKeySpec& key = named_keys()[chord.named];
        if (key.id == NamedKey::numpad_enter)
            return event.scancode == key.scancode;
        return event.key_code == key.key_code;
    }
    case TriggerKind::mouse:
        return event.kind == InputKind::button && event.button == chord.button && chord.button != 0;
    case TriggerKind::wheel:
        if (event.kind != InputKind::wheel)
            return false;
        if (chord.wheel == 1)
            return event.wheel_y > 0;
        if (chord.wheel == -1)
            return event.wheel_y < 0;
        if (chord.wheel == 2)
            return event.wheel_x > 0;
        if (chord.wheel == -2)
            return event.wheel_x < 0;
        return false;
    case TriggerKind::pad:
    case TriggerKind::modifier_alone:
        return false;
    }
    return false;
}

/// How the modifiers of a chord that already matched its trigger compare.
///
/// @param modifiers the chord's modifier bits
/// @param held_modifiers the physical modifiers held
/// @param platform which key Primary means
/// @return exact, with extra modifiers, or none
Match modifier_match(std::uint8_t modifiers, std::uint8_t held_modifiers, Platform platform) {
    const Need need = required_modifiers(modifiers, platform);
    const bool got_ctrl = has_bit(held_modifiers, ctrl);
    const bool got_alt = has_bit(held_modifiers, alt);
    const bool got_shift = has_bit(held_modifiers, shift);
    const bool got_meta = has_bit(held_modifiers, meta);
    if (got_ctrl != need.ctrl)
        return Match::none;
    if (need.alt && !got_alt)
        return Match::none;
    if (need.shift && !got_shift)
        return Match::none;
    if (need.meta && !got_meta)
        return Match::none;
    const bool extra =
        (got_shift && !need.shift) || (got_alt && !need.alt) || (got_meta && !need.meta);
    return extra ? Match::with_extra_modifiers : Match::exact;
}

} // namespace

std::optional<Chord> parse_chord(std::string_view text, ChordError& error) {
    error = {};
    if (text.empty())
        return fail(error, ChordErrorReason::empty, 0);
    if (text.size() > chord_text_limit)
        return fail(
            error, ChordErrorReason::too_long, static_cast<std::uint32_t>(chord_text_limit)
        );

    std::string_view body = text;
    Gesture gesture = Gesture::press;
    std::uint32_t gesture_at = 0;
    const std::size_t colon = text.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string_view suffix = text.substr(colon + 1);
        if (suffix == "double" || suffix == "hold" || suffix == "release") {
            gesture_at = static_cast<std::uint32_t>(colon);
            if (suffix == "double")
                gesture = Gesture::double_press;
            else if (suffix == "hold")
                gesture = Gesture::hold;
            else
                gesture = Gesture::release;
            body = text.substr(0, colon);
        } else if (!suffix.empty()) {
            return fail(
                error, ChordErrorReason::unknown_gesture, static_cast<std::uint32_t>(colon)
            );
        }
    }

    std::uint8_t modifiers = 0;
    std::size_t cursor = 0;
    while (cursor < body.size()) {
        const ModifierName* modifier = modifier_at(body.substr(cursor));
        if (modifier == nullptr)
            break;
        const std::size_t name_end = cursor + modifier->name.size();
        const bool at_end = name_end == body.size();
        const bool then_plus = name_end < body.size() && body[name_end] == '+';
        if (!at_end && !then_plus)
            break;
        if ((modifiers & modifier->bit) != 0)
            return fail(
                error, ChordErrorReason::modifier_repeated, static_cast<std::uint32_t>(cursor)
            );
        if (at_end) {
            if (modifiers != 0)
                return fail(
                    error, ChordErrorReason::not_a_trigger, static_cast<std::uint32_t>(cursor)
                );
            if (gesture != Gesture::press)
                return fail(error, ChordErrorReason::gesture_not_allowed, gesture_at);
            Chord chord;
            chord.kind = TriggerKind::modifier_alone;
            chord.modifiers = modifier->bit;
            error = {};
            return chord;
        }
        modifiers = static_cast<std::uint8_t>(modifiers | modifier->bit);
        cursor = name_end + 1;
    }

    // A remainder of "+" is the plus key. A longer remainder that still
    // starts with "+" has an empty part between the two pluses.
    const std::string_view trigger = body.substr(cursor);
    if (trigger.size() > 1 && trigger.front() == '+')
        return fail(error, ChordErrorReason::empty_part, static_cast<std::uint32_t>(cursor));

    std::optional<Chord> parsed = parse_trigger(trigger, static_cast<std::uint32_t>(cursor), error);
    if (!parsed)
        return std::nullopt;
    if (parsed->kind == TriggerKind::wheel && gesture != Gesture::press)
        return fail(error, ChordErrorReason::gesture_not_allowed, gesture_at);
    parsed->modifiers = modifiers;
    parsed->gesture = gesture;
    error = {};
    return parsed;
}

std::string_view chord_error_text(ChordErrorReason reason) noexcept {
    switch (reason) {
    case ChordErrorReason::ok:
        return {};
    case ChordErrorReason::empty:
        return "the chord is empty";
    case ChordErrorReason::too_long:
        return "a chord is at most 64 bytes";
    case ChordErrorReason::empty_part:
        return "the chord has an empty part";
    case ChordErrorReason::modifier_repeated:
        return "a modifier is named twice";
    case ChordErrorReason::not_a_trigger:
        return "not a trigger";
    case ChordErrorReason::unknown_position:
        return "unknown key position";
    case ChordErrorReason::lock_key:
        return "a lock key cannot be bound";
    case ChordErrorReason::modifier_key:
        return "a modifier key's position cannot be bound";
    case ChordErrorReason::gesture_not_allowed:
        return "a gesture is not allowed here";
    case ChordErrorReason::unknown_gesture:
        return "unknown gesture";
    case ChordErrorReason::bad_pad:
        return "the pad name is not a name";
    }
    return {};
}

std::string chord_text(const Chord& chord) {
    std::string text;
    auto add_modifier = [&](std::uint8_t bit, std::string_view name) {
        if ((chord.modifiers & bit) == 0)
            return;
        if (!text.empty())
            text.push_back('+');
        text.append(name);
    };
    add_modifier(primary, "Primary");
    add_modifier(ctrl, "Ctrl");
    add_modifier(alt, "Alt");
    add_modifier(shift, "Shift");
    add_modifier(meta, "Meta");

    if (!text.empty() && chord.kind != TriggerKind::modifier_alone)
        text.push_back('+');

    switch (chord.kind) {
    case TriggerKind::character: {
        char32_t character = chord.character;
        if (character >= U'a' && character <= U'z')
            character = static_cast<char32_t>(character - U'a' + U'A');
        append_utf8(text, character);
        break;
    }
    case TriggerKind::place:
        text.push_back('@');
        text.append(code_of(chord.scancode));
        break;
    case TriggerKind::named:
        if (chord.named < named_keys().size())
            text.append(named_keys()[chord.named].name);
        break;
    case TriggerKind::mouse:
        if (chord.button >= 1 && chord.button <= std::size(kMouse))
            text.append(kMouse[chord.button - 1]);
        break;
    case TriggerKind::wheel:
        if (chord.wheel == 1)
            text.append("WheelUp");
        else if (chord.wheel == -1)
            text.append("WheelDown");
        else if (chord.wheel == 2)
            text.append("WheelRight");
        else if (chord.wheel == -2)
            text.append("WheelLeft");
        break;
    case TriggerKind::pad:
        text.append("Pad.");
        text.append(chord.pad);
        break;
    case TriggerKind::modifier_alone:
        break;
    }

    if (chord.kind != TriggerKind::wheel && chord.kind != TriggerKind::modifier_alone) {
        if (chord.gesture == Gesture::double_press)
            text.append(":double");
        else if (chord.gesture == Gesture::hold)
            text.append(":hold");
        else if (chord.gesture == Gesture::release)
            text.append(":release");
    }
    return text;
}

Match match(
    const Chord& chord, const InputEvent& event, const KeyboardMap& map, Platform platform
) {
    (void)map;
    if (!trigger_matches(chord, event))
        return Match::none;
    return modifier_match(chord.modifiers, event.modifiers, platform);
}

int rank(const Chord& chord) {
    int named = 0;
    for (const std::uint8_t bit :
         {std::uint8_t{primary},
          std::uint8_t{ctrl},
          std::uint8_t{alt},
          std::uint8_t{shift},
          std::uint8_t{meta}}) {
        if ((chord.modifiers & bit) != 0)
            ++named;
    }
    const bool by_position = chord.kind == TriggerKind::place ||
                             (chord.kind == TriggerKind::named &&
                              chord.named == static_cast<std::uint8_t>(NamedKey::numpad_enter));
    return named * 2 + (by_position ? 1 : 0);
}

bool held(const Chord& modifier_alone, std::uint8_t modifiers, Platform platform) {
    if (modifier_alone.kind != TriggerKind::modifier_alone)
        return false;
    const Need need = required_modifiers(modifier_alone.modifiers, platform);
    if (need.ctrl && !has_bit(modifiers, ctrl))
        return false;
    if (need.alt && !has_bit(modifiers, alt))
        return false;
    if (need.shift && !has_bit(modifiers, shift))
        return false;
    if (need.meta && !has_bit(modifiers, meta))
        return false;
    return need.ctrl || need.alt || need.shift || need.meta;
}

} // namespace oa::ui::key_bindings
