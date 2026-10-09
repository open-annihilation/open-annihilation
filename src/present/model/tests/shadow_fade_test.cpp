// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Shadows fading as the view zooms out: the strength curve, full at the
// game's view and closer, easing to none at a quarter of it and staying
// none farther out; the levels it is drawn at; the faded alpha table, the
// display's own wherever it is not faded, made again only when what it is
// made from changes; and the drawing of a silhouette through it.

#include "oa/present/model/shadow_fade.hpp"

#include "oa/present/blit.hpp"
#include "oa/present/display.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using namespace oa::present::model;

/// The step the mouse wheel zooms by.
constexpr float wheel_step = 1.15F;

/// A palette of grays, entry i the gray of value i, so that every mix of
/// two entries has an entry of its own: the alpha table's row 0 (black)
/// halves each gray, rounded down.
oa::Palette gray_palette() {
    oa::Palette palette{};
    for (std::size_t index = 0; index < 256; ++index) {
        const auto value = static_cast<uint8_t>(index);
        palette.entries[index] = {value, value, value, 0};
    }
    return palette;
}

/// The curve: full at zoom 1 and closer, none from a quarter out, half at
/// a half, falling the whole way and flat at both ends.
void test_strength_curve() {
    for (const float zoom : {1.0F, 1.5F, 2.0F, 4.0F})
        CHECK(shadow_strength(zoom) == 1.0F);
    for (const float zoom : {shadowless_zoom, 0.2F, 1.0F / 6.0F, 0.0F, -1.0F})
        CHECK(shadow_strength(zoom) == 0.0F);
    CHECK(std::abs(shadow_strength(0.5F) - 0.5F) < 1.0e-6F);
    // Three times as far out as the game's view: about a ninth.
    CHECK(shadow_strength(1.0F / 3.0F) > 0.1F && shadow_strength(1.0F / 3.0F) < 0.12F);
    // Eased at both ends: a hundredth past either end changes it by less
    // than a thousandth.
    CHECK(shadow_strength(0.99F) > 0.999F);
    CHECK(shadow_strength(shadowless_zoom * 1.01F) < 0.001F);
    CHECK(shadow_strength(shadowless_zoom * 1.01F) > 0.0F);
    // Each step of the wheel out lightens it, never darkens it, and the
    // steps either side of 2x out change it by about as much.
    float last = shadow_strength(1.0F);
    for (float zoom = 1.0F; zoom > shadowless_zoom; zoom /= wheel_step) {
        const float strength = shadow_strength(zoom);
        CHECK(strength <= last);
        last = strength;
    }
    const float before = shadow_strength(0.5F * wheel_step) - shadow_strength(0.5F);
    const float after = shadow_strength(0.5F) - shadow_strength(0.5F / wheel_step);
    CHECK(std::abs(before - after) < 0.01F);
    // Zooming back in returns the same strengths.
    CHECK(shadow_strength(0.5F / wheel_step * wheel_step) == shadow_strength(0.5F));
}

/// The levels: the game's own exactly where the strength is 1, a step of
/// the strength rounded between, and none from where it rounds below the
/// faintest level drawn, with nothing drawn again closer to the strength's 0.
void test_levels() {
    CHECK(shadow_level(1.0F) == shadow_full_level);
    CHECK(shadow_level(2.5F) == shadow_full_level);
    CHECK(shadow_level(shadowless_zoom) == 0);
    CHECK(shadow_level(1.0F / 6.0F) == 0);
    CHECK(shadow_level(0.5F) == shadow_full_level / 2);
    CHECK(shadow_level(0.99F) == shadow_full_level - 1);
    CHECK(shadow_level(shadowless_zoom * 1.01F) == 0);
    bool faded_out = false;
    float last_drawn = 1.0F;
    for (float zoom = 0.999F; zoom > shadowless_zoom * 1.001F; zoom *= 0.99F) {
        const uint32_t level = shadow_level(zoom);
        if (level == 0) {
            faded_out = true;
            continue;
        }
        CHECK(!faded_out);
        CHECK(level >= shadow_least_level && level < shadow_full_level);
        last_drawn = zoom;
    }
    CHECK(faded_out);
    // The faintest shadow drawn is a sixteenth of the game's darkness, a
    // little closer in than the strength's 0.
    CHECK(last_drawn > shadowless_zoom && last_drawn < 0.35F);
    CHECK(shadow_level_strength(shadow_full_level) == 1.0F);
    CHECK(shadow_level_strength(0) == 0.0F);
    CHECK(shadow_level_strength(shadow_full_level / 2) == 0.5F);
    // A channel mixed by a level: the shadow's at the full level, the
    // colour under it at 0, rounded between.
    CHECK(fade_shadow_channel(200, 100, shadow_full_level) == 100);
    CHECK(fade_shadow_channel(200, 100, 0) == 200);
    CHECK(fade_shadow_channel(200, 100, shadow_full_level / 2) == 150);
    CHECK(fade_shadow_channel(201, 100, shadow_full_level / 2) == 151);
    CHECK(fade_shadow_channel(0, 255, shadow_full_level) == 255);
}

