// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The grades, the vignette, the grain and the frames. Every number is an
// integer, so a picture comes out the same on every machine. The bands
// write only their own rows and share nothing that changes.
#include "oa/present/photo/looks.hpp"

#include "oa/platform/job_pool.hpp"
#include "oa/platform/text_font.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>

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

} // namespace

bool look_idle(const Look& look) noexcept {
    return look.preset == Preset::none && look.vignette <= 0 && look.grain <= 0 &&
           look.frame == Frame::none;
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

void apply_look(
    Picture picture,
    const Look& look,
    oa::platform::text_font::FontStack* fonts,
    oa::platform::job_pool::Pool* pool
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
