// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Card command lists run on SDL's software renderer over a surface, with
// no window, and read back against a reference rasteriser on the
// processor: textured and untextured quads and triangles under every blend
// mode, lighten from a page of no alpha alone, premultiplied alpha from a
// premultiplied page and from vertex
// colours among them, with vertex colours, partly off the canvas; a
// scissor; a draw's own sampling mode; a page's levels and a part of a
// level updated; render targets at supersampling factors 2 and 4, cleared,
// drawn into and resolved into the canvas with a downscale, by none and by
// alpha; a transparent target drawn into and resolved by premultiplied
// alpha, which composites a half-covered pixel at half the canvas; a
// factor-4 target cleared and drawn again between resolves; a frame of
// edges between the target's pixels at factors 2 and 4, whose reduction
// lands each edge pixel at the share of it the drawing covers; and the
// two-level reduction of a target that keeps its half, by one half, where
// it is the box of four pixels, by three quarters and nine tenths, where
// the half at twice the scale lies under the part at alpha 1 - log2(1 /
// scale), and by 1, at factors 1 and 2 and under a scissor. Where nothing
// blends the read-back equals the reference; where it does, the renderer's
// own rounding of each blend keeps every channel within 2 levels of the
// reference and 0.5 on average. Malformed frames, and frames naming a page
// or target that is destroyed or never was, are refused with nothing drawn;
// pages and targets beyond the renderer's texture limit are refused with an
// error naming the size and the limit; a new target reads back
// transparent; draw batches that share their state run in one geometry
// call; and the renderer's target, scissor, draw colour and draw blend
// mode are as the executor found them. The processor cost of frames of
// 5,000 and 20,000 quads is measured and printed, never checked, and so are
// the texture bytes of the supersampled targets.
//
// The reference rasteriser samples each pixel at its centre with the
// top-left rule, interpolates colours and texel coordinates exactly and
// truncates them, reads the nearest texel, and blends in whole levels with
// each product divided by 255 and truncated, as SDL's software renderer
// does; the test keeps vertices on whole pixels of each target's texture
// and texture coordinates on whole texels, where the renderer's own
// truncation of them changes nothing. Its LINEAR stretch of a render
// target places and weighs pixels as that renderer's stretch does, in
// 16.16 positions with 7-bit fractions, the rows first and the columns
// after, the sum truncated once; a build of that renderer without SSE2 or
// NEON truncates after each pass and lands within a level of it.
#include "oa/app/card/executor.hpp"

#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

namespace card = oa::app::card;

/// The canvas the cases draw on.
constexpr int canvas_width = 160;
constexpr int canvas_height = 120;
/// What the canvas is cleared to.
constexpr std::array<uint8_t, 3> canvas_colour{20, 40, 60};
/// Most a channel may differ from the reference where a blend rounds, and
/// the most the mean difference may be.
constexpr int most_difference = 2;
constexpr double most_mean_difference = 0.5;
/// The first SDL release whose software renderer samples a texture drawn
/// through a triangle that is not half of a rectangle at the texel each
/// pixel's centre lands on, as the reference does. An older one takes the
/// texel beside it over parts of such a triangle.
constexpr int triangle_texels_version = SDL_VERSIONNUM(3, 4, 0);
/// The first SDL release whose software renderer draws a scaled texture
/// that a scissor cuts with the pixels the whole draw gives inside the
/// scissor. An older one scales the part of the texture it works out for
/// the cut, rounded to whole texels, which moves the picture by up to a
/// texel.
constexpr int scissored_scaling_version = SDL_VERSIONNUM(3, 4, 0);
/// Seeds of the pages' texels.
constexpr uint32_t seed_page = 0x243F6A88U;
constexpr uint32_t seed_level = 0x85A308D3U;
constexpr uint32_t seed_benchmark = 0x13198A2EU;
/// The benchmark's canvas and quads.
constexpr int benchmark_width = 1280;
constexpr int benchmark_height = 720;
constexpr int benchmark_quad_edge = 8;
constexpr int benchmark_page_edge = 256;
constexpr int benchmark_frames = 3;

/// A level of a channel as the colour's float.
///
/// @param value the level, 0 to 255
/// @return the float, exact at the level
constexpr float level(uint8_t value) noexcept {
    return static_cast<float>(value) / 255.0F;
}

/// A 32-bit xorshift sequence, the same on every platform.
struct Random {
    uint32_t state{};

    /// Returns the next number of the sequence.
    ///
    /// @return 32 bits
    uint32_t next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

/// An RGBA pixel, straight alpha.
using Pixel = std::array<uint8_t, 4>;

/// RGBA pixels, rows one after another.
struct Image {
    uint32_t width{};
    uint32_t height{};
    std::vector<Pixel> pixels;

    Image() = default;

    /// Makes an image filled with one pixel.
    ///
    /// @param image_width columns
    /// @param image_height rows
    /// @param fill every pixel
    Image(uint32_t image_width, uint32_t image_height, Pixel fill)
        : width(image_width), height(image_height),
          pixels(std::size_t{image_width} * image_height, fill) {}

    /// Returns a pixel.
    ///
    /// @param x the column
    /// @param y the row
    /// @return the pixel
    [[nodiscard]] Pixel& at(uint32_t x, uint32_t y) { return pixels[std::size_t{y} * width + x]; }

    /// Returns a pixel.
    ///
    /// @param x the column
    /// @param y the row
    /// @return the pixel
    [[nodiscard]] const Pixel& at(uint32_t x, uint32_t y) const {
        return pixels[std::size_t{y} * width + x];
    }

    /// Returns the image as the bytes of page texels.
    ///
    /// @return red, green, blue and alpha of each pixel, rows one after another
    [[nodiscard]] std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out;
        out.reserve(pixels.size() * card::texel_bytes);
        for (const Pixel& pixel : pixels)
            out.insert(out.end(), pixel.begin(), pixel.end());
        return out;
    }

    /// Returns the bytes from one row of texels to the next.
    ///
    /// @return the row's bytes
    [[nodiscard]] uint32_t pitch() const noexcept { return width * card::texel_bytes; }
};

/// Returns an image of seeded texels: a quarter opaque, a quarter clear,
/// the rest part transparent.
///
/// @param seed the sequence's seed
/// @param width columns
/// @param height rows
/// @return the image
Image seeded(uint32_t seed, uint32_t width, uint32_t height) {
    Random random{seed};
    Image image(width, height, {});
    for (auto& pixel : image.pixels) {
        const uint32_t word = random.next();
        pixel[0] = static_cast<uint8_t>(word >> 24);
        pixel[1] = static_cast<uint8_t>(word >> 16);
        pixel[2] = static_cast<uint8_t>(word >> 8);
        const uint32_t kind = random.next() % 4U;
        pixel[3] = kind == 0 ? 255 : kind == 1 ? 0 : static_cast<uint8_t>(word);
    }
    return image;
}

/// Returns an image with each colour multiplied by its alpha, truncated, as
/// a page drawn by premultiplied alpha holds its texels.
///
/// @param image the image, straight alpha
/// @return the premultiplied image
Image premultiplied(Image image) {
    for (auto& pixel : image.pixels)
        for (std::size_t channel = 0; channel < 3; ++channel)
            pixel[channel] = static_cast<uint8_t>(int{pixel[channel]} * pixel[3] / 255);
    return image;
}

/// Reads a surface's pixels as RGBA.
///
/// @param surface the surface, destroyed here
/// @return the image; empty when the surface is null
Image unpack(SDL_Surface* surface) {
    Image image;
    OA_CHECK(surface != nullptr);
    if (surface == nullptr)
        return image;
    // RGBA32 lays each pixel out as red, green, blue, alpha in memory, as
    // a Pixel is.
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    OA_CHECK(rgba != nullptr);
    if (rgba == nullptr)
        return image;
    image = Image(static_cast<uint32_t>(rgba->w), static_cast<uint32_t>(rgba->h), {});
    for (int y = 0; y < rgba->h; ++y) {
        const auto* row = static_cast<const uint8_t*>(rgba->pixels) +
                          static_cast<std::ptrdiff_t>(y) * rgba->pitch;
        for (int x = 0; x < rgba->w; ++x) {
            const uint8_t* bytes = row + static_cast<std::ptrdiff_t>(x) * card::texel_bytes;
            image.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y)) = {
                bytes[0], bytes[1], bytes[2], bytes[3]
            };
        }
    }
    SDL_DestroySurface(rgba);
    return image;
}

/// SDL's software renderer over an XRGB8888 surface.
struct Canvas {
    SDL_Surface* surface{};
    SDL_Renderer* renderer{};

    /// Makes the surface and the renderer.
    ///
    /// @param width columns
    /// @param height rows
    Canvas(int width, int height) {
        surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_XRGB8888);
        renderer = surface != nullptr ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        OA_CHECK(renderer != nullptr);
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    ~Canvas() {
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }

    /// Clears the surface to the canvas colour.
    void clear() const {
        OA_CHECK(SDL_SetRenderTarget(renderer, nullptr));
        OA_CHECK(SDL_SetRenderDrawColor(
            renderer, canvas_colour[0], canvas_colour[1], canvas_colour[2], 255
        ));
        OA_CHECK(SDL_RenderClear(renderer));
    }

    /// Reads the surface back, alpha 255 throughout.
    ///
    /// @return its pixels
    [[nodiscard]] Image read() const {
        OA_CHECK(SDL_SetRenderTarget(renderer, nullptr));
        Image image = unpack(SDL_RenderReadPixels(renderer, nullptr));
        for (auto& pixel : image.pixels)
            pixel[3] = 255;
        return image;
    }

    /// Reads a render target of the renderer back.
    ///
    /// @param texture the target
    /// @return its pixels, alpha included
    [[nodiscard]] Image read_target(SDL_Texture* texture) const {
        OA_CHECK(SDL_SetRenderTarget(renderer, texture));
        Image image = unpack(SDL_RenderReadPixels(renderer, nullptr));
        OA_CHECK(SDL_SetRenderTarget(renderer, nullptr));
        return image;
    }
};

/// Returns the canvas as the reference starts from.
///
/// @return the cleared canvas
Image cleared_canvas() {
    return {
        static_cast<uint32_t>(canvas_width),
        static_cast<uint32_t>(canvas_height),
        {canvas_colour[0], canvas_colour[1], canvas_colour[2], 255}
    };
}

/// How far a read-back is from a reference.
struct Difference {
    int most{};
    double mean{};
};

/// Compares two images of one size.
///
/// @param read the read-back
/// @param reference the reference
/// @return the largest and the mean difference of a colour channel; alpha left out
Difference compare(const Image& read, const Image& reference) {
    Difference difference;
    OA_CHECK(read.width == reference.width && read.height == reference.height);
    if (read.pixels.size() != reference.pixels.size())
        return {255, 255.0};
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t at = 0; at < read.pixels.size(); ++at)
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int delta =
                std::abs(int{read.pixels[at][channel]} - int{reference.pixels[at][channel]});
            difference.most = std::max(difference.most, delta);
            sum += delta;
            ++count;
        }
    difference.mean = count != 0 ? sum / static_cast<double>(count) : 0.0;
    return difference;
}

/// Tells whether the SDL library the test runs on, which can be older than
/// the headers it was built with, draws a case as the reference does.
///
/// @param version the first SDL release that draws it as the reference does
/// @return true on that release or a later one
bool drawn_as_reference_from(int version) {
    return SDL_GetVersion() >= version;
}

/// Checks a read-back against a reference, exact or within the tolerance.
///
/// @param what the case, for the report
/// @param read the read-back
/// @param reference the reference
/// @param exact whether every channel must be equal
/// @param held whether the SDL the test runs on draws the case as the
///     reference does; when it does not, the difference is reported only
void expect_match(
    const char* what, const Image& read, const Image& reference, bool exact, bool held
) {
    const Difference difference = compare(read, reference);
    std::printf("%s: most %d, mean %.3f\n", what, difference.most, difference.mean);
    if (!held) {
        const int version = SDL_GetVersion();
        std::printf(
            "%s: not held to the reference on SDL %d.%d.%d\n",
            what,
            SDL_VERSIONNUM_MAJOR(version),
            SDL_VERSIONNUM_MINOR(version),
            SDL_VERSIONNUM_MICRO(version)
        );
        return;
    }
    if (exact)
        OA_CHECK(difference.most == 0);
    OA_CHECK(difference.most <= most_difference);
    OA_CHECK(difference.mean <= most_mean_difference);
}

