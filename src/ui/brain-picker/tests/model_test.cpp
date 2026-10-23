// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain seat picker's model: opening on a seat's choice, a level and an
// option mended to what the brain allows, a brain that cannot play, selecting
// each row, levels and options, the rows, the details, the summary, the
// seat's label and hover card, and a lookup that translates. The brains are
// made up.
#include "oa/test/check.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace picker = oa::ui::brain_picker;
namespace mutators = oa::ui::mutator_picker;
namespace seats = oa::data::brain_library;
namespace library = oa::ui::library;

using Options = std::vector<std::pair<std::string, std::string>>;

/// One level.
///
/// @param id the level id
/// @param name the name a player sees
/// @param difficulty the income
/// @return the level
picker::Level make_level(std::string id, std::string name, seats::Difficulty difficulty) {
    picker::Level made;
    made.id = std::move(id);
    made.name = std::move(name);
    made.difficulty = difficulty;
    return made;
}

/// A choice option.
///
/// @param id the option id
/// @param label the label
/// @param values the values
/// @param fallback the default
/// @return the option
mutators::OptionSpec choice_option(
    std::string id, std::string label, std::vector<std::string> values, std::string fallback
) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::choice;
    spec.label = std::move(label);
    spec.values = std::move(values);
    spec.fallback = std::move(fallback);
    return spec;
}

/// A toggle, off then on.
///
/// @param id the option id
/// @param label the label
/// @param fallback the default, one of the two values
/// @return the option
mutators::OptionSpec toggle_option(std::string id, std::string label, std::string fallback) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::toggle;
    spec.label = std::move(label);
    spec.values = {"off", "on"};
    spec.fallback = std::move(fallback);
    return spec;
}

/// A number option.
///
/// @param id the option id
/// @param label the label
/// @param least the minimum, as written
/// @param most the maximum, as written
/// @param step the step, as written
/// @param fallback the default, as written
/// @return the option
mutators::OptionSpec number_option(
    std::string id,
    std::string label,
    std::string least,
    std::string most,
    std::string step,
    std::string fallback
) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::number;
    spec.label = std::move(label);
    spec.least = std::move(least);
    spec.most = std::move(most);
    spec.step = std::move(step);
    spec.fallback = std::move(fallback);
    return spec;
}

/// Steady Commander: three levels and one option of each kind.
///
/// @return the brain
picker::Brain steady() {
    picker::Brain brain;
    brain.id = "steady-commander";
    brain.name = "Steady Commander";
    brain.version = "1.2";
    brain.author = "Ada";
    brain.summary = "Holds the line.";
    brain.description = "A commander that holds.";
    brain.tags = {"classic"};
    brain.works_with_text = "Any mod";
    brain.levels = {
        make_level("gentle", "Gentle", seats::Difficulty::easy),
        make_level("firm", "Firm", seats::Difficulty::medium),
        make_level("fierce", "Fierce", seats::Difficulty::hard),
    };
    brain.options.push_back(choice_option("aggression", "Aggression", {"low", "high"}, "low"));
    brain.options.push_back(toggle_option("hold", "Hold", "off"));
    brain.options.push_back(number_option("focus", "Focus", "0", "4", "2", "0"));
    return brain;
}

/// Lantern Scout sees the whole map and carries a network. No medium level.
///
/// @return the brain
picker::Brain lantern() {
    picker::Brain brain;
    brain.id = "lantern-scout";
    brain.name = "Lantern Scout";
    brain.version = "0.4";
    brain.author = "Bea";
    brain.summary = "Scouts ahead.";
    brain.description = "Sees the map.";
    brain.tags = {"experimental"};
    brain.full_awareness = true;
    brain.network_bytes = 1048576;
    brain.levels = {
        make_level("dim", "Dim", seats::Difficulty::easy),
        make_level("bright", "Bright", seats::Difficulty::hard),
    };
    return brain;
}

/// A fitting brain whose name is written in lower case, with one hard level.
///
/// @return the brain
picker::Brain quiet() {
    picker::Brain brain;
    brain.id = "quiet-garrison";
    brain.name = "quiet garrison";
    brain.summary = "Stays home.";
    brain.description = "One level.";
    brain.tags = {"turtle"};
    brain.levels = {make_level("only", "Only", seats::Difficulty::hard)};
    return brain;
}

