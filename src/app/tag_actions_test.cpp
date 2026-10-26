// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Category words from made-up unit lines, including one empty bad-target name.
#include "tag_actions.hpp"

#include "oa/test/check.hpp"

#include <string>
#include <utility>
#include <vector>

namespace {

using oa::app::TagWord;

void register_line(
    oa::data::defs::CategoryRegistry& registry, oa::UnitDef& unit, const char* line
) {
    OA_CHECK(oa::data::defs::register_unit_categories(&registry, &unit, line));
}

void the_fixture_words() {
    oa::data::defs::CategoryRegistry registry{};
    oa::data::defs::category_registry_init(&registry);
    oa::UnitDef unit{};
    unit.type_id = 1;
    register_line(registry, unit, "ARM KBOT LEVEL1 CONSTR NOWEAPON NOTAIR NOTSUB CTRL_B");
    register_line(registry, unit, "ARM commander LEVEL10 WEAPON NOTAIR NOTSUB CTRL_C");
    register_line(registry, unit, "ARM PLANT CTRL_F LEVEL1 NOWEAPON NOTAIR NOTSUB");
    register_line(registry, unit, "RIDGE_SCOUT CTRL_W");
    register_line(registry, unit, "ctrl_w CTRL_S A.B");
    OA_CHECK(oa::data::defs::category_registry_ref(&registry, "VTOL") != 0);

    std::vector<std::string> skipped;
    const std::vector<TagWord> words = oa::app::tag_words(registry, skipped);
    oa::data::defs::category_registry_clear(&registry);

    const std::pair<const char*, const char*> expect[] = {
        {"ARM", "select.tag-arm"},
        {"commander", "select.tag-commander"},
        {"CONSTR", "select.tag-constr"},
        {"CTRL_B", "select.builders"},
        {"CTRL_C", "select.commander"},
        {"CTRL_F", "select.factories"},
        {"CTRL_S", "select.tag-ctrl-s"},
        {"CTRL_W", "select.category-w"},
        {"KBOT", "select.tag-kbot"},
        {"LEVEL1", "select.tag-level1"},
        {"LEVEL10", "select.tag-level10"},
        {"NOTAIR", "select.tag-notair"},
        {"NOTSUB", "select.tag-notsub"},
        {"NOWEAPON", "select.tag-noweapon"},
        {"PLANT", "select.tag-plant"},
        {"RIDGE_SCOUT", "select.tag-ridge-scout"},
        {"WEAPON", "select.tag-weapon"},
    };
    OA_CHECK(words.size() == sizeof expect / sizeof expect[0]);
    for (std::size_t index = 0; index < words.size() && index < sizeof expect / sizeof expect[0];
         ++index) {
        OA_CHECK(words[index].word == expect[index].first);
        OA_CHECK(words[index].action == expect[index].second);
    }
    OA_CHECK(skipped.size() == 1);
    if (skipped.size() == 1)
        OA_CHECK(skipped[0] == "A.B");
}

} // namespace

int main() {
    the_fixture_words();
    return oa::test::check_exit_status();
}
