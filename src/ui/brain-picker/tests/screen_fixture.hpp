// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the brain picker's screen tests share: the made-up brains (Steady
// Commander, Lantern Scout, quiet garrison, Ridge Raider and Tide Keeper),
// the synthetic font those layouts are measured in, and the checks a
// layout's parts are held to.
#pragma once

#include "oa/formats/fnt.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace brain_fixture {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::brain_picker;
namespace seats = oa::data::brain_library;
namespace lib = oa::ui::library;
namespace mutators = oa::ui::mutator_picker;

/// The synthetic font of the kit's tests, for both roles. A glyph for byte
/// b is 2 + b % 4 columns by 8 rows.
///
/// @return the fonts
inline kit::Fonts block_fonts() {
    oa::ui::frontend_renderer::TextFont font;
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
    font.ink[ink_index] = static_cast<uint16_t>(oa::ui::frontend_renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The fonts every layout here is measured in.
///
/// @return the fonts
inline const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

/// Returns the part of an item that is drawn: its rectangle, within its clip
/// when it has one, and a focus ring's outline round its control.
///
/// @param item the item
/// @return the rectangle
inline kit::Rect drawn_area(const kit::Item& item) {
    kit::Rect area = item.rect;
    if (item.role == kit::Role::focus_ring) {
        const auto* look = std::get_if<kit::FocusRingLook>(&item.look);
        if (look != nullptr && look->around)
            area = kit::grown(area, kit::compact_metrics.focus_inset);
    }
    if (item.clip.width > 0 && item.clip.height > 0)
        area = kit::intersect(area, item.clip);
    return area;
}

/// Returns the part of a control a press reaches.
///
/// @param control the control
/// @return its rectangle, within its clip when it has one
inline kit::Rect press_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
inline bool has_room(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Tells whether two rectangles are the same place.
///
/// @param one one rectangle
/// @param other the other
/// @return true when each edge matches
inline bool same_rect(const kit::Rect& one, const kit::Rect& other) {
    return one.x == other.x && one.y == other.y && one.width == other.width &&
           one.height == other.height;
}

/// Lists the parts of a layout that lie outside its window.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each part outside; none when all lie inside
inline std::vector<std::string> outside_window(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const kit::Rect area = drawn_area(item);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back(
                "item " + std::to_string(static_cast<int>(item.role)) + " \"" + item.text + "\""
            );
    }
    for (const kit::Control& control : list.controls) {
        const kit::Rect area = press_area(control);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back("control " + control.name);
    }
    return problems;
}

/// Lists the texts of a layout that do not fit their rectangles by the kit's
/// measure. Buttons, chips, drop-downs, menu items, the header, a heading
/// and a switch are measured.
///
/// @param list the display list
/// @param measured the fonts the layout measured in
/// @return a line for each text that does not fit; none when all fit
inline std::vector<std::string>
texts_not_fitting(const kit::DisplayList& list, const kit::Fonts& measured) {
    const kit::Metrics& metrics = kit::compact_metrics;
    std::vector<std::string> problems;
    const auto check = [&](bool fits, const std::string& what) {
        if (!fits)
            problems.push_back(what);
    };
    const auto width = [&measured](kit::FontRole role, std::string_view text) {
        return kit::text_width(measured, role, text);
    };
    for (const kit::Item& item : list.items) {
        switch (item.role) {
        case kit::Role::text:
            check(width(item.font, item.text) <= item.rect.width, "text \"" + item.text + "\"");
            break;
        case kit::Role::button:
            if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
                check(
                    width(kit::FontRole::small, look->caption) <= item.rect.width,
                    "button \"" + look->caption + "\""
                );
            break;
        case kit::Role::chip:
            if (const auto* look = std::get_if<kit::ChipLook>(&item.look))
                check(
                    kit::chip_width(measured, *look) <= item.rect.width,
                    "chip \"" + look->text + "\""
                );
            break;
        case kit::Role::choice:
            if (const auto* look = std::get_if<kit::ChoiceLook>(&item.look))
                check(
                    width(kit::FontRole::regular, look->text) <=
                        item.rect.width - metrics.choice_text_inset - metrics.choice_arrow_room,
                    "drop-down \"" + look->text + "\""
                );
            break;
        case kit::Role::choice_menu:
            if (const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look))
                for (std::size_t place = 0; place < look->shown.size(); ++place) {
                    const kit::Rect box = kit::choice_item(item.rect, static_cast<int32_t>(place));
                    check(
                        width(kit::FontRole::regular, look->shown[place]) <=
                            box.width - metrics.choice_item_text_inset,
                        "menu item \"" + look->shown[place] + "\""
                    );
                }
            break;
        case kit::Role::header:
            if (const auto* look = std::get_if<kit::HeaderLook>(&item.look)) {
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->title, look->tracking
                    ) <= look->title_width,
                    "header title \"" + look->title + "\""
                );
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->second, look->tracking
                    ) <= look->second_width,
                    "header word \"" + look->second + "\""
                );
                check(
                    width(kit::FontRole::small, look->version) <= look->version_width,
                    "header version \"" + look->version + "\""
                );
                const int32_t title_column =
                    metrics.padding + metrics.mark_side + metrics.header_gap;
                const int32_t words_right =
                    title_column + look->title_width +
                    (look->second.empty() ? 0 : metrics.header_gap + look->second_width);
                const int32_t version_column =
                    look->width - metrics.edge - metrics.padding - look->version_width;
                check(words_right <= version_column, "header words reach the version");
            }
            break;
        case kit::Role::heading:
            if (const auto* look = std::get_if<kit::HeadingLook>(&item.look))
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::small, look->text, metrics.heading_tracking
                    ) <= item.rect.width,
                    "heading \"" + look->text + "\""
                );
            break;
        case kit::Role::toggle:
            if (const auto* look = std::get_if<kit::SwitchLook>(&item.look)) {
                const int32_t half = std::max((item.rect.width - 2) / 2, int32_t{0});
                check(
                    width(kit::FontRole::small, look->off_caption) <= half,
                    "switch off \"" + look->off_caption + "\""
                );
                check(
                    width(kit::FontRole::small, look->on_caption) <= half,
                    "switch on \"" + look->on_caption + "\""
                );
            }
            break;
        case kit::Role::levels:
            if (const auto* look = std::get_if<kit::LevelsLook>(&item.look)) {
                for (const std::string& caption : look->captions) {
                    check(
                        width(kit::FontRole::small, caption) <= look->level_width,
                        "level \"" + caption + "\""
                    );
                }
            }
            break;
        default:
            break;
        }
    }
    return problems;
}

