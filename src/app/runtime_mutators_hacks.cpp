// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lays a match's mutator hacks over the profile the match plays. Developer
// Mode's overrides are the baseline. The pipeline has already refused a hack
// that does not resolve, so this copies that resolution into the one profile
// the run already plays. The match's release puts the player's profile back.

#include "oa/app/runtime.hpp"
#include "oa/app/enhanced_mode.hpp"
#include "oa/data/enhanced_mode.hpp"

#include "engine_settings_state.hpp"
#include "mutator_state.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/value.hpp"
#include "oa/data/mutators/hacks.hpp"
#include "oa/data/mutators/pipeline.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace profiles = oa::data::mod_profile;
namespace mutators = oa::data::mutators;

/// The registry entry a parameter path names. Parameter names have no dot.
std::string entry_of(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return std::string{path};
    return std::string{path.substr(0, dot)};
}

/// The entries a profile turns on before overrides and match choices.
std::vector<std::string> entries_on(const profiles::Resolution& resolution) {
    std::vector<std::string> on;
    if (const profiles::Value* hacks = profiles::find_member(resolution.effective, "hacks");
        hacks != nullptr) {
        for (const profiles::Member& member : hacks->members)
            on.push_back(member.key);
    }
    for (const profiles::Provenance& provenance : resolution.provenance) {
        if (!provenance.path.starts_with("limits.") || provenance.source == "baseline")
            continue;
        const std::string entry = entry_of(provenance.path);
        bool known = false;
        for (const std::string& id : on) {
            if (id == entry)
                known = true;
        }
        if (!known)
            on.push_back(entry);
    }
    return on;
}

/// The text of the value a resolution plays for ENTRY.PARAM, or empty when
/// it has none. A limit's entry sits under `limits` without that prefix, a
/// hack's under `hacks` by its id.
std::string played_value(const profiles::Resolution& resolution, std::string_view path) {
    const std::string entry = entry_of(path);
    if (entry.size() >= path.size())
        return {};
    constexpr std::string_view limits_prefix = "limits.";
    const bool limit = entry.starts_with(limits_prefix);
    const profiles::Value* block =
        profiles::find_member(resolution.effective, limit ? "limits" : "hacks");
    if (block == nullptr)
        return {};
    const std::string_view key =
        limit ? std::string_view{entry}.substr(limits_prefix.size()) : std::string_view{entry};
    const profiles::Value* values = profiles::find_member(*block, key);
    if (values == nullptr)
        return {};
    const profiles::Value* value = profiles::find_member(*values, path.substr(entry.size() + 1));
    if (value == nullptr)
        return {};
    return profiles::value_text(*value);
}

/// Records one hack note on the conflict report. It is not a journal entry,
/// so the applied line does not count it as a rewritten file.
void record_note(mutators::PipelineResult& result, const mutators::HackNote& note) {
    mutators::Conflict conflict;
    conflict.severity = mutators::Severity::note;
    conflict.kind = mutators::ConflictKind::hack_note;
    conflict.record = "hacks:" + note.entry;
    conflict.detail = note.text;
    const auto slot_of = [&result](std::string_view id) -> std::optional<uint16_t> {
        for (std::size_t slot = 0; slot < result.mutators.size(); ++slot) {
            if (result.mutators[slot].id == id)
                return static_cast<uint16_t>(slot);
        }
        return std::nullopt;
    };
    if (!note.earlier.empty()) {
        if (const std::optional<uint16_t> earlier = slot_of(note.earlier); earlier.has_value())
            conflict.mutators.push_back(*earlier);
    }
    if (const std::optional<uint16_t> winner = slot_of(note.mutator); winner.has_value())
        conflict.mutators.push_back(*winner);
    result.report.conflicts.push_back(std::move(conflict));
}

