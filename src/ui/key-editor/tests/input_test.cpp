// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's keys, typed search and finger at Large (1280x720, 1x),
// and Enter on Compact's list. The pad's Y is EditorCommand::find: the kit
// has no Y key, so the test calls editor_command.
//
// oa-ui-key-editor-input-test

#include "oa/test/check.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "screen_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
using key_editor_fixture::editor_fixture;
using key_editor_fixture::fonts;
using key_editor_fixture::frame_for;
using key_editor_fixture::has_room;
using key_editor_fixture::press_area;

/// True when a name starts with a prefix.
///
/// @param name the name
/// @param prefix the prefix
/// @return true when it does
bool starts(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}

/// A control of a name, or null.
///
/// @param list the display list
/// @param name the name
/// @return the control
const kit::Control* find_named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// How many row controls a list has.
///
/// @param list the display list
/// @return the count
std::size_t row_controls(const kit::DisplayList& list) {
    std::size_t count = 0;
    for (const kit::Control& control : list.controls) {
        if (starts(control.name, editor::name_row_prefix))
            ++count;
    }
    return count;
}

/// True when the chosen action is one of the rows the screen is filtering.
///
/// @param model the editor
/// @param state the screen's state
/// @return true when the id is among those rows
bool selected_is_listed(const editor::Editor& model, const editor::ScreenState& state) {
    if (state.selected.empty())
        return false;
    const std::vector<editor::Row> rows =
        model.rows(state.category, state.filter, state.search.text);
    for (const editor::Row& row : rows) {
        if (row.id == state.selected)
            return true;
    }
    return false;
}

/// The editor, its screen state and the list last laid out.
struct Session {
    editor::Editor model;
    editor::ScreenState state{};
    kit::Frame frame{};
    kit::DisplayList list{};

    /// Lays the fixture out for a frame.
    ///
    /// @param framed the frame
    explicit Session(kit::Frame framed) : model(editor_fixture()), frame(std::move(framed)) {
        show();
    }

    /// Lays the screen out again.
    void show() { list = editor::editor_layout(model, state, frame, fonts()); }

    /// Lays the screen out again when the result asks for it.
    ///
    /// @param result what the input returned
    /// @return that result
    editor::EditorResult take(editor::EditorResult result) {
        if (result.redraw)
            show();
        return result;
    }
};

/// Puts the focus on the list, laying out first so the control id is current.
///
/// @param session the session
void focus_list(Session& session) {
    session.show();
    const kit::Control* list = find_named(session.list, editor::name_list);
    if (list == nullptr)
        std::fprintf(stderr, "the list is not a control\n");
    OA_CHECK(list != nullptr);
    if (list != nullptr)
        session.state.interaction.focused = list->id;
}

/// A point inside a control's press area.
///
/// @param control the control
/// @return the point
kit::Point centre_of(const kit::Control& control) {
    const kit::Rect area = press_area(control);
    return {area.x + area.width / 2, area.y + area.height / 2};
}

/// The search takes the focus, and typing "attack" leaves fewer rows.
void search_and_escape() {
    const kit::Frame frame = frame_for(1280, 720, 100);
    OA_CHECK(frame.size_class == kit::SizeClass::large);
    Session session(frame);
    const std::size_t before = row_controls(session.list);
    // The pad's Y. editor_command is how it arrives; Key has no Y.
    session.take(
        editor::editor_command(
            session.model, session.state, session.list, editor::EditorCommand::find
        )
    );
    const std::optional<kit::Rect> field = editor::focused_field(session.state, session.list);
    if (!field)
        std::fprintf(stderr, "find did not focus the search\n");
    OA_CHECK(field.has_value());

    session.take(editor::editor_text(session.model, session.state, session.list, "attack"));
    if (session.state.search.text != "attack")
        std::fprintf(stderr, "search text is \"%s\"\n", session.state.search.text.c_str());
    OA_CHECK(session.state.search.text == "attack");
    session.show();
    const std::size_t after = row_controls(session.list);
    if (after >= before)
        std::fprintf(stderr, "search rows %zu of %zu\n", after, before);
    OA_CHECK(after < before);

    const editor::EditorResult cleared = session.take(
        editor::editor_key(session.model, session.state, session.list, kit::Key::escape)
    );
    OA_CHECK(cleared.action == editor::EditorAction::none);
    OA_CHECK(session.state.search.text.empty());

    OA_CHECK(session.model.unsaved() == 0);
    const editor::EditorResult closed = session.take(
        editor::editor_key(session.model, session.state, session.list, kit::Key::escape)
    );
    OA_CHECK(closed.action == editor::EditorAction::cancel);
}

