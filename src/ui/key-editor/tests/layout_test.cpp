// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor laid out with the synthetic font at 640x480 (1x, Compact),
// 1280x720 (1x, Large) and 1920x1080 (2x, Regular). At each frame every
// control lies inside the window, no two controls meet apart from the list
// and what it holds, the names are sound and unique, and Tab reaches the
// layout (under MORE at Compact), the search, the list, Cancel, Save and close
// in that order. Every
// text fits, including a longer French lookup. Long German Save and Restore
// grow and stay inside the window. Compact's details page has Back, Save
// and Attack. The keyboard and the mouse are laid out at each class, and
// the ISO and JIS keyboards at Large. A key that scrolls past the well is
// clipped, so its own rectangle may leave the window.
//
// oa-ui-key-editor-layout-test

#include "oa/data/input_actions/actions.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/key_editor/text.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "screen_fixture.hpp"

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
namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;
using key_editor_fixture::editor_fixture;
using key_editor_fixture::fonts;
using key_editor_fixture::frame_for;
using key_editor_fixture::has_room;
using key_editor_fixture::outside_window;
using key_editor_fixture::press_area;
using key_editor_fixture::texts_not_fitting;

/// Prints each problem and fails the check when there are any.
///
/// @param problems the problems
/// @param where the frame they belong to
void expect_none(const std::vector<std::string>& problems, const std::string& where) {
    for (const std::string& problem : problems)
        std::fprintf(stderr, "%s: %s\n", where.c_str(), problem.c_str());
    OA_CHECK(problems.empty());
}

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

/// True for an open menu's item. Those may lie over the controls under the menu.
/// A list row is not one of them. The menu is closed unless a test opens it.
///
/// @param control the control
/// @return true for a menu item
bool menu_item(const kit::Control& control) {
    if (control.kind != kit::ControlKind::list_item)
        return false;
    return !starts(control.name, editor::name_row_prefix);
}

/// True when the outer control may hold the inner one: the list holds its rows,
/// the number-keys switch and its scroll bar, and a row may hold a chip.
///
/// @param outer one control
/// @param inner the other
/// @return true when the inner one may lie inside the outer
bool held_inside(const kit::Control& outer, const kit::Control& inner) {
    if (!kit::wholly_in(press_area(inner), press_area(outer)))
        return false;
    if (outer.name == editor::name_list) {
        return starts(inner.name, editor::name_row_prefix) ||
               inner.name == editor::name_number_keys || inner.name == editor::name_list_bar;
    }
    if (!starts(outer.name, editor::name_row_prefix))
        return false;
    return !starts(inner.name, editor::name_row_prefix);
}

/// Pairs of controls whose press areas meet, apart from a list and its rows
/// and an open menu's items.
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

/// Controls whose own rectangle, not only the clipped press, leaves the window.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each control outside
std::vector<std::string> rects_outside(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Control& control : list.controls) {
        if (control.rect.width > 0 && control.rect.height > 0 &&
            !kit::wholly_in(control.rect, whole))
            problems.push_back(control.name);
    }
    return problems;
}

/// Names that are used twice.
///
/// @param list the display list
/// @return a line for each repeated name
std::vector<std::string> repeated_names(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (std::size_t first_at = 0; first_at < list.controls.size(); ++first_at) {
        for (std::size_t second_at = first_at + 1; second_at < list.controls.size(); ++second_at) {
            if (list.controls[first_at].name == list.controls[second_at].name)
                problems.push_back(list.controls[first_at].name);
        }
    }
    return problems;
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

/// The place of a name in Tab's order, or -1.
///
/// @param names Tab's names
/// @param name the name
/// @return the place
int tab_index(const std::vector<std::string>& names, std::string_view name) {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name)
            return static_cast<int>(index);
    }
    return -1;
}

/// A control's width, or -1 when the list has no such control.
///
/// @param list the display list
/// @param name the name
/// @return the width, in points
int32_t control_width(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = find_named(list, name);
    if (control == nullptr)
        return -1;
    return control->rect.width;
}

/// The words a control shows: its text, or the button look drawn for it.
///
/// @param list the display list
/// @param name the name
/// @return the words; empty when the control is absent
std::string caption_of(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = find_named(list, name);
    if (control == nullptr)
        return {};
    if (!control->text.empty())
        return control->text;
    for (const kit::Item& item : list.items) {
        if (item.control != control->id)
            continue;
        if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
            return look->caption;
    }
    return {};
}

/// True when any drawn text or control text contains a word.
///
/// @param list the display list
/// @param word the word
/// @return true when it is shown
bool shows(const kit::DisplayList& list, std::string_view word) {
    const auto holds = [word](std::string_view text) {
        return text.find(word) != std::string_view::npos;
    };
    for (const kit::Item& item : list.items) {
        if (holds(item.text))
            return true;
        if (const auto* button = std::get_if<kit::ButtonLook>(&item.look);
            button && holds(button->caption))
            return true;
        if (const auto* header = std::get_if<kit::HeaderLook>(&item.look)) {
            if (holds(header->title) || holds(header->second) || holds(header->note))
                return true;
        }
        if (const auto* row = std::get_if<kit::ListRowLook>(&item.look)) {
            if (holds(row->title) || holds(row->by))
                return true;
            for (const std::string& line : row->lines) {
                if (holds(line))
                    return true;
            }
        }
        if (const auto* choice = std::get_if<kit::ChoiceLook>(&item.look);
            choice && holds(choice->text))
            return true;
        if (const auto* search = std::get_if<kit::SearchLook>(&item.look)) {
            if (holds(search->placeholder) || holds(search->field.text))
                return true;
        }
    }
    for (const kit::Control& control : list.controls) {
        if (holds(control.text))
            return true;
    }
    return false;
}

