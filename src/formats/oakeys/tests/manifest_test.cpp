// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Layout manifests: the standard's example, each refused rule, the paste,
// and a chord of every printable character but space.

#include "oa/formats/oakeys/manifest.hpp"

#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace keys = oa::formats::oakeys;
namespace oamut = oa::formats::oamut;

using keys::Base;
using keys::BindingEntry;
using keys::ChordText;
using keys::EdgeScroll;
using keys::Helpers;
using keys::ImportedFrom;
using keys::Keyboard;
using keys::Manifest;
using keys::Pointer;
using keys::Problem;
using keys::RadialOrders;

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
/// @param nth which occurrence, counting from 0
/// @param offset bytes after the marker's first byte
/// @return the place, or zeros when the marker is absent
Place place_of(
    std::string_view text, std::string_view marker, std::size_t nth, std::size_t offset
) {
    std::size_t from = 0;
    std::size_t at = std::string_view::npos;
    for (std::size_t seen = 0; seen <= nth; ++seen) {
        at = text.find(marker, from);
        if (at == std::string_view::npos)
            break;
        from = at + 1;
    }
    if (at == std::string_view::npos || (offset != 0 && at + offset >= text.size())) {
        std::fprintf(
            stderr,
            "marker missing (%zu): %.*s\n",
            nth,
            static_cast<int>(marker.size()),
            marker.data()
        );
        OA_CHECK(false);
        return {};
    }
    at += offset;
    Place place{1, 1};
    for (std::size_t index = 0; index < at; ++index) {
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
void show_problems(const std::vector<Problem>& problems) {
    for (const Problem& problem : problems)
        std::fprintf(stderr, "  %s\n", keys::describe(problem).c_str());
}

/// The standard's section 10 example.
///
/// @return the example
std::string example_text() {
    return R"(oakeys: 1
id: grid-left-hand
name: "Grid, left hand"
version: "1.0"
author: {name: "OA team"}
description: "Orders on Q W E R, the build menu on a 4 by 3 grid of keys, groups on the number row."
tags: [grid, left-hand]
base: oa-default
keyboard: any
bindings:
  order.move: ["@KeyQ"]
  order.attack: ["@KeyW"]
  order.patrol: ["@KeyE"]
  order.reclaim: ["@KeyR"]
  order.stop: ["@KeyS"]
  order.guard: ["@KeyG"]
  build.cell-1: ["@KeyZ"]
  build.cell-2: ["@KeyX"]
  build.cell-3: ["@KeyC"]
  build.cell-4: ["@KeyV"]
  select.idle-builders: ["@KeyB"]
helpers:
  toggle: []
strings:
  de: {name: "Raster, linke Hand"}
)";
}

/// A manifest of the required keys only.
///
/// @return the manifest's text
std::string minimal_text() {
    return R"(oakeys: 1
id: grid-left-hand
name: "Grid"
version: "1.0"
author: {name: "OA team"}
tags: [grid]
base: oa-default
)";
}

/// Replaces the one occurrence of a piece of a manifest.
///
/// @param text the manifest
/// @param from the piece
/// @param to the replacement
/// @return the manifest
std::string edited(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    const std::size_t again = at == std::string::npos ? std::string::npos : text.find(from, at + 1);
    if (at == std::string::npos || again != std::string::npos) {
        std::fprintf(
            stderr, "edit is not unique: %.*s\n", static_cast<int>(from.size()), from.data()
        );
        OA_CHECK(false);
        return text;
    }
    text.replace(at, from.size(), to);
    return text;
}

