// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map browser's screen in a synthetic font: 3, 4 and 6 columns at
// Compact, Regular and Large; nothing outside the window; the footer's
// buttons; every automation name, sound and unique, two maps whose names
// give one word told apart; the pictures asked for only for the cards in
// view and the row after; focus by geometry and Tab in the declared order;
// and what a press, a tap, the keys, typing, the wheel and the scroll bar
// do.
#include "oa/formats/fnt.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/map_browser/model.hpp"
#include "oa/ui/map_browser/screen.hpp"
#include "oa/ui/map_previews/map_previews.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <system_error>
#include <stdint.h>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

/// Runs the screen's tests; model_test.cpp's main calls it.
void layout_tests();

namespace {

namespace kit = oa::ui::kit;
namespace mb = oa::ui::map_browser;
namespace renderer = oa::ui::frontend_renderer;
namespace previews = oa::ui::map_previews;
namespace png = oa::formats::png;
namespace text_font = oa::platform::text_font;

/// The synthetic font of the settings pixel test, for both roles.
///
/// @return the fonts
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 1);
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// Returns the fonts every layout here is measured in.
///
/// @return the fonts
const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

/// One size class's frame, as the OA layer gives it for a window.
struct ClassFrame {
    kit::Frame frame{};    ///< the frame
    std::size_t columns{}; ///< the grid's columns there
    kit::Point window{};   ///< the browser's window there
};

/// The frames: Compact at 640 by 480, Regular at 960 by 540 points and
/// Large at 1280 by 720.
///
/// @return the frames
std::array<ClassFrame, 3> frames() {
    return {{
        {{{0, 0, 640, 480}, kit::SizeClass::compact, 100}, 3, {480, 324}},
        {{{0, 0, 960, 540}, kit::SizeClass::regular, 200}, 4, {720, 486}},
        {{{0, 0, 1280, 720}, kit::SizeClass::large, 200}, 6, {960, 600}},
    }};
}

/// Returns a browser of many maps: forty game maps, one mod map, two
/// installed, one of which does not fit, and three online, two of whose
/// names give one word.
///
/// @return the browser, its filters applied
mb::Browser many_maps() {
    std::vector<mb::MapCard> game;
    for (int32_t index = 0; index < 40; ++index) {
        mb::MapCard card;
        card.name = "Base Map " + std::to_string(100 + index);
        card.title = card.name;
        card.players = 2 + index % 9;
        card.size = std::to_string(8 + index % 25) + " x " + std::to_string(8 + index % 20);
        card.preview_key = card.name;
        if (index % 3 == 1)
            card.source_name = "Core Contingency";
        else if (index % 3 == 2)
            card.source_name = "Battle Tactics";
        else
            card.source_name = "TA 3.1c";
        game.push_back(std::move(card));
    }
    std::vector<mb::MapCard> mod(1);
    mod[0].name = "ridge_valley";
    mod[0].title = "Ridge Valley";
    mod[0].source_name = "Ridge";
    mod[0].players = 3;
    mod[0].size = "14 x 14";
    mod[0].preview_key = mod[0].name;
    std::vector<mb::MapCard> installed(2);
    installed[0].name = "check_isle@oa-check-isles";
    installed[0].title = "Check Isle";
    installed[0].pack_name = "Map Pack Check Isles";
    installed[0].players = 2;
    installed[0].size = "2x2";
    installed[1].name = "check_clash@oa-check-clash";
    installed[1].title = "Check Clash";
    installed[1].pack_name = "Map Pack Check Clash";
    installed[1].players = 2;
    installed[1].size = "2x2";
    installed[1].base_fit = mb::MapCard::Fit::does_not_fit;
    installed[1].fit_reason = "New names: the feature 'dragonsteeth' is defined already";
    std::vector<mb::MapCard> online(3);
    online[0].name = "isle_of_ashes@archipelago";
    online[0].title = "Isle of Ashes";
    online[0].pack_name = "Archipelago Pack";
    online[0].players = 4;
    online[0].size = "16x16";
    online[0].download_bytes = 2400000;
    online[0].preview_key = "http://127.0.0.1:9/v1/i/one.png";
    online[1].name = "a_b@twins";
    online[1].title = "Twin One";
    online[1].pack_name = "Twin Pack";
    online[1].players = 4;
    online[1].size = "16x16";
    online[2].name = "a-b@twins";
    online[2].title = "Twin Two";
    online[2].pack_name = "Twin Pack";
    online[2].players = 4;
    online[2].size = "16x16";
    mb::Browser browser;
    browser.cards =
        mb::merge_cards(std::move(game), std::move(mod), std::move(installed), std::move(online));
    browser.mod_name = "Ridge";
    mb::apply_filters(browser);
    return browser;
}

/// Counts the hooks' asks, and which cards they were for.
struct AskCounter {
    std::vector<std::string> asked{}; ///< the cards' names, in order
};

/// The counting hooks.
///
/// @param counter the counter
/// @return the hooks
mb::PreviewHooks counting(AskCounter& counter) {
    mb::PreviewHooks hooks;
    hooks.context = &counter;
    hooks.ask = [](void* context, const mb::MapCard& card) -> const previews::Thumbnail* {
        static_cast<AskCounter*>(context)->asked.push_back(card.name);
        return nullptr;
    };
    return hooks;
}

/// Lays a browser out with hooks that ask for nothing.
///
/// @param browser the browser
/// @param state the screen's state
/// @param frame the frame
/// @return the display list
kit::DisplayList
laid_out(const mb::Browser& browser, const mb::ScreenState& state, const kit::Frame& frame) {
    return mb::browser_layout(browser, state, frame, fonts(), {});
}

/// Returns the control of a name.
///
/// @param list the display list
/// @param name the name
/// @return the control; null when none has it
const kit::Control* named(const kit::DisplayList& list, std::string_view name) {
    const kit::ControlId id = kit::control_named(list, name);
    return id == kit::no_control ? nullptr : kit::control_of(list, id);
}

