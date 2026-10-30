// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's model: opening a set, prerequisites and their order,
// rows locked by a prerequisite that cannot be had, a loop or a conflict,
// refreshing, options and a number's values, rows, the footer, the hint, the
// turn-off question, and random walks that check the set after every call.
// The mutators are made up.
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace picker = oa::ui::mutator_picker;
namespace lib = oa::ui::library;

/// A made-up mutator that is installed and can be switched on.
///
/// @param id the id
/// @param name the name a player sees
/// @return the mutator
picker::Mutator made(std::string id, std::string name) {
    picker::Mutator mutator;
    mutator.id = std::move(id);
    mutator.name = std::move(name);
    mutator.installed = true;
    return mutator;
}

/// Records that `mutator` needs `id`.
///
/// @param[in,out] mutator the mutator
/// @param id the prerequisite
void need(picker::Mutator& mutator, std::string id) {
    lib::Prerequisite prerequisite;
    prerequisite.id = std::move(id);
    mutator.needs.push_back(std::move(prerequisite));
}

/// Records that `mutator` needs `id` in a version range.
///
/// @param[in,out] mutator the mutator
/// @param id the prerequisite
/// @param range the range as written; empty for any version
/// @param name the prerequisite's name; empty when its record names none
void need_range(picker::Mutator& mutator, std::string id, std::string range, std::string name) {
    lib::Prerequisite prerequisite;
    prerequisite.id = std::move(id);
    prerequisite.range = std::move(range);
    prerequisite.name = std::move(name);
    mutator.needs.push_back(std::move(prerequisite));
}

/// The prerequisite graph of the picker's inputs.
///
/// @param model the picker
/// @return the graph
std::vector<lib::Need> graph_of(const picker::Picker& model) {
    std::vector<lib::Need> graph;
    for (const picker::Mutator& mutator : model.inputs.mutators) {
        lib::Need record;
        record.id = mutator.id;
        for (const lib::Prerequisite& prerequisite : mutator.needs)
            record.needs.push_back(prerequisite.id);
        graph.push_back(std::move(record));
    }
    return graph;
}

/// The On ids, top to bottom.
///
/// @param model the picker
/// @return the ids
std::vector<std::string> ids_of(const picker::Picker& model) {
    std::vector<std::string> ids;
    for (const picker::Chosen& chosen : model.on)
        ids.push_back(chosen.id);
    return ids;
}

/// Checks the On ids.
///
/// @param model the picker
/// @param expected the ids, top to bottom
void check_ids(const picker::Picker& model, const std::vector<std::string>& expected) {
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(ids.size() == expected.size());
    if (ids.size() != expected.size())
        return;
    for (std::size_t index = 0; index < ids.size(); ++index)
        OA_CHECK(ids[index] == expected[index]);
}

/// The row with `id`, or null.
///
/// @param listed the rows
/// @param id the mutator
/// @return the row
const picker::Row* row_of(const std::vector<picker::Row>& listed, std::string_view id) {
    for (const picker::Row& row : listed)
        if (row.id == id)
            return &row;
    return nullptr;
}

/// A choice option.
///
/// @param id the option id
/// @param label the label
/// @param values the values
/// @param captions the captions, in the same order
/// @param fallback the default
/// @return the option
picker::OptionSpec choice_option(
    std::string id,
    std::string label,
    std::vector<std::string> values,
    std::vector<std::string> captions,
    std::string fallback
) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::choice;
    spec.label = std::move(label);
    spec.values = std::move(values);
    spec.captions = std::move(captions);
    spec.fallback = std::move(fallback);
    return spec;
}

/// A toggle. The off value is first.
///
/// @param id the option id
/// @param label the label
/// @param off the off value
/// @param on the on value
/// @return the option
picker::OptionSpec
toggle_option(std::string id, std::string label, std::string off, std::string on) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::toggle;
    spec.label = std::move(label);
    spec.values = {std::move(off), std::move(on)};
    spec.fallback = spec.values.front();
    return spec;
}

/// A number option.
///
/// @param id the option id
/// @param label the label
/// @param least the low end
/// @param most the high end
/// @param step the step
/// @param fallback the default
/// @return the option
picker::OptionSpec number_option(
    std::string id,
    std::string label,
    std::string least,
    std::string most,
    std::string step,
    std::string fallback
) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::number;
    spec.label = std::move(label);
    spec.least = std::move(least);
    spec.most = std::move(most);
    spec.step = std::move(step);
    spec.fallback = std::move(fallback);
    return spec;
}

picker::Chosen chosen_id(std::string id) {
    picker::Chosen chosen;
    chosen.id = std::move(id);
    return chosen;
}

/// An empty remembered set switches nothing on and shows no last-set hint.
void open_empty() {
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.mutators.push_back(made("ridge-walls", "Ridge Walls"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(model.on.empty());
    OA_CHECK(!model.from_last_set);
    OA_CHECK(picker::hint(model).empty());
    OA_CHECK(picker::ready(model));
    const picker::Footer foot = picker::footer(model);
    OA_CHECK(foot.summary == "None on");
    OA_CHECK(foot.chip.empty());
    OA_CHECK(foot.chip_mark == lib::Mark::none);
    OA_CHECK(model.selected && *model.selected == "fast-builders");
}

/// A full set keeps its order and its allowed option values.
void open_full_set() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4", "8"}, {"1×", "4×", "8×"}, "1")
    );
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.setup = picker::Setup::skirmish;
    inputs.mutators.push_back(builders);
    picker::Chosen chosen = chosen_id("fast-builders");
    chosen.options.emplace_back("build-speed", "4");
    inputs.last_set.push_back(std::move(chosen));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(model.from_last_set);
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.on[0].options.size() == 1);
    OA_CHECK(model.on[0].options[0].second == "4");
    OA_CHECK(
        picker::hint(model) ==
        "Your last skirmish set, ready to use. Cancel plays without mutators."
    );
    OA_CHECK(model.selected && *model.selected == "fast-builders");
}

/// A removed mutator is named, a removed option is dropped, and a value the
/// mutator no longer allows takes the default.
void open_drops_what_went() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.options.push_back(number_option("income", "Income", "0", "10", "0.5", "1"));
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    picker::Chosen gone = chosen_id("not-in-the-list");
    picker::Chosen chosen = chosen_id("fast-builders");
    chosen.options.emplace_back("removed-option", "yes");
    chosen.options.emplace_back("build-speed", "nope");
    chosen.options.emplace_back("income", "99");
    inputs.last_set.push_back(std::move(gone));
    inputs.last_set.push_back(std::move(chosen));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 1);
    OA_CHECK(model.left_out[0] == "not-in-the-list");
    OA_CHECK(model.on[0].options.size() == 2);
    OA_CHECK(model.on[0].options[0].first == "build-speed");
    OA_CHECK(model.on[0].options[0].second == "1");
    OA_CHECK(model.on[0].options[1].first == "income");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(picker::hint(model).find("not-in-the-list") != std::string::npos);
}

/// A set stored with a dependant above its prerequisite is mended, and the
/// prerequisite is not recorded as brought: the set already named it.
void open_mends_order() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.last_set.push_back(chosen_id("gatehouse-keep"));
    inputs.last_set.push_back(chosen_id("gatehouse"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(model.came_with.empty());
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// A prerequisite the set does not name is brought above what needs it.
void open_brings_missing() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    need(gatehouse, "grounded");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "grounded");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    need(keep, "ridge-walls");
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(keep);
    inputs.last_set.push_back(chosen_id("gatehouse-keep"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    check_ids(model, {"grounded", "gatehouse", "ridge-walls", "gatehouse-keep"});
    OA_CHECK(model.came_with.size() == 3);
    OA_CHECK(model.came_with[0].prerequisite == "grounded");
    OA_CHECK(model.came_with[0].dependant == "gatehouse-keep");
    OA_CHECK(model.came_with[1].prerequisite == "gatehouse");
    OA_CHECK(model.came_with[2].prerequisite == "ridge-walls");
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
    const picker::Footer foot = picker::footer(model);
    OA_CHECK(foot.summary == "4 on · Grounded came with Gatehouse Keep");
}

/// Past the cap, the end of the list is left out.
void open_trims_the_cap() {
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    for (int index = 0; index < 33; ++index) {
        const std::string id = std::string("m") + (index < 10 ? "0" : "") + std::to_string(index);
        inputs.mutators.push_back(made(id, id));
        inputs.last_set.push_back(chosen_id(id));
    }
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(model.on.size() == picker::most_on);
    OA_CHECK(model.on.front().id == "m00");
    OA_CHECK(model.on.back().id == "m31");
    OA_CHECK(model.left_out.size() == 1);
    OA_CHECK(model.left_out[0] == "m32");
}

/// Switching Gatehouse Keep on brings Gatehouse above it.
void switch_on_brings() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const picker::SwitchResult outcome = picker::switch_on(model, "gatehouse-keep");
    OA_CHECK(outcome.changed);
    OA_CHECK(outcome.refused.empty());
    OA_CHECK(outcome.brought.size() == 1);
    OA_CHECK(outcome.brought[0] == "gatehouse");
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(model.came_with.size() == 1);
    OA_CHECK(model.came_with[0].prerequisite == "gatehouse");
    OA_CHECK(model.came_with[0].dependant == "gatehouse-keep");
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// Grounded is refused while Quiet Seas, which conflicts with it, is on, and
/// a last set holding both opens with the later one off.
void switch_on_conflict() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator seas = made("quiet-seas", "Quiet Seas");
    seas.conflicts.push_back("grounded");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(seas);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "quiet-seas").changed);
    const std::vector<std::string> before = ids_of(model);
    const picker::SwitchResult outcome = picker::switch_on(model, "grounded");
    OA_CHECK(!outcome.changed);
    OA_CHECK(outcome.refused == "Cannot run beside Quiet Seas");
    check_ids(model, before);
    OA_CHECK(picker::ready(model));
    inputs.last_set.push_back(chosen_id("quiet-seas"));
    inputs.last_set.push_back(chosen_id("grounded"));
    picker::open(model, inputs);
    check_ids(model, {"quiet-seas"});
    OA_CHECK(model.left_out.size() == 1 && model.left_out[0] == "Grounded");
    OA_CHECK(picker::ready(model));
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* seas_row = row_of(listed, "quiet-seas");
    const picker::Row* grounded_row = row_of(listed, "grounded");
    OA_CHECK(seas_row != nullptr && seas_row->on && !seas_row->locked);
    OA_CHECK(grounded_row != nullptr && !grounded_row->on && grounded_row->locked);
    OA_CHECK(grounded_row != nullptr && grounded_row->tone == picker::Tone::bad);
    OA_CHECK(grounded_row != nullptr && grounded_row->line == "Cannot run beside Quiet Seas");
}

