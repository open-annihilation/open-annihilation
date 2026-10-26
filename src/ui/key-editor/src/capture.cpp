// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Capturing a chord: held modifiers, the key that finishes it, Escape's
// second, and the line that says the chord is free, taken or refused.
#include "oa/ui/key_editor/model.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_editor/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

/// How long bare Escape is held before it becomes the chord, in milliseconds.
constexpr uint64_t kEscapeHoldMs = 1000;

/// A capital ASCII letter, or the character unchanged.
///
/// @param character the character
/// @return the capital, for a letter
char32_t capital_of(char32_t character) noexcept {
    if (character >= U'a' && character <= U'z')
        return static_cast<char32_t>(character - U'a' + U'A');
    return character;
}

/// The first letter in lower case, for the fixed chord's line. Only an ASCII
/// capital is changed.
///
/// @param text the description
/// @return the description
std::string lower_first(std::string text) {
    if (!text.empty()) {
        const auto letter = static_cast<unsigned char>(text[0]);
        if (letter >= 'A' && letter <= 'Z')
            text[0] = static_cast<char>(letter - 'A' + 'a');
    }
    return text;
}

/// A character chord with no position stored, so it matches a chord the
/// registry parsed. The position stays on the capture's own chord, where
/// Means can still switch to a place.
///
/// @param chord the chord
/// @return the chord a clash compares
keys::Chord comparable(keys::Chord chord) {
    if (chord.kind == keys::TriggerKind::character)
        chord.scancode = 0;
    return chord;
}

/// The named key an event is, matched by position first.
///
/// @param event the key
/// @return the named key, or null
const keys::NamedKeySpec* named_of(const keys::InputEvent& event) noexcept {
    if (event.scancode != 0) {
        for (const keys::NamedKeySpec& spec : keys::named_keys()) {
            if (spec.scancode == event.scancode)
                return &spec;
        }
    }
    if (event.key_code != 0 && (event.key_code & keys::position_key_code_bit) == 0) {
        for (const keys::NamedKeySpec& spec : keys::named_keys()) {
            if (spec.key_code == event.key_code)
                return &spec;
        }
    }
    return nullptr;
}

/// True when the key is Escape.
///
/// @param event the key
/// @return true for Escape
bool is_escape(const keys::InputEvent& event) noexcept {
    const keys::NamedKeySpec* named = named_of(event);
    return named != nullptr && named->id == keys::NamedKey::escape;
}

/// True when a position is a lock. A lock neither shows as held nor finishes.
///
/// @param code the UI Events code
/// @return true for CapsLock, NumLock and ScrollLock
bool lock_code(std::string_view code) noexcept {
    return code == "CapsLock" || code == "NumLock" || code == "ScrollLock";
}

/// The stored bit of one modifier key.
///
/// @param code the UI Events code, such as ShiftLeft
/// @param platform which key Primary means
/// @return the bit, or 0 when the code is not a modifier
uint8_t modifier_bit(std::string_view code, keys::Platform platform) noexcept {
    if (code.starts_with("Shift"))
        return keys::shift;
    if (code.starts_with("Alt"))
        return keys::alt;
    if (code.starts_with("Control"))
        return platform.primary_is_meta ? keys::ctrl : keys::primary;
    if (code.starts_with("Meta"))
        return platform.primary_is_meta ? keys::primary : keys::meta;
    return 0;
}

/// The character a key prints, in capitals, or 0.
///
/// @param editor the keyboard
/// @param event the key
/// @return the character
char32_t printed_of(const Editor& editor, const keys::InputEvent& event) noexcept {
    char32_t printed = editor.keyboard().printed(event.scancode);
    if (printed == 0 && event.key_code >= 32 && event.key_code < 128)
        printed = static_cast<char32_t>(event.key_code);
    return capital_of(printed);
}

/// True when a character chord is a letter some key on this keyboard types.
///
/// @param editor the keyboard
/// @param character the character, in capitals
/// @return true when Means can offer the letter
bool letter_on_keyboard(const Editor& editor, char32_t character) {
    if (character == 0)
        return false;
    keys::Chord trial;
    trial.kind = keys::TriggerKind::character;
    trial.character = character;
    return keys::on_keyboard(trial, editor.keyboard());
}

