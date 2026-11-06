// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The way into the Boneyards on a main menu: the mode, and the place of the
// strip's ENTER plate.
#include "oa/boneyards/console/menu_entry.hpp"

#include <algorithm>
#include <span>
#include <string_view>

namespace oa::boneyards::console {
namespace {

namespace renderer = oa::ui::frontend_renderer;

/// Tells whether two rectangles share a pixel.
///
/// @param first a rectangle
/// @param second another
/// @return true when they overlap
bool overlap(const renderer::SourceRect& first, const renderer::SourceRect& second) noexcept {
    return first.width > 0 && first.height > 0 && second.width > 0 && second.height > 0 &&
           first.x < second.x + second.width && second.x < first.x + first.width &&
           first.y < second.y + second.height && second.y < first.y + first.height;
}

} // namespace

MenuEntryAsk menu_entry_ask_named(std::string_view text) noexcept {
    if (text == "strip")
        return MenuEntryAsk::strip;
    if (text == "button")
        return MenuEntryAsk::button;
    if (text == "none")
        return MenuEntryAsk::none;
    return MenuEntryAsk::automatic;
}

MenuEntryMode choose_menu_entry_mode(MenuEntryAsk ask, uint32_t differing_per_mille) noexcept {
    switch (ask) {
    case MenuEntryAsk::strip:
        return MenuEntryMode::strip;
    case MenuEntryAsk::button:
        return MenuEntryMode::button;
    case MenuEntryAsk::none:
        return MenuEntryMode::none;
    case MenuEntryAsk::automatic:
        break;
    }
    return differing_per_mille <= strip_art_limit_per_mille ? MenuEntryMode::strip
                                                            : MenuEntryMode::button;
}

renderer::SourceRect entry_plate_place(int32_t x, int32_t y) noexcept {
    const int32_t right = renderer::menu_picture_width - entry_plate.width;
    const int32_t bottom = renderer::menu_picture_height - entry_plate.height;
    return {
        std::clamp(x, 0, right), std::clamp(y, 0, bottom), entry_plate.width, entry_plate.height
    };
}

bool entry_plate_covers(
    const renderer::SourceRect& place, std::span<const renderer::SourceRect> others
) noexcept {
    const renderer::SourceRect line{
        place.x - entry_plate_outline,
        place.y - entry_plate_outline,
        place.width + 2 * entry_plate_outline,
        place.height + 2 * entry_plate_outline,
    };
    return std::any_of(others.begin(), others.end(), [&line](const renderer::SourceRect& other) {
        return overlap(line, other);
    });
}

} // namespace oa::boneyards::console
