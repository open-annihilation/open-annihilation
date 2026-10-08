// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player's own folder: where it is, the folders each mod's files go in,
// the moves of saved games and recordings from where earlier versions kept
// them, and the notice of the saved games' moves; and the warning that a
// mod's games cannot start until its files are in its folder.

#include "oa/app/user_folder.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace fs = std::filesystem;

namespace {

/// The name of the Image Output Directory's screenshots folder, matched
/// without case.
constexpr std::string_view game_screenshots_name = "SCREENSHOTS";
/// What the name of a film's folder below the Image Output Directory starts
/// with, a number after it.
constexpr std::string_view game_film_prefix = "MOVIE";

/// Converts UTF-8 text to a path.
///
/// @param text the path, UTF-8
/// @return the path
fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Converts a path to UTF-8 text.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string to_utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// Returns a text with its ASCII letters raised.
///
/// @param text the text
/// @return the text in capitals
std::string capitals(std::string_view text) {
    std::string raised(text);
    for (auto& character : raised)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return raised;
}

/// Tells whether a folder's name is a film's: MOVIE, matched without case,
/// then digits, or the '*' of a search for them.
///
/// @param name the name
/// @return true for a film's folder
bool film_folder_name(std::string_view name) {
    const std::string raised = capitals(name);
    if (!raised.starts_with(game_film_prefix))
        return false;
    const std::string_view rest = std::string_view(raised).substr(game_film_prefix.size());
    return std::all_of(rest.begin(), rest.end(), [](char character) {
        return (character >= '0' && character <= '9') || character == '*';
    });
}

/// Records a move under a preference, and its notice due under another
/// when it moved or left a saved game.
///
/// @param[in,out] values the preferences
/// @param move what the move did
/// @param record the preference that records it
/// @param notice the preference that records its notice
void record_move(
    platform::preferences::Values& values,
    const SavesMove& move,
    std::string_view record,
    std::string_view notice
) {
    values[std::string(record)] = std::to_string(move.moved) + " " + std::to_string(move.left);
    if (move.moved != 0 || move.left != 0)
        values[std::string(notice)] = std::string(saves_notice_due);
}

/// Reads what a recorded move moved and left.
///
/// @param values the preferences
/// @param record the preference that records it
/// @return the counts; nullopt when it is absent or cannot be read
std::optional<RecordedMove>
read_move(const platform::preferences::Values& values, std::string_view record) {
    const auto found = values.find(std::string(record));
    if (found == values.end())
        return std::nullopt;
    const std::string& text = found->second;
    RecordedMove move;
    const char* const end = text.data() + text.size();
    const auto first = std::from_chars(text.data(), end, move.moved);
    if (first.ec != std::errc{} || first.ptr == end || *first.ptr != ' ')
        return std::nullopt;
    const auto second = std::from_chars(first.ptr + 1, end, move.left);
    if (second.ec != std::errc{} || second.ptr != end)
        return std::nullopt;
    return move;
}

/// Tells whether a move's notice waits to be shown.
///
/// @param values the preferences
/// @param notice the preference that records its notice
/// @return true while it holds saves_notice_due
bool notice_due(const platform::preferences::Values& values, std::string_view notice) {
    const auto found = values.find(std::string(notice));
    return found != values.end() && found->second == saves_notice_due;
}

/// Returns "1 saved game" or "N saved games".
///
/// @param count the saved games
/// @return the words
std::string saved_games(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " saved game" : " saved games");
}

/// Returns a folder's path in its plainest spelling: normalised, without a
/// separator at its end.
///
/// @param folder the folder
/// @return the path
fs::path plain_folder(const fs::path& folder) {
    fs::path plain = folder.lexically_normal();
    if (!plain.has_filename() && plain.has_parent_path() && plain != plain.root_path())
        plain = plain.parent_path();
    return plain;
}

} // namespace

fs::path user_folder_beside(const fs::path& preferences_file) {
    std::error_code error;
    const fs::path absolute = fs::absolute(preferences_file, error);
    const fs::path file = (error ? preferences_file : absolute).lexically_normal();
    return file.parent_path() / std::string(oa::platform::preferences::user_folder_name);
}