/// An unavailable mutator is refused with its reason.
void switch_on_unavailable() {
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    walls.unavailable = "needs Ridge's weapons";
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const picker::SwitchResult outcome = picker::switch_on(model, "ridge-walls");
    OA_CHECK(!outcome.changed);
    OA_CHECK(outcome.refused.find("needs Ridge's weapons") != std::string::npos);
    OA_CHECK(model.on.empty());
}

/// Two mutators that need each other are refused with the Library's words,
/// naming the ids in the loop, and their rows are locked. So is a mutator
/// that needs one of them. A last set naming one opens without it.
void switch_on_cycle() {
    picker::Mutator left = made("loop-a", "Loop A");
    picker::Mutator right = made("loop-b", "Loop B");
    need(left, "loop-b");
    need(right, "loop-a");
    picker::Mutator above = made("loop-c", "Loop C");
    need(above, "loop-a");
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(left);
    inputs.mutators.push_back(right);
    inputs.mutators.push_back(above);
    picker::Picker model;
    picker::open(model, inputs);
    // The loop's ids, in the order the mutator's own closure meets them.
    const std::string line = "Not compatible with this mod: These mutators need each other: ";
    const picker::SwitchResult outcome = picker::switch_on(model, "loop-a");
    OA_CHECK(!outcome.changed);
    OA_CHECK(model.on.empty());
    OA_CHECK(outcome.refused == line + "loop-a and loop-b");
    OA_CHECK(picker::switch_on(model, "loop-c").refused == line + "loop-a and loop-b");
    OA_CHECK(!picker::drop(model, "loop-b", picker::Drop{true, 0}));
    OA_CHECK(model.on.empty());
    const std::vector<picker::Row> listed = picker::rows(model);
    const std::pair<std::string_view, std::string> expected[] = {
        {"loop-a", line + "loop-a and loop-b"},
        {"loop-b", line + "loop-b and loop-a"},
        {"loop-c", line + "loop-a and loop-b"},
    };
    for (const auto& [id, text] : expected) {
        const picker::Row* row = row_of(listed, id);
        OA_CHECK(row != nullptr && row->locked && row->tone == picker::Tone::bad);
        OA_CHECK(row != nullptr && row->line == text);
    }
    inputs.last_set.push_back(chosen_id("loop-a"));
    picker::open(model, std::move(inputs));
    OA_CHECK(model.on.empty());
    OA_CHECK(model.left_out.size() == 1 && model.left_out[0] == "Loop A");
    OA_CHECK(picker::ready(model));
}

/// A read-only picker still opens on the set, and then refuses every change.
void read_only_refuses() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.read_only = true;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    inputs.last_set.push_back(chosen_id("fast-builders"));
    picker::Picker model;
    picker::open(model, inputs);
    check_ids(model, {"fast-builders"});
    OA_CHECK(picker::hint(model) == "The host's mutators. Only the host can change them.");
    const picker::SwitchResult switched = picker::switch_on(model, "gatehouse");
    OA_CHECK(!switched.changed);
    OA_CHECK(!switched.refused.empty());
    OA_CHECK(!picker::switch_off(model, "fast-builders"));
    OA_CHECK(!picker::move_to(model, "fast-builders", 0));
    OA_CHECK(!picker::drop(model, "gatehouse", picker::Drop{true, 0}));
    OA_CHECK(!picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(model.on[0].options[0].second == "1");
    check_ids(model, {"fast-builders"});

    inputs.read_only = false;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_off(model, "fast-builders") == std::nullopt);
    OA_CHECK(model.on.empty());
    picker::open(model, inputs);
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    model.inputs.mutators.push_back(keep);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    model.inputs.read_only = true;
    picker::confirm_turn_off(model);
    OA_CHECK(model.asking);
    OA_CHECK(model.on.size() == 3);
    picker::cancel_turn_off(model);
    OA_CHECK(!model.asking);
    OA_CHECK(model.on.size() == 3);
}

/// Switching a prerequisite off asks, and nothing changes until the answer.
void turn_off_asks() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    const std::optional<picker::TurnOff> asked = picker::switch_off(model, "gatehouse");
    OA_CHECK(asked);
    OA_CHECK(model.asking);
    OA_CHECK(asked->id == "gatehouse");
    OA_CHECK(asked->dependants.size() == 1);
    OA_CHECK(asked->dependants[0] == "gatehouse-keep");
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    picker::cancel_turn_off(model);
    OA_CHECK(!model.asking);
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(!model.came_with.empty());
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    picker::confirm_turn_off(model);
    OA_CHECK(!model.asking);
    OA_CHECK(model.on.empty());
    OA_CHECK(model.came_with.empty());
}

/// A chain of three can be moved only inside each mutator's band.
void chain_moves() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    need(gatehouse, "grounded");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    check_ids(model, {"grounded", "gatehouse", "gatehouse-keep"});
    OA_CHECK(!picker::move_up(model, "grounded"));
    OA_CHECK(!picker::move_down(model, "grounded"));
    OA_CHECK(!picker::move_up(model, "gatehouse"));
    OA_CHECK(!picker::move_down(model, "gatehouse"));
    OA_CHECK(!picker::move_up(model, "gatehouse-keep"));
    OA_CHECK(!picker::move_down(model, "gatehouse-keep"));
    const picker::Band band = picker::allowed_places(model, "gatehouse");
    OA_CHECK(band.first == 1);
    OA_CHECK(band.last == 1);
    OA_CHECK(picker::allowed_places(model, "fast-missing").first == 0);
    for (std::size_t place = 0; place <= model.on.size(); ++place) {
        std::ignore = picker::move_to(model, "gatehouse", place);
        std::ignore = picker::drop(model, "gatehouse", picker::Drop{true, place});
        const std::vector<lib::Need> graph = graph_of(model);
        const std::vector<std::string> ids = ids_of(model);
        OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
    }
    check_ids(model, {"grounded", "gatehouse", "gatehouse-keep"});
    OA_CHECK(picker::drop(model, "gatehouse", picker::Drop{false, 0}));
    OA_CHECK(model.asking);
    picker::cancel_turn_off(model);
    OA_CHECK(!picker::drop(model, "gatehouse-keep", picker::Drop{false, 0}));
    OA_CHECK(!model.asking);
    check_ids(model, {"grounded", "gatehouse"});
    OA_CHECK(!picker::drop(model, "gatehouse-keep", picker::Drop{true, 0}));
    check_ids(model, {"grounded", "gatehouse", "gatehouse-keep"});
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// A diamond lets the two middle mutators swap, and nothing else.
void diamond_moves() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    need(gatehouse, "grounded");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "grounded");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    need(keep, "ridge-walls");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    check_ids(model, {"grounded", "gatehouse", "ridge-walls", "gatehouse-keep"});
    OA_CHECK(picker::move_down(model, "gatehouse"));
    check_ids(model, {"grounded", "ridge-walls", "gatehouse", "gatehouse-keep"});
    OA_CHECK(!picker::move_down(model, "gatehouse"));
    OA_CHECK(picker::move_up(model, "gatehouse"));
    check_ids(model, {"grounded", "gatehouse", "ridge-walls", "gatehouse-keep"});
    OA_CHECK(!picker::move_up(model, "grounded"));
    OA_CHECK(!picker::move_down(model, "gatehouse-keep"));
    for (std::size_t place = 0; place <= model.on.size(); ++place) {
        std::ignore = picker::move_to(model, "ridge-walls", place);
        std::ignore = picker::drop(model, "gatehouse", picker::Drop{true, place});
        const std::vector<lib::Need> graph = graph_of(model);
        const std::vector<std::string> ids = ids_of(model);
        OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
    }
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    OA_CHECK(oa::ui::library::prerequisites_above(graph, ids));
}

/// Mutators with no prerequisites move up and down until the ends.
void free_moves() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.mutators.push_back(made("gatehouse", "Gatehouse"));
    inputs.mutators.push_back(made("ridge-walls", "Ridge Walls"));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    OA_CHECK(!picker::move_up(model, "fast-builders"));
    OA_CHECK(!picker::move_down(model, "ridge-walls"));
    OA_CHECK(picker::move_up(model, "gatehouse"));
    check_ids(model, {"gatehouse", "fast-builders", "ridge-walls"});
    OA_CHECK(picker::move_down(model, "fast-builders"));
    check_ids(model, {"gatehouse", "ridge-walls", "fast-builders"});
}

