// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a replay still needs before it can be watched: the packages its
// manifest names, set against what is installed and against the releases a
// registry lists. Nothing here fetches, and nothing here reads a file.
#pragma once

#include "oa/data/replays/replays.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::replays {

/// Whether a registry could name the package a replay asks for.
enum class ReleaseReach : uint8_t {
    unknown,  ///< the registry has not been asked
    listed,   ///< the catalogue or its releases list names this digest
    unlisted, ///< a catalogue was read, and it does not name this digest
    offline,  ///< the registry could not be reached, and no catalogue is held
};

/// One release a registry lists, found by the package file's SHA-256.
struct ListedRelease {
    /// The package file's SHA-256, 64 lower-case hex digits.
    std::string sha256{};
    ReleaseReach reach{ReleaseReach::unknown};
    /// The registry's id. Empty unless `reach` is `listed`.
    std::string registry{};
    /// `oamod`, `oamap`, `oamut`, `expansion`, or another catalogue kind.
    std::string package_kind{};
    std::string id{};      ///< the package key
    std::string name{};    ///< the name a player sees
    std::string version{}; ///< the version, when the entry names one
    std::optional<int64_t> revision{};
    std::string file{}; ///< the package reference
    int64_t release{};  ///< the release number
    uint64_t size{};    ///< the package's size in bytes
};

/// One mod that is installed.
struct HaveMod {
    std::string id{};
    std::string name{};
    std::string version{};
    std::optional<int64_t> revision{};
    /// The installed package file's SHA-256, when it is known.
    std::string sha256{};
};

/// One mutator that is installed.
struct HaveMutator {
    std::string id{};
    /// The digest the manifest's origin names. Empty when the folder names none.
    std::string sha256{};
};

/// What is installed, for one comparison. The engine is major.minor.patch.
struct HaveContent {
    uint16_t engine_major{};
    uint16_t engine_minor{};
    uint16_t engine_patch{};
    std::vector<HaveMod> mods{};
    /// The mod being played, when the comparison should treat it as installed
    /// even if `mods` does not list it the same way. Empty when none is playing.
    std::string playing_mod_id{};
    std::string playing_mod_name{};
    /// Map names that are installed.
    std::vector<std::string> maps{};
    /// Mutators that are installed.
    std::vector<HaveMutator> mutators{};
    /// Expansion ids that are on.
    std::vector<std::string> expansions_on{};
    /// Expansion ids that are installed, on or off.
    std::vector<std::string> expansions_have{};
};

/// Which missing thing a row's chip names.
enum class GapKind : uint8_t {
    mod_missing,       ///< the mod is not installed
    mod_other,         ///< another release of the mod is installed
    map,               ///< a map is not installed
    mutator,           ///< a mutator is not installed
    expansion_off,     ///< an expansion is installed and off
    expansion_missing, ///< an expansion is not installed
    engine,            ///< this build is older than the replay needs
};

/// Why a missing package cannot be fetched.
enum class GapBlock : uint8_t {
    none,     ///< it can be fetched, or it needs no fetch
    unlisted, ///< it is not in the catalogue that was read
    offline,  ///< the registry could not be reached
};

/// One thing still missing, in the order the replay named its packages.
struct ContentGap {
    GapKind kind{};
    GapBlock block{GapBlock::none};
    std::string id{};
    std::string name{};
    std::string version{};
    /// The installed release's version, when `kind` is `mod_other`.
    std::string have_version{};
    std::optional<int64_t> revision{};
    std::optional<int64_t> have_revision{};
    std::string map_name{};
    std::string pack_name{};
    std::string sha256{};
    /// Set when `fetchable` is true: the release the download asks for.
    std::string registry{};
    std::string package_kind{};
    std::string file{};
    int64_t release{};
    uint64_t size{};
    /// True when this release can be fetched. Never set for another release
    /// of a mod that is already installed, and never set for a digest the
    /// list does not name.
    bool fetchable{};
    /// True when an expansion has to be turned on for this replay.
    bool turn_on{};
    /// True when another release of this mod is what is installed.
    bool other_release{};
    /// True when this release is installed and is not the mod being played.
    /// Watch plays the replay with that folder and puts the player's mod back.
    bool play_installed{};
};

/// Compares a replay's needs with what is installed and what a registry lists.
///
/// A package is fetched only when `listed` names its SHA-256 as `listed`.
/// An expansion is also fetched when `listed` names that id as `listed`,
/// because a manifest names an expansion by id and not by a digest. A
/// different release of the same package is not a substitute. A mod that
/// is installed in another version or revision is `mod_other` and is not
/// fetchable. The release a manifest names, when it is installed and is not
/// the mod being played, is a gap with `play_installed` set: it is not fetched.
/// A need that names a mod by name and not by id matches an
/// installed mod of that name, and the mod being played. A mutator whose
/// need names a digest matches an install only when that install names the
/// same digest, or names none. A map the player has, a mutator the player
/// has, and an expansion that is on are left out. An expansion that is not
/// installed is not turned on. An engine requirement this build meets is
/// left out. A need whose kind is `base`, or a kind this build does not
/// compare, is left out. Two missing maps from one pack, named by one
/// SHA-256, are one gap.
///
/// @param replay the replay, including its needs and its engine requirement
/// @param have what is installed
/// @param listed the releases a registry named, by package SHA-256
/// @return the gaps, in the order the replay named the packages, with an
///         unmet engine last
[[nodiscard]] std::vector<ContentGap>
compare_needs(const Replay& replay, const HaveContent& have, std::span<const ListedRelease> listed);

} // namespace oa::data::replays
