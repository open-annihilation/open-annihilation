// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keyboard's shape on macOS, from the layout the system is using.
// The map is unused: Carbon names the shape, and the player can choose
// another one in the key editor.

#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/kit/looks.hpp"

#include <Carbon/Carbon.h>

namespace oa::app {
namespace kit = oa::ui::kit;

kit::KeyboardShape keyboard_shape(const oa::ui::key_bindings::KeyboardMap& /*map*/) {
    const OSType kind = KBGetLayoutType(LMGetKbdType());
    if (kind == kKeyboardJIS)
        return kit::KeyboardShape::jis;
    if (kind == kKeyboardISO)
        return kit::KeyboardShape::iso;
    return kit::KeyboardShape::ansi;
}

} // namespace oa::app