/// Resolves the profile with these hacks as match choices that turn entries on.
profiles::ResolveResult resolve_with(
    const ProfileSource& source,
    const std::vector<profiles::HackOverride>& overrides,
    std::span<const mutators::MutatorHack> hacks,
    uint32_t enhanced_version
) {
    profiles::ResolveOptions options = source.options;
    options.overrides = overrides;
    options.match.clear();
    for (const mutators::MutatorHack& hack : hacks) {
        profiles::MatchOption option;
        option.path = hack.path;
        option.value = hack.value;
        option.source = hack.mutator;
        option.turn_on = true;
        options.match.push_back(std::move(option));
    }
    return oa::data::enhanced_mode::resolve_profile(
        source.text, source.name, options, enhanced_version
    );
}

/// The faults in a planned hack list, or none when the profile accepts them.
std::vector<mutators::HackCheckError> check_planned(
    bool can_change,
    const ProfileSource* source,
    const std::vector<profiles::HackOverride>& overrides,
    std::span<const mutators::MutatorHack> hacks,
    uint32_t enhanced_version
) {
    if (hacks.empty())
        return {};
    if (!can_change || source == nullptr) {
        std::vector<mutators::HackCheckError> faults;
        std::vector<std::string> named;
        for (const mutators::MutatorHack& hack : hacks) {
            bool seen = false;
            for (const std::string& id : named) {
                if (id == hack.mutator)
                    seen = true;
            }
            if (seen)
                continue;
            named.push_back(hack.mutator);
            const std::string entry = entry_of(hack.path);
            faults.push_back(
                mutators::HackCheckError{
                    hack.mutator,
                    entry,
                    "hacks: " + entry + ": the game's rules cannot change in this run"
                }
            );
        }
        return faults;
    }
    const profiles::ResolveResult resolved =
        resolve_with(*source, overrides, hacks, enhanced_version);
    if (resolved.resolution.has_value())
        return {};
    return mutators::hack_faults(resolved.errors, hacks);
}

/// Fails one mutator for its hacks and drops the records.
void fail_for_hacks(mutators::MutatorResult& one, std::string message) {
    one.hacks.clear();
    if (one.outcome != mutators::Outcome::applied)
        return;
    one.outcome = mutators::Outcome::failed;
    one.phase = mutators::Phase::plan;
    one.cause = mutators::Cause::hack_refused;
    one.message = std::move(message);
}

} // namespace

void Runtime::bind_planned_hack_check(mutators::PipelineInput& input) const {
    const EngineSettingsState* settings = engine_settings_.get();
    const bool can_change = settings != nullptr && settings->layered &&
                            settings->profile_source.has_value() && settings->played != nullptr;
    // Copied at bind time. The check reads these copies and not the runtime,
    // so a dry run's thread can call it after this method has returned.
    const std::optional<ProfileSource> source =
        can_change ? std::optional<ProfileSource>{*settings->profile_source} : std::nullopt;
    std::vector<profiles::HackOverride> overrides;
    if (can_change && settings->current.developer_mode)
        overrides = settings->current.hack_overrides;
    const uint32_t enhanced_version = enhanced_mode_version(*this);
    input.check_hacks = [can_change, source, overrides, enhanced_version](
                            std::span<const mutators::MutatorHack> hacks
                        ) {
        return check_planned(
            can_change, source ? &*source : nullptr, overrides, hacks, enhanced_version
        );
    };
}

std::optional<uint16_t> Runtime::mutator_units_per_player() const noexcept {
    if (mutator_state_ == nullptr || !mutator_state_->hacks_played)
        return std::nullopt;
    return mutator_state_->units_per_player;
}