// ---------------------------------------------------------------------------
// The reference rasteriser

/// A page's levels as the reference knows them.
struct ReferencePage {
    std::vector<Image> levels;
};

/// A render target as the reference knows it.
struct ReferenceTarget {
    uint32_t width{};
    uint32_t height{};
    uint32_t factor{};
    bool keep_half{}; ///< made with a half for the two-level reduction
    Image image;      ///< the size times the factor
};

/// The cross product of ab and ac.
///
/// @param ax a's x
/// @param ay a's y
/// @param bx b's x
/// @param by b's y
/// @param cx c's x
/// @param cy c's y
/// @return the product
int64_t cross(int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t cx, int64_t cy) noexcept {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/// Says whether an edge is a top or a left edge of its triangle, which
/// owns the pixel centres that lie on it.
///
/// @param ax the edge's first point's x
/// @param ay its y
/// @param bx the edge's second point's x
/// @param by its y
/// @param clockwise whether the triangle's area is positive
/// @return true for a top or left edge
bool top_left(int64_t ax, int64_t ay, int64_t bx, int64_t by, bool clockwise) noexcept {
    if (clockwise)
        return (ay == by && ax < bx) || by < ay;
    return (ay == by && bx < ax) || ay < by;
}

/// A vertex as the reference rasterises it: whole pixels of the target's
/// texture, whole levels of colour, whole texels.
struct Corner {
    int64_t x{};
    int64_t y{};
    std::array<int64_t, 4> colour{};
    int64_t s{};
    int64_t t{};
};

/// Returns a channel as a whole level, rounded as the renderer rounds.
///
/// @param value the channel, 0 to 1
/// @return the level
int64_t quantise(float value) noexcept {
    return static_cast<int64_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

/// Blends one source pixel over one destination pixel in whole levels, each
/// product divided by 255 and truncated.
///
/// @param blend the blend
/// @param source the source: straight alpha, or multiplied by its alpha already for
///     alpha_premultiplied
/// @param[in,out] destination the pixel under it
void blend_pixel(card::Blend blend, std::array<int64_t, 4> source, Pixel& destination) {
    std::array<int64_t, 4> under{destination[0], destination[1], destination[2], destination[3]};
    const int64_t alpha = source[3];
    if ((blend == card::Blend::alpha || blend == card::Blend::additive) && alpha < 255)
        for (std::size_t channel = 0; channel < 3; ++channel)
            source[channel] = source[channel] * alpha / 255;
    switch (blend) {
    case card::Blend::none:
        under = source;
        break;
    case card::Blend::alpha:
        for (std::size_t channel = 0; channel < 3; ++channel)
            under[channel] = source[channel] + (255 - alpha) * under[channel] / 255;
        under[3] = alpha + (255 - alpha) * under[3] / 255;
        break;
    case card::Blend::alpha_premultiplied:
        // The source is multiplied by its alpha already; the sum is clamped
        // at white, since a source multiplied by more than its alpha can
        // overflow.
        for (std::size_t channel = 0; channel < 4; ++channel)
            under[channel] =
                std::min<int64_t>(255, source[channel] + (255 - alpha) * under[channel] / 255);
        break;
    case card::Blend::additive:
        for (std::size_t channel = 0; channel < 3; ++channel)
            under[channel] = std::min<int64_t>(255, source[channel] + under[channel]);
        break;
    case card::Blend::modulate:
        for (std::size_t channel = 0; channel < 3; ++channel)
            under[channel] = source[channel] * under[channel] / 255;
        break;
    case card::Blend::darken:
        for (std::size_t channel = 0; channel < 3; ++channel)
            under[channel] = under[channel] * (255 - alpha) / 255;
        break;
    case card::Blend::minimum:
        for (std::size_t channel = 0; channel < 3; ++channel)
            under[channel] = std::min(under[channel], source[channel]);
        break;
    case card::Blend::lighten:
        for (std::size_t channel = 0; channel < 3; ++channel)
            under[channel] = std::min<int64_t>(
                255, (source[channel] * under[channel] + under[channel] * (255 - alpha)) / 255
            );
        break;
    }
    for (std::size_t channel = 0; channel < 4; ++channel)
        destination[channel] = static_cast<uint8_t>(under[channel]);
}

/// Halves an image, each pixel the truncated mean of four.
///
/// @param image the image, of even size
/// @return the halved image
Image halve(const Image& image) {
    Image out(image.width / 2, image.height / 2, {});
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x)
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const int sum =
                    image.at(2 * x, 2 * y)[channel] + image.at(2 * x + 1, 2 * y)[channel] +
                    image.at(2 * x, 2 * y + 1)[channel] + image.at(2 * x + 1, 2 * y + 1)[channel];
                out.at(x, y)[channel] = static_cast<uint8_t>(sum / 4);
            }
    return out;
}

/// The bits of a fraction the renderer's LINEAR stretch keeps, and one in
/// that many bits.
constexpr uint32_t stretch_fraction_bits = 7;
constexpr uint32_t stretch_fraction_one = 1U << stretch_fraction_bits;
/// The bits of a position the stretch keeps below the pixel.
constexpr uint32_t stretch_position_bits = 16;

/// Where the pixels of one axis of a LINEAR stretch sample the source, as
/// the renderer's own stretch places them: each destination pixel's centre
/// in the source, positions in 16.16 fixed point stepped by the truncated
/// ratio from half a step less half a pixel, the fraction kept to
/// stretch_fraction_bits; a pixel before the first source pixel's centre
/// reads the first pixel alone, and one past the second-last reads the
/// last alone.
struct StretchAxis {
    std::vector<uint32_t> index;    ///< the lower source pixel of each destination pixel
    std::vector<uint32_t> fraction; ///< its weight toward the next, in stretch_fraction_one
};

/// Places one axis of a LINEAR stretch.
///
/// @param source_count source pixels along the axis
/// @param destination_count destination pixels along it
/// @return the placement
StretchAxis stretch_axis(uint32_t source_count, uint32_t destination_count) {
    StretchAxis axis;
    const int64_t one = int64_t{1} << stretch_position_bits;
    const int64_t step = (int64_t{source_count} << stretch_position_bits) / destination_count;
    const int64_t start = ((step + 1) / 2) - one / 2;
    for (uint32_t at = 0; at < destination_count; ++at) {
        const int64_t position = start + static_cast<int64_t>(at) * step;
        uint32_t index = 0;
        uint32_t fraction = 0;
        if (position >= 0) {
            index = static_cast<uint32_t>(position >> stretch_position_bits);
            if (index + 2 > source_count) {
                index = source_count - 1;
            } else {
                fraction = static_cast<uint32_t>(
                               position >> (stretch_position_bits - stretch_fraction_bits)
                           ) &
                           (stretch_fraction_one - 1);
            }
        }
        axis.index.push_back(index);
        axis.fraction.push_back(fraction);
    }
    return axis;
}

/// Stretches a part of an image LINEAR, as the renderer's own stretch
/// does: each destination pixel the two source pixels each way nearest its
/// centre, weighted by stretch_axis's fractions, the rows blended first
/// and then the columns, the sum truncated once, as that renderer's
/// stretch does on a processor with SSE2 or NEON; a build without either
/// truncates after each pass and lands within a level.
///
/// @param image the source
/// @param part the part stretched, in the source's pixels
/// @param width destination pixels across
/// @param height destination pixels down
/// @return the stretched part
Image stretch_linear(const Image& image, const card::Rect& part, uint32_t width, uint32_t height) {
    const StretchAxis across = stretch_axis(static_cast<uint32_t>(part.width), width);
    const StretchAxis down = stretch_axis(static_cast<uint32_t>(part.height), height);
    Image out(width, height, {});
    const auto source = [&](uint32_t column, uint32_t row) -> const Pixel& {
        return image.at(
            static_cast<uint32_t>(part.x) + column, static_cast<uint32_t>(part.y) + row
        );
    };
    const auto last_column = static_cast<uint32_t>(part.width - 1);
    const auto last_row = static_cast<uint32_t>(part.height - 1);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t row = down.index[y];
        const uint32_t next_row = std::min(row + 1, last_row);
        const uint32_t weight_down = down.fraction[y];
        const uint32_t weight_up = stretch_fraction_one - weight_down;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t column = across.index[x];
            const uint32_t next_column = std::min(column + 1, last_column);
            const uint32_t weight_right = across.fraction[x];
            const uint32_t weight_left = stretch_fraction_one - weight_right;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                // The rows first, each column of the pair, then the columns,
                // the sum truncated once.
                const uint32_t left = weight_up * source(column, row)[channel] +
                                      weight_down * source(column, next_row)[channel];
                const uint32_t right = weight_up * source(next_column, row)[channel] +
                                       weight_down * source(next_column, next_row)[channel];
                out.at(x, y)[channel] = static_cast<uint8_t>(
                    (weight_left * left + weight_right * right) >> (2 * stretch_fraction_bits)
                );
            }
        }
    }
    return out;
}

/// Blends one channel of an opaque source over a destination at an alpha,
/// as the renderer's own blend of a texture drawn at an alpha does: the
/// source weighted by the alpha and the destination by the rest, divided
/// by 255 with rounding.
///
/// @param source the source's level
/// @param destination the level under it
/// @param alpha the alpha, 0 to 255
/// @return the blended level
uint8_t blend_at_alpha(uint32_t source, uint32_t destination, uint32_t alpha) noexcept {
    uint32_t value = source * alpha + destination * (255 - alpha) + 1;
    value += value >> 8;
    return static_cast<uint8_t>(value >> 8);
}

/// Reduces a part of a target's texture by the two-level blend, as the
/// executor does: the texture's half, stretched LINEAR at exactly one
/// half, drawn at twice the scale, then the part drawn over it at alpha
/// 1 - t, t = log2(1 / scale) by the scale across, left out at a scale of
/// one half.
///
/// @param texture the target's texture
/// @param part the part reduced, in the texture's pixels, even throughout
/// @param width destination pixels across
/// @param height destination pixels down
/// @return the reduced part, opaque
Image blend_reduce_reference(
    const Image& texture, const card::Rect& part, uint32_t width, uint32_t height
) {
    const card::Rect whole{
        0, 0, static_cast<int32_t>(texture.width), static_cast<int32_t>(texture.height)
    };
    const Image half = stretch_linear(texture, whole, texture.width / 2, texture.height / 2);
    const card::Rect half_part{part.x / 2, part.y / 2, part.width / 2, part.height / 2};
    Image out = stretch_linear(half, half_part, width, height);
    const double scale = static_cast<double>(width) / part.width;
    const double t = std::clamp(std::log2(1.0 / scale), 0.0, 1.0);
    if (t < 1.0) {
        const Image over = stretch_linear(texture, part, width, height);
        const auto alpha = static_cast<uint32_t>(std::lround((1.0 - t) * 255.0));
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                for (std::size_t channel = 0; channel < 3; ++channel)
                    out.at(x, y)[channel] =
                        blend_at_alpha(over.at(x, y)[channel], out.at(x, y)[channel], alpha);
    }
    for (auto& pixel : out.pixels)
        pixel[3] = 255;
    return out;
}

/// Runs frames on the processor as the executor runs them on the renderer.
struct Reference {
    std::map<uint32_t, ReferencePage> pages;
    std::map<uint32_t, ReferenceTarget> targets;
    Image final_image = cleared_canvas();
    /// The renderer takes the minimum blend; where it does not, a minimum
    /// draw blends as alpha.
    bool minimum_composed{};

    /// Returns the image a batch draws into and the factor its pixels are scaled by.
    ///
    /// @param target the batch's target
    /// @param[out] factor the target's factor; 1 for the canvas
    /// @return the image
    Image& target_image(card::TargetHandle target, uint32_t& factor) {
        if (target == card::TargetHandle{}) {
            factor = 1;
            return final_image;
        }
        ReferenceTarget& found = targets.at(target.value);
        factor = found.factor;
        return found.image;
    }