/// Each option type accepts a legal value and refuses the rest.
/// Switching off and on again restores the defaults.
void options_accept_and_refuse() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(toggle_option("aircraft", "Aircraft", "off", "on"));
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4", "8"}, {"1×", "4×", "8×"}, "1")
    );
    builders.options.push_back(number_option("income", "Income", "0", "10", "0.5", "1"));
    builders.brings = true;
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.clash_default = picker::Clash::replace;
    inputs.mutators.push_back(std::move(builders));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(model.on[0].options[0].second == "off");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(model.on[0].options[2].second == "1");
    OA_CHECK(model.on[0].clash && *model.on[0].clash == picker::Clash::replace);
    OA_CHECK(picker::set_option(model, "fast-builders", "aircraft", "on"));
    OA_CHECK(picker::set_option(model, "fast-builders", "aircraft", "on"));
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "0"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "0.5"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "10"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "+4"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "00.5"));
    OA_CHECK(picker::set_option(model, "fast-builders", "income", "1e1"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "0.3"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "10.1"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "-1"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "income", "4×"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "build-speed", "2"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "aircraft", "maybe"));
    OA_CHECK(!picker::set_option(model, "fast-builders", "missing", "1"));
    OA_CHECK(model.on[0].options[1].second == "4");
    OA_CHECK(model.on[0].options[2].second == "10");
    OA_CHECK(picker::set_clash(model, "fast-builders", picker::Clash::add));
    OA_CHECK(model.on[0].clash && *model.on[0].clash == picker::Clash::add);
    OA_CHECK(!picker::switch_off(model, "fast-builders"));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(model.on[0].options[0].second == "off");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(model.on[0].options[2].second == "1");
    OA_CHECK(model.on[0].clash && *model.on[0].clash == picker::Clash::replace);

    picker::Mutator fine = made("ridge-walls", "Ridge Walls");
    fine.options.push_back(number_option("gap", "Gap", "0.1", "1", "0.2", "0.1"));
    model.inputs.mutators.push_back(std::move(fine));
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    OA_CHECK(picker::set_option(model, "ridge-walls", "gap", "0.5"));
    OA_CHECK(!picker::set_option(model, "ridge-walls", "gap", "0.4"));
    OA_CHECK(!picker::set_option(model, "not-on", "gap", "0.5"));
    OA_CHECK(!picker::set_clash(model, "ridge-walls", picker::Clash::add));
}

/// The thirty-third mutator is refused, prerequisites included.
void switch_on_cap() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    for (int index = 0; index < 33; ++index) {
        const std::string id = std::string("m") + (index < 10 ? "0" : "") + std::to_string(index);
        inputs.mutators.push_back(made(id, id));
    }
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "m00");
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    for (int index = 0; index < 32; ++index) {
        const std::string id = std::string("m") + (index < 10 ? "0" : "") + std::to_string(index);
        OA_CHECK(picker::switch_on(model, id).changed);
    }
    const picker::SwitchResult extra = picker::switch_on(model, "m32");
    OA_CHECK(!extra.changed);
    OA_CHECK(extra.refused.find("32") != std::string::npos);
    OA_CHECK(model.on.size() == 32);
}

/// Every row line in the order the model chooses them.
void row_lines() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.summary = "Builds faster";
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.tags = {"economy"};
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.summary = "A gate";
    gatehouse.tags = {"gameplay", "custom-tag"};
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    keep.installed = false;
    keep.summary = "The keep";
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    walls.installed = false;
    walls.unavailable = "needs Ridge's weapons";
    walls.summary = "Walls";
    picker::Mutator queued = made("queued-walls", "Queued Walls");
    queued.installed = false;
    queued.summary = "Waiting walls";
    lib::QueueItem item;
    item.phase = lib::QueueItem::Phase::waiting;
    item.name = "Queued Walls";
    queued.queued = item;
    picker::Mutator plain = made("quiet-field", "Quiet Field");
    plain.installed = false;
    plain.summary = "A field";
    plain.author = "Ridge";
    picker::Mutator bad = made("bad-fit", "Bad Fit");
    bad.summary = "Does not fit";
    bad.verdict.state = lib::Verdict::State::incompatible;
    bad.verdict.reason = "the mod has no spot for it";
    picker::Mutator failed = made("stopped-fit", "Stopped Fit");
    failed.summary = "The check stopped";
    failed.verdict.state = lib::Verdict::State::failed;
    failed.verdict.reason = "the check stopped";
    picker::Mutator no_change = made("idle-fit", "Idle Fit");
    no_change.summary = "Changes nothing";
    no_change.verdict.state = lib::Verdict::State::no_effect;
    picker::Mutator seas = made("quiet-seas", "Quiet Seas");
    seas.summary = "Seas";
    seas.conflicts.push_back("grounded");
    seas.verdict.state = lib::Verdict::State::compatible;

    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(queued);
    inputs.mutators.push_back(plain);
    inputs.mutators.push_back(bad);
    inputs.mutators.push_back(failed);
    inputs.mutators.push_back(no_change);
    inputs.mutators.push_back(seas);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_on(model, "bad-fit").changed);
    OA_CHECK(picker::switch_on(model, "stopped-fit").changed);
    OA_CHECK(picker::switch_on(model, "idle-fit").changed);
    OA_CHECK(picker::switch_on(model, "quiet-seas").changed);

    const std::vector<picker::Row> listed = picker::rows(model);
    OA_CHECK(listed.size() >= 6);
    const picker::Row* builders_row = row_of(listed, "fast-builders");
    OA_CHECK(builders_row != nullptr && builders_row->on && builders_row->number == 1);
    OA_CHECK(builders_row->line == "Build speed: 4×");
    OA_CHECK(builders_row->tone == picker::Tone::hint);
    OA_CHECK(!builders_row->locked && !builders_row->to_download);

    const picker::Row* gatehouse_row = row_of(listed, "gatehouse");
    OA_CHECK(gatehouse_row != nullptr && gatehouse_row->on);
    OA_CHECK(gatehouse_row->line == "Needed by Gatehouse Keep");
    OA_CHECK(gatehouse_row->tone == picker::Tone::warn);
    OA_CHECK(gatehouse_row->number == 2);

    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && keep_row->on && keep_row->to_download);

    const picker::Row* bad_row = row_of(listed, "bad-fit");
    OA_CHECK(bad_row != nullptr && bad_row->tone == picker::Tone::bad && !bad_row->locked);
    OA_CHECK(bad_row->line == "Not compatible with this mod: the mod has no spot for it");
    const picker::Row* failed_row = row_of(listed, "stopped-fit");
    OA_CHECK(failed_row != nullptr && failed_row->tone == picker::Tone::bad && !failed_row->locked);
    OA_CHECK(failed_row->line == "Stopped with this mod: the check stopped");
    const picker::Row* idle_row = row_of(listed, "idle-fit");
    OA_CHECK(idle_row != nullptr && idle_row->tone == picker::Tone::warn);
    OA_CHECK(idle_row->line == "No effect with this mod");

    const picker::Row* walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && !walls_row->on && walls_row->number == 0);
    OA_CHECK(walls_row->locked && walls_row->tone == picker::Tone::bad);
    OA_CHECK(walls_row->line == "Not compatible with this mod: needs Ridge's weapons");
    OA_CHECK(!walls_row->to_download);

    const picker::Row* queued_row = row_of(listed, "queued-walls");
    OA_CHECK(queued_row != nullptr && queued_row->tone == picker::Tone::hint);
    OA_CHECK(queued_row->line == "Waiting to download");
    OA_CHECK(!queued_row->to_download);
    const picker::Row* field_row = row_of(listed, "quiet-field");
    OA_CHECK(
        field_row != nullptr && field_row->to_download && field_row->tone == picker::Tone::hint
    );
    OA_CHECK(field_row->line == "Not installed: switching it on downloads it");

    std::size_t previous = 0;
    bool seen_off = false;
    for (const picker::Row& row : listed) {
        if (row.on) {
            OA_CHECK(!seen_off);
            OA_CHECK(row.number == previous + 1);
            previous = row.number;
        } else {
            seen_off = true;
            OA_CHECK(row.number == 0);
        }
    }

    model.filter = picker::Filter::installed;
    const std::vector<picker::Row> installed = picker::rows(model);
    OA_CHECK(row_of(installed, "fast-builders") != nullptr);
    OA_CHECK(row_of(installed, "quiet-seas") != nullptr);
    OA_CHECK(row_of(installed, "quiet-field") == nullptr);
    model.filter = picker::Filter::downloads;
    model.query = "ridge";
    model.tag = "economy";
    const std::vector<picker::Row> hidden = picker::rows(model);
    OA_CHECK(row_of(hidden, "fast-builders") != nullptr);
    OA_CHECK(row_of(hidden, "gatehouse") != nullptr);
    OA_CHECK(row_of(hidden, "quiet-field") == nullptr);

    model.filter = picker::Filter::all;
    model.tag.clear();
    model.query = "ridge field";
    OA_CHECK(row_of(picker::rows(model), "quiet-field") != nullptr);
    model.query = "no-such-word";
    OA_CHECK(row_of(picker::rows(model), "quiet-field") == nullptr);
    model.query.clear();

    const std::size_t all = picker::filter_count(model, picker::Filter::all);
    const std::size_t installed_count = picker::filter_count(model, picker::Filter::installed);
    const std::size_t downloads = picker::filter_count(model, picker::Filter::downloads);
    model.tag = "economy";
    model.query = "ridge";
    OA_CHECK(picker::filter_count(model, picker::Filter::all) == all);
    OA_CHECK(picker::filter_count(model, picker::Filter::installed) == installed_count);
    OA_CHECK(picker::filter_count(model, picker::Filter::downloads) == downloads);
    OA_CHECK(all == model.inputs.mutators.size());
    OA_CHECK(downloads == 4);
    model.tag.clear();
    model.query.clear();
    model.filter = picker::Filter::all;
    const std::vector<std::string> tags = picker::tags_shown(model);
    OA_CHECK(tags.size() == 3);
    OA_CHECK(tags[0] == "economy");
    OA_CHECK(tags[1] == "gameplay");
    OA_CHECK(tags[2] == "custom-tag");
}

/// The chip in German: the base game's own sentence, and a mod's.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view chip_in_german(void*, std::string_view english) {
    if (english == picker::words::no_mod)
        return "Kein Mod";
    if (english == picker::words::verdict_checked_base)
        return "Ohne Mod geprüft ✓";
    if (english == picker::words::verdict_checked)
        return "Geprüft mit {mod} ✓";
    if (english == picker::words::verdict_failed_base)
        return "{name} wurde ohne Mod abgebrochen";
    return english;
}

