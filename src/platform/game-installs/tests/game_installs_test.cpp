// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The search for Total Annihilation folders over synthetic home folders
// built in a temporary folder (Steam with an SD-card library and a Flatpak
// Steam, Heroic native and Flatpak, a Lutris prefix and a Bottles bottle),
// and the readers of each library file, malformed ones included.
#include "oa/platform/game_installs.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace installs = oa::platform::game_installs;

/// Returns a folder's canonical path, as the search reports it.
///
/// @param folder the folder
/// @return its canonical path, or the folder itself when it cannot be found
fs::path canonical_of(const fs::path& folder) {
    std::error_code error;
    const fs::path path = fs::canonical(folder, error);
    return error ? folder : path;
}

/// Writes a file, making its folders.
///
/// @param file the file
/// @param text what it holds
void write_file(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << text;
}

/// Makes a game folder: a folder holding the game's first archive under a name.
///
/// @param folder the folder
/// @param archive the archive's name, in any case
void make_game_folder(const fs::path& folder, const std::string& archive = "totala1.hpi") {
    write_file(folder / archive, "HAPI");
}

/// Returns UTF-8 text for a path, for the files the test writes.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

/// Returns a path as it stands inside a quoted string of Steam's and Heroic's files: UTF-8, each
/// backslash and quote escaped, as both programs write them. A Windows path's backslashes
/// would otherwise read as escapes (\u, \n, \t) or break the string.
///
/// @param path the path
/// @return its escaped UTF-8 spelling
std::string quoted_text(const fs::path& path) {
    std::string text;
    for (const char character : utf8(path)) {
        if (character == '\\' || character == '"')
            text += '\\';
        text += character;
    }
    return text;
}

/// Returns a Steam manifest of an app.
///
/// @param app the app number
/// @param install_dir the folder's name under steamapps/common
/// @return the manifest's text
std::string manifest(uint32_t app, const std::string& install_dir) {
    return "\"AppState\"\n{\n\t\"appid\"\t\t\"" + std::to_string(app) +
           "\"\n\t\"Universe\"\t\t\"1\"\n\t\"name\"\t\t\"Total Annihilation\"\n"
           "\t\"installdir\"\t\t\"" +
           install_dir + "\"\n\t\"UserConfig\"\n\t{\n\t\t\"language\"\t\t\"english\"\n\t}\n}\n";
}

/// Returns the file name Steam gives an app's manifest.
///
/// @param app the app number
/// @return the manifest's file name
std::string manifest_file(uint32_t app) {
    return "appmanifest_" + std::to_string(app) + ".acf";
}

/// Checks the words for where a folder was found.
void check_source_words() {
    OA_CHECK(installs::source_words(installs::Source::steam, false) == "your Steam library");
    OA_CHECK(
        installs::source_words(installs::Source::steam, true) == "your Steam library on the SD card"
    );
    OA_CHECK(installs::source_words(installs::Source::heroic, false) == "Heroic");
    OA_CHECK(installs::source_words(installs::Source::lutris, false) == "Lutris");
    OA_CHECK(installs::source_words(installs::Source::bottles, true) == "Bottles");
}

/// Checks the places under a home folder.
void check_search_roots() {
    const fs::path home = "/home/deck";
    const installs::SearchRoots roots = installs::search_roots_under(home);
    OA_CHECK(roots.home == home);
    OA_CHECK(roots.steam_roots.size() == 3);
    OA_CHECK(roots.steam_roots[0] == home / ".local" / "share" / "Steam");
    OA_CHECK(roots.steam_roots[1] == home / ".steam" / "steam");
    OA_CHECK(
        roots.steam_roots[2] ==
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam"
    );
    OA_CHECK(roots.heroic_configs.size() == 2);
    OA_CHECK(roots.heroic_configs[0] == home / ".config" / "heroic");
    OA_CHECK(
        roots.heroic_configs[1] ==
        home / ".var" / "app" / "com.heroicgameslauncher.hgl" / "config" / "heroic"
    );
    OA_CHECK(roots.prefix_parents.size() == roots.prefix_sources.size());
    OA_CHECK(!roots.prefix_parents.empty() && roots.prefix_parents[0] == home / "Games");
    OA_CHECK(!roots.prefix_sources.empty() && roots.prefix_sources[0] == installs::Source::lutris);
    const auto flatpak_bottles =
        home / ".var" / "app" / "com.usebottles.bottles" / "data" / "bottles" / "bottles";
    const auto bottles =
        std::find(roots.prefix_parents.begin(), roots.prefix_parents.end(), flatpak_bottles);
    OA_CHECK(bottles != roots.prefix_parents.end());
    if (bottles != roots.prefix_parents.end())
        OA_CHECK(
            roots
                .prefix_sources[static_cast<std::size_t>(bottles - roots.prefix_parents.begin())] ==
            installs::Source::bottles
        );
    OA_CHECK(roots.removable_mounts.size() == 1 && roots.removable_mounts[0] == "/run/media");
#if !defined(__linux__)
    OA_CHECK(installs::default_search_roots().steam_roots.empty());
#endif
}

