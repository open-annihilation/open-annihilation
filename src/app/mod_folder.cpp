// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A game folder's profile with the settings it binds, the mod folders a game
// folder offers, and what a profile sets for the data layout, discovery and
// the movies.

#include "oa/app/mod_profile_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace mod_profile = data::mod_profile;

/// Lower-cases ASCII letters.
///
/// @param text the text
/// @return the text with A-Z made a-z
std::string lowered(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

/// Writes a path as UTF-8.
///
/// @param path the path
/// @return its text
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// The folder whose own profile this mount plays.
///
/// A layered mod folder carries the profile. With no mod folder, the game
/// folder does: it is the last folder. An expansion bundle ahead of it is
/// not a mod.
///
/// @param folders the loose roots, highest precedence first; not empty
/// @param choice the mod folder and the --mod file
/// @return the folder to read oamod.yaml from
const fs::path& profile_folder(const std::vector<fs::path>& folders, const ModChoice& choice) {
    if (!choice.folder.empty())
        return choice.folder;
    return folders.back();
}

/// Trims spaces and tabs from both ends.
///
/// @param text the text
/// @return the text without them
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

/// Finds an entry of a folder by name, compared without case.
///
/// @param folder the folder
/// @param name the name
/// @return the entry's path, or nullopt
std::optional<fs::path> entry_named(const fs::path& folder, std::string_view name) {
    std::error_code error;
    const auto wanted = lowered(name);
    for (fs::directory_iterator entry{folder, error}, end; !error && entry != end;
         entry.increment(error))
        if (lowered(utf8(entry->path().filename())) == wanted)
            return entry->path();
    return std::nullopt;
}

/// Reads a file of at most mod_ini_most_bytes.
///
/// @param file the file
/// @return its bytes, or nullopt when it cannot be read or is too large
std::optional<std::string> read_small_file(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > mod_ini_most_bytes)
        return std::nullopt;
    std::ifstream in{file, std::ios::binary};
    if (!in)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Reads a profile's bytes, one past the largest the profile reader takes,
/// so that it refuses a file too large.
///
/// @param file the profile
/// @param[out] bytes its bytes
/// @return true when the file could be read
bool read_profile_bytes(const fs::path& file, std::vector<uint8_t>& bytes) {
    std::ifstream in{file, std::ios::binary};
    if (!in)
        return false;
    bytes.clear();
    for (std::istreambuf_iterator<char> at{in}, end;
         at != end && bytes.size() <= formats::oamod::max_input_bytes;
         ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    return true;
}

/// Adds each diagnostic, formatted, to a list of lines.
///
/// @param diagnostics the diagnostics
/// @param[out] lines receives one line each
void add_lines(
    const std::vector<mod_profile::Diagnostic>& diagnostics, std::vector<std::string>& lines
) {
    for (const auto& diagnostic : diagnostics)
        lines.push_back(mod_profile::format_diagnostic(diagnostic));
}

} // namespace

mod_profile::Settings mod_settings_of(
    const mod_profile::ModProfile& profile,
    const std::vector<fs::path>& folders,
    const platform::preferences::Values* preferences
) {
    for (const auto& folder : folders) {
        const auto file = entry_named(folder, profile.identity.settings_file);
        if (!file)
            continue;
        const auto text = read_small_file(*file);
        return mod_settings_from(
            profile,
            text ? std::optional<std::string_view>(*text) : std::optional<std::string_view>(),
            preferences
        );
    }
    return mod_settings_from(profile, std::nullopt, preferences);
}

mod_profile::Settings mod_settings_from(
    const mod_profile::ModProfile& profile,
    std::optional<std::string_view> ini_text,
    const platform::preferences::Values* preferences
) {
    mod_profile::Settings settings{};
    if (ini_text)
        settings.ini = read_ini_settings(*ini_text);
    if (preferences != nullptr) {
        const auto prefix = registry_key_prefix(profile) + std::string(registry_game_section) + '|';
        for (const auto& [key, value] : *preferences)
            if (key.size() > prefix.size() && key.starts_with(prefix))
                settings.registry.push_back({key.substr(prefix.size()), value});
    }
    return settings;
}

FolderProfile
resolve_folder_profile(const std::vector<fs::path>& folders, const ModChoice& choice) {
    FolderProfile result{};
    if (folders.empty())
        return result;
    std::string error;
    if (!choice.folder.empty()) {
        // A mod folder layers over plain 3.1c only.
        const auto base_profile = find_mod_profile(folders.back(), error);
        if (!error.empty()) {
            result.errors.push_back(error);
            return result;
        }
        if (base_profile) {
            result.errors.push_back(
                utf8(folders.back()) +
                ": the game folder holds its own oamod.yaml, so it is a mod's copied install "
                "and cannot carry a mod folder"
            );
            return result;
        }
    }
    fs::path file = choice.profile_file;
    if (file.empty()) {
        const auto found = find_mod_profile(profile_folder(folders, choice), error);
        if (!error.empty()) {
            result.errors.push_back(error);
            return result;
        }
        // A folder without a profile, a mod folder among them, plays by
        // 3.1c's own rules; a mod folder's files still layer over the game
        // folder's. An expansion bundle is not that folder.
        if (!found)
            return result;
        file = *found;
    }
    result.file = file;
    // The first pass names the settings file and registry root; the second
    // resolves with the settings they hold.
    const auto first = load_mod_profile(file, choice.accept_unimplemented_hacks);
    if (!first.resolution) {
        add_lines(first.errors, result.errors);
        add_lines(first.warnings, result.warnings);
        return result;
    }
    const auto settings = mod_settings_of(first.resolution->profile, folders, choice.preferences);
    const auto second = load_mod_profile(file, choice.accept_unimplemented_hacks, settings);
    add_lines(second.errors, result.errors);
    add_lines(second.warnings, result.warnings);
    if (second.resolution)
        result.profile =
            std::make_shared<const mod_profile::ModProfile>(second.resolution->profile);
    return result;
}

std::optional<ProfileSource>
folder_profile_source(const std::vector<fs::path>& folders, const ModChoice& choice) {
    if (folders.empty())
        return std::nullopt;
    fs::path file = choice.profile_file;
    if (file.empty()) {
        std::string error;
        const auto found = find_mod_profile(profile_folder(folders, choice), error);
        if (!found)
            return std::nullopt;
        file = *found;
    }
    ProfileSource source{};
    if (!read_profile_bytes(file, source.text))
        return std::nullopt;
    source.name = utf8(file);
    source.options.accept_unimplemented_hacks = choice.accept_unimplemented_hacks;
    // The first pass names the settings file and registry root, as
    // resolve_folder_profile's does.
    const auto first = mod_profile::resolve_profile(source.text, source.name, source.options);
    if (!first.resolution)
        return std::nullopt;
    source.options.settings =
        mod_settings_of(first.resolution->profile, folders, choice.preferences);
    return source;
}

std::vector<fs::path> list_mod_folders(const fs::path& game_folder) {
    const auto mods = entry_named(game_folder, mods_folder_name);
    if (!mods)
        return {};
    return list_mods_in(*mods);
}

std::vector<fs::path> list_mods_in(const fs::path& mods) {
    std::vector<fs::path> folders;
    std::error_code error;
    for (fs::directory_iterator entry{mods, error}, end; !error && entry != end;
         entry.increment(error)) {
        // A folder whose name starts with a dot is hidden, as file managers
        // hide it: the folders a mod's install keeps while it works.
        const std::string name = utf8(entry->path().filename());
        std::error_code status;
        if (!name.starts_with('.') && entry->is_directory(status))
            folders.push_back(entry->path());
    }
    std::sort(folders.begin(), folders.end(), [](const fs::path& left, const fs::path& right) {
        return lowered(utf8(left.filename())) < lowered(utf8(right.filename()));
    });
    return folders;
}

PickedFolderCheck check_picked_mod_folder(
    const fs::path& folder, const fs::path& game_folder, const ModChoice& choice
) {
    PickedFolderCheck check{};
    std::error_code missing;
    if (!fs::is_directory(folder, missing)) {
        check.refusal = "That folder cannot be found.";
        return check;
    }
    std::string error;
    const auto found = find_mod_profile(folder, error);
    if (!error.empty()) {
        check.refusal = "That folder's oamod.yaml cannot be read.";
        check.errors.push_back(error);
        return check;
    }
    const ModChoice played{folder, {}, choice.accept_unimplemented_hacks, choice.preferences};
    auto resolved = resolve_folder_profile({folder, game_folder}, played);
    if (!found) {
        // Played without a profile, by 3.1c's own rules, once the player
        // agrees; a game folder that is a mod's own install carries none.
        if (!resolved.errors.empty()) {
            check.refusal = "That folder cannot be played; the log says why.";
            check.errors = std::move(resolved.errors);
            return check;
        }
        check.without_profile = true;
        check.profile_id = folder_overrides_id(folder);
        return check;
    }
    if (!resolved.errors.empty() || !resolved.profile) {
        check.refusal = "Its oamod.yaml cannot be played; the log says why.";
        check.errors = std::move(resolved.errors);
        return check;
    }
    check.profile_id = resolved.profile->id;
    return check;
}

std::string folder_overrides_id(const fs::path& folder) {
    std::error_code error;
    fs::path whole = fs::absolute(folder, error);
    if (error)
        whole = folder;
    std::string id{folder_overrides_prefix};
    for (const char character : utf8(whole.lexically_normal())) {
        if (character == '%')
            id += "%25";
        else if (character == '|')
            id += "%7C";
        else
            id += character;
    }
    return id;
}

std::vector<mod_profile::SettingValue> read_ini_settings(std::string_view text) {
    std::vector<mod_profile::SettingValue> values;
    std::string section;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';')
            continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close != std::string_view::npos)
                section = std::string(trimmed(line.substr(1, close - 1)));
            continue;
        }
        const auto equals = line.find('=');
        if (section.empty() || equals == std::string_view::npos)
            continue;
        const auto key = trimmed(line.substr(0, equals));
        auto value = line.substr(equals + 1);
        if (const auto comment = value.find(';'); comment != std::string_view::npos)
            value = value.substr(0, comment);
        if (key.empty())
            continue;
        values.push_back({section + '/' + std::string(key), std::string(trimmed(value))});
    }
    return values;
}

