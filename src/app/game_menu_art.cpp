// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Draws a screen's parts with the game's own art (game_menu_art.hpp).

#include "game_menu_art.hpp"

#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
using menu_paint::nearest;

/// How many columns at each end of the game's plain buttons and of its sunk
/// bar keep their width: their bevels and the shade round them.
constexpr int32_t plain_button_end = 6;
constexpr int32_t well_end = 8;

/// How much of a box's height the game's captions fill at most: their
/// pixels are squares of as many points as keep them within it.
constexpr double caption_fill = 0.8;

/// How many points of a 1080-row window a pixel of the game's font covers,
/// before it is cut to whole points: every text on a screen is as large,
/// smaller only where its box is too small for it.
constexpr double font_pixel_rows = 1.5;

/// The brightness of the font's body, which a lit caption draws in amber
/// itself: brighter pixels are clamped, darker ones darken the amber.
constexpr uint32_t font_body_brightness = 200;

/// How much of its brightness a faded caption keeps, in 256ths.
constexpr uint32_t faded_share = 140;

/// Returns where a picture lies that covers a screen of a size, its shape
/// kept: centred, cut evenly at the edges it overhangs.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param picture the picture
/// @return the picture's place, in points; the whole screen for an empty picture
kit::Rect covering_place(int32_t width, int32_t height, const WindowIcon& picture) noexcept {
    if (picture.width == 0 || picture.height == 0)
        return {0, 0, width, height};
    const double scale = std::max(
        static_cast<double>(width) / picture.width, static_cast<double>(height) / picture.height
    );
    const int32_t columns = std::max(nearest(scale * picture.width), width);
    const int32_t rows = std::max(nearest(scale * picture.height), height);
    return {(width - columns) / 2, (height - rows) / 2, columns, rows};
}

/// Returns where the game's title stands on the screen: at its place on the
/// picture, as high as it is there, its shape kept. Where the screen cuts
/// the picture's top off, the title stands at least half as far below the
/// screen's top as it stands below the picture's, so that it stays whole.
///
/// @param picture where the picture lies on the screen, in points
/// @param art the game's art
/// @return the title's place, in points; empty for an empty title
kit::Rect title_place(const kit::Rect& picture, const GameMenuArt& art) noexcept {
    if (art.title.width == 0 || art.title.height == 0 || art.background.width == 0)
        return {};
    const double scale = static_cast<double>(picture.width) / art.background.width;
    const int32_t rows = nearest(scale * art.title_height);
    return {picture.x + nearest(scale * art.title_left),
            std::max(picture.y + nearest(scale * art.title_top), nearest(scale * art.title_top / 2.0)),
            nearest(static_cast<double>(rows) * art.title.width / art.title.height),
            rows};
}

/// Draws a picture unsmoothed: each of its pixels a square of the canvas's
/// pixels, as many as fill the rectangle's height best, from the
/// rectangle's top left.
///
/// @param canvas where it draws
/// @param rect where the picture stands, in points
/// @param picture the picture
void draw_unsmoothed(const kit::Canvas& canvas, const kit::Rect& rect, const WindowIcon& picture) {
    const kit::Point covered = kit::covered_pixels(canvas, rect);
    if (covered.x <= 0 || covered.y <= 0 || picture.width == 0 || picture.height == 0)
        return;
    const int32_t square = std::max(nearest(static_cast<double>(covered.y) / picture.height), 1);
    const kit::Rect squared{
        rect.x,
        rect.y,
        std::max(nearest(static_cast<double>(rect.width) * square * picture.width / covered.x), 1),
        std::max(nearest(static_cast<double>(rect.height) * square * picture.height / covered.y), 1)
    };
    const kit::Point drawn = kit::covered_pixels(canvas, squared);
    if (drawn.x <= 0 || drawn.y <= 0)
        return;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(drawn.x) * static_cast<std::size_t>(drawn.y) * 4U);
    for (int32_t row = 0; row < drawn.y; ++row) {
        const auto from_row = static_cast<std::size_t>(
            std::min<int64_t>(int64_t{row} * picture.height / drawn.y, picture.height - 1)
        );
        for (int32_t column = 0; column < drawn.x; ++column) {
            const auto from_column = static_cast<std::size_t>(
                std::min<int64_t>(int64_t{column} * picture.width / drawn.x, picture.width - 1)
            );
            std::copy_n(
                picture.pixels.begin() +
                    static_cast<std::ptrdiff_t>((from_row * picture.width + from_column) * 4U),
                4,
                pixels.begin() + static_cast<std::ptrdiff_t>(
                                     (static_cast<std::size_t>(row) * static_cast<std::size_t>(drawn.x) +
                                      static_cast<std::size_t>(column)) *
                                     4U
                                 )
            );
        }
    }
    kit::draw_picture(
        canvas, squared, {static_cast<uint32_t>(drawn.x), static_cast<uint32_t>(drawn.y), pixels}
    );
}