/// Returns the cards' controls, in list order.
///
/// @param list the display list
/// @return the controls
std::vector<const kit::Control*> card_controls(const kit::DisplayList& list) {
    std::vector<const kit::Control*> cards;
    for (const kit::Control& control : list.controls)
        if (control.name.starts_with(mb::name_card_prefix))
            cards.push_back(&control);
    return cards;
}

/// Returns what of a rectangle a clip leaves; the rectangle when no clip.
///
/// @param rect the rectangle
/// @param clip the clip; empty for none
/// @return the part that can show
kit::Rect visible_part(const kit::Rect& rect, const kit::Rect& clip) {
    if (clip.width <= 0 || clip.height <= 0)
        return rect;
    return kit::intersect(rect, clip);
}

/// Returns where a named control or one of its parts lies.
///
/// @param list the display list
/// @param name the control's name, or a part's (`maps-players.4`)
/// @return the rectangle a press reaches; empty when nothing has the name
kit::Rect named_rect(const kit::DisplayList& list, std::string_view name) {
    if (const kit::Control* control = named(list, name))
        return visible_part(control->rect, control->clip);
    for (const kit::AutomationEntry& entry : kit::automation_parts(list, {}))
        if (entry.name == name)
            return entry.rect;
    return {};
}

/// Presses and releases the pointer at a control's centre, or a part's, or taps it.
///
/// @param browser the browser
/// @param state the screen's state
/// @param list the display list
/// @param name the control's name, or a part's
/// @param reach a finger's reach; 0 for a mouse
/// @return what the release asked for
mb::ScreenResult press(
    mb::Browser& browser,
    mb::ScreenState& state,
    const kit::DisplayList& list,
    std::string_view name,
    int32_t reach = 0
) {
    const kit::Rect shown = named_rect(list, name);
    OA_CHECK(shown.width > 0 && shown.height > 0);
    if (shown.width <= 0 || shown.height <= 0)
        return {};
    const kit::Point at{shown.x + shown.width / 2, shown.y + shown.height / 2};
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::move, 0, at, reach);
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::down, 1, at, reach);
    return mb::browser_pointer(browser, state, list, mb::PointerKind::up, 1, at, reach);
}

void columns_at_each_class() {
    for (const ClassFrame& each : frames()) {
        const mb::Browser browser = many_maps();
        const kit::DisplayList list = laid_out(browser, {}, each.frame);
        OA_CHECK(mb::window_size(each.frame).x == each.window.x);
        OA_CHECK(mb::window_size(each.frame).y == each.window.y);
        OA_CHECK(mb::grid_columns(each.frame.size_class) == each.columns);
        const std::vector<const kit::Control*> cards = card_controls(list);
        OA_CHECK(cards.size() == browser.shown.size());
        std::size_t first_row = 0;
        for (const kit::Control* control : cards)
            if (control->rect.y == cards.front()->rect.y)
                ++first_row;
        OA_CHECK(first_row == each.columns);
    }
}

void nothing_outside_the_window() {
    for (const ClassFrame& each : frames()) {
        mb::Browser browser = many_maps();
        browser.selected = browser.shown.front();
        mb::ScreenState state;
        // The last row, and the first, scrolled to.
        for (const int32_t scroll : {0, 100000}) {
            state.scroll = scroll;
            const kit::DisplayList list = laid_out(browser, state, each.frame);
            const kit::Rect window{0, 0, each.window.x, each.window.y};
            for (const kit::Item& item : list.items) {
                const kit::Rect drawn = visible_part(item.rect, item.clip);
                if (drawn.width <= 0 || drawn.height <= 0)
                    continue;
                if (!kit::wholly_in(drawn, window))
                    std::fprintf(
                        stderr,
                        "an item of role %d lies outside the window at %d,%d %dx%d\n",
                        static_cast<int>(item.role),
                        drawn.x,
                        drawn.y,
                        drawn.width,
                        drawn.height
                    );
                OA_CHECK(kit::wholly_in(drawn, window));
            }
            for (const kit::Control& control : list.controls) {
                const kit::Rect reach = visible_part(control.rect, control.clip);
                if (reach.width > 0 && reach.height > 0)
                    OA_CHECK(kit::wholly_in(reach, window));
            }
        }
        // A download under way, and a failure, keep inside too.
        browser.downloading = browser.cards[browser.shown.front()].name;
        browser.progress = 0.5F;
        kit::DisplayList list = laid_out(browser, {}, each.frame);
        bool bar = false;
        for (const kit::Item& item : list.items)
            if (item.role == kit::Role::progress) {
                bar = true;
                OA_CHECK(kit::wholly_in(item.rect, {0, 0, each.window.x, each.window.y}));
            }
        OA_CHECK(bar);
        browser.downloading.reset();
        browser.error = std::string(300, 'e');
        list = laid_out(browser, {}, each.frame);
        for (const kit::Item& item : list.items) {
            const kit::Rect drawn = visible_part(item.rect, item.clip);
            if (drawn.width > 0 && drawn.height > 0)
                OA_CHECK(kit::wholly_in(drawn, {0, 0, each.window.x, each.window.y}));
        }
    }
}

