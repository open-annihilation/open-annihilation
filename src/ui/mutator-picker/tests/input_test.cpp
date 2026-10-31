// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's input at Compact, Regular and Large: a mouse drag, a
// finger, the keys, the controller's commands, the search, the option
// controls and read-only. After every input, every prerequisite still stands
// above the On mutators that need it.
//
// The synthetic font is picker_fixture::fonts.

#include "oa/test/check.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
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

/// The On ids, top to bottom.
///
/// @param model the picker
/// @return the ids
std::vector<std::string> ids_of(const picker::Picker& model) {
    std::vector<std::string> ids;
    for (const picker::Chosen& chosen : model.on)
        ids.push_back(chosen.id);
    return ids;
}

/// The prerequisite graph of the inputs.
///
/// @param model the picker
/// @return the graph
std::vector<lib::Need> graph_of(const picker::Picker& model) {
    std::vector<lib::Need> graph;
    for (const picker::Mutator& mutator : model.inputs.mutators) {
        lib::Need record;
        record.id = mutator.id;
        for (const lib::Prerequisite& prerequisite : mutator.needs)
            record.needs.push_back(prerequisite.id);
        graph.push_back(std::move(record));
    }
    return graph;
}

/// Checks that every prerequisite stands above the On mutators that need it.
///
/// Called after every input.
///
/// @param model the picker
void expect_prerequisites(const picker::Picker& model) {
    OA_CHECK(lib::prerequisites_above(graph_of(model), ids_of(model)));
}

/// Checks the On ids, and prints them when they differ.
///
/// @param model the picker
/// @param expected the ids, top to bottom
/// @param where the case
void expect_ids(
    const picker::Picker& model, const std::vector<std::string>& expected, const char* where
) {
    const std::vector<std::string> ids = ids_of(model);
    if (ids != expected) {
        std::fprintf(stderr, "%s on list:", where);
        for (const std::string& id : ids)
            std::fprintf(stderr, " %s", id.c_str());
        std::fprintf(stderr, "\n%s expected:", where);
        for (const std::string& id : expected)
            std::fprintf(stderr, " %s", id.c_str());
        std::fprintf(stderr, "\n");
    }
    OA_CHECK(ids == expected);
}

/// Returns a list's control of a name, or null.
///
/// @param list the display list
/// @param name the name
/// @return the control
const kit::Control* find_named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// Tells whether a press can reach a control.
///
/// @param control the control
/// @return true when some of its press area shows
bool shown_press(const kit::Control* control) {
    return control != nullptr && picker_fixture::has_room(picker_fixture::press_area(*control));
}