/// One level.
///
/// @param id the level id
/// @param name the name a player sees
/// @param difficulty the income
/// @return the level
inline picker::Level make_level(std::string id, std::string name, seats::Difficulty difficulty) {
    picker::Level made;
    made.id = std::move(id);
    made.name = std::move(name);
    made.difficulty = difficulty;
    return made;
}

/// A choice option. The values are also the captions.
///
/// @param id the option id
/// @param label the label
/// @param values the values
/// @param fallback the default
/// @return the option
inline mutators::OptionSpec choice_option(
    std::string id, std::string label, std::vector<std::string> values, std::string fallback
) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::choice;
    spec.label = std::move(label);
    spec.values = std::move(values);
    spec.captions = spec.values;
    spec.fallback = std::move(fallback);
    return spec;
}

/// A toggle, off then on.
///
/// @param id the option id
/// @param label the label
/// @param fallback the default
/// @return the option
inline mutators::OptionSpec toggle_option(std::string id, std::string label, std::string fallback) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::toggle;
    spec.label = std::move(label);
    spec.values = {"off", "on"};
    spec.captions = spec.values;
    spec.fallback = std::move(fallback);
    return spec;
}

/// A number option.
///
/// @param id the option id
/// @param label the label
/// @return the option
inline mutators::OptionSpec number_option(std::string id, std::string label) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::number;
    spec.label = std::move(label);
    spec.least = "0";
    spec.most = "4";
    spec.step = "2";
    spec.fallback = "0";
    return spec;
}

