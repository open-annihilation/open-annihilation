// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The import's plan (game_files_import.hpp): which files are copied and which
// are left out, the name check of a chosen folder, the parts Ready to copy
// shows, the space a copy needs, what is installed, and the files of a part.
//
// The whole tree is copied minus a short list of files no game data uses,
// because the engine reads loose files from anywhere in the game folder. The
// device's volume may tell capital letters apart while the engine matches
// names without case, so the plan folds every folder onto one spelling and
// copies only the first of two files that differ in case.
#include "oa/app/game_files_import.hpp"

#include "oa/app/mod_profile_loader.hpp"
#include "oa/formats/zip.hpp"
#include "oa/platform/file_time.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oa::app::game_files {
namespace {

/// The extensions of Windows programs, libraries and shortcuts, which no game data uses.
constexpr std::array<std::string_view, 6> windows_program_extensions{
    ".exe", ".dll", ".msi", ".cab", ".lnk", ".scr"
};
/// The extensions of icons, manuals and help files (.txt is kept: a mod may ship some).
constexpr std::array<std::string_view, 7> help_file_extensions{
    ".ico", ".pdf", ".doc", ".rtf", ".hlp", ".chm", ".cnt"
};
/// The extensions of an uninstaller's files, after "unins" and digits.
constexpr std::array<std::string_view, 4> uninstaller_extensions{".exe", ".dat", ".ini", ".msg"};
/// The start of an uninstaller's file names.
constexpr std::string_view uninstaller_prefix = "unins";
/// Files the system writes beside the player's own, matched without case.
constexpr std::array<std::string_view, 2> clutter_names{"thumbs.db", "desktop.ini"};
/// The folder a zip made on one system carries its resource forks in.
constexpr std::string_view resource_fork_folder = "__macosx";

/// The game's own archives.
constexpr std::array<std::string_view, 5> game_archive_names{
    "totala1.hpi", "totala2.hpi", "totala3.hpi", "totala4.hpi", "worlds.hpi"
};
/// The 3.1c update's archive.
constexpr std::string_view update_archive_name = "rev31.gp3";
/// The Core Contingency expansion's archives.
constexpr std::array<std::string_view, 3> core_contingency_names{
    "ccdata.ccx", "ccmaps.ccx", "ccmiss.ccx"
};
/// The Battle Tactics expansion's .ccx archives; its tactics1-8.hpi are matched apart.
constexpr std::array<std::string_view, 2> battle_tactics_names{"btdata.ccx", "btmaps.ccx"};
/// The start of the Battle Tactics mission archives, tactics1.hpi to tactics8.hpi.
constexpr std::string_view tactics_prefix = "tactics";
/// The last Battle Tactics mission archive's number.
constexpr char last_tactics_digit = '8';
/// The archive extensions an extra unit or map pack comes in.
constexpr std::array<std::string_view, 4> extra_extensions{".ufo", ".ccx", ".hpi", ".gp3"};
/// The archive extensions the name check takes at a folder's top, besides rev31.gp3.
constexpr std::array<std::string_view, 3> archive_extensions{".hpi", ".ufo", ".ccx"};
/// The folder the game's music lies in.
constexpr std::string_view music_folder_name = "music";
/// The audio files a music track is read from.
constexpr std::array<std::string_view, 4> music_extensions{".mp3", ".ogg", ".wav", ".flac"};
/// The folder the movies lie in.
constexpr std::string_view movies_folder_name = "data";
/// The movies' extension: 1.zrb the logo, 2.zrb the intro, 3.zrb and 4.zrb the campaign
/// endings, 5.zrb the credits.
constexpr std::string_view movie_extension = ".zrb";
/// A mod folder's name when its profile names no usable id.
constexpr std::string_view unnamed_mod_folder = "mod";

/// Lower-cases one ASCII letter, as the engine matches names without case.
///
/// @param c a byte of UTF-8 text
/// @return the byte, with A-Z made a-z
[[nodiscard]] constexpr char lower_ascii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Lower-cases the ASCII letters of a name.
///
/// @param text UTF-8 text
/// @return the text with A-Z made a-z
[[nodiscard]] std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        c = lower_ascii(c);
    return out;
}

/// Tells whether two names are the same without case.
///
/// @param left a name
/// @param right another name
/// @return true when they differ at most in ASCII capital letters
[[nodiscard]] bool same_folded(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (lower_ascii(left[index]) != lower_ascii(right[index]))
            return false;
    return true;
}

/// Tells whether a name starts with a lower-case prefix, without case.
///
/// @param text the name
/// @param prefix the prefix, lower case
/// @return true when it does
[[nodiscard]] bool starts_folded(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && same_folded(text.substr(0, prefix.size()), prefix);
}