/// Ridge Raider cannot play this seat.
///
/// @return the brain
picker::Brain ridge() {
    picker::Brain brain;
    brain.id = "ridge-raider";
    brain.name = "Ridge Raider";
    brain.version = "3.0";
    brain.author = "Cara";
    brain.summary = "Raids.";
    brain.description = "Made for one mod.";
    brain.tags = {"rush"};
    brain.works_with_text = "Ridge";
    brain.unfit = "Made for Ridge";
    brain.levels = {make_level("raid", "Raid", seats::Difficulty::hard)};
    return brain;
}

/// Tide Keeper cannot play this seat, and has nothing else to say.
///
/// @return the brain
picker::Brain tide() {
    picker::Brain brain;
    brain.id = "tide-keeper";
    brain.name = "Tide Keeper";
    brain.summary = "Holds the shore.";
    brain.description = "Needs an expansion.";
    brain.tags = {"naval"};
    brain.unfit = "Needs Core Contingency";
    brain.levels = {make_level("ebb", "Ebb", seats::Difficulty::easy)};
    return brain;
}

/// The five brains, not in name order.
///
/// @return the brains
std::vector<picker::Brain> roster() {
    return {steady(), tide(), lantern(), ridge(), quiet()};
}

/// Inputs over the roster.
///
/// @param difficulty the setup's difficulty
/// @param fixed true when the Difficulty button owns the built-in level
/// @param offer true when every computer seat is offered
/// @return the inputs
picker::Inputs inputs_for(seats::Difficulty difficulty, bool fixed, bool offer) {
    picker::Inputs inputs;
    inputs.setup = seats::Setup::skirmish;
    inputs.seat_name = "North";
    inputs.mod_name = "No Mod";
    inputs.brains = roster();
    inputs.setup_difficulty = difficulty;
    inputs.built_in_level_fixed = fixed;
    inputs.built_in_level_names = {"Easy", "Medium", "Hard"};
    inputs.offer_every_seat = offer;
    return inputs;
}

/// Opens a picker on `current`.
///
/// @param current the seat's choice
/// @param difficulty the setup's difficulty
/// @param fixed true when the Difficulty button owns the built-in level
/// @param offer true when every computer seat is offered
/// @return the picker
picker::Picker
opened(picker::Choice current, seats::Difficulty difficulty, bool fixed, bool offer) {
    picker::Inputs inputs = inputs_for(difficulty, fixed, offer);
    inputs.current = std::move(current);
    picker::Picker model;
    picker::open(model, std::move(inputs));
    return model;
}

/// The value of one option, or empty when the choice does not name it.
///
/// @param choice the choice
/// @param id the option id
/// @return the value
std::string option_of(const picker::Choice& choice, std::string_view id) {
    for (const auto& [option_id, value] : choice.options) {
        if (option_id == id)
            return value;
    }
    return {};
}

/// Tells whether an option allows `value`.
///
/// @param spec the option
/// @param value the value
/// @return true when the model would keep it
bool allows(const mutators::OptionSpec& spec, std::string_view value) {
    if (spec.type == mutators::OptionSpec::Type::number) {
        for (const std::string& allowed : mutators::number_values(spec)) {
            if (allowed == value)
                return true;
        }
        return false;
    }
    for (const std::string& allowed : spec.values) {
        if (allowed == value)
            return true;
    }
    return false;
}

/// The brain with `id`, or null.
///
/// @param model the picker
/// @param id the brain's id
/// @return the brain
const picker::Brain* brain_of(const picker::Picker& model, std::string_view id) {
    for (const picker::Brain& brain : model.inputs.brains) {
        if (brain.id == id)
            return &brain;
    }
    return nullptr;
}

