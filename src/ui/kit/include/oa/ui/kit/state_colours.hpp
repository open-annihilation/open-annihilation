// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The kit's ok, caution, danger and information colours. With colour vision
// off they are today's tokens, the same bytes the screens draw now. A mode
// substitutes that mode's colours. Selection, a switch, the OK button and
// the focus ring stay on the tokens in every mode.
#pragma once

#include "oa/ui/kit/theme.hpp"

#include <cstdint>

namespace oa::ui::kit {

/// A state a chip, a pill, a callout or a warning can be in.
enum class StateTone : uint8_t {
    ok,          ///< all is well
    caution,     ///< take care: the lock colour
    danger,      ///< something is wrong
    information, ///< a note
};

/// Returns a state's colour.
///
/// Off, ok is the accent, caution the lock colour, and danger and
/// information their tokens. A colour-vision mode uses that mode's kit
/// colours. The focus ring is not a state and stays the accent's light.
///
/// @param tone the state
/// @return the colour, opaque
[[nodiscard]] Colour state_ink(StateTone tone) noexcept;

/// Returns a state's tinted fill, the ink laid over the raised surface.
///
/// Off, these are the badge and chip tints. A mode mixes that mode's ink
/// over the same surface by the same share.
///
/// @param tone the state
/// @return the fill, opaque
[[nodiscard]] Colour state_tint(StateTone tone) noexcept;

/// Returns the words' colour on an ok chip or an ok pill.
///
/// Off, this is the accent's light, which is lighter than the ok ink. A
/// mode uses the ok ink for the words as well.
///
/// @return the colour, opaque
[[nodiscard]] Colour state_ok_words() noexcept;

} // namespace oa::ui::kit
