// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unpacking a release zip into a staging folder. The zip holds one top
// folder. A name that leaves that folder, or a link, is refused before the
// staging folder is created.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace oa::update_helper {

/// The top folder a release zip uses: `open-annihilation-<version>-<package>`.
///
/// @param version the release's version text
/// @param package the package id
/// @return the folder name
[[nodiscard]] std::string top_folder_name(std::string_view version, std::string_view package);

/// Unpacks `archive` into `stage`.
///
/// Every name is checked before `stage` is created. An absolute path, a
/// `..` part, a drive letter, a backslash, a link, a special file, or a name
/// outside `top` refuses the archive and leaves `stage` uncreated. On a
/// failure after `stage` was created, `stage` is removed. The executable bit
/// is set on POSIX for `open-annihilation`, `oa-intro`, `oa-tool` and
/// `oa-update`.
///
/// @param archive the release zip
/// @param stage the staging folder, removed when the run ends
/// @param top the one top folder the zip must hold
/// @param[out] error why the archive was refused; may be null
/// @return true when every entry was unpacked
[[nodiscard]] bool unpack_package(
    const std::filesystem::path& archive,
    const std::filesystem::path& stage,
    std::string_view top,
    std::string* error
);

} // namespace oa::update_helper
