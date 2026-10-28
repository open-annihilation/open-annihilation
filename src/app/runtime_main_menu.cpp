// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's own main menu is given LIBRARY and SETTINGS buttons of its own
// look (Runtime::add_main_menu_entries): copies of SINGLE and INTRO, with
// copies of their plates one button's step above them, and the six buttons
// stand on three rows of two. They open Settings and the Library, drawn with
// the game's art over the whole window where it can be read
// (Runtime::main_menu_opens_game_art_screens).

#include "gui_record.hpp"
#include "oa_layer.hpp"
#include "runtime_game_menu_art.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/gui_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

namespace menu = oa::ui::frontend_state::main_menu;

/// Tells whether two record names are the same, compared in either case as
/// the game's own lookup compares them.
///
/// @param first one name
/// @param second the other
/// @return true when they are the same
bool same_record_name(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    for (std::size_t index = 0; index < first.size(); ++index)
        if (lower(first[index]) != lower(second[index]))
            return false;
    return true;
}

/// One button MAINMENU.GUI is given: the record it copies, its record name, its caption in English and its
/// quick key.
struct AddedEntry {
    std::string_view copied{};
    std::string_view name{};
    std::string_view caption{};
    char quick_key{};
};

/// The buttons MAINMENU.GUI is given, each made from a copy of the button
/// it names, on a copy of that button's plate one button's step above it.
constexpr std::array<AddedEntry, 2> added_entries{{
    {menu::resource_name(menu::Button::single_player), Runtime::main_menu_library_record, "LIBRARY", 'L'},
    {menu::resource_name(menu::Button::intro), Runtime::main_menu_settings_record, "SETTINGS", 'T'},
}};

/// How far a button's plate on the menu's picture reaches past the button,
/// in the picture's pixels: FrontendX's plate around SINGLE stands from 10
/// columns left of it to 9 right of it, and from 7 rows above it to 6 below.
constexpr int32_t kPlateLeft = 10;
constexpr int32_t kPlateRight = 9;
constexpr int32_t kPlateTop = 7;
constexpr int32_t kPlateBottom = 6;
/// The rows from one main-menu button to the next when MAINMENU.GUI has no
/// MULTI under SINGLE to measure them by: SINGLE's 393 to MULTI's 430.
constexpr int32_t kEntryStep = 37;

/// The main menu's buttons once MAINMENU.GUI is given LIBRARY and SETTINGS,
/// left to right and top to bottom on three rows of two: the left column is
/// SINGLE's, the right INTRO's, the top row the plates added above them.
constexpr std::array<std::string_view, 6> entry_order{{
    menu::resource_name(menu::Button::single_player),
    menu::resource_name(menu::Button::multiplayer),
    Runtime::main_menu_settings_record,
    Runtime::main_menu_library_record,
    menu::resource_name(menu::Button::intro),
    menu::resource_name(menu::Button::exit),
}};

/// Copies a rectangle of a picture onto another place of it, the colours and,
/// on a paletted picture, the indices.
///
/// @param picture the picture
/// @param x the rectangle's first column
/// @param y the rectangle's first row
/// @param width its columns
/// @param height its rows
/// @param to_y the first row it is copied to, in the same columns
void copy_rows(
    oa::Image& picture, int32_t x, int32_t y, int32_t width, int32_t height,
    int32_t to_y
) {
    const auto columns = static_cast<std::size_t>(picture.width);
    const bool indexed = picture.indices.size() == columns * picture.height;
    for (int32_t row = 0; row < height; ++row) {
        const std::size_t from = static_cast<std::size_t>(y + row) * columns + static_cast<std::size_t>(x);
        const std::size_t to = static_cast<std::size_t>(to_y + row) * columns + static_cast<std::size_t>(x);
        std::copy_n(picture.rgb.begin() + static_cast<std::ptrdiff_t>(from * 3U), static_cast<std::size_t>(width) * 3U,
                    picture.rgb.begin() + static_cast<std::ptrdiff_t>(to * 3U));
        if (indexed)
            std::copy_n(picture.indices.begin() + static_cast<std::ptrdiff_t>(from), width,
                        picture.indices.begin() + static_cast<std::ptrdiff_t>(to));
    }
}

} // namespace

