// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-steam-artwork: makes the pictures Steam shows for Open Annihilation's
// shortcut from the engine's icon in branding/, at packaging time, so that
// no copy of the art is kept in the repository (cmake/OaSteamDeck.cmake,
// docs/installation/steam-deck.md). It writes, into the output folder:
//
//   portrait.png  600 x 900, the library's portrait capsule
//   wide.png      920 x 430, the wide capsule
//   hero.png      1920 x 620, the banner behind the logo on the game's page
//   logo.png      the icon and the name on a transparent background
//   icon.png      256 x 256, the shortcut's icon
//
// Each but the logo is the icon on the Game files screen's dark background,
// with the name "OPEN ANNIHILATION" in the bundled bold font on the two
// capsules. Nothing of Total Annihilation is drawn.
#include "oa/formats/png.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/game_files.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace png = oa::formats::png;
namespace text_font = oa::platform::text_font;
namespace game_files = oa::ui::game_files;

/// The exit status of a run that wrote every picture.
constexpr int exit_done = 0;
/// The exit status of a run that could not read the icon or the fonts, or write a picture.
constexpr int exit_failed = 1;
/// The exit status of a command line the tool refuses.
constexpr int exit_usage = 2;

/// The largest icon file the tool reads, in bytes.
constexpr std::size_t max_icon_bytes = std::size_t{64} << 20;
/// The longest side of an icon the tool takes, in pixels.
constexpr uint32_t max_icon_side = 8192;
/// Bytes per pixel of every picture: red, green, blue and opacity.
constexpr std::size_t pixel_bytes = 4;
/// The largest value of a channel.
constexpr float channel_max = 255.0f;

/// The name the pictures show, on two lines.
constexpr std::string_view name_first_line = "OPEN";
constexpr std::string_view name_second_line = "ANNIHILATION";
/// Pixels added after each letter of the name.
constexpr int32_t name_letter_spacing = 3;
/// The smallest pixel size the name is drawn at when it is fitted to a width.
constexpr int32_t smallest_name_size = 12;
/// The gap between the name's two lines, as a share of the first line's ink height.
constexpr float name_line_gap = 0.42f;

/// The portrait capsule.
constexpr int32_t portrait_width = 600;
constexpr int32_t portrait_height = 900;
constexpr int32_t portrait_icon = 400;
constexpr int32_t portrait_icon_top = 140;
constexpr int32_t portrait_name_top = 600;
constexpr int32_t portrait_name_width = 500;
constexpr int32_t portrait_name_size = 72;

/// The wide capsule.
constexpr int32_t wide_width = 920;
constexpr int32_t wide_height = 430;
constexpr int32_t wide_icon = 330;
constexpr int32_t wide_icon_left = 60;
constexpr int32_t wide_name_left = 440;
constexpr int32_t wide_name_width = 430;
constexpr int32_t wide_name_size = 72;

/// The hero: the icon at the right, leaving the left free for the logo Steam draws over it.
constexpr int32_t hero_width = 1920;
constexpr int32_t hero_height = 620;
constexpr int32_t hero_icon = 460;
constexpr int32_t hero_icon_centre_x = 1440;

/// The logo: the icon and the name on a transparent background, as wide as they need.
constexpr int32_t logo_height = 360;
constexpr int32_t logo_icon = 320;
constexpr int32_t logo_gap = 40;
constexpr int32_t logo_name_size = 104;
constexpr int32_t logo_margin = 16;
/// The dark edge drawn around the logo's name, so it reads on any picture behind it.
constexpr int32_t logo_outline_radius = 4;
constexpr float logo_outline_opacity = 0.85f;

/// The shortcut's icon.
constexpr int32_t icon_size = 256;
constexpr int32_t icon_margin = 10;

/// The glow behind the icon: its radius as a share of the icon's size, and its strength.
constexpr float glow_radius = 1.3f;
constexpr float glow_strength = 0.75f;
/// The shadow under the capsules' name: its offset in pixels and its opacity.
constexpr int32_t shadow_offset_x = 2;
constexpr int32_t shadow_offset_y = 3;
constexpr float shadow_opacity = 0.6f;
/// The green bar under the capsules' name.
constexpr int32_t accent_width = 120;
constexpr int32_t accent_height = 6;
constexpr int32_t accent_gap = 34;

