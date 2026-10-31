// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's screen over a made-up set, laid out with a synthetic
// font at Compact, Regular and Large, and at the windows of 640x480 (1x),
// 1920x1080 (2x) and 2560x1440 (2x). At each frame: every part lies inside
// the window; no two controls meet, apart from the list and its rows and a
// row and its grip and switch; every row, Off rows included, has a kit grip
// and a switch; On rows are numbered 1 to n; the headings are present; every
// option is a control of its type; ▲ and ▼ are disabled at a band's ends;
// Compact has no details pane, and DETAILS opens the page, which has BACK.
// Tab follows the declared order, and no grip or label is in it. The names
// are sound and unique. Every text fits, and no button is cut, with a French
// lookup and with long German. A drag draws its row, its well and its line,
// and a refused drop draws no line. Read-only locks the switches, disables
// the grips and shows CLOSE alone. Both empty lists are drawn. waiting
// disables USE THESE and is the summary. The window takes Settings' sizes:
// Kit 2's header and footer, the buttons at the control height in the
// regular font, the filter pills at Kit 2's filter chip height, and Tags,
// the search and the details' controls at the control height.
//
// The synthetic font is picker_fixture::fonts.

#include "oa/test/check.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/mutator_picker/text.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace lib = oa::ui::library;
using picker_fixture::fonts;
using picker_fixture::has_room;
using picker_fixture::made;
using picker_fixture::opened;
using picker_fixture::outside_window;
using picker_fixture::press_area;
using picker_fixture::same_rect;
using picker_fixture::texts_not_fitting;

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
    return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
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
/// controls under the menu. A row is not one of them.
///
/// @param control the control
/// @return true for a menu item
bool menu_item(const kit::Control& control) {
    if (control.kind != kit::ControlKind::list_item)
        return false;
    if (starts(control.name, picker::name_row_prefix))
        return false;
    return starts(control.name, std::string(picker::name_filter) + ".") ||
           starts(control.name, std::string(picker::name_tags) + ".") ||
           starts(control.name, picker::name_option_prefix);
}

/// Tells whether two controls may occupy the same points: the list and its
/// hint or its rows, or a row and its own grip and switch.
///
/// @param outer one control
/// @param inner the other
/// @return true when the pair is one of those
bool held_inside(const kit::Control& outer, const kit::Control& inner) {
    if (outer.name == picker::name_list &&
        (inner.name == picker::name_hint || starts(inner.name, picker::name_row_prefix)))
        return true;
    if (!starts(outer.name, picker::name_row_prefix))
        return false;
    if (outer.name.size() >= picker::name_grip_suffix.size() &&
        outer.name.ends_with(picker::name_grip_suffix))
        return false;
    if (outer.name.size() >= picker::name_switch_suffix.size() &&
        outer.name.ends_with(picker::name_switch_suffix))
        return false;
    return inner.name == outer.name + std::string(picker::name_grip_suffix) ||
           inner.name == outer.name + std::string(picker::name_switch_suffix);
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
            if (held_inside(first, second) || held_inside(second, first))
                continue;
            if (has_room(kit::intersect(press_area(first), press_area(second))))
                problems.push_back(first.name + " meets " + second.name);
        }
    }
    return problems;
}

/// The design's French, for this test only. The Off heading and the switch
/// use different words.
///
/// @param context unused
/// @param english the English word
/// @return the French, or the English when this test leaves it
std::string_view french_shown(void* /*context*/, std::string_view english) {
    using picker::words::back_button;
    using picker::words::cancel;
    using picker::words::details_button;
    using picker::words::filter_all;
    using picker::words::heading_off;
    using picker::words::heading_on;
    using picker::words::no_mod;
    using picker::words::search_mutators;
    using picker::words::setup_skirmish;
    using picker::words::switch_off;
    using picker::words::switch_on;
    using picker::words::title_mutators_header;
    using picker::words::use_these;
    if (english == title_mutators_header)
        return "MUTATEURS";
    if (english == setup_skirmish)
        return "Escarmouche";
    if (english == no_mod)
        return "Sans mod";
    if (english == filter_all)
        return "Tous";
    if (english == search_mutators)
        return "Rechercher des mutateurs";
    if (english == heading_on)
        return "Actifs · appliqués de haut en bas";
    if (english == heading_off)
        return "Inactifs · non appliqués";
    if (english == switch_off)
        return "Non";
    if (english == switch_on)
        return "Oui";
    if (english == details_button)
        return "Détails";
    if (english == cancel)
        return "Annuler";
    if (english == use_these)
        return "Valider";
    if (english == back_button)
        return "Retour";
    return english;
}

