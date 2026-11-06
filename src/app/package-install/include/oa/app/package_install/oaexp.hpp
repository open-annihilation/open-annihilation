// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oaexp kind: an expansion package. Its files are the built-in table's
// files, by name, size and SHA-256, and they are copied into
// Expansions/<game>/<id> with the .oa-content record. The game's install
// path is install_oaexp, which uses that table only.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::package_install::oaexp {

/// The extension of an expansion package, matched without case.
inline constexpr std::string_view oaexp_extension = ".oaexp";

/// The manifest's file name.
inline constexpr std::string_view manifest_file_name = "oaexp.yaml";

/// The package an install came from, as the expansion's record keeps it.
struct PackageFrom {
    /// The package file's SHA-256 when it is already known, as a catalogue
    /// download's is. Nothing reads the file again to hash it then.
    std::optional<oa::base::sha256::Digest> sha256{};
    /// The catalogue's release; 0 for a file the player opened.
    int64_t release{};
};

/// Reads an expansion package's manifest and checks that every file is the
/// built-in table's file. A package whose bytes the table does not know is
/// refused. Fills package.incoming.
///
/// @param manifest the manifest's bytes
/// @param context the options and a reader for the other files
/// @param[in,out] package receives what the expansion installs
/// @param[out] problem why it was refused
/// @return true when the package can be installed
[[nodiscard]] bool read_manifest(
    std::span<const uint8_t> manifest,
    const ManifestContext& context,
    Package& package,
    Problem& problem
);

/// Reads a folder the generic installer walks. An expansion is not one of
/// those packages: the folder is reported as something else, so it is not
/// listed beside mods, map packs and languages.
///
/// @param folder the folder
/// @return other
[[nodiscard]] InstalledPackage read_installed(const std::filesystem::path& folder);

/// Reads a backup. An expansion keeps none.
///
/// @param folder the folder
/// @return nothing
[[nodiscard]] std::optional<InstalledPackage> read_backup(const std::filesystem::path& folder);

/// Refuses the generic install. An expansion is copied by install_oaexp,
/// which writes the table's files and the .oa-content record.
///
/// @param incoming the expansion
/// @param hooks what the folder holds; unused
/// @return a refusal
[[nodiscard]] InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks);

/// Returns the prompt shown while an expansion package is checked.
///
/// @param incoming the expansion
/// @param done_bytes the bytes hashed or copied so far
/// @param total_bytes the package file's size, or the bytes it copies
/// @param phase what the prompt says
/// @param file_name the package's name
/// @return the prompt: CANCEL
[[nodiscard]] PackagePrompt installing_prompt(
    const Incoming& incoming,
    uint64_t done_bytes,
    uint64_t total_bytes,
    InstallingPhase phase,
    std::string_view file_name
);

/// Returns the question a file the player opened would ask.
///
/// @param plan the plan
/// @param incoming the expansion
/// @param file_name the package's name
/// @param root the folder packages of this kind use
/// @param played unused; an expansion is not played as a mod
/// @return the prompt: CANCEL, and INSTALL
[[nodiscard]] PackagePrompt question_prompt(
    const InstallPlan& plan,
    const Incoming& incoming,
    std::string_view file_name,
    const std::filesystem::path& root,
    bool played
);

/// Returns the prompt shown when an expansion the player opened is in place.
///
/// @param incoming the expansion
/// @param folder the folder it was copied into
/// @param play_now unused; an expansion is turned on in the Library
/// @return the prompt: OPEN FOLDER and OK
[[nodiscard]] PackagePrompt
installed_prompt(const Incoming& incoming, const std::filesystem::path& folder, bool play_now);

/// Returns the prompt shown when an expansion already in place is copied again.
///
/// @param change what the copy did
/// @param now the expansion
/// @param before what the folder held
/// @param folder the folder
/// @param result what the copy left behind
/// @param play_now unused
/// @return the prompt: OPEN FOLDER and OK
[[nodiscard]] PackagePrompt updated_prompt(
    Change change,
    const Incoming& now,
    const InstalledPackage& before,
    const std::filesystem::path& folder,
    const ChangeResult& result,
    bool play_now
);

/// Returns why an expansion package was refused, in the player's words.
///
/// @param problem the refusal
/// @return the sentence
[[nodiscard]] std::string refusal_text(const Problem& problem);

/// Returns the prompt shown when an expansion package was refused.
///
/// @param file_name the package's name
/// @param problem the refusal
/// @param change_failed true when the copy itself failed
/// @return the prompt: OK
[[nodiscard]] PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed);

/// Copies an expansion package into the player's folder.
///
/// The manifest's id is a built-in expansion, and every file is checked
/// against that expansion's table. Bytes the table does not know are not
/// written. The copy lands in Expansions/<game>/<id>, using the expansions
/// folder's name, with a .oa-content record. `registry_id` empty records the
/// package file's path as the source; otherwise the source is
/// "registry <id>". A registry id that holds a newline is refused. The
/// record also names the package: its SHA-256, the file's own unless `from`
/// gives it, and `from`'s release.
///
/// Over a copy already in place, whose record names this expansion, the
/// files are replaced where they are: a file already holding the right bytes
/// is left as it is, the folders stay, and a file the package does not hold,
/// such as one the game wrote beside its own, is kept. A file the old record
/// named that this package no longer holds is removed, and so is an empty
/// folder a file sync left beside one of the package's folders under a
/// numbered name ("Help 2").
///
/// @param package the .oaexp file
/// @param user_folder the player's folder
/// @param registry_id the registry the package came from; empty for a file
/// @param[out] installed the folder it was copied into; may be null
/// @param[out] problem why it was refused
/// @param from the package's SHA-256, when known, and its catalogue release
/// @return true when the expansion is in place
[[nodiscard]] bool install_oaexp(
    const std::filesystem::path& package,
    const std::filesystem::path& user_folder,
    std::string_view registry_id,
    std::filesystem::path* installed,
    Problem& problem,
    const PackageFrom& from = {}
);

/// Copies a package whose files match `files`.
///
/// The manifest is still read against the built-in table, so the id, the
/// name, extends and needs are that expansion's. `files` is the table the
/// bytes are compared to. The game does not call this: install_oaexp passes
/// the built-in bundle. A test passes its own table, whose bytes the built-in
/// table does not know, and install_oaexp refuses that package.
///
/// @param package the .oaexp file
/// @param user_folder the player's folder
/// @param registry_id the registry the package came from; empty for a file
/// @param files the table the bytes must match
/// @param[out] installed the folder it was copied into; may be null
/// @param[out] problem why it was refused
/// @param from the package's SHA-256, when known, and its catalogue release
/// @return true when the expansion is in place
[[nodiscard]] bool install_expansion_package(
    const std::filesystem::path& package,
    const std::filesystem::path& user_folder,
    std::string_view registry_id,
    const oa::data::content_bundles::Bundle& files,
    std::filesystem::path* installed,
    Problem& problem,
    const PackageFrom& from = {}
);

/// Removes an installed expansion.
///
/// The folder is removed only when its .oa-content record names `id`. A
/// link is left in place. Another expansion's folder, and the game's folder,
/// are left in place.
///
/// @param user_folder the player's folder
/// @param id the expansion's id
/// @param[out] problem why it was not removed; may be anything when this returns true
/// @return true when the expansion is not installed, including when it was not
[[nodiscard]] bool uninstall_expansion(
    const std::filesystem::path& user_folder, std::string_view id, Problem& problem
);

} // namespace oa::app::package_install::oaexp
