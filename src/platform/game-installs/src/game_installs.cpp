// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The places looked in for Total Annihilation folders, the search and the
// words for where a folder was found (game_installs.hpp). Every file is read
// up to its named limit and every folder listed up to prefix_entries_limit
// entries; a place that cannot be read is passed over.
#include "oa/platform/game_installs.hpp"

#include "oa/platform/files.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa::platform::game_installs {

namespace fs = std::filesystem;

namespace {

/// The folder of a Steam library that holds its manifests and games.
constexpr std::string_view steam_apps_folder = "steamapps";
/// The folder under steamapps that holds the games.
constexpr std::string_view steam_common_folder = "common";
/// The folder of Steam's own folder that newer clients keep a copy of the library list in.
constexpr std::string_view steam_config_folder = "config";
/// The list of a Steam folder's libraries.
constexpr std::string_view library_list_name = "libraryfolders.vdf";
/// What the name of an app's manifest starts with, before its app number.
constexpr std::string_view manifest_prefix = "appmanifest_";
/// What the name of an app's manifest ends with.
constexpr std::string_view manifest_suffix = ".acf";
/// Heroic's folder of GOG games, under its config folder.
constexpr std::string_view heroic_gog_folder = "gog_store";
/// Heroic's list of installed GOG games.
constexpr std::string_view heroic_list_name = "installed.json";
/// The folder of a Wine prefix that stands for the C: drive.
constexpr std::string_view prefix_drive_folder = "drive_c";
/// Folders at the top of drive_c that hold Windows itself and its users' folders (which link
/// to the home folder), never a game: they are not searched.
constexpr std::array<std::string_view, 2> passed_over_drive_folders{"windows", "users"};
/// Where removable drives mount on Linux.
constexpr std::string_view linux_removable_mount = "/run/media";
#if defined(__linux__)
/// Where removable drives mounted on older Linux systems.
constexpr std::string_view linux_legacy_mount = "/media";
#elif defined(__APPLE__)
/// Where macOS shows its volumes.
constexpr std::string_view mac_volumes_folder = "/Volumes";
#elif defined(_WIN32)
/// The first drive letter drive_folders lists on Windows: A: and B: are floppy drives and C:
/// the system's.
constexpr char first_listed_drive = 'D';
/// The last drive letter.
constexpr char last_drive = 'Z';
#endif

/// Converts UTF-8 text to a path, so that it is not read in a narrow code page.
///
/// @param text UTF-8 text
/// @return the path
fs::path path_of(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Returns a path's last component as UTF-8 text.
///
/// @param path the path
/// @return its file name
std::string name_of(const fs::path& path) {
    const std::u8string name = path.filename().u8string();
    return {name.begin(), name.end()};
}

/// Tells whether two names are equal, ASCII letters regardless of case.
///
/// @param left one name
/// @param right the other
/// @return true when they are the same
bool same_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        char a = left[at];
        char b = right[at];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/// Reads at most a number of bytes of a file.
///
/// @param file the file
/// @param limit the most bytes read
/// @return its first bytes, up to the limit; none when it is not a file or cannot be read
std::optional<std::string> read_bounded(const fs::path& file, std::size_t limit) {
    std::error_code error;
    if (!fs::is_regular_file(file, error))
        return std::nullopt;
    const uintmax_t size = fs::file_size(file, error);
    if (error)
        return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string text(static_cast<std::size_t>(std::min<uintmax_t>(size, limit)), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return text;
}

/// A folder's entries as the search needs them.
struct FolderListing {
    bool readable{};                    ///< the folder could be listed
    bool holds_archive{};               ///< it holds game_archive_name as a file
    std::vector<fs::path> subfolders{}; ///< its folders that are not links, in name order
};

/// Lists a folder, at most prefix_entries_limit entries.
///
/// @param folder the folder
/// @param with_subfolders keep its folders too
/// @return what it holds
FolderListing list_folder(const fs::path& folder, bool with_subfolders) {
    FolderListing listing;
    std::error_code error;
    fs::directory_iterator entry(folder, fs::directory_options::skip_permission_denied, error);
    if (error)
        return listing;
    listing.readable = true;
    std::size_t seen = 0;
    for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
        if (++seen > prefix_entries_limit)
            break;
        std::error_code kind_error;
        const std::string name = name_of(entry->path());
        if (same_name(name, game_archive_name) && entry->is_regular_file(kind_error))
            listing.holds_archive = true;
        if (with_subfolders && !entry->is_symlink(kind_error) && entry->is_directory(kind_error) &&
            !kind_error)
            listing.subfolders.push_back(entry->path());
    }
    std::sort(listing.subfolders.begin(), listing.subfolders.end());
    return listing;
}

/// Returns a folder's canonical path, or its absolute normal one when that cannot be found.
///
/// @param folder the folder
/// @return the path
fs::path canonical_or_normal(const fs::path& folder) {
    std::error_code error;
    fs::path canonical = fs::canonical(folder, error);
    if (!error)
        return canonical;
    fs::path absolute = fs::absolute(folder, error);
    return (error ? folder : absolute).lexically_normal();
}

/// Tells whether a path lies below a folder.
///
/// @param path the path
/// @param folder the folder
/// @return true when the path is the folder or lies under it
bool lies_under(const fs::path& path, const fs::path& folder) {
    const fs::path relative = path.lexically_relative(folder);
    return !relative.empty() && *relative.begin() != "..";
}

/// Tells whether an install folder's name is one folder: no separators, not "." or "..".
///
/// @param name the name a manifest gives
/// @return true when it names a folder directly under steamapps/common
bool plain_folder_name(std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string_view::npos &&
           name.find(':') == std::string_view::npos;
}

/// The search's findings so far.
class Search {
  public:

    /// Starts a search: the removable mounts are found once.
    ///
    /// @param roots the places looked in
    explicit Search(const SearchRoots& roots) {
        for (const fs::path& mount : roots.removable_mounts) {
            std::error_code error;
            if (fs::is_directory(mount, error))
                removable_.push_back(canonical_or_normal(mount));
        }
    }

    /// Tells whether the search has found as many folders as it returns.
    ///
    /// @return true at most_candidates
    [[nodiscard]] bool full() const noexcept { return found_.size() >= most_candidates; }

    /// Takes a folder when it holds the archive and was not found before.
    ///
    /// @param folder the folder
    /// @param source where it was found
    void offer(const fs::path& folder, Source source) {
        if (full() || folder.empty())
            return;
        if (!list_folder(folder, false).holds_archive)
            return;
        take(folder, source);
    }

    /// Takes a folder known to hold the archive, unless it was found before.
    ///
    /// @param folder the folder
    /// @param source where it was found
    void take(const fs::path& folder, Source source) {
        if (full())
            return;
        fs::path canonical = canonical_or_normal(folder);
        if (std::find(found_paths_.begin(), found_paths_.end(), canonical) != found_paths_.end())
            return;
        Candidate candidate;
        candidate.removable =
            std::any_of(removable_.begin(), removable_.end(), [&canonical](const fs::path& mount) {
                return lies_under(canonical, mount);
            });
        candidate.source = source;
        candidate.folder = canonical;
        found_paths_.push_back(std::move(canonical));
        found_.push_back(std::move(candidate));
    }

    /// Searches a Wine prefix's drive_c to prefix_search_depth for folders holding the archive.
    /// drive_c's windows and users folders, and any folder that cannot hold the game, are
    /// left unopened.
    ///
    /// @param drive the prefix's drive_c
    /// @param source who keeps the prefix
    void search_prefix(const fs::path& drive, Source source) {
        std::deque<std::pair<fs::path, uint32_t>> waiting;
        waiting.emplace_back(drive, 0);
        std::size_t looked = 0;
        while (!waiting.empty() && !full() && looked < prefix_folders_limit) {
            auto [folder, depth] = std::move(waiting.front());
            waiting.pop_front();
            ++looked;
            const bool deeper = depth < prefix_search_depth;
            FolderListing listing = list_folder(folder, deeper);
            if (listing.holds_archive) {
                take(folder, source);
                continue;
            }
            for (fs::path& subfolder : listing.subfolders) {
                if (depth == 0 && std::any_of(
                                      passed_over_drive_folders.begin(),
                                      passed_over_drive_folders.end(),
                                      [&subfolder](std::string_view name) {
                                          return same_name(name_of(subfolder), name);
                                      }
                                  ))
                    continue;
                if (folder_cannot_hold_game(name_of(subfolder)))
                    continue;
                waiting.emplace_back(std::move(subfolder), depth + 1);
            }
        }
    }

    /// Hands over what was found.
    ///
    /// @return the folders, in the order found
    std::vector<Candidate> results() && { return std::move(found_); }

  private:

    std::vector<fs::path> removable_{};   ///< the removable mounts, canonical
    std::vector<fs::path> found_paths_{}; ///< the canonical paths found
    std::vector<Candidate> found_{};      ///< the folders found
};

/// Searches the Steam libraries of one Steam folder: the folder itself and every library its
/// lists name, each one's manifest of Total Annihilation and the folder it names.
///
/// @param[in,out] search the search
/// @param root Steam's folder
/// @param[in,out] libraries the libraries already searched, canonical
void search_steam(Search& search, const fs::path& root, std::vector<fs::path>& libraries) {
    std::error_code error;
    if (!fs::is_directory(root, error))
        return;
    std::vector<fs::path> named{root};
    for (const fs::path& list : {
             root / steam_apps_folder / library_list_name,
             root / steam_config_folder / library_list_name,
         })
        if (const auto text = read_bounded(list, library_file_limit))
            for (fs::path& folder : library_folders(*text))
                named.push_back(std::move(folder));
    const std::string manifest_name = std::string(manifest_prefix) +
                                      std::to_string(total_annihilation_steam_app) +
                                      std::string(manifest_suffix);
    for (const fs::path& library : named) {
        if (search.full())
            return;
        if (!library.is_absolute() || !fs::is_directory(library, error))
            continue;
        fs::path canonical = canonical_or_normal(library);
        if (std::find(libraries.begin(), libraries.end(), canonical) != libraries.end())
            continue;
        libraries.push_back(std::move(canonical));
        const fs::path apps = library / steam_apps_folder;
        const auto manifest = read_bounded(apps / manifest_name, library_file_limit);
        if (!manifest)
            continue;
        const auto install_dir = manifest_install_dir(*manifest, total_annihilation_steam_app);
        if (!install_dir || !plain_folder_name(*install_dir))
            continue;
        search.offer(apps / steam_common_folder / path_of(*install_dir), Source::steam);
    }
}

/// Searches the prefixes under one folder: each child's drive_c.
///
/// @param[in,out] search the search
/// @param parent the folder whose children are prefixes
/// @param source who keeps them
void search_prefixes(Search& search, const fs::path& parent, Source source) {
    const FolderListing listing = list_folder(parent, true);
    for (const fs::path& prefix : listing.subfolders) {
        if (search.full())
            return;
        std::error_code error;
        const fs::path drive = prefix / prefix_drive_folder;
        if (fs::is_directory(drive, error))
            search.search_prefix(drive, source);
    }
}

/// Returns the places under a home folder, with the data and config folders given.
///
/// @param home the home folder
/// @param data_home the folder of the player's data (~/.local/share)
/// @param config_home the folder of the player's settings (~/.config)
/// @return the places
SearchRoots
roots_with(const fs::path& home, const fs::path& data_home, const fs::path& config_home) {
    SearchRoots roots;
    roots.home = home;
    const fs::path flatpak = home / ".var" / "app";
    roots.steam_roots = {
        data_home / "Steam",
        home / ".steam" / "steam",
        flatpak / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
    };
    roots.heroic_configs = {
        config_home / "heroic",
        flatpak / "com.heroicgameslauncher.hgl" / "config" / "heroic",
    };
    roots.prefix_parents = {
        home / "Games",
        data_home / "bottles" / "bottles",
        flatpak / "com.usebottles.bottles" / "data" / "bottles" / "bottles",
    };
    roots.prefix_sources = {Source::lutris, Source::bottles, Source::bottles};
    roots.removable_mounts = {fs::path(linux_removable_mount)};
    return roots;
}

/// Reads an environment variable as a path.
///
/// @param name the variable
/// @return its value; empty when unset or empty
fs::path environment_path(const char* name) {
#if defined(_WIN32)
    // The value in UTF-16, so that a folder named outside the narrow code page reads whole.
    std::wstring wide_name;
    for (const char* at = name; *at != '\0'; ++at)
        wide_name += static_cast<wchar_t>(*at);
    // Asked with no room, Windows gives the room the value needs with its terminating null; the
    // value can grow between the calls, so it is asked again until it fits.
    std::wstring value;
    DWORD room = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    while (room != 0) {
        value.resize(room);
        const DWORD copied = GetEnvironmentVariableW(wide_name.c_str(), value.data(), room);
        if (copied < room) {
            value.resize(copied);
            return fs::path(value);
        }
        room = copied;
    }
    return fs::path{};
#else
    const char* value = std::getenv(name);
    return value == nullptr ? fs::path{} : path_of(value);
#endif
}

#if defined(__linux__)
/// Adds a folder's subfolders to a list of drives, at most most_candidates in all.
///
/// @param[in,out] drives the drives
/// @param folder the folder whose subfolders are drives
void add_subfolders(std::vector<fs::path>& drives, const fs::path& folder) {
    for (fs::path& drive : list_folder(folder, true).subfolders)
        if (drives.size() < most_candidates)
            drives.push_back(std::move(drive));
}
#endif

} // namespace

bool folder_cannot_hold_game(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 17> skipped{
        "Library",
        "node_modules",
        ".git",
        ".svn",
        ".hg",
        "Caches",
        "Cache",
        "Pictures",
        "Movies",
        ".Trash",
        "$RECYCLE.BIN",
        "System Volume Information",
        "__MACOSX",
        ".npm",
        ".cache",
        "DerivedData",
        "Pods",
    };
    for (const std::string_view skipped_name : skipped) {
        if (same_name(name, skipped_name))
            return true;
    }
    return false;
}

SearchRoots search_roots_under(const fs::path& home) {
    return roots_with(home, home / ".local" / "share", home / ".config");
}

SearchRoots default_search_roots() {
#if defined(__linux__)
    const fs::path home = environment_path("HOME");
    if (home.empty() || !home.is_absolute())
        return {};
    // The XDG folders are honoured only as absolute paths, as the specification asks.
    fs::path data_home = environment_path("XDG_DATA_HOME");
    if (data_home.empty() || !data_home.is_absolute())
        data_home = home / ".local" / "share";
    fs::path config_home = environment_path("XDG_CONFIG_HOME");
    if (config_home.empty() || !config_home.is_absolute())
        config_home = home / ".config";
    return roots_with(home, data_home, config_home);
#else
    return {};
#endif
}

std::vector<Candidate> find_candidates(const SearchRoots& roots) {
    Search search(roots);
    std::vector<fs::path> libraries;
    for (const fs::path& root : roots.steam_roots)
        search_steam(search, root, libraries);
    for (const fs::path& config : roots.heroic_configs) {
        const auto text =
            read_bounded(config / heroic_gog_folder / heroic_list_name, heroic_file_limit);
        if (!text)
            continue;
        for (const fs::path& folder : heroic_install_paths(*text))
            if (folder.is_absolute())
                search.offer(folder, Source::heroic);
    }
    // Lutris's prefixes before Bottles', each in the order given.
    for (const Source source : {Source::lutris, Source::bottles})
        for (std::size_t index = 0; index < roots.prefix_parents.size(); ++index) {
            const Source keeper =
                index < roots.prefix_sources.size() ? roots.prefix_sources[index] : Source::lutris;
            if (keeper == source)
                search_prefixes(search, roots.prefix_parents[index], source);
        }
    return std::move(search).results();
}

std::string_view source_words(Source source, bool removable) noexcept {
    switch (source) {
    case Source::steam:
        return removable ? "your Steam library on the SD card" : "your Steam library";
    case Source::heroic:
        return "Heroic";
    case Source::lutris:
        return "Lutris";
    case Source::bottles:
        return "Bottles";
    }
    return {};
}

bool holds_game_archive(const fs::path& folder) {
    return list_folder(folder, false).holds_archive;
}

fs::path home_folder() {
#if defined(_WIN32)
    return environment_path("USERPROFILE");
#else
    return environment_path("HOME");
#endif
}

std::vector<fs::path> drive_folders() {
    std::vector<fs::path> drives;
#if defined(__linux__)
    // Today's systems mount each drive under a folder named for the user; older ones put the
    // drives directly under the mount folder.
    const fs::path user_variable = environment_path("USER");
    const std::string user_name = name_of(user_variable.empty() ? home_folder() : user_variable);
    for (const std::string_view base : {linux_removable_mount, linux_legacy_mount})
        for (const fs::path& folder : list_folder(fs::path(base), true).subfolders) {
            if (!user_name.empty() && name_of(folder) == user_name)
                add_subfolders(drives, folder);
            else if (drives.size() < most_candidates && base == linux_removable_mount)
                drives.push_back(folder);
        }
#elif defined(__APPLE__)
    // The start-up disk shows in /Volumes as a link to the root folder.
    for (const fs::path& volume : list_folder(fs::path(mac_volumes_folder), true).subfolders)
        if (drives.size() < most_candidates && canonical_or_normal(volume) != fs::path("/"))
            drives.push_back(volume);
#elif defined(_WIN32)
    // The drive letters Windows has, without touching the drives themselves.
    const DWORD letters = GetLogicalDrives();
    const std::string system = path_to_utf8(environment_path("SystemDrive"));
    for (char letter = first_listed_drive; letter <= last_drive; ++letter) {
        if ((letters & (DWORD{1} << static_cast<unsigned>(letter - 'A'))) == 0)
            continue;
        const std::string root = std::string(1, letter) + ":\\";
        if (!system.empty() && same_name(system.substr(0, 1), root.substr(0, 1)))
            continue;
        drives.emplace_back(root);
    }
#endif
    std::sort(drives.begin(), drives.end());
    drives.erase(std::unique(drives.begin(), drives.end()), drives.end());
    if (drives.size() > most_candidates)
        drives.resize(most_candidates);
    return drives;
}

namespace {

/// Tells whether a path is a folder that can be opened.
///
/// @param folder the path
/// @return true when it is a directory
bool folder_there(const fs::path& folder) {
    if (folder.empty())
        return false;
    std::error_code error;
    return fs::is_directory(folder, error);
}

/// Returns a path when it is a folder, else an empty path.
///
/// @param folder the path
/// @return the folder, or empty
fs::path existing_folder(const fs::path& folder) {
    return folder_there(folder) ? folder : fs::path{};
}

/// The last name of a volume, or "System" for the root folder.
///
/// @param volume the volume
/// @return the label
std::string volume_label(const fs::path& volume) {
    if (volume == "/")
        return "System";
    const std::string name = name_of(volume);
    if (!name.empty())
        return name;
    return path_to_utf8(volume);
}

/// Adds a place when its folder is not empty.
///
/// @param[in,out] places the places
/// @param folder the folder; empty is skipped
/// @param label the label
/// @param group the band
void add_place(
    std::vector<LocatorPlace>& places, const fs::path& folder, std::string label, LocatorGroup group
) {
    if (folder.empty())
        return;
    places.push_back(LocatorPlace{folder, std::move(label), group});
}

} // namespace

std::vector<LocatorPlace> locator_places(const LocatorFacts& facts) {
    std::vector<LocatorPlace> places;
    add_place(places, facts.home, "Home", LocatorGroup::places);
    add_place(places, facts.desktop, "Desktop", LocatorGroup::places);
    add_place(places, facts.documents, "Documents", LocatorGroup::places);
    add_place(places, facts.downloads, "Downloads", LocatorGroup::places);
    add_place(
        places,
        facts.applications,
        facts.program_files ? "Program Files" : "Applications",
        LocatorGroup::places
    );
    if (facts.deck) {
        for (std::size_t index = 0; index < facts.sd_cards.size(); ++index) {
            std::string label = "microSD card";
            if (facts.sd_cards.size() > 1) {
                const std::string name = name_of(facts.sd_cards[index]);
                if (!name.empty())
                    label += " " + name;
            }
            add_place(places, facts.sd_cards[index], std::move(label), LocatorGroup::places);
        }
    } else {
        for (const fs::path& volume : facts.volumes)
            add_place(places, volume, volume_label(volume), LocatorGroup::volumes);
    }
    add_place(places, facts.steam, "Steam · Proton", LocatorGroup::deck_games);
    add_place(places, facts.heroic, "Heroic", LocatorGroup::deck_games);
    add_place(places, facts.lutris, "Lutris", LocatorGroup::deck_games);
    add_place(places, facts.bottles, "Bottles", LocatorGroup::deck_games);
    return places;
}

LocatorFacts locator_place_facts(bool deck) {
    LocatorFacts facts;
    facts.deck = deck;
    facts.home = home_folder();
    if (folder_there(facts.home)) {
        facts.desktop = existing_folder(facts.home / "Desktop");
        facts.documents = existing_folder(facts.home / "Documents");
        facts.downloads = existing_folder(facts.home / "Downloads");
    } else {
        facts.home.clear();
    }
#if defined(_WIN32)
    facts.applications = existing_folder(environment_path("ProgramFiles"));
    facts.program_files = !facts.applications.empty();
#elif defined(__APPLE__)
    facts.applications = existing_folder(fs::path("/Applications"));
#endif
    const std::vector<fs::path> drives = drive_folders();
    if (deck)
        facts.sd_cards = drives;
    else
        facts.volumes = drives;
    const fs::path home = facts.home;
    if (!home.empty()) {
        const fs::path steam_tail = fs::path("steamapps") / "compatdata" /
                                    std::to_string(total_annihilation_steam_app) / "pfx";
        const fs::path candidates[] = {
            home / ".local" / "share" / "Steam" / steam_tail,
            home / ".steam" / "steam" / steam_tail,
            home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam" /
                steam_tail,
            home / "Library" / "Application Support" / "Steam" / steam_tail,
        };
        for (const fs::path& candidate : candidates) {
            if (folder_there(candidate)) {
                facts.steam = candidate;
                break;
            }
        }
        facts.heroic = existing_folder(home / "Games" / "Heroic");
        facts.lutris = existing_folder(home / "Games");
        facts.bottles = existing_folder(home / ".local" / "share" / "bottles" / "bottles");
        if (facts.bottles.empty()) {
            facts.bottles = existing_folder(
                home / ".var" / "app" / "com.usebottles.bottles" / "data" / "bottles" / "bottles"
            );
        }
    }
    return facts;
}

std::string path_under_home(const fs::path& home, const fs::path& folder) {
    if (folder.empty())
        return {};
    const fs::path shown = folder.lexically_normal();
    if (home.empty())
        return path_to_generic_utf8(shown);
    const fs::path relative = shown.lexically_relative(home.lexically_normal());
    const std::string tail = path_to_generic_utf8(relative);
    if (tail == ".")
        return "~";
    if (tail.empty() || tail.starts_with(".."))
        return path_to_generic_utf8(shown);
    return "~/" + tail;
}

bool drop_folder_name(std::string_view name) noexcept {
    return same_name(name, "Downloads") || same_name(name, "Desktop") ||
           same_name(name, "Documents");
}

bool offer_as_installation(
    std::string_view folder_name, bool holds_archive, bool holds_installer_file, bool home_itself
) noexcept {
    if (home_itself || !holds_archive || holds_installer_file || drop_folder_name(folder_name))
        return false;
    return true;
}

SetupFolderOffer offer_from_folder(
    const fs::path& folder,
    bool (*is_installer)(const fs::path& file, uint64_t size, void* context),
    void* context
) {
    SetupFolderOffer offer;
    std::error_code failure;
    if (!fs::is_directory(folder, failure) || failure)
        return offer;
    fs::directory_iterator entry(folder, fs::directory_options::skip_permission_denied, failure);
    if (failure)
        return offer;
    bool holds_archive = false;
    std::size_t seen = 0;
    for (; !failure && entry != fs::directory_iterator(); entry.increment(failure)) {
        if (++seen > prefix_entries_limit)
            break;
        std::error_code kind;
        if (!entry->is_regular_file(kind) || kind)
            continue;
        const fs::path path = entry->path();
        if (same_name(name_of(path), game_archive_name))
            holds_archive = true;
        if (is_installer == nullptr)
            continue;
        const uintmax_t bytes = fs::file_size(path, kind);
        if (kind || bytes > static_cast<uintmax_t>(std::numeric_limits<uint64_t>::max()))
            continue;
        if (is_installer(path, static_cast<uint64_t>(bytes), context))
            offer.installers.push_back(path);
    }
    bool home_itself = false;
    const fs::path house = home_folder();
    if (!house.empty()) {
        std::error_code same;
        home_itself = fs::equivalent(folder, house, same) && !same;
    }
    offer.folder_offered = offer_as_installation(
        name_of(folder), holds_archive, !offer.installers.empty(), home_itself
    );
    return offer;
}

namespace {

/// The rank of a place's kind. An installer and an installation outrank a loose folder, and
/// neither outranks the other.
///
/// @param place the kind
/// @return 1 for an installer or an installation, 0 for a loose folder
int place_rank(SetupPlace place) noexcept {
    return place == SetupPlace::loose ? 0 : 1;
}

} // namespace

std::vector<SetupSource> rank_setup_sources(std::vector<SetupSource> found) {
    std::vector<SetupSource> kept;
    kept.reserve(found.size());
    for (SetupSource& source : found) {
        if (source.place != SetupPlace::loose)
            kept.push_back(std::move(source));
    }
    std::stable_sort(
        kept.begin(), kept.end(), [](const SetupSource& left, const SetupSource& right) {
            if (left.bundle_count != right.bundle_count)
                return left.bundle_count > right.bundle_count;
            if (left.base_rank != right.base_rank)
                return left.base_rank > right.base_rank;
            if (left.expansion_count != right.expansion_count)
                return left.expansion_count > right.expansion_count;
            return place_rank(left.place) > place_rank(right.place);
        }
    );
    return kept;
}

} // namespace oa::platform::game_installs
