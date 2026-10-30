// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A manifest becomes a layout: a bad chord is an error at the chord, and an
// unknown action, Escape, a repeated chord and a missing character are warnings.

#include "oa/ui/key_bindings/layout_file.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/legacy.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace actions = oa::data::input_actions;
namespace files = oa::formats::oakeys;
namespace keys = oa::ui::key_bindings;

using keys::Layout;
using keys::LayoutProblem;

/// Views a text's bytes.
///
/// @param text the text
/// @return its bytes
std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// Where a marker sits in a text.
struct Place {
    uint32_t line{0};   ///< the 1-based line
    uint32_t column{0}; ///< the 1-based byte column
};

/// The line and column of one occurrence of a marker.
///
/// @param text the text
/// @param marker the marker
/// @param offset bytes after the marker's first byte
/// @return the place, or zeros when the marker is absent
Place place_of(std::string_view text, std::string_view marker, std::size_t offset) {
    const std::size_t at = text.find(marker);
    if (at == std::string_view::npos || at + offset >= text.size()) {
        std::fprintf(
            stderr, "marker missing: %.*s\n", static_cast<int>(marker.size()), marker.data()
        );
        OA_CHECK(false);
        return {};
    }
    Place place{1, 1};
    for (std::size_t index = 0; index < at + offset; ++index) {
        if (text[index] == '\n') {
            ++place.line;
            place.column = 1;
        } else {
            ++place.column;
        }
    }
    return place;
}

/// Prints every problem.
///
/// @param problems the problems
void show_problems(const std::vector<LayoutProblem>& problems) {
    for (const LayoutProblem& problem : problems) {
        std::fprintf(
            stderr,
            "  %u:%u: %s: %s%s\n",
            problem.line,
            problem.column,
            problem.key.c_str(),
            problem.message.c_str(),
            problem.warning ? " (warning)" : ""
        );
    }
}

/// Reads a paste that must pass.
///
/// @param text the paste
/// @param[out] manifest the manifest
/// @return true when it passed
bool read_ok(std::string_view text, files::Manifest& manifest) {
    std::vector<files::Problem> problems;
    const bool ok = files::read_bindings_text(as_bytes(text), manifest, problems);
    if (!ok || !problems.empty()) {
        std::fprintf(stderr, "expected a paste, got %zu problems\n", problems.size());
        for (const files::Problem& problem : problems)
            std::fprintf(stderr, "  %s\n", files::describe(problem).c_str());
        OA_CHECK(false);
        return false;
    }
    return true;
}

/// Tells whether two layouts agree, including each chord and its place.
///
/// @param first the first layout
/// @param second the second layout
/// @return true when they agree
bool same_layout(const Layout& first, const Layout& second) {
    if (first.id != second.id || first.base != second.base ||
        first.bindings.size() != second.bindings.size())
        return false;
    for (std::size_t index = 0; index < first.bindings.size(); ++index) {
        const keys::LayoutBinding& left = first.bindings[index];
        const keys::LayoutBinding& right = second.bindings[index];
        if (left.action != right.action || left.line != right.line || left.column != right.column ||
            left.chords != right.chords)
            return false;
    }
    return true;
}

/// Counts problems that are errors.
///
/// @param problems the problems
/// @return how many are not warnings
std::size_t error_count(const std::vector<LayoutProblem>& problems) {
    std::size_t count = 0;
    for (const LayoutProblem& problem : problems) {
        if (!problem.warning)
            ++count;
    }
    return count;
}

/// A chord that does not parse is an error at the chord, not at the list.
void test_bad_chord() {
    const std::string text = "order.attack: [\"Ctrl+Q:sometimes\"]\n";
    files::Manifest manifest;
    OA_CHECK(read_ok(text, manifest));
    const Place chord = place_of(text, "\"Ctrl+Q:sometimes\"", 0);
    const Place list = place_of(text, "[", 0);
    OA_CHECK(chord.column > list.column);

    Layout layout;
    layout.id = "sentinel";
    std::vector<LayoutProblem> problems;
    const bool ok = keys::layout_from_manifest(manifest, nullptr, layout, problems);
    if (ok || problems.size() != 1)
        show_problems(problems);
    OA_CHECK(!ok);
    OA_CHECK(layout.id == "sentinel");
    OA_CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        OA_CHECK(!problems[0].warning);
        OA_CHECK(problems[0].key == "bindings.order.attack[0]");
        OA_CHECK(problems[0].line == chord.line && problems[0].column == chord.column);
        OA_CHECK(problems[0].column != list.column);
        OA_CHECK(
            problems[0].message == keys::chord_error_text(keys::ChordErrorReason::unknown_gesture)
        );
    }
}