/// Reads a manifest that must be refused, with one problem at a marker.
///
/// A problem already in the list stays there, and the manifest is left as it was.
///
/// @param yaml the manifest
/// @param key the problem's key
/// @param marker a piece of text at the value, or ending where `offset` says
/// @param message a piece of the problem's message
/// @param nth which occurrence of the marker
/// @param offset bytes from the marker to the value
void expect_one(
    std::string_view yaml,
    std::string_view key,
    std::string_view marker,
    std::string_view message,
    std::size_t nth = 0,
    std::size_t offset = 0
) {
    Manifest manifest;
    manifest.id = "sentinel";
    manifest.name = "Sentinel";
    manifest.homepage = "https://sentinel.example/kept";
    std::vector<Problem> problems;
    problems.push_back(Problem{7, 7, "kept", "kept"});
    const bool read = keys::read_manifest(as_bytes(yaml), manifest, problems);
    const Place place = place_of(yaml, marker, nth, offset);
    bool matched = false;
    if (problems.size() == 2 && problems[0].key == "kept" && problems[0].message == "kept") {
        const Problem& problem = problems[1];
        matched = problem.key == key && problem.line == place.line &&
                  problem.column == place.column &&
                  problem.message.find(message) != std::string::npos;
    }
    if (read || manifest.id != "sentinel" || manifest.name != "Sentinel" ||
        manifest.homepage != "https://sentinel.example/kept" || !matched) {
        std::fprintf(
            stderr,
            "expected one %.*s at %u:%u (%.*s)\n",
            static_cast<int>(key.size()),
            key.data(),
            place.line,
            place.column,
            static_cast<int>(message.size()),
            message.data()
        );
        show_problems(problems);
    }
    OA_CHECK(!read);
    OA_CHECK(manifest.id == "sentinel");
    OA_CHECK(manifest.name == "Sentinel");
    OA_CHECK(manifest.homepage == "https://sentinel.example/kept");
    OA_CHECK(matched);
    if (problems.size() == 2) {
        const std::string described = keys::describe(problems[1]);
        const std::string wanted = "oakeys.yaml:" + std::to_string(problems[1].line) + ":" +
                                   std::to_string(problems[1].column) + ": " + problems[1].key +
                                   ": " + problems[1].message;
        OA_CHECK(described == wanted);
    }
}

/// Reads a manifest that must pass.
///
/// @param yaml the manifest
/// @param[out] manifest the manifest
/// @return true when it passed
bool read_ok(std::string_view yaml, Manifest& manifest) {
    std::vector<Problem> problems;
    Manifest read;
    const bool ok = keys::read_manifest(as_bytes(yaml), read, problems);
    if (!ok || !problems.empty()) {
        std::fprintf(stderr, "expected a manifest, got %zu problems\n", problems.size());
        show_problems(problems);
        OA_CHECK(false);
        return false;
    }
    manifest = std::move(read);
    return true;
}

/// Reads a paste that must pass.
///
/// @param yaml the paste
/// @param[out] manifest the manifest
/// @return true when it passed
bool read_text_ok(std::string_view yaml, Manifest& manifest) {
    std::vector<Problem> problems;
    Manifest read;
    const bool ok = keys::read_bindings_text(as_bytes(yaml), read, problems);
    if (!ok || !problems.empty()) {
        std::fprintf(stderr, "expected a paste, got %zu problems\n", problems.size());
        show_problems(problems);
        OA_CHECK(false);
        return false;
    }
    manifest = std::move(read);
    return true;
}

/// Tells whether two versions were written the same way.
///
/// @param first the first version
/// @param second the second version
/// @return true when the value and the written parts agree
bool same_version(const oamut::Version& first, const oamut::Version& second) {
    return first == second && first.parts == second.parts &&
           oamut::version_text(first) == oamut::version_text(second);
}

/// Tells whether two conditions were written the same way.
///
/// @param first the first condition
/// @param second the second condition
/// @return true when the id and the range text agree
bool same_condition(const oamut::Condition& first, const oamut::Condition& second) {
    return first.id == second.id && first.range_text == second.range_text;
}

