// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keyboard's shape where the system does not say. A key that prints on
// the JIS row is JIS, a key that prints on the ISO row is ISO, and anything
// else is ANSI. The player can choose another shape in the key editor.

#include "oa/ui/key_bindings/input.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::app {
namespace kit = oa::ui::kit;

kit::KeyboardShape keyboard_shape(const oa::ui::key_bindings::KeyboardMap& map) {
    namespace keys = oa::ui::key_bindings;
    const auto prints = [&](std::string_view code) {
        const std::optional<uint16_t> scan = keys::scancode_of(code);
        if (!scan)
            return false;
        return map.printed(*scan) != 0;
    };
    if (prints("IntlYen") || prints("IntlRo"))
        return kit::KeyboardShape::jis;
    if (prints("IntlBackslash"))
        return kit::KeyboardShape::iso;
    return kit::KeyboardShape::ansi;
}

} // namespace oa::app