/// Returns a name's extension, from its last dot.
///
/// @param name a file name
/// @return the extension with its dot; empty when there is none
[[nodiscard]] std::string_view extension_of(std::string_view name) noexcept {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view{} : name.substr(dot);
}

/// Tells whether a name's extension is one of a list, without case.
///
/// @param name a file name
/// @param extensions lower-case extensions with their dots
/// @return true when it is
template <std::size_t count>
[[nodiscard]] bool has_extension(
    std::string_view name, const std::array<std::string_view, count>& extensions
) noexcept {
    const auto extension = extension_of(name);
    return std::any_of(extensions.begin(), extensions.end(), [&](std::string_view candidate) {
        return same_folded(extension, candidate);
    });
}

/// Tells whether a name is one of a list, without case.
///
/// @param name a file name
/// @param names lower-case names
/// @return true when it is
template <std::size_t count>
[[nodiscard]] bool
is_one_of(std::string_view name, const std::array<std::string_view, count>& names) noexcept {
    return std::any_of(names.begin(), names.end(), [&](std::string_view candidate) {
        return same_folded(name, candidate);
    });
}

/// Returns the last component of a '/'-separated path.
///
/// @param path the path
/// @return its last component
[[nodiscard]] std::string_view name_of(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Splits a '/'-separated path into its components.
///
/// @param path the path
/// @return its components, empty ones left out
[[nodiscard]] std::vector<std::string_view> components_of(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (!part.empty())
            parts.push_back(part);
        if (slash == std::string_view::npos)
            break;
        path.remove_prefix(slash + 1);
    }
    return parts;
}

/// Tells whether a path component is the system's clutter: a name starting with '.', or the
/// resource fork folder.
///
/// @param part one component of a path
/// @return true when it is
[[nodiscard]] bool is_clutter_component(std::string_view part) noexcept {
    return part.empty() || part.front() == '.' || same_folded(part, resource_fork_folder);
}

/// Tells whether a file name is an uninstaller's: "unins", digits, then .exe, .dat, .ini or
/// .msg.
///
/// @param name the file name
/// @return true when it is
[[nodiscard]] bool is_uninstaller(std::string_view name) noexcept {
    if (!starts_folded(name, uninstaller_prefix))
        return false;
    std::size_t at = uninstaller_prefix.size();
    const std::size_t digits = at;
    while (at < name.size() && name[at] >= '0' && name[at] <= '9')
        ++at;
    if (at == digits)
        return false;
    return is_one_of(name.substr(at), uninstaller_extensions);
}

/// Tells whether a file name is one of Battle Tactics' mission archives, tactics1.hpi to
/// tactics8.hpi.
///
/// @param name the file name
/// @return true when it is
[[nodiscard]] bool is_tactics_archive(std::string_view name) noexcept {
    constexpr std::string_view extension = ".hpi";
    if (name.size() != tactics_prefix.size() + 1 + extension.size() ||
        !starts_folded(name, tactics_prefix))
        return false;
    const char digit = name[tactics_prefix.size()];
    return digit >= '1' && digit <= last_tactics_digit &&
           same_folded(name.substr(tactics_prefix.size() + 1), extension);
}

/// Tells whether a listed entry at the top of a folder says the folder is a game folder: an
/// archive name, oamod.yaml or a file of the demo installer's size.
///
/// @param entry the entry
/// @return true when it does
[[nodiscard]] bool entry_marks_game(const SourceEntry& entry) noexcept {
    if (entry.folder || entry.link)
        return false;
    const auto name = name_of(entry.path);
    return is_archive_name(name) || same_folded(name, mod_profile_name) ||
           (entry.size_known && entry.size == demo_1997.installer_size);
}

/// Tells whether a listing holds a game folder's marks among its entries directly below
/// `prefix`.
///
/// @param entries the listing
/// @param prefix "" for the listed folder's own entries, else a folder followed by '/'
/// @return true when they do
[[nodiscard]] bool
marks_game_below(const std::vector<SourceEntry>& entries, std::string_view prefix) {
    return std::any_of(entries.begin(), entries.end(), [&](const SourceEntry& entry) {
        std::string_view path = entry.path;
        if (path.size() <= prefix.size() || path.substr(0, prefix.size()) != prefix)
            return false;
        path.remove_prefix(prefix.size());
        return path.find('/') == std::string_view::npos && entry_marks_game(entry);
    });
}

/// Tells whether a folder's top holds archives.
///
/// @param entries the folder's listing
/// @return true when a file at its top has an archive name
[[nodiscard]] bool top_holds_archives(const std::vector<SourceEntry>& entries) {
    return std::any_of(entries.begin(), entries.end(), [](const SourceEntry& entry) {
        return !entry.folder && !entry.link && entry.path.find('/') == std::string::npos &&
               is_archive_name(entry.path);
    });
}

