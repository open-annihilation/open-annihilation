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
#include <utility>
#include <vector>

#ifndef OA_PACKAGER_NAME
#error "OA_PACKAGER_NAME is who updates this copy when it cannot replace itself"
#endif
#ifndef OA_PACKAGER_UPDATE_COMMAND
#error "OA_PACKAGER_UPDATE_COMMAND is a command a player can type, or empty"
#endif

namespace oa::app::update {
namespace {

/// Copies one looked-up word before the next lookup replaces it.
///
/// @param lookup the words, or null for the English
/// @param context what lookup is called with
/// @param english the word
/// @return the word, copied
[[nodiscard]] std::string
looked_up(PackagerLookup lookup, void* context, std::string_view english) {
    if (lookup == nullptr)
        return std::string(english);
    const std::string_view translated = lookup(context, english);
    return std::string(translated);
}

/// Fills one {place} in a copied word. The value is not looked up.
///
/// @param[in,out] text the word
/// @param place the place's name, without braces
/// @param value what replaces {place}
void fill_place(std::string& text, std::string_view place, std::string_view value) {
    const std::string token = std::string("{") + std::string(place) + "}";
    std::string::size_type at = 0;
    while ((at = text.find(token, at)) != std::string::npos) {
        text.replace(at, token.size(), value.data(), value.size());
        at += value.size();
    }
}

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

std::string_view packager_name() noexcept {
    return OA_PACKAGER_NAME;
}

std::string_view packager_update_command() noexcept {
    return OA_PACKAGER_UPDATE_COMMAND;
}

bool replaces_itself() noexcept {
    return self_update_built;
}

PackagerNotice packager_notice(
    std::string_view version,
    std::string_view name,
    std::string_view command,
    PackagerLookup lookup,
    void* context
) {
    PackagerNotice notice;
    notice.title = looked_up(lookup, context, version_is_out);
    fill_place(notice.title, "version", version);
    if (name.empty()) {
        notice.paragraphs.push_back(looked_up(lookup, context, cannot_replace));
        notice.open_the_page = true;
        return notice;
    }
    std::string from = looked_up(lookup, context, copy_from_packager);
    fill_place(from, "name", name);
    notice.paragraphs.push_back(std::move(from));
    if (command.empty()) {
        notice.paragraphs.push_back(looked_up(lookup, context, open_centre));
        return notice;
    }
    notice.paragraphs.push_back(looked_up(lookup, context, open_centre_or_run));
    notice.paragraphs.emplace_back(command);
    return notice;
}

} // namespace oa::app::update
