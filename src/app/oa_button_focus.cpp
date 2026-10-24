// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa_button_focus.hpp"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace oa::app {
namespace {

/// How far a record's left edge may lie from the anchor's and still count
/// as the anchor's column, in frame pixels.
constexpr int32_t column_slack = 10;

} // namespace

int64_t order_key(const oa::ui::frontend_state::PlaceRect& rect) noexcept {
    return static_cast<int64_t>(rect.y) * 5000 + rect.x;
}

int32_t record_before(
    const std::vector<FocusRecord>& records, const oa::ui::frontend_state::PlaceRect& button
) {
    const int64_t key = order_key(button);
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusRecord& record : records) {
        const int64_t other = order_key(record.at);
        if (other <= key && (found < 0 || other > best)) {
            best = other;
            found = record.index;
        }
    }
    return found;
}

int32_t record_after(
    const std::vector<FocusRecord>& records, const oa::ui::frontend_state::PlaceRect& button
) {
    const int64_t key = order_key(button);
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusRecord& record : records) {
        const int64_t other = order_key(record.at);
        if (other > key && (found < 0 || other < best)) {
            best = other;
            found = record.index;
        }
    }
    return found;
}

int32_t first_record(const std::vector<FocusRecord>& records) {
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusRecord& record : records) {
        const int64_t other = order_key(record.at);
        if (found < 0 || other < best) {
            best = other;
            found = record.index;
        }
    }
    return found;
}

int32_t last_record(const std::vector<FocusRecord>& records) {
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusRecord& record : records) {
        const int64_t other = order_key(record.at);
        if (found < 0 || other > best) {
            best = other;
            found = record.index;
        }
    }
    return found;
}

int32_t column_below(
    const std::vector<FocusRecord>& records,
    int32_t anchor_x,
    const oa::ui::frontend_state::PlaceRect& button
) {
    int32_t best_y = 0;
    int32_t found = -1;
    for (const FocusRecord& record : records) {
        const int32_t delta = record.frame_x - anchor_x;
        if (delta <= -column_slack || delta >= column_slack)
            continue;
        if (record.at.y < button.y + button.height)
            continue;
        if (found < 0 || record.frame_y < best_y) {
            best_y = record.frame_y;
            found = record.index;
        }
    }
    return found;
}

int32_t nearest_record(
    const std::vector<FocusRecord>& records,
    const oa::ui::frontend_state::PlaceRect& button,
    Aim aim
) {
    const int32_t origin_x = button.x + button.width / 2;
    const int32_t origin_y = button.y + button.height / 2;
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusRecord& record : records) {
        const int32_t centre_x = record.at.x + record.at.width / 2;
        const int32_t centre_y = record.at.y + record.at.height / 2;
        const int32_t dx = centre_x - origin_x;
        const int32_t dy = centre_y - origin_y;
        int32_t primary = 0;
        int32_t secondary = 0;
        switch (aim) {
        case Aim::up:
            primary = -dy;
            secondary = dx;
            break;
        case Aim::down:
            primary = dy;
            secondary = dx;
            break;
        case Aim::left:
            primary = -dx;
            secondary = dy;
            break;
        case Aim::right:
            primary = dx;
            secondary = dy;
            break;
        }
        if (primary <= 0 || std::abs(primary) < std::abs(secondary))
            continue;
        const int64_t distance = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
        if (found < 0 || distance < best) {
            best = distance;
            found = record.index;
        }
    }
    return found;
}

} // namespace oa::app
