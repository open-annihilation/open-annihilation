// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker's pointer, a finger, the keys, the wheel and the
// controller, at Compact, Regular and Large. A click selects each row, a
// locked row is selected and not chosen, and a click changes each level,
// each option and the every-seat switch. A finger's tap selects. A finger's
// drag on the list scrolls and selects nothing. After every input the
// model's choice still fits a listed brain.

#include "oa/test/check.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::brain_picker;
namespace mutators = oa::ui::mutator_picker;
using brain_fixture::fonts;
using brain_fixture::has_room;
using brain_fixture::opened;
using brain_fixture::opened_crowd;
using brain_fixture::opened_packed;
using brain_fixture::press_area;

/// How far a finger reaches, in points.
constexpr int32_t finger_reach = 16;

/// The brain with this id, or null. Empty is not a listed brain.
///
/// @param model the picker
/// @param id the brain's id
/// @return the brain
const picker::Brain* brain_of(const picker::Picker& model, std::string_view id) {
    if (id.empty())
        return nullptr;
    for (const picker::Brain& brain : model.inputs.brains) {
        if (brain.id == id)
            return &brain;
    }
    return nullptr;
}

/// Tells whether a value is one an option allows.
///
/// @param spec the option
/// @param value the value
/// @return true when the model would keep it
bool allows(const mutators::OptionSpec& spec, std::string_view value) {
    if (spec.type == mutators::OptionSpec::Type::number) {
        for (const std::string& allowed : mutators::number_values(spec)) {
            if (allowed == value)
                return true;
        }
        return false;
    }
    for (const std::string& allowed : spec.values) {
        if (allowed == value)
            return true;
    }
    return false;
}

/// Tells whether the choice is one the model would keep, and the selection
/// is a listed row.
///
/// @param model the picker
/// @param where the step
void expect_fits(const picker::Picker& model, const char* where) {
    const picker::Choice choice = picker::result(model);
    bool choice_ok = choice.brain.empty();
    const picker::Brain* brain = brain_of(model, choice.brain);
    if (brain != nullptr)
        choice_ok = brain->unfit.empty();
    if (!choice_ok)
        std::fprintf(stderr, "%s: choice \"%s\" cannot play\n", where, choice.brain.c_str());
    OA_CHECK(choice_ok);

    bool level_ok = false;
    if (choice.brain.empty()) {
        level_ok = choice.level == "easy" || choice.level == "medium" || choice.level == "hard";
        if (model.inputs.built_in_level_fixed && choice.level != "medium" &&
            model.inputs.setup_difficulty == oa::data::brain_library::Difficulty::medium)
            level_ok = choice.level == "medium";
        if (model.inputs.built_in_level_fixed)
            level_ok = choice.level == "medium";
    } else if (brain != nullptr) {
        for (const picker::Level& level : brain->levels) {
            if (level.id == choice.level)
                level_ok = true;
        }
    }
    if (!level_ok)
        std::fprintf(
            stderr,
            "%s: level \"%s\" is not one \"%s\" has\n",
            where,
            choice.level.c_str(),
            choice.brain.c_str()
        );
    OA_CHECK(level_ok);

    bool options_ok = choice.brain.empty() ? choice.options.empty() : brain != nullptr;
    if (brain != nullptr) {
        options_ok = choice.options.size() == brain->options.size();
        for (std::size_t place = 0; options_ok && place < brain->options.size(); ++place) {
            options_ok = choice.options[place].first == brain->options[place].id &&
                         allows(brain->options[place], choice.options[place].second);
        }
    }
    if (!options_ok)
        std::fprintf(stderr, "%s: options do not fit \"%s\"\n", where, choice.brain.c_str());
    OA_CHECK(options_ok);

    if (model.every_seat && !model.inputs.offer_every_seat)
        std::fprintf(stderr, "%s: every seat is set where it is not offered\n", where);
    OA_CHECK(!model.every_seat || model.inputs.offer_every_seat);

    bool selected_ok = model.selected.empty();
    if (!selected_ok) {
        for (const picker::Brain& one : model.inputs.brains) {
            if (one.id == model.selected)
                selected_ok = true;
        }
    }
    if (!selected_ok)
        std::fprintf(stderr, "%s: \"%s\" is not a row\n", where, model.selected.c_str());
    OA_CHECK(selected_ok);
}