/// Drops a trailing separator from a folder's path, so that its name is its last component.
///
/// @param folder the folder, as a picker may give it
/// @return the same folder without a trailing separator
[[nodiscard]] fs::path without_trailing_separator(fs::path folder) {
    while (!folder.has_filename() && folder.has_relative_path())
        folder = folder.parent_path();
    return folder;
}

/// Tells whether two paths name the same existing file or folder.
///
/// @param left a path
/// @param right another path
/// @return true when both exist and are one
[[nodiscard]] bool same_place(const fs::path& left, const fs::path& right) noexcept {
    std::error_code error;
    return !left.empty() && !right.empty() && fs::equivalent(left, right, error) && !error;
}

/// Makes a folder name of a mod's id, or of its folder's name when its profile gave none.
///
/// @param text the id or the folder's name
/// @return one safe path component
[[nodiscard]] std::string mod_folder_component(std::string_view text) {
    std::string out;
    for (const char c : text)
        out += c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20 ? '-' : c;
    if (out.empty() || out == "." || out == ".." || out.front() == '.')
        out = std::string(unnamed_mod_folder);
    return out;
}

/// Reads a mod's profile for its id, its name and the reasons it cannot be used.
///
/// @param folder the mod's folder, absolute
/// @param relative its folder in the source, '/'-separated; empty for the source itself
/// @param display the folder's own name, for a profile that names no id
/// @param remote the profile is held in the cloud: it is not read before the copy
/// @return the mod as found
[[nodiscard]] ModFound
read_mod(const fs::path& folder, std::string relative, std::string_view display, bool remote) {
    ModFound mod;
    mod.folder = std::move(relative);
    if (!remote) {
        const auto profile = resolve_folder_profile({folder}, ModChoice{});
        if (profile.profile) {
            mod.id = mod_folder_component(profile.profile->id);
            mod.name = profile.profile->name;
        }
        mod.errors = profile.errors;
    }
    if (mod.id.empty())
        mod.id = mod_folder_component(display);
    if (mod.name.empty())
        mod.name = std::string(display);
    return mod;
}

/// Finds the mod folders of a listing: mods/<folder>/oamod.yaml, in listing order, the first
/// spelling of each.
///
/// @param entries the listing
/// @return each mod folder's path as listed, and whether its profile is held in the cloud
[[nodiscard]] std::vector<std::pair<std::string, bool>>
mod_folders_of(const std::vector<SourceEntry>& entries) {
    std::vector<std::pair<std::string, bool>> found;
    std::unordered_set<std::string> seen;
    for (const auto& entry : entries) {
        if (entry.folder || entry.link)
            continue;
        const auto parts = components_of(entry.path);
        if (parts.size() != 3 || !same_folded(parts[0], mods_folder_name) ||
            !same_folded(parts[2], mod_profile_name))
            continue;
        if (left_out(entry.path, entry.size, true))
            continue;
        std::string folder = std::string(parts[0]) + "/" + std::string(parts[1]);
        if (seen.insert(folded(folder)).second)
            found.emplace_back(std::move(folder), entry.remote);
    }
    return found;
}

/// The spellings a plan folds names onto: the first seen of each folder, or the installed
/// folder's for an addition.
struct Folding {
    std::unordered_map<std::string, std::string> folders{}; ///< lower-case path to its spelling
    std::unordered_map<std::string, std::string> files{};   ///< lower-case path to its spelling
};

/// Folds a path's folders onto the spellings seen before, remembering the new ones.
///
/// @param[in,out] folding the spellings seen so far
/// @param path the path, '/'-separated
/// @param files the files a folder must not take the name of, lower case
/// @return the folded path; nothing when one of its folders has the name of a file
[[nodiscard]] std::optional<std::string> fold_folders(
    Folding& folding,
    std::string_view path,
    const std::unordered_map<std::string, std::string>& files
) {
    const auto parts = components_of(path);
    if (parts.empty())
        return std::nullopt;
    std::string spelled;
    std::string lower;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        std::string next =
            lower.empty() ? folded(parts[index]) : lower + "/" + folded(parts[index]);
        if (files.contains(next) || folding.files.contains(next))
            return std::nullopt;
        const auto known = folding.folders.find(next);
        if (known != folding.folders.end()) {
            spelled = known->second;
        } else {
            spelled = spelled.empty() ? std::string(parts[index])
                                      : spelled + "/" + std::string(parts[index]);
            folding.folders.emplace(next, spelled);
        }
        lower = std::move(next);
    }
    return spelled.empty() ? std::string(parts.back()) : spelled + "/" + std::string(parts.back());
}