/// One picture: 8-bit red, green, blue and opacity per pixel, rows top first, not premultiplied.
struct Picture {
    int32_t width{};
    int32_t height{};
    std::vector<uint8_t> rgba{};
};

/// The ink of a drawn line: the rows and columns of its coverage that any glyph touches.
struct Ink {
    int32_t left{};
    int32_t top{};
    int32_t width{};
    int32_t height{};
};

/// A line of text drawn at a size, with its ink.
struct DrawnLine {
    text_font::Coverage coverage{};
    Ink ink{};
    int32_t pixel_size{}; ///< the size it was drawn at
};

/// What the command line asks for.
struct Options {
    std::filesystem::path icon{};
    std::filesystem::path output{};
    std::filesystem::path fonts{};
};

/// Writes how the tool is used.
///
/// @param out where to write it
void print_usage(std::ostream& out) {
    out << "usage: oa-steam-artwork --icon PNG --output FOLDER [--fonts FOLDER]\n"
           "  Makes Steam's pictures for Open Annihilation from its icon: portrait.png, wide.png,\n"
           "  hero.png, logo.png and icon.png in FOLDER. --fonts names the folder of the bundled\n"
           "  fonts (default: the fonts folder beside the tool).\n";
}

/// Makes a picture of one colour.
///
/// @param width columns
/// @param height rows
/// @param colour the colour every pixel takes
/// @return the picture
Picture filled(int32_t width, int32_t height, game_files::Colour colour) {
    Picture picture{width, height, {}};
    picture.rgba.resize(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * pixel_bytes
    );
    for (std::size_t at = 0; at < picture.rgba.size(); at += pixel_bytes) {
        picture.rgba[at] = colour.r;
        picture.rgba[at + 1] = colour.g;
        picture.rgba[at + 2] = colour.b;
        picture.rgba[at + 3] = colour.a;
    }
    return picture;
}

/// Reads a PNG file into a picture, whatever its colour type and depth.
///
/// @param path the file
/// @param[out] error why it could not be read
/// @return the picture; empty when the file is missing, too large or not a PNG the reader takes
std::optional<Picture> read_png(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot open " + oa::platform::path_to_utf8(path);
        return std::nullopt;
    }
    std::vector<uint8_t> file;
    char byte{};
    while (in.get(byte)) {
        if (file.size() >= max_icon_bytes) {
            *error = oa::platform::path_to_utf8(path) + " is larger than the icon limit";
            return std::nullopt;
        }
        file.push_back(static_cast<uint8_t>(byte));
    }
    std::string fault;
    png::Messages messages{
        &fault,
        [](void*, const char*) {},
        [](void* user, const char* text) { *static_cast<std::string*>(user) = text; },
    };
    png::Info info{};
    if (!png::read_info(file, messages, &info)) {
        *error = oa::platform::path_to_utf8(path) + ": " + fault;
        return std::nullopt;
    }
    const png::Header header = png::header_of(info, messages);
    if (header.width == 0 || header.height == 0 || header.width > max_icon_side ||
        header.height > max_icon_side) {
        *error = oa::platform::path_to_utf8(path) + ": the picture's size is out of range";
        return std::nullopt;
    }
    const png::Transforms transforms{true, true};
    const std::size_t row = png::row_bytes(header, transforms);
    std::vector<uint8_t> rows(row * header.height);
    if (png::read_image(file, info, transforms, messages, rows) == png::Progress::none) {
        *error = oa::platform::path_to_utf8(path) + ": " +
                 (fault.empty() ? std::string("its image data ends early") : fault);
        return std::nullopt;
    }
    const uint32_t channels = png::channel_count(header.color_type);
    Picture picture =
        filled(static_cast<int32_t>(header.width), static_cast<int32_t>(header.height), {});
    for (uint32_t y = 0; y < header.height; ++y) {
        for (uint32_t x = 0; x < header.width; ++x) {
            const uint8_t* sample = &rows[y * row + x * channels];
            uint8_t* pixel =
                &picture.rgba[(static_cast<std::size_t>(y) * header.width + x) * pixel_bytes];
            switch (header.color_type) {
            case png::ColorType::grey:
            case png::ColorType::grey_alpha:
                pixel[0] = pixel[1] = pixel[2] = sample[0];
                pixel[3] =
                    header.color_type == png::ColorType::grey_alpha ? sample[1] : uint8_t{255};
                break;
            case png::ColorType::palette: {
                const png::Rgb entry = info.palette[sample[0]];
                pixel[0] = entry.r;
                pixel[1] = entry.g;
                pixel[2] = entry.b;
                pixel[3] = 255;
                break;
            }
            case png::ColorType::rgb:
            case png::ColorType::rgb_alpha:
                pixel[0] = sample[0];
                pixel[1] = sample[1];
                pixel[2] = sample[2];
                pixel[3] =
                    header.color_type == png::ColorType::rgb_alpha ? sample[3] : uint8_t{255};
                break;
            }
        }
    }
    return picture;
}

