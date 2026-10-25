// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A stored key from the options dialog, as a chord, and the binding list
// that key takes when it replaces an action's default.
#pragma once

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace oa::ui::key_bindings {

/// Maps one key the options dialog stores to a chord.
///
/// The sixteen keys that dialog offers are Alt, Ctrl, Shift, Tab, the
/// letters b, c, e, f, g, q, r, v, x and z, and `/` and `\`. Any other key
/// code maps to nothing. Alt, Ctrl and Shift are that modifier on its own,
/// whatever `by` says. A letter, Tab, `/` or `\` follows `by`: a place is
/// `@` and the UI Events code of `scancode`, and a letter or neither is the
/// character, or the named key Tab.
///
/// @param sdl_key_code the stored SDL key code
/// @param scancode the key's position, from the caller; used when `by` is place
/// @param by whether the action follows the letter or the place
/// @return the chord, or nothing when the key code is not one of the sixteen
[[nodiscard]] std::optional<Chord>
legacy_key_chord(uint32_t sdl_key_code, uint16_t scancode, oa::data::input_actions::By by);

/// The action's default chords with one stored key in place of each trigger.
///
/// Extra modifiers and the gesture stay. A default that is a modifier on its
/// own loses that modifier before the stored key is applied, so a letter in
/// place of Alt is the letter. A stored key that is a modifier on its own
/// replaces the whole chord with that modifier. Identical chords are kept
/// once.
///
/// @param action the action, including its default chords
/// @param old_key the stored key, as a chord
/// @return the chords, in the default list's order
[[nodiscard]] std::vector<Chord>
imported_list(const oa::data::input_actions::Action& action, const Chord& old_key);

} // namespace oa::ui::key_bindings
