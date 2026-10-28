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
using key_editor_fixture::prompt_problems;
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
    {
        // The close question: Kit 2's buttons, each as wide as its words,
        // the box no wider than the widest prompt and narrower than the window.
        editor::Editor edited(editor_fixture());
        OA_CHECK(edited.remove_chord("order.attack", 0));
        editor::ScreenState state;
        state.prompt = editor::Prompt::leave;
        const kit::DisplayList list = laid_out(edited, state, frame);
        check_list(list, where + " leave");
        require_named(list, editor::name_leave, where + " leave");
        const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
        for (const std::string_view name :
             {editor::name_leave_save, editor::name_leave_discard, editor::name_leave_keep}) {
            require_named(list, name, where + " leave");
            const kit::Control* button = find_named(list, name);
            if (button != nullptr && button->rect.height != metrics.control_height)
                std::fprintf(
                    stderr,
                    "%s leave: %s is %d high, not %d\n",
                    where.c_str(),
                    std::string(name).c_str(),
                    button->rect.height,
                    metrics.control_height
                );
            OA_CHECK(button != nullptr && button->rect.height == metrics.control_height);
        }
        const kit::Control* save = find_named(list, editor::name_leave_save);
        const kit::Control* question = find_named(list, editor::name_leave);
        if (save != nullptr && question != nullptr)
            OA_CHECK(question->rect.width < window.x);
        // Tab: Discard, Keep editing, Save.
        const std::vector<kit::ControlId> expected{
            kit::control_named(list, editor::name_leave_discard),
            kit::control_named(list, editor::name_leave_keep),
            kit::control_named(list, editor::name_leave_save),
        };
        OA_CHECK(list.tab_order == expected);
    }
    {
        // Clone's question: the name and description fields, the line under
        // them, Cancel and Clone at control height, Tab through them in that
        // order; a taken name says so and keeps Clone off.
        editor::ScreenState state;
        state.prompt = editor::Prompt::clone;
        state.layout_name_field.text = model.clone_name();
        state.layout_description_field.text = "Based on Ridge grid";
        const kit::DisplayList list = laid_out(model, state, frame);
        check_list(list, where + " clone");
        const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
        for (const std::string_view name :
             {editor::name_layout_name,
              editor::name_layout_description,
              editor::name_layout_status,
              editor::name_layout_ok,
              editor::name_layout_cancel})
            require_named(list, name, where + " clone");
        for (const std::string_view name : {editor::name_layout_ok, editor::name_layout_cancel}) {
            const kit::Control* button = find_named(list, name);
            OA_CHECK(button != nullptr && button->rect.height == metrics.control_height);
        }
        OA_CHECK(control_enabled(list, editor::name_layout_ok));
        const std::vector<kit::ControlId> expected{
            kit::control_named(list, editor::name_layout_name),
            kit::control_named(list, editor::name_layout_description),
            kit::control_named(list, editor::name_layout_cancel),
            kit::control_named(list, editor::name_layout_ok),
        };
        OA_CHECK(list.tab_order == expected);
        const kit::Control* status = find_named(list, editor::name_layout_status);
        OA_CHECK(status != nullptr && status->text.find("Clone Ridge grid") == 0);

        state.layout_name_field.text = "ridge GRID";
        const kit::DisplayList taken = laid_out(model, state, frame);
        check_list(taken, where + " clone taken");
        OA_CHECK(!control_enabled(taken, editor::name_layout_ok));
        const kit::Control* said = find_named(taken, editor::name_layout_status);
        OA_CHECK(
            said != nullptr &&
            said->text.find("Another key layout is called ridge GRID.") != std::string::npos
        );

        state.prompt = editor::Prompt::describe;
        state.layout_name_field.text = "Ridge grid";
        state.layout_description_field.text = {};
        const kit::DisplayList same = laid_out(model, state, frame);
        check_list(same, where + " describe");
        OA_CHECK(!control_enabled(same, editor::name_layout_ok));
        state.layout_description_field.text = "Mine";
        OA_CHECK(control_enabled(laid_out(model, state, frame), editor::name_layout_ok));
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
        // The callouts beside the picture show whole roles, and count the
        // rest, rather than cutting one off.
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::text || item.text.find("...") == std::string::npos)
                continue;
            std::fprintf(stderr, "%s: \"%s\" is cut\n", where.c_str(), item.text.c_str());
            OA_CHECK(false);
        }
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
/// a count of one uses the singular word, paste's rows name their actions,
/// keys and chips, and a share text longer than the preview shows its first
/// lines and "...".
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
    if (!shows(shared, "From Ridge grid · 2 keys and 0 helpers changed"))
        std::fprintf(stderr, "%s: the share count is not plural\n", where.c_str());
    OA_CHECK(shows(shared, "From Ridge grid · 2 keys and 0 helpers changed"));

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
    if (!shows(one_laid, "1 key and 1 helper changed"))
        std::fprintf(stderr, "%s: one key and one helper are not singular\n", where.c_str());
    OA_CHECK(shows(one_laid, "1 key and 1 helper changed"));

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
    // Rows name their actions as the list does; no id reaches the player
    // but an action's this version does not know.
    const std::string move_name = model.details("order.move").name;
    const std::string all_name = model.details("select.all").name;
    OA_CHECK(!move_name.empty() && !all_name.empty());
    if (!line_holds(paste_list, move_name, all_name))
        std::fprintf(stderr, "%s: Move shares a line with Select all\n", where.c_str());
    OA_CHECK(line_holds(paste_list, move_name, all_name));
    OA_CHECK(rows != nullptr && rows->text.find("order.move") == std::string::npos);
    OA_CHECK(rows != nullptr && rows->text.find("select.all") == std::string::npos);
    // A key by its place, a clash with the line under it, and an action this
    // version does not know with the line under it.
    const std::string listed = rows != nullptr ? rows->text : std::string{};
    const bool rows_read =
        listed.find(move_name + " · Q · By place") != std::string::npos &&
        listed.find(all_name + " · Ctrl+B · Clash\nCtrl+B already belongs to ") !=
            std::string::npos &&
        listed.find(". Apply keeps yours and lists the clash.") != std::string::npos &&
        listed.find(
            "camera.orbit-left · Q · Not in this version\nKept in your layout for a version "
            "that has it."
        ) != std::string::npos;
    if (!rows_read)
        std::fprintf(stderr, "%s: the paste rows are:\n%s\n", where.c_str(), listed.c_str());
    OA_CHECK(rows_read);
    OA_CHECK(shows(paste_list, "Nothing changes until you apply it."));
    const std::string apply = caption_of(paste_list, editor::name_paste_apply);
    if (apply.find("Apply 3 Changes") == std::string::npos)
        std::fprintf(stderr, "%s: APPLY is \"%s\"\n", where.c_str(), apply.c_str());
    OA_CHECK(apply.find("Apply 3 Changes") != std::string::npos);

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
    if (one_apply != "Apply 1 Change")
        std::fprintf(stderr, "%s: APPLY one is \"%s\"\n", where.c_str(), one_apply.c_str());
    OA_CHECK(one_apply == "Apply 1 Change");

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
    first.source_line = "MegamapKey=77";
    editor::ImportRow second;
    second.action = "whiteboard.draw";
    second.source_line = "WhiteboardKey=114";
    importing.importing.rows.push_back(std::move(first));
    importing.importing.rows.push_back(std::move(second));
    const kit::DisplayList imported = laid_out(model, importing, frame);
    check_list(imported, where + " import lines");
    const auto registry_name = [&](std::string_view id) {
        const oa::data::input_actions::Action* action = oa::data::input_actions::find(id);
        return action == nullptr ? std::string(id) : std::string(action->words.name);
    };
    const std::string megamap = registry_name("megamap.toggle");
    const std::string whiteboard = registry_name("whiteboard.draw");
    if (!line_holds(imported, megamap, whiteboard)) {
        std::fprintf(
            stderr,
            "%s: %s shares a line with %s\n",
            where.c_str(),
            megamap.c_str(),
            whiteboard.c_str()
        );
    }
    OA_CHECK(line_holds(imported, megamap, whiteboard));
    // Names, not ids or the file's raw lines.
    const kit::Control* import_rows = find_named(imported, editor::name_import_list);
    OA_CHECK(
        import_rows != nullptr && import_rows->text.find("whiteboard.draw") == std::string::npos
    );
    OA_CHECK(
        import_rows != nullptr && import_rows->text.find("WhiteboardKey") == std::string::npos
    );
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

    // A share text longer than the preview shows its first lines and "...".
    editor::EditorInputs many = editor_fixture();
    for (int32_t index = 0; index < 40; ++index) {
        many.own.bindings.push_back(
            key_editor_fixture::binding_of("shared.line." + std::to_string(index), "Q")
        );
    }
    const editor::Editor tall(std::move(many));
    const kit::DisplayList tall_list = laid_out(tall, share, frame);
    check_list(tall_list, where + " share preview");
    const kit::Control* block = find_named(tall_list, editor::name_share_text);
    int32_t preview_lines = 0;
    bool more_said = false;
    for (std::size_t index = key_editor_fixture::prompt_first(tall_list);
         index < tall_list.items.size();
         ++index) {
        const kit::Item& item = tall_list.items[index];
        if (block == nullptr || item.role != kit::Role::text ||
            !kit::wholly_in(item.rect, block->rect))
            continue;
        ++preview_lines;
        more_said = more_said || item.text == "...";
    }
    if (block == nullptr || preview_lines < 2 || preview_lines > 6 || !more_said)
        std::fprintf(
            stderr,
            "%s: the preview shows %d lines, more %d\n",
            where.c_str(),
            static_cast<int>(preview_lines),
            more_said ? 1 : 0
        );
    OA_CHECK(block != nullptr && preview_lines >= 2 && preview_lines <= 6 && more_said);
    OA_CHECK(find_named(tall_list, editor::name_exchange_bar) == nullptr);
}

