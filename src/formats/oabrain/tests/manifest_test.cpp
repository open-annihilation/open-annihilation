// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Brain manifests: a full steady-commander document, a minimal one, each rule
// refused once, the file roles, and the summary cut on a character boundary.

#include "oa/formats/oabrain/manifest.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brain = oa::formats::oabrain;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;

using brain::Difficulty;
using brain::FileRole;
using brain::Level;
using brain::Manifest;
using brain::Network;
using brain::Problem;
using brain::Translation;

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
        std::fprintf(stderr, "  %s\n", brain::describe(problem).c_str());
}

/// A manifest that names every key.
///
/// @return the manifest's text
std::string full_text() {
    return R"(oabrain: 1
id: steady-commander
name: "Steady Commander"
version: "2.0"
author:
  name: "A. Maker"
  email: "maker@example.com"
description: "Holds the line.\nNever rushes."
homepage: "https://example.com/steady"
icon: "icon.png"
tags: ["classic", "economy"]
works-with: ["ta-3.1c", "ridge >= 28"]
requires:
  engine: ">= 0.8.0"
  expansions: ["core-contingency"]
levels:
  - id: steady
    name: "Steady"
    difficulty: easy
  - id: bold
    name: "Bold"
    difficulty: hard
fullAwareness: true
networks:
  - id: threat
    file: "nets/threat.oann"
    sha256: "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
options:
  waves:
    type: "number"
    label: "Waves"
    min: 1
    max: 8
    step: 1
    default: 3
  stance:
    type: "choice"
    label: "Stance"
    values: ["hold", "push"]
    default: "hold"
strings:
  de:
    name: "Steter Kommandant"
    description: "Hält die Linie."
    options:
      waves: "Wellen"
      stance: "Haltung"
    labels:
      stance: ["Halten", "Drücken"]
    levels:
      steady: "Stetig"
      bold: "Kühn"
entry: "main.lua"
)";
}

