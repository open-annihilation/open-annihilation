// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Drawing without the game's art (artless.hpp): fills and blends, bevels,
// outlines, one-colour text, one-bit marks and scaled RGBA pictures, placed
// and scaled from source pixels and clipped to the surface and to a
// placement's clip, pixel by pixel.

#include "oa/ui/frontend_renderer/artless.hpp"

#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition)
        return;
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

namespace renderer = oa::ui::frontend_renderer;
namespace fnt = oa::formats::fnt;

constexpr renderer::Rgb black{0, 0, 0};
constexpr renderer::Rgb green{0x9c, 0xcc, 0x3c};
constexpr renderer::Rgb light{0x4a, 0x51, 0x43};
constexpr renderer::Rgb dark{0x08, 0x09, 0x07};

renderer::Surface blank(uint32_t width, uint32_t height) {
    renderer::Surface surface{width, height, {}};
    surface.rgb.assign(static_cast<std::size_t>(width) * height * 3U, 0);
    return surface;
}

renderer::Rgb pixel(const renderer::Surface& surface, uint32_t x, uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * surface.width + x) * 3U;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

// Checks every pixel of a surface against a picture: one character a pixel,
// '.' black, '#' the given colour, 'L' light and 'D' dark.
void check_picture(
    const renderer::Surface& surface,
    const std::vector<std::string_view>& picture,
    renderer::Rgb hash,
    const char* file,
    int line
) {
    check(picture.size() == surface.height, "picture height", file, line);
    for (uint32_t y = 0; y < surface.height && y < picture.size(); ++y) {
        check(picture[y].size() == surface.width, "picture width", file, line);
        for (uint32_t x = 0; x < surface.width && x < picture[y].size(); ++x) {
            const char expected = picture[y][x];
            const renderer::Rgb want = expected == '#'   ? hash
                                       : expected == 'L' ? light
                                       : expected == 'D' ? dark
                                                         : black;
            if (pixel(surface, x, y) != want) {
                std::cerr << file << ':' << line << ": pixel (" << x << ", " << y
                          << ") differs from the picture\n";
                ++failures;
            }
        }
    }
}

#define CHECK_PICTURE(surface, picture, hash)                                                      \
    check_picture((surface), (picture), (hash), __FILE__, __LINE__)

void fill_is_placed_and_scaled() {
    auto surface = blank(8, 8);
    renderer::fill_source_rect(surface, {2, 1, 2}, {1, 1, 1, 1}, green);
    CHECK(pixel(surface, 4, 3) == green);
    CHECK(pixel(surface, 5, 4) == green);
    CHECK(pixel(surface, 3, 3) == black);
    CHECK(pixel(surface, 6, 3) == black);
    CHECK(pixel(surface, 4, 2) == black);
    CHECK(pixel(surface, 4, 5) == black);
}

void nothing_below_scale_one() {
    auto surface = blank(4, 4);
    renderer::fill_source_rect(surface, {0, 0, 0}, {0, 0, 4, 4}, {255, 255, 255});
    renderer::draw_bevel(surface, {0, 0, -1}, {0, 0, 4, 4}, light, dark);
    renderer::draw_mark(surface, {0, 0, 0}, renderer::oa_mark_thin, 0, 0, green);
    CHECK(surface.rgb == blank(4, 4).rgb);
}

void blend_rounds_each_channel() {
    auto surface = blank(2, 1);
    renderer::fill_source_rect(surface, {}, {0, 0, 2, 1}, {100, 100, 100});
    renderer::blend_source_rect(surface, {}, {1, 0, 1, 1}, {200, 0, 50}, 128);
    // (100 * 128 + colour * 128 + 128) / 256, rounded down.
    CHECK(pixel(surface, 0, 0) == (renderer::Rgb{100, 100, 100}));
    CHECK(pixel(surface, 1, 0) == (renderer::Rgb{150, 50, 75}));
    renderer::blend_source_rect(surface, {}, {0, 0, 2, 1}, {200, 0, 50}, 0);
    CHECK(pixel(surface, 1, 0) == (renderer::Rgb{150, 50, 75}));
}

