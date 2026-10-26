// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oakeys kind: a key layout's manifest, what a folder of Keys holds,
// where a layout installs, and the words the player is shown.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/prompts.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::package_install::oakeys {

/// The extension of a key-layout package, matched without case.
inline constexpr std::string_view oakeys_extension = ".oakeys";

/// The folder of the player's own folder that holds installed layouts.
///
/// The same name as oa::data::key_library::keys_folder_name. This header
/// does not include that library: the module links it privately, and a
/// public header would then fail to compile for a target that does not.
inline constexpr std::string_view keys_folder_name = "Keys";

/// The most bytes of a key layout's icon. A larger file is refused.
inline constexpr uint64_t most_icon_bytes = uint64_t{256} << 10;

/// The most pixels on one side of a key layout's icon.
inline constexpr uint32_t most_icon_side = 256;

/// Reads an oakeys package's manifest, checks the files it may hold, that
/// its icon is a square PNG within the limits, and that requires.engine is
/// met by this build. Warnings from the layout fill incoming.cannot_use.
/// Nothing is written.
///
/// @param manifest the manifest's bytes
/// @param context the options, the source name and a reader for the icon
/// @param[in,out] package receives what the layout installs
/// @param[out] problem why it was refused
/// @return true when the package can be installed
[[nodiscard]] bool read_manifest(
    std::span<const uint8_t> manifest,
    const ManifestContext& context,
    Package& package,
    Problem& problem
);

/// Reads what a folder in Keys holds: its oakeys.yaml's id, name and version.
///
/// A folder named own-…, or one whose origin record says the player's own
/// changes, is not a package: an install leaves it as it is.
///
/// @param folder the folder
/// @return what it holds; other when it is not a folder of a key layout
[[nodiscard]] InstalledPackage read_installed(const std::filesystem::path& folder);

/// Reads what a key layout's .backup holds.
///
/// @param folder the layout's folder
/// @return nothing when it has no .backup; else what it holds, of kind
///         package only when it is a folder, not a link, whose oakeys.yaml
///         gives the folder's own id
[[nodiscard]] std::optional<InstalledPackage> read_backup(const std::filesystem::path& folder);

/// Plans an install from what Keys/<id> holds and from both origin records.
///
/// A missing folder is installed with no question when the package came from
/// a catalogue, and asks first when the player opened the file. Nothing is
/// installed alongside. A catalogue package whose target's record names the
/// same registry replaces that folder with no question. The same id from
/// another registry asks. The same version asks to reinstall, another version
/// to update. A folder without this layout's oakeys.yaml, a folder the
/// listing marks as the player's own changes, and a folder named own-…, are
/// left alone and the install refused. The incoming revision is always 0.
///
/// @param incoming the layout the package installs, origin included
/// @param hooks what the folder holds
/// @return the plan
[[nodiscard]] InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks);

/// Returns the prompt shown while a key layout is checked, unpacked or put
/// in place (INSTALLING KEY LAYOUT).
///
/// @param incoming the layout
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

/// Returns the question a plan asks.
///
/// A new layout the player opened asks "Install the key layout {name}
/// {version}?" and, when this build cannot use something in it, lists up to
/// three of those. An update and a reinstall name the versions, and a
/// replace across registries names both. Nothing offers Install alongside.
///
/// @param plan the plan, one of the ask kinds
/// @param incoming the layout
/// @param file_name the package's name
/// @param root the Keys folder
/// @param played unused; a layout is never what the game plays
/// @return the prompt
[[nodiscard]] PackagePrompt question_prompt(
    const InstallPlan& plan,
    const Incoming& incoming,
    std::string_view file_name,
    const std::filesystem::path& root,
    bool played
);

/// Returns the prompt that says a key layout was installed (KEY LAYOUT
/// INSTALLED).
///
/// It tells the player to choose USE in the Library's Keys tab. PLAY NOW is
/// not offered: installing never switches a layout on.
///
/// @param incoming the layout
/// @param folder its folder
/// @param play_now unused; PLAY NOW is not offered
/// @return the prompt: OPEN FOLDER, OK
[[nodiscard]] PackagePrompt
installed_prompt(const Incoming& incoming, const std::filesystem::path& folder, bool play_now);

/// Returns the prompt that says a key layout's folder was updated or
/// installed again (KEY LAYOUT UPDATED).
///
/// @param change what was done
/// @param now what the folder holds now
/// @param before what it held before
/// @param folder the folder
/// @param result what the change did
/// @param play_now unused; PLAY NOW is not offered
/// @return the prompt: OPEN FOLDER, OK
[[nodiscard]] PackagePrompt updated_prompt(
    Change change,
    const Incoming& now,
    const InstalledPackage& before,
    const std::filesystem::path& folder,
    const ChangeResult& result,
    bool play_now
);

/// Returns the sentence that says why a key layout was not installed.
///
/// @param problem the problem
/// @return the sentence
[[nodiscard]] std::string refusal_text(const Problem& problem);

/// Returns the prompt that says a key layout was not installed (KEY LAYOUT
/// NOT INSTALLED): the reason, and at most three of its manifest's diagnostics.
///
/// @param file_name the package's name
/// @param problem why
/// @param change_failed a change was tried and undone
/// @return the prompt: OK
[[nodiscard]] PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed);

} // namespace oa::app::package_install::oakeys