/// Long German button words, long enough to grow a button past the kit's
/// button width, and the long On heading. The switch halves stay short.
///
/// @param context unused
/// @param english the English word
/// @return the German, or the English when this test leaves it
std::string_view german_shown(void* /*context*/, std::string_view english) {
    using picker::words::back_button;
    using picker::words::cancel;
    using picker::words::close_button;
    using picker::words::details_button;
    using picker::words::heading_off;
    using picker::words::heading_on;
    using picker::words::switch_off;
    using picker::words::switch_on;
    using picker::words::use_these;
    if (english == use_these)
        return "ÜBERNEHMEN UND LOS";
    if (english == details_button)
        return "EINZELHEITEN ANZEIGEN";
    if (english == cancel)
        return "ABBRECHEN UND ZURÜCK";
    if (english == back_button)
        return "ZURÜCK ZUR LISTE";
    if (english == close_button)
        return "SCHLIESSEN UND GEHEN";
    if (english == heading_on)
        return "Aktiv · von oben nach unten angewandt";
    if (english == heading_off)
        return "AUS · NICHT ANGEWANDT";
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

/// The frames the screen is checked at, and the window each one gives.
struct FrameCase {
    const char* name{};  ///< the case
    kit::Frame frame{};  ///< the frame
    kit::Point window{}; ///< the window's size
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

/// The six frames.
///
/// @return the frames
std::vector<FrameCase> frames() {
    const kit::Frame compact_frame{{0, 0, 480, 324}, kit::SizeClass::compact, 100};
    const kit::Frame regular_frame{{0, 0, 720, 486}, kit::SizeClass::regular, 100};
    const kit::Frame large_frame{{0, 0, 960, 600}, kit::SizeClass::large, 100};
    const kit::Frame narrow = window_frame(640, 480, 100);
    const kit::Frame desktop = window_frame(1920, 1080, 200);
    const kit::Frame wide = window_frame(2560, 1440, 200);
    return {
        {"compact", compact_frame, picker::window_size(compact_frame)},
        {"regular", regular_frame, picker::window_size(regular_frame)},
        {"large", large_frame, picker::window_size(large_frame)},
        {"640x480", narrow, picker::window_size(narrow)},
        {"1920x1080", desktop, picker::window_size(desktop)},
        {"2560x1440", wide, picker::window_size(wide)},
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

/// Tells whether Tab stops on a name.
///
/// @param list the display list
/// @param name the name
/// @return true when that control is in Tab's order
bool in_tab(const kit::DisplayList& list, std::string_view name) {
    for (const std::string& one : tab_names(list)) {
        if (one == name)
            return true;
    }
    return false;
}

/// Checks Tab's order against the controls that can take it.
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
    if (!page && compact) {
        append_tab(expected, list, picker::name_filter);
    } else if (!page) {
        append_tab(expected, list, picker::name_filter_all);
        append_tab(expected, list, picker::name_filter_installed);
        append_tab(expected, list, picker::name_filter_downloads);
        for (const kit::Control& control : list.controls) {
            if (control.kind == kit::ControlKind::button &&
                starts(control.name, std::string(picker::name_tags) + ".") && tabbable(control))
                expected.push_back(control.name);
        }
        append_tab(expected, list, picker::name_tags);
    }
    if (!page) {
        append_tab(expected, list, picker::name_search);
        append_tab(expected, list, picker::name_list);
    }
    for (const kit::Control& control : list.controls) {
        if (starts(control.name, picker::name_option_prefix) && tabbable(control))
            expected.push_back(control.name);
    }
    append_tab(expected, list, picker::name_clash);
    append_tab(expected, list, picker::name_up);
    append_tab(expected, list, picker::name_down);
    if (page)
        append_tab(expected, list, picker::name_back);
    else if (compact)
        append_tab(expected, list, picker::name_details);
    append_tab(expected, list, picker::name_cancel);
    append_tab(expected, list, picker::name_use);
    append_tab(expected, list, picker::name_done);
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
        OA_CHECK(!name.ends_with(picker::name_grip_suffix));
        OA_CHECK(
            !(starts(name, picker::name_row_prefix) && name.ends_with(picker::name_switch_suffix))
        );
        OA_CHECK(name != picker::name_hint);
        OA_CHECK(name != picker::name_summary);
        OA_CHECK(name != picker::name_verdict);
        OA_CHECK(name != "mutators.order");
        OA_CHECK(name != "mutators.list.bar");
        OA_CHECK(name != "mutators.details.bar");
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

/// Tells whether a text item lies inside a row.
///
/// @param item the item
/// @param row the row
/// @return true when its rectangle is inside the row
bool text_in_row(const kit::Item& item, const kit::Rect& row) {
    return item.role == kit::Role::text && kit::wholly_in(item.rect, row);
}

/// Checks one frame of the fixture: the geometry, the rows, the options, the
/// names and the texts.
///
/// @param model the picker
/// @param where the case's name
/// @param frame the frame
/// @param window the window's size
void check_frame(
    const picker::Picker& model,
    const std::string& where,
    const kit::Frame& frame,
    kit::Point window
) {
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const int32_t row_height = compact ? picker::compact_row_height
                               : frame.size_class == kit::SizeClass::large
                                   ? picker::large_row_height
                                   : picker::regular_row_height;
    const int32_t number_width = compact ? picker::compact_number_column : picker::number_column;

    OA_CHECK(window.x == picker::window_size(frame).x);
    OA_CHECK(window.y == picker::window_size(frame).y);
    OA_EXPECT_NONE(outside_window(list, window), where);
    OA_EXPECT_NONE(meeting_controls(list), where);
    OA_EXPECT_NONE(texts_not_fitting(list, fonts()), where);
    OA_EXPECT_NONE(cut_buttons(list), where);
    const std::string names = kit::name_problem(list);
    if (!names.empty())
        std::fprintf(stderr, "%s: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    check_tabs(list, frame, false, where);

    const std::vector<picker::Row> shown = picker::rows(model);
    std::vector<std::string> numbers;
    for (const picker::Row& row : shown) {
        const std::string row_name = std::string(picker::name_row_prefix) + row.id;
        const kit::Control* row_control = find_named(list, row_name);
        const std::string grip_name = row_name + std::string(picker::name_grip_suffix);
        const std::string switch_name = row_name + std::string(picker::name_switch_suffix);
        const kit::Control* grip = find_named(list, grip_name);
        const kit::Control* toggle = find_named(list, switch_name);
        OA_CHECK(row_control != nullptr);
        OA_CHECK(grip != nullptr);
        OA_CHECK(toggle != nullptr);
        if (row_control == nullptr || grip == nullptr || toggle == nullptr)
            continue;
        OA_CHECK(row_control->kind == kit::ControlKind::list_item);
        OA_CHECK(!row_control->text.empty());
        if (row.id == "fast-builders")
            OA_CHECK(row_control->text == "Fast Builders");
        if (row.id == "ridge-walls")
            OA_CHECK(row_control->text == "Ridge Walls");
        OA_CHECK(row_control->rect.height == row_height);
        OA_CHECK(!row_control->focusable);
        OA_CHECK(grip->kind == kit::ControlKind::button);
        OA_CHECK(!grip->focusable);
        OA_CHECK(grip->enabled);
        OA_CHECK(grip->rect.width == kit::grip_width);
        OA_CHECK(grip->rect.height == kit::grip_height);
        OA_CHECK(toggle->kind == kit::ControlKind::toggle);
        OA_CHECK(!toggle->focusable);
        bool grip_drawn = false;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::grip && item.control == grip->id) {
                grip_drawn = true;
                OA_CHECK(item.rect.width == kit::grip_width);
                OA_CHECK(item.rect.height == kit::grip_height);
                OA_CHECK(std::get_if<kit::GripLook>(&item.look) != nullptr);
            }
        }
        OA_CHECK(grip_drawn);
        int32_t lines = 0;
        for (const kit::Item& item : list.items) {
            if (!text_in_row(item, row_control->rect))
                continue;
            OA_CHECK(item.text.find('\n') == std::string::npos);
            if (item.font == kit::FontRole::small && item.align == kit::Align::left)
                ++lines;
            if (row.on && item.align == kit::Align::right && item.colour == kit::colour::quiet) {
                numbers.push_back(item.text);
                OA_CHECK(item.rect.width == number_width);
            }
        }
        OA_CHECK(lines == 1);
    }
    OA_CHECK(numbers.size() == 4);
    if (numbers.size() == 4) {
        OA_CHECK(numbers[0] == "1");
        OA_CHECK(numbers[1] == "2");
        OA_CHECK(numbers[2] == "3");
        OA_CHECK(numbers[3] == "4");
    }

    std::vector<std::string> headings;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::heading)
            continue;
        const auto* look = std::get_if<kit::HeadingLook>(&item.look);
        if (look != nullptr)
            headings.push_back(look->text);
    }
    OA_CHECK(headings.size() == 2);
    if (headings.size() == 2) {
        OA_CHECK(headings[0] == picker::words::heading_on);
        OA_CHECK(headings[1] == picker::words::heading_off);
    }

    const int32_t left = metrics.edge + metrics.window_padding;
    const int32_t content = window.x - 2 * left;
    const int32_t list_width = compact ? content
                               : frame.size_class == kit::SizeClass::large
                                   ? picker::large_list_width
                                   : picker::regular_list_width;
    bool pane = false;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::fill && item.colour == kit::colour::list &&
            item.rect.x == left && item.rect.width == list_width && item.rect.height > row_height)
            pane = true;
    }
    OA_CHECK(pane);
    const kit::Control* search = find_named(list, picker::name_search);
    OA_CHECK(search != nullptr);
    if (search != nullptr) {
        const int32_t search_width =
            compact ? content - picker::compact_filter_width - 2 * metrics.space
            : frame.size_class == kit::SizeClass::large ? picker::large_search_width
                                                        : picker::regular_search_width;
        OA_CHECK(search->rect.width == search_width);
    }

    bool details_badge = false;
    bool locked = false;
    bool download = false;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::package_badge && item.rect.width == picker::details_badge_side)
            details_badge = true;
        if (item.role == kit::Role::locked_fade)
            locked = true;
        if (const auto* look = std::get_if<kit::ChipLook>(&item.look);
            item.role == kit::Role::chip && look != nullptr && look->icon &&
            *look->icon == kit::StateIcon::download_arrow)
            download = true;
    }
    OA_CHECK(locked);
    OA_CHECK(download);
    OA_CHECK(details_badge == !compact);

    const kit::Control* speed = find_named(list, "mutators.option.speed");
    const kit::Control* signal = find_named(list, "mutators.option.signal");
    const kit::Control* alarm = find_named(list, "mutators.option.alarm");
    const kit::Control* range = find_named(list, "mutators.option.range");
    if (compact) {
        OA_CHECK(speed == nullptr);
        OA_CHECK(find_named(list, picker::name_clash) == nullptr);
        OA_CHECK(find_named(list, picker::name_details) != nullptr);
        OA_CHECK(find_named(list, picker::name_filter) != nullptr);
        OA_CHECK(find_named(list, picker::name_tags) == nullptr);
        OA_CHECK(find_named(list, picker::name_verdict) == nullptr);
        const kit::Control* filter = find_named(list, picker::name_filter);
        OA_CHECK(filter != nullptr && filter->rect.width == picker::compact_filter_width);
    } else {
        OA_CHECK(speed != nullptr && speed->kind == kit::ControlKind::levels);
        OA_CHECK(signal != nullptr && signal->kind == kit::ControlKind::choice);
        OA_CHECK(alarm != nullptr && alarm->kind == kit::ControlKind::toggle);
        OA_CHECK(range != nullptr && range->kind == kit::ControlKind::slider);
        const kit::Control* clash = find_named(list, picker::name_clash);
        OA_CHECK(clash != nullptr && clash->kind == kit::ControlKind::levels);
        OA_CHECK(clash != nullptr && clash->enabled && clash->focusable);
        if (clash != nullptr) {
            OA_CHECK(clash->parts.size() == 2);
            OA_CHECK(clash->parts.size() == 2 && clash->parts[0] == "add");
            OA_CHECK(clash->parts.size() == 2 && clash->parts[1] == "replace");
            bool strip = false;
            for (const kit::Item& item : list.items) {
                if (item.control != clash->id || item.role != kit::Role::levels)
                    continue;
                const auto* look = std::get_if<kit::LevelsLook>(&item.look);
                OA_CHECK(look != nullptr && look->chosen == 1);
                OA_CHECK(look != nullptr && look->captions.size() == 2);
                if (look != nullptr && look->captions.size() == 2) {
                    OA_CHECK(look->captions[0] == picker::words::clash_mode_add);
                    OA_CHECK(look->captions[1] == picker::words::clash_mode_replace);
                }
                strip = true;
            }
            OA_CHECK(strip);
        }
        OA_CHECK(find_named(list, picker::name_up) != nullptr);
        OA_CHECK(find_named(list, picker::name_down) != nullptr);
        OA_CHECK(find_named(list, picker::name_tags) != nullptr);
        OA_CHECK(find_named(list, picker::name_details) == nullptr);
        const kit::Control* verdict = find_named(list, picker::name_verdict);
        OA_CHECK(verdict != nullptr);
        if (verdict != nullptr)
            OA_CHECK(verdict->text.find("Ridge") != std::string::npos);
        const kit::Control* all = find_named(list, picker::name_filter_all);
        const kit::Control* installed = find_named(list, picker::name_filter_installed);
        const kit::Control* downloads = find_named(list, picker::name_filter_downloads);
        OA_CHECK(all != nullptr && all->text == "All (6)");
        OA_CHECK(installed != nullptr && installed->text == "Installed (4)");
        OA_CHECK(downloads != nullptr && downloads->text == "Downloads (2)");
    }
    const kit::Control* hint = find_named(list, picker::name_hint);
    OA_CHECK(hint != nullptr);
    if (hint != nullptr)
        OA_CHECK(hint->text.find("skirmish") != std::string::npos);
    const kit::Control* summary = find_named(list, picker::name_summary);
    OA_CHECK(summary != nullptr);
}