/// Tells whether two translations agree.
///
/// @param first the first translation
/// @param second the second translation
/// @return true when the language and its words agree
bool same_translation(const oamut::Translation& first, const oamut::Translation& second) {
    if (first.language != second.language || first.name != second.name ||
        first.description != second.description || first.options.size() != second.options.size() ||
        first.labels.size() != second.labels.size())
        return false;
    for (std::size_t index = 0; index < first.options.size(); ++index) {
        if (first.options[index].id != second.options[index].id ||
            first.options[index].label != second.options[index].label)
            return false;
    }
    for (std::size_t index = 0; index < first.labels.size(); ++index) {
        if (first.labels[index].id != second.labels[index].id ||
            first.labels[index].labels != second.labels[index].labels)
            return false;
    }
    return true;
}

/// Tells whether two helper blocks agree, including a list that was set empty.
///
/// @param first the first helpers
/// @param second the second helpers
/// @return true when every helper agrees
bool same_helpers(const Helpers& first, const Helpers& second) {
    if (first.toggle.has_value() != second.toggle.has_value() ||
        first.sticky_modifiers != second.sticky_modifiers ||
        first.hold_to_confirm.has_value() != second.hold_to_confirm.has_value() ||
        first.pointer_keys != second.pointer_keys || first.radial_orders != second.radial_orders ||
        first.edge_scroll != second.edge_scroll)
        return false;
    if (first.toggle.has_value() && *first.toggle != *second.toggle)
        return false;
    if (first.hold_to_confirm.has_value() && *first.hold_to_confirm != *second.hold_to_confirm)
        return false;
    return true;
}

/// Tells whether two binding lists agree.
///
/// @param first the first list
/// @param second the second list
/// @param positions whether lines and columns must agree
/// @return true when the actions and chords agree
bool same_bindings(
    const std::vector<BindingEntry>& first, const std::vector<BindingEntry>& second, bool positions
) {
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const BindingEntry& left = first[index];
        const BindingEntry& right = second[index];
        if (left.action != right.action || left.chords.size() != right.chords.size())
            return false;
        if (positions && (left.line != right.line || left.column != right.column))
            return false;
        for (std::size_t chord_index = 0; chord_index < left.chords.size(); ++chord_index) {
            const ChordText& left_chord = left.chords[chord_index];
            const ChordText& right_chord = right.chords[chord_index];
            if (left_chord.text != right_chord.text)
                return false;
            if (positions &&
                (left_chord.line != right_chord.line || left_chord.column != right_chord.column))
                return false;
        }
    }
    return true;
}

/// Tells whether two manifests agree.
///
/// @param first the first manifest
/// @param second the second manifest
/// @param positions whether binding lines and columns must agree
/// @return true when the keys agree
bool same_manifest(const Manifest& first, const Manifest& second, bool positions) {
    if (first.format != second.format || first.id != second.id || first.name != second.name ||
        !same_version(first.version, second.version) || first.author.name != second.author.name ||
        first.author.email != second.author.email || first.description != second.description ||
        first.homepage != second.homepage || first.icon != second.icon ||
        first.tags != second.tags || first.base != second.base ||
        first.requires_engine != second.requires_engine || first.keyboard != second.keyboard ||
        first.pointer != second.pointer || first.works_with.size() != second.works_with.size() ||
        first.strings.size() != second.strings.size() ||
        !same_helpers(first.helpers, second.helpers))
        return false;
    if (first.imported_from.has_value() != second.imported_from.has_value())
        return false;
    if (first.imported_from.has_value() &&
        (first.imported_from->tool != second.imported_from->tool ||
         first.imported_from->file != second.imported_from->file ||
         first.imported_from->date != second.imported_from->date))
        return false;
    for (std::size_t index = 0; index < first.works_with.size(); ++index) {
        if (!same_condition(first.works_with[index], second.works_with[index]))
            return false;
    }
    for (std::size_t index = 0; index < first.strings.size(); ++index) {
        if (!same_translation(first.strings[index], second.strings[index]))
            return false;
    }
    return same_bindings(first.bindings, second.bindings, positions);
}

/// The example's eleven chords, in file order.
constexpr std::string_view example_actions[] = {
    "order.move",
    "order.attack",
    "order.patrol",
    "order.reclaim",
    "order.stop",
    "order.guard",
    "build.cell-1",
    "build.cell-2",
    "build.cell-3",
    "build.cell-4",
    "select.idle-builders",
};