/// Returns a list's control of a name, or null.
///
/// @param list the display list
/// @param name the name
/// @return the control
const kit::Control* find_named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// Returns the centre of a rectangle.
///
/// @param rect the rectangle
/// @return a point inside it when it has room
kit::Point centre_of(const kit::Rect& rect) {
    return {rect.x + rect.width / 2, rect.y + rect.height / 2};
}

/// The screen and the input, laid out again whenever an input asks.
struct Session {
    picker::Picker model{};
    picker::ScreenState state{};
    kit::Frame frame{};
    kit::DisplayList list{};
    const char* where{"input"}; ///< the step the last check names

    /// Lays the screen out.
    void lay() { list = picker::picker_layout(model, state, frame, fonts()); }

    /// Checks the choice, then lays the screen out when the input asked.
    ///
    /// @param answered what the input returned
    void after(const picker::ScreenResult& answered) {
        expect_fits(model, where);
        if (answered.redraw)
            lay();
    }

    /// Takes a pointer event.
    ///
    /// @param kind a move, a press or a release
    /// @param button the button; 1 is the left
    /// @param at the pointer
    /// @param reach a finger's reach; 0 for a mouse
    /// @return what the host is to do
    picker::ScreenResult
    pointer(picker::PointerKind kind, uint8_t button, kit::Point at, int32_t reach) {
        const picker::ScreenResult answered =
            picker::picker_pointer(model, state, list, kind, button, at, reach);
        after(answered);
        return answered;
    }

    /// Presses and releases at one point.
    ///
    /// @param at the pointer
    /// @param reach a finger's reach; 0 for a mouse
    /// @return what the release asked, or the press when that asked something
    picker::ScreenResult click(kit::Point at, int32_t reach) {
        const picker::ScreenResult pressed = pointer(picker::PointerKind::down, 1, at, reach);
        const picker::ScreenResult released = pointer(picker::PointerKind::up, 1, at, reach);
        if (released.action != picker::PickerAction::none)
            return released;
        return pressed;
    }

    /// Takes a key.
    ///
    /// @param pressed the key
    /// @return what the host is to do
    picker::ScreenResult key(kit::Key pressed) {
        const picker::ScreenResult answered = picker::picker_key(model, state, list, pressed);
        after(answered);
        return answered;
    }

    /// Takes a turn of the wheel.
    ///
    /// @param at the pointer
    /// @param notches the turn
    /// @return what the host is to do
    picker::ScreenResult wheel(kit::Point at, float notches) {
        const picker::ScreenResult answered = picker::picker_wheel(model, state, list, at, notches);
        after(answered);
        return answered;
    }

    /// Takes a command.
    ///
    /// @param command the command
    /// @return what the host is to do
    picker::ScreenResult command(picker::ScreenCommand command) {
        const picker::ScreenResult answered = picker::picker_command(model, state, list, command);
        after(answered);
        return answered;
    }

    /// Takes the right stick.
    ///
    /// @param notches the turn
    /// @return what the host is to do
    picker::ScreenResult stick(float notches) {
        const picker::ScreenResult answered = picker::picker_stick(model, state, list, notches);
        after(answered);
        return answered;
    }
};

