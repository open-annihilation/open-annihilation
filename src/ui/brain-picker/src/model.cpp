// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain seat picker's choice: which brain, at which level, with which
// options. A level is one the brain has. An option value is one the mutator
// picker's rules allow.
#include "oa/ui/brain_picker/model.hpp"

#include "internal.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::brain_picker {
namespace detail {

/// The option with `id`, or null.
///
/// @param brain the brain
/// @param id the option's id
/// @return the option
const mutator_picker::OptionSpec* find_option(const Brain& brain, std::string_view id) {
    for (const mutator_picker::OptionSpec& spec : brain.options) {
        if (spec.id == id)
            return &spec;
    }
    return nullptr;
}

/// Tells whether the brain has `level`.
///
/// @param brain the brain
/// @param level the level id
/// @return true when one of its levels has that id
bool has_level(const Brain& brain, std::string_view level) {
    return level_index(brain, level) < brain.levels.size();
}

/// Makes the built-in player the choice, at `level`.
///
/// @param[in,out] picker the picker
/// @param level the level id
void choose_built_in(Picker& picker, std::string level) {
    picker.choice.brain.clear();
    picker.choice.level = std::move(level);
    picker.choice.options.clear();
    picker.selected.clear();
}

/// Makes `brain` the choice at `level`, with `options`.
///
/// @param[in,out] picker the picker
/// @param brain the brain
/// @param level the level id
/// @param options the option values
void choose_brain(
    Picker& picker,
    const Brain& brain,
    std::string level,
    std::vector<std::pair<std::string, std::string>> options
) {
    picker.choice.brain = brain.id;
    picker.choice.level = std::move(level);
    picker.choice.options = std::move(options);
    picker.selected = brain.id;
}

const Brain* find_brain(const Picker& picker, std::string_view id) {
    if (id.empty())
        return nullptr;
    for (const Brain& brain : picker.inputs.brains) {
        if (!brain.id.empty() && brain.id == id)
            return &brain;
    }
    return nullptr;
}

bool listed(const Picker& picker, std::string_view id) {
    return id.empty() || find_brain(picker, id) != nullptr;
}

bool can_play(const Brain& brain) {
    return brain.unfit.empty();
}

bool built_in_level(std::string_view level) {
    return level == "easy" || level == "medium" || level == "hard";
}

std::string_view difficulty_id(data::brain_library::Difficulty difficulty) {
    if (difficulty == data::brain_library::Difficulty::easy)
        return "easy";
    if (difficulty == data::brain_library::Difficulty::hard)
        return "hard";
    return "medium";
}

std::string
opened_built_in_level(bool fixed, data::brain_library::Difficulty setup, std::string_view current) {
    if (!fixed && built_in_level(current))
        return std::string(current);
    return std::string(difficulty_id(setup));
}

std::string built_in_level_name(const Inputs& inputs, std::string_view level) {
    std::size_t index = 1;
    if (level == "easy")
        index = 0;
    else if (level == "hard")
        index = 2;
    else if (level != "medium")
        return std::string(level);
    return inputs.built_in_level_names[index];
}

std::size_t level_index(const Brain& brain, std::string_view level) {
    for (std::size_t index = 0; index < brain.levels.size(); ++index) {
        if (brain.levels[index].id == level)
            return index;
    }
    return brain.levels.size();
}

std::string default_level_id(const Brain& brain, data::brain_library::Difficulty wanted) {
    std::vector<data::brain_library::Difficulty> difficulties;
    difficulties.reserve(brain.levels.size());
    for (const Level& level : brain.levels)
        difficulties.push_back(level.difficulty);
    const std::size_t index = data::brain_library::default_level(difficulties, wanted);
    if (index >= brain.levels.size())
        return {};
    return brain.levels[index].id;
}

std::string level_name(const Brain& brain, std::string_view level) {
    const std::size_t index = level_index(brain, level);
    if (index >= brain.levels.size())
        return std::string(level);
    if (brain.levels[index].name.empty())
        return brain.levels[index].id;
    return brain.levels[index].name;
}

std::optional<std::string>
kept_option(const mutator_picker::OptionSpec& spec, std::string_view value) {
    if (spec.type == mutator_picker::OptionSpec::Type::number) {
        for (const std::string& allowed : mutator_picker::number_values(spec)) {
            if (allowed == value)
                return allowed;
        }
        return std::nullopt;
    }
    for (const std::string& allowed : spec.values) {
        if (allowed == value)
            return allowed;
    }
    return std::nullopt;
}

std::string default_option(const mutator_picker::OptionSpec& spec) {
    if (spec.type == mutator_picker::OptionSpec::Type::number) {
        const std::vector<std::string> values = mutator_picker::number_values(spec);
        for (const std::string& allowed : values) {
            if (allowed == spec.fallback)
                return allowed;
        }
        if (!values.empty())
            return values.front();
        return spec.fallback;
    }
    for (const std::string& allowed : spec.values) {
        if (allowed == spec.fallback)
            return spec.fallback;
    }
    if (!spec.values.empty())
        return spec.values.front();
    return spec.fallback;
}

std::vector<std::pair<std::string, std::string>> normalize_options(
    const Brain& brain, const std::vector<std::pair<std::string, std::string>>& given
) {
    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(brain.options.size());
    for (const mutator_picker::OptionSpec& spec : brain.options) {
        std::string value = default_option(spec);
        for (const auto& [option_id, stored] : given) {
            if (option_id != spec.id)
                continue;
            if (std::optional<std::string> kept = kept_option(spec, stored))
                value = std::move(*kept);
            break;
        }
        options.emplace_back(spec.id, std::move(value));
    }
    return options;
}

} // namespace detail