/// Returns a picture blended between its pixels to a size: each pixel the
/// mix of the four nearest the point it stands on, weighed by their
/// nearness and their opacity, so that its edges soften at every size, as
/// on the game's own buttons drawn larger than they were made.
///
/// @param picture the picture
/// @param columns the width it is blended to
/// @param rows the height it is blended to
/// @return columns x rows pixels, four bytes each
std::vector<uint8_t> blended(const WindowIcon& picture, int32_t columns, int32_t rows) {
    std::vector<uint8_t> pixels(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows) * 4U, 0);
    const auto width = static_cast<int32_t>(picture.width);
    const auto height = static_cast<int32_t>(picture.height);
    // The pixel of the picture a point stands on, and how far past its
    // centre towards the next it lies.
    const auto source = [](int32_t at, int32_t from, int32_t to) {
        const double spot = std::clamp((at + 0.5) * from / to - 0.5, 0.0, static_cast<double>(from - 1));
        const auto first = static_cast<int32_t>(spot);
        return std::make_pair(first, spot - first);
    };
    for (int32_t row = 0; row < rows; ++row) {
        const auto [top, down] = source(row, height, rows);
        const int32_t bottom = std::min(top + 1, height - 1);
        for (int32_t column = 0; column < columns; ++column) {
            const auto [left, across] = source(column, width, columns);
            const int32_t right = std::min(left + 1, width - 1);
            const std::array<std::pair<std::size_t, double>, 4> taps{{
                {static_cast<std::size_t>(top * width + left), (1.0 - across) * (1.0 - down)},
                {static_cast<std::size_t>(top * width + right), across * (1.0 - down)},
                {static_cast<std::size_t>(bottom * width + left), (1.0 - across) * down},
                {static_cast<std::size_t>(bottom * width + right), across * down},
            }};
            std::array<double, 4> mixed{};
            for (const auto& [at, weight] : taps) {
                const uint8_t* from = picture.pixels.data() + at * 4U;
                const double opacity = from[3] * weight;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    mixed[channel] += from[channel] * opacity;
                mixed[3] += opacity;
            }
            if (mixed[3] <= 0.0)
                continue;
            uint8_t* to = pixels.data() +
                          (static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                           static_cast<std::size_t>(column)) *
                              4U;
            for (std::size_t channel = 0; channel < 3; ++channel)
                to[channel] = static_cast<uint8_t>(std::lround(std::min(mixed[channel] / mixed[3], 255.0)));
            to[3] = static_cast<uint8_t>(std::lround(std::min(mixed[3], 255.0)));
        }
    }
    return pixels;
}

/// Draws a picture blended to fill a rectangle, keeping what it blended.
///
/// @param canvas where it draws
/// @param rect where the picture stands, in points
/// @param picture the picture; it stays as long as kept
/// @param kept the pictures blended, by the picture and their size
void draw_smoothed(
    const kit::Canvas& canvas,
    const kit::Rect& rect,
    const WindowIcon& picture,
    std::map<std::tuple<const WindowIcon*, int32_t, int32_t>, std::vector<uint8_t>>& kept
) {
    const kit::Point covered = kit::covered_pixels(canvas, rect);
    if (covered.x <= 0 || covered.y <= 0 || picture.width == 0 || picture.height == 0)
        return;
    auto found = kept.find({&picture, covered.x, covered.y});
    if (found == kept.end())
        found = kept.emplace(std::make_tuple(&picture, covered.x, covered.y), blended(picture, covered.x, covered.y))
                    .first;
    kit::draw_picture(
        canvas, rect, {static_cast<uint32_t>(covered.x), static_cast<uint32_t>(covered.y), found->second}
    );
}