constexpr std::string_view example_chords[] = {
    "@KeyQ",
    "@KeyW",
    "@KeyE",
    "@KeyR",
    "@KeyS",
    "@KeyG",
    "@KeyZ",
    "@KeyX",
    "@KeyC",
    "@KeyV",
    "@KeyB",
};

/// The standard's example, then the writer's text of it.
void test_example() {
    const std::string example = example_text();
    Manifest manifest;
    OA_CHECK(read_ok(example, manifest));
    OA_CHECK(manifest.format == keys::format_version);
    OA_CHECK(manifest.id == "grid-left-hand");
    OA_CHECK(manifest.name == "Grid, left hand");
    OA_CHECK(manifest.version.parts == 2 && oamut::version_text(manifest.version) == "1.0");
    OA_CHECK(manifest.author.name == "OA team" && manifest.author.email.empty());
    OA_CHECK(
        manifest.description ==
        "Orders on Q W E R, the build menu on a 4 by 3 grid of keys, groups on the number row."
    );
    OA_CHECK(
        manifest.tags.size() == 2 && manifest.tags[0] == "grid" && manifest.tags[1] == "left-hand"
    );
    OA_CHECK(manifest.base == Base::oa_default);
    OA_CHECK(manifest.keyboard == Keyboard::any);
    OA_CHECK(!manifest.pointer.has_value());
    OA_CHECK(manifest.works_with.empty());
    OA_CHECK(manifest.requires_engine.empty());
    OA_CHECK(manifest.helpers.toggle.has_value() && manifest.helpers.toggle->empty());
    OA_CHECK(!manifest.helpers.sticky_modifiers.has_value());
    OA_CHECK(manifest.strings.size() == 1);
    if (manifest.strings.size() == 1) {
        OA_CHECK(manifest.strings[0].language == "de");
        OA_CHECK(manifest.strings[0].name == "Raster, linke Hand");
    }
    OA_CHECK(manifest.bindings.size() == 11);
    if (manifest.bindings.size() == 11) {
        for (std::size_t index = 0; index < 11; ++index) {
            OA_CHECK(manifest.bindings[index].action == example_actions[index]);
            OA_CHECK(manifest.bindings[index].chords.size() == 1);
            if (manifest.bindings[index].chords.size() == 1)
                OA_CHECK(manifest.bindings[index].chords[0].text == example_chords[index]);
        }
        const Place key = place_of(example, "order.move", 0, 0);
        const Place chord = place_of(example, "\"@KeyQ\"", 0, 0);
        const Place list = place_of(example, "[", 0, 0);
        OA_CHECK(
            manifest.bindings[0].line == key.line && manifest.bindings[0].column == key.column
        );
        OA_CHECK(
            manifest.bindings[0].chords[0].line == chord.line &&
            manifest.bindings[0].chords[0].column == chord.column
        );
        OA_CHECK(chord.column > list.column);
    }

    const std::string once = keys::write_manifest(manifest);
    const char* order[] = {
        "oakeys:",
        "\nid:",
        "\nname:",
        "\nversion:",
        "\nauthor:",
        "\ndescription:",
        "\ntags:",
        "\nbase:",
        "\nbindings:",
        "\nhelpers:",
        "\nstrings:",
    };
    std::size_t from = 0;
    for (const char* key : order) {
        const std::size_t found = once.find(key, from);
        if (found == std::string::npos)
            std::fprintf(stderr, "key out of order: %s\n%s", key, once.c_str());
        OA_CHECK(found != std::string::npos);
        from = found + 1;
    }
    OA_CHECK(once.find("keyboard:") == std::string::npos);
    OA_CHECK(once.find("toggle: []") != std::string::npos);
    OA_CHECK(once.find("  order.move: [\"@KeyQ\"]\n") != std::string::npos);

    Manifest again;
    OA_CHECK(read_ok(once, again));
    if (!same_manifest(manifest, again, false))
        std::fprintf(stderr, "round trip lost a field:\n%s", once.c_str());
    OA_CHECK(same_manifest(manifest, again, false));
    OA_CHECK(keys::write_manifest(again) == once);

    Manifest third;
    OA_CHECK(read_ok(once, third));
    OA_CHECK(same_manifest(again, third, true));
}

