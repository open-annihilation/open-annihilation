// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Ok, caution, danger and information follow the colour-vision mode. Off
// returns the kit tokens themselves, not a mix worked out again, so a
// screen drawn with the mode off keeps today's bytes.
#include "oa/ui/kit/state_colours.hpp"

#include "oa/present/colour_vision/colour_vision.hpp"

namespace oa::ui::kit {
namespace {

namespace vision = oa::present::colour_vision;

/// Whether a colour-vision mode is in effect.
///
/// @return true when the mode is not off
bool mode_on() noexcept {
    return vision::active_mode() != vision::Mode::off;
}

/// Returns an sRGB colour as an opaque kit colour.
///
/// @param rgb the sRGB colour
/// @return the kit colour
Colour from_rgb(vision::Rgb rgb) noexcept {
    return Colour{rgb.red, rgb.green, rgb.blue};
}

/// Returns one kit colour of the active mode.
///
/// @param tone the state
/// @return that mode's colour for it
Colour mode_ink(StateTone tone) noexcept {
    const vision::SemanticColours colours = vision::semantic_colours(vision::active_mode());
    switch (tone) {
    case StateTone::ok:
        return from_rgb(colours.kit_ok);
    case StateTone::caution:
        return from_rgb(colours.kit_caution);
    case StateTone::danger:
        return from_rgb(colours.kit_danger);
    case StateTone::information:
        return from_rgb(colours.kit_information);
    }
    return colour::accent;
}

} // namespace

Colour state_ink(StateTone tone) noexcept {
    if (!mode_on()) {
        switch (tone) {
        case StateTone::ok:
            return colour::accent;
        case StateTone::caution:
            return colour::lock;
        case StateTone::danger:
            return colour::danger;
        case StateTone::information:
            return colour::information;
        }
        return colour::accent;
    }
    return mode_ink(tone);
}

Colour state_tint(StateTone tone) noexcept {
    if (!mode_on()) {
        switch (tone) {
        case StateTone::ok:
            return colour::ok_tint;
        case StateTone::caution:
            return colour::caution_tint;
        case StateTone::danger:
            return colour::danger_tint;
        case StateTone::information:
            return colour::information_tint;
        }
        return colour::ok_tint;
    }
    return mixed(state_ink(tone), colour::panel, badge_tint);
}

Colour state_ok_words() noexcept {
    if (!mode_on())
        return colour::accent_light;
    return state_ink(StateTone::ok);
}

} // namespace oa::ui::kit
