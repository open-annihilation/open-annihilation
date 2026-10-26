// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The smooth form of the kit's canvas at one and a half times: fills whose
// edges fall on whole pixels, each rounded on its own; outlines, raised
// edges and rules of whole pixels; text in the bundled modern fonts at its
// own size, measured wider than at 1x and narrower than at 2x, and kept in
// its box; a clip that keeps everything inside it; a picture drawn
// filtered; a one-bit mark; a moved origin; a coverage map and a state icon
// at the canvas's own pixels; and a button drawn through the same draw
// function as on the crisp form. The text checks read the bundled
// fonts beside the test.
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/test/check.hpp"

#include "oa/formats/fnt.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/paint/painter.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;
namespace text_font = oa::platform::text_font;

/// The scale the checks draw at: one and a half.
constexpr int32_t kOneAndAHalf = 150;

/// A colour the checks fill with.
constexpr kit::Colour kInk{200, 100, 50};

/// A smooth canvas over a painter's canvas.
struct Smooth {
    paint::Canvas pixels;   ///< the painter's canvas
    paint::Painter painter; ///< paints it
    kit::Canvas canvas{};   ///< the kit's canvas over it

    /// Makes a clear canvas of a size at a scale.
    ///
    /// @param width pixels
    /// @param height pixels
    /// @param scale_percent the scale
    /// @param fonts the modern fonts; null draws no text
    Smooth(int width, int height, int32_t scale_percent, text_font::FontStack* fonts = nullptr)
        : pixels(paint::make_canvas(width, height)), painter(pixels) {
        canvas.smooth.painter = &painter;
        canvas.smooth.fonts = fonts;
        canvas.smooth.scale_percent = scale_percent;
    }
};

/// Tells whether a pixel is the fill colour, fully opaque.
///
/// @param pixels the canvas
/// @param x column
/// @param y row
/// @param ink the colour
/// @return true when it is
bool is(const paint::Canvas& pixels, int x, int y, kit::Colour ink) {
    return paint::pixel_at(pixels, x, y) == (paint::Rgba{ink.r, ink.g, ink.b, 255});
}

/// Returns the box of every pixel a canvas holds that is not clear.
///
/// @param pixels the canvas
/// @return the box; empty when every pixel is clear
paint::Box inked(const paint::Canvas& pixels) {
    paint::Box box{};
    for (int y = 0; y < pixels.height; ++y)
        for (int x = 0; x < pixels.width; ++x)
            if (paint::pixel_at(pixels, x, y).a != 0)
                box = paint::unite(box, {x, y, 1, 1});
    return box;
}

/// Checks that a fill of 10 by 10 points at 1.5x covers exactly 15 by 15
/// pixels from (lround(x * 1.5), lround(y * 1.5)), at even and odd points,
/// and that fills sharing an edge in points share it in pixels.
void fills_cover_whole_pixels() {
    for (int32_t x = 0; x < 4; ++x) {
        for (int32_t y = 0; y < 4; ++y) {
            Smooth smooth(40, 40, kOneAndAHalf);
            kit::fill_rect(smooth.canvas, {x, y, 10, 10}, kInk);
            const auto left = static_cast<int>(std::lround(x * 1.5));
            const auto top = static_cast<int>(std::lround(y * 1.5));
            OA_CHECK(inked(smooth.pixels) == (paint::Box{left, top, 15, 15}));
            int full = 0;
            for (int row = top; row < top + 15; ++row)
                for (int column = left; column < left + 15; ++column)
                    full += is(smooth.pixels, column, row, kInk) ? 1 : 0;
            OA_CHECK(full == 15 * 15);
        }
    }
    // Two fills side by side, and one under them: no gap, no overlap, and
    // an odd width takes its pixels by its edges, 1 or 2 a point.
    Smooth smooth(40, 40, kOneAndAHalf);
    const kit::Colour other{10, 20, 30};
    kit::fill_rect(smooth.canvas, {1, 1, 3, 4}, kInk);
    kit::fill_rect(smooth.canvas, {4, 1, 5, 4}, other);
    OA_CHECK(is(smooth.pixels, 5, 2, kInk) && is(smooth.pixels, 6, 2, other));
    OA_CHECK(paint::pixel_at(smooth.pixels, 1, 2).a == 0);
    OA_CHECK(is(smooth.pixels, 2, 2, kInk));
    OA_CHECK(is(smooth.pixels, 13, 2, other) && paint::pixel_at(smooth.pixels, 14, 2).a == 0);
    // At 1x and 2x the smooth form covers what the crisp form covers.
    Smooth one(40, 40, 100);
    kit::fill_rect(one.canvas, {3, 5, 7, 2}, kInk);
    OA_CHECK(inked(one.pixels) == (paint::Box{3, 5, 7, 2}));
    Smooth two(40, 40, 200);
    kit::fill_rect(two.canvas, {3, 5, 7, 2}, kInk);
    OA_CHECK(inked(two.pixels) == (paint::Box{6, 10, 14, 4}));
}

