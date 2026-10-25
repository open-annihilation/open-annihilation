// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The options dialog's stored keys as chords. This file does not link SDL:
// the caller passes the key's position.

#include "oa/ui/key_bindings/legacy.hpp"

#include "oa/ui/key_bindings/key_codes.hpp"

#include <string>

namespace oa::ui::key_bindings {
namespace {

/// One key the options dialog offers, and the character chord it writes.
struct Offered {
    uint32_t code{};           ///< the SDL key code
    std::string_view letter{}; ///< the chord when the action follows the letter
};

/// The sixteen keys, in the dialog's order.
constexpr Offered offered_keys[] = {
    {0x400000e2U, "Alt"},
    {0x400000e0U, "Ctrl"},
    {0x400000e1U, "Shift"},
    {'\t', "Tab"},
    {'b', "B"},
    {'c', "C"},
    {'e', "E"},
    {'f', "F"},
    {'g', "G"},
    {'q', "Q"},
    {'r', "R"},
    {'v', "V"},
    {'x', "X"},
    {'z', "Z"},
    {'/', "/"},
    {'\\', "\\"},
};

/// Tells whether a stored code is a modifier on its own.
///
/// @param code the SDL key code
/// @return true for Alt, Ctrl and Shift
bool modifier_code(uint32_t code) noexcept {
    return code == 0x400000e2U || code == 0x400000e0U || code == 0x400000e1U;
}

/// Parses a chord text that this file built.
///
/// @param text the chord
/// @return the chord; nothing only when the text is not one this file builds
std::optional<Chord> chord_of(std::string_view text) {
    ChordError error;
    return parse_chord(text, error);
}

/// Copies a stored key's trigger onto a default chord.
///
/// A default that is a modifier on its own drops that modifier first. A
/// stored modifier replaces the whole chord.
///
/// @param fallback the action's default chord
/// @param old_key the stored key
/// @return the chord to keep
Chord replaced(const Chord& fallback, const Chord& old_key) {
    if (old_key.kind == TriggerKind::modifier_alone)
        return old_key;
    Chord chord = fallback;
    if (chord.kind == TriggerKind::modifier_alone)
        chord.modifiers = 0;
    chord.kind = old_key.kind;
    chord.character = old_key.character;
    chord.scancode = old_key.scancode;
    chord.named = old_key.named;
    chord.button = old_key.button;
    chord.wheel = old_key.wheel;
    chord.pad = old_key.pad;
    return chord;
}

} // namespace

std::optional<Chord>
legacy_key_chord(uint32_t sdl_key_code, uint16_t scancode, oa::data::input_actions::By by) {
    const Offered* offered = nullptr;
    for (const Offered& one : offered_keys) {
        if (one.code == sdl_key_code) {
            offered = &one;
            break;
        }
    }
    if (offered == nullptr)
        return std::nullopt;
    if (modifier_code(sdl_key_code))
        return chord_of(offered->letter);
    if (by == oa::data::input_actions::By::place) {
        const std::string_view code = code_of(scancode);
        if (code.empty())
            return std::nullopt;
        return chord_of(std::string("@") + std::string(code));
    }
    return chord_of(offered->letter);
}

std::vector<Chord>
imported_list(const oa::data::input_actions::Action& action, const Chord& old_key) {
    std::vector<Chord> chords;
    for (const std::string_view text : action.defaults) {
        ChordError error;
        const std::optional<Chord> parsed = parse_chord(text, error);
        if (!parsed)
            continue;
        const Chord chord = replaced(*parsed, old_key);
        bool seen = false;
        for (const Chord& kept : chords) {
            if (kept == chord)
                seen = true;
        }
        if (!seen)
            chords.push_back(chord);
    }
    return chords;
}

} // namespace oa::ui::key_bindings
