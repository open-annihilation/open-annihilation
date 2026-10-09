// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/model/shadow_fade.hpp"

#include "oa/present/display.hpp"
#include "oa/present/palette_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oa::present::model {
namespace {

/// Colours in a palette, and in each row of the alpha table.
constexpr std::size_t palette_size = OA_PALETTE_COLORS;

/// Halvings of the zoom from the game's view to shadowless_zoom.
constexpr float halvings_to_shadowless = 2.0F;

/// Returns the squared distance between two colours.
///
/// @param a the first colour
/// @param r the second's red
/// @param g the second's green
/// @param b the second's blue
/// @return the sum of the squared channel differences
int32_t squared_distance(const PaletteEntry& a, int32_t r, int32_t g, int32_t b) noexcept {
    const int32_t dr = static_cast<int32_t>(a.r) - r;
    const int32_t dg = static_cast<int32_t>(a.g) - g;
    const int32_t db = static_cast<int32_t>(a.b) - b;
    return dr * dr + dg * dg + db * db;
}

/// Returns whether two palette entries show the same colour.
///
/// @param a the first entry
/// @param b the second
/// @return true when red, green and blue match
bool same_colour(const PaletteEntry& a, const PaletteEntry& b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/// Returns the colour a faded shadow turns a colour under it into: the
/// palette's entry nearest the colour under it mixed toward the colour the
/// game's shadow makes of it, the colour under it where that is as near or
/// shows the same colour, then the game's shadow's, then the lowest entry.
///
/// @param palette the palette
/// @param under the colour under the shadow
/// @param shadowed the colour the game's shadow makes of it
/// @param level 1 to shadow_full_level - 1
/// @return the faded colour
uint8_t faded_colour(const Palette& palette, uint8_t under, uint8_t shadowed, uint32_t level) {
    const PaletteEntry& from = palette.entries[under];
    const PaletteEntry& to = palette.entries[shadowed];
    const int32_t r = fade_shadow_channel(from.r, to.r, level);
    const int32_t g = fade_shadow_channel(from.g, to.g, level);
    const int32_t b = fade_shadow_channel(from.b, to.b, level);
    uint8_t best = under;
    int32_t best_distance = squared_distance(from, r, g, b);
    if (const int32_t distance = squared_distance(to, r, g, b); distance < best_distance) {
        best = shadowed;
        best_distance = distance;
    }
    for (std::size_t index = 0; index < palette_size && best_distance != 0; ++index) {
        const int32_t distance = squared_distance(palette.entries[index], r, g, b);
        if (distance < best_distance) {
            best = static_cast<uint8_t>(index);
            best_distance = distance;
        }
    }
    return same_colour(palette.entries[best], from) ? under : best;
}

} // namespace

float shadow_strength(float zoom) noexcept {
    if (zoom >= 1.0F)
        return 1.0F;
    if (zoom <= shadowless_zoom)
        return 0.0F;
    const float t = std::clamp(-std::log2(zoom) / halvings_to_shadowless, 0.0F, 1.0F);
    const float eased = t * t * (3.0F - 2.0F * t);
    return std::clamp(1.0F - eased, 0.0F, 1.0F);
}

uint32_t shadow_level(float zoom) noexcept {
    const float strength = shadow_strength(zoom);
    if (strength >= 1.0F)
        return shadow_full_level;
    if (strength <= 0.0F)
        return 0;
    const auto steps = static_cast<int64_t>(
        std::lround(static_cast<double>(strength) * static_cast<double>(shadow_full_level))
    );
    if (steps < static_cast<int64_t>(shadow_least_level))
        return 0;
    return static_cast<uint32_t>(std::min<int64_t>(steps, int64_t{shadow_full_level} - 1));
}

const uint8_t* ShadowTable::prepare(
    const ModelDisplay& display, uint32_t level, const std::array<bool, 256>& colours
) {
    const uint8_t* alpha = display.context.alpha_table;
    if (level == 0 || level >= shadow_full_level ||
        (display.context.flags & display_flag_alpha_table) == 0 || alpha == nullptr)
        return nullptr;
    const auto size = static_cast<std::size_t>(alpha_table_size);
    const bool current = level == level_ && colours == faded_rows_ && source_.size() == size &&
                         table_.size() == size &&
                         std::memcmp(&palette_, &display.palette, sizeof(Palette)) == 0 &&
                         std::memcmp(source_.data(), alpha, size) == 0;
    if (current)
        return table_.data();
    source_.assign(alpha, alpha + size);
    table_ = source_;
    palette_ = display.palette;
    faded_rows_ = colours;
    level_ = level;
    for (std::size_t colour = 0; colour < palette_size; ++colour) {
        if (!colours[colour])
            continue;
        uint8_t* row = table_.data() + colour * palette_size;
        const uint8_t* shadowed = source_.data() + colour * palette_size;
        for (std::size_t under = 0; under < palette_size; ++under)
            row[under] =
                shadowed[under] == under
                    ? static_cast<uint8_t>(under)
                    : faded_colour(palette_, static_cast<uint8_t>(under), shadowed[under], level);
    }
    ++builds_;
    return table_.data();
}

void note_shadow_colours(const Sprite* sprite, std::array<bool, 256>& colours) noexcept {
    if (sprite == nullptr || sprite->data == nullptr)
        return;
    if (sprite->encoding != OA_SPRITE_RAW || sprite->child_count != 0) {
        colours[0] = true;
        return;
    }
    const auto* pixels = static_cast<const uint8_t*>(sprite->data);
    const std::size_t count = static_cast<std::size_t>(sprite->width) * sprite->height;
    for (std::size_t i = 0; i < count; ++i)
        if (pixels[i] != sprite->key)
            colours[pixels[i]] = true;
}

} // namespace oa::present::model