    /// Rasterises one triangle.
    ///
    /// @param image the image drawn into
    /// @param corners the triangle's corners
    /// @param texture the page level read; null for untextured
    /// @param blend the blend
    /// @param scissored whether a scissor applies
    /// @param scissor the scissor, in the image's pixels
    void triangle(
        Image& image,
        const std::array<Corner, 3>& corners,
        const Image* texture,
        card::Blend blend,
        bool scissored,
        const card::Rect& scissor
    ) const {
        // Doubled coordinates put pixel centres on whole numbers.
        std::array<int64_t, 3> xs{2 * corners[0].x, 2 * corners[1].x, 2 * corners[2].x};
        std::array<int64_t, 3> ys{2 * corners[0].y, 2 * corners[1].y, 2 * corners[2].y};
        int64_t area = cross(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2]);
        if (area == 0)
            return;
        const bool clockwise = area > 0;
        if (!clockwise)
            area = -area;
        const std::array<int64_t, 3> bias{
            top_left(xs[1], ys[1], xs[2], ys[2], clockwise) ? 0 : -1,
            top_left(xs[2], ys[2], xs[0], ys[0], clockwise) ? 0 : -1,
            top_left(xs[0], ys[0], xs[1], ys[1], clockwise) ? 0 : -1
        };
        int64_t left = std::min({corners[0].x, corners[1].x, corners[2].x});
        int64_t top = std::min({corners[0].y, corners[1].y, corners[2].y});
        int64_t right = std::max({corners[0].x, corners[1].x, corners[2].x});
        int64_t bottom = std::max({corners[0].y, corners[1].y, corners[2].y});
        left = std::max<int64_t>(left, 0);
        top = std::max<int64_t>(top, 0);
        right = std::min<int64_t>(right, image.width);
        bottom = std::min<int64_t>(bottom, image.height);
        if (scissored) {
            left = std::max<int64_t>(left, scissor.x);
            top = std::max<int64_t>(top, scissor.y);
            right = std::min<int64_t>(right, int64_t{scissor.x} + scissor.width);
            bottom = std::min<int64_t>(bottom, int64_t{scissor.y} + scissor.height);
        }
        for (int64_t y = top; y < bottom; ++y)
            for (int64_t x = left; x < right; ++x) {
                const int64_t px = 2 * x + 1;
                const int64_t py = 2 * y + 1;
                std::array<int64_t, 3> w{
                    cross(xs[1], ys[1], xs[2], ys[2], px, py),
                    cross(xs[2], ys[2], xs[0], ys[0], px, py),
                    cross(xs[0], ys[0], xs[1], ys[1], px, py)
                };
                if (!clockwise)
                    for (auto& weight : w)
                        weight = -weight;
                if (w[0] + bias[0] < 0 || w[1] + bias[1] < 0 || w[2] + bias[2] < 0)
                    continue;
                std::array<int64_t, 4> source{255, 255, 255, 255};
                if (texture != nullptr) {
                    int64_t s =
                        (w[0] * corners[0].s + w[1] * corners[1].s + w[2] * corners[2].s) / area;
                    int64_t t =
                        (w[0] * corners[0].t + w[1] * corners[1].t + w[2] * corners[2].t) / area;
                    s = std::clamp<int64_t>(s, 0, texture->width - 1);
                    t = std::clamp<int64_t>(t, 0, texture->height - 1);
                    const Pixel& texel =
                        texture->at(static_cast<uint32_t>(s), static_cast<uint32_t>(t));
                    source = {texel[0], texel[1], texel[2], texel[3]};
                }
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const int64_t modulation =
                        (w[0] * corners[0].colour[channel] + w[1] * corners[1].colour[channel] +
                         w[2] * corners[2].colour[channel]) /
                        area;
                    source[channel] = source[channel] * modulation / 255;
                }
                blend_pixel(
                    blend, source, image.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                );
            }
    }

    /// Runs a frame.
    ///
    /// @param frame the frame, well formed and naming pages and targets the reference holds
    void run(const card::CardFrame& frame) {
        for (const card::Batch& batch : frame.batches) {
            uint32_t factor = 1;
            Image& image = target_image(batch.target, factor);
            switch (batch.operation) {
            case card::Operation::draw: {
                const Image* texture = nullptr;
                if (batch.page != card::PageHandle{})
                    texture = &pages.at(batch.page.value).levels.at(batch.level);
                const card::Rect scissor{
                    batch.scissor.x * static_cast<int32_t>(factor),
                    batch.scissor.y * static_cast<int32_t>(factor),
                    batch.scissor.width * static_cast<int32_t>(factor),
                    batch.scissor.height * static_cast<int32_t>(factor)
                };
                for (uint32_t at = 0; at + 2 < batch.index_count; at += 3) {
                    std::array<Corner, 3> corners{};
                    for (std::size_t k = 0; k < 3; ++k) {
                        const card::Vertex& vertex =
                            frame.vertices[frame.indices[batch.first_index + at + k]];
                        const double x = static_cast<double>(vertex.x) * factor;
                        const double y = static_cast<double>(vertex.y) * factor;
                        // The test keeps vertices on whole pixels of the texture.
                        OA_CHECK(x == std::floor(x) && y == std::floor(y));
                        corners[k].x = static_cast<int64_t>(x);
                        corners[k].y = static_cast<int64_t>(y);
                        corners[k].colour = {
                            quantise(vertex.colour.red),
                            quantise(vertex.colour.green),
                            quantise(vertex.colour.blue),
                            quantise(vertex.colour.alpha)
                        };
                        if (texture != nullptr) {
                            corners[k].s =
                                static_cast<int64_t>(vertex.u * static_cast<float>(texture->width));
                            corners[k].t = static_cast<int64_t>(
                                vertex.v * static_cast<float>(texture->height)
                            );
                        }
                    }
                    const card::Blend blend =
                        batch.blend == card::Blend::minimum && !minimum_composed
                            ? card::Blend::alpha
                            : batch.blend;
                    triangle(image, corners, texture, blend, batch.scissored, scissor);
                }
                break;
            }
            case card::Operation::clear: {
                const Pixel fill{
                    static_cast<uint8_t>(quantise(batch.colour.red)),
                    static_cast<uint8_t>(quantise(batch.colour.green)),
                    static_cast<uint8_t>(quantise(batch.colour.blue)),
                    static_cast<uint8_t>(quantise(batch.colour.alpha))
                };
                std::fill(image.pixels.begin(), image.pixels.end(), fill);
                break;
            }
            case card::Operation::resolve: {
                const ReferenceTarget& source = targets.at(batch.source.value);
                Image reduced = source.image;
                for (uint32_t done = 1; done < source.factor; done *= 2)
                    reduced = halve(reduced);
                // The test resolves every target at its own size.
                OA_CHECK(
                    reduced.width == static_cast<uint32_t>(batch.destination.width) &&
                    reduced.height == static_cast<uint32_t>(batch.destination.height)
                );
                for (uint32_t y = 0; y < reduced.height; ++y)
                    for (uint32_t x = 0; x < reduced.width; ++x) {
                        const int64_t px = int64_t{batch.destination.x} + x;
                        const int64_t py = int64_t{batch.destination.y} + y;
                        if (px < 0 || py < 0 || px >= int64_t{image.width} * factor ||
                            py >= int64_t{image.height} * factor)
                            continue;
                        if (batch.scissored &&
                            (px < batch.scissor.x || py < batch.scissor.y ||
                             px >= int64_t{batch.scissor.x} + batch.scissor.width ||
                             py >= int64_t{batch.scissor.y} + batch.scissor.height))
                            continue;
                        const Pixel& pixel = reduced.at(x, y);
                        blend_pixel(
                            batch.blend,
                            {pixel[0], pixel[1], pixel[2], pixel[3]},
                            image.at(static_cast<uint32_t>(px), static_cast<uint32_t>(py))
                        );
                    }
                break;
            }
            case card::Operation::blend_reduce: {
                const ReferenceTarget& source = targets.at(batch.source.value);
                OA_CHECK(source.keep_half || source.factor >= 4);
                const Image reduced = blend_reduce_reference(
                    source.image,
                    batch.source_part,
                    static_cast<uint32_t>(batch.destination.width),
                    static_cast<uint32_t>(batch.destination.height)
                );
                for (uint32_t y = 0; y < reduced.height; ++y)
                    for (uint32_t x = 0; x < reduced.width; ++x) {
                        const int64_t px = int64_t{batch.destination.x} + x;
                        const int64_t py = int64_t{batch.destination.y} + y;
                        if (px < 0 || py < 0 || px >= int64_t{image.width} * factor ||
                            py >= int64_t{image.height} * factor)
                            continue;
                        if (batch.scissored &&
                            (px < batch.scissor.x || py < batch.scissor.y ||
                             px >= int64_t{batch.scissor.x} + batch.scissor.width ||
                             py >= int64_t{batch.scissor.y} + batch.scissor.height))
                            continue;
                        image.at(static_cast<uint32_t>(px), static_cast<uint32_t>(py)) =
                            reduced.at(x, y);
                    }
                break;
            }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Fixtures

/// An executor on a canvas, with the reference beside it.
struct Fixture {
    Canvas canvas{canvas_width, canvas_height};
    card::Executor executor;
    Reference reference;

    /// Opens the executor on the canvas's renderer.
    ///
    /// @param texture_limit the limit given to the executor; 0 for none
    explicit Fixture(uint32_t texture_limit = 0) {
        OA_CHECK(executor.open(canvas.renderer, texture_limit));
        reference.minimum_composed = executor.capabilities().minimum_composed;
        canvas.clear();
    }

    /// Makes a page from images of its levels, in the executor and the reference.
    ///
    /// @param levels the levels' images, level 0 first
    /// @return the page
    card::PageHandle make_page(const std::vector<Image>& levels) {
        card::PageDescription description;
        description.width = levels.front().width;
        description.height = levels.front().height;
        description.level_count = static_cast<uint8_t>(levels.size());
        const card::PageHandle page = executor.create_page(description);
        OA_CHECK(page != card::PageHandle{});
        if (page == card::PageHandle{}) {
            std::fprintf(stderr, "create_page: %s\n", executor.error().c_str());
            return page;
        }
        for (std::size_t at = 0; at < levels.size(); ++at) {
            const auto bytes = levels[at].bytes();
            OA_CHECK(executor.update_page(
                page, static_cast<uint8_t>(at), nullptr, bytes.data(), levels[at].pitch()
            ));
        }
        reference.pages[page.value] = {levels};
        return page;
    }

    /// Makes a render target in the executor and the reference.
    ///
    /// @param width pixels across
    /// @param height pixels down
    /// @param factor the supersampling factor
    /// @param keep_half whether it keeps a half for the two-level reduction
    /// @return the target
    card::TargetHandle
    make_target(uint32_t width, uint32_t height, uint32_t factor, bool keep_half = false) {
        const card::TargetHandle target = executor.create_target(width, height, factor, keep_half);
        OA_CHECK(target != card::TargetHandle{});
        if (target == card::TargetHandle{}) {
            std::fprintf(stderr, "create_target: %s\n", executor.error().c_str());
            return target;
        }
        reference.targets[target.value] = {
            width, height, factor, keep_half, Image(width * factor, height * factor, {0, 0, 0, 0})
        };
        return target;
    }

    /// Runs a frame on both and compares the canvas.
    ///
    /// @param what the case, for the report
    /// @param frame the frame
    /// @param exact whether the read-back must equal the reference
    /// @param held whether the SDL the test runs on draws the frame as the
    ///     reference does; when it does not, the difference is reported only
    void run(const char* what, const card::CardFrame& frame, bool exact, bool held = true) {
        const bool ran = executor.execute(frame, nullptr);
        OA_CHECK(ran);
        if (!ran)
            std::fprintf(stderr, "execute: %s\n", executor.error().c_str());
        reference.run(frame);
        expect_match(what, canvas.read(), reference.final_image, exact, held);
    }
};

/// Appends a draw batch over the indices appended since `first_index`.
///
/// @param[in,out] frame the frame
/// @param first_index the first index of the batch
/// @param page the page; none for untextured
/// @param blend the blend
/// @param target the target; none for the canvas
/// @return the batch, to set a level or scissor on
card::Batch& draw_since(
    card::CardFrame& frame,
    card::Index first_index,
    card::PageHandle page,
    card::Blend blend,
    card::TargetHandle target = {}
) {
    card::Batch batch;
    batch.operation = card::Operation::draw;
    batch.target = target;
    batch.page = page;
    batch.blend = blend;
    batch.first_index = first_index;
    batch.index_count = static_cast<uint32_t>(frame.indices.size()) - first_index;
    frame.batches.push_back(batch);
    return frame.batches.back();
}

/// Appends a triangle.
///
/// @param[in,out] frame the frame
/// @param corners the corners
void append_triangle(card::CardFrame& frame, const std::array<card::Vertex, 3>& corners) {
    const auto first = static_cast<card::Index>(frame.vertices.size());
    for (const auto& corner : corners)
        frame.vertices.push_back(corner);
    frame.indices.push_back(first);
    frame.indices.push_back(first + 1);
    frame.indices.push_back(first + 2);
}

/// The number of indices a frame holds, as the next batch's first index.
///
/// @param frame the frame
/// @return the count
card::Index next_index(const card::CardFrame& frame) {
    return static_cast<card::Index>(frame.indices.size());
}

// ---------------------------------------------------------------------------
// Cases

/// Opaque draws, textured and not, quads and a triangle with a colour
/// gradient: the read-back equals the reference.
void test_opaque_draws_are_exact() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 64, 64);
    const card::PageHandle page = fixture.make_page({texels});
    card::CardFrame frame;
    const card::Colour white{};
    card::Index first = next_index(frame);
    card::append_quad(frame, 10.0F, 8.0F, 64.0F, 64.0F, 0.0F, 0.0F, 1.0F, 1.0F, white);
    // A quad partly off the canvas.
    card::append_quad(frame, 130.0F, 90.0F, 64.0F, 64.0F, 0.0F, 0.0F, 1.0F, 1.0F, white);
    draw_since(frame, first, page, card::Blend::none);
    first = next_index(frame);
    card::append_quad(
        frame,
        30.0F,
        100.0F,
        50.0F,
        15.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        {level(51), level(102), level(153), 1.0F}
    );
    draw_since(frame, first, {}, card::Blend::none);
    first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{100.0F, 20.0F, {1.0F, 0.0F, 0.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{150.0F, 20.0F, {0.0F, 1.0F, 0.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{125.0F, 60.0F, {0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, 0.0F}}
    );
    draw_since(frame, first, {}, card::Blend::none);
    // A textured triangle, 1:1 to its texels, with a colour gradient.
    first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{80.0F, 70.0F, {1.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{144.0F, 70.0F, {1.0F, level(128), 0.0F, 1.0F}, 1.0F, 0.0F},
         card::Vertex{80.0F, 118.0F, {0.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 0.75F}}
    );
    draw_since(frame, first, page, card::Blend::none);
    OA_CHECK(card::check_frame(frame).empty());
    fixture.run("opaque draws", frame, true, drawn_as_reference_from(triangle_texels_version));
    // The untextured quad and the untextured triangle share their state
    // and follow on, so they run in one call.
    OA_CHECK(fixture.executor.counts().draw_calls == 3);
    OA_CHECK(fixture.executor.counts().batches_merged == 1);
    OA_CHECK(fixture.executor.counts().triangles == 8);
}

/// Every blend mode, textured and untextured, with vertex colours and
/// alpha: within the tolerance of the renderer's own rounding.
void test_blended_draws_match_the_reference() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 64, 64);
    const card::PageHandle page = fixture.make_page({texels});
    card::CardFrame frame;
    card::Index first = next_index(frame);
    card::append_quad(frame, 10.0F, 8.0F, 64.0F, 64.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, first, page, card::Blend::none);
    // Alpha, a part of the page, tinted.
    first = next_index(frame);
    card::append_quad(
        frame,
        80.0F,
        8.0F,
        32.0F,
        32.0F,
        0.25F,
        0.25F,
        0.75F,
        0.75F,
        {1.0F, level(128), level(64), 1.0F}
    );
    draw_since(frame, first, page, card::Blend::alpha);
    // Additive, half strength.
    first = next_index(frame);
    card::append_quad(
        frame, 10.0F, 70.0F, 48.0F, 40.0F, 0.0F, 0.0F, 0.75F, 0.625F, {1.0F, 1.0F, 1.0F, level(128)}
    );
    draw_since(frame, first, page, card::Blend::additive);
    // Modulate, tinted.
    first = next_index(frame);
    card::append_quad(
        frame,
        70.0F,
        70.0F,
        64.0F,
        40.0F,
        0.0F,
        0.0F,
        1.0F,
        0.625F,
        {level(204), level(204), 1.0F, 1.0F}
    );
    draw_since(frame, first, page, card::Blend::modulate);
    // Darken by the texels' alpha at three quarters, partly off the canvas.
    first = next_index(frame);
    card::append_quad(
        frame, 120.0F, 60.0F, 32.0F, 32.0F, 0.0F, 0.0F, 0.5F, 0.5F, {1.0F, 1.0F, 1.0F, level(192)}
    );
    card::append_quad(
        frame, 140.0F, 100.0F, 32.0F, 32.0F, 0.5F, 0.5F, 1.0F, 1.0F, {1.0F, 1.0F, 1.0F, level(192)}
    );
    draw_since(frame, first, page, card::Blend::darken);
    // An untextured translucent quad, and an untextured black shadow quad.
    first = next_index(frame);
    card::append_quad(
        frame, 0.0F, 0.0F, 20.0F, 20.0F, 0.0F, 0.0F, 0.0F, 0.0F, {0.0F, 1.0F, 0.0F, level(128)}
    );
    draw_since(frame, first, {}, card::Blend::alpha);
    first = next_index(frame);
    card::append_quad(
        frame, 40.0F, 40.0F, 30.0F, 30.0F, 0.0F, 0.0F, 0.0F, 0.0F, {0.0F, 0.0F, 0.0F, level(128)}
    );
    draw_since(frame, first, {}, card::Blend::darken);
    // An untextured quad that holds what is under it to a dark grey.
    first = next_index(frame);
    card::append_quad(
        frame,
        128.0F,
        0.0F,
        24.0F,
        6.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        {level(43), level(43), level(43), 1.0F}
    );
    draw_since(frame, first, {}, card::Blend::minimum);
    // Lighten: a quad of the page drawn with no alpha, so that each texel's
    // colour lights what is under it, and one at half its colour.
    first = next_index(frame);
    card::append_quad(
        frame, 20.0F, 20.0F, 40.0F, 40.0F, 0.0F, 0.0F, 0.625F, 0.625F, {1.0F, 1.0F, 1.0F, 0.0F}
    );
    card::append_quad(
        frame,
        84.0F,
        60.0F,
        32.0F,
        32.0F,
        0.5F,
        0.5F,
        1.0F,
        1.0F,
        {level(128), level(128), level(128), 0.0F}
    );
    draw_since(frame, first, page, card::Blend::lighten);
    // An untextured additive triangle with a gradient, and a textured
    // alpha triangle with a gradient and an alpha gradient.
    first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{100.0F, 20.0F, {level(128), level(64), 0.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{150.0F, 20.0F, {0.0F, level(64), level(128), 1.0F}, 0.0F, 0.0F},
         card::Vertex{125.0F, 60.0F, {0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, 0.0F}}
    );
    draw_since(frame, first, {}, card::Blend::additive);
    first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{10.0F, 8.0F, {1.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{74.0F, 8.0F, {1.0F, 0.0F, 0.0F, 1.0F}, 1.0F, 0.0F},
         card::Vertex{10.0F, 72.0F, {0.0F, 0.0F, 1.0F, level(128)}, 0.0F, 1.0F}}
    );
    draw_since(frame, first, page, card::Blend::alpha);
    // Premultiplied alpha: a quad of one colour from a premultiplied page,
    // tinted by a premultiplied vertex colour, and an untextured triangle
    // with a premultiplied colour gradient. A textured triangle that is no
    // such quad is left out: the software renderer draws it by none.
    const card::PageHandle premultiplied_page =
        fixture.make_page({premultiplied(seeded(seed_level, 32, 32))});
    first = next_index(frame);
    card::append_quad(
        frame,
        120.0F,
        8.0F,
        32.0F,
        32.0F,
        0.0F,
        0.0F,
        1.0F,
        1.0F,
        {level(192), level(192), level(192), level(192)}
    );
    draw_since(frame, first, premultiplied_page, card::Blend::alpha_premultiplied);
    first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{60.0F, 80.0F, {level(128), 0.0F, 0.0F, level(128)}, 0.0F, 0.0F},
         card::Vertex{110.0F, 80.0F, {0.0F, level(128), 0.0F, level(128)}, 0.0F, 0.0F},
         card::Vertex{85.0F, 118.0F, {0.0F, 0.0F, level(64), level(64)}, 0.0F, 0.0F}}
    );
    draw_since(frame, first, {}, card::Blend::alpha_premultiplied);
    OA_CHECK(card::check_frame(frame).empty());
    fixture.run("blended draws", frame, false, drawn_as_reference_from(triangle_texels_version));
    // The software renderer takes no composed blend mode, so darken and
    // minimum ran as their fallbacks.
    OA_CHECK(!fixture.executor.capabilities().darken_composed);
    OA_CHECK(!fixture.executor.capabilities().minimum_composed);
}

/// A scissor limits a draw to its rectangle; the pixels outside are untouched.
void test_scissor_limits_the_pixels() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 64, 64);
    const card::PageHandle page = fixture.make_page({texels});
    card::CardFrame frame;
    card::Index first = next_index(frame);
    card::append_quad(frame, 10.0F, 10.0F, 64.0F, 64.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    card::Batch& clipped = draw_since(frame, first, page, card::Blend::none);
    clipped.scissored = true;
    clipped.scissor = {30, 20, 50, 40};
    // The same scissor again, then none: two clip changes in all, and the
    // batches merge into one call.
    first = next_index(frame);
    card::append_quad(frame, 0.0F, 90.0F, 64.0F, 30.0F, 0.0F, 0.0F, 1.0F, 0.46875F, {});
    card::Batch& again = draw_since(frame, first, page, card::Blend::none);
    again.scissored = true;
    again.scissor = {30, 20, 50, 40};
    first = next_index(frame);
    card::append_quad(frame, 100.0F, 0.0F, 20.0F, 20.0F, 0.0F, 0.0F, 0.3125F, 0.3125F, {});
    draw_since(frame, first, page, card::Blend::none);
    fixture.run("scissored draws", frame, true);
    OA_CHECK(fixture.executor.counts().draw_calls == 2);
    OA_CHECK(fixture.executor.counts().batches_merged == 1);
    OA_CHECK(fixture.executor.counts().clip_changes == 2);
    // The read-back outside the scissor is the canvas colour.
    const Image read = fixture.canvas.read();
    OA_CHECK((read.at(10, 10) == Pixel{canvas_colour[0], canvas_colour[1], canvas_colour[2], 255}));
    OA_CHECK(
        (read.at(31, 21) ==
         Pixel{texels.at(21, 11)[0], texels.at(21, 11)[1], texels.at(21, 11)[2], 255})
    );
    OA_CHECK((read.at(29, 21) == Pixel{canvas_colour[0], canvas_colour[1], canvas_colour[2], 255}));
    // The scissor is cleared again when the executor is done.
    OA_CHECK(!SDL_RenderClipEnabled(fixture.canvas.renderer));
}

/// Render targets at factors 2 to 16: cleared, drawn into with vertices on
/// the texture's pixels, and resolved into the canvas by alpha and by
/// none, the reference reduced by halving; a second frame draws the same.
void test_render_targets_resolve_with_a_downscale() {
    for (const uint32_t factor : {2U, 4U, 8U, 16U}) {
        Fixture fixture;
        const Image texels = seeded(seed_page, 64, 64);
        const card::PageHandle page = fixture.make_page({texels});
        const card::TargetHandle target = fixture.make_target(40, 30, factor);
        // A new target reads back transparent black throughout, after its
        // one-pixel check.
        const Image fresh = fixture.canvas.read_target(fixture.executor.target_texture(target));
        OA_CHECK(fresh.width == 40 * factor && fresh.height == 30 * factor);
        OA_CHECK(std::all_of(fresh.pixels.begin(), fresh.pixels.end(), [](const Pixel& pixel) {
            return pixel == Pixel{0, 0, 0, 0};
        }));
        card::CardFrame frame;
        card::Batch clear;
        clear.operation = card::Operation::clear;
        clear.target = target;
        clear.colour = {0.0F, 0.0F, 0.0F, 0.0F};
        frame.batches.push_back(clear);
        const float step = 1.0F / static_cast<float>(factor);
        card::Index first = next_index(frame);
        // A textured quad between the target's pixels, and a slanted triangle.
        card::append_quad(frame, 2.0F + step, 3.0F, 16.0F, 16.0F, 0.0F, 0.0F, 0.25F, 0.25F, {});
        draw_since(frame, first, page, card::Blend::alpha, target);
        first = next_index(frame);
        append_triangle(
            frame,
            {card::Vertex{20.0F, 2.0F + step, {1.0F, level(128), 0.0F, 1.0F}, 0.0F, 0.0F},
             card::Vertex{38.0F, 5.0F, {0.0F, 1.0F, 0.0F, 1.0F}, 0.0F, 0.0F},
             card::Vertex{25.0F, 28.0F, {0.0F, 0.0F, 1.0F, level(192)}, 0.0F, 0.0F}}
        );
        draw_since(frame, first, {}, card::Blend::alpha, target);
        card::Batch over;
        over.operation = card::Operation::resolve;
        over.source = target;
        over.blend = card::Blend::alpha;
        over.destination = {50, 40, 40, 30};
        frame.batches.push_back(over);
        card::Batch copy = over;
        copy.blend = card::Blend::none;
        copy.destination = {100, 40, 40, 30};
        copy.scissored = true;
        copy.scissor = {100, 40, 30, 20};
        frame.batches.push_back(copy);
        OA_CHECK(card::check_frame(frame).empty());
        const std::string what = "render target at factor " + std::to_string(factor);
        fixture.run(what.c_str(), frame, false);
        OA_CHECK(fixture.executor.counts().resolves == 2);
        OA_CHECK(fixture.executor.counts().clears == 1);
        // The target's texture holds the drawing at the factor's size,
        // exact where nothing blended over the clear.
        SDL_Texture* texture = fixture.executor.target_texture(target);
        OA_CHECK(texture != nullptr);
        float width = 0.0F;
        float height = 0.0F;
        OA_CHECK(SDL_GetTextureSize(texture, &width, &height));
        OA_CHECK(
            width == 40.0F * static_cast<float>(factor) &&
            height == 30.0F * static_cast<float>(factor)
        );
        // A second frame of the same draws the same.
        const Image once = fixture.canvas.read();
        fixture.canvas.clear();
        fixture.reference.final_image = cleared_canvas();
        fixture.run(what.c_str(), frame, false);
        OA_CHECK(fixture.canvas.read().pixels == once.pixels);
        OA_CHECK(fixture.executor.counts().target_switches > 0);
    }
}

/// A transparent target drawn into by premultiplied alpha and resolved by
/// it composites each pixel by its coverage: a pixel half covered by an
/// opaque colour lands at half that colour over half the canvas, where a
/// resolve by straight alpha would weight the colour by its coverage
/// twice.
void test_a_transparent_target_resolves_by_premultiplied_alpha() {
    Fixture fixture;
    const card::PageHandle page = fixture.make_page({premultiplied(seeded(seed_page, 32, 32))});
    const card::TargetHandle target = fixture.make_target(40, 30, 2);
    card::CardFrame frame;
    card::Batch clear;
    clear.operation = card::Operation::clear;
    clear.target = target;
    clear.colour = {0.0F, 0.0F, 0.0F, 0.0F};
    frame.batches.push_back(clear);
    // An opaque red quad whose left edge halves the target's pixel column 2.
    card::Index first = next_index(frame);
    card::append_quad(
        frame, 2.5F, 3.0F, 16.0F, 16.0F, 0.0F, 0.0F, 0.0F, 0.0F, {1.0F, 0.0F, 0.0F, 1.0F}
    );
    draw_since(frame, first, {}, card::Blend::alpha_premultiplied, target);
    // The premultiplied page at three quarters, 1:1 to the target's
    // texture, and an untextured triangle with a premultiplied gradient.
    first = next_index(frame);
    card::append_quad(
        frame,
        20.0F,
        2.0F,
        16.0F,
        16.0F,
        0.0F,
        0.0F,
        1.0F,
        1.0F,
        {level(192), level(192), level(192), level(192)}
    );
    draw_since(frame, first, page, card::Blend::alpha_premultiplied, target);
    first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{4.0F, 20.0F, {level(128), 0.0F, 0.0F, level(128)}, 0.0F, 0.0F},
         card::Vertex{36.0F, 22.0F, {0.0F, level(128), 0.0F, level(128)}, 0.0F, 0.0F},
         card::Vertex{18.0F, 29.0F, {0.0F, 0.0F, level(64), level(64)}, 0.0F, 0.0F}}
    );
    draw_since(frame, first, {}, card::Blend::alpha_premultiplied, target);
    card::Batch over;
    over.operation = card::Operation::resolve;
    over.source = target;
    over.blend = card::Blend::alpha_premultiplied;
    over.destination = {50, 40, 40, 30};
    frame.batches.push_back(over);
    OA_CHECK(card::check_frame(frame).empty());
    fixture.run("transparent target resolved by premultiplied alpha", frame, false);
    // The half-covered column: the reduced pixel is red at half alpha, and
    // keeps half the canvas under it: 127 red, and half of the canvas's
    // 20, 40, 60. By straight alpha it would keep half the canvas under a
    // quarter of the red.
    const Image read = fixture.canvas.read();
    const Pixel half_covered = read.at(52, 43);
    const Pixel expected{137, 20, 30, 255};
    for (std::size_t channel = 0; channel < 3; ++channel)
        OA_CHECK(std::abs(int{half_covered[channel]} - int{expected[channel]}) <= most_difference);
    // The next column is covered whole: red, with nothing of the canvas.
    OA_CHECK((read.at(53, 43) == Pixel{255, 0, 0, 255}));
}

