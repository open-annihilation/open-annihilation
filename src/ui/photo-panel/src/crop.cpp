// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Guides and the centred crop a chosen shape applies to a photo.

#include "oa/ui/photo_panel/crop.hpp"

#include "oa/ui/photo_panel/words.hpp"

#include <cstring>
#include <vector>

namespace oa::ui::photo_panel {

Guide next_guide(Guide guide) noexcept {
    const auto index = static_cast<uint8_t>(guide);
    const auto last = static_cast<uint8_t>(Guide::count);
    if (index >= last)
        return Guide::off;
    return static_cast<Guide>(static_cast<uint8_t>(index + 1U) % last);
}

bool crop_ratio(Guide guide, int& width_parts, int& height_parts) noexcept {
    width_parts = 0;
    height_parts = 0;
    switch (guide) {
    case Guide::crop_16_9:
        width_parts = 16;
        height_parts = 9;
        return true;
    case Guide::crop_3_2:
        width_parts = 3;
        height_parts = 2;
        return true;
    case Guide::crop_4_3:
        width_parts = 4;
        height_parts = 3;
        return true;
    case Guide::crop_1_1:
        width_parts = 1;
        height_parts = 1;
        return true;
    case Guide::crop_4_5:
        width_parts = 4;
        height_parts = 5;
        return true;
    case Guide::crop_9_16:
        width_parts = 9;
        height_parts = 16;
        return true;
    case Guide::crop_21_9:
        width_parts = 21;
        height_parts = 9;
        return true;
    case Guide::off:
    case Guide::thirds:
    case Guide::centre:
    case Guide::safe:
    case Guide::count:
        break;
    }
    return false;
}

namespace {

/// The centred crop's origin and size, in pixels.
///
/// @param width the picture's width
/// @param height the picture's height
/// @param width_parts the shape's width
/// @param height_parts the shape's height
/// @param[out] crop_x the left column
/// @param[out] crop_y the top row
/// @param[out] crop_w the cropped width
/// @param[out] crop_h the cropped height
void crop_span(
    uint32_t width,
    uint32_t height,
    int width_parts,
    int height_parts,
    uint32_t& crop_x,
    uint32_t& crop_y,
    uint32_t& crop_w,
    uint32_t& crop_h
) noexcept {
    crop_x = 0;
    crop_y = 0;
    crop_w = width;
    crop_h = height;
    if (width == 0 || height == 0 || width_parts <= 0 || height_parts <= 0)
        return;
    const uint64_t wide = static_cast<uint64_t>(width) * static_cast<uint64_t>(height_parts);
    const uint64_t tall = static_cast<uint64_t>(height) * static_cast<uint64_t>(width_parts);
    if (wide > tall) {
        crop_h = height;
        crop_w = static_cast<uint32_t>(tall / static_cast<uint64_t>(height_parts));
    } else {
        crop_w = width;
        crop_h = static_cast<uint32_t>(wide / static_cast<uint64_t>(width_parts));
    }
    if (crop_w == 0 || crop_h == 0 || crop_w > width || crop_h > height) {
        crop_w = width;
        crop_h = height;
        return;
    }
    crop_x = (width - crop_w) / 2U;
    crop_y = (height - crop_h) / 2U;
}

} // namespace

kit::Rect crop_bounds(int32_t width, int32_t height, Guide guide) noexcept {
    int width_parts = 0;
    int height_parts = 0;
    if (width <= 0 || height <= 0 || !crop_ratio(guide, width_parts, height_parts))
        return {};
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = 0;
    uint32_t crop_h = 0;
    crop_span(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        width_parts,
        height_parts,
        crop_x,
        crop_y,
        crop_w,
        crop_h
    );
    return {
        static_cast<int32_t>(crop_x),
        static_cast<int32_t>(crop_y),
        static_cast<int32_t>(crop_w),
        static_cast<int32_t>(crop_h),
    };
}

void crop_centred(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height, Guide guide) {
    int width_parts = 0;
    int height_parts = 0;
    if (!crop_ratio(guide, width_parts, height_parts))
        return;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || rgb.size() != pixels * 3U)
        return;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_w = 0;
    uint32_t crop_h = 0;
    crop_span(width, height, width_parts, height_parts, crop_x, crop_y, crop_w, crop_h);
    if (crop_w == width && crop_h == height)
        return;
    std::vector<uint8_t> cropped(static_cast<std::size_t>(crop_w) * crop_h * 3U);
    for (uint32_t row = 0; row < crop_h; ++row) {
        const uint8_t* from =
            rgb.data() + (static_cast<std::size_t>(crop_y + row) * width + crop_x) * 3U;
        uint8_t* to = cropped.data() + static_cast<std::size_t>(row) * crop_w * 3U;
        std::memcpy(to, from, static_cast<std::size_t>(crop_w) * 3U);
    }
    rgb = std::move(cropped);
    width = crop_w;
    height = crop_h;
}

std::string_view guide_name(Guide guide) noexcept {
    switch (guide) {
    case Guide::thirds:
        return words::thirds;
    case Guide::centre:
        return words::centre_lines;
    case Guide::safe:
        return words::safe_area;
    case Guide::crop_16_9:
        return "16:9";
    case Guide::crop_3_2:
        return "3:2";
    case Guide::crop_4_3:
        return "4:3";
    case Guide::crop_1_1:
        return "1:1";
    case Guide::crop_4_5:
        return "4:5";
    case Guide::crop_9_16:
        return "9:16";
    case Guide::crop_21_9:
        return "21:9";
    case Guide::off:
    case Guide::count:
        break;
    }
    return words::off;
}

} // namespace oa::ui::photo_panel