/// Checks that a control is a height, and says which when it is not.
///
/// @param list the display list
/// @param name the control's name
/// @param height the height it must be, in points
/// @param where the case
void expect_height(
    const kit::DisplayList& list, std::string_view name, int32_t height, const std::string& where
) {
    const kit::Control* control = find_named(list, name);
    if (control == nullptr || control->rect.height != height)
        std::fprintf(
            stderr,
            "%s: %s is %d high, not %d\n",
            where.c_str(),
            std::string(name).c_str(),
            control == nullptr ? -1 : control->rect.height,
            height
        );
    OA_CHECK(control != nullptr && control->rect.height == height);
}

/// Checks that the window's header and toolbar are Settings' and the
/// Library's: Kit 2's window header at Regular and Large and the kit's band
/// at Compact, with the close mark in its middle; the tab strip, the text
/// fields and drop-downs, the filter chips and the buttons at Kit 2's sizes,
/// in the list, the keyboard and the mouse.
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
    expect_height(list, editor::name_view_list, metrics.tab_strip_height, where);
    expect_height(list, editor::name_search, metrics.text_field_height, where);
    expect_height(list, editor::name_find, metrics.control_height, where);
    if (frame.size_class != kit::SizeClass::large)
        expect_height(list, editor::name_category, metrics.text_field_height, where);
    if (!compact) {
        expect_height(list, editor::name_layout, metrics.text_field_height, where);
        expect_height(list, editor::name_filter_all, metrics.filter_chip_height, where);
    }

    editor::ScreenState keyboard;
    keyboard.view = editor::View::keyboard;
    const kit::DisplayList keys_list = laid_out(model, keyboard, frame);
    expect_height(keys_list, editor::name_view_keyboard, metrics.tab_strip_height, where);
    expect_height(keys_list, editor::name_layer_none, metrics.filter_chip_height, where);
    expect_height(keys_list, editor::name_keyboard_shape, metrics.text_field_height, where);
    expect_height(keys_list, editor::name_show_as_list, metrics.control_height, where);

    editor::ScreenState mouse;
    mouse.view = editor::View::mouse;
    const kit::DisplayList mouse_list = laid_out(model, mouse, frame);
    expect_height(mouse_list, editor::name_view_mouse, metrics.tab_strip_height, where);
    expect_height(mouse_list, editor::name_scheme_right, metrics.filter_chip_height, where);
}

