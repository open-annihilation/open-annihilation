// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutators a run chose, applied at every match start and released when
// the match ends or the start fails. An empty choice changes nothing.

#include "oa/app/runtime.hpp"

#include "mutator_state.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/clashes.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine for a mutator's requirement"
#endif

namespace oa::app {

namespace mutators = oa::data::mutators;

static_assert(mutators::unit_sync_limit == oa::ui::frontend_multiplayer::kMaxSyncUnits);

namespace {

/// The phase word a log line uses.
///
/// @param phase the phase
/// @return its name
const char* phase_name(mutators::Phase phase) {
    switch (phase) {
    case mutators::Phase::loadout:
        return "loadout";
    case mutators::Phase::manifest:
        return "manifest";
    case mutators::Phase::plan:
        return "plan";
    case mutators::Phase::files:
        return "files";
    case mutators::Phase::rules:
        return "rules";
    case mutators::Phase::hooks:
        return "hooks";
    case mutators::Phase::integrity:
        return "integrity";
    }
    return "loadout";
}

/// The seed every machine shares for one mod and one mutator list.
///
/// The first eight bytes, little-endian, of the SHA-256 of the simulation
/// hash's hexadecimal digits followed by each mutator's id, a NUL, its
/// version and a NUL.
///
/// @param runtime the run
/// @param chosen the mutators, top first
/// @return the seed
uint64_t
mutator_seed(const Runtime& runtime, const std::vector<mutators::ResolvedMutator>& chosen) {
    std::string material = simulation_hash(runtime);
    for (const mutators::ResolvedMutator& one : chosen) {
        material += one.manifest.id;
        material.push_back('\0');
        material += oa::formats::oamut::version_text(one.manifest.version);
        material.push_back('\0');
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(material.size());
    for (const char byte : material)
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(byte)));
    const oa::base::sha256::Digest digest = oa::base::sha256::digest_of(bytes);
    uint64_t seed = 0;
    for (int index = 0; index < 8; ++index) {
        seed |= static_cast<uint64_t>(digest[static_cast<std::size_t>(index)])
                << (static_cast<unsigned>(index) * 8U);
    }
    return seed;
}

/// The clash mode a mutator plays with when neither the player nor its
/// manifest names one: the `mutators.clash-default` preference.
///
/// The match start reads it here. The battle room reads the same preference
/// when it digests and sends the list, so a room's layer and digest are the
/// ones its match start makes.
///
/// @param values the preferences
/// @return the mode
oa::formats::oamut::ClashMode clash_default_of(const std::map<std::string, std::string>& values) {
    const auto stored = values.find(std::string{mutators::clash_default_preference});
    return stored == values.end() ? mutators::clash_setting_default
                                  : mutators::clash_from_setting(stored->second);
}

/// Why a result cannot be played.
///
/// A refusal is that text. Otherwise the mutator `first_blocking` names,
/// by its manifest name and its id, with that mutator's own reason. A
/// mutator that failed alone is not named while another failure stops the
/// start. When nothing else stops it, the first mutator that did not apply
/// is named.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return the reason
std::string failure_text(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    if (!result.refusal.empty())
        return one_line(result.refusal);
    std::vector<mutators::PipelineMutator> listed;
    listed.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen) {
        mutators::PipelineMutator item;
        item.manifest = one.manifest;
        listed.push_back(std::move(item));
    }
    const auto named = [&](std::size_t index) {
        const mutators::MutatorResult& one = result.mutators[index];
        std::string shown = one.id;
        std::string id = one.id;
        if (index < chosen.size()) {
            id = chosen[index].manifest.id;
            if (!chosen[index].manifest.name.empty())
                shown = chosen[index].manifest.name;
        }
        return shown + " (" + id + "): " + phase_name(one.phase) + ": " + one_line(one.message);
    };
    const std::optional<std::size_t> stopper = mutators::first_blocking(result, listed);
    if (stopper.has_value() && *stopper < result.mutators.size())
        return named(*stopper);
    for (std::size_t index = 0; index < result.mutators.size(); ++index) {
        if (result.mutators[index].outcome != mutators::Outcome::applied)
            return named(index);
    }
    return "the mutators did not apply";
}