/// The search field's placeholder, or empty.
///
/// @param list the display list
/// @return the placeholder
std::string placeholder_of(const kit::DisplayList& list) {
    for (const kit::Item& item : list.items) {
        if (const auto* search = std::get_if<kit::SearchLook>(&item.look))
            return search->placeholder;
    }
    return {};
}

/// Lays the editor out.
///
/// @param model the editor
/// @param state the screen's state
/// @param frame the frame
/// @return the display list
kit::DisplayList
laid_out(const editor::Editor& model, const editor::ScreenState& state, const kit::Frame& frame) {
    return editor::editor_layout(model, state, frame, fonts());
}

/// Checks one frame's list page.
///
/// @param frame the frame
/// @param window the dialog size this frame must give
/// @param size_class the size class
/// @param where the case
void check_frame(
    const kit::Frame& frame, kit::Point window, kit::SizeClass size_class, const std::string& where
) {
    if (frame.size_class != size_class)
        std::fprintf(stderr, "%s: size class differs\n", where.c_str());
    OA_CHECK(frame.size_class == size_class);
    const editor::Editor model(editor_fixture());
    const editor::ScreenState state;
    const kit::Point sized = editor::window_size(frame);
    if (sized.x != window.x || sized.y != window.y) {
        std::fprintf(
            stderr,
            "%s: window %d x %d\n",
            where.c_str(),
            static_cast<int>(sized.x),
            static_cast<int>(sized.y)
        );
    }
    OA_CHECK(sized.x == window.x && sized.y == window.y);

    const kit::DisplayList list = laid_out(model, state, frame);
    expect_none(outside_window(list, sized), where + " drawn");
    expect_none(rects_outside(list, sized), where + " control");
    expect_none(meeting_controls(list), where + " meets");
    expect_none(texts_not_fitting(list, fonts()), where + " text");
    const std::string names = kit::name_problem(list);
    if (!names.empty())
        std::fprintf(stderr, "%s: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    expect_none(repeated_names(list), where + " name");

    const std::vector<std::string> tabs = tab_names(list);
    // Compact has no LAYOUT drop-down: it lives under MORE.
    const std::string_view with_layout[] = {
        editor::name_layout,
        editor::name_search,
        editor::name_list,
        editor::name_cancel,
        editor::name_save,
        editor::name_close,
    };
    const std::string_view compact_order[] = {
        editor::name_search,
        editor::name_list,
        editor::name_cancel,
        editor::name_save,
        editor::name_close,
    };
    const bool compact_frame = size_class == kit::SizeClass::compact;
    const std::string_view* order = compact_frame ? compact_order : with_layout;
    const std::size_t order_count = compact_frame ? 5 : 6;
    int previous = -1;
    for (std::size_t step = 0; step < order_count; ++step) {
        const std::string_view name = order[step];
        const int index = tab_index(tabs, name);
        if (index <= previous)
            std::fprintf(
                stderr,
                "%s: tab misses %.*s after %d\n",
                where.c_str(),
                static_cast<int>(name.size()),
                name.data(),
                previous
            );
        OA_CHECK(index > previous);
        previous = index;
    }
}

/// French for the chrome words this test lengthens. Anything else stays English.
///
/// @param context unused
/// @param english the English word
/// @return the text to show
std::string french_shown(void* /*context*/, std::string_view english) {
    using editor::words::cancel_title;
    using editor::words::layout;
    using editor::words::restore_title;
    using editor::words::save_changes;
    using editor::words::search_commands;
    if (english == layout)
        return "DISPOSITION DES TOUCHES";
    if (english == save_changes)
        return "Enregistrer";
    if (english == cancel_title)
        return "Annuler la saisie";
    if (english == restore_title)
        return "Rétablir les valeurs par défaut";
    if (english == search_commands)
        return "Rechercher des commandes";
    return std::string(english);
}

/// German long enough to grow Save and Restore. Anything else stays English.
///
/// @param context unused
/// @param english the English word
/// @return the text to show
std::string german_shown(void* /*context*/, std::string_view english) {
    if (english == editor::words::save_changes)
        return "Speichern Sie bitte dieses Layout";
    if (english == editor::words::restore_title)
        return "Standardwerte wiederherstellen";
    return std::string(english);
}

/// An editor whose words come from a hook.
///
/// @param shown the hook
/// @return the editor
editor::Editor hooked(std::string (*shown)(void*, std::string_view)) {
    editor::EditorInputs inputs = editor_fixture();
    inputs.text.shown = shown;
    return editor::Editor(std::move(inputs));
}

/// French chrome still fits, and the looked-up words are the ones drawn.
///
/// @param frame the frame
/// @param where the case
void check_french(const kit::Frame& frame, const std::string& where) {
    const editor::Editor model = hooked(french_shown);
    const editor::ScreenState state;
    const kit::Point window = editor::window_size(frame);
    const kit::DisplayList list = laid_out(model, state, frame);
    expect_none(outside_window(list, window), where + " drawn");
    expect_none(rects_outside(list, window), where + " control");
    expect_none(texts_not_fitting(list, fonts()), where + " text");
    const std::string save = caption_of(list, editor::name_save);
    if (save != "Enregistrer")
        std::fprintf(stderr, "%s: save shows \"%s\"\n", where.c_str(), save.c_str());
    OA_CHECK(save == "Enregistrer");
    const std::string want = french_shown(nullptr, editor::words::search_commands);
    const std::string placeholder = placeholder_of(list);
    if (placeholder != want)
        std::fprintf(stderr, "%s: search shows \"%s\"\n", where.c_str(), placeholder.c_str());
    OA_CHECK(placeholder == want);
    // Compact keeps LAYOUT under MORE, so the closed screen does not draw it.
    if (frame.size_class != kit::SizeClass::compact) {
        if (!shows(list, "DISPOSITION DES TOUCHES"))
            std::fprintf(stderr, "%s: LAYOUT was not looked up\n", where.c_str());
        OA_CHECK(shows(list, "DISPOSITION DES TOUCHES"));
    }
}

/// Save and Restore grow for the long German and stay inside the window.
///
/// @param frame the frame
/// @param where the case
void check_german(const kit::Frame& frame, const std::string& where) {
    const editor::Editor english(editor_fixture());
    const editor::Editor german = hooked(german_shown);
    const editor::ScreenState state;
    const kit::DisplayList before = laid_out(english, state, frame);
    const kit::DisplayList after = laid_out(german, state, frame);
    const kit::Point window = editor::window_size(frame);
    expect_none(outside_window(after, window), where + " drawn");
    expect_none(rects_outside(after, window), where + " control");
    expect_none(texts_not_fitting(after, fonts()), where + " text");

    const int32_t save_before = control_width(before, editor::name_save);
    const int32_t save_after = control_width(after, editor::name_save);
    if (save_after <= save_before) {
        std::fprintf(
            stderr,
            "%s: save width %d then %d\n",
            where.c_str(),
            static_cast<int>(save_before),
            static_cast<int>(save_after)
        );
    }
    OA_CHECK(save_after > save_before);
    const std::string save = caption_of(after, editor::name_save);
    OA_CHECK(save == "Speichern Sie bitte dieses Layout");

    // Restore defaults is an item of More: the menu grows for the German.
    editor::ScreenState more;
    more.menu = editor::Menu::more;
    const kit::DisplayList more_before = laid_out(english, more, frame);
    const kit::DisplayList more_after = laid_out(german, more, frame);
    expect_none(texts_not_fitting(more_after, fonts()), where + " More text");
    const int32_t restore_before = control_width(more_before, editor::name_restore);
    const int32_t restore_after = control_width(more_after, editor::name_restore);
    if (restore_before < 0 || restore_after < 0)
        std::fprintf(stderr, "%s: restore is missing from More\n", where.c_str());
    OA_CHECK(restore_before >= 0 && restore_after >= 0);
    if (restore_before >= 0 && restore_after >= 0) {
        if (restore_after <= restore_before) {
            std::fprintf(
                stderr,
                "%s: restore width %d then %d\n",
                where.c_str(),
                static_cast<int>(restore_before),
                static_cast<int>(restore_after)
            );
        }
        OA_CHECK(restore_after > restore_before);
        OA_CHECK(caption_of(more_after, editor::name_restore) == "Standardwerte wiederherstellen");
    }
}

/// Compact's details page holds Back, Save and Attack inside the window.
void check_details() {
    const kit::Frame frame = frame_for(640, 480, 100);
    const editor::Editor model(editor_fixture());
    editor::ScreenState state;
    state.details_page = true;
    state.selected = "order.attack";
    const kit::Point window = editor::window_size(frame);
    const kit::DisplayList page = editor::details_page_layout(model, state, frame, fonts());
    const kit::DisplayList delegated = editor::editor_layout(model, state, frame, fonts());
    for (const kit::DisplayList& list : {page, delegated}) {
        expect_none(outside_window(list, window), "compact details drawn");
        expect_none(rects_outside(list, window), "compact details control");
        if (find_named(list, editor::name_back) == nullptr)
            std::fprintf(stderr, "compact details: no Back\n");
        OA_CHECK(find_named(list, editor::name_back) != nullptr);
        if (find_named(list, editor::name_save) == nullptr)
            std::fprintf(stderr, "compact details: no Save\n");
        OA_CHECK(find_named(list, editor::name_save) != nullptr);
        if (!shows(list, "Attack"))
            std::fprintf(stderr, "compact details: Attack is not shown\n");
        OA_CHECK(shows(list, "Attack"));
        if (find_named(list, editor::name_detail_add) == nullptr)
            std::fprintf(stderr, "compact details: no + ADD KEY\n");
        OA_CHECK(find_named(list, editor::name_detail_add) != nullptr);
    }
}

/// A letter chord with Primary held.
///
/// @param character the letter
/// @return the chord
keys::Chord primary_letter(char32_t character) {
    keys::Chord chord;
    chord.modifiers = keys::primary;
    chord.kind = keys::TriggerKind::character;
    chord.character = character;
    return chord;
}

/// True when a named control is enabled.
///
/// @param list the display list
/// @param name the name
/// @return true when it is shown and enabled
bool control_enabled(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = find_named(list, name);
    return control != nullptr && control->enabled;
}

/// The capture, the clash prompt and the card at one size.
///
/// @param frame the frame
/// @param where the size
void check_prompts(const kit::Frame& frame, const std::string& where) {
    const editor::Editor model(editor_fixture());
    const kit::Point window = editor::window_size(frame);
    const auto check_list = [&](const kit::DisplayList& list, const std::string& name) {
        expect_none(outside_window(list, window), name + " drawn");
        expect_none(rects_outside(list, window), name + " control");
        expect_none(meeting_controls(list), name + " meets");
        expect_none(texts_not_fitting(list, fonts()), name + " text");
        const std::string names = kit::name_problem(list);
        if (!names.empty())
            std::fprintf(stderr, "%s: %s\n", name.c_str(), names.c_str());
        OA_CHECK(names.empty());
        expect_none(repeated_names(list), name + " name");
    };
    const auto require_named =
        [&](const kit::DisplayList& list, std::string_view name, const std::string& why) {
            if (find_named(list, name) == nullptr)
                std::fprintf(stderr, "%s: missing %s\n", why.c_str(), std::string(name).c_str());
            OA_CHECK(find_named(list, name) != nullptr);
        };

    {
        editor::ScreenState state;
        state.prompt = editor::Prompt::capture;
        state.selected = "order.attack";
        state.capture.action = "order.attack";
        state.capture.means = acts::By::letter;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " waiting");
        require_named(list, editor::name_capture, where + " waiting");
        require_named(list, editor::name_capture_well, where + " waiting");
        require_named(list, editor::name_capture_more, where + " waiting");
        require_named(list, editor::name_capture_cancel, where + " waiting");
        require_named(list, editor::name_capture_ok, where + " waiting");
        OA_CHECK(!control_enabled(list, editor::name_capture_ok));
        if (!shows(list, "Press a key"))
            std::fprintf(stderr, "%s waiting: the well has no prompt\n", where.c_str());
        OA_CHECK(shows(list, "Press a key"));
        state.capture_more = true;
        const kit::DisplayList more = laid_out(model, state, frame);
        check_list(more, where + " waiting, More options");
        require_named(more, editor::name_capture_means, where + " More options");
        require_named(more, editor::name_capture_press, where + " More options");
        require_named(more, editor::name_capture_double, where + " More options");
        require_named(more, editor::name_capture_hold, where + " More options");
    }
    {
        editor::ScreenState state;
        state.prompt = editor::Prompt::capture;
        state.selected = "order.attack";
        state.capture.action = "order.attack";
        state.capture.chord = primary_letter(U'Q');
        state.capture.state = editor::CaptureState::complete;
        state.capture.means = acts::By::letter;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " complete");
        require_named(list, editor::name_capture_status, where + " complete");
        if (!shows(list, "is free"))
            std::fprintf(stderr, "%s complete: the chord is not free\n", where.c_str());
        OA_CHECK(shows(list, "is free"));
        OA_CHECK(control_enabled(list, editor::name_capture_ok));
    }
    {
        editor::ScreenState state;
        state.prompt = editor::Prompt::capture;
        state.selected = "order.attack";
        state.capture.action = "order.attack";
        state.capture.chord = primary_letter(U'B');
        state.capture.state = editor::CaptureState::clash;
        state.capture.means = acts::By::letter;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " clash");
        if (!shows(list, "Select builders"))
            std::fprintf(
                stderr, "%s clash: the line does not name Select builders\n", where.c_str()
            );
        OA_CHECK(shows(list, "Select builders"));
        OA_CHECK(control_enabled(list, editor::name_capture_ok));
    }
    {
        editor::ScreenState state;
        state.prompt = editor::Prompt::capture;
        state.selected = "chat.send";
        state.capture.action = "chat.send";
        state.capture.state = editor::CaptureState::refused;
        state.capture.means = acts::By::place;
        keys::Chord escape;
        escape.kind = keys::TriggerKind::named;
        escape.named = static_cast<uint8_t>(keys::NamedKey::escape);
        state.capture.chord = escape;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " refused");
        if (!shows(list, "Choose another key"))
            std::fprintf(stderr, "%s refused: the line does not refuse the chord\n", where.c_str());
        OA_CHECK(shows(list, "Choose another key"));
        OA_CHECK(!control_enabled(list, editor::name_capture_ok));
    }
    {
        editor::ScreenState state;
        state.prompt = editor::Prompt::clash;
        state.selected = "select.all";
        state.capture.action = "select.all";
        state.capture.replacing = 0;
        state.capture.chord = primary_letter(U'B');
        state.capture.state = editor::CaptureState::clash;
        state.pending_clash.a = "select.all";
        state.pending_clash.b = "select.builders";
        state.pending_clash.chord = *state.capture.chord;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " question");
        require_named(list, editor::name_clash, where + " question");
        require_named(list, editor::name_clash_replace, where + " question");
        require_named(list, editor::name_clash_swap, where + " question");
        require_named(list, editor::name_clash_cancel, where + " question");
        require_named(list, editor::name_clash_confirm, where + " question");
        require_named(list, editor::name_clash_another, where + " question");
        state.capture.replacing.reset();
        const kit::DisplayList added = laid_out(model, state, frame);
        check_list(added, where + " question add");
        if (find_named(added, editor::name_clash_swap) != nullptr)
            std::fprintf(stderr, "%s: + ADD KEY shows SWAP\n", where.c_str());
        OA_CHECK(find_named(added, editor::name_clash_swap) == nullptr);
    }
    {
        editor::ScreenState state;
        state.selected = "select.all";
        state.filter = editor::Filter::clashes;
        state.details_page = frame.size_class == kit::SizeClass::compact;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " card");
        require_named(list, editor::name_card_settle, where + " card");
        state.prompt = editor::Prompt::clash;
        state.settling = true;
        if (const std::optional<keys::Clash> clash = model.draft_clash("select.all"))
            state.pending_clash = *clash;
        const kit::DisplayList settling = laid_out(model, state, frame);
        check_list(settling, where + " settle");
        require_named(settling, editor::name_card_here, where + " settle");
        require_named(settling, editor::name_card_there, where + " settle");
        require_named(settling, editor::name_clash_confirm, where + " settle");
    }
    {
        editor::ScreenState state;
        state.prompt = editor::Prompt::find;
        state.finding.chord = primary_letter(U'B');
        state.finding.state = editor::CaptureState::complete;
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " find");
        require_named(list, editor::name_find_prompt, where + " find");
        require_named(list, editor::name_find_well, where + " find");
        require_named(list, std::string(editor::name_find_row_prefix) + "1", where + " find");
        require_named(list, editor::name_find_close, where + " find");
    }
}

