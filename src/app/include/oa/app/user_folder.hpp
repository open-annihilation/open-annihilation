// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player's own folder, "Open Annihilation" in their Documents folder:
// where it is, the folders it holds (Saves, Screenshots, Films, Recordings
// and Cache, each of the first four with a folder for each mod, and Mods),
// the one-time moves of saved games from where earlier versions kept them,
// beside the preferences file and
// loose in Saves, and of recordings from beside the preferences file, and the
// main menu's notice of the saved games' moves; and the warning, in the same
// look, that a mod's games cannot start until its files are in its folder.
// Nothing here reads the clock or SDL; the runtime does the rest.
#pragma once

#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/notice.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {

/// The folder of the player's own folder that holds the saved games, in a
/// folder for each mod (mod_subfolder_name).
inline constexpr std::string_view saves_folder_name = "Saves";
/// The folder of the player's own folder screenshots and posters go in
/// while no Image Output Directory is set, in a folder for each mod.
inline constexpr std::string_view screenshots_folder_name = "Screenshots";
/// The folder of the player's own folder film captures go in while no
/// Image Output Directory is set, in a folder for each mod.
inline constexpr std::string_view films_folder_name = "Films";
/// The folder of the player's own folder the recordings of network games go
/// in, in a folder for each mod, unless --net-record names the file.
inline constexpr std::string_view recordings_folder_name = "Recordings";
/// The folder of Saves, Screenshots, Films and Recordings that holds the
/// files of games played without a mod's profile: 3.1c's, and a mod
/// folder's that holds no oamod.yaml.
inline constexpr std::string_view no_mod_folder_name = "default";
/// The folder of Saves, Screenshots, Films and Recordings that holds the
/// files of a mod whose id is no_mod_folder_name, a name no id can take,
/// since an id is kebab-case.
inline constexpr std::string_view default_id_folder_name = "default (mod)";
/// The folder of the player's own folder whose mod folders the Mod setting
/// offers besides the game folder's mods folder.
inline constexpr std::string_view user_mods_folder_name = "Mods";
/// The folder of the player's own folder that holds remembered mutator
/// verdicts. Mutators themselves stay in the Mutators folder.
inline constexpr std::string_view cache_folder_name = "Cache";

/// The folder below the preferences file's folder, and below each mods/<id>
/// folder there, that held saved games before they moved to Saves.
inline constexpr std::string_view earlier_saves_folder_name = "SAVEGAME";
/// The folder below the preferences file's folder, and below each mods/<id>
/// folder there, that held recordings before they moved to Recordings.
inline constexpr std::string_view earlier_recordings_folder_name = "demos";
/// The folder below the preferences file's folder that held each mod's
/// earlier save and recordings folders, in a folder named after the mod's
/// id.
inline constexpr std::string_view earlier_mods_folder_name = "mods";
/// The extension of a saved game, matched without case.
inline constexpr std::string_view saved_game_extension = ".SAV";
/// What the name of a file being copied into Saves or Recordings ends with
/// until the copy is whole; a copy cut short keeps it, and no dialog lists
/// it.
inline constexpr std::string_view partial_copy_suffix = ".moving";

/// The preference that moves the player's own folder, as an absolute UTF-8
/// path; absent or not absolute for the default.
inline constexpr std::string_view user_folder_preference = "open-annihilation.user-folder";
/// The preference that records that the saved games were moved, so that the
/// move never runs again: the saved games moved and the ones left where
/// they were, as two decimal numbers, "12 0".
inline constexpr std::string_view saves_moved_preference = "open-annihilation.saves-moved";
/// The preference that records the main menu's notice of the move:
/// saves_notice_due until a run someone watches has shown it, then
/// saves_notice_told; absent when the move moved and left nothing.
inline constexpr std::string_view saves_notice_preference = "open-annihilation.saves-moved-notice";
/// saves_notice_preference's value while the notice waits to be shown.
inline constexpr std::string_view saves_notice_due = "due";
/// saves_notice_preference's value once the notice has shown.
inline constexpr std::string_view saves_notice_told = "told";
/// The preference that records that the saved games of 3.1c kept loose in
/// Saves were moved into its no_mod_folder_name folder, so that the move
/// never runs again: the saved games moved and the ones left where they
/// were, as saves_moved_preference holds them.
inline constexpr std::string_view loose_saves_moved_preference =
    "open-annihilation.loose-saves-moved";
