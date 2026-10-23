// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words a brain seat picker's rows, details, summary, hint, seat label
// and hover card show. Every sentence is looked up through the Library's
// TextHooks.
#include "oa/ui/brain_picker/model.hpp"

#include "internal.hpp"

#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::brain_picker {
namespace {

/// The name folded to lower case, ASCII letters only.
///
/// @param name the name
/// @return the folded name
std::string folded_name(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char byte : name)
        folded.push_back(static_cast<char>(std::tolower(byte)));
    return folded;
}

/// The built-in player's row.
///
/// @param picker the picker
/// @return the row
Row built_in_row(const Picker& picker) {
    const library::TextHooks& hooks = picker.inputs.text;
    Row row;
    row.name = std::string(library::shown(hooks, words::built_in_player));
    row.chosen = picker.choice.brain.empty();
    if (row.chosen) {
        row.line = detail::built_in_level_name(picker.inputs, picker.choice.level);
        row.tone = Tone::hint;
    } else {
        row.line = std::string(library::shown(hooks, words::built_into));
    }
    return row;
}

/// One brain's row.
///
/// @param picker the picker
/// @param brain the brain
/// @return the row
Row brain_row(const Picker& picker, const Brain& brain) {
    Row row;
    row.id = brain.id;
    row.name = brain.name.empty() ? brain.id : brain.name;
    row.full_awareness = brain.full_awareness;
    row.chosen = picker.choice.brain == brain.id;
    row.locked = !detail::can_play(brain);
    if (row.locked) {
        row.line = brain.unfit;
        row.tone = Tone::lock;
    } else if (row.chosen) {
        row.line = detail::level_name(brain, picker.choice.level);
        row.tone = Tone::hint;
    } else {
        row.line = brain.summary;
    }
    return row;
}

/// The version and the author, as the details show them.
///
/// @param hooks how texts are looked up
/// @param version the version; empty when unnamed
/// @param author the author; empty when unnamed
/// @return the line; empty when both are empty
std::string
version_author(const library::TextHooks& hooks, std::string_view version, std::string_view author) {
    if (!version.empty() && !author.empty()) {
        return library::filled(
            hooks, mutator_picker::words::version_author, {{"version", version}, {"author", author}}
        );
    }
    if (!version.empty())
        return std::string(version);
    return std::string(author);
}

/// Appends a fact.
///
/// @param[in,out] facts the facts so far
/// @param label the label; empty when the value stands alone
/// @param value the value
/// @param tone how the value is drawn
void add_fact(std::vector<Fact>& facts, std::string label, std::string value, Tone tone) {
    Fact fact;
    fact.label = std::move(label);
    fact.value = std::move(value);
    fact.tone = tone;
    facts.push_back(std::move(fact));
}

/// The facts of a brain: its reason, then Works with, Full awareness and Network.
///
/// @param hooks how texts are looked up
/// @param brain the brain
/// @return the facts
std::vector<Fact> brain_facts(const library::TextHooks& hooks, const Brain& brain) {
    std::vector<Fact> facts;
    if (!brain.unfit.empty())
        add_fact(facts, {}, brain.unfit, Tone::lock);
    if (!brain.works_with_text.empty()) {
        add_fact(
            facts,
            std::string(library::shown(hooks, library::words::fact_works_with)),
            brain.works_with_text,
            Tone::plain
        );
    }
    if (brain.full_awareness) {
        add_fact(
            facts,
            std::string(library::shown(hooks, library::words::chip_full_awareness)),
            std::string(library::shown(hooks, library::words::full_awareness_sees)),
            Tone::lock
        );
    }
    if (brain.network_bytes > 0) {
        const std::string size = library::size_text(brain.network_bytes, hooks);
        add_fact(
            facts,
            std::string(library::shown(hooks, library::words::fact_network)),
            library::filled(hooks, library::words::network_learned, {{"size", size}}),
            Tone::plain
        );
    }
    return facts;
}

