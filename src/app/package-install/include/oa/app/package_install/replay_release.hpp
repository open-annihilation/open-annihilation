// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A release a replay names, kept beside the one the player uses. The folder
// is under the player's own folder, not under Mods or Maps, so the Mods tab
// does not list it, an update is not offered for it, and uninstalling the
// release in use leaves it where it is.
#pragma once

#include "oa/app/package_install/origin.hpp"
#include "oa/data/map_pack/manifest.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app::package_install {

/// The folder of the player's own folder that holds releases a replay needs
/// beside the one in use. Each release is a folder named with the package
/// file's SHA-256.
inline constexpr std::string_view replay_releases_folder_name = "Replay releases";

/// Returns the folder a replay's release is kept in, when that digest is
/// already there and its origin record names the same digest.
///
/// @param player_folder the player's own folder
/// @param sha256 the package file's SHA-256, 64 lower-case hex digits
/// @return the folder; nothing when it is not kept, or its record names another digest
[[nodiscard]] std::optional<std::filesystem::path>
replay_release_folder(const std::filesystem::path& player_folder, std::string_view sha256);

/// The pack `folder` lists, when its `oamap.yaml` reads as a package and
/// names an id.
///
/// @param folder the pack's folder
/// @return the manifest; nothing when the file is missing or does not read
[[nodiscard]] std::optional<oa::data::map_pack::Manifest>
installed_pack_manifest(const std::filesystem::path& folder);

/// Installs a package into Replay releases, in a folder named with the
/// package file's SHA-256. An existing folder whose origin names that digest
/// is left as it is when it still reads as a mod or a map pack. A folder
/// that is already there and does not verify (no record, another digest, a
/// half copy, or a record whose package no longer reads) is replaced.
/// Nothing under Mods, Maps or any other kind root is written.
///
/// @param player_folder the player's own folder
/// @param package_file the package
/// @param origin the origin to record; its sha256 must be set, and its
///        installed date is filled with today when it is empty
/// @param[out] error why it was not installed; may be null
/// @return the folder; nothing when the package could not be installed
[[nodiscard]] std::optional<std::filesystem::path> install_replay_release(
    const std::filesystem::path& player_folder,
    const std::filesystem::path& package_file,
    const Origin& origin,
    std::string* error
);

} // namespace oa::app::package_install