/// The preference that records the main menu's notice of that move, as
/// saves_notice_preference records the notice of the move from beside the
/// preferences file.
inline constexpr std::string_view loose_saves_notice_preference =
    "open-annihilation.loose-saves-moved-notice";
/// The preference that records that the recordings kept beside the
/// preferences file were moved into Recordings, so that the move never runs
/// again: the recordings moved and the ones left where they were, as
/// saves_moved_preference holds them. No notice follows it.
inline constexpr std::string_view recordings_moved_preference =
    "open-annihilation.recordings-moved";

/// The folder a run with --preferences-file keeps as the player's own,
/// beside that file, unless --user-folder or the file's own key names one,
/// so that a check never reaches the player's Documents folder.
///
/// @param preferences_file the --preferences-file
/// @return the folder named user_folder_name in the file's folder
[[nodiscard]] std::filesystem::path
user_folder_beside(const std::filesystem::path& preferences_file);

/// Picks the player's own folder: --user-folder, else the preferences' key
/// while it holds an absolute path, else `fallback`. A relative
/// --user-folder is taken from the current directory.
///
/// @param option the --user-folder value, when given
/// @param values the loaded preferences
/// @param fallback the default folder (default_user_folder, or
///        user_folder_beside with --preferences-file)
/// @return the folder, normalised and without a separator at its end,
///     absolute unless `fallback` is not; it need not exist
[[nodiscard]] std::filesystem::path choose_user_folder(
    const std::optional<std::filesystem::path>& option,
    const platform::preferences::Values& values,
    const std::filesystem::path& fallback
);

/// Returns the player's own folder a run uses, as the runtime chooses it at
/// its start: --user-folder, else the preferences' key, else beside a named
/// preferences file, else the Documents folder, else beside the preferences
/// file the run reads when there is no Documents folder.
///
/// @param user_folder_option the --user-folder value, when given
/// @param preferences_file_option the --preferences-file value, when given
/// @param preference_path the preferences file the run reads
/// @param values the loaded preferences
/// @param[out] note why the Documents folder was not used, for the log;
///        left empty when it was, or not needed
/// @return the folder, as choose_user_folder gives it
[[nodiscard]] std::filesystem::path own_user_folder(
    const std::optional<std::filesystem::path>& user_folder_option,
    const std::optional<std::filesystem::path>& preferences_file_option,
    const std::filesystem::path& preference_path,
    const platform::preferences::Values& values,
    std::string& note
);

/// Whether the player's own folder can be read.
enum class UserFolderAccess : uint8_t {
    missing,  ///< there is no folder yet: a first start
    readable, ///< the folder is there and can be listed
    refused,  ///< the system refuses to let this process look at the folder
};

/// Tells whether the player's own folder can be read.
///
/// A folder the system will not let this process look at or list, for want
/// of permission, is refused, not missing: macOS refuses the Documents
/// folder to a copy of the game that was not opened from the Finder or
/// Applications until the player allows it. A folder that is not there is
/// missing. A folder that can be looked at but fails to list for another
/// reason counts as readable, and the start reports that failure itself.
///
/// @param folder the player's own folder
/// @return what the start may take it for
[[nodiscard]] UserFolderAccess user_folder_access(const std::filesystem::path& folder);

/// Returns the message a start shows, and stops with, when the player's own
/// folder is refused: the folder, that the player's files are still there,
/// and how to let the game read it.
///
/// @param folder the player's own folder
/// @return the message, in English
[[nodiscard]] std::string refused_user_folder_message(const std::filesystem::path& folder);

/// Returns the name of the folder of Saves, Screenshots, Films and
/// Recordings that holds the files of games played with a mod, or without
/// one.
///
/// @param mod_id the id of the mod's profile; empty without one
/// @return the id; no_mod_folder_name without one, and
///     default_id_folder_name for the id no_mod_folder_name
[[nodiscard]] std::string mod_subfolder_name(std::string_view mod_id);

/// Returns the folder the saved games of a game or a mod are kept in.
///
/// @param user_folder the player's own folder
/// @param mod_id the id of the mod's profile; empty without one
/// @return Saves/<mod_subfolder_name(mod_id)>: Saves/default without a mod
[[nodiscard]] std::filesystem::path
saves_folder(const std::filesystem::path& user_folder, std::string_view mod_id);