/// Checks ▲ and ▼ at one selection.
///
/// @param model the picker; its selection is replaced
/// @param id the mutator
/// @param up_enabled ▲ can be pressed
/// @param down_enabled ▼ can be pressed
/// @param frame the frame
void check_band(
    picker::Picker model,
    std::string id,
    bool up_enabled,
    bool down_enabled,
    const kit::Frame& frame
) {
    model.selected = std::move(id);
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Control* up = find_named(list, picker::name_up);
    const kit::Control* down = find_named(list, picker::name_down);
    OA_CHECK(up != nullptr && down != nullptr);
    if (up == nullptr || down == nullptr)
        return;
    OA_CHECK(up->enabled == up_enabled);
    OA_CHECK(down->enabled == down_enabled);
}

/// Returns where a dragged row's top is clamped into the list's view.
///
/// @param port the list's view
/// @param row_height the row's height
/// @param asked the y the drag asks for
/// @return the top, inside the view
int32_t clamped_row_top(const kit::Rect& port, int32_t row_height, int32_t asked) {
    const int32_t least = port.y;
    const int32_t most = port.y + std::max(port.height - row_height, int32_t{0});
    int32_t top = asked;
    if (top < least)
        top = least;
    if (top > most)
        top = most;
    return top;
}

/// Finds the floating row: the selected fill at a top.
///
/// @param list the display list
/// @param top the clamped top
/// @param row_height the row's height
/// @param[out] at the fill's place in the items; -1 when it is missing
/// @return the fill's rectangle; empty when it is missing
kit::Rect floating_row(const kit::DisplayList& list, int32_t top, int32_t row_height, int& at) {
    at = -1;
    for (std::size_t index = 0; index < list.items.size(); ++index) {
        const kit::Item& item = list.items[index];
        if (item.role != kit::Role::fill || item.colour != kit::colour::list_selected)
            continue;
        if (item.rect.y != top || item.rect.height != row_height)
            continue;
        at = static_cast<int>(index);
        return item.rect;
    }
    return {};
}