void fills_clip_to_the_surface() {
    auto surface = blank(4, 3);
    renderer::fill_source_rect(surface, {-3, -1, 2}, {1, 0, 3, 2}, green);
    CHECK_PICTURE(surface, (std::vector<std::string_view>{"####", "####", "####"}), green);

    // Placements and rectangles far beyond the surface draw nothing and do not overflow.
    auto far = blank(4, 3);
    const int32_t most = std::numeric_limits<int32_t>::max();
    const int32_t least = std::numeric_limits<int32_t>::min();
    renderer::fill_source_rect(far, {most, most, most}, {most, most, most, most}, green);
    renderer::fill_source_rect(far, {least, least, most}, {least, least, 2, 2}, green);
    renderer::fill_source_rect(far, {0, 0, 1}, {0, 0, 0, 3}, green);
    renderer::fill_source_rect(far, {0, 0, 1}, {0, 0, 4, -1}, green);
    CHECK(far.rgb == blank(4, 3).rgb);

    // The whole surface, from a rectangle far larger than it.
    renderer::fill_source_rect(far, {0, 0, 1}, {least / 2, least / 2, most, most}, green);
    CHECK_PICTURE(far, (std::vector<std::string_view>{"####", "####", "####"}), green);
}

void a_surface_that_does_not_fill_its_size_is_left_alone() {
    renderer::Surface surface{4, 4, std::vector<uint8_t>(5, 0)};
    renderer::fill_source_rect(surface, {}, {0, 0, 4, 4}, green);
    renderer::draw_outline(surface, {}, {0, 0, 4, 4}, green);
    renderer::draw_mark(surface, {}, renderer::oa_mark_thin, 0, 0, green);
    CHECK(surface.rgb == std::vector<uint8_t>(5, 0));
}

void bevel_is_light_above_and_left_and_dark_below_and_right() {
    auto surface = blank(6, 5);
    renderer::fill_source_rect(surface, {}, {1, 1, 4, 3}, green);
    renderer::draw_bevel(surface, {}, {0, 0, 6, 5}, light, dark);
    CHECK_PICTURE(
        surface,
        (std::vector<std::string_view>{"LLLLLD", "L####D", "L####D", "L####D", "DDDDDD"}),
        green
    );
    CHECK(pixel(surface, 0, 0) == light);
    CHECK(pixel(surface, 4, 0) == light);
    CHECK(pixel(surface, 0, 3) == light);

    // Placed at (1, 1) and drawn twice the size.
    auto scaled = blank(6, 6);
    renderer::draw_bevel(scaled, {1, 1, 2}, {0, 0, 2, 2}, light, dark);
    CHECK(pixel(scaled, 1, 1) == light);
    CHECK(pixel(scaled, 2, 2) == light);
    CHECK(pixel(scaled, 3, 1) == dark);
    CHECK(pixel(scaled, 4, 2) == dark);
    CHECK(pixel(scaled, 1, 3) == dark);
    CHECK(pixel(scaled, 4, 4) == dark);
    CHECK(pixel(scaled, 0, 0) == black);
    CHECK(pixel(scaled, 5, 5) == black);

    // One pixel high: dark throughout.
    auto thin = blank(3, 1);
    renderer::draw_bevel(thin, {}, {0, 0, 3, 1}, light, dark);
    CHECK_PICTURE(thin, (std::vector<std::string_view>{"DDD"}), green);
}

void outline_rings_the_inside() {
    auto surface = blank(5, 4);
    renderer::draw_outline(surface, {}, {1, 0, 4, 4}, green);
    CHECK_PICTURE(
        surface, (std::vector<std::string_view>{".####", ".#..#", ".#..#", ".####"}), green
    );
}

// A font of three glyphs: 'A' 3x3 with origin (0, 3), so its last row is the
// one above the pen row; 'g' 2x3 with origin (0, 1), so it starts on the row
// above the pen row and hangs a row below it; and a space two pixels wide. Glyph pixels hold palette indices the text never shows.
fnt::Font test_font() {
    fnt::Font font;
    font.nominal_height = 3;
    font.glyphs['A'] = fnt::Glyph{
        3,
        3,
        0,
        3,
        std::vector<uint8_t>(9, 7),
        {0, 1, 0, 1, 1, 1, 1, 0, 1},
    };
    font.glyphs['g'] = fnt::Glyph{
        2,
        3,
        0,
        1,
        std::vector<uint8_t>(6, 0),
        {1, 1, 1, 1, 0, 1},
    };
    font.glyphs[' '] =
        fnt::Glyph{2, 3, 0, 3, std::vector<uint8_t>(6, 9), std::vector<uint8_t>(6, 1)};
    return font;
}