/// With no mod played the chip is the base game's own sentence.
void base_game_chip_cases() {
    picker::Inputs inputs;
    inputs.mod_name = "Kein Mod";
    inputs.text = lib::TextHooks{nullptr, chip_in_german};
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    model.inputs.mutators[0].verdict.state = lib::Verdict::State::compatible;
    OA_CHECK(picker::footer(model).chip == "Ohne Mod geprüft ✓");
    model.inputs.mutators[0].verdict.state = lib::Verdict::State::failed;
    OA_CHECK(picker::footer(model).chip == "Fast Builders wurde ohne Mod abgebrochen");
    model.inputs.mod_name = "Ridge";
    model.inputs.mutators[0].verdict.state = lib::Verdict::State::compatible;
    OA_CHECK(picker::footer(model).chip == "Geprüft mit Ridge ✓");
    model.inputs.mod_name = "No Mod";
    model.inputs.text = {};
    OA_CHECK(picker::footer(model).chip == "Checked with No Mod ✓");
}

/// The footer's chip follows the On verdicts, and the summary names the first brought.
void footer_cases() {
    picker::Mutator first = made("fast-builders", "Fast Builders");
    picker::Mutator second = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(first);
    inputs.mutators.push_back(second);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    picker::Footer foot = picker::footer(model);
    OA_CHECK(foot.chip.empty());
    OA_CHECK(foot.summary == "2 on");

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::checking;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::incompatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Checking with No Mod…");
    OA_CHECK(foot.chip_mark == lib::Mark::none);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::unchecked;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip.empty());

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::incompatible;
    model.inputs.mutators[0].verdict.reason = "no";
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::failed;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Not compatible with No Mod: Fast Builders");
    OA_CHECK(foot.chip_mark == lib::Mark::bad);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::failed;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::incompatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Fast Builders stopped with No Mod");
    OA_CHECK(foot.chip_mark == lib::Mark::bad);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::no_effect;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "No effect with No Mod: Fast Builders");
    OA_CHECK(foot.chip_mark == lib::Mark::warn);

    model.inputs.mutators[0].verdict.state = lib::Verdict::State::compatible;
    model.inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    foot = picker::footer(model);
    OA_CHECK(foot.chip == "Checked with No Mod ✓");
    OA_CHECK(foot.chip_mark == lib::Mark::good);
}

/// The hint for each setup, the campaign's two sentences, and a guest's view.
void hint_cases() {
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.last_set.push_back(chosen_id("fast-builders"));
    inputs.last_set.push_back(chosen_id("missing-one"));
    picker::Picker model;

    inputs.setup = picker::Setup::skirmish;
    picker::open(model, inputs);
    OA_CHECK(picker::hint(model).find("last skirmish set") != std::string::npos);
    OA_CHECK(picker::hint(model).find("missing-one") != std::string::npos);

    inputs.setup = picker::Setup::multiplayer;
    picker::open(model, inputs);
    OA_CHECK(picker::hint(model).find("last multiplayer set") != std::string::npos);

    inputs.setup = picker::Setup::campaign;
    picker::open(model, inputs);
    const std::string campaign = picker::hint(model);
    OA_CHECK(campaign.find("last campaign set") != std::string::npos);
    OA_CHECK(campaign.find("every mission of this campaign") != std::string::npos);
    const std::size_t first = campaign.find("last campaign set");
    const std::size_t second = campaign.find("every mission");
    OA_CHECK(first != std::string::npos && second != std::string::npos && first < second);

    inputs.remembered = false;
    inputs.last_set.clear();
    inputs.setup = picker::Setup::campaign;
    picker::open(model, inputs);
    OA_CHECK(
        picker::hint(model) ==
        "The set holds for every mission of this campaign, and is saved with it."
    );

    inputs.read_only = true;
    inputs.remembered = true;
    inputs.last_set.push_back(chosen_id("fast-builders"));
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::hint(model) == "The host's mutators. Only the host can change them.");
}

/// The short texts a setup's line is written from.
void short_texts() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.options.push_back(toggle_option("aircraft", "Aircraft", "off", "on"));
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    OA_CHECK(picker::set_option(model, "fast-builders", "aircraft", "on"));
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::item_text(model, model.on[0]) == "Fast Builders 4×");
    const std::vector<picker::Chosen> chosen = picker::result(model);
    OA_CHECK(picker::list_text(model, chosen) == "Fast Builders 4× · Gatehouse");
    OA_CHECK(picker::ready(model));
}

/// The question names the dependants, and its buttons are Cancel and Turn off.
void question_words() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const oa::ui::kit::Question idle = picker::turn_off_question(model);
    OA_CHECK(idle.title.empty());
    OA_CHECK(idle.buttons.empty());
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    const oa::ui::kit::Question one = picker::turn_off_question(model);
    OA_CHECK(one.title == "Turn off Gatehouse?");
    OA_CHECK(one.paragraphs.size() == 2);
    OA_CHECK(one.paragraphs[0].text == "Gatehouse Keep turns off too: it needs Gatehouse.");
    OA_CHECK(one.paragraphs[1].text.find("Gatehouse Keep") != std::string::npos);
    OA_CHECK(one.paragraphs[1].text.find("comes back with it") != std::string::npos);
    OA_CHECK(one.buttons.size() == 2);
    OA_CHECK(one.buttons[0].caption == "Cancel");
    OA_CHECK(!one.buttons[0].accent);
    OA_CHECK(one.buttons[0].id == "cancel");
    OA_CHECK(one.buttons[1].caption == "Turn off");
    OA_CHECK(one.buttons[1].accent);
    OA_CHECK(one.buttons[1].id == "turn-off");
    OA_CHECK(one.marked == 0);
    OA_CHECK(one.cancel_button == 0);
    OA_CHECK(one.primary_button == 1);
    picker::cancel_turn_off(model);

    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    const oa::ui::kit::Question many = picker::turn_off_question(model);
    OA_CHECK(many.paragraphs[0].text.find("turn off too") != std::string::npos);
    OA_CHECK(many.paragraphs[0].text.find("Gatehouse Keep") != std::string::npos);
    OA_CHECK(many.paragraphs[0].text.find("Ridge Walls") != std::string::npos);
    OA_CHECK(many.paragraphs[1].text.find("They all stay installed") != std::string::npos);
}

/// A lookup replaces the English pattern before its places are filled.
void translated_row() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.text.shown = [](void*, std::string_view english) -> std::string_view {
        if (english == picker::words::line_needed_by)
            return "Requis par {name}";
        return english;
    };
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    const std::vector<picker::Row> shown_rows = picker::rows(model);
    const picker::Row* row = row_of(shown_rows, "gatehouse");
    OA_CHECK(row != nullptr);
    OA_CHECK(row != nullptr && row->line == "Requis par Gatehouse Keep");
}

/// Refresh keeps the order, the options and the filters, and drops a mutator
/// the new inputs do not hold.
void refresh_keeps() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    builders.verdict.state = lib::Verdict::State::unchecked;
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::set_option(model, "fast-builders", "build-speed", "4"));
    model.filter = picker::Filter::installed;
    model.query = "fast";
    model.selected = "fast-builders";
    inputs.mutators[0].verdict.state = lib::Verdict::State::compatible;
    inputs.mutators[1].verdict.state = lib::Verdict::State::compatible;
    picker::refresh(model, inputs);
    check_ids(model, {"gatehouse", "fast-builders"});
    OA_CHECK(model.on[1].options[0].second == "4");
    OA_CHECK(model.filter == picker::Filter::installed);
    OA_CHECK(model.query == "fast");
    OA_CHECK(model.selected && *model.selected == "fast-builders");
    OA_CHECK(picker::footer(model).chip == "Checked with No Mod ✓");

    inputs.mutators.pop_back();
    picker::refresh(model, std::move(inputs));
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 1);
    OA_CHECK(model.left_out[0] == "gatehouse");
}

/// Not being installed does not refuse a switch, and an empty On list is ready.
void not_installed_switches_on() {
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.installed = false;
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.mutators.push_back(std::move(builders));
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::ready(model));
    const picker::SwitchResult outcome = picker::switch_on(model, "fast-builders");
    OA_CHECK(outcome.changed);
    OA_CHECK(outcome.refused.empty());
    OA_CHECK(!picker::ready(model));
}

/// A prerequisite the inputs do not hold locks the row and refuses the
/// switch in the Library's words, with its range, or without one.
void locked_missing_prerequisite() {
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need_range(keep, "gatehouse", ">= 4.0", "Gatehouse");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "ridge-tower");
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    picker::Picker model;
    picker::open(model, inputs);
    const std::string keep_line = "Not compatible with this mod: Needs Gatehouse " +
                                  lib::range_words(">= 4.0") + ", which no registry offers";
    const std::string walls_line =
        "Not compatible with this mod: Needs ridge-tower, which no registry offers";
    OA_CHECK(lib::range_words(">= 4.0") == "4.0 or later");
    OA_CHECK(walls_line.find(" ,") == std::string::npos);
    picker::SwitchResult outcome = picker::switch_on(model, "gatehouse-keep");
    OA_CHECK(!outcome.changed);
    OA_CHECK(outcome.refused == keep_line);
    outcome = picker::switch_on(model, "ridge-walls");
    OA_CHECK(!outcome.changed);
    OA_CHECK(outcome.refused == walls_line);
    OA_CHECK(!picker::drop(model, "gatehouse-keep", picker::Drop{true, 0}));
    OA_CHECK(model.on.empty());
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && keep_row->locked && keep_row->tone == picker::Tone::bad);
    OA_CHECK(keep_row != nullptr && keep_row->line == keep_line);
    const picker::Row* walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && walls_row->locked && walls_row->line == walls_line);
    const picker::Row* builders_row = row_of(listed, "fast-builders");
    OA_CHECK(builders_row != nullptr && !builders_row->locked);

    inputs.last_set.push_back(chosen_id("gatehouse-keep"));
    inputs.last_set.push_back(chosen_id("fast-builders"));
    picker::open(model, std::move(inputs));
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 1 && model.left_out[0] == "Gatehouse Keep");
    OA_CHECK(picker::ready(model));
    OA_CHECK(
        picker::hint(model) == "Your last skirmish set, ready to use. Cancel plays without "
                               "mutators. Left out of your last set: Gatehouse Keep"
    );
}