/// Cuts a rectangle out of a picture.
///
/// @param picture the picture
/// @param x the rectangle's first column
/// @param y its first row
/// @param width its columns
/// @param height its rows
/// @return the rectangle's pixels; empty when it does not lie on the picture
WindowIcon piece_of(const WindowIcon& picture, int32_t x, int32_t y, int32_t width, int32_t height) {
    WindowIcon piece;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        static_cast<uint32_t>(x + width) > picture.width ||
        static_cast<uint32_t>(y + height) > picture.height)
        return piece;
    piece.width = static_cast<uint32_t>(width);
    piece.height = static_cast<uint32_t>(height);
    piece.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (int32_t row = 0; row < height; ++row)
        std::copy_n(
            picture.pixels.begin() +
                static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y + row) * picture.width +
                                             static_cast<std::size_t>(x)) *
                                            4U),
            static_cast<std::ptrdiff_t>(width) * 4,
            piece.pixels.begin() + static_cast<std::ptrdiff_t>(
                                       static_cast<std::size_t>(row) * piece.width * 4U
                                   )
        );
    return piece;
}

/// Cuts a picture in three across.
///
/// @param picture the picture
/// @param end how many columns each end takes
/// @return the three; empty when the ends leave no middle
Sliced slice_across(const WindowIcon& picture, int32_t end) {
    const auto width = static_cast<int32_t>(picture.width);
    const auto height = static_cast<int32_t>(picture.height);
    if (width <= 2 * end)
        return {};
    return {
        piece_of(picture, 0, 0, end, height),
        piece_of(picture, end, 0, width - 2 * end, height),
        piece_of(picture, width - end, 0, end, height)
    };
}

/// Draws a caption of the game's font in a tint.
///
/// @param caption the caption, in the font's own colours
/// @param ink the tint
/// @return the caption tinted
WindowIcon tinted(WindowIcon caption, Ink ink) {
    if (ink == Ink::plain)
        return caption;
    const std::array<uint32_t, 3> amber{game_amber.r, game_amber.g, game_amber.b};
    for (std::size_t at = 0; at + 3U < caption.pixels.size(); at += 4U) {
        uint8_t* pixel = caption.pixels.data() + at;
        if (ink == Ink::faded) {
            for (std::size_t channel = 0; channel < 3U; ++channel)
                pixel[channel] = static_cast<uint8_t>(pixel[channel] * faded_share / 256U);
            continue;
        }
        const uint32_t brightness = std::max({pixel[0], pixel[1], pixel[2]});
        for (std::size_t channel = 0; channel < 3U; ++channel)
            pixel[channel] = static_cast<uint8_t>(
                std::min<uint32_t>(amber[channel] * brightness / font_body_brightness, 255U)
            );
    }
    return caption;
}

} // namespace

GameMenuPainter::GameMenuPainter(GameMenuArt art) : art_(std::move(art)) {
    const auto [left, top, right, bottom] = art_.frame_corners;
    const auto width = static_cast<int32_t>(art_.frame.width);
    const auto height = static_cast<int32_t>(art_.frame.height);
    const int32_t across = width - left - right;
    const int32_t down = height - top - bottom;
    if (across > 0 && down > 0) {
        const auto cut = [&](FramePiece piece, int32_t x, int32_t y, int32_t columns, int32_t rows) {
            frame_pieces_[static_cast<std::size_t>(piece)] = piece_of(art_.frame, x, y, columns, rows);
        };
        cut(FramePiece::top_left, 0, 0, left, top);
        cut(FramePiece::top_right, width - right, 0, right, top);
        cut(FramePiece::bottom_left, 0, height - bottom, left, bottom);
        cut(FramePiece::bottom_right, width - right, height - bottom, right, bottom);
        cut(FramePiece::top, left, 0, across, top);
        cut(FramePiece::bottom, left, height - bottom, across, bottom);
        cut(FramePiece::left, 0, top, left, down);
        cut(FramePiece::right, width - right, top, right, down);
    }
    for (const auto& button : art_.plain_buttons) {
        std::array<Sliced, 3> sliced{};
        for (std::size_t frame = 0; frame < button.size(); ++frame)
            sliced[frame] = slice_across(button[frame], plain_button_end);
        if (sliced[0].middle.width != 0 && sliced[1].middle.width != 0 && sliced[2].middle.width != 0)
            plain_buttons_.push_back(std::move(sliced));
    }
    well_ = slice_across(art_.well, well_end);
    const auto whole = [](const WindowIcon& piece) { return piece.width != 0; };
    content_ = !plain_buttons_.empty() && well_.middle.width != 0 &&
               art_.sliders.size() >= slider_piece_count && art_.caption &&
               std::all_of(art_.list_box.begin(), art_.list_box.end(), whole) &&
               std::all_of(art_.check_box.begin(), art_.check_box.end(), whole) &&
               art_.plate.width != 0;
}