/// An unknown action is a warning, kept, and written back. A family id is not unknown.
void test_unknown_action() {
    files::Manifest manifest;
    OA_CHECK(read_ok("ridge.extra: [Q]\n", manifest));
    Layout layout;
    std::vector<LayoutProblem> problems;
    const bool ok = keys::layout_from_manifest(manifest, nullptr, layout, problems);
    if (!ok)
        show_problems(problems);
    OA_CHECK(ok);
    OA_CHECK(error_count(problems) == 0);
    OA_CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        OA_CHECK(problems[0].warning);
        OA_CHECK(problems[0].key == "bindings.ridge.extra");
        OA_CHECK(problems[0].message == "Not in this version of Open Annihilation");
    }
    OA_CHECK(layout.bindings.size() == 1);
    if (layout.bindings.size() == 1) {
        OA_CHECK(layout.bindings[0].action == "ridge.extra");
        OA_CHECK(layout.bindings[0].chords.size() == 1);
    }

    files::Manifest rest;
    rest.name = "Kept";
    rest.description = "Kept description";
    const files::Manifest written = keys::manifest_from_layout(layout, rest);
    OA_CHECK(written.name == "Kept");
    OA_CHECK(written.description == "Kept description");
    OA_CHECK(written.bindings.size() == 1);
    if (written.bindings.size() == 1 && !written.bindings[0].chords.empty()) {
        OA_CHECK(written.bindings[0].action == "ridge.extra");
        OA_CHECK(written.bindings[0].chords[0].text == "Q");
    }
    const std::string text = files::write_bindings_text(written);
    OA_CHECK(text.find("ridge.extra: [Q]\n") != std::string::npos);

    files::Manifest family;
    OA_CHECK(read_ok("gui.attack: [Q]\n", family));
    Layout family_layout;
    std::vector<LayoutProblem> family_problems;
    OA_CHECK(keys::layout_from_manifest(family, nullptr, family_layout, family_problems));
    for (const LayoutProblem& problem : family_problems)
        OA_CHECK(problem.message != "Not in this version of Open Annihilation");
    OA_CHECK(family_layout.bindings.size() == 1);
    if (family_layout.bindings.size() == 1)
        OA_CHECK(family_layout.bindings[0].action == "gui.attack");

    files::Manifest tag;
    OA_CHECK(read_ok("select.tag-ridge-scout: [Y]\n", tag));
    Layout tag_layout;
    std::vector<LayoutProblem> tag_problems;
    OA_CHECK(keys::layout_from_manifest(tag, nullptr, tag_layout, tag_problems));
    for (const LayoutProblem& problem : tag_problems)
        OA_CHECK(problem.message != "Not in this version of Open Annihilation");
    OA_CHECK(tag_layout.bindings.size() == 1);
    if (tag_layout.bindings.size() == 1)
        OA_CHECK(tag_layout.bindings[0].action == "select.tag-ridge-scout");
}

/// Escape missing from the fixed action, and Escape given to chat, are warnings.
/// F9 on the settings screen is not.
void test_escape() {
    const std::string text = "ui.settings: [F9]\nchat.cancel: [F9]\nchat.send: [Escape]\n";
    files::Manifest manifest;
    OA_CHECK(read_ok(text, manifest));
    Layout layout;
    std::vector<LayoutProblem> problems;
    const bool ok = keys::layout_from_manifest(manifest, nullptr, layout, problems);
    if (!ok)
        show_problems(problems);
    OA_CHECK(ok);
    OA_CHECK(error_count(problems) == 0);
    OA_CHECK(layout.bindings.size() == 3);

    bool cancel_missing = false;
    bool send_escape = false;
    bool repeated = false;
    for (const LayoutProblem& problem : problems) {
        OA_CHECK(problem.warning);
        OA_CHECK(problem.key.find("ui.settings") == std::string::npos);
        if (problem.key == "bindings.chat.cancel" &&
            problem.message == "Escape always closes the chat line and Open Annihilation's screens")
            cancel_missing = true;
        if (problem.key == "bindings.chat.send[0]" &&
            problem.message == "Escape always closes the chat line and Open Annihilation's screens")
            send_escape = true;
        if (problem.message.find("ui.settings") != std::string::npos &&
            problem.message.find("chat.cancel") != std::string::npos)
            repeated = true;
    }
    if (!cancel_missing || !send_escape || !repeated)
        show_problems(problems);
    OA_CHECK(cancel_missing);
    OA_CHECK(send_escape);
    OA_CHECK(repeated);
}