/// Checks one drag: the controls stay on the well, nothing meets, and the
/// floating row is drawn last and whole inside the list's view.
///
/// @param model the picker
/// @param frame the frame
/// @param window the window
/// @param asked the y the drag asks for
/// @param targeted true when the drop has a place
/// @param where the case
void check_one_drag(
    const picker::Picker& model,
    const kit::Frame& frame,
    kit::Point window,
    int32_t asked,
    bool targeted,
    const std::string& where
) {
    picker::ScreenState resting_state;
    const kit::DisplayList resting = laid_out(model, resting_state, frame);
    const kit::Control* home = find_named(resting, "mutators.row.scout-post");
    const kit::Control* home_grip = find_named(resting, "mutators.row.scout-post.grip");
    const kit::Control* home_switch = find_named(resting, "mutators.row.scout-post.switch");
    const kit::Control* view = find_named(resting, picker::name_list);
    OA_CHECK(home != nullptr && home_grip != nullptr && home_switch != nullptr && view != nullptr);
    if (home == nullptr || view == nullptr)
        return;

    picker::ScreenState state;
    picker::DragShown drag;
    drag.id = "scout-post";
    drag.y = asked;
    if (targeted)
        drag.target = picker::Drop{true, 1};
    state.drag = drag;
    const kit::DisplayList moving = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(moving, window), where);
    OA_EXPECT_NONE(meeting_controls(moving), where);

    const kit::Control* row = find_named(moving, "mutators.row.scout-post");
    const kit::Control* grip = find_named(moving, "mutators.row.scout-post.grip");
    const kit::Control* toggle = find_named(moving, "mutators.row.scout-post.switch");
    const kit::Control* port = find_named(moving, picker::name_list);
    OA_CHECK(row != nullptr && grip != nullptr && toggle != nullptr && port != nullptr);
    if (row == nullptr || grip == nullptr || toggle == nullptr || port == nullptr ||
        home_grip == nullptr || home_switch == nullptr)
        return;
    OA_CHECK(same_rect(row->rect, home->rect));
    OA_CHECK(same_rect(grip->rect, home_grip->rect));
    OA_CHECK(same_rect(toggle->rect, home_switch->rect));

    const int32_t top = clamped_row_top(port->rect, home->rect.height, asked);
    int float_at = -1;
    const kit::Rect floating = floating_row(moving, top, home->rect.height, float_at);
    OA_CHECK(float_at >= 0);
    OA_CHECK(kit::wholly_in(floating, port->rect));
    if (float_at < 0)
        return;

    bool well = false;
    bool line = false;
    bool line_before = false;
    for (std::size_t index = 0; index < moving.items.size(); ++index) {
        const kit::Item& item = moving.items[index];
        if (item.role == kit::Role::fill && item.colour == kit::colour::well &&
            same_rect(item.rect, home->rect))
            well = true;
        if (item.role == kit::Role::fill && item.colour == kit::colour::accent &&
            item.rect.height == 2) {
            line = true;
            if (static_cast<int>(index) < float_at)
                line_before = true;
        }
        if (static_cast<int>(index) <= float_at)
            continue;
        if (!has_room(kit::intersect(item.rect, floating)))
            continue;
        if (!kit::wholly_in(item.rect, floating)) {
            std::fprintf(
                stderr, "%s: an item after the floating row paints over it\n", where.c_str()
            );
            OA_CHECK(false);
        }
    }
    OA_CHECK(well);
    OA_CHECK(line == targeted);
    OA_CHECK(!targeted || line_before);

    for (const kit::Control& control : moving.controls) {
        if (!starts(control.name, picker::name_row_prefix))
            continue;
        if (control.name.ends_with(picker::name_grip_suffix) ||
            control.name.ends_with(picker::name_switch_suffix))
            continue;
        if (control.name == "mutators.row.scout-post")
            continue;
        // A row scrolled partly out of the list shows only inside the list.
        const kit::Rect shown = press_area(control);
        for (std::size_t index = 0; index < moving.items.size(); ++index) {
            const kit::Item& item = moving.items[index];
            if (!has_room(kit::intersect(item.rect, shown)))
                continue;
            if (static_cast<int>(index) >= float_at && kit::wholly_in(item.rect, floating))
                continue;
            OA_CHECK(static_cast<int>(index) < float_at);
        }
    }
}

/// Checks a drag onto a middle row, a y past the view, a y above it, and a
/// refused drop.
///
/// @param model the picker
/// @param frame the frame
/// @param window the window
void check_drag(const picker::Picker& model, const kit::Frame& frame, kit::Point window) {
    picker::ScreenState state;
    const kit::DisplayList resting = laid_out(model, state, frame);
    const kit::Control* gatehouse = find_named(resting, "mutators.row.gatehouse");
    const kit::Control* port = find_named(resting, picker::name_list);
    const kit::Control* home = find_named(resting, "mutators.row.scout-post");
    OA_CHECK(gatehouse != nullptr && port != nullptr && home != nullptr);
    if (gatehouse == nullptr || port == nullptr || home == nullptr)
        return;

    const std::string where(
        frame.size_class == kit::SizeClass::compact ? "drag compact"
        : frame.size_class == kit::SizeClass::large ? "drag large"
                                                    : "drag"
    );
    check_one_drag(model, frame, window, gatehouse->rect.y + 10, true, where);
    const int32_t most = port->rect.y + std::max(port->rect.height - home->rect.height, int32_t{0});
    check_one_drag(model, frame, window, 2000, true, where + " below");
    check_one_drag(model, frame, window, port->rect.y - 80, true, where + " above");

    picker::DragShown drag;
    drag.id = "scout-post";
    drag.y = most;
    state.drag = drag;
    const kit::DisplayList refused = laid_out(model, state, frame);
    bool refused_line = false;
    for (const kit::Item& item : refused.items) {
        if (item.role == kit::Role::fill && item.colour == kit::colour::accent &&
            item.rect.height == 2)
            refused_line = true;
    }
    OA_CHECK(!refused_line);
    const kit::Control* moved = find_named(refused, "mutators.row.scout-post");
    OA_CHECK(moved != nullptr && same_rect(moved->rect, home->rect));
    int float_at = -1;
    const kit::Rect floating = floating_row(refused, most, home->rect.height, float_at);
    const kit::Control* refused_port = find_named(refused, picker::name_list);
    OA_CHECK(float_at >= 0 && refused_port != nullptr);
    if (refused_port != nullptr)
        OA_CHECK(kit::wholly_in(floating, refused_port->rect));
    OA_CHECK(floating.y == most);
}

