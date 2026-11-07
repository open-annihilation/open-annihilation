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
    /// The folder this release is installed in, under Mods or Replay releases.
    /// Watch plays this folder. The first folder of the id is not this release.
    std::string folder{};
};

/// One mutator that is installed.
struct HaveMutator {
    std::string id{};
    /// The digest the manifest's origin names. Empty when the folder names none.
    std::string sha256{};
};

/// One map pack that is installed, in Maps or in Replay releases.
///
/// `sha256` is the identity a recording stores: the digest in the folder's
/// origin record when that record has one, otherwise the digest of the
/// folder. It is not the digest of `oamap.yaml`. A hand-copied pack and a
/// catalogue copy of the same files are the same pack only when the recording
/// names that same identity.
struct HaveMapPack {
    std::string id{};
    std::string name{};
    std::string version{};
    /// The origin's digest, or the folder digest when the pack has no record.
    std::string sha256{};
    /// The folder this release is installed in. Watch plays this folder.
    std::string folder{};
    /// Map names, titles and stems this pack lists.
    std::vector<std::string> maps{};
    /// True when this pack was read from Replay releases. A match plays it
    /// even when Maps holds no other release of the id.
    bool kept{};
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
    /// The version of the mod being played. Empty when none is playing or the
    /// profile names none. Another release of the same mod is not this one.
    std::string playing_mod_version{};
    /// The package digest of the mod being played, when it is known.
    std::string playing_mod_sha256{};
    /// Map names that are installed. A name alone is enough when the recording
    /// names no pack identity. It is not another release of a pack the
    /// recording names.
    std::vector<std::string> maps{};
    /// Map packs that are installed, in Maps and in Replay releases.
    std::vector<HaveMapPack> map_packs{};
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
    /// The registry's id. Set when the list names the release: the download
    /// asks for it. Also set when the package was looked for and is not
    /// listed, so the card can name that registry, and for another release
    /// of a mod that is already installed, which stays not fetchable so the
    /// button keeps its own words. The press installs that release beside
    /// the one in use.
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
    /// True when another release of this mod or map pack is what is installed.
    bool other_release{};
    /// True when this release is installed and is not the one being played.
    /// Watch plays the replay with that folder. A mod is put back afterwards.
    /// A map pack is mounted for the replay and the player's packs stay.
    bool play_installed{};
    /// The folder Watch plays, when `play_installed` is set. It is the folder
    /// of the release the row matched, from `HaveMod::folder` or
    /// `HaveMapPack::folder`. Empty otherwise. The first folder of the id is
    /// not this release.
    std::string play_folder{};
};

/// Compares a replay's needs with what is installed and what a registry lists.
///
/// A package is fetched only when `listed` names its SHA-256 as `listed`.
/// An expansion is also fetched when `listed` names that id as `listed`,
/// because a manifest names an expansion by id and not by a digest. A
/// different release of the same package is not a substitute. A mod that
/// is installed in another version, revision or package digest is `mod_other`
/// and is not fetchable; the list's file, registry and release are still
/// filled when it names that digest. The release a manifest names, when it
/// is installed and is not the mod being played, is a gap with
/// `play_installed` set: it is not fetched. `play_folder` is that release's
/// folder, the one Watch plays. The first folder of the id is not that
/// release. Several releases of one mod may
/// be listed; the one whose version, revision and digest match is that
/// release, and another release of the same id is not. A need that names a
/// mod by name and not by id matches an installed mod of that name, and the
/// mod being played. Digests are compared only when both the need and the
/// install name one. The mod being played is this release when its id
/// matches and, when its version or digest is known, those match too. A
/// mutator whose need names a digest matches an install only when that
/// install names the same digest, or names none. A map pack the recording
/// names by digest matches a pack with that identity. When that pack is
/// installed and another release of the same pack is also installed, the
/// gap is `play_installed` and `play_folder` is the matching folder: it is
/// not fetched. When only another release of that pack is installed, the
/// gap is `other_release` and is not fetchable; the list's file, registry
/// and release are still filled when it names that digest. A map the
/// player has by name is left out when the recording names no pack, and
/// when no installed pack of that id is a different release. A mutator the
/// player has, and an expansion that is on, are left out. An
/// expansion that is not installed is not turned on. An engine requirement
/// this build meets is
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