/// A manifest that sets every optional key, including a false helper.
void test_full_round_trip() {
    Manifest manifest;
    manifest.id = "ridge-keys";
    manifest.name = "Ridge keys";
    const std::optional<oamut::Version> version = oamut::parse_version("1.2.3");
    OA_CHECK(version.has_value());
    if (version.has_value())
        manifest.version = *version;
    manifest.author.name = "Ridge team";
    manifest.author.email = "ridge@example.com";
    manifest.description = "Line one.\nLine two.";
    manifest.homepage = "https://example.com/ridge";
    manifest.icon = "icons/ridge.png";
    manifest.tags = {"ridge", "left-hand"};
    manifest.base = Base::none;
    oamut::Condition condition;
    condition.id = "ridge";
    condition.range_text = ">= 28";
    manifest.works_with.push_back(condition);
    manifest.requires_engine = ">= 0.8.0";
    manifest.keyboard = Keyboard::iso;
    manifest.pointer = Pointer::left_click;
    BindingEntry empty;
    empty.action = "order.attack";
    manifest.bindings.push_back(empty);
    BindingEntry special;
    special.action = "order.patrol";
    special.chords.push_back(ChordText{"1", 0, 0});
    special.chords.push_back(ChordText{"true", 0, 0});
    special.chords.push_back(ChordText{"1e2", 0, 0});
    special.chords.push_back(ChordText{"~", 0, 0});
    manifest.bindings.push_back(special);
    manifest.helpers.toggle = std::vector<std::string>{"order.attack"};
    manifest.helpers.sticky_modifiers = false;
    manifest.helpers.hold_to_confirm = std::vector<std::string>{};
    manifest.helpers.pointer_keys = true;
    manifest.helpers.radial_orders = RadialOrders::hold_middle;
    manifest.helpers.edge_scroll = EdgeScroll::off;
    oamut::Translation german;
    german.language = "de";
    german.name = "Kamm";
    manifest.strings.push_back(german);
    ImportedFrom imported;
    imported.tool = "recorder";
    imported.file = "keys.ini";
    imported.date = "2026-10-26";
    manifest.imported_from = imported;

    const std::string once = keys::write_manifest(manifest);
    OA_CHECK(once.find("keyboard: iso\n") != std::string::npos);
    OA_CHECK(once.find("pointer: left-click\n") != std::string::npos);
    OA_CHECK(once.find("sticky-modifiers: false\n") != std::string::npos);
    OA_CHECK(once.find("hold-to-confirm: []\n") != std::string::npos);
    OA_CHECK(once.find("order.attack: []\n") != std::string::npos);
    OA_CHECK(once.find("[\"1\", \"true\", \"1e2\", \"~\"]") != std::string::npos);
    Manifest again;
    OA_CHECK(read_ok(once, again));
    if (!same_manifest(manifest, again, false))
        std::fprintf(stderr, "full round trip lost a field:\n%s", once.c_str());
    OA_CHECK(same_manifest(manifest, again, false));
    OA_CHECK(keys::write_manifest(again) == once);
    OA_CHECK(again.helpers.sticky_modifiers.has_value() && !*again.helpers.sticky_modifiers);
    OA_CHECK(again.helpers.hold_to_confirm.has_value() && again.helpers.hold_to_confirm->empty());
}

/// An id that starts with own- is a normal id here.
void test_own_id() {
    Manifest manifest;
    OA_CHECK(read_ok(edited(minimal_text(), "id: grid-left-hand", "id: own-ridge"), manifest));
    OA_CHECK(manifest.id == "own-ridge");
}