void names_and_buttons() {
    const std::array<std::string_view, 11> listed{
        mb::name_filter_all,
        mb::name_filter_game,
        mb::name_filter_mod,
        mb::name_filter_downloads,
        mb::name_players,
        mb::name_sizes,
        mb::name_search,
        mb::name_cancel,
        mb::name_select,
        mb::name_close,
        "maps-card-isle-of-ashes-archipelago",
    };
    const std::array<std::string_view, 9> parts{
        "maps-players.any",
        "maps-players.2",
        "maps-players.3",
        "maps-players.4",
        "maps-players.5-10",
        "maps-size.any",
        "maps-size.s",
        "maps-size.m",
        "maps-size.l",
    };
    for (const ClassFrame& each : frames()) {
        const mb::Browser browser = many_maps();
        const kit::DisplayList list = laid_out(browser, {}, each.frame);
        OA_CHECK(kit::name_problem(list).empty());
        for (const std::string_view name : listed) {
            if (named(list, name) == nullptr)
                std::fprintf(
                    stderr, "no control is named %.*s\n", static_cast<int>(name.size()), name.data()
                );
            OA_CHECK(named(list, name) != nullptr);
        }
        for (const std::string_view part : parts) {
            if (named_rect(list, part).width <= 0)
                std::fprintf(
                    stderr, "no part is named %.*s\n", static_cast<int>(part.size()), part.data()
                );
            OA_CHECK(named_rect(list, part).width > 0);
        }
        // The pills name the source and how many maps it holds.
        const kit::Control* all_pill = named(list, mb::name_filter_all);
        const kit::Control* game_pill = named(list, mb::name_filter_game);
        const kit::Control* mod_pill = named(list, mb::name_filter_mod);
        const kit::Control* downloads_pill = named(list, mb::name_filter_downloads);
        OA_CHECK(all_pill != nullptr && all_pill->text == "All (46)");
        OA_CHECK(game_pill != nullptr && game_pill->text == "Game (40)");
        OA_CHECK(mod_pill != nullptr && mod_pill->text == "Mod (1)");
        OA_CHECK(downloads_pill != nullptr && downloads_pill->text == "Downloads (5)");
        // Two maps whose names give one word: the second in grid order takes -2.
        const kit::Control* one = named(list, "maps-card-a-b-twins");
        const kit::Control* two = named(list, "maps-card-a-b-twins-2");
        OA_CHECK(one != nullptr && two != nullptr);
        if (one != nullptr && two != nullptr) {
            OA_CHECK(one->text == "a_b@twins");
            OA_CHECK(two->text == "a-b@twins");
        }
        // The card's text is its game name.
        const kit::Control* isle = named(list, "maps-card-check-isle-oa-check-isles");
        OA_CHECK(isle != nullptr && isle->text == "check_isle@oa-check-isles");
        // The footer's buttons: Cancel, and Select disabled with nothing selected.
        const kit::Control* cancel = named(list, mb::name_cancel);
        const kit::Control* select_button = named(list, mb::name_select);
        OA_CHECK(cancel != nullptr && cancel->enabled && cancel->text == "Cancel");
        OA_CHECK(
            select_button != nullptr && !select_button->enabled && select_button->text == "Select"
        );
    }
}

void pictures_asked_for_the_view_alone() {
    for (const ClassFrame& each : frames()) {
        const mb::Browser browser = many_maps();
        for (const int32_t scroll : {0, 400}) {
            mb::ScreenState state;
            state.scroll = scroll;
            AskCounter counter;
            const kit::DisplayList list =
                mb::browser_layout(browser, state, each.frame, fonts(), counting(counter));
            const std::vector<std::size_t> in_view =
                mb::cards_in_view(browser, state, each.frame, fonts());
            // Every card in view and the next row with a picture, and no other.
            std::vector<std::string> expected;
            for (const std::size_t index : in_view)
                if (!browser.cards[index].preview_key.empty())
                    expected.push_back(browser.cards[index].name);
            OA_CHECK(counter.asked == expected);
            OA_CHECK(!counter.asked.empty());
            OA_CHECK(counter.asked.size() < browser.shown.size());
            // The cards in view are those whose controls show, and one row more.
            std::size_t showing = 0;
            for (const kit::Control* control : card_controls(list))
                if (visible_part(control->rect, control->clip).height > 0)
                    ++showing;
            OA_CHECK(in_view.size() >= showing && in_view.size() <= showing + each.columns);
            OA_CHECK(in_view.size() > showing);
        }
    }
}

void focus_moves_by_geometry() {
    for (const ClassFrame& each : frames()) {
        mb::Browser browser = many_maps();
        mb::ScreenState state;
        kit::DisplayList list = laid_out(browser, state, each.frame);
        const std::vector<const kit::Control*> cards = card_controls(list);
        OA_CHECK(cards.size() > each.columns);
        if (cards.size() <= each.columns)
            continue;
        const kit::ControlId first = cards[0]->id;
        const kit::ControlId second = cards[1]->id;
        const kit::ControlId below = cards[each.columns]->id;
        state.interaction.focused = first;
        state.interaction.focus_shown = true;
        std::ignore = mb::browser_key(browser, state, list, kit::Key::right);
        OA_CHECK(state.interaction.focused == second);
        list = laid_out(browser, state, each.frame);
        std::ignore = mb::browser_key(browser, state, list, kit::Key::left);
        OA_CHECK(state.interaction.focused == first);
        list = laid_out(browser, state, each.frame);
        std::ignore = mb::browser_key(browser, state, list, kit::Key::down);
        OA_CHECK(state.interaction.focused == below);
        // The grid scrolls to show the focused card.
        list = laid_out(browser, state, each.frame);
        const kit::Control* focused = kit::control_of(list, state.interaction.focused);
        OA_CHECK(focused != nullptr && kit::wholly_in(focused->rect, focused->clip));
        // Up from the first row leaves the grid for the bar above it.
        state.interaction.focused = first;
        state.scroll = 0;
        list = laid_out(browser, state, each.frame);
        std::ignore = mb::browser_key(browser, state, list, kit::Key::up);
        const kit::Control* above = kit::control_of(list, state.interaction.focused);
        OA_CHECK(above != nullptr && !above->name.starts_with(mb::name_card_prefix));
        const std::vector<const kit::Control*> now = card_controls(list);
        OA_CHECK(above != nullptr && !now.empty() && above->rect.y < now[0]->rect.y);
    }
}

