// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installed key layouts, read from each folder's manifest. Settings, the
// editor and the Library share this list. Listing never opens a package
// zip. The packer writes one, the same bytes for the same files.
#pragma once

#include "oa/formats/oakeys/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
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
/// have been made by hand, and `origin` is then `file`. The catalogue facts
/// are empty, or 0, for a file, an own layout or a record that does not
/// read. A file origin keeps the package's SHA-256 only.
struct Installed {
    oa::formats::oakeys::Manifest manifest{}; ///< its oakeys.yaml
    std::filesystem::path folder{};           ///< the folder
    Origin origin{Origin::file};              ///< where it came from
    std::string package_sha256{};             ///< the package file's SHA-256, or empty
    std::string registry{};                   ///< a catalogue origin's registry; empty otherwise
    std::string catalogue_id{};               ///< a catalogue origin's package key; empty otherwise
    int64_t release{};                        ///< a catalogue origin's release; 0 otherwise
};

/// What one name in a key-layout package is.
enum class FileRole {
    manifest, ///< oakeys.yaml, matched without ASCII case
    icon,     ///< the PNG the manifest names, matched exactly
    note,     ///< a top-level name ending in .md or .txt, as written
    outside,  ///< any other file, including one in a folder
};

/// Tells what a name in a key-layout package is.
///
/// `oakeys.yaml` is the manifest, without ASCII case. The icon is the name
/// `icon` states, matched exactly, and only when `icon` is not empty. A
/// top-level name ending in `.md` or `.txt`, as written, is a note. A name
/// that holds a slash, and any other name, is outside. A directory is the
/// caller's to skip: a name ending in '/' is outside.
///
/// @param name the name as written, '/' between folders
/// @param icon the manifest's icon; empty when it names none
/// @return the role
[[nodiscard]] FileRole file_role(std::string_view name, std::string_view icon) noexcept;

/// One file a key-layout package holds.
struct PackageFile {
    std::string name{};           ///< its name as written, '/' between folders
    std::vector<uint8_t> bytes{}; ///< its bytes
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

/// Reads the files a key-layout package may hold from one folder.
///
/// The folder's `oakeys.yaml` is read with the manifest reader. A manifest
/// that does not read is a problem. The result holds that manifest, the PNG
/// it names and the top-level notes. A name that starts with '.' is the
/// install record, or a file the system leaves, and is skipped. Any other
/// file, and any subdirectory, is a problem. A symbolic link is not followed.
/// A named icon that is not in the folder is a problem.
///
/// @param folder the folder
/// @param[out] problem why the folder cannot be packed; unchanged on success
/// @return the files; nothing when the folder cannot be packed
[[nodiscard]] std::optional<std::vector<PackageFile>>
read_folder(const std::filesystem::path& folder, std::string* problem);

/// Writes a key-layout package's bytes.
///
/// `oakeys.yaml` is parsed to learn the icon. A name `file_role` refuses is
/// a problem, and so is a manifest that does not read. `oakeys.yaml` is
/// written first and the rest follow in byte order of name. A name ending
/// in `.png`, exactly, is stored. Every other file is deflated. The same
/// files give the same bytes: the writer records no clock time.
///
/// @param files the files
/// @param[out] problem why the package was not written; unchanged on success
/// @return the archive; nothing when a name or the manifest is refused
[[nodiscard]] std::optional<std::vector<uint8_t>>
package_bytes(std::span<const PackageFile> files, std::string* problem);

/// Writes a key-layout package, whole or not at all.
///
/// The bytes go to `<out>.part`, which is then renamed over `out`. A crash
/// mid-write leaves the previous file whole.
///
/// @param out the package file
/// @param bytes the archive
/// @param[out] problem why the write failed; unchanged on success
/// @return true when `out` holds the bytes
[[nodiscard]] bool write_package(
    const std::filesystem::path& out, std::span<const uint8_t> bytes, std::string* problem
);

} // namespace oa::data::key_library