/// Tells whether a planned file is copied under the switches.
///
/// @param file the planned file
/// @param switches the player's switches
/// @return true when its part, and its mod, are on (parts without a switch always are)
[[nodiscard]] bool switched_on(const PlannedFile& file, const Switches& switches) noexcept {
    if (!part_switchable(file.part))
        return true;
    if (!switches.parts[static_cast<std::size_t>(file.part)])
        return false;
    if (file.part == Part::mods && file.mod != PlannedFile::no_mod &&
        file.mod < switches.mods.size())
        return switches.mods[file.mod] != 0;
    return true;
}

/// Adds one file to a part's summary.
///
/// @param[in,out] summary the part's summary
/// @param name the file's display name
/// @param size its size in bytes, when size_known
/// @param size_known false when its size was not reported
void add_to_part(PartSummary& summary, std::string_view name, uint64_t size, bool size_known) {
    summary.found = true;
    ++summary.files;
    if (size_known)
        summary.bytes += size;
    else
        summary.bytes_known = false;
    if (summary.names.size() < most_listed_names)
        summary.names.emplace_back(name);
    if (summary.part == Part::music && has_extension(name, music_extensions))
        ++summary.music_tracks;
}

/// Makes the empty summary of every part.
///
/// @return one summary per part, each naming its part
[[nodiscard]] std::array<PartSummary, part_count> empty_parts() {
    std::array<PartSummary, part_count> parts{};
    for (std::size_t index = 0; index < part_count; ++index)
        parts[index].part = static_cast<Part>(index);
    return parts;
}

/// Sums the sizes of the regular files below a folder, symbolic links not followed.
///
/// @param folder the folder
/// @return their bytes; 0 when there is none
[[nodiscard]] uint64_t folder_bytes(const fs::path& folder) noexcept {
    uint64_t bytes = 0;
    std::error_code error;
    if (!fs::is_directory(folder, error))
        return 0;
    for (fs::recursive_directory_iterator entry{folder, error}, end; !error && entry != end;
         entry.increment(error)) {
        std::error_code status;
        if (entry->is_regular_file(status) && !entry->is_symlink(status)) {
            const auto size = entry->file_size(status);
            if (!status)
                bytes += size;
        }
    }
    return bytes;
}

/// Returns the nearest existing folder of a path, itself or an ancestor.
///
/// @param path the path
/// @return the folder; empty when none exists
[[nodiscard]] fs::path existing_folder(const fs::path& path) {
    std::error_code error;
    for (fs::path at = path; !at.empty(); at = at.parent_path()) {
        if (fs::is_directory(at, error))
            return at;
        if (at == at.parent_path())
            break;
    }
    return {};
}

/// Tells whether the demo's archive is already unpacked in the data folder.
///
/// @param paths the import's folders
/// @return true when the archive is there with the release's size
[[nodiscard]] bool demo_archive_present(const ImportPaths& paths) noexcept {
    if (paths.data_folder.empty())
        return false;
    std::error_code error;
    const auto archive =
        demo_bundle_directory(paths.data_folder) / fs::path(demo_1997.archive_name);
    const auto size = fs::file_size(archive, error);
    return !error && size == demo_1997.archive_size;
}

} // namespace

std::optional<LeftOutReason>
left_out(std::string_view relative_path, uint64_t size, bool folder_has_archives) noexcept {
    if (relative_path.empty() || relative_path.back() == '/' ||
        !oa::formats::zip::name_is_safe(relative_path))
        return LeftOutReason::unsafe_name;
    std::string_view rest = relative_path;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto part = rest.substr(0, slash);
        if (is_clutter_component(part))
            return LeftOutReason::system_clutter;
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    const auto name = name_of(relative_path);
    if (is_one_of(name, clutter_names))
        return LeftOutReason::system_clutter;
    // The demo's installer is recognised by its size, whatever its name.
    if (!folder_has_archives && size == demo_1997.installer_size)
        return std::nullopt;
    if (is_uninstaller(name))
        return LeftOutReason::uninstaller;
    if (has_extension(name, windows_program_extensions))
        return LeftOutReason::windows_program;
    if (has_extension(name, help_file_extensions))
        return LeftOutReason::help_file;
    return std::nullopt;
}

bool is_archive_name(std::string_view name) noexcept {
    return has_extension(name, archive_extensions) || same_folded(name, update_archive_name);
}