/// Checks read-only viewing: locked switches, disabled grips, CLOSE alone.
///
/// @param frame the frame
/// @param window the window
void check_read_only(const kit::Frame& frame, kit::Point window) {
    const picker::Picker model = opened({}, true);
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(list, window), "read-only");
    OA_EXPECT_NONE(meeting_controls(list), "read-only");
    OA_EXPECT_NONE(texts_not_fitting(list, fonts()), "read-only");
    check_tabs(list, frame, false, "read-only");
    OA_CHECK(find_named(list, picker::name_done) != nullptr);
    OA_CHECK(find_named(list, picker::name_cancel) == nullptr);
    OA_CHECK(find_named(list, picker::name_use) == nullptr);
    OA_CHECK(find_named(list, picker::name_details) == nullptr);
    OA_CHECK(find_named(list, picker::name_back) == nullptr);
    OA_CHECK(find_named(list, picker::name_close) != nullptr);
    OA_CHECK(find_named(list, picker::name_verdict) == nullptr);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const kit::Control* up = find_named(list, picker::name_up);
    const kit::Control* down = find_named(list, picker::name_down);
    const kit::Control* clash = find_named(list, picker::name_clash);
    if (compact) {
        OA_CHECK(up == nullptr);
        OA_CHECK(down == nullptr);
        OA_CHECK(clash == nullptr);
    } else {
        OA_CHECK(up != nullptr && !up->enabled);
        OA_CHECK(down != nullptr && !down->enabled);
        OA_CHECK(!in_tab(list, picker::name_up));
        OA_CHECK(!in_tab(list, picker::name_down));
        OA_CHECK(clash != nullptr && !clash->enabled && !clash->focusable);
        OA_CHECK(!in_tab(list, picker::name_clash));
        bool clash_locked = false;
        if (clash != nullptr) {
            for (const kit::Item& item : list.items) {
                if (item.control != clash->id || item.role != kit::Role::levels)
                    continue;
                const auto* look = std::get_if<kit::LevelsLook>(&item.look);
                clash_locked = look != nullptr && look->locked && look->chosen == 1;
            }
        }
        OA_CHECK(clash_locked);
    }
    const kit::Control* search = find_named(list, picker::name_search);
    OA_CHECK(search != nullptr && search->focusable && search->enabled);
    for (const kit::Control& control : list.controls) {
        if (control.kind == kit::ControlKind::button &&
            control.name.ends_with(picker::name_grip_suffix))
            OA_CHECK(!control.enabled);
        if (starts(control.name, picker::name_option_prefix))
            OA_CHECK(!control.enabled);
    }
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::grip) {
            const auto* look = std::get_if<kit::GripLook>(&item.look);
            OA_CHECK(look != nullptr && look->disabled);
        }
        if (item.role == kit::Role::toggle) {
            const auto* look = std::get_if<kit::SwitchLook>(&item.look);
            OA_CHECK(look != nullptr && look->locked);
        }
    }
    const kit::Control* done = find_named(list, picker::name_done);
    if (done != nullptr) {
        bool accent = false;
        for (const kit::Item& item : list.items) {
            const auto* look = std::get_if<kit::ButtonLook>(&item.look);
            if (item.control == done->id && look != nullptr)
                accent = look->style == kit::ButtonStyle::accent;
        }
        OA_CHECK(accent);
    }
}

/// Checks that waiting replaces the summary and leaves USE THESE enabled.
///
/// @param model the picker
/// @param frame the frame
void check_waiting(const picker::Picker& model, const kit::Frame& frame) {
    picker::ScreenState state;
    state.waiting = "Checking with Ridge";
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Control* use = find_named(list, picker::name_use);
    const kit::Control* summary = find_named(list, picker::name_summary);
    OA_CHECK(use != nullptr && use->enabled);
    OA_CHECK(summary != nullptr && summary->text == "Checking with Ridge");
    bool use_in_tab = false;
    for (const std::string& name : tab_names(list)) {
        if (name == picker::name_use)
            use_in_tab = true;
    }
    OA_CHECK(use_in_tab);
}

/// Checks both empty lists.
///
/// @param frame the frame
/// @param window the window
void check_empty(const kit::Frame& frame, kit::Point window) {
    picker::Inputs none;
    none.mod_name = "Ridge";
    none.setup = picker::Setup::skirmish;
    picker::Picker empty;
    picker::open(empty, std::move(none));
    picker::ScreenState state;
    const kit::DisplayList bare = laid_out(empty, state, frame);
    OA_EXPECT_NONE(outside_window(bare, window), "empty");
    bool none_drawn = false;
    for (const kit::Item& item : bare.items) {
        if (item.role == kit::Role::text &&
            item.text.find("No mutators here yet") != std::string::npos)
            none_drawn = true;
    }
    OA_CHECK(none_drawn);
    OA_CHECK(picker::rows(empty).empty());
    const kit::Control* bare_list = find_named(bare, picker::name_list);
    OA_CHECK(bare_list != nullptr);
    bool sentence_in_list = false;
    if (bare_list != nullptr) {
        for (const kit::Item& item : bare.items) {
            if (item.role == kit::Role::text &&
                item.text.find("No mutators here yet") != std::string::npos &&
                kit::wholly_in(item.rect, bare_list->rect))
                sentence_in_list = true;
        }
    }
    OA_CHECK(sentence_in_list);
    if (frame.size_class == kit::SizeClass::compact) {
        const kit::Control* details = find_named(bare, picker::name_details);
        OA_CHECK(details != nullptr && !details->enabled);
        OA_CHECK(!in_tab(bare, picker::name_details));
    }

    picker::Inputs filtered_inputs;
    filtered_inputs.mod_name = "Ridge";
    filtered_inputs.setup = picker::Setup::skirmish;
    picker::Mutator installed = made("fast-builders", "Fast Builders", true);
    installed.summary = "Builds fast.";
    filtered_inputs.mutators.push_back(std::move(installed));
    picker::Picker filtered;
    picker::open(filtered, std::move(filtered_inputs));
    filtered.filter = picker::Filter::downloads;
    const kit::DisplayList kept = laid_out(filtered, state, frame);
    OA_EXPECT_NONE(outside_window(kept, window), "filtered");
    bool filtered_drawn = false;
    for (const kit::Item& item : kept.items) {
        if (item.role == kit::Role::text &&
            item.text.find("No mutator matches these filters") != std::string::npos)
            filtered_drawn = true;
    }
    OA_CHECK(filtered_drawn);
    OA_CHECK(picker::rows(filtered).empty());
}