/// Checks outlines, raised edges and rules of whole pixels: max(1,
/// lround(1.5)) = 2 a point at 1.5x, wherever they lie.
void lines_are_whole_pixels() {
    Smooth smooth(40, 40, kOneAndAHalf);
    kit::outline_rect(smooth.canvas, {2, 2, 10, 10}, kInk);
    // From 3 to 18: two pixels on each side, the inside left clear.
    OA_CHECK(inked(smooth.pixels) == (paint::Box{3, 3, 15, 15}));
    for (int along = 3; along < 18; ++along) {
        OA_CHECK(is(smooth.pixels, along, 3, kInk) && is(smooth.pixels, along, 4, kInk));
        OA_CHECK(is(smooth.pixels, along, 16, kInk) && is(smooth.pixels, along, 17, kInk));
        OA_CHECK(is(smooth.pixels, 3, along, kInk) && is(smooth.pixels, 17, along, kInk));
    }
    OA_CHECK(paint::pixel_at(smooth.pixels, 5, 5).a == 0);
    OA_CHECK(paint::pixel_at(smooth.pixels, 15, 15).a == 0);

    // A raised edge: light top and left, dark bottom and right, whose
    // corners at the top right and the bottom left are dark.
    Smooth raised(40, 40, kOneAndAHalf);
    const kit::Colour light{250, 250, 250};
    const kit::Colour dark{5, 5, 5};
    kit::bevel_rect(raised.canvas, {0, 0, 10, 10}, light, dark);
    OA_CHECK(is(raised.pixels, 0, 0, light) && is(raised.pixels, 1, 1, light));
    OA_CHECK(is(raised.pixels, 14, 0, dark) && is(raised.pixels, 13, 1, dark));
    OA_CHECK(is(raised.pixels, 0, 14, dark) && is(raised.pixels, 14, 14, dark));
    OA_CHECK(is(raised.pixels, 7, 0, light) && is(raised.pixels, 7, 13, dark));
    OA_CHECK(paint::pixel_at(raised.pixels, 7, 7).a == 0);

    // A one-point rule is two rows at 1.5x wherever it lies, across or down.
    for (int32_t row = 0; row < 4; ++row) {
        Smooth ruled(40, 40, kOneAndAHalf);
        kit::rule_rect(ruled.canvas, {2, row, 10, 1}, kInk);
        const auto top = static_cast<int>(std::lround(row * 1.5));
        OA_CHECK(inked(ruled.pixels) == (paint::Box{3, top, 15, 2}));
    }
    Smooth upright(40, 40, kOneAndAHalf);
    kit::rule_rect(upright.canvas, {3, 2, 1, 10}, kInk);
    OA_CHECK(inked(upright.pixels) == (paint::Box{5, 3, 2, 15}));
    // A blend lays its colour over at its share.
    Smooth blended(4, 4, kOneAndAHalf);
    kit::fill_rect(blended.canvas, {0, 0, 2, 2}, {0, 0, 0});
    kit::blend_rect(blended.canvas, {0, 0, 2, 2}, {255, 255, 255}, 128);
    const auto grey = paint::pixel_at(blended.pixels, 1, 1);
    OA_CHECK(grey.r >= 126 && grey.r <= 129 && grey.a == 255);
}