void text_draws_its_glyphs_in_one_colour() {
    const auto font = test_font();
    CHECK(renderer::text_width(font, "A g") == 7);
    // A control byte and a byte without a glyph are skipped.
    CHECK(renderer::text_width(font, "A\x01Zg") == 5);

    auto surface = blank(9, 6);
    const int32_t end = renderer::draw_text(surface, {}, font, "A g\x01Z", 1, 3, green);
    CHECK(end == 8);
    CHECK_PICTURE(
        surface,
        (std::vector<std::string_view>{
            "..#......",
            ".###.....",
            ".#.#..##.",
            "......##.",
            ".......#.",
            ".........",
        }),
        green
    );
}

void text_is_placed_scaled_and_clipped() {
    const auto font = test_font();
    auto whole = blank(18, 12);
    CHECK(renderer::draw_text(whole, {0, 0, 2}, font, "Ag", 1, 3, green) == 6);

    // The same text placed up and to the left on a smaller surface shows the
    // same pixels, moved, and nothing past the surface's edges.
    auto clipped = blank(5, 5);
    CHECK(renderer::draw_text(clipped, {-3, -4, 2}, font, "Ag", 1, 3, green) == 6);
    for (uint32_t y = 0; y < clipped.height; ++y)
        for (uint32_t x = 0; x < clipped.width; ++x)
            CHECK(pixel(clipped, x, y) == pixel(whole, x + 3, y + 4));

    // Off the surface altogether, the pen still advances.
    auto none = blank(4, 4);
    CHECK(renderer::draw_text(none, {100, 100, 1}, font, "AAA", 0, 3, green) == 9);
    CHECK(renderer::draw_text(none, {0, 0, 0}, font, "AAA", 0, 3, green) == 9);
    CHECK(none.rgb == blank(4, 4).rgb);
}

// Draws a fill, a blend, an outline, a bevel, a mark and text through a placement.
/// A picture's pixels, four bytes a pixel; the picture refers to them.
using PicturePixels = std::vector<uint8_t>;

/// Returns a picture over pixels kept by the caller.
renderer::RgbaPicture picture_of(uint32_t width, uint32_t height, const PicturePixels& pixels) {
    return {width, height, pixels};
}

/// A 4 by 4 picture of four opaque colours in quarters, its corner clear.
const PicturePixels& quartered_pixels() {
    static const PicturePixels pixels = [] {
        PicturePixels quarters;
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x) {
                const bool right = x >= 2;
                const bool lower = y >= 2;
                quarters.push_back(right ? 0x20 : 0xe0);
                quarters.push_back(lower ? 0xd0 : 0x30);
                quarters.push_back(right != lower ? 0xc0 : 0x40);
                quarters.push_back(x == 0 && y == 0 ? 0 : 0xff);
            }
        return quarters;
    }();
    return pixels;
}

void draw_every_primitive(renderer::Surface& surface, const renderer::Placement& placement) {
    const auto font = test_font();
    renderer::draw_picture(surface, placement, {5, 4, 6, 5}, picture_of(4, 4, quartered_pixels()));
    renderer::fill_source_rect(surface, placement, {0, 0, 3, 9}, green);
    renderer::blend_source_rect(surface, placement, {3, 0, 4, 9}, green, 128);
    renderer::draw_outline(surface, placement, {1, 1, 9, 7}, light);
    renderer::draw_bevel(surface, placement, {7, 2, 5, 5}, light, dark);
    renderer::draw_mark(surface, placement, renderer::oa_mark_thin, 2, 2, green);
    static_cast<void>(renderer::draw_text(surface, placement, font, "AgA", 3, 6, dark));
}