/// Menu buttons are grouped under the screen's title, and the row says the
/// button's text.
void menu_screen_headings() {
    editor::EditorInputs inputs = editor_fixture();
    auto add = [&](const char* action, const char* caption, const char* menu, char32_t letter) {
        keys::GuiKey key;
        key.action = action;
        key.caption = caption;
        key.menu = menu;
        key.letter = letter;
        inputs.mod.keys.push_back(std::move(key));
    };
    add("gui.mainmenu.single", "SINGLE", "Mainmenu", U'S');
    add("gui.mainmenu.multi", "MULTI", "Mainmenu", U'M');
    add("gui.armopt.load", "LoadList", "GAME OPTIONS", U'L');

    const editor::Editor model(std::move(inputs));
    const kit::Frame frame = frame_for(1280, 720, 100);
    editor::ScreenState menus;
    menus.category = "mod.menus";
    const kit::DisplayList list = laid_out(model, menus, frame);

    std::vector<std::string> headings;
    bool saw_single = false;
    for (const kit::Item& item : list.items) {
        if (const auto* heading = std::get_if<kit::HeadingLook>(&item.look))
            headings.push_back(heading->text);
        if (const auto* row = std::get_if<kit::ListRowLook>(&item.look)) {
            if (row->title == "Single")
                saw_single = true;
        }
    }
    for (const kit::Control& control : list.controls) {
        if (starts(control.name, editor::name_row_prefix) &&
            control.text.find("Single") != std::string::npos)
            saw_single = true;
    }
    const std::vector<std::string> expected = {"Menu buttons", "Mainmenu", "Game Options"};
    if (headings != expected) {
        std::string got;
        for (const std::string& heading : headings) {
            if (!got.empty())
                got += ", ";
            got += heading;
        }
        std::fprintf(stderr, "menu headings: %s\n", got.c_str());
    }
    OA_CHECK(headings == expected);
    if (!saw_single)
        std::fprintf(stderr, "the Menus list does not show Single\n");
    OA_CHECK(saw_single);

    editor::ScreenState all;
    all.category = "all";
    all.search.text = "loadlist";
    const kit::DisplayList searched = laid_out(model, all, frame);
    bool game_options = false;
    bool joined_place = false;
    for (const kit::Item& item : searched.items) {
        if (const auto* heading = std::get_if<kit::HeadingLook>(&item.look)) {
            if (heading->text == "Game Options")
                game_options = true;
            if (heading->text.find("·") != std::string::npos)
                joined_place = true;
        }
    }
    if (!game_options || joined_place)
        std::fprintf(stderr, "All actions did not head LoadList with Game Options\n");
    OA_CHECK(game_options && !joined_place);
}

