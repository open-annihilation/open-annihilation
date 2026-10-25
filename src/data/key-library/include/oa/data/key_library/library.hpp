// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installed key layouts, read from each folder's manifest. Settings, the
// editor and the Library share this list. It never opens a package zip.
#pragma once

#include "oa/formats/oakeys/manifest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::key_library {

/// The folder of the player's own folder that holds installed layouts.
inline constexpr std::string_view keys_folder_name = "Keys";

/// Where a layout came from, as its origin record states it.
enum class Origin {
    file,      ///< a folder made by hand, or one whose record does not read
    catalogue, ///< installed from a catalogue
    own,       ///< the player's own changes
};

/// One installed layout, as its folder's manifest and origin record state it.
///
/// A missing or unreadable origin record is not a problem: the folder may
/// have been made by hand, and `origin` is then `file`.
struct Installed {
    oa::formats::oakeys::Manifest manifest{}; ///< its oakeys.yaml
    std::filesystem::path folder{};           ///< the folder
    Origin origin{Origin::file};              ///< where it came from
};

/// What a Keys folder holds, and the folders that could not be listed.
struct Listing {
    std::vector<Installed> layouts{};    ///< installed layouts, sorted by id
    std::vector<std::string> problems{}; ///< folders left out, and why
};

/// The folder name of the player's own changes for one layout.
///
/// @param base_id the layout's id, such as `oa-default`
/// @return `own-` and that id
[[nodiscard]] std::string own_folder_name(std::string_view base_id);

/// Lists the layouts installed in a Keys folder.
///
/// A missing folder is an empty listing. A folder whose name starts with '.'
/// is skipped, and so is a symbolic link, which is never followed. A plain
/// file at the top is neither listed nor opened. Each real directory is read
/// for `oakeys.yaml` (at most 256 KiB) and `.oa-origin.yaml` (at most 4,096
/// bytes) and for nothing else. A folder with no readable manifest, or one
/// with problems, is left out and named in `problems`. Two folders with one
/// id keep the folder named as the id, or else the first by name, and the
/// other is named in `problems`.
///
/// @param keys the Keys folder
/// @return the layouts and the problems
[[nodiscard]] Listing list_installed(const std::filesystem::path& keys);

/// Finds a listed layout by its id.
///
/// @param listing a listing from list_installed
/// @param id the layout's id
/// @return the layout in `listing`, or null when none has that id
[[nodiscard]] const Installed* find(const Listing& listing, std::string_view id) noexcept;

/// Writes the player's own changes for one layout, whole or not at all.
///
/// Creates `keys/own-<base id>/` and writes `oakeys.yaml` and an origin
/// record `origin: own` under temporary names in that folder, then renames
/// them over the old files. Nothing is written outside that folder. A base
/// id that is empty, is `..`, or holds a path separator is refused, and so
/// is a folder that is a symbolic link.
///
/// @param keys the Keys folder
/// @param base_id the layout the changes sit on
/// @param manifest the own layout's manifest
/// @param[out] error why the write was refused; empty on success
/// @return true when both files were replaced
[[nodiscard]] bool write_own(
    const std::filesystem::path& keys,
    std::string_view base_id,
    const oa::formats::oakeys::Manifest& manifest,
    std::string& error
);

} // namespace oa::data::key_library