/// Returns the folder the screenshots and posters of a game or a mod go in
/// while no Image Output Directory is set.
///
/// @param user_folder the player's own folder
/// @param mod_id the id of the mod's profile; empty without one
/// @return Screenshots/<mod_subfolder_name(mod_id)>
[[nodiscard]] std::filesystem::path
screenshots_folder(const std::filesystem::path& user_folder, std::string_view mod_id);

/// Returns the folder that holds the MOVIEnnn folders of a game's or a mod's
/// films while no Image Output Directory is set.
///
/// @param user_folder the player's own folder
/// @param mod_id the id of the mod's profile; empty without one
/// @return Films/<mod_subfolder_name(mod_id)>
[[nodiscard]] std::filesystem::path
films_folder(const std::filesystem::path& user_folder, std::string_view mod_id);

/// Returns the folder the recordings of a game's or a mod's network games
/// are written to, unless --net-record names the file.
///
/// @param user_folder the player's own folder
/// @param mod_id the id of the mod's profile; empty without one
/// @return Recordings/<mod_subfolder_name(mod_id)>: Recordings/default
///     without a mod
[[nodiscard]] std::filesystem::path
recordings_folder(const std::filesystem::path& user_folder, std::string_view mod_id);

/// Returns the file a network game's recording is written to: the file
/// --net-record names, as it is given, else the file of that name in the
/// folder of the mod played in Recordings (recordings_folder).
///
/// @param net_record the --net-record file; empty when none is given
/// @param user_folder the player's own folder
/// @param mod_id the id of the mod's profile; empty without one
/// @param name the recording's file name: .record's, or the date, time and
///        map's, with the recorder's extension
/// @return the file
[[nodiscard]] std::filesystem::path recording_file(
    const std::filesystem::path& net_record,
    const std::filesystem::path& user_folder,
    std::string_view mod_id,
    const std::filesystem::path& name
);

/// Places a path in the player's own folder, which stands for the Image
/// Output Directory while none is set: its screenshots folder, SCREENSHOTS
/// matched without case, is screenshots_folder, and a MOVIEnnn folder, or
/// the MOVIE* of a search for them, lies in films_folder. Any other path,
/// and a path outside the player's own folder, such as one in an Image
/// Output Directory the player chose, stays as it is.
///
/// @param path the path, absolute
/// @param user_folder the player's own folder; empty places nothing
/// @param mod_id the id of the mod's profile; empty without one
/// @return the path placed
[[nodiscard]] std::filesystem::path place_capture_path(
    const std::filesystem::path& path,
    const std::filesystem::path& user_folder,
    std::string_view mod_id
);

/// Tells whether a folder holds a file, the folders within it apart.
///
/// @param folder the folder
/// @return true when it holds one; false when it is missing or cannot be listed
[[nodiscard]] bool holds_a_file(const std::filesystem::path& folder);

/// Finds a folder's entry whose name matches, without case.
///
/// @param folder the folder
/// @param name the name
/// @return the entry's path; nullopt when there is none or the folder cannot be listed
[[nodiscard]] std::optional<std::filesystem::path>
entry_without_case(const std::filesystem::path& folder, std::string_view name);

/// Ways a move of files may fail, which the tests stand in for; each null
/// member uses std::filesystem.
struct FileMoveHooks {
    void* context{};
    /// Renames `from` to `to`; null uses std::filesystem::rename. Sets
    /// `error` when it fails.
    void (*rename)(
        void* context,
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error
    ){};
    /// Copies `from` to the new file `to`, overwriting nothing; null uses
    /// std::filesystem::copy_file. Sets `error` when it fails.
    void (*copy)(
        void* context,
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error
    ){};
};

/// What the move of the saved games did.
struct SavesMove {
    std::size_t moved{};       ///< saved games now in a Saves folder
    std::size_t renamed{};     ///< of those, the ones kept under another name, theirs taken
    std::size_t left{};        ///< saved games that could not move and stay where they were
    std::size_t other_files{}; ///< other files moved with them, such as restriction lists
    /// What happened, a line each, for standard error and the log.
    std::vector<std::string> lines;
};