/// Checks the reader of Steam's library list, malformed lists included.
void check_library_folders() {
    const std::string today =
        "\"libraryfolders\"\n{\n"
        "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/home/deck/.local/share/Steam\"\n"
        "\t\t\"label\"\t\t\"\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"298030\"\t\t\"1234\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/run/media/mmcblk0p1\"\n\t}\n}\n";
    const auto folders = installs::library_folders(today);
    OA_CHECK(folders.size() == 2);
    OA_CHECK(folders.size() == 2 && folders[0] == "/home/deck/.local/share/Steam");
    OA_CHECK(folders.size() == 2 && folders[1] == "/run/media/mmcblk0p1");

    // Escapes are undone; a backslash before another character is kept.
    const auto escaped = installs::library_folders(
        "\"libraryfolders\" { \"0\" { \"path\" \"D:\\\\SteamLibrary\" } "
        "\"1\" { \"PATH\" \"/with \\\"quote\\\"\" } \"2\" { \"path\" \"C:\\x\" } }"
    );
    OA_CHECK(escaped.size() == 3);
    OA_CHECK(escaped.size() == 3 && escaped[0] == fs::path("D:\\SteamLibrary"));
    OA_CHECK(escaped.size() == 3 && escaped[1] == fs::path("/with \"quote\""));
    OA_CHECK(escaped.size() == 3 && escaped[2] == fs::path("C:\\x"));

    // Older lists name each library by a number directly under the top block.
    const auto older = installs::library_folders(
        "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1700000000\"\n"
        "\t\"ContentStatsID\"\t\t\"-123\"\n\t\"1\"\t\t\"/mnt/games\"\n}\n"
    );
    OA_CHECK(older.size() == 1 && older[0] == "/mnt/games");

    // Comments are skipped.
    const auto commented = installs::library_folders(
        "// a comment\n\"libraryfolders\" { \"0\" { \"path\" \"/a\" // and another\n } }"
    );
    OA_CHECK(commented.size() == 1 && commented[0] == "/a");

    // A list that breaks off, or breaks the format, gives what was read before the fault.
    const auto cut = installs::library_folders(
        "\"libraryfolders\" { \"0\" { \"path\" \"/a\" } \"1\" { \"path\" \"/b"
    );
    OA_CHECK(cut.size() == 1 && cut[0] == "/a");
    const auto extra_close = installs::library_folders(
        "\"libraryfolders\" { \"0\" { \"path\" \"/a\" } } } \"path\" \"/c\""
    );
    OA_CHECK(extra_close.size() == 1 && extra_close[0] == "/a");
    const auto no_value =
        installs::library_folders("\"libraryfolders\" { \"0\" { \"path\" \"/a\" \"path\" } }");
    OA_CHECK(no_value.size() == 1 && no_value[0] == "/a");
    std::string deep;
    for (std::size_t level = 0; level <= installs::most_nesting; ++level)
        deep += "\"k\" { ";
    deep += "\"path\" \"/deep\"";
    OA_CHECK(installs::library_folders(deep).empty());
    OA_CHECK(installs::library_folders("").empty());
    OA_CHECK(installs::library_folders("{{{").empty());
}