/// Checks the choice is a brain that can play, at a level it has, with values
/// its options allow.
///
/// @param model the picker
void check_choice(const picker::Picker& model) {
    const picker::Choice& choice = model.choice;
    if (choice.brain.empty()) {
        OA_CHECK(choice.level == "easy" || choice.level == "medium" || choice.level == "hard");
        OA_CHECK(choice.options.empty());
        if (model.inputs.built_in_level_fixed) {
            const std::string_view wanted =
                model.inputs.setup_difficulty == seats::Difficulty::easy   ? "easy"
                : model.inputs.setup_difficulty == seats::Difficulty::hard ? "hard"
                                                                           : "medium";
            OA_CHECK(choice.level == wanted);
        }
        return;
    }
    const picker::Brain* brain = brain_of(model, choice.brain);
    OA_CHECK(brain != nullptr);
    if (brain == nullptr)
        return;
    OA_CHECK(brain->unfit.empty());
    bool found = false;
    for (const picker::Level& level : brain->levels) {
        if (level.id == choice.level)
            found = true;
    }
    OA_CHECK(found);
    OA_CHECK(choice.options.size() == brain->options.size());
    for (std::size_t index = 0; index < choice.options.size() && index < brain->options.size();
         ++index) {
        OA_CHECK(choice.options[index].first == brain->options[index].id);
        OA_CHECK(allows(brain->options[index], choice.options[index].second));
    }
}

