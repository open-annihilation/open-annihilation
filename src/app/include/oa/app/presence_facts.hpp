// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What this machine is playing, for the presence it tells other machines:
// the platform and architecture, Developer Mode, the simulation hash, whether
// the rules differ from 3.1c, the mod and where it was published, which
// standard hacks are on, and whether it can fetch content. The header
// includes no other header of the engine's.
//
// Limitations: an override Developer Mode holds still counts, and still
// decides whether its hack is on, when the resolver refused it and the
// profile in play does not carry it.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::app {

class Runtime;

/// The mod in play, as its profile names it.
struct PresenceMod {
    std::string id{};             ///< the mod's stable id
    std::string name{};           ///< the mod's display name
    std::string version{};        ///< the mod's version
    int64_t packaging_revision{}; ///< how many times the package has been updated

    /// Compares two mods: every field.
    ///
    /// @param left one mod
    /// @param right the other
    /// @return true when every field is equal
    friend bool operator==(const PresenceMod& left, const PresenceMod& right) = default;
};

/// Where the mod in play was published: one release of a registry's catalogue.
struct PresenceOrigin {
    std::string registry{}; ///< the registry's id
    /// The address the registry's descriptor was read from; empty for a
    /// built-in registry, one added from a file, and one not in effect.
    std::string descriptor_url{};
    int64_t catalogue_release{};      ///< the catalogue release, from 1
    std::array<uint8_t, 32> sha256{}; ///< the SHA-256 of the package file as installed

    /// Compares two origins: every field.
    ///
    /// @param left one origin
    /// @param right the other
    /// @return true when every field is equal
    friend bool operator==(const PresenceOrigin& left, const PresenceOrigin& right) = default;
};

/// One Developer Mode override: the hack and whether the override turns it on.
struct PresenceOverride {
    std::string hack{}; ///< the hack's id
    bool on{};          ///< the override turns the hack on

    /// Compares two overrides: the hack and whether it is on.
    ///
    /// @param left one override
    /// @param right the other
    /// @return true when both fields are equal
    friend bool operator==(const PresenceOverride& left, const PresenceOverride& right) = default;
};

/// What this machine is playing.
struct PresenceFacts {
    /// The platform the game runs on, as SDL names it: "Windows", "macOS",
    /// "Linux", "iOS", "Android" and so on.
    std::string platform;
    /// The processor architecture the game was built for: "x64", "x86",
    /// "arm64" or "armhf", else "unknown".
    std::string architecture;
    /// Developer Mode is on (Runtime::developer_mode).
    bool developer_mode{};
    /// The simulation hash the game plays, as simulation_hash reports it:
    /// the played profile's, or the plain baseline's when the game plays 3.1c,
    /// folded with the running match's mutator digest when that match applied
    /// mutators. With no mutators applied it is that profile hash alone.
    std::array<uint8_t, 32> sim_hash{};
    /// The played profile's simulation hash differs from the plain baseline's,
    /// or a running match has folded a mutator digest into the hash. With the
    /// settings not yet layered, any played profile counts as differing.
    /// False when the game plays no profile and no mutator digest.
    bool rules_differ_from_base{};
    /// The mod in play: its id, name, version and packaging revision
    /// (ModProfile::id, name, version, packaging.revision). None when the
    /// game plays no profile.
    std::optional<PresenceMod> mod;
    /// Where the mod in play was published, from the installer's origin
    /// record in the mod's folder (.oa-origin.yaml), the first of the game's
    /// folders when there are two or more, and the registry's descriptor URL
    /// from the registries in effect. Only a catalogue origin with its
    /// SHA-256 gives one: none for a mod installed from a file, a folder
    /// without a record, or no mod. The record is read again only when the
    /// folder or the mod's id, version or packaging revision changes, and the
    /// URL looked up again only when the registries change.
    std::optional<PresenceOrigin> mod_origin;
    /// Standard hacks of simulation scope that are on, in registry order.
    /// A hack is on when the played profile's shipped state has it on, or,
    /// while Developer Mode is on and an override names it, when that
    /// override is on.
    std::vector<std::string> game_hacks;
    /// Standard hacks of view scope that are on, in registry order, by the
    /// same rule as game_hacks.
    std::vector<std::string> view_hacks;
    /// Developer Mode's overrides, each hack's id and whether it is on
    /// (EngineSettings::hack_overrides). None while Developer Mode is off.
    std::vector<PresenceOverride> overrides;
    /// The machine can fetch content: the content service has a downloads
    /// folder and at least one registry that is on, not set aside for
    /// another of its id, allowed by Developer Mode and whose downloads are
    /// on (its install ID is not off where it needs one). False before the
    /// content service has started.
    bool fetches_content{};

    /// Compares two readings: every field.
    ///
    /// @param left one reading
    /// @param right the other
    /// @return true when every field is equal
    friend bool operator==(const PresenceFacts& left, const PresenceFacts& right) = default;
};

/// Reads what this machine is playing.
///
/// @param runtime the running app
/// @return the facts
/// @throws std::runtime_error when the plain baseline does not resolve and
///         the facts need its simulation hash
[[nodiscard]] PresenceFacts presence_facts(const Runtime& runtime);

/// One standard hack that is on, with each parameter's value.
///
/// The value is the canonical JSON the profile resolves. These values are
/// not part of the presence told to other machines.
struct ActiveHack {
    std::string id{}; ///< the hack's registry id

    /// One parameter and its canonical JSON.
    struct Value {
        std::string name{}; ///< the parameter's name
        std::string json{}; ///< the value, as canonical JSON
    };

    std::vector<Value> values{}; ///< the parameters, in registry order
};

/// The mod, the hacks and the mutators a match report reads.
///
/// has_mod is false for the base game, including a baseline profile. The
/// hacks are the standard hacks that are on. The mutators are the ones the
/// running match applied.
struct MatchProfileReading {
    bool has_mod{};            ///< a mod is in play
    std::string mod_id{};      ///< the mod's id
    std::string mod_name{};    ///< the mod's display name
    std::string mod_version{}; ///< the mod's version
    std::vector<ActiveHack> hacks{};

    /// One mutator the running match applied.
    struct Mutator {
        std::string id{};      ///< the mutator's id
        std::string version{}; ///< the mutator's version
        std::string name{};    ///< the name the player saw
    };

    std::vector<Mutator> mutators{}; ///< empty when this match applied none
};

/// Reads the standard hacks that are on, with each parameter's value.
///
/// A hack is on when the played profile's state has it on. While Developer
/// Mode is on, an override is laid over that state, parameters included.
///
/// @param runtime the running app
/// @return the hacks that are on, in registry order
[[nodiscard]] std::vector<ActiveHack> active_standard_hacks(const Runtime& runtime);

/// Reads the mod, the active standard hacks and the mutators the running
/// match applied.
///
/// @param runtime the running app
/// @return the reading; has_mod is false for the base game
[[nodiscard]] MatchProfileReading match_profile_reading(const Runtime& runtime);

} // namespace oa::app