/// Checks the reader of Steam's app manifests.
void check_manifest() {
    const std::string text = manifest(installs::total_annihilation_steam_app, "Total Annihilation");
    const auto install_dir =
        installs::manifest_install_dir(text, installs::total_annihilation_steam_app);
    OA_CHECK(install_dir && *install_dir == "Total Annihilation");
    OA_CHECK(!installs::manifest_install_dir(text, 1));
    OA_CHECK(!installs::manifest_install_dir(manifest(42, "Other"), 298030));
    OA_CHECK(!installs::manifest_install_dir("\"AppState\" { \"appid\" \"298030\" }", 298030));
    OA_CHECK(!installs::manifest_install_dir(
        "\"AppState\" { \"appid\" \"298030x\" \"installdir\" \"TA\" }", 298030
    ));
    // Keys inside a nested block are not the manifest's.
    OA_CHECK(!installs::manifest_install_dir(
        "\"AppState\" { \"UserConfig\" { \"appid\" \"298030\" \"installdir\" \"TA\" } }", 298030
    ));
    // A manifest cut short after both keys still names the folder.
    const auto cut = installs::manifest_install_dir(
        "\"AppState\" { \"appid\" \"298030\" \"installdir\" \"TA\" \"name\" \"Total", 298030
    );
    OA_CHECK(cut && *cut == "TA");
    OA_CHECK(!installs::manifest_install_dir("\"AppState\" { \"appid\" \"2980", 298030));
}

/// Checks the reader of Heroic's installed list, malformed lists included.
void check_heroic_list() {
    const std::string text =
        "{\n  \"installed\": [\n    {\n      \"platform\": \"windows\",\n"
        "      \"executable\": \"Launcher.exe\",\n"
        "      \"install_path\": \"/home/deck/Games/Heroic/Total Annihilation\",\n"
        "      \"install_size\": \"1.1 GiB\",\n      \"is_dlc\": false,\n"
        "      \"version\": \"3.1\",\n      \"appName\": \"1207658782\",\n"
        "      \"installedDLCs\": [],\n      \"cyberpunk\": {\"install_path\": \"/no\"}\n    },\n"
        "    {\"install_path\": \"/home/deck/Games/Heroic/Caf\\u00e9 \\ud83d\\ude00\", "
        "\"pinned\": null, \"size\": -1.5e3}\n  ],\n"
        "  \"other\": [{\"install_path\": \"/elsewhere\"}]\n}\n";
    const auto paths = installs::heroic_install_paths(text);
    OA_CHECK(paths.size() == 2);
    OA_CHECK(paths.size() == 2 && paths[0] == "/home/deck/Games/Heroic/Total Annihilation");
    OA_CHECK(
        paths.size() == 2 &&
        utf8(paths[1]) == "/home/deck/Games/Heroic/Caf\xc3\xa9 \xf0\x9f\x98\x80"
    );
    // A Windows path, its backslashes escaped as Heroic writes them.
    const auto windows = installs::heroic_install_paths(
        "{\"installed\": [{\"install_path\": \"C:\\\\Games\\\\Total Annihilation\"}]}"
    );
    OA_CHECK(windows.size() == 1 && windows[0] == fs::path("C:\\Games\\Total Annihilation"));
    // The list "installed" must be the top object's.
    OA_CHECK(
        installs::heroic_install_paths("{\"other\": {\"installed\": [{\"install_path\": \"/x\"}]}}")
            .empty()
    );
    // A list that breaks off, or breaks the format, gives what was read before the fault.
    const auto cut = installs::heroic_install_paths(
        "{\"installed\": [{\"install_path\": \"/a\"}, {\"install_path\": \"/b\""
    );
    OA_CHECK(cut.size() == 2 && cut[0] == "/a" && cut[1] == "/b");
    const auto no_comma = installs::heroic_install_paths(
        "{\"installed\": [{\"install_path\": \"/a\"} {\"install_path\": \"/c\"}]}"
    );
    OA_CHECK(no_comma.size() == 1 && no_comma[0] == "/a");
    const auto bad_escape = installs::heroic_install_paths(
        "{\"installed\": [{\"install_path\": \"/a\"}, {\"install_path\": \"/b\\q\"}]}"
    );
    OA_CHECK(bad_escape.size() == 1 && bad_escape[0] == "/a");
    const auto lone_surrogate = installs::heroic_install_paths(
        "{\"installed\": [{\"install_path\": \"/a\"}, {\"install_path\": \"/\\udc00\"}]}"
    );
    OA_CHECK(lone_surrogate.size() == 1);
    const auto number = installs::heroic_install_paths(
        "{\"installed\": [{\"install_path\": 5}, {\"install_path\": \"/c\"}]}"
    );
    OA_CHECK(number.size() == 1 && number[0] == "/c");
    std::string deep = "{\"installed\": [";
    for (std::size_t level = 0; level <= installs::most_nesting; ++level)
        deep += "[";
    OA_CHECK(installs::heroic_install_paths(deep).empty());
    OA_CHECK(installs::heroic_install_paths("").empty());
    OA_CHECK(installs::heroic_install_paths("[1, 2]").empty());
    OA_CHECK(installs::heroic_install_paths("{\"installed\": []}").empty());
}

