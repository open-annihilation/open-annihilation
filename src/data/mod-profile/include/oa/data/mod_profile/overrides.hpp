// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A player's overrides of a profile's standard hacks (Developer Mode): each
// hack turned on or off whatever the profile says, with the parameters it
// sets while on. The settings keep them apart from the profile, which is
// never written; the resolver lays them over the profile after its own steps
// and the player's settings, and before the host's match choices
// (ResolveOptions::overrides). Also the state of every standard hack a
// resolved profile holds, which the settings dialog shows, and the text the
// settings keep the overrides as.
#pragma once

#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/mod_profile/value.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mod_profile {

/// The base game every profile builds on, as a profile's requires.base names
/// it. The settings keep the overrides of the plain 3.1c baseline, played
/// without a mod, under it; no profile's id can be the same, since an id is
/// kebab-case.
inline constexpr std::string_view base_game_id = "ta-3.1c";

/// One parameter an override sets.
struct ParameterOverride {
    std::string name{}; ///< the parameter's name, as the registry spells it
    Value value{};      ///< its value, as a profile writes it

    /// Compares two parameters: their names, and their values as values_equal does.
    ///
    /// @param left one parameter
    /// @param right the other
    /// @return true when they are equal
    friend bool operator==(const ParameterOverride& left, const ParameterOverride& right) noexcept;
};

/// A player's override of one standard hack: on or off whatever the profile
/// says, and the parameters it sets while on.
struct HackOverride {
    std::string hack{}; ///< the hack's registry id, such as "ai.attack-wave-size"
    bool on{};          ///< the hack is on; off turns it off, as false in a profile does
    /// The parameters it sets while on, each once. Every other parameter
    /// keeps the value the profile resolves for it, or the registry's
    /// default when the profile has the hack off.
    std::vector<ParameterOverride> parameters{};

    /// Compares two overrides: their hacks, whether each is on, and their
    /// parameters in order.
    ///
    /// @param left one override
    /// @param right the other
    /// @return true when they are equal
    friend bool operator==(const HackOverride& left, const HackOverride& right) noexcept;
};

/// Whether a standard hack is on, and the values its parameters hold.
struct HackState {
    bool on{}; ///< the hack is on
    /// Its parameters' values by their index within the hack: the resolved
    /// ones while on, the 3.1c baselines while off.
    std::vector<Value> values{};
};

/// Returns the registry's standard hacks: the hacks a player can override,
/// which the settings list. A retired hack is not one.
///
/// @return every hack of the registry that is not retired, in the
///     registry's order
[[nodiscard]] std::span<const registry::Entry* const> standard_hacks();

/// Finds a standard hack's place among standard_hacks.
///
/// @param id the hack's registry id
/// @return its index; nothing for an id that is not a hack's
[[nodiscard]] std::optional<std::size_t> standard_hack_index(std::string_view id) noexcept;

/// Returns every standard hack as an effective profile holds it.
///
/// @param effective the effective profile (Resolution::effective)
/// @return one state for each of standard_hacks, in its order: on with the
///     values the profile's hacks block holds when it names the hack, else
///     off at the baselines
[[nodiscard]] std::vector<HackState> hack_states(const Value& effective);

/// Returns every standard hack as 3.1c plays it.
///
/// @return one state for each of standard_hacks, in its order, every one
///     off at its baselines
[[nodiscard]] std::vector<HackState> base_hack_states();

/// Returns the values a hack's parameters take once it is on, before an
/// override sets any.
///
/// @param hack a standard hack
/// @param profile the hack as the profile resolves it
/// @return the profile's values when it has the hack on, else the
///     registry's defaults
[[nodiscard]] std::vector<Value> on_values(const registry::Entry& hack, const HackState& profile);

/// Lays an override over a hack as the resolver lays one that fits: off
/// gives the hack off at its baselines; on gives on_values with each
/// parameter the override sets in its place. A parameter the hack does
/// not have is passed over.
///
/// @param hack a standard hack
/// @param profile the hack as the profile resolves it
/// @param override the hack's override; null for none
/// @return the hack as it plays with the override
[[nodiscard]] HackState overridden_state(
    const registry::Entry& hack, const HackState& profile, const HackOverride* override
);

/// Finds a hack's override.
///
/// @param overrides the overrides
/// @param hack the hack's registry id
/// @return the first override of the hack; null for none
[[nodiscard]] const HackOverride*
find_override(std::span<const HackOverride> overrides, std::string_view hack) noexcept;

/// Writes overrides as the settings keep them: one JSON object, a flow
/// mapping as a profile reads it, from each hack's id to false, true, or an
/// object of the parameters it sets.
///
/// @param overrides the overrides
/// @return the text, on one line; "{}" for none
[[nodiscard]] std::string overrides_text(std::span<const HackOverride> overrides);

/// Reads overrides from the text overrides_text writes.
///
/// Each member of the mapping is one hack's override: true or false, or a
/// mapping of its parameters, which turns it on. A member of any other form
/// is left out, and so is all of a text that is not a mapping the profile
/// grammar reads. A retired hack's override is left out too, so settings
/// kept while the hack was offered load without it. The overrides are not
/// otherwise checked against the registry here: the resolver checks each as
/// it lays it, as it checks a profile.
///
/// @param text the text
/// @return the overrides, in the text's order
[[nodiscard]] std::vector<HackOverride> read_overrides(std::string_view text);

/// Writes the profile that changes nothing of 3.1c: the plain baseline,
/// which a game played without a mod resolves to lay its overrides over.
///
/// @return the profile's text, id "base-game", version "3.1c"
[[nodiscard]] std::string base_game_profile_text();

} // namespace oa::data::mod_profile
