// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A photo's look: a colour grade, a vignette, grain, a frame, a tilt-shift
// and haze by ground height. Pure over the pixels it is given. It opens no
// file and reads no match.
#pragma once

#include <cstdint>
#include <string>

namespace oa::platform::job_pool {
class Pool;
}

namespace oa::platform::text_font {
class FontStack;
}

namespace oa::present::photo {

/// A colour grade. None leaves the colour alone.
enum class Preset : uint8_t {
    none = 0,
    y1997,
    dusk,
    arctic,
    noir,
    bleach,
    count,
};

/// A frame drawn over the graded picture. None draws nothing.
enum class Frame : uint8_t {
    none = 0,
    thin,
    mat,
    caption,
    count,
};

/// One look. Amounts are 0 to 100. Grain size is pixels of one grain, at
/// least 1. The seed makes the grain again: the same seed and size write
/// the same grain. The caption is the line a caption bar draws, already
/// joined by the caller.
///
/// Tilt-shift is a sharp band with the blur rising above and below it, by
/// screen row. It is not a depth of field. The centre is a percent of the
/// picture's height. The width and the radius are pixels of this picture:
/// a caller that draws a larger photo scales them first. A radius of 0
/// leaves the picture sharp.
///
/// Haze fades lower ground toward a fixed pale colour. The amount is 0 to
/// 100. The heights come from the field passed to apply_look. An amount of
/// 0 leaves the picture clear.
struct Look {
    Preset preset{Preset::none};
    int32_t vignette{0};
    int32_t roundness{50};
    int32_t grain{0};
    int32_t grain_size{2};
    uint32_t seed{0};
    Frame frame{Frame::none};
    std::string caption{};
    /// Centre of the sharp band, 0 to 100 percent of the picture's height.
    int32_t tilt_centre{50};
    /// Width of the sharp band, in this picture's pixels.
    int32_t tilt_width{0};
    /// Blur at the picture's top and bottom, in this picture's pixels.
    /// 0 leaves every row sharp.
    int32_t tilt_radius{0};
    /// How far lower ground fades toward the haze colour, 0 to 100.
    int32_t haze{0};
};

/// Ground heights for one picture, so haze can follow the ground under each
/// pixel. The caller fills one map column at a time. Present does not keep
/// a map.
///
/// The callback may run on several threads at once. It only reads, it does
/// not throw, and it writes `count` heights, one for each map row from
/// `first_row`. A height below 0 is no ground, and that pixel stays clear.
/// Only the map rows that can reach the picture are asked for: a height
/// lifts its row by half itself, so heights above `most_height` would be
/// misread.
struct HazeField {
    int32_t amount{0};  ///< 0 to 100; 0 hazes nothing
    uint8_t red{176};   ///< the pale air
    uint8_t green{196}; ///< the pale air
    uint8_t blue{214};  ///< the pale air
    /// Writes the heights of map rows `first_row` onward of column `map_x`.
    void (*column)(void* user, int32_t map_x, int32_t first_row, int32_t* heights, int32_t count){
        nullptr
    };
    /// Marks the screen rows of column `map_x`, from `first_row`, that the
    /// fog of war draws black: 1 there, 0 elsewhere. A screen row is the map
    /// row the classic view draws there before any height, which is where
    /// the fog is drawn, not the ground under it. The haze leaves those
    /// pixels as they are and finds the ground for the rest as though there
    /// were no fog. Null marks none. The same rules as `column` hold.
    void (*fog)(void* user, int32_t map_x, int32_t first_row, uint8_t* black, int32_t count){
        nullptr
    };
    int32_t most_height{255};  ///< the highest height `column` writes; 255 for the map's grid
    void* user{nullptr};       ///< passed to column
    int32_t map_columns{0};    ///< map width, in pixels
    int32_t map_rows{0};       ///< map height, in pixels
    int32_t origin_x_256{0};   ///< map x of picture pixel (0, 0), in 256ths
    int32_t origin_row_256{0}; ///< screen row of picture pixel (0, 0), in 256ths
    int32_t step_256{256};     ///< map pixels per picture pixel, in 256ths
};

/// Packed pixels, row by row. Three channels are RGB. Four are RGBA, and
/// the look writes only the colour: the fourth byte of each pixel stays.
/// A stride of 0 is width times channels. Bytes past each row are left alone.
struct Picture {
    uint8_t* bytes{nullptr};
    int32_t width{0};
    int32_t height{0};
    int32_t stride{0};
    int32_t channels{3};
};

/// Tells whether the look would change a picture.
///
/// None, a vignette of 0, a grain of 0, no frame, a tilt radius of 0 and a
/// haze of 0 change nothing. Roundness and grain size do nothing while their
/// amounts are 0. The band's centre and width do nothing while the radius is 0.
///
/// @param look the look
/// @return true when applying it leaves every pixel as it was
[[nodiscard]] bool look_idle(const Look& look) noexcept;

/// The preset's name in a photo's record.
///
/// @param preset the preset
/// @return none, 1997, dusk, arctic, noir or bleach
[[nodiscard]] const char* preset_key(Preset preset) noexcept;

/// The frame's name in a photo's record.
///
/// @param frame the frame
/// @return none, thin, mat or caption
[[nodiscard]] const char* frame_key(Frame frame) noexcept;

/// The grain size a photo drawn `factor` times the battlefield uses.
///
/// Grain size is in pixels of the picture the look is painted on. The
/// preview paints it on the battlefield as the window shows it, so a photo
/// two to four times that size draws each grain that many times larger:
/// it then covers the same part of the map, and the photo carries the grain
/// the preview shows. A frame already scales with the picture's short side.
///
/// @param grain_size the panel's grain size, in the battlefield's pixels;
///        below 1 counts as 1
/// @param factor the photo's size factor; below 1 counts as 1
/// @return the grain size in the photo's pixels, at least 1
[[nodiscard]] int32_t photo_grain_size(int32_t grain_size, int32_t factor) noexcept;

/// The sharp band's width in a photo drawn `factor` times the battlefield.
///
/// The panel's width is in the battlefield's pixels. A photo two to four
/// times that size draws the band that many times wider, so it covers the
/// same part of the picture the preview shows. 0 stays 0: there is no band
/// to widen.
///
/// @param width the panel's band width, in the battlefield's pixels
/// @param factor the photo's size factor; below 1 counts as 1
/// @return the band's width in the photo's pixels; 0 when `width` is below 1
[[nodiscard]] int32_t photo_tilt_width(int32_t width, int32_t factor) noexcept;

/// The blur radius in a photo drawn `factor` times the battlefield.
///
/// The panel's radius is in the battlefield's pixels. A larger photo draws
/// the blur that many times wider. 0 stays 0, and the picture stays sharp.
///
/// @param radius the panel's blur, in the battlefield's pixels
/// @param factor the photo's size factor; below 1 counts as 1
/// @return the blur in the photo's pixels; 0 when `radius` is below 1
[[nodiscard]] int32_t photo_tilt_radius(int32_t radius, int32_t factor) noexcept;

/// Ground height under each picture row of one map column.
///
/// The classic view puts map row `z` at screen row `z - height / 2`, before
/// the camera and the zoom. Samples are written from the far end of the
/// column toward the near end, so a nearer slope covers a farther one, and
/// the rows between two samples are filled so a ramp leaves no hole. A
/// picture row no sample reaches is -1.
///
/// @param heights one height per map row from `first_map_row`; null writes -1
/// @param map_rows how many heights
/// @param screen_first_256 screen row of picture row 0, in 256ths of a map pixel
/// @param screen_step_256 screen rows per picture row, in 256ths; 0 writes -1
/// @param picture_rows how many rows to write
/// @param out one height per picture row; a row with no ground is -1
/// @param first_map_row the map row `heights[0]` belongs to
void column_ground_heights(
    const int32_t* heights,
    int32_t map_rows,
    int32_t screen_first_256,
    int32_t screen_step_256,
    int32_t picture_rows,
    int32_t* out,
    int32_t first_map_row = 0
) noexcept;

/// The caption bar's height, in pixels, for a picture of this size.
///
/// The same height the frame paints, so a key line can sit above the bar.
///
/// @param width the picture's width
/// @param height the picture's height
/// @return the bar's rows; at least 8, and at most half the picture
[[nodiscard]] int32_t caption_bar_height(int32_t width, int32_t height) noexcept;

/// Paints the look into the picture, in bands of 32 rows.
///
/// Haze runs first, then the tilt-shift, then the grade, the vignette and
/// the grain, then the frame. The blur reads an unchanged copy of the hazed
/// picture, so grain stays sharp and a border is not blurred. An idle look
/// returns without reading or writing a pixel and without running a band.
/// The same picture, look, field and seed give the same bytes however many
/// threads the pool has, and with no pool at all. A caption is drawn on the
/// calling thread, before the bands, with the interface face when `fonts`
/// is set. A null stack still paints the caption bar, with no letters.
///
/// A null field hazes nothing. Haze follows the ground the field reports.
/// It does not follow units.
///
/// @param picture the pixels; nothing happens when they are missing or the
///        size is empty, or the channels are not 3 or 4
/// @param look the look
/// @param fonts the faces a caption bar draws with; null draws the bar only.
///        Drawing a line changes the stack, so it is not const. One thread
///        at a time uses a stack.
/// @param pool the bands' pool; null runs the bands in order on the caller
/// @param haze the ground under the picture; null hazes nothing
void apply_look(
    Picture picture,
    const Look& look,
    oa::platform::text_font::FontStack* fonts,
    oa::platform::job_pool::Pool* pool,
    const HazeField* haze = nullptr
);

} // namespace oa::present::photo