/// Returns the bundled fonts beside the test, or null with a note.
///
/// @return the fonts
std::unique_ptr<text_font::FontStack> bundled_fonts() {
    auto fonts = text_font::FontStack::open(text_font::bundled_font_directory());
    if (!fonts)
        std::printf("ui-kit-smooth: no fonts beside the test, so the text was not drawn\n");
    return fonts;
}

/// Checks text in the modern fonts: measured at 1.5x it is wider than at 1x
/// and narrower than at 2x, drawn antialiased in its box with its capitals
/// centred, the regular font bold and larger than the small one, and a text
/// too wide for its box drawn at full size and recorded.
void text_is_drawn_in_the_modern_fonts() {
    auto fonts = bundled_fonts();
    OA_CHECK(fonts != nullptr);
    if (!fonts)
        return;
    constexpr std::string_view words = "Use modern fonts for game text";
    const kit::Rect wide{10, 10, 400, 16};
    int32_t widths[3]{};
    const int32_t scales[3]{100, kOneAndAHalf, 200};
    for (std::size_t at = 0; at < 3; ++at) {
        Smooth smooth(900, 80, scales[at], fonts.get());
        kit::box_text(smooth.canvas, kit::FontRole::regular, words, wide, kit::Align::left, kInk);
        const paint::Box ink = inked(smooth.pixels);
        widths[at] = ink.width;
        // The text starts at its box's left and stands inside its rows.
        const kit::Rect box = kit::to_canvas(wide, scales[at]);
        OA_CHECK(ink.x >= box.x && ink.x <= box.x + 3);
        OA_CHECK(ink.y >= box.y && ink.y + ink.height <= box.y + box.height + box.height / 2);
    }
    OA_CHECK(widths[0] > 100);
    OA_CHECK(widths[1] > widths[0] && widths[1] < widths[2]);
    // measure_text counts the same text in points at each scale.
    Smooth measuring(10, 10, kOneAndAHalf, fonts.get());
    const int32_t points = kit::measure_text(measuring.canvas, kit::FontRole::regular, words);
    OA_CHECK(points * 3 / 2 >= widths[1] && points * 3 / 2 <= widths[1] + 6);

    // Antialiased: the letters' edges are partly covered pixels.
    Smooth smooth(900, 80, kOneAndAHalf, fonts.get());
    kit::box_text(smooth.canvas, kit::FontRole::regular, words, wide, kit::Align::left, kInk);
    int partial = 0;
    for (int y = 0; y < smooth.pixels.height; ++y)
        for (int x = 0; x < smooth.pixels.width; ++x) {
            const auto pixel = paint::pixel_at(smooth.pixels, x, y);
            if (pixel.a == 0)
                continue;
            OA_CHECK(pixel.r == kInk.r && pixel.g == kInk.g && pixel.b == kInk.b);
            partial += pixel.a < 255 ? 1 : 0;
        }
    OA_CHECK(partial > 20);

    // Its capitals centred on the box's height: an H in a tall box sits in
    // its middle.
    Smooth centred(200, 200, kOneAndAHalf, fonts.get());
    kit::box_text(
        centred.canvas, kit::FontRole::regular, "H", {0, 0, 100, 100}, kit::Align::centre, kInk
    );
    const paint::Box letter = inked(centred.pixels);
    OA_CHECK(std::abs((letter.y + letter.height / 2) - 75) <= 1);
    OA_CHECK(std::abs((letter.x + letter.width / 2) - 75) <= 2);

    // The regular font is bold and larger than the small one.
    Smooth regular(900, 80, kOneAndAHalf, fonts.get());
    Smooth small(900, 80, kOneAndAHalf, fonts.get());
    kit::box_text(regular.canvas, kit::FontRole::regular, words, wide, kit::Align::left, kInk);
    kit::box_text(small.canvas, kit::FontRole::small, words, wide, kit::Align::left, kInk);
    OA_CHECK(inked(regular.pixels).height > inked(small.pixels).height);
    OA_CHECK(inked(regular.pixels).width > inked(small.pixels).width);
    OA_CHECK(kit::smooth_text_bold(kit::FontRole::regular));
    OA_CHECK(!kit::smooth_text_bold(kit::FontRole::small));
    OA_CHECK(kit::smooth_text_points(kit::FontRole::regular) == 12);
    OA_CHECK(kit::smooth_text_points(kit::FontRole::small) == 10);

    // A text too wide for its box keeps its size and is recorded. The wide
    // box above drew the same words at this scale; the narrow box draws them
    // no smaller. Left aligned, so the run starts where the wide one did and
    // the ink that fits on the canvas is the whole text.
    kit::begin_text_fit_frame();
    kit::set_text_fit_screen("smooth");
    Smooth narrow(900, 80, kOneAndAHalf, fonts.get());
    const kit::Rect tight{10, 10, 190, 16};
    kit::box_text(narrow.canvas, kit::FontRole::regular, words, tight, kit::Align::left, kInk);
    const paint::Box fitted = inked(narrow.pixels);
    OA_CHECK(fitted.width == widths[1]);
#if defined(NDEBUG)
    // Release builds record nothing.
    OA_CHECK(kit::text_fit_records().empty());
#else
    OA_CHECK(!kit::text_fit_records().empty());
    OA_CHECK(kit::text_fit_records().back().wider);
    OA_CHECK(kit::text_fit_records().back().screen == "smooth");
    OA_CHECK(kit::text_fit_records().back().text == words);
#endif

    // Tracking spreads the characters: two pixels a point at 1.5x.
    Smooth tracked(900, 80, kOneAndAHalf, fonts.get());
    kit::box_text(
        tracked.canvas, kit::FontRole::small, "SETTINGS", wide, kit::Align::left, kInk, 1
    );
    Smooth plain(900, 80, kOneAndAHalf, fonts.get());
    kit::box_text(plain.canvas, kit::FontRole::small, "SETTINGS", wide, kit::Align::left, kInk);
    OA_CHECK(inked(tracked.pixels).width == inked(plain.pixels).width + 7 * 2);

    // Without fonts no text is drawn and none measured.
    Smooth bare(200, 80, kOneAndAHalf);
    kit::box_text(bare.canvas, kit::FontRole::regular, words, wide, kit::Align::left, kInk);
    OA_CHECK(inked(bare.pixels).empty());
    OA_CHECK(kit::measure_text(bare.canvas, kit::FontRole::regular, words) == 0);
}