Part part_of(
    std::string_view relative_path,
    uint64_t size,
    bool folder_has_archives,
    std::span<const std::string> mod_folders
) {
    const auto parts = components_of(relative_path);
    if (parts.empty())
        return Part::other;
    if (parts.size() == 1) {
        const auto name = parts.front();
        if (is_one_of(name, game_archive_names))
            return Part::game_archives;
        if (same_folded(name, update_archive_name))
            return Part::update_31c;
        if (is_one_of(name, core_contingency_names))
            return Part::core_contingency;
        if (is_one_of(name, battle_tactics_names) || is_tactics_archive(name))
            return Part::battle_tactics;
        if (has_extension(name, extra_extensions))
            return Part::extra;
        if (!folder_has_archives && size == demo_1997.installer_size)
            return Part::demo;
        return Part::other;
    }
    if (same_folded(parts[0], music_folder_name))
        return Part::music;
    if (parts.size() == 2 && same_folded(parts[0], movies_folder_name) &&
        same_folded(extension_of(parts[1]), movie_extension))
        return Part::movies;
    if (parts.size() >= 3 && same_folded(parts[0], mods_folder_name)) {
        const auto key = folded(std::string(parts[0]) + "/" + std::string(parts[1]));
        if (std::find(mod_folders.begin(), mod_folders.end(), key) != mod_folders.end())
            return Part::mods;
    }
    return Part::other;
}

ImportMode import_mode_of(const ImportPlan& plan) noexcept {
    switch (plan.kind) {
    case SourceKind::game_folder:
    case SourceKind::demo_installer:
        return ImportMode::replace;
    case SourceKind::archives:
        return ImportMode::add;
    case SourceKind::additions_folder:
        break;
    }
    return plan.mods.size() == 1 && plan.mods.front().folder.empty() ? ImportMode::mod
                                                                     : ImportMode::add;
}

std::string old_folder_name(uint32_t number) {
    std::string name = std::string(game_folder_name) + std::string(old_folder_suffix);
    if (number > 1)
        name += " " + std::to_string(number);
    return name;
}

int64_t seconds_since_1970(fs::file_time_type time) noexcept {
    return oa::platform::file_seconds_since_1970(time);
}

fs::file_time_type file_time_from_seconds(int64_t seconds) noexcept {
    return oa::platform::file_time_from_seconds(seconds);
}

ImportPaths import_paths(const fs::path& game_folder, const fs::path& data_folder) {
    ImportPaths paths;
    paths.game_folder = game_folder;
    paths.documents = game_folder.parent_path();
    paths.data_folder = data_folder;
    paths.import_root = data_folder / fs::path(import_folder_name);
    paths.staging = paths.import_root / fs::path(game_folder_name);
    paths.state_file = paths.import_root / fs::path(state_file_name);
    return paths;
}

NameCheck
check_names(const GameFilesHooks& hooks, const ImportPaths& paths, const fs::path& chosen) {
    NameCheck check;
    const auto folder = without_trailing_separator(chosen);
    if (same_place(folder, paths.game_folder)) {
        check.look = TopLook::already_there;
        return check;
    }
    std::vector<SourceEntry> top;
    if (!list_source(hooks, folder, 0, &top, &check.error)) {
        if (check.error.empty())
            check.error = "the folder could not be listed";
        check.look = TopLook::nothing;
        return check;
    }
    const TopLook top_game =
        same_place(folder.parent_path(), paths.documents) ? TopLook::in_documents : TopLook::game;
    // A top whose only mark is a file of the demo's installer size (an installer kept beside
    // a game folder) is looked below first: a game folder there is offered instead.
    const bool installer_only = marks_game_below(top, {}) && !top_holds_archives(top) &&
                                std::none_of(top.begin(), top.end(), [](const SourceEntry& e) {
                                    return !e.folder && e.path.find('/') == std::string::npos &&
                                           same_folded(name_of(e.path), mod_profile_name);
                                });
    if (marks_game_below(top, {}) && !installer_only) {
        check.look = top_game;
        return check;
    }
    // Two levels down, stopping at the first level that has a game folder.
    std::vector<std::string> folders;
    for (const auto& entry : top)
        if (entry.folder && !entry.link && !is_clutter_component(entry.path))
            folders.push_back(entry.path);
    std::sort(folders.begin(), folders.end());
    std::vector<std::string> first_level;
    std::vector<std::string> second_level;
    for (const auto& name : folders) {
        std::vector<SourceEntry> below;
        std::string ignored;
        if (!list_source(hooks, folder / path_from_utf8(name), 1, &below, &ignored))
            continue;
        if (marks_game_below(below, {})) {
            first_level.push_back(name);
            continue;
        }
        if (!first_level.empty())
            continue;
        std::vector<std::string> inner;
        for (const auto& entry : below)
            if (entry.folder && !entry.link && entry.path.find('/') == std::string::npos &&
                !is_clutter_component(entry.path))
                inner.push_back(entry.path);
        std::sort(inner.begin(), inner.end());
        for (const auto& sub : inner)
            if (marks_game_below(below, sub + "/"))
                second_level.push_back(name + "/" + sub);
    }
    check.nested = first_level.empty() ? std::move(second_level) : std::move(first_level);
    if (installer_only && check.nested.empty())
        check.look = top_game;
    else
        check.look = check.nested.empty() ? TopLook::nothing : TopLook::nested;
    return check;
}