/// A draw's sampling mode is its own: the same page is drawn by nearest,
/// linear and pixel-art sampling in one frame, each 1:1 and so exact, the
/// mode set on the page's level only when it changes and keeping the
/// batches apart; a frame that draws it by one mode throughout merges
/// them.
void test_sampling_is_the_draws_own() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 16, 16);
    const card::PageHandle page = fixture.make_page({texels});
#if SDL_VERSION_ATLEAST(3, 4, 0)
    OA_CHECK(fixture.executor.capabilities().pixel_art_sampling);
#else
    OA_CHECK(!fixture.executor.capabilities().pixel_art_sampling);
#endif
    card::CardFrame frame;
    const std::array<card::Sampling, 3> modes{
        card::Sampling::nearest, card::Sampling::linear, card::Sampling::pixel_art
    };
    for (std::size_t at = 0; at < modes.size(); ++at) {
        const card::Index first = next_index(frame);
        card::append_quad(
            frame, 20.0F * static_cast<float>(at), 10.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {}
        );
        draw_since(frame, first, page, card::Blend::none).sampling = modes[at];
    }
    fixture.run("sampling modes", frame, true);
    OA_CHECK(fixture.executor.counts().draw_calls == 3);
    OA_CHECK(fixture.executor.counts().batches_merged == 0);
    // The level starts at nearest; linear, then pixel-art or nearest again
    // where the renderer lacks it, are two changes.
    OA_CHECK(fixture.executor.counts().sampling_changes == 2);
    card::CardFrame same;
    for (int at = 0; at < 3; ++at) {
        const card::Index first = next_index(same);
        card::append_quad(
            same, 20.0F * static_cast<float>(at), 40.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {}
        );
        draw_since(same, first, page, card::Blend::none).sampling = card::Sampling::linear;
    }
    fixture.run("one sampling mode", same, true);
    OA_CHECK(fixture.executor.counts().draw_calls == 4);
    OA_CHECK(fixture.executor.counts().batches_merged == 2);
    OA_CHECK(fixture.executor.counts().sampling_changes == 3);
}