/// Gives the weights a tent filter gives the source samples that make one output sample.
///
/// @param output the output sample's index
/// @param scale source samples per output sample
/// @param source_size how many source samples there are
/// @param[out] first the index of the first weighted source sample
/// @return the weights, summing to 1
std::vector<float> tent_weights(int32_t output, float scale, int32_t source_size, int32_t* first) {
    const float centre = (static_cast<float>(output) + 0.5f) * scale - 0.5f;
    const float radius = std::max(scale, 1.0f);
    const int32_t low = std::max(0, static_cast<int32_t>(std::floor(centre - radius)));
    const int32_t high =
        std::min(source_size - 1, static_cast<int32_t>(std::ceil(centre + radius)));
    std::vector<float> weights;
    float total = 0.0f;
    for (int32_t at = low; at <= high; ++at) {
        const float weight =
            std::max(0.0f, 1.0f - std::fabs(static_cast<float>(at) - centre) / radius);
        weights.push_back(weight);
        total += weight;
    }
    if (total <= 0.0f) {
        weights.assign(1, 1.0f);
        *first = std::clamp(static_cast<int32_t>(std::lround(centre)), 0, source_size - 1);
        return weights;
    }
    for (float& weight : weights)
        weight /= total;
    *first = low;
    return weights;
}

/// Scales a picture to a size with a tent filter over premultiplied colour, which averages every
/// source pixel a smaller picture covers.
///
/// @param source the picture
/// @param width the new width
/// @param height the new height
/// @return the scaled picture
Picture scaled(const Picture& source, int32_t width, int32_t height) {
    const float scale_x = static_cast<float>(source.width) / static_cast<float>(width);
    const float scale_y = static_cast<float>(source.height) / static_cast<float>(height);
    // Rows of the source, columns already scaled: premultiplied red, green, blue and opacity, 0 to 1.
    std::vector<float> across(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(source.height) * pixel_bytes
    );
    for (int32_t x = 0; x < width; ++x) {
        int32_t first{};
        const std::vector<float> weights = tent_weights(x, scale_x, source.width, &first);
        for (int32_t y = 0; y < source.height; ++y) {
            float sum[pixel_bytes]{};
            for (std::size_t tap = 0; tap < weights.size(); ++tap) {
                const uint8_t* pixel =
                    &source.rgba
                         [(static_cast<std::size_t>(y) * source.width + first + tap) * pixel_bytes];
                const float opacity = static_cast<float>(pixel[3]) / channel_max;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    sum[channel] +=
                        weights[tap] * opacity * static_cast<float>(pixel[channel]) / channel_max;
                sum[3] += weights[tap] * opacity;
            }
            std::copy(
                std::begin(sum),
                std::end(sum),
                across.begin() + static_cast<std::ptrdiff_t>(
                                     (static_cast<std::size_t>(y) * width + x) * pixel_bytes
                                 )
            );
        }
    }
    Picture result = filled(width, height, {0, 0, 0, 0});
    for (int32_t y = 0; y < height; ++y) {
        int32_t first{};
        const std::vector<float> weights = tent_weights(y, scale_y, source.height, &first);
        for (int32_t x = 0; x < width; ++x) {
            float sum[pixel_bytes]{};
            for (std::size_t tap = 0; tap < weights.size(); ++tap) {
                const float* pixel =
                    &across[((first + tap) * static_cast<std::size_t>(width) + x) * pixel_bytes];
                for (std::size_t channel = 0; channel < pixel_bytes; ++channel)
                    sum[channel] += weights[tap] * pixel[channel];
            }
            uint8_t* out = &result.rgba[(static_cast<std::size_t>(y) * width + x) * pixel_bytes];
            const float opacity = std::clamp(sum[3], 0.0f, 1.0f);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float value = opacity > 0.0f ? sum[channel] / opacity : 0.0f;
                out[channel] =
                    static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * channel_max));
            }
            out[3] = static_cast<uint8_t>(std::lround(opacity * channel_max));
        }
    }
    return result;
}