void tab_follows_the_declared_order() {
    const std::array<std::string_view, 7> bar_order{
        mb::name_filter_all,
        mb::name_filter_game,
        mb::name_filter_mod,
        mb::name_filter_downloads,
        mb::name_players,
        mb::name_sizes,
        mb::name_search,
    };
    for (const ClassFrame& each : frames()) {
        mb::Browser browser = many_maps();
        // The selected card is where Tab enters the grid.
        std::size_t isle = 0;
        for (std::size_t index = 0; index < browser.cards.size(); ++index)
            if (browser.cards[index].name == "isle_of_ashes@archipelago")
                isle = index;
        browser.selected = isle;
        mb::ScreenState state;
        std::vector<std::string> walked;
        for (int32_t step = 0; step < 11; ++step) {
            const kit::DisplayList list = laid_out(browser, state, each.frame);
            std::ignore = mb::browser_key(browser, state, list, kit::Key::tab);
            const kit::DisplayList after = laid_out(browser, state, each.frame);
            const kit::Control* control = kit::control_of(after, state.interaction.focused);
            walked.push_back(control != nullptr ? control->name : std::string());
        }
        for (std::size_t index = 0; index < bar_order.size(); ++index)
            OA_CHECK(walked[index] == bar_order[index]);
        OA_CHECK(walked[7] == "maps-card-isle-of-ashes-archipelago");
        OA_CHECK(walked[8] == mb::name_cancel);
        OA_CHECK(walked[9] == mb::name_select);
        OA_CHECK(walked[10] == mb::name_close);
        // Shift+Tab from the close mark goes back to the grid at the selected card.
        kit::DisplayList list = laid_out(browser, state, each.frame);
        std::ignore = mb::browser_key(browser, state, list, kit::Key::back_tab);
        list = laid_out(browser, state, each.frame);
        std::ignore = mb::browser_key(browser, state, list, kit::Key::back_tab);
        list = laid_out(browser, state, each.frame);
        std::ignore = mb::browser_key(browser, state, list, kit::Key::back_tab);
        list = laid_out(browser, state, each.frame);
        const kit::Control* control = kit::control_of(list, state.interaction.focused);
        OA_CHECK(control != nullptr && control->name == "maps-card-isle-of-ashes-archipelago");
        OA_CHECK(control != nullptr && kit::wholly_in(control->rect, control->clip));
    }
}

void pointer_and_keys() {
    const kit::Frame frame = frames()[1].frame;
    mb::Browser browser = many_maps();
    mb::ScreenState state;
    kit::DisplayList list = laid_out(browser, state, frame);

    // Downloads holds the installed packs and the online packs together.
    std::ignore = press(browser, state, list, mb::name_filter_downloads);
    OA_CHECK(browser.filters.source == mb::Filters::Source::downloads);
    OA_CHECK(browser.shown.size() == 5);
    list = laid_out(browser, state, frame);
    const kit::Control* chip = named(list, mb::name_filter_downloads);
    OA_CHECK(chip != nullptr && chip->checked);
    std::ignore = press(browser, state, list, mb::name_filter_all);
    list = laid_out(browser, state, frame);
    std::ignore = press(browser, state, list, "maps-players.4");
    OA_CHECK(browser.filters.players && *browser.filters.players == 4);
    list = laid_out(browser, state, frame);
    std::ignore = press(browser, state, list, "maps-size.m");
    OA_CHECK(browser.filters.size && *browser.filters.size == 'M');
    for (const std::size_t index : browser.shown) {
        OA_CHECK(browser.cards[index].players == 4);
        OA_CHECK(mb::size_letter(browser.cards[index].size) == 'M');
    }
    // Any clears a selector. The segment already chosen stays chosen.
    list = laid_out(browser, state, frame);
    std::ignore = press(browser, state, list, "maps-players.4");
    OA_CHECK(browser.filters.players && *browser.filters.players == 4);
    std::ignore = press(browser, state, list, "maps-players.any");
    list = laid_out(browser, state, frame);
    std::ignore = press(browser, state, list, "maps-size.any");
    OA_CHECK(!browser.filters.players && !browser.filters.size);
    // Left and Right step the focused strip, and stop at the ends. Enter and
    // Space on it leave the choice and do not accept the dialog.
    list = laid_out(browser, state, frame);
    state.interaction.focused = kit::control_named(list, mb::name_players);
    state.interaction.focus_shown = true;
    mb::ScreenResult stepped = mb::browser_key(browser, state, list, kit::Key::left);
    OA_CHECK(stepped.action == mb::BrowserAction::none && !browser.filters.players);
    stepped = mb::browser_key(browser, state, list, kit::Key::right);
    OA_CHECK(browser.filters.players && *browser.filters.players == 2);
    list = laid_out(browser, state, frame);
    stepped = mb::browser_key(browser, state, list, kit::Key::left);
    OA_CHECK(!browser.filters.players);
    stepped = mb::browser_key(browser, state, list, kit::Key::enter);
    OA_CHECK(stepped.action == mb::BrowserAction::none && !browser.filters.players);
    stepped = mb::browser_key(browser, state, list, kit::Key::space);
    OA_CHECK(stepped.action == mb::BrowserAction::none && !browser.filters.players);

    // Typing in the search filters as it goes.
    list = laid_out(browser, state, frame);
    std::ignore = press(browser, state, list, mb::name_search);
    list = laid_out(browser, state, frame);
    OA_CHECK(mb::focused_field(state, list).has_value());
    std::ignore = mb::browser_text(browser, state, list, "is");
    OA_CHECK(browser.filters.query == "is");
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_text(browser, state, list, "le");
    OA_CHECK(browser.filters.query == "isle");
    OA_CHECK(browser.shown.size() == 2);
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_key(browser, state, list, kit::Key::backspace);
    OA_CHECK(browser.filters.query == "isl");
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_text(browser, state, list, "e");

    // The controller's Y gives the search the focus.
    state.interaction = {};
    list = laid_out(browser, state, frame);
    OA_CHECK(!mb::focused_field(state, list).has_value());
    std::ignore = mb::browser_find(state, list);
    OA_CHECK(mb::focused_field(state, list).has_value());

    // A press on a card selects it; a press on the selected card acts.
    list = laid_out(browser, state, frame);
    mb::ScreenResult result = press(browser, state, list, "maps-card-check-isle-oa-check-isles");
    OA_CHECK(result.action == mb::BrowserAction::none);
    OA_CHECK(
        browser.selected && browser.cards[*browser.selected].name == "check_isle@oa-check-isles"
    );
    list = laid_out(browser, state, frame);
    const kit::Control* select_button = named(list, mb::name_select);
    OA_CHECK(select_button != nullptr && select_button->enabled);
    result = press(browser, state, list, "maps-card-check-isle-oa-check-isles");
    OA_CHECK(result.action == mb::BrowserAction::choose);
    result = press(browser, state, list, mb::name_select);
    OA_CHECK(result.action == mb::BrowserAction::choose);

    // A tap on a map that does not fit selects it, and acts on nothing.
    state.search = {};
    browser.filters.query.clear();
    browser.filters.source = mb::Filters::Source::downloads;
    mb::apply_filters(browser);
    list = laid_out(browser, state, frame);
    result = press(browser, state, list, "maps-card-check-clash-oa-check-clash", 8);
    OA_CHECK(result.action == mb::BrowserAction::none);
    list = laid_out(browser, state, frame);
    result = press(browser, state, list, "maps-card-check-clash-oa-check-clash", 8);
    OA_CHECK(result.action == mb::BrowserAction::none);
    select_button = named(list, mb::name_select);
    OA_CHECK(select_button != nullptr && !select_button->enabled);
    // Its card is greyed with the problem chip, and the footer gives the reason.
    bool greyed = false;
    bool reason = false;
    for (const kit::Item& item : list.items) {
        if (const auto* card = std::get_if<kit::CardLook>(&item.look);
            card != nullptr && card->title == "Check Clash")
            greyed = card->greyed && card->chips.size() == 2 &&
                     card->chips[1].state == kit::ChipState::problem;
        if (item.role == kit::Role::text && item.text.find("New names") != std::string::npos)
            reason = true;
    }
    OA_CHECK(greyed);
    OA_CHECK(reason);

    // Enter on the focused selected card acts; Escape cancels.
    state.interaction.focused = kit::control_named(list, "maps-card-check-isle-oa-check-isles");
    state.interaction.focus_shown = true;
    result = mb::browser_key(browser, state, list, kit::Key::enter);
    OA_CHECK(result.action == mb::BrowserAction::none);
    OA_CHECK(browser.cards[*browser.selected].name == "check_isle@oa-check-isles");
    list = laid_out(browser, state, frame);
    result = mb::browser_key(browser, state, list, kit::Key::enter);
    OA_CHECK(result.action == mb::BrowserAction::choose);
    result = mb::browser_key(browser, state, list, kit::Key::escape);
    OA_CHECK(result.action == mb::BrowserAction::cancel);
    result = press(browser, state, list, mb::name_cancel);
    OA_CHECK(result.action == mb::BrowserAction::cancel);
    result = press(browser, state, list, mb::name_close);
    OA_CHECK(result.action == mb::BrowserAction::cancel);
}