/// A chord of every printable ASCII character but space.
void test_printable_chord() {
    std::string chord;
    for (int character = 0x21; character <= 0x7E; ++character)
        chord.push_back(static_cast<char>(character));
    Manifest manifest;
    OA_CHECK(read_ok(minimal_text(), manifest));
    BindingEntry entry;
    entry.action = "order.attack";
    entry.chords.push_back(ChordText{chord, 0, 0});
    manifest.bindings.push_back(entry);
    const std::string once = keys::write_manifest(manifest);
    Manifest again;
    OA_CHECK(read_ok(once, again));
    OA_CHECK(again.bindings.size() == 1 && again.bindings[0].chords.size() == 1);
    if (again.bindings.size() == 1 && again.bindings[0].chords.size() == 1)
        OA_CHECK(again.bindings[0].chords[0].text == chord);
    OA_CHECK(keys::write_manifest(again) == once);
}

/// Each rule the reader refuses, at the value the rule names.
void test_refusals() {
    const std::string full = minimal_text();
    expect_one(full + "colour: \"blue\"\n", "colour", "\"blue\"", "is not a key of oakeys.yaml");
    expect_one(full + "7: 1\n", "7", "7: 1", "is not a key of oakeys.yaml", 0, 3);
    expect_one(
        edited(full, "oakeys: 1", "oakeys: 2"), "oakeys", "oakeys: 2", "oakeys must be 1", 0, 8
    );
    expect_one(edited(full, "oakeys: 1", "oakeys: 1.0"), "oakeys", "1.0", "oakeys must be 1");
    expect_one(edited(full, "oakeys: 1", "oakeys: \"1\""), "oakeys", "\"1\"", "oakeys must be 1");
    expect_one(edited(full, "oakeys: 1\n", ""), "oakeys", "id:", "oakeys must be 1");

    expect_one(
        edited(full, "id: grid-left-hand", "id: Grid"), "id", "Grid", "lower-case kebab-case"
    );
    expect_one(
        edited(full, "id: grid-left-hand", "id: \"a-\""), "id", "\"a-\"", "lower-case kebab-case"
    );
    expect_one(
        edited(full, "id: grid-left-hand", "id: " + std::string(65, 'a')),
        "id",
        std::string(65, 'a'),
        "1 to 64"
    );
    expect_one(
        edited(full, "id: grid-left-hand", "id: classic"), "id", "classic", "built-in layout"
    );
    expect_one(
        edited(full, "id: grid-left-hand", "id: oa-default"), "id", "oa-default", "built-in layout"
    );
    expect_one(edited(full, "id: grid-left-hand\n", ""), "id", "oakeys:", "id must be");

    expect_one(edited(full, "name: \"Grid\"\n", ""), "name", "oakeys:", "name is required");
    expect_one(
        edited(full, "name: \"Grid\"", "name: \"" + std::string(65, 'n') + "\""),
        "name",
        "\"" + std::string(65, 'n') + "\"",
        "no control character"
    );
    expect_one(edited(full, R"(name: "Grid")", R"(name: "a\tb")"), "name", R"("a\tb")", "control");

    expect_one(edited(full, "version: \"1.0\"\n", ""), "version", "oakeys:", "version is required");
    expect_one(edited(full, "version: \"1.0\"", "version: 1.0"), "version", "1.0", "in quotes");
    expect_one(edited(full, "version: \"1.0\"", "version: two"), "version", "two", "in quotes");
    expect_one(
        edited(full, "version: \"1.0\"", "version: true"), "version", "true", "must be a string"
    );
    expect_one(
        edited(full, "version: \"1.0\"", "version: \"nope\""),
        "version",
        "\"nope\"",
        "mutator version"
    );

    expect_one(
        edited(full, "author: {name: \"OA team\"}\n", ""), "author", "oakeys:", "author is required"
    );
    expect_one(
        edited(full, "author: {name: \"OA team\"}", "author: {name: \"OA team\", email: \"nope\"}"),
        "author.email",
        "\"nope\"",
        "e-mail address"
    );

    expect_one(
        full + "description: \"" + std::string(2001, 'd') + "\"\n",
        "description",
        "\"" + std::string(2001, 'd') + "\"",
        "1 to 2000"
    );
    expect_one(full + "description: \"\\t\"\n", "description", "\"\\t\"", "control character");

    expect_one(full + "homepage: \"notaurl\"\n", "homepage", "\"notaurl\"", "http or https");
    expect_one(full + "icon: \"picture.jpg\"\n", "icon", "\"picture.jpg\"", "ending in .png");

    expect_one(edited(full, "tags: [grid]\n", ""), "tags", "oakeys:", "tags is required");
    expect_one(edited(full, "tags: [grid]", "tags: []"), "tags", "[]", "leave the key out");
    expect_one(
        edited(full, "tags: [grid]", "tags: [one, two, three, four, five]"),
        "tags",
        "[one, two, three, four, five]",
        "at most 4"
    );
    expect_one(
        edited(full, "tags: [grid]", "tags: [a, b, c, d, e, f, g, h, i]"),
        "tags",
        "[a, b, c, d, e, f, g, h, i]",
        "at most 8"
    );
    expect_one(edited(full, "tags: [grid]", "tags: [NotATag]"), "tags[0]", "NotATag", "kebab-case");

    expect_one(edited(full, "base: oa-default\n", ""), "base", "oakeys:", "base is required");
    expect_one(
        edited(full, "base: oa-default", "base: classic"), "base", "classic", "oa-default or none"
    );

    expect_one(
        full + "works-with: []\n", "works-with", "works-with: []", "must list 1 to 32", 0, 12
    );
    expect_one(full + "requires: 1\n", "requires", "requires: 1", "mapping of engine", 0, 10);
    expect_one(
        full + "requires:\n  engine: \">= 0.8.0\"\n  mutators: []\n",
        "requires.mutators",
        "mutators: []",
        "is not a key of requires",
        0,
        10
    );

    expect_one(full + "keyboard: qwerty\n", "keyboard", "qwerty", "any, ansi, iso or jis");
    expect_one(
        full + "pointer: middle-click\n", "pointer", "middle-click", "right-click or left-click"
    );

    expect_one(full + "bindings: 1\n", "bindings", "bindings: 1", "mapping of action ids", 0, 10);
    expect_one(full + "bindings:\n  Attack: [A]\n", "bindings.Attack", "Attack", "joined by dots");
    expect_one(
        full + "bindings:\n  order.attack: A\n",
        "bindings.order.attack",
        "order.attack: A",
        "list of chords",
        0,
        std::string("order.attack: ").size()
    );
    expect_one(
        full + "bindings:\n  order.attack: [1]\n",
        "bindings.order.attack[0]",
        "order.attack: [1]",
        "must be a string",
        0,
        std::string("order.attack: [").size()
    );
    expect_one(
        full + "bindings:\n  order.attack: [A, B, C, D, E]\n",
        "bindings.order.attack",
        "[A, B, C, D, E]",
        "lists 5 chords"
    );

    expect_one(full + "helpers: 1\n", "helpers", "helpers: 1", "must be a mapping", 0, 9);
    expect_one(
        full + "helpers:\n  colour: 1\n",
        "helpers.colour",
        "colour: 1",
        "is not a key of helpers",
        0,
        std::string("colour: ").size()
    );
    expect_one(
        full + "helpers:\n  radial-orders: hold-left\n",
        "helpers.radial-orders",
        "hold-left",
        "off, hold-right or hold-middle"
    );
    expect_one(
        full + "helpers:\n  edge-scroll: maybe\n", "helpers.edge-scroll", "maybe", "on or off"
    );
    expect_one(
        full + "helpers:\n  sticky-modifiers: \"true\"\n",
        "helpers.sticky-modifiers",
        "\"true\"",
        "true or false"
    );
}