/// Checks the French lookup and the long German words.
///
/// @param frame the frame
/// @param window the window
/// @param where the case
void check_language(const kit::Frame& frame, kit::Point window, const std::string& where) {
    lib::TextHooks french;
    french.shown = french_shown;
    const picker::Picker french_model = opened(french, false);
    picker::ScreenState state;
    const kit::DisplayList french_list = laid_out(french_model, state, frame);
    OA_EXPECT_NONE(outside_window(french_list, window), where + " fr");
    OA_EXPECT_NONE(meeting_controls(french_list), where + " fr");
    OA_EXPECT_NONE(texts_not_fitting(french_list, fonts()), where + " fr");
    OA_EXPECT_NONE(cut_buttons(french_list), where + " fr");
    bool title = false;
    bool heading = false;
    bool off_heading = false;
    bool placeholder = false;
    bool switched = false;
    for (const kit::Item& item : french_list.items) {
        if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
            title = look->title == "OPEN ANNIHILATION" && look->second == "MUTATEURS" &&
                    look->version.find("Escarmouche") != std::string::npos;
        if (const auto* look = std::get_if<kit::HeadingLook>(&item.look)) {
            if (look->text == "Actifs · appliqués de haut en bas")
                heading = true;
            if (look->text == "Inactifs · non appliqués")
                off_heading = true;
        }
        if (const auto* look = std::get_if<kit::SearchLook>(&item.look))
            placeholder = look->placeholder == "Rechercher des mutateurs";
        if (const auto* look = std::get_if<kit::SwitchLook>(&item.look))
            switched = switched || (look->off_caption == "Non" && look->on_caption == "Oui");
    }
    OA_CHECK(title);
    OA_CHECK(heading);
    OA_CHECK(off_heading);
    OA_CHECK(placeholder);
    OA_CHECK(switched);
    const kit::Control* use = find_named(french_list, picker::name_use);
    OA_CHECK(use != nullptr && use->text == "Valider");
    if (frame.size_class == kit::SizeClass::compact) {
        const kit::Control* details = find_named(french_list, picker::name_details);
        OA_CHECK(details != nullptr && details->text == "Détails");
    }

    lib::TextHooks german;
    german.shown = german_shown;
    const picker::Picker german_model = opened(german, false);
    const kit::DisplayList german_list = laid_out(german_model, state, frame);
    OA_EXPECT_NONE(outside_window(german_list, window), where + " de");
    OA_EXPECT_NONE(texts_not_fitting(german_list, fonts()), where + " de");
    OA_EXPECT_NONE(cut_buttons(german_list), where + " de");
    const kit::Control* german_use = find_named(german_list, picker::name_use);
    const kit::Control* german_cancel = find_named(german_list, picker::name_cancel);
    OA_CHECK(german_use != nullptr && german_use->text == "ÜBERNEHMEN UND LOS");
    OA_CHECK(german_cancel != nullptr && german_cancel->text == "ABBRECHEN UND ZURÜCK");
    if (frame.size_class == kit::SizeClass::compact) {
        const kit::Control* details = find_named(german_list, picker::name_details);
        OA_CHECK(details != nullptr && details->text == "EINZELHEITEN ANZEIGEN");
        OA_CHECK(details->rect.width > kit::compact_metrics.button_width);
    }
    bool german_heading = false;
    bool german_off = false;
    bool german_switch = false;
    for (const kit::Item& item : german_list.items) {
        const auto* heading_look = std::get_if<kit::HeadingLook>(&item.look);
        if (heading_look != nullptr &&
            heading_look->text == "Aktiv · von oben nach unten angewandt")
            german_heading = true;
        if (heading_look != nullptr && heading_look->text == "AUS · NICHT ANGEWANDT")
            german_off = true;
        const auto* switch_look = std::get_if<kit::SwitchLook>(&item.look);
        if (switch_look != nullptr && switch_look->off_caption == "AUS" &&
            switch_look->on_caption == "AN")
            german_switch = true;
    }
    OA_CHECK(german_heading);
    OA_CHECK(german_off);
    OA_CHECK(german_switch);
}

/// Checks Compact's details page.
///
/// @param model the picker
/// @param frame a Compact frame
/// @param window the window
void check_details_page(const picker::Picker& model, const kit::Frame& frame, kit::Point window) {
    picker::ScreenState state;
    state.details_page = true;
    const kit::DisplayList page = laid_out(model, state, frame);
    const kit::DisplayList direct = picker::details_page_layout(model, state, frame, fonts());
    OA_EXPECT_NONE(outside_window(page, window), "details");
    OA_EXPECT_NONE(meeting_controls(page), "details");
    OA_EXPECT_NONE(texts_not_fitting(page, fonts()), "details");
    OA_EXPECT_NONE(cut_buttons(page), "details");
    check_tabs(page, frame, true, "details");
    OA_CHECK(find_named(page, picker::name_back) != nullptr);
    OA_CHECK(find_named(page, picker::name_list) == nullptr);
    OA_CHECK(find_named(page, picker::name_details) == nullptr);
    OA_CHECK(find_named(page, picker::name_filter) == nullptr);
    OA_CHECK(find_named(direct, picker::name_back) != nullptr);
    OA_CHECK(find_named(direct, picker::name_list) == nullptr);
    const kit::Control* speed = find_named(page, "mutators.option.speed");
    const kit::Control* signal = find_named(page, "mutators.option.signal");
    const kit::Control* alarm = find_named(page, "mutators.option.alarm");
    const kit::Control* range = find_named(page, "mutators.option.range");
    OA_CHECK(speed != nullptr && speed->kind == kit::ControlKind::levels);
    OA_CHECK(signal != nullptr && signal->kind == kit::ControlKind::choice);
    OA_CHECK(alarm != nullptr && alarm->kind == kit::ControlKind::toggle);
    OA_CHECK(range != nullptr && range->kind == kit::ControlKind::slider);
    const kit::Control* clash = find_named(page, picker::name_clash);
    OA_CHECK(clash != nullptr && clash->kind == kit::ControlKind::levels && clash->enabled);
    if (clash != nullptr && clash->clip.height > 0 && !kit::wholly_in(clash->rect, clash->clip))
        state.details_scroll = std::max(clash->rect.y - clash->clip.y, int32_t{0});
    const kit::DisplayList scrolled = laid_out(model, state, frame);
    const kit::Control* seen = find_named(scrolled, picker::name_clash);
    OA_CHECK(seen != nullptr && seen->enabled && seen->focusable);
    OA_CHECK(in_tab(scrolled, picker::name_clash));
    check_tabs(scrolled, frame, true, "details scrolled");
    bool badge = false;
    for (const kit::Item& item : page.items) {
        if (item.role == kit::Role::package_badge && item.rect.width == picker::details_badge_side)
            badge = true;
    }
    OA_CHECK(badge);

    state.menu = picker::Menu::choice;
    state.menu_option = "signal";
    const kit::DisplayList menu = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(menu, window), "choice menu");
    OA_EXPECT_NONE(meeting_controls(menu), "choice menu");
    bool item_named = false;
    for (const kit::Control& control : menu.controls) {
        if (starts(control.name, "mutators.option.signal.") &&
            control.kind == kit::ControlKind::list_item)
            item_named = true;
    }
    OA_CHECK(item_named);
}

