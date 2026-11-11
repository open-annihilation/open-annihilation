// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The grades, the vignette, the grain, the frames, the tilt-shift and the
// haze. Every number is an integer, so a picture comes out the same on
// every machine. The bands write only their own rows and share nothing
// that changes.
#include "oa/present/photo/looks.hpp"

#include "oa/platform/job_pool.hpp"
#include "oa/platform/text_font.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <vector>

namespace oa::present::photo {
namespace {

/// Rows one band covers. The last band of a picture may be shorter.
constexpr int32_t kBandRows = 32;

/// One colour grade: a 3×3 matrix in eighths of 256, a saturation in the
/// same units, and three tone curves. 256 is unchanged.
struct Grade {
    std::array<int16_t, 9> matrix{};
    int16_t saturation{256};
    std::array<std::array<uint8_t, 256>, 3> curve{};
};

/// Clamps a sample to a channel.
///
/// @param sample the sample
/// @return 0 to 255
constexpr int clamp_channel(int sample) noexcept {
    if (sample < 0)
        return 0;
    if (sample > 255)
        return 255;
    return sample;
}

/// One step of a tone curve: contrast about the middle, then brightness,
/// a lift that fades toward white, and a bias.
///
/// @param sample 0 to 255
/// @param contrast contrast, 256 unchanged
/// @param brightness brightness, 256 unchanged
/// @param lift added in the shadows
/// @param bias added to the channel
/// @return the curved sample
constexpr uint8_t tone_of(int sample, int contrast, int brightness, int lift, int bias) noexcept {
    int curved = 128 + (sample - 128) * contrast / 256;
    curved = curved * brightness / 256;
    curved += lift * (255 - sample) / 255;
    curved += bias;
    return static_cast<uint8_t>(clamp_channel(curved));
}

/// Fills one grade's three curves from one contrast and brightness.
///
/// @param grade the grade
/// @param contrast contrast, 256 unchanged
/// @param brightness brightness, 256 unchanged
/// @param lift shadow lift
/// @param bias_r red bias
/// @param bias_g green bias
/// @param bias_b blue bias
constexpr void fill_curves(
    Grade& grade, int contrast, int brightness, int lift, int bias_r, int bias_g, int bias_b
) noexcept {
    for (int sample = 0; sample < 256; ++sample) {
        grade.curve[0][static_cast<std::size_t>(sample)] =
            tone_of(sample, contrast, brightness, lift, bias_r);
        grade.curve[1][static_cast<std::size_t>(sample)] =
            tone_of(sample, contrast, brightness, lift, bias_g);
        grade.curve[2][static_cast<std::size_t>(sample)] =
            tone_of(sample, contrast, brightness, lift, bias_b);
    }
}

/// The six grades. None is the identity, so a picture run through it is
/// unchanged even where a look's other parts still run.
constexpr std::array<Grade, static_cast<std::size_t>(Preset::count)> grades = [] {
    std::array<Grade, static_cast<std::size_t>(Preset::count)> table{};
    Grade& plain = table[static_cast<std::size_t>(Preset::none)];
    plain.matrix = {256, 0, 0, 0, 256, 0, 0, 0, 256};
    plain.saturation = 256;
    fill_curves(plain, 256, 256, 0, 0, 0, 0);

    // Warm, a little more colour, a slight lift in the shadows.
    Grade& year = table[static_cast<std::size_t>(Preset::y1997)];
    year.matrix = {271, 8, 0, 5, 261, 0, 0, 8, 236};
    year.saturation = 302;
    fill_curves(year, 276, 256, 6, 4, 0, -6);

    // Orange highlights and a deep blue pulled down.
    Grade& dusk = table[static_cast<std::size_t>(Preset::dusk)];
    dusk.matrix = {294, 20, 0, 8, 243, 0, 0, 0, 184};
    dusk.saturation = 320;
    fill_curves(dusk, 271, 259, 0, 0, 0, 0);

    // Cool, less colour, a little brighter.
    Grade& arctic = table[static_cast<std::size_t>(Preset::arctic)];
    arctic.matrix = {220, 0, 10, 0, 251, 10, 0, 15, 297};
    arctic.saturation = 179;
    fill_curves(arctic, 271, 271, 0, 0, 0, 0);

    // No colour, harder contrast, slightly darker.
    Grade& noir = table[static_cast<std::size_t>(Preset::noir)];
    noir.matrix = {256, 0, 0, 0, 256, 0, 0, 0, 256};
    noir.saturation = 0;
    fill_curves(noir, 333, 240, 0, 0, 0, 0);

    // Washed colour, hard contrast, slightly brighter.
    Grade& bleach = table[static_cast<std::size_t>(Preset::bleach)];
    bleach.matrix = {256, 0, 0, 0, 256, 0, 0, 0, 256};
    bleach.saturation = 115;
    fill_curves(bleach, 346, 266, 0, 0, 0, 0);
    return table;
}();

/// Integer square root. The same value on every machine.
///
/// Newton's steps fall to the root from any start at or above it. The
/// vignette asks once a pixel, so the start is the power of two just above
/// the root, a few steps away, rather than the value itself.
///
/// @param value a non-negative square
/// @return the greatest integer whose square is at most value
int32_t isqrt64(int64_t value) noexcept {
    if (value <= 0)
        return 0;
    const uint64_t left = static_cast<uint64_t>(value);
    const auto bits = static_cast<unsigned>(std::bit_width(left));
    uint64_t root = uint64_t{1} << ((bits + 1U) / 2U);
    uint64_t next = (root + left / root) / 2U;
    while (next < root) {
        root = next;
        next = (root + left / root) / 2U;
    }
    return static_cast<int32_t>(root);
}

/// Grain for one cell. The same seed and cell give the same value.
///
/// @param seed the picture's seed
/// @param cell_x the cell's column
/// @param cell_y the cell's row
/// @return a value from 0 to 255
uint32_t grain_hash(uint32_t seed, int32_t cell_x, int32_t cell_y) noexcept {
    uint32_t mixed = seed + 0x9E3779B9u;
    mixed ^= static_cast<uint32_t>(cell_x) * 0x85EBCA6Bu;
    mixed ^= static_cast<uint32_t>(cell_y) * 0xC2B2AE35u;
    mixed = (mixed ^ (mixed >> 16)) * 0x7FEB352Du;
    mixed = (mixed ^ (mixed >> 15)) * 0x846CA68Bu;
    return mixed ^ (mixed >> 16);
}

/// How far a pixel is from the middle, 0 at the middle and about 1024 at
/// the corner. Roundness 0 is a square, 100 a circle.
///
/// @param column the pixel's column
/// @param row the pixel's row
/// @param width the picture's width
/// @param height the picture's height
/// @param roundness 0 to 100
/// @return the distance
int32_t vignette_distance(
    int32_t column, int32_t row, int32_t width, int32_t height, int32_t roundness
) noexcept {
    const int64_t across =
        (static_cast<int64_t>(column) * 2 + 1 - width) * 1024 / std::max(width, int32_t{1});
    const int64_t down =
        (static_cast<int64_t>(row) * 2 + 1 - height) * 1024 / std::max(height, int32_t{1});
    const int64_t abs_x = across < 0 ? -across : across;
    const int64_t abs_y = down < 0 ? -down : down;
    const int64_t box = abs_x > abs_y ? abs_x : abs_y;
    const int64_t circle =
        static_cast<int64_t>(isqrt64(abs_x * abs_x + abs_y * abs_y)) * 1024 / 1448;
    const int32_t mix = std::clamp(roundness, int32_t{0}, int32_t{100});
    return static_cast<int32_t>(box + (circle - box) * mix / 100);
}

/// How much of the pixel a vignette keeps, 256 keeping all of it. A full
/// vignette keeps about a twelfth of a corner.
///
/// @param distance vignette_distance
/// @param amount 0 to 100
/// @return the factor
int32_t vignette_keep(int32_t distance, int32_t amount) noexcept {
    int32_t dark = 0;
    if (distance >= 1024)
        dark = 256;
    else if (distance > 460) {
        const int32_t along = (distance - 460) * 256 / 564;
        const int32_t clamped = std::clamp(along, int32_t{0}, int32_t{256});
        dark = static_cast<int32_t>(
            static_cast<int64_t>(clamped) * clamped * (768 - 2 * clamped) / 65536
        );
    }
    const int32_t strength = std::clamp(amount, int32_t{0}, int32_t{100});
    const int32_t lost = static_cast<int32_t>(static_cast<int64_t>(dark) * strength * 236 / 25600);
    return 256 - lost;
}

/// The grade of one pixel.
///
/// @param grade the preset's grade
/// @param red the red sample
/// @param green the green sample
/// @param blue the blue sample
void grade_pixel(const Grade& grade, int& red, int& green, int& blue) noexcept {
    const int16_t* matrix = grade.matrix.data();
    const int mixed_r = (matrix[0] * red + matrix[1] * green + matrix[2] * blue) / 256;
    const int mixed_g = (matrix[3] * red + matrix[4] * green + matrix[5] * blue) / 256;
    const int mixed_b = (matrix[6] * red + matrix[7] * green + matrix[8] * blue) / 256;
    const int clamped_r = clamp_channel(mixed_r);
    const int clamped_g = clamp_channel(mixed_g);
    const int clamped_b = clamp_channel(mixed_b);
    const int luma = (77 * clamped_r + 150 * clamped_g + 29 * clamped_b) / 256;
    const int sat = grade.saturation;
    red = clamp_channel(luma + (clamped_r - luma) * sat / 256);
    green = clamp_channel(luma + (clamped_g - luma) * sat / 256);
    blue = clamp_channel(luma + (clamped_b - luma) * sat / 256);
    red = grade.curve[0][static_cast<std::size_t>(red)];
    green = grade.curve[1][static_cast<std::size_t>(green)];
    blue = grade.curve[2][static_cast<std::size_t>(blue)];
}

/// A frame's thicknesses, in pixels, for a picture of this size.
struct FrameMetrics {
    int32_t thin{1};
    int32_t mat{3};
    int32_t bar{8};
    int32_t pad{2};
};

/// Thicknesses that stay visible on a small picture and on a large one.
///
/// @param width the picture's width
/// @param height the picture's height
/// @return the thicknesses
FrameMetrics frame_metrics(int32_t width, int32_t height) noexcept {
    const int32_t short_side = std::min(width, height);
    FrameMetrics metrics;
    metrics.thin = std::max(int32_t{1}, short_side / 72);
    metrics.mat = std::max(metrics.thin + 2, short_side / 24);
    metrics.bar = std::max(int32_t{8}, short_side * 46 / 720);
    const int32_t half = std::max(height / 2, int32_t{1});
    if (metrics.bar > half)
        metrics.bar = half;
    metrics.pad = std::max(int32_t{2}, width * 20 / 720);
    return metrics;
}

/// Letters drawn for a caption bar, or none when the line was not drawn.
struct CaptionInk {
    const uint8_t* alpha{nullptr};
    int32_t width{0};
    int32_t height{0};
    int32_t origin{0};
    int32_t pen_x{0};
    int32_t pen_y{0};
};

/// Places a drawn line in the caption bar. A line taller than the bar is
/// centred on it and the paint clips what falls outside.
///
/// @param ink the ink to fill
/// @param drawn the coverage, kept until the bands return
/// @param pad the bar's left pad
/// @param picture_height the picture's height
/// @param bar the bar's height
void place_caption(
    CaptionInk& ink,
    const oa::platform::text_font::Coverage& drawn,
    int32_t pad,
    int32_t picture_height,
    int32_t bar
) noexcept {
    ink.alpha = drawn.alpha.data();
    ink.width = drawn.width;
    ink.height = drawn.height;
    ink.origin = drawn.origin;
    ink.pen_x = pad;
    ink.pen_y = picture_height - bar + (bar - drawn.height) / 2;
}

/// Draws the caption into the bar, shrinking until the line fits. A short
/// picture has a short bar, so the size may fall to one pixel. A line that
/// still does not fit is drawn at that size and clipped to the bar. A
/// missing stack, an empty line or a face that will not draw leaves the ink
/// empty and the bar is still painted.
///
/// @param fonts the faces
/// @param caption the line
/// @param picture_width the picture's width
/// @param picture_height the picture's height
/// @param metrics the frame's thicknesses
/// @param drawn where the coverage is kept until the bands return
/// @return the ink, or an empty ink
CaptionInk draw_caption(
    oa::platform::text_font::FontStack* fonts,
    const std::string& caption,
    int32_t picture_width,
    int32_t picture_height,
    const FrameMetrics& metrics,
    oa::platform::text_font::Coverage& drawn
) {
    CaptionInk ink;
    if (fonts == nullptr || caption.empty() || metrics.bar < 1)
        return ink;
    const int32_t room = std::max(picture_width - metrics.pad * 2, int32_t{1});
    oa::platform::text_font::Style style;
    style.family = oa::platform::text_font::Family::interface;
    style.weight = oa::platform::text_font::Weight::semibold;
    style.rendering = oa::platform::text_font::Rendering::antialiased;
    int32_t pixel_size =
        std::min(std::max(metrics.bar - 1, int32_t{1}), oa::platform::text_font::max_pixel_size);
    std::optional<oa::platform::text_font::Coverage> smallest;
    for (; pixel_size >= 1; --pixel_size) {
        style.pixel_size = pixel_size;
        std::optional<oa::platform::text_font::Coverage> coverage = fonts->draw(caption, style);
        if (!coverage.has_value() || coverage->width <= 0 || coverage->height <= 0)
            continue;
        const bool fits = coverage->advance <= room && coverage->height <= metrics.bar;
        if (fits) {
            drawn = std::move(*coverage);
            place_caption(ink, drawn, metrics.pad, picture_height, metrics.bar);
            return ink;
        }
        smallest = std::move(coverage);
    }
    if (!smallest.has_value())
        return ink;
    drawn = std::move(*smallest);
    place_caption(ink, drawn, metrics.pad, picture_height, metrics.bar);
    return ink;
}

/// What one band needs. Nothing here changes while the bands run.
struct BandWork {
    uint8_t* bytes{nullptr};
    int32_t width{0};
    int32_t height{0};
    int32_t stride{0};
    int32_t channels{0};
    const Grade* grade{nullptr};
    int32_t vignette{0};
    int32_t roundness{0};
    int32_t grain{0};
    int32_t grain_size{1};
    uint32_t seed{0};
    Frame frame{Frame::none};
    FrameMetrics metrics{};
    CaptionInk ink{};
};

/// Paints one pixel, colour only.
///
/// @param work the look
/// @param column the column
/// @param row the row
void paint_pixel(const BandWork& work, int32_t column, int32_t row) noexcept {
    uint8_t* pixel = work.bytes +
                     static_cast<std::size_t>(row) * static_cast<std::size_t>(work.stride) +
                     static_cast<std::size_t>(column) * static_cast<std::size_t>(work.channels);
    int red = pixel[0];
    int green = pixel[1];
    int blue = pixel[2];
    if (work.grade != nullptr)
        grade_pixel(*work.grade, red, green, blue);
    if (work.vignette > 0) {
        const int32_t keep = vignette_keep(
            vignette_distance(column, row, work.width, work.height, work.roundness), work.vignette
        );
        red = red * keep / 256;
        green = green * keep / 256;
        blue = blue * keep / 256;
    }
    if (work.grain > 0) {
        const int32_t cell = std::max(work.grain_size, int32_t{1});
        const uint32_t hashed = grain_hash(work.seed, column / cell, row / cell);
        const int delta = static_cast<int>(hashed & 255u) - 128;
        const int added = delta * work.grain / 200;
        red = clamp_channel(red + added);
        green = clamp_channel(green + added);
        blue = clamp_channel(blue + added);
    }
    const int32_t from_left = column;
    const int32_t from_right = work.width - 1 - column;
    const int32_t from_top = row;
    const int32_t from_bottom = work.height - 1 - row;
    const int32_t edge = std::min(std::min(from_left, from_right), std::min(from_top, from_bottom));
    if (work.frame == Frame::thin && edge < work.metrics.thin) {
        red = 242;
        green = 239;
        blue = 230;
    } else if (work.frame == Frame::mat) {
        if (edge < work.metrics.mat) {
            red = 236;
            green = 232;
            blue = 220;
        } else if (edge == work.metrics.mat) {
            red = red * 65 / 100;
            green = green * 65 / 100;
            blue = blue * 65 / 100;
        }
    } else if (work.frame == Frame::caption && row >= work.height - work.metrics.bar) {
        red = (red * 12 + 10 * 88) / 100;
        green = (green * 12 + 11 * 88) / 100;
        blue = (blue * 12 + 9 * 88) / 100;
        if (work.ink.alpha != nullptr) {
            const int32_t ink_x = column - work.ink.pen_x + work.ink.origin;
            const int32_t ink_y = row - work.ink.pen_y;
            if (ink_x >= 0 && ink_y >= 0 && ink_x < work.ink.width && ink_y < work.ink.height) {
                const int cover = work.ink.alpha
                                      [static_cast<std::size_t>(ink_y) *
                                           static_cast<std::size_t>(work.ink.width) +
                                       static_cast<std::size_t>(ink_x)];
                red = (236 * cover + red * (255 - cover)) / 255;
                green = (232 * cover + green * (255 - cover)) / 255;
                blue = (220 * cover + blue * (255 - cover)) / 255;
            }
        }
    }
    pixel[0] = static_cast<uint8_t>(red);
    pixel[1] = static_cast<uint8_t>(green);
    pixel[2] = static_cast<uint8_t>(blue);
}

/// Screen row, in map pixels, of a 256ths position. Toward −∞.
///
/// @param value the position, in 256ths
/// @return the map pixel
int32_t floor_div_256(int64_t value) noexcept {
    if (value >= 0) {
        const int64_t divided = value / 256;
        if (divided > std::numeric_limits<int32_t>::max())
            return std::numeric_limits<int32_t>::max();
        return static_cast<int32_t>(divided);
    }
    if (value == std::numeric_limits<int64_t>::min())
        return std::numeric_limits<int32_t>::min();
    const int64_t divided = -((-value + 255) / 256);
    if (divided < std::numeric_limits<int32_t>::min())
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(divided);
}

/// Classic screen row of one map row: `z - height / 2`, toward zero.
///
/// @param map_row the map row
/// @param height the ground height
/// @return the screen row
int32_t project_row(int32_t map_row, int32_t height) noexcept {
    return static_cast<int32_t>(static_cast<int64_t>(map_row) - static_cast<int64_t>(height) / 2);
}

/// Height on the span from one sample to the next.
///
/// @param h0 the far height
/// @param s0 the far screen row
/// @param h1 the near height
/// @param s1 the near screen row
/// @param screen the screen row
/// @return the height
int32_t blended_height(int32_t h0, int32_t s0, int32_t h1, int32_t s1, int32_t screen) noexcept {
    if (s0 == s1)
        return h0;
    const int64_t rise = static_cast<int64_t>(h1) - static_cast<int64_t>(h0);
    const int64_t along = static_cast<int64_t>(screen) - static_cast<int64_t>(s0);
    const int64_t span = static_cast<int64_t>(s1) - static_cast<int64_t>(s0);
    return static_cast<int32_t>(static_cast<int64_t>(h0) + rise * along / span);
}

/// Blur radius of one picture row. 0 inside the sharp band, then rising to
/// `radius` over four times that many rows.
///
/// @param row the picture row
/// @param height the picture's height
/// @param centre_percent the band's centre, 0 to 100
/// @param width the sharp band, in pixels
/// @param radius the blur at the top and the bottom
/// @return the radius of this row
int32_t row_radius(
    int32_t row, int32_t height, int32_t centre_percent, int32_t width, int32_t radius
) noexcept {
    if (radius <= 0 || height <= 0)
        return 0;
    const int32_t percent = std::clamp(centre_percent, int32_t{0}, int32_t{100});
    int32_t centre = static_cast<int32_t>((static_cast<int64_t>(height) * percent) / 100);
    if (centre >= height)
        centre = height - 1;
    if (centre < 0)
        centre = 0;
    const int32_t half = std::max(width, int32_t{0}) / 2;
    const int32_t distance = row >= centre ? row - centre : centre - row;
    if (distance <= half)
        return 0;
    const int32_t rise = radius > std::numeric_limits<int32_t>::max() / 4
                             ? std::numeric_limits<int32_t>::max()
                             : std::max(radius * 4, int32_t{1});
    const int32_t beyond = distance - half;
    if (beyond >= rise)
        return radius;
    const int64_t scaled = static_cast<int64_t>(beyond) * radius;
    const int32_t partial = static_cast<int32_t>((scaled + rise - 1) / rise);
    return std::min(partial, radius);
}

/// One channel of a box of `radius` on a row, edges held. The source is a
/// row of the picture, `step` bytes a pixel. The row written is packed RGB.
///
/// @param src the source row
/// @param step bytes a source pixel, 3 or 4; only the colour is read
/// @param dst the packed RGB row to write
/// @param width the row's pixels
/// @param radius samples on each side; 0 copies
/// @param prefix width + 1 sums per channel, scratch for this band
void box_row(
    const uint8_t* src, int32_t step, uint8_t* dst, int32_t width, int32_t radius, int32_t* prefix
) noexcept {
    if (width <= 0)
        return;
    const auto source = [src, step](int32_t column) {
        return src + static_cast<std::size_t>(column) * static_cast<std::size_t>(step);
    };
    if (radius <= 0) {
        for (int32_t column = 0; column < width; ++column) {
            const uint8_t* pixel = source(column);
            uint8_t* out = dst + static_cast<std::size_t>(column) * 3U;
            out[0] = pixel[0];
            out[1] = pixel[1];
            out[2] = pixel[2];
        }
        return;
    }
    for (int channel = 0; channel < 3; ++channel)
        prefix[channel] = 0;
    for (int32_t column = 0; column < width; ++column) {
        const uint8_t* pixel = source(column);
        int32_t* next = prefix + static_cast<std::size_t>(column + 1) * 3U;
        const int32_t* prev = prefix + static_cast<std::size_t>(column) * 3U;
        next[0] = prev[0] + pixel[0];
        next[1] = prev[1] + pixel[1];
        next[2] = prev[2] + pixel[2];
    }
    for (int32_t column = 0; column < width; ++column) {
        const int32_t left = std::max(column - radius, int32_t{0});
        const int32_t right = std::min(column + radius, width - 1);
        const int32_t count = right - left + 1;
        const int32_t* end = prefix + static_cast<std::size_t>(right + 1) * 3U;
        const int32_t* begin = prefix + static_cast<std::size_t>(left) * 3U;
        uint8_t* pixel = dst + static_cast<std::size_t>(column) * 3U;
        pixel[0] = static_cast<uint8_t>((end[0] - begin[0]) / count);
        pixel[1] = static_cast<uint8_t>((end[1] - begin[1]) / count);
        pixel[2] = static_cast<uint8_t>((end[2] - begin[2]) / count);
    }
}

/// Scratch a pass may keep at once, in bytes. Up to a 1080p picture every
/// band has its own buffer; a 4x photo's bands share a few.
constexpr uint64_t kScratchBudget = uint64_t{64} * 1024U * 1024U;

/// How many scratch buffers a pass keeps: as many as fit kScratchBudget, at
/// least two a thread, never more than its bands. The pass runs its bands
/// that many at a time, each band with a buffer no other band of that run
/// uses, so the scratch does not grow with the picture.
///
/// @param pool the pool; null is the caller alone
/// @param bands the pass's bands
/// @param band_bytes one band's scratch, in bytes
/// @return at least 1
uint32_t scratch_lanes(
    const oa::platform::job_pool::Pool* pool, uint32_t bands, uint64_t band_bytes
) noexcept {
    const uint64_t threads = pool != nullptr ? pool->threads() : 1U;
    const uint64_t fit = kScratchBudget / std::max(band_bytes, uint64_t{1});
    const uint64_t lanes = std::min<uint64_t>(bands, std::max(fit, threads * 2U));
    return static_cast<uint32_t>(std::max<uint64_t>(lanes, 1U));
}

/// Runs `bands` bands, `lanes` at a time. Each band is told its lane, whose
/// scratch is its own while it runs. The bands write disjoint pixels, so the
/// bytes do not depend on the lanes.
///
/// @param pool the pool; null runs on the caller
/// @param bands how many bands
/// @param lanes how many at a time, at least 1
/// @param band called with the band and its lane
template <typename Band>
void run_in_lanes(
    oa::platform::job_pool::Pool* pool, uint32_t bands, uint32_t lanes, const Band& band
) {
    for (uint32_t first = 0; first < bands; first += lanes) {
        const uint32_t count = std::min(lanes, bands - first);
        oa::platform::job_pool::run_bands(pool, count, [&band, first](uint32_t lane) {
            band(first + lane, lane);
        });
    }
}

/// Map x of a picture column, in map pixels.
///
/// @param origin_x_256 map x of column 0, in 256ths
/// @param step_256 map pixels per picture pixel, in 256ths
/// @param column the picture column
/// @return the map column
int32_t map_column_of(int32_t origin_x_256, int32_t step_256, int32_t column) noexcept {
    const int64_t at = static_cast<int64_t>(origin_x_256) + static_cast<int64_t>(column) * step_256;
    return floor_div_256(at);
}

/// Fades one pixel toward the haze colour. A height below 0 leaves it.
///
/// @param pixel the colour bytes
/// @param height the ground height
/// @param amount 0 to 100
/// @param red the haze colour
/// @param green the haze colour
/// @param blue the haze colour
void haze_pixel(
    uint8_t* pixel, int32_t height, int32_t amount, int red, int green, int blue
) noexcept {
    if (height < 0 || amount <= 0)
        return;
    const int held = std::clamp(height, int32_t{0}, int32_t{255});
    const int fade = amount * (255 - held) / 255;
    int mix = fade * 256 / 100;
    if (mix > 256)
        mix = 256;
    const int keep = 256 - mix;
    pixel[0] = static_cast<uint8_t>((pixel[0] * keep + red * mix) / 256);
    pixel[1] = static_cast<uint8_t>((pixel[1] * keep + green * mix) / 256);
    pixel[2] = static_cast<uint8_t>((pixel[2] * keep + blue * mix) / 256);
}

/// The map rows a haze reads: the first and how many.
struct MapRows {
    int32_t first{0}; ///< the first map row
    int32_t count{0}; ///< how many; 0 when none can show
};

/// The map rows that can show on the picture's screen rows. A height lifts
/// its row by half itself, so a row more than half the field's highest
/// height below the picture's last screen row cannot reach the picture, and
/// a row above its first screen row cannot either. The rows between are all
/// read, so the ground found is the ground the whole column gives.
///
/// @param field the ground
/// @param picture_rows the picture's rows
/// @return the rows to read
MapRows rows_in_reach(const HazeField& field, int32_t picture_rows) noexcept {
    const int32_t step = field.step_256 == 0 ? 256 : field.step_256;
    const int64_t first_256 = field.origin_row_256;
    const int64_t last_256 =
        first_256 + static_cast<int64_t>(std::max(picture_rows - 1, int32_t{0})) * step;
    const int64_t low = std::min(floor_div_256(first_256), floor_div_256(last_256));
    const int64_t high = std::max(floor_div_256(first_256), floor_div_256(last_256));
    const int64_t lift = static_cast<int64_t>(std::max(field.most_height, int32_t{0})) / 2 + 2;
    const int64_t rows = std::max(field.map_rows, int32_t{0});
    const int64_t from = std::clamp(low - 1, int64_t{0}, rows);
    const int64_t to = std::clamp(high + lift + 1, int64_t{0}, rows);
    MapRows reach;
    reach.first = static_cast<int32_t>(from);
    reach.count = static_cast<int32_t>(std::max(to - from, int64_t{0}));
    return reach;
}

/// The screen rows the picture's rows show: the first and how many. None
/// when the field marks no fog, or the span is past any picture's.
///
/// @param field the ground
/// @param picture_rows the picture's rows
/// @return the rows the fog is asked for
MapRows screen_rows_shown(const HazeField& field, int32_t picture_rows) noexcept {
    if (field.fog == nullptr || picture_rows <= 0)
        return {};
    const int32_t step = field.step_256 == 0 ? 256 : field.step_256;
    const int64_t first_256 = field.origin_row_256;
    const int64_t last_256 = first_256 + static_cast<int64_t>(picture_rows - 1) * step;
    const int64_t low = std::min(floor_div_256(first_256), floor_div_256(last_256));
    const int64_t high = std::max(floor_div_256(first_256), floor_div_256(last_256));
    const int64_t span = high - low + 1;
    if (span <= 0 || span > 4000000)
        return {};
    MapRows shown;
    shown.first = static_cast<int32_t>(low);
    shown.count = static_cast<int32_t>(span);
    return shown;
}

/// Hazes every row of one band of columns. A map column is sampled once and
/// reused while the next picture columns share it.
///
/// @param picture the picture
/// @param stride the stride
/// @param field the ground
/// @param reach the map rows that can show in the picture
/// @param shown the screen rows the picture shows, for the fog
/// @param first the first column
/// @param end one past the last column
/// @param raw one height per map row of `reach`, this lane's own buffer
/// @param screen one height per picture row, this lane's own buffer
/// @param black one mark per screen row of `shown`, this lane's own buffer
void haze_columns(
    Picture picture,
    int32_t stride,
    const HazeField& field,
    MapRows reach,
    MapRows shown,
    int32_t first,
    int32_t end,
    std::vector<int32_t>& raw,
    std::vector<int32_t>& screen,
    std::vector<uint8_t>& black
) noexcept {
    if (field.column == nullptr || field.map_rows <= 0)
        return;
    const int32_t step = field.step_256 == 0 ? 256 : field.step_256;
    int32_t held_column = std::numeric_limits<int32_t>::min();
    bool held = false;
    for (int32_t column = first; column < end; ++column) {
        const int32_t map_x = map_column_of(field.origin_x_256, step, column);
        if (!held || map_x != held_column) {
            held = true;
            held_column = map_x;
            if (map_x < 0 || map_x >= field.map_columns || reach.count <= 0) {
                std::fill(screen.begin(), screen.end(), -1);
            } else {
                std::fill(raw.begin(), raw.end(), -1);
                field.column(field.user, map_x, reach.first, raw.data(), reach.count);
                column_ground_heights(
                    raw.data(),
                    reach.count,
                    field.origin_row_256,
                    step,
                    picture.height,
                    screen.data(),
                    reach.first
                );
                // The fog is drawn where the map row is drawn, before any
                // height: the pixels it blacks out keep their colour.
                if (field.fog != nullptr && shown.count > 0 &&
                    black.size() >= static_cast<std::size_t>(shown.count)) {
                    std::fill(black.begin(), black.end(), uint8_t{0});
                    field.fog(field.user, map_x, shown.first, black.data(), shown.count);
                    for (int32_t row = 0; row < picture.height; ++row) {
                        const int64_t at = static_cast<int64_t>(field.origin_row_256) +
                                           static_cast<int64_t>(row) * step;
                        const int64_t index = static_cast<int64_t>(floor_div_256(at)) - shown.first;
                        if (index >= 0 && index < shown.count &&
                            black[static_cast<std::size_t>(index)] != 0)
                            screen[static_cast<std::size_t>(row)] = -1;
                    }
                }
            }
        }
        for (int32_t row = 0; row < picture.height; ++row) {
            uint8_t* pixel =
                picture.bytes + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) +
                static_cast<std::size_t>(column) * static_cast<std::size_t>(picture.channels);
            haze_pixel(
                pixel,
                screen[static_cast<std::size_t>(row)],
                field.amount,
                field.red,
                field.green,
                field.blue
            );
        }
    }
}

/// The separable blur. Row bands read the picture and write `blurred`.
/// Column bands then read `blurred` and write the picture. Neither reads a
/// buffer the other is writing.
///
/// @param picture the picture
/// @param stride the stride
/// @param look the look, with a radius above 0
/// @param blurred the horizontal pass, packed RGB
/// @param pool the pool; null runs on the caller
void blur_picture(
    Picture picture,
    int32_t stride,
    const Look& look,
    std::vector<uint8_t>& blurred,
    oa::platform::job_pool::Pool* pool
) {
    const int32_t width = picture.width;
    const int32_t height = picture.height;
    const uint32_t row_bands = oa::platform::job_pool::bands_of_rows(
        static_cast<uint32_t>(height), static_cast<uint32_t>(kBandRows)
    );
    const uint64_t row_bytes = (static_cast<uint64_t>(width) + 1U) * 3U * sizeof(int32_t);
    const uint32_t row_lanes = scratch_lanes(pool, row_bands, row_bytes);
    std::vector<std::vector<int32_t>> row_scratch(row_lanes);
    for (std::vector<int32_t>& scratch : row_scratch)
        scratch.assign(static_cast<std::size_t>(width + 1) * 3U, 0);
    run_in_lanes(pool, row_bands, row_lanes, [&](uint32_t band, uint32_t lane) {
        const int32_t first = static_cast<int32_t>(band) * kBandRows;
        const int32_t end = std::min(first + kBandRows, height);
        int32_t* prefix = row_scratch[lane].data();
        for (int32_t row = first; row < end; ++row) {
            const int32_t radius =
                row_radius(row, height, look.tilt_centre, look.tilt_width, look.tilt_radius);
            const uint8_t* from =
                picture.bytes + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride);
            uint8_t* into = blurred.data() +
                            static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 3U;
            box_row(from, picture.channels, into, width, radius, prefix);
        }
    });

