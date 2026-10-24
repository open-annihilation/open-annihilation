// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A setup's computer seats: read from the preferences when the setup opens,
// written back at START, and handed to the match before its first tick. A
// seat that names no brain stays with the built-in player. A seat that names
// none takes the default brain. A brain that is not installed, or that does
// not fit the game being played, is not seated.

#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "library_host.hpp"

#include "oa/app/brain_seats.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/library/library.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace brain = oa::data::brain_library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace oamod = oa::formats::oamod;
namespace settings = oa::ui::engine_settings;
namespace library = oa::ui::library;

namespace {

/// The preference key of one setup's seats.
///
/// @param setup the setup
/// @return the key
constexpr std::string_view seats_key(brain::Setup setup) noexcept {
    switch (setup) {
    case brain::Setup::skirmish:
        return settings::key::brains_skirmish;
    case brain::Setup::multiplayer:
        return settings::key::brains_multiplayer;
    case brain::Setup::campaign:
        return settings::key::brains_campaign;
    }
    return settings::key::brains_skirmish;
}

static_assert(seats_key(brain::Setup::skirmish) == settings::key::brains_skirmish);
static_assert(seats_key(brain::Setup::multiplayer) == settings::key::brains_multiplayer);
static_assert(seats_key(brain::Setup::campaign) == settings::key::brains_campaign);

/// The place of one setup in the runtime's three records.
///
/// @param setup the setup
/// @return 0, 1 or 2
std::size_t setup_index(brain::Setup setup) noexcept {
    switch (setup) {
    case brain::Setup::skirmish:
        return 0;
    case brain::Setup::multiplayer:
        return 1;
    case brain::Setup::campaign:
        return 2;
    }
    return 0;
}

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

/// The income a seat's level plays at.
///
/// @param difficulty the level's difficulty
/// @return OA_DIFFICULTY_EASY, MEDIUM or HARD
int32_t income_of(brain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case brain::Difficulty::easy:
        return OA_DIFFICULTY_EASY;
    case brain::Difficulty::medium:
        return OA_DIFFICULTY_MEDIUM;
    case brain::Difficulty::hard:
        return OA_DIFFICULTY_HARD;
    }
    return OA_DIFFICULTY_MEDIUM;
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

/// The seat of one place, or null.
///
/// @param seats the setup's seats
/// @param place the player, 0 to 9
/// @return the seat, or null
const brain::Seat* seat_at(const brain::Seats& seats, uint8_t place) noexcept {
    for (const brain::Seat& seat : seats.seats)
        if (seat.place == static_cast<int32_t>(place))
            return &seat;
    return nullptr;
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

/// Says the built-in player plays this seat.
///
/// @param player the player
/// @param id the brain that was not seated
/// @param reason why
void log_builtin(uint8_t player, std::string_view id, std::string_view reason) {
    std::cerr << "open-annihilation: brains: player " << static_cast<int>(player) << ": " << id
              << ": " << reason << "; the built-in player plays\n";
}

/// The sentence a refused seat carries.
///
/// @param outcome what seating returned
/// @return the sentence
std::string failure_text(const oa::sim::brains::SeatOutcome& outcome) {
    if (outcome.reason[0] != '\0')
        return std::string(outcome.reason.data());
    const std::string described = oa::data::script_host::describe(outcome.error);
    if (!described.empty())
        return described;
    return "the brain could not be seated";
}

} // namespace

std::optional<brain::Seats> Runtime::remembered_brain_seats(brain::Setup setup) const {
    const auto found = preference_values_.find(std::string(seats_key(setup)));
    if (found == preference_values_.end())
        return std::nullopt;
    return brain::parse_seats(found->second);
}

void Runtime::remember_brain_seats(brain::Setup setup, const brain::Seats& seats) {
    bool names_brain = false;
    for (const brain::Seat& seat : seats.seats)
        if (!seat.brain.empty())
            names_brain = true;
    const std::string key{seats_key(setup)};
    if (!names_brain && preference_values_.find(key) == preference_values_.end())
        return;
    try {
        preference_values_[key] = brain::seats_text(seats);
        setup_brain_seats(setup) = seats;
        preferences_dirty_ = true;
        save_preferences();
    } catch (const std::exception& error) {
        if (brain_seats_failure_logged_)
            return;
        brain_seats_failure_logged_ = true;
        std::cerr << "open-annihilation: brains: the seats could not be stored: " << error.what()
                  << '\n';
    } catch (...) {
        if (brain_seats_failure_logged_)
            return;
        brain_seats_failure_logged_ = true;
        std::cerr << "open-annihilation: brains: the seats could not be stored\n";
    }
}

brain::Seats& Runtime::setup_brain_seats(brain::Setup setup) {
    return setup_brain_seats_[setup_index(setup)];
}

std::vector<BrainSeat> Runtime::resolve_brain_seats(
    const brain::Seats& seats,
    std::span<const uint8_t> computer_players,
    brain::Difficulty setup_difficulty
) {
    library::Inputs inputs;
    inputs.engine = content::this_engine_rules().engine;
    const EngineSettingsState& mods = engine_settings_state();
    if (!mods.playing_mod_folder.empty()) {
        for (std::size_t index = 0;
             index < mods.mod_folders.size() && index < mods.mod_names.size() &&
             index < mods.mod_details.size();
             ++index) {
            if (mods.mod_folders[index] != mods.playing_mod_folder)
                continue;
            const EngineSettingsState::OfferedMod& details = mods.mod_details[index];
            inputs.played.id = details.profile_id;
            inputs.played.name = mods.mod_names[index];
            if (details.version != mod_summary_no_version)
                inputs.played.version = details.version;
            break;
        }
    }
    inputs.content.enabled = content_choice().expansions;

    const brain::Listing& listing = installed_brains();
    const std::string fallback_id = default_brain();
    std::vector<BrainSeat> play;
    for (const uint8_t player : computer_players) {
        const brain::Seat* own = seat_at(seats, player);
        if (own != nullptr && own->brain.empty())
            continue;
        const std::string id = own != nullptr ? own->brain : fallback_id;
        if (id.empty())
            continue;
        const brain::InstalledBrain* installed = brain::find(listing, id);
        if (installed == nullptr) {
            log_builtin(player, id, "it is not installed");
            continue;
        }
        const std::string unfit = library::brain_unfit_reason(
            inputs, brain_facts_of(installed->manifest, installed->network_bytes)
        );
        if (!unfit.empty()) {
            log_builtin(player, id, unfit);
            continue;
        }
        const oabrain::Level* level = nullptr;
        if (own != nullptr)
            level = oabrain::find_level(installed->manifest, own->level);
        if (level == nullptr) {
            const std::string chosen = level_for(installed->manifest, setup_difficulty);
            level = oabrain::find_level(installed->manifest, chosen);
        }
        if (level == nullptr) {
            log_builtin(player, id, "the brain has no level");
            continue;
        }
        BrainSeat resolved;
        resolved.player = player;
        resolved.folder = installed->folder;
        resolved.id = installed->manifest.id;
        resolved.version = oamut::version_text(installed->manifest.version);
        resolved.package_sha256 = installed->package_sha256;
        resolved.level = level->id;
        resolved.difficulty = library_difficulty(level->difficulty);
        if (own != nullptr) {
            for (const auto& named : own->options) {
                const oamut::Option* option = option_named(installed->manifest, named.first);
                if (option == nullptr)
                    continue;
                std::optional<std::string> text = accepted_text(*option, named.second);
                if (!text)
                    text = default_text(*option);
                if (!text->empty())
                    resolved.options.emplace_back(named.first, std::move(*text));
            }
        }
        play.push_back(std::move(resolved));
    }
    return play;
}

void Runtime::seat_brains_once(std::vector<BrainSeat> seats) {
    pending_brain_seats_ = std::move(seats);
}

void Runtime::cancel_brain_seats_once() noexcept {
    pending_brain_seats_.reset();
}

bool Runtime::brain_seats_held_for_next_match() const noexcept {
    return pending_brain_seats_.has_value();
}

const std::vector<BrainSeat>& Runtime::seated_brains() const noexcept {
    return seated_brains_;
}

void Runtime::hand_setup_brain_seats(brain::Setup setup) {
    std::vector<uint8_t> computers;
    const std::size_t limit = skirmish_settings_.slots.size() < brain::most_seats
                                  ? skirmish_settings_.slots.size()
                                  : brain::most_seats;
    for (std::size_t place = 0; place < limit; ++place) {
        if (skirmish_settings_.slots[place].controller == entry::controller::computer)
            computers.push_back(static_cast<uint8_t>(place));
    }
    const brain::Difficulty difficulty = difficulty_of(preferences_.difficulty);
    const brain::Seats& current = setup_brain_seats(setup);
    std::vector<BrainSeat> play = resolve_brain_seats(current, computers, difficulty);

    const std::string fallback_id = default_brain();
    const brain::InstalledBrain* fallback = brain::find(installed_brains(), fallback_id);
    brain::Seats memory;
    for (const uint8_t player : computers) {
        if (const brain::Seat* own = seat_at(current, player)) {
            memory.seats.push_back(*own);
            continue;
        }
        brain::Seat taken;
        taken.place = static_cast<int32_t>(player);
        if (fallback != nullptr) {
            taken.brain = fallback->manifest.id;
            taken.level = level_for(fallback->manifest, difficulty);
        }
        memory.seats.push_back(std::move(taken));
    }

    cancel_brain_seats_once();
    seat_brains_once(std::move(play));
    remember_brain_seats(setup, memory);
}

void Runtime::take_pending_brain_seats() {
    if (!pending_brain_seats_ || match_ == nullptr)
        return;
    std::vector<BrainSeat> pending = std::move(*pending_brain_seats_);
    pending_brain_seats_.reset();
    const std::filesystem::path brains_folder = user_folder() / std::string(brains_folder_name);
    std::vector<BrainSeat> seated;
    for (BrainSeat& seat : pending) {
        if (seat.player >= OA_PLAYER_COUNT)
            continue;
        const oa::Player& record = match_->state().game.players[seat.player];
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        try {
            std::string error;
            const std::optional<InstalledBrain> installed =
                read_installed_brain(brains_folder, seat.id, error);
            if (!installed) {
                log_builtin(
                    seat.player, seat.id, error.empty() ? "the brain could not be read" : error
                );
                oa::sim::ai::computer_players_set_difficulty(
                    oa::sim::ai::match_computer_players(*match_),
                    seat.player,
                    income_of(seat.difficulty)
                );
                continue;
            }
            BrainSeatChoice choice;
            choice.id = seat.id;
            choice.level = seat.level;
            choice.options = seat.options;
            ResolvedSeat resolved;
            if (!resolve_seat(*installed, choice, resolved, error)) {
                log_builtin(
                    seat.player, seat.id, error.empty() ? "the brain could not be seated" : error
                );
                oa::sim::ai::computer_players_set_difficulty(
                    oa::sim::ai::match_computer_players(*match_),
                    seat.player,
                    income_of(seat.difficulty)
                );
                continue;
            }
            std::vector<oa::sim::brains::PackageFile> files;
            std::vector<oa::sim::brains::OptionValue> options;
            std::vector<oa::sim::brains::NetworkFile> networks;
            oa::sim::brains::SeatSpec spec =
                seat_spec(seat.player, *installed, resolved, files, options, networks);
            if (seat.saved.has_value() && !seat.saved->empty())
                spec.saved = *seat.saved;
            prepare_brain_report(
                seat.player,
                installed->manifest.name,
                resolved.version,
                installed->manifest.id,
                resolved.level
            );
            const oa::sim::brains::SeatOutcome outcome = oa::sim::brains::seat_brain(*match_, spec);
            if (outcome.result != oa::sim::brains::SeatResult::seated) {
                log_builtin(seat.player, seat.id, failure_text(outcome));
                report_unseated_brain(seat.player, installed->manifest.name, outcome);
                oa::sim::ai::computer_players_set_difficulty(
                    oa::sim::ai::match_computer_players(*match_),
                    seat.player,
                    income_of(seat.difficulty)
                );
                continue;
            }
            std::cerr << "open-annihilation: brains: player " << static_cast<int>(seat.player)
                      << " plays " << seat.id << ' ' << resolved.version << " at " << resolved.level
                      << '\n';
            seat.version = resolved.version;
            seat.level = resolved.level;
            seated.push_back(std::move(seat));
        } catch (const std::exception& error) {
            const char* text = error.what();
            log_builtin(
                seat.player, seat.id, text != nullptr ? text : "the brain could not be seated"
            );
            oa::sim::ai::computer_players_set_difficulty(
                oa::sim::ai::match_computer_players(*match_),
                seat.player,
                income_of(seat.difficulty)
            );
        }
    }
    seated_brains_ = std::move(seated);
    attach_brain_chat();
}

} // namespace oa::app
