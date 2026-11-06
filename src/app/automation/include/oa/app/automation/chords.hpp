// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A chord or an action id, as key events an input drive can push. An action
// uses the key the action registry gives it, and the five chords the
// automation client's action request uses when the registry gives none.
// A mouse, wheel or pad binding is refused: this path presses keys.
#pragma once

#include "input_events.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::automation {

/// How a chord is pressed. tap is down then up, press is down, release is up.
enum class KeyPhase : uint8_t {
    tap,     ///< the keys go down and come up
    press,   ///< the keys go down and stay
    release, ///< the keys come up
};

/// Reads a phase name.
///
/// @param text tap, press or release
/// @return the phase, or nothing when the text is none of those
[[nodiscard]] std::optional<KeyPhase> key_phase(std::string_view text) noexcept;

/// The key events of one chord.
///
/// Modifiers go down first and come up last. A tap of a key that types,
/// with no Control, Alt or Meta held, carries the typed characters between
/// the down and the up. An `\@Key` prefix in front of a key's place is dropped, as the
/// automation client's action request drops it. A lower-case name such as
/// escape or ctrl is read as the chord's own spelling.
///
/// @param chord the chord text
/// @param phase how the keys are pressed
/// @param repeat how many times; 1 to 30
/// @param[out] failure why the chord could not be pressed; unchanged on success
/// @return the events, or nothing when the chord cannot be pressed
[[nodiscard]] std::optional<std::vector<InputEvent>>
chord_events(std::string_view chord, KeyPhase phase, int repeat, std::string& failure);

/// The key events of one action's binding.
///
/// The first default the registry gives that is a key is pressed. When the
/// action is unknown or gives no default, the five chords the automation
/// client's action request uses stand in: order.attack A, order.stop S,
/// select.builders Ctrl+B, select.all Ctrl+A, select.commander Ctrl+C.
/// Anything else fails with chord is missing. A default that is a mouse
/// button, a wheel or a pad fails with the binding is not a key.
///
/// @param action the action id
/// @param phase how the keys are pressed
/// @param repeat how many times; 1 to 30
/// @param[out] chord_used the chord that was pressed
/// @param[out] failure why it could not be pressed; unchanged on success
/// @return the events, or nothing when the action cannot be pressed
[[nodiscard]] std::optional<std::vector<InputEvent>> action_events(
    std::string_view action,
    KeyPhase phase,
    int repeat,
    std::string& chord_used,
    std::string& failure
);

} // namespace oa::app::automation