void a_clip_keeps_every_primitive_inside_it() {
    for (const int32_t scale : {1, 2}) {
        const renderer::Placement whole{1, 2, scale};
        auto unclipped =
            blank(static_cast<uint32_t>(14 * scale), static_cast<uint32_t>(12 * scale));
        draw_every_primitive(unclipped, whole);

        // Clipped to source columns 2 to 7 and rows 3 to 6: the same pixels
        // inside the clip's blocks, and nothing outside them.
        const renderer::SourceRect clip{2, 3, 6, 4};
        renderer::Placement clipped_placement = whole;
        clipped_placement.clip = clip;
        auto clipped = blank(unclipped.width, unclipped.height);
        draw_every_primitive(clipped, clipped_placement);
        std::size_t drawn = 0;
        for (uint32_t y = 0; y < clipped.height; ++y)
            for (uint32_t x = 0; x < clipped.width; ++x) {
                const int32_t source_x = (static_cast<int32_t>(x) - whole.x) / scale;
                const int32_t source_y = (static_cast<int32_t>(y) - whole.y) / scale;
                const bool in_clip = static_cast<int32_t>(x) >= whole.x &&
                                     static_cast<int32_t>(y) >= whole.y && source_x >= clip.x &&
                                     source_y >= clip.y && source_x < clip.x + clip.width &&
                                     source_y < clip.y + clip.height;
                const renderer::Rgb want = in_clip ? pixel(unclipped, x, y) : black;
                CHECK(pixel(clipped, x, y) == want);
                drawn += in_clip && want != black ? 1U : 0U;
            }
        CHECK(drawn != 0);

        // A clip the drawing does not meet draws nothing; an empty clip
        // clips to the surface alone.
        renderer::Placement apart = whole;
        apart.clip = {40, 40, 3, 3};
        auto none = blank(unclipped.width, unclipped.height);
        draw_every_primitive(none, apart);
        CHECK(none.rgb == blank(unclipped.width, unclipped.height).rgb);
        renderer::Placement empty = whole;
        empty.clip = {2, 3, 0, 4};
        auto all = blank(unclipped.width, unclipped.height);
        draw_every_primitive(all, empty);
        CHECK(all.rgb == unclipped.rgb);
    }
}

// A palette whose colour i is grey i, so its brightness is 1000 * i.
oa::PaletteBytes grey_palette() {
    oa::PaletteBytes palette{};
    for (std::size_t index = 0; index < oa::palette_color_count; ++index)
        for (std::size_t channel = 0; channel < 3; ++channel)
            palette[index * oa::palette_entry_bytes + channel] = static_cast<uint8_t>(index);
    return palette;
}

// A shaded and outlined glyph, 'O' 4x4 with origin (0, 0): a ring of 40
// (and one pixel of 20, darker still, on its edge) round a face of 200 and
// 120.
fnt::Font outlined_font() {
    fnt::Font font;
    font.nominal_height = 4;
    font.glyphs['O'] = fnt::Glyph{
        4,
        4,
        0,
        0,
        {20, 40, 40, 40, 40, 200, 120, 40, 40, 200, 200, 40, 40, 40, 40, 0},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0},
    };
    return font;
}

void text_fonts_keep_the_shading_and_drop_the_ring() {
    const auto readied = renderer::text_font(outlined_font(), grey_palette());
    CHECK(readied.ink[200] == 256);
    CHECK(readied.ink[40] == 0);
    CHECK(readied.ink[20] == 0);
    // (120 - 40) / (200 - 40) of 256.
    CHECK(readied.ink[120] == 128);
    CHECK(readied.ink[0] == 0);
    CHECK(renderer::text_width(readied, "OO") == 8);

    // Over a grey of 100: the ring leaves it, the face paints white, and 120
    // blends half of it: (100 * 128 + 255 * 128 + 128) / 256 = 178.
    auto surface = blank(5, 4);
    renderer::fill_source_rect(surface, {}, {0, 0, 5, 4}, {100, 100, 100});
    CHECK(renderer::draw_text(surface, {}, readied, "O", 1, 0, {255, 255, 255}) == 5);
    const renderer::Rgb grey{100, 100, 100};
    const renderer::Rgb white{255, 255, 255};
    const renderer::Rgb half{178, 178, 178};
    const std::array<std::array<renderer::Rgb, 5>, 4> expected{{
        {grey, grey, grey, grey, grey},
        {grey, grey, white, half, grey},
        {grey, grey, white, white, grey},
        {grey, grey, grey, grey, grey},
    }};
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 5; ++x)
            CHECK(pixel(surface, x, y) == expected[y][x]);

    // A one-colour font, as a one-bit FNT font is, draws every glyph pixel
    // in the colour.
    auto one_colour = test_font();
    for (auto& glyph : one_colour.glyphs)
        if (glyph)
            std::fill(glyph->pixels.begin(), glyph->pixels.end(), fnt::foreground_index);
    const auto plain = renderer::text_font(one_colour, grey_palette());
    CHECK(plain.ink[fnt::foreground_index] == 256);
    CHECK(plain.ink[0] == 0);
    auto one = blank(9, 6);
    auto other = blank(9, 6);
    CHECK(renderer::draw_text(one, {}, plain, "A g", 1, 3, green) == 8);
    CHECK(renderer::draw_text(other, {}, test_font(), "A g", 1, 3, green) == 8);
    CHECK(one.rgb == other.rgb);

    // A font without glyphs draws nothing.
    const auto empty = renderer::text_font(fnt::Font{}, grey_palette());
    CHECK(empty.ink == renderer::GlyphInk{});
}

