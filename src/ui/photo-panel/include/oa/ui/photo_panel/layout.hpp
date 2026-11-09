// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where the photo panel, its chips, its key line and its toast lie.
#pragma once

#include "oa/ui/kit/text.hpp"
#include "oa/ui/photo_panel/model.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace oa::ui::photo_panel {

/// What the panel draws, and the bands a photo comparison skips.
struct Layout {
    kit::Rect panel{};                   ///< the panel; empty while it is hidden
    kit::Rect chips{};                   ///< the chips at the top left, as one band
    std::array<kit::Rect, 3> chip_box{}; ///< each chip; empty when that chip is not drawn
    kit::Rect alerts{};                  ///< the lines under the chips
    kit::Rect keyline{};                 ///< the chords along the bottom
    kit::Rect hint{};                    ///< "{key} shows the panel"; empty while the panel shows
    kit::Rect toast{};                   ///< the notice after a photo; empty while there is none
    kit::DisplayList list{};             ///< the controls, in Tab order
};

/// The panel's width at a size class, kept inside the window.
///
/// @param size_class the class
/// @param window_width the window's width, in points
/// @return the width, in points
[[nodiscard]] int32_t panel_width(kit::SizeClass size_class, int32_t window_width) noexcept;

/// The zoom stops between `least` and `most`, on the wheel's scale.
///
/// Each step is 1.15 times the last, the same factor photo.zoom-in uses,
/// and 1.0 is a stop whenever it lies between the limits. The two limits
/// are stops too. Stops closer than a thousandth of each other collapse.
///
/// @param least the farthest zoom the tier draws
/// @param most the nearest zoom the tier draws
/// @return the stops, ascending; one stop when the limits are not usable
[[nodiscard]] std::vector<float> zoom_stops(float least, float most);

/// The stop nearest `zoom`, measured in log space.
///
/// @param stops the stops zoom_stops returned
/// @param zoom the zoom the picture is at
/// @return the stop's index; 0 when `stops` is empty or `zoom` is not positive
[[nodiscard]] int32_t zoom_stop_nearest(const std::vector<float>& stops, float zoom) noexcept;

/// The scale's name for a zoom. Whole map and Far are below 1.
///
/// @param zoom the zoom the slider is showing
/// @return the English key: Whole map, Far, Medium, Near or Closest
[[nodiscard]] std::string_view zoom_scale_key(float zoom) noexcept;

/// Lays the panel out for `model`.
///
/// Every control lies inside the panel, and the panel inside the window,
/// except the hint, which replaces the panel, and photo.saved, the notice
/// after a photo. Both lie inside the window. A folded section's controls
/// are not in the list. A lock's reason is the control's text and its own
/// line. The zoom track is at least 64 points while the row allows it.
///
/// @param model the panel
/// @param fonts the fonts the chips are measured in
/// @return the layout
[[nodiscard]] Layout layout_of(const Model& model, const kit::Fonts& fonts);

/// Tells whether a window pixel can hold the panel's chrome.
///
/// The bands cover the chips, the key line, the panel's column and the
/// toast, whichever of them the visit is drawing. A photo comparison skips
/// them. Guides are not chrome: they are drawn on this layer and are not
/// in a photo.
///
/// @param x the column, in window points
/// @param y the row, in window points
/// @param width the window's width
/// @param height the window's height
/// @return true when the chrome can cover that pixel
[[nodiscard]] bool covers_chrome(int x, int y, int width, int height) noexcept;

} // namespace oa::ui::photo_panel