InstalledSummary summarize_installed(const GameFilesHooks& hooks, const ImportPaths& paths) {
    InstalledSummary summary;
    summary.parts = empty_parts();
    std::error_code error;
    summary.present = fs::is_directory(paths.game_folder, error);
    bool has_archives = false;
    if (summary.present) {
        std::string ignored;
        static_cast<void>(list_source(
            hooks,
            paths.game_folder,
            std::numeric_limits<uint32_t>::max(),
            &summary.listing,
            &ignored
        ));
        has_archives = top_holds_archives(summary.listing);
        std::vector<std::string> mod_keys;
        for (const auto& [folder, remote] : mod_folders_of(summary.listing)) {
            mod_keys.push_back(folded(folder));
            summary.mods.push_back(read_mod(
                paths.game_folder / path_from_utf8(folder), folder, name_of(folder), remote
            ));
        }
        for (const auto& entry : summary.listing) {
            if (entry.folder || entry.link)
                continue;
            if (entry.size_known)
                summary.bytes += entry.size;
            const auto part = part_of(entry.path, entry.size, has_archives, mod_keys);
            add_to_part(
                summary.parts[static_cast<std::size_t>(part)],
                name_of(entry.path),
                entry.size,
                entry.size_known
            );
        }
        summary.demo = !has_archives && summary.parts[static_cast<std::size_t>(Part::demo)].found;
    }
    if (!paths.data_folder.empty()) {
        // The archive in Games/totala-demo. The record beside it is not part
        // of the archive's size.
        const fs::path demo_archive = demo_bundle_directory(paths.data_folder) /
                                      fs::path(std::string(demo_1997.archive_name));
        error.clear();
        if (fs::is_regular_file(demo_archive, error) && !error) {
            const auto bytes = fs::file_size(demo_archive, error);
            if (!error)
                summary.demo_data_bytes = static_cast<uint64_t>(bytes);
        }
        error.clear();
        summary.demo_data_unused = summary.demo_data_bytes > 0 && has_archives;
    }
    if (!paths.documents.empty()) {
        const std::string first = old_folder_name(1);
        for (fs::directory_iterator entry{paths.documents, error}, end; !error && entry != end;
             entry.increment(error)) {
            std::error_code status;
            if (entry->is_symlink(status) || !entry->is_directory(status))
                continue;
            const auto name = path_to_utf8(entry->path().filename());
            bool old = name == first;
            if (!old && name.size() > first.size() + 1 &&
                name.compare(0, first.size(), first) == 0 && name[first.size()] == ' ') {
                const auto number = std::string_view(name).substr(first.size() + 1);
                old =
                    number.front() != '0' && std::all_of(number.begin(), number.end(), [](char c) {
                        return c >= '0' && c <= '9';
                    });
            }
            if (old)
                summary.old_folders.push_back(entry->path());
        }
        std::sort(summary.old_folders.begin(), summary.old_folders.end());
        for (const auto& folder : summary.old_folders)
            summary.old_folder_bytes += folder_bytes(folder);
    }
    return summary;
}