std::string registry_key_prefix(const mod_profile::ModProfile& profile) {
    if (profile.identity.registry_root == mod_profile::Identity{}.registry_root)
        return {};
    return "registry:" + profile.identity.registry_root + '\\';
}

bool seed_registry(const mod_profile::ModProfile& profile, platform::preferences::Values& values) {
    const auto prefix = registry_key_prefix(profile) + std::string(registry_game_section) + '|';
    // Registry value names are matched without case.
    std::vector<std::string> present;
    for (const auto& [key, value] : values)
        present.push_back(lowered(key));
    bool written = false;
    for (const auto& seed : profile.registry_seeds) {
        const auto key = prefix + seed.name;
        if (std::find(present.begin(), present.end(), lowered(key)) != present.end())
            continue;
        values.emplace(key, seed.value);
        present.push_back(lowered(key));
        written = true;
    }
    return written;
}

data::defs::DataLayout data_layout_of(const mod_profile::ModProfile* profile) {
    data::defs::DataLayout layout{};
    if (profile == nullptr)
        return layout;
    const auto& directories = profile->layout.directories;
    using data::defs::DataDirectory;
    const auto set = [&](DataDirectory directory, const std::string& name) {
        layout.directories[static_cast<std::size_t>(directory)] = name;
    };
    set(DataDirectory::units, directories.units);
    set(DataDirectory::weapons, directories.weapons);
    set(DataDirectory::gamedata, directories.gamedata);
    set(DataDirectory::ai, directories.ai);
    set(DataDirectory::guis, directories.guis);
    set(DataDirectory::unitpics, directories.unitpics);
    set(DataDirectory::download, directories.download);
    layout.unit_extension = profile->layout.file_extensions.unit_definition;
    layout.map_units_section = profile->layout.map_units_section;
    // The unit files' build version is the game's network version, held to
    // the largest version a unit file's gate can compare.
    const auto version_byte = [](int32_t value) {
        return static_cast<int8_t>(std::clamp(value, 0, int32_t{INT8_MAX}));
    };
    layout.build_version = {
        version_byte(profile->identity.network_version[0]),
        version_byte(profile->identity.network_version[1])
    };
    layout.side_names = profile->identity.side_names;
    layout.watcher_side_frame =
        static_cast<uint8_t>(std::clamp(profile->identity.watcher_side_frame, 0, 63));
    const auto side_bits = [](const auto& sides) {
        uint32_t bits = 0;
        for (uint16_t at = 0; at < sides.count; ++at)
            if (sides.items[at] >= 0 && sides.items[at] < 32)
                bits |= 1U << static_cast<uint32_t>(sides.items[at]);
        return bits;
    };
    layout.skirmish_hidden_sides = side_bits(profile->identity.skirmish_hidden_sides);
    layout.battle_room_hidden_sides = side_bits(profile->identity.battle_room_hidden_sides);
    return layout;
}

DiscoveryPlan discovery_plan_of(const mod_profile::ModProfile* profile) {
    DiscoveryPlan plan{};
    // An install without discs keeps the disc archives in its folders.
    plan.folders_as_disc = true;
    if (profile == nullptr)
        return plan;
    plan.revision_archive = profile->layout.revision_archive;
    plan.installation_archives = profile->layout.installation_archives;
    plan.ccx_pattern = profile->layout.archive_patterns.ccx;
    plan.ufo_pattern = profile->layout.archive_patterns.ufo;
    plan.hpi_pattern = profile->layout.archive_patterns.hpi;
    return plan;
}

std::string_view movie_file_of(const mod_profile::ModProfile* profile, std::string_view movie) {
    if (profile == nullptr)
        return movie;
    // Each movie is known by 3.1c's file for it, its entry's baseline.
    const mod_profile::MediaMovies base{};
    const auto& movies = profile->media.movies;
    if (movie == base.logo)
        return movies.logo;
    if (movie == base.intro)
        return movies.intro;
    if (movie == base.ending_a)
        return movies.ending_a;
    if (movie == base.ending_b)
        return movies.ending_b;
    if (movie == base.credits)
        return movies.credits;
    return movie;
}

} // namespace oa::app