/// True for one key of the keyboard picture.
///
/// @param name the control's name
/// @return true when it is a key
bool key_control(std::string_view name) {
    return starts(name, "key-editor.keyboard.");
}

/// True for one button of the mouse picture, not a row of the table.
///
/// @param name the control's name
/// @return true when it is a button on the picture
bool mouse_button_control(std::string_view name) {
    return starts(name, "key-editor.mouse.");
}

/// Controls whose own rectangle leaves the window, apart from a key the
/// compact keyboard has scrolled past the well. The press is clipped.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each control outside
std::vector<std::string> picture_rects(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Control& control : list.controls) {
        if (key_control(control.name))
            continue;
        if (control.rect.width > 0 && control.rect.height > 0 &&
            !kit::wholly_in(control.rect, whole))
            problems.push_back(control.name);
    }
    return problems;
}

/// Pairs whose press areas meet. Keys share the picture's grid, and the
/// mouse's buttons share its drawing, so those pairs are not a meeting.
/// A key that meets the card still is.
///
/// @param list the display list
/// @return a line for each pair
std::vector<std::string> picture_meetings(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (std::size_t first_at = 0; first_at < list.controls.size(); ++first_at) {
        for (std::size_t second_at = first_at + 1; second_at < list.controls.size(); ++second_at) {
            const kit::Control& first = list.controls[first_at];
            const kit::Control& second = list.controls[second_at];
            if (menu_item(first) || menu_item(second))
                continue;
            if (held_inside(first, second) || held_inside(second, first))
                continue;
            if (key_control(first.name) && key_control(second.name))
                continue;
            if (mouse_button_control(first.name) && mouse_button_control(second.name))
                continue;
            if (has_room(kit::intersect(press_area(first), press_area(second))))
                problems.push_back(first.name + " meets " + second.name);
        }
    }
    return problems;
}