    constexpr int32_t kColumnBand = 32;
    const uint32_t column_bands = oa::platform::job_pool::bands_of_rows(
        static_cast<uint32_t>(width), static_cast<uint32_t>(kColumnBand)
    );
    const uint64_t column_bytes = static_cast<uint64_t>(kColumnBand) *
                                  (static_cast<uint64_t>(height) + 1U) * 3U * sizeof(int32_t);
    const uint32_t column_lanes = scratch_lanes(pool, column_bands, column_bytes);
    std::vector<std::vector<int32_t>> column_scratch(column_lanes);
    for (std::vector<int32_t>& scratch : column_scratch)
        scratch.assign(
            static_cast<std::size_t>(kColumnBand) * static_cast<std::size_t>(height + 1) * 3U, 0
        );
    run_in_lanes(pool, column_bands, column_lanes, [&](uint32_t band, uint32_t lane) {
        const int32_t first = static_cast<int32_t>(band) * kColumnBand;
        const int32_t count = std::min(kColumnBand, width - first);
        int32_t* prefix = column_scratch[lane].data();
        const std::size_t column_stride = static_cast<std::size_t>(height + 1) * 3U;
        for (int32_t row = 0; row < height; ++row) {
            const uint8_t* from =
                blurred.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(first)) *
                                     3U;
            for (int32_t column = 0; column < count; ++column) {
                int32_t* next = prefix + static_cast<std::size_t>(column) * column_stride +
                                static_cast<std::size_t>(row + 1) * 3U;
                const int32_t* prev = prefix + static_cast<std::size_t>(column) * column_stride +
                                      static_cast<std::size_t>(row) * 3U;
                const uint8_t* pixel = from + static_cast<std::size_t>(column) * 3U;
                next[0] = prev[0] + pixel[0];
                next[1] = prev[1] + pixel[1];
                next[2] = prev[2] + pixel[2];
            }
        }
        for (int32_t row = 0; row < height; ++row) {
            const int32_t radius =
                row_radius(row, height, look.tilt_centre, look.tilt_width, look.tilt_radius);
            const int32_t top = std::max(row - radius, int32_t{0});
            const int32_t bottom = std::min(row + radius, height - 1);
            const int32_t window = bottom - top + 1;
            uint8_t* into =
                picture.bytes + static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) +
                static_cast<std::size_t>(first) * static_cast<std::size_t>(picture.channels);
            for (int32_t column = 0; column < count; ++column) {
                const int32_t* end = prefix + static_cast<std::size_t>(column) * column_stride +
                                     static_cast<std::size_t>(bottom + 1) * 3U;
                const int32_t* begin = prefix + static_cast<std::size_t>(column) * column_stride +
                                       static_cast<std::size_t>(top) * 3U;
                uint8_t* pixel = into + static_cast<std::size_t>(column) *
                                            static_cast<std::size_t>(picture.channels);
                pixel[0] = static_cast<uint8_t>((end[0] - begin[0]) / window);
                pixel[1] = static_cast<uint8_t>((end[1] - begin[1]) / window);
                pixel[2] = static_cast<uint8_t>((end[2] - begin[2]) / window);
            }
        }
    });
}

} // namespace