/// Returns a name for a file that takes no name a folder holds, matched
/// without case: the name itself when it is free, else the name with " (2)",
/// " (3)" and so on before its extension.
///
/// @param name the file's name
/// @param taken the names the folder holds, and those already given, in
///        capitals (ASCII letters raised)
/// @return the free name
[[nodiscard]] std::string
free_file_name(const std::string& name, const std::vector<std::string>& taken);

/// Moves the files of an earlier saves folder into a Saves folder, never
/// overwriting one: a name the Saves folder holds already, matched without
/// case, is kept for the file there, and the file moved takes a free name
/// (free_file_name). A file is renamed into place; where that fails, as
/// across volumes, it is copied under its name and partial_copy_suffix,
/// renamed into place once the copy is whole, and the original removed;
/// where that fails too, it stays where it is, and the game keeps listing
/// that folder. The earlier folder is removed once it is empty. Folders
/// within it are left.
///
/// @param earlier the earlier folder, SAVEGAME
/// @param saves the Saves folder; made when missing
/// @param[in,out] move what the move did, added to
/// @param hooks stand-ins for the file system's rename and copy
void move_saves_folder(
    const std::filesystem::path& earlier,
    const std::filesystem::path& saves,
    SavesMove& move,
    const FileMoveHooks& hooks = {}
);

/// Moves the saved games from where earlier versions kept them, beside the
/// preferences file: <root>/SAVEGAME into saves_folder(user_folder, ""),
/// and <root>/mods/<id>/SAVEGAME into saves_folder(user_folder, id) for
/// each mod (move_saves_folder). The names are matched without case.
///
/// @param earlier_root the folder that held them: the preferences file's
/// @param user_folder the player's own folder
/// @param hooks stand-ins for the file system's rename and copy
/// @return what the move did
[[nodiscard]] SavesMove move_earlier_saves(
    const std::filesystem::path& earlier_root,
    const std::filesystem::path& user_folder,
    const FileMoveHooks& hooks = {}
);

/// Moves the files of 3.1c's saved games that lie loose in Saves, where
/// earlier versions kept them, into saves_folder(user_folder, "")
/// (move_saves_folder): never overwriting one, and leaving where it is one
/// that cannot move, which the dialogs still list. Saves stays, and so do
/// the folders it holds.
///
/// @param user_folder the player's own folder
/// @param hooks stand-ins for the file system's rename and copy
/// @return what the move did; nothing when Saves holds no file
[[nodiscard]] SavesMove
move_loose_saves(const std::filesystem::path& user_folder, const FileMoveHooks& hooks = {});

/// What the move of the recordings did. Every file of an earlier recordings
/// folder counts as a recording, whatever its extension, since a mod's
/// profile names its own.
struct RecordingsMove {
    std::size_t moved{};   ///< recordings now in a Recordings folder
    std::size_t renamed{}; ///< of those, the ones kept under another name, theirs taken
    std::size_t left{};    ///< recordings that could not move and stay where they were
    /// What happened, a line each, for standard error and the log.
    std::vector<std::string> lines;
};

/// Moves the recordings from where earlier versions kept them, beside the
/// preferences file: <root>/demos into recordings_folder(user_folder, ""),
/// and <root>/mods/<id>/demos into recordings_folder(user_folder, id) for
/// each mod, as move_saves_folder moves saved games: never overwriting one,
/// a name taken kept for the file there and the file moved given a free
/// one, and one that cannot move left where it is. The names are matched
/// without case.
///
/// @param earlier_root the folder that held them: the preferences file's
/// @param user_folder the player's own folder
/// @param hooks stand-ins for the file system's rename and copy
/// @return what the move did
[[nodiscard]] RecordingsMove move_earlier_recordings(
    const std::filesystem::path& earlier_root,
    const std::filesystem::path& user_folder,
    const FileMoveHooks& hooks = {}
);

/// Records a move in the preferences: saves_moved_preference, and the notice
/// due when it moved or left a saved game.
///
/// @param[in,out] values the preferences
/// @param move what the move did
void record_saves_move(platform::preferences::Values& values, const SavesMove& move);

/// Records the move of the saved games loose in Saves (move_loose_saves) in
/// the preferences: loose_saves_moved_preference, and its notice due
/// (loose_saves_notice_preference) when it moved or left a saved game.
///
/// @param[in,out] values the preferences
/// @param move what the move did
void record_loose_saves_move(platform::preferences::Values& values, const SavesMove& move);