/// Gatehouse 3.9 does not meet Gatehouse Keep's ">= 4.0": the row is locked
/// and the switch refused, through a dependant too. Gatehouse 4.1 does.
void locked_version_range() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.version = "3.9";
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need_range(keep, "gatehouse", ">= 4.0", "");
    picker::Mutator tower = made("gatehouse-tower", "Gatehouse Tower");
    need(tower, "gatehouse-keep");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(tower);
    picker::Picker model;
    picker::open(model, inputs);
    const std::string line = "Not compatible with this mod: Needs Gatehouse " +
                             lib::range_words(">= 4.0") + ", which no registry offers";
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").refused == line);
    OA_CHECK(picker::switch_on(model, "gatehouse-tower").refused == line);
    OA_CHECK(model.on.empty());
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").refused == line);
    check_ids(model, {"gatehouse"});
    OA_CHECK(picker::ready(model));
    std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && keep_row->locked && keep_row->line == line);
    const picker::Row* tower_row = row_of(listed, "gatehouse-tower");
    OA_CHECK(tower_row != nullptr && tower_row->locked && tower_row->line == line);

    // Two comparisons, and a range that does not read.
    model.inputs.mutators[0].version = "5.1";
    model.inputs.mutators[1].needs[0].range = ">= 4.0, < 5.0";
    OA_CHECK(
        picker::switch_on(model, "gatehouse-keep").refused ==
        "Not compatible with this mod: Needs Gatehouse " + lib::range_words(">= 4.0, < 5.0") +
            ", which no registry offers"
    );
    model.inputs.mutators[1].needs[0].range = "about 4";
    OA_CHECK(
        picker::switch_on(model, "gatehouse-keep").refused ==
        "Not compatible with this mod: Needs Gatehouse " + lib::range_words("about 4") +
            ", which no registry offers"
    );

    inputs.mutators[0].version = "4.1";
    picker::refresh(model, inputs);
    const picker::SwitchResult outcome = picker::switch_on(model, "gatehouse-tower");
    OA_CHECK(outcome.changed && outcome.refused.empty());
    check_ids(model, {"gatehouse", "gatehouse-keep", "gatehouse-tower"});
    OA_CHECK(picker::ready(model));
    listed = picker::rows(model);
    for (const picker::Row& row : listed)
        OA_CHECK(!row.locked);
}

/// A prerequisite that is unavailable locks its dependant with its reason.
void locked_unavailable_prerequisite() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.unavailable = "needs Ridge's gates";
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    const std::string line = "Not compatible with this mod: needs Ridge's gates";
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").refused == line);
    OA_CHECK(model.on.empty());
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && keep_row->locked && keep_row->line == line);
}

/// A prerequisite the switch would bring that conflicts with an On mutator
/// refuses it naming the On one; so does a mutator that conflicts with its
/// own prerequisite, or two prerequisites that conflict with each other.
void locked_brought_conflict() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.conflicts.push_back("quiet-seas");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Mutator seas = made("quiet-seas", "Quiet Seas");
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "grounded");
    walls.conflicts.push_back("grounded");
    picker::Mutator tower = made("ridge-tower", "Ridge Tower");
    need(tower, "grounded");
    need(tower, "fast-builders");
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.conflicts.push_back("grounded");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    for (const picker::Mutator& mutator : {gatehouse, keep, seas, grounded, walls, tower, builders})
        inputs.mutators.push_back(mutator);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "quiet-seas").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").refused == "Cannot run beside Quiet Seas");
    OA_CHECK(picker::switch_on(model, "gatehouse").refused == "Cannot run beside Quiet Seas");
    OA_CHECK(picker::switch_on(model, "ridge-walls").refused == "Cannot run beside Grounded");
    OA_CHECK(picker::switch_on(model, "ridge-tower").refused == "Cannot run beside Fast Builders");
    check_ids(model, {"quiet-seas"});
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && keep_row->locked && keep_row->tone == picker::Tone::bad);
    OA_CHECK(keep_row != nullptr && keep_row->line == "Cannot run beside Quiet Seas");
    const picker::Row* walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && walls_row->locked);
    OA_CHECK(walls_row != nullptr && walls_row->line == "Cannot run beside Grounded");
    const picker::Row* grounded_row = row_of(listed, "grounded");
    OA_CHECK(grounded_row != nullptr && !grounded_row->locked);
}

/// A conflict between two On mutators does not refuse an unrelated one, and
/// a last set holding a conflicting pair opens with the later one off.
void stale_conflict() {
    picker::Mutator grounded = made("grounded", "Grounded");
    picker::Mutator seas = made("quiet-seas", "Quiet Seas");
    seas.conflicts.push_back("grounded");
    picker::Inputs inputs;
    inputs.remembered = true;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(grounded);
    inputs.mutators.push_back(seas);
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.mutators.push_back(made("gatehouse", "Gatehouse"));
    inputs.last_set.push_back(chosen_id("grounded"));
    inputs.last_set.push_back(chosen_id("quiet-seas"));
    inputs.last_set.push_back(chosen_id("fast-builders"));
    picker::Picker model;
    picker::open(model, inputs);
    check_ids(model, {"grounded", "fast-builders"});
    OA_CHECK(model.left_out.size() == 1 && model.left_out[0] == "Quiet Seas");
    OA_CHECK(
        picker::hint(model) == "Your last skirmish set, ready to use. Cancel plays without "
                               "mutators. Left out of your last set: Quiet Seas"
    );
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* seas_row = row_of(listed, "quiet-seas");
    OA_CHECK(seas_row != nullptr && !seas_row->on && seas_row->locked);
    OA_CHECK(seas_row != nullptr && seas_row->line == "Cannot run beside Grounded");
    OA_CHECK(picker::ready(model));
    OA_CHECK(picker::switch_on(model, "gatehouse").changed);

    // A pair already on, however it got there, is not the next mutator's.
    model.on.clear();
    model.on.push_back(chosen_id("grounded"));
    model.on.push_back(chosen_id("quiet-seas"));
    OA_CHECK(!picker::ready(model));
    const picker::SwitchResult outcome = picker::switch_on(model, "gatehouse");
    OA_CHECK(outcome.changed && outcome.refused.empty());
}

/// Gatehouse leaves the inputs: Gatehouse Keep goes off with it, and says why.
void refresh_prerequisite_goes() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.version = "4.1";
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need_range(keep, "gatehouse", ">= 4.0", "Gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    check_ids(model, {"gatehouse", "gatehouse-keep", "fast-builders"});
    inputs.mutators.erase(inputs.mutators.begin());
    picker::refresh(model, inputs);
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 2);
    OA_CHECK(model.left_out.size() == 2 && model.left_out[0] == "gatehouse");
    OA_CHECK(model.left_out.size() == 2 && model.left_out[1] == "Gatehouse Keep");
    OA_CHECK(model.came_with.empty());
    OA_CHECK(picker::ready(model));
    OA_CHECK(oa::ui::library::prerequisites_above(graph_of(model), ids_of(model)));
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && !keep_row->on && keep_row->locked);
    OA_CHECK(
        keep_row != nullptr && keep_row->line == "Not compatible with this mod: Needs Gatehouse "
                                                 "4.0 or later, which no registry offers"
    );
    OA_CHECK(picker::hint(model) == "Left out of your last set: gatehouse and Gatehouse Keep");
}

/// Gatehouse falls to 3.9: Gatehouse Keep and what needs it go off, and
/// Gatehouse stays.
void refresh_version_falls() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.version = "4.1";
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need_range(keep, "gatehouse", ">= 4.0", "Gatehouse");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "gatehouse-keep");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    check_ids(model, {"gatehouse", "gatehouse-keep", "ridge-walls"});
    OA_CHECK(picker::ready(model));
    inputs.mutators[0].version = "3.9";
    picker::refresh(model, inputs);
    check_ids(model, {"gatehouse"});
    OA_CHECK(model.left_out.size() == 2);
    OA_CHECK(model.left_out.size() == 2 && model.left_out[0] == "Gatehouse Keep");
    OA_CHECK(model.left_out.size() == 2 && model.left_out[1] == "Ridge Walls");
    OA_CHECK(picker::ready(model));
    const std::string line = "Not compatible with this mod: Needs Gatehouse " +
                             lib::range_words(">= 4.0") + ", which no registry offers";
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(keep_row != nullptr && keep_row->locked && keep_row->line == line);
    const picker::Row* walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && walls_row->locked && walls_row->line == line);
}

/// New needs make a loop: the mutators in it, and one that needs them, go off.
void refresh_loop() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("loop-a", "Loop A"));
    inputs.mutators.push_back(made("loop-b", "Loop B"));
    inputs.mutators.push_back(made("loop-c", "Loop C"));
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    picker::Picker model;
    picker::open(model, inputs);
    for (const std::string_view id : {"loop-a", "loop-b", "loop-c", "fast-builders"})
        OA_CHECK(picker::switch_on(model, id).changed);
    need(inputs.mutators[0], "loop-b");
    need(inputs.mutators[1], "loop-a");
    need(inputs.mutators[2], "loop-a");
    picker::refresh(model, inputs);
    check_ids(model, {"fast-builders"});
    OA_CHECK(model.left_out.size() == 3);
    OA_CHECK(oa::ui::library::prerequisites_above(graph_of(model), ids_of(model)));
    OA_CHECK(picker::ready(model));
    // Each names the loop in its own closure's order.
    const std::string line = "Not compatible with this mod: These mutators need each other: ";
    const std::vector<picker::Row> listed = picker::rows(model);
    const std::pair<std::string_view, std::string> expected[] = {
        {"loop-a", line + "loop-a and loop-b"},
        {"loop-b", line + "loop-b and loop-a"},
        {"loop-c", line + "loop-a and loop-b"},
    };
    for (const auto& [id, text] : expected) {
        const picker::Row* row = row_of(listed, id);
        OA_CHECK(row != nullptr && !row->on && row->locked && row->line == text);
    }
}