/// Checks the keyboard focus ring, and Compact's open filter menu.
///
/// @param model the picker
/// @param compact a Compact frame
/// @param regular a Regular frame
/// @param window the Compact window
void check_focus_and_menu(
    const picker::Picker& model,
    const kit::Frame& compact,
    const kit::Frame& regular,
    kit::Point window
) {
    picker::ScreenState state;
    const kit::DisplayList plain = laid_out(model, state, regular);
    const kit::Control* cancel = find_named(plain, picker::name_cancel);
    OA_CHECK(cancel != nullptr);
    if (cancel != nullptr) {
        state.interaction.focus_shown = true;
        state.interaction.focused = cancel->id;
        const kit::DisplayList focused = laid_out(model, state, regular);
        bool ring = false;
        for (const kit::Item& item : focused.items) {
            if (item.role == kit::Role::focus_ring && same_rect(item.rect, cancel->rect))
                ring = true;
        }
        OA_CHECK(ring);
    }
    const kit::Control* search = find_named(plain, picker::name_search);
    OA_CHECK(search != nullptr);
    if (search != nullptr) {
        state.interaction.focus_shown = true;
        state.interaction.focused = search->id;
        const kit::DisplayList field = laid_out(model, state, regular);
        bool ring = false;
        for (const kit::Item& item : field.items) {
            if (item.role == kit::Role::focus_ring)
                ring = true;
        }
        OA_CHECK(!ring);
    }

    picker::ScreenState open_menu;
    open_menu.menu = picker::Menu::filter;
    const kit::DisplayList filtering = laid_out(model, open_menu, compact);
    OA_EXPECT_NONE(outside_window(filtering, window), "filter menu");
    OA_EXPECT_NONE(meeting_controls(filtering), "filter menu");
    OA_EXPECT_NONE(texts_not_fitting(filtering, fonts()), "filter menu");
    OA_CHECK(find_named(filtering, "mutators.filter.all") != nullptr);
    OA_CHECK(find_named(filtering, "mutators.filter.installed") != nullptr);
    OA_CHECK(find_named(filtering, "mutators.filter.downloads") != nullptr);

    picker::ScreenState tags;
    tags.menu = picker::Menu::tags;
    const kit::DisplayList tag_menu = laid_out(model, tags, regular);
    OA_EXPECT_NONE(outside_window(tag_menu, picker::window_size(regular)), "tags menu");
    OA_EXPECT_NONE(meeting_controls(tag_menu), "tags menu");
    const std::vector<std::string> shown = picker::tags_shown(model);
    std::vector<std::string> pills;
    std::vector<std::string> overflow;
    for (const kit::Control& control : tag_menu.controls) {
        if (!starts(control.name, std::string(picker::name_tags) + "."))
            continue;
        if (control.kind == kit::ControlKind::button)
            pills.push_back(control.name.substr(std::string(picker::name_tags).size() + 1));
        if (control.kind == kit::ControlKind::list_item)
            overflow.push_back(control.name.substr(std::string(picker::name_tags).size() + 1));
    }
    std::size_t at = 0;
    for (const std::string& pill : pills) {
        if (at >= shown.size() || shown[at] != pill) {
            std::fprintf(stderr, "pill \"%s\" is not the next tag\n", pill.c_str());
            OA_CHECK(false);
        }
        ++at;
    }
    for (const std::string& item : overflow) {
        if (at >= shown.size() || shown[at] != item) {
            std::fprintf(stderr, "menu item \"%s\" is not the next tag\n", item.c_str());
            OA_CHECK(false);
        }
        ++at;
    }
    OA_CHECK(at == shown.size());
}

/// Checks that TAGS is disabled when every tag is a pill.
///
/// @param frame a Regular frame
void check_tags_idle(const kit::Frame& frame) {
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.setup = picker::Setup::skirmish;
    picker::Mutator watch = made("watch", "Watch", true);
    watch.summary = "A watch.";
    watch.tags = {"ai"};
    inputs.mutators.push_back(std::move(watch));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    picker::ScreenState state;
    state.menu = picker::Menu::tags;
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Control* tags = find_named(list, picker::name_tags);
    OA_CHECK(tags != nullptr && !tags->enabled && !tags->focusable);
    OA_CHECK(!in_tab(list, picker::name_tags));
    for (const kit::Control& control : list.controls) {
        OA_CHECK(
            !(control.kind == kit::ControlKind::list_item &&
              starts(control.name, std::string(picker::name_tags) + "."))
        );
    }
}

/// Checks that DETAILS is disabled when nothing is selected, and when the
/// selection names no mutator.
///
/// @param model the picker
/// @param frame a Compact frame
void check_details_locked(const picker::Picker& model, const kit::Frame& frame) {
    const auto check = [&](picker::Picker picked, const char* where) {
        picker::ScreenState state;
        const kit::DisplayList list = laid_out(picked, state, frame);
        const kit::Control* details = find_named(list, picker::name_details);
        if (details == nullptr || details->enabled || in_tab(list, picker::name_details))
            std::fprintf(stderr, "%s: DETAILS is open\n", where);
        OA_CHECK(details != nullptr && !details->enabled);
        OA_CHECK(!in_tab(list, picker::name_details));
    };
    picker::Picker empty_selection = model;
    empty_selection.selected.reset();
    check(std::move(empty_selection), "no selection");
    picker::Picker gone = model;
    gone.selected = "gone";
    check(std::move(gone), "missing selection");
}

/// Checks the Off heading and the empty sentence when a search hides every
/// Off row and leaves the On rows.
///
/// @param model the picker
/// @param frame the frame
void check_off_hidden(const picker::Picker& model, const kit::Frame& frame) {
    picker::Picker filtered = model;
    filtered.query = "zzz-no-match";
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(filtered, state, frame);
    OA_CHECK(find_named(list, "mutators.row.scout-post") != nullptr);
    OA_CHECK(find_named(list, "mutators.row.grounded") == nullptr);
    OA_CHECK(find_named(list, "mutators.row.ridge-walls") == nullptr);
    bool off_heading = false;
    bool sentence = false;
    for (const kit::Item& item : list.items) {
        const auto* look = std::get_if<kit::HeadingLook>(&item.look);
        if (item.role == kit::Role::heading && look != nullptr &&
            look->text == picker::words::heading_off)
            off_heading = true;
        if (item.role == kit::Role::text &&
            item.text.find("No mutator matches these filters") != std::string::npos)
            sentence = true;
    }
    OA_CHECK(off_heading);
    OA_CHECK(sentence);
}