void wheel_and_drag_scroll() {
    const kit::Frame frame = frames()[0].frame;
    mb::Browser browser = many_maps();
    mb::ScreenState state;
    kit::DisplayList list = laid_out(browser, state, frame);
    std::ignore = mb::browser_wheel(browser, state, list, -1.0F);
    OA_CHECK(state.scroll > 0);
    const int32_t wheeled = state.scroll;
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_wheel(browser, state, list, 1.0F);
    OA_CHECK(state.scroll < wheeled);
    // A finger's drag up the grid scrolls it, and selects nothing.
    state.scroll = 0;
    list = laid_out(browser, state, frame);
    const std::vector<const kit::Control*> cards = card_controls(list);
    const kit::Rect first = visible_part(cards[0]->rect, cards[0]->clip);
    const kit::Point from{first.x + first.width / 2, first.y + first.height / 2};
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::down, 1, from, 8);
    std::ignore = mb::browser_pointer(
        browser, state, list, mb::PointerKind::move, 0, {from.x, from.y - 40}, 8
    );
    const mb::ScreenResult result =
        mb::browser_pointer(browser, state, list, mb::PointerKind::up, 1, {from.x, from.y - 40}, 8);
    OA_CHECK(state.scroll == 40);
    OA_CHECK(result.action == mb::BrowserAction::none);
    OA_CHECK(!browser.selected);
    // Page Down and Home scroll it too.
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_key(browser, state, list, kit::Key::page_down);
    OA_CHECK(state.scroll > 40);
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_key(browser, state, list, kit::Key::home);
    OA_CHECK(state.scroll == 0);
}

/// Returns the grid's scroll bar.
///
/// @param list the display list
/// @return the bar's item; null while the cards fit
const kit::Item* scroll_bar(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::scroll_bar)
            return &item;
    return nullptr;
}

/// Returns the scroll bar's thumb.
///
/// @param bar the bar's item
/// @return the thumb
kit::Rect thumb_of(const kit::Item& bar) {
    const auto* look = std::get_if<kit::ScrollBarLook>(&bar.look);
    return look != nullptr ? look->thumb : kit::Rect{};
}

