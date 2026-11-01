// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unpacking a release zip into a staging folder. The zip holds one top
// folder, named for the release. A name that leaves that folder, or a link,
// is refused before the staging folder is created.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// The most an unpacked entry may allow: read, write and run for its owner,
/// read and run for the group and for others. No set-user, set-group or
/// sticky bit, and nothing the group or others may write.
inline constexpr uint32_t unpacked_mode_limit = 0755;

/// The permission bits an entry is given when it is unpacked on POSIX.
///
/// An entry made on Unix or macOS records its mode, and keeps it within
/// unpacked_mode_limit; the release zips record 0755 for the programs and
/// 0644 for every other file. A folder always keeps read, write and search
/// for its owner, so its entries can be unpacked into it and the bundle can
/// later be moved. An entry that records no mode, one made on another
/// system or one whose permission bits are all clear, is 0644, or 0755 for
/// `open-annihilation`, `oa-intro`, `oa-tool` and `oa-update`; a folder that
/// records none is 0755.
///
/// @param name the entry's name, '/' between folders
/// @param recorded the entry's recorded file type and mode, 0 when it has none
/// @param folder the entry is a folder
/// @return the permission bits
[[nodiscard]] uint32_t
unpacked_mode(std::string_view name, uint32_t recorded, bool folder) noexcept;

/// The top folders a release zip of `version` may hold:
/// `open-annihilation-<version>-<package>`, and the same name with `v`
/// before the version, the spelling of the release's own zips
/// (`open-annihilation-v0.8.8-macos-universal`).
///
/// @param version the release's version text, as the feed gives it
/// @param package the package id
/// @return the two names, the one without `v` first
[[nodiscard]] std::vector<std::string>
top_folder_names(std::string_view version, std::string_view package);

/// Unpacks `archive` into `stage`.
///
/// Every name is checked before `stage` is created. The first name picks
/// the archive's top folder, which must be one of `tops`, and every other
/// name must lie in that same folder. An absolute path, a `..` part, a drive
/// letter, a backslash, a link, a special file, a name outside that folder,
/// or a first name in no folder of `tops` refuses the archive and leaves
/// `stage` uncreated. On a failure after `stage` was created, `stage` is
/// removed. On POSIX each file and folder entry is given the mode
/// unpacked_mode names, whatever the process's file mode mask, and a folder
/// the zip does not name is 0755; Windows keeps the mode a new file has.
///
/// @param archive the release zip
/// @param stage the staging folder, removed when the run ends
/// @param tops the names the zip's one top folder may have
/// @param[out] error why the archive was refused; may be null
/// @return true when every entry was unpacked
[[nodiscard]] bool unpack_package(
    const std::filesystem::path& archive,
    const std::filesystem::path& stage,
    std::span<const std::string> tops,
    std::string* error
);

} // namespace oa::update_helper