/// A manifest of the required keys only.
///
/// @return the manifest's text
std::string minimal_text() {
    return R"(oabrain: 1
id: quick-raider
name: "Quick Raider"
version: "1"
author:
  name: "A. Maker"
description: "Raids early."
tags: ["rush"]
works-with: ["*"]
levels:
  - id: only
    name: "Only"
    difficulty: medium
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
    manifest.entry = "sentinel.lua";
    manifest.full_awareness = false;
    std::vector<Problem> problems;
    problems.push_back(Problem{7, 7, "kept", "kept"});
    const bool read = brain::read_manifest(as_bytes(yaml), manifest, problems);
    const Place place = place_of(yaml, marker, nth, offset);
    bool matched = false;
    if (problems.size() == 2 && problems[0].key == "kept" && problems[0].message == "kept") {
        const Problem& problem = problems[1];
        matched = problem.key == key && problem.line == place.line &&
                  problem.column == place.column &&
                  problem.message.find(message) != std::string::npos;
    }
    if (read || manifest.id != "sentinel" || manifest.name != "Sentinel" ||
        manifest.entry != "sentinel.lua" || manifest.full_awareness || !matched) {
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
    OA_CHECK(manifest.entry == "sentinel.lua");
    OA_CHECK(!manifest.full_awareness);
    OA_CHECK(matched);
    if (problems.size() == 2) {
        const std::string described = brain::describe(problems[1]);
        const std::string wanted = "oabrain.yaml:" + std::to_string(problems[1].line) + ":" +
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
    const bool ok = brain::read_manifest(as_bytes(yaml), read, problems);
    if (!ok || !problems.empty()) {
        std::fprintf(stderr, "expected a manifest, got %zu problems\n", problems.size());
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
/// @return true when the id, the range text and the comparisons agree
bool same_condition(const oamut::Condition& first, const oamut::Condition& second) {
    if (first.id != second.id || first.range_text != second.range_text)
        return false;
    if (first.range.has_value() != second.range.has_value())
        return false;
    if (!first.range.has_value())
        return true;
    if (first.range->terms.size() != second.range->terms.size())
        return false;
    for (std::size_t index = 0; index < first.range->terms.size(); ++index) {
        if (first.range->terms[index].first != second.range->terms[index].first)
            return false;
        if (!same_version(first.range->terms[index].second, second.range->terms[index].second))
            return false;
    }
    return true;
}

/// Tells whether two numbers are the same value.
///
/// @param first the first number
/// @param second the second number
/// @return true when they compare equal
bool same_number(const oamod::Number& first, const oamod::Number& second) {
    return oamod::compare_numbers(first, second) == 0;
}

/// Tells whether two choice values were written the same way.
///
/// @param first the first value
/// @param second the second value
/// @return true when both are the same number or the same string
bool same_choice(const oamut::ChoiceValue& first, const oamut::ChoiceValue& second) {
    if (first.number != second.number || first.text != second.text)
        return false;
    if (!first.number)
        return true;
    return same_number(first.number_value, second.number_value);
}

/// Tells whether two options were written the same way.
///
/// @param first the first option
/// @param second the second option
/// @return true when every field that option uses agrees
bool same_option(const oamut::Option& first, const oamut::Option& second) {
    if (first.id != second.id || first.type != second.type || first.label != second.label)
        return false;
    if (first.type == oamut::OptionType::toggle)
        return first.toggle_default == second.toggle_default;
    if (first.type == oamut::OptionType::choice) {
        if (first.default_index != second.default_index ||
            first.values.size() != second.values.size() ||
            first.value_labels != second.value_labels)
            return false;
        for (std::size_t index = 0; index < first.values.size(); ++index)
            if (!same_choice(first.values[index], second.values[index]))
                return false;
        return true;
    }
    return same_number(first.minimum, second.minimum) &&
           same_number(first.maximum, second.maximum) && same_number(first.step, second.step) &&
           same_number(first.number_default, second.number_default);
}

/// Tells whether two translations were written the same way.
///
/// @param first the first translation
/// @param second the second translation
/// @return true when every field agrees
bool same_translation(const Translation& first, const Translation& second) {
    const oamut::Translation& left = first.words;
    const oamut::Translation& right = second.words;
    if (left.language != right.language || left.name != right.name ||
        left.description != right.description || left.options.size() != right.options.size() ||
        left.labels.size() != right.labels.size() || first.levels.size() != second.levels.size())
        return false;
    for (std::size_t index = 0; index < left.options.size(); ++index)
        if (left.options[index].id != right.options[index].id ||
            left.options[index].label != right.options[index].label)
            return false;
    for (std::size_t index = 0; index < left.labels.size(); ++index)
        if (left.labels[index].id != right.labels[index].id ||
            left.labels[index].labels != right.labels[index].labels)
            return false;
    for (std::size_t index = 0; index < first.levels.size(); ++index)
        if (first.levels[index].id != second.levels[index].id ||
            first.levels[index].name != second.levels[index].name)
            return false;
    return true;
}

/// Tells whether two manifests were written the same way.
///
/// @param first the first manifest
/// @param second the second manifest
/// @return true when every field agrees
bool same_manifest(const Manifest& first, const Manifest& second) {
    if (first.format != second.format || first.id != second.id || first.name != second.name ||
        first.description != second.description || !same_version(first.version, second.version) ||
        first.author.name != second.author.name || first.author.email != second.author.email ||
        first.homepage != second.homepage || first.icon != second.icon ||
        first.tags != second.tags || first.requires_engine != second.requires_engine ||
        first.requires_expansions != second.requires_expansions ||
        first.full_awareness != second.full_awareness || first.entry != second.entry)
        return false;
    if (first.works_with.size() != second.works_with.size() ||
        first.levels.size() != second.levels.size() ||
        first.networks.size() != second.networks.size() ||
        first.options.size() != second.options.size() ||
        first.strings.size() != second.strings.size())
        return false;
    for (std::size_t index = 0; index < first.works_with.size(); ++index)
        if (!same_condition(first.works_with[index], second.works_with[index]))
            return false;
    for (std::size_t index = 0; index < first.levels.size(); ++index) {
        if (first.levels[index].id != second.levels[index].id ||
            first.levels[index].name != second.levels[index].name ||
            first.levels[index].difficulty != second.levels[index].difficulty)
            return false;
    }
    for (std::size_t index = 0; index < first.networks.size(); ++index) {
        if (first.networks[index].id != second.networks[index].id ||
            first.networks[index].file != second.networks[index].file ||
            first.networks[index].sha256 != second.networks[index].sha256)
            return false;
    }
    for (std::size_t index = 0; index < first.options.size(); ++index)
        if (!same_option(first.options[index], second.options[index]))
            return false;
    for (std::size_t index = 0; index < first.strings.size(); ++index)
        if (!same_translation(first.strings[index], second.strings[index]))
            return false;
    return true;
}

/// The manifest steady-commander, every field as written, then written back.
void test_full() {
    Manifest manifest;
    OA_CHECK(read_ok(full_text(), manifest));
    OA_CHECK(manifest.format == brain::format_version);
    OA_CHECK(manifest.id == "steady-commander");
    OA_CHECK(manifest.name == "Steady Commander");
    OA_CHECK(manifest.description == "Holds the line.\nNever rushes.");
    OA_CHECK(brain::summary_of(manifest) == "Holds the line.");
    OA_CHECK(manifest.version.parts == 2 && oamut::version_text(manifest.version) == "2.0");
    OA_CHECK(manifest.author.name == "A. Maker" && manifest.author.email == "maker@example.com");
    OA_CHECK(manifest.homepage == "https://example.com/steady");
    OA_CHECK(manifest.icon == "icon.png");
    OA_CHECK(
        manifest.tags.size() == 2 && manifest.tags[0] == "classic" && manifest.tags[1] == "economy"
    );
    OA_CHECK(manifest.works_with.size() == 2);
    if (manifest.works_with.size() == 2) {
        OA_CHECK(
            manifest.works_with[0].id == "ta-3.1c" && manifest.works_with[0].range_text.empty()
        );
        OA_CHECK(
            manifest.works_with[1].id == "ridge" && manifest.works_with[1].range_text == ">= 28"
        );
    }
    OA_CHECK(manifest.requires_engine == ">= 0.8.0");
    OA_CHECK(
        manifest.requires_expansions.size() == 1 &&
        manifest.requires_expansions[0] == "core-contingency"
    );
    OA_CHECK(manifest.levels.size() == 2);
    if (manifest.levels.size() == 2) {
        OA_CHECK(manifest.levels[0].id == "steady" && manifest.levels[0].name == "Steady");
        OA_CHECK(manifest.levels[0].difficulty == Difficulty::easy);
        OA_CHECK(manifest.levels[1].id == "bold" && manifest.levels[1].name == "Bold");
        OA_CHECK(manifest.levels[1].difficulty == Difficulty::hard);
    }
    OA_CHECK(brain::find_level(manifest, "steady") == &manifest.levels[0]);
    OA_CHECK(brain::find_level(manifest, "brutal") == nullptr);
    OA_CHECK(manifest.full_awareness);
    OA_CHECK(manifest.networks.size() == 1);
    if (manifest.networks.size() == 1) {
        OA_CHECK(manifest.networks[0].id == "threat");
        OA_CHECK(manifest.networks[0].file == "nets/threat.oann");
        OA_CHECK(manifest.networks[0].sha256.size() == 64);
    }
    OA_CHECK(manifest.options.size() == 2);
    if (manifest.options.size() == 2) {
        OA_CHECK(
            manifest.options[0].id == "waves" &&
            manifest.options[0].type == oamut::OptionType::number
        );
        OA_CHECK(oamod::canonical_number_text(manifest.options[0].minimum) == "1");
        OA_CHECK(oamod::canonical_number_text(manifest.options[0].maximum) == "8");
        OA_CHECK(oamod::canonical_number_text(manifest.options[0].step) == "1");
        OA_CHECK(oamod::canonical_number_text(manifest.options[0].number_default) == "3");
        OA_CHECK(
            manifest.options[1].id == "stance" &&
            manifest.options[1].type == oamut::OptionType::choice
        );
        OA_CHECK(manifest.options[1].values.size() == 2 && manifest.options[1].default_index == 0);
        OA_CHECK(
            manifest.options[1].values[0].text == "hold" &&
            manifest.options[1].values[1].text == "push"
        );
    }
    OA_CHECK(manifest.strings.size() == 1);
    if (manifest.strings.size() == 1) {
        const Translation& german = manifest.strings[0];
        OA_CHECK(german.words.language == "de");
        OA_CHECK(german.words.name == "Steter Kommandant");
        OA_CHECK(german.words.description == "Hält die Linie.");
        OA_CHECK(german.words.options.size() == 2 && german.words.options[0].id == "waves");
        OA_CHECK(
            german.words.options[0].label == "Wellen" && german.words.options[1].label == "Haltung"
        );
        OA_CHECK(german.words.labels.size() == 1 && german.words.labels[0].labels.size() == 2);
        OA_CHECK(german.levels.size() == 2);
        OA_CHECK(german.levels[0].id == "steady" && german.levels[0].name == "Stetig");
        OA_CHECK(german.levels[1].id == "bold" && german.levels[1].name == "Kühn");
    }
    OA_CHECK(manifest.entry == "main.lua");

    const std::string once = brain::write_manifest(manifest);
    const char* order[] = {
        "oabrain:",
        "\nid:",
        "\nname:",
        "\nversion:",
        "\nauthor:",
        "\ndescription:",
        "\nhomepage:",
        "\nicon:",
        "\ntags:",
        "\nworks-with:",
        "\nrequires:",
        "\nlevels:",
        "\nfullAwareness:",
        "\nnetworks:",
        "\noptions:",
        "\nstrings:",
        "\nentry:",
    };
    std::size_t from = 0;
    for (const char* key : order) {
        const std::size_t found = once.find(key, from);
        if (found == std::string::npos)
            std::fprintf(stderr, "key out of order: %s\n%s", key, once.c_str());
        OA_CHECK(found != std::string::npos);
        from = found + 1;
    }
    Manifest again;
    OA_CHECK(read_ok(once, again));
    if (!same_manifest(manifest, again))
        std::fprintf(stderr, "round trip lost a field:\n%s", once.c_str());
    OA_CHECK(same_manifest(manifest, again));
    OA_CHECK(brain::write_manifest(again) == once);
}

/// Required keys only: full awareness stays off and the entry stays brain.lua.
void test_minimal() {
    Manifest manifest;
    OA_CHECK(read_ok(minimal_text(), manifest));
    OA_CHECK(manifest.id == "quick-raider");
    OA_CHECK(manifest.name == "Quick Raider");
    OA_CHECK(manifest.version.parts == 1 && oamut::version_text(manifest.version) == "1");
    OA_CHECK(manifest.author.name == "A. Maker" && manifest.author.email.empty());
    OA_CHECK(manifest.description == "Raids early.");
    OA_CHECK(manifest.homepage.empty() && manifest.icon.empty());
    OA_CHECK(manifest.tags.size() == 1 && manifest.tags[0] == "rush");
    OA_CHECK(manifest.works_with.size() == 1 && manifest.works_with[0].id == "*");
    OA_CHECK(manifest.requires_engine.empty() && manifest.requires_expansions.empty());
    OA_CHECK(manifest.levels.size() == 1 && manifest.levels[0].difficulty == Difficulty::medium);
    OA_CHECK(!manifest.full_awareness);
    OA_CHECK(manifest.networks.empty() && manifest.options.empty() && manifest.strings.empty());
    OA_CHECK(manifest.entry == brain::default_entry);
    const std::string written = brain::write_manifest(manifest);
    OA_CHECK(written.find("fullAwareness") == std::string::npos);
    OA_CHECK(written.find("entry") == std::string::npos);
    OA_CHECK(written.find("homepage") == std::string::npos);
    OA_CHECK(written.find("networks") == std::string::npos);
    OA_CHECK(written.find("options") == std::string::npos);
    OA_CHECK(written.find("strings") == std::string::npos);
    Manifest again;
    OA_CHECK(read_ok(written, again));
    OA_CHECK(same_manifest(manifest, again));
}

/// One flow list of quoted ids.
///
/// @param count how many
/// @param prefix the text before each index
/// @return the flow sequence
std::string quoted_ids(int count, std::string_view prefix) {
    std::string out = "[";
    for (int index = 0; index < count; ++index) {
        if (index != 0)
            out += ", ";
        out += "\"" + std::string{prefix} + std::to_string(index) + "\"";
    }
    out += "]";
    return out;
}

/// Each broken rule is one problem, at the value, and the manifest is unchanged.
void test_refusals() {
    const std::string full = full_text();
    const std::string minimal = minimal_text();

    expect_one(full + "identity: \"x\"\n", "identity", "\"x\"", "belongs in a mod's oamod.yaml");
    expect_one(full + "hacks: 1\n", "hacks", "hacks: 1", "belongs in a mod's oamod.yaml", 0, 7);
    expect_one(full + "layers: [\"units\"]\n", "layers", "[\"units\"]", "is a mutator's key");
    expect_one(full + "clashes: add\n", "clashes", "clashes: add", "is a mutator's key", 0, 9);
    expect_one(
        full + "conflicts-with: [\"ridge\"]\n",
        "conflicts-with",
        "[\"ridge\"]",
        "is a mutator's key"
    );
    expect_one(full + "legacy: []\n", "legacy", "legacy: []", "is a mutator's key", 0, 8);
    expect_one(full + "colour: \"blue\"\n", "colour", "\"blue\"", "is not a key of oabrain.yaml");
    expect_one(full + "7: 1\n", "7", "7: 1", "is not a key of oabrain.yaml", 0, 3);

    expect_one(
        edited(full, "oabrain: 1", "oabrain: 2"), "oabrain", "oabrain: 2", "oabrain must be 1", 0, 9
    );
    expect_one(edited(full, "oabrain: 1", "oabrain: 1.0"), "oabrain", "1.0", "oabrain must be 1");
    expect_one(
        edited(full, "oabrain: 1", "oabrain: \"1\""), "oabrain", "\"1\"", "oabrain must be 1"
    );
    expect_one(edited(full, "oabrain: 1\n", ""), "oabrain", "id:", "oabrain must be 1");

    expect_one(
        edited(full, "id: steady-commander", "id: Steady"), "id", "Steady", "lower-case kebab-case"
    );
    expect_one(
        edited(full, "id: steady-commander\n", ""), "id", "oabrain:", "lower-case kebab-case"
    );
    expect_one(
        edited(full, "id: steady-commander", "id: \"" + std::string(65, 'a') + "\""),
        "id",
        "\"" + std::string(65, 'a') + "\"",
        "lower-case kebab-case"
    );
    expect_one(
        edited(full, "id: steady-commander", "id: \"-ab\""),
        "id",
        "\"-ab\"",
        "lower-case kebab-case"
    );

    expect_one(
        edited(full, "name: \"Steady Commander\"\n", ""), "name", "oabrain:", "name is required"
    );
    expect_one(
        edited(full, "name: \"Steady Commander\"", "name: \"" + std::string(65, 'n') + "\""),
        "name",
        "\"" + std::string(65, 'n') + "\"",
        "no control character"
    );
    expect_one(
        edited(full, R"(name: "Steady Commander")", R"(name: "a\tb")"),
        "name",
        R"("a\tb")",
        "control"
    );

    expect_one(
        edited(full, "description: \"Holds the line.\\nNever rushes.\"\n", ""),
        "description",
        "oabrain:",
        "description is required"
    );
    expect_one(
        edited(full, R"(description: "Holds the line.\nNever rushes.")", R"(description: "\t")"),
        "description",
        R"("\t")",
        "control character"
    );
    expect_one(
        edited(
            full,
            R"(description: "Holds the line.\nNever rushes.")",
            "description: \"" + std::string(2001, 'd') + "\""
        ),
        "description",
        "\"" + std::string(2001, 'd') + "\"",
        "1 to 2000"
    );
    expect_one(
        edited(full, R"(description: "Holds the line.\nNever rushes.")", "description: 1"),
        "description",
        "description: 1",
        "must be a string",
        0,
        13
    );

    expect_one(
        edited(full, "version: \"2.0\"", "version: 2"), "version", "version: 2", "in quotes", 0, 9
    );
    expect_one(edited(full, "version: \"2.0\"", "version: two"), "version", "two", "in quotes");
    expect_one(
        edited(full, "version: \"2.0\"", "version: \"2.0.0.1\""),
        "version",
        "\"2.0.0.1\"",
        "mutator version"
    );
    expect_one(
        edited(full, "version: \"2.0\"\n", ""), "version", "oabrain:", "version is required"
    );
    expect_one(
        edited(full, "version: \"2.0\"", "version: [1]"),
        "version",
        "[1]",
        "version must be a string"
    );

    expect_one(
        edited(
            full,
            "author:\n  name: \"A. Maker\"\n  email: \"maker@example.com\"",
            "author: \"A. Maker\""
        ),
        "author",
        "\"A. Maker\"",
        "must be an object"
    );
    expect_one(
        edited(full, "email: \"maker@example.com\"", "email: \"maker\""),
        "author.email",
        "\"maker\"",
        "e-mail"
    );
    expect_one(
        edited(full, "author:\n  name: \"A. Maker\"\n  email: \"maker@example.com\"\n", ""),
        "author",
        "oabrain:",
        "author is required"
    );

    expect_one(
        edited(full, "homepage: \"https://example.com/steady\"", "homepage: \"not-a-url\""),
        "homepage",
        "\"not-a-url\"",
        "http"
    );
    expect_one(
        edited(full, "engine: \">= 0.8.0\"", "engine: \"soon\""),
        "requires.engine",
        "\"soon\"",
        "comparison"
    );

    const std::string five = R"(tags: ["classic", "economy", "rush", "turtle", "air"])";
    expect_one(
        edited(full, R"(tags: ["classic", "economy"])", five),
        "tags",
        R"(["classic", "economy", "rush", "turtle", "air"])",
        "at most 4"
    );
    expect_one(
        edited(full, R"(tags: ["classic", "economy"])", R"(tags: ["aim"])"),
        "tags[0]",
        "\"aim\"",
        "must be one of classic"
    );
    expect_one(
        edited(full, "tags: [\"classic\", \"economy\"]\n", ""),
        "tags",
        "oabrain:",
        "tags is required"
    );
    expect_one(
        edited(full, R"(tags: ["classic", "economy"])", R"(tags: ["Aim"])"),
        "tags[0]",
        "\"Aim\"",
        "kebab-case"
    );
    expect_one(
        edited(full, R"(tags: ["classic", "economy"])", "tags: []"),
        "tags",
        "tags: []",
        "leave the key out",
        0,
        6
    );

    expect_one(
        edited(full, "works-with: [\"ta-3.1c\", \"ridge >= 28\"]\n", ""),
        "works-with",
        "oabrain:",
        "works-with is required"
    );
    expect_one(
        edited(
            full, R"(works-with: ["ta-3.1c", "ridge >= 28"])", R"(works-with: ["ridge", "ridge"])"
        ),
        "works-with[1]",
        "\"ridge\"",
        "repeats ridge",
        1
    );
    expect_one(
        edited(full, R"(works-with: ["ta-3.1c", "ridge >= 28"])", R"(works-with: ["*", "ridge"])"),
        "works-with[0]",
        "\"*\"",
        "only entry"
    );

    expect_one(
        edited(full, "icon: \"icon.png\"", "icon: \"badge.png\""),
        "icon",
        "\"badge.png\"",
        "icon must be icon.png"
    );

    expect_one(
        edited(
            full,
            "requires:\n  engine: \">= 0.8.0\"\n  expansions: [\"core-contingency\"]",
            "requires: \"no\""
        ),
        "requires",
        "\"no\"",
        "must be an object"
    );
    expect_one(
        edited(
            full,
            "expansions: [\"core-contingency\"]",
            "expansions: [\"core-contingency\"]\n  mutators: [\"quick-raider\"]"
        ),
        "requires.mutators",
        "[\"quick-raider\"]",
        "mutators is not a key of requires"
    );
    expect_one(
        edited(full, "expansions: [\"core-contingency\"]", "expansions: [\"Core\"]"),
        "requires.expansions[0]",
        "\"Core\"",
        "kebab-case"
    );
    expect_one(
        edited(
            full,
            "expansions: [\"core-contingency\"]",
            "expansions: [\"core-contingency\", \"core-contingency\"]"
        ),
        "requires.expansions[1]",
        "\"core-contingency\"",
        "repeats core-contingency",
        1
    );
    const std::string many = quoted_ids(17, "e");
    expect_one(
        edited(full, "expansions: [\"core-contingency\"]", "expansions: " + many),
        "requires.expansions",
        many,
        "must list 1 to 16"
    );
    expect_one(
        edited(full, "expansions: [\"core-contingency\"]", "expansions: []"),
        "requires.expansions",
        "expansions: []",
        "must list 1 to 16",
        0,
        12
    );
    expect_one(
        edited(full, "expansions: [\"core-contingency\"]", "expansions: \"core-contingency\""),
        "requires.expansions",
        "\"core-contingency\"",
        "must be a list"
    );

    expect_one(
        edited(minimal, "levels:\n  - id: only\n    name: \"Only\"\n    difficulty: medium\n", ""),
        "levels",
        "oabrain:",
        "levels is required"
    );
    expect_one(
        edited(
            minimal,
            "levels:\n  - id: only\n    name: \"Only\"\n    difficulty: medium\n",
            "levels: {}\n"
        ),
        "levels",
        "levels: {}",
        "must be a list",
        0,
        8
    );
    std::string seven = "levels:\n";
    for (int index = 0; index < 7; ++index) {
        seven += "  - id: l" + std::to_string(index) + "\n";
        seven += "    name: \"L" + std::to_string(index) + "\"\n";
        seven += "    difficulty: easy\n";
    }
    expect_one(
        edited(
            minimal, "levels:\n  - id: only\n    name: \"Only\"\n    difficulty: medium\n", seven
        ),
        "levels",
        "  - id: l0",
        "must list 1 to 6",
        0,
        2
    );
    expect_one(
        edited(
            minimal, "  - id: only\n    name: \"Only\"\n    difficulty: medium\n", "  - \"only\"\n"
        ),
        "levels[0]",
        "\"only\"",
        "must be an object"
    );
    expect_one(
        edited(minimal, "    difficulty: medium\n", "    difficulty: medium\n    colour: blue\n"),
        "levels[0].colour",
        "blue",
        "is not a key of levels[0]"
    );
    expect_one(
        edited(minimal, "id: only", "id: Bad"), "levels[0].id", "Bad", "lower-case kebab-case"
    );
    expect_one(
        edited(
            minimal,
            "  - id: only\n    name: \"Only\"\n    difficulty: medium\n",
            "  - id: only\n    name: \"Only\"\n    difficulty: medium\n  - id: only\n    name: "
            "\"Other\"\n    difficulty: easy\n"
        ),
        "levels[1].id",
        "id: only",
        "repeats only",
        1,
        4
    );
    expect_one(
        edited(minimal, "name: \"Only\"", "name: \"a\\tb\""),
        "levels[0].name",
        "\"a\\tb\"",
        "control"
    );
    expect_one(
        edited(full, "difficulty: easy", "difficulty: brutal"),
        "levels[0].difficulty",
        "brutal",
        "must be easy, medium or hard"
    );
    expect_one(
        edited(minimal, "    difficulty: medium\n", ""),
        "levels[0].difficulty",
        "id: only",
        "must be easy, medium or hard"
    );
    expect_one(
        edited(minimal, "    name: \"Only\"\n", ""),
        "levels[0].name",
        "id: only",
        "no control character"
    );

    expect_one(
        edited(full, "fullAwareness: true", "fullAwareness: \"true\""),
        "fullAwareness",
        "\"true\"",
        "must be true or false"
    );
    expect_one(
        edited(full, "fullAwareness: true", "fullAwareness: yes"),
        "fullAwareness",
        "yes",
        "must be true or false"
    );

    expect_one(
        edited(
            full,
            "networks:\n  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n",
            "networks: []\n"
        ),
        "networks",
        "networks: []",
        "must list 1 to 16",
        0,
        10
    );
    expect_one(
        edited(
            full,
            "networks:\n  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n",
            "networks: [\"threat\"]\n"
        ),
        "networks[0]",
        "\"threat\"",
        "must be an object"
    );
    expect_one(
        edited(
            full,
            "networks:\n  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n",
            "networks: {}\n"
        ),
        "networks",
        "networks: {}",
        "must be a list",
        0,
        10
    );
    expect_one(
        edited(full, "id: threat", "id: \"for\""),
        "networks[0].id",
        "\"for\"",
        "is a word Lua reserves"
    );
    expect_one(edited(full, "id: threat", "id: Threat"), "networks[0].id", "Threat", "Lua name");
    expect_one(
        edited(
            full,
            "  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n",
            "  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n  - id: "
            "threat\n    file: \"nets/other.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
        ),
        "networks[1].id",
        "id: threat",
        "repeats threat",
        1,
        4
    );
    expect_one(
        edited(full, "file: \"nets/threat.oann\"", "file: \"nets/Threat.oann\""),
        "networks[0].file",
        "\"nets/Threat.oann\"",
        "lower-case letters"
    );
    expect_one(
        edited(
            full,
            "  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n",
            "  - id: threat\n    file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n  - id: spare\n "
            "   file: \"nets/threat.oann\"\n    sha256: "
            "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
        ),
        "networks[1].file",
        "\"nets/threat.oann\"",
        "repeats nets/threat.oann",
        1
    );
    expect_one(
        edited(
            full,
            "sha256: \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"",
            "sha256: 1"
        ),
        "networks[0].sha256",
        "sha256: 1",
        "64 lower-case hex digits in quotes",
        0,
        8
    );
    expect_one(
        edited(
            full,
            "sha256: \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"",
            "sha256: \"abc\""
        ),
        "networks[0].sha256",
        "\"abc\"",
        "64 lower-case hex digits in quotes"
    );
    expect_one(
        edited(
            full,
            "sha256: \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"",
            "sha256: \"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\""
        ),
        "networks[0].sha256",
        "\"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\"",
        "64 lower-case hex digits in quotes"
    );

    expect_one(
        edited(
            full,
            "options:\n  waves:\n    type: \"number\"\n    label: \"Waves\"\n    min: 1\n    max: "
            "8\n    step: 1\n    default: 3\n  stance:\n    type: \"choice\"\n    label: "
            "\"Stance\"\n    values: [\"hold\", \"push\"]\n    default: \"hold\"\n",
            "options: []\n"
        ),
        "options",
        "options: []",
        "must be an object",
        0,
        9
    );
    expect_one(
        edited(full, "  waves:\n", "  \"for\":\n"),
        "options.for",
        "\"for\"",
        "is a word Lua reserves"
    );
    expect_one(
        edited(full, "step: 1", "step: 0"), "options.waves.step", "step: 0", "must be above 0", 0, 6
    );

    expect_one(
        edited(
            full,
            "strings:\n  de:\n    name: \"Steter Kommandant\"\n    description: \"Hält die "
            "Linie.\"\n    options:\n      waves: \"Wellen\"\n      stance: \"Haltung\"\n    "
            "labels:\n      stance: [\"Halten\", \"Drücken\"]\n    levels:\n      steady: "
            "\"Stetig\"\n      bold: \"Kühn\"\n",
            "strings: []\n"
        ),
        "strings",
        "strings: []",
        "must be an object",
        0,
        9
    );
    expect_one(
        edited(full, "bold: \"Kühn\"", "brutal: \"Kühn\""),
        "strings.de.levels.brutal",
        "\"Kühn\"",
        "is not a level of this brain"
    );
    expect_one(
        edited(full, "steady: \"Stetig\"", "steady: \"" + std::string(65, 's') + "\""),
        "strings.de.levels.steady",
        "\"" + std::string(65, 's') + "\"",
        "1 to 64"
    );
    expect_one(
        edited(
            full,
            "    levels:\n      steady: \"Stetig\"\n      bold: \"Kühn\"\n",
            "    levels: \"Stetig\"\n"
        ),
        "strings.de.levels",
        "\"Stetig\"",
        "must be an object"
    );

    expect_one(
        edited(full, "entry: \"main.lua\"", "entry: \"lua/brain.lua\""),
        "entry",
        "\"lua/brain.lua\"",
        "package's top"
    );
    expect_one(
        edited(full, "entry: \"main.lua\"", "entry: \"Brain.lua\""),
        "entry",
        "\"Brain.lua\"",
        "lower-case letters"
    );
    expect_one(
        edited(full, "entry: \"main.lua\"", "entry: \"main.LUA\""),
        "entry",
        "\"main.LUA\"",
        "lower-case letters"
    );

    std::vector<Problem> problems;
    Manifest manifest;
    manifest.id = "sentinel";
    OA_CHECK(
        !brain::read_manifest(as_bytes("oabrain: 1\n\tid: steady-commander\n"), manifest, problems)
    );
    OA_CHECK(problems.size() == 1 && problems[0].key == "document");
    OA_CHECK(problems[0].line == 2 && problems[0].column == 1);
    OA_CHECK(problems[0].message.find("tab") != std::string::npos);
    OA_CHECK(manifest.id == "sentinel");
    problems.clear();
    OA_CHECK(!brain::read_manifest(as_bytes("- item\n"), manifest, problems));
    OA_CHECK(problems.size() == 1 && problems[0].key == "document");
    OA_CHECK(problems[0].message.find("mapping") != std::string::npos);
}

/// The summary stops on a UTF-8 boundary.
void test_summary() {
    Manifest manifest;
    manifest.description = std::string(200, 'a');
    OA_CHECK(brain::summary_of(manifest).size() == 200);
    manifest.description = std::string(201, 'a');
    OA_CHECK(brain::summary_of(manifest).size() == 200);
    manifest.description = std::string(199, 'a') + "\xc3\xa9";
    OA_CHECK(brain::summary_of(manifest).size() == 199);
    OA_CHECK(brain::summary_of(manifest) == std::string(199, 'a'));
    manifest.description = std::string(198, 'a') + "\xe4\xbd\xa0";
    OA_CHECK(brain::summary_of(manifest) == std::string(198, 'a'));
    manifest.description = std::string(200, 'a') + "\nrest";
    OA_CHECK(brain::summary_of(manifest).size() == 200);
    OA_CHECK(brain::summary_of(manifest).find('\n') == std::string::npos);
}

/// Each file role, and the paths a brain may not hold.
void test_files() {
    Manifest manifest;
    manifest.entry = "main.lua";
    manifest.icon = std::string{brain::icon_file};
    manifest.networks.push_back(Network{"threat", "nets/threat.oann", std::string(64, 'a')});
    const auto role = [&](std::string_view path) { return brain::file_role(path, manifest); };
    OA_CHECK(role("oabrain.yaml") == FileRole::manifest);
    OA_CHECK(role("OABRAIN.YAML") == FileRole::manifest);
    OA_CHECK(role("main.lua") == FileRole::entry);
    OA_CHECK(role("lua/a.lua") == FileRole::module);
    OA_CHECK(role("lua/a/b.lua") == FileRole::module);
    OA_CHECK(role("lua/a/b/c.lua") == FileRole::module);
    OA_CHECK(role("nets/threat.oann") == FileRole::network);
    OA_CHECK(role("data/plans.csv") == FileRole::data);
    OA_CHECK(role("data/waves.tdf") == FileRole::data);
    OA_CHECK(role("data/notes.json") == FileRole::data);
    OA_CHECK(role("data/hints.txt") == FileRole::data);
    OA_CHECK(role("icon.png") == FileRole::icon);
    OA_CHECK(role("README.md") == FileRole::readme);
    OA_CHECK(role("readme.MD") == FileRole::readme);
    OA_CHECK(role("LICENSE") == FileRole::licence);
    OA_CHECK(role("License") == FileRole::licence);

    OA_CHECK(role("lua/Tasks/x.lua") == FileRole::outside);
    OA_CHECK(role("lua/a/b.txt") == FileRole::outside);
    OA_CHECK(role("nets/spare.oann") == FileRole::outside);
    OA_CHECK(role("data/a/b.csv") == FileRole::outside);
    OA_CHECK(role("data/x.bin") == FileRole::outside);
    OA_CHECK(role("licence") == FileRole::outside);
    OA_CHECK(role("brain.lua") == FileRole::outside);
    OA_CHECK(role("data/Bad.csv") == FileRole::outside);
    OA_CHECK(role("lua/a-b.lua") == FileRole::outside);
    OA_CHECK(role("docs/README.md") == FileRole::outside);

    const std::string stem(64, 'a');
    OA_CHECK(role("lua/" + stem + ".lua") == FileRole::module);
    OA_CHECK(role("lua/" + stem + "b.lua") == FileRole::outside);
    OA_CHECK(role("data/" + stem + ".csv") == FileRole::data);
    OA_CHECK(role("data/" + std::string(65, 'b') + ".txt") == FileRole::outside);

    Manifest bare;
    bare.entry = "main.lua";
    OA_CHECK(brain::file_role("icon.png", bare) == FileRole::outside);
    OA_CHECK(brain::file_role("brain.lua", bare) == FileRole::outside);
}

} // namespace

int main() {
    test_full();
    test_minimal();
    test_refusals();
    test_summary();
    test_files();
    return oa::test::check_exit_status();
}