const char* outcome_word(mutators::Outcome outcome);

/// The mutators a stopped start asks about.
///
/// A refusal names no mutator. Every other mutator that did not apply is
/// named by its manifest, or by its id, with its own reason and not the
/// phase.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return those stops; empty when every mutator applied
std::vector<MutatorStartStop> stops_of(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    std::vector<MutatorStartStop> stops;
    if (!result.refusal.empty()) {
        MutatorStartStop stop;
        stop.reason = one_line(result.refusal);
        stops.push_back(std::move(stop));
        return stops;
    }
    const std::size_t count =
        chosen.size() < result.mutators.size() ? chosen.size() : result.mutators.size();
    for (std::size_t index = 0; index < count; ++index) {
        const mutators::MutatorResult& one = result.mutators[index];
        if (one.outcome == mutators::Outcome::applied)
            continue;
        MutatorStartStop stop;
        stop.id = one.id;
        if (!chosen[index].manifest.id.empty())
            stop.id = chosen[index].manifest.id;
        stop.name = chosen[index].manifest.name.empty() ? stop.id : chosen[index].manifest.name;
        stop.reason = one_line(one.message);
        if (stop.reason.empty())
            stop.reason = outcome_word(one.outcome);
        stops.push_back(std::move(stop));
    }
    return stops;
}

/// The mutators of one run whose outcome is applied, in that order.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return those mutators
std::vector<mutators::ResolvedMutator> mutators_that_applied(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    std::vector<mutators::ResolvedMutator> kept;
    const std::size_t count =
        chosen.size() < result.mutators.size() ? chosen.size() : result.mutators.size();
    kept.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (result.mutators[index].outcome != mutators::Outcome::applied)
            continue;
        kept.push_back(chosen[index]);
    }
    return kept;
}

/// The ids a run left out, in list order.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return those ids, separated by commas
std::string mutators_left_out(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    std::string omitted;
    const std::size_t count =
        chosen.size() < result.mutators.size() ? chosen.size() : result.mutators.size();
    for (std::size_t index = 0; index < count; ++index) {
        if (result.mutators[index].outcome == mutators::Outcome::applied)
            continue;
        if (!omitted.empty())
            omitted += ", ";
        omitted += result.mutators[index].id;
    }
    for (std::size_t index = count; index < chosen.size(); ++index) {
        if (!omitted.empty())
            omitted += ", ";
        omitted += chosen[index].manifest.id;
    }
    if (omitted.empty())
        omitted = "a mutator";
    return omitted;
}

/// The outcome word a log line uses when a mutator left no message.
///
/// @param outcome the outcome
/// @return its name
const char* outcome_word(mutators::Outcome outcome) {
    switch (outcome) {
    case mutators::Outcome::applied:
        return "applied";
    case mutators::Outcome::failed:
        return "failed";
    case mutators::Outcome::incompatible:
        return "incompatible";
    case mutators::Outcome::excluded:
        return "excluded";
    }
    return "excluded";
}

/// Logs what one run applied, each conflict and each mutator that did not.
///
/// A refused list is one line and nothing about each mutator. The applied
/// line is written only when a mutator applied. A mutator with no message
/// is named by its outcome.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
void log_applied(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    if (!result.refusal.empty()) {
        std::cerr << "open-annihilation: mutators: refused: " << one_line(result.refusal) << '\n';
        return;
    }
    std::string names;
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome != mutators::Outcome::applied)
            continue;
        if (!names.empty())
            names += ", ";
        names += one.id;
        names += ' ';
        names += one.version;
    }
    if (!names.empty()) {
        std::size_t added = 0;
        std::size_t hidden = 0;
        std::set<std::string> rewritten;
        for (const mutators::JournalEntry& entry : result.journal.entries) {
            if (entry.change == mutators::Change::added) {
                if (entry.new_text != "shared")
                    ++added;
            } else if (entry.change == mutators::Change::hidden) {
                ++hidden;
            } else if (
                entry.change == mutators::Change::set ||
                entry.change == mutators::Change::removed_field ||
                entry.change == mutators::Change::removed
            ) {
                rewritten.insert(entry.path.empty() ? entry.record : entry.path);
            }
        }
        std::string line = "open-annihilation: mutators: applied ";
        line += names;
        line += " (";
        line += std::to_string(added);
        line += " files added, ";
        line += std::to_string(hidden);
        line += " hidden, ";
        line += std::to_string(rewritten.size());
        line += " rewritten)";
        std::cerr << line << '\n';
    }

    std::vector<std::string> ids;
    ids.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen)
        ids.push_back(one.manifest.id);
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        const std::string text = mutators::describe(conflict, ids);
        if (!text.empty())
            std::cerr << "open-annihilation: mutators: " << text << '\n';
    }
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome == mutators::Outcome::applied)
            continue;
        std::string message = one_line(one.message);
        if (message.empty())
            message = outcome_word(one.outcome);
        std::cerr << "open-annihilation: mutator " << one.id << ": " << phase_name(one.phase)
                  << ": " << message << '\n';
    }
}