/// Checks a clip: what lies outside it is left as it is, inside and across
/// clips, and two clips that do not meet draw nothing.
void a_clip_keeps_what_lies_outside_it() {
    Smooth smooth(40, 40, kOneAndAHalf);
    const kit::Canvas inside = kit::clipped(smooth.canvas, {2, 2, 4, 4});
    kit::fill_rect(inside, {0, 0, 20, 20}, kInk);
    OA_CHECK(inked(smooth.pixels) == (paint::Box{3, 3, 6, 6}));
    // A clip inside a clip keeps to both.
    const kit::Canvas both = kit::clipped(inside, {4, 0, 10, 10});
    kit::fill_rect(both, {0, 0, 20, 20}, {1, 2, 3});
    OA_CHECK(paint::pixel_at(smooth.pixels, 6, 4) == (paint::Rgba{1, 2, 3, 255}));
    OA_CHECK(is(smooth.pixels, 5, 4, kInk));
    // Clips that do not meet draw nothing.
    Smooth apart(40, 40, kOneAndAHalf);
    const kit::Canvas nowhere =
        kit::clipped(kit::clipped(apart.canvas, {0, 0, 4, 4}), {8, 8, 4, 4});
    kit::fill_rect(nowhere, {0, 0, 20, 20}, kInk);
    kit::outline_rect(nowhere, {0, 0, 20, 20}, kInk);
    OA_CHECK(inked(apart.pixels).empty());
    // The text keeps to its clip too.
    auto fonts = bundled_fonts();
    if (!fonts)
        return;
    Smooth text(400, 60, kOneAndAHalf, fonts.get());
    const kit::Canvas cut = kit::clipped(text.canvas, {0, 0, 30, 30});
    kit::box_text(
        cut, kit::FontRole::regular, "Graphics and more", {0, 5, 200, 16}, kit::Align::left, kInk
    );
    const paint::Box ink = inked(text.pixels);
    OA_CHECK(!ink.empty() && ink.x + ink.width <= 45);
}

