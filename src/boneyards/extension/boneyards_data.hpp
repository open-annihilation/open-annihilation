// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the data a game is playing is one the Boneyards launches and
// reports: plain 3.1c, Core Contingency and Battle Tactics, and the
// Boneyards' own files. Those share one simulation. Another mod, or a
// Developer Mode change to the simulation, does not.
#pragma once

#include "oa/data/mod_profile.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace oa::app {

/// The sentence a refused launch or report shows. 3.1c has no sentence of
/// its own for this.
inline constexpr const char* kBoneyardsDataRefusal =
    "The Boneyards launches and reports games only on 3.1c, Core Contingency and Battle Tactics. "
    "Switch your mod off to play in the Boneyards.";

/// What is known about the data the game is playing, before it is judged.
struct DataFacts {
    /// The profile's text; empty when the game plays without one.
    std::string profile_text{};
    /// The profile's name, for its diagnostics; empty when there is none.
    std::string profile_name{};
    /// Developer Mode is on, so `options.overrides` are in play. Ignored when
    /// `run_sim_hash` is set: the engine's hash already includes those overrides.
    bool developer_mode{};
    /// The simulation cannot be confirmed: the engine's hash could not be
    /// read, or a profile supplied here cannot be read.
    bool unreadable{};
    /// Settings the profile binds, and Developer Mode's overrides while it is on.
    /// Ignored when `run_sim_hash` is set.
    oa::data::mod_profile::ResolveOptions options{};
    /// The simulation hash of the profile the game already resolved, before
    /// Developer Mode's overrides. Set only for that profile; a game without
    /// one leaves it empty. Used only while `options.overrides` is empty and
    /// `run_sim_hash` is empty.
    std::optional<oa::base::sha256::Digest> shipped_sim_hash{};
    /// The simulation hash the run plays under, from the engine's
    /// simulation_hash: 64 lower-case hexadecimal digits. When set, and no
    /// foreign mod or mutator was already refused, equal to the plain 3.1c
    /// hash is allowed and any other value is refused. Empty leaves that
    /// part of the decision to the profile and the overrides, which the
    /// tests supply directly. A matching hash does not allow a foreign mod
    /// or a mutator.
    std::string run_sim_hash{};
    /// The profile's id. Empty when none was named here; an id in
    /// `profile_text` is read then. `boneyards` is the Boneyards' own profile.
    std::string profile_id{};
    /// A mod folder is playing and it has no profile. A folder whose profile
    /// id is `boneyards` is the Boneyards' own and leaves this clear.
    bool mod_folder{false};
    /// Mutators the match would carry. Any number above zero is refused.
    uint32_t mutator_count{0};
};

/// Whether the Boneyards may launch and report on these facts.
struct DataVerdict {
    /// True for plain 3.1c data, including Core Contingency, Battle Tactics
    /// and the Boneyards' own files.
    bool allowed{true};
    /// The refusal the player is shown; null when the data is allowed.
    /// Static storage when set.
    const char* message{};
};

/// Judges the data a game is playing.
///
/// Allowed when its simulation hash is the plain 3.1c one and no foreign mod
/// or mutator is in the match. A profile other than the Boneyards' own, a
/// mod folder with no profile, and any mutator are refused even when the
/// hash matches. When `run_sim_hash` is set, that is the hash the engine
/// reports for the run, including every Developer Mode override that changes
/// the simulation. Otherwise a game without a profile and without a
/// Developer Mode override plays that simulation, and so do Core Contingency
/// and Battle Tactics, which add no profile. The Boneyards' own profile is
/// allowed while it changes no simulation data. Developer Mode's overrides
/// count only while Developer Mode is on; a display change stays allowed.
/// Unreadable facts are refused.
///
/// @param facts what is known about the data
/// @return the verdict; `message` is kBoneyardsDataRefusal when refused
[[nodiscard]] DataVerdict boneyards_data_verdict(const DataFacts& facts);

/// Reads the facts of the running game. Null answers as plain 3.1c, which is allowed.
using DataFactsSource = DataFacts (*)(void* context);

/// Installs what the launch and the report read. A null source is plain 3.1c.
///
/// @param source the reader; null for none
/// @param context passed to `source`
void boneyards_set_data_facts_source(DataFactsSource source, void* context) noexcept;

/// Judges the facts the installed source reads now.
///
/// @return the verdict; allowed when no source is installed
[[nodiscard]] DataVerdict boneyards_current_data_verdict();

/// Returns the sentence a refused launch or report shows.
///
/// @return kBoneyardsDataRefusal
[[nodiscard]] const char* boneyards_data_refusal_message() noexcept;

} // namespace oa::app
