// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// This build's version, release number and package id.
#include "oa/app/update/build.hpp"

#include "oa/app/version_line.hpp"
#include "oa/data/engine_feed/feed.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::app::update {
namespace {

/// True when this translation unit was built for Windows 95.
///
/// @return true when OA_WINDOWS_95 is defined
[[nodiscard]] bool windows_95_build() noexcept {
#if defined(OA_WINDOWS_95)
    return true;
#else
    return false;
#endif
}

/// The version text compiled into this module.
///
/// @return the version macro, or an empty view when it was not defined
[[nodiscard]] std::string_view compiled_version() noexcept {
#if defined(OA_ENGINE_VERSION)
    return OA_ENGINE_VERSION;
#else
    return {};
#endif
}

/// The version label compiled into this module.
///
/// @return the label, or an empty view when it was not defined
[[nodiscard]] std::string_view compiled_label() noexcept {
#if defined(OA_ENGINE_VERSION_LABEL)
    return OA_ENGINE_VERSION_LABEL;
#else
    return {};
#endif
}

/// True when `folder` is a directory.
///
/// A missing path is not a directory. The check does not throw.
///
/// @param folder the path
/// @return true when it is a directory
[[nodiscard]] bool folder_exists(const std::filesystem::path& folder) noexcept {
    std::error_code error;
    return std::filesystem::is_directory(folder, error);
}

} // namespace

std::string package_id(std::string_view platform_name, bool windows_95, bool steam_deck_folder) {
    if (windows_95)
        return "windows-95";
    if (platform_name == "linux-x86_64" && steam_deck_folder)
        return "steam-deck";
    return std::string(platform_name);
}

ThisBuild this_build(const std::filesystem::path& program_folder) {
    ThisBuild built;
    const std::string_view version_text = compiled_version();
    const std::optional<formats::oamod::EngineVersion> parsed =
        formats::oamod::parse_engine_version(version_text);
    if (parsed) {
        built.version = *parsed;
        built.version_text = formats::oamod::engine_version_text(*parsed);
        const std::optional<int64_t> number = oa::data::engine_feed::release_number(*parsed);
        built.release = number ? *number : 0;
    } else {
        built.version_text = std::string(version_text);
    }
    built.development = !compiled_label().empty();
    const bool deck = folder_exists(program_folder / "steam-deck");
    built.package = package_id(release_platform_name(), windows_95_build(), deck);
    return built;
}

} // namespace oa::app::update