/// The keyboard or the mouse at one size. Keys may scroll past the window.
/// The card sits under the picture, and to the right of the legend at Regular
/// and Large.
///
/// @param frame the frame
/// @param window the dialog size this frame must give
/// @param where the case
/// @param shown the picture
/// @param shape the keyboard, when the picture is the keyboard
void check_view(
    const kit::Frame& frame,
    kit::Point window,
    const std::string& where,
    editor::View shown,
    kit::KeyboardShape shape
) {
    const editor::Editor model(editor_fixture());
    editor::ScreenState state;
    state.view = shown;
    state.shape = shape;
    state.shape_from_system = shape == kit::KeyboardShape::ansi;
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Point sized = editor::window_size(frame);
    OA_CHECK(sized.x == window.x && sized.y == window.y);
    expect_none(outside_window(list, sized), where + " drawn");
    expect_none(picture_rects(list, sized), where + " control");
    expect_none(picture_meetings(list), where + " meets");
    expect_none(texts_not_fitting(list, fonts()), where + " text");
    const std::string names = kit::name_problem(list);
    if (!names.empty())
        std::fprintf(stderr, "%s: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    expect_none(repeated_names(list), where + " name");
    if (find_named(list, editor::name_view_list) == nullptr)
        std::fprintf(stderr, "%s: no view tabs\n", where.c_str());
    OA_CHECK(find_named(list, editor::name_view_list) != nullptr);
    OA_CHECK(find_named(list, editor::name_view_keyboard) != nullptr);
    OA_CHECK(find_named(list, editor::name_view_mouse) != nullptr);

    if (shown == editor::View::keyboard) {
        OA_CHECK(find_named(list, editor::name_layer_none) != nullptr);
        OA_CHECK(find_named(list, editor::name_keyboard_shape) != nullptr);
        int32_t lowest = 0;
        int keys = 0;
        for (const kit::Control& control : list.controls) {
            if (!key_control(control.name))
                continue;
            ++keys;
            const int32_t bottom = control.rect.y + control.rect.height;
            if (bottom > lowest)
                lowest = bottom;
        }
        if (keys == 0)
            std::fprintf(stderr, "%s: the keyboard has no keys\n", where.c_str());
        OA_CHECK(keys > 0);
        // The inspector is a column at the right of the picture at Regular
        // and Large, and under it at Compact.
        const kit::Control* card = find_named(list, editor::name_key_card);
        int32_t rightmost = 0;
        for (const kit::Control& control : list.controls) {
            if (key_control(control.name) && control.clip.width > 0)
                rightmost = std::max(rightmost, control.clip.x + control.clip.width);
        }
        const bool beside = window.x >= 720;
        const bool placed =
            card != nullptr && (beside ? card->rect.x >= rightmost : card->rect.y >= lowest);
        if (!placed)
            std::fprintf(
                stderr, "%s: the inspector is not beside or under the keyboard\n", where.c_str()
            );
        OA_CHECK(placed);
        OA_CHECK(find_named(list, editor::name_show_as_list) != nullptr);
        return;
    }

    OA_CHECK(find_named(list, editor::name_scheme_right) != nullptr);
    OA_CHECK(find_named(list, editor::name_scheme_left) != nullptr);
    OA_CHECK(find_named(list, "key-editor.mouse.mouse-left") != nullptr);
    bool table = false;
    for (const kit::Control& control : list.controls) {
        if (starts(control.name, editor::name_mouse_table_prefix))
            table = true;
    }
    if (!table)
        std::fprintf(stderr, "%s: the mouse table has no row\n", where.c_str());
    OA_CHECK(table);
    // At Regular and Large a row's control text names the button and the role.
    if (window.x >= 720) {
        bool labelled = false;
        const std::string playing = std::string(editor::name_mouse_table_prefix) + "playing";
        for (const kit::Control& control : list.controls) {
            if (!starts(control.name, playing))
                continue;
            if (control.text.find(" · ") != std::string::npos)
                labelled = true;
        }
        if (!labelled)
            std::fprintf(stderr, "%s: a playing row does not name its button\n", where.c_str());
        OA_CHECK(labelled);
    }
}

} // namespace