/// Checks that a mutator which brings nothing has no clash strip.
///
/// @param model the picker
/// @param frame a Regular frame
/// At a frame the window takes Settings' sizes: Kit 2's window header
/// (Compact's band at Compact) with the close mark in its middle, Kit 2's
/// window footer, Cancel and Use these at the control height, at least
/// twice as wide as they are tall, two spaces apart in the regular font, and
/// the filters, Tags, the search, ▲ Up, ▼ Down and the clash strip at the
/// control height, the pills at Kit 2's filter chip height.
///
/// @param model the picker
/// @param frame the frame
/// @param window the window
void check_settings_sizes(const picker::Picker& model, const kit::Frame& frame, kit::Point window) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const kit::DisplayList list = laid_out(model, {}, frame);
    const int32_t band = compact ? metrics.header_height : metrics.window_header_height;
    bool header = false;
    bool footer = false;
    bool regular_font = false;
    const kit::ControlId use_id = kit::control_named(list, picker::name_use);
    for (const kit::Item& item : list.items) {
        if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
            header = look->height == band && item.rect.height == metrics.edge + band + 1;
        if (item.role == kit::Role::footer_band)
            footer = item.rect.y == window.y - metrics.edge - metrics.window_footer_height - 1;
        if (const auto* look = std::get_if<kit::ButtonLook>(&item.look);
            look != nullptr && item.control == use_id)
            regular_font =
                look->font == kit::FontRole::regular && look->style == kit::ButtonStyle::accent;
    }
    OA_CHECK(header);
    OA_CHECK(footer);
    OA_CHECK(regular_font);
    const kit::Control* cancel = find_named(list, picker::name_cancel);
    const kit::Control* use = find_named(list, picker::name_use);
    const kit::Control* close = find_named(list, picker::name_close);
    OA_CHECK(cancel != nullptr && use != nullptr && close != nullptr);
    if (cancel == nullptr || use == nullptr || close == nullptr)
        return;
    for (const kit::Control* button : {cancel, use}) {
        OA_CHECK(button->rect.height == metrics.control_height);
        OA_CHECK(button->rect.width >= 2 * metrics.control_height);
    }
    OA_CHECK(use->rect.x - (cancel->rect.x + cancel->rect.width) == 2 * metrics.space);
    OA_CHECK(use->rect.x + use->rect.width == window.x - metrics.edge - metrics.window_padding);
    OA_CHECK(std::abs(close->rect.y + close->rect.height / 2 - (metrics.edge + band / 2)) <= 1);
    const kit::Control* search = find_named(list, picker::name_search);
    OA_CHECK(search != nullptr && search->rect.height == metrics.control_height);
    if (compact) {
        const kit::Control* filter = find_named(list, picker::name_filter);
        OA_CHECK(filter != nullptr && filter->rect.height == metrics.control_height);
        return;
    }
    const kit::Control* all = find_named(list, picker::name_filter_all);
    OA_CHECK(all != nullptr && all->rect.height == metrics.filter_chip_height);
    const kit::Control* tags = find_named(list, picker::name_tags);
    OA_CHECK(tags != nullptr && tags->rect.height == metrics.control_height);
    for (const std::string_view name : {picker::name_up, picker::name_down}) {
        const kit::Control* order = find_named(list, name);
        OA_CHECK(order != nullptr && order->rect.height == metrics.control_height);
    }
}

void check_no_clash(const picker::Picker& model, const kit::Frame& frame) {
    for (const char* id : {"gatehouse", "grounded"}) {
        picker::Picker picked = model;
        picked.selected = id;
        picker::ScreenState state;
        const kit::DisplayList list = laid_out(picked, state, frame);
        if (find_named(list, picker::name_clash) != nullptr)
            std::fprintf(stderr, "%s shows a clash strip\n", id);
        OA_CHECK(find_named(list, picker::name_clash) == nullptr);
    }
}

} // namespace

int main() {
    const std::vector<FrameCase> cases = frames();
    OA_CHECK(cases.size() == 6);
    OA_CHECK(cases[0].window.x == 480 && cases[0].window.y == 324);
    OA_CHECK(cases[1].window.x == 720 && cases[1].window.y == 486);
    OA_CHECK(cases[2].window.x == 960 && cases[2].window.y == 600);
    OA_CHECK(cases[3].frame.size_class == kit::SizeClass::compact);
    OA_CHECK(cases[3].window.x == 480 && cases[3].window.y == 324);
    OA_CHECK(cases[4].frame.size_class == kit::SizeClass::regular);
    OA_CHECK(cases[4].window.x == 720 && cases[4].window.y == 486);
    OA_CHECK(cases[5].frame.size_class == kit::SizeClass::large);
    OA_CHECK(cases[5].window.x == 960 && cases[5].window.y == 600);

    const picker::Picker model = opened({}, false);
    const std::vector<picker::Row> shown = picker::rows(model);
    OA_CHECK(shown.size() == 6);
    OA_CHECK(model.on.size() == 4);
    if (shown.size() == 6) {
        OA_CHECK(shown[0].id == "fast-builders" && shown[0].on && shown[0].number == 1);
        OA_CHECK(shown[1].id == "scout-post" && shown[1].number == 2);
        OA_CHECK(shown[2].id == "gatehouse" && shown[2].number == 3);
        OA_CHECK(shown[3].id == "gatehouse-keep" && shown[3].on && shown[3].to_download);
        OA_CHECK(shown[4].id == "grounded" && !shown[4].on && shown[4].to_download);
        OA_CHECK(shown[5].id == "ridge-walls" && shown[5].locked);
    }

    for (const FrameCase& one : cases) {
        check_frame(model, one.name, one.frame, one.window);
        check_settings_sizes(model, one.frame, one.window);
    }

    const kit::Frame regular = cases[1].frame;
    check_band(model, "fast-builders", false, true, regular);
    check_band(model, "scout-post", true, true, regular);
    check_band(model, "gatehouse", true, false, regular);
    check_band(model, "gatehouse-keep", false, false, regular);
    check_band(model, "ridge-walls", false, false, regular);
    check_band(model, "grounded", false, false, regular);

    for (const FrameCase& one : cases)
        check_drag(model, one.frame, one.window);

    check_read_only(regular, cases[1].window);
    check_read_only(cases[0].frame, cases[0].window);
    check_waiting(model, regular);
    check_empty(regular, cases[1].window);
    check_empty(cases[0].frame, cases[0].window);
    check_language(cases[0].frame, cases[0].window, "compact");
    check_language(regular, cases[1].window, "regular");
    check_details_page(model, cases[0].frame, cases[0].window);
    check_focus_and_menu(model, cases[0].frame, regular, cases[0].window);
    check_tags_idle(regular);
    check_details_locked(model, cases[0].frame);
    check_off_hidden(model, regular);
    check_off_hidden(model, cases[0].frame);
    check_no_clash(model, regular);

    picker::ScreenState page_state;
    page_state.details_page = true;
    const picker::Picker guest = opened({}, true);
    const kit::DisplayList guest_page = laid_out(guest, page_state, cases[0].frame);
    OA_CHECK(find_named(guest_page, picker::name_done) != nullptr);
    OA_CHECK(find_named(guest_page, picker::name_back) == nullptr);
    OA_CHECK(find_named(guest_page, picker::name_use) == nullptr);
    check_tabs(guest_page, cases[0].frame, true, "read-only page");

    return oa::test::check_exit_status();
}