/// Sets the capture's state from the line the chord would show.
///
/// @param[in,out] capture the finished chord
/// @param editor the draft
void refresh(Capture& capture, const Editor& editor) {
    if (!capture.chord) {
        capture.state = CaptureState::waiting;
        return;
    }
    const CaptureStatus status = capture_status(editor, capture);
    if (status.refused)
        capture.state = CaptureState::refused;
    else if (status.clash)
        capture.state = CaptureState::clash;
    else
        capture.state = CaptureState::complete;
}

/// Finishes the capture on a chord and reads whether it is free.
///
/// @param[in,out] capture the capture
/// @param editor the draft
/// @param chord the chord, still carrying a letter's position
void finish(Capture& capture, const Editor& editor, keys::Chord chord) {
    capture.escape_down_ms = 0;
    capture.held_modifiers = chord.modifiers;
    const bool named = chord.kind == keys::TriggerKind::named;
    const bool device = chord.kind == keys::TriggerKind::mouse ||
                        chord.kind == keys::TriggerKind::wheel ||
                        chord.kind == keys::TriggerKind::modifier_alone;
    if (device) {
        capture.means = acts::By::none;
        capture.starts = keys::Gesture::press;
        chord.gesture = keys::Gesture::press;
    } else if (named) {
        capture.means = acts::By::place;
        chord.gesture = capture.starts;
    } else if (chord.kind == keys::TriggerKind::character) {
        capture.means = acts::By::letter;
        chord.gesture = capture.starts;
    } else {
        capture.means = acts::By::place;
        chord.gesture = capture.starts;
    }
    capture.chord = std::move(chord);
    refresh(capture, editor);
}

/// Builds the chord a key finishes, before the gesture is applied.
///
/// @param editor the keyboard and the action's `by`
/// @param capture the action
/// @param event the key
/// @param modifiers the stored modifier bits
/// @return the chord
keys::Chord chord_from_key(
    const Editor& editor, const Capture& capture, const keys::InputEvent& event, uint8_t modifiers
) {
    keys::Chord chord;
    chord.modifiers = modifiers;
    chord.gesture = keys::Gesture::press;
    if (const keys::NamedKeySpec* named = named_of(event)) {
        chord.kind = keys::TriggerKind::named;
        chord.named = static_cast<uint8_t>(named->id);
        chord.scancode = named->scancode != 0 ? named->scancode : event.scancode;
        return chord;
    }
    const char32_t printed = printed_of(editor, event);
    // Means chosen on the prompt wins over the registry's by, including a
    // choice made before the key goes down.
    const bool want_letter = capture.means != acts::By::place;
    chord.scancode = event.scancode;
    if (printed != 0)
        chord.character = printed;
    if (want_letter && letter_on_keyboard(editor, printed))
        chord.kind = keys::TriggerKind::character;
    else
        chord.kind = keys::TriggerKind::place;
    return chord;
}

/// Joins action names so the status line names every one of them.
///
/// @param names the names
/// @return the phrase
std::string join_names(const std::vector<std::string>& names) {
    std::string line;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index > 0)
            line += index + 1 == names.size() ? " and " : ", ";
        line += names[index];
    }
    return line;
}

/// The context bits an action is read in.
///
/// @param id the action id
/// @return the bits, or 0 when the registry has no such action
uint32_t context_bits(std::string_view id) noexcept {
    const acts::Action* action = acts::find(id);
    if (action == nullptr)
        return 0;
    return action->context;
}