/// Records the move of the recordings (move_earlier_recordings) in the
/// preferences: recordings_moved_preference.
///
/// @param[in,out] values the preferences
/// @param move what the move did
void record_recordings_move(platform::preferences::Values& values, const RecordingsMove& move);

/// The counts a recorded move keeps.
struct RecordedMove {
    std::size_t moved{}; ///< saved games, or recordings, moved
    std::size_t left{};  ///< saved games, or recordings, left where they were
};

/// Reads what the recorded move from beside the preferences file moved and
/// left.
///
/// @param values the preferences
/// @return the counts; nullopt when no move is recorded, or its value
///         cannot be read
[[nodiscard]] std::optional<RecordedMove>
recorded_saves_move(const platform::preferences::Values& values);

/// Reads what the recorded move of the saved games loose in Saves moved and
/// left.
///
/// @param values the preferences
/// @return the counts; nullopt when no move is recorded, or its value
///         cannot be read
[[nodiscard]] std::optional<RecordedMove>
recorded_loose_saves_move(const platform::preferences::Values& values);

/// Reads what the recorded move of the recordings moved and left.
///
/// @param values the preferences
/// @return the counts; nullopt when no move is recorded, or its value
///         cannot be read
[[nodiscard]] std::optional<RecordedMove>
recorded_recordings_move(const platform::preferences::Values& values);

/// Tells whether the main menu's notice of a move waits to be shown.
///
/// @param values the preferences
/// @return true while saves_notice_preference or
///     loose_saves_notice_preference holds saves_notice_due
[[nodiscard]] bool saves_notice_due_in(const platform::preferences::Values& values);

/// The moves the main menu's notice tells of.
struct MovesTold {
    /// The move from beside the preferences file (move_earlier_saves);
    /// empty when the notice tells nothing of it.
    std::optional<RecordedMove> beside_preferences;
    /// The move of the saved games loose in Saves (move_loose_saves); empty
    /// when the notice tells nothing of it.
    std::optional<RecordedMove> loose_in_saves;
};

/// Returns the moves whose notice waits: each whose notice preference holds
/// saves_notice_due and whose record can be read.
///
/// @param values the preferences
/// @return the moves; both empty when none waits
[[nodiscard]] MovesTold moves_to_tell(const platform::preferences::Values& values);

/// Records the notices that waited as told, so that no later start shows
/// them again.
///
/// @param[in,out] values the preferences
void record_saves_notice_told(platform::preferences::Values& values);

/// What opening a folder in the system's file manager came to.
struct FolderOpening {
    bool opened{}; ///< the file manager was asked to show the folder
    /// Why not, in a few words of the engine's own that a dialog shows;
    /// empty when it was opened.
    std::string reason;
    /// What happened, a line for standard error and the log; empty when
    /// there is nothing to tell.
    std::string detail;
};

/// Why a folder was not shown: the file manager did not take it.
inline constexpr std::string_view file_manager_failed_text = "The file manager could not open it.";
/// Why a folder was not shown: the system has no way to show one.
inline constexpr std::string_view no_file_manager_text = "No file manager is there to open it.";
/// Why a folder was not shown: it was missing and could not be made.
inline constexpr std::string_view folder_not_made_text = "The folder cannot be made.";

/// Opens folders in the system's file manager.
struct FolderOpenerHooks {
    void* context{};
    /// Shows `folder`, which exists, in the system's file manager; null
    /// opens nothing and tells file_manager_failed_text.
    FolderOpening (*open)(void* context, const std::filesystem::path& folder){};
};

/// Returns hooks that show a folder in the system's file manager: through the
/// platform's own show_folder hook where it has one (PlatformHooks, as the
/// Files app on iOS); else on macOS through open, on Windows through the
/// shell's open verb, and on Linux through xdg-open, or, where xdg-open is
/// missing, by asking the desktop portal over the session bus with gdbus. A
/// build that starts no other programs (OA_PROCESS_SPAWNING off) and has no
/// such hook shows none and tells no_file_manager_text.
///
/// @return the hooks; they need no context
[[nodiscard]] FolderOpenerHooks system_folder_opener() noexcept;