unsigned char folded_resource_byte(unsigned char byte) noexcept {
    if (byte == '\\')
        return '/';
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

bool same_resource(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        const unsigned char lhs = folded_resource_byte(static_cast<unsigned char>(left[offset]));
        const unsigned char rhs = folded_resource_byte(static_cast<unsigned char>(right[offset]));
        if (lhs != rhs)
            return false;
    }
    return true;
}

/// True when the layer serves the chosen map's scenario as given bytes.
bool serves_chosen_ota(
    const mutators::PipelineResult& result, const std::optional<mutators::ChosenMap>& chosen
) noexcept {
    if (!chosen.has_value())
        return false;
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.kind != oa::MutatorSourceKind::contents || entry.contents == nullptr)
            continue;
        if (same_resource(entry.path, chosen->ota_path))
            return true;
    }
    return false;
}

} // namespace

bool resolve_run_mutator(
    std::string_view text,
    std::string_view version,
    const std::filesystem::path& folder,
    mutators::ResolvedMutator* resolved,
    std::string* error
) {
    return resolve_run_mutator(
        text, version, oa::data::mutator_library::list_installed(folder), resolved, error
    );
}

bool resolve_run_mutator(
    std::string_view text,
    std::string_view version,
    const oa::data::mutator_library::Listing& listing,
    mutators::ResolvedMutator* resolved,
    std::string* error
) {
    const auto fail = [&](std::string message) {
        if (error != nullptr)
            *error = std::move(message);
        return false;
    };
    std::string parse_error;
    const std::optional<mutators::MutatorChoice> choice =
        mutators::parse_choice(text, &parse_error);
    if (!choice)
        return fail(std::move(parse_error));
    std::string resolve_error;
    const std::optional<mutators::ResolvedMutator> found =
        mutators::resolve_choice(*choice, listing, &resolve_error);
    if (!found)
        return fail(std::move(resolve_error));
    if (!version.empty()) {
        const std::optional<oa::formats::oamut::Version> asked =
            oa::formats::oamut::parse_version(version);
        if (!asked || *asked != found->manifest.version) {
            return fail(
                "\"" + choice->target + "\" " + std::string(version) + " is not installed; " +
                oa::formats::oamut::version_text(found->manifest.version) + " is"
            );
        }
    }
    if (resolved != nullptr)
        *resolved = *found;
    return true;
}

void Runtime::destroy_mutator_state(MutatorState* state) noexcept {
    delete state;
}

void Runtime::choose_match_mutators(std::vector<mutators::ResolvedMutator> chosen) {
    if (chosen.empty()) {
        if (mutator_state_ != nullptr)
            mutator_state_->chosen.clear();
        return;
    }
    if (mutator_state_ == nullptr) {
        std::unique_ptr<MutatorState, void (*)(MutatorState*) noexcept> made{
            new MutatorState, destroy_mutator_state
        };
        made->chosen = std::move(chosen);
        mutator_state_ = std::move(made);
        return;
    }
    mutator_state_->chosen = std::move(chosen);
}

std::span<const mutators::ResolvedMutator> Runtime::chosen_match_mutators() const noexcept {
    if (mutator_state_ == nullptr)
        return {};
    return mutator_state_->chosen;
}

const mutators::PipelineResult* Runtime::match_mutators() const noexcept {
    if (mutator_state_ == nullptr || !mutator_state_->result.has_value())
        return nullptr;
    return &*mutator_state_->result;
}