/// 2,049 bindings are one problem, at the mapping.
void test_too_many_bindings() {
    std::string yaml = minimal_text();
    yaml += "bindings: {";
    for (int index = 0; index < 2049; ++index) {
        if (index != 0)
            yaml += ", ";
        yaml += "extra.b" + std::to_string(index) + ": [A]";
    }
    yaml += "}\n";
    expect_one(yaml, "bindings", "bindings: {", "lists 2049", 0, std::string("bindings: ").size());
}

/// A text longer than 256 KiB is one problem on the key document.
void test_too_large() {
    const std::string huge(257 * 1024, 'a');
    Manifest manifest;
    manifest.id = "sentinel";
    manifest.name = "Sentinel";
    std::vector<Problem> problems;
    const bool read = keys::read_manifest(as_bytes(huge), manifest, problems);
    OA_CHECK(!read);
    OA_CHECK(manifest.id == "sentinel");
    OA_CHECK(manifest.name == "Sentinel");
    OA_CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        OA_CHECK(problems[0].key == "document");
        OA_CHECK(problems[0].line == 1 && problems[0].column == 1);
        OA_CHECK(problems[0].message.find("256") != std::string::npos);
        OA_CHECK(keys::describe(problems[0]).starts_with("oakeys.yaml:1:1: document:"));
    }
}