/// One row of an open menu: a heading's words, or an item's name and words.
struct MenuRow {
    bool heading{false};   ///< a heading, not a choice
    std::string name{};    ///< the item's control; empty for a heading
    std::string caption{}; ///< the words shown
};

/// Every row of the open menu, top to bottom, each scrolled into view.
///
/// @param model the editor
/// @param state the open menu
/// @param frame the frame
/// @return the rows
std::vector<MenuRow>
menu_rows(const editor::Editor& model, editor::ScreenState state, const kit::Frame& frame) {
    std::vector<MenuRow> rows;
    for (int32_t index = 0; index < 64; ++index) {
        state.menu_first = index;
        const kit::DisplayList list = laid_out(model, state, frame);
        const kit::ChoiceMenuLook* look = nullptr;
        for (const kit::Item& item : list.items) {
            if (const auto* menu = std::get_if<kit::ChoiceMenuLook>(&item.look))
                look = menu;
        }
        if (look == nullptr || index >= look->total ||
            static_cast<std::size_t>(index) >= look->items.size())
            break;
        const int32_t place = index - look->first;
        if (place < 0 || place >= static_cast<int32_t>(look->shown.size()))
            break;
        MenuRow row;
        row.heading = look->items[static_cast<std::size_t>(index)].divider;
        row.name = row.heading ? std::string{} : look->items[static_cast<std::size_t>(index)].id;
        row.caption = look->shown[static_cast<std::size_t>(place)];
        rows.push_back(std::move(row));
    }
    return rows;
}