/// A mutator gains a prerequisite: brought above it when it can be had,
/// otherwise the mutator goes off, with the reason on its row.
void refresh_new_need() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("gatehouse", "Gatehouse"));
    inputs.mutators.push_back(made("gatehouse-keep", "Gatehouse Keep"));
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    picker::Picker model;
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    need(inputs.mutators[1], "gatehouse");
    picker::refresh(model, inputs);
    check_ids(model, {"gatehouse", "gatehouse-keep"});
    OA_CHECK(model.came_with.size() == 1);
    OA_CHECK(model.came_with.size() == 1 && model.came_with[0].prerequisite == "gatehouse");
    OA_CHECK(model.came_with.size() == 1 && model.came_with[0].dependant == "gatehouse-keep");
    OA_CHECK(model.left_out.empty());
    OA_CHECK(picker::ready(model));
    OA_CHECK(picker::footer(model).summary == "2 on · Gatehouse came with Gatehouse Keep");

    // A prerequisite no input holds.
    need(inputs.mutators[1], "gatehouse-tower");
    picker::refresh(model, inputs);
    check_ids(model, {"gatehouse"});
    OA_CHECK(model.left_out.size() == 1 && model.left_out[0] == "Gatehouse Keep");
    OA_CHECK(model.came_with.empty());
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* keep_row = row_of(listed, "gatehouse-keep");
    OA_CHECK(
        keep_row != nullptr && keep_row->locked &&
        keep_row->line ==
            "Not compatible with this mod: Needs gatehouse-tower, which no registry offers"
    );

    // A prerequisite that conflicts with a mutator above.
    inputs.mutators[1].needs.clear();
    picker::refresh(model, inputs);
    OA_CHECK(picker::switch_off(model, "gatehouse") == std::nullopt);
    OA_CHECK(picker::switch_on(model, "fast-builders").changed);
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    check_ids(model, {"fast-builders", "gatehouse-keep"});
    need(inputs.mutators[1], "gatehouse");
    inputs.mutators[0].conflicts.push_back("fast-builders");
    picker::refresh(model, inputs);
    check_ids(model, {"fast-builders"});
    OA_CHECK(picker::ready(model));
    const std::vector<picker::Row> after = picker::rows(model);
    const picker::Row* gone = row_of(after, "gatehouse-keep");
    OA_CHECK(gone != nullptr && gone->locked && gone->line == "Cannot run beside Fast Builders");
}

/// A refresh that makes two On mutators conflict keeps the earlier one, and
/// one that makes a mutator unavailable switches it off.
void refresh_conflict_and_unavailable() {
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(made("grounded", "Grounded"));
    inputs.mutators.push_back(made("fast-builders", "Fast Builders"));
    inputs.mutators.push_back(made("quiet-seas", "Quiet Seas"));
    picker::Picker model;
    picker::open(model, inputs);
    for (const std::string_view id : {"grounded", "fast-builders", "quiet-seas"})
        OA_CHECK(picker::switch_on(model, id).changed);
    inputs.mutators[2].conflicts.push_back("grounded");
    picker::refresh(model, inputs);
    check_ids(model, {"grounded", "fast-builders"});
    OA_CHECK(model.left_out.size() == 1 && model.left_out[0] == "Quiet Seas");
    OA_CHECK(picker::ready(model));
    inputs.mutators[1].unavailable = "needs Ridge";
    picker::refresh(model, inputs);
    check_ids(model, {"grounded"});
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* builders_row = row_of(listed, "fast-builders");
    OA_CHECK(builders_row != nullptr && builders_row->locked);
    OA_CHECK(
        builders_row != nullptr && builders_row->line == "Not compatible with this mod: needs Ridge"
    );
    OA_CHECK(picker::ready(model));
}

/// A dependant switched on while the question is open goes off at TURN OFF
/// too, and the question names it.
void confirm_recomputes() {
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    need(keep, "gatehouse");
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    need(walls, "gatehouse");
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.mutators.push_back(gatehouse);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    const std::optional<picker::TurnOff> asked = picker::switch_off(model, "gatehouse");
    OA_CHECK(asked && asked->dependants.size() == 1);
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    check_ids(model, {"gatehouse", "gatehouse-keep", "ridge-walls"});
    const oa::ui::kit::Question question = picker::turn_off_question(model);
    OA_CHECK(!question.paragraphs.empty());
    OA_CHECK(
        !question.paragraphs.empty() &&
        question.paragraphs[0].text ==
            "Gatehouse Keep and Ridge Walls turn off too: they need Gatehouse."
    );
    picker::confirm_turn_off(model);
    OA_CHECK(model.on.empty());
    OA_CHECK(!model.asking);
    OA_CHECK(model.came_with.empty());

    // The dependant went off before the answer: only the prerequisite goes.
    OA_CHECK(picker::switch_on(model, "gatehouse-keep").changed);
    OA_CHECK(picker::switch_off(model, "gatehouse"));
    OA_CHECK(picker::switch_off(model, "gatehouse-keep") == std::nullopt);
    OA_CHECK(model.asking && model.asking->id == "gatehouse");
    picker::confirm_turn_off(model);
    OA_CHECK(model.on.empty());
}

/// The values a number offers, and the spellings a number is kept in.
void number_values_and_spellings() {
    const picker::OptionSpec income = number_option("income", "Income", "0", "10", "0.5", "1");
    const std::vector<std::string> stops = picker::number_values(income);
    OA_CHECK(stops.size() == 21);
    OA_CHECK(stops.size() == 21 && stops[0] == "0" && stops[1] == "0.5" && stops[2] == "1");
    OA_CHECK(stops.size() == 21 && stops[19] == "9.5" && stops[20] == "10");
    const picker::OptionSpec speed =
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1");
    OA_CHECK(picker::number_values(speed).empty());
    OA_CHECK(picker::number_values(toggle_option("aircraft", "Aircraft", "off", "on")).empty());
    const std::vector<std::string> wide =
        picker::number_values(number_option("count", "Count", "0", "1000", "1", "0"));
    OA_CHECK(wide.size() == oa::formats::oamut::most_number_steps + 1);
    OA_CHECK(!wide.empty() && wide.back() == "1000");
    OA_CHECK(picker::number_values(number_option("count", "Count", "0", "1001", "1", "0")).empty());
    OA_CHECK(picker::number_values(number_option("bad", "Bad", "0", "10", "0", "0")).empty());
    OA_CHECK(picker::number_values(number_option("bad", "Bad", "0", "10", "-1", "0")).empty());
    OA_CHECK(picker::number_values(number_option("bad", "Bad", "10", "0", "1", "0")).empty());
    OA_CHECK(picker::number_values(number_option("bad", "Bad", "+1", "10", "1", "1")).empty());
    OA_CHECK(picker::number_values(number_option("bad", "Bad", "0", "10", "1e400", "0")).empty());

    const auto values_are = [](const picker::OptionSpec& spec,
                               const std::vector<std::string>& expected) {
        const std::vector<std::string> values = picker::number_values(spec);
        OA_CHECK(values == expected);
        if (values != expected) {
            for (const std::string& value : values)
                std::fprintf(stderr, "  offered %s\n", value.c_str());
        }
    };
    values_are(
        number_option("gap", "Gap", "0.1", "1", "0.2", "0.1"), {"0.1", "0.3", "0.5", "0.7", "0.9"}
    );
    values_are(
        number_option("tilt", "Tilt", "-1", "1", "0.5", "0"), {"-1", "-0.5", "0", "0.5", "1"}
    );
    values_are(
        number_option("health", "Health", "1000", "4000", "1000", "2000"),
        {"1000", "2000", "3000", "4000"}
    );
    values_are(
        number_option("far", "Far", "0", "3e21", "1e21", "0"), {"0", "1e+21", "2e+21", "3e+21"}
    );
    values_are(
        number_option("near", "Near", "1e-7", "3e-7", "1e-7", "1e-7"), {"1e-7", "2e-7", "3e-7"}
    );
    values_are(
        number_option("tiny", "Tiny", "0.000001", "0.000003", "0.000001", "0.000001"),
        {"0.000001", "0.000002", "0.000003"}
    );
    // 512 steps of 1953125 from 1e21: only the stops the number grammar can
    // write exactly are offered.
    values_are(
        number_option("exact", "Exact", "1e21", "1.000000000001e21", "1953125", "1e21"),
        {"1e+21",
         "1.00000000000025e+21",
         "1.0000000000005e+21",
         "1.00000000000075e+21",
         "1.000000000001e+21"}
    );

    picker::Mutator tough = made("ridge-tough", "Ridge Tough");
    tough.options.push_back(number_option("health", "Health", "1000", "4000", "1000", "2000.0"));
    tough.options.push_back(number_option("income", "Income", "0", "10", "0.5", "1"));
    tough.options.push_back(number_option("tilt", "Tilt", "-1", "1", "0.5", "0"));
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.mutators.push_back(tough);
    picker::Chosen remembered = chosen_id("ridge-tough");
    remembered.options.emplace_back("health", "3.0e+3");
    remembered.options.emplace_back("income", "+4");
    inputs.last_set.push_back(std::move(remembered));
    picker::Picker model;
    picker::open(model, inputs);
    check_ids(model, {"ridge-tough"});
    OA_CHECK(model.on[0].options[0].second == "3000");
    OA_CHECK(model.on[0].options[1].second == "1");
    OA_CHECK(model.on[0].options[2].second == "0");
    inputs.last_set.clear();
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "ridge-tough").changed);
    OA_CHECK(model.on[0].options[0].second == "2000");

    const auto kept_as =
        [&model](std::string_view option, std::string_view value, std::string_view kept) {
            OA_CHECK(picker::set_option(model, "ridge-tough", option, value));
            for (const auto& [id, stored] : model.on[0].options) {
                if (id == option)
                    OA_CHECK(stored == kept);
            }
        };
    kept_as("health", "2.0e+3", "2000");
    OA_CHECK(picker::item_text(model, model.on[0]) == "Ridge Tough 2000 1 0");
    kept_as("health", "2000.000", "2000");
    kept_as("health", "2e3", "2000");
    kept_as("health", "4E3", "4000");
    kept_as("income", "1e1", "10");
    kept_as("income", "10.0", "10");
    kept_as("income", "0.50", "0.5");
    kept_as("income", "5E-1", "0.5");
    kept_as("tilt", "-0", "0");
    kept_as("tilt", "-0.5", "-0.5");
    for (const std::string_view refused :
         {"+2000", "02000", "2000.", ".5", "0x10", "", " 2000", "2000 ", "1500", "5000", "1e400"})
        OA_CHECK(!picker::set_option(model, "ridge-tough", "health", refused));
    for (const std::string_view refused : {"+4", "00.5", "0.3", "10.5", "-0.5"})
        OA_CHECK(!picker::set_option(model, "ridge-tough", "income", refused));
    const std::vector<picker::Chosen> chosen = picker::result(model);
    OA_CHECK(chosen.size() == 1);
    OA_CHECK(chosen.size() == 1 && chosen[0].options[0].second == "4000");
    OA_CHECK(chosen.size() == 1 && chosen[0].options[1].second == "0.5");
}

