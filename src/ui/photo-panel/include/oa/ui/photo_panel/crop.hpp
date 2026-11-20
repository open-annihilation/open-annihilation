// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Guides and the centred crop a chosen shape applies to a photo.
#pragma once

#include "oa/ui/kit/layout.hpp"
#include "oa/ui/photo_panel/model.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace oa::ui::photo_panel {

/// The guide after `guide`, wrapping from the last crop back to off.
///
/// @param guide the guide showing now
/// @return the next one
[[nodiscard]] Guide next_guide(Guide guide) noexcept;

/// The width and height parts of a crop guide.
///
/// Thirds, centre, safe area and off are not crops.
///
/// @param guide the guide
/// @param[out] width_parts the shape's width
/// @param[out] height_parts the shape's height
/// @return true when the guide crops
[[nodiscard]] bool crop_ratio(Guide guide, int& width_parts, int& height_parts) noexcept;

/// The centred rectangle of a crop, in a picture of `width` by `height`.
///
/// Empty when the guide is not a crop, or the picture has no area.
///
/// @param width the picture's width, in points
/// @param height the picture's height, in points
/// @param guide the guide
/// @return the crop, centred
[[nodiscard]] kit::Rect crop_bounds(int32_t width, int32_t height, Guide guide) noexcept;

/// Crops packed RGB to a guide's shape, centred.
///
/// The written size is the cropped size. A guide that is not a crop, or a
/// picture already of that shape, leaves the rows as they are.
///
/// @param[in,out] rgb packed RGB, three bytes a pixel
/// @param[in,out] width the picture's width
/// @param[in,out] height the picture's height
/// @param guide the guide
void crop_centred(std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height, Guide guide);

/// The English name of a guide, for a control's value.
///
/// @param guide the guide
/// @return the catalogue key; Off when the guide is off
[[nodiscard]] std::string_view guide_name(Guide guide) noexcept;

} // namespace oa::ui::photo_panel