/// More holds two groups under their headings, Layouts and Sharing, in
/// order, with Rename and Describe… only on a layout other than OA default.
/// Compact's More leads with the layouts, and keeps the layout menu's names
/// for Clone and Rename and Describe. No item appears twice.
///
/// @param frame the frame
/// @param where the case
void check_more_groups(const kit::Frame& frame, const std::string& where) {
    const bool compact = frame.size_class == kit::SizeClass::compact;
    for (const bool on_default : {false, true}) {
        editor::EditorInputs inputs = editor_fixture();
        if (on_default) {
            inputs.has_chosen = false;
            inputs.chosen = {};
        }
        const editor::Editor model(std::move(inputs));
        editor::ScreenState state;
        state.menu = editor::Menu::more;
        const std::vector<MenuRow> rows = menu_rows(model, state, frame);
        std::vector<MenuRow> expected;
        const auto item = [&](std::string_view name, std::string_view caption) {
            expected.push_back({false, std::string(name), std::string(caption)});
        };
        const auto heading = [&](std::string_view caption) {
            expected.push_back({true, {}, std::string(caption)});
        };
        if (compact) {
            item("key-editor.layout.oa-default", "OA default");
            item("key-editor.layout.ridge-grid", "Ridge grid");
        }
        heading("Layouts");
        item(editor::name_import, "Import New Layout…");
        item(compact ? "key-editor.layout.clone" : editor::name_clone, "Clone as New Layout…");
        if (!on_default)
            item(
                compact ? "key-editor.layout.describe" : editor::name_describe,
                "Rename and Describe…"
            );
        item(editor::name_restore, "Restore Defaults");
        heading("Sharing");
        item(editor::name_share, "Share My Custom Keys…");
        item(editor::name_paste, "Paste Someone's Layout…");
        bool same = rows.size() == expected.size();
        for (std::size_t index = 0; same && index < rows.size(); ++index)
            same = rows[index].heading == expected[index].heading &&
                   rows[index].name == expected[index].name &&
                   rows[index].caption == expected[index].caption;
        if (!same) {
            std::fprintf(
                stderr, "%s%s: More is\n", where.c_str(), on_default ? " on OA default" : ""
            );
            for (const MenuRow& row : rows)
                std::fprintf(
                    stderr,
                    "  %s %s \"%s\"\n",
                    row.heading ? "heading" : "item",
                    row.name.c_str(),
                    row.caption.c_str()
                );
        }
        OA_CHECK(same);
        for (std::size_t first = 0; first < rows.size(); ++first) {
            for (std::size_t second = first + 1; second < rows.size(); ++second) {
                const bool twice = rows[first].caption == rows[second].caption ||
                                   (!rows[first].heading && rows[first].name == rows[second].name);
                if (twice)
                    std::fprintf(
                        stderr,
                        "%s: More shows %s twice\n",
                        where.c_str(),
                        rows[first].caption.c_str()
                    );
                OA_CHECK(!twice);
            }
        }
        // The layout drop-down's Clone… takes the same words.
        if (!compact) {
            editor::ScreenState chooser;
            chooser.menu = editor::Menu::layout;
            bool clone_named = false;
            for (const MenuRow& row : menu_rows(model, chooser, frame))
                clone_named = clone_named || (row.name == "key-editor.layout.clone" &&
                                              row.caption == "Clone as New Layout…");
            if (!clone_named)
                std::fprintf(stderr, "%s: the layout menu's Clone is not renamed\n", where.c_str());
            OA_CHECK(clone_named);
        }
    }
}

/// Words longer than the English, as German and French run: each word with
/// half its length again in short words after it, its places kept.
///
/// @param context unused
/// @param english the English word
/// @return the longer words
std::string longer_shown(void* /*context*/, std::string_view english) {
    std::string longer(english);
    for (std::size_t added = 0; added < english.size() / 2; added += 5)
        longer += " lang";
    return longer;
}

/// Checks a Share or Paste prompt: inside the window, its controls apart,
/// its texts fitting, and nothing it draws meeting anything else, nor any
/// word cut short but the preview's "..." and a saved path cut in its middle.
///
/// @param list the display list
/// @param window the window's size
/// @param where the case
void check_prompt_fit(const kit::DisplayList& list, kit::Point window, const std::string& where) {
    expect_none(outside_window(list, window), where + " drawn");
    expect_none(rects_outside(list, window), where + " control");
    expect_none(meeting_controls(list), where + " meets");
    expect_none(texts_not_fitting(list, fonts()), where + " text");
    std::vector<kit::Rect> exempt;
    if (const kit::Control* preview = find_named(list, editor::name_share_text))
        exempt.push_back(preview->rect);
    if (const kit::Control* folder = find_named(list, editor::name_share_open))
        exempt.push_back({0, folder->rect.y, folder->rect.x, folder->rect.height});
    expect_none(prompt_problems(list, exempt), where + " part");
}