void marks_draw_their_set_pixels() {
    CHECK(renderer::oa_mark_thin.width == 9);
    CHECK(renderer::oa_mark_thin.height == 5);
    CHECK(renderer::oa_mark_bold.width == 13);
    CHECK(renderer::oa_mark_bold.height == 7);

    auto thin = blank(11, 7);
    renderer::draw_mark(thin, {}, renderer::oa_mark_thin, 1, 1, green);
    CHECK_PICTURE(
        thin,
        (std::vector<std::string_view>{
            "...........",
            "..##...##..",
            ".#..#.#..#.",
            ".#..#.####.",
            ".#..#.#..#.",
            "..##..#..#.",
            "...........",
        }),
        green
    );

    auto bold = blank(13, 7);
    renderer::draw_mark(bold, {}, renderer::oa_mark_bold, 0, 0, green);
    CHECK_PICTURE(
        bold,
        (std::vector<std::string_view>{
            ".####...####.",
            "##..##.##..##",
            "##..##.##..##",
            "##..##.######",
            "##..##.##..##",
            "##..##.##..##",
            ".####..##..##",
        }),
        green
    );

    // Twice the size: each set pixel is a 2x2 block.
    auto scaled = blank(4, 4);
    const std::array<uint8_t, 2> bits{1, 0};
    renderer::draw_mark(scaled, {0, 0, 2}, {2, 1, bits}, 0, 1, green);
    CHECK_PICTURE(scaled, (std::vector<std::string_view>{"....", "....", "##..", "##.."}), green);

    // Fewer bits than the mark's size: nothing.
    auto short_bits = blank(4, 4);
    renderer::draw_mark(short_bits, {}, {2, 2, bits}, 0, 0, green);
    CHECK(short_bits.rgb == blank(4, 4).rgb);
}

// Draws a text in an installed game font and checks it against the font's
// own text drawing: every pixel raster_text covers, and only those, in the
// colour, each as a block of the placement's scale.
void check_installed_text(
    const fnt::Font& font, std::string_view text, int32_t scale, const char* file, int line
) {
    const int32_t width = renderer::text_width(font, text) + 8;
    const int32_t height = fnt::line_height(font) + 8;
    std::vector<uint8_t> indices(static_cast<std::size_t>(width) * height);
    std::vector<uint8_t> coverage(indices.size());
    const fnt::IndexedSurface expected{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<std::size_t>(width),
        indices,
        coverage
    };
    const int32_t pen_row = height - 4;
    const int32_t expected_end = fnt::raster_text(expected, font, text, 4, pen_row);

    auto surface =
        blank(static_cast<uint32_t>(width * scale), static_cast<uint32_t>(height * scale));
    const int32_t end = renderer::draw_text(surface, {0, 0, scale}, font, text, 4, pen_row, green);
    check(end == expected_end, "the pen ends where raster_text ends it", file, line);
    check(
        end - 4 == renderer::text_width(font, text),
        "the pen advances by the text's width",
        file,
        line
    );
    std::size_t covered = 0;
    for (uint32_t y = 0; y < surface.height; ++y)
        for (uint32_t x = 0; x < surface.width; ++x) {
            const std::size_t source =
                static_cast<std::size_t>(y / scale) * width + x / static_cast<uint32_t>(scale);
            const renderer::Rgb want = coverage[source] != 0 ? green : black;
            covered += coverage[source] != 0 ? 1U : 0U;
            if (pixel(surface, x, y) != want) {
                std::cerr << file << ':' << line << ": \"" << text << "\" at scale " << scale
                          << ": pixel (" << x << ", " << y << ") differs from raster_text\n";
                ++failures;
                return;
            }
        }
    check(covered != 0, "the text draws pixels", file, line);
}