/// The table: none at the full level or at 0, the faded rows' each colour
/// mixed toward the display's blend by the level, every other row the
/// display's, and made again only when what it is made from changes.
void test_table() {
    ModelDisplay display;
    build_model_display(display, gray_palette());
    // Row 0: black over each gray halves it, rounded down.
    for (std::size_t under = 0; under < 256; ++under)
        CHECK(display.alpha[under] == under / 2);
    ShadowTable table;
    std::array<bool, 256> colours{};
    colours[0] = true;
    CHECK(table.prepare(display, shadow_full_level, colours) == nullptr);
    CHECK(table.prepare(display, 0, colours) == nullptr);
    CHECK(table.builds() == 0);
    const uint8_t* half = table.prepare(display, shadow_full_level / 2, colours);
    CHECK(half != nullptr);
    CHECK(table.builds() == 1);
    if (half == nullptr)
        return;
    for (std::size_t under = 0; under < 256; ++under) {
        const auto value = static_cast<uint8_t>(under);
        CHECK(
            half[under] ==
            fade_shadow_channel(value, static_cast<uint8_t>(under / 2), shadow_full_level / 2)
        );
    }
    // Every row but 0 is the display's own.
    bool others_kept = true;
    for (std::size_t at = 256; at < display.alpha.size(); ++at)
        others_kept = others_kept && half[at] == display.alpha[at];
    CHECK(others_kept);
    // The same level and colours make nothing again; another level does.
    CHECK(table.prepare(display, shadow_full_level / 2, colours) == half);
    CHECK(table.builds() == 1);
    const uint8_t* faint = table.prepare(display, 1, colours);
    CHECK(table.builds() == 2);
    if (faint != nullptr) {
        // A faint shadow leaves the darkest grays as they are and lightens
        // the rest by at most one step in sixty-four of what the game's own
        // takes off.
        CHECK(faint[1] == 1);
        CHECK(faint[200] == fade_shadow_channel(200, 100, 1));
        CHECK(faint[200] == 198);
    }
    // Another colour faded makes it again, its row faded too.
    colours[7] = true;
    const uint8_t* both = table.prepare(display, 1, colours);
    CHECK(table.builds() == 3);
    if (both != nullptr)
        CHECK(both[7 * 256 + 200] != display.alpha[7 * 256 + 200]);
    // A display with no alpha table gives none.
    ModelDisplay bare;
    CHECK(table.prepare(bare, shadow_full_level / 2, colours) == nullptr);
}

/// The colours of a sprite: a raw sprite's pixels other than its key; any
/// other sprite colour 0.
void test_colours() {
    std::vector<uint8_t> pixels{9, 9, 0xff, 3};
    oa::Sprite raw{};
    raw.width = 2;
    raw.height = 2;
    raw.key = 0xff;
    raw.encoding = OA_SPRITE_RAW;
    raw.data = pixels.data();
    std::array<bool, 256> colours{};
    note_shadow_colours(&raw, colours);
    CHECK(colours[9] && colours[3] && !colours[0xff] && !colours[0]);
    std::array<bool, 256> none{};
    note_shadow_colours(nullptr, none);
    const std::array<bool, 256> empty{};
    CHECK(none == empty);
    oa::Sprite encoded = raw;
    encoded.encoding = OA_SPRITE_ROW_RLE;
    std::array<bool, 256> silhouette{};
    note_shadow_colours(&encoded, silhouette);
    CHECK(silhouette[0] && !silhouette[9]);
}

/// A colour-0 silhouette drawn through the table darkens each pixel under
/// it to the table's colour, and through the display's own to the game's.
void test_draw_through() {
    ModelDisplay display;
    build_model_display(display, gray_palette());
    oa::present::bind_display(&display.context);
    std::vector<uint8_t> pixels(4 * 4, 200);
    oa::Surface surface{};
    surface.width = 4;
    surface.height = 4;
    surface.pitch = 4;
    surface.pixels = pixels.data();
    surface.clip = {0, 0, 3, 3};
    std::vector<uint8_t> silhouette(2 * 2, 0);
    oa::Sprite sprite{};
    sprite.width = 2;
    sprite.height = 2;
    sprite.key = 0xff;
    sprite.encoding = OA_SPRITE_RAW;
    sprite.data = silhouette.data();
    oa::present::draw_sprite_blended(&surface, &sprite, 0, 0);
    CHECK(pixels[0] == 100 && pixels[5] == 100 && pixels[2] == 200 && pixels[15] == 200);
    ShadowTable table;
    std::array<bool, 256> colours{};
    colours[0] = true;
    const uint8_t* faded = table.prepare(display, shadow_full_level / 2, colours);
    std::fill(pixels.begin(), pixels.end(), 200);
    oa::present::draw_sprite_blended_through(&surface, &sprite, 2, 2, faded);
    CHECK(pixels[10] == 150 && pixels[15] == 150 && pixels[0] == 200);
    std::fill(pixels.begin(), pixels.end(), 200);
    oa::present::draw_sprite_blended_through(&surface, &sprite, 0, 0, nullptr);
    CHECK(pixels[0] == 200);
    oa::present::bind_display(nullptr);
}

} // namespace

int main() {
    test_strength_curve();
    test_levels();
    test_table();
    test_colours();
    test_draw_through();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
