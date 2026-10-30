// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player's own folder in scratch folders: where it is, the folders each
// mod's files go in, where a network game's recording is written, the saved
// games and recordings moved from where earlier versions kept them, names
// kept apart, renames and copies that fail, the records of the moves and the
// notice they give.

#include "oa/app/user_folder.hpp"

#include "oa/app/mod_profile_loader.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using oa::app::FileMoveHooks;
using oa::app::MovesTold;
using oa::app::RecordedMove;
using oa::app::RecordingsMove;
using oa::app::SavesMove;

/// Writes a small file, making its folder.
///
/// @param file the file
/// @param text what it holds
void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

/// Reads a file whole.
///
/// @param file the file
/// @return what it holds; empty when it cannot be read
std::string read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

/// Returns the names a folder holds, sorted.
///
/// @param folder the folder
/// @return the names
std::vector<std::string> names_in(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator entry{folder, error}, end; !error && entry != end;
         entry.increment(error))
        names.push_back(entry->path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

void the_folder_is_chosen_in_order() {
    const fs::path fallback = fs::path("/fallback") / "Open Annihilation";
    oa::platform::preferences::Values values;
    OA_CHECK(oa::app::choose_user_folder(std::nullopt, values, fallback) == fallback);
    // The key holds an absolute folder; a relative one is passed over.
    values[std::string(oa::app::user_folder_preference)] = "relative/folder";
    OA_CHECK(oa::app::choose_user_folder(std::nullopt, values, fallback) == fallback);
    const fs::path stored = fs::current_path().root_path() / "stored" / "files";
    values[std::string(oa::app::user_folder_preference)] = stored.string();
    OA_CHECK(oa::app::choose_user_folder(std::nullopt, values, fallback) == stored);
    // --user-folder wins over both, a relative one taken from the current
    // directory.
    const fs::path option = fs::current_path().root_path() / "option";
    OA_CHECK(oa::app::choose_user_folder(option, values, fallback) == option);
    OA_CHECK(
        oa::app::choose_user_folder(fs::path("here"), values, fallback) ==
        (fs::current_path() / "here").lexically_normal()
    );
    // Beside a --preferences-file.
    const fs::path file = fs::current_path().root_path() / "checks" / "a.conf";
    OA_CHECK(
        oa::app::user_folder_beside(file) ==
        fs::current_path().root_path() / "checks" / "Open Annihilation"
    );
    // Each comes back in its plainest spelling.
    const fs::path spelt = fs::current_path().root_path() / "a" / "b" / ".." / "c" / "";
    OA_CHECK(
        oa::app::choose_user_folder(spelt, values, fallback) ==
        fs::current_path().root_path() / "a" / "c"
    );
    values[std::string(oa::app::user_folder_preference)] = spelt.string();
    OA_CHECK(
        oa::app::choose_user_folder(std::nullopt, values, fallback) ==
        fs::current_path().root_path() / "a" / "c"
    );
}

void each_mod_has_its_own_folders(const fs::path& scratch) {
    const fs::path user = scratch / "layout" / "Open Annihilation";
    // Saves, Screenshots and Films hold a folder for each mod, default
    // without one; a mod whose id is default takes a name no id can.
    OA_CHECK(oa::app::saves_folder(user, {}) == user / "Saves" / "default");
    OA_CHECK(oa::app::saves_folder(user, "my-mod") == user / "Saves" / "my-mod");
    OA_CHECK(oa::app::saves_folder(user, "default") == user / "Saves" / "default (mod)");
    OA_CHECK(oa::app::screenshots_folder(user, "my-mod") == user / "Screenshots" / "my-mod");
    OA_CHECK(oa::app::films_folder(user, {}) == user / "Films" / "default");
    // The captures the game names in the player's own folder, its
    // screenshots folder matched without case, go in the mod's folders.
    const fs::path shot = user / "screenshots" / "SHOT0001.pcx";
    const fs::path frame = user / "MOVIE001" / "FRAM0001.pcx";
    OA_CHECK(
        oa::app::place_capture_path(shot, user, {}) ==
        user / "Screenshots" / "default" / "SHOT0001.pcx"
    );
    OA_CHECK(
        oa::app::place_capture_path(frame, user, {}) ==
        user / "Films" / "default" / "MOVIE001" / "FRAM0001.pcx"
    );
    OA_CHECK(
        oa::app::place_capture_path(user / "SCREENSHOTS", user, "my-mod") ==
        user / "Screenshots" / "my-mod"
    );
    OA_CHECK(
        oa::app::place_capture_path(frame, user, "my-mod") ==
        user / "Films" / "my-mod" / "MOVIE001" / "FRAM0001.pcx"
    );
    OA_CHECK(
        oa::app::place_capture_path(user / "MOVIE*", user, "my-mod") ==
        user / "Films" / "my-mod" / "MOVIE*"
    );
    // A folder mod without an oamod.yaml plays without a profile, so its
    // files go with the game's in default.
    const fs::path plain = scratch / "layout" / "Mods" / "Plain Folder";
    const fs::path game = scratch / "layout" / "game";
    fs::create_directories(plain);
    fs::create_directories(game);
    const auto resolved =
        oa::app::resolve_folder_profile({plain, game}, {plain, {}, false, nullptr});
    OA_CHECK(resolved.errors.empty() && !resolved.profile);
    const std::string id = resolved.profile ? resolved.profile->id : std::string();
    OA_CHECK(oa::app::saves_folder(user, id) == user / "Saves" / "default");
    OA_CHECK(
        oa::app::place_capture_path(shot, user, id) ==
        user / "Screenshots" / "default" / "SHOT0001.pcx"
    );
    // An Image Output Directory the player chose is used as it is, and so
    // is any other path.
    const fs::path chosen = scratch / "layout" / "Pictures" / "screenshots" / "SHOT0001.pcx";
    OA_CHECK(oa::app::place_capture_path(chosen, user, "my-mod") == chosen);
    const fs::path chosen_film = scratch / "layout" / "Pictures" / "MOVIE002";
    OA_CHECK(oa::app::place_capture_path(chosen_film, user, {}) == chosen_film);
    OA_CHECK(
        oa::app::place_capture_path(user / "Saves" / "A.SAV", user, {}) == user / "Saves" / "A.SAV"
    );
    OA_CHECK(oa::app::place_capture_path(shot, {}, {}) == shot);
}

void recordings_go_in_the_folder_of_the_mod_played() {
    const fs::path user = fs::current_path().root_path() / "player" / "Open Annihilation";
    const fs::path name = "2026-10-05 1432 Coast To Coast.tad";
    // Without a mod, in Recordings/default; with one, in its id's folder.
    OA_CHECK(oa::app::recording_file({}, user, {}, name) == user / "Recordings" / "default" / name);
    OA_CHECK(
        oa::app::recording_file({}, user, "my-mod", "duel.tad") ==
        user / "Recordings" / "my-mod" / "duel.tad"
    );
    // A file --net-record names is written as it is given, mod or not.
    const fs::path named = fs::path("games") / "match.tad";
    OA_CHECK(oa::app::recording_file(named, user, "my-mod", name) == named);
    // --user-folder takes the place of the player's own folder.
    const fs::path option = fs::current_path().root_path() / "elsewhere";
    const fs::path chosen =
        oa::app::choose_user_folder(option, oa::platform::preferences::Values{}, user);
    OA_CHECK(
        oa::app::recording_file({}, chosen, {}, name) == option / "Recordings" / "default" / name
    );
}

void the_recordings_move_once_and_overwrite_nothing(const fs::path& scratch) {
    const fs::path root = scratch / "recordings" / "preferences";
    const fs::path user = scratch / "recordings" / "Open Annihilation";
    const fs::path recordings = user / "Recordings" / "default";
    write(root / "demos" / "ONE.tad", "one");
    write(root / "demos" / "two.tad", "two");
    write(root / "mods" / "my-mod" / "demos" / "THREE.rec", "three");
    write(root / "mods" / "my-mod" / "SAVEGAME" / "KEPT.SAV", "kept");
    // A recording of the same name, in another case, is there already.
    write(recordings / "one.tad", "newer one");
    const RecordingsMove move = oa::app::move_earlier_recordings(root, user);
    OA_CHECK(move.moved == 3 && move.renamed == 1 && move.left == 0);
    OA_CHECK(read(recordings / "one.tad") == "newer one");
    OA_CHECK(read(recordings / "ONE (2).tad") == "one");
    OA_CHECK(read(recordings / "two.tad") == "two");
    OA_CHECK(read(user / "Recordings" / "my-mod" / "THREE.rec") == "three");
    // The emptied folders go; the saved games stay for their own move.
    OA_CHECK(!fs::exists(root / "demos") && !fs::exists(root / "mods" / "my-mod" / "demos"));
    OA_CHECK(read(root / "mods" / "my-mod" / "SAVEGAME" / "KEPT.SAV") == "kept");
    OA_CHECK(!fs::exists(user / "Saves"));
    // Recorded, as saves_moved_preference records a move, with no notice.
    oa::platform::preferences::Values values;
    OA_CHECK(!oa::app::recorded_recordings_move(values));
    oa::app::record_recordings_move(values, move);
    OA_CHECK(values.at(std::string(oa::app::recordings_moved_preference)) == "3 0");
    const auto recorded = oa::app::recorded_recordings_move(values);
    OA_CHECK(recorded && recorded->moved == 3 && recorded->left == 0);
    OA_CHECK(!oa::app::saves_notice_due_in(values));
    // Again, there is nothing left to move.
    const RecordingsMove again = oa::app::move_earlier_recordings(root, user);
    OA_CHECK(again.moved == 0 && again.left == 0 && again.lines.empty());
}

void free_names_keep_both_files() {
    std::vector<std::string> taken{"GAME.SAV", "GAME (2).SAV", "LIST.LST"};
    OA_CHECK(oa::app::free_file_name("other.sav", taken) == "other.sav");
    OA_CHECK(oa::app::free_file_name("game.sav", taken) == "game (3).sav");
    OA_CHECK(oa::app::free_file_name("List.lst", taken) == "List (2).lst");
    taken.push_back("README");
    OA_CHECK(oa::app::free_file_name("readme", taken) == "readme (2)");
}

void the_saved_games_move_once_and_overwrite_nothing(const fs::path& scratch) {
    const fs::path root = scratch / "moved" / "preferences";
    const fs::path user = scratch / "moved" / "Open Annihilation";
    write(root / "SAVEGAME" / "ONE.SAV", "one");
    write(root / "SAVEGAME" / "TWO.SAV", "two");
    write(root / "SAVEGAME" / "UNITS.LST", "list");
    write(root / "mods" / "my-mod" / "SAVEGAME" / "THREE.SAV", "three");
    write(root / "mods" / "empty-mod" / "SAVEGAME" / ".keep-folder" / "x", "x");
    write(root / "preferences.conf", "kept");
    // A save of the same name, in another case, is there already.
    const fs::path saves = user / "Saves" / "default";
    write(saves / "one.sav", "newer one");
    const SavesMove move = oa::app::move_earlier_saves(root, user);
    OA_CHECK(move.moved == 3 && move.renamed == 1 && move.left == 0 && move.other_files == 1);
    OA_CHECK(read(saves / "one.sav") == "newer one");
    OA_CHECK(read(saves / "ONE (2).SAV") == "one");
    OA_CHECK(read(saves / "TWO.SAV") == "two");
    OA_CHECK(read(saves / "UNITS.LST") == "list");
    OA_CHECK(read(user / "Saves" / "my-mod" / "THREE.SAV") == "three");
    // The emptied folders go; a folder holding a folder stays; the
    // preferences stay where they are.
    OA_CHECK(!fs::exists(root / "SAVEGAME"));
    OA_CHECK(!fs::exists(root / "mods" / "my-mod" / "SAVEGAME"));
    OA_CHECK(fs::exists(root / "mods" / "empty-mod" / "SAVEGAME" / ".keep-folder"));
    OA_CHECK(read(root / "preferences.conf") == "kept");
    // It says what happened: the renamed file and each folder moved.
    const auto said = [&move](const std::string& text) {
        return std::any_of(move.lines.begin(), move.lines.end(), [&text](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    };
    OA_CHECK(said("ONE (2).SAV") && said("one.sav was there already"));
    OA_CHECK(said("moved 3 files from"));
    OA_CHECK(said("moved 1 file from"));
    // Again, there is nothing left to move.
    const SavesMove again = oa::app::move_earlier_saves(root, user);
    OA_CHECK(again.moved == 0 && again.left == 0 && again.lines.empty());
    // A root with neither folder moves nothing.
    const SavesMove none = oa::app::move_earlier_saves(scratch / "nothing", user);
    OA_CHECK(none.moved == 0 && none.left == 0);
}

void the_saved_games_loose_in_saves_move_into_default(const fs::path& scratch) {
    const fs::path user = scratch / "loose" / "Open Annihilation";
    const fs::path saves = user / "Saves";
    // A Saves that holds only folders moves nothing and stays.
    write(saves / "my-mod" / "MINE.SAV", "mine");
    const SavesMove nothing = oa::app::move_loose_saves(user);
    OA_CHECK(nothing.moved == 0 && nothing.other_files == 0 && nothing.lines.empty());
    OA_CHECK(!oa::app::holds_a_file(saves) && !fs::exists(saves / "default"));
    // 3.1c's saved games and their lists move into default, one named as a
    // saved game there kept beside it; the mods' folders stay as they are.
    write(saves / "OLD.SAV", "old");
    write(saves / "UNITS.LST", "list");
    write(saves / "default" / "old.sav", "newer");
    const SavesMove move = oa::app::move_loose_saves(user);
    OA_CHECK(move.moved == 1 && move.renamed == 1 && move.left == 0 && move.other_files == 1);
    OA_CHECK(read(saves / "default" / "old.sav") == "newer");
    OA_CHECK(read(saves / "default" / "OLD (2).SAV") == "old");
    OA_CHECK(read(saves / "default" / "UNITS.LST") == "list");
    OA_CHECK(read(saves / "my-mod" / "MINE.SAV") == "mine");
    OA_CHECK(!oa::app::holds_a_file(saves) && fs::is_directory(saves));
    // One that cannot move stays where it is.
    write(saves / "STUCK.SAV", "stuck");
    FileMoveHooks stuck{};
    stuck.rename = [](void*, const fs::path&, const fs::path&, std::error_code& error) {
        error = std::make_error_code(std::errc::cross_device_link);
    };
    stuck.copy = [](void*, const fs::path&, const fs::path&, std::error_code& error) {
        error = std::make_error_code(std::errc::no_space_on_device);
    };
    const SavesMove left = oa::app::move_loose_saves(user, stuck);
    OA_CHECK(left.moved == 0 && left.left == 1);
    OA_CHECK(read(saves / "STUCK.SAV") == "stuck");
}

/// Refuses every rename, as across volumes.
void refuse_rename(void*, const fs::path&, const fs::path&, std::error_code& error) {
    error = std::make_error_code(std::errc::cross_device_link);
}

/// Refuses every copy, as on a full disk, leaving part of the file.
void refuse_copy(void*, const fs::path&, const fs::path& to, std::error_code& error) {
    std::ofstream(to, std::ios::binary) << "par";
    error = std::make_error_code(std::errc::no_space_on_device);
}

void a_refused_rename_copies_and_a_refused_copy_leaves_the_file(const fs::path& scratch) {
    const fs::path root = scratch / "refused" / "preferences";
    const fs::path user = scratch / "refused" / "user";
    write(root / "SAVEGAME" / "FAR.SAV", "far away");
    FileMoveHooks across{};
    across.rename = refuse_rename;
    const SavesMove copied = oa::app::move_earlier_saves(root, user, across);
    OA_CHECK(copied.moved == 1 && copied.left == 0);
    OA_CHECK(read(user / "Saves" / "default" / "FAR.SAV") == "far away");
    OA_CHECK(!fs::exists(root / "SAVEGAME" / "FAR.SAV"));

    write(root / "SAVEGAME" / "STUCK.SAV", "stuck");
    write(root / "SAVEGAME" / "STUCK.LST", "list");
    FileMoveHooks stuck{};
    stuck.rename = refuse_rename;
    stuck.copy = refuse_copy;
    const SavesMove left = oa::app::move_earlier_saves(root, user, stuck);
    OA_CHECK(left.moved == 0 && left.left == 1 && left.other_files == 0);
    // Nothing is lost or half there: the originals stay, no part copy.
    OA_CHECK(read(root / "SAVEGAME" / "STUCK.SAV") == "stuck");
    OA_CHECK(read(root / "SAVEGAME" / "STUCK.LST") == "list");
    OA_CHECK(!fs::exists(user / "Saves" / "default" / "STUCK.SAV"));
    OA_CHECK(!fs::exists(user / "Saves" / "default" / "STUCK.LST"));
    OA_CHECK(names_in(user / "Saves" / "default") == std::vector<std::string>{"FAR.SAV"});
    OA_CHECK(std::any_of(left.lines.begin(), left.lines.end(), [](const std::string& line) {
        return line.find("lists it where it is") != std::string::npos;
    }));

    // A Saves "folder" that is a file leaves everything where it is.
    const fs::path blocked = scratch / "blocked";
    write(blocked / "preferences" / "SAVEGAME" / "A.SAV", "a");
    write(blocked / "user" / "Saves", "not a folder");
    const SavesMove refused =
        oa::app::move_earlier_saves(blocked / "preferences", blocked / "user");
    OA_CHECK(refused.moved == 0 && refused.left == 1);
    OA_CHECK(read(blocked / "preferences" / "SAVEGAME" / "A.SAV") == "a");
}

void the_move_is_recorded_and_its_notice_waits() {
    oa::platform::preferences::Values values;
    OA_CHECK(!oa::app::recorded_saves_move(values));
    OA_CHECK(!oa::app::saves_notice_due_in(values));
    SavesMove nothing;
    oa::app::record_saves_move(values, nothing);
    const auto recorded = oa::app::recorded_saves_move(values);
    OA_CHECK(recorded && recorded->moved == 0 && recorded->left == 0);
    // Nothing moved or left: no notice.
    OA_CHECK(!values.contains(std::string(oa::app::saves_notice_preference)));
    SavesMove some;
    some.moved = 12;
    some.left = 2;
    oa::app::record_saves_move(values, some);
    OA_CHECK(values.at(std::string(oa::app::saves_moved_preference)) == "12 2");
    OA_CHECK(oa::app::saves_notice_due_in(values));
    const auto counts = oa::app::recorded_saves_move(values);
    OA_CHECK(counts && counts->moved == 12 && counts->left == 2);
    // The move of those loose in Saves is recorded and waits apart; both
    // notices are told at once.
    SavesMove loose;
    loose.moved = 4;
    oa::app::record_loose_saves_move(values, loose);
    OA_CHECK(values.at(std::string(oa::app::loose_saves_moved_preference)) == "4 0");
    const auto told = oa::app::moves_to_tell(values);
    OA_CHECK(told.beside_preferences && told.beside_preferences->moved == 12);
    OA_CHECK(told.loose_in_saves && told.loose_in_saves->moved == 4);
    oa::app::record_saves_notice_told(values);
    OA_CHECK(!oa::app::saves_notice_due_in(values));
    OA_CHECK(
        values.at(std::string(oa::app::loose_saves_notice_preference)) == oa::app::saves_notice_told
    );
    const auto none = oa::app::moves_to_tell(values);
    OA_CHECK(!none.beside_preferences && !none.loose_in_saves);
    for (const char* bad : {"", "12", "12 x", "x 2", "12 2 3", "-1 2"}) {
        values[std::string(oa::app::saves_moved_preference)] = bad;
        OA_CHECK(!oa::app::recorded_saves_move(values));
    }
}

void the_notice_says_what_moved_and_where() {
    const fs::path saves = fs::path("/home/player/Documents/Open Annihilation/Saves");
    const auto notice = oa::app::saves_moved_notice(MovesTold{RecordedMove{12, 0}, {}}, saves);
    OA_CHECK(notice.title == "SAVED GAMES MOVED" && notice.open_caption == "Open folder");
    OA_CHECK(notice.paragraphs.size() == 3);
    OA_CHECK(notice.paragraphs[0].text == "12 saved games have moved to:");
    OA_CHECK(notice.paragraphs[1].path && notice.paragraphs[1].text == saves.string());
    OA_CHECK(
        notice.paragraphs[2].text ==
        "Screenshots, films and mods now go in the same Open Annihilation folder."
    );
    const auto one = oa::app::saves_moved_notice(MovesTold{RecordedMove{1, 1}, {}}, saves);
    OA_CHECK(one.paragraphs.size() == 4);
    OA_CHECK(one.paragraphs[0].text == "1 saved game has moved to:");
    OA_CHECK(
        one.paragraphs[2].text ==
        "1 saved game could not be moved, so the game lists it where it is. The log says why."
    );
    const auto stuck = oa::app::saves_moved_notice(MovesTold{RecordedMove{0, 3}, {}}, saves);
    OA_CHECK(stuck.paragraphs.size() == 4);
    OA_CHECK(stuck.paragraphs[0].text == "New saved games go in:");
    OA_CHECK(stuck.paragraphs[1].path && stuck.paragraphs[1].text == saves.string());
    OA_CHECK(
        stuck.paragraphs[2].text ==
        "3 saved games could not be moved, so the game lists them where they are. The log says "
        "why."
    );
    // The move of those loose in Saves says where each mod's go; told with
    // the other, the counts add up and both say theirs.
    const auto loose = oa::app::saves_moved_notice(MovesTold{{}, RecordedMove{2, 0}}, saves);
    OA_CHECK(loose.paragraphs.size() == 3);
    OA_CHECK(loose.paragraphs[0].text == "2 saved games have moved to:");
    OA_CHECK(
        loose.paragraphs[2].text ==
        "Saved games, screenshots and films now go in a folder for each mod, or in default "
        "without a mod."
    );
    const auto both =
        oa::app::saves_moved_notice(MovesTold{RecordedMove{1, 0}, RecordedMove{1, 1}}, saves);
    OA_CHECK(both.paragraphs.size() == 5);
    OA_CHECK(both.paragraphs[0].text == "2 saved games have moved to:");
    OA_CHECK(both.paragraphs[2].text.starts_with("1 saved game could not be moved"));
    OA_CHECK(both.paragraphs[3].text.starts_with("Screenshots, films and mods"));
    OA_CHECK(both.paragraphs[4].text.starts_with("Saved games, screenshots and films"));
}

void a_mod_without_its_units_cannot_start() {
    using oa::app::SideCommander;
    const std::vector<SideCommander> sides{{"Red", "REDCOM"}, {"Blue", "BLUECOM"}};
    // Every commander among the units, matched without case: nothing is missing.
    const auto whole = oa::app::find_mod_start_gaps({"redcom", "BLUECOM", "REDTANK"}, sides);
    OA_CHECK(!whole.any());
    // A side whose commander is not among them.
    const auto one = oa::app::find_mod_start_gaps({"REDCOM", "REDTANK"}, sides);
    OA_CHECK(one.any() && !one.no_units && one.missing_commanders.size() == 1);
    OA_CHECK(one.missing_commanders[0].commander == "BLUECOM");
    // No units at all: every side's commander is missing too.
    const auto none = oa::app::find_mod_start_gaps({}, sides);
    OA_CHECK(none.no_units && none.missing_commanders.size() == 2);
    // A side that names no commander has none for the files to miss.
    const auto unnamed = oa::app::find_mod_start_gaps({"REDCOM"}, {{"Grey", ""}});
    OA_CHECK(!unnamed.any());

    const fs::path folder = fs::path("/home/player/Documents/Open Annihilation/Mods/Made Up/");
    const auto notice = oa::app::mod_files_missing_notice("Made Up", none, folder);
    OA_CHECK(notice.title == "MOD FILES MISSING" && notice.open_caption == "Open mod folder");
    OA_CHECK(notice.paragraphs.size() == 6);
    OA_CHECK(notice.paragraphs[0].text == "Made Up is missing files.");
    OA_CHECK(notice.paragraphs[1].text == "None of this mod's units were found.");
    OA_CHECK(notice.paragraphs[2].text == "The Red commander (REDCOM) isn't in this mod's units.");
    OA_CHECK(
        notice.paragraphs[3].text == "The Blue commander (BLUECOM) isn't in this mod's units."
    );
    OA_CHECK(
        notice.paragraphs[4].text ==
        "Its games can't start until the mod's files are added to its folder:"
    );
    // The folder's path whole, in the system's separators and without its
    // last one, for the notice to wrap.
    OA_CHECK(notice.paragraphs[5].path);
    OA_CHECK(
        notice.paragraphs[5].text ==
        fs::path("/home/player/Documents/Open Annihilation/Mods/Made Up").make_preferred().string()
    );
    const auto nameless = oa::app::mod_files_missing_notice("", one, folder);
    OA_CHECK(nameless.paragraphs.size() == 4);
    OA_CHECK(nameless.paragraphs[0].text == "This mod is missing files.");
    OA_CHECK(
        nameless.paragraphs[1].text == "The Blue commander (BLUECOM) isn't in this mod's units."
    );
    // A file a side names that the mod's files lack is named after the
    // commanders; games show without it, and it alone keeps none from
    // starting.
    oa::app::ModStartGaps art = one;
    art.missing_side_files.push_back({"Blue", "anims/BLUEINT.GAF"});
    OA_CHECK(art.games_cannot_start());
    const auto art_notice = oa::app::mod_files_missing_notice("Made Up", art, folder);
    OA_CHECK(art_notice.paragraphs.size() == 5);
    OA_CHECK(
        art_notice.paragraphs[1].text == "The Blue commander (BLUECOM) isn't in this mod's units."
    );
    OA_CHECK(
        art_notice.paragraphs[2].text ==
        "The Blue side's anims/BLUEINT.GAF isn't in this mod's files; games show without it."
    );
    OA_CHECK(
        art_notice.paragraphs[3].text ==
        "Its games can't start until the mod's files are added to its folder:"
    );
    oa::app::ModStartGaps art_alone;
    art_alone.missing_side_files = art.missing_side_files;
    art_alone.missing_side_files.push_back({"Blue", "fonts/BLUEFONT.FNT"});
    OA_CHECK(art_alone.any() && !art_alone.games_cannot_start());
    const auto alone_notice = oa::app::mod_files_missing_notice("", art_alone, folder);
    OA_CHECK(alone_notice.title == "MOD FILES MISSING");
    OA_CHECK(alone_notice.paragraphs.size() == 5);
    OA_CHECK(alone_notice.paragraphs[0].text == "This mod is missing files.");
    OA_CHECK(
        alone_notice.paragraphs[2].text ==
        "The Blue side's fonts/BLUEFONT.FNT isn't in this mod's files; games show without it."
    );
    OA_CHECK(
        alone_notice.paragraphs[3].text ==
        "Its games still start. Add the missing files to its folder to show them:"
    );
    OA_CHECK(alone_notice.paragraphs[4].path);
}

void folders_open_through_the_hooks(const fs::path& scratch) {
    // A missing folder is made, then shown; the record keeps each request.
    std::vector<fs::path> requests;
    const auto recorded = oa::app::recorded_folder_opener(requests);
    const fs::path missing = scratch / "open" / "Open Annihilation" / "Saves";
    const auto shown = oa::app::open_folder(recorded, missing);
    OA_CHECK(shown.opened && shown.reason.empty());
    OA_CHECK(fs::is_directory(missing));
    OA_CHECK(requests == std::vector<fs::path>{missing});
    // A file where the folder should be cannot be made into one, and
    // nothing is asked of the hooks.
    write(scratch / "open" / "Films", "a file");
    const auto refused = oa::app::open_folder(recorded, scratch / "open" / "Films");
    OA_CHECK(!refused.opened && refused.reason == oa::app::folder_not_made_text);
    OA_CHECK(!refused.detail.empty() && requests.size() == 1);
    // Without an opener, the file manager fails.
    const auto none = oa::app::open_folder(oa::app::FolderOpenerHooks{}, missing);
    OA_CHECK(!none.opened && none.reason == oa::app::file_manager_failed_text);
}

void a_folder_has_a_file_uri() {
    // Letters, digits, '-', '.', '_', '~', '/' and ':' stay; every other byte,
    // UTF-8 included, is written as '%' and two capital hexadecimal digits.
    OA_CHECK(
        oa::app::file_uri(fs::path("/home/player/Documents/Open Annihilation/Saves")) ==
        "file:///home/player/Documents/Open%20Annihilation/Saves"
    );
    const std::string accented = "/home/Jos\xc3\xa9/50% 'off'#";
    OA_CHECK(
        oa::app::file_uri(fs::path(std::u8string(accented.begin(), accented.end()))) ==
        "file:///home/Jos%C3%A9/50%25%20%27off%27%23"
    );
#ifdef _WIN32
    OA_CHECK(oa::app::file_uri(fs::path(L"C:\\Users\\A B")) == "file:///C:/Users/A%20B");
#endif
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-user-folder");
    the_folder_is_chosen_in_order();
    each_mod_has_its_own_folders(scratch);
    recordings_go_in_the_folder_of_the_mod_played();
    free_names_keep_both_files();
    the_saved_games_move_once_and_overwrite_nothing(scratch);
    the_saved_games_loose_in_saves_move_into_default(scratch);
    the_recordings_move_once_and_overwrite_nothing(scratch);
    a_refused_rename_copies_and_a_refused_copy_leaves_the_file(scratch);
    the_move_is_recorded_and_its_notice_waits();
    the_notice_says_what_moved_and_where();
    a_mod_without_its_units_cannot_start();
    folders_open_through_the_hooks(scratch);
    a_folder_has_a_file_uri();
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return oa::test::check_exit_status();
}