/// The built-in player's three levels, named as the inputs give them.
///
/// @param inputs the host's inputs
/// @return Easy, Medium and Hard
std::vector<Level> built_in_levels(const Inputs& inputs) {
    std::vector<Level> levels;
    levels.reserve(3);
    const std::string_view ids[] = {"easy", "medium", "hard"};
    const data::brain_library::Difficulty difficulties[] = {
        data::brain_library::Difficulty::easy,
        data::brain_library::Difficulty::medium,
        data::brain_library::Difficulty::hard,
    };
    for (std::size_t index = 0; index < 3; ++index) {
        Level level;
        level.id = std::string(ids[index]);
        level.name = inputs.built_in_level_names[index];
        level.difficulty = difficulties[index];
        levels.push_back(std::move(level));
    }
    return levels;
}

/// Which of the built-in player's levels `level` is.
///
/// @param level the level id
/// @return 0, 1 or 2; 1 when `level` is not one of the three
std::size_t built_in_index(std::string_view level) {
    if (level == "easy")
        return 0;
    if (level == "hard")
        return 2;
    return 1;
}

/// The details of the built-in player.
///
/// @param picker the picker
/// @return the details
Details built_in_details(const Picker& picker) {
    const library::TextHooks& hooks = picker.inputs.text;
    Details shown;
    shown.name = std::string(library::shown(hooks, words::built_in_player));
    shown.description = std::string(library::shown(hooks, words::built_into));
    shown.levels = built_in_levels(picker.inputs);
    shown.chosen = picker.choice.brain.empty();
    const std::string level_id =
        shown.chosen ? picker.choice.level
                     : std::string(detail::difficulty_id(picker.inputs.setup_difficulty));
    shown.level = built_in_index(level_id);
    shown.levels_locked = picker.inputs.built_in_level_fixed;
    if (shown.levels_locked) {
        const std::string name = detail::built_in_level_name(picker.inputs, level_id);
        shown.level_note = library::filled(hooks, words::level_from_difficulty, {{"level", name}});
    }
    return shown;
}

/// The value shown for one option.
///
/// The choice's value when this brain is the choice and the value is one
/// the option allows. Otherwise the default.
///
/// @param picker the picker
/// @param brain the brain
/// @param spec the option
/// @return the value
std::string
option_value(const Picker& picker, const Brain& brain, const mutator_picker::OptionSpec& spec) {
    if (picker.choice.brain == brain.id) {
        for (const auto& [option_id, stored] : picker.choice.options) {
            if (option_id != spec.id)
                continue;
            if (detail::kept_option(spec, stored))
                return stored;
            break;
        }
    }
    return detail::default_option(spec);
}

/// The details of one brain.
///
/// @param picker the picker
/// @param brain the brain
/// @return the details
Details brain_details(const Picker& picker, const Brain& brain) {
    const library::TextHooks& hooks = picker.inputs.text;
    Details shown;
    shown.name = brain.name.empty() ? brain.id : brain.name;
    shown.version_author = version_author(hooks, brain.version, brain.author);
    shown.description = brain.description;
    shown.levels = brain.levels;
    shown.chosen = picker.choice.brain == brain.id;
    shown.levels_locked = !detail::can_play(brain);
    std::size_t index =
        shown.chosen ? detail::level_index(brain, picker.choice.level) : brain.levels.size();
    if (index >= brain.levels.size())
        index = detail::level_index(
            brain, detail::default_level_id(brain, picker.inputs.setup_difficulty)
        );
    shown.level = index < brain.levels.size() ? index : 0;
    shown.options = brain.options;
    shown.values.reserve(brain.options.size());
    for (const mutator_picker::OptionSpec& spec : brain.options)
        shown.values.push_back(option_value(picker, brain, spec));
    shown.facts = brain_facts(hooks, brain);
    return shown;
}