/// Builds synthetic home folders and checks what the search finds in them, and in what order.
///
/// @param temporary a folder of the test's own
void check_search(const fs::path& temporary) {
    const fs::path home = temporary / "home" / "deck";
    const fs::path media = temporary / "run" / "media";
    const fs::path sd_card = media / "mmcblk0p1";
    installs::SearchRoots roots = installs::search_roots_under(home);
    roots.removable_mounts = {media};

    // Native Steam: its own library and an SD card's, both holding the game; the SD card is
    // named twice.
    const fs::path steam = home / ".local" / "share" / "Steam";
    const fs::path native_game = steam / "steamapps" / "common" / "Total Annihilation";
    const fs::path sd_game = sd_card / "steamapps" / "common" / "Total Annihilation";
    write_file(
        steam / "steamapps" / "libraryfolders.vdf",
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + quoted_text(steam) +
            "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + quoted_text(sd_card) +
            "\"\n\t}\n"
            "\t\"2\"\n\t{\n\t\t\"path\"\t\t\"" +
            quoted_text(sd_card) +
            "\"\n\t}\n"
            "\t\"3\"\n\t{\n\t\t\"path\"\t\t\"" +
            quoted_text(temporary / "unplugged") + "\"\n\t}\n}\n"
    );
    write_file(
        steam / "steamapps" / manifest_file(installs::total_annihilation_steam_app),
        manifest(298030, "Total Annihilation")
    );
    make_game_folder(native_game);
    write_file(
        sd_card / "steamapps" / manifest_file(installs::total_annihilation_steam_app),
        manifest(298030, "Total Annihilation")
    );
    make_game_folder(sd_game);
    // A library whose manifest names a folder outside steamapps/common is passed over, and a
    // folder that is there but has no manifest is never guessed.
    const fs::path sneaky = temporary / "sneaky";
    write_file(
        sneaky / "steamapps" / manifest_file(installs::total_annihilation_steam_app),
        manifest(298030, "../../outside")
    );
    make_game_folder(temporary / "outside");
    make_game_folder(temporary / "guessed" / "steamapps" / "common" / "Total Annihilation");
    write_file(
        steam / "config" / "libraryfolders.vdf",
        "\"libraryfolders\" { \"0\" { \"path\" \"" + quoted_text(sneaky) +
            "\" } \"1\" { \"path\" \"" + quoted_text(temporary / "guessed") + "\" } }"
    );
    // ~/.steam/steam is a link to the same Steam folder: its libraries are searched once.
    std::error_code link_error;
    fs::create_directories(home / ".steam", link_error);
    fs::create_directory_symlink(steam, home / ".steam" / "steam", link_error);

    // Flatpak Steam, with no library list: its own folder is its library.
    const fs::path flatpak_steam =
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam";
    const fs::path flatpak_game = flatpak_steam / "steamapps" / "common" / "TA Flatpak";
    write_file(
        flatpak_steam / "steamapps" / manifest_file(installs::total_annihilation_steam_app),
        manifest(298030, "TA Flatpak")
    );
    make_game_folder(flatpak_game, "TOTALA1.HPI");

    // Heroic, native and Flatpak: another game's folder and a folder named twice.
    const fs::path heroic_game = home / "Games" / "Heroic" / "Total Annihilation";
    const fs::path other_game = home / "Games" / "Heroic" / "Another Game";
    make_game_folder(heroic_game, "TotalA1.Hpi");
    write_file(other_game / "game.exe", "MZ");
    write_file(
        home / ".config" / "heroic" / "gog_store" / "installed.json",
        "{\"installed\": [{\"install_path\": \"" + quoted_text(other_game) +
            "\"}, {\"install_path\": \"" + quoted_text(heroic_game) + "\"}]}"
    );
    const fs::path flatpak_heroic_game = home / "Games" / "Heroic Flatpak" / "Total Annihilation";
    make_game_folder(flatpak_heroic_game);
    write_file(
        home / ".var" / "app" / "com.heroicgameslauncher.hgl" / "config" / "heroic" / "gog_store" /
            "installed.json",
        "{\"installed\": [{\"install_path\": \"" + quoted_text(flatpak_heroic_game) +
            "\"}, {\"install_path\": \"" + quoted_text(native_game) +
            "\"}, {\"install_path\": \"relative\"}"
    );

    // A Lutris prefix, with copies of the archive where the search never looks: under drive_c's
    // windows folder, deeper than the search goes, and below a game folder already found.
    const fs::path prefix = home / "Games" / "total-annihilation" / "drive_c";
    const fs::path lutris_game = prefix / "GOG Games" / "Total Annihilation";
    make_game_folder(lutris_game);
    make_game_folder(lutris_game / "nested copy");
    make_game_folder(prefix / "windows" / "system32");
    make_game_folder(prefix / "a" / "b" / "c" / "d" / "too deep");
    // A Bottles bottle, the game four folders below drive_c.
    const fs::path bottle = home / ".var" / "app" / "com.usebottles.bottles" / "data" / "bottles" /
                            "bottles" / "Total Annihilation" / "drive_c";
    const fs::path bottles_game =
        bottle / "Program Files (x86)" / "GOG Galaxy" / "Games" / "Total Annihilation";
    make_game_folder(bottles_game);

    const std::vector<installs::Candidate> found = installs::find_candidates(roots);
    const std::vector<std::pair<fs::path, installs::Source>> expected{
        {native_game, installs::Source::steam},
        {sd_game, installs::Source::steam},
        {flatpak_game, installs::Source::steam},
        {heroic_game, installs::Source::heroic},
        {flatpak_heroic_game, installs::Source::heroic},
        {lutris_game, installs::Source::lutris},
        {bottles_game, installs::Source::bottles},
    };
    OA_CHECK(found.size() == expected.size());
    for (std::size_t index = 0; index < std::min(found.size(), expected.size()); ++index) {
        OA_CHECK(found[index].folder == canonical_of(expected[index].first));
        OA_CHECK(found[index].source == expected[index].second);
        OA_CHECK(found[index].removable == (index == 1));
    }
    for (const installs::Candidate& candidate : found)
        if (candidate.folder == canonical_of(sd_game))
            OA_CHECK(
                installs::source_words(candidate.source, candidate.removable) ==
                "your Steam library on the SD card"
            );

    OA_CHECK(installs::holds_game_archive(flatpak_game));
    OA_CHECK(installs::holds_game_archive(heroic_game));
    OA_CHECK(!installs::holds_game_archive(other_game));
    OA_CHECK(!installs::holds_game_archive(temporary / "missing"));

    // Places that are not there are passed over.
    installs::SearchRoots nowhere = installs::search_roots_under(temporary / "nobody");
    nowhere.removable_mounts = {temporary / "no media"};
    OA_CHECK(installs::find_candidates(nowhere).empty());
    OA_CHECK(installs::find_candidates({}).empty());

    // At most most_candidates folders are returned.
    installs::SearchRoots many;
    many.prefix_parents = {temporary / "many"};
    for (std::size_t index = 0; index < installs::most_candidates + 3; ++index)
        make_game_folder(
            temporary / "many" / ("p" + std::to_string(100 + index)) / "drive_c" / "TA"
        );
    OA_CHECK(installs::find_candidates(many).size() == installs::most_candidates);
}