#define CHECK_INSTALLED_TEXT(font, text, scale)                                                    \
    check_installed_text((font), (text), (scale), __FILE__, __LINE__)

// Draws a text in a readied installed font over the panel colour and checks
// each pixel: the panel where raster_text covers nothing, else the colour
// blended over it at the ink of the index raster_text writes.
void check_installed_shaded_text(
    const renderer::TextFont& font, std::string_view text, const char* file, int line
) {
    const renderer::Rgb panel{0x1b, 0x1e, 0x19};
    const renderer::Rgb ink_color{0xe7, 0xe8, 0xdf};
    const int32_t width = renderer::text_width(font, text) + 8;
    const int32_t height = fnt::line_height(font.font) + 8;
    std::vector<uint8_t> indices(static_cast<std::size_t>(width) * height);
    std::vector<uint8_t> coverage(indices.size());
    const fnt::IndexedSurface expected{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<std::size_t>(width),
        indices,
        coverage
    };
    static_cast<void>(fnt::raster_text(expected, font.font, text, 4, 4));
    auto surface = blank(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    renderer::fill_source_rect(surface, {}, {0, 0, width, height}, panel);
    static_cast<void>(renderer::draw_text(surface, {}, font, text, 4, 4, ink_color));
    for (uint32_t y = 0; y < surface.height; ++y)
        for (uint32_t x = 0; x < surface.width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * width + x;
            renderer::Rgb want = panel;
            if (coverage[at] != 0) {
                const uint32_t share = font.ink[indices[at]];
                for (std::size_t channel = 0; channel < 3; ++channel)
                    want[channel] = static_cast<uint8_t>(
                        (panel[channel] * (256U - share) + ink_color[channel] * share + 128U) / 256U
                    );
            }
            if (pixel(surface, x, y) != want) {
                std::cerr << file << ':' << line << ": \"" << text << "\": pixel (" << x << ", "
                          << y << ") is not the ink's blend\n";
                ++failures;
                return;
            }
        }
}

void installed_fonts_draw_as_the_game_draws_them(oa::AssetStore& assets) {
    const auto regular = fnt::load_gaf(assets, "anims/hattfont12.gaf").value.value();
    const auto small = fnt::load_gaf(assets, "anims/hattfont11.gaf").value.value();
    const auto palette_file = assets.read("palettes/palette.pal").bytes;
    CHECK(palette_file.size() >= std::tuple_size_v<oa::PaletteBytes>);
    if (palette_file.size() < std::tuple_size_v<oa::PaletteBytes>)
        return;
    oa::PaletteBytes palette{};
    std::copy_n(palette_file.begin(), palette.size(), palette.begin());

    // The GUI fonts' face (66) draws the colour, their outline (93, and 94
    // on a few glyphs) nothing, and their shades between in part.
    for (const auto* font : {&regular, &small}) {
        const auto readied = renderer::text_font(*font, palette);
        CHECK(readied.ink[66] == 256);
        CHECK(readied.ink[93] == 0);
        CHECK(readied.ink[94] == 0);
        CHECK(readied.ink[71] > 0 && readied.ink[71] < readied.ink[69]);
        CHECK(readied.ink[69] < readied.ink[67] && readied.ink[67] < 256);
        check_installed_shaded_text(readied, "Pathfinding cycles", __FILE__, __LINE__);
        check_installed_shaded_text(readied, "OPEN ANNIHILATION", __FILE__, __LINE__);
    }
    for (const int32_t scale : {1, 2, 3}) {
        CHECK_INSTALLED_TEXT(regular, "Pathfinding cycles", scale);
        CHECK_INSTALLED_TEXT(regular, "OPEN ANNIHILATION", scale);
        CHECK_INSTALLED_TEXT(small, "More cycles find routes faster but use more CPU.", scale);
        CHECK_INSTALLED_TEXT(small, "Locked during a game", scale);
    }
}

} // namespace