bool look_idle(const Look& look) noexcept {
    return look.preset == Preset::none && look.vignette <= 0 && look.grain <= 0 &&
           look.frame == Frame::none && look.tilt_radius <= 0 && look.haze <= 0;
}

const char* preset_key(Preset preset) noexcept {
    switch (preset) {
    case Preset::y1997:
        return "1997";
    case Preset::dusk:
        return "dusk";
    case Preset::arctic:
        return "arctic";
    case Preset::noir:
        return "noir";
    case Preset::bleach:
        return "bleach";
    case Preset::none:
    case Preset::count:
        break;
    }
    return "none";
}

const char* frame_key(Frame frame) noexcept {
    switch (frame) {
    case Frame::thin:
        return "thin";
    case Frame::mat:
        return "mat";
    case Frame::caption:
        return "caption";
    case Frame::none:
    case Frame::count:
        break;
    }
    return "none";
}

int32_t caption_bar_height(int32_t width, int32_t height) noexcept {
    return frame_metrics(width, height).bar;
}

int32_t photo_grain_size(int32_t grain_size, int32_t factor) noexcept {
    const int32_t size = std::max(grain_size, int32_t{1});
    const int32_t times = std::max(factor, int32_t{1});
    if (size > std::numeric_limits<int32_t>::max() / times)
        return std::numeric_limits<int32_t>::max();
    return size * times;
}