/// Names that cannot hold the game are left unopened. A wrapper whose name ends in .app is
/// still walked, so a game under its drive_c is found.
///
/// @param temporary a folder this check owns
void check_skipped_folders(const fs::path& temporary) {
    OA_CHECK(installs::folder_cannot_hold_game("Library"));
    OA_CHECK(installs::folder_cannot_hold_game("LIBRARY"));
    OA_CHECK(installs::folder_cannot_hold_game("node_modules"));
    OA_CHECK(installs::folder_cannot_hold_game(".git"));
    OA_CHECK(installs::folder_cannot_hold_game("Caches"));
    OA_CHECK(installs::folder_cannot_hold_game("$RECYCLE.BIN"));
    OA_CHECK(installs::folder_cannot_hold_game("System Volume Information"));
    OA_CHECK(installs::folder_cannot_hold_game("__MACOSX"));
    OA_CHECK(!installs::folder_cannot_hold_game("Game.app"));
    OA_CHECK(!installs::folder_cannot_hold_game("Downloads"));
    OA_CHECK(!installs::folder_cannot_hold_game("Documents"));
    OA_CHECK(!installs::folder_cannot_hold_game("Desktop"));
    OA_CHECK(!installs::folder_cannot_hold_game("Applications"));
    OA_CHECK(!installs::folder_cannot_hold_game("drive_c"));
    OA_CHECK(!installs::folder_cannot_hold_game("windows"));
    OA_CHECK(!installs::folder_cannot_hold_game("Total Annihilation"));

    const fs::path parent = temporary / "skipped-prefixes";
    const fs::path hidden = parent / "hidden" / "drive_c" / "Library" / "Total Annihilation";
    const fs::path wrapper = parent / "wrapper" / "drive_c" / "Game.app" / "Contents" /
                             "Resources" / "Total Annihilation";
    make_game_folder(hidden);
    make_game_folder(wrapper);
    installs::SearchRoots roots;
    roots.prefix_parents = {parent};
    const std::vector<installs::Candidate> found = installs::find_candidates(roots);
    OA_CHECK(found.size() == 1);
    if (!found.empty())
        OA_CHECK(found[0].folder == canonical_of(wrapper));
}

} // namespace

