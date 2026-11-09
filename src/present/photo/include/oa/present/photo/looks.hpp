// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A photo's look: a colour grade, a vignette, grain and a frame. Pure over
// the pixels it is given. It opens no file and reads no match.
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
struct Look {
    Preset preset{Preset::none};
    int32_t vignette{0};
    int32_t roundness{50};
    int32_t grain{0};
    int32_t grain_size{2};
    uint32_t seed{0};
    Frame frame{Frame::none};
    std::string caption{};
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
/// None, a vignette of 0, a grain of 0 and no frame change nothing. Roundness
/// and grain size do nothing while their amounts are 0.
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
/// The grade, the vignette and the grain run first, then the frame, so a
/// border is not graded and carries no grain. An idle look returns without
/// reading or writing a pixel and without running a band. The same picture,
/// look and seed give the same bytes however many threads the pool has,
/// and with no pool at all. A caption is drawn on the calling thread, before
/// the bands, with the interface face when `fonts` is set. A null stack
/// still paints the caption bar, with no letters.
///
/// @param picture the pixels; nothing happens when they are missing or the
///        size is empty, or the channels are not 3 or 4
/// @param look the look
/// @param fonts the faces a caption bar draws with; null draws the bar only.
///        Drawing a line changes the stack, so it is not const. One thread
///        at a time uses a stack.
/// @param pool the bands' pool; null runs the bands in order on the caller
void apply_look(
    Picture picture,
    const Look& look,
    oa::platform::text_font::FontStack* fonts,
    oa::platform::job_pool::Pool* pool
);

} // namespace oa::present::photo