/// A factor-4 target cleared or drawn into again between two resolves is
/// halved again, so each resolve shows the content of its moment, not the
/// half kept from the first; two resolves with nothing written between
/// share one halving.
void test_a_target_written_again_resolves_again() {
    Fixture fixture;
    const card::TargetHandle target = fixture.make_target(8, 8, 4);
    card::CardFrame frame;
    const auto clear_to = [&](const card::Colour& colour) {
        card::Batch clear;
        clear.operation = card::Operation::clear;
        clear.target = target;
        clear.colour = colour;
        frame.batches.push_back(clear);
    };
    const auto resolve_at = [&](int32_t x) {
        card::Batch resolve;
        resolve.operation = card::Operation::resolve;
        resolve.source = target;
        resolve.destination = {x, 10, 8, 8};
        frame.batches.push_back(resolve);
    };
    clear_to({1.0F, 0.0F, 0.0F, 1.0F});
    resolve_at(10);
    clear_to({0.0F, 1.0F, 0.0F, 1.0F});
    resolve_at(30);
    // A draw over the whole target, in blue, then two resolves of it.
    const card::Index first = next_index(frame);
    card::append_quad(
        frame, 0.0F, 0.0F, 8.0F, 8.0F, 0.0F, 0.0F, 0.0F, 0.0F, {0.0F, 0.0F, 1.0F, 1.0F}
    );
    draw_since(frame, first, {}, card::Blend::none, target);
    resolve_at(50);
    resolve_at(70);
    OA_CHECK(card::check_frame(frame).empty());
    fixture.run("a target written between resolves", frame, true);
    const Image read = fixture.canvas.read();
    OA_CHECK((read.at(10, 10) == Pixel{255, 0, 0, 255}));
    OA_CHECK((read.at(30, 10) == Pixel{0, 255, 0, 255}));
    OA_CHECK((read.at(50, 10) == Pixel{0, 0, 255, 255}));
    OA_CHECK((read.at(77, 17) == Pixel{0, 0, 255, 255}));
    OA_CHECK(fixture.executor.counts().resolves == 4);
}

/// A resolve into a target at the source's own factor, 2 or 4, halves
/// nothing and lands the source texel for texel: the second target's
/// texture equals the first's, a seeded page's texels drawn between its
/// pixels included, where a source halved first and enlarged again would
/// not.
void test_a_resolve_into_a_target_at_its_factor_lands_texel_for_texel() {
    for (const uint32_t factor : {2U, 4U}) {
        Fixture fixture;
        const Image texels = seeded(seed_page, 64, 64);
        const card::PageHandle page = fixture.make_page({texels});
        const card::TargetHandle drawn = fixture.make_target(24, 20, factor);
        const card::TargetHandle copied = fixture.make_target(24, 20, factor);
        card::CardFrame frame;
        const card::Index first = next_index(frame);
        card::append_quad(frame, 0.0F, 0.0F, 24.0F, 20.0F, 0.0F, 0.0F, 0.75F, 0.625F, {});
        draw_since(frame, first, page, card::Blend::none, drawn);
        card::Batch onto;
        onto.operation = card::Operation::resolve;
        onto.source = drawn;
        onto.target = copied;
        onto.destination = {0, 0, 24, 20};
        frame.batches.push_back(onto);
        OA_CHECK(card::check_frame(frame).empty());
        OA_CHECK(fixture.executor.execute(frame, nullptr));
        OA_CHECK(fixture.executor.counts().halvings == 0);
        const Image from = fixture.canvas.read_target(fixture.executor.target_texture(drawn));
        const Image into = fixture.canvas.read_target(fixture.executor.target_texture(copied));
        OA_CHECK(from.width == 24 * factor && into.width == from.width);
        OA_CHECK(into.pixels == from.pixels);
    }
}