/// The locator's places, from facts, with empty paths left out. No disk is read.
void check_locator_places() {
    installs::LocatorFacts facts;
    facts.home = "/home/play";
    facts.desktop = "/home/play/Desktop";
    facts.downloads = "/home/play/Downloads";
    facts.applications = "/Applications";
    facts.volumes = {"/", "/Volumes/TOTALA"};
    facts.steam = "/home/play/.local/share/Steam/steamapps/compatdata/298030/pfx";
    facts.heroic = "/home/play/Games/Heroic";
    const std::vector<installs::LocatorPlace> places = installs::locator_places(facts);
    OA_CHECK(places.size() == 8);
    OA_CHECK(places[0].label == "Home" && places[0].group == installs::LocatorGroup::places);
    OA_CHECK(places[1].label == "Desktop");
    OA_CHECK(places[2].label == "Downloads");
    OA_CHECK(places[3].label == "Applications");
    OA_CHECK(places[4].label == "System" && places[4].group == installs::LocatorGroup::volumes);
    OA_CHECK(places[5].label == "TOTALA" && places[5].group == installs::LocatorGroup::volumes);
    OA_CHECK(
        places[6].label == "Steam · Proton" && places[6].group == installs::LocatorGroup::deck_games
    );
    OA_CHECK(places[6].folder == facts.steam);
    OA_CHECK(places[7].label == "Heroic" && places[7].group == installs::LocatorGroup::deck_games);
    OA_CHECK(places[7].folder == facts.heroic);
    OA_CHECK(installs::path_under_home("/home/play", "/home/play") == "~");
    OA_CHECK(installs::path_under_home("/home/play", "/home/play/Games") == "~/Games");
    OA_CHECK(
        installs::path_under_home("/home/play", "/home/play/Games/Total Annihilation") ==
        "~/Games/Total Annihilation"
    );
    OA_CHECK(installs::path_under_home("/home/play", "/Volumes/TOTALA") == "/Volumes/TOTALA");
    OA_CHECK(installs::path_under_home({}, "/home/play/Games") == "/home/play/Games");

    installs::LocatorFacts deck;
    deck.deck = true;
    deck.home = "/home/deck";
    deck.sd_cards = {"/run/media/mmcblk0p1", "/run/media/extra"};
    deck.volumes = {"/should-not-show"};
    deck.lutris = "/home/deck/Games";
    deck.bottles = "/home/deck/.local/share/bottles/bottles";
    const std::vector<installs::LocatorPlace> on_deck = installs::locator_places(deck);
    OA_CHECK(on_deck.size() == 5);
    OA_CHECK(on_deck[0].label == "Home");
    OA_CHECK(
        on_deck[1].label == "microSD card mmcblk0p1" &&
        on_deck[1].group == installs::LocatorGroup::places
    );
    OA_CHECK(on_deck[2].label == "microSD card extra");
    OA_CHECK(
        on_deck[3].label == "Lutris" && on_deck[3].group == installs::LocatorGroup::deck_games
    );
    OA_CHECK(on_deck[4].label == "Bottles");

    installs::LocatorFacts windows;
    windows.applications = "C:/Program Files";
    windows.program_files = true;
    const std::vector<installs::LocatorPlace> programs = installs::locator_places(windows);
    OA_CHECK(programs.size() == 1 && programs[0].label == "Program Files");

    OA_CHECK(installs::locator_places({}).empty());
}

