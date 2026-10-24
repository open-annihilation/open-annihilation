// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker's screen over made-up brains, laid out with a synthetic
// font at Compact, Regular and Large, in the windows 640x480 (1x),
// 1920x1080 (2x) and 2560x1440 (2x). At each frame: every part lies inside
// the window; no two controls meet, apart from the list and its rows; every
// row is a list item; the chosen row carries the check mark; locked rows
// are locked; the level strip's parts are the level ids; every option is a
// control of its form; Compact has no details pane, and DETAILS opens the
// page, which has BACK. Tab follows the declared order. The names are sound
// and unique. Every text fits, and no button is cut, with a French lookup
// and with long German.
//
// The synthetic font is brain_fixture::fonts.

#include "oa/test/check.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::brain_picker;
namespace lib = oa::ui::library;
using brain_fixture::fonts;
using brain_fixture::has_room;
using brain_fixture::opened;
using brain_fixture::opened_crowd;
using brain_fixture::opened_empty;
using brain_fixture::opened_left_out;
using brain_fixture::opened_note;
using brain_fixture::outside_window;
using brain_fixture::press_area;
using brain_fixture::texts_not_fitting;

/// Prints each problem a check found, and fails the check when there are any.
///
/// @param problems the problems
/// @param where the frame or case they belong to
/// @param file the check's file
/// @param line the check's line
void expect_none(
    const std::vector<std::string>& problems, const std::string& where, const char* file, int line
) {
    for (const std::string& problem : problems)
        std::fprintf(stderr, "%s:%d: %s: %s\n", file, line, where.c_str(), problem.c_str());
    if (!problems.empty())
        ++oa::test::failed_checks();
}

#define OA_EXPECT_NONE(problems, where) expect_none((problems), (where), __FILE__, __LINE__)

/// Tells whether a name starts with a prefix.
///
/// @param name the name
/// @param prefix the prefix
/// @return true when it does
bool starts(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}

/// Returns a list's control of a name, or null.
///
/// @param list the display list
/// @param name the name
/// @return the control
const kit::Control* find_named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// Tells whether a control is an open menu's item. Those may lie over the
/// controls under the menu.
///
/// @param control the control
/// @return true for a menu item
bool menu_item(const kit::Control& control) {
    if (control.kind != kit::ControlKind::list_item)
        return false;
    if (starts(control.name, picker::name_row_prefix))
        return false;
    return starts(control.name, picker::name_option_prefix);
}

/// Tells whether two controls may occupy the same points: the list and a row.
///
/// @param outer one control
/// @param inner the other
/// @return true when one is the list and the other is a row
bool row_in_list(const kit::Control& outer, const kit::Control& inner) {
    if (outer.name == picker::name_list && starts(inner.name, picker::name_row_prefix))
        return true;
    if (inner.name == picker::name_list && starts(outer.name, picker::name_row_prefix))
        return true;
    return false;
}

/// Lists the pairs of controls whose press areas meet.
///
/// @param list the display list
/// @return a line for each pair
std::vector<std::string> meeting_controls(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (std::size_t first_at = 0; first_at < list.controls.size(); ++first_at) {
        for (std::size_t second_at = first_at + 1; second_at < list.controls.size(); ++second_at) {
            const kit::Control& first = list.controls[first_at];
            const kit::Control& second = list.controls[second_at];
            if (menu_item(first) || menu_item(second))
                continue;
            if (row_in_list(first, second))
                continue;
            if (has_room(kit::intersect(press_area(first), press_area(second))))
                problems.push_back(first.name + " meets " + second.name);
        }
    }
    return problems;
}

/// The design's French, for this test only.
///
/// @param context unused
/// @param english the English word
/// @return the French, or the English when this test leaves it
std::string_view french_shown(void* /*context*/, std::string_view english) {
    using picker::words::back;
    using picker::words::cancel;
    using picker::words::close_button;
    using picker::words::context_seat;
    using picker::words::details;
    using picker::words::empty_brains;
    using picker::words::level;
    using picker::words::no_mod;
    using picker::words::switch_off;
    using picker::words::switch_on;
    using picker::words::title_player;
    using picker::words::use_every_seat;
    using picker::words::use_this;
    if (english == title_player)
        return "Joueur ordinateur";
    if (english == context_seat)
        return "{seat} · {mod}";
    if (english == no_mod)
        return "Sans mod";
    if (english == level)
        return "Niveau";
    if (english == use_every_seat)
        return "Pour chaque siège ordinateur";
    if (english == switch_off)
        return "NON";
    if (english == switch_on)
        return "OUI";
    if (english == details)
        return "DÉTAILS";
    if (english == cancel)
        return "ANNULER";
    if (english == use_this)
        return "UTILISER";
    if (english == back)
        return "RETOUR";
    if (english == close_button)
        return "FERMER";
    if (english == empty_brains)
        return "Pas encore de cerveaux ici. La Bibliothèque du menu principal les récupère.";
    return english;
}