/// Returns an opaque pixel's four bytes.
std::array<uint8_t, 4> opaque(renderer::Rgb color) {
    return {color[0], color[1], color[2], 0xff};
}

void pictures_shrink_to_the_mean_of_their_pixels() {
    constexpr renderer::Rgb red{0xff, 0, 0};
    constexpr renderer::Rgb blue{0, 0, 0xff};
    // Two red columns, then blue over clear: each half shrinks to one pixel.
    PicturePixels pixels;
    for (const auto& pixel :
         {opaque(red),
          opaque(red),
          opaque(blue),
          opaque(blue),
          opaque(red),
          opaque(red),
          std::array<uint8_t, 4>{},
          std::array<uint8_t, 4>{}})
        pixels.insert(pixels.end(), pixel.begin(), pixel.end());
    const auto picture = picture_of(4, 2, pixels);
    CHECK(renderer::picture_drawable(picture));

    auto surface = blank(4, 3);
    renderer::draw_picture(surface, {}, {1, 1, 2, 1}, picture);
    // The red half is opaque; the blue half is half covered: alpha 128 of
    // 255, laid over black.
    CHECK(pixel(surface, 1, 1) == red);
    CHECK((pixel(surface, 2, 1) == renderer::Rgb{0, 0, 128}));
    std::size_t lit = 0;
    for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            lit += pixel(surface, x, y) != black ? 1U : 0U;
    CHECK(lit == 2);

    // Over grey 100 each channel is (colour x 128 + 100 x 127 + 127) / 255.
    auto grey = blank(4, 3);
    std::fill(grey.rgb.begin(), grey.rgb.end(), uint8_t{100});
    renderer::draw_picture(grey, {}, {1, 1, 2, 1}, picture);
    CHECK((pixel(grey, 2, 1) == renderer::Rgb{50, 50, 178}));
    CHECK((pixel(grey, 0, 0) == renderer::Rgb{100, 100, 100}));
}

void a_picture_at_its_own_size_is_laid_pixel_by_pixel() {
    constexpr renderer::Rgb red{0xff, 0, 0};
    constexpr renderer::Rgb blue{0, 0, 0xff};
    // Opaque red, half covered blue and clear, drawn three surface pixels
    // wide at scale 1, its first column off the surface's left edge.
    PicturePixels pixels;
    for (const auto& pixel :
         {opaque(red), std::array<uint8_t, 4>{0, 0, 255, 128}, std::array<uint8_t, 4>{}})
        pixels.insert(pixels.end(), pixel.begin(), pixel.end());
    const auto picture = picture_of(3, 1, pixels);

    auto grey = blank(3, 2);
    std::fill(grey.rgb.begin(), grey.rgb.end(), uint8_t{100});
    renderer::draw_picture(grey, {-1, 1, 1}, {0, 0, 3, 1}, picture);
    // The red column falls off the edge; the blue one is (colour x 128 + 100
    // x 127 + 127) / 255; the clear one leaves the grey.
    CHECK((pixel(grey, 0, 1) == renderer::Rgb{50, 50, 178}));
    CHECK((pixel(grey, 1, 1) == renderer::Rgb{100, 100, 100}));
    CHECK((pixel(grey, 0, 0) == renderer::Rgb{100, 100, 100}));

    // At scale 2 a picture twice the rectangle's size is at its own size.
    auto doubled = blank(2, 2);
    PicturePixels four;
    for (const auto& pixel : {opaque(red), opaque(blue), opaque(blue), opaque(red)})
        four.insert(four.end(), pixel.begin(), pixel.end());
    renderer::draw_picture(doubled, {0, 0, 2}, {0, 0, 1, 1}, picture_of(2, 2, four));
    CHECK(pixel(doubled, 0, 0) == red && pixel(doubled, 1, 0) == blue);
    CHECK(pixel(doubled, 0, 1) == blue && pixel(doubled, 1, 1) == red);
}