/// True when this key is one the empty search lists.
///
/// @param named the named key
/// @return true for a function key, Print Screen, Pause, Insert, the page
///     keys, Home, End and the numpad
bool search_named(const keys::NamedKeySpec& named) noexcept {
    switch (named.id) {
    case keys::NamedKey::f1:
    case keys::NamedKey::f2:
    case keys::NamedKey::f3:
    case keys::NamedKey::f4:
    case keys::NamedKey::f5:
    case keys::NamedKey::f6:
    case keys::NamedKey::f7:
    case keys::NamedKey::f8:
    case keys::NamedKey::f9:
    case keys::NamedKey::f10:
    case keys::NamedKey::f11:
    case keys::NamedKey::f12:
    case keys::NamedKey::f13:
    case keys::NamedKey::f14:
    case keys::NamedKey::f15:
    case keys::NamedKey::f16:
    case keys::NamedKey::f17:
    case keys::NamedKey::f18:
    case keys::NamedKey::f19:
    case keys::NamedKey::f20:
    case keys::NamedKey::f21:
    case keys::NamedKey::f22:
    case keys::NamedKey::f23:
    case keys::NamedKey::f24:
    case keys::NamedKey::print_screen:
    case keys::NamedKey::pause:
    case keys::NamedKey::insert:
    case keys::NamedKey::page_up:
    case keys::NamedKey::page_down:
    case keys::NamedKey::home:
    case keys::NamedKey::end:
    case keys::NamedKey::numpad_0:
    case keys::NamedKey::numpad_1:
    case keys::NamedKey::numpad_2:
    case keys::NamedKey::numpad_3:
    case keys::NamedKey::numpad_4:
    case keys::NamedKey::numpad_5:
    case keys::NamedKey::numpad_6:
    case keys::NamedKey::numpad_7:
    case keys::NamedKey::numpad_8:
    case keys::NamedKey::numpad_9:
    case keys::NamedKey::numpad_add:
    case keys::NamedKey::numpad_subtract:
    case keys::NamedKey::numpad_multiply:
    case keys::NamedKey::numpad_divide:
    case keys::NamedKey::numpad_decimal:
    case keys::NamedKey::numpad_enter:
        return true;
    default:
        return false;
    }
}

} // namespace

uint8_t stored_modifiers(uint8_t physical, keys::Platform platform) noexcept {
    uint8_t out = 0;
    if ((physical & keys::alt) != 0)
        out = static_cast<uint8_t>(out | keys::alt);
    if ((physical & keys::shift) != 0)
        out = static_cast<uint8_t>(out | keys::shift);
    if (platform.primary_is_meta) {
        if ((physical & keys::ctrl) != 0)
            out = static_cast<uint8_t>(out | keys::ctrl);
        if ((physical & keys::meta) != 0)
            out = static_cast<uint8_t>(out | keys::primary);
    } else {
        if ((physical & keys::ctrl) != 0)
            out = static_cast<uint8_t>(out | keys::primary);
        if ((physical & keys::meta) != 0)
            out = static_cast<uint8_t>(out | keys::meta);
    }
    return out;
}

std::optional<keys::Chord> paired_chord(const keys::Chord& chord, keys::Platform platform) {
    const uint8_t physical = platform.primary_is_meta ? keys::meta : keys::ctrl;
    const bool has_primary = (chord.modifiers & keys::primary) != 0;
    const bool has_physical = (chord.modifiers & physical) != 0;
    if (has_primary == has_physical)
        return std::nullopt;
    keys::Chord paired = chord;
    const auto cleared = static_cast<uint8_t>(keys::primary | physical);
    paired.modifiers = static_cast<uint8_t>(paired.modifiers & static_cast<uint8_t>(~cleared));
    if (has_primary)
        paired.modifiers = static_cast<uint8_t>(paired.modifiers | physical);
    else
        paired.modifiers = static_cast<uint8_t>(paired.modifiers | keys::primary);
    return paired;
}

std::string context_phrase(const Editor& editor, std::string_view action) {
    const Details shown = editor.details(action);
    const std::string& where = shown.where;
    const std::string mark = " · ";
    const std::size_t at = where.rfind(mark);
    if (at == std::string::npos)
        return where;
    std::string phrase = where.substr(at + mark.size());
    const std::string held = show(editor.text(), words::aside_held);
    const std::string prefix = held + " ";
    if (phrase.starts_with(prefix))
        phrase.erase(0, prefix.size());
    return phrase;
}