/// Steady Commander: three levels and one option of each form, including a
/// choice long enough to be a drop-down.
///
/// @return the brain
inline picker::Brain steady() {
    picker::Brain brain;
    brain.id = "steady-commander";
    brain.name = "Steady Commander";
    brain.version = "1.2";
    brain.author = "Ada";
    brain.summary = "Holds the line.";
    brain.description = "A commander that holds the ridge and waits.";
    brain.works_with_text = "Any mod";
    brain.levels = {
        make_level("gentle", "Gentle", seats::Difficulty::easy),
        make_level("firm", "Firm", seats::Difficulty::medium),
        make_level("fierce", "Fierce", seats::Difficulty::hard),
    };
    brain.options.push_back(choice_option("aggression", "Aggression", {"low", "high"}, "low"));
    brain.options.push_back(choice_option(
        "signal", "Signal", {"North", "South", "East", "West", "Near", "Far"}, "North"
    ));
    brain.options.push_back(toggle_option("hold", "Hold", "off"));
    brain.options.push_back(number_option("focus", "Focus"));
    return brain;
}

/// Lantern Scout sees the whole map and carries a network.
///
/// @return the brain
inline picker::Brain lantern() {
    picker::Brain brain;
    brain.id = "lantern-scout";
    brain.name = "Lantern Scout";
    brain.version = "0.4";
    brain.author = "Bea";
    brain.summary = "Scouts ahead.";
    brain.description = "Sees the map.";
    brain.full_awareness = true;
    brain.network_bytes = 1048576;
    brain.levels = {
        make_level("dim", "Dim", seats::Difficulty::easy),
        make_level("bright", "Bright", seats::Difficulty::hard),
    };
    return brain;
}

/// A fitting brain whose name is written in lower case.
///
/// @return the brain
inline picker::Brain quiet() {
    picker::Brain brain;
    brain.id = "quiet-garrison";
    brain.name = "quiet garrison";
    brain.summary = "Stays home.";
    brain.description = "One level.";
    brain.levels = {make_level("only", "Only", seats::Difficulty::hard)};
    return brain;
}

/// Ridge Raider cannot play this seat.
///
/// @return the brain
inline picker::Brain ridge() {
    picker::Brain brain;
    brain.id = "ridge-raider";
    brain.name = "Ridge Raider";
    brain.version = "3.0";
    brain.author = "Cara";
    brain.summary = "Raids.";
    brain.description = "Made for one mod.";
    brain.works_with_text = "Ridge";
    brain.unfit = "Made for Ridge";
    brain.levels = {make_level("raid", "Raid", seats::Difficulty::hard)};
    return brain;
}

/// Tide Keeper cannot play this seat.
///
/// @return the brain
inline picker::Brain tide() {
    picker::Brain brain;
    brain.id = "tide-keeper";
    brain.name = "Tide Keeper";
    brain.summary = "Holds the shore.";
    brain.description = "Needs an expansion.";
    brain.unfit = "Needs Core Contingency";
    brain.levels = {make_level("ebb", "Ebb", seats::Difficulty::easy)};
    return brain;
}

/// The five brains, not in name order.
///
/// @return the brains
inline std::vector<picker::Brain> roster() {
    return {steady(), tide(), lantern(), ridge(), quiet()};
}

/// Extra brains, enough that the list scrolls at every class.
///
/// @param count how many
/// @return the brains
inline std::vector<picker::Brain> crowd_brains(int count) {
    std::vector<picker::Brain> brains;
    for (int index = 1; index <= count; ++index) {
        picker::Brain brain;
        const std::string number = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        brain.id = "amber-" + number;
        brain.name = "Amber " + number;
        brain.summary = "Watches.";
        brain.description = "One of the crowd.";
        brain.levels = {make_level("only", "Only", seats::Difficulty::medium)};
        brains.push_back(std::move(brain));
    }
    return brains;
}

/// A brain with enough options that the details scroll at every class.
///
/// @return the brain
inline picker::Brain packed() {
    picker::Brain brain;
    brain.id = "packed-sentry";
    brain.name = "Packed Sentry";
    brain.summary = "Many switches.";
    brain.description = "A sentry with a switch for every watch.";
    brain.levels = {
        make_level("gentle", "Gentle", seats::Difficulty::easy),
        make_level("firm", "Firm", seats::Difficulty::medium),
        make_level("fierce", "Fierce", seats::Difficulty::hard),
    };
    for (int index = 1; index <= 20; ++index) {
        const std::string number = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        brain.options.push_back(toggle_option("watch-" + number, "Watch " + number, "off"));
    }
    return brain;
}