/// A mutator that brings units or weapons shows its clash mode on its row.
void clash_rows() {
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    walls.brings = true;
    walls.summary = "Walls";
    picker::Mutator builders = made("fast-builders", "Fast Builders");
    builders.brings = true;
    builders.clashes = picker::Clash::add;
    builders.options.push_back(
        choice_option("build-speed", "Build speed", {"1", "4"}, {"1×", "4×"}, "1")
    );
    picker::Mutator gatehouse = made("gatehouse", "Gatehouse");
    gatehouse.summary = "A gate";
    picker::Inputs inputs;
    inputs.mod_name = "No Mod";
    inputs.clash_default = picker::Clash::replace;
    inputs.mutators.push_back(walls);
    inputs.mutators.push_back(builders);
    inputs.mutators.push_back(gatehouse);
    picker::Picker model;
    picker::open(model, inputs);
    for (const std::string_view id : {"ridge-walls", "fast-builders", "gatehouse"})
        OA_CHECK(picker::switch_on(model, id).changed);
    std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && walls_row->line == "Units and weapons: Replace");
    OA_CHECK(walls_row != nullptr && walls_row->tone == picker::Tone::hint);
    const picker::Row* builders_row = row_of(listed, "fast-builders");
    OA_CHECK(
        builders_row != nullptr && builders_row->line == "Build speed: 1× · Units and weapons: Add"
    );
    const picker::Row* gatehouse_row = row_of(listed, "gatehouse");
    OA_CHECK(gatehouse_row != nullptr && gatehouse_row->line == "A gate");
    OA_CHECK(picker::set_clash(model, "ridge-walls", picker::Clash::add));
    OA_CHECK(!picker::set_clash(model, "gatehouse", picker::Clash::add));
    listed = picker::rows(model);
    walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && walls_row->line == "Units and weapons: Add");
    const std::vector<picker::Chosen> chosen = picker::result(model);
    OA_CHECK(chosen.size() == 3);
    OA_CHECK(chosen.size() == 3 && chosen[0].clash == picker::Clash::add);
    OA_CHECK(chosen.size() == 3 && chosen[1].clash == picker::Clash::add);
    OA_CHECK(chosen.size() == 3 && !chosen[2].clash);

    // The setting's default, and a mode carried in a last set.
    inputs.clash_default = picker::Clash::add;
    picker::Chosen remembered = chosen_id("ridge-walls");
    remembered.clash = picker::Clash::replace;
    inputs.last_set.push_back(remembered);
    picker::open(model, inputs);
    OA_CHECK(!model.on.empty() && model.on[0].clash == picker::Clash::replace);
    inputs.last_set.clear();
    picker::open(model, inputs);
    OA_CHECK(picker::switch_on(model, "ridge-walls").changed);
    listed = picker::rows(model);
    walls_row = row_of(listed, "ridge-walls");
    OA_CHECK(walls_row != nullptr && walls_row->line == "Units and weapons: Add");
}

/// Tag counts follow the current filter and ignore the tag and the search.
/// An Off mutator's default line uses the clash mode open would keep.
void counts_and_default_lines() {
    picker::Mutator scout = made("scout-post", "Scout Post");
    scout.tags = {"ai", "economy"};
    scout.brings = true;
    scout.options.push_back(choice_option("speed", "Speed", {"1x", "2x"}, {"1x", "2x"}, "1x"));
    picker::OptionSpec range;
    range.id = "range";
    range.type = picker::OptionSpec::Type::number;
    range.label = "Range";
    range.least = "0";
    range.most = "10";
    range.step = "2";
    range.fallback = "4.0";
    scout.options.push_back(std::move(range));
    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep");
    keep.installed = false;
    keep.tags = {"ai"};
    keep.brings = true;
    keep.clashes = picker::Clash::add;
    picker::Mutator walls = made("ridge-walls", "Ridge Walls");
    walls.tags = {"maps"};
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.clash_default = picker::Clash::replace;
    inputs.mutators.push_back(scout);
    inputs.mutators.push_back(keep);
    inputs.mutators.push_back(walls);
    picker::Picker model;
    picker::open(model, inputs);
    model.tag = "maps";
    model.query = "zzz-no-match";
    OA_CHECK(picker::tag_count(model, "ai") == 2);
    OA_CHECK(picker::tag_count(model, "economy") == 1);
    OA_CHECK(picker::tag_count(model, "maps") == 1);
    OA_CHECK(picker::tag_count(model, "missing") == 0);
    model.filter = picker::Filter::installed;
    OA_CHECK(picker::tag_count(model, "ai") == 1);
    OA_CHECK(
        picker::default_options_text(model, "scout-post") ==
        "Speed: 1x · Range: 4 · Units and weapons: Replace"
    );
    OA_CHECK(picker::default_options_text(model, "scout-post").find("4.0") == std::string::npos);
    OA_CHECK(picker::default_options_text(model, "gatehouse-keep") == "Units and weapons: Add");
    OA_CHECK(picker::default_options_text(model, "ridge-walls").empty());
    OA_CHECK(picker::default_options_text(model, "gone").empty());
}

/// The size of the random walks' pool of mutators.
constexpr std::size_t walk_size = 8;

/// A small generator with the same sequence on every machine.
struct Steps {
    uint64_t state{}; ///< the generator's state; never zero

    /// Returns the next number.
    ///
    /// @return a number
    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    /// Returns a number below `count`.
    ///
    /// @param count how many numbers to choose from
    /// @return a number from 0 to `count` - 1; 0 when `count` is 0
    std::size_t below(std::size_t count) {
        if (count == 0)
            return 0;
        return static_cast<std::size_t>(next() % count);
    }
};

/// The id of the walk's mutator `index`. Ids at `walk_size` and above are
/// never among the inputs.
///
/// @param index the mutator's number
/// @return its id
std::string walk_id(std::size_t index) {
    return "walk-" + std::to_string(index);
}

/// A random made-up mutator: installed or not, a version, up to two needs
/// with or without a range (a loop is allowed), perhaps a conflict, perhaps
/// unavailable, perhaps bringing units, and a number option.
///
/// @param steps the generator
/// @param index the mutator's number
/// @return the mutator
picker::Mutator walk_mutator(Steps& steps, std::size_t index) {
    static constexpr std::string_view versions[] = {"1.0", "3.9", "4.0", "4.1"};
    static constexpr std::string_view ranges[] = {"", ">= 4.0", "< 4.0", ">= 1.0, < 5.0"};
    picker::Mutator mutator = made(walk_id(index), "Walk " + std::to_string(index));
    mutator.installed = steps.below(4) != 0;
    mutator.version = std::string(versions[steps.below(4)]);
    const std::size_t edges = steps.below(3);
    for (std::size_t edge = 0; edge < edges; ++edge) {
        const std::size_t target = steps.below(walk_size);
        if (target == index)
            continue;
        need_range(mutator, walk_id(target), std::string(ranges[steps.below(4)]), "");
    }
    if (steps.below(6) == 0)
        mutator.conflicts.push_back(walk_id(steps.below(walk_size)));
    if (steps.below(12) == 0)
        mutator.unavailable = "needs Ridge";
    mutator.brings = steps.below(2) == 0;
    mutator.options.push_back(number_option("level", "Level", "0", "2", "0.5", "1"));
    return mutator;
}

/// A level value, valid or not, in one spelling or another.
///
/// @param steps the generator
/// @return the text
std::string walk_level(Steps& steps) {
    static constexpr std::string_view levels[] = {"0", "1.5", "2.0", "+1", "3", "05e-1", "5e-1"};
    return std::string(levels[steps.below(std::size(levels))]);
}

/// Random inputs: most of the pool, and a last set that may name mutators
/// the inputs lack, repeat one, and carry option values.
///
/// @param steps the generator
/// @return the inputs
picker::Inputs walk_inputs(Steps& steps) {
    picker::Inputs inputs;
    inputs.mod_name = "Ridge";
    inputs.remembered = steps.below(2) == 0;
    for (std::size_t index = 0; index < walk_size; ++index) {
        if (steps.below(8) != 0)
            inputs.mutators.push_back(walk_mutator(steps, index));
    }
    const std::size_t count = steps.below(walk_size + 2);
    for (std::size_t index = 0; index < count; ++index) {
        picker::Chosen chosen = chosen_id(walk_id(steps.below(walk_size + 2)));
        if (steps.below(3) == 0)
            chosen.options.emplace_back("level", walk_level(steps));
        inputs.last_set.push_back(std::move(chosen));
    }
    return inputs;
}