/// Returns hooks that show nothing and append each folder asked for to a
/// list instead, for runs nobody watches.
///
/// @param[in,out] requests receives every folder asked for, in order; it
///     must outlive the hooks
/// @return the hooks
[[nodiscard]] FolderOpenerHooks
recorded_folder_opener(std::vector<std::filesystem::path>& requests) noexcept;

/// Makes a folder when it is missing, then shows it through the hooks.
///
/// @param hooks how folders are shown
/// @param folder the folder
/// @return what came of it; folder_not_made_text when it cannot be made
[[nodiscard]] FolderOpening
open_folder(const FolderOpenerHooks& hooks, const std::filesystem::path& folder);

/// Returns a folder's file URI: "file://" and its absolute path in UTF-8,
/// '/' between its parts and before a drive, each byte other than an ASCII
/// letter, a digit, '-', '.', '_', '~', '/' and ':' written as '%' and two
/// capital hexadecimal digits.
///
/// @param folder the folder, absolute
/// @return the URI
[[nodiscard]] std::string file_uri(const std::filesystem::path& folder);

/// Returns the notice of the moves, in the engine's own words, in English:
/// how many saved games moved, the moves told added together, and to which
/// folder, or with none moved that new ones go there, its path whole for the
/// notice to wrap; how many could not move and stay listed where they were;
/// for the move from beside the preferences file, that screenshots, films
/// and mods now go in the same Open Annihilation folder; and for the move
/// of those loose in Saves, that saved games, screenshots and films go in a
/// folder for each mod, or no_mod_folder_name without one.
///
/// @param moves the moves told
/// @param saves the folder of 3.1c's saved games (saves_folder)
/// @return the notice, with "OPEN FOLDER" for the folder's button
[[nodiscard]] oa::ui::engine_settings::Notice
saves_moved_notice(const MovesTold& moves, const std::filesystem::path& saves);

/// A side and the unit that is its commander, as SIDEDATA names them.
struct SideCommander {
    std::string side;      ///< the side's name
    std::string commander; ///< its commander's unit name; empty when it names none
};

/// A file a side's SIDEDATA section names, its interface art or font, that
/// the mod's files lack.
struct SideFileGap {
    std::string side; ///< the side's name
    std::string path; ///< the file, such as "anims/NAME.GAF"
};

/// What a mod's files lack: none of its units found, or a side's commander
/// missing from them, either of which keeps its games from starting; and the
/// files its sides name that are missing, without which its games start and
/// show what they can.
struct ModStartGaps {
    bool no_units{};                                 ///< no unit definition was found
    std::vector<SideCommander> missing_commanders{}; ///< the sides whose commander is missing
    std::vector<SideFileGap> missing_side_files{};   ///< the side files missing, by side

    /// Tells whether anything is missing.
    ///
    /// @return true when the mod warns
    [[nodiscard]] bool any() const noexcept {
        return games_cannot_start() || !missing_side_files.empty();
    }

    /// Tells whether what is missing keeps the mod's games from starting:
    /// its units, or a commander. Missing side files alone do not.
    ///
    /// @return true when a game cannot start
    [[nodiscard]] bool games_cannot_start() const noexcept {
        return no_units || !missing_commanders.empty();
    }
};

/// Finds what keeps a mod's games from starting: no units at all, and each
/// side whose commander is not among them, its name matched without case. A
/// side that names no commander is passed over.
///
/// @param unit_names the names of the mod's units
/// @param sides each side and its commander, in the sides' order
/// @return the gaps; none when every side's commander is among the units
[[nodiscard]] ModStartGaps find_mod_start_gaps(
    const std::vector<std::string>& unit_names, const std::vector<SideCommander>& sides
);

/// Returns the warning that a mod's files are missing, in the engine's own
/// words, in English: the mod's name, that no units were found, each side
/// whose commander is not among its units, each file a side names that its
/// files lack, which games show without, and either that its games cannot
/// start until its files are added to its folder or, when only side files
/// are missing, that they still start; then the folder's path whole for the
/// notice to wrap.
///
/// @param mod_name the mod's name, as its profile gives it; empty for a
///     folder without a profile
/// @param gaps what its files lack
/// @param folder the mod's folder
/// @return the notice, with "OPEN MOD FOLDER" for the folder's button
[[nodiscard]] oa::ui::engine_settings::Notice mod_files_missing_notice(
    std::string_view mod_name, const ModStartGaps& gaps, const std::filesystem::path& folder
);

} // namespace oa::app