oa::ui::kit::Rect game_menu_stage(int32_t width, int32_t height) noexcept {
    const int32_t times = std::max(std::min(width / stage_columns, height / stage_rows), 1);
    const int32_t columns = std::min(width, times * stage_columns);
    const int32_t rows = std::min(height, times * stage_rows);
    return {(width - columns) / 2, (height - rows) / 2, columns, rows};
}

void GameMenuPainter::lay_out(int32_t width, int32_t height, double unit) {
    fitted_.clear();
    smoothed_.clear();
    text_square_ = std::max(static_cast<int32_t>(unit * font_pixel_rows), 1);
    background_ = covering_place(width, height, art_.background);
    const kit::Rect stage = game_menu_stage(width, height);
    kit::Rect staged = covering_place(stage.width, stage.height, art_.background);
    staged.x += stage.x;
    staged.y += stage.y;
    staged_width_ = staged.width;
    title_ = title_place(staged, art_);
}

double GameMenuPainter::picture_scale() const noexcept {
    if (art_.background.width == 0)
        return 1.0;
    return static_cast<double>(staged_width_) / art_.background.width;
}

void GameMenuPainter::draw_background(const kit::Canvas& canvas) const {
    fitted_.draw(canvas, background_, art_.background);
    draw_unsmoothed(canvas, title_, art_.title);
}

const WindowIcon& GameMenuPainter::caption(std::string_view text, Ink ink) const {
    auto key = std::make_pair(std::string(text), ink);
    auto known = captions_.find(key);
    if (known == captions_.end()) {
        WindowIcon drawn = art_.caption ? art_.caption(text) : WindowIcon{};
        known = captions_.emplace(std::move(key), tinted(std::move(drawn), ink)).first;
    }
    return known->second;
}

int32_t GameMenuPainter::caption_square(const kit::Rect& box, const WindowIcon& caption) const noexcept {
    if (caption.height == 0)
        return 1;
    int32_t square = std::clamp(static_cast<int32_t>(box.height * caption_fill / caption.height), 1, text_square_);
    while (square > 1 && static_cast<int32_t>(caption.width) * square > box.width)
        --square;
    return square;
}

int32_t GameMenuPainter::text_width(const kit::Rect& box, std::string_view text) const {
    const WindowIcon& drawn = caption(text, Ink::plain);
    return static_cast<int32_t>(drawn.width) * caption_square(box, drawn);
}

int32_t GameMenuPainter::text_square(int32_t height) const {
    return caption_square({0, 0, std::numeric_limits<int32_t>::max(), height}, caption("A", Ink::plain));
}

int32_t GameMenuPainter::line_height(int32_t square) const {
    const WindowIcon& sample = caption("A", Ink::plain);
    return static_cast<int32_t>(std::ceil(static_cast<double>(sample.height) * square / caption_fill));
}

std::function<int32_t(std::string_view)> GameMenuPainter::measure(int32_t square) const {
    return [this, square](std::string_view line) {
        return static_cast<int32_t>(caption(line, Ink::plain).width) * square;
    };
}

void GameMenuPainter::draw_text(
    const kit::Canvas& canvas, const kit::Rect& box, std::string_view text, kit::Align align, Ink ink,
    TextEdge edge
) const {
    if (box.width <= 0 || box.height <= 0 || text.empty())
        return;
    draw_text_at(canvas, box, text, align, ink, 0, edge);
}