void Runtime::play_mutator_hacks() {
    if (mutator_state_ == nullptr || !mutator_state_->result.has_value())
        return;
    mutators::PipelineResult& result = *mutator_state_->result;
    std::vector<mutators::MutatorHack> hacks;
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome != mutators::Outcome::applied)
            continue;
        hacks.insert(hacks.end(), one.hacks.begin(), one.hacks.end());
    }
    if (hacks.empty())
        return;

    // The pipeline judged these same records. A refusal here means the two
    // checks disagreed, so the start stops instead of keeping a half-applied
    // mutator.
    const EngineSettingsState* settings_now = engine_settings_.get();
    const bool can_change = settings_now != nullptr && settings_now->layered &&
                            settings_now->profile_source.has_value() &&
                            settings_now->played != nullptr;
    const ProfileSource* source = can_change ? &(*settings_now->profile_source) : nullptr;
    const std::vector<profiles::HackOverride> overrides =
        can_change && settings_now->current.developer_mode ? settings_now->current.hack_overrides
                                                           : std::vector<profiles::HackOverride>{};
    const std::vector<mutators::HackCheckError> faults =
        check_planned(can_change, source, overrides, hacks, enhanced_mode_version(*this));
    if (!faults.empty()) {
        for (const mutators::HackCheckError& fault : faults) {
            for (mutators::MutatorResult& one : result.mutators) {
                if (one.id == fault.mutator)
                    fail_for_hacks(one, fault.message);
            }
        }
        const std::string reason = faults.front().mutator + ": plan: " + faults.front().message;
        play_latest_profile();
        release_match_mutators();
        throw std::runtime_error(reason);
    }

    auto& settings = *engine_settings_;
    std::vector<std::string> profile_on;
    {
        profiles::ResolveOptions bare = settings.profile_source->options;
        bare.match.clear();
        bare.overrides.clear();
        const profiles::ResolveResult plain = profiles::resolve_profile(
            settings.profile_source->text, settings.profile_source->name, bare
        );
        if (plain.resolution.has_value())
            profile_on = entries_on(*plain.resolution);
    }
    const profiles::ResolveResult resolved =
        resolve_with(*settings.profile_source, overrides, hacks, enhanced_mode_version(*this));
    if (!resolved.resolution.has_value()) {
        const std::vector<mutators::HackCheckError> again =
            mutators::hack_faults(resolved.errors, hacks);
        const std::string reason = again.empty()
                                       ? std::string{"the hacks were refused"}
                                       : again.front().mutator + ": plan: " + again.front().message;
        play_latest_profile();
        release_match_mutators();
        throw std::runtime_error(reason);
    }

    // Copy into a local first. A throw while copying leaves the profile the
    // match already plays untouched, and hacks_played stays clear so a later
    // release does not lay another profile over a half-written one.
    oa::data::mod_profile::ModProfile next = resolved.resolution->profile;
    mutator_state_->hacks_played = true;
    *settings.played = std::move(next);
    settings.plays_base_rules = false;
    mutator_state_->units_per_player.reset();
    for (const profiles::Provenance& provenance : resolved.resolution->provenance) {
        if (provenance.path != "limits.units-per-player.default")
            continue;
        if (!provenance.source.starts_with("match"))
            continue;
        mutator_state_->units_per_player = settings.played->limits.units_per_player.default_limit;
        break;
    }

    const mutators::HackMerge merged = mutators::merge_hacks(hacks);
    const std::vector<mutators::HackNote> overlaps =
        mutators::developer_conflicts(merged.hacks, overrides, profile_on);
    for (const mutators::HackNote& note : merged.notes)
        record_note(result, note);
    for (const mutators::HackNote& note : overlaps)
        record_note(result, note);

    // A value the profile holds back, such as a unit cap past its
    // clamp-between bounds, is named with the value the match plays, and the
    // resolver's warning says why.
    std::string line = "open-annihilation: mutators: hacks ";
    std::vector<std::string> reasons;
    for (std::size_t slot = 0; slot < merged.hacks.size(); ++slot) {
        const mutators::MutatorHack& hack = merged.hacks[slot];
        if (slot != 0)
            line += ", ";
        line += hack.path;
        line += '=';
        line += hack.value;
        const std::string plays = played_value(*resolved.resolution, hack.path);
        if (!plays.empty() && plays != hack.value)
            line += " (plays " + plays + ")";
        for (const profiles::Diagnostic& warning : resolved.warnings) {
            if (warning.path == hack.path || warning.path == "match " + hack.path)
                reasons.push_back(hack.path + ": " + warning.message);
        }
    }
    std::cerr << line << '\n';
    for (const std::string& reason : reasons)
        std::cerr << "open-annihilation: mutators: hacks: " << reason << '\n';
}

} // namespace oa::app