/// Changes one thing about the inputs, as a finished download or a new
/// catalogue would: a mutator goes or comes, a version moves, a need or a
/// conflict appears (a loop may form), or availability or installation flips.
///
/// @param steps the generator
/// @param[in,out] inputs the inputs
void walk_change(Steps& steps, picker::Inputs& inputs) {
    const std::size_t kind = steps.below(7);
    if (inputs.mutators.empty() || kind == 1) {
        const std::size_t index = steps.below(walk_size);
        for (const picker::Mutator& mutator : inputs.mutators)
            if (mutator.id == walk_id(index))
                return;
        inputs.mutators.push_back(walk_mutator(steps, index));
        return;
    }
    picker::Mutator& mutator = inputs.mutators[steps.below(inputs.mutators.size())];
    switch (kind) {
    case 0:
        for (std::size_t index = 0; index < inputs.mutators.size(); ++index) {
            if (&inputs.mutators[index] == &mutator) {
                inputs.mutators.erase(inputs.mutators.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
        }
        break;
    case 2:
        mutator.version = steps.below(2) == 0 ? "3.9" : "4.1";
        break;
    case 3:
        need_range(
            mutator,
            walk_id(steps.below(walk_size)),
            steps.below(2) == 0 ? std::string() : std::string(">= 4.0"),
            ""
        );
        break;
    case 4:
        mutator.conflicts.push_back(walk_id(steps.below(walk_size)));
        break;
    case 5:
        mutator.unavailable = mutator.unavailable.empty() ? "needs Ridge" : "";
        break;
    default:
        mutator.installed = !mutator.installed;
        break;
    }
}

/// Finds a mutator in the picker's inputs.
///
/// @param model the picker
/// @param id the id
/// @return the mutator, or null
const picker::Mutator* input_of(const picker::Picker& model, std::string_view id) {
    for (const picker::Mutator& mutator : model.inputs.mutators)
        if (mutator.id == id)
            return &mutator;
    return nullptr;
}

/// Tells whether a version meets a range, by the manifest's matcher.
///
/// @param need the record of the prerequisite
/// @param version the version
/// @return true when it does; an empty range is any version
bool range_holds(const lib::Prerequisite& need, const std::string& version) {
    if (need.range.empty())
        return true;
    oa::formats::oamut::Condition condition;
    condition.id = need.id;
    condition.range_text = need.range;
    condition.range = oa::formats::oamut::parse_version_range(need.range);
    return condition.range && oa::formats::oamut::condition_met(condition, need.id, version);
}

/// Tells whether the On list could start a game, read from the inputs alone:
/// each On mutator held and available, its prerequisites a closure without a
/// loop and all on above it, each in its range and available, and no two On
/// mutators in conflict.
///
/// @param model the picker
/// @return true when the set is playable, installation aside
bool playable(const picker::Picker& model) {
    const std::vector<lib::Need> graph = graph_of(model);
    const std::vector<std::string> ids = ids_of(model);
    if (ids.size() > picker::most_on || !lib::prerequisites_above(graph, ids))
        return false;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const picker::Mutator* mutator = input_of(model, ids[index]);
        if (mutator == nullptr || !mutator->unavailable.empty())
            return false;
        const std::string roots[]{ids[index]};
        const lib::Closure closure = lib::prerequisite_closure(graph, roots);
        if (closure.cyclic)
            return false;
        for (const std::string& item : closure.order) {
            bool present = false;
            for (std::size_t above = 0; above <= index; ++above)
                present = present || ids[above] == item;
            if (!present)
                return false;
        }
        for (const lib::Prerequisite& prerequisite : mutator->needs) {
            const picker::Mutator* needed = input_of(model, prerequisite.id);
            if (needed == nullptr || !needed->unavailable.empty() ||
                !range_holds(prerequisite, needed->version))
                return false;
        }
        for (std::size_t other = 0; other < ids.size(); ++other) {
            if (other == index)
                continue;
            if (ids[other] == ids[index])
                return false;
            const picker::Mutator* beside = input_of(model, ids[other]);
            if (beside == nullptr)
                return false;
            for (const std::string& conflict : mutator->conflicts)
                if (conflict == beside->id)
                    return false;
        }
    }
    return true;
}

/// Checks the picker after one call of the walk: the order, a playable set,
/// `ready` exactly when that set is installed, each row agreeing with what
/// `switch_on` does, and records and a question that name On mutators.
///
/// @param model the picker
/// @return true when everything holds
bool walk_holds(const picker::Picker& model) {
    if (!lib::prerequisites_above(graph_of(model), ids_of(model)))
        return false;
    if (!playable(model))
        return false;
    bool installed = true;
    for (const picker::Chosen& chosen : model.on) {
        const picker::Mutator* mutator = input_of(model, chosen.id);
        installed = installed && mutator != nullptr && mutator->installed;
    }
    if (picker::ready(model) != installed)
        return false;
    for (const picker::CameWith& record : model.came_with) {
        bool prerequisite_on = false;
        bool dependant_on = false;
        for (const picker::Chosen& chosen : model.on) {
            prerequisite_on = prerequisite_on || chosen.id == record.prerequisite;
            dependant_on = dependant_on || chosen.id == record.dependant;
        }
        if (!prerequisite_on || !dependant_on)
            return false;
    }
    if (model.asking) {
        bool asked_on = false;
        for (const picker::Chosen& chosen : model.on)
            asked_on = asked_on || chosen.id == model.asking->id;
        if (!asked_on)
            return false;
    }
    for (const picker::Row& row : picker::rows(model)) {
        if (row.on) {
            if (row.locked)
                return false;
            continue;
        }
        if (model.inputs.read_only)
            continue;
        picker::Picker trial = model;
        const picker::SwitchResult outcome = picker::switch_on(trial, row.id);
        if (row.locked && (outcome.changed || outcome.refused != row.line))
            return false;
        if (!row.locked && !outcome.changed)
            return false;
        if (!playable(trial))
            return false;
    }
    std::ignore = picker::footer(model);
    std::ignore = picker::hint(model);
    return true;
}

/// Random walks through every public call, with questions left open across
/// other calls and refreshes that change the inputs underneath the set. After
/// every call the set keeps its order and stays playable.
void random_walks() {
    Steps steps{0x9e3779b97f4a7c15ULL};
    int broken = 0;
    for (int walk = 0; walk < 300; ++walk) {
        picker::Picker model;
        picker::open(model, walk_inputs(steps));
        for (int call = 0; call < 50; ++call) {
            // An open question is answered now and then, after other calls.
            std::size_t kind = steps.below(18);
            if (model.asking && steps.below(5) == 0)
                kind = 6;
            std::string id = walk_id(steps.below(walk_size + 1));
            switch (kind) {
            case 0:
            case 1:
            case 2:
                // While a question is open, often switch on a mutator that
                // needs the prerequisite it asks about.
                if (model.asking && steps.below(4) != 0) {
                    std::vector<std::string> needing;
                    for (const picker::Mutator& mutator : model.inputs.mutators) {
                        bool on = false;
                        for (const picker::Chosen& chosen : model.on)
                            on = on || chosen.id == mutator.id;
                        for (const lib::Prerequisite& prerequisite : mutator.needs) {
                            if (!on && prerequisite.id == model.asking->id)
                                needing.push_back(mutator.id);
                        }
                    }
                    if (!needing.empty())
                        id = needing[steps.below(needing.size())];
                }
                if (steps.below(2) == 0)
                    std::ignore = picker::switch_on(model, id);
                else
                    std::ignore = picker::switch_on(model, id, steps.below(model.on.size() + 2));
                break;
            case 3:
            case 4:
            case 5:
                // The question is often left open across later calls.
                std::ignore = picker::switch_off(model, id);
                break;
            case 6:
                picker::confirm_turn_off(model);
                break;
            case 7:
                picker::cancel_turn_off(model);
                break;
            case 8:
                std::ignore = picker::move_to(model, id, steps.below(model.on.size() + 2));
                break;
            case 9:
                if (steps.below(2) == 0)
                    std::ignore = picker::move_up(model, id);
                else
                    std::ignore = picker::move_down(model, id);
                break;
            case 10: {
                const bool into_on = steps.below(2) == 0;
                std::ignore = picker::drop(
                    model, id, picker::Drop{into_on, steps.below(model.on.size() + 2)}
                );
                break;
            }
            case 11:
                std::ignore = picker::set_option(model, id, "level", walk_level(steps));
                break;
            case 12:
                std::ignore = picker::set_clash(
                    model, id, steps.below(2) == 0 ? picker::Clash::add : picker::Clash::replace
                );
                break;
            case 13:
            case 14:
            case 15: {
                picker::Inputs changed = model.inputs;
                walk_change(steps, changed);
                picker::refresh(model, std::move(changed));
                break;
            }
            case 16:
                picker::refresh(model, walk_inputs(steps));
                break;
            default:
                picker::open(model, walk_inputs(steps));
                break;
            }
            if (!walk_holds(model)) {
                if (broken == 0)
                    std::fprintf(
                        stderr,
                        "walk %d broke at call %d (%d)\n",
                        walk,
                        call,
                        static_cast<int>(kind)
                    );
                ++broken;
            }
        }
    }
    OA_CHECK(broken == 0);
}

} // namespace

int main() {
    open_empty();
    open_full_set();
    open_drops_what_went();
    open_mends_order();
    open_brings_missing();
    open_trims_the_cap();
    switch_on_brings();
    switch_on_conflict();
    switch_on_unavailable();
    switch_on_cycle();
    read_only_refuses();
    turn_off_asks();
    chain_moves();
    diamond_moves();
    free_moves();
    options_accept_and_refuse();
    switch_on_cap();
    row_lines();
    footer_cases();
    base_game_chip_cases();
    hint_cases();
    short_texts();
    question_words();
    translated_row();
    refresh_keeps();
    not_installed_switches_on();
    locked_missing_prerequisite();
    locked_version_range();
    locked_unavailable_prerequisite();
    locked_brought_conflict();
    stale_conflict();
    refresh_prerequisite_goes();
    refresh_version_falls();
    refresh_loop();
    refresh_new_need();
    refresh_conflict_and_unavailable();
    confirm_recomputes();
    number_values_and_spellings();
    clash_rows();
    counts_and_default_lines();
    random_walks();
    return oa::test::check_exit_status();
}