void GameMenuPainter::draw_text_at(
    const kit::Canvas& canvas, const kit::Rect& box, std::string_view text, kit::Align align, Ink ink,
    int32_t square, TextEdge edge
) const {
    if (box.width <= 0 || box.height <= 0 || text.empty())
        return;
    // At a size the caption is cut where it outgrows the box at that size;
    // without one, where it outgrows it at a point a pixel.
    const int32_t fit_square = std::max(square, 1);
    std::string kept(text);
    const WindowIcon* drawn = &caption(kept, ink);
    while (static_cast<int32_t>(drawn->width) * fit_square > box.width && !kept.empty()) {
        // Cuts a whole character, its continuation bytes with it.
        while (!kept.empty() && (static_cast<uint8_t>(kept.back()) & 0xC0U) == 0x80U)
            kept.pop_back();
        if (!kept.empty())
            kept.pop_back();
        drawn = &caption(kept + "...", ink);
    }
    if (drawn->width == 0 || drawn->height == 0)
        return;
    const int32_t size = square > 0 ? square : caption_square(box, *drawn);
    const int32_t width = static_cast<int32_t>(drawn->width) * size;
    const int32_t height = static_cast<int32_t>(drawn->height) * size;
    const int32_t x = align == kit::Align::left    ? box.x
                      : align == kit::Align::right ? box.x + box.width - width
                                                   : box.x + (box.width - width) / 2;
    const kit::Rect place{x, box.y + (box.height - height) / 2, width, height};
    if (edge == TextEdge::smoothed)
        draw_smoothed(kit::clipped(canvas, box), place, *drawn, smoothed_);
    else
        draw_unsmoothed(kit::clipped(canvas, box), place, *drawn);
}

void GameMenuPainter::draw_sliced(const kit::Canvas& canvas, const kit::Rect& rect, const Sliced& sliced) const {
    if (sliced.middle.height == 0 || rect.width <= 0 || rect.height <= 0)
        return;
    const double scale = static_cast<double>(rect.height) / sliced.middle.height;
    const int32_t left = std::min(nearest(scale * sliced.left.width), rect.width / 2);
    const int32_t right = std::min(nearest(scale * sliced.right.width), rect.width - left);
    fitted_.draw(canvas, {rect.x, rect.y, left, rect.height}, sliced.left);
    if (rect.width - left - right > 0)
        fitted_.draw(canvas, {rect.x + left, rect.y, rect.width - left - right, rect.height}, sliced.middle);
    fitted_.draw(canvas, {rect.x + rect.width - right, rect.y, right, rect.height}, sliced.right);
}

void GameMenuPainter::draw_plain_button(const kit::Canvas& canvas, const kit::Rect& rect, std::size_t frame) const {
    if (plain_buttons_.empty() || rect.height <= 0)
        return;
    const double shape = static_cast<double>(rect.width) / rect.height;
    const auto* best = &plain_buttons_.front();
    double nearest_shape = std::numeric_limits<double>::max();
    for (const auto& button : plain_buttons_) {
        const Sliced& up = button[0];
        const double width = static_cast<double>(up.left.width + up.middle.width + up.right.width);
        const double difference = std::abs(width / std::max<uint32_t>(up.middle.height, 1) - shape);
        if (difference < nearest_shape) {
            nearest_shape = difference;
            best = &button;
        }
    }
    draw_sliced(canvas, rect, (*best)[std::min<std::size_t>(frame, 2)]);
}

void GameMenuPainter::draw_captioned_button(
    const kit::Canvas& canvas, const kit::Rect& rect, std::string_view caption, bool usable, bool lit, bool held
) const {
    draw_plain_button(canvas, rect, !usable ? 2 : held ? 1 : 0);
    const int32_t inset = std::max(rect.height / 5, 2);
    draw_text(
        canvas,
        {rect.x + inset, rect.y, rect.width - 2 * inset, rect.height},
        caption,
        kit::Align::centre,
        !usable ? Ink::faded : lit ? Ink::lit : Ink::plain,
        TextEdge::smoothed
    );
}

int32_t GameMenuPainter::big_button_width(int32_t height) const noexcept {
    if (art_.plate.height == 0)
        return height;
    return nearest(static_cast<double>(height) * art_.plate.width / art_.plate.height);
}

void GameMenuPainter::draw_big_button(
    const kit::Canvas& canvas, const kit::Rect& rect, std::string_view caption, bool usable, bool lit, bool held
) const {
    if (art_.plate.height == 0)
        return;
    fitted_.draw(canvas, rect, art_.plate);
    const double across = static_cast<double>(rect.width) / art_.plate.width;
    const double down = static_cast<double>(rect.height) / art_.plate.height;
    const auto [x, y, width, height] = art_.plate_slot;
    const kit::Rect slot{
        rect.x + nearest(across * x), rect.y + nearest(down * y), nearest(across * width), nearest(down * height)
    };
    draw_plain_button(canvas, slot, !usable ? 2 : held ? 1 : 0);
    // The caption fills the button as the Options screen's do, and sinks a
    // pixel right and down while it is held.
    const int32_t sink = held ? std::max(nearest(down), 1) : 0;
    draw_text(
        canvas,
        {slot.x + sink, slot.y + sink, slot.width, slot.height},
        caption,
        kit::Align::centre,
        !usable ? Ink::faded : lit ? Ink::lit : Ink::plain,
        TextEdge::smoothed
    );
}