/// Fills the host fields the screen tests share.
///
/// @param brains the brains
/// @param text how texts are looked up
/// @param fixed true when the Difficulty button owns the built-in level
/// @param offer true when every computer seat is offered
/// @return the inputs, before the current choice
inline picker::Inputs
base_inputs(std::vector<picker::Brain> brains, lib::TextHooks text, bool fixed, bool offer) {
    picker::Inputs inputs;
    inputs.setup = seats::Setup::skirmish;
    inputs.seat_name = "North";
    inputs.mod_name = "No Mod";
    inputs.brains = std::move(brains);
    inputs.setup_difficulty = seats::Difficulty::medium;
    inputs.built_in_level_fixed = fixed;
    inputs.built_in_level_names = {"Easy", "Medium", "Hard"};
    inputs.offer_every_seat = offer;
    inputs.text = text;
    return inputs;
}

/// Opens a picker on a choice.
///
/// @param brains the brains
/// @param current the seat's choice
/// @param text how texts are looked up
/// @param fixed true when the Difficulty button owns the built-in level
/// @param offer true when every computer seat is offered
/// @return the picker
inline picker::Picker open_on(
    std::vector<picker::Brain> brains,
    picker::Choice current,
    lib::TextHooks text,
    bool fixed,
    bool offer
) {
    picker::Inputs inputs = base_inputs(std::move(brains), text, fixed, offer);
    inputs.current = std::move(current);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    return model;
}

/// Steady Commander's choice at Firm, with each option away from its default
/// where the screen tests want a visible change.
///
/// @return the choice
inline picker::Choice steady_choice() {
    picker::Choice choice;
    choice.brain = "steady-commander";
    choice.level = "firm";
    choice.options = {
        {"aggression", "low"},
        {"signal", "North"},
        {"hold", "off"},
        {"focus", "0"},
    };
    return choice;
}

/// The roster, opened on Steady Commander. Ridge Raider and Tide Keeper
/// cannot play. The picker offers every computer seat.
///
/// @param text how texts are looked up; empty shows the English
/// @return the picker
inline picker::Picker opened(lib::TextHooks text) {
    return open_on(roster(), steady_choice(), text, false, true);
}

/// The roster, opened on the built-in player, whose level the Difficulty
/// button owns.
///
/// @param text how texts are looked up
/// @return the picker
inline picker::Picker opened_note(lib::TextHooks text) {
    return open_on(roster(), {}, text, true, true);
}

/// The roster, opened on Ridge Raider, which cannot play, so the hint names
/// it and the choice is the built-in player.
///
/// @param text how texts are looked up
/// @return the picker
inline picker::Picker opened_left_out(lib::TextHooks text) {
    picker::Choice current;
    current.brain = "ridge-raider";
    current.level = "raid";
    return open_on(roster(), std::move(current), text, false, true);
}

/// No brain but the built-in player.
///
/// @param text how texts are looked up
/// @return the picker
inline picker::Picker opened_empty(lib::TextHooks text) {
    return open_on({}, {}, text, false, false);
}

/// The roster plus enough extra brains that the list scrolls.
///
/// @param text how texts are looked up
/// @return the picker
inline picker::Picker opened_crowd(lib::TextHooks text) {
    std::vector<picker::Brain> brains = roster();
    std::vector<picker::Brain> extra = crowd_brains(18);
    brains.insert(brains.end(), extra.begin(), extra.end());
    return open_on(std::move(brains), steady_choice(), text, false, true);
}

/// Packed Sentry, so the details scroll, opened on that brain.
///
/// @param text how texts are looked up
/// @return the picker
inline picker::Picker opened_packed(lib::TextHooks text) {
    picker::Choice current;
    current.brain = "packed-sentry";
    current.level = "firm";
    return open_on({packed()}, std::move(current), text, false, true);
}

} // namespace brain_fixture