/// A frame drawn into a target at a supersampling factor and reduced to its
/// size: an edge that falls between the target's pixels lands at the share
/// of the pixel the drawing covers, one of two columns at 127 at factor 2
/// and one of four at 63 or three of four at 191 at factor 4, a slanted
/// edge at each pixel's share of samples, and the whole frame within the
/// tolerance of the reference reduced by halving. The target's texture
/// bytes are printed.
void test_supersampled_edges_reduce_to_their_coverage() {
    for (const uint32_t factor : {2U, 4U, 8U, 16U}) {
        Fixture fixture;
        const card::TargetHandle target = fixture.make_target(40, 30, factor);
        const uint64_t texture_bytes = uint64_t{40} * 30 * factor * factor * card::texel_bytes;
        // The halves the resolve reduces through: one fewer than the
        // factor's doublings, each a quarter of the one before.
        uint64_t half_bytes = 0;
        for (uint32_t halving = 1, doubled = 4; doubled <= factor; ++halving, doubled *= 2)
            half_bytes += texture_bytes >> (2 * halving);
        std::printf(
            "a 40x30 target at factor %u holds %llu bytes of textures\n",
            factor,
            static_cast<unsigned long long>(fixture.executor.counts().texture_bytes)
        );
        OA_CHECK(fixture.executor.counts().texture_bytes == texture_bytes + half_bytes);
        card::CardFrame frame;
        card::Batch clear;
        clear.operation = card::Operation::clear;
        clear.target = target;
        clear.colour = {0.0F, 0.0F, 0.0F, 1.0F};
        frame.batches.push_back(clear);
        const float step = 1.0F / static_cast<float>(factor);
        const card::Colour white{};
        const card::Index first = next_index(frame);
        // A square whose left edge lies one step past column 2, so that the
        // square covers factor - 1 of the column's samples, and whose right
        // edge is column 12's left edge; a square whose right edge lies one
        // step into column 30; and a triangle whose slanted edge drops one
        // row every two columns, from (14, 16) to (2, 22), so that no sample
        // centre lies on it.
        card::append_quad(
            frame, 2.0F + step, 3.0F, 10.0F - step, 10.0F, 0.0F, 0.0F, 0.0F, 0.0F, white
        );
        card::append_quad(frame, 20.0F, 3.0F, 10.0F + step, 10.0F, 0.0F, 0.0F, 0.0F, 0.0F, white);
        append_triangle(
            frame,
            {card::Vertex{2.0F, 16.0F, white, 0.0F, 0.0F},
             card::Vertex{14.0F, 16.0F, white, 0.0F, 0.0F},
             card::Vertex{2.0F, 22.0F, white, 0.0F, 0.0F}}
        );
        draw_since(frame, first, {}, card::Blend::none, target);
        card::Batch resolve;
        resolve.operation = card::Operation::resolve;
        resolve.source = target;
        resolve.destination = {10, 10, 40, 30};
        frame.batches.push_back(resolve);
        OA_CHECK(card::check_frame(frame).empty());
        const std::string what = "supersampled edges at factor " + std::to_string(factor);
        fixture.run(what.c_str(), frame, false);
        const Image read = fixture.canvas.read();
        const Pixel black{0, 0, 0, 255};
        const Pixel full{255, 255, 255, 255};
        // The shares, (factor - 1) / factor and 1 / factor of white, within
        // the rounding of each halving: 3 levels across four of them.
        const auto near_grey = [](const Pixel& pixel, double share) {
            const int level = pixel[0];
            return pixel[1] == pixel[0] && pixel[2] == pixel[0] && pixel[3] == 255 &&
                   std::abs(level - static_cast<int>(255.0 * share)) <= 3;
        };
        OA_CHECK(read.at(11, 15) == black);
        OA_CHECK(near_grey(read.at(12, 15), (factor - 1.0) / factor));
        OA_CHECK(read.at(13, 15) == full);
        OA_CHECK(read.at(21, 15) == full);
        OA_CHECK(read.at(22, 15) == black);
        OA_CHECK(read.at(39, 15) == full);
        OA_CHECK(near_grey(read.at(40, 15), 1.0 / factor));
        OA_CHECK(read.at(41, 15) == black);
        // The triangle's row 19: column 5 inside, column 6 covered three
        // quarters at every factor, column 7 one quarter, column 8 outside.
        OA_CHECK(read.at(15, 29) == full);
        OA_CHECK(near_grey(read.at(16, 29), 0.75));
        OA_CHECK(near_grey(read.at(17, 29), 0.25));
        OA_CHECK(read.at(18, 29) == black);
    }
}

/// Returns an image with every pixel opaque.
///
/// @param image the image
/// @return the image with alpha 255 throughout
Image opaque(Image image) {
    for (auto& pixel : image.pixels)
        pixel[3] = 255;
    return image;
}

/// The two-level reduction of a render target that keeps its half: the
/// target drawn 1:1 and its texture reduced by one half, where the result
/// is the box of four pixels; by three quarters, under a scissor, and by
/// nine tenths of a part within, where the half at twice the scale lies
/// under the part at alpha 1 - log2(1 / scale); and by 1, where it is the
/// part itself; at factor 1, and at factor 2 with the part in the texture's
/// pixels; each within the tolerance of the reference, which stretches as
/// the renderer does; the half made once a run and again for the next.
void test_two_level_reductions_match_the_reference() {
    const Image texels = opaque(seeded(seed_page, 64, 64));
    constexpr uint32_t texture_width = 96;
    constexpr uint32_t texture_height = 72;
    for (const uint32_t factor : {1U, 2U}) {
        Fixture fixture;
        const card::PageHandle page = fixture.make_page({texels});
        const card::TargetHandle target =
            fixture.make_target(texture_width / factor, texture_height / factor, factor, true);
        // The texture and its half.
        const uint64_t texture_bytes = uint64_t{texture_width} * texture_height * card::texel_bytes;
        OA_CHECK(
            fixture.executor.counts().texture_bytes ==
            texels.pixels.size() * card::texel_bytes + texture_bytes + texture_bytes / 4
        );
        card::CardFrame frame;
        card::Batch clear;
        clear.operation = card::Operation::clear;
        clear.target = target;
        clear.colour = {level(26), level(51), level(77), 1.0F};
        frame.batches.push_back(clear);
        // Vertices in pixels of the size: one texture pixel is `unit`.
        const float unit = 1.0F / static_cast<float>(factor);
        card::Index first = next_index(frame);
        card::append_quad(
            frame, 0.0F, 0.0F, 64.0F * unit, 64.0F * unit, 0.0F, 0.0F, 1.0F, 1.0F, {}
        );
        draw_since(frame, first, page, card::Blend::none, target);
        first = next_index(frame);
        append_triangle(
            frame,
            {card::Vertex{66.0F * unit, 4.0F * unit, {1.0F, 0.0F, 0.0F, 1.0F}, 0.0F, 0.0F},
             card::Vertex{94.0F * unit, 10.0F * unit, {0.0F, 1.0F, 0.0F, 1.0F}, 0.0F, 0.0F},
             card::Vertex{70.0F * unit, 68.0F * unit, {0.0F, 0.0F, 1.0F, 1.0F}, 0.0F, 0.0F}}
        );
        card::append_quad(
            frame,
            8.0F * unit,
            62.0F * unit,
            50.0F * unit,
            8.0F * unit,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            {1.0F, 1.0F, 0.0F, 1.0F}
        );
        draw_since(frame, first, {}, card::Blend::none, target);
        const auto reduce = [&](const card::Rect& part, const card::Rect& destination) {
            card::Batch batch;
            batch.operation = card::Operation::blend_reduce;
            batch.source = target;
            batch.source_part = part;
            batch.destination = destination;
            frame.batches.push_back(batch);
            return frame.batches.size() - 1;
        };
        const card::Rect whole{0, 0, texture_width, texture_height};
        reduce(whole, {0, 0, 48, 36});
        const std::size_t scissored = reduce(whole, {50, 0, 72, 54});
        frame.batches[scissored].scissored = true;
        frame.batches[scissored].scissor = {54, 4, 60, 40};
        reduce(whole, {0, 40, 96, 72});
        reduce({16, 12, 40, 30}, {120, 60, 36, 27});
        OA_CHECK(card::check_frame(frame).empty());
        const std::string what = "two-level reductions at factor " + std::to_string(factor);
        // The reduction under a scissor is a scaled draw the scissor cuts.
        const bool held = drawn_as_reference_from(scissored_scaling_version);
        fixture.run(what.c_str(), frame, false, held);
        OA_CHECK(fixture.executor.counts().blend_reductions == 4);
        OA_CHECK(fixture.executor.counts().halvings == 1);
        // The reduction by one half is the box of four texture pixels,
        // within the level the renderer's two truncations may lose.
        const Image boxed = halve(fixture.reference.targets.at(target.value).image);
        const Image read = fixture.canvas.read();
        int most = 0;
        for (uint32_t y = 0; y < boxed.height; ++y)
            for (uint32_t x = 0; x < boxed.width; ++x)
                for (std::size_t channel = 0; channel < 3; ++channel)
                    most = std::max(
                        most, std::abs(int{read.at(x, y)[channel]} - int{boxed.at(x, y)[channel]})
                    );
        std::printf(
            "%s: the reduction by one half against the box of four: most %d\n", what.c_str(), most
        );
        OA_CHECK(most <= 1);
        // Outside the scissor the canvas is untouched.
        OA_CHECK(
            (read.at(52, 2) == Pixel{canvas_colour[0], canvas_colour[1], canvas_colour[2], 255})
        );
        // The reduction by 1 is the texture itself.
        const Image& texture = fixture.reference.targets.at(target.value).image;
        OA_CHECK(
            read.at(10, 50)[0] == texture.at(10, 10)[0] &&
            read.at(90, 100)[2] == texture.at(90, 60)[2]
        );
        // A second frame halves again and draws the same.
        fixture.canvas.clear();
        fixture.reference.final_image = cleared_canvas();
        fixture.run(what.c_str(), frame, false, held);
        OA_CHECK(fixture.canvas.read().pixels == read.pixels);
        OA_CHECK(fixture.executor.counts().halvings == 2);
    }
}

/// A page's levels hold what the engine wrote into each, a part of one
/// updated, and a draw names the level it reads.
void test_levels_of_a_page() {
    Fixture fixture;
    const Image level0 = seeded(seed_page, 32, 32);
    const Image level1 = seeded(seed_level, 16, 16);
    Image level2 = seeded(seed_level + 1, 8, 8);
    const card::PageHandle page = fixture.make_page({level0, level1, level2});
    OA_CHECK(fixture.executor.page_texture(page, 2) != nullptr);
    OA_CHECK(fixture.executor.page_texture(page, 3) == nullptr);
    // A part of level 2 is written again.
    const Image patch = seeded(seed_level + 2, 4, 4);
    const card::Rect part{2, 2, 4, 4};
    const auto patch_bytes = patch.bytes();
    OA_CHECK(fixture.executor.update_page(page, 2, &part, patch_bytes.data(), patch.pitch()));
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            level2.at(2 + x, 2 + y) = patch.at(x, y);
    fixture.reference.pages[page.value].levels[2] = level2;
    card::CardFrame frame;
    card::Index first = next_index(frame);
    card::append_quad(frame, 5.0F, 5.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, first, page, card::Blend::none).level = 1;
    first = next_index(frame);
    card::append_quad(frame, 40.0F, 5.0F, 8.0F, 8.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, first, page, card::Blend::none).level = 2;
    first = next_index(frame);
    card::append_quad(frame, 60.0F, 5.0F, 32.0F, 32.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, first, page, card::Blend::none);
    fixture.run("levels of a page", frame, true);
    const Image read = fixture.canvas.read();
    OA_CHECK(read.at(5, 5)[0] == level1.at(0, 0)[0] && read.at(20, 20)[2] == level1.at(15, 15)[2]);
    OA_CHECK(read.at(42, 7)[1] == patch.at(0, 0)[1]);
    OA_CHECK(read.at(40, 5)[1] == level2.at(0, 0)[1]);
    // A level's edge halves and never falls below 1.
    OA_CHECK(
        card::level_edge(32, 3) == 4 && card::level_edge(5, 2) == 1 && card::level_edge(1, 7) == 1
    );
}