/// Lays one colour over a pixel of a picture.
///
/// @param[in,out] picture the picture
/// @param x the pixel's column; outside the picture nothing is drawn
/// @param y the pixel's row; outside the picture nothing is drawn
/// @param red the colour's red, 0 to 255
/// @param green the colour's green, 0 to 255
/// @param blue the colour's blue, 0 to 255
/// @param opacity how much of the colour covers the pixel, 0 to 1
void blend_pixel(
    Picture& picture, int32_t x, int32_t y, float red, float green, float blue, float opacity
) {
    if (x < 0 || y < 0 || x >= picture.width || y >= picture.height || opacity <= 0.0f)
        return;
    uint8_t* pixel = &picture.rgba[(static_cast<std::size_t>(y) * picture.width + x) * pixel_bytes];
    const float below = static_cast<float>(pixel[3]) / channel_max;
    const float result = opacity + below * (1.0f - opacity);
    if (result <= 0.0f)
        return;
    const float source[3]{red, green, blue};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float value = (source[channel] * opacity +
                             static_cast<float>(pixel[channel]) * below * (1.0f - opacity)) /
                            result;
        pixel[channel] = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, channel_max)));
    }
    pixel[3] = static_cast<uint8_t>(std::lround(std::clamp(result, 0.0f, 1.0f) * channel_max));
}

/// Lays a picture over another.
///
/// @param[in,out] canvas the picture drawn on
/// @param layer the picture laid over it
/// @param left the canvas column the layer's left edge lands on
/// @param top the canvas row the layer's top edge lands on
void draw_over(Picture& canvas, const Picture& layer, int32_t left, int32_t top) {
    for (int32_t y = 0; y < layer.height; ++y) {
        for (int32_t x = 0; x < layer.width; ++x) {
            const uint8_t* pixel =
                &layer.rgba[(static_cast<std::size_t>(y) * layer.width + x) * pixel_bytes];
            blend_pixel(
                canvas, left + x, top + y, pixel[0], pixel[1], pixel[2], pixel[3] / channel_max
            );
        }
    }
}

/// Lightens a picture towards the outline colour around a point, softly.
///
/// @param[in,out] canvas the picture
/// @param centre_x the glow's centre column
/// @param centre_y the glow's centre row
/// @param radius how far the glow reaches, in pixels
void draw_glow(Picture& canvas, int32_t centre_x, int32_t centre_y, float radius) {
    const game_files::Colour colour = game_files::line_colour;
    for (int32_t y = 0; y < canvas.height; ++y) {
        for (int32_t x = 0; x < canvas.width; ++x) {
            const float distance =
                std::hypot(static_cast<float>(x - centre_x), static_cast<float>(y - centre_y));
            const float near = std::clamp(1.0f - distance / radius, 0.0f, 1.0f);
            const float eased = near * near * (3.0f - 2.0f * near);
            blend_pixel(canvas, x, y, colour.r, colour.g, colour.b, eased * glow_strength);
        }
    }
}

/// Fills a rectangle of a picture with one colour.
///
/// @param[in,out] canvas the picture
/// @param left the rectangle's left column
/// @param top the rectangle's top row
/// @param width its width in pixels
/// @param height its height in pixels
/// @param colour the colour
void fill_rect(
    Picture& canvas,
    int32_t left,
    int32_t top,
    int32_t width,
    int32_t height,
    game_files::Colour colour
) {
    for (int32_t y = top; y < top + height; ++y)
        for (int32_t x = left; x < left + width; ++x)
            blend_pixel(canvas, x, y, colour.r, colour.g, colour.b, colour.a / channel_max);
}