/// Returns the centre of a rectangle. The right and bottom edges are outside it.
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

    /// Lays the screen out.
    void lay() { list = picker::picker_layout(model, state, frame, fonts()); }

    /// Checks the order, then lays the screen out when the input asked.
    ///
    /// @param answered what the input returned
    void after(const picker::ScreenResult& answered) {
        expect_prerequisites(model);
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

    /// Takes a key.
    ///
    /// @param pressed the key
    /// @return what the host is to do
    picker::ScreenResult key(kit::Key pressed) {
        const picker::ScreenResult answered = picker::picker_key(model, state, list, pressed);
        after(answered);
        return answered;
    }

    /// Takes typed text.
    ///
    /// @param utf8 the text
    /// @return what the host is to do
    picker::ScreenResult text(std::string_view utf8) {
        const picker::ScreenResult answered = picker::picker_text(model, state, list, utf8);
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

/// Opens the Ridge fixture at a frame.
///
/// @param frame the frame
/// @param read_only true when a guest is looking
/// @return the session
Session open_at(const kit::Frame& frame, bool read_only) {
    Session session;
    session.model = picker_fixture::opened({}, read_only);
    session.frame = frame;
    session.lay();
    expect_prerequisites(session.model);
    return session;
}

/// A row's automation name.
///
/// @param id the mutator's id
/// @return the name
std::string row_name(std::string_view id) {
    return std::string(picker::name_row_prefix) + std::string(id);
}

/// The list's port, or an empty rectangle.
///
/// @param session the session
/// @return the port
kit::Rect list_port(const Session& session) {
    const kit::Control* listed = find_named(session.list, picker::name_list);
    if (listed == nullptr)
        return {};
    return listed->rect;
}

/// Scrolls a control into view. Details controls scroll the details.
///
/// @param[in,out] session the session
/// @param name the control
void reveal(Session& session, std::string_view name) {
    const bool details = name == picker::name_up || name == picker::name_down ||
                         name == picker::name_clash || name == picker::name_back ||
                         name.rfind(picker::name_option_prefix, 0) == 0;
    for (int step = 0; step < 80; ++step) {
        const kit::Control* control = find_named(session.list, name);
        if (control == nullptr || shown_press(control))
            return;
        const kit::Rect port = details ? control->clip : list_port(session);
        int32_t& offset = details ? session.state.details_scroll : session.state.list_scroll;
        if (port.height > 0 && control->rect.y + control->rect.height <= port.y)
            offset = std::max(offset - 24, int32_t{0});
        else
            offset += 24;
        session.lay();
    }
}

/// The On row at a place, from the top.
///
/// @param session the session
/// @param place the place
/// @return the row; null when the list has none there
const kit::Control* on_row_at(const Session& session, std::size_t place) {
    std::size_t seen = 0;
    for (const picker::Row& row : picker::rows(session.model)) {
        if (!row.on)
            continue;
        if (seen == place)
            return find_named(session.list, row_name(row.id));
        ++seen;
    }
    return nullptr;
}

/// The focused control, or null.
///
/// @param session the session
/// @return the control
const kit::Control* focused_control(const Session& session) {
    if (!session.state.interaction.focus_shown)
        return nullptr;
    return kit::control_of(session.list, session.state.interaction.focused);
}

/// Tabs until a control has the focus.
///
/// @param[in,out] session the session
/// @param name the control
/// @return true when it took the focus
bool focus_name(Session& session, std::string_view name) {
    session.state.interaction.focus_shown = false;
    std::string first;
    for (int step = 0; step < 48; ++step) {
        session.key(kit::Key::tab);
        const kit::Control* control = focused_control(session);
        if (control == nullptr)
            continue;
        if (control->name == name)
            return true;
        if (first.empty())
            first = control->name;
        else if (control->name == first)
            return false;
    }
    return false;
}

/// A point on a row that is the row itself, not its grip or its switch.
///
/// @param session the session
/// @param row the row
/// @return the point
kit::Point row_body(const Session& session, const kit::Control& row) {
    const kit::Rect area = picker_fixture::press_area(row);
    for (int32_t column = area.x + 4; column < area.x + area.width - 4; column += 4) {
        const kit::Point at{column, area.y + area.height / 2};
        const kit::Control* hit = kit::control_of(session.list, kit::hit(session.list, at));
        if (hit != nullptr && hit->id == row.id)
            return at;
    }
    return centre_of(area);
}

/// Adds Off mutators so the list scrolls at every size class.
///
/// @param[in,out] session the session
/// @param count how many
void add_off_rows(Session& session, int count) {
    picker::Inputs inputs = session.model.inputs;
    for (int index = 0; index < count; ++index) {
        const std::string number = std::to_string(index);
        inputs.mutators.push_back(
            picker_fixture::made("filler-" + number, "Filler " + number, true)
        );
    }
    picker::refresh(session.model, std::move(inputs));
    expect_prerequisites(session.model);
    session.lay();
}

/// Adds Tower, which is not installed and needs Moat, which is not either.
///
/// @param[in,out] session the session
void add_tower(Session& session) {
    picker::Inputs inputs = session.model.inputs;
    picker::Mutator moat = picker_fixture::made("moat", "Moat", false);
    picker::Mutator tower = picker_fixture::made("tower", "Tower", false);
    lib::Prerequisite need;
    need.id = "moat";
    tower.needs.push_back(std::move(need));
    inputs.mutators.push_back(std::move(moat));
    inputs.mutators.push_back(std::move(tower));
    picker::refresh(session.model, std::move(inputs));
    expect_prerequisites(session.model);
    session.lay();
}

/// The height of a row.
///
/// @param session the session
/// @return the height, in points; 0 when the list shows no row
int32_t row_height(const Session& session) {
    for (const kit::Control& control : session.list.controls) {
        if (control.name.rfind(picker::name_row_prefix, 0) != 0)
            continue;
        if (control.name.ends_with(picker::name_grip_suffix) ||
            control.name.ends_with(picker::name_switch_suffix))
            continue;
        if (control.rect.height > 0)
            return control.rect.height;
    }
    return 0;
}

/// An enabled grip a press can reach.
///
/// @param session the session
/// @return the grip; null when none shows
const kit::Control* visible_grip(const Session& session) {
    for (const kit::Control& control : session.list.controls) {
        if (!control.enabled || !control.name.ends_with(picker::name_grip_suffix))
            continue;
        if (shown_press(&control))
            return &control;
    }
    return nullptr;
}

/// Presses a control and releases over it.
///
/// @param[in,out] session the session
/// @param name the control
/// @param at the point; the centre when it is not in the control
/// @return what the release asked
picker::ScreenResult tap(Session& session, std::string_view name, std::optional<kit::Point> at) {
    reveal(session, name);
    const kit::Control* control = find_named(session.list, name);
    OA_CHECK(shown_press(control));
    if (!shown_press(control))
        return {};
    const kit::Point pressed = at ? *at : centre_of(picker_fixture::press_area(*control));
    session.pointer(picker::PointerKind::down, 1, pressed, 0);
    return session.pointer(picker::PointerKind::up, 1, pressed, 0);
}

/// Where a drag should be released, from the list as it is now.
using Landing = kit::Point (*)(const Session&);

/// Drags a grip to a point of the current list. A mouse moves 3 points first.
/// The list scrolls itself until that point lies in the list.
///
/// @param[in,out] session the session
/// @param grip_name the grip
/// @param landing the point, read again after each scroll
/// @param reach a finger's reach; 0 for a mouse
/// @return what the release asked
picker::ScreenResult
drag_grip(Session& session, std::string_view grip_name, Landing landing, int32_t reach) {
    reveal(session, grip_name);
    const kit::Control* grip = find_named(session.list, grip_name);
    OA_CHECK(shown_press(grip));
    if (!shown_press(grip))
        return {};
    const kit::Point from = centre_of(picker_fixture::press_area(*grip));
    session.pointer(picker::PointerKind::down, 1, from, reach);
    if (reach == 0) {
        const kit::Rect port = list_port(session);
        kit::Point nudged{from.x, from.y + 3};
        if (!kit::contains(port, nudged))
            nudged = {port.x + port.width / 2, port.y + port.height / 2};
        session.pointer(picker::PointerKind::move, 0, nudged, 0);
    }
    for (int step = 0; step < 40; ++step) {
        const kit::Rect port = list_port(session);
        const kit::Point target = landing(session);
        if (port.height <= 0 || kit::contains(port, target))
            break;
        const int32_t edge_y = target.y < port.y ? port.y + 4 : port.y + port.height - 4;
        session.pointer(picker::PointerKind::move, 0, {port.x + 8, edge_y}, reach);
    }
    const kit::Point target = landing(session);
    return session.pointer(picker::PointerKind::up, 1, target, reach);
}

/// The gap just under an On row's top, in the list.
///
/// @param session the session
/// @param place the On place
/// @return the point
kit::Point on_gap(const Session& session, std::size_t place) {
    const kit::Rect port = list_port(session);
    const kit::Control* row = on_row_at(session, place);
    const int32_t column = port.width > 0 ? port.x + port.width / 2 : 0;
    const int32_t y = row != nullptr ? row->rect.y + 1 : port.y;
    return {column, y};
}

/// A point in the Off section: the lower heading.
///
/// @param session the session
/// @return the point
kit::Point off_section(const Session& session) {
    const kit::Rect port = list_port(session);
    const kit::Item* lower = nullptr;
    for (const kit::Item& item : session.list.items) {
        if (item.role != kit::Role::heading)
            continue;
        if (lower == nullptr || item.rect.y > lower->rect.y)
            lower = &item;
    }
    const int32_t column = port.width > 0 ? port.x + 8 : 0;
    if (lower == nullptr)
        return {column, port.y};
    int32_t y = lower->rect.y;
    if (!kit::contains(port, {column, y}) && lower->rect.height > 1)
        y = lower->rect.y + 1;
    return {column, y};
}

/// An option's stored value.
///
/// @param model the picker
/// @param id the mutator
/// @param option the option
/// @return the value; empty when it is not on
std::string
stored_option(const picker::Picker& model, std::string_view id, std::string_view option) {
    for (const picker::Chosen& chosen : model.on) {
        if (chosen.id != id)
            continue;
        for (const auto& pair : chosen.options) {
            if (pair.first == option)
                return pair.second;
        }
    }
    return {};
}

/// The levels item of a control.
///
/// @param list the display list
/// @param id the control
/// @return the item; null when the list draws no strip for it
const kit::Item* levels_item(const kit::DisplayList& list, kit::ControlId id) {
    for (const kit::Item& item : list.items) {
        if (item.control == id && item.role == kit::Role::levels)
            return &item;
    }
    return nullptr;
}

/// A point on one level of a strip, halfway down the part of it that shows.
///
/// @param session the session
/// @param name the control
/// @param level the level, from 0
/// @return the point; the shown part's centre when the strip is not drawn
kit::Point level_point(const Session& session, std::string_view name, std::size_t level) {
    const kit::Control* control = find_named(session.list, name);
    if (control == nullptr)
        return {};
    const kit::Item* item = levels_item(session.list, control->id);
    const auto* look = item == nullptr ? nullptr : std::get_if<kit::LevelsLook>(&item->look);
    const kit::Rect shown = picker_fixture::press_area(*control);
    if (look == nullptr || look->level_width <= 0)
        return centre_of(shown);
    const int32_t column =
        item->rect.x + 1 + look->level_width / 2 + look->level_width * static_cast<int32_t>(level);
    return {column, shown.y + shown.height / 2};
}

/// Tells whether the frame is Compact.
///
/// @param frame the frame
/// @return true at Compact
bool is_compact(const kit::Frame& frame) {
    return frame.size_class == kit::SizeClass::compact;
}

/// Opens Compact's details page.
///
/// @param[in,out] session the session
void open_details(Session& session) {
    const picker::ScreenResult answered = tap(session, picker::name_details, std::nullopt);
    OA_CHECK(session.state.details_page);
    OA_CHECK(answered.action == picker::PickerAction::none);
}

/// Drags Grounded into On place 1.
///
/// @param where the size class
/// @param frame the frame
void drag_grounded_on(const char* where, const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const picker::ScreenResult answered = drag_grip(
        session,
        row_name("grounded") + std::string(picker::name_grip_suffix),
        [](const Session& current) { return on_gap(current, 1); },
        0
    );
    OA_CHECK(answered.action == picker::PickerAction::get);
    OA_CHECK(answered.ids.size() == 1 && answered.ids[0] == "grounded");
    expect_ids(
        session.model,
        {"fast-builders", "grounded", "scout-post", "gatehouse", "gatehouse-keep"},
        where
    );
    OA_CHECK(!session.state.drag);
}

/// Drags Gatehouse Keep toward Gatehouse. The band lands it just below.
///
/// @param where the size class
/// @param frame the frame
void drag_keep_below_gatehouse(const char* where, const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const picker::SwitchResult switched = picker::switch_on(session.model, "grounded");
    OA_CHECK(switched.changed);
    OA_CHECK(picker::move_to(session.model, "gatehouse-keep", session.model.on.size()));
    expect_prerequisites(session.model);
    session.lay();
    expect_ids(
        session.model,
        {"fast-builders", "scout-post", "gatehouse", "grounded", "gatehouse-keep"},
        where
    );
    const picker::ScreenResult answered = drag_grip(
        session,
        row_name("gatehouse-keep") + std::string(picker::name_grip_suffix),
        [](const Session& current) { return on_gap(current, 2); },
        0
    );
    OA_CHECK(answered.action == picker::PickerAction::none);
    expect_ids(
        session.model,
        {"fast-builders", "scout-post", "gatehouse", "gatehouse-keep", "grounded"},
        where
    );
}

/// Drags Gatehouse into the Off section. Keep needs it, so nothing changes.
///
/// @param where the size class
/// @param frame the frame
void drag_gatehouse_off(const char* /*where*/, const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const std::vector<std::string> before = ids_of(session.model);
    const picker::ScreenResult answered = drag_grip(
        session, row_name("gatehouse") + std::string(picker::name_grip_suffix), off_section, 0
    );
    OA_CHECK(answered.action == picker::PickerAction::ask_turn_off);
    OA_CHECK(
        answered.ids.size() == 2 && answered.ids[0] == "gatehouse" &&
        answered.ids[1] == "gatehouse-keep"
    );
    OA_CHECK(ids_of(session.model) == before);
    OA_CHECK(session.model.asking.has_value());
}

/// A release outside the list changes nothing.
///
/// @param frame the frame
void release_outside(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const std::vector<std::string> before = ids_of(session.model);
    reveal(session, row_name("fast-builders") + std::string(picker::name_grip_suffix));
    const kit::Control* grip =
        find_named(session.list, row_name("fast-builders") + std::string(picker::name_grip_suffix));
    OA_CHECK(shown_press(grip));
    if (!shown_press(grip))
        return;
    const kit::Point from = centre_of(picker_fixture::press_area(*grip));
    session.pointer(picker::PointerKind::down, 1, from, 0);
    const kit::Point inside{from.x, from.y + 3};
    session.pointer(picker::PointerKind::move, 0, inside, 0);
    OA_CHECK(session.state.drag.has_value());
    session.pointer(picker::PointerKind::up, 1, {-10, -10}, 0);
    OA_CHECK(!session.state.drag);
    OA_CHECK(ids_of(session.model) == before);
}

/// Two points do not start a mouse drag. Three do. A command then ends it.
///
/// @param frame the frame
void drag_threshold(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const std::vector<std::string> before = ids_of(session.model);
    const kit::Control* grip =
        find_named(session.list, row_name("fast-builders") + std::string(picker::name_grip_suffix));
    OA_CHECK(shown_press(grip));
    if (!shown_press(grip))
        return;
    const kit::Point from = centre_of(picker_fixture::press_area(*grip));
    session.pointer(picker::PointerKind::down, 1, from, 0);
    session.pointer(picker::PointerKind::move, 0, {from.x, from.y + 2}, 0);
    OA_CHECK(!session.state.drag);
    session.pointer(picker::PointerKind::move, 0, {from.x, from.y + 3}, 0);
    OA_CHECK(session.state.drag.has_value());
    const picker::ScreenResult answered = session.command(picker::ScreenCommand::find);
    OA_CHECK(!session.state.drag);
    OA_CHECK(ids_of(session.model) == before);
    OA_CHECK(answered.redraw);
    OA_CHECK(picker::focused_field(session.state, session.list).has_value());
}

/// The list scrolls one row when a drag is held near either edge.
///
/// @param where the size class
/// @param frame the frame
void auto_scroll(const char* where, const kit::Frame& frame) {
    Session session = open_at(frame, false);
    add_off_rows(session, 40);
    session.state.list_scroll = 100;
    session.lay();
    const kit::Control* grip = visible_grip(session);
    OA_CHECK(grip != nullptr);
    if (grip == nullptr)
        return;
    const kit::Point from = centre_of(picker_fixture::press_area(*grip));
    session.pointer(picker::PointerKind::down, 1, from, 0);
    const kit::Rect port = list_port(session);
    session.pointer(
        picker::PointerKind::move, 0, {port.x + port.width / 2, port.y + port.height / 2}, 0
    );
    OA_CHECK(session.state.drag.has_value());
    const int32_t height = row_height(session);
    const int32_t before_top = session.state.list_scroll;
    session.pointer(picker::PointerKind::move, 0, {port.x + 8, port.y + 4}, 0);
    if (session.state.list_scroll != before_top - height)
        std::fprintf(
            stderr,
            "%s scroll top %d -> %d, row %d\n",
            where,
            before_top,
            session.state.list_scroll,
            height
        );
    OA_CHECK(session.state.list_scroll == before_top - height);

    session.state.list_scroll = 100;
    session.lay();
    const kit::Rect again = list_port(session);
    const int32_t before_bottom = session.state.list_scroll;
    session.pointer(picker::PointerKind::move, 0, {again.x + 8, again.y + again.height - 4}, 0);
    if (session.state.list_scroll != before_bottom + height)
        std::fprintf(
            stderr,
            "%s scroll bottom %d -> %d, row %d\n",
            where,
            before_bottom,
            session.state.list_scroll,
            height
        );
    OA_CHECK(session.state.list_scroll == before_bottom + height);

    const int32_t scrolled = session.state.list_scroll;
    session.wheel({again.x + again.width / 2, again.y + again.height / 2}, 1.f);
    OA_CHECK(session.state.list_scroll < scrolled);
    const int32_t stuck = session.state.list_scroll;
    session.stick(1.f);
    OA_CHECK(session.state.list_scroll < stuck);
    const int32_t held = session.state.list_scroll;
    session.wheel(
        {again.x + 8, again.y + again.height / 2}, std::numeric_limits<float>::quiet_NaN()
    );
    session.stick(std::numeric_limits<float>::infinity());
    OA_CHECK(session.state.list_scroll == held);
}

/// A finger on a grip drags at once.
///
/// @param frame the frame
void finger_drags_at_once(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const kit::Control* grip =
        find_named(session.list, row_name("fast-builders") + std::string(picker::name_grip_suffix));
    OA_CHECK(shown_press(grip));
    if (!shown_press(grip))
        return;
    const kit::Point from = centre_of(picker_fixture::press_area(*grip));
    session.pointer(picker::PointerKind::down, 1, from, 12);
    OA_CHECK(session.state.drag.has_value());
    OA_CHECK(session.state.drag->id == "fast-builders");
    const std::vector<std::string> before = ids_of(session.model);
    session.key(kit::Key::escape);
    OA_CHECK(!session.state.drag);
    OA_CHECK(ids_of(session.model) == before);
}

/// A finger on a row's body that moves scrolls, and lifts nothing.
///
/// @param frame the frame
void finger_scrolls_the_body(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    add_off_rows(session, 40);
    const kit::Control* row = find_named(session.list, row_name("fast-builders"));
    OA_CHECK(shown_press(row));
    if (!shown_press(row))
        return;
    const kit::Point from = row_body(session, *row);
    constexpr int32_t reach = 8;
    session.pointer(picker::PointerKind::down, 1, from, reach);
    OA_CHECK(!session.state.drag);
    OA_CHECK(session.model.selected && *session.model.selected == "scout-post");
    const kit::Point moved{from.x, from.y - (reach + 1)};
    session.pointer(picker::PointerKind::move, 0, moved, reach);
    OA_CHECK(session.state.list_scroll > 0);
    session.pointer(picker::PointerKind::up, 1, moved, reach);
    OA_CHECK(session.model.selected && *session.model.selected == "scout-post");
    OA_CHECK(!session.state.drag);
    expect_ids(
        session.model, {"fast-builders", "scout-post", "gatehouse", "gatehouse-keep"}, "finger"
    );
}

/// A tap on Scout Post's switch turns it off.
///
/// @param frame the frame
void tap_switch(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    const picker::ScreenResult answered = tap(
        session, row_name("scout-post") + std::string(picker::name_switch_suffix), std::nullopt
    );
    OA_CHECK(answered.action == picker::PickerAction::none);
    expect_ids(session.model, {"fast-builders", "gatehouse", "gatehouse-keep"}, "switch");
}

/// A tap on Tower answers get with Tower and Moat, which it needs.
///
/// @param frame the frame
void tap_uninstalled(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    add_tower(session);
    const picker::ScreenResult answered =
        tap(session, row_name("tower") + std::string(picker::name_switch_suffix), std::nullopt);
    OA_CHECK(answered.action == picker::PickerAction::get);
    OA_CHECK(answered.ids.size() == 2 && answered.ids[0] == "moat" && answered.ids[1] == "tower");
    const std::vector<std::string> ids = ids_of(session.model);
    const auto moat = std::find(ids.begin(), ids.end(), "moat");
    const auto tower = std::find(ids.begin(), ids.end(), "tower");
    OA_CHECK(moat != ids.end() && tower != ids.end() && moat < tower);
}

/// ▲ and ▼ by pointer and by Space. The controller's A is Space.
///
/// @param frame the frame
void move_with_up_and_down(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    if (is_compact(frame))
        open_details(session);
    const std::vector<std::string> started = {
        "fast-builders", "scout-post", "gatehouse", "gatehouse-keep"
    };
    const std::vector<std::string> raised = {
        "scout-post", "fast-builders", "gatehouse", "gatehouse-keep"
    };
    const picker::ScreenResult by_pointer = tap(session, picker::name_up, std::nullopt);
    OA_CHECK(by_pointer.action == picker::PickerAction::none);
    expect_ids(session.model, raised, "pointer up");
    const picker::ScreenResult restored = tap(session, picker::name_down, std::nullopt);
    OA_CHECK(restored.action == picker::PickerAction::none);
    expect_ids(session.model, started, "pointer down");

    // The controller's A is Space.
    OA_CHECK(focus_name(session, picker::name_up));
    const picker::ScreenResult by_space = session.key(kit::Key::space);
    OA_CHECK(by_space.action == picker::PickerAction::none);
    expect_ids(session.model, raised, "space up");
    OA_CHECK(focus_name(session, picker::name_down));
    session.key(kit::Key::space);
    expect_ids(session.model, started, "space down");
}

/// Tab's walk, including a details control the page has clipped.
///
/// @param frame the frame
void tab_walk(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    if (is_compact(frame)) {
        std::vector<std::string> names;
        session.state.interaction.focus_shown = false;
        std::string first;
        for (int step = 0; step < 24; ++step) {
            session.key(kit::Key::tab);
            const kit::Control* control = focused_control(session);
            if (control == nullptr)
                continue;
            if (!first.empty() && control->name == first)
                break;
            if (first.empty())
                first = control->name;
            names.push_back(control->name);
        }
        for (const char* name :
             {"mutators.filter",
              "mutators.search",
              "mutators.list",
              "mutators.details",
              "mutators.cancel",
              "mutators.use",
              "mutators.close"})
            OA_CHECK(std::find(names.begin(), names.end(), name) != names.end());
        open_details(session);
    }
    session.state.interaction.focus_shown = false;
    std::string first;
    bool saw_up = false;
    bool saw_down = false;
    bool saw_speed = false;
    for (int step = 0; step < 48; ++step) {
        session.key(kit::Key::tab);
        const kit::Control* control = focused_control(session);
        if (control == nullptr)
            continue;
        if (!first.empty() && control->name == first)
            break;
        if (first.empty())
            first = control->name;
        if (control->name == picker::name_up || control->name == picker::name_down ||
            control->name.rfind(picker::name_option_prefix, 0) == 0 ||
            control->name == picker::name_clash)
            OA_CHECK(shown_press(control));
        saw_up = saw_up || control->name == picker::name_up;
        saw_down = saw_down || control->name == picker::name_down;
        saw_speed = saw_speed || control->name == "mutators.option.speed";
    }
    OA_CHECK(saw_up);
    OA_CHECK(saw_down);
    OA_CHECK(saw_speed);
    OA_CHECK(focus_name(session, first));
    session.key(kit::Key::back_tab);
    const kit::Control* wrapped = focused_control(session);
    OA_CHECK(wrapped != nullptr && wrapped->name != first);
}

/// The keys of the list and the details.
///
/// @param where the size class
/// @param frame the frame
void keys_of_step_3(const char* where, const kit::Frame& frame) {
    Session session = open_at(frame, false);
    OA_CHECK(focus_name(session, picker::name_list));
    session.key(kit::Key::home);
    OA_CHECK(session.model.selected && *session.model.selected == "fast-builders");
    OA_CHECK(focused_control(session) && focused_control(session)->name == picker::name_list);
    session.key(kit::Key::end);
    OA_CHECK(session.model.selected && *session.model.selected == "ridge-walls");
    OA_CHECK(focused_control(session) && focused_control(session)->name == picker::name_list);
    session.key(kit::Key::page_up);
    OA_CHECK(focused_control(session) && focused_control(session)->name == picker::name_list);
    OA_CHECK(session.model.selected.has_value());
    session.key(kit::Key::home);
    session.key(kit::Key::page_down);
    OA_CHECK(focused_control(session) && focused_control(session)->name == picker::name_list);
    session.key(kit::Key::home);
    session.key(kit::Key::down);
    OA_CHECK(session.model.selected && *session.model.selected == "scout-post");
    OA_CHECK(focused_control(session) && focused_control(session)->name == picker::name_list);
    session.key(kit::Key::up);
    OA_CHECK(session.model.selected && *session.model.selected == "fast-builders");
    session.key(kit::Key::up);
    OA_CHECK(session.model.selected && *session.model.selected == "fast-builders");
    OA_CHECK(
        focused_control(session) == nullptr || focused_control(session)->name != picker::name_list
    );

    OA_CHECK(focus_name(session, picker::name_list));
    session.key(kit::Key::down);
    const picker::ScreenResult flipped = session.key(kit::Key::space);
    OA_CHECK(flipped.action == picker::PickerAction::none);
    expect_ids(session.model, {"fast-builders", "gatehouse", "gatehouse-keep"}, where);

    Session entered = open_at(frame, false);
    OA_CHECK(focus_name(entered, picker::name_list));
    const picker::ScreenResult opening = entered.key(kit::Key::enter);
    OA_CHECK(opening.action == picker::PickerAction::none);
    if (is_compact(frame))
        OA_CHECK(entered.state.details_page);
    const kit::Control* detail = focused_control(entered);
    OA_CHECK(detail != nullptr && detail->name == "mutators.option.speed");
    const picker::ScreenResult stepped = entered.key(kit::Key::left);
    OA_CHECK(stepped.action == picker::PickerAction::check);
    OA_CHECK(stepped.ids.size() == 1 && stepped.ids[0] == "scout-post");
    OA_CHECK(stored_option(entered.model, "scout-post", "speed") == "1x");
    OA_CHECK(focus_name(entered, "mutators.option.signal"));
    const picker::ScreenResult menu = entered.key(kit::Key::space);
    OA_CHECK(menu.action == picker::PickerAction::none);
    OA_CHECK(entered.state.menu == picker::Menu::choice);

    Session escaping = open_at(frame, false);
    escaping.command(picker::ScreenCommand::find);
    escaping.text("gate");
    OA_CHECK(escaping.model.query == "gate");
    const picker::ScreenResult cleared = escaping.key(kit::Key::escape);
    OA_CHECK(cleared.action == picker::PickerAction::none);
    OA_CHECK(escaping.model.query.empty());
    if (is_compact(frame)) {
        open_details(escaping);
        const picker::ScreenResult left = escaping.key(kit::Key::escape);
        OA_CHECK(left.action == picker::PickerAction::none);
        OA_CHECK(!escaping.state.details_page);
    }
    const picker::ScreenResult cancelled = escaping.key(kit::Key::escape);
    OA_CHECK(cancelled.action == picker::PickerAction::cancel);
}

/// find, back, flip and use.
///
/// @param frame the frame
void commands(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    if (is_compact(frame))
        open_details(session);
    const picker::ScreenResult found = session.command(picker::ScreenCommand::find);
    OA_CHECK(found.redraw);
    OA_CHECK(!session.state.details_page);
    OA_CHECK(picker::focused_field(session.state, session.list).has_value());

    Session backing = open_at(frame, false);
    if (is_compact(frame)) {
        open_details(backing);
        const picker::ScreenResult left = backing.command(picker::ScreenCommand::back);
        OA_CHECK(left.action == picker::PickerAction::none);
        OA_CHECK(!backing.state.details_page);
    }
    const picker::ScreenResult cancelled = backing.command(picker::ScreenCommand::back);
    OA_CHECK(cancelled.action == picker::PickerAction::cancel);

    Session flipping = open_at(frame, false);
    const picker::ScreenResult flipped = flipping.command(picker::ScreenCommand::flip);
    OA_CHECK(flipped.action == picker::PickerAction::none);
    expect_ids(flipping.model, {"fast-builders", "gatehouse", "gatehouse-keep"}, "flip");

    Session applying = open_at(frame, false);
    const picker::ScreenResult used = applying.command(picker::ScreenCommand::use);
    OA_CHECK(used.action == picker::PickerAction::use);
    applying.state.waiting = "Checking";
    applying.lay();
    const picker::ScreenResult waiting = applying.command(picker::ScreenCommand::use);
    OA_CHECK(waiting.action == picker::PickerAction::use);
}

/// Typing and Backspace in the search, and typing while it has not the focus.
///
/// @param frame the frame
void search_text(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    session.text("nope");
    OA_CHECK(session.model.query.empty());
    session.command(picker::ScreenCommand::find);
    const std::optional<kit::Rect> field = picker::focused_field(session.state, session.list);
    OA_CHECK(field.has_value());
    session.text("ridge");
    OA_CHECK(session.model.query == "ridge");
    OA_CHECK(session.state.search.text == "ridge");
    session.key(kit::Key::backspace);
    OA_CHECK(session.model.query == "ridg");
    session.key(kit::Key::backspace);
    session.key(kit::Key::backspace);
    session.key(kit::Key::backspace);
    session.key(kit::Key::backspace);
    OA_CHECK(session.model.query.empty());
}

/// A level strip, a slider drag and a drop-down set an option and answer check.
///
/// @param frame the frame
void option_controls(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    if (is_compact(frame))
        open_details(session);
    OA_CHECK(stored_option(session.model, "scout-post", "speed") == "2x");
    reveal(session, "mutators.option.speed");
    const picker::ScreenResult level =
        tap(session, "mutators.option.speed", level_point(session, "mutators.option.speed", 0));
    OA_CHECK(level.action == picker::PickerAction::check);
    OA_CHECK(level.ids.size() == 1 && level.ids[0] == "scout-post");
    OA_CHECK(stored_option(session.model, "scout-post", "speed") == "1x");

    reveal(session, "mutators.option.range");
    const kit::Control* slider = find_named(session.list, "mutators.option.range");
    OA_CHECK(shown_press(slider));
    if (shown_press(slider)) {
        const kit::Rect area = picker_fixture::press_area(*slider);
        const kit::Point at{area.x + area.width - 2, area.y + area.height / 2};
        const picker::ScreenResult dragged = session.pointer(picker::PointerKind::down, 1, at, 0);
        OA_CHECK(dragged.action == picker::PickerAction::check);
        OA_CHECK(dragged.ids.size() == 1 && dragged.ids[0] == "scout-post");
        OA_CHECK(stored_option(session.model, "scout-post", "range") == "10");
        session.pointer(picker::PointerKind::move, 0, {at.x - 4, at.y}, 0);
        session.pointer(picker::PointerKind::up, 1, at, 0);
        OA_CHECK(stored_option(session.model, "scout-post", "range") == "10");
    }

    OA_CHECK(focus_name(session, "mutators.option.range"));
    const picker::ScreenResult stepped_left = session.key(kit::Key::left);
    OA_CHECK(stepped_left.action == picker::PickerAction::check);
    OA_CHECK(stepped_left.ids.size() == 1 && stepped_left.ids[0] == "scout-post");
    OA_CHECK(stored_option(session.model, "scout-post", "range") == "8");
    const picker::ScreenResult stepped_right = session.key(kit::Key::right);
    OA_CHECK(stepped_right.action == picker::PickerAction::check);
    OA_CHECK(stepped_right.ids.size() == 1 && stepped_right.ids[0] == "scout-post");
    OA_CHECK(stored_option(session.model, "scout-post", "range") == "10");

    OA_CHECK(focus_name(session, "mutators.option.alarm"));
    const picker::ScreenResult alarm_space = session.key(kit::Key::space);
    OA_CHECK(alarm_space.action == picker::PickerAction::check);
    OA_CHECK(alarm_space.ids.size() == 1 && alarm_space.ids[0] == "scout-post");
    OA_CHECK(stored_option(session.model, "scout-post", "alarm") == "Yes");
    const picker::ScreenResult alarm_enter = session.key(kit::Key::enter);
    OA_CHECK(alarm_enter.action == picker::PickerAction::check);
    OA_CHECK(alarm_enter.ids.size() == 1 && alarm_enter.ids[0] == "scout-post");
    OA_CHECK(stored_option(session.model, "scout-post", "alarm") == "No");

    OA_CHECK(focus_name(session, "mutators.option.signal"));
    const picker::ScreenResult signal_enter = session.key(kit::Key::enter);
    OA_CHECK(signal_enter.action == picker::PickerAction::none);
    OA_CHECK(session.state.menu == picker::Menu::choice);
    session.key(kit::Key::escape);
    OA_CHECK(session.state.menu == picker::Menu::none);

    const picker::ScreenResult opened = tap(session, "mutators.option.signal", std::nullopt);
    OA_CHECK(opened.action == picker::PickerAction::none);
    OA_CHECK(session.state.menu == picker::Menu::choice);
    const kit::Control* south = nullptr;
    for (const kit::Control& control : session.list.controls) {
        if (control.kind == kit::ControlKind::list_item && control.text == "South" &&
            control.enabled)
            south = &control;
    }
    OA_CHECK(south != nullptr);
    if (south != nullptr) {
        const kit::Point at = centre_of(picker_fixture::press_area(*south));
        session.pointer(picker::PointerKind::down, 1, at, 0);
        const picker::ScreenResult chosen = session.pointer(picker::PointerKind::up, 1, at, 0);
        OA_CHECK(chosen.action == picker::PickerAction::check);
        OA_CHECK(chosen.ids.size() == 1 && chosen.ids[0] == "scout-post");
        OA_CHECK(stored_option(session.model, "scout-post", "signal") == "South");
    }

    reveal(session, picker::name_clash);
    const picker::ScreenResult clash =
        tap(session, picker::name_clash, level_point(session, picker::name_clash, 0));
    OA_CHECK(clash.action == picker::PickerAction::check);
    OA_CHECK(clash.ids.size() == 1 && clash.ids[0] == "scout-post");
    bool added = false;
    for (const picker::Chosen& chosen : session.model.on) {
        if (chosen.id == "scout-post")
            added = chosen.clash == picker::Clash::add;
    }
    OA_CHECK(added);
}

/// Read-only selects and scrolls, and refuses every change.
///
/// @param frame the frame
void read_only(const kit::Frame& frame) {
    Session session = open_at(frame, true);
    const std::vector<std::string> before = ids_of(session.model);
    const kit::Control* grip =
        find_named(session.list, row_name("fast-builders") + std::string(picker::name_grip_suffix));
    if (shown_press(grip)) {
        const kit::Point from = centre_of(picker_fixture::press_area(*grip));
        session.pointer(picker::PointerKind::down, 1, from, 0);
        session.pointer(picker::PointerKind::move, 0, {from.x + 8, from.y + 8}, 0);
        session.pointer(picker::PointerKind::up, 1, {from.x + 8, from.y + 8}, 0);
    }
    OA_CHECK(!session.state.drag);
    OA_CHECK(ids_of(session.model) == before);
    tap(session, row_name("scout-post") + std::string(picker::name_switch_suffix), std::nullopt);
    OA_CHECK(ids_of(session.model) == before);

    if (is_compact(frame)) {
        OA_CHECK(find_named(session.list, picker::name_details) == nullptr);
        session.state.details_page = true;
        session.lay();
    }
    const kit::Control* speed = find_named(session.list, "mutators.option.speed");
    const kit::Control* clash_strip = find_named(session.list, picker::name_clash);
    const kit::Control* up = find_named(session.list, picker::name_up);
    OA_CHECK(speed != nullptr && !speed->enabled);
    OA_CHECK(clash_strip != nullptr && !clash_strip->enabled);
    OA_CHECK(up != nullptr && !up->enabled);
    reveal(session, "mutators.option.speed");
    const picker::ScreenResult level =
        tap(session, "mutators.option.speed", level_point(session, "mutators.option.speed", 0));
    OA_CHECK(level.action == picker::PickerAction::none);
    OA_CHECK(stored_option(session.model, "scout-post", "speed") == "2x");
    reveal(session, picker::name_clash);
    const picker::ScreenResult clash =
        tap(session, picker::name_clash, level_point(session, picker::name_clash, 0));
    OA_CHECK(clash.action == picker::PickerAction::none);
    bool replacing = false;
    for (const picker::Chosen& chosen : session.model.on) {
        if (chosen.id == "scout-post")
            replacing = chosen.clash == picker::Clash::replace;
    }
    OA_CHECK(replacing);
    const picker::ScreenResult raised = tap(session, picker::name_up, std::nullopt);
    OA_CHECK(raised.action == picker::PickerAction::none);
    OA_CHECK(ids_of(session.model) == before);

    const picker::ScreenResult closed = tap(session, picker::name_done, std::nullopt);
    OA_CHECK(closed.action == picker::PickerAction::cancel);
    const picker::ScreenResult marked = tap(session, picker::name_close, std::nullopt);
    OA_CHECK(marked.action == picker::PickerAction::cancel);

    Session selecting = open_at(frame, true);
    tap(selecting, row_name("gatehouse"), std::nullopt);
    OA_CHECK(selecting.model.selected && *selecting.model.selected == "gatehouse");
    OA_CHECK(ids_of(selecting.model) == before);
    const picker::ScreenResult refused = selecting.command(picker::ScreenCommand::flip);
    OA_CHECK(refused.action == picker::PickerAction::none);
    OA_CHECK(ids_of(selecting.model) == before);
    const picker::ScreenResult unused = selecting.command(picker::ScreenCommand::use);
    OA_CHECK(unused.action == picker::PickerAction::none);
}

/// The mutator id of a row's switch name, or empty when the name is not one.
///
/// @param name the control's name
/// @return the id
std::string switch_row(std::string_view name) {
    const std::string_view prefix = picker::name_row_prefix;
    const std::string_view suffix = picker::name_switch_suffix;
    if (name.size() <= prefix.size() + suffix.size() || name.substr(0, prefix.size()) != prefix ||
        !name.ends_with(suffix))
        return {};
    return std::string(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
}

/// A press on a switch that is cut by the list's bottom edge switches it on
/// and does not scroll the list.
///
/// @param frame the frame
void press_cut_switch(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    add_off_rows(session, 40);
    const kit::Rect port = list_port(session);
    std::string id;
    int32_t content_y = 0;
    int32_t full_height = 0;
    for (const kit::Control& control : session.list.controls) {
        if (!control.enabled)
            continue;
        const std::string row = switch_row(control.name);
        if (row.rfind("filler-", 0) != 0)
            continue;
        const int32_t centre = control.rect.y + control.rect.height / 2;
        if (centre < port.y + port.height)
            continue;
        id = row;
        content_y = control.rect.y - port.y + session.state.list_scroll;
        full_height = control.rect.height;
        break;
    }
    OA_CHECK(!id.empty() && full_height > 1 && port.height > 0);
    if (id.empty() || full_height <= 1 || port.height <= 0)
        return;
    const int32_t offset = content_y + full_height / 2 - port.height;
    OA_CHECK(offset >= 0);
    session.state.list_scroll = offset;
    session.lay();
    const kit::Rect shown_port = list_port(session);
    const kit::Control* sw =
        find_named(session.list, row_name(id) + std::string(picker::name_switch_suffix));
    OA_CHECK(sw != nullptr && sw->enabled);
    if (sw == nullptr)
        return;
    const kit::Rect shown = picker_fixture::press_area(*sw);
    const int32_t half = sw->rect.height / 2;
    OA_CHECK(picker_fixture::has_room(shown));
    OA_CHECK(sw->rect.y < shown_port.y + shown_port.height);
    OA_CHECK(sw->rect.y + sw->rect.height > shown_port.y + shown_port.height);
    OA_CHECK(shown.height + 2 >= half && shown.height <= half + 2);
    const int32_t held = session.state.list_scroll;
    const kit::Point at = centre_of(shown);
    session.pointer(picker::PointerKind::down, 1, at, 0);
    OA_CHECK(session.state.list_scroll == held);
    const picker::ScreenResult answered = session.pointer(picker::PointerKind::up, 1, at, 0);
    OA_CHECK(answered.action == picker::PickerAction::none);
    const std::vector<std::string> ids = ids_of(session.model);
    OA_CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
}

/// A pane's scroll bar, as the parts clipped to its port and the named well show it.
struct MeasuredBar {
    kit::Rect port{}; ///< the pane
    kit::Rect well{}; ///< the bar
    int32_t offset{}; ///< how far the pane is scrolled
    int32_t limit{};  ///< the greatest offset
    bool ok{};        ///< the bar is on the screen and the pane scrolls
};

/// Measures one scroll bar.
///
/// @param list the display list
/// @param bar_name the bar
/// @param port the pane
/// @return the measurement; not ok when the bar is missing or the pane fits
MeasuredBar measure_bar(const kit::DisplayList& list, std::string_view bar_name, kit::Rect port) {
    MeasuredBar measured;
    const kit::Control* bar = find_named(list, bar_name);
    if (bar == nullptr || port.width <= 0 || port.height <= 0)
        return measured;
    int32_t top = 0;
    int32_t bottom = 0;
    bool any = false;
    const auto grow = [&](const kit::Rect& rect) {
        const int32_t rect_bottom = rect.y + rect.height;
        if (!any) {
            top = rect.y;
            bottom = rect_bottom;
            any = true;
            return;
        }
        if (rect.y < top)
            top = rect.y;
        if (rect_bottom > bottom)
            bottom = rect_bottom;
    };
    for (const kit::Item& item : list.items) {
        if (picker_fixture::same_rect(item.clip, port))
            grow(item.rect);
    }
    for (const kit::Control& control : list.controls) {
        if (picker_fixture::same_rect(control.clip, port))
            grow(control.rect);
    }
    if (!any)
        return measured;
    measured.port = port;
    measured.well = bar->rect;
    measured.offset = std::max(port.y - top, int32_t{0});
    measured.limit = kit::ScrollArea::limit_for(bottom - top, port.height);
    if (measured.offset > measured.limit)
        measured.offset = measured.limit;
    measured.ok = measured.limit > 0;
    return measured;
}

/// The scroll area a measurement describes, as the input holds a bar.
///
/// @param measured the measurement
/// @return the area
kit::ScrollArea area_of(const MeasuredBar& measured) {
    kit::ScrollArea area;
    area.view = measured.port;
    area.well = measured.well;
    area.content_height = measured.limit + measured.port.height;
    area.limit = measured.limit;
    area.offset = measured.offset;
    return area;
}

/// How far a pane is scrolled.
///
/// @param session the session
/// @param details true for the details
/// @return the offset
int32_t scrolled(const Session& session, bool details) {
    return details ? session.state.details_scroll : session.state.list_scroll;
}

/// A press on the thumb, a press on the well and the wheel over the well.
///
/// @param[in,out] session the session
/// @param bar_name the bar
/// @param port the pane
/// @param details true for the details
void exercise_bar(Session& session, std::string_view bar_name, kit::Rect port, bool details) {
    const MeasuredBar measured = measure_bar(session.list, bar_name, port);
    OA_CHECK(measured.ok);
    if (!measured.ok)
        return;
    OA_CHECK(measured.offset == scrolled(session, details));
    const int32_t least = kit::compact_metrics.least_thumb_height;
    const kit::ScrollArea area = area_of(measured);
    const kit::Rect thumb = area.thumb(least);
    const kit::Point held = centre_of(thumb);
    OA_CHECK(kit::contains(thumb, held));
    const int32_t before = scrolled(session, details);
    session.pointer(picker::PointerKind::down, 1, held, 0);
    OA_CHECK(scrolled(session, details) == before);
    OA_CHECK(session.state.bar != picker::ScreenState::Bar::none);
    const kit::Point moved{held.x, held.y + 60};
    session.pointer(picker::PointerKind::move, 0, moved, 0);
    const int32_t expected = area.offset_at(moved.y - (held.y - thumb.y), least);
    OA_CHECK(scrolled(session, details) == expected);
    OA_CHECK(scrolled(session, details) != before);
    session.pointer(picker::PointerKind::up, 1, moved, 0);
    OA_CHECK(session.state.bar == picker::ScreenState::Bar::none);

    if (details)
        session.state.details_scroll = 0;
    else
        session.state.list_scroll = 0;
    session.lay();
    const MeasuredBar reset = measure_bar(session.list, bar_name, port);
    OA_CHECK(reset.ok);
    if (!reset.ok)
        return;
    const kit::ScrollArea reset_area = area_of(reset);
    const kit::Rect reset_thumb = reset_area.thumb(least);
    const kit::Point low{reset.well.x + reset.well.width / 2, reset.well.y + reset.well.height - 2};
    OA_CHECK(!kit::contains(reset_thumb, low));
    session.pointer(picker::PointerKind::down, 1, low, 0);
    const int32_t jumped = reset_area.offset_at(low.y - reset_thumb.height / 2, least);
    OA_CHECK(scrolled(session, details) == jumped);
    OA_CHECK(jumped != 0);
    session.pointer(picker::PointerKind::up, 1, low, 0);
    OA_CHECK(session.state.bar == picker::ScreenState::Bar::none);

    const int32_t parked = std::min(int32_t{80}, reset.limit);
    if (details)
        session.state.details_scroll = parked;
    else
        session.state.list_scroll = parked;
    session.lay();
    const MeasuredBar wheeled = measure_bar(session.list, bar_name, port);
    OA_CHECK(wheeled.ok);
    const kit::Point over{
        wheeled.well.x + wheeled.well.width / 2, wheeled.well.y + wheeled.well.height / 2
    };
    const int32_t prior = scrolled(session, details);
    session.wheel(over, 1.f);
    OA_CHECK(scrolled(session, details) < prior);
}

/// The list's scroll bar at every class, and Compact's details-page bar.
///
/// @param frame the frame
void scroll_bars(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    add_off_rows(session, 40);
    exercise_bar(session, picker::name_list_bar, list_port(session), false);
    if (!is_compact(frame))
        return;
    open_details(session);
    const kit::Control* speed = find_named(session.list, "mutators.option.speed");
    OA_CHECK(speed != nullptr && speed->clip.height > 0);
    if (speed == nullptr)
        return;
    exercise_bar(session, picker::name_details_bar, speed->clip, true);
}

/// Page Up and Page Down move the selection by the rows the list shows.
///
/// @param frame the frame
void page_distance(const kit::Frame& frame) {
    Session session = open_at(frame, false);
    add_off_rows(session, 40);
    OA_CHECK(focus_name(session, picker::name_list));
    std::vector<std::string> order;
    for (const picker::Row& row : picker::rows(session.model))
        order.push_back(row.id);
    const int32_t height = row_height(session);
    const kit::Rect port = list_port(session);
    OA_CHECK(height > 0 && port.height > 0 && !order.empty());
    if (height <= 0 || port.height <= 0 || order.empty())
        return;
    const int page = std::max(port.height / height, int32_t{1});
    const auto stepped = static_cast<std::size_t>(page);
    OA_CHECK(stepped < order.size());
    if (stepped >= order.size())
        return;
    session.key(kit::Key::home);
    OA_CHECK(session.model.selected && *session.model.selected == order.front());
    session.key(kit::Key::page_down);
    OA_CHECK(session.model.selected && *session.model.selected == order[stepped]);
    session.key(kit::Key::page_up);
    OA_CHECK(session.model.selected && *session.model.selected == order.front());
}

/// One size class.
struct Class {
    const char* name{}; ///< the case
    kit::Frame frame{}; ///< the frame
};

/// The three size classes.
///
/// @return the classes
std::vector<Class> classes() {
    return {
        {"compact", kit::Frame{{0, 0, 480, 324}, kit::SizeClass::compact, 100}},
        {"regular", kit::Frame{{0, 0, 720, 486}, kit::SizeClass::regular, 100}},
        {"large", kit::Frame{{0, 0, 960, 600}, kit::SizeClass::large, 100}},
    };
}

} // namespace

int main() {
    const std::vector<Class> sizes = classes();
    OA_CHECK(sizes.size() == 3);
    for (const Class& size : sizes) {
        drag_grounded_on(size.name, size.frame);
        drag_keep_below_gatehouse(size.name, size.frame);
        drag_gatehouse_off(size.name, size.frame);
        release_outside(size.frame);
        drag_threshold(size.frame);
        auto_scroll(size.name, size.frame);
        finger_drags_at_once(size.frame);
        finger_scrolls_the_body(size.frame);
        tap_switch(size.frame);
        tap_uninstalled(size.frame);
        move_with_up_and_down(size.frame);
        tab_walk(size.frame);
        keys_of_step_3(size.name, size.frame);
        commands(size.frame);
        search_text(size.frame);
        option_controls(size.frame);
        read_only(size.frame);
        press_cut_switch(size.frame);
        scroll_bars(size.frame);
        page_distance(size.frame);
    }
    return oa::test::check_exit_status();
}