fs::path own_user_folder(
    const std::optional<fs::path>& user_folder_option,
    const std::optional<fs::path>& preferences_file_option,
    const fs::path& preference_path,
    const platform::preferences::Values& values,
    std::string& note
) {
    namespace platform_preferences = oa::platform::preferences;
    std::error_code error;
    const fs::path preference_folder =
        fs::absolute(preference_path, error).lexically_normal().parent_path();
    fs::path fallback;
    if (preferences_file_option) {
        fallback = user_folder_beside(*preferences_file_option);
    } else {
        try {
            fallback = platform_preferences::default_user_folder();
        } catch (const std::exception& failure) {
            fallback = preference_folder / std::string(platform_preferences::user_folder_name);
            note = failure.what();
        }
    }
    const fs::path chosen = choose_user_folder(user_folder_option, values, fallback);
    return chosen;
}

fs::path choose_user_folder(
    const std::optional<fs::path>& option,
    const platform::preferences::Values& values,
    const fs::path& fallback
) {
    std::error_code error;
    if (option && !option->empty()) {
        const fs::path absolute = fs::absolute(*option, error);
        return plain_folder(error ? *option : absolute);
    }
    const auto stored = values.find(std::string(user_folder_preference));
    if (stored != values.end() && !stored->second.empty()) {
        const fs::path folder = from_utf8(stored->second);
        if (folder.is_absolute())
            return plain_folder(folder);
    }
    return plain_folder(fallback);
}

std::string mod_subfolder_name(std::string_view mod_id) {
    if (mod_id.empty())
        return std::string(no_mod_folder_name);
    if (mod_id == no_mod_folder_name)
        return std::string(default_id_folder_name);
    return std::string(mod_id);
}

fs::path saves_folder(const fs::path& user_folder, std::string_view mod_id) {
    return user_folder / std::string(saves_folder_name) / from_utf8(mod_subfolder_name(mod_id));
}

fs::path screenshots_folder(const fs::path& user_folder, std::string_view mod_id) {
    return user_folder / std::string(screenshots_folder_name) /
           from_utf8(mod_subfolder_name(mod_id));
}

fs::path films_folder(const fs::path& user_folder, std::string_view mod_id) {
    return user_folder / std::string(films_folder_name) / from_utf8(mod_subfolder_name(mod_id));
}

fs::path recordings_folder(const fs::path& user_folder, std::string_view mod_id) {
    return user_folder / std::string(recordings_folder_name) /
           from_utf8(mod_subfolder_name(mod_id));
}

fs::path recording_file(
    const fs::path& net_record,
    const fs::path& user_folder,
    std::string_view mod_id,
    const fs::path& name
) {
    if (!net_record.empty())
        return net_record;
    return recordings_folder(user_folder, mod_id) / name;
}

fs::path
place_capture_path(const fs::path& path, const fs::path& user_folder, std::string_view mod_id) {
    if (user_folder.empty())
        return path;
    const fs::path relative = path.lexically_normal().lexically_relative(user_folder);
    if (relative.empty())
        return path;
    auto part = relative.begin();
    const std::string first = to_utf8(*part);
    if (first == "." || first == "..")
        return path;
    fs::path placed;
    if (capitals(first) == game_screenshots_name)
        placed = screenshots_folder(user_folder, mod_id);
    else if (film_folder_name(first))
        placed = films_folder(user_folder, mod_id) / *part;
    else
        return path;
    for (++part; part != relative.end(); ++part)
        placed /= *part;
    return placed;
}

bool holds_a_file(const fs::path& folder) {
    std::error_code error;
    for (fs::directory_iterator entry{folder, error}, end; !error && entry != end;
         entry.increment(error)) {
        std::error_code status;
        if (entry->is_regular_file(status))
            return true;
    }
    return false;
}

std::optional<fs::path> entry_without_case(const fs::path& folder, std::string_view name) {
    const std::string wanted = capitals(name);
    std::error_code error;
    for (fs::directory_iterator entry{folder, error}, end; !error && entry != end;
         entry.increment(error))
        if (capitals(to_utf8(entry->path().filename())) == wanted)
            return entry->path();
    return std::nullopt;
}