/// Import…, Share… and Paste… are items of the footer's More at every class.
/// Share… shows the id a name folds to.
///
/// @param frame the frame
/// @param where the case
void check_exchange(const kit::Frame& frame, const std::string& where) {
    const editor::Editor model(editor_fixture());
    editor::ScreenState closed;
    const kit::DisplayList list = laid_out(model, closed, frame);
    OA_CHECK(find_named(list, editor::name_more) != nullptr);
    OA_CHECK(find_named(list, editor::name_import) == nullptr);
    closed.menu = editor::Menu::more;
    const kit::DisplayList opened = laid_out(model, closed, frame);
    const bool import_shown = find_named(opened, editor::name_import) != nullptr;
    const bool share_shown = find_named(opened, editor::name_share) != nullptr;
    const bool paste_shown = find_named(opened, editor::name_paste) != nullptr;
    const bool restore_shown = find_named(opened, editor::name_restore) != nullptr;
    if (!import_shown || !share_shown || !paste_shown || !restore_shown) {
        std::fprintf(
            stderr,
            "%s: More's items import %d share %d paste %d restore %d\n",
            where.c_str(),
            import_shown ? 1 : 0,
            share_shown ? 1 : 0,
            paste_shown ? 1 : 0,
            restore_shown ? 1 : 0
        );
    }
    OA_CHECK(import_shown && share_shown && paste_shown && restore_shown);

    editor::Editor named(editor_fixture());
    named.set_share_name("Grid, left hand");
    editor::ScreenState share;
    share.prompt = editor::Prompt::share;
    share.share_field.text = "Grid, left hand";
    share.share_field.caret = static_cast<int32_t>(share.share_field.text.size());
    const kit::Point window = editor::window_size(frame);
    const kit::DisplayList laid = laid_out(named, share, frame);
    expect_none(outside_window(laid, window), where + " share drawn");
    expect_none(rects_outside(laid, window), where + " share control");
    expect_none(meeting_controls(laid), where + " share meets");
    expect_none(texts_not_fitting(laid, fonts()), where + " share text");
    const std::string names = kit::name_problem(laid);
    if (!names.empty())
        std::fprintf(stderr, "%s share: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    const kit::Control* id = find_named(laid, editor::name_share_id);
    if (id == nullptr || id->text != "grid-left-hand")
        std::fprintf(
            stderr, "%s: share id is \"%s\"\n", where.c_str(), id == nullptr ? "" : id->text.c_str()
        );
    OA_CHECK(id != nullptr && id->text == "grid-left-hand");
    OA_CHECK(find_named(laid, editor::name_share_copy) != nullptr);
    OA_CHECK(control_enabled(laid, editor::name_share_save));
}

/// True when one drawn line holds `kept` and not `other`.
///
/// @param list the display list
/// @param kept the words that line shows
/// @param other words from the next row, which must not share the line
/// @return true when such a line is drawn
bool line_holds(const kit::DisplayList& list, std::string_view kept, std::string_view other) {
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::text)
            continue;
        if (item.text.find(kept) != std::string::npos && item.text.find(other) == std::string::npos)
            return true;
    }
    return false;
}