/// Checks a picture: a 2 by 2 picture of black and white drawn into 10 by
/// 10 points at 1.5x fills its 15 by 15 pixels with greys between them,
/// filtered rather than nearest.
void pictures_are_drawn_filtered() {
    const std::array<uint8_t, 16> picture{
        0,
        0,
        0,
        255,
        255,
        255,
        255,
        255,
        255,
        255,
        255,
        255,
        0,
        0,
        0,
        255,
    };
    Smooth smooth(40, 40, kOneAndAHalf);
    kit::draw_picture(smooth.canvas, {2, 2, 10, 10}, renderer::RgbaPicture{2, 2, picture});
    OA_CHECK(inked(smooth.pixels) == (paint::Box{3, 3, 15, 15}));
    int greys = 0;
    for (int y = 3; y < 18; ++y)
        for (int x = 3; x < 18; ++x) {
            const auto pixel = paint::pixel_at(smooth.pixels, x, y);
            greys += pixel.r > 20 && pixel.r < 235 ? 1 : 0;
        }
    OA_CHECK(greys > 30);
    // An empty picture draws nothing.
    Smooth empty(10, 10, kOneAndAHalf);
    kit::draw_picture(empty.canvas, {0, 0, 4, 4}, renderer::RgbaPicture{});
    OA_CHECK(inked(empty.pixels).empty());
}

/// Checks a one-bit mark: each set point is its whole pixels, and a moved
/// origin moves it in points.
void marks_and_origins_are_in_points() {
    const std::array<uint8_t, 4> bits{1, 0, 0, 1};
    const renderer::Mark diagonal{2, 2, bits};
    Smooth smooth(20, 20, kOneAndAHalf);
    kit::draw_mark(smooth.canvas, diagonal, {1, 1}, kInk);
    // Point 1 is pixel 2, point 2 pixels 3 and 4: the set points (1, 1) and (2, 2).
    OA_CHECK(is(smooth.pixels, 2, 2, kInk));
    OA_CHECK(is(smooth.pixels, 3, 3, kInk) && is(smooth.pixels, 4, 4, kInk));
    OA_CHECK(paint::pixel_at(smooth.pixels, 3, 2).a == 0);
    OA_CHECK(inked(smooth.pixels) == (paint::Box{2, 2, 3, 3}));
    // A canvas moved to point (2, 2) draws its (0, 0) where (2, 2) is.
    Smooth moved(20, 20, kOneAndAHalf);
    kit::fill_rect(kit::placed_at(moved.canvas, {2, 2}), {0, 0, 1, 1}, kInk);
    OA_CHECK(inked(moved.pixels) == (paint::Box{3, 3, 2, 2}));
}

/// Checks a coverage map and a state icon at 1.5x: the map lands on the
/// pixels a rectangle covers, whole where it is 255, and a short map draws
/// nothing; a state icon stays inside its square's pixels, worked out at
/// them, its edges smoothed and its strokes whole somewhere.
void coverage_lands_on_the_canvass_own_pixels() {
    Smooth smooth(40, 40, kOneAndAHalf);
    const kit::Rect square{2, 2, 4, 4};
    const kit::Point size = kit::covered_pixels(smooth.canvas, square);
    // 2 * 1.5 = 3 to 6 * 1.5 = 9.
    OA_CHECK(size.x == 6 && size.y == 6);
    const std::vector<uint8_t> whole(36, 255);
    kit::blend_coverage(smooth.canvas, square, whole, kInk);
    OA_CHECK(inked(smooth.pixels) == (paint::Box{3, 3, 6, 6}));
    OA_CHECK(is(smooth.pixels, 3, 3, kInk) && is(smooth.pixels, 8, 8, kInk));
    Smooth short_map(40, 40, kOneAndAHalf);
    kit::blend_coverage(short_map.canvas, square, std::vector<uint8_t>(35, 255), kInk);
    OA_CHECK(inked(short_map.pixels).empty());

    for (const kit::StateIcon icon :
         {kit::StateIcon::circled_check,
          kit::StateIcon::circled_up_arrow,
          kit::StateIcon::download_arrow,
          kit::StateIcon::key}) {
        Smooth drawn(40, 40, kOneAndAHalf);
        kit::draw_state_icon(drawn.canvas, {2, 2, 14, 14}, icon, kInk);
        // 2 * 1.5 = 3 to 16 * 1.5 = 24: 21 pixels square.
        const paint::Box box = inked(drawn.pixels);
        OA_CHECK(!box.empty() && box.x >= 3 && box.y >= 3);
        OA_CHECK(box.x + box.width <= 24 && box.y + box.height <= 24);
        int whole_pixels = 0;
        int smoothed = 0;
        for (int y = 3; y < 24; ++y)
            for (int x = 3; x < 24; ++x) {
                const paint::Rgba pixel = paint::pixel_at(drawn.pixels, x, y);
                whole_pixels += is(drawn.pixels, x, y, kInk) ? 1 : 0;
                smoothed += pixel.a > 0 && pixel.a < 255 ? 1 : 0;
            }
        OA_CHECK(whole_pixels > 0 && smoothed > 0);
    }
}