std::string free_file_name(const std::string& name, const std::vector<std::string>& taken) {
    const auto is_taken = [&taken](const std::string& candidate) {
        return std::find(taken.begin(), taken.end(), capitals(candidate)) != taken.end();
    };
    if (!is_taken(name))
        return name;
    const auto dot = name.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string extension =
        dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
    for (std::size_t number = 2;; ++number) {
        std::string candidate = stem + " (" + std::to_string(number) + ")" + extension;
        if (!is_taken(candidate))
            return candidate;
    }
}

namespace {

/// The files a move counts, and the words it says what happened in.
struct MovedFiles {
    /// Tells whether a file counts, rather than moving with them.
    bool (*counts)(const fs::path& file){};
    /// What the files that count are called: "the saved games".
    std::string_view them;
    /// What follows "they stay there" when none of them can move.
    std::string_view left_them;
    /// What follows "it stays there" when one of them cannot move.
    std::string_view left_it;
};

/// Saved games, which the dialogs still list where they stay; other files,
/// such as restriction lists, move with them.
constexpr MovedFiles saved_games_moved{
    [](const fs::path& file) {
        return capitals(to_utf8(file.extension())) == saved_game_extension;
    },
    "the saved games",
    ", and the game lists them where they are",
    ", and the game lists it where it is"
};

/// Recordings: every file of an earlier recordings folder, whatever its
/// extension.
constexpr MovedFiles recordings_moved{
    [](const fs::path&) { return true; }, "the recordings", "", ""
};

/// Moves the files of an earlier folder into another, as move_saves_folder
/// describes, counting those of a kind and saying what happened in its
/// words.
///
/// @param earlier the earlier folder
/// @param into the folder they move to; made when missing
/// @param[in,out] move what the move did, added to: the files of the kind
///        in moved, renamed and left, the others in other_files
/// @param hooks stand-ins for the file system's rename and copy
/// @param kind which files count, and their words
void move_folder_files(
    const fs::path& earlier,
    const fs::path& into,
    SavesMove& move,
    const FileMoveHooks& hooks,
    const MovedFiles& kind
) {
    std::error_code error;
    if (!fs::is_directory(earlier, error))
        return;
    // The files to move, in name order, so that a run is told the same way
    // each time.
    std::vector<fs::path> files;
    for (fs::directory_iterator entry{earlier, error}, end; !error && entry != end;
         entry.increment(error)) {
        std::error_code status;
        if (entry->is_regular_file(status))
            files.push_back(entry->path());
    }
    std::sort(files.begin(), files.end());
    const auto leave_all = [&](const std::string& why) {
        std::size_t counted = 0;
        for (const auto& file : files)
            counted += kind.counts(file) ? 1 : 0;
        move.left += counted;
        move.lines.push_back(
            "cannot move " + std::string(kind.them) + " in " + to_utf8(earlier) + " to " +
            to_utf8(into) + ": " + why + "; they stay there" + std::string(kind.left_them)
        );
    };
    if (files.empty()) {
        // An empty earlier folder goes; one that holds folders stays.
        std::error_code ignored;
        fs::remove(earlier, ignored);
        return;
    }
    fs::create_directories(into, error);
    if (error || !fs::is_directory(into, error)) {
        leave_all(error ? error.message() : std::string("it is not a folder"));
        return;
    }
    // The names the folder moved into holds, which nothing moved
    // overwrites, in capitals and as they are spelt.
    std::vector<std::string> taken;
    std::vector<std::string> spelt;
    for (fs::directory_iterator entry{into, error}, end; !error && entry != end;
         entry.increment(error)) {
        spelt.push_back(to_utf8(entry->path().filename()));
        taken.push_back(capitals(spelt.back()));
    }
    if (error) {
        leave_all(error.message());
        return;
    }
    std::size_t moved_here = 0;
    for (const auto& file : files) {
        const std::string name = to_utf8(file.filename());
        std::string given = free_file_name(name, taken);
        // A name made since the folder was listed is taken too.
        std::error_code exists_error;
        while (fs::exists(into / from_utf8(given), exists_error)) {
            taken.push_back(capitals(given));
            spelt.push_back(given);
            given = free_file_name(name, taken);
        }
        const fs::path target = into / from_utf8(given);
        const bool counted = kind.counts(file);
        std::error_code failure;
        if (hooks.rename != nullptr)
            hooks.rename(hooks.context, file, target, failure);
        else
            fs::rename(file, target, failure);
        if (failure) {
            // Across volumes, or where a rename is refused: a copy under a
            // name of its own, renamed into place once it is whole, so that
            // a copy cut short never stands as a whole file; the original
            // is removed after.
            const fs::path partial = into / from_utf8(given + std::string(partial_copy_suffix));
            std::error_code copied;
            if (hooks.copy != nullptr)
                hooks.copy(hooks.context, file, partial, copied);
            else
                fs::copy_file(file, partial, fs::copy_options::overwrite_existing, copied);
            std::error_code size_error;
            if (!copied && fs::file_size(file, size_error) != fs::file_size(partial, size_error))
                copied = std::make_error_code(std::errc::io_error);
            if (!copied && fs::exists(target, exists_error))
                copied = std::make_error_code(std::errc::file_exists);
            if (!copied)
                fs::rename(partial, target, copied);
            if (copied) {
                // A copy cut short goes; the original stays.
                std::error_code ignored;
                if (fs::exists(partial, ignored))
                    fs::remove(partial, ignored);
                if (counted)
                    ++move.left;
                move.lines.push_back(
                    "cannot move " + to_utf8(file) + " to " + to_utf8(target) + ": " +
                    failure.message() + "; nor copy it: " + copied.message() + "; it stays there" +
                    std::string(kind.left_it)
                );
                continue;
            }
            std::error_code removed;
            fs::remove(file, removed);
            if (removed)
                move.lines.push_back(
                    "copied " + to_utf8(file) + " to " + to_utf8(target) +
                    ", but cannot remove it: " + removed.message()
                );
        }
        taken.push_back(capitals(given));
        spelt.push_back(given);
        ++moved_here;
        if (counted)
            ++move.moved;
        else
            ++move.other_files;
        if (given != name) {
            if (counted)
                ++move.renamed;
            // The name it would have taken, as the folder moved into spells it.
            const auto there = std::find(taken.begin(), taken.end(), capitals(name));
            const std::string holder = there != taken.end()
                                           ? spelt[static_cast<std::size_t>(there - taken.begin())]
                                           : name;
            move.lines.push_back(
                "kept " + to_utf8(file) + " as " + to_utf8(target) + ", since " +
                to_utf8(into / from_utf8(holder)) + " was there already"
            );
        }
    }
    if (moved_here != 0)
        move.lines.push_back(
            "moved " + std::to_string(moved_here) + (moved_here == 1 ? " file" : " files") +
            " from " + to_utf8(earlier) + " to " + to_utf8(into)
        );
    std::error_code ignored;
    fs::remove(earlier, ignored); // only once it is empty
}

/// Returns the folders, one for each mod, of the folder beside the
/// preferences file that held each mod's earlier folders, in name order.
///
/// @param earlier_root the preferences file's folder
/// @return the mods/<id> folders; none when there is no mods folder
std::vector<fs::path> earlier_mod_folders(const fs::path& earlier_root) {
    std::vector<fs::path> mod_folders;
    const auto mods = entry_without_case(earlier_root, earlier_mods_folder_name);
    if (!mods)
        return mod_folders;
    std::error_code error;
    for (fs::directory_iterator entry{*mods, error}, end; !error && entry != end;
         entry.increment(error)) {
        std::error_code status;
        if (entry->is_directory(status))
            mod_folders.push_back(entry->path());
    }
    std::sort(mod_folders.begin(), mod_folders.end());
    return mod_folders;
}

} // namespace

