// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words a planet and the two win icons speak. The facts line a planet
// shows stays the line the map already writes. One battle is singular.
#include "oa/boneyards/console/status_widget.hpp"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_text(const char* got, const char* want, const char* what) {
    if (got == nullptr || want == nullptr || std::strcmp(got, want) != 0) {
        std::fprintf(stderr, "FAIL: %s, got \"%s\"\n", what, got != nullptr ? got : "(null)");
        ++g_failures;
    }
}

oa::boneyards::console::PlanetValuePatterns english_patterns() {
    using namespace oa::boneyards::console;
    PlanetValuePatterns patterns;
    patterns.battles = k_planet_value;
    patterns.battle = k_planet_battle;
    patterns.battles_chosen = k_planet_value_chosen;
    patterns.battle_chosen = k_planet_battle_chosen;
    return patterns;
}

void check_planet(
    const char* facts,
    const char* name,
    bool chosen,
    const char* owner_word,
    bool parsed,
    const char* want_label,
    const char* want_value,
    const char* what
) {
    using oa::boneyards::console::planet_access_words;
    const oa::boneyards::console::PlanetValuePatterns patterns = english_patterns();
    char label[48];
    char value[160];
    std::memset(label, 'x', sizeof label);
    std::memset(value, 'x', sizeof value);
    label[sizeof label - 1] = '\0';
    value[sizeof value - 1] = '\0';
    const bool got = planet_access_words(
        label, sizeof label, value, sizeof value, facts, name, chosen, patterns, owner_word
    );
    check(got == parsed, what);
    if (!parsed) {
        check_text(label, "", what);
        check_text(value, "", what);
        return;
    }
    check_text(label, want_label, what);
    check_text(value, want_value, what);
}

} // namespace

int main() {
    using oa::boneyards::console::planet_owner_word;
    using oa::boneyards::console::planet_parse_facts;
    using oa::boneyards::console::summary_win_icon_label;
    using oa::boneyards::console::summary_win_icon_labels;

    check(std::strcmp(planet_owner_word('A'), "Arm") == 0, "Arm");
    check(std::strcmp(planet_owner_word('c'), "Core") == 0, "Core");
    check(std::strcmp(planet_owner_word('I'), "contested") == 0, "contested");
    check(std::strcmp(planet_owner_word('?'), "unknown") == 0, "unknown owner");

    int planet_id = 0;
    char owner = '\0';
    int battles = 0;
    int forming = 0;
    check(
        planet_parse_facts(
            "id=1801 owner=I battles=2 forming=1", &planet_id, &owner, &battles, &forming
        ),
        "facts parse"
    );
    check(planet_id == 1801 && owner == 'I' && battles == 2 && forming == 1, "facts fields");
    check(
        !planet_parse_facts("id=1801 owner=I", nullptr, nullptr, nullptr, nullptr), "short facts"
    );
    check(!planet_parse_facts(nullptr, nullptr, nullptr, nullptr, nullptr), "null facts");

    check_planet(
        "id=1801 owner=I battles=2 forming=1",
        "Slag",
        false,
        "contested",
        true,
        "Slag",
        "contested, 2 battles, 1 forming",
        "planet value"
    );
    check_planet(
        "id=1801 owner=I battles=2 forming=1",
        "Slag",
        true,
        "contested",
        true,
        "Slag",
        "contested, 2 battles, 1 forming, chosen",
        "chosen planet value"
    );
    check_planet(
        "id=12 owner=A battles=1 forming=1",
        "Last Waypoint",
        false,
        "Arm",
        true,
        "Last Waypoint",
        "Arm, 1 battle, 1 forming",
        "one battle"
    );
    check_planet(
        "id=12 owner=A battles=1 forming=1",
        "Last Waypoint",
        true,
        "Arm",
        true,
        "Last Waypoint",
        "Arm, 1 battle, 1 forming, chosen",
        "one chosen battle"
    );
    check_planet(
        "id=4 owner=A battles=0 forming=0",
        "Empyrrean",
        true,
        "Arm",
        true,
        "Empyrrean",
        "Arm, 0 battles, 0 forming, chosen",
        "Arm planet"
    );
    check_planet(
        "id=9 owner=C battles=3 forming=0",
        "Empyrrean",
        false,
        "Core",
        true,
        "Empyrrean",
        "Core, 3 battles, 0 forming",
        "Core planet"
    );
    check_planet(
        "id=1 owner=I battles=1 forming=4",
        nullptr,
        false,
        "contested",
        true,
        "",
        "contested, 1 battle, 4 forming",
        "missing name"
    );
    check_planet(
        "not a planet", "Slag", false, "contested", false, "", "", "facts that do not parse"
    );
    check_planet(
        "id=1801 owner=I battles=2 forming=1 extra",
        "Slag",
        false,
        "contested",
        false,
        "",
        "",
        "facts with a tail"
    );

    // The higher icon is Arm's. Both keep the name SmallArmIcon; the words
    // tell them apart. 0x9d sits above 0xaa, as the summary lays them out.
    const int32_t pair[] = {0x9d, 0xaa};
    const char* pair_labels[2]{};
    summary_win_icon_labels(pair, 2, pair_labels);
    check_text(pair_labels[0], "Arm wins", "the higher icon is Arm");
    check_text(pair_labels[1], "Core wins", "the lower icon is Core");

    const int32_t alone[] = {0x9d};
    const char* alone_label[1]{};
    summary_win_icon_labels(alone, 1, alone_label);
    check_text(alone_label[0], "Arm wins", "a single icon is Arm");

    check_text(summary_win_icon_label(true), "Arm wins", "Arm wins");
    check_text(summary_win_icon_label(false), "Core wins", "Core wins");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failed\n", g_failures);
        return 1;
    }
    return 0;
}