const mutators::Loadout* Runtime::match_loadout() const noexcept {
    if (mutator_state_ == nullptr || !mutator_state_->loadout.has_value())
        return nullptr;
    return &*mutator_state_->loadout;
}

std::optional<oa::base::sha256::Digest> Runtime::match_mutator_digest() const noexcept {
    if (mutator_state_ == nullptr)
        return std::nullopt;
    return mutator_state_->digest;
}

void Runtime::take_command_line_mutators() {
    if (options_.no_mutators) {
        note_mutators_refused(mutator_list_text(options_.mutators));
        choose_match_mutators({});
        return;
    }
    if (options_.mutators.empty()) {
        choose_match_mutators({});
        return;
    }
    const oa::data::mutator_library::Listing listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
    std::vector<mutators::ResolvedMutator> chosen;
    chosen.reserve(options_.mutators.size());
    for (std::size_t index = 0; index < options_.mutators.size(); ++index) {
        const std::string& value = options_.mutators[index];
        const std::string version = index < options_.mutator_versions.size()
                                        ? options_.mutator_versions[index]
                                        : std::string{};
        mutators::ResolvedMutator resolved;
        std::string error;
        if (!resolve_run_mutator(value, version, listing, &resolved, &error)) {
            if (index < options_.mutator_fields.size() && !options_.mutator_fields[index].empty())
                throw std::runtime_error(options_.mutator_fields[index] + ": " + error);
            throw std::runtime_error("--mutator: " + value + ": " + error);
        }
        chosen.push_back(std::move(resolved));
    }
    choose_match_mutators(std::move(chosen));
}

std::string Runtime::launch_mutator_refusal(
    std::string_view source,
    const std::vector<std::string>& texts,
    const std::vector<std::string>& versions,
    const std::vector<std::string>& fields
) const {
    // The flag refuses the list before any package is read, so a missing
    // mutator does not stop the launch.
    if (options_.no_mutators)
        return {};
    const oa::data::mutator_library::Listing listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
    std::vector<mutators::PipelineMutator> listed;
    std::vector<std::string> installed;
    listed.reserve(texts.size());
    installed.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const std::string version = index < versions.size() ? versions[index] : std::string{};
        const std::string field =
            index < fields.size() ? fields[index] : std::string("content.mutators");
        std::string error;
        mutators::ResolvedMutator resolved;
        if (!resolve_run_mutator(texts[index], version, listing, &resolved, &error))
            return std::string(source) + ": " + field + ": " + error;
        mutators::PipelineMutator entry;
        entry.package = resolved.package_folder;
        entry.manifest = resolved.manifest;
        entry.options = resolved.options;
        installed.push_back(oa::formats::oamut::version_text(resolved.manifest.version));
        listed.push_back(std::move(entry));
    }
    const std::string refusal = mutators::list_refusal(listed, installed);
    if (!refusal.empty())
        return std::string(source) + ": content.mutators: " + refusal;
    return {};
}

std::string Runtime::take_launch_mutators(
    std::string_view source,
    std::vector<std::string> texts,
    std::vector<std::string> versions,
    std::vector<std::string> fields
) {
    if (options_.no_mutators) {
        note_mutators_refused(mutator_list_text(texts));
        choose_match_mutators({});
        return {};
    }
    const std::string refusal = launch_mutator_refusal(source, texts, versions, fields);
    if (!refusal.empty())
        return refusal;
    const oa::data::mutator_library::Listing listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
    std::vector<mutators::ResolvedMutator> chosen;
    chosen.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const std::string version = index < versions.size() ? versions[index] : std::string{};
        mutators::ResolvedMutator resolved;
        if (!resolve_run_mutator(texts[index], version, listing, &resolved, nullptr))
            return std::string(source) + ": content.mutators: a mutator could not be read";
        chosen.push_back(std::move(resolved));
    }
    options_.mutators = std::move(texts);
    options_.mutator_versions = std::move(versions);
    options_.mutator_fields = std::move(fields);
    choose_match_mutators(std::move(chosen));
    return {};
}