/// A home folder, a prefix and a game folder named in scripts no one code page holds are
/// searched and shown in UTF-8: the game is found, and the words for where it lies, the volume
/// labels and the microSD card's name are its names' UTF-8 spellings.
///
/// @param temporary a folder this check owns
void check_names_outside_code_page(const fs::path& temporary) {
    // "Игрок 玩家 🎮", "Префикс" and "Тотал 全面戰爭".
    const std::string player = "\xd0\x98\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba \xe7\x8e\xa9\xe5\xae\xb6 "
                               "\xf0\x9f\x8e\xae";
    const std::string prefix = "\xd0\x9f\xd1\x80\xd0\xb5\xd1\x84\xd0\xb8\xd0\xba\xd1\x81";
    const std::string game = "\xd0\xa2\xd0\xbe\xd1\x82\xd0\xb0\xd0\xbb "
                             "\xe5\x85\xa8\xe9\x9d\xa2\xe6\x88\xb0\xe7\x88\xad";
    const auto named = [](const std::string& text) {
        return fs::path(std::u8string(text.begin(), text.end()));
    };
    const fs::path home = temporary / named(player);
    const fs::path folder = home / "Games" / named(prefix) / "drive_c" / named(game);
    make_game_folder(folder);
    installs::SearchRoots roots;
    roots.prefix_parents = {home / "Games"};
    roots.prefix_sources = {installs::Source::lutris};
    const std::vector<installs::Candidate> found = installs::find_candidates(roots);
    OA_CHECK(found.size() == 1);
    if (found.size() == 1) {
        OA_CHECK(utf8(found[0].folder.filename()) == game);
        OA_CHECK(
            installs::path_under_home(canonical_of(home), found[0].folder) ==
            "~/Games/" + prefix + "/drive_c/" + game
        );
        const std::u8string shown = found[0].folder.lexically_normal().generic_u8string();
        OA_CHECK(
            installs::path_under_home({}, found[0].folder) ==
            std::string(shown.begin(), shown.end())
        );
    }

    installs::LocatorFacts facts;
    facts.volumes = {temporary / named(game)};
    const std::vector<installs::LocatorPlace> places = installs::locator_places(facts);
    OA_CHECK(places.size() == 1 && places[0].label == game);
    installs::LocatorFacts deck;
    deck.deck = true;
    deck.sd_cards = {temporary / named(prefix), temporary / named(player)};
    const std::vector<installs::LocatorPlace> cards = installs::locator_places(deck);
    OA_CHECK(cards.size() == 2 && cards[0].label == "microSD card " + prefix);
    OA_CHECK(cards.size() == 2 && cards[1].label == "microSD card " + player);
}