/// Malformed frames are refused by check_frame, and the executor refuses
/// them and frames naming pages and targets that do not exist, drawing
/// nothing.
void test_malformed_frames_are_refused() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 16, 16);
    const card::PageHandle page = fixture.make_page({texels});
    const card::TargetHandle target = fixture.make_target(16, 16, 2);
    card::CardFrame good;
    card::append_quad(good, 1.0F, 1.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(good, 0, page, card::Blend::alpha);
    OA_CHECK(card::check_frame(good).empty());

    const auto refused = [&](const char* what, const card::CardFrame& frame, const char* word) {
        const std::string fault = card::check_frame(frame);
        const bool named = fault.find(word) != std::string::npos;
        OA_CHECK(!fault.empty());
        OA_CHECK(named);
        if (!named)
            std::fprintf(stderr, "%s: check_frame said \"%s\"\n", what, fault.c_str());
        const Image before = fixture.canvas.read();
        const uint64_t refusals = fixture.executor.counts().frames_refused;
        OA_CHECK(!fixture.executor.execute(frame, nullptr));
        OA_CHECK(fixture.executor.error().find("refused") != std::string::npos);
        OA_CHECK(fixture.executor.counts().frames_refused == refusals + 1);
        OA_CHECK(fixture.canvas.read().pixels == before.pixels);
    };
    card::CardFrame frame = good;
    frame.indices[2] = 4;
    refused("an index beyond the vertices", frame, "names vertex 4 of 4");
    frame = good;
    frame.batches[0].index_count = 4;
    refused("indices that are not whole triangles", frame, "not whole triangles");
    frame = good;
    frame.batches[0].first_index = 3;
    refused("a range beyond the indices", frame, "beyond the 6");
    frame = good;
    frame.batches[0].blend = static_cast<card::Blend>(card::blend_count);
    refused("a blend that names none", frame, "blend 8 names none");
    frame = good;
    frame.batches[0].sampling = static_cast<card::Sampling>(9);
    refused("a sampling that names none", frame, "sampling 9 names none");
    frame = good;
    frame.batches[0].operation = static_cast<card::Operation>(4);
    refused("an operation that names none", frame, "operation 4 names none");
    frame = good;
    frame.batches[0].scissored = true;
    frame.batches[0].scissor = {0, 0, 10, 0};
    refused("an empty scissor", frame, "empty scissor");
    frame = good;
    frame.batches[0].page = {};
    frame.batches[0].level = 1;
    refused("a level without a page", frame, "without a page");
    frame = good;
    frame.batches[0].level = card::most_page_levels;
    refused("a level beyond the most", frame, "beyond the 8 levels");
    frame = good;
    frame.vertices[1].x = std::nanf("");
    refused("a vertex that is not a number", frame, "vertex 1");
    frame = good;
    frame.vertices[2].y = 2.0F * card::largest_coordinate;
    refused("a vertex beyond reach", frame, "vertex 2");
    frame = good;
    frame.vertices[3].colour.alpha = std::nanf("");
    refused("a colour that is not a number", frame, "vertex 3");
    frame = good;
    card::Batch resolve;
    resolve.operation = card::Operation::resolve;
    resolve.destination = {0, 0, 16, 16};
    frame.batches.push_back(resolve);
    refused("a resolve of no target", frame, "no render target");
    frame.batches.back().source = target;
    frame.batches.back().target = target;
    refused("a resolve into itself", frame, "into itself");
    frame.batches.back().target = {};
    frame.batches.back().blend = card::Blend::additive;
    refused("a resolve by additive", frame, "only by none, alpha or premultiplied alpha");
    frame.batches.back().blend = card::Blend::none;
    frame.batches.back().destination = {0, 0, 0, 16};
    refused("an empty destination", frame, "empty destination");
    // A resolve lands less than a pixel past its destination; a whole pixel,
    // or a shift on a draw, is refused.
    frame.batches.back().destination = {0, 0, 16, 16};
    frame.batches.back().shift_x = -0.75F;
    frame.batches.back().shift_y = 0.25F;
    OA_CHECK(card::check_frame(frame).empty());
    frame.batches.back().shift_x = 1.0F;
    refused("a shift of a whole pixel", frame, "a whole pixel or more");
    frame.batches.back().shift_x = std::nanf("");
    refused("a shift that is not a number", frame, "not a number");
    frame = good;
    frame.batches[0].shift_y = 0.5F;
    refused("a shift on a draw", frame, "neither a resolve nor");
    frame = good;
    card::Batch clear;
    clear.operation = card::Operation::clear;
    clear.colour.red = std::nanf("");
    frame.batches.push_back(clear);
    refused("a clear colour that is not a number", frame, "clear colour");
    // The two-level reduction: a well-formed one of the target's whole
    // 32x32 texture into 16x16, then each fault.
    card::Batch two_level;
    two_level.operation = card::Operation::blend_reduce;
    two_level.source = target;
    two_level.source_part = {0, 0, 32, 32};
    two_level.destination = {0, 0, 16, 16};
    frame = good;
    frame.batches.push_back(two_level);
    OA_CHECK(card::check_frame(frame).empty());
    frame.batches.back().source = {};
    refused("a two-level reduction of no target", frame, "reduction of no render target");
    frame.batches.back().source = target;
    frame.batches.back().target = target;
    refused("a two-level reduction into itself", frame, "into itself");
    frame.batches.back().target = {};
    frame.batches.back().blend = card::Blend::alpha;
    refused("a two-level reduction by alpha", frame, "blends only by none");
    frame.batches.back().blend = card::Blend::none;
    frame.batches.back().source_part = {0, 0, 32, 0};
    refused("an empty source part", frame, "empty source part");
    frame.batches.back().source_part = {-2, 0, 32, 32};
    refused("a source part left of the texture", frame, "left of or above");
    frame.batches.back().source_part = {0, 1, 32, 32};
    refused("a source part on an odd row", frame, "not on even pixels");
    frame.batches.back().source_part = {0, 0, 32, 32};
    frame.batches.back().destination = {0, 0, 8, 16};
    refused("a two-level reduction below one half", frame, "by one half to 1");
    frame.batches.back().destination = {0, 0, 16, 40};
    refused("a two-level reduction above 1", frame, "by one half to 1");
    frame.batches.back().destination = {0, 0, 16, 16};
    frame.batches.back().scissored = true;
    frame.batches.back().scissor = {0, 0, 0, 4};
    refused("a two-level reduction with an empty scissor", frame, "empty scissor");

    // Frames that are well formed but name what the executor does not have.
    const auto executor_refuses =
        [&](const char* what, const card::CardFrame& named, const char* word) {
            OA_CHECK(card::check_frame(named).empty());
            const Image before = fixture.canvas.read();
            OA_CHECK(!fixture.executor.execute(named, nullptr));
            const bool said = fixture.executor.error().find(word) != std::string::npos;
            OA_CHECK(said);
            if (!said)
                std::fprintf(
                    stderr, "%s: execute said \"%s\"\n", what, fixture.executor.error().c_str()
                );
            OA_CHECK(fixture.canvas.read().pixels == before.pixels);
        };
    frame = good;
    frame.batches[0].page = {page.value + 1U};
    executor_refuses("a page that never was", frame, "is not alive");
    frame = good;
    frame.batches[0].level = 1;
    executor_refuses("a level the page lacks", frame, "has no level 1");
    frame = good;
    frame.batches[0].target = {target.value + 1U};
    executor_refuses("a target that never was", frame, "is not alive");
    frame = good;
    resolve.source = {target.value + 1U};
    resolve.destination = {0, 0, 16, 16};
    frame.batches.push_back(resolve);
    executor_refuses("a resolve of a target that never was", frame, "the source");
    // A two-level reduction needs a target made with its half, and a part
    // within its texture.
    frame = good;
    frame.batches.push_back(two_level);
    executor_refuses("a two-level reduction of a target without a half", frame, "without its half");
    const card::TargetHandle halved = fixture.make_target(16, 16, 2, true);
    frame.batches.back().source = halved;
    OA_CHECK(fixture.executor.execute(frame, nullptr));
    frame.batches.back().source_part = {0, 0, 32, 34};
    frame.batches.back().destination = {0, 0, 16, 17};
    executor_refuses("a part beyond the texture", frame, "beyond render target");
    frame.batches.back().source_part = {4, 0, 32, 32};
    frame.batches.back().destination = {0, 0, 16, 16};
    executor_refuses("a part past the texture's right edge", frame, "beyond render target");
    fixture.executor.destroy_target(halved);
    // The final target may not be one of the executor's own.
    OA_CHECK(!fixture.executor.execute(good, fixture.executor.target_texture(target)));
    OA_CHECK(fixture.executor.error().find("final target") != std::string::npos);
    // A destroyed page is not alive, and its handle names nothing after.
    fixture.executor.destroy_page(page);
    OA_CHECK(!fixture.executor.page_alive(page));
    OA_CHECK(fixture.executor.page_texture(page, 0) == nullptr);
    executor_refuses("a destroyed page", good, "is not alive");
    // A page made in the slot after it has a new handle, and the old one still misses.
    const card::PageHandle next = fixture.make_page({texels});
    OA_CHECK(
        next != page && fixture.executor.page_alive(next) && !fixture.executor.page_alive(page)
    );
    executor_refuses("a destroyed page whose slot is used again", good, "is not alive");
    fixture.executor.destroy_target(target);
    OA_CHECK(!fixture.executor.target_alive(target));
    OA_CHECK(
        fixture.executor.counts().pages_alive == 1 && fixture.executor.counts().targets_alive == 0
    );
    // Destroying what is gone is ignored.
    fixture.executor.destroy_page(page);
    fixture.executor.destroy_target(target);
    // A frame is also refused when the executor is closed.
    fixture.executor.close();
    OA_CHECK(!fixture.executor.is_open());
    OA_CHECK(!fixture.executor.execute(good, nullptr));
    OA_CHECK(
        fixture.executor.counts().pages_alive == 0 && fixture.executor.counts().texture_bytes == 0
    );
}

/// A retired page draws on in the next frame, as it was, and is destroyed
/// once that frame has run, refused or not; a frame refused before drawing
/// says so, and one that ran does not.
void test_a_retired_page_lasts_its_frame() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 16, 16);
    const card::PageHandle page = fixture.make_page({texels});
    card::CardFrame frame;
    card::append_quad(frame, 1.0F, 1.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, 0, page, card::Blend::none);
    fixture.executor.retire_page(page);
    OA_CHECK(fixture.executor.page_alive(page));
    fixture.run("a retired page's last frame", frame, true);
    OA_CHECK(!fixture.executor.frame_refused());
    OA_CHECK(!fixture.executor.page_alive(page));
    OA_CHECK(!fixture.executor.execute(frame, nullptr));
    OA_CHECK(fixture.executor.frame_refused());
    OA_CHECK(fixture.executor.error().find("is not alive") != std::string::npos);
    const card::PageHandle next = fixture.make_page({texels});
    fixture.executor.retire_page(next);
    OA_CHECK(!fixture.executor.execute(frame, nullptr));
    OA_CHECK(!fixture.executor.page_alive(next));
    // Retiring a page that is gone is ignored.
    fixture.executor.retire_page(page);
    OA_CHECK(fixture.executor.counts().pages_alive == 0);
}

/// A frame of more than a million vertices, as a battle of thousands of
/// units at the widest zoom builds, runs whole.
void test_a_frame_of_many_vertices_runs() {
    Fixture fixture;
    constexpr uint32_t quads = 300'000;
    card::CardFrame frame;
    frame.vertices.reserve(std::size_t{quads} * 4U);
    frame.indices.reserve(std::size_t{quads} * 6U);
    for (uint32_t at = 0; at < quads; ++at) {
        const auto x = static_cast<float>(at % canvas_width);
        const auto y = static_cast<float>((at / canvas_width) % canvas_height);
        card::append_quad(
            frame, x, y, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, {1.0F, 1.0F, 1.0F, 1.0F}
        );
    }
    draw_since(frame, 0, {}, card::Blend::none);
    OA_CHECK(frame.vertices.size() > std::size_t{1} << 20);
    OA_CHECK(card::check_frame(frame).empty());
    const bool ran = fixture.executor.execute(frame, nullptr);
    OA_CHECK(ran);
    if (!ran)
        std::fprintf(stderr, "execute: %s\n", fixture.executor.error().c_str());
    OA_CHECK(fixture.executor.counts().triangles == uint64_t{quads} * 2U);
    const Image read = fixture.canvas.read();
    OA_CHECK(read.pixels.front() == (Pixel{255, 255, 255, 255}));
}