void open(Picker& picker, Inputs inputs) {
    const Choice current = inputs.current;
    const bool fixed = inputs.built_in_level_fixed;
    const data::brain_library::Difficulty setup = inputs.setup_difficulty;
    Picker next;
    next.inputs = std::move(inputs);

    const Brain* held = detail::find_brain(next, current.brain);
    const bool built_in = current.brain.empty();
    if (!built_in && (held == nullptr || !detail::can_play(*held))) {
        next.left_out = current.brain;
        detail::choose_built_in(next, detail::opened_built_in_level(fixed, setup, current.level));
        picker = std::move(next);
        return;
    }
    if (built_in) {
        detail::choose_built_in(next, detail::opened_built_in_level(fixed, setup, current.level));
        picker = std::move(next);
        return;
    }
    std::string level = current.level;
    if (!detail::has_level(*held, level))
        level = detail::default_level_id(*held, setup);
    detail::choose_brain(
        next, *held, std::move(level), detail::normalize_options(*held, current.options)
    );
    picker = std::move(next);
}

bool select(Picker& picker, std::string_view id) {
    if (!detail::listed(picker, id))
        return false;
    const Brain* brain = detail::find_brain(picker, id);
    const bool locked = brain != nullptr && !detail::can_play(*brain);
    if (picker.selected == id && (locked || picker.choice.brain == id))
        return false;
    picker.selected = std::string(id);
    if (locked || picker.choice.brain == id)
        return true;
    if (id.empty()) {
        detail::choose_built_in(
            picker, std::string(detail::difficulty_id(picker.inputs.setup_difficulty))
        );
        return true;
    }
    detail::choose_brain(
        picker,
        *brain,
        detail::default_level_id(*brain, picker.inputs.setup_difficulty),
        detail::normalize_options(*brain, {})
    );
    return true;
}

bool set_level(Picker& picker, std::string_view level) {
    if (picker.choice.brain.empty()) {
        if (picker.inputs.built_in_level_fixed || !detail::built_in_level(level))
            return false;
        if (picker.choice.level == level)
            return false;
        picker.choice.level = std::string(level);
        return true;
    }
    const Brain* brain = detail::find_brain(picker, picker.choice.brain);
    if (brain == nullptr || !detail::has_level(*brain, level))
        return false;
    if (picker.choice.level == level)
        return false;
    picker.choice.level = std::string(level);
    return true;
}

bool set_option(Picker& picker, std::string_view option, std::string_view value) {
    const Brain* brain = detail::find_brain(picker, picker.choice.brain);
    if (brain == nullptr)
        return false;
    const mutator_picker::OptionSpec* spec = detail::find_option(*brain, option);
    if (spec == nullptr)
        return false;
    std::optional<std::string> kept = detail::kept_option(*spec, value);
    if (!kept)
        return false;
    for (auto& [option_id, stored] : picker.choice.options) {
        if (option_id != option)
            continue;
        if (stored == *kept)
            return false;
        stored = std::move(*kept);
        return true;
    }
    picker.choice.options.emplace_back(std::string(option), std::move(*kept));
    return true;
}

bool set_every_seat(Picker& picker, bool every) {
    if (!picker.inputs.offer_every_seat || picker.every_seat == every)
        return false;
    picker.every_seat = every;
    return true;
}

Choice result(const Picker& picker) {
    const Brain* brain = detail::find_brain(picker, picker.choice.brain);
    if (brain == nullptr)
        return picker.choice;
    Choice choice = picker.choice;
    choice.options = detail::normalize_options(*brain, picker.choice.options);
    return choice;
}

} // namespace oa::ui::brain_picker
