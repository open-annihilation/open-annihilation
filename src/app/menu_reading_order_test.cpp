// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's reading order, with the strip and without it, and where
// More maps… sits on map selection.
#include "oa/app/menu_reading_order.hpp"

#include "oa/test/check.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using oa::app::MenuCandidate;
using oa::app::MenuPressRect;
using oa::app::MenuReadingWay;

MenuCandidate button_at(
    std::string name, int32_t x, int32_t y, int32_t width, int32_t height, std::string shortcut
) {
    MenuCandidate candidate;
    candidate.name = std::move(name);
    candidate.x = x;
    candidate.y = y;
    candidate.width = width;
    candidate.height = height;
    candidate.button = true;
    candidate.shortcut = std::move(shortcut);
    return candidate;
}

MenuCandidate gadget_at(std::string name, int32_t x, int32_t y) {
    MenuCandidate candidate = button_at(std::move(name), x, y, 96, 20, {});
    candidate.gadget = true;
    return candidate;
}

MenuCandidate appended_at(std::string name, int32_t x, int32_t y, int32_t width, int32_t height) {
    MenuCandidate candidate = button_at(std::move(name), x, y, width, height, {});
    candidate.appended = true;
    return candidate;
}

std::vector<MenuCandidate> column_buttons() {
    return {
        gadget_at("SINGLE", 139, 393),
        gadget_at("MULTI", 139, 430),
        gadget_at("INTRO", 409, 393),
        gadget_at("EXIT", 409, 430)
    };
}

std::vector<MenuCandidate> strip_buttons() {
    return {
        button_at("single", 82, 393, 96, 20, "S"),
        button_at("multi", 82, 430, 96, 20, "M"),
        button_at("enter", 260, 412, 45, 28, "B"),
        button_at("intro", 464, 393, 96, 20, "I"),
        button_at("exit", 464, 430, 96, 20, "E")
    };
}

std::string joined(
    const std::vector<MenuCandidate>& candidates, const std::vector<oa::app::MenuReadingStop>& order
) {
    std::string names;
    for (const oa::app::MenuReadingStop& stop : order) {
        if (!names.empty())
            names += ", ";
        names += candidates[stop.index].name;
    }
    return names;
}

void without_the_strip_reads_the_two_columns_then_the_icons() {
    std::vector<MenuCandidate> candidates = column_buttons();
    candidates.push_back(appended_at("library", 520, 420, 32, 32));
    candidates.push_back(appended_at("settings", 560, 420, 32, 32));
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    OA_CHECK(joined(candidates, order) == "SINGLE, MULTI, INTRO, EXIT, library, settings");
}

void the_strip_replaces_the_covered_columns_and_keeps_enter() {
    std::vector<MenuCandidate> candidates = column_buttons();
    const std::vector<MenuCandidate> strip = strip_buttons();
    candidates.insert(candidates.end(), strip.begin(), strip.end());
    MenuCandidate former = button_at("former", 179, 393, 56, 20, "S");
    former.hidden = true;
    candidates.push_back(std::move(former));
    candidates.push_back(button_at("credits", 295, 454, 50, 24, {}));
    // The menu's own credits lies under the strip, so the strip's button is the stop.
    MenuCandidate menu_credits = gadget_at("menu-credits", 280, 440);
    menu_credits.width = 80;
    menu_credits.height = 40;
    menu_credits.follows_extension = true;
    candidates.push_back(std::move(menu_credits));
    MenuCandidate lamp = button_at("lamp", 600, 450, 16, 16, {});
    lamp.button = false;
    candidates.push_back(std::move(lamp));
    candidates.push_back(appended_at("library", 520, 12, 32, 32));
    candidates.push_back(appended_at("settings", 560, 12, 32, 32));
    candidates.push_back(appended_at("pill", 400, 48, 180, 28));
    const MenuPressRect press{0, 378, 640, 102};
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {{press}});
    OA_CHECK(
        joined(candidates, order) ==
        "single, multi, enter, credits, intro, exit, library, settings, pill"
    );
}

void button_mode_keeps_the_columns_and_puts_enter_between_them() {
    std::vector<MenuCandidate> candidates = column_buttons();
    candidates.push_back(button_at("enter", 260, 412, 45, 28, "B"));
    candidates.push_back(appended_at("library", 520, 420, 32, 32));
    candidates.push_back(appended_at("settings", 560, 420, 32, 32));
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    OA_CHECK(joined(candidates, order) == "SINGLE, MULTI, enter, INTRO, EXIT, library, settings");
}

MenuCandidate menu_credits_at_its_place() {
    // Between the columns by place. The mark keeps it after the plate's enter.
    MenuCandidate credits = gadget_at("credits", 280, 440);
    credits.width = 80;
    credits.height = 40;
    credits.follows_extension = true;
    return credits;
}

void button_mode_puts_credits_after_the_plate() {
    std::vector<MenuCandidate> candidates = column_buttons();
    candidates.push_back(button_at("enter", 502, 38, 45, 28, "B"));
    candidates.push_back(menu_credits_at_its_place());
    candidates.push_back(appended_at("library", 8, 8, 32, 32));
    candidates.push_back(appended_at("settings", 48, 8, 32, 32));
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    OA_CHECK(
        joined(candidates, order) == "SINGLE, MULTI, INTRO, EXIT, enter, credits, library, settings"
    );
}