/// Long German button words, long enough to grow a button past the kit's
/// button width. The switch halves stay short, so each half still fits.
///
/// @param context unused
/// @param english the English word
/// @return the German, or the English when this test leaves it
std::string_view german_shown(void* /*context*/, std::string_view english) {
    using picker::words::back;
    using picker::words::cancel;
    using picker::words::close_button;
    using picker::words::details;
    using picker::words::switch_off;
    using picker::words::switch_on;
    using picker::words::use_this;
    if (english == use_this)
        return "ÜBERNEHMEN UND LOS";
    if (english == details)
        return "EINZELHEITEN ANZEIGEN";
    if (english == cancel)
        return "ABBRECHEN UND ZURÜCK";
    if (english == back)
        return "ZURÜCK ZUR LISTE";
    if (english == close_button)
        return "SCHLIESSEN UND GEHEN";
    if (english == switch_off)
        return "AUS";
    if (english == switch_on)
        return "AN";
    return english;
}

/// Lays the picker out in the synthetic font.
///
/// @param model the picker
/// @param state the screen's state
/// @param frame the frame
/// @return the display list
kit::DisplayList
laid_out(const picker::Picker& model, const picker::ScreenState& state, const kit::Frame& frame) {
    return picker::picker_layout(model, state, frame, fonts());
}

/// One window the screen is checked in.
struct FrameCase {
    const char* name{};  ///< the case
    kit::Frame frame{};  ///< the frame
    kit::Point window{}; ///< the window's size
    int32_t width{};     ///< the dialog width this class names
    int32_t height{};    ///< the dialog height this class names
};

/// Returns one window frame.
///
/// @param width the canvas's width, in pixels
/// @param height the canvas's height, in pixels
/// @param percent the scale, as a percentage
/// @return the frame
kit::Frame window_frame(int32_t width, int32_t height, int32_t percent) {
    kit::Viewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.scale_percent = percent;
    return kit::frame_of(viewport);
}

/// The three windows.
///
/// @return the frames
std::vector<FrameCase> frames() {
    const kit::Frame narrow = window_frame(640, 480, 100);
    const kit::Frame desktop = window_frame(1920, 1080, 200);
    const kit::Frame wide = window_frame(2560, 1440, 200);
    return {
        {"640x480", narrow, picker::window_size(narrow), 480, 324},
        {"1920x1080", desktop, picker::window_size(desktop), 720, 486},
        {"2560x1440", wide, picker::window_size(wide), 960, 600},
    };
}

/// Tells whether a control can take Tab.
///
/// @param control the control
/// @return true when it is enabled and focusable
bool tabbable(const kit::Control& control) {
    return control.enabled && control.focusable;
}

/// Appends a control's name when it is present and can take Tab.
///
/// @param[in,out] names the names so far
/// @param list the display list
/// @param name the name
void append_tab(
    std::vector<std::string>& names, const kit::DisplayList& list, std::string_view name
) {
    const kit::Control* control = find_named(list, name);
    if (control != nullptr && tabbable(*control))
        names.push_back(control->name);
}

/// Tab's names, in order.
///
/// @param list the display list
/// @return the names
std::vector<std::string> tab_names(const kit::DisplayList& list) {
    std::vector<std::string> names;
    for (const kit::ControlId id : list.tab_order) {
        const kit::Control* control = kit::control_of(list, id);
        names.push_back(control != nullptr ? control->name : std::string("?"));
    }
    return names;
}