ImportPlan plan_import(
    SourceKind kind,
    const fs::path& source,
    std::string location,
    std::span<const SourceEntry> entries,
    const InstalledSummary* installed
) {
    ImportPlan plan;
    plan.kind = kind;
    plan.source = source;
    plan.location = location.empty() ? path_to_utf8(source.filename()) : std::move(location);
    plan.parts = empty_parts();

    std::vector<SourceEntry> sorted(entries.begin(), entries.end());
    std::sort(sorted.begin(), sorted.end(), [](const SourceEntry& left, const SourceEntry& right) {
        return left.path < right.path;
    });

    plan.has_archives = kind == SourceKind::archives
                            ? std::any_of(
                                  sorted.begin(),
                                  sorted.end(),
                                  [](const SourceEntry& entry) {
                                      return !entry.folder && is_archive_name(name_of(entry.path));
                                  }
                              )
                            : top_holds_archives(sorted);

    // An additions folder whose top holds a mod profile is one mod, copied into mods/<id>/.
    bool one_mod = false;
    if (kind == SourceKind::additions_folder) {
        for (const auto& entry : sorted)
            if (!entry.folder && !entry.link && entry.path.find('/') == std::string::npos &&
                same_folded(entry.path, mod_profile_name)) {
                plan.mods.push_back(
                    read_mod(source, std::string{}, path_to_utf8(source.filename()), entry.remote)
                );
                one_mod = true;
                break;
            }
    }
    std::vector<std::string> mod_keys;
    if (!one_mod && (kind == SourceKind::game_folder || kind == SourceKind::additions_folder)) {
        for (const auto& [folder, remote] : mod_folders_of(sorted)) {
            mod_keys.push_back(folded(folder));
            plan.mods.push_back(
                read_mod(source / path_from_utf8(folder), folder, name_of(folder), remote)
            );
        }
    }

    // The spellings to fold onto, and the names an addition keeps.
    Folding folding;
    std::unordered_map<std::string, std::string> installed_files;
    if (installed != nullptr) {
        for (const auto& entry : installed->listing) {
            const auto parts = components_of(entry.path);
            std::string spelled;
            for (std::size_t index = 0; index < parts.size(); ++index) {
                spelled = spelled.empty() ? std::string(parts[index])
                                          : spelled + "/" + std::string(parts[index]);
                if (index + 1 < parts.size() || entry.folder)
                    folding.folders.emplace(folded(spelled), spelled);
                else
                    installed_files.emplace(folded(spelled), spelled);
            }
        }
    }
    const std::string mod_root =
        one_mod ? std::string(mods_folder_name) + "/" + plan.mods.front().id + "/" : std::string{};

    for (const auto& entry : sorted) {
        if (entry.folder)
            continue;
        if (entry.link) {
            plan.left_out.push_back({entry.path, entry.size, LeftOutReason::link, {}});
            continue;
        }
        if (kind != SourceKind::demo_installer) {
            if (const auto reason = left_out(entry.path, entry.size, plan.has_archives)) {
                plan.left_out.push_back({entry.path, entry.size, *reason, {}});
                continue;
            }
        }
        std::string wanted;
        switch (kind) {
        case SourceKind::game_folder:
        case SourceKind::additions_folder:
            wanted = mod_root + entry.path;
            break;
        case SourceKind::demo_installer:
        case SourceKind::archives:
            wanted = std::string(name_of(entry.path));
            break;
        }
        const auto target = fold_folders(folding, wanted, installed_files);
        const std::string lower = target ? folded(*target) : folded(wanted);
        if (target) {
            const auto there = installed_files.find(lower);
            if (there != installed_files.end()) {
                plan.kept.push_back(there->second);
                continue;
            }
        }
        const auto clash = folding.files.find(lower);
        if (!target || clash != folding.files.end() || folding.folders.contains(lower)) {
            LeftOutFile file{entry.path, entry.size, LeftOutReason::case_clash, {}};
            file.kept_path = clash != folding.files.end() ? clash->second : std::string{};
            if (file.kept_path.empty()) {
                const auto folder = folding.folders.find(lower);
                file.kept_path = folder != folding.folders.end() ? folder->second : wanted;
            }
            plan.warnings.push_back(
                file.kept_path + " and " + entry.path +
                " differ only in capital letters; the game reads one of them. The first is "
                "copied."
            );
            plan.left_out.push_back(std::move(file));
            continue;
        }
        PlannedFile file;
        file.path = entry.path;
        file.target = *target;
        file.size = entry.size;
        file.size_known = entry.size_known;
        file.modified = entry.modified;
        file.remote = entry.remote;
        if (kind == SourceKind::demo_installer) {
            file.part = Part::demo;
        } else if (one_mod) {
            file.part = Part::mods;
            file.mod = 0;
        } else {
            file.part = part_of(*target, entry.size, plan.has_archives, mod_keys);
            if (file.part == Part::mods) {
                const auto parts = components_of(*target);
                const auto key = folded(std::string(parts[0]) + "/" + std::string(parts[1]));
                const auto found = std::find(mod_keys.begin(), mod_keys.end(), key);
                file.mod = static_cast<uint32_t>(found - mod_keys.begin());
            }
        }
        folding.files.emplace(lower, entry.path);
        add_to_part(
            plan.parts[static_cast<std::size_t>(file.part)],
            name_of(file.target),
            file.size,
            file.size_known
        );
        if (file.size_known)
            plan.total_bytes += file.size;
        else
            plan.sizes_unknown = true;
        if (file.remote && file.size_known)
            plan.remote_bytes += file.size;
        plan.files.push_back(std::move(file));
    }
    plan.demo = kind == SourceKind::demo_installer ||
                plan.parts[static_cast<std::size_t>(Part::demo)].found;
    return plan;
}