/// The share, paste and import blocks keep each source line on its own line,
/// a count of one uses the singular word, and a share text taller than the
/// Compact room scrolls under the wheel.
///
/// @param frame the frame
/// @param where the size
void check_exchange_lines(const kit::Frame& frame, const std::string& where) {
    const editor::Editor model(editor_fixture());
    const kit::Point window = editor::window_size(frame);
    const auto check_list = [&](const kit::DisplayList& list, const std::string& name) {
        expect_none(outside_window(list, window), name + " drawn");
        expect_none(rects_outside(list, window), name + " control");
        expect_none(meeting_controls(list), name + " meets");
        expect_none(texts_not_fitting(list, fonts()), name + " text");
    };

    editor::Editor named(editor_fixture());
    named.set_share_name("Grid, left hand");
    editor::ScreenState share;
    share.prompt = editor::Prompt::share;
    share.share_field.text = "Grid, left hand";
    const kit::DisplayList shared = laid_out(named, share, frame);
    check_list(shared, where + " share lines");
    const kit::Control* share_text = find_named(shared, editor::name_share_text);
    if (share_text == nullptr || share_text->text.find("\norder.attack:") == std::string::npos ||
        share_text->text.find("\nselect.all:") == std::string::npos) {
        std::fprintf(stderr, "%s: the share text is not three lines\n", where.c_str());
    }
    OA_CHECK(
        share_text != nullptr && share_text->text.find("\norder.attack:") != std::string::npos &&
        share_text->text.find("\nselect.all:") != std::string::npos
    );
    if (!line_holds(shared, "order.attack:", "select.all:"))
        std::fprintf(stderr, "%s: order.attack shares a line with select.all\n", where.c_str());
    OA_CHECK(line_holds(shared, "order.attack:", "select.all:"));
    if (!shows(shared, "2 keys, 0 helpers"))
        std::fprintf(stderr, "%s: the share count is not plural\n", where.c_str());
    OA_CHECK(shows(shared, "2 keys, 0 helpers"));

    editor::EditorInputs one = editor_fixture();
    one.chosen.layout.bindings.clear();
    one.own.bindings.clear();
    one.own.bindings.push_back(key_editor_fixture::binding_of("order.attack", "W"));
    one.helpers.sticky_modifiers = true;
    editor::Editor single(std::move(one));
    editor::ScreenState one_share;
    one_share.prompt = editor::Prompt::share;
    const kit::DisplayList one_laid = laid_out(single, one_share, frame);
    check_list(one_laid, where + " one key");
    if (!shows(one_laid, "1 key, 1 helper"))
        std::fprintf(stderr, "%s: one key and one helper are not singular\n", where.c_str());
    OA_CHECK(shows(one_laid, "1 key, 1 helper"));

    editor::ScreenState pasted;
    pasted.prompt = editor::Prompt::paste;
    constexpr std::string_view sample = "# oakeys bindings, base oa-default\n"
                                        "order.move: [\"@KeyQ\"]\n"
                                        "select.all: [Ctrl+B]\n"
                                        "camera.orbit-left: [Q]\n";
    const auto* bytes = reinterpret_cast<const uint8_t*>(sample.data());
    pasted.paste = editor::paste_preview(model, {bytes, sample.size()});
    const kit::DisplayList paste_list = laid_out(model, pasted, frame);
    check_list(paste_list, where + " paste lines");
    const kit::Control* rows = find_named(paste_list, editor::name_paste_list);
    if (rows == nullptr || rows->text.find('\n') == std::string::npos) {
        std::fprintf(stderr, "%s: the paste rows are one line\n", where.c_str());
    }
    OA_CHECK(rows != nullptr && rows->text.find('\n') != std::string::npos);
    if (!line_holds(paste_list, "order.move", "select.all"))
        std::fprintf(stderr, "%s: order.move shares a line with select.all\n", where.c_str());
    OA_CHECK(line_holds(paste_list, "order.move", "select.all"));
    const std::string apply = caption_of(paste_list, editor::name_paste_apply);
    if (apply.find("Apply 3 changes") == std::string::npos)
        std::fprintf(stderr, "%s: APPLY is \"%s\"\n", where.c_str(), apply.c_str());
    OA_CHECK(apply.find("Apply 3 changes") != std::string::npos);

    editor::ScreenState one_paste;
    one_paste.prompt = editor::Prompt::paste;
    one_paste.paste.changes = 1;
    const kit::DisplayList one_paste_list = laid_out(model, one_paste, frame);
    const std::string one_summary = caption_of(one_paste_list, editor::name_paste_summary);
    const std::string one_apply = caption_of(one_paste_list, editor::name_paste_apply);
    if (one_summary.find("1 key of") == std::string::npos ||
        one_summary.find("1 keys") != std::string::npos) {
        std::fprintf(stderr, "%s: paste summary is \"%s\"\n", where.c_str(), one_summary.c_str());
    }
    OA_CHECK(
        one_summary.find("1 key of") != std::string::npos &&
        one_summary.find("1 keys") == std::string::npos
    );
    if (one_apply != "Apply 1 change")
        std::fprintf(stderr, "%s: APPLY one is \"%s\"\n", where.c_str(), one_apply.c_str());
    OA_CHECK(one_apply == "Apply 1 change");

    editor::ScreenState importing;
    importing.prompt = editor::Prompt::import;
    importing.import_chosen = 0;
    editor::ImportOffer offer;
    offer.file = "Settings.ini";
    importing.import_offers.push_back(offer);
    importing.importing.found = 2;
    importing.importing.differ = 2;
    importing.importing.file = "Settings.ini";
    editor::ImportRow first;
    first.action = "megamap.toggle";
    editor::ImportRow second;
    second.action = "whiteboard.draw";
    importing.importing.rows.push_back(std::move(first));
    importing.importing.rows.push_back(std::move(second));
    const kit::DisplayList imported = laid_out(model, importing, frame);
    check_list(imported, where + " import lines");
    if (!line_holds(imported, "megamap.toggle", "whiteboard.draw")) {
        std::fprintf(
            stderr, "%s: megamap.toggle shares a line with whiteboard.draw\n", where.c_str()
        );
    }
    OA_CHECK(line_holds(imported, "megamap.toggle", "whiteboard.draw"));
    if (!shows(imported, "Found 2 keys"))
        std::fprintf(stderr, "%s: two keys are not plural\n", where.c_str());
    OA_CHECK(shows(imported, "Found 2 keys"));

    importing.importing.found = 1;
    importing.importing.differ = 1;
    importing.importing.rows.pop_back();
    const kit::DisplayList one_import = laid_out(model, importing, frame);
    const std::string found = caption_of(one_import, editor::name_import_summary);
    const std::string import_apply = caption_of(one_import, editor::name_import_apply);
    if (found.find("Found 1 key") == std::string::npos ||
        found.find("Found 1 keys") != std::string::npos ||
        found.find("1 differs") == std::string::npos) {
        std::fprintf(stderr, "%s: import summary is \"%s\"\n", where.c_str(), found.c_str());
    }
    OA_CHECK(
        found.find("Found 1 key") != std::string::npos &&
        found.find("Found 1 keys") == std::string::npos &&
        found.find("1 differs") != std::string::npos
    );
    if (import_apply != "Import 1 key")
        std::fprintf(stderr, "%s: IMPORT one is \"%s\"\n", where.c_str(), import_apply.c_str());
    OA_CHECK(import_apply == "Import 1 key");

    if (frame.size_class != kit::SizeClass::compact)
        return;
    editor::EditorInputs many = editor_fixture();
    for (int32_t index = 0; index < 40; ++index) {
        many.own.bindings.push_back(
            key_editor_fixture::binding_of("shared.line." + std::to_string(index), "Q")
        );
    }
    editor::Editor tall(std::move(many));
    editor::ScreenState scrolling = share;
    const kit::DisplayList tall_list = laid_out(tall, scrolling, frame);
    check_list(tall_list, where + " share scroll");
    const kit::Control* block = find_named(tall_list, editor::name_share_text);
    const kit::Control* bar = find_named(tall_list, editor::name_exchange_bar);
    if (block == nullptr || bar == nullptr || bar->group <= block->rect.height) {
        std::fprintf(
            stderr,
            "%s: the share bar is missing (text %d, bar %d)\n",
            where.c_str(),
            block == nullptr ? 0 : block->rect.height,
            bar == nullptr ? 0 : bar->group
        );
    }
    OA_CHECK(block != nullptr && bar != nullptr && bar->group > block->rect.height);
    if (block == nullptr || bar == nullptr)
        return;
    const kit::Point at{
        block->rect.x + block->rect.width / 2, block->rect.y + block->rect.height / 2
    };
    const editor::EditorResult turned = editor::editor_wheel(tall, scrolling, tall_list, at, -1.F);
    if (!turned.redraw || scrolling.exchange_scroll <= 0) {
        std::fprintf(
            stderr,
            "%s: the wheel left the share text at %d\n",
            where.c_str(),
            scrolling.exchange_scroll
        );
    }
    OA_CHECK(turned.redraw && scrolling.exchange_scroll > 0);
}