void pictures_are_drawn_at_the_surface_resolution() {
    // Four colours across, drawn two source pixels wide at scale 3: six
    // surface columns share the picture's four, so source pixels split.
    const std::array<renderer::Rgb, 4> colors{
        renderer::Rgb{10, 0, 0},
        renderer::Rgb{20, 0, 0},
        renderer::Rgb{30, 0, 0},
        renderer::Rgb{40, 0, 0}
    };
    PicturePixels pixels;
    for (const auto& color : colors) {
        const auto bytes = opaque(color);
        pixels.insert(pixels.end(), bytes.begin(), bytes.end());
    }
    auto surface = blank(10, 4);
    renderer::draw_picture(surface, {1, 0, 3}, {0, 0, 2, 1}, picture_of(4, 1, pixels));
    const std::array<renderer::Rgb, 6> shown{
        colors[0], colors[0], colors[1], colors[2], colors[2], colors[3]
    };
    for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 6; ++x)
            CHECK(pixel(surface, 1 + x, y) == shown[x]);
    CHECK(pixel(surface, 0, 0) == black);
    CHECK(pixel(surface, 7, 0) == black);
    CHECK(pixel(surface, 1, 3) == black);

    // A placement off the surface's top left draws the part that shows.
    auto shifted = blank(3, 2);
    renderer::draw_picture(shifted, {-3, -1, 3}, {0, 0, 2, 1}, picture_of(4, 1, pixels));
    CHECK(pixel(shifted, 0, 0) == colors[2]);
    CHECK(pixel(shifted, 2, 1) == colors[3]);
}

void pictures_that_cannot_draw_draw_nothing() {
    const PicturePixels pixels(4U * 4U * renderer::picture_pixel_bytes, 0xff);
    const auto untouched = blank(8, 8);
    const auto drawn = [&](const renderer::RgbaPicture& picture,
                           const renderer::Placement& placement,
                           const renderer::SourceRect& rect) {
        auto surface = blank(8, 8);
        renderer::draw_picture(surface, placement, rect, picture);
        return surface.rgb == untouched.rgb;
    };
    CHECK(!drawn(picture_of(4, 4, pixels), {}, {0, 0, 4, 4}));
    CHECK(drawn(renderer::RgbaPicture{}, {}, {0, 0, 4, 4}));
    CHECK(!renderer::picture_drawable(renderer::RgbaPicture{}));
    const PicturePixels short_of_one(pixels.begin(), pixels.end() - 1);
    CHECK(!renderer::picture_drawable(picture_of(4, 4, short_of_one)));
    CHECK(drawn(picture_of(4, 4, short_of_one), {}, {0, 0, 4, 4}));
    CHECK(drawn(picture_of(4, 4, pixels), {0, 0, 0}, {0, 0, 4, 4}));
    CHECK(drawn(picture_of(4, 4, pixels), {}, {0, 0, 0, 4}));
    CHECK(drawn(picture_of(4, 4, pixels), {}, {8, 0, 4, 4}));
    const PicturePixels clear(4U * 4U * renderer::picture_pixel_bytes, 0);
    CHECK(drawn(picture_of(4, 4, clear), {}, {0, 0, 4, 4}));
    // A surface whose pixels do not fill its size is left alone.
    renderer::Surface cut = blank(4, 4);
    cut.rgb.pop_back();
    const auto before = cut.rgb;
    renderer::draw_picture(cut, {}, {0, 0, 4, 4}, picture_of(4, 4, pixels));
    CHECK(cut.rgb == before);
}

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        auto assets = oa::test::require_game_assets("the installed fonts' artless text");
        installed_fonts_draw_as_the_game_draws_them(assets);
        if (failures != 0)
            return 1;
        std::cout << "artless drawing in the installed fonts: ok\n";
        return 0;
    }
    fill_is_placed_and_scaled();
    nothing_below_scale_one();
    blend_rounds_each_channel();
    fills_clip_to_the_surface();
    a_surface_that_does_not_fill_its_size_is_left_alone();
    bevel_is_light_above_and_left_and_dark_below_and_right();
    outline_rings_the_inside();
    text_draws_its_glyphs_in_one_colour();
    text_is_placed_scaled_and_clipped();
    a_clip_keeps_every_primitive_inside_it();
    text_fonts_keep_the_shading_and_drop_the_ring();
    marks_draw_their_set_pixels();
    pictures_shrink_to_the_mean_of_their_pixels();
    a_picture_at_its_own_size_is_laid_pixel_by_pixel();
    pictures_are_drawn_at_the_surface_resolution();
    pictures_that_cannot_draw_draw_nothing();
    if (failures != 0)
        return 1;
    std::cout << "artless drawing: ok\n";
    return 0;
}