/// The OA badge and the bone at 1.5x stay inside their square and keep a
/// partial edge, so the smooth form is not a doubled bitmap.
void maker_marks_are_smooth_at_one_and_a_half() {
    for (const kit::StateIcon icon : {kit::StateIcon::oa, kit::StateIcon::bone}) {
        Smooth drawn(40, 40, kOneAndAHalf);
        kit::draw_state_icon(drawn.canvas, {2, 2, 14, 14}, icon, kInk);
        const paint::Box box = inked(drawn.pixels);
        OA_CHECK(!box.empty() && box.x >= 3 && box.y >= 3);
        OA_CHECK(box.x + box.width <= 24 && box.y + box.height <= 24);
        int partial = 0;
        for (int y = 3; y < 24; ++y)
            for (int x = 3; x < 24; ++x) {
                const paint::Rgba pixel = paint::pixel_at(drawn.pixels, x, y);
                partial += pixel.a > 0 && pixel.a < 255 ? 1 : 0;
            }
        OA_CHECK(partial > 0);
    }
}

/// Checks that the components draw on the smooth form through the same draw
/// functions: a button's face and border fill its pixels at 1.5x and its
/// caption is drawn inside it.
void components_draw_on_the_smooth_form() {
    auto fonts = bundled_fonts();
    Smooth smooth(200, 60, kOneAndAHalf, fonts.get());
    kit::ButtonLook look;
    look.caption = "CANCEL";
    look.style = kit::ButtonStyle::accent;
    kit::draw_button(smooth.canvas, {4, 4, 52, 17}, look);
    // 4 * 1.5 = 6 to 56 * 1.5 = 84; 4 * 1.5 = 6 to 21 * 1.5 = 31.5, 32.
    OA_CHECK(inked(smooth.pixels) == (paint::Box{6, 6, 78, 26}));
    OA_CHECK(is(smooth.pixels, 7, 7, kit::colour::accent_light));
    OA_CHECK(is(smooth.pixels, 20, 10, kit::colour::accent));
    if (!fonts)
        return;
    int caption = 0;
    for (int y = 8; y < 30; ++y)
        for (int x = 8; x < 82; ++x) {
            const auto pixel = paint::pixel_at(smooth.pixels, x, y);
            caption +=
                pixel.r == kit::colour::on_accent.r && pixel.g == kit::colour::on_accent.g ? 1 : 0;
        }
    OA_CHECK(caption > 40);
    // A display list painted on it: a fill item, then a text item.
    Smooth listed(200, 60, kOneAndAHalf, fonts.get());
    kit::DisplayList list;
    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, 20, 10};
    face.colour = kInk;
    list.items.push_back(face);
    kit::paint(listed.canvas, list);
    OA_CHECK(inked(listed.pixels) == (paint::Box{0, 0, 30, 15}));
}