void Runtime::add_main_menu_entries() {
    main_menu_entries_ = false;
    if (options_.classic_main_menu || main_menu_overlay_)
        return;
    auto& gadgets = resources_.layout.gadgets;
    auto& picture = resources_.background;
    if (gadgets.size() + added_entries.size() > oa::ui::gui_layout::limit::gadgets ||
        picture.rgb.size() != static_cast<std::size_t>(picture.width) * picture.height * 3U)
        return;
    // The step between SINGLE and MULTI, as MAINMENU.GUI places them.
    int32_t step = kEntryStep;
    const auto single = gui_record_named(gadgets, menu::resource_name(menu::Button::single_player));
    const auto multi = gui_record_named(gadgets, menu::resource_name(menu::Button::multiplayer));
    if (single && multi && gadgets[*multi].common.y > gadgets[*single].common.y)
        step = gadgets[*multi].common.y - gadgets[*single].common.y;
    // Every button and its plate must find its place before any is added.
    std::array<std::size_t, added_entries.size()> copied{};
    for (std::size_t at = 0; at < added_entries.size(); ++at) {
        const auto found = gui_record_named(gadgets, added_entries[at].copied);
        if (!found || gui_record_named(gadgets, added_entries[at].name) ||
            !std::holds_alternative<oa::ui::gui_layout::ButtonFields>(gadgets[*found].fields))
            return;
        const auto& common = gadgets[*found].common;
        const int32_t left = common.x - kPlateLeft;
        const int32_t top = common.y - kPlateTop;
        if (left < 0 || top - step < 0 ||
            common.x + common.width + kPlateRight > static_cast<int32_t>(picture.width) ||
            common.y + common.height + kPlateBottom > static_cast<int32_t>(picture.height))
            return;
        copied[at] = *found;
    }
    const int16_t left_x = gadgets[copied[0]].common.x;
    const int16_t right_x = gadgets[copied[1]].common.x;
    const int32_t top_y = gadgets[copied[0]].common.y - step;
    for (std::size_t at = 0; at < added_entries.size(); ++at) {
        const AddedEntry& entry = added_entries[at];
        oa::ui::gui_layout::Gadget gadget = gadgets[copied[at]];
        const auto& common = gadget.common;
        // The plate goes up with its button; the pipes beside it stay.
        copy_rows(
            picture,
            common.x - kPlateLeft,
            common.y - kPlateTop,
            common.width + kPlateLeft + kPlateRight,
            common.height + kPlateTop + kPlateBottom,
            common.y - kPlateTop - step
        );
        gadget.common.name = std::string(entry.name);
        gadget.common.y = static_cast<int16_t>(common.y - step);
        gadget.common.active = 1;
        auto& button = std::get<oa::ui::gui_layout::ButtonFields>(gadget.fields);
        button.source_text = std::string(entry.caption);
        button.text = std::string(oa::data::languages::interface_text(entry.caption));
        button.quick_key = static_cast<int8_t>(entry.quick_key);
        button.status = 0;
        button.grayed_out = false;
        gadgets.push_back(std::move(gadget));
    }
    for (std::size_t at = 0; at < entry_order.size(); ++at) {
        const auto found = gui_record_named(gadgets, entry_order[at]);
        if (!found)
            continue;
        auto& common = gadgets[*found].common;
        common.x = at % 2 == 0 ? left_x : right_x;
        common.y = static_cast<int16_t>(top_y + static_cast<int32_t>(at / 2) * step);
    }
    main_menu_entries_ = true;
}

bool Runtime::activate_main_menu_entry() {
    if (screen_ != Screen::main_menu || !main_menu_entries_ || selected_ < 0 ||
        static_cast<std::size_t>(selected_) >= resources_.layout.gadgets.size())
        return false;
    const std::string_view name = resources_.layout.gadgets[static_cast<std::size_t>(selected_)].common.name;
    const bool library = name == added_entries[0].name;
    if (!library && name != added_entries[1].name)
        return false;
    selected_ = -1;
    if (library)
        open_library(std::nullopt);
    else
        open_engine_settings_from_menu();
    return true;
}

bool Runtime::main_menu_opens_game_art_screens() {
    if (screen_ != Screen::main_menu || options_.classic_main_menu || frame_owned_by_package() ||
        oa_layer().screen_fonts() == nullptr)
        return false;
    if (!game_menu_art_readable_)
        game_menu_art_readable_ = read_game_menu_art(assets_, "LIBRARY", "library").has_value();
    return *game_menu_art_readable_;
}

std::optional<std::size_t>
gui_record_named(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name) {
    if (name.empty())
        return std::nullopt;
    for (std::size_t index = 0; index < gadgets.size(); ++index)
        if (same_record_name(gadgets[index].common.name, name))
            return index;
    return std::nullopt;
}

} // namespace oa::app