/// Checks Tab's order against the controls that can take it.
///
/// The close mark stays on Compact's details page, so a pointer press on it
/// is a key press too.
///
/// @param list the display list
/// @param frame the frame
/// @param page Compact's details page
/// @param where the case
void check_tabs(
    const kit::DisplayList& list, const kit::Frame& frame, bool page, const std::string& where
) {
    const bool compact = frame.size_class == kit::SizeClass::compact;
    std::vector<std::string> expected;
    if (!page)
        append_tab(expected, list, picker::name_list);
    if (!compact || page)
        append_tab(expected, list, picker::name_level);
    for (const kit::Control& control : list.controls) {
        if (starts(control.name, picker::name_option_prefix) && tabbable(control) &&
            control.kind != kit::ControlKind::list_item)
            expected.push_back(control.name);
    }
    append_tab(expected, list, picker::name_every_seat);
    if (page)
        append_tab(expected, list, picker::name_back);
    else if (compact)
        append_tab(expected, list, picker::name_details);
    if (!page)
        append_tab(expected, list, picker::name_cancel);
    append_tab(expected, list, picker::name_use);
    append_tab(expected, list, picker::name_close);

    const std::vector<std::string> got = tab_names(list);
    if (got != expected) {
        std::fprintf(stderr, "%s: tab order\n", where.c_str());
        for (const std::string& name : got)
            std::fprintf(stderr, "  got %s\n", name.c_str());
        for (const std::string& name : expected)
            std::fprintf(stderr, "  want %s\n", name.c_str());
    }
    OA_CHECK(got == expected);
    for (const std::string& name : got) {
        OA_CHECK(name != picker::name_hint);
        OA_CHECK(name != picker::name_summary);
        OA_CHECK(name != picker::name_brain);
        OA_CHECK(name != picker::name_list_bar);
        OA_CHECK(name != picker::name_details_bar);
        OA_CHECK(!starts(name, picker::name_row_prefix));
    }
}

/// The button captions that contain the ellipsis.
///
/// @param list the display list
/// @return a line for each one
std::vector<std::string> cut_buttons(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const auto* look = std::get_if<kit::ButtonLook>(&item.look);
        if (item.role == kit::Role::button && look != nullptr &&
            look->caption.find(kit::ellipsis) != std::string::npos)
            problems.push_back(look->caption);
    }
    return problems;
}

/// A row's automation name.
///
/// @param id the brain's id; empty is the built-in player
/// @return the name
std::string row_name(std::string_view id) {
    if (id.empty())
        return std::string(picker::name_row_prefix) + "built-in";
    return std::string(picker::name_row_prefix) + std::string(id);
}

/// Tells whether an item is the chosen row's check mark.
///
/// @param item the item
/// @return true for the circled check
bool check_mark(const kit::Item& item) {
    const auto* look = std::get_if<kit::ChipLook>(&item.look);
    return item.role == kit::Role::chip && look != nullptr && look->icon &&
           *look->icon == kit::StateIcon::circled_check;
}