void scroll_bar_drags() {
    const kit::Frame frame = frames()[0].frame;
    mb::Browser browser = many_maps();
    mb::ScreenState state;
    kit::DisplayList list = laid_out(browser, state, frame);
    const kit::Item* bar = scroll_bar(list);
    OA_CHECK(bar != nullptr);
    if (bar == nullptr)
        return;
    const kit::Rect well = bar->rect;
    const kit::Rect thumb = thumb_of(*bar);
    OA_CHECK(thumb.height > 0 && thumb.y + thumb.height < well.y + well.height);
    // A mouse's press on the thumb holds the bar, and the thumb follows the
    // pointer down: the grid scrolls down, and nothing is selected.
    const kit::Point from{thumb.x + thumb.width / 2, thumb.y + thumb.height / 2};
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::down, 1, from, 0);
    OA_CHECK(state.bar_held);
    std::ignore = mb::browser_pointer(
        browser, state, list, mb::PointerKind::move, 0, {from.x, from.y + 30}, 0
    );
    OA_CHECK(state.scroll > 0);
    list = laid_out(browser, state, frame);
    bar = scroll_bar(list);
    OA_CHECK(bar != nullptr);
    if (bar == nullptr)
        return;
    // The thumb is drawn held, 30 rows lower, to the nearest row.
    const kit::Rect moved = thumb_of(*bar);
    OA_CHECK(std::abs(moved.y - (thumb.y + 30)) <= 1);
    const auto* look = std::get_if<kit::ScrollBarLook>(&bar->look);
    OA_CHECK(look != nullptr && look->hot);
    // Above the well's top the grid stops at its first row.
    std::ignore = mb::browser_pointer(
        browser, state, list, mb::PointerKind::move, 0, {from.x, well.y - 50}, 0
    );
    OA_CHECK(state.scroll == 0);
    const mb::ScreenResult released =
        mb::browser_pointer(browser, state, list, mb::PointerKind::up, 1, {from.x, well.y - 50}, 0);
    OA_CHECK(!state.bar_held);
    OA_CHECK(released.action == mb::BrowserAction::none);
    OA_CHECK(!browser.selected);
    // A press on the well at its foot puts the thumb's middle there: the
    // grid scrolls to its end, as End does.
    list = laid_out(browser, state, frame);
    const kit::Point foot{from.x, well.y + well.height - 2};
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::down, 1, foot, 0);
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::up, 1, foot, 0);
    const int32_t pressed = state.scroll;
    OA_CHECK(pressed > 0);
    state.scroll = 0;
    list = laid_out(browser, state, frame);
    std::ignore = mb::browser_key(browser, state, list, kit::Key::end);
    OA_CHECK(state.scroll == pressed);
    // A finger takes hold of the bar as the mouse does.
    list = laid_out(browser, state, frame);
    bar = scroll_bar(list);
    OA_CHECK(bar != nullptr);
    if (bar == nullptr)
        return;
    const kit::Rect at_end = thumb_of(*bar);
    const kit::Point grip{at_end.x + at_end.width / 2, at_end.y + at_end.height / 2};
    std::ignore = mb::browser_pointer(browser, state, list, mb::PointerKind::down, 1, grip, 8);
    std::ignore = mb::browser_pointer(
        browser, state, list, mb::PointerKind::move, 0, {grip.x, grip.y - 30}, 8
    );
    std::ignore =
        mb::browser_pointer(browser, state, list, mb::PointerKind::up, 1, {grip.x, grip.y - 30}, 8);
    OA_CHECK(state.scroll > 0 && state.scroll < pressed);
    OA_CHECK(!browser.selected);
}

void card_reads_the_largest_count() {
    mb::MapCard map;
    map.name = "Acid Foursome";
    map.title = "Acid Foursome";
    map.size = "13 x 13";
    map.players = 10;
    map.player_counts = {2, 4};
    map.origin = mb::MapCard::Origin::game;
    map.source_name = "TA 3.1c";
    mb::Browser browser;
    browser.cards = {map};
    browser.selected = 0;
    mb::apply_filters(browser);
    const kit::DisplayList list = laid_out(browser, {}, frames()[1].frame);
    bool subtitle = false;
    bool details = false;
    for (const kit::Item& item : list.items) {
        if (const auto* look = std::get_if<kit::CardLook>(&item.look))
            subtitle = look->title == "Acid Foursome" && look->subtitle == "4 · 13×13";
        if (item.role == kit::Role::text && item.text == "TA 3.1c · 4 players · 13×13")
            details = true;
    }
    OA_CHECK(subtitle);
    OA_CHECK(details);
}

/// At Compact a long pack name stays whole on the byline, and the fit chips
/// sit on that line inside the footer.
void compact_byline_and_chips_fit() {
    const kit::Frame frame = frames()[0].frame;
    mb::Browser browser = many_maps();
    for (std::size_t index = 0; index < browser.cards.size(); ++index) {
        if (browser.cards[index].name != "isle_of_ashes@archipelago")
            continue;
        browser.selected = index;
        browser.cards[index].pack_name = "Western Isles Pack";
        browser.cards[index].pack_version = "1.2";
        browser.cards[index].registry_name = "Example Maps";
        browser.cards[index].download_bytes = 2400000;
        browser.cards[index].base_fit = mb::MapCard::Fit::fits;
        browser.cards[index].mod_fit = mb::MapCard::Fit::unknown;
    }
    browser.mod_name = "Ridge";
    const kit::DisplayList list = laid_out(browser, {}, frame);
    const std::string byline = "Western Isles Pack · 4 players";
    bool byline_whole = false;
    kit::Rect byline_rect{};
    std::vector<kit::Rect> chip_rects;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::text && item.text == byline) {
            byline_whole = true;
            byline_rect = item.rect;
        }
        if (const auto* chip = std::get_if<kit::ChipLook>(&item.look);
            chip != nullptr && item.control == kit::no_control &&
            (chip->text == "Fits No Mod ✓" ||
             chip->text == "Fit with Ridge is checked when it's installed"))
            chip_rects.push_back(item.rect);
    }
    OA_CHECK(byline_whole);
    OA_CHECK(chip_rects.size() == 2);
    const kit::Point window = mb::window_size(frame);
    const auto inside = [&](const kit::Rect& rect) {
        return rect.width > 0 && rect.x >= 0 && rect.y >= 0 && rect.x + rect.width <= window.x &&
               rect.y + rect.height <= window.y;
    };
    OA_CHECK(inside(byline_rect));
    for (const kit::Rect& rect : chip_rects) {
        OA_CHECK(inside(rect));
        OA_CHECK(
            rect.y == byline_rect.y ||
            (rect.y >= byline_rect.y && rect.y < byline_rect.y + byline_rect.height)
        );
        OA_CHECK(rect.x >= byline_rect.x + byline_rect.width);
    }
}