int32_t photo_tilt_width(int32_t width, int32_t factor) noexcept {
    if (width <= 0)
        return 0;
    const int32_t times = std::max(factor, int32_t{1});
    if (width > std::numeric_limits<int32_t>::max() / times)
        return std::numeric_limits<int32_t>::max();
    return width * times;
}

int32_t photo_tilt_radius(int32_t radius, int32_t factor) noexcept {
    if (radius <= 0)
        return 0;
    const int32_t times = std::max(factor, int32_t{1});
    if (radius > std::numeric_limits<int32_t>::max() / times)
        return std::numeric_limits<int32_t>::max();
    return radius * times;
}

void column_ground_heights(
    const int32_t* heights,
    int32_t map_rows,
    int32_t screen_first_256,
    int32_t screen_step_256,
    int32_t picture_rows,
    int32_t* out,
    int32_t first_map_row
) noexcept {
    if (out == nullptr || picture_rows <= 0)
        return;
    for (int32_t row = 0; row < picture_rows; ++row)
        out[row] = -1;
    if (heights == nullptr || map_rows <= 0 || screen_step_256 == 0)
        return;
    const int64_t last_256 = static_cast<int64_t>(screen_first_256) +
                             static_cast<int64_t>(picture_rows - 1) * screen_step_256;
    const int32_t screen_lo = std::min(floor_div_256(screen_first_256), floor_div_256(last_256));
    const int32_t screen_hi = std::max(floor_div_256(screen_first_256), floor_div_256(last_256));
    const int64_t span = static_cast<int64_t>(screen_hi) - static_cast<int64_t>(screen_lo) + 1;
    if (span <= 0 || span > 4000000)
        return;
    std::vector<int32_t> table;
    try {
        table.assign(static_cast<std::size_t>(span), -1);
    } catch (const std::bad_alloc&) {
        return;
    }
    for (int32_t map_row = 0; map_row < map_rows; ++map_row) {
        const int32_t far_height = heights[map_row];
        const int32_t far_screen = project_row(first_map_row + map_row, far_height);
        int32_t near_height = far_height;
        int32_t near_screen = far_screen;
        if (map_row + 1 < map_rows) {
            near_height = heights[map_row + 1];
            near_screen = project_row(first_map_row + map_row + 1, near_height);
        }
        const int32_t left = std::min(far_screen, near_screen);
        const int32_t right = std::max(far_screen, near_screen);
        const int32_t from = std::max(left, screen_lo);
        const int32_t to = std::min(right, screen_hi);
        for (int32_t screen = from; screen <= to; ++screen) {
            table[static_cast<std::size_t>(screen - screen_lo)] =
                blended_height(far_height, far_screen, near_height, near_screen, screen);
        }
    }
    for (int32_t row = 0; row < picture_rows; ++row) {
        const int64_t at =
            static_cast<int64_t>(screen_first_256) + static_cast<int64_t>(row) * screen_step_256;
        const int32_t screen = floor_div_256(at);
        if (screen < screen_lo || screen > screen_hi)
            continue;
        out[row] = table[static_cast<std::size_t>(screen - screen_lo)];
    }
}

