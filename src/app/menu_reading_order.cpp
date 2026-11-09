// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/menu_reading_order.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

constexpr int32_t kColumnSlack = 16;
constexpr int32_t kRowCentreSlack = 8;
constexpr int32_t kRowGap = 40;
constexpr int32_t kArrowColumnSlack = 10;
constexpr int32_t kArrowRowStride = 5000;
constexpr int32_t kArrowWrap = 25'000'000;

struct Placed {
    std::size_t index = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t centre_y = 0;
};

[[nodiscard]] bool shown(const MenuCandidate& candidate) {
    return candidate.visible && candidate.enabled && !candidate.hidden && candidate.width > 0 &&
           candidate.height > 0;
}

[[nodiscard]] bool inside_press(const MenuCandidate& candidate, const MenuPressRect& press) {
    if (press.width <= 0 || press.height <= 0 || candidate.width <= 0 || candidate.height <= 0)
        return false;
    const int64_t right = static_cast<int64_t>(candidate.x) + static_cast<int64_t>(candidate.width);
    const int64_t bottom =
        static_cast<int64_t>(candidate.y) + static_cast<int64_t>(candidate.height);
    const int64_t press_right = static_cast<int64_t>(press.x) + static_cast<int64_t>(press.width);
    const int64_t press_bottom = static_cast<int64_t>(press.y) + static_cast<int64_t>(press.height);
    return static_cast<int64_t>(candidate.x) >= static_cast<int64_t>(press.x) &&
           static_cast<int64_t>(candidate.y) >= static_cast<int64_t>(press.y) &&
           right <= press_right && bottom <= press_bottom;
}

[[nodiscard]] bool covered(const MenuCandidate& candidate, std::span<const MenuPressRect> presses) {
    if (!candidate.gadget)
        return false;
    return std::any_of(presses.begin(), presses.end(), [&](const MenuPressRect& press) {
        return inside_press(candidate, press);
    });
}

[[nodiscard]] bool
joins_column(const MenuCandidate& candidate, std::span<const MenuPressRect> presses) {
    if (candidate.appended || !shown(candidate) || covered(candidate, presses))
        return false;
    if (candidate.gadget)
        return true;
    return candidate.button;
}

[[nodiscard]] std::vector<std::size_t> column_order(std::vector<Placed> places) {
    std::sort(places.begin(), places.end(), [](const Placed& left, const Placed& right) {
        if (left.x != right.x)
            return left.x < right.x;
        if (left.y != right.y)
            return left.y < right.y;
        return left.index < right.index;
    });
    std::vector<std::vector<Placed>> columns;
    int32_t column_x = 0;
    for (const Placed& placed : places) {
        if (columns.empty() || placed.x - column_x >= kColumnSlack) {
            columns.emplace_back();
            column_x = placed.x;
        }
        columns.back().push_back(placed);
    }
    for (std::vector<Placed>& column : columns) {
        std::sort(column.begin(), column.end(), [](const Placed& left, const Placed& right) {
            if (left.y != right.y)
                return left.y < right.y;
            if (left.x != right.x)
                return left.x < right.x;
            return left.index < right.index;
        });
    }
    std::vector<Placed> reading;
    for (const std::vector<Placed>& column : columns)
        reading.insert(reading.end(), column.begin(), column.end());

    std::vector<std::size_t> order;
    std::vector<char> emitted(reading.size(), 0);
    for (std::size_t index = 0; index < reading.size(); ++index) {
        if (emitted[index] != 0)
            continue;
        std::vector<std::size_t> run;
        run.push_back(index);
        const int32_t centre = reading[index].centre_y;
        for (std::size_t later = index + 1; later < reading.size(); ++later) {
            if (emitted[later] != 0)
                continue;
            const int32_t delta = reading[later].centre_y - centre;
            if (delta <= kRowCentreSlack && delta >= -kRowCentreSlack)
                run.push_back(later);
        }
        std::sort(run.begin(), run.end(), [&](std::size_t left, std::size_t right) {
            if (reading[left].x != reading[right].x)
                return reading[left].x < reading[right].x;
            return reading[left].index < reading[right].index;
        });
        std::vector<std::size_t> chain;
        chain.push_back(run.front());
        for (std::size_t at = 1; at < run.size(); ++at) {
            const Placed& previous = reading[chain.back()];
            const Placed& candidate = reading[run[at]];
            const int32_t gap = candidate.x - (previous.x + previous.width);
            if (gap > kRowGap)
                break;
            chain.push_back(run[at]);
        }
        for (const std::size_t at : chain) {
            emitted[at] = 1;
            order.push_back(reading[at].index);
        }
    }
    return order;
}

[[nodiscard]] MenuReadingStop stop_of(const MenuCandidate& candidate, std::size_t index) {
    MenuReadingStop stop;
    stop.index = index;
    stop.x = candidate.x;
    stop.y = candidate.y;
    stop.width = candidate.width;
    stop.height = candidate.height;
    return stop;
}

