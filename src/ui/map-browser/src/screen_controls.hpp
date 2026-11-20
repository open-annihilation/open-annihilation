// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map browser's control numbers and words, which its layout and its
// input share. Private to the module.
#pragma once

#include "oa/ui/kit/layout.hpp"
#include "oa/ui/map_browser/model.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::map_browser::controls {

/// The first source pill, All; Game, Mod and Downloads follow.
inline constexpr kit::ControlId first_source = 1;
/// The players strip: Any, then 2, 3, 4 and 5–10.
inline constexpr kit::ControlId players = 10;
/// The size strip: Any, then S, M and L.
inline constexpr kit::ControlId sizes = 20;
/// The search field.
inline constexpr kit::ControlId search = 30;
/// Cancel.
inline constexpr kit::ControlId cancel = 40;
/// The primary button.
inline constexpr kit::ControlId primary = 41;
/// The header's close mark.
inline constexpr kit::ControlId close = 42;
/// The source pills, one stop: All, Game, Mod and Downloads. Not a control
/// of its own. 100 sits clear of the strips
/// and of the cards. Named apart from the filters parameter these helpers take.
inline constexpr kit::ControlId filters_bar = 100;
/// The map cards, one stop. Not a control of its own.
inline constexpr kit::ControlId cards = 101;
/// The first card, the first shown; the card shown at place p is first_card + p.
inline constexpr kit::ControlId first_card = 1000;

/// The scroll group of the cards.
inline constexpr int32_t grid_group = 0;

/// The sources, in the order their pills show.
inline constexpr std::array<Filters::Source, 4> sources{
    Filters::Source::all, Filters::Source::game, Filters::Source::mod, Filters::Source::downloads
};
/// The players strip's segments, in order. 0 is Any; the rest are the chips.
inline constexpr std::array<int32_t, 5> player_levels{0, 2, 3, 4, many_players_chip};
/// The size strip's segments, in order. 0 is Any.
inline constexpr std::array<char, 4> size_levels{'\0', 'S', 'M', 'L'};
/// The players strip's part words, one per segment.
inline constexpr std::array<std::string_view, 5> player_parts{"any", "2", "3", "4", "5-10"};
/// The size strip's part words, one per segment.
inline constexpr std::array<std::string_view, 4> size_parts{"any", "s", "m", "l"};

/// Returns the players strip's chosen segment. 0 is Any.
///
/// @param filters the filters
/// @return the segment, from 0
[[nodiscard]] inline std::size_t players_index(const Filters& filters) noexcept {
    if (!filters.players || *filters.players == 0)
        return 0;
    for (std::size_t index = 1; index < player_levels.size(); ++index)
        if (player_levels[index] == *filters.players)
            return index;
    return 0;
}

/// Returns the size strip's chosen segment. 0 is Any.
///
/// @param filters the filters
/// @return the segment, from 0
[[nodiscard]] inline std::size_t size_index(const Filters& filters) noexcept {
    if (!filters.size)
        return 0;
    for (std::size_t index = 1; index < size_levels.size(); ++index)
        if (size_levels[index] == *filters.size)
            return index;
    return 0;
}

/// Sets the players selector from a segment. 0, and a segment past the
/// strip, are Any.
///
/// @param[in,out] filters the filters
/// @param index the segment, from 0
inline void choose_players(Filters& filters, std::size_t index) noexcept {
    if (index == 0 || index >= player_levels.size())
        filters.players.reset();
    else
        filters.players = player_levels[index];
}

/// Sets the size selector from a segment. 0, and a segment past the strip,
/// are Any.
///
/// @param[in,out] filters the filters
/// @param index the segment, from 0
inline void choose_size(Filters& filters, std::size_t index) noexcept {
    if (index == 0 || index >= size_levels.size())
        filters.size.reset();
    else
        filters.size = size_levels[index];
}

/// Tells whether a control is a card.
///
/// @param id the control
/// @return true for a card's control
[[nodiscard]] constexpr bool is_card(kit::ControlId id) noexcept {
    return id >= first_card;
}

/// Returns a card's place in what shows from its control.
///
/// @param id the card's control
/// @return its place, from 0
[[nodiscard]] constexpr std::size_t card_place(kit::ControlId id) noexcept {
    return static_cast<std::size_t>(id - first_card);
}

/// Returns the control of the card shown at a place.
///
/// @param place its place, from 0
/// @return the control's number
[[nodiscard]] constexpr kit::ControlId card_control(std::size_t place) noexcept {
    return first_card + static_cast<kit::ControlId>(place);
}

/// Returns one of the browser's words in the language the interface shows.
///
/// @param english the word, in English as this module writes it
/// @return its translation, or the English
[[nodiscard]] std::string word(std::string_view english);

/// Returns a pattern's words with each {name} replaced by its value: the
/// pattern is looked up first (word), the values are put in as given.
///
/// @param english the pattern, in English
/// @param values each name and its value
/// @return the text
[[nodiscard]] std::string filled(
    std::string_view english, const std::vector<std::pair<std::string_view, std::string>>& values
);

} // namespace oa::ui::map_browser::controls