void apply_look(
    Picture picture,
    const Look& look,
    oa::platform::text_font::FontStack* fonts,
    oa::platform::job_pool::Pool* pool,
    const HazeField* haze
) {
    if (look_idle(look))
        return;
    if (picture.bytes == nullptr || picture.width <= 0 || picture.height <= 0)
        return;
    if (picture.channels != 3 && picture.channels != 4)
        return;
    const int32_t stride = picture.stride > 0 ? picture.stride : picture.width * picture.channels;
    if (stride < picture.width * picture.channels)
        return;

    BandWork work;
    work.bytes = picture.bytes;
    work.width = picture.width;
    work.height = picture.height;
    work.stride = stride;
    work.channels = picture.channels;
    if (look.preset != Preset::none && look.preset < Preset::count)
        work.grade = &grades[static_cast<std::size_t>(look.preset)];
    work.vignette = std::clamp(look.vignette, int32_t{0}, int32_t{100});
    work.roundness = look.roundness;
    work.grain = std::clamp(look.grain, int32_t{0}, int32_t{100});
    work.grain_size = std::max(look.grain_size, int32_t{1});
    work.seed = look.seed;
    work.frame = look.frame < Frame::count ? look.frame : Frame::none;
    work.metrics = frame_metrics(picture.width, picture.height);
    oa::platform::text_font::Coverage drawn;
    if (work.frame == Frame::caption)
        work.ink =
            draw_caption(fonts, look.caption, picture.width, picture.height, work.metrics, drawn);

    const int32_t haze_amount = std::clamp(look.haze, int32_t{0}, int32_t{100});
    const bool hazing = haze_amount > 0 && haze != nullptr && haze->column != nullptr &&
                        haze->map_rows > 0 && haze->map_columns > 0;
    if (hazing) {
        // Bands of columns: each map column is inverted once, over the map
        // rows that can reach the picture, for every row of the picture.
        HazeField field = *haze;
        field.amount = haze_amount;
        const MapRows reach = rows_in_reach(field, picture.height);
        const MapRows shown = screen_rows_shown(field, picture.height);
        constexpr int32_t kHazeColumns = 32;
        const uint32_t haze_bands = oa::platform::job_pool::bands_of_rows(
            static_cast<uint32_t>(picture.width), static_cast<uint32_t>(kHazeColumns)
        );
        const uint64_t lane_bytes =
            (static_cast<uint64_t>(reach.count) + static_cast<uint64_t>(picture.height)) *
                sizeof(int32_t) +
            static_cast<uint64_t>(shown.count);
        const uint32_t lanes = scratch_lanes(pool, haze_bands, lane_bytes);
        std::vector<std::vector<int32_t>> raw(lanes);
        std::vector<std::vector<int32_t>> screen(lanes);
        std::vector<std::vector<uint8_t>> black(lanes);
        for (uint32_t lane = 0; lane < lanes; ++lane) {
            raw[lane].assign(static_cast<std::size_t>(reach.count), -1);
            screen[lane].assign(static_cast<std::size_t>(picture.height), -1);
            black[lane].assign(static_cast<std::size_t>(shown.count), 0);
        }
        run_in_lanes(pool, haze_bands, lanes, [&](uint32_t band, uint32_t lane) {
            const int32_t first = static_cast<int32_t>(band) * kHazeColumns;
            const int32_t end = std::min(first + kHazeColumns, picture.width);
            haze_columns(
                picture,
                stride,
                field,
                reach,
                shown,
                first,
                end,
                raw[lane],
                screen[lane],
                black[lane]
            );
        });
    }
    if (look.tilt_radius > 0) {
        const std::size_t pixels =
            static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height);
        std::vector<uint8_t> blurred(pixels * 3U);
        blur_picture(picture, stride, look, blurred, pool);
    }

    const uint32_t bands = oa::platform::job_pool::bands_of_rows(
        static_cast<uint32_t>(picture.height), static_cast<uint32_t>(kBandRows)
    );
    oa::platform::job_pool::run_bands(pool, bands, [&work](uint32_t band) {
        const int32_t first = static_cast<int32_t>(band) * kBandRows;
        const int32_t end = std::min(first + kBandRows, work.height);
        for (int32_t row = first; row < end; ++row) {
            for (int32_t column = 0; column < work.width; ++column)
                paint_pixel(work, column, row);
        }
    });
}

} // namespace oa::present::photo