/// A chord bound twice names both actions.
void test_repeated_chord() {
    files::Manifest manifest;
    OA_CHECK(read_ok("order.attack: [A]\norder.move: [A]\n", manifest));
    Layout layout;
    std::vector<LayoutProblem> problems;
    OA_CHECK(keys::layout_from_manifest(manifest, nullptr, layout, problems));
    OA_CHECK(error_count(problems) == 0);
    OA_CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        OA_CHECK(problems[0].warning);
        OA_CHECK(problems[0].key == "bindings.order.move[0]");
        OA_CHECK(problems[0].message.find("order.move") != std::string::npos);
        OA_CHECK(problems[0].message.find("order.attack") != std::string::npos);
    }
    OA_CHECK(layout.bindings.size() == 2);
}

/// A backtick the keyboard does not type is a warning. The US map does type it.
void test_not_on_keyboard() {
    const std::string text = "order.attack: [\"`\"]\n";
    files::Manifest manifest;
    OA_CHECK(read_ok(text, manifest));
    const Place chord = place_of(text, "\"`\"", 0);
    keys::KeyboardMap without_grave = keys::KeyboardMap::us();
    without_grave.set(53, 0, 0);

    Layout layout;
    std::vector<LayoutProblem> problems;
    const bool ok = keys::layout_from_manifest(manifest, &without_grave, layout, problems);
    if (!ok)
        show_problems(problems);
    OA_CHECK(ok);
    OA_CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        OA_CHECK(problems[0].warning);
        OA_CHECK(problems[0].message == "Not on this keyboard");
        OA_CHECK(problems[0].key == "bindings.order.attack[0]");
        OA_CHECK(problems[0].line == chord.line && problems[0].column == chord.column);
    }
    OA_CHECK(layout.bindings.size() == 1);

    const keys::KeyboardMap us = keys::KeyboardMap::us();
    Layout on_us;
    std::vector<LayoutProblem> us_problems;
    OA_CHECK(keys::layout_from_manifest(manifest, &us, on_us, us_problems));
    OA_CHECK(us_problems.empty());
}

/// A modifier on its own is an error on a press, and a chord on a modifier action.
void test_lone_modifier() {
    files::Manifest manifest;
    OA_CHECK(read_ok("order.attack: [Shift]\n", manifest));
    Layout layout;
    layout.id = "sentinel";
    std::vector<LayoutProblem> problems;
    const bool ok = keys::layout_from_manifest(manifest, nullptr, layout, problems);
    OA_CHECK(!ok);
    OA_CHECK(layout.id == "sentinel");
    OA_CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        OA_CHECK(!problems[0].warning);
        OA_CHECK(problems[0].message == "a modifier on its own is not a chord of this action");
        OA_CHECK(problems[0].key == "bindings.order.attack[0]");
    }

    files::Manifest modifier;
    OA_CHECK(read_ok("order.queue-modifier: [Shift]\n", modifier));
    Layout kept;
    std::vector<LayoutProblem> kept_problems;
    OA_CHECK(keys::layout_from_manifest(modifier, nullptr, kept, kept_problems));
    OA_CHECK(error_count(kept_problems) == 0);
    OA_CHECK(kept.bindings.size() == 1);
    if (kept.bindings.size() == 1 && !kept.bindings[0].chords.empty())
        OA_CHECK(kept.bindings[0].chords[0].kind == keys::TriggerKind::modifier_alone);
}

/// Every registry action's defaults round-trip through the paste to the same layout.
void test_defaults_round_trip() {
    files::Manifest manifest;
    manifest.base = files::Base::oa_default;
    for (const actions::Action& action : actions::actions()) {
        files::BindingEntry entry;
        entry.action = std::string{action.id};
        for (const std::string_view chord : action.defaults) {
            files::ChordText text;
            text.text = std::string{chord};
            entry.chords.push_back(std::move(text));
        }
        manifest.bindings.push_back(std::move(entry));
    }
    const std::string text = files::write_bindings_text(manifest);
    files::Manifest read;
    OA_CHECK(read_ok(text, read));
    Layout layout;
    std::vector<LayoutProblem> problems;
    const bool ok = keys::layout_from_manifest(read, nullptr, layout, problems);
    if (!ok)
        show_problems(problems);
    OA_CHECK(ok);
    OA_CHECK(error_count(problems) == 0);
    OA_CHECK(layout.bindings.size() == actions::actions().size());

    const files::Manifest restored = keys::manifest_from_layout(layout, read);
    const std::string again = files::write_bindings_text(restored);
    if (again != text)
        std::fprintf(stderr, "defaults text changed\n");
    OA_CHECK(again == text);

    files::Manifest read_again;
    OA_CHECK(read_ok(again, read_again));
    Layout layout_again;
    std::vector<LayoutProblem> again_problems;
    OA_CHECK(keys::layout_from_manifest(read_again, nullptr, layout_again, again_problems));
    OA_CHECK(same_layout(layout, layout_again));
}

