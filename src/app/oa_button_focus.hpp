// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Focus shared by the kit buttons that sit on a screen of gadgets: which
// record comes before a button in Tab's order, and which record an arrow
// from the button lands on. The buttons are MUTATORS… and a computer
// seat's brain.
#pragma once

#include "oa/ui/frontend_state/oa_button_place.hpp"

#include <cstdint>
#include <vector>

namespace oa::app {

/// One focusable record, in the layer's points, with the frame column it sits in.
struct FocusRecord {
    int32_t index{-1};       ///< the gadget's index
    oa::ui::frontend_state::PlaceRect at{}; ///< its rectangle, in points
    int32_t frame_x{};       ///< its left edge, in frame pixels
    int32_t frame_y{};       ///< its top edge, in frame pixels
    int32_t focus_place{-1}; ///< its place in the battle room's order; -1 elsewhere
};

/// Which way an arrow from a button looks for a record.
enum class Aim : uint8_t {
    up,    ///< towards the top
    down,  ///< towards the bottom
    left,  ///< towards the left
    right, ///< towards the right
};

/// The Tab key of a rectangle: its top times 5000 plus its left.
///
/// @param rect the rectangle, in points
/// @return the key the gadget engine orders records by
[[nodiscard]] int64_t order_key(const oa::ui::frontend_state::PlaceRect& rect) noexcept;

/// The record just before the button, the first of a shared key.
///
/// @param records the focusable records
/// @param button the button
/// @return its index, or -1 when none comes before
[[nodiscard]] int32_t
record_before(const std::vector<FocusRecord>& records, const oa::ui::frontend_state::PlaceRect& button);

/// The record just after the button, the first of a shared key.
///
/// @param records the focusable records
/// @param button the button
/// @return its index, or -1 when none comes after
[[nodiscard]] int32_t
record_after(const std::vector<FocusRecord>& records, const oa::ui::frontend_state::PlaceRect& button);

/// The record with the smallest Tab key.
///
/// @param records the focusable records
/// @return its index, or -1 when there are none
[[nodiscard]] int32_t first_record(const std::vector<FocusRecord>& records);

/// The record with the largest Tab key.
///
/// @param records the focusable records
/// @return its index, or -1 when there are none
[[nodiscard]] int32_t last_record(const std::vector<FocusRecord>& records);

/// The next record down the anchor's column that sits wholly under the button.
///
/// The column is the frame pixels whose left edge lies within 10 of the anchor's.
///
/// @param records the focusable records
/// @param anchor_x the anchor's left edge, in frame pixels
/// @param button the button, in points
/// @return its index, or -1 when the column has none under the button
[[nodiscard]] int32_t column_below(
    const std::vector<FocusRecord>& records,
    int32_t anchor_x,
    const oa::ui::frontend_state::PlaceRect& button
);

/// The nearest focusable record in a direction, by the rectangles' centres.
///
/// A record is in the direction when the primary step is positive and at
/// least the step across. The nearest centre wins.
///
/// @param records the focusable records
/// @param button the button
/// @param aim the direction
/// @return its index, or -1 when none lies that way
[[nodiscard]] int32_t nearest_record(
    const std::vector<FocusRecord>& records,
    const oa::ui::frontend_state::PlaceRect& button,
    Aim aim
);

} // namespace oa::app