/// Escape and Back ask to leave once a chord has been removed. Save on a clean editor saves.
void leave_and_save() {
    Session dirty(frame_for(1280, 720, 100));
    const editor::Details attack = dirty.model.details("order.attack");
    OA_CHECK(!attack.chords.empty());
    OA_CHECK(dirty.model.remove_chord("order.attack", 0));
    OA_CHECK(dirty.model.unsaved() > 0);
    const editor::EditorResult escape =
        dirty.take(editor::editor_key(dirty.model, dirty.state, dirty.list, kit::Key::escape));
    OA_CHECK(escape.action == editor::EditorAction::ask_leave);

    Session back(frame_for(1280, 720, 100));
    OA_CHECK(back.model.remove_chord("order.attack", 0));
    const editor::EditorResult leaving = back.take(
        editor::editor_command(back.model, back.state, back.list, editor::EditorCommand::back)
    );
    OA_CHECK(leaving.action == editor::EditorAction::ask_leave);

    Session clean(frame_for(1280, 720, 100));
    if (!clean.model.can_save())
        std::fprintf(stderr, "the fixture cannot save\n");
    OA_CHECK(clean.model.can_save());
    const editor::EditorResult saved = clean.take(
        editor::editor_command(clean.model, clean.state, clean.list, editor::EditorCommand::save)
    );
    OA_CHECK(saved.action == editor::EditorAction::save);
}

/// Up and Down on the focused list move the chosen row.
void arrows_move_the_row() {
    Session session(frame_for(1280, 720, 100));
    const std::size_t rows =
        session.model.rows(session.state.category, session.state.filter, "").size();
    OA_CHECK(rows > 2);
    focus_list(session);
    const std::string first = session.state.selected;
    session.take(editor::editor_key(session.model, session.state, session.list, kit::Key::down));
    focus_list(session);
    if (session.state.selected == first || !selected_is_listed(session.model, session.state))
        std::fprintf(stderr, "down stayed on \"%s\"\n", session.state.selected.c_str());
    OA_CHECK(session.state.selected != first);
    OA_CHECK(selected_is_listed(session.model, session.state));

    const std::string second = session.state.selected;
    session.take(editor::editor_key(session.model, session.state, session.list, kit::Key::down));
    focus_list(session);
    OA_CHECK(session.state.selected != second);
    OA_CHECK(selected_is_listed(session.model, session.state));

    const std::string third = session.state.selected;
    session.take(editor::editor_key(session.model, session.state, session.list, kit::Key::up));
    OA_CHECK(session.state.selected != third);
    OA_CHECK(selected_is_listed(session.model, session.state));
}

/// Enter on Compact's focused list opens the details page.
void enter_opens_details() {
    Session session(frame_for(640, 480, 100));
    OA_CHECK(session.frame.size_class == kit::SizeClass::compact);
    focus_list(session);
    session.take(editor::editor_key(session.model, session.state, session.list, kit::Key::down));
    focus_list(session);
    if (session.state.selected.empty())
        std::fprintf(stderr, "enter had no selected row\n");
    OA_CHECK(!session.state.selected.empty());
    session.take(editor::editor_key(session.model, session.state, session.list, kit::Key::enter));
    if (!session.state.details_page)
        std::fprintf(stderr, "enter did not open the details page\n");
    OA_CHECK(session.state.details_page);
}

/// A finger drag in the list scrolls it. A tap with no move chooses a row.
void finger_scrolls_and_taps() {
    constexpr int32_t reach = 12;
    bool scrolled = false;
    for (const int32_t dy : {int32_t{-80}, int32_t{80}}) {
        Session session(frame_for(1280, 720, 100));
        const kit::Control* list = find_named(session.list, editor::name_list);
        OA_CHECK(list != nullptr && has_room(press_area(*list)));
        if (list == nullptr || !has_room(press_area(*list)))
            continue;
        const kit::Point at = centre_of(*list);
        const kit::Point moved{at.x, at.y + dy};
        session.take(
            editor::editor_pointer(
                session.model, session.state, session.list, editor::PointerKind::down, 1, at, reach
            )
        );
        session.take(
            editor::editor_pointer(
                session.model,
                session.state,
                session.list,
                editor::PointerKind::move,
                0,
                moved,
                reach
            )
        );
        if (session.state.list_scroll != 0)
            scrolled = true;
    }
    if (!scrolled)
        std::fprintf(stderr, "a finger drag did not scroll the list\n");
    OA_CHECK(scrolled);

    Session tap(frame_for(1280, 720, 100));
    const kit::Control* row = nullptr;
    for (const kit::Control& control : tap.list.controls) {
        if (starts(control.name, editor::name_row_prefix) && has_room(press_area(control))) {
            row = &control;
            break;
        }
    }
    if (row == nullptr)
        std::fprintf(stderr, "the list has no row to tap\n");
    OA_CHECK(row != nullptr);
    if (row == nullptr)
        return;
    const kit::Point at = centre_of(*row);
    const std::string before = tap.state.selected;
    tap.take(
        editor::editor_pointer(
            tap.model, tap.state, tap.list, editor::PointerKind::down, 1, at, reach
        )
    );
    tap.take(
        editor::editor_pointer(
            tap.model, tap.state, tap.list, editor::PointerKind::up, 1, at, reach
        )
    );
    if (tap.state.selected == before || !selected_is_listed(tap.model, tap.state))
        std::fprintf(stderr, "tap selected \"%s\"\n", tap.state.selected.c_str());
    OA_CHECK(tap.state.selected != before);
    OA_CHECK(selected_is_listed(tap.model, tap.state));
}

} // namespace

int main() {
    search_and_escape();
    leave_and_save();
    arrows_move_the_row();
    enter_opens_details();
    finger_scrolls_and_taps();
    return oa::test::failed_checks() == 0 ? 0 : 1;
}