void header_and_footer_words() {
    const kit::Frame frame = frames()[2].frame;
    mb::Browser browser = many_maps();
    for (std::size_t index = 0; index < browser.cards.size(); ++index)
        if (browser.cards[index].name == "isle_of_ashes@archipelago") {
            browser.selected = index;
            browser.cards[index].pack_name = "Archipelago Pack";
            browser.cards[index].pack_version = "1.2";
            browser.cards[index].registry_name = "Example Maps";
            browser.cards[index].mod_fit = mb::MapCard::Fit::unknown;
        }
    const kit::DisplayList list = laid_out(browser, {}, frame);
    bool header = false;
    bool details = false;
    std::vector<std::string> chips;
    for (const kit::Item& item : list.items) {
        if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
            header = look->title == "Maps" && look->version == "Skirmish · playing Ridge";
        if (item.role == kit::Role::text &&
            item.text == "Archipelago Pack 1.2 · Example Maps · 4 players · 16×16 · 2.3 MB")
            details = true;
        if (const auto* chip = std::get_if<kit::ChipLook>(&item.look);
            chip != nullptr && item.control == kit::no_control)
            chips.push_back(chip->text);
    }
    OA_CHECK(header);
    OA_CHECK(details);
    OA_CHECK(
        std::find(chips.begin(), chips.end(), "Fits No Mod ✓") != chips.end() &&
        std::find(chips.begin(), chips.end(), "Fit with Ridge is checked when it's installed") !=
            chips.end()
    );
    const kit::Control* select_button = named(list, mb::name_select);
    OA_CHECK(
        select_button != nullptr && select_button->enabled &&
        select_button->text == "Download and select"
    );
}

/// The chip on a card of this title, or empty when that card is not drawn.
///
/// @param list the display list
/// @param title the card's title
/// @return the chip's text
std::string card_chip(const kit::DisplayList& list, std::string_view title) {
    for (const kit::Item& item : list.items) {
        if (const auto* card = std::get_if<kit::CardLook>(&item.look);
            card != nullptr && card->title == title && !card->chips.empty())
            return card->chips.front().text;
    }
    return {};
}

/// Battle room rows: a catalogue map not installed does not say Downloaded,
/// an installed one does, and the map being downloaded says so.
void battle_room_map_rows() {
    auto made = [](std::string_view name, std::string_view title, std::string_view pack) {
        mb::MapCard card;
        card.name = std::string(name);
        card.title = std::string(title);
        card.pack_name = std::string(pack);
        card.players = 2;
        card.size = "8x8";
        return card;
    };
    const mb::MapCard installed = made("dune@dune", "Dune", "Dune");
    const mb::MapCard offered = made("tidewater@tidewater", "Tidewater", "Tidewater");
    const mb::MapCard lowlands = made("fen_crossing@lowlands", "Fen Crossing", "Lowlands");
    mb::Browser browser;
    browser.opener = mb::Browser::Opener::battle_room;
    browser.cards = mb::merge_cards({}, {}, {installed}, {offered, lowlands});
    mb::apply_filters(browser);
    const kit::Frame frame = frames()[2].frame;
    const kit::DisplayList list = laid_out(browser, {}, frame);
    bool header = false;
    for (const kit::Item& item : list.items)
        if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
            header = look->title == "Maps" && look->version == "Battle room · No Mod";
    OA_CHECK(header);
    OA_CHECK(card_chip(list, "Dune") == "Downloaded");
    OA_CHECK(card_chip(list, "Tidewater") == "Available");
    OA_CHECK(card_chip(list, "Tidewater") != "Downloaded");
    OA_CHECK(card_chip(list, "Fen Crossing") == "Lowlands");
    browser.downloading = "tidewater@tidewater";
    const kit::DisplayList fetching = laid_out(browser, {}, frame);
    OA_CHECK(card_chip(fetching, "Tidewater") == "Downloading…");
    OA_CHECK(card_chip(fetching, "Dune") == "Downloaded");
    OA_CHECK(card_chip(fetching, "Fen Crossing") == "Lowlands");
}

/// Draws a line in the modern fonts, as the game does for the characters
/// its fonts lack.
///
/// @param context the font stack
/// @param text the line, in UTF-8
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @return the line's mask; null when the fonts cannot draw it
std::shared_ptr<const oa::present::TextMask> draw_modern(
    void* context, std::string_view text, oa::present::TextFace face, int32_t scale, int32_t size
) {
    auto& stack = *static_cast<text_font::FontStack*>(context);
    if (text.empty() || text.size() > text_font::max_text_bytes)
        return nullptr;
    const bool label = face == oa::present::TextFace::label;
    const int32_t face_pixels =
        face == oa::present::TextFace::message  ? text_font::message_log_pixel_size
        : face == oa::present::TextFace::status ? text_font::status_readout_pixel_size
                                                : text_font::label_pixel_size;
    text_font::Style style;
    style.pixel_size = std::clamp(
        oa::present::face_pixel_size(face_pixels, scale, size),
        int32_t{1},
        text_font::max_pixel_size
    );
    style.weight = label ? text_font::Weight::regular : text_font::Weight::bold;
    style.rendering = text_font::Rendering::mono;
    const auto drawn = stack.draw(text, style);
    const auto placements = stack.layout(text, style);
    const auto characters = text_font::decode_utf8(text);
    if (!drawn || !placements || !characters)
        return nullptr;
    auto mask = std::make_shared<oa::present::TextMask>();
    mask->width = drawn->width;
    mask->height = drawn->height;
    mask->baseline = drawn->baseline;
    mask->origin = drawn->origin;
    mask->advance = drawn->advance;
    mask->alpha = drawn->alpha;
    std::size_t placed = 0;
    int32_t pen = 0;
    for (const char32_t character : *characters) {
        if (!text_font::is_invisible(character) && placed < placements->size()) {
            const auto& placement = (*placements)[placed++];
            pen = placement.pen + placement.advance;
        }
        mask->character_ends.push_back(pen);
    }
    return mask;
}