CaptureEvent capture_key(Capture& capture, const Editor& editor, const keys::InputEvent& event) {
    if (capture.state != CaptureState::waiting)
        return CaptureEvent::kept;
    if (event.kind != keys::InputKind::key || event.repeat)
        return CaptureEvent::kept;
    const std::string_view code = keys::code_of(event.scancode);
    const bool refused = !code.empty() && keys::position_refused(code);
    const bool lock = refused && lock_code(code);
    const bool modifier = refused && !lock;
    if (lock)
        return CaptureEvent::kept;
    const uint8_t stored = stored_modifiers(event.modifiers, editor.platform());
    if (!event.down) {
        if (is_escape(event) && capture.escape_down_ms != 0) {
            capture.escape_down_ms = 0;
            return CaptureEvent::cancelled;
        }
        if (capture.held_modifiers == stored)
            return CaptureEvent::kept;
        capture.held_modifiers = stored;
        return CaptureEvent::changed;
    }
    if (modifier) {
        capture.held_modifiers = stored;
        const acts::Action* action = acts::find(capture.action);
        if (action != nullptr && action->kind == acts::Kind::modifier) {
            keys::Chord chord;
            chord.kind = keys::TriggerKind::modifier_alone;
            chord.modifiers = stored;
            const uint8_t bit = modifier_bit(code, editor.platform());
            if (bit != 0 && (stored & bit) == 0)
                chord.modifiers = static_cast<uint8_t>(chord.modifiers | bit);
            finish(capture, editor, chord);
        }
        return CaptureEvent::changed;
    }
    if (is_escape(event) && stored == 0) {
        if (capture.escape_down_ms == 0) {
            capture.escape_down_ms = event.time_ms == 0 ? 1 : event.time_ms;
            return CaptureEvent::changed;
        }
        return CaptureEvent::kept;
    }
    capture.escape_down_ms = 0;
    finish(capture, editor, chord_from_key(editor, capture, event, stored));
    return CaptureEvent::changed;
}

void capture_button(Capture& capture, const Editor& editor, const keys::InputEvent& event) {
    if (capture.state != CaptureState::waiting)
        return;
    if (event.kind != keys::InputKind::button || !event.down)
        return;
    if (event.button < 1 || event.button > 8)
        return;
    keys::Chord chord;
    chord.kind = keys::TriggerKind::mouse;
    chord.button = event.button;
    chord.modifiers = stored_modifiers(event.modifiers, editor.platform());
    finish(capture, editor, chord);
}

void capture_wheel(Capture& capture, const Editor& editor, const keys::InputEvent& event) {
    if (capture.state != CaptureState::waiting)
        return;
    if (event.kind != keys::InputKind::wheel)
        return;
    int8_t turn = 0;
    if (event.wheel_y > 0)
        turn = 1;
    else if (event.wheel_y < 0)
        turn = -1;
    else if (event.wheel_x > 0)
        turn = 2;
    else if (event.wheel_x < 0)
        turn = -2;
    if (turn == 0)
        return;
    keys::Chord chord;
    chord.kind = keys::TriggerKind::wheel;
    chord.wheel = turn;
    chord.modifiers = stored_modifiers(event.modifiers, editor.platform());
    finish(capture, editor, chord);
}

void capture_tick(Capture& capture, const Editor& editor, uint64_t now_ms) {
    if (capture.state != CaptureState::waiting || capture.escape_down_ms == 0)
        return;
    if (now_ms < capture.escape_down_ms)
        return;
    if (now_ms - capture.escape_down_ms < kEscapeHoldMs)
        return;
    keys::Chord chord;
    chord.kind = keys::TriggerKind::named;
    chord.named = static_cast<uint8_t>(keys::NamedKey::escape);
    for (const keys::NamedKeySpec& spec : keys::named_keys()) {
        if (spec.id == keys::NamedKey::escape)
            chord.scancode = spec.scancode;
    }
    chord.modifiers = capture.held_modifiers;
    finish(capture, editor, chord);
}

void set_means(Capture& capture, const Editor& editor, acts::By means) {
    if (means != acts::By::letter && means != acts::By::place)
        return;
    if (!capture.chord) {
        capture.means = means;
        return;
    }
    keys::Chord& chord = *capture.chord;
    if (chord.kind == keys::TriggerKind::mouse || chord.kind == keys::TriggerKind::wheel ||
        chord.kind == keys::TriggerKind::modifier_alone || chord.kind == keys::TriggerKind::pad)
        return;
    if (means == acts::By::letter) {
        if (chord.kind == keys::TriggerKind::named)
            return;
        if (!letter_on_keyboard(editor, chord.character))
            return;
        chord.kind = keys::TriggerKind::character;
    } else if (means == acts::By::place) {
        if (chord.kind == keys::TriggerKind::named)
            capture.means = acts::By::place;
        else if (chord.scancode == 0)
            return;
        else
            chord.kind = keys::TriggerKind::place;
    } else {
        return;
    }
    capture.means = means;
    refresh(capture, editor);
}