/// Checks one frame of the fixture: the geometry, the rows, the options, the
/// names and the texts.
///
/// @param model the picker
/// @param where the case's name
/// @param frame the frame
/// @param window the window's size
/// @param want_summary true when the footer has room for the summary
void check_frame(
    const picker::Picker& model,
    const std::string& where,
    const kit::Frame& frame,
    kit::Point window,
    bool want_summary
) {
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const int32_t row_height = compact ? picker::compact_row_height
                               : frame.size_class == kit::SizeClass::large
                                   ? picker::large_row_height
                                   : picker::regular_row_height;

    OA_EXPECT_NONE(outside_window(list, window), where);
    OA_EXPECT_NONE(meeting_controls(list), where);
    OA_EXPECT_NONE(texts_not_fitting(list, fonts()), where);
    OA_EXPECT_NONE(cut_buttons(list), where);
    const std::string names = kit::name_problem(list);
    if (!names.empty())
        std::fprintf(stderr, "%s: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    check_tabs(list, frame, false, where);

    const kit::Control* listed = find_named(list, picker::name_list);
    OA_CHECK(listed != nullptr);
    if (listed != nullptr) {
        const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
        const int32_t list_width = compact ? window.x - metrics.edge - 2 * metrics.padding
                                   : frame.size_class == kit::SizeClass::large
                                       ? picker::large_list_width
                                       : picker::regular_list_width;
        OA_CHECK(listed->rect.width == list_width);
        OA_CHECK(listed->kind == kit::ControlKind::list);
    }

    bool marked = false;
    bool locked = false;
    const std::vector<picker::Row> shown = picker::rows(model);
    for (const picker::Row& row : shown) {
        const kit::Control* row_control = find_named(list, row_name(row.id));
        OA_CHECK(row_control != nullptr);
        if (row_control == nullptr)
            continue;
        OA_CHECK(row_control->kind == kit::ControlKind::list_item);
        OA_CHECK(row_control->text == row.name);
        OA_CHECK(row_control->rect.height == row_height);
        OA_CHECK(!row_control->focusable);
        OA_CHECK(row_control->checked == (model.selected == row.id));
        if (row.chosen) {
            for (const kit::Item& item : list.items) {
                if (!check_mark(item))
                    continue;
                if (kit::wholly_in(item.rect, row_control->rect))
                    marked = true;
            }
        }
        if (row.locked) {
            for (const kit::Item& item : list.items) {
                if (item.role == kit::Role::locked_fade &&
                    has_room(kit::intersect(item.rect, row_control->rect)))
                    locked = true;
            }
        }
    }
    OA_CHECK(marked);
    OA_CHECK(locked);

    const kit::Control* level = find_named(list, picker::name_level);
    const kit::Control* aggression = find_named(list, "brains.option.aggression");
    const kit::Control* signal = find_named(list, "brains.option.signal");
    const kit::Control* hold = find_named(list, "brains.option.hold");
    const kit::Control* focus = find_named(list, "brains.option.focus");
    const kit::Control* every = find_named(list, picker::name_every_seat);
    if (compact) {
        OA_CHECK(level == nullptr);
        OA_CHECK(aggression == nullptr);
        OA_CHECK(signal == nullptr);
        OA_CHECK(hold == nullptr);
        OA_CHECK(focus == nullptr);
        OA_CHECK(every == nullptr);
        OA_CHECK(find_named(list, picker::name_details) != nullptr);
        OA_CHECK(find_named(list, picker::name_back) == nullptr);
        OA_CHECK(find_named(list, picker::name_brain) == nullptr);
    } else {
        OA_CHECK(level != nullptr && level->kind == kit::ControlKind::levels);
        OA_CHECK(level != nullptr && level->enabled && level->focusable);
        if (level != nullptr) {
            OA_CHECK(level->parts.size() == 3);
            if (level->parts.size() == 3) {
                OA_CHECK(level->parts[0] == "gentle");
                OA_CHECK(level->parts[1] == "firm");
                OA_CHECK(level->parts[2] == "fierce");
            }
        }
        OA_CHECK(aggression != nullptr && aggression->kind == kit::ControlKind::levels);
        OA_CHECK(signal != nullptr && signal->kind == kit::ControlKind::choice);
        OA_CHECK(hold != nullptr && hold->kind == kit::ControlKind::toggle);
        OA_CHECK(focus != nullptr && focus->kind == kit::ControlKind::slider);
        OA_CHECK(every != nullptr && every->kind == kit::ControlKind::toggle);
        OA_CHECK(aggression != nullptr && aggression->enabled && aggression->focusable);
        OA_CHECK(find_named(list, picker::name_details) == nullptr);
        OA_CHECK(find_named(list, picker::name_brain) != nullptr);
        bool badge = false;
        bool awareness = false;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::package_badge &&
                item.rect.width == picker::details_badge_side)
                badge = true;
            const auto* chip = std::get_if<kit::ChipLook>(&item.look);
            if (item.role == kit::Role::chip && chip != nullptr &&
                chip->state == kit::ChipState::update)
                awareness = true;
        }
        OA_CHECK(badge);
        OA_CHECK(awareness);
    }
    OA_CHECK(find_named(list, picker::name_cancel) != nullptr);
    OA_CHECK(find_named(list, picker::name_use) != nullptr);
    OA_CHECK(find_named(list, picker::name_close) != nullptr);
    if (want_summary)
        OA_CHECK(find_named(list, picker::name_summary) != nullptr);

    picker::ScreenState page_state;
    page_state.details_page = true;
    const kit::DisplayList page = laid_out(model, page_state, frame);
    if (compact) {
        OA_EXPECT_NONE(outside_window(page, window), where + " page");
        OA_EXPECT_NONE(meeting_controls(page), where + " page");
        OA_EXPECT_NONE(texts_not_fitting(page, fonts()), where + " page");
        OA_EXPECT_NONE(cut_buttons(page), where + " page");
        const std::string page_names = kit::name_problem(page);
        if (!page_names.empty())
            std::fprintf(stderr, "%s page: %s\n", where.c_str(), page_names.c_str());
        OA_CHECK(page_names.empty());
        OA_CHECK(find_named(page, picker::name_list) == nullptr);
        OA_CHECK(find_named(page, picker::name_details) == nullptr);
        OA_CHECK(find_named(page, picker::name_back) != nullptr);
        OA_CHECK(find_named(page, picker::name_use) != nullptr);
        OA_CHECK(find_named(page, picker::name_level) != nullptr);
        check_tabs(page, frame, true, where + " page");
    }
}

