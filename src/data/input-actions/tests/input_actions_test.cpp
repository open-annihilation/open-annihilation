// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The generated action table: its count and order, the rows the catalogue
// calls out, the one fixed chord, and the exclusive conditions.

#include "oa/data/input_actions/actions.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

namespace actions = oa::data::input_actions;

// The catalogue's ranges, written out. select.category-<x> is a family, and
// watch.player-view-<n> is the one action watch.player-view.
constexpr size_t catalogue_count = 186;

/// Compares a chord list with the chords the test names.
///
/// @param got the table's chords
/// @param want the expected chords
/// @return true when they match, in order
template <size_t Count>
bool same_chords(std::span<const std::string_view> got, const std::string_view (&want)[Count]) {
    if (got.size() != Count)
        return false;
    for (size_t index = 0; index < Count; ++index) {
        if (got[index] != want[index])
            return false;
    }
    return true;
}

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context
/// @return its bit
uint32_t context_bit(std::string_view name) {
    const auto rows = actions::contexts();
    for (size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The bit of a condition, or 0 when the table has no such name.
///
/// @param name the condition
/// @return its bit
uint32_t condition_bit(std::string_view name) {
    const auto rows = actions::conditions();
    for (size_t index = 0; index < rows.size(); ++index) {
        if (rows[index] == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Reports whether every set bit of a mask lies inside a table.
///
/// @param mask the bits
/// @param width the table's length
/// @return true when no bit falls outside it
bool bits_fit(uint32_t mask, size_t width) {
    for (uint32_t index = 0; index < 32; ++index) {
        if ((mask & (uint32_t{1} << index)) == 0)
            continue;
        if (index >= width)
            return false;
    }
    return true;
}

} // namespace

int main() {
    const auto rows = actions::actions();
    OA_CHECK(rows.size() == catalogue_count);
    for (size_t index = 1; index < rows.size(); ++index)
        OA_CHECK(rows[index - 1].id < rows[index].id);

    const uint32_t play = context_bit("play");
    OA_CHECK(play != 0);
    const actions::Action* attack = actions::find("order.attack");
    OA_CHECK(attack != nullptr);
    if (attack != nullptr) {
        const std::string_view want[]{"A"};
        OA_CHECK(same_chords(attack->defaults, want));
        OA_CHECK(attack->by == actions::By::letter);
        OA_CHECK(attack->gadget == "ATTACK");
        OA_CHECK(attack->area == "orders");
        OA_CHECK(attack->context == play);
        OA_CHECK(attack->words.name == "Attack");
        OA_CHECK(attack->words.description == "Arms Attack");
    }

    const actions::Action* group = actions::find("group.select-1");
    OA_CHECK(group != nullptr);
    if (group != nullptr) {
        const std::string_view want[]{"Alt+@Digit1", "Alt+Numpad1"};
        OA_CHECK(same_chords(group->defaults, want));
        OA_CHECK(group->by == actions::By::place);
    }

    const actions::Action* shot = actions::find("game.screenshot");
    OA_CHECK(shot != nullptr);
    if (shot != nullptr) {
        bool saw = false;
        for (const actions::HackChange& change : shot->hacks) {
            if (change.hack != "ui.display-modes")
                continue;
            saw = true;
            OA_CHECK(change.sets_default);
            const std::string_view want[]{"Ctrl+F9", "PrintScreen"};
            OA_CHECK(same_chords(change.defaults, want));
        }
        OA_CHECK(saw);
    }

    const actions::Action* idle = actions::find("select.idle-builders");
    OA_CHECK(idle != nullptr);
    if (idle != nullptr) {
        OA_CHECK(idle->replaces == "select.builders");
        OA_CHECK(idle->defaults.empty());
    }

    const uint32_t no_zoom = condition_bit("no-wheel-zoom");
    OA_CHECK(no_zoom != 0);
    const actions::Action* mega = actions::find("megamap.toggle");
    OA_CHECK(mega != nullptr);
    if (mega != nullptr)
        OA_CHECK((mega->when & no_zoom) != 0);

    const actions::Action* cancel = actions::find("chat.cancel");
    OA_CHECK(cancel != nullptr);
    if (cancel != nullptr) {
        OA_CHECK(cancel->fixed);
        const std::string_view want[]{"Escape"};
        OA_CHECK(same_chords(cancel->defaults, want));
    }

    const actions::Action* settings = actions::find("ui.settings");
    OA_CHECK(settings != nullptr);
    if (settings != nullptr) {
        const std::string_view want[]{"Primary+,"};
        OA_CHECK(same_chords(settings->defaults, want));
        OA_CHECK(!settings->fixed);
    }
    const actions::Action* back = actions::find("game.back");
    OA_CHECK(back != nullptr);
    if (back != nullptr) {
        const std::string_view want[]{"Escape", "Ctrl+Escape"};
        OA_CHECK(same_chords(back->defaults, want));
        OA_CHECK(!back->fixed);
    }
    const actions::Action* quit = actions::find("ui.quit");
    OA_CHECK(quit != nullptr);
    if (quit != nullptr) {
        OA_CHECK(quit->defaults.empty());
        OA_CHECK(!quit->fixed);
    }
    for (const actions::Action& action : rows) {
        if (action.id == "chat.cancel")
            OA_CHECK(action.fixed);
        else
            OA_CHECK(!action.fixed);
        OA_CHECK(action.context != 0);
        OA_CHECK(bits_fit(action.context, actions::contexts().size()));
        OA_CHECK(bits_fit(action.when, actions::conditions().size()));
    }

    const uint32_t single = condition_bit("single-player");
    const uint32_t multi = condition_bit("multiplayer");
    OA_CHECK(single != 0);
    OA_CHECK(multi != 0);
    OA_CHECK(actions::exclusive(single, multi));
    OA_CHECK(!actions::exclusive(condition_bit("selection"), condition_bit("developer")));

    OA_CHECK(actions::find("not.an.action") == nullptr);
    OA_CHECK(actions::families().size() == 2);
    OA_CHECK(actions::contexts().size() > 0);
    OA_CHECK(actions::contexts()[0].name == "global");
    OA_CHECK(actions::contexts()[0].parent == actions::no_parent);
    bool saw_display = false;
    for (const std::string_view hack : actions::hack_ids()) {
        if (hack == "ui.display-modes")
            saw_display = true;
    }
    OA_CHECK(saw_display);

    return oa::test::check_exit_status();
}