/// Pages and targets beyond the limits, and parts outside a level, are
/// refused with an error naming what and the limit.
void test_pages_beyond_the_limit_are_refused() {
    Fixture fixture(128);
    OA_CHECK(fixture.executor.capabilities().texture_limit == 128);
    const auto refused = [&](const char* what, bool made, const char* word) {
        OA_CHECK(!made);
        const bool said = fixture.executor.error().find(word) != std::string::npos;
        OA_CHECK(said);
        if (!said)
            std::fprintf(stderr, "%s: error was \"%s\"\n", what, fixture.executor.error().c_str());
    };
    card::PageDescription description;
    description.width = 256;
    description.height = 16;
    refused(
        "a wide page",
        fixture.executor.create_page(description) != card::PageHandle{},
        "256x16 texels is beyond the renderer's texture limit of 128"
    );
    description.width = 16;
    description.height = 129;
    refused(
        "a tall page",
        fixture.executor.create_page(description) != card::PageHandle{},
        "texture limit of 128"
    );
    description.height = 0;
    refused(
        "a page with no texels",
        fixture.executor.create_page(description) != card::PageHandle{},
        "no texels"
    );
    description.height = 16;
    description.level_count = 0;
    refused(
        "a page of no levels",
        fixture.executor.create_page(description) != card::PageHandle{},
        "1 to 8"
    );
    description.level_count = card::most_page_levels + 1;
    refused(
        "a page of too many levels",
        fixture.executor.create_page(description) != card::PageHandle{},
        "1 to 8"
    );
    description.level_count = 2;
    description.width = 128;
    description.height = 128;
    const card::PageHandle page = fixture.executor.create_page(description);
    OA_CHECK(page != card::PageHandle{});
    OA_CHECK(
        fixture.executor.counts().texture_bytes == (128U * 128U + 64U * 64U) * card::texel_bytes
    );
    const Image filled(128, 128, {1, 2, 3, 255});
    const std::vector<uint8_t> texels = filled.bytes();
    const card::Rect outside{120, 0, 16, 16};
    refused(
        "a part outside the level",
        fixture.executor.update_page(page, 0, &outside, texels.data(), filled.pitch()),
        "outside level 0"
    );
    const card::Rect inside{0, 0, 16, 16};
    // A pitch of 8 bytes holds two of the part's 16 texels.
    refused(
        "a short pitch",
        fixture.executor.update_page(page, 0, &inside, texels.data(), 8),
        "pitch of 8 bytes under the 64"
    );
    refused(
        "a pitch beyond reach",
        fixture.executor.update_page(page, 0, &inside, texels.data(), 0x80000000U),
        "beyond reach"
    );
    refused(
        "no texels",
        fixture.executor.update_page(page, 0, &inside, nullptr, filled.pitch()),
        "no texels"
    );
    refused(
        "a level the page lacks",
        fixture.executor.update_page(page, 2, nullptr, texels.data(), filled.pitch()),
        "has no level 2"
    );
    OA_CHECK(fixture.executor.update_page(page, 1, nullptr, texels.data(), 64 * card::texel_bytes));
    refused(
        "a target beyond the limit",
        fixture.executor.create_target(100, 100, 2) != card::TargetHandle{},
        "needs a texture of 200x200, beyond the renderer's texture limit of 128"
    );
    refused(
        "a factor of 3",
        fixture.executor.create_target(10, 10, 3) != card::TargetHandle{},
        "a power of two from 1 to 16"
    );
    refused(
        "a target with no pixels",
        fixture.executor.create_target(0, 10, 1) != card::TargetHandle{},
        "no pixels"
    );
    const card::TargetHandle target = fixture.executor.create_target(32, 32, 4);
    OA_CHECK(target != card::TargetHandle{});
    OA_CHECK(
        fixture.executor.counts().texture_bytes ==
        (128U * 128U + 64U * 64U + 128U * 128U + 64U * 64U) * card::texel_bytes
    );
    fixture.executor.destroy_target(target);
    OA_CHECK(
        fixture.executor.counts().texture_bytes == (128U * 128U + 64U * 64U) * card::texel_bytes
    );
    // Beyond the largest page edge, whatever the renderer's limit.
    card::Executor unlimited;
    OA_CHECK(unlimited.open(fixture.canvas.renderer, 0));
    description.level_count = 1;
    description.width = card::largest_page_edge + 1;
    description.height = 1;
    OA_CHECK(unlimited.create_page(description) == card::PageHandle{});
    OA_CHECK(unlimited.error().find("largest page edge") != std::string::npos);
    OA_CHECK(unlimited.create_target(card::largest_target_edge, 1, 2) == card::TargetHandle{});
    OA_CHECK(unlimited.error().find("largest target edge") != std::string::npos);
    // An executor with no renderer.
    card::Executor closed;
    OA_CHECK(!closed.open(nullptr, 0));
    OA_CHECK(closed.create_page(description) == card::PageHandle{});
    OA_CHECK(closed.error().find("not open") != std::string::npos);
}

/// Draw batches that share their state and follow on in the indices run
/// in one geometry call; a change of state or a gap starts another.
void test_batches_that_share_their_state_merge() {
    Fixture fixture;
    const Image texels = seeded(seed_page, 16, 16);
    const card::PageHandle page = fixture.make_page({texels});
    card::CardFrame frame;
    for (int at = 0; at < 3; ++at) {
        const card::Index first = next_index(frame);
        card::append_quad(
            frame, 16.0F * static_cast<float>(at), 0.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {}
        );
        draw_since(frame, first, page, card::Blend::alpha);
    }
    card::Index first = next_index(frame);
    card::append_quad(frame, 0.0F, 20.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, first, page, card::Blend::additive);
    // The same state as the last, but after a gap in the indices.
    card::append_quad(frame, 20.0F, 20.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    first = next_index(frame);
    card::append_quad(frame, 40.0F, 20.0F, 16.0F, 16.0F, 0.0F, 0.0F, 1.0F, 1.0F, {});
    draw_since(frame, first, page, card::Blend::additive);
    // An empty batch adds nothing and merges with its neighbours.
    card::Batch empty;
    empty.page = page;
    empty.blend = card::Blend::additive;
    empty.first_index = next_index(frame);
    frame.batches.push_back(empty);
    fixture.run("merged batches", frame, false);
    OA_CHECK(fixture.executor.counts().draw_calls == 3);
    OA_CHECK(fixture.executor.counts().batches_merged == 3);
    OA_CHECK(fixture.executor.counts().triangles == 10);
    OA_CHECK(fixture.executor.counts().blend_changes == 2);
    OA_CHECK(fixture.executor.counts().frames == 1);
}

/// The executor leaves the renderer's target, scissor, draw colour and draw
/// blend mode as it found them.
void test_the_renderers_state_is_put_back() {
    Fixture fixture;
    const card::TargetHandle target = fixture.make_target(8, 8, 1);
    SDL_Renderer* renderer = fixture.canvas.renderer;
    const SDL_Rect clip{3, 4, 50, 60};
    OA_CHECK(SDL_SetRenderClipRect(renderer, &clip));
    OA_CHECK(SDL_SetRenderDrawColor(renderer, 11, 22, 33, 44));
    OA_CHECK(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_MOD));
    card::CardFrame frame;
    card::Batch clear;
    clear.operation = card::Operation::clear;
    clear.target = target;
    clear.colour = {1.0F, 0.0F, 0.0F, 1.0F};
    frame.batches.push_back(clear);
    card::Index first = next_index(frame);
    append_triangle(
        frame,
        {card::Vertex{10.0F, 10.0F, {1.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{60.0F, 10.0F, {1.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 0.0F},
         card::Vertex{10.0F, 60.0F, {1.0F, 1.0F, 1.0F, 1.0F}, 0.0F, 0.0F}}
    );
    card::Batch& scissored = draw_since(frame, first, {}, card::Blend::additive);
    scissored.scissored = true;
    scissored.scissor = {0, 0, 30, 30};
    card::Batch resolve;
    resolve.operation = card::Operation::resolve;
    resolve.source = target;
    resolve.destination = {100, 100, 8, 8};
    frame.batches.push_back(resolve);
    OA_CHECK(fixture.executor.execute(frame, nullptr));
    OA_CHECK(SDL_GetRenderTarget(renderer) == nullptr);
    SDL_Rect found{};
    OA_CHECK(SDL_RenderClipEnabled(renderer));
    OA_CHECK(SDL_GetRenderClipRect(renderer, &found));
    OA_CHECK(found.x == clip.x && found.y == clip.y && found.w == clip.w && found.h == clip.h);
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    uint8_t alpha = 0;
    OA_CHECK(SDL_GetRenderDrawColor(renderer, &red, &green, &blue, &alpha));
    OA_CHECK(red == 11 && green == 22 && blue == 33 && alpha == 44);
    SDL_BlendMode mode = SDL_BLENDMODE_INVALID;
    OA_CHECK(SDL_GetRenderDrawBlendMode(renderer, &mode) && mode == SDL_BLENDMODE_MOD);
    // The resolve of the red target landed, inside the renderer's own scissor.
    const Image read = fixture.canvas.read();
    OA_CHECK((read.at(100, 100) == Pixel{255, 0, 0, 255}));
    OA_CHECK((read.at(2, 2) == Pixel{canvas_colour[0], canvas_colour[1], canvas_colour[2], 255}));
}

/// Measures and prints the processor cost of a frame of quads on the
/// software renderer: sprite quads of one colour, and lit quads with a
/// colour at each corner, which the renderer fills as triangles.
void benchmark_quads() {
    Canvas canvas(benchmark_width, benchmark_height);
    card::Executor executor;
    OA_CHECK(executor.open(canvas.renderer, 0));
    const Image texels = seeded(seed_benchmark, benchmark_page_edge, benchmark_page_edge);
    card::PageDescription description;
    description.width = benchmark_page_edge;
    description.height = benchmark_page_edge;
    const card::PageHandle page = executor.create_page(description);
    OA_CHECK(page != card::PageHandle{});
    const auto page_bytes = texels.bytes();
    OA_CHECK(executor.update_page(page, 0, nullptr, page_bytes.data(), texels.pitch()));
    for (const bool lit : {false, true})
        for (const uint32_t count : {5000U, 20000U}) {
            Random random{seed_benchmark + count};
            card::CardFrame frame;
            const auto texel_step = static_cast<float>(benchmark_quad_edge) / benchmark_page_edge;
            for (uint32_t at = 0; at < count; ++at) {
                const auto x =
                    static_cast<float>(random.next() % (benchmark_width - benchmark_quad_edge));
                const auto y =
                    static_cast<float>(random.next() % (benchmark_height - benchmark_quad_edge));
                const auto u = static_cast<float>(
                                   random.next() % (benchmark_page_edge / benchmark_quad_edge)
                               ) *
                               texel_step;
                const auto v = static_cast<float>(
                                   random.next() % (benchmark_page_edge / benchmark_quad_edge)
                               ) *
                               texel_step;
                const card::Index first = next_index(frame);
                card::append_quad(
                    frame,
                    x,
                    y,
                    benchmark_quad_edge,
                    benchmark_quad_edge,
                    u,
                    v,
                    u + texel_step,
                    v + texel_step,
                    {}
                );
                if (lit)
                    for (std::size_t corner = 0; corner < 4; ++corner)
                        frame.vertices[frame.vertices.size() - 4 + corner].colour.green =
                            level(static_cast<uint8_t>(64 * corner));
                // One batch a quad, merged by the executor.
                draw_since(frame, first, page, card::Blend::alpha);
            }
            OA_CHECK(card::check_frame(frame).empty());
            double execute_ms = 0.0;
            double flush_ms = 0.0;
            for (int pass = 0; pass <= benchmark_frames; ++pass) {
                canvas.clear();
                const auto started = std::chrono::steady_clock::now();
                OA_CHECK(executor.execute(frame, nullptr));
                const auto queued = std::chrono::steady_clock::now();
                OA_CHECK(SDL_FlushRenderer(canvas.renderer));
                const auto flushed = std::chrono::steady_clock::now();
                // The first pass warms the caches and is left out.
                if (pass == 0)
                    continue;
                execute_ms += std::chrono::duration<double, std::milli>(queued - started).count();
                flush_ms += std::chrono::duration<double, std::milli>(flushed - queued).count();
            }
            execute_ms /= benchmark_frames;
            flush_ms /= benchmark_frames;
            std::printf(
                "card frame of %u %s quads of %dx%d on the software renderer: "
                "%.2f ms to run, %.2f ms to draw, %.2f ms a frame, %llu geometry calls\n",
                count,
                lit ? "lit" : "sprite",
                benchmark_quad_edge,
                benchmark_quad_edge,
                execute_ms,
                flush_ms,
                execute_ms + flush_ms,
                static_cast<unsigned long long>(executor.counts().draw_calls)
            );
        }
}

} // namespace

int main() {
    if (!SDL_Init(0)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    test_opaque_draws_are_exact();
    test_blended_draws_match_the_reference();
    test_scissor_limits_the_pixels();
    test_render_targets_resolve_with_a_downscale();
    test_a_transparent_target_resolves_by_premultiplied_alpha();
    test_sampling_is_the_draws_own();
    test_a_target_written_again_resolves_again();
    test_a_resolve_into_a_target_at_its_factor_lands_texel_for_texel();
    test_supersampled_edges_reduce_to_their_coverage();
    test_two_level_reductions_match_the_reference();
    test_levels_of_a_page();
    test_malformed_frames_are_refused();
    test_a_retired_page_lasts_its_frame();
    test_a_frame_of_many_vertices_runs();
    test_pages_beyond_the_limit_are_refused();
    test_batches_that_share_their_state_merge();
    test_the_renderers_state_is_put_back();
    benchmark_quads();
    SDL_Quit();
    const int status = oa::test::check_exit_status();
    if (status == 0)
        std::puts("card frame: command lists draw on the software renderer as the reference does");
    return status;
}
