// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where the photo panel, its chips, its key line and its toast lie.
#pragma once

#include "oa/ui/kit/text.hpp"
#include "oa/ui/photo_panel/model.hpp"

#include <array>
#include <cstdint>
#include <string>
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
    int32_t scroll{0};                   ///< the scroll a touch layout used, after clamping
    /// The rows between Leave and Take photo. Empty on the keyboard layout,
    /// and whenever the touch layout has no view to scroll.
    kit::ScrollArea rows{};
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

/// One size or smooth stop, as the plan judged it.
struct ChoiceStop {
    int factor{1};      ///< 1, 2, 3 or 4
    std::string lock{}; ///< empty when the player can choose it
    int32_t width{0};   ///< the picture, for a size
    int32_t height{0};  ///< the picture, for a size
};

/// The words a size row and a smooth row are built from, already looked up.
struct ChoiceWords {
    std::string_view screen{"Screen"};
    std::string_view off{"Off"};
    std::string_view two{"2×"};
    std::string_view three{"3×"};
    std::string_view four{"4×"};
    std::string_view size{"Size"};
    std::string_view smooth{"Smooth edges"};
    std::string_view and_word{"and"};
    /// What this look writes for a multiplication sign. Classic passes "x".
    std::string_view times{"×"};
};

/// The captions, the one pixel value and the one grouped reason.
struct ChoiceCopy {
    std::array<std::string, 4> size_captions{};
    std::array<std::string, 3> smooth_captions{};
    std::string size_label{};
    std::string size_value{};
    std::string smooth_label{};
    std::string reason{};
};

/// Captions the way the panel lists them. Stop captions are the factor
/// words. The chosen size's pixels are one value. Locks that share a reason
/// are named once. When every stop shares one lock, the reason is that
/// sentence alone.
///
/// @param sizes Screen, 2×, 3× and 4×
/// @param smooth Off, 2× and 4×
/// @param size_index the lit size, 0 through 3
/// @param smooth_index the lit smooth step, 0 through 2
/// @param words the looked-up words, and the look's multiplication sign
/// @return the copy the rows draw
[[nodiscard]] ChoiceCopy choice_copy(
    const std::array<ChoiceStop, 4>& sizes,
    const std::array<ChoiceStop, 3>& smooth,
    int size_index,
    int smooth_index,
    const ChoiceWords& words
);

/// Lays the panel out for `model`.
///
/// Every control's visible part lies inside the panel, and the panel inside
/// the window, except the hint, which replaces the panel, and photo.saved,
/// the notice after a photo. Both lie inside the window. A scrolling row may
/// extend past the view; the part that shows is the row clipped to `rows.view`.
/// A folded section's controls are not in the list. A lock's reason is the
/// control's text and its own line. The zoom track is at least 64 points
/// while the row allows it.
///
/// With `model.touch`, every control a finger presses is at least
/// `model.touch_row` tall (and at least 44). One section is open. Leave and
/// Take photo stay on screen. Every row between them stays in the list, in
/// one scroll area (`rows`): a row the view cuts is drawn and hit in the part
/// that shows, and `model.reveal` scrolls the focused row into the view. The
/// panel starts under the chips, and it stops above the key line while that
/// line has words.
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
