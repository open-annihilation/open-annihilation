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
    using editor::words::cancel;
    using editor::words::layout;
    using editor::words::restore;
    using editor::words::save;
    using editor::words::search_actions;
    using editor::words::search_short;
    if (english == layout)
        return "DISPOSITION DES TOUCHES";
    if (english == save)
        return "ENREGISTRER";
    if (english == cancel)
        return "ANNULER LA SAISIE";
    if (english == restore)
        return "RETABLIR LES VALEURS PAR DEFAUT";
    if (english == search_actions)
        return "Rechercher des actions, ou appuyer sur une touche";
    if (english == search_short)
        return "Rechercher, ou appuyer sur une touche";
    return std::string(english);
}

/// German long enough to grow Save and Restore. Anything else stays English.
///
/// @param context unused
/// @param english the English word
/// @return the text to show
std::string german_shown(void* /*context*/, std::string_view english) {
    if (english == editor::words::save)
        return "SPEICHERN SIE BITTE DIESES LAYOUT";
    if (english == editor::words::restore)
        return "STANDARDWERTE WIEDERHERSTELLEN";
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
    if (save != "ENREGISTRER")
        std::fprintf(stderr, "%s: save shows \"%s\"\n", where.c_str(), save.c_str());
    OA_CHECK(save == "ENREGISTRER");
    const bool large = frame.size_class == kit::SizeClass::large;
    const std::string want = large
                                 ? std::string(french_shown(nullptr, editor::words::search_actions))
                                 : std::string(french_shown(nullptr, editor::words::search_short));
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
    OA_CHECK(save == "SPEICHERN SIE BITTE DIESES LAYOUT");

    const int32_t restore_before = control_width(before, editor::name_restore);
    const int32_t restore_after = control_width(after, editor::name_restore);
    const bool footer = frame.size_class != kit::SizeClass::compact;
    if (footer && (restore_before < 0 || restore_after < 0))
        std::fprintf(stderr, "%s: restore is missing\n", where.c_str());
    if (footer)
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
        OA_CHECK(caption_of(after, editor::name_restore) == "STANDARDWERTE WIEDERHERSTELLEN");
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
        require_named(list, editor::name_capture_letter, where + " waiting");
        require_named(list, editor::name_capture_place, where + " waiting");
        require_named(list, editor::name_capture_press, where + " waiting");
        require_named(list, editor::name_capture_double, where + " waiting");
        require_named(list, editor::name_capture_hold, where + " waiting");
        require_named(list, editor::name_capture_cancel, where + " waiting");
        require_named(list, editor::name_capture_ok, where + " waiting");
        OA_CHECK(!control_enabled(list, editor::name_capture_ok));
        if (!shows(list, "Press a key"))
            std::fprintf(stderr, "%s waiting: the well has no prompt\n", where.c_str());
        OA_CHECK(shows(list, "Press a key"));
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
        require_named(list, editor::name_card_here, where + " card");
        require_named(list, editor::name_card_there, where + " card");
        require_named(list, editor::name_card_choose, where + " card");
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
        const kit::Control* card = find_named(list, editor::name_key_card);
        if (card == nullptr || card->rect.y < lowest)
            std::fprintf(stderr, "%s: the card is not under the keyboard\n", where.c_str());
        OA_CHECK(card != nullptr && card->rect.y >= lowest);
        if (window.x >= 720 && (card == nullptr || card->rect.x < window.x / 3))
            std::fprintf(stderr, "%s: the card is not beside the legend\n", where.c_str());
        if (window.x >= 720)
            OA_CHECK(card != nullptr && card->rect.x >= window.x / 3);
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