/// The choice as a seat, without a version.
///
/// @param picker the picker
/// @return the seat
SeatShown choice_shown(const Picker& picker) {
    SeatShown shown;
    if (picker.choice.brain.empty()) {
        shown.level = detail::built_in_level_name(picker.inputs, picker.choice.level);
        return shown;
    }
    const Brain* brain = detail::find_brain(picker, picker.choice.brain);
    if (brain == nullptr) {
        shown.name = picker.choice.brain;
        shown.level = picker.choice.level;
        return shown;
    }
    shown.name = brain->name.empty() ? brain->id : brain->name;
    shown.level = detail::level_name(*brain, picker.choice.level);
    shown.full_awareness = brain->full_awareness;
    return shown;
}

} // namespace

std::vector<Row> rows(const Picker& picker) {
    std::vector<Row> listed;
    listed.push_back(built_in_row(picker));
    const std::vector<Brain>& brains = picker.inputs.brains;
    std::vector<std::size_t> order;
    order.reserve(brains.size());
    for (std::size_t index = 0; index < brains.size(); ++index) {
        if (!brains[index].id.empty())
            order.push_back(index);
    }
    std::stable_sort(order.begin(), order.end(), [&brains](std::size_t left, std::size_t right) {
        return folded_name(brains[left].name) < folded_name(brains[right].name);
    });
    listed.reserve(order.size() + 1);
    for (const std::size_t index : order)
        listed.push_back(brain_row(picker, brains[index]));
    return listed;
}

Details details(const Picker& picker) {
    if (picker.selected.empty())
        return built_in_details(picker);
    const Brain* brain = detail::find_brain(picker, picker.selected);
    if (brain == nullptr)
        return {};
    return brain_details(picker, *brain);
}

std::string summary(const Picker& picker) {
    const std::string label = seat_label(choice_shown(picker), false, picker.inputs.text);
    if (!picker.every_seat)
        return label;
    return library::filled(picker.inputs.text, words::every_seat, {{"label", label}});
}

std::string hint(const Picker& picker) {
    if (picker.left_out.empty())
        return {};
    std::string name = picker.left_out;
    if (const Brain* brain = detail::find_brain(picker, picker.left_out)) {
        if (!brain->name.empty())
            name = brain->name;
    }
    return library::filled(picker.inputs.text, words::left_out, {{"name", name}});
}

std::string seat_label(const SeatShown& shown, bool with_version, const library::TextHooks& hooks) {
    if (shown.name.empty()) {
        const std::string brain(library::shown(hooks, words::built_in));
        return library::filled(
            hooks, words::brain_level, {{"brain", brain}, {"level", shown.level}}
        );
    }
    if (with_version && !shown.version.empty()) {
        return library::filled(
            hooks,
            words::brain_version_level,
            {{"brain", shown.name}, {"version", shown.version}, {"level", shown.level}}
        );
    }
    return library::filled(
        hooks, words::brain_level, {{"brain", shown.name}, {"level", shown.level}}
    );
}

kit::HoverCardLook seat_card(const SeatShown& shown, const library::TextHooks& hooks) {
    kit::HoverCardLook look;
    look.arrow = kit::Side::none;
    if (shown.name.empty())
        look.title = std::string(library::shown(hooks, words::built_in_player));
    else
        look.title = shown.name;
    if (!shown.name.empty()) {
        kit::HoverCardRow version;
        version.label = std::string(library::shown(hooks, words::version));
        version.value = shown.version;
        look.rows.push_back(std::move(version));
    }
    kit::HoverCardRow level;
    level.label = std::string(library::shown(hooks, words::level));
    level.value = shown.level;
    look.rows.push_back(std::move(level));
    if (shown.full_awareness) {
        kit::HoverCardRow awareness;
        awareness.label = std::string(library::shown(hooks, library::words::chip_full_awareness));
        awareness.value = std::string(library::shown(hooks, words::sees_every_unit));
        awareness.value_colour = kit::colour::lock;
        look.rows.push_back(std::move(awareness));
    }
    return look;
}

} // namespace oa::ui::brain_picker
