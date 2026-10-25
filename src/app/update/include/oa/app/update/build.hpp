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
#include <vector>

#ifndef OA_SELF_UPDATE
#error "OA_SELF_UPDATE (0 or 1) says whether the game may replace itself"
#endif

namespace oa::app::update {

/// True when this build may replace itself through oa-update.
inline constexpr bool self_update_built = OA_SELF_UPDATE != 0;

/// The notice's title. {version} is the release that is out.
inline constexpr std::string_view version_is_out = "{version} is out";
/// Who shipped this copy, when a packager is named. {name} is that packager.
inline constexpr std::string_view copy_from_packager =
    "This copy came from {name}, which updates it.";
/// Where to update, when the packager also gives a command. The command
/// follows on its own line and is not one of these words.
inline constexpr std::string_view open_centre_or_run = "Open your software centre, or run:";
/// Where to update, when a packager is named and gives no command.
inline constexpr std::string_view open_centre = "Open your software centre to update it.";
/// Where to get a release when nobody is named, as on iOS.
inline constexpr std::string_view cannot_replace =
    "This copy cannot replace itself. Get the new release from its page.";

/// Looks up one of the notice's words. A null lookup leaves the English.
///
/// The view is valid only until the next call. packager_notice copies it
/// before it asks again. The command is never passed here.
using PackagerLookup = std::string_view (*)(void* context, std::string_view english);

/// The notice a build that cannot replace itself shows for a release.
struct PackagerNotice {
    std::string title; ///< {version} is out, filled
    std::vector<std::string>
        paragraphs; ///< the lines; the command is its own and was not looked up
    /// True when no packager is named: the notice offers OPEN THE PAGE and OK.
    bool open_the_page{};
};

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

/// Who updates this copy when it cannot replace itself.
///
/// Empty when the build names nobody, which is the default and the iOS
/// build. The text is the one configured, not a translation.
///
/// @return the packager's name
[[nodiscard]] std::string_view packager_name() noexcept;

/// A command a player can type to update this copy.
///
/// Empty when the configuration gives none. Shown as written, on its own
/// line, and never translated. Ignored when packager_name is empty.
///
/// @return the command
[[nodiscard]] std::string_view packager_update_command() noexcept;

/// Whether this build may replace itself.
///
/// The same answer as self_update_built. It is a function so a caller
/// compiles both the prompt that replaces the game and the notice that
/// does not: a constant in the caller's own file would leave one of them
/// unbuilt on a compiler that drops the other.
///
/// @return true when this build may replace itself
[[nodiscard]] bool replaces_itself() noexcept;

/// The notice for a release this build will not install itself.
///
/// A name and a command give who updates the copy, then "Open your software
/// centre, or run:", then the command as its own paragraph. A name alone
/// asks the player to open the software centre. No name, which ignores any
/// command, says the copy cannot replace itself and that the release is on
/// its page. {version} and {name} are filled after the words are looked up,
/// so the values are not translated. The command is not looked up.
///
/// @param version the release that is out
/// @param name who updates this copy; empty names nobody
/// @param command a command a player can type; empty for none; ignored when name is empty
/// @param lookup the words, or null for the English; the command is not passed
/// @param context what lookup is called with
/// @return the notice
[[nodiscard]] PackagerNotice packager_notice(
    std::string_view version,
    std::string_view name,
    std::string_view command,
    PackagerLookup lookup,
    void* context
);

} // namespace oa::app::update