void move_saves_folder(
    const fs::path& earlier, const fs::path& saves, SavesMove& move, const FileMoveHooks& hooks
) {
    move_folder_files(earlier, saves, move, hooks, saved_games_moved);
}

SavesMove move_earlier_saves(
    const fs::path& earlier_root, const fs::path& user_folder, const FileMoveHooks& hooks
) {
    SavesMove move;
    if (const auto earlier = entry_without_case(earlier_root, earlier_saves_folder_name))
        move_saves_folder(*earlier, saves_folder(user_folder, {}), move, hooks);
    for (const auto& mod : earlier_mod_folders(earlier_root))
        if (const auto earlier = entry_without_case(mod, earlier_saves_folder_name))
            move_saves_folder(
                *earlier, saves_folder(user_folder, to_utf8(mod.filename())), move, hooks
            );
    return move;
}

SavesMove move_loose_saves(const fs::path& user_folder, const FileMoveHooks& hooks) {
    SavesMove move;
    // Only files move; a Saves that holds none, or only folders, stays as
    // it is.
    const fs::path saves = user_folder / std::string(saves_folder_name);
    if (holds_a_file(saves))
        move_saves_folder(saves, saves_folder(user_folder, {}), move, hooks);
    return move;
}

RecordingsMove move_earlier_recordings(
    const fs::path& earlier_root, const fs::path& user_folder, const FileMoveHooks& hooks
) {
    SavesMove files;
    if (const auto earlier = entry_without_case(earlier_root, earlier_recordings_folder_name))
        move_folder_files(
            *earlier, recordings_folder(user_folder, {}), files, hooks, recordings_moved
        );
    for (const auto& mod : earlier_mod_folders(earlier_root))
        if (const auto earlier = entry_without_case(mod, earlier_recordings_folder_name))
            move_folder_files(
                *earlier,
                recordings_folder(user_folder, to_utf8(mod.filename())),
                files,
                hooks,
                recordings_moved
            );
    return RecordingsMove{files.moved, files.renamed, files.left, std::move(files.lines)};
}

