// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installed brains, read from each folder's manifest. The Library and the
// game share this list. It never reads a script, a network or a table.
#pragma once

#include "oa/formats/oabrain/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::brain_library {

/// The folder of the player's own folder that holds installed brains.
inline constexpr std::string_view brains_folder_name = "Brains";

/// One installed brain, as its folder's manifest and origin record state it.
///
/// The origin facts are empty, or 0, when the folder has no readable origin
/// record. A missing record is not a problem: the folder may have been made
/// by hand. `network_bytes` is the sizes of the network files the manifest
/// names that exist, added together. A missing file adds nothing, and no
/// network is opened.
struct InstalledBrain {
    std::string folder_name{};                 ///< the folder's name in Brains
    std::filesystem::path folder{};            ///< the folder
    oa::formats::oabrain::Manifest manifest{}; ///< its oabrain.yaml
    std::string package_sha256{};              ///< the package file's SHA-256, or empty
    std::string registry{};                    ///< a catalogue origin's registry; empty otherwise
    std::string catalogue_id{}; ///< a catalogue origin's package key; empty otherwise
    int64_t release{};          ///< a catalogue origin's release; 0 otherwise
    uint64_t network_bytes{};   ///< sizes of the named network files that exist
};

/// What a Brains folder holds, and the folders that could not be listed.
struct Listing {
    std::vector<InstalledBrain> brains{}; ///< installed brains, sorted by id
    std::vector<std::string> problems{};  ///< folders left out, and why
};

/// Lists the brains installed in a Brains folder.
///
/// A missing folder is an empty listing. A folder whose name starts with '.'
/// is the installer's own and is skipped, and so is a symbolic link, which
/// is never followed. A plain file at the top is neither listed nor opened.
/// Each real directory is read for `oabrain.yaml` (at most 256 KiB) and
/// `.oa-origin.yaml` (at most 4,096 bytes) and for nothing else. A network
/// file the manifest names is measured, when it is a regular file inside the
/// folder, and not opened. A folder with no readable manifest, or one with
/// problems, is left out and named in `problems`. Two folders with one id
/// keep the folder named as the id, or else the first by name, and the other
/// is named in `problems`.
///
/// @param brains_folder the Brains folder
/// @return the brains and the problems
[[nodiscard]] Listing list_installed(const std::filesystem::path& brains_folder);

/// Finds a listed brain by its id.
///
/// @param listing a listing from list_installed
/// @param id the brain's id
/// @return the brain in `listing`, or null when none has that id
[[nodiscard]] const InstalledBrain* find(const Listing& listing, std::string_view id) noexcept;

} // namespace oa::data::brain_library