/// The synthetic font the crisp checks use: a few columns a glyph, so the
/// bold face at 150% is wider than this measure.
///
/// @return the fonts
kit::Fonts block_fonts() {
    renderer::TextFont font;
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
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// At 150% the name is drawn in the bold face, which is wider than the game
/// font, and the maker still starts list_row_gap after that drawn name. A
/// name with spaces at either end draws as the trimmed name.
void the_maker_starts_after_the_drawn_name_at_one_and_a_half() {
    auto modern = bundled_fonts();
    OA_CHECK(modern != nullptr);
    if (!modern)
        return;
    const kit::Fonts game = block_fonts();
    const kit::Rect rect{0, 0, 420, kit::compact_metrics.list_row_height};
    const auto draw = [&](Smooth& smooth, std::string title, std::string by) {
        smooth.canvas.fonts = &game;
        kit::ListRowLook look;
        look.badge_text = "AL";
        look.badge_colour = kit::colour::lock;
        look.title = std::move(title);
        look.by = std::move(by);
        kit::draw_list_row(smooth.canvas, rect, look);
    };
    Smooth spaced(700, 90, kOneAndAHalf, modern.get());
    Smooth plain(700, 90, kOneAndAHalf, modern.get());
    draw(spaced, "  Lake  ", "by Cavedog Entertainment");
    draw(plain, "Lake", "by Cavedog Entertainment");
    OA_CHECK(spaced.pixels.width == plain.pixels.width);
    OA_CHECK(spaced.pixels.height == plain.pixels.height);
    bool matches = true;
    for (int y = 0; y < plain.pixels.height && matches; ++y)
        for (int x = 0; x < plain.pixels.width; ++x)
            matches = matches &&
                      paint::pixel_at(spaced.pixels, x, y) == paint::pixel_at(plain.pixels, x, y);
    OA_CHECK(matches);

    Smooth named(700, 90, kOneAndAHalf, modern.get());
    Smooth bare(700, 90, kOneAndAHalf, modern.get());
    draw(named, "Lake", "by Cavedog Entertainment");
    draw(bare, "Lake", "");
    int leftmost = named.pixels.width;
    for (int y = 0; y < named.pixels.height; ++y)
        for (int x = 0; x < named.pixels.width; ++x)
            if (paint::pixel_at(named.pixels, x, y) != paint::pixel_at(bare.pixels, x, y))
                leftmost = std::min(leftmost, x);
    const int32_t inset = kit::compact_metrics.list_row_inset;
    const int32_t side = std::min(rect.height - 2 * inset, kit::compact_metrics.list_row_badge);
    const int32_t text_left = rect.x + inset + side + inset;
    const int32_t measured = kit::measure_text(named.canvas, kit::FontRole::regular, "Lake");
    const int32_t game_width = kit::text_width(game, kit::FontRole::regular, "Lake");
    OA_CHECK(measured > game_width);
    const int32_t by_left = text_left + measured + kit::compact_metrics.list_row_gap;
    const int32_t old_left = text_left + game_width + kit::compact_metrics.list_row_gap;
    const int start = kit::to_canvas(kit::Rect{by_left, 0, 1, 1}, kOneAndAHalf).x;
    const int old_start = kit::to_canvas(kit::Rect{old_left, 0, 1, 1}, kOneAndAHalf).x;
    OA_CHECK(leftmost < named.pixels.width);
    OA_CHECK(leftmost + 1 >= start);
    OA_CHECK(leftmost > old_start);
}

/// A pill's edge at 150% is partly covered pixels, not a step.
void a_pill_is_smooth_at_one_and_a_half() {
    Smooth smooth(120, 40, kOneAndAHalf);
    kit::draw_pill(smooth.canvas, {4, 4, 40, 12}, {"", kit::PillState::playing});
    int partial = 0;
    for (int y = 0; y < smooth.pixels.height; ++y) {
        for (int x = 0; x < smooth.pixels.width; ++x) {
            const paint::Rgba pixel = paint::pixel_at(smooth.pixels, x, y);
            if (pixel.a == 0)
                continue;
            if (pixel.a < 255)
                ++partial;
        }
    }
    OA_CHECK(partial > 0);
}

} // namespace

int main() {
    fills_cover_whole_pixels();
    lines_are_whole_pixels();
    text_is_drawn_in_the_modern_fonts();
    a_clip_keeps_what_lies_outside_it();
    pictures_are_drawn_filtered();
    marks_and_origins_are_in_points();
    coverage_lands_on_the_canvass_own_pixels();
    components_draw_on_the_smooth_form();
    a_pill_is_smooth_at_one_and_a_half();
    the_maker_starts_after_the_drawn_name_at_one_and_a_half();
    maker_marks_are_smooth_at_one_and_a_half();
    return oa::test::check_exit_status();
}
