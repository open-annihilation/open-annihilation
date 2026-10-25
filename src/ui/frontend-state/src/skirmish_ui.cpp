// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_state/skirmish_ui.hpp"
#include "oa/core/side.h"
#include "oa/data/match_rules/difficulty_names.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace oa::ui::frontend_state::skirmish_ui {
namespace {
constexpr uint16_t empty_alliance_frame = 10;
constexpr int32_t left_button = 1, right_button = 2;
constexpr uint32_t raised_button_flags = 2, resource_button_flag = 0x10000;
constexpr int32_t resource_floor_step_artifact = resource_minimum + resource_step;
constexpr std::string_view side_fields[] = {"Side", "Allies", "Metal", "Energy", "Color"};

void validate(const Settings& s) {
    if (s.slot_count < 0 || s.slot_count > static_cast<int32_t>(s.slots.size()))
        throw std::invalid_argument("skirmish slot count outside the slot storage");
}

game_entry::SkirmishSlot& slot(Settings& s, int32_t index) {
    validate(s);
    if (index < 0 || index >= s.slot_count)
        throw std::out_of_range("skirmish slot index");
    return s.slots[static_cast<std::size_t>(index)];
}

int32_t add_wrapped(int32_t a, int32_t b) {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

std::string widget(std::string_view prefix, int32_t index) {
    return std::string(prefix) + std::to_string(index);
}

void enable_slot(int32_t index, bool enabled, Host& h) {
    h.set_enabled(widget("Player", index), 1);
    for (auto field : side_fields)
        h.set_enabled(widget(field, index), enabled ? 1 : 0);
}

std::string_view controller_text(int32_t controller) {
    switch (controller) {
    case game_entry::controller::disabled:
        return "Open";
    case game_entry::controller::human:
        return "Player";
    case game_entry::controller::computer:
        return "Computer";
    default:
        return {};
    }
}

void tooltip(Host& h, std::string_view name, std::string_view text) {
    h.set_tooltip(name, h.translate_ui(text));
}

std::string_view los_tooltip(const Preferences& p) {
    if (p.skirmish.line_of_sight == 0)
        return "All mapped terrain is visible.";
    if (p.skirmish.los_type == 1)
        return "Terrain elevations affect a unit's view.";
    return "Terrain elevations do not affect a unit's view.";
}

void boolean_tooltip(Preferences& p, Button b, Host& h) {
    switch (b) {
    case Button::commander_death:
        tooltip(
            h,
            resource_name(b),
            p.skirmish.commander_death == 0 ? "Game continues after Commander is destroyed."
                                            : "Game ends when commander is destroyed."
        );
        break;
    case Button::start_location:
        tooltip(
            h,
            resource_name(b),
            p.skirmish_location == 0 ? "Commanders are randomly placed on the battle field."
                                     : "Commanders are placed at pre-determined locations."
        );
        break;
    case Button::mapping:
        tooltip(
            h,
            resource_name(b),
            p.skirmish.mapping == 0 ? "Terrain is visible."
                                    : "Terrain is blacked out until explored."
        );
        break;
    case Button::line_of_sight:
        tooltip(h, resource_name(b), los_tooltip(p));
        break;
    default:
        throw std::logic_error("not a skirmish rule button");
    }
}
} // namespace

bool all_slots_disabled(const Settings& s) {
    validate(s);
    for (int32_t i = 0; i < s.slot_count; ++i)
        if (s.slots[static_cast<std::size_t>(i)].controller != 0)
            return false;
    return true;
}

bool color_in_use(const Settings& s, int32_t color, int32_t excluded) {
    validate(s);
    for (int32_t i = 0; i < s.slot_count; ++i) {
        const auto& value = s.slots[static_cast<std::size_t>(i)];
        if (i != excluded && value.controller != 0 && value.color == color)
            return true;
    }
    return false;
}

int32_t first_unused_color(const Settings& s) {
    validate(s);
    for (int32_t color = 0; color < color_search_limit; ++color) {
        bool found = false;
        for (int32_t i = 0; i < s.slot_count; ++i)
            if (s.slots[static_cast<std::size_t>(i)].color == color) {
                found = true;
                break;
            }
        if (!found)
            return color;
    }
    return -1;
}

int32_t alliance_members(const Settings& s, int32_t alliance) {
    validate(s);
    int32_t count = 0;
    for (int32_t i = 0; i < s.slot_count; ++i) {
        const auto& value = s.slots[static_cast<std::size_t>(i)];
        if (value.controller != 0 && value.alliance == alliance)
            ++count;
    }
    return count;
}

void update_alliance_images(const Settings& s, Host& h) {
    validate(s);
    for (int32_t i = 0; i < s.slot_count; ++i) {
        const auto alliance = s.slots[static_cast<std::size_t>(i)].alliance;
        const auto count = alliance_members(s, alliance);
        const auto frame = count == 0
                               ? empty_alliance_frame
                               : static_cast<uint16_t>(
                                     static_cast<uint32_t>(alliance) * 2U + (count == 1 ? 1U : 0U)
                                 );
        h.set_image_frame(widget("Allies", i), frame);
    }
    h.invalidate_menu();
}

/// The sides a slot's Side button never offers.
///
/// A computer slot also skips the sides computer players may not take.
///
/// @param ui Panel state.
/// @param value The slot.
/// @return bit n set for a side the button never offers
uint32_t slot_hidden_sides(const UiState& ui, const game_entry::SkirmishSlot& value) noexcept {
    uint32_t hidden = ui.hidden_sides;
    if (value.controller == game_entry::controller::computer)
        hidden |= ui.computer_hidden_sides;
    return hidden;
}

void cycle_player(Settings& s, int32_t index, const UiState& ui, Host& h) {
    auto& value = slot(s, index);
    const int32_t before = value.controller;
    switch (value.controller) {
    case game_entry::controller::disabled:
        value.controller = game_entry::controller::computer;
        break;
    case game_entry::controller::human:
        value.controller = game_entry::controller::disabled;
        break;
    case game_entry::controller::computer:
        value.controller = game_entry::human_count(s) == 0 ? game_entry::controller::human
                                                           : game_entry::controller::disabled;
        break;
    default:
        break;
    }
    if (value.controller >= 0 && value.controller <= 2)
        h.set_text(
            h.frontend_menu(),
            widget("Player", index),
            h.translate_ui(controller_text(value.controller)),
            0
        );
    if (value.controller != 0 && color_in_use(s, value.color, index)) {
        value.color = first_unused_color(s);
        h.set_image(
            widget("Color", index), Sprite::player_colors, static_cast<uint16_t>(value.color)
        );
    }
    if (value.controller == game_entry::controller::computer &&
        before != game_entry::controller::computer) {
        value.side = offered_side(value.side, ui.side_count, slot_hidden_sides(ui, value));
        h.set_side_stage(widget("Side", index), static_cast<uint8_t>(value.side));
    }
    enable_slot(index, value.controller != 0, h);
    update_alliance_images(s, h);
}

void cycle_color(Settings& s, int32_t index, bool reverse, Host& h) {
    auto& value = slot(s, index);
    const auto count = h.color_frame_count();
    if (count == 0)
        throw std::invalid_argument("empty player color animation");
    // Settings with every color occupied are refused; valid forward and
    // reverse selection is unchanged.
    for (uint32_t tries = 0; tries < count; ++tries) {
        value.color = add_wrapped(value.color, reverse ? -1 : 1) % count;
        if (value.color == -1)
            value.color = count - 1;
        if (!color_in_use(s, value.color, index)) {
            h.set_image(
                widget("Color", index), Sprite::player_colors, static_cast<uint16_t>(value.color)
            );
            h.invalidate_menu();
            return;
        }
    }
    throw std::invalid_argument("all player colors occupied");
}

void create_slot_widgets(const Settings& s, Host& h) {
    validate(s);
    if (s.slot_count == 0)
        throw std::invalid_argument("skirmish layout requires player slots");
    constexpr int32_t available_height = 200, row_center_span = 180, top_offset = 79;
    constexpr int16_t row_height = 20;
    const auto spacing = available_height / s.slot_count;
    auto y = (row_center_span - (s.slot_count - 1) * spacing) / 2 + top_offset;
    for (int32_t i = 0; i < s.slot_count; ++i, y += spacing) {
        auto emit = [&](WidgetKind kind,
                        std::string_view name,
                        int16_t x,
                        int16_t width,
                        uint32_t flags,
                        uint8_t stages,
                        std::string_view sprite,
                        std::string_view help) {
            h.create_slot_widget(
                {kind,
                 widget(name, i),
                 x,
                 static_cast<int16_t>(y),
                 width,
                 row_height,
                 flags,
                 stages,
                 sprite,
                 help.empty() ? std::string{} : h.translate_ui(help)}
            );
        };
        emit(WidgetKind::button, "Player", 45, 112, raised_button_flags, 0, "skirmname", "");
        emit(WidgetKind::button, "Side", 163, 45, raised_button_flags, 2, "SIDEx", "");
        emit(WidgetKind::image, "Color", 214, 20, 0, 0, "", "");
        emit(
            WidgetKind::image, "Allies", 241, 40, 0, 0, "", "Click to select an allegiance symbol."
        );
        emit(
            WidgetKind::button,
            "Metal",
            286,
            45,
            raised_button_flags | resource_button_flag,
            0,
            "skirmmet",
            "Left click to increase metal. Right click to decrease metal."
        );
        emit(
            WidgetKind::button,
            "Energy",
            337,
            45,
            raised_button_flags | resource_button_flag,
            0,
            "skirmmet",
            "Left click to increase energy. Right click to decrease energy."
        );
    }
}

void populate(Settings& s, Preferences& p, UiState& ui, Host& h) {
    validate(s);
    ui.base_widget_count = h.widget_count();
    create_slot_widgets(s, h);
    if (all_slots_disabled(s)) {
        // The first two slots are written even when fewer visible rows
        // are selected. Storage always contains eleven slots.
        s.slots[0].controller = game_entry::controller::human;
        s.slots[1].controller = game_entry::controller::computer;
    }
    const auto team_frames = h.team_icon_frame_count();
    if (team_frames)
        for (uint32_t i = 0; i < *team_frames; ++i)
            h.zero_team_icon_frame_origin(0); // Frame zero is fetched every time.
    for (int32_t i = 0; i < s.slot_count; ++i) {
        auto& value = s.slots[static_cast<std::size_t>(i)];
        value.side = offered_side(value.side, ui.side_count, slot_hidden_sides(ui, value));
        if (value.controller >= 0 && value.controller <= 2)
            h.set_text(
                h.frontend_menu(),
                widget("Player", i),
                value.controller == 0 ? std::string(controller_text(value.controller))
                                      : h.translate_ui(controller_text(value.controller)),
                0
            );
        if (value.controller == 0)
            enable_slot(i, false, h);
        h.set_side_stage(widget("Side", i), static_cast<uint8_t>(value.side));
        h.set_text(h.frontend_menu(), widget("Metal", i), std::to_string(value.metal), 0);
        h.set_text(h.frontend_menu(), widget("Energy", i), std::to_string(value.energy), 0);
        h.set_image(widget("Color", i), Sprite::player_colors, static_cast<uint16_t>(value.color));
        h.set_image(widget("Allies", i), Sprite::team_icons, empty_alliance_frame);
    }
    update_alliance_images(s, h);
    h.set_button_stage("StartLocation", p.skirmish_location == 0 ? 1 : 0);
    boolean_tooltip(p, Button::start_location, h);
    h.set_button_stage("CommanderDeath", p.skirmish.commander_death == 0 ? 1 : 0);
    boolean_tooltip(p, Button::commander_death, h);
    h.set_button_stage("Mapping", p.skirmish.mapping == 0 ? 1 : 0);
    boolean_tooltip(p, Button::mapping, h);
    h.set_button_stage(
        "LineOfSight",
        p.skirmish.line_of_sight == 0 ? 0
        : p.skirmish.los_type == 1    ? 1
                                      : 2
    );
    boolean_tooltip(p, Button::line_of_sight, h);
    h.set_text(h.frontend_menu(), "MapName", s.map_name, 0);
    h.invalidate_menu();
}

void setup(State&, Settings& s, Preferences& p, UiState& ui, Host& h) {
    h.clear_backbuffer();
    const auto menu = h.load_menu("SKIRMISH.GUI");
    h.install_event_callback(menu);
    h.load_background("Skirmsetup4x");
    p.difficulty = p.skirmish_difficulty;
    if (p.difficulty <= 2) {
        constexpr std::string_view names[] = {"Easy", "Medium", "Hard"};
        h.set_button_stage("Difficulty", static_cast<uint8_t>(p.difficulty));
        h.select_difficulty_label(
            names[data::match_rules::difficulty_name_index(
                h.difficulty_names(), static_cast<int32_t>(p.difficulty)
            )],
            1
        );
    }
    h.invalidate_menu();
    if (h.select_map(s.map_name) == 0) {
        h.select_map_index(0);
        const auto name = h.selected_map_name();
        if (name.size() >= 256 || name.find('\0') != std::string::npos)
            throw std::invalid_argument("selected map name exceeds the game's string field");
        s.map_name = name;
    }
    populate(s, p, ui, h);
    h.install_input_callback();
    h.set_input_enabled(1);
    h.add_menu_flags(0x40);
    h.select_cursor_animation(active_cursor_index);
}

int32_t offered_side(int32_t side, int32_t side_count, uint32_t hidden_sides) noexcept {
    const auto hidden = [&](int32_t at) {
        return at >= 0 && at < 32 && (hidden_sides & (1U << static_cast<uint32_t>(at))) != 0;
    };
    if (side_count <= 0)
        return side;
    // A side past the loaded ones, saved while another mod with more sides
    // was played, wraps to the first, as the Side button does after the last.
    const int32_t first = side >= 0 && side < side_count ? side : 0;
    if (!hidden(first))
        return first;
    for (int32_t step = 1; step < side_count; ++step) {
        const int32_t next = add_wrapped(first, step) % side_count;
        if (!hidden(next))
            return next;
    }
    return first;
}

void advance_side(Settings& s, int32_t index, int32_t side_count, uint32_t hidden_sides) {
    if (side_count <= 0)
        throw std::invalid_argument("empty side table");
    auto& value = slot(s, index);
    const int32_t next = add_wrapped(value.side, 1) % side_count;
    value.side = offered_side(next, side_count, hidden_sides);
}

void retreat_side(Settings& s, int32_t index, int32_t side_count, uint32_t hidden_sides) {
    if (side_count <= 0)
        throw std::invalid_argument("empty side table");
    auto& value = slot(s, index);
    const auto hidden = [&](int32_t at) {
        return at >= 0 && at < 32 && (hidden_sides & (1U << static_cast<uint32_t>(at))) != 0;
    };
    // offered_side of a negative index opens on side 0, so a step back walks
    // the positive indices itself.
    const auto wrapped_before = [&](int32_t step) {
        int32_t previous = value.side - step;
        previous %= side_count;
        if (previous < 0)
            previous += side_count;
        return previous;
    };
    for (int32_t step = 1; step < side_count; ++step) {
        const int32_t previous = wrapped_before(step);
        if (!hidden(previous)) {
            value.side = previous;
            return;
        }
    }
    // Nothing earlier is offered. Stay on the one side that is. When this
    // side is hidden too, step back one, as when every side is hidden.
    if (value.side >= 0 && value.side < side_count && !hidden(value.side))
        return;
    value.side = wrapped_before(1);
}

void cycle_alliance(Settings& s, int32_t index, Host& h) {
    auto& value = slot(s, index);
    value.alliance = add_wrapped(value.alliance, 1) % alliance_count;
    update_alliance_images(s, h);
}

void handle_player_count_code(
    State& state,
    Settings& s,
    Preferences& p,
    UiState& ui,
    TypedKeys& typed,
    Host& h,
    initialization::PreferencesHost& preferences
) {
    // Each code is matched against the newest keys, the end of the history.
    const auto ends_with = [&typed](std::string_view code) {
        return std::equal(
            code.begin(), code.end(), typed.end() - static_cast<std::ptrdiff_t>(code.size())
        );
    };
    int32_t count = default_player_count;
    bool clear_history = true;
    if (ends_with("*IV")) {
        count = 4;
    } else if (ends_with("*V")) {
        count = 5;
        clear_history = false;
    } else if (ends_with("*VI")) {
        count = 6;
        clear_history = false;
    } else if (ends_with("*VII")) {
        count = 7;
        clear_history = false;
    } else if (ends_with("*VIII")) {
        count = 8;
    } else if (!ends_with("*III")) {
        if (ends_with("*IX"))
            count = 9;
        else if (ends_with("*X"))
            count = 10;
        else
            return;
    }
    if (clear_history)
        typed.fill(0);
    h.save_preferences();
    initialization::set_skirmish_player_count(s, count, preferences);
    initialization::load_preferences(state, s, p, preferences);
    h.set_widget_count(static_cast<int16_t>(ui.base_widget_count));
    populate(s, p, ui, h);
    h.play_ui_sound(player_count_sound, 0);
    h.invalidate_menu();
}

bool handle_event(
    State& state, Settings& s, Preferences& p, UiState& ui, const game_entry::Event& event, Host& h
) {
    if (event.code == -1)
        return false;
    auto selected = h.selected_widget_name(event).substr(0, 16);
    if (selected.empty())
        throw std::invalid_argument("empty skirmish selected widget name");
    const char last = selected.back();
    ui.selected_slot = last >= '0' && last <= '9' ? last - '0' : 0;
    selected.pop_back();
    if (h.button_result(event.menu, Button::start) != 0)
        return game_entry::start_selected_skirmish(state, s, event.menu, h);
    if (h.button_result(event.menu, Button::previous_menu) != 0) {
        h.play_ui_sound("Previous", 0);
        h.select_cursor_animation(transition_cursor_index);
        state.pending_signal = signal_id::back;
        return false;
    }
    auto sound = [&] { h.play_ui_sound("Skirmish", 0); };
    if (selected == "Player") {
        sound();
        cycle_player(s, ui.selected_slot, ui, h);
    } else if (selected == "Side") {
        sound();
        const uint32_t hidden = slot_hidden_sides(ui, slot(s, ui.selected_slot));
        // Past 3.1c's five sides a right click steps back; fewer still go forward.
        if (h.event_button(event.menu) == right_button && ui.side_count > OA_SIDE_COUNT)
            retreat_side(s, ui.selected_slot, ui.side_count, hidden);
        else
            advance_side(s, ui.selected_slot, ui.side_count, hidden);
        // The button's own two stages wrap at Core; its frame follows the
        // side instead, so a third or later SIDEDATA side shows its own art.
        h.set_side_stage(
            widget("Side", ui.selected_slot), static_cast<uint8_t>(slot(s, ui.selected_slot).side)
        );
    } else if (selected == "Allies") {
        sound();
        cycle_alliance(s, ui.selected_slot, h);
    } else if (selected == "Color") {
        sound();
        h.capture_input();
        if (h.event_button(event.menu) == left_button)
            cycle_color(s, ui.selected_slot, false, h);
        if (h.event_button(event.menu) == right_button)
            cycle_color(s, ui.selected_slot, true, h);
    } else if (selected == "Energy" || selected == "Metal") {
        if (selected == "Energy")
            h.capture_input();
        auto& value = slot(s, ui.selected_slot);
        auto& amount = selected == "Energy" ? value.energy : value.metal;
        auto update = [&](bool increase) {
            sound();
            amount = increase ? std::min(add_wrapped(amount, resource_step), resource_maximum)
                              : std::max(add_wrapped(amount, -resource_step), resource_minimum);
            if (increase && amount == resource_floor_step_artifact)
                amount = resource_step;
            h.set_text(event.menu, widget(selected, ui.selected_slot), std::to_string(amount), 10);
        };
        if (h.event_button(event.menu) == left_button)
            update(true);
        if (h.event_button(event.menu) == right_button)
            update(false);
    } else if (h.button_result(event.menu, Button::commander_death) != 0) {
        sound();
        p.skirmish.commander_death ^= 1;
        boolean_tooltip(p, Button::commander_death, h);
        h.refresh_help_text();
    } else if (h.button_result(event.menu, Button::start_location) != 0) {
        sound();
        p.skirmish_location ^= 1;
        boolean_tooltip(p, Button::start_location, h);
        h.refresh_help_text();
    } else if (h.button_result(event.menu, Button::mapping) != 0) {
        sound();
        p.skirmish.mapping ^= 1;
        boolean_tooltip(p, Button::mapping, h);
        h.refresh_help_text();
    } else if (h.button_result(event.menu, Button::line_of_sight) != 0) {
        sound();
        if (p.skirmish.line_of_sight == 0) {
            p.skirmish.line_of_sight = 1;
            p.skirmish.los_type = 1;
        } else if (p.skirmish.los_type == 1)
            p.skirmish.los_type = 0;
        else {
            p.skirmish.line_of_sight = 0;
            p.skirmish.los_type = 1;
        }
        boolean_tooltip(p, Button::line_of_sight, h);
        h.refresh_help_text();
    } else if (h.button_result(event.menu, Button::select_map) != 0) {
        sound();
        h.select_cursor_animation(transition_cursor_index);
        h.open_map_selection();
    } else if (h.button_result(event.menu, Button::difficulty) != 0) {
        h.play_ui_sound("SKirmish", 0);
        if (p.difficulty <= 2) {
            p.difficulty = (p.difficulty + 1) % 3;
            p.skirmish_difficulty = p.difficulty;
        }
    }
    h.clear_event_selection(event.menu);
    return false;
}

} // namespace oa::ui::frontend_state::skirmish_ui