/// The sixteen option keys, and a key code outside them.
void test_legacy_keys() {
    struct Sample {
        uint32_t code{};
        const char* place{};
        const char* letter{};
    };

    const Sample samples[] = {
        {0x400000e2U, nullptr, "Alt"},
        {0x400000e0U, nullptr, "Ctrl"},
        {0x400000e1U, nullptr, "Shift"},
        {'\t', "Tab", "Tab"},
        {'b', "KeyB", "B"},
        {'c', "KeyC", "C"},
        {'e', "KeyE", "E"},
        {'f', "KeyF", "F"},
        {'g', "KeyG", "G"},
        {'q', "KeyQ", "Q"},
        {'r', "KeyR", "R"},
        {'v', "KeyV", "V"},
        {'x', "KeyX", "X"},
        {'z', "KeyZ", "Z"},
        {'/', "Slash", "/"},
        {'\\', "Backslash", "\\"},
    };
    const actions::By ways[] = {actions::By::none, actions::By::letter, actions::By::place};
    for (const Sample& sample : samples) {
        for (const actions::By by : ways) {
            uint16_t scancode = 0;
            if (sample.place != nullptr) {
                const std::optional<uint16_t> found = keys::scancode_of(sample.place);
                OA_CHECK(found.has_value());
                if (!found)
                    continue;
                scancode = *found;
            }
            const std::optional<keys::Chord> chord =
                keys::legacy_key_chord(sample.code, scancode, by);
            OA_CHECK(chord.has_value());
            if (!chord)
                continue;
            const bool modifier = sample.code == 0x400000e2U || sample.code == 0x400000e0U ||
                                  sample.code == 0x400000e1U;
            std::string text;
            if (modifier || by != actions::By::place)
                text = sample.letter;
            else
                text = std::string("@") + sample.place;
            keys::ChordError error;
            const std::optional<keys::Chord> parsed = keys::parse_chord(text, error);
            OA_CHECK(parsed.has_value());
            OA_CHECK(parsed && *parsed == *chord);
        }
    }
    const uint32_t outside[] = {
        0U, static_cast<uint32_t>('a'), static_cast<uint32_t>('Q'), 0x4000003aU
    };
    for (const uint32_t code : outside) {
        for (const actions::By by : ways)
            OA_CHECK(!keys::legacy_key_chord(code, 0, by).has_value());
    }

    const actions::Action* rotate = actions::find("build.rotate");
    const actions::Action* snap = actions::find("build.snap-override");
    OA_CHECK(rotate != nullptr && snap != nullptr);
    const std::optional<uint16_t> key_q = keys::scancode_of("KeyQ");
    OA_CHECK(key_q.has_value());
    if (rotate == nullptr || snap == nullptr || !key_q)
        return;
    const std::optional<keys::Chord> placed =
        keys::legacy_key_chord('q', *key_q, actions::By::place);
    OA_CHECK(placed.has_value());
    if (!placed)
        return;
    const std::vector<keys::Chord> turned = keys::imported_list(*rotate, *placed);
    OA_CHECK(turned.size() == 2);
    if (turned.size() == 2) {
        OA_CHECK(keys::chord_text(turned[0]) == "@KeyQ");
        OA_CHECK(keys::chord_text(turned[1]) == "Shift+@KeyQ");
    }
    const std::optional<keys::Chord> letter =
        keys::legacy_key_chord('q', *key_q, actions::By::none);
    OA_CHECK(letter.has_value());
    if (letter) {
        const std::vector<keys::Chord> snapped = keys::imported_list(*snap, *letter);
        OA_CHECK(snapped.size() == 1);
        if (snapped.size() == 1)
            OA_CHECK(keys::chord_text(snapped[0]) == "Q");
    }
    keys::ChordError alt_error;
    const std::optional<keys::Chord> alt = keys::parse_chord("Alt", alt_error);
    OA_CHECK(alt.has_value());
    if (alt) {
        const std::vector<keys::Chord> alone = keys::imported_list(*rotate, *alt);
        OA_CHECK(alone.size() == 1);
        if (alone.size() == 1)
            OA_CHECK(keys::chord_text(alone[0]) == "Alt");
    }
}

} // namespace

int main() {
    test_bad_chord();
    test_unknown_action();
    test_escape();
    test_repeated_chord();
    test_not_on_keyboard();
    test_lone_modifier();
    test_defaults_round_trip();
    test_legacy_keys();
    return oa::test::check_exit_status();
}