/// Checks the open drop-down: its items stay in the window and are not in
/// the meeting check.
///
/// @param model the picker
/// @param where the case
/// @param frame the frame
/// @param window the window
void check_menu(
    const picker::Picker& model,
    const std::string& where,
    const kit::Frame& frame,
    kit::Point window
) {
    if (frame.size_class == kit::SizeClass::compact)
        return;
    picker::ScreenState state;
    state.menu = picker::Menu::choice;
    state.menu_option = "signal";
    const kit::DisplayList list = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(list, window), where + " menu");
    OA_EXPECT_NONE(meeting_controls(list), where + " menu");
    const std::string names = kit::name_problem(list);
    if (!names.empty())
        std::fprintf(stderr, "%s menu: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    int items = 0;
    for (const kit::Control& control : list.controls) {
        if (menu_item(control))
            ++items;
    }
    OA_CHECK(items == 6);
}

/// Checks a crowd's list scroll bar, the empty sentence, the Difficulty
/// note and the hint.
///
/// @param where the case
/// @param frame the frame
/// @param window the window
void check_states(const std::string& where, const kit::Frame& frame, kit::Point window) {
    const picker::Picker crowd = opened_crowd({});
    const kit::DisplayList crowded = laid_out(crowd, {}, frame);
    OA_EXPECT_NONE(outside_window(crowded, window), where + " crowd");
    OA_CHECK(find_named(crowded, picker::name_list_bar) != nullptr);

    const picker::Picker empty = opened_empty({});
    const kit::DisplayList bare = laid_out(empty, {}, frame);
    OA_EXPECT_NONE(outside_window(bare, window), where + " empty");
    bool sentence = false;
    for (const kit::Item& item : bare.items) {
        if (item.role == kit::Role::text &&
            item.text.find("No brains here yet") != std::string::npos)
            sentence = true;
    }
    OA_CHECK(sentence);
    OA_CHECK(find_named(bare, row_name("")) != nullptr);

    const picker::Picker noted = opened_note({});
    const kit::DisplayList note_list = laid_out(noted, {}, frame);
    OA_CHECK(find_named(note_list, picker::name_level) == nullptr);
    if (frame.size_class != kit::SizeClass::compact) {
        bool note = false;
        for (const kit::Item& item : note_list.items) {
            if (item.role == kit::Role::text && item.text.find("Difficulty") != std::string::npos)
                note = true;
        }
        OA_CHECK(note);
    }

    const picker::Picker left = opened_left_out({});
    const kit::DisplayList hinted = laid_out(left, {}, frame);
    const kit::Control* hint = find_named(hinted, picker::name_hint);
    OA_CHECK(hint != nullptr);
    if (hint != nullptr)
        OA_CHECK(hint->text.find("cannot play") != std::string::npos);
    bool lock_ink = false;
    for (const kit::Item& item : hinted.items) {
        if (item.role == kit::Role::text && item.colour == kit::colour::lock &&
            item.text.find("cannot play") != std::string::npos)
            lock_ink = true;
    }
    OA_CHECK(lock_ink);
    OA_EXPECT_NONE(meeting_controls(hinted), where + " hint");
}

} // namespace

int main() {
    const picker::Picker model = opened({});
    lib::TextHooks french_text;
    french_text.shown = french_shown;
    lib::TextHooks german_text;
    german_text.shown = german_shown;
    const picker::Picker french = opened(french_text);
    const picker::Picker german = opened(german_text);
    for (const FrameCase& size : frames()) {
        OA_CHECK(size.frame.size_class == kit::SizeClass::compact || size.window.x == size.width);
        OA_CHECK(size.window.x == size.width);
        OA_CHECK(size.window.y == size.height);
        if (size.name == std::string_view{"640x480"})
            OA_CHECK(size.frame.size_class == kit::SizeClass::compact);
        if (size.name == std::string_view{"1920x1080"})
            OA_CHECK(size.frame.size_class == kit::SizeClass::regular);
        if (size.name == std::string_view{"2560x1440"})
            OA_CHECK(size.frame.size_class == kit::SizeClass::large);
        check_frame(model, size.name, size.frame, size.window, true);
        check_frame(french, std::string(size.name) + " fr", size.frame, size.window, true);
        check_frame(german, std::string(size.name) + " de", size.frame, size.window, false);
        check_menu(model, size.name, size.frame, size.window);
        check_states(size.name, size.frame, size.window);
    }
    return oa::test::check_exit_status();
}