/// Finds the ink of a drawn line.
///
/// @param coverage the line
/// @return the rows and columns any glyph covers; all zero for a line with no ink
Ink ink_of(const text_font::Coverage& coverage) {
    int32_t left = coverage.width;
    int32_t right = -1;
    int32_t top = coverage.height;
    int32_t bottom = -1;
    for (int32_t y = 0; y < coverage.height; ++y) {
        for (int32_t x = 0; x < coverage.width; ++x) {
            if (coverage.alpha[static_cast<std::size_t>(y) * coverage.width + x] == 0)
                continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (right < 0)
        return {};
    return {left, top, right - left + 1, bottom - top + 1};
}

/// Draws a line of the name in the bundled bold font, at the largest size up to a wanted one whose ink
/// fits a width.
///
/// @param stack the fonts
/// @param text the line
/// @param wanted_size the pixel size wanted
/// @param max_width the widest the ink may be, in pixels
/// @return the drawn line; empty when the fonts draw nothing
std::optional<DrawnLine> draw_fitted(
    text_font::FontStack& stack, std::string_view text, int32_t wanted_size, int32_t max_width
) {
    for (int32_t size = std::min(wanted_size, text_font::max_pixel_size);
         size >= smallest_name_size;
         --size) {
        text_font::Style style{};
        style.pixel_size = size;
        style.weight = text_font::Weight::bold;
        style.rendering = text_font::Rendering::antialiased;
        style.letter_spacing = name_letter_spacing;
        std::optional<text_font::Coverage> coverage = stack.draw(text, style);
        if (!coverage)
            return std::nullopt;
        const Ink ink = ink_of(*coverage);
        if (ink.width <= max_width || size == smallest_name_size)
            return DrawnLine{std::move(*coverage), ink, size};
    }
    return std::nullopt;
}

/// Draws the name's two lines at one size: the largest up to a wanted one at which both fit a width.
///
/// @param stack the fonts
/// @param wanted_size the pixel size wanted
/// @param max_width the widest either line's ink may be, in pixels
/// @param[out] lines the two drawn lines
/// @return false when the fonts draw nothing
bool draw_name(
    text_font::FontStack& stack,
    int32_t wanted_size,
    int32_t max_width,
    std::vector<DrawnLine>* lines
) {
    // The second line is the longer, so the first fits at the size the second fitted at.
    std::optional<DrawnLine> second = draw_fitted(stack, name_second_line, wanted_size, max_width);
    if (!second)
        return false;
    std::optional<DrawnLine> first =
        draw_fitted(stack, name_first_line, second->pixel_size, max_width);
    if (!first)
        return false;
    lines->clear();
    lines->push_back(std::move(*first));
    lines->push_back(std::move(*second));
    return true;
}

/// Draws a line's ink in a colour, its ink's top-left corner at a point.
///
/// @param[in,out] canvas the picture
/// @param line the drawn line
/// @param left the canvas column the ink's left edge lands on
/// @param top the canvas row the ink's top edge lands on
/// @param colour the colour
/// @param opacity the colour's opacity where the line covers a pixel fully, 0 to 1
void draw_line(
    Picture& canvas,
    const DrawnLine& line,
    int32_t left,
    int32_t top,
    game_files::Colour colour,
    float opacity
) {
    const text_font::Coverage& coverage = line.coverage;
    for (int32_t y = 0; y < coverage.height; ++y) {
        for (int32_t x = 0; x < coverage.width; ++x) {
            const uint8_t covered =
                coverage.alpha[static_cast<std::size_t>(y) * coverage.width + x];
            blend_pixel(
                canvas,
                left - line.ink.left + x,
                top - line.ink.top + y,
                colour.r,
                colour.g,
                colour.b,
                opacity * covered / channel_max
            );
        }
    }
}

/// Draws a dark edge around a line's ink: the ink widened by a radius, in the background colour.
///
/// @param[in,out] canvas the picture
/// @param line the drawn line
/// @param left the canvas column the ink's left edge lands on
/// @param top the canvas row the ink's top edge lands on
/// @param radius how far the edge reaches past the ink, in pixels
void draw_outline(
    Picture& canvas, const DrawnLine& line, int32_t left, int32_t top, int32_t radius
) {
    const text_font::Coverage& coverage = line.coverage;
    const game_files::Colour colour = game_files::background_colour;
    for (int32_t y = -radius; y < coverage.height + radius; ++y) {
        for (int32_t x = -radius; x < coverage.width + radius; ++x) {
            uint8_t widest = 0;
            for (int32_t dy = -radius; dy <= radius; ++dy) {
                for (int32_t dx = -radius; dx <= radius; ++dx) {
                    const int32_t sx = x + dx;
                    const int32_t sy = y + dy;
                    if (dx * dx + dy * dy > radius * radius || sx < 0 || sy < 0 ||
                        sx >= coverage.width || sy >= coverage.height)
                        continue;
                    widest = std::max(
                        widest, coverage.alpha[static_cast<std::size_t>(sy) * coverage.width + sx]
                    );
                }
            }
            blend_pixel(
                canvas,
                left - line.ink.left + x,
                top - line.ink.top + y,
                colour.r,
                colour.g,
                colour.b,
                logo_outline_opacity * widest / channel_max
            );
        }
    }
}

/// Gives the height of the name's two lines laid one above the other.
///
/// @param lines the two drawn lines
/// @return the rows from the first line's ink top to the second line's ink bottom
int32_t name_height(const std::vector<DrawnLine>& lines) {
    const int32_t gap = static_cast<int32_t>(std::lround(lines[0].ink.height * name_line_gap));
    return lines[0].ink.height + gap + lines[1].ink.height;
}

/// Draws the name's two lines, light with a soft shadow, each centred on a column or from a left edge.
///
/// @param[in,out] canvas the picture
/// @param lines the two drawn lines
/// @param x the column the lines are centred on, or their left edge
/// @param top the row of the first line's ink top
/// @param centred true to centre each line on x, false to start each at x
/// @return the row below the second line's ink
int32_t draw_name_lines(
    Picture& canvas, const std::vector<DrawnLine>& lines, int32_t x, int32_t top, bool centred
) {
    const int32_t gap = static_cast<int32_t>(std::lround(lines[0].ink.height * name_line_gap));
    int32_t row = top;
    for (const DrawnLine& line : lines) {
        const int32_t left = centred ? x - line.ink.width / 2 : x;
        draw_line(
            canvas,
            line,
            left + shadow_offset_x,
            row + shadow_offset_y,
            {0, 0, 0, 255},
            shadow_opacity
        );
        draw_line(canvas, line, left, row, game_files::text_colour, 1.0f);
        row += line.ink.height + gap;
    }
    return row - gap;
}

/// Makes the portrait capsule.
///
/// @param icon the icon
/// @param stack the fonts
/// @return the picture; empty when the fonts draw nothing
std::optional<Picture> make_portrait(const Picture& icon, text_font::FontStack& stack) {
    Picture canvas = filled(portrait_width, portrait_height, game_files::background_colour);
    const int32_t centre_x = portrait_width / 2;
    draw_glow(canvas, centre_x, portrait_icon_top + portrait_icon / 2, portrait_icon * glow_radius);
    draw_over(
        canvas,
        scaled(icon, portrait_icon, portrait_icon),
        centre_x - portrait_icon / 2,
        portrait_icon_top
    );
    std::vector<DrawnLine> lines;
    if (!draw_name(stack, portrait_name_size, portrait_name_width, &lines))
        return std::nullopt;
    const int32_t bottom = draw_name_lines(canvas, lines, centre_x, portrait_name_top, true);
    fill_rect(
        canvas,
        centre_x - accent_width / 2,
        bottom + accent_gap,
        accent_width,
        accent_height,
        game_files::green_colour
    );
    return canvas;
}

/// Makes the wide capsule.
///
/// @param icon the icon
/// @param stack the fonts
/// @return the picture; empty when the fonts draw nothing
std::optional<Picture> make_wide(const Picture& icon, text_font::FontStack& stack) {
    Picture canvas = filled(wide_width, wide_height, game_files::background_colour);
    const int32_t icon_top = (wide_height - wide_icon) / 2;
    draw_glow(canvas, wide_icon_left + wide_icon / 2, wide_height / 2, wide_icon * glow_radius);
    draw_over(canvas, scaled(icon, wide_icon, wide_icon), wide_icon_left, icon_top);
    std::vector<DrawnLine> lines;
    if (!draw_name(stack, wide_name_size, wide_name_width, &lines))
        return std::nullopt;
    const int32_t block = name_height(lines) + accent_gap + accent_height;
    const int32_t bottom =
        draw_name_lines(canvas, lines, wide_name_left, (wide_height - block) / 2, false);
    fill_rect(
        canvas,
        wide_name_left,
        bottom + accent_gap,
        accent_width,
        accent_height,
        game_files::green_colour
    );
    return canvas;
}

/// Makes the hero: the icon at the right on the dark background, with no text, since Steam draws the
/// logo over it.
///
/// @param icon the icon
/// @return the picture
Picture make_hero(const Picture& icon) {
    Picture canvas = filled(hero_width, hero_height, game_files::background_colour);
    draw_glow(canvas, hero_icon_centre_x, hero_height / 2, hero_icon * glow_radius);
    draw_over(
        canvas,
        scaled(icon, hero_icon, hero_icon),
        hero_icon_centre_x - hero_icon / 2,
        (hero_height - hero_icon) / 2
    );
    return canvas;
}

/// Makes the logo: the icon and the name, with a dark edge, on a transparent background as wide as they
/// need.
///
/// @param icon the icon
/// @param stack the fonts
/// @return the picture; empty when the fonts draw nothing
std::optional<Picture> make_logo(const Picture& icon, text_font::FontStack& stack) {
    std::vector<DrawnLine> lines;
    if (!draw_name(stack, logo_name_size, text_font::max_line_width, &lines))
        return std::nullopt;
    const int32_t name_width = std::max(lines[0].ink.width, lines[1].ink.width);
    const int32_t name_left = logo_margin + logo_icon + logo_gap;
    Picture canvas = filled(
        name_left + name_width + logo_outline_radius + logo_margin, logo_height, {0, 0, 0, 0}
    );
    draw_over(
        canvas, scaled(icon, logo_icon, logo_icon), logo_margin, (logo_height - logo_icon) / 2
    );
    const int32_t gap = static_cast<int32_t>(std::lround(lines[0].ink.height * name_line_gap));
    int32_t row = (logo_height - name_height(lines)) / 2;
    for (const DrawnLine& line : lines) {
        draw_outline(canvas, line, name_left, row, logo_outline_radius);
        draw_line(canvas, line, name_left, row, game_files::text_colour, 1.0f);
        row += line.ink.height + gap;
    }
    return canvas;
}

/// Makes the shortcut's icon: the icon on the dark background.
///
/// @param icon the icon
/// @return the picture
Picture make_icon(const Picture& icon) {
    Picture canvas = filled(icon_size, icon_size, game_files::background_colour);
    const int32_t inner = icon_size - 2 * icon_margin;
    draw_over(canvas, scaled(icon, inner, inner), icon_margin, icon_margin);
    return canvas;
}

/// Writes a picture as an 8-bit RGBA PNG file.
///
/// @param picture the picture
/// @param path the file
/// @param[out] error why it could not be written
/// @return false when it could not be encoded or written
bool write_png(const Picture& picture, const std::filesystem::path& path, std::string* error) {
    png::Image image{};
    image.header.width = static_cast<uint32_t>(picture.width);
    image.header.height = static_cast<uint32_t>(picture.height);
    image.header.bit_depth = 8;
    image.header.color_type = png::ColorType::rgb_alpha;
    image.header.interlace = png::Interlace::none;
    image.rows = picture.rgba;
    std::vector<uint8_t> file;
    if (!png::write(image, &file)) {
        *error = "cannot encode " + oa::platform::path_to_utf8(path);
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!out) {
        *error = "cannot write " + oa::platform::path_to_utf8(path);
        return false;
    }
    return true;
}

/// Reads the command line.
///
/// @param arguments the arguments after the program's name
/// @param[out] options what they ask for
/// @param[out] error why they were refused
/// @return false when they are refused, or when they ask for the usage (error empty)
bool parse_options(std::span<char* const> arguments, Options* options, std::string* error) {
    for (std::size_t at = 0; at < arguments.size(); ++at) {
        const std::string_view argument = arguments[at];
        if (argument == "--help" || argument == "-h")
            return false;
        std::filesystem::path* target = argument == "--icon"     ? &options->icon
                                        : argument == "--output" ? &options->output
                                        : argument == "--fonts"  ? &options->fonts
                                                                 : nullptr;
        if (target == nullptr) {
            *error = "unknown argument " + std::string(argument);
            return false;
        }
        if (at + 1 >= arguments.size() || std::string_view(arguments[at + 1]).empty()) {
            *error = std::string(argument) + " needs a value";
            return false;
        }
        *target = arguments[++at];
    }
    if (options->icon.empty() || options->output.empty()) {
        *error = "--icon and --output are needed";
        return false;
    }
    return true;
}

} // namespace

/// Makes Steam's pictures for Open Annihilation from its icon.
///
/// @param argc the number of arguments
/// @param argv the arguments: --icon PNG, --output FOLDER and optionally --fonts FOLDER
/// @return exit_done when every picture was written, exit_failed when one could not be, exit_usage for a
///         command line it refuses
int main(int argc, char** argv) {
    Options options{};
    std::string error;
    const std::span<char* const> arguments(
        argv + 1, argc > 0 ? static_cast<std::size_t>(argc - 1) : 0
    );
    if (!parse_options(arguments, &options, &error)) {
        if (error.empty()) {
            print_usage(std::cout);
            return exit_done;
        }
        std::cerr << "oa-steam-artwork: " << error << "\n";
        print_usage(std::cerr);
        return exit_usage;
    }
    std::optional<Picture> icon = read_png(options.icon, &error);
    if (!icon) {
        std::cerr << "oa-steam-artwork: " << error << "\n";
        return exit_failed;
    }
    const std::filesystem::path fonts =
        options.fonts.empty() ? text_font::bundled_font_directory() : options.fonts;
    std::unique_ptr<text_font::FontStack> stack = text_font::FontStack::open(fonts);
    if (!stack) {
        std::cerr << "oa-steam-artwork: cannot open the bundled fonts in "
                  << oa::platform::path_to_utf8(fonts) << "\n";
        return exit_failed;
    }
    std::error_code made;
    std::filesystem::create_directories(options.output, made);
    if (made) {
        std::cerr << "oa-steam-artwork: cannot make " << oa::platform::path_to_utf8(options.output)
                  << ": " << made.message() << "\n";
        return exit_failed;
    }
    std::optional<Picture> portrait = make_portrait(*icon, *stack);
    std::optional<Picture> wide = make_wide(*icon, *stack);
    std::optional<Picture> logo = make_logo(*icon, *stack);
    if (!portrait || !wide || !logo) {
        std::cerr << "oa-steam-artwork: the bundled fonts drew nothing for the name\n";
        return exit_failed;
    }
    const std::pair<const char*, const Picture*> pictures[]{
        {"portrait.png", &*portrait},
        {"wide.png", &*wide},
        {"logo.png", &*logo},
    };
    const Picture hero = make_hero(*icon);
    const Picture small_icon = make_icon(*icon);
    for (const auto& [name, picture] : pictures) {
        if (!write_png(*picture, options.output / name, &error)) {
            std::cerr << "oa-steam-artwork: " << error << "\n";
            return exit_failed;
        }
    }
    if (!write_png(hero, options.output / "hero.png", &error) ||
        !write_png(small_icon, options.output / "icon.png", &error)) {
        std::cerr << "oa-steam-artwork: " << error << "\n";
        return exit_failed;
    }
    std::cout
        << "oa-steam-artwork: wrote portrait.png, wide.png, hero.png, logo.png and icon.png in "
        << oa::platform::path_to_utf8(options.output) << "\n";
    return exit_done;
}