/// Checks that the window's header is Settings' and the Library's: Kit 2's
/// window header at Regular and Large and the kit's band at Compact, with the
/// close mark in its middle.
///
/// @param frame the frame
/// @param where the case
void check_shell(const kit::Frame& frame, const std::string& where) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const editor::Editor model(editor_fixture());
    const kit::DisplayList list = laid_out(model, {}, frame);
    const int32_t header = compact ? metrics.header_height : metrics.window_header_height;
    bool header_drawn = false;
    for (const kit::Item& item : list.items) {
        if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
            header_drawn = look->height == header && item.rect.height == header + 2;
    }
    if (!header_drawn)
        std::fprintf(stderr, "%s: the header is not %d points\n", where.c_str(), header);
    OA_CHECK(header_drawn);
    const kit::Control* close = find_named(list, editor::name_close);
    OA_CHECK(close != nullptr && close->rect.y == metrics.edge + (header - close->rect.height) / 2);
}

int main() {
    const kit::Frame compact = frame_for(640, 480, 100);
    const kit::Frame large = frame_for(1280, 720, 100);
    const kit::Frame regular = frame_for(1920, 1080, 200);
    if (compact.area.width != 640 || compact.area.height != 480)
        std::fprintf(stderr, "640x480 frame is not 640x480 points\n");
    OA_CHECK(compact.area.width == 640 && compact.area.height == 480);
    if (large.area.width != 1280 || large.area.height != 720)
        std::fprintf(stderr, "1280x720 frame is not 1280x720 points\n");
    OA_CHECK(large.area.width == 1280 && large.area.height == 720);
    if (regular.area.width != 960 || regular.area.height != 540 || regular.scale_percent != 200)
        std::fprintf(stderr, "1920x1080 at 2x is not 960x540\n");
    OA_CHECK(
        regular.area.width == 960 && regular.area.height == 540 && regular.scale_percent == 200
    );

    check_frame(compact, {480, 324}, kit::SizeClass::compact, "compact");
    check_frame(large, {960, 600}, kit::SizeClass::large, "large");
    check_frame(regular, {720, 486}, kit::SizeClass::regular, "regular");
    check_shell(compact, "compact");
    check_shell(large, "large");
    check_shell(regular, "regular");

    check_french(compact, "french compact");
    check_french(large, "french large");
    check_french(regular, "french regular");
    check_german(compact, "german compact");
    check_german(large, "german large");
    check_german(regular, "german regular");
    check_details();
    check_prompts(compact, "compact");
    check_prompts(large, "large");
    check_prompts(regular, "regular");
    check_exchange(compact, "compact");
    check_exchange(large, "large");
    check_exchange(regular, "regular");
    check_exchange_lines(compact, "compact");
    check_exchange_lines(large, "large");
    check_exchange_lines(regular, "regular");

    check_view(
        compact, {480, 324}, "keyboard compact", editor::View::keyboard, kit::KeyboardShape::ansi
    );
    check_view(
        large, {960, 600}, "keyboard large", editor::View::keyboard, kit::KeyboardShape::ansi
    );
    check_view(
        regular, {720, 486}, "keyboard regular", editor::View::keyboard, kit::KeyboardShape::ansi
    );
    check_view(large, {960, 600}, "keyboard iso", editor::View::keyboard, kit::KeyboardShape::iso);
    check_view(large, {960, 600}, "keyboard jis", editor::View::keyboard, kit::KeyboardShape::jis);
    check_view(compact, {480, 324}, "mouse compact", editor::View::mouse, kit::KeyboardShape::ansi);
    check_view(large, {960, 600}, "mouse large", editor::View::mouse, kit::KeyboardShape::ansi);
    check_view(regular, {720, 486}, "mouse regular", editor::View::mouse, kit::KeyboardShape::ansi);
    return oa::test::failed_checks() == 0 ? 0 : 1;
}