/// Share with changes and with none, before and after Save File… and Copy as
/// Text, and Paste with a key by its place, a clash and an unknown action,
/// and with a text that is no key layout, in English and in longer words:
/// nothing meets and no word is cut. With nothing changed, Share's two
/// buttons are off; Open Folder shows once a file is saved; Copied shows
/// after a copy; Paste's refusal says what to do and offers Close.
///
/// @param frame the frame
/// @param where the case
void check_dialog_fit(const kit::Frame& frame, const std::string& where) {
    const kit::Point window = editor::window_size(frame);
    for (const bool longer : {false, true}) {
        editor::EditorInputs inputs = editor_fixture();
        if (longer)
            inputs.text.shown = longer_shown;
        editor::Editor model(std::move(inputs));
        const std::string in = where + (longer ? " longer" : "");
        model.set_share_name("Grid, left hand");
        editor::ScreenState share;
        share.prompt = editor::Prompt::share;
        share.share_field.text = "Grid, left hand";
        share.share_field.caret = static_cast<int32_t>(share.share_field.text.size());
        check_prompt_fit(laid_out(model, share, frame), window, in + " share");
        OA_CHECK(find_named(laid_out(model, share, frame), editor::name_share_open) == nullptr);
        editor::ScreenState done = share;
        done.share_copied = true;
        done.share_place = "/Users/ridge/Library/Application Support/Open Annihilation/Keys/Shared/"
                           "grid-left-hand.oakeys";
        const kit::DisplayList done_list = laid_out(model, done, frame);
        check_prompt_fit(done_list, window, in + " share saved and copied");
        OA_CHECK(find_named(done_list, editor::name_share_open) != nullptr);
        OA_CHECK(shows(done_list, longer ? longer_shown(nullptr, "Copied") : "Copied"));
        OA_CHECK(shows(done_list, "grid-left-hand.oakeys"));

        editor::Editor bare(std::move(model));
        static_cast<void>(bare.choose_layout("oa-default", editor::KeepChanges::aside));
        bare.set_share_name("Grid, left hand");
        const kit::DisplayList none_list = laid_out(bare, share, frame);
        check_prompt_fit(none_list, window, in + " share with nothing changed");
        const bool off = !control_enabled(none_list, editor::name_share_save) &&
                         !control_enabled(none_list, editor::name_share_copy);
        if (!off)
            std::fprintf(stderr, "%s: Share's buttons are on with nothing changed\n", in.c_str());
        OA_CHECK(off);
        if (!longer)
            OA_CHECK(shows(none_list, "No changes from OA default yet: there is nothing to share"));

        editor::EditorInputs paste_inputs = editor_fixture();
        if (longer)
            paste_inputs.text.shown = longer_shown;
        const editor::Editor paste_model(std::move(paste_inputs));
        constexpr std::string_view sample = "# oakeys bindings, base oa-default\n"
                                            "order.move: [\"@KeyQ\"]\n"
                                            "select.all: [Ctrl+B]\n"
                                            "camera.orbit-left: [Q]\n";
        editor::ScreenState pasted;
        pasted.prompt = editor::Prompt::paste;
        pasted.paste = editor::paste_preview(
            paste_model, {reinterpret_cast<const uint8_t*>(sample.data()), sample.size()}
        );
        check_prompt_fit(laid_out(paste_model, pasted, frame), window, in + " paste");
        constexpr std::string_view hello = "Hello, these are my keys\n";
        editor::ScreenState refused;
        refused.prompt = editor::Prompt::paste;
        refused.paste = editor::paste_preview(
            paste_model, {reinterpret_cast<const uint8_t*>(hello.data()), hello.size()}
        );
        const kit::DisplayList refused_list = laid_out(paste_model, refused, frame);
        check_prompt_fit(refused_list, window, in + " paste refused");
        OA_CHECK(refused.paste.not_layout);
        OA_CHECK(find_named(refused_list, editor::name_paste_apply) == nullptr);
        if (!longer) {
            OA_CHECK(caption_of(refused_list, editor::name_paste_cancel) == "Close");
            OA_CHECK(shows(refused_list, "it starts with # oakeys"));
        }
    }
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
    check_more_groups(compact, "compact");
    check_more_groups(large, "large");
    check_more_groups(regular, "regular");
    check_dialog_fit(compact, "compact");
    check_dialog_fit(large, "large");
    check_dialog_fit(regular, "regular");

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
    menu_screen_headings();
    return oa::test::failed_checks() == 0 ? 0 : 1;
}