void Runtime::apply_match_mutators(const MatchBootstrap& bootstrap) {
    // The held list is the one this start plays, including a start that chooses
    // none. A teardown before this line is not the end of that match.
    start_loadout_once();
    // Taken even when this start applies nothing, so a later start that no
    // setup made is not labelled with a setup that already started a game.
    const std::optional<mutators_memory::Setup> pending_setup =
        std::exchange(mutator_setup_, std::nullopt);
    mutator_start_pending_ = false;
    mutator_start_stops_.clear();
    mutator_start_setup_.reset();
    if (options_.no_mutators) {
        if (mutator_state_ != nullptr && !mutator_state_->chosen.empty()) {
            note_mutators_refused(mutator_names(mutators::loadout_of(mutator_state_->chosen)));
        }
        return;
    }
    if (mutator_state_ == nullptr || mutator_state_->chosen.empty())
        return;
    const std::vector<mutators::ResolvedMutator> listed = mutator_state_->chosen;
    const oa::formats::oamut::ClashMode clash_default = clash_default_of(preference_values_);
    const mutators::Loadout requested = mutators::loadout_of(listed, clash_default);
    // Written before the pipeline, so a crash while the mutators are applied
    // leaves the file. A playable run rewrites it with what applied.
    arm_mutator_sentinel(pending_setup, requested);
    // A layer already mounted makes the pipeline refuse the list and leave
    // that layer in place.
    if (assets_.mutator_layer_mounted())
        release_match_mutators();

    mutators::PipelineInput input = mutator_pipeline_input(listed);
    input.limits.shared = bootstrap.multiplayer;
    input.map = chosen_map_for_mutators();
    bind_planned_hack_check(input);
    mutators::PipelineResult result = mutators::run_pipeline(assets_, input);
    if (mutator_state_ == nullptr) {
        release_match_mutators();
        throw std::runtime_error("the mutators did not apply");
    }
    mutator_state_->result = std::move(result);
    mutator_state_->served_map = serves_chosen_ota(*mutator_state_->result, input.map);
    play_mutator_hacks();
    log_applied(listed, *mutator_state_->result);
    const std::optional<std::size_t> stopper =
        mutators::first_blocking(*mutator_state_->result, input.mutators);
    const bool blocking = !mutator_state_->result->refusal.empty() || stopper.has_value();
    if (blocking) {
        // A hack that was copied into the played profile is undone with the
        // profile the match would have played without mutators.
        play_latest_profile();
        const std::string reason = failure_text(listed, *mutator_state_->result);
        remember_mutator_start_stops(pending_setup, stops_of(listed, *mutator_state_->result));
        release_match_mutators();
        // A refused start is reported, not a crash: nothing is in play.
        clear_mutator_sentinel();
        throw std::runtime_error(reason);
    }
    // Taken after the hacks were played, so a mutator their check failed is
    // not in the loadout. Kept as it is when the list is run again: the
    // second result lists only these mutators, so pairing it with the list
    // the start was given would name a mutator that did not apply.
    const mutators::Loadout applied = mutators::applied_loadout(requested, *mutator_state_->result);
    const std::vector<mutators::ResolvedMutator> played =
        mutators_that_applied(listed, *mutator_state_->result);
    // The scripts' seed is the list they run. A replay plays the applied
    // list, so a start that left a mutator out runs again with that list
    // and keeps the second layer. A list that applied in full runs once.
    if (played.size() < listed.size()) {
        const std::string skipped = mutators_left_out(listed, *mutator_state_->result);
        if (mutator_state_->hacks_played) {
            play_latest_profile();
            mutator_state_->hacks_played = false;
            mutator_state_->units_per_player.reset();
        }
        if (assets_.mutator_layer_mounted() && !assets_.unmount_mutator_layer()) {
            release_match_mutators();
            clear_mutator_sentinel();
            throw std::runtime_error("the mutator layer could not be released");
        }
        mutators::PipelineInput again_input = mutator_pipeline_input(played);
        again_input.limits.shared = bootstrap.multiplayer;
        again_input.map = chosen_map_for_mutators();
        bind_planned_hack_check(again_input);
        mutators::PipelineResult again = mutators::run_pipeline(assets_, again_input);
        if (mutator_state_ == nullptr) {
            release_match_mutators();
            throw std::runtime_error("the mutators did not apply");
        }
        mutator_state_->result = std::move(again);
        mutator_state_->served_map = serves_chosen_ota(*mutator_state_->result, again_input.map);
        // A mutator that does not apply on this run stops the start. The
        // first run already let a refused hack fail alone; this run is the
        // list a replay will play, so every one of it has to apply.
        if (!mutator_state_->result->playable()) {
            const std::string reason = failure_text(played, *mutator_state_->result);
            remember_mutator_start_stops(pending_setup, stops_of(played, *mutator_state_->result));
            release_match_mutators();
            clear_mutator_sentinel();
            throw std::runtime_error(reason);
        }
        play_mutator_hacks();
        std::cerr << "open-annihilation: mutators: seeded again from the mutators that "
                     "applied, because "
                  << skipped << " did not apply\n";
    }
    if (mutator_state_->served_map)
        reread_chosen_map(true);
    if (applied.entries.empty()) {
        clear_mutator_sentinel();
        return;
    }
    arm_mutator_sentinel(pending_setup, applied);
    // The applied line above names the same mutators and then the files
    // they changed. This line is the one a crash report can quote.
    std::cerr << "open-annihilation: mutators in play: " << mutator_names(applied) << '\n';
    try {
        const oa::base::sha256::Digest layer =
            mutators::layer_digest(mutator_state_->result->layer, [this](std::string_view path) {
                return assets_.read(path).bytes;
            });
        mutator_state_->digest = mutators::mutator_digest(applied, layer);
        mutator_state_->loadout = applied;
    } catch (...) {
        release_match_mutators();
        throw;
    }
}