void the_menu_credits_stays_out_when_no_extension_button_joins() {
    std::vector<MenuCandidate> candidates = column_buttons();
    candidates.push_back(menu_credits_at_its_place());
    candidates.push_back(appended_at("library", 520, 420, 32, 32));
    candidates.push_back(appended_at("settings", 560, 420, 32, 32));
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    OA_CHECK(joined(candidates, order) == "SINGLE, MULTI, INTRO, EXIT, library, settings");
}

void an_unmarked_gadget_between_the_columns_stays_there() {
    std::vector<MenuCandidate> candidates = column_buttons();
    candidates.push_back(button_at("enter", 502, 38, 45, 28, "B"));
    MenuCandidate credits = gadget_at("credits", 280, 440);
    credits.width = 80;
    credits.height = 40;
    candidates.push_back(std::move(credits));
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    OA_CHECK(joined(candidates, order) == "SINGLE, MULTI, credits, INTRO, EXIT, enter");
}

void a_hidden_or_disabled_appended_icon_stays_out() {
    std::vector<MenuCandidate> candidates = column_buttons();
    MenuCandidate library = appended_at("library", 520, 420, 32, 32);
    library.hidden = true;
    candidates.push_back(std::move(library));
    MenuCandidate settings = appended_at("settings", 560, 420, 32, 32);
    settings.enabled = false;
    candidates.push_back(std::move(settings));
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    OA_CHECK(joined(candidates, order) == "SINGLE, MULTI, INTRO, EXIT");
}

void arrows_move_by_place_and_wrap() {
    const std::vector<MenuCandidate> candidates = column_buttons();
    const std::vector<oa::app::MenuReadingStop> order =
        oa::app::main_menu_reading_order(candidates, {});
    const std::size_t single = 0;
    OA_CHECK(order.size() == 4U);
    OA_CHECK(
        candidates[order[oa::app::menu_reading_step(order, single, MenuReadingWay::right)].index]
            .name == "INTRO"
    );
    OA_CHECK(
        candidates[order[oa::app::menu_reading_step(order, single, MenuReadingWay::down)].index]
            .name == "MULTI"
    );
    OA_CHECK(
        candidates[order[oa::app::menu_reading_step(order, single, MenuReadingWay::left)].index]
            .name == "EXIT"
    );
}

void the_press_rectangle_covers_a_gadget_and_not_an_extension_button() {
    const MenuCandidate gadget = gadget_at("SINGLE", 139, 393);
    const MenuPressRect press{0, 378, 640, 102};
    OA_CHECK(oa::app::gadget_inside_press(gadget, press));
    MenuCandidate extension = button_at("single", 82, 393, 96, 20, "S");
    OA_CHECK(!oa::app::gadget_inside_press(extension, press));
    const MenuPressRect none{0, 0, 0, 0};
    OA_CHECK(!oa::app::gadget_inside_press(gadget, none));
}

void map_selection_puts_more_maps_after_the_list() {
    const std::vector<std::string> names{"MAPNAMES", "LOAD", "PREVMENU"};
    const std::vector<std::string> with_button = oa::app::map_picker_reading_order(names, true);
    OA_CHECK(with_button.size() == 4U);
    OA_CHECK(with_button[0] == "MAPNAMES");
    OA_CHECK(with_button[1] == "more-maps");
    OA_CHECK(with_button[2] == "LOAD");
    OA_CHECK(with_button[3] == "PREVMENU");
    const std::vector<std::string> without = oa::app::map_picker_reading_order(names, false);
    OA_CHECK(without.size() == 3U);
    OA_CHECK(without[0] == "MAPNAMES");
    OA_CHECK(without[1] == "LOAD");
    OA_CHECK(without[2] == "PREVMENU");
}

void map_selection_appends_more_maps_when_the_list_is_absent() {
    const std::vector<std::string> names{"LOAD", "PREVMENU"};
    const std::vector<std::string> order = oa::app::map_picker_reading_order(names, true);
    OA_CHECK(order.size() == 3U);
    OA_CHECK(order[2] == "more-maps");
}

} // namespace

int main() {
    without_the_strip_reads_the_two_columns_then_the_icons();
    the_strip_replaces_the_covered_columns_and_keeps_enter();
    button_mode_keeps_the_columns_and_puts_enter_between_them();
    button_mode_puts_credits_after_the_plate();
    the_menu_credits_stays_out_when_no_extension_button_joins();
    an_unmarked_gadget_between_the_columns_stays_there();
    a_hidden_or_disabled_appended_icon_stays_out();
    arrows_move_by_place_and_wrap();
    the_press_rectangle_covers_a_gadget_and_not_an_extension_button();
    map_selection_puts_more_maps_after_the_list();
    map_selection_appends_more_maps_when_the_list_is_absent();
    return oa::test::check_exit_status();
}