void record_saves_move(platform::preferences::Values& values, const SavesMove& move) {
    record_move(values, move, saves_moved_preference, saves_notice_preference);
}

void record_loose_saves_move(platform::preferences::Values& values, const SavesMove& move) {
    record_move(values, move, loose_saves_moved_preference, loose_saves_notice_preference);
}

void record_recordings_move(platform::preferences::Values& values, const RecordingsMove& move) {
    values[std::string(recordings_moved_preference)] =
        std::to_string(move.moved) + " " + std::to_string(move.left);
}

std::optional<RecordedMove> recorded_saves_move(const platform::preferences::Values& values) {
    return read_move(values, saves_moved_preference);
}

std::optional<RecordedMove> recorded_loose_saves_move(const platform::preferences::Values& values) {
    return read_move(values, loose_saves_moved_preference);
}

std::optional<RecordedMove> recorded_recordings_move(const platform::preferences::Values& values) {
    return read_move(values, recordings_moved_preference);
}

bool saves_notice_due_in(const platform::preferences::Values& values) {
    return notice_due(values, saves_notice_preference) ||
           notice_due(values, loose_saves_notice_preference);
}

MovesTold moves_to_tell(const platform::preferences::Values& values) {
    MovesTold moves;
    if (notice_due(values, saves_notice_preference))
        moves.beside_preferences = recorded_saves_move(values);
    if (notice_due(values, loose_saves_notice_preference))
        moves.loose_in_saves = recorded_loose_saves_move(values);
    return moves;
}

void record_saves_notice_told(platform::preferences::Values& values) {
    for (const std::string_view notice : {saves_notice_preference, loose_saves_notice_preference})
        if (notice_due(values, notice))
            values[std::string(notice)] = std::string(saves_notice_told);
}

FolderOpenerHooks recorded_folder_opener(std::vector<fs::path>& requests) noexcept {
    FolderOpenerHooks hooks{};
    hooks.context = &requests;
    hooks.open = [](void* context, const fs::path& folder) {
        static_cast<std::vector<fs::path>*>(context)->push_back(folder);
        return FolderOpening{true, {}, {}};
    };
    return hooks;
}

FolderOpening open_folder(const FolderOpenerHooks& hooks, const fs::path& folder) {
    std::error_code error;
    fs::create_directories(folder, error);
    if (error || !fs::is_directory(folder, error))
        return FolderOpening{
            false,
            std::string(folder_not_made_text),
            "cannot make " + to_utf8(folder) +
                (error ? ": " + error.message() : std::string(": it is not a folder"))
        };
    if (hooks.open == nullptr)
        return FolderOpening{false, std::string(file_manager_failed_text), {}};
    return hooks.open(hooks.context, folder);
}