/// The row with `id`, or null. The list must outlive the pointer.
///
/// @param listed the rows
/// @param id the brain's id; empty is the built-in player
/// @return the row
const picker::Row* row_named(const std::vector<picker::Row>& listed, std::string_view id) {
    for (const picker::Row& row : listed) {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

/// Opening on the built-in player, with and without the Difficulty button.
void open_built_in() {
    const picker::Picker plain = opened({}, seats::Difficulty::medium, false, true);
    OA_CHECK(plain.choice.brain.empty());
    OA_CHECK(plain.choice.level == "medium");
    OA_CHECK(plain.selected.empty());
    OA_CHECK(plain.left_out.empty());
    OA_CHECK(!plain.every_seat);
    OA_CHECK(picker::hint(plain).empty());
    OA_CHECK(picker::summary(plain) == "Built-in · Medium");
    check_choice(plain);

    picker::Choice remembered;
    remembered.level = "easy";
    const picker::Picker kept = opened(remembered, seats::Difficulty::hard, false, false);
    OA_CHECK(kept.choice.level == "easy");
    OA_CHECK(picker::summary(kept) == "Built-in · Easy");
    check_choice(kept);

    remembered.level = "firm";
    const picker::Picker foreign = opened(remembered, seats::Difficulty::hard, false, false);
    OA_CHECK(foreign.choice.level == "hard");
    check_choice(foreign);

    remembered.level = "easy";
    picker::Picker fixed = opened(remembered, seats::Difficulty::hard, true, false);
    OA_CHECK(fixed.choice.level == "hard");
    OA_CHECK(!picker::set_level(fixed, "easy"));
    OA_CHECK(fixed.choice.level == "hard");
    OA_CHECK(!picker::set_level(fixed, "hard"));
    check_choice(fixed);

    picker::Picker free = opened(picker::Choice{}, seats::Difficulty::hard, false, false);
    OA_CHECK(free.choice.level == "hard");
    OA_CHECK(picker::set_level(free, "easy"));
    OA_CHECK(free.choice.level == "easy");
    OA_CHECK(!picker::set_level(free, "easy"));
    OA_CHECK(!picker::set_level(free, "firm"));
    OA_CHECK(free.choice.level == "easy");
    OA_CHECK(picker::set_level(free, "medium"));
    OA_CHECK(!picker::set_option(free, "aggression", "high"));
    check_choice(free);
}

/// Opening on Steady Commander, including a level and options it does not keep.
void open_steady() {
    picker::Choice current;
    current.brain = "steady-commander";
    current.level = "firm";
    current.options = {{"aggression", "high"}};
    picker::Picker held = opened(current, seats::Difficulty::easy, false, true);
    OA_CHECK(held.choice.brain == "steady-commander");
    OA_CHECK(held.choice.level == "firm");
    OA_CHECK(held.selected == "steady-commander");
    OA_CHECK(option_of(held.choice, "aggression") == "high");
    OA_CHECK(option_of(held.choice, "hold") == "off");
    OA_CHECK(option_of(held.choice, "focus") == "0");
    OA_CHECK(held.choice.options.size() == 3);
    OA_CHECK(held.left_out.empty());
    OA_CHECK(picker::summary(held) == "Steady Commander · Firm");
    OA_CHECK(!picker::select(held, "steady-commander"));
    check_choice(held);

    current.level = "missing";
    current.options = {{"nope", "x"}, {"aggression", "high"}, {"focus", "9"}, {"hold", "on"}};
    const picker::Picker mended = opened(current, seats::Difficulty::medium, false, false);
    OA_CHECK(mended.choice.level == "firm");
    OA_CHECK(option_of(mended.choice, "aggression") == "high");
    OA_CHECK(option_of(mended.choice, "hold") == "on");
    OA_CHECK(option_of(mended.choice, "focus") == "0");
    OA_CHECK(mended.choice.options.size() == 3);
    OA_CHECK(option_of(mended.choice, "nope").empty());
    check_choice(mended);

    const picker::Picker hardest = opened(current, seats::Difficulty::hard, true, false);
    OA_CHECK(hardest.choice.level == "fierce");
    check_choice(hardest);
}

/// A brain that cannot play, and one the inputs do not list, open on the built-in player.
void open_left_out() {
    picker::Choice current;
    current.brain = "ridge-raider";
    current.level = "raid";
    const picker::Picker ridge_seat = opened(current, seats::Difficulty::easy, true, false);
    OA_CHECK(ridge_seat.choice.brain.empty());
    OA_CHECK(ridge_seat.choice.level == "easy");
    OA_CHECK(ridge_seat.left_out == "ridge-raider");
    OA_CHECK(ridge_seat.selected.empty());
    OA_CHECK(
        picker::hint(ridge_seat) ==
        "Ridge Raider cannot play this seat now, so it starts on the built-in player."
    );
    check_choice(ridge_seat);

    current.brain = "tide-keeper";
    const picker::Picker tide_seat = opened(current, seats::Difficulty::medium, false, false);
    OA_CHECK(tide_seat.left_out == "tide-keeper");
    OA_CHECK(
        picker::hint(tide_seat) ==
        "Tide Keeper cannot play this seat now, so it starts on the built-in player."
    );
    check_choice(tide_seat);

    current.brain = "ghost-brain";
    current.level = "firm";
    const picker::Picker missing = opened(current, seats::Difficulty::hard, false, false);
    OA_CHECK(missing.choice.brain.empty());
    OA_CHECK(missing.choice.level == "hard");
    OA_CHECK(missing.left_out == "ghost-brain");
    OA_CHECK(
        picker::hint(missing) ==
        "ghost-brain cannot play this seat now, so it starts on the built-in player."
    );
    check_choice(missing);
}

/// Selecting each row, and the level `default_level` picks.
void select_rows() {
    picker::Picker model = opened({}, seats::Difficulty::medium, false, true);
    OA_CHECK(picker::select(model, "steady-commander"));
    OA_CHECK(model.choice.brain == "steady-commander");
    OA_CHECK(model.choice.level == "firm");
    OA_CHECK(option_of(model.choice, "aggression") == "low");
    OA_CHECK(model.selected == "steady-commander");
    check_choice(model);

    OA_CHECK(picker::set_level(model, "fierce"));
    OA_CHECK(picker::set_option(model, "aggression", "high"));
    OA_CHECK(!picker::select(model, "no-such"));
    OA_CHECK(model.choice.level == "fierce");
    OA_CHECK(model.selected == "steady-commander");

    OA_CHECK(picker::select(model, "ridge-raider"));
    OA_CHECK(model.selected == "ridge-raider");
    OA_CHECK(model.choice.brain == "steady-commander");
    OA_CHECK(model.choice.level == "fierce");
    OA_CHECK(option_of(model.choice, "aggression") == "high");
    OA_CHECK(!picker::select(model, "ridge-raider"));
    check_choice(model);

    OA_CHECK(picker::select(model, "tide-keeper"));
    OA_CHECK(model.selected == "tide-keeper");
    OA_CHECK(model.choice.brain == "steady-commander");
    OA_CHECK(!picker::set_level(model, "ebb"));
    OA_CHECK(model.choice.level == "fierce");

    OA_CHECK(picker::select(model, "steady-commander"));
    OA_CHECK(model.choice.level == "fierce");
    OA_CHECK(option_of(model.choice, "aggression") == "high");
    check_choice(model);

    OA_CHECK(picker::select(model, "lantern-scout"));
    OA_CHECK(model.choice.brain == "lantern-scout");
    OA_CHECK(model.choice.level == "bright");
    OA_CHECK(model.choice.options.empty());
    check_choice(model);

    OA_CHECK(picker::select(model, "quiet-garrison"));
    OA_CHECK(model.choice.level == "only");
    check_choice(model);

    OA_CHECK(picker::select(model, ""));
    OA_CHECK(model.choice.brain.empty());
    OA_CHECK(model.choice.level == "medium");
    OA_CHECK(model.selected.empty());
    check_choice(model);

    const seats::Difficulty difficulties[] = {
        seats::Difficulty::easy, seats::Difficulty::medium, seats::Difficulty::hard
    };
    const char* steady_levels[] = {"gentle", "firm", "fierce"};
    const char* lantern_levels[] = {"dim", "bright", "bright"};
    for (std::size_t index = 0; index < 3; ++index) {
        picker::Picker again = opened({}, difficulties[index], false, false);
        OA_CHECK(picker::select(again, "steady-commander"));
        OA_CHECK(again.choice.level == steady_levels[index]);
        OA_CHECK(picker::select(again, "lantern-scout"));
        OA_CHECK(again.choice.level == lantern_levels[index]);
        OA_CHECK(picker::select(again, "quiet-garrison"));
        OA_CHECK(again.choice.level == "only");
        check_choice(again);
    }
}

/// Setting a level the brain has, and one it lacks.
void change_level() {
    picker::Choice current;
    current.brain = "steady-commander";
    current.level = "firm";
    picker::Picker model = opened(current, seats::Difficulty::medium, false, false);
    OA_CHECK(picker::set_level(model, "gentle"));
    OA_CHECK(model.choice.level == "gentle");
    OA_CHECK(!picker::set_level(model, "gentle"));
    OA_CHECK(!picker::set_level(model, "bright"));
    OA_CHECK(!picker::set_level(model, "easy"));
    OA_CHECK(model.choice.level == "gentle");
    OA_CHECK(picker::summary(model) == "Steady Commander · Gentle");
    check_choice(model);
}

/// A choice, a toggle and a number, kept and refused.
void change_options() {
    picker::Choice current;
    current.brain = "steady-commander";
    current.level = "firm";
    picker::Picker model = opened(std::move(current), seats::Difficulty::medium, false, false);
    const picker::Brain* brain = brain_of(model, "steady-commander");
    OA_CHECK(brain != nullptr && brain->options.size() == 3);
    if (brain == nullptr)
        return;
    const std::vector<std::string> stops = mutators::number_values(brain->options[2]);
    OA_CHECK(stops == std::vector<std::string>({"0", "2", "4"}));

    OA_CHECK(picker::set_option(model, "aggression", "high"));
    OA_CHECK(!picker::set_option(model, "aggression", "high"));
    OA_CHECK(!picker::set_option(model, "aggression", "medium"));
    OA_CHECK(option_of(model.choice, "aggression") == "high");

    OA_CHECK(picker::set_option(model, "hold", "on"));
    OA_CHECK(!picker::set_option(model, "hold", "maybe"));
    OA_CHECK(option_of(model.choice, "hold") == "on");

    OA_CHECK(picker::set_option(model, "focus", "2"));
    OA_CHECK(!picker::set_option(model, "focus", "1"));
    OA_CHECK(!picker::set_option(model, "focus", "1e1"));
    OA_CHECK(picker::set_option(model, "focus", "4"));
    OA_CHECK(option_of(model.choice, "focus") == "4");
    OA_CHECK(!picker::set_option(model, "nope", "1"));
    check_choice(model);

    const picker::Choice stored = picker::result(model);
    OA_CHECK(stored.brain == "steady-commander");
    OA_CHECK(stored.level == "firm");
    OA_CHECK(stored.options.size() == 3);
    OA_CHECK(stored.options[0] == std::make_pair(std::string("aggression"), std::string("high")));
    OA_CHECK(stored.options[1] == std::make_pair(std::string("hold"), std::string("on")));
    OA_CHECK(stored.options[2] == std::make_pair(std::string("focus"), std::string("4")));
}

/// Every computer seat, offered and not.
void every_seat() {
    picker::Picker offered = opened({}, seats::Difficulty::hard, true, true);
    OA_CHECK(!picker::set_every_seat(offered, false));
    OA_CHECK(picker::set_every_seat(offered, true));
    OA_CHECK(!picker::set_every_seat(offered, true));
    OA_CHECK(picker::summary(offered) == "Built-in · Hard · every computer seat");
    OA_CHECK(picker::set_every_seat(offered, false));
    OA_CHECK(picker::summary(offered) == "Built-in · Hard");
    check_choice(offered);

    picker::Picker plain = opened({}, seats::Difficulty::hard, true, false);
    OA_CHECK(!picker::set_every_seat(plain, true));
    OA_CHECK(!plain.every_seat);
    OA_CHECK(picker::summary(plain) == "Built-in · Hard");
}

/// The rows' order, lines, tones and locks.
void row_list() {
    picker::Choice current;
    current.brain = "steady-commander";
    current.level = "firm";
    current.options = {{"aggression", "high"}};
    const picker::Picker model = opened(current, seats::Difficulty::easy, false, false);
    const std::vector<picker::Row> listed = picker::rows(model);
    OA_CHECK(listed.size() == 6);
    const std::vector<std::string> ids = {
        listed[0].id,
        listed[1].id,
        listed[2].id,
        listed[3].id,
        listed[4].id,
        listed[5].id,
    };
    OA_CHECK(
        ids == std::vector<std::string>(
                   {"",
                    "lantern-scout",
                    "quiet-garrison",
                    "ridge-raider",
                    "steady-commander",
                    "tide-keeper"}
               )
    );

    const picker::Row* built_in = row_named(listed, "");
    OA_CHECK(built_in != nullptr);
    if (built_in != nullptr) {
        OA_CHECK(built_in->name == "Built-in computer player");
        OA_CHECK(built_in->line == "Built into Open Annihilation");
        OA_CHECK(built_in->tone == picker::Tone::plain);
        OA_CHECK(!built_in->locked && !built_in->chosen && !built_in->full_awareness);
    }
    const picker::Row* scout = row_named(listed, "lantern-scout");
    OA_CHECK(
        scout != nullptr && scout->line == "Scouts ahead." && scout->tone == picker::Tone::plain
    );
    OA_CHECK(scout != nullptr && scout->full_awareness && !scout->locked && !scout->chosen);
    const picker::Row* garrison = row_named(listed, "quiet-garrison");
    OA_CHECK(
        garrison != nullptr && garrison->name == "quiet garrison" && garrison->line == "Stays home."
    );
    const picker::Row* raider = row_named(listed, "ridge-raider");
    OA_CHECK(
        raider != nullptr && raider->line == "Made for Ridge" && raider->tone == picker::Tone::lock
    );
    OA_CHECK(raider != nullptr && raider->locked && !raider->chosen);
    const picker::Row* commander = row_named(listed, "steady-commander");
    OA_CHECK(
        commander != nullptr && commander->line == "Firm" && commander->tone == picker::Tone::hint
    );
    OA_CHECK(commander != nullptr && commander->chosen && !commander->locked);
    const picker::Row* keeper = row_named(listed, "tide-keeper");
    OA_CHECK(
        keeper != nullptr && keeper->line == "Needs Core Contingency" &&
        keeper->tone == picker::Tone::lock
    );
    OA_CHECK(keeper != nullptr && keeper->locked && !keeper->chosen && !keeper->full_awareness);

    const picker::Picker home = opened({}, seats::Difficulty::hard, true, false);
    const std::vector<picker::Row> home_rows = picker::rows(home);
    const picker::Row* home_built_in = row_named(home_rows, "");
    OA_CHECK(home_built_in != nullptr && home_built_in->line == "Hard" && home_built_in->chosen);
    OA_CHECK(home_built_in != nullptr && home_built_in->tone == picker::Tone::hint);
    const picker::Row* home_commander = row_named(home_rows, "steady-commander");
    OA_CHECK(home_commander != nullptr && home_commander->line == "Holds the line.");
    OA_CHECK(home_commander != nullptr && home_commander->tone == picker::Tone::plain);
}

/// The details of the built-in player, a chosen brain, a locked brain and a quiet one.
void detail_pages() {
    picker::Choice current;
    current.brain = "steady-commander";
    current.level = "firm";
    current.options = {{"aggression", "high"}};
    picker::Picker model = opened(current, seats::Difficulty::medium, false, false);
    const picker::Details commander = picker::details(model);
    OA_CHECK(commander.name == "Steady Commander");
    OA_CHECK(commander.version_author == "1.2 · Ada");
    OA_CHECK(commander.description == "A commander that holds.");
    OA_CHECK(commander.levels.size() == 3 && commander.level == 1);
    OA_CHECK(!commander.levels_locked && commander.level_note.empty() && commander.chosen);
    OA_CHECK(commander.options.size() == 3 && commander.values.size() == 3);
    OA_CHECK(
        commander.values[0] == "high" && commander.values[1] == "off" && commander.values[2] == "0"
    );
    OA_CHECK(commander.facts.size() == 1);
    if (!commander.facts.empty()) {
        OA_CHECK(commander.facts[0].label == "Works with");
        OA_CHECK(commander.facts[0].value == "Any mod");
        OA_CHECK(commander.facts[0].tone == picker::Tone::plain);
    }

    OA_CHECK(picker::select(model, "quiet-garrison"));
    const picker::Details garrison = picker::details(model);
    OA_CHECK(garrison.facts.empty());
    OA_CHECK(garrison.chosen && !garrison.levels_locked && garrison.level == 0);
    OA_CHECK(garrison.options.empty() && garrison.values.empty());
    OA_CHECK(garrison.version_author.empty());

    OA_CHECK(picker::select(model, "lantern-scout"));
    const picker::Details scout = picker::details(model);
    OA_CHECK(scout.chosen && scout.level == 1);
    OA_CHECK(scout.facts.size() == 2);
    if (scout.facts.size() == 2) {
        OA_CHECK(scout.facts[0].label == "Full awareness");
        OA_CHECK(
            scout.facts[0].value ==
            "Sees every unit on the map, as Total Annihilation's own computer player does"
        );
        OA_CHECK(scout.facts[0].tone == picker::Tone::lock);
        const std::string size = library::size_text(1048576);
        OA_CHECK(scout.facts[1].label == "Network");
        OA_CHECK(
            scout.facts[1].value ==
            library::filled(library::TextHooks{}, library::words::network_learned, {{"size", size}})
        );
        OA_CHECK(scout.facts[1].tone == picker::Tone::plain);
    }

    OA_CHECK(picker::select(model, "ridge-raider"));
    const picker::Details raider = picker::details(model);
    OA_CHECK(!raider.chosen && raider.levels_locked);
    OA_CHECK(raider.facts.size() == 2);
    if (raider.facts.size() == 2) {
        OA_CHECK(raider.facts[0].label.empty());
        OA_CHECK(raider.facts[0].value == "Made for Ridge");
        OA_CHECK(raider.facts[0].tone == picker::Tone::lock);
        OA_CHECK(raider.facts[1].label == "Works with" && raider.facts[1].value == "Ridge");
    }
    OA_CHECK(model.choice.brain == "lantern-scout");

    OA_CHECK(picker::select(model, "tide-keeper"));
    const picker::Details keeper = picker::details(model);
    OA_CHECK(keeper.facts.size() == 1);
    if (!keeper.facts.empty()) {
        OA_CHECK(keeper.facts[0].value == "Needs Core Contingency");
        OA_CHECK(keeper.facts[0].tone == picker::Tone::lock);
    }

    const picker::Picker fixed = opened({}, seats::Difficulty::hard, true, false);
    const picker::Details built_in = picker::details(fixed);
    OA_CHECK(built_in.name == "Built-in computer player");
    OA_CHECK(built_in.version_author.empty());
    OA_CHECK(built_in.description == "Built into Open Annihilation");
    OA_CHECK(built_in.levels.size() == 3 && built_in.level == 2);
    OA_CHECK(built_in.levels[0].id == "easy" && built_in.levels[2].name == "Hard");
    OA_CHECK(built_in.levels_locked && built_in.chosen && built_in.facts.empty());
    OA_CHECK(built_in.level_note == "Set by the Difficulty button: Hard");
    OA_CHECK(built_in.options.empty());
    check_choice(model);
    check_choice(fixed);
}

/// The seat's label in both forms, and its hover card.
void seat_words() {
    picker::SeatShown built_in;
    built_in.level = "Hard";
    OA_CHECK(picker::seat_label(built_in, false) == "Built-in · Hard");
    OA_CHECK(picker::seat_label(built_in, true) == "Built-in · Hard");

    picker::SeatShown commander;
    commander.name = "Steady Commander";
    commander.version = "1.2";
    commander.level = "Firm";
    OA_CHECK(picker::seat_label(commander, false) == "Steady Commander · Firm");
    OA_CHECK(picker::seat_label(commander, true) == "Steady Commander 1.2 · Firm");
    commander.version.clear();
    OA_CHECK(picker::seat_label(commander, true) == "Steady Commander · Firm");

    const oa::ui::kit::HoverCardLook home = picker::seat_card(built_in);
    OA_CHECK(home.title == "Built-in computer player");
    OA_CHECK(home.arrow == oa::ui::kit::Side::none);
    OA_CHECK(home.rows.size() == 1);
    if (!home.rows.empty()) {
        OA_CHECK(home.rows[0].label == "Level");
        OA_CHECK(home.rows[0].value == "Hard");
    }

    picker::SeatShown scout;
    scout.name = "Lantern Scout";
    scout.version = "0.4";
    scout.level = "Bright";
    scout.full_awareness = true;
    const oa::ui::kit::HoverCardLook card = picker::seat_card(scout);
    OA_CHECK(card.title == "Lantern Scout");
    OA_CHECK(card.arrow == oa::ui::kit::Side::none);
    OA_CHECK(card.rows.size() == 3);
    if (card.rows.size() == 3) {
        OA_CHECK(card.rows[0].label == "Version" && card.rows[0].value == "0.4");
        OA_CHECK(card.rows[1].label == "Level" && card.rows[1].value == "Bright");
        OA_CHECK(card.rows[2].label == "Full awareness");
        OA_CHECK(card.rows[2].value == "Sees every unit on the map");
        OA_CHECK(card.rows[2].value_colour == oa::ui::kit::colour::lock);
    }
}

/// A lookup replaces the English pattern before its places are filled.
void translated() {
    picker::Inputs inputs = inputs_for(seats::Difficulty::hard, true, true);
    inputs.current.brain = "ridge-raider";
    inputs.text.shown = [](void*, std::string_view english) -> std::string_view {
        if (english == picker::words::built_in)
            return "Eingebaut";
        if (english == picker::words::brain_level)
            return "{level} / {brain}";
        if (english == picker::words::left_out)
            return "{name} bleibt draussen.";
        if (english == picker::words::built_into)
            return "Eingebaut in Open Annihilation";
        if (english == picker::words::every_seat)
            return "{label} · alle";
        if (english == library::words::fact_works_with)
            return "Passt zu";
        return english;
    };
    picker::Picker model;
    picker::open(model, std::move(inputs));
    OA_CHECK(picker::hint(model) == "Ridge Raider bleibt draussen.");
    OA_CHECK(picker::summary(model) == "Hard / Eingebaut");
    OA_CHECK(picker::set_every_seat(model, true));
    OA_CHECK(picker::summary(model) == "Hard / Eingebaut · alle");
    OA_CHECK(picker::select(model, "steady-commander"));
    const std::vector<picker::Row> listed = picker::rows(model);
    const picker::Row* built_in = row_named(listed, "");
    OA_CHECK(built_in != nullptr && built_in->line == "Eingebaut in Open Annihilation");
    OA_CHECK(!picker::select(model, "steady-commander"));
    const picker::Details commander = picker::details(model);
    OA_CHECK(!commander.facts.empty() && commander.facts[0].label == "Passt zu");

    picker::SeatShown shown;
    shown.level = "Hard";
    OA_CHECK(picker::seat_label(shown, false, model.inputs.text) == "Hard / Eingebaut");
    check_choice(model);
}

} // namespace

int main() {
    open_built_in();
    open_steady();
    open_left_out();
    select_rows();
    change_level();
    change_options();
    every_seat();
    row_list();
    detail_pages();
    seat_words();
    translated();
    return oa::test::check_exit_status();
}
