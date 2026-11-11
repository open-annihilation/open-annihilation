// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The synthetic font the photo panel's layout is measured in. A glyph for
// byte b is 2 + b % 4 columns by 8 rows.
#pragma once

#include "oa/formats/fnt.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/text.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace photo_panel_test {

/// The block font, for both roles.
///
/// @return the fonts
inline oa::ui::kit::Fonts block_fonts() {
    oa::ui::frontend_renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(oa::ui::frontend_renderer::blend_opaque);
    return {font, font, {}, {}};
}

} // namespace photo_panel_test
