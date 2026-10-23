// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A saved game's computer players. The save names each seated brain and keeps
// the plain data its save() returned. A load seats those brains before the
// first tick, or asks to play the built-in player when one is not installed.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/core/game_state.h"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/persist/hapibank.hpp"
#include "oa/data/persist/save_brains.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/ui/brain_picker/prompts.hpp"
#include "oa/ui/library/text.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace brain = oa::data::brain_library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace persist = oa::data::persist;
namespace picker = oa::ui::brain_picker;
namespace library = oa::ui::library;
namespace kit = oa::ui::kit;

namespace {

constexpr std::string_view kUnreadable =
    "The computer players this game was saved with cannot be read.";
constexpr std::string_view kNeeds =
    "This saved game needs {name} {version}, which is not installed.";

/// The seat difficulty a preference difficulty names.
///
/// @param difficulty the preference, 0 easy, 2 hard, and medium otherwise
/// @return the difficulty
brain::Difficulty difficulty_of(uint32_t difficulty) noexcept {
    if (difficulty == static_cast<uint32_t>(OA_DIFFICULTY_EASY))
        return brain::Difficulty::easy;
    if (difficulty == static_cast<uint32_t>(OA_DIFFICULTY_HARD))
        return brain::Difficulty::hard;
    return brain::Difficulty::medium;
}

/// Maps a manifest difficulty onto the seat record's.
///
/// @param difficulty the manifest's difficulty
/// @return the seat difficulty
brain::Difficulty library_difficulty(oabrain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case oabrain::Difficulty::easy:
        return brain::Difficulty::easy;
    case oabrain::Difficulty::medium:
        return brain::Difficulty::medium;
    case oabrain::Difficulty::hard:
        return brain::Difficulty::hard;
    }
    return brain::Difficulty::medium;
}

/// Tells whether a number lies on the option's step.
///
/// @param value the number
/// @param minimum the option's minimum
/// @param step the option's step
/// @return true when it does
bool lies_on_step(
    const oamod::Number& value, const oamod::Number& minimum, const oamod::Number& step
) {
    int64_t value_i = 0;
    int64_t minimum_i = 0;
    int64_t step_i = 0;
    if (oamod::integer_value(value, value_i) && oamod::integer_value(minimum, minimum_i) &&
        oamod::integer_value(step, step_i) && step_i > 0)
        return (value_i - minimum_i) % step_i == 0;
    const double step_count = oamod::number_to_double(step);
    if (step_count == 0.0)
        return false;
    const double taken =
        (oamod::number_to_double(value) - oamod::number_to_double(minimum)) / step_count;
    const double nearest = std::round(taken);
    return std::fabs(taken - nearest) <= 1e-8;
}

/// The option of an id, or null.
///
/// @param manifest the brain
/// @param id the option's id
/// @return the option, or null
const oamut::Option* option_named(const oabrain::Manifest& manifest, std::string_view id) noexcept {
    for (const oamut::Option& option : manifest.options)
        if (option.id == id)
            return &option;
    return nullptr;
}

/// The canonical text of a value the option allows, or nothing.
///
/// @param option the option
/// @param text the player's text
/// @return the canonical text, or nothing when the option refuses it
std::optional<std::string> accepted_text(const oamut::Option& option, std::string_view text) {
    if (option.type == oamut::OptionType::toggle) {
        if (text == "true" || text == "false")
            return std::string{text};
        return std::nullopt;
    }
    if (option.type == oamut::OptionType::choice) {
        oamod::Number parsed{};
        const bool parsed_ok = oamod::parse_number(text, parsed) == oamod::NumberStatus::ok;
        for (const oamut::ChoiceValue& listed : option.values) {
            const bool matches =
                listed.number
                    ? parsed_ok && oamod::compare_numbers(parsed, listed.number_value) == 0
                    : listed.text == text;
            if (matches)
                return listed.text;
        }
        return std::nullopt;
    }
    oamod::Number parsed{};
    if (oamod::parse_number(text, parsed) != oamod::NumberStatus::ok)
        return std::nullopt;
    if (oamod::compare_numbers(parsed, option.minimum) < 0 ||
        oamod::compare_numbers(parsed, option.maximum) > 0)
        return std::nullopt;
    if (!lies_on_step(parsed, option.minimum, option.step))
        return std::nullopt;
    return oamod::canonical_number_text(parsed);
}

/// The canonical text of an option's default.
///
/// @param option the option
/// @return the text; empty when a choice lists nothing
std::string default_text(const oamut::Option& option) {
    if (option.type == oamut::OptionType::toggle)
        return option.toggle_default ? "true" : "false";
    if (option.type == oamut::OptionType::number)
        return oamod::canonical_number_text(option.number_default);
    if (option.values.empty())
        return {};
    const std::size_t index =
        option.default_index < option.values.size() ? option.default_index : 0;
    return option.values[index].text;
}