void GameMenuPainter::draw_well(const kit::Canvas& canvas, const kit::Rect& rect) const {
    draw_sliced(canvas, rect, well_);
}

void GameMenuPainter::draw_box(const kit::Canvas& canvas, const kit::Rect& rect) const {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const auto& pieces = art_.list_box;
    // The corners and the bevel keep the picture's scale, a pixel at least;
    // the sides and the middle stretch between them.
    const int32_t side = std::max(nearest(picture_scale() * pieces[0].width), 1);
    const int32_t left = std::min(side, rect.width / 2);
    const int32_t top = std::min(side, rect.height / 2);
    const std::array<int32_t, 3> xs{rect.x, rect.x + left, rect.x + rect.width - left};
    const std::array<int32_t, 3> ys{rect.y, rect.y + top, rect.y + rect.height - top};
    const std::array<int32_t, 3> widths{left, rect.width - 2 * left, left};
    const std::array<int32_t, 3> heights{top, rect.height - 2 * top, top};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            if (widths[column] > 0 && heights[row] > 0)
                fitted_.draw(
                    canvas, {xs[column], ys[row], widths[column], heights[row]}, pieces[row * 3 + column]
                );
}

void GameMenuPainter::draw_check_box(const kit::Canvas& canvas, const kit::Rect& rect, bool on) const {
    fitted_.draw(canvas, rect, art_.check_box[on ? 1 : 0]);
}

void GameMenuPainter::repeat(
    const kit::Canvas& canvas, const kit::Rect& along, const WindowIcon& piece, bool across
) const {
    const double scale = picture_scale();
    const int32_t step = std::max(nearest(scale * static_cast<double>(across ? piece.width : piece.height)), 1);
    const kit::Canvas inside = kit::clipped(canvas, along);
    const int32_t end = across ? along.x + along.width : along.y + along.height;
    for (int32_t at = across ? along.x : along.y; at < end; at += step)
        fitted_.draw(
            inside,
            across ? kit::Rect{at, along.y, step, along.height} : kit::Rect{along.x, at, along.width, step},
            piece
        );
}

bool GameMenuPainter::draw_frame(const kit::Canvas& canvas, const kit::Rect& panel) const {
    if (frame_piece(FramePiece::top_left).width == 0)
        return false;
    const double scale = picture_scale();
    const auto points = [scale](int32_t pixels) { return nearest(scale * pixels); };
    const auto& reach = art_.frame_reach;
    const auto& corners = art_.frame_corners;
    const kit::Rect outer{
        panel.x - points(reach[0]),
        panel.y - points(reach[1]),
        panel.width + points(reach[0]) + points(reach[2]),
        panel.height + points(reach[1]) + points(reach[3])
    };
    const int32_t left = points(corners[0]);
    const int32_t top = points(corners[1]);
    const int32_t right = points(corners[2]);
    const int32_t bottom = points(corners[3]);
    const int32_t across = outer.width - left - right;
    const int32_t down = outer.height - top - bottom;
    if (across <= 0 || down <= 0)
        return false;
    const auto& ground = art_.frame_ground;
    kit::fill_rect(canvas, {outer.x + left, outer.y + top, across, down}, {ground[0], ground[1], ground[2]});
    // Each side repeats its stretch, at the picture's scale, as far as the
    // side reaches.
    repeat(canvas, {outer.x + left, outer.y, across, top}, frame_piece(FramePiece::top), true);
    repeat(canvas, {outer.x + left, outer.y + outer.height - bottom, across, bottom}, frame_piece(FramePiece::bottom), true);
    repeat(canvas, {outer.x, outer.y + top, left, down}, frame_piece(FramePiece::left), false);
    repeat(canvas, {outer.x + outer.width - right, outer.y + top, right, down}, frame_piece(FramePiece::right), false);
    fitted_.draw(canvas, {outer.x, outer.y, left, top}, frame_piece(FramePiece::top_left));
    fitted_.draw(canvas, {outer.x + outer.width - right, outer.y, right, top}, frame_piece(FramePiece::top_right));
    fitted_.draw(canvas, {outer.x, outer.y + outer.height - bottom, left, bottom}, frame_piece(FramePiece::bottom_left));
    fitted_.draw(
        canvas,
        {outer.x + outer.width - right, outer.y + outer.height - bottom, right, bottom},
        frame_piece(FramePiece::bottom_right)
    );
    return true;
}