/// A browser whose first rows show a map from each place: the base game,
/// two expansions, a mod, an installed pack, a pack offered online, and a
/// download whose pack is named for the map.
///
/// @return the browser, one online map selected
mb::Browser picture_browser() {
    auto made =
        [](std::string_view name, std::string_view title, int32_t players, std::string_view size) {
            mb::MapCard card;
            card.name = std::string(name);
            card.title = std::string(title);
            card.players = players;
            card.size = std::string(size);
            return card;
        };
    std::vector<mb::MapCard> game{
        made("Acid Foursome", "Acid Foursome", 4, "13 x 13"),
        made("Coast To Coast", "Coast to Coast", 2, "20 x 20"),
        made("Big Wars", "Big Wars", 8, "32 x 32"),
    };
    game[0].source_name = "TA 3.1c";
    game[1].source_name = "Core Contingency";
    game[2].source_name = "Battle Tactics";
    std::vector<mb::MapCard> mod{made("ridge_valley", "Ridge Valley", 3, "14 x 14")};
    mod[0].source_name = "Ridge";
    std::vector<mb::MapCard> installed{
        made("tidewater@lowlands", "Tidewater", 4, "24x24"),
        made("dune", "Dune", 2, "10x10"),
    };
    installed[0].pack_name = "Lowlands";
    installed[1].pack_name = "Dune";
    std::vector<mb::MapCard> online{
        made("isle_of_ashes@archipelago", "Isle of Ashes", 4, "16x16"),
        made("black_shoals@archipelago", "Black Shoals", 6, "12x12"),
    };
    online[0].pack_name = "Western Isles Pack";
    online[0].pack_version = "1.2";
    online[0].registry_name = "Example Maps";
    online[0].download_bytes = 2400000;
    online[1].pack_name = "Archipelago Pack";
    mb::Browser browser;
    browser.cards =
        mb::merge_cards(std::move(game), std::move(mod), std::move(installed), std::move(online));
    browser.mod_name = "Ridge";
    mb::apply_filters(browser);
    for (std::size_t index = 0; index < browser.cards.size(); ++index)
        if (browser.cards[index].name == "isle_of_ashes@archipelago")
            browser.selected = index;
    return browser;
}

/// Writes one size class's browser as a PNG of the dialog.
///
/// @param directory where the file goes
/// @param file_name the file's name
/// @param frame the frame
/// @param fonts the fonts, the modern look while they hold Barlow
/// @return true when the file was written
bool write_picture(
    const std::filesystem::path& directory,
    std::string_view file_name,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    const mb::Browser browser = picture_browser();
    const kit::DisplayList list = mb::browser_layout(browser, {}, frame, fonts, {});
    const kit::Point window = mb::window_size(frame);
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(window.x);
    surface.height = static_cast<uint32_t>(window.y);
    surface.rgb.assign(
        static_cast<std::size_t>(window.x) * static_cast<std::size_t>(window.y) * 3,
        static_cast<uint8_t>(0)
    );
    kit::Canvas canvas;
    canvas.surface = &surface;
    canvas.placement = {0, 0, 1, {}};
    canvas.fonts = &fonts;
    mb::draw_browser(canvas, list, window);
    png::Header header;
    header.width = surface.width;
    header.height = surface.height;
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, surface.rgb}, &file))
        return false;
    const std::filesystem::path path = directory / std::string(file_name);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

/// Writes the three size classes and returns 0, or 1 when a picture failed.
/// A missing game directory ends the process as a skipped test.
///
/// @param directory where the pictures go
/// @return 0 when all three were written
int write_pictures(const std::filesystem::path& directory) {
    oa::AssetStore assets = oa::test::require_game_assets("the map browser's pictures");
    const kit::Fonts fonts = kit::load_game_fonts(assets);
    const std::unique_ptr<text_font::FontStack> modern =
        text_font::FontStack::open(text_font::bundled_font_directory());
    if (modern) {
        oa::present::GameTextHooks hooks;
        hooks.context = modern.get();
        hooks.draw = draw_modern;
        oa::present::set_game_text_hooks(hooks);
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::fprintf(stderr, "map browser: cannot make %s\n", directory.string().c_str());
        oa::present::set_game_text_hooks({});
        return 1;
    }
    const std::array<std::pair<std::string_view, kit::Frame>, 3> shots{{
        {"map-browser-compact.png", frames()[0].frame},
        {"map-browser-regular.png", frames()[1].frame},
        {"map-browser-large.png", frames()[2].frame},
    }};
    int failed = 0;
    for (const auto& [file_name, frame] : shots) {
        kit::Fonts shown = fonts;
        if (modern) {
            shown.modern = modern.get();
            shown.size_class = frame.size_class;
        }
        if (!write_picture(directory, file_name, frame, shown)) {
            std::fprintf(
                stderr,
                "map browser: cannot write %.*s\n",
                static_cast<int>(file_name.size()),
                file_name.data()
            );
            ++failed;
        }
    }
    oa::present::set_game_text_hooks({});
    return failed == 0 ? 0 : 1;
}

} // namespace

int write_map_browser_frames(const char* directory) {
    if (directory == nullptr || directory[0] == '\0')
        return 1;
    return write_pictures(std::filesystem::path(directory));
}

void layout_tests() {
    columns_at_each_class();
    nothing_outside_the_window();
    names_and_buttons();
    pictures_asked_for_the_view_alone();
    focus_moves_by_geometry();
    tab_follows_the_declared_order();
    pointer_and_keys();
    wheel_and_drag_scroll();
    scroll_bar_drags();
    card_reads_the_largest_count();
    header_and_footer_words();
    compact_byline_and_chips_fit();
    battle_room_map_rows();
}