/// The level id `default_level` chooses.
///
/// @param manifest the brain
/// @param wanted the setup's difficulty
/// @return the level id; empty when the brain has no level
std::string level_for(const oabrain::Manifest& manifest, brain::Difficulty wanted) {
    if (manifest.levels.empty())
        return {};
    std::vector<brain::Difficulty> levels;
    levels.reserve(manifest.levels.size());
    for (const oabrain::Level& level : manifest.levels)
        levels.push_back(library_difficulty(level.difficulty));
    const std::size_t index = brain::default_level(levels, wanted);
    if (index >= manifest.levels.size())
        return {};
    return manifest.levels[index].id;
}

/// Copies one saved state into the caller's buffer.
///
/// @param context a vector the bytes are copied into
/// @param bytes the state's bytes
void accept_state(void* context, std::span<const uint8_t> bytes) {
    static_cast<std::vector<uint8_t>*>(context)->assign(bytes.begin(), bytes.end());
}

/// The state saved for one player, or null.
///
/// @param states the account's states
/// @param player the player
/// @return the state, or null
const persist::SavedBrainState*
state_for(std::span<const persist::SavedBrainState> states, uint8_t player) noexcept {
    for (const persist::SavedBrainState& state : states)
        if (state.player == player)
            return &state;
    return nullptr;
}

/// The sentence that names a brain the save needs.
///
/// @param name the name players see, or the id
/// @param version the version the save recorded
/// @return the sentence, in the language shown
std::string needs_sentence(std::string_view name, std::string_view version) {
    std::string sentence{oa::data::languages::interface_text(kNeeds)};
    const auto replace = [&](std::string_view place, std::string_view value) {
        const std::size_t at = sentence.find(place);
        if (at != std::string::npos)
            sentence.replace(at, place.size(), value);
    };
    replace("{name}", name);
    replace("{version}", version);
    return sentence;
}

/// The difficulty a save was played at, or the preference when it names none.
///
/// @param bank the save
/// @param preference the preference difficulty
/// @return the difficulty
brain::Difficulty saved_difficulty(persist::Bank* bank, uint32_t preference) {
    if (persist::bank_open_account(bank, persist::save_key::summary) &&
        persist::bank_has_field(bank, persist::save_key::difficulty)) {
        const int32_t stored = persist::bank_get_int(bank, persist::save_key::difficulty, 0);
        if (stored >= 0)
            return difficulty_of(static_cast<uint32_t>(stored));
    }
    return difficulty_of(preference);
}

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// One save file's bank, destroyed with the guard.
struct BankGuard {
    persist::Bank bank{};

    BankGuard() { persist::bank_init(&bank); }

    ~BankGuard() { persist::bank_destroy(&bank); }

    BankGuard(const BankGuard&) = delete;
    BankGuard& operator=(const BankGuard&) = delete;
};

} // namespace

void Runtime::save_brains(persist::Bank* bank) {
    if (seated_brains_.empty())
        return;
    brain::Seats seats;
    std::vector<persist::SavedBrainState> states;
    seats.seats.reserve(seated_brains_.size());
    const brain::Listing& listing = installed_brains();
    for (const BrainSeat& seated : seated_brains_) {
        brain::Seat named;
        named.place = static_cast<int32_t>(seated.player);
        named.brain = seated.id;
        named.version = seated.version;
        named.package_sha256 = seated.package_sha256;
        named.level = seated.level;
        named.options = seated.options;
        if (const brain::InstalledBrain* installed = brain::find(listing, seated.id))
            named.name = installed->manifest.name;
        seats.seats.push_back(std::move(named));
        if (match_ == nullptr)
            continue;
        std::vector<uint8_t> bytes;
        oa::sim::brains::SaveHooks hooks;
        hooks.context = &bytes;
        hooks.accept = accept_state;
        const oa::sim::brains::SaveOutcome outcome =
            oa::sim::brains::save_brain_state(*match_, seated.player, hooks);
        if (outcome != oa::sim::brains::SaveOutcome::stored || bytes.empty())
            continue;
        persist::SavedBrainState state;
        state.player = seated.player;
        state.bytes = std::move(bytes);
        states.push_back(std::move(state));
    }
    const std::string text = brain::seats_text(seats);
    if (!persist::save_write_brains(bank, text, states))
        throw std::runtime_error("the computer players' account cannot be saved");
}