/// The paste round-trips, and a whole manifest is read as a paste.
void test_text_form() {
    Manifest manifest;
    manifest.base = Base::none;
    BindingEntry entry;
    entry.action = "order.attack";
    entry.chords.push_back(ChordText{"A", 0, 0});
    entry.chords.push_back(ChordText{"@KeyQ", 0, 0});
    manifest.bindings.push_back(entry);
    const std::string once = keys::write_bindings_text(manifest);
    OA_CHECK(once.starts_with("# oakeys bindings, base none\n"));
    OA_CHECK(once.find("order.attack: [A, \"@KeyQ\"]\n") != std::string::npos);
    Manifest again;
    OA_CHECK(read_text_ok(once, again));
    OA_CHECK(again.base == Base::none);
    OA_CHECK(again.id.empty());
    OA_CHECK(same_bindings(manifest.bindings, again.bindings, false));
    OA_CHECK(keys::write_bindings_text(again) == once);

    Manifest bare;
    OA_CHECK(read_text_ok("order.move: [Q]\n", bare));
    OA_CHECK(bare.base == Base::oa_default);
    OA_CHECK(bare.bindings.size() == 1 && bare.bindings[0].action == "order.move");
    if (bare.bindings.size() == 1 && !bare.bindings[0].chords.empty())
        OA_CHECK(bare.bindings[0].chords[0].text == "Q");

    const std::string bad = "# oakeys bindings, base classic\norder.attack: [A]\n";
    Manifest sentinel;
    sentinel.id = "sentinel";
    std::vector<Problem> problems;
    problems.push_back(Problem{7, 7, "kept", "kept"});
    const bool refused = keys::read_bindings_text(as_bytes(bad), sentinel, problems);
    const Place token = place_of(bad, "# oakeys bindings, base ", 0, 24);
    OA_CHECK(!refused);
    OA_CHECK(sentinel.id == "sentinel");
    OA_CHECK(problems.size() == 2);
    if (problems.size() == 2) {
        OA_CHECK(problems[1].key == "base");
        OA_CHECK(problems[1].line == token.line && problems[1].column == token.column);
        OA_CHECK(problems[1].message.find("oa-default or none") != std::string::npos);
    }

    const std::string example = example_text();
    Manifest from_manifest;
    Manifest from_text;
    OA_CHECK(read_ok(example, from_manifest));
    OA_CHECK(read_text_ok(example, from_text));
    OA_CHECK(same_manifest(from_manifest, from_text, true));

    const std::string commented = "# oakeys bindings, base classic\n" + example;
    Manifest ignored;
    OA_CHECK(read_text_ok(commented, ignored));
    OA_CHECK(ignored.base == Base::oa_default);
    OA_CHECK(ignored.id == "grid-left-hand");
}

} // namespace

int main() {
    test_example();
    test_full_round_trip();
    test_own_id();
    test_printable_chord();
    test_refusals();
    test_too_many_bindings();
    test_too_large();
    test_text_form();
    return oa::test::check_exit_status();
}