bool GameMenuPainter::draw_rule(const kit::Canvas& canvas, const kit::Rect& panel, int32_t row) const {
    if (art_.rule.width == 0)
        return false;
    const double scale = picture_scale();
    const int32_t height = std::max(nearest(scale * art_.rule.height), 1);
    const int32_t left = panel.x + nearest(scale * art_.rule_inset[0]);
    const int32_t right = panel.x + panel.width - nearest(scale * art_.rule_inset[1]);
    repeat(canvas, {left, row - height / 2, right - left, height}, art_.rule, true);
    return true;
}

const WindowIcon& GameMenuPainter::slider_piece(SliderPiece piece, bool held) const {
    return art_.sliders[static_cast<std::size_t>(piece) + (held ? 1U : 0U)];
}

double GameMenuPainter::piece_scale(int32_t room, uint32_t pixels) const noexcept {
    return std::min(picture_scale(), static_cast<double>(room) / std::max<uint32_t>(pixels, 1));
}

kit::Rect GameMenuPainter::piece_place(kit::Point at, const WindowIcon& piece, double scale) noexcept {
    return {at.x, at.y, std::max(nearest(scale * piece.width), 1), std::max(nearest(scale * piece.height), 1)};
}

void GameMenuPainter::draw_scroll_bar(
    const kit::Canvas& canvas, const kit::Rect& bar, const kit::Rect& up, const kit::Rect& down,
    const kit::Rect& well, const kit::Rect& thumb, bool up_held, bool down_held
) const {
    const double scale =
        static_cast<double>(bar.width) / std::max<uint32_t>(slider_piece(SliderPiece::upright_track).width, 1);
    for (const bool upper : {true, false}) {
        const WindowIcon& arrow =
            slider_piece(upper ? SliderPiece::up : SliderPiece::down, upper ? up_held : down_held);
        const kit::Rect& rect = upper ? up : down;
        kit::Rect place = piece_place({bar.x, 0}, arrow, scale);
        place.y = upper ? rect.y : rect.y + rect.height - place.height;
        fitted_.draw(canvas, place, arrow);
    }
    const WindowIcon& start = slider_piece(SliderPiece::upright_start);
    const WindowIcon& track = slider_piece(SliderPiece::upright_track);
    const WindowIcon& end = slider_piece(SliderPiece::upright_end);
    const kit::Rect first = piece_place({bar.x, well.y}, start, scale);
    kit::Rect last = piece_place({bar.x, 0}, end, scale);
    last.y = well.y + well.height - last.height;
    const kit::Rect between{bar.x, first.y + first.height, bar.width, last.y - first.y - first.height};
    const int32_t step = std::max(nearest(scale * track.height), 1);
    const kit::Canvas inside = kit::clipped(canvas, between);
    for (int32_t y = between.y; y < between.y + between.height; y += step)
        fitted_.draw(inside, {bar.x, y, bar.width, step}, track);
    fitted_.draw(canvas, first, start);
    fitted_.draw(canvas, last, end);
    const WindowIcon& knob_start = slider_piece(SliderPiece::upright_knob_start);
    const WindowIcon& knob_middle = slider_piece(SliderPiece::upright_knob_middle);
    const WindowIcon& knob_end = slider_piece(SliderPiece::upright_knob_end);
    const int32_t width = std::max(nearest(scale * knob_middle.width), 1);
    const int32_t x = bar.x + (bar.width - width) / 2;
    const int32_t cap = std::max(nearest(scale * knob_start.height), 1);
    fitted_.draw(canvas, {x, thumb.y, width, cap}, knob_start);
    fitted_.draw(canvas, {x, thumb.y + cap, width, std::max(thumb.height - 2 * cap, 1)}, knob_middle);
    fitted_.draw(canvas, {x, thumb.y + thumb.height - cap, width, cap}, knob_end);
}

} // namespace oa::app