bool Runtime::check_saved_brains(persist::Bank* bank) {
    const bool play_builtin = load_missing_brains_as_builtin_;
    load_missing_brains_as_builtin_ = false;
    cancel_brain_seats_once();
    loaded_brain_seats_ = {};
    const std::optional<persist::SavedBrains> saved = persist::save_read_brains(bank);
    if (!saved) {
        seat_brains_once({});
        return true;
    }
    const std::optional<brain::Seats> parsed = brain::parse_seats(saved->seats_text);
    if (!parsed) {
        status_ = std::string(oa::data::languages::interface_text(kUnreadable));
        return false;
    }
    const brain::Difficulty setup_difficulty = saved_difficulty(bank, preferences_.difficulty);
    const brain::Listing& listing = installed_brains();
    std::vector<BrainSeat> play;
    brain::Seats handed;
    for (const brain::Seat& seat : parsed->seats) {
        if (seat.brain.empty())
            continue;
        const brain::InstalledBrain* installed = brain::find(listing, seat.brain);
        if (installed == nullptr) {
            if (!play_builtin) {
                const std::string_view name =
                    seat.name.empty() ? std::string_view{seat.brain} : std::string_view{seat.name};
                status_ = needs_sentence(name, seat.version);
                return false;
            }
            continue;
        }
        const oabrain::Level* level = oabrain::find_level(installed->manifest, seat.level);
        if (level == nullptr) {
            const std::string chosen = level_for(installed->manifest, setup_difficulty);
            level = oabrain::find_level(installed->manifest, chosen);
        }
        if (level == nullptr)
            continue;
        BrainSeat resolved;
        resolved.player = static_cast<uint8_t>(seat.place);
        resolved.folder = installed->folder;
        resolved.id = installed->manifest.id;
        resolved.version = oamut::version_text(installed->manifest.version);
        resolved.package_sha256 = installed->package_sha256;
        resolved.level = level->id;
        resolved.difficulty = library_difficulty(level->difficulty);
        for (const auto& named : seat.options) {
            const oamut::Option* option = option_named(installed->manifest, named.first);
            if (option == nullptr)
                continue;
            std::optional<std::string> text = accepted_text(*option, named.second);
            if (!text)
                text = default_text(*option);
            if (text && !text->empty())
                resolved.options.emplace_back(named.first, std::move(*text));
        }
        const persist::SavedBrainState* state = state_for(saved->states, resolved.player);
        if (state != nullptr) {
            if (seat.version == resolved.version && seat.package_sha256 == resolved.package_sha256)
                resolved.saved = state->bytes;
            else
                std::cerr << "open-annihilation: brains: player "
                          << static_cast<int>(resolved.player)
                          << ": the saved state was left out: it was saved by " << seat.version
                          << '\n';
        }
        brain::Seat record;
        record.place = seat.place;
        record.brain = resolved.id;
        record.version = resolved.version;
        record.package_sha256 = resolved.package_sha256;
        record.level = resolved.level;
        record.name = installed->manifest.name;
        record.options = resolved.options;
        handed.seats.push_back(std::move(record));
        play.push_back(std::move(resolved));
    }
    loaded_brain_seats_ = std::move(handed);
    seat_brains_once(std::move(play));
    return true;
}

const brain::Seats& Runtime::loaded_brain_seats() const noexcept {
    return loaded_brain_seats_;
}

bool Runtime::offer_builtin_brain_load(const std::filesystem::path& path) {
    BankGuard guard;
    persist::BankError error{};
    const persist::FileSource source = persist::stdio_file_source();
    if (!persist::bank_read_file(
            &guard.bank,
            path_to_utf8(path).c_str(),
            persist::savegame_description,
            nullptr,
            &source,
            &error
        ))
        return false;
    const std::optional<persist::SavedBrains> saved = persist::save_read_brains(&guard.bank);
    if (!saved)
        return false;
    const std::optional<brain::Seats> parsed = brain::parse_seats(saved->seats_text);
    if (!parsed)
        return false;
    const brain::Listing& listing = installed_brains();
    std::vector<picker::MissingBrain> missing;
    for (const brain::Seat& seat : parsed->seats) {
        if (seat.brain.empty() || brain::find(listing, seat.brain) != nullptr)
            continue;
        picker::MissingBrain one;
        one.name = seat.name.empty() ? seat.brain : seat.name;
        one.version = seat.version;
        one.player = seat.place;
        missing.push_back(std::move(one));
    }
    if (missing.empty())
        return false;
    const library::TextHooks hooks{nullptr, shown_word};
    kit::Question question = picker::load_built_in_question(missing, hooks);
    int32_t load_button = 1;
    for (int32_t index = 0; index < static_cast<int32_t>(question.buttons.size()); ++index) {
        if (question.buttons[static_cast<std::size_t>(index)].id == picker::load_built_in_id)
            load_button = index;
    }
    const std::filesystem::path stored = path;
    QuestionScreen::Host answers;
    answers.answer = [this, stored, load_button](int32_t button) {
        if (button != load_button)
            return true;
        load_missing_brains_as_builtin_ = true;
        std::ignore = load_saved_game(stored);
        return true;
    };
    OaLayer& layer = oa_layer();
    layer.push(std::make_unique<QuestionScreen>(layer, screen_, std::move(question), answers));
    return true;
}

} // namespace oa::app
