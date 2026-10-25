// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// This build, as a check for a new release names it: the version, the
// release number, whether a label marks it as a development build, and the
// package id the feed uses for it.
#pragma once

#include "oa/formats/oamod/package_keys.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace oa::app::update {

/// The build that is asking whether a feed has a newer release.
struct ThisBuild {
    formats::oamod::EngineVersion version{}; ///< the version compiled into this build
    int64_t release{};        ///< the release number of that version; 0 when it has none
    std::string version_text; ///< the version as text, without a label
    bool development{};       ///< true when the version label is not empty
    std::string package;      ///< the package id, such as windows-x64 or steam-deck
};

/// The package id for a platform name.
///
/// `windows-95` when `windows_95` is set. `steam-deck` when the platform is
/// `linux-x86_64` and `steam_deck_folder` is set. Otherwise the platform
/// name itself, including `ios`, which no package uses.
///
/// @param platform_name the name release_platform_name would give
/// @param windows_95 this build is a Windows 95 build
/// @param steam_deck_folder the program's folder holds a steam-deck folder
/// @return the package id
[[nodiscard]] std::string
package_id(std::string_view platform_name, bool windows_95, bool steam_deck_folder);

/// The build that is running, and the package its program folder selects.
///
/// The version and the label are the ones compiled into this module. A
/// Windows 95 build is `windows-95`. A Linux x86_64 build whose program
/// folder holds a `steam-deck` directory is `steam-deck`. Any other build
/// uses its platform name.
///
/// @param program_folder the folder that holds the program; empty skips the Deck folder
/// @return this build
[[nodiscard]] ThisBuild this_build(const std::filesystem::path& program_folder);

} // namespace oa::app::update