void set_gesture(Capture& capture, const Editor& editor, keys::Gesture gesture) {
    if (gesture != keys::Gesture::press && gesture != keys::Gesture::double_press &&
        gesture != keys::Gesture::hold)
        return;
    if (capture.chord && (capture.chord->kind == keys::TriggerKind::wheel ||
                          capture.chord->kind == keys::TriggerKind::modifier_alone))
        return;
    capture.starts = gesture;
    if (!capture.chord)
        return;
    capture.chord->gesture = gesture;
    refresh(capture, editor);
}

CaptureStatus capture_status(const Editor& editor, const Capture& capture) {
    CaptureStatus status;
    if (!capture.chord)
        return status;
    const keys::Chord chord = comparable(*capture.chord);
    const std::string shown = keys::display_text(chord, editor.keyboard(), editor.platform());
    const std::string context = context_phrase(editor, capture.action);
    for (const acts::Action& action : acts::actions()) {
        if (!action.fixed)
            continue;
        keys::Chord escape;
        escape.kind = keys::TriggerKind::named;
        escape.named = static_cast<uint8_t>(keys::NamedKey::escape);
        if (!keys::same_key(chord, escape, &editor.keyboard()))
            continue;
        if (!keys::layers_meet(context_bits(capture.action), action.context))
            continue;
        const Details fixed = editor.details(action.id);
        const std::string description = lower_first(
            fixed.description.empty() ? std::string(action.words.description) : fixed.description
        );
        status.refused = true;
        status.ok_enabled = false;
        status.line =
            fill(show(editor.text(), words::always_fixed), {{"description", description}});
        return status;
    }
    const std::vector<keys::Clash> clashes = editor.clashes_for(capture.action, chord);
    if (!clashes.empty()) {
        std::vector<std::string> names;
        names.reserve(clashes.size());
        for (const keys::Clash& clash : clashes) {
            const std::string other = clash.a == capture.action ? clash.b : clash.a;
            const Details row = editor.details(other);
            names.push_back(row.name.empty() ? other : row.name);
        }
        const std::string first_other =
            clashes.front().a == capture.action ? clashes.front().b : clashes.front().a;
        const Details other = editor.details(first_other);
        status.clash = clashes.front();
        if (status.clash->a != capture.action && status.clash->b == capture.action)
            std::swap(status.clash->a, status.clash->b);
        status.ok_enabled = true;
        status.line = fill(
            show(editor.text(), words::clash_line),
            {{"name", join_names(names)}, {"description", other.description}, {"context", context}}
        );
        return status;
    }
    status.ok_enabled = true;
    status.line =
        fill(show(editor.text(), words::chord_free), {{"chord", shown}, {"context", context}});
    return status;
}

std::optional<keys::Chord> chord_to_find(const Editor& editor, const keys::InputEvent& event) {
    if (event.kind == keys::InputKind::button) {
        if (!event.down || event.button < 4 || event.button > 8)
            return std::nullopt;
        keys::Chord chord;
        chord.kind = keys::TriggerKind::mouse;
        chord.button = event.button;
        chord.modifiers = stored_modifiers(event.modifiers, editor.platform());
        return chord;
    }
    if (event.kind != keys::InputKind::key || !event.down || event.repeat)
        return std::nullopt;
    const std::string_view code = keys::code_of(event.scancode);
    if (!code.empty() && keys::position_refused(code))
        return std::nullopt;
    const uint8_t stored = stored_modifiers(event.modifiers, editor.platform());
    const uint8_t searched =
        static_cast<uint8_t>(keys::primary | keys::ctrl | keys::alt | keys::meta);
    const bool with_modifier = (stored & searched) != 0;
    const keys::NamedKeySpec* named = named_of(event);
    const bool special = named != nullptr && search_named(*named);
    if (!with_modifier && !special)
        return std::nullopt;
    Capture stand_in;
    stand_in.action = "order.attack";
    keys::Chord chord = chord_from_key(editor, stand_in, event, stored);
    return comparable(chord);
}

} // namespace oa::ui::key_editor