/// Opens a picker at a frame.
///
/// @param frame the frame
/// @param model the picker
/// @param where the case
/// @return the session
Session open_at(const kit::Frame& frame, picker::Picker model, const char* where) {
    Session session;
    session.model = std::move(model);
    session.frame = frame;
    session.where = where;
    session.lay();
    expect_fits(session.model, where);
    return session;
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

/// The name of the control that holds the focus, or empty.
///
/// @param session the session
/// @return the name
std::string focus_name(const Session& session) {
    if (!session.state.interaction.focus_shown)
        return {};
    const kit::Control* control = kit::control_of(session.list, session.state.interaction.focused);
    if (control == nullptr)
        return {};
    return control->name;
}

/// Scrolls a details control until the whole of it shows, or the walk ends.
///
/// @param[in,out] session the session
/// @param name the control
void reveal(Session& session, std::string_view name) {
    for (int step = 0; step < 80; ++step) {
        const kit::Control* control = find_named(session.list, name);
        if (control == nullptr)
            return;
        const kit::Rect port = control->clip;
        const bool whole =
            port.height <= 0 || (control->rect.y >= port.y &&
                                 control->rect.y + control->rect.height <= port.y + port.height);
        if (whole && has_room(press_area(*control)))
            return;
        if (port.height > 0 && control->rect.y + control->rect.height <= port.y)
            session.state.details_scroll = std::max(session.state.details_scroll - 24, int32_t{0});
        else
            session.state.details_scroll += 24;
        session.lay();
    }
}

/// Scrolls a details control to the top of its pane, so a drop-down under it
/// still lies in the window.
///
/// @param[in,out] session the session
/// @param name the control
void reveal_top(Session& session, std::string_view name) {
    const kit::Control* control = find_named(session.list, name);
    if (control == nullptr || control->clip.height <= 0)
        return;
    const int32_t local = control->rect.y - control->clip.y + session.state.details_scroll;
    session.state.details_scroll = std::max(local, int32_t{0});
    session.lay();
}

/// Opens Compact's details page. Regular and Large already show the details.
///
/// @param[in,out] session the session
void show_details(Session& session) {
    if (session.frame.size_class != kit::SizeClass::compact)
        return;
    const kit::Control* details = find_named(session.list, picker::name_details);
    OA_CHECK(details != nullptr);
    if (details == nullptr)
        return;
    session.where = "details";
    session.click(centre_of(press_area(*details)), 0);
    OA_CHECK(session.state.details_page);
    OA_CHECK(find_named(session.list, picker::name_back) != nullptr);
}

/// One value of the choice.
///
/// @param model the picker
/// @param id the option
/// @return the value; empty when the choice does not name it
std::string option_value(const picker::Picker& model, std::string_view id) {
    for (const auto& [option_id, value] : picker::result(model).options) {
        if (option_id == id)
            return value;
    }
    return {};
}

/// A point on one caption of a level strip.
///
/// @param list the display list
/// @param id the strip's control
/// @param index the caption
/// @return the point; nothing when the strip has no such caption
std::optional<kit::Point>
level_point(const kit::DisplayList& list, kit::ControlId id, std::size_t index) {
    for (const kit::Item& item : list.items) {
        if (item.control != id || item.role != kit::Role::levels)
            continue;
        const auto* look = std::get_if<kit::LevelsLook>(&item.look);
        if (look == nullptr || index >= look->captions.size())
            return std::nullopt;
        const int32_t top = item.rect.y;
        const int32_t bottom = item.rect.y + item.rect.height;
        for (int32_t column = item.rect.x; column < item.rect.x + item.rect.width; ++column) {
            if (kit::level_at(item.rect, look->captions.size(), look->level_width, column) != index)
                continue;
            return kit::Point{column, top + (bottom - top) / 2};
        }
    }
    return std::nullopt;
}

/// Clicks every row. A locked row is selected and is not the choice.
///
/// @param frame the frame
void click_rows(const kit::Frame& frame) {
    Session session = open_at(frame, opened({}), "rows");
    std::string choice = session.model.choice.brain;
    for (const picker::Row& row : picker::rows(session.model)) {
        const kit::Control* control = find_named(session.list, row_name(row.id));
        OA_CHECK(control != nullptr);
        if (control == nullptr)
            continue;
        const std::string step = row.id.empty() ? std::string("row built-in") : "row " + row.id;
        session.where = step.c_str();
        session.click(centre_of(press_area(*control)), 0);
        OA_CHECK(session.model.selected == row.id);
        if (row.locked) {
            OA_CHECK(session.model.choice.brain == choice);
            OA_CHECK(session.model.choice.brain != row.id);
        } else {
            OA_CHECK(session.model.choice.brain == row.id);
            choice = session.model.choice.brain;
        }
        OA_CHECK(focus_name(session) == picker::name_list);
    }
}

/// Clicks each level of the chosen brain, then each option and the switch.
///
/// @param frame the frame
void click_controls(const kit::Frame& frame) {
    Session session = open_at(frame, opened({}), "levels");
    show_details(session);
    reveal(session, picker::name_level);
    kit::ControlId level_id = kit::no_control;
    std::vector<std::string> level_parts;
    if (const kit::Control* level = find_named(session.list, picker::name_level)) {
        level_id = level->id;
        level_parts = level->parts;
    }
    OA_CHECK(level_id != kit::no_control);
    for (std::size_t index = 0; index < level_parts.size(); ++index) {
        const std::optional<kit::Point> at = level_point(session.list, level_id, index);
        OA_CHECK(at.has_value());
        if (!at)
            continue;
        const std::string step = "level " + std::to_string(index);
        session.where = step.c_str();
        const std::string part = level_parts[index];
        session.click(*at, 0);
        OA_CHECK(session.model.choice.level == part);
    }

    const char* strips[] = {"brains.option.aggression"};
    for (const char* name : strips) {
        const kit::Control* control = find_named(session.list, name);
        OA_CHECK(control != nullptr && control->kind == kit::ControlKind::levels);
        if (control == nullptr)
            continue;
        reveal(session, name);
        control = find_named(session.list, name);
        if (control == nullptr)
            continue;
        const std::string before = option_value(session.model, "aggression");
        const std::size_t other = before == "low" ? std::size_t{1} : std::size_t{0};
        const std::optional<kit::Point> at = level_point(session.list, control->id, other);
        OA_CHECK(at.has_value());
        if (!at)
            continue;
        session.where = "aggression";
        session.click(*at, 0);
        OA_CHECK(option_value(session.model, "aggression") == (other == 1 ? "high" : "low"));
    }

    const kit::Control* signal = find_named(session.list, "brains.option.signal");
    OA_CHECK(signal != nullptr && signal->kind == kit::ControlKind::choice);
    if (signal != nullptr) {
        reveal_top(session, "brains.option.signal");
        signal = find_named(session.list, "brains.option.signal");
    }
    if (signal != nullptr && has_room(press_area(*signal))) {
        session.where = "signal open";
        session.click(centre_of(press_area(*signal)), 0);
        OA_CHECK(session.state.menu == picker::Menu::choice);
        const kit::Control* south = find_named(session.list, "brains.option.signal.south");
        OA_CHECK(south != nullptr);
        if (south != nullptr && has_room(press_area(*south))) {
            session.where = "signal choose";
            session.click(centre_of(press_area(*south)), 0);
            OA_CHECK(option_value(session.model, "signal") == "South");
            OA_CHECK(session.state.menu == picker::Menu::none);
        }
    }

    const kit::Control* hold = find_named(session.list, "brains.option.hold");
    OA_CHECK(hold != nullptr && hold->kind == kit::ControlKind::toggle);
    if (hold != nullptr) {
        reveal(session, "brains.option.hold");
        hold = find_named(session.list, "brains.option.hold");
    }
    if (hold != nullptr && has_room(press_area(*hold))) {
        session.where = "hold";
        session.click(centre_of(press_area(*hold)), 0);
        OA_CHECK(option_value(session.model, "hold") == "on");
    }

    const kit::Control* focus = find_named(session.list, "brains.option.focus");
    OA_CHECK(focus != nullptr && focus->kind == kit::ControlKind::slider);
    if (focus != nullptr) {
        reveal(session, "brains.option.focus");
        focus = find_named(session.list, "brains.option.focus");
    }
    if (focus != nullptr && has_room(press_area(*focus))) {
        const kit::Point at{
            focus->rect.x + focus->rect.width - 1, focus->rect.y + focus->rect.height / 2
        };
        session.where = "focus";
        session.click(at, 0);
        const std::string value = option_value(session.model, "focus");
        OA_CHECK(value == "2" || value == "4");
    }

    const kit::Control* every = find_named(session.list, picker::name_every_seat);
    OA_CHECK(every != nullptr);
    if (every != nullptr) {
        reveal(session, picker::name_every_seat);
        every = find_named(session.list, picker::name_every_seat);
    }
    if (every != nullptr && has_room(press_area(*every))) {
        session.where = "every seat";
        session.click(centre_of(press_area(*every)), 0);
        OA_CHECK(session.model.every_seat);
    }
}

/// A finger's tap selects. A drag on the list scrolls and selects nothing.
///
/// @param frame the frame
void finger(const kit::Frame& frame) {
    Session tapped = open_at(frame, opened({}), "finger tap");
    const kit::Control* lantern = find_named(tapped.list, row_name("lantern-scout"));
    OA_CHECK(lantern != nullptr);
    if (lantern != nullptr) {
        const kit::Point at = centre_of(press_area(*lantern));
        tapped.pointer(picker::PointerKind::down, 1, at, finger_reach);
        OA_CHECK(tapped.model.selected == "steady-commander");
        tapped.pointer(picker::PointerKind::up, 1, at, finger_reach);
        OA_CHECK(tapped.model.selected == "lantern-scout");
        OA_CHECK(tapped.model.choice.brain == "lantern-scout");
    }

    Session dragged = open_at(frame, opened_crowd({}), "finger drag");
    const std::string selected = dragged.model.selected;
    const kit::Control* listed = find_named(dragged.list, picker::name_list);
    OA_CHECK(listed != nullptr);
    OA_CHECK(find_named(dragged.list, picker::name_list_bar) != nullptr);
    if (listed != nullptr) {
        const kit::Point from = centre_of(listed->rect);
        dragged.pointer(picker::PointerKind::down, 1, from, finger_reach);
        const kit::Point moved{from.x, from.y - 40};
        dragged.pointer(picker::PointerKind::move, 0, moved, finger_reach);
        dragged.pointer(picker::PointerKind::up, 1, moved, finger_reach);
        OA_CHECK(dragged.model.selected == selected);
        OA_CHECK(dragged.model.choice.brain == "steady-commander");
        OA_CHECK(dragged.state.list_scroll > 0);
    }
}

/// Tabs until a control's name is the one wanted, or the walk ends.
///
/// @param[in,out] session the session
/// @param name the name
/// @return true when the focus is there
bool tab_to(Session& session, std::string_view name) {
    for (int step = 0; step < 24; ++step) {
        if (focus_name(session) == name)
            return true;
        session.key(kit::Key::tab);
    }
    return focus_name(session) == name;
}

/// The keys of the brief: Tab, the list, the strips, Space, Enter and Escape.
///
/// @param frame the frame
void keys(const kit::Frame& frame) {
    const bool compact = frame.size_class == kit::SizeClass::compact;
    Session session = open_at(frame, opened({}), "tab");
    session.key(kit::Key::tab);
    OA_CHECK(focus_name(session) == picker::name_list);

    const std::string first = session.model.selected;
    session.where = "down";
    session.key(kit::Key::down);
    OA_CHECK(session.model.selected != first);
    OA_CHECK(focus_name(session) == picker::name_list);
    session.where = "up";
    session.key(kit::Key::up);
    OA_CHECK(session.model.selected == first);
    OA_CHECK(focus_name(session) == picker::name_list);

    session.where = "enter list";
    const picker::ScreenResult entered = session.key(kit::Key::enter);
    OA_CHECK(entered.action == picker::PickerAction::none);
    if (compact) {
        OA_CHECK(session.state.details_page);
        OA_CHECK(focus_name(session) == picker::name_level);
    } else {
        OA_CHECK(focus_name(session) == picker::name_level);
        OA_CHECK(!session.state.details_page);
    }

    session.where = "level keys";
    const std::string level_before = session.model.choice.level;
    session.key(kit::Key::right);
    OA_CHECK(session.model.choice.level != level_before);
    OA_CHECK(focus_name(session) == picker::name_level);
    session.key(kit::Key::left);
    OA_CHECK(session.model.choice.level == level_before);

    OA_CHECK(tab_to(session, "brains.option.aggression"));
    session.where = "aggression keys";
    session.key(kit::Key::right);
    OA_CHECK(option_value(session.model, "aggression") == "high");
    session.key(kit::Key::left);
    OA_CHECK(option_value(session.model, "aggression") == "low");

    OA_CHECK(tab_to(session, "brains.option.signal"));
    session.where = "menu keys";
    session.key(kit::Key::space);
    OA_CHECK(session.state.menu == picker::Menu::choice);
    const picker::ScreenResult closed = session.key(kit::Key::escape);
    OA_CHECK(closed.action == picker::PickerAction::none);
    OA_CHECK(session.state.menu == picker::Menu::none);
    OA_CHECK(option_value(session.model, "signal") == "North");

    OA_CHECK(tab_to(session, "brains.option.hold"));
    session.where = "hold keys";
    session.key(kit::Key::space);
    OA_CHECK(option_value(session.model, "hold") == "on");

    OA_CHECK(tab_to(session, "brains.option.focus"));
    session.where = "slider keys";
    session.key(kit::Key::right);
    OA_CHECK(option_value(session.model, "focus") == "2");
    session.key(kit::Key::left);
    OA_CHECK(option_value(session.model, "focus") == "0");

    OA_CHECK(tab_to(session, picker::name_every_seat));
    session.where = "switch keys";
    session.key(kit::Key::space);
    OA_CHECK(session.model.every_seat);
    session.key(kit::Key::left);
    OA_CHECK(!session.model.every_seat);
    session.key(kit::Key::right);
    OA_CHECK(session.model.every_seat);

    OA_CHECK(tab_to(session, picker::name_use));
    session.where = "use key";
    const picker::ScreenResult used = session.key(kit::Key::enter);
    OA_CHECK(used.action == picker::PickerAction::use);

    if (compact) {
        Session page = open_at(frame, opened({}), "escape page");
        page.key(kit::Key::tab);
        page.key(kit::Key::enter);
        OA_CHECK(page.state.details_page);
        const picker::ScreenResult left_page = page.key(kit::Key::escape);
        OA_CHECK(left_page.action == picker::PickerAction::none);
        OA_CHECK(!page.state.details_page);
        OA_CHECK(find_named(page.list, picker::name_details) != nullptr);
    }

    Session closing = open_at(frame, opened({}), "escape");
    closing.key(kit::Key::tab);
    const picker::ScreenResult cancelled = closing.key(kit::Key::escape);
    OA_CHECK(cancelled.action == picker::PickerAction::cancel);

    Session ends = open_at(frame, opened({}), "list ends");
    ends.key(kit::Key::tab);
    OA_CHECK(focus_name(ends) == picker::name_list);
    ends.where = "home";
    ends.key(kit::Key::home);
    const std::string at_home = ends.model.selected;
    ends.key(kit::Key::up);
    OA_CHECK(ends.model.selected == at_home);
    OA_CHECK(focus_name(ends) == picker::name_list);
    ends.where = "end";
    ends.key(kit::Key::end);
    const std::string at_end = ends.model.selected;
    ends.key(kit::Key::down);
    OA_CHECK(ends.model.selected == at_end);
    OA_CHECK(focus_name(ends) == picker::name_list);
    ends.where = "page";
    ends.key(kit::Key::page_up);
    OA_CHECK(focus_name(ends) == picker::name_list);
    ends.key(kit::Key::page_down);
    OA_CHECK(focus_name(ends) == picker::name_list);

    Session backward = open_at(frame, opened({}), "back tab");
    backward.key(kit::Key::tab);
    backward.key(kit::Key::tab);
    const std::string second = focus_name(backward);
    backward.key(kit::Key::back_tab);
    OA_CHECK(focus_name(backward) == picker::name_list);
    backward.key(kit::Key::tab);
    OA_CHECK(focus_name(backward) == second);
}

/// The controller's back and use, and the footer buttons.
///
/// @param frame the frame
void commands(const kit::Frame& frame) {
    Session used = open_at(frame, opened({}), "command use");
    const picker::ScreenResult use = used.command(picker::ScreenCommand::use);
    OA_CHECK(use.action == picker::PickerAction::use);

    Session backed = open_at(frame, opened({}), "command back");
    const picker::ScreenResult back = backed.command(picker::ScreenCommand::back);
    OA_CHECK(back.action == picker::PickerAction::cancel);

    if (frame.size_class == kit::SizeClass::compact) {
        Session page = open_at(frame, opened({}), "command page");
        page.key(kit::Key::tab);
        page.key(kit::Key::enter);
        OA_CHECK(page.state.details_page);
        const picker::ScreenResult left_page = page.command(picker::ScreenCommand::back);
        OA_CHECK(left_page.action == picker::PickerAction::none);
        OA_CHECK(!page.state.details_page);
    }

    Session buttons = open_at(frame, opened({}), "footer");
    if (frame.size_class == kit::SizeClass::compact) {
        const kit::Control* details = find_named(buttons.list, picker::name_details);
        OA_CHECK(details != nullptr);
        if (details != nullptr)
            buttons.click(centre_of(press_area(*details)), 0);
        OA_CHECK(buttons.state.details_page);
        const kit::Control* back_button = find_named(buttons.list, picker::name_back);
        OA_CHECK(back_button != nullptr);
        if (back_button != nullptr)
            buttons.click(centre_of(press_area(*back_button)), 0);
        OA_CHECK(!buttons.state.details_page);
    }
    const kit::Control* cancel = find_named(buttons.list, picker::name_cancel);
    OA_CHECK(cancel != nullptr);
    if (cancel != nullptr) {
        const picker::ScreenResult answered = buttons.click(centre_of(press_area(*cancel)), 0);
        OA_CHECK(answered.action == picker::PickerAction::cancel);
    }

    Session closing = open_at(frame, opened({}), "close");
    const kit::Control* mark = find_named(closing.list, picker::name_close);
    OA_CHECK(mark != nullptr);
    if (mark != nullptr) {
        const picker::ScreenResult answered = closing.click(centre_of(press_area(*mark)), 0);
        OA_CHECK(answered.action == picker::PickerAction::cancel);
    }

    Session using_button = open_at(frame, opened({}), "use button");
    const kit::Control* use_button = find_named(using_button.list, picker::name_use);
    OA_CHECK(use_button != nullptr);
    if (use_button != nullptr) {
        const picker::ScreenResult answered =
            using_button.click(centre_of(press_area(*use_button)), 0);
        OA_CHECK(answered.action == picker::PickerAction::use);
    }
}

/// The wheel scrolls the pane under the pointer. The stick scrolls the pane
/// that holds the focus.
///
/// @param frame the frame
void wheel_and_stick(const kit::Frame& frame) {
    Session listed = open_at(frame, opened_crowd({}), "wheel list");
    const kit::Control* list = find_named(listed.list, picker::name_list);
    OA_CHECK(list != nullptr);
    if (list != nullptr) {
        listed.wheel(centre_of(list->rect), -1.0F);
        OA_CHECK(listed.state.list_scroll > 0);
    }
    listed.where = "stick list";
    listed.key(kit::Key::tab);
    OA_CHECK(focus_name(listed) == picker::name_list);
    const int32_t before = listed.state.list_scroll;
    listed.stick(-1.0F);
    OA_CHECK(listed.state.list_scroll > before);

    Session details = open_at(frame, opened_packed({}), "wheel details");
    show_details(details);
    OA_CHECK(find_named(details.list, picker::name_details_bar) != nullptr);
    const kit::Control* name = find_named(details.list, picker::name_brain);
    OA_CHECK(name != nullptr);
    if (name != nullptr) {
        const kit::Rect port = name->clip.width > 0 ? name->clip : name->rect;
        details.wheel(centre_of(port), -1.0F);
        OA_CHECK(details.state.details_scroll > 0);
    }
}

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

} // namespace

int main() {
    const kit::Frame sizes[] = {
        window_frame(640, 480, 100),
        window_frame(1920, 1080, 200),
        window_frame(2560, 1440, 200),
    };
    for (const kit::Frame& frame : sizes) {
        click_rows(frame);
        click_controls(frame);
        finger(frame);
        keys(frame);
        commands(frame);
        wheel_and_stick(frame);
    }
    return oa::test::check_exit_status();
}