SpaceNeed space_need(
    const GameFilesHooks& hooks,
    const ImportPaths& paths,
    const ImportPlan& plan,
    const Switches& switches
) {
    SpaceNeed need;
    std::array<uint64_t, part_count> part_bytes{};
    if (!plan.move_in_place) {
        for (const auto& file : plan.files) {
            if (!switched_on(file, switches) || plan.movable || !file.size_known)
                continue;
            if (staged_matches(paths.staging / path_from_utf8(file.target), file))
                continue;
            need.copy_bytes += file.size;
            part_bytes[static_cast<std::size_t>(file.part)] += file.size;
        }
    }
    const uint64_t unpack = plan.demo && !demo_archive_present(paths) ? demo_1997.archive_size : 0;
    need.need_bytes = need.copy_bytes + unpack + space_margin_bytes;
    const auto folder = existing_folder(paths.game_folder);
    if (!folder.empty()) {
        uint64_t bytes = 0;
        if (hooks.free_space != nullptr) {
            need.free_known = hooks.free_space(hooks.context, path_to_utf8(folder).c_str(), &bytes);
        } else {
            std::error_code error;
            const auto space = fs::space(folder, error);
            need.free_known = !error;
            bytes = error ? 0 : space.available;
        }
        need.free_bytes = need.free_known ? bytes : 0;
    }
    need.fits = !need.free_known || need.need_bytes <= need.free_bytes;
    if (!need.fits) {
        for (std::size_t index = 0; index < part_count; ++index) {
            const auto part = static_cast<Part>(index);
            if (!part_switchable(part) || part_bytes[index] == 0)
                continue;
            if (need.need_bytes - part_bytes[index] <= need.free_bytes)
                need.fitting_off.push_back(part);
        }
        std::stable_sort(
            need.fitting_off.begin(), need.fitting_off.end(), [&](Part left, Part right) {
                return part_bytes[static_cast<std::size_t>(left)] >
                       part_bytes[static_cast<std::size_t>(right)];
            }
        );
    }
    return need;
}

bool passes_through_link(const fs::path& root, const fs::path& path) {
    const fs::path relative = path.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..")
        return true;
    fs::path at = root;
    for (const auto& part : relative) {
        if (part == ".")
            continue;
        at /= part;
        std::error_code error;
        const auto status = fs::symlink_status(at, error);
        if (!fs::exists(status))
            return false;
        // A link, or a junction or other entry no folder or file is.
        if (!fs::is_directory(status) && !fs::is_regular_file(status))
            return true;
    }
    return false;
}

bool staged_matches(const fs::path& staged, const PlannedFile& file) noexcept {
    if (!file.size_known)
        return false;
    std::error_code error;
    const auto status = fs::symlink_status(staged, error);
    if (error || !fs::is_regular_file(status))
        return false;
    const auto size = fs::file_size(staged, error);
    if (error || size != file.size)
        return false;
    const auto time = fs::last_write_time(staged, error);
    return !error && seconds_since_1970(time) == file.modified;
}

std::vector<std::string> part_files(
    const GameFilesHooks& hooks, const ImportPaths& paths, Part part, std::string_view mod_id
) {
    std::vector<std::string> files;
    std::vector<SourceEntry> entries;
    std::string ignored;
    std::error_code error;
    if (!fs::is_directory(paths.game_folder, error))
        return files;
    static_cast<void>(list_source(
        hooks, paths.game_folder, std::numeric_limits<uint32_t>::max(), &entries, &ignored
    ));
    std::sort(
        entries.begin(), entries.end(), [](const SourceEntry& left, const SourceEntry& right) {
            return left.path < right.path;
        }
    );
    const bool has_archives = top_holds_archives(entries);
    std::vector<std::string> mod_keys;
    std::string wanted_mod;
    for (const auto& [folder, remote] : mod_folders_of(entries)) {
        mod_keys.push_back(folded(folder));
        if (part == Part::mods && !mod_id.empty() && wanted_mod.empty()) {
            const auto mod = read_mod(
                paths.game_folder / path_from_utf8(folder), folder, name_of(folder), remote
            );
            if (mod.id == mod_id || name_of(folder) == mod_id)
                wanted_mod = folded(folder) + "/";
        }
    }
    if (part == Part::mods && !mod_id.empty() && wanted_mod.empty())
        return files;
    for (const auto& entry : entries) {
        if (entry.folder || entry.link)
            continue;
        if (part_of(entry.path, entry.size, has_archives, mod_keys) != part)
            continue;
        if (!wanted_mod.empty() &&
            folded(entry.path).compare(0, wanted_mod.size(), wanted_mod) != 0)
            continue;
        files.push_back(entry.path);
    }
    return files;
}

} // namespace oa::app::game_files