[[nodiscard]] bool same_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto folded = [](unsigned char byte) {
            return static_cast<char>(std::tolower(byte));
        };
        if (folded(static_cast<unsigned char>(left[index])) !=
            folded(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

} // namespace

bool gadget_inside_press(const MenuCandidate& candidate, const MenuPressRect& press) {
    return candidate.gadget && inside_press(candidate, press);
}

std::vector<MenuReadingStop> main_menu_reading_order(
    std::span<const MenuCandidate> candidates, std::span<const MenuPressRect> presses
) {
    std::vector<Placed> columns;
    std::vector<std::size_t> appended;
    // A marked gadget waits until the columns are known. It follows the last
    // extension button that joined, and stays out when none did.
    std::vector<std::size_t> following;
    bool extension_joins = false;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const MenuCandidate& candidate = candidates[index];
        if (candidate.appended) {
            if (shown(candidate))
                appended.push_back(index);
            continue;
        }
        if (candidate.follows_extension && candidate.gadget && shown(candidate) &&
            !covered(candidate, presses)) {
            following.push_back(index);
            continue;
        }
        if (!joins_column(candidate, presses))
            continue;
        if (!candidate.gadget && candidate.button)
            extension_joins = true;
        Placed placed;
        placed.index = index;
        placed.x = candidate.x;
        placed.y = candidate.y;
        placed.width = candidate.width;
        placed.centre_y = candidate.y + candidate.height / 2;
        columns.push_back(placed);
    }
    std::vector<MenuReadingStop> order;
    for (const std::size_t index : column_order(std::move(columns)))
        order.push_back(stop_of(candidates[index], index));
    if (extension_joins && !following.empty()) {
        std::size_t after = order.size();
        for (std::size_t index = 0; index < order.size(); ++index) {
            const MenuCandidate& candidate = candidates[order[index].index];
            if (!candidate.gadget && candidate.button)
                after = index + 1;
        }
        std::vector<MenuReadingStop> merged;
        merged.reserve(order.size() + following.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            if (index == after) {
                for (const std::size_t held : following)
                    merged.push_back(stop_of(candidates[held], held));
            }
            merged.push_back(order[index]);
        }
        if (after == order.size()) {
            for (const std::size_t held : following)
                merged.push_back(stop_of(candidates[held], held));
        }
        order = std::move(merged);
    }
    for (const std::size_t index : appended)
        order.push_back(stop_of(candidates[index], index));
    return order;
}

std::size_t
menu_reading_step(std::span<const MenuReadingStop> stops, std::size_t from, MenuReadingWay way) {
    if (stops.empty())
        return 0;
    if (from >= stops.size())
        from = 0;
    std::vector<int32_t> column(stops.size(), 0);
    for (std::size_t index = 0; index < stops.size(); ++index) {
        const int32_t left = stops[index].x;
        int32_t snapped = left;
        for (std::size_t probe = 0; probe < index; ++probe) {
            const int32_t delta = column[probe] - left;
            if (delta < kArrowColumnSlack && delta > -kArrowColumnSlack) {
                snapped = column[probe];
                break;
            }
        }
        column[index] = snapped;
    }
    const bool horizontal = way == MenuReadingWay::left || way == MenuReadingWay::right;
    const bool backward = way == MenuReadingWay::left || way == MenuReadingWay::up;
    const auto score_of = [&](std::size_t index) {
        if (horizontal)
            return stops[index].y * kArrowRowStride + stops[index].x;
        return column[index] * kArrowRowStride + stops[index].y;
    };
    const int32_t origin = score_of(from);
    int32_t best = backward ? origin - kArrowWrap : origin + kArrowWrap;
    std::size_t chosen = from;
    for (std::size_t index = 0; index < stops.size(); ++index) {
        if (index == from)
            continue;
        int32_t score = score_of(index);
        if (backward) {
            if (score >= origin)
                score -= kArrowWrap;
            if (score > best) {
                chosen = index;
                best = score;
            }
        } else if (score <= origin)
            score += kArrowWrap;
        if (!backward && score < best) {
            chosen = index;
            best = score;
        }
    }
    return chosen;
}

std::vector<std::string>
map_picker_reading_order(std::span<const std::string> names, bool more_maps) {
    std::vector<std::string> order;
    order.reserve(names.size() + (more_maps ? 1U : 0U));
    bool inserted = false;
    for (const std::string& name : names) {
        order.push_back(name);
        if (more_maps && !inserted && same_name(name, "MAPNAMES")) {
            order.emplace_back("more-maps");
            inserted = true;
        }
    }
    if (more_maps && !inserted)
        order.emplace_back("more-maps");
    return order;
}

} // namespace oa::app