/// A Downloads folder of loose game files and two installers lists the installers, and the
/// fuller one is the place to preselect.
///
/// @param root a temporary folder
void check_setup_sources(const fs::path& root) {
    const fs::path downloads = root / "G58-setup" / "Downloads";
    write_file(downloads / "totala1.hpi", "loose-3.1c");
    write_file(downloads / "music.mp3", "loose-track");
    write_file(downloads / "tileset.ufo", "loose-tiles");
    write_file(downloads / "setup-full.exe", "fuller-installer");
    write_file(downloads / "setup-thin.exe", "thinner-installer");
    const auto named_installer = [](const fs::path& file, uint64_t, void*) {
        const std::string name = file.filename().string();
        return name == "setup-full.exe" || name == "setup-thin.exe";
    };
    const installs::SetupFolderOffer offer =
        installs::offer_from_folder(downloads, named_installer, nullptr);
    OA_CHECK(!offer.folder_offered);
    OA_CHECK(offer.installers.size() == 2);

    const auto no_installer = [](const fs::path&, uint64_t, void*) { return false; };
    const fs::path copied = root / "G58-setup" / "Total Annihilation";
    make_game_folder(copied);
    const installs::SetupFolderOffer copied_offer =
        installs::offer_from_folder(copied, no_installer, nullptr);
    OA_CHECK(copied_offer.folder_offered);
    OA_CHECK(copied_offer.installers.empty());

    const fs::path desktop = root / "G58-setup" / "Desktop";
    make_game_folder(desktop);
    OA_CHECK(!installs::offer_from_folder(desktop, no_installer, nullptr).folder_offered);
    const fs::path documents = root / "G58-setup" / "documents";
    make_game_folder(documents);
    OA_CHECK(!installs::offer_from_folder(documents, no_installer, nullptr).folder_offered);
    const fs::path mixed = root / "G58-setup" / "Mixed";
    make_game_folder(mixed);
    write_file(mixed / "setup-full.exe", "inside");
    const installs::SetupFolderOffer mixed_offer =
        installs::offer_from_folder(mixed, named_installer, nullptr);
    OA_CHECK(!mixed_offer.folder_offered);
    OA_CHECK(mixed_offer.installers.size() == 1);

    OA_CHECK(installs::drop_folder_name("Downloads"));
    OA_CHECK(installs::drop_folder_name("downloads"));
    OA_CHECK(installs::drop_folder_name("Desktop"));
    OA_CHECK(installs::drop_folder_name("Documents"));
    OA_CHECK(!installs::drop_folder_name("Total Annihilation"));
    OA_CHECK(!installs::offer_as_installation("Downloads", true, false, false));
    OA_CHECK(!installs::offer_as_installation("Games", true, true, false));
    OA_CHECK(!installs::offer_as_installation("Games", false, false, false));
    OA_CHECK(!installs::offer_as_installation("Games", true, false, true));
    OA_CHECK(installs::offer_as_installation("Games", true, false, false));

    // The loose folder holds more bundles than either installer. It is still not a row, and
    // the fuller installer, given second, is the one to preselect.
    installs::SetupSource loose;
    loose.token = "downloads";
    loose.place = installs::SetupPlace::loose;
    loose.bundle_count = 8;
    loose.base_rank = 2;
    loose.expansion_count = 6;
    installs::SetupSource thin;
    thin.token = "setup-thin.exe";
    thin.place = installs::SetupPlace::installer;
    thin.bundle_count = 2;
    thin.base_rank = 2;
    thin.expansion_count = 1;
    installs::SetupSource full;
    full.token = "setup-full.exe";
    full.place = installs::SetupPlace::installer;
    full.bundle_count = 5;
    full.base_rank = 2;
    full.expansion_count = 4;
    bool saw_full = false;
    bool saw_thin = false;
    for (const fs::path& installer : offer.installers) {
        const std::string name = installer.filename().string();
        if (name == "setup-full.exe")
            saw_full = true;
        if (name == "setup-thin.exe")
            saw_thin = true;
        OA_CHECK(name != "totala1.hpi");
    }
    OA_CHECK(saw_full && saw_thin);
    installs::SetupSource demo;
    demo.token = "demo";
    demo.place = installs::SetupPlace::installer;
    demo.bundle_count = 1;
    demo.base_rank = 1;
    demo.expansion_count = 0;
    installs::SetupSource base;
    base.token = "base";
    base.place = installs::SetupPlace::installation;
    base.bundle_count = 1;
    base.base_rank = 2;
    base.expansion_count = 0;
    const std::vector<installs::SetupSource> ranked = installs::rank_setup_sources(
        {std::move(loose), std::move(thin), std::move(full), std::move(demo), std::move(base)}
    );
    OA_CHECK(ranked.size() == 4);
    OA_CHECK(ranked[0].token == "setup-full.exe");
    OA_CHECK(ranked[1].token == "setup-thin.exe");
    OA_CHECK(ranked[2].token == "base");
    OA_CHECK(ranked[3].token == "demo");
}

/// Runs the checks.
int main() {
    check_source_words();
    check_locator_places();
    check_search_roots();
    check_library_folders();
    check_manifest();
    check_heroic_list();
    const fs::path temporary =
        fs::temp_directory_path() /
        ("oa-game-installs-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        check_search(temporary);
        check_skipped_folders(temporary);
        check_setup_sources(temporary);
        check_names_outside_code_page(temporary);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "check failed: %s\n", error.what());
        ++oa::test::failed_checks();
    }
    std::error_code error;
    fs::remove_all(temporary, error);
    return oa::test::check_exit_status();
}
