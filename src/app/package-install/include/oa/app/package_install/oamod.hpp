// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oamod kind: its manifest resolved as a mod's profile, what a folder of
// Mods holds, where a mod package installs, and the words the player is shown.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/data/mod_profile.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::package_install::oamod {

/// Reads an oamod package's manifest: the profile resolved twice, the second
/// time with the settings its INI file and the registry give, then the id
/// checked as a folder name and package.incoming filled.
///
/// @param manifest the manifest's bytes
/// @param context the options, the source name and a reader for the INI file
/// @param[in,out] package receives the profile, its warnings and what it installs
/// @param[out] problem why it was refused
/// @return true when the package can be installed
[[nodiscard]] bool read_manifest(
    std::span<const uint8_t> manifest,
    const ManifestContext& context,
    Package& package,
    Problem& problem
);

/// Reads what a folder in Mods holds: its oamod.yaml's id, name, version
/// and packaging.revision, and the folder's origin record when it has one,
/// read without resolving the profile. The origin's SHA-256 is the installed
/// package file's digest. A folder with no record leaves that digest unknown.
///
/// @param folder the folder
/// @return what it holds
[[nodiscard]] InstalledPackage read_installed_mod(const std::filesystem::path& folder);

/// Reads what a mod folder's .backup holds.
///
/// @param folder the mod folder
/// @return nothing when it has no .backup; else what it holds, of kind
///         package only when it is a folder, not a link, whose oamod.yaml
///         gives the folder's own id
[[nodiscard]] std::optional<InstalledPackage> read_backup(const std::filesystem::path& folder);

/// Returns the mod a profile installs.
///
/// @param profile the package's profile
/// @return its id, name, version and revision
[[nodiscard]] Incoming incoming_of(const oa::data::mod_profile::ModProfile& profile);

/// Returns the folder-safe form of a version, for Mods/<id>-<version>: ASCII
/// letters lowered, digits, '.', '_' and '-' kept, every other byte '-',
/// runs of '-' made one, '.' and '-' cut from both ends, at most 32 bytes;
/// "version" when nothing is left. "10.2" gives "10.2", "2.1 Beta" "2.1-beta".
///
/// @param version the profile's version
/// @return the folder-safe form
[[nodiscard]] std::string version_folder_part(std::string_view version);

/// Returns the folder names Install alongside tries for a version, in order:
/// <id>-<version>, then <id>-<version>-2 to -alongside_tries.
///
/// @param incoming the mod
/// @return the names
[[nodiscard]] std::vector<std::string> alongside_names(const Incoming& incoming);

/// Plans an install from what the folders of Mods hold. The package's file
/// name never matters: only its id and version do. Its own id's folder
/// that holds it at the same version is updated or reinstalled; else the
/// first of the alongside folders that holds it at that version; else a
/// missing folder of its id is installed into; a folder of its id at
/// another version asks whether to replace it or install alongside; a
/// folder of that name that holds something else is never replaced, and
/// asks to install alongside.
///
/// @param incoming the mod the package installs
/// @param hooks what each folder holds
/// @return the plan
[[nodiscard]] InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks);

/// Returns a version as a prompt names it: "{version}", or "{version}
/// revision {n}" where the versions it is shown beside are the same.
///
/// @param version the version
/// @param revision the revision; 0 names an unknown one
/// @param with_revision the revision is named
/// @return the label
[[nodiscard]] std::string
version_label(std::string_view version, int64_t revision, bool with_revision);

/// Returns the prompt shown while a package is checked or unpacks (INSTALLING
/// MOD), with its progress, or while its files are put in place.
///
/// @param incoming the mod
/// @param done_bytes the bytes hashed or unpacked so far
/// @param total_bytes the package file's size, or the bytes it unpacks to
/// @param phase what the prompt says
/// @param file_name the package's name, which the checking phase names
/// @return the prompt: CANCEL
[[nodiscard]] PackagePrompt installing_prompt(
    const Incoming& incoming,
    uint64_t done_bytes,
    uint64_t total_bytes,
    InstallingPhase phase,
    std::string_view file_name
);

/// Returns the question a plan asks: UPDATE MOD, ANOTHER VERSION, ALREADY
/// INSTALLED or FOLDER IN USE. Where the answer removes something that
/// cannot be brought back (a .backup that exists, or the files a reinstall
/// replaces), CANCEL is marked first.
///
/// @param plan the plan, one of the ask kinds
/// @param incoming the mod
/// @param file_name the package's name
/// @param root the Mods folder, whose alongside folder the text shows
/// @param played the target is the mod the game plays now
/// @return the prompt
[[nodiscard]] PackagePrompt question_prompt(
    const InstallPlan& plan,
    const Incoming& incoming,
    std::string_view file_name,
    const std::filesystem::path& root,
    bool played
);

/// Returns the prompt that says a mod was installed in a folder of its own
/// (MOD INSTALLED).
///
/// @param incoming the mod
/// @param folder its folder
/// @param play_now PLAY NOW is offered; else the text says where to choose it
/// @return the prompt: OPEN FOLDER, PLAY NOW when offered, OK
[[nodiscard]] PackagePrompt
installed_prompt(const Incoming& incoming, const std::filesystem::path& folder, bool play_now);

/// Returns the prompt that says a mod's folder was updated, installed
/// again or rolled back (MOD UPDATED).
///
/// @param change what was done
/// @param now what the folder holds now
/// @param before what it held before
/// @param folder the folder
/// @param result what the change did: whether the earlier version was kept,
///        and where it is when it could not be
/// @param play_now PLAY NOW is offered
/// @return the prompt: OPEN FOLDER, PLAY NOW when offered, OK
[[nodiscard]] PackagePrompt updated_prompt(
    Change change,
    const Incoming& now,
    const InstalledPackage& before,
    const std::filesystem::path& folder,
    const ChangeResult& result,
    bool play_now
);

/// Returns the sentence that says why a package was not installed.
///
/// @param problem the problem
/// @return the sentence
[[nodiscard]] std::string refusal_text(const Problem& problem);

/// Returns the prompt that says a package was not installed (MOD NOT
/// INSTALLED): the reason, at most three of its profile's diagnostics, and
/// whether a change failed.
///
/// @param file_name the package's name
/// @param problem why
/// @param change_failed a change was tried and undone
/// @return the prompt: OK
[[nodiscard]] PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed);

/// Returns the prompt, shown over the settings dialog, that says the Mods
/// page's ROLL BACK changed nothing (MOD NOT ROLLED BACK).
///
/// @param title the mod's name
/// @return the prompt: OK
[[nodiscard]] PackagePrompt roll_back_failed_prompt(std::string_view title);

/// Returns the prompt, shown over the settings dialog, that says the kept
/// version cannot be played, so it was not rolled back to (MOD NOT ROLLED
/// BACK).
///
/// @param title the mod's name
/// @param to the kept version's label
/// @param reason why it cannot be played, a sentence in the language shown
/// @return the prompt: OK
[[nodiscard]] PackagePrompt
roll_back_refused_prompt(std::string_view title, std::string_view to, std::string_view reason);

} // namespace oa::app::package_install::oamod