std::string file_uri(const fs::path& folder) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string uri = "file://";
    const auto generic = folder.generic_u8string();
    const std::string path(generic.begin(), generic.end());
    // A drive's path ("C:/...") is written after a third slash.
    if (!path.starts_with('/'))
        uri += '/';
    for (const char character : path) {
        const auto byte = static_cast<unsigned char>(character);
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            (character >= '0' && character <= '9');
        if (letter || character == '-' || character == '.' || character == '_' ||
            character == '~' || character == '/' || character == ':') {
            uri += character;
            continue;
        }
        uri += '%';
        uri += digits[byte >> 4U];
        uri += digits[byte & 0x0FU];
    }
    return uri;
}

oa::ui::engine_settings::Notice saves_moved_notice(const MovesTold& moves, const fs::path& saves) {
    // The moves told, added together.
    RecordedMove move;
    for (const auto& told : {moves.beside_preferences, moves.loose_in_saves})
        if (told) {
            move.moved += told->moved;
            move.left += told->left;
        }
    oa::ui::engine_settings::Notice notice;
    notice.title = "SAVED GAMES MOVED";
    notice.open_caption = "OPEN FOLDER";
    // Where they went, or with none moved, where new ones go.
    notice.paragraphs.push_back(
        {move.moved != 0
             ? saved_games(move.moved) + (move.moved == 1 ? " has" : " have") + " moved to:"
             : std::string("New saved games go in:"),
         false}
    );
    notice.paragraphs.push_back({to_utf8(saves), true});
    if (move.left != 0)
        notice.paragraphs.push_back(
            {saved_games(move.left) + " could not be moved, so the game lists " +
                 (move.left == 1 ? "it" : "them") + " where " +
                 (move.left == 1 ? "it is" : "they are") + ". The log says why.",
             false}
        );
    if (moves.beside_preferences)
        notice.paragraphs.push_back(
            {"Screenshots, films and mods now go in the same Open Annihilation folder.", false}
        );
    if (moves.loose_in_saves)
        notice.paragraphs.push_back(
            {"Saved games, screenshots and films now go in a folder for each mod, or in " +
                 std::string(no_mod_folder_name) + " without a mod.",
             false}
        );
    return notice;
}

ModStartGaps find_mod_start_gaps(
    const std::vector<std::string>& unit_names, const std::vector<SideCommander>& sides
) {
    ModStartGaps gaps;
    gaps.no_units = unit_names.empty();
    for (const auto& side : sides) {
        // A side that names no commander has none for the files to miss.
        if (side.commander.empty())
            continue;
        const std::string wanted = capitals(side.commander);
        const bool found =
            std::any_of(unit_names.begin(), unit_names.end(), [&wanted](const std::string& name) {
                return capitals(name) == wanted;
            });
        if (!found)
            gaps.missing_commanders.push_back(side);
    }
    return gaps;
}

oa::ui::engine_settings::Notice mod_files_missing_notice(
    std::string_view mod_name, const ModStartGaps& gaps, const fs::path& folder
) {
    oa::ui::engine_settings::Notice notice;
    notice.title = "MOD FILES MISSING";
    notice.open_caption = "OPEN MOD FOLDER";
    notice.paragraphs.push_back(
        {(mod_name.empty() ? std::string("This mod") : std::string(mod_name)) +
             " is missing files.",
         false}
    );
    if (gaps.no_units)
        notice.paragraphs.push_back({"None of this mod's units were found.", false});
    for (const auto& side : gaps.missing_commanders)
        notice.paragraphs.push_back(
            {"The " + side.side + " commander (" + side.commander + ") isn't in this mod's units.",
             false}
        );
    for (const auto& file : gaps.missing_side_files)
        notice.paragraphs.push_back(
            {"The " + file.side + " side's " + file.path +
                 " isn't in this mod's files; games show without it.",
             false}
        );
    notice.paragraphs.push_back(
        {gaps.games_cannot_start()
             ? "Its games can't start until the mod's files are added to its folder:"
             : "Its games still start. Add the missing files to its folder to show them:",
         false}
    );
    notice.paragraphs.push_back({to_utf8(plain_folder(folder)), true});
    return notice;
}

} // namespace oa::app