oa::formats::oamut::ClashMode Runtime::mutator_clash_default() const {
    return clash_default_of(preference_values_);
}

mutators::PipelineInput
Runtime::mutator_pipeline_input(const std::vector<mutators::ResolvedMutator>& chosen) const {
    mutators::PipelineInput input;
    input.clash_default = mutator_clash_default();
    input.mutators.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen) {
        mutators::PipelineMutator item;
        item.package = one.package_folder;
        item.manifest = one.manifest;
        item.options = one.options;
        item.clashes = one.clashes;
        input.mutators.push_back(std::move(item));
    }
    if (const oa::data::mod_profile::ModProfile* profile = mod_profile()) {
        input.mod.id = profile->id;
        input.mod.version = profile->version;
    } else {
        input.mod.id = std::string(oa::data::mod_profile::base_game_id);
    }
    const PresenceAnnouncement announcement = presence_announcement();
    input.mod.game = announcement.base_game.empty()
                         ? std::string(oa::data::content_bundles::default_base_game_id)
                         : announcement.base_game;
    input.engine_version = OA_ENGINE_VERSION;
    input.layout = &oa::data::defs::data_layout();
    input.limits.category_mask_types = limits_.category_masks.types;
    input.limits.weapon_types = limits_.weapon_types.types;
    input.variant = side_files_language_;
    input.seed = mutator_seed(*this, chosen);
    return input;
}

std::string Runtime::mutator_failure_text(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    return failure_text(chosen, result);
}

void Runtime::release_match_mutators() noexcept {
    try {
        if (mutator_state_ != nullptr && mutator_state_->hacks_played) {
            play_latest_profile();
            mutator_state_->hacks_played = false;
            mutator_state_->units_per_player.reset();
        }
        const bool held = mutator_state_ != nullptr && mutator_state_->result.has_value();
        const bool served = mutator_state_ != nullptr && mutator_state_->served_map;
        bool unmounted = false;
        if (assets_.mutator_layer_mounted()) {
            if (!assets_.unmount_mutator_layer())
                return;
            unmounted = true;
        }
        if (!unmounted && !held && !served)
            return;
        if (mutator_state_ != nullptr) {
            mutator_state_->result.reset();
            mutator_state_->loadout.reset();
            mutator_state_->digest.reset();
        }
        if (served) {
            reread_chosen_map(false);
            if (mutator_state_ != nullptr)
                mutator_state_->served_map = false;
        }
        std::cerr << "open-annihilation: mutators: released\n";
    } catch (...) {
    }
}

} // namespace oa::app
