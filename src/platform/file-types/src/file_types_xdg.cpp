// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The XDG registration: the MIME package, the two desktop entries (the files the game
// installs or adds, and launch files) and the icons in the user's data folder, and the
// database tools run after a change, without a shell, and at later starts until a run
// succeeds.

#include "oa/platform/file_types.hpp"

#include "oa/platform/files.hpp"
#include "oa/base/threads.hpp"
#include "oa/platform/system.hpp"
#include "utf8.hpp"

#include <stdint.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#ifndef OA_PROCESS_SPAWNING
#error "OA_PROCESS_SPAWNING (0 or 1) says whether the game may start other programs"
#endif

#if OA_PROCESS_SPAWNING
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
// The process's environment, which each tool is given.
extern char** environ;
#endif

namespace oa::platform::file_types {

namespace fs = std::filesystem;

namespace {

/// The variable a Flatpak sandbox sets; such a package registers its own file types.
constexpr const char* flatpak_variable = "FLATPAK_ID";
/// The variable a Snap sets; such a package registers its own file types.
constexpr const char* snap_variable = "SNAP";
/// The size of the hicolor folder the icon goes in, in pixels.
constexpr std::string_view icon_size_folder = "256x256";
/// What ends the name of the stamp file a database tool's run leaves in the
/// folder it works on, after a dot and desktop_id, so that file managers hide it.
constexpr std::string_view stamp_suffix = ".updated";
/// How often a running tool is looked at, in milliseconds.
[[maybe_unused]] constexpr uint32_t tool_poll_ms = 10;

/// What came of writing one file.
enum class Written : uint8_t {
    same,    ///< the file already held the bytes
    written, ///< the file was written
    failed,  ///< it could not be written; the reason is given
};

/// Returns the value of an environment variable that is set and not empty.
///
/// @param name the variable
/// @return its value; nullopt when it is unset or empty
std::optional<std::string> set_value(const char* name) {
    auto value = oa::platform::environment_value(name);
    if (!value || value->empty())
        return std::nullopt;
    return value;
}

/// Reports whether `path` lies inside `folder`, comparing their parts after a lexical
/// normalisation.
///
/// @param path the path, absolute
/// @param folder the folder, absolute
/// @return true when every part of the folder begins the path
bool inside(const fs::path& path, const fs::path& folder) {
    const fs::path inner = path.lexically_normal();
    const fs::path outer = folder.lexically_normal();
    auto inner_part = inner.begin();
    for (auto outer_part = outer.begin(); outer_part != outer.end(); ++outer_part) {
        // A folder written with a final '/' ends in an empty part.
        if (outer_part->empty() && std::next(outer_part) == outer.end())
            break;
        if (inner_part == inner.end() || *inner_part != *outer_part)
            return false;
        ++inner_part;
    }
    return true;
}

/// Returns the temporary folder that holds `executable`: $TMPDIR when it is absolute, or
/// /tmp, compared as written and with links resolved.
///
/// @param executable the program, absolute
/// @return the folder; nullopt when the program lies in neither
std::optional<fs::path> temporary_folder_of(const fs::path& executable) {
    std::vector<fs::path> folders;
    if (const auto variable = set_value("TMPDIR"); variable && fs::path(*variable).is_absolute())
        folders.emplace_back(*variable);
    folders.emplace_back("/tmp");
    std::error_code error;
    const fs::path resolved = fs::weakly_canonical(executable, error);
    for (const fs::path& folder : folders) {
        if (inside(executable, folder))
            return folder;
        std::error_code folder_error;
        const fs::path resolved_folder = fs::weakly_canonical(folder, folder_error);
        if (!error && !folder_error && inside(resolved, resolved_folder))
            return folder;
    }
    return std::nullopt;
}

/// Returns the XDG data folder the registration writes in.
///
/// @param places the places a caller named
/// @param[out] why why there is none, when there is none
/// @return the folder; nullopt when none is named
std::optional<fs::path> data_folder(const Places& places, std::string& why) {
    if (!places.data_home.empty()) {
        if (places.data_home.is_absolute())
            return places.data_home;
        why = "the data folder " + detail::utf8_of(places.data_home) + " is not absolute";
        return std::nullopt;
    }
    // A relative XDG_DATA_HOME is not valid, and is passed over as if unset.
    if (const auto variable = set_value("XDG_DATA_HOME");
        variable && fs::path(*variable).is_absolute())
        return fs::path(*variable);
    if (const auto home = set_value("HOME"); home && fs::path(*home).is_absolute())
        return fs::path(*home) / ".local" / "share";
    why = "neither XDG_DATA_HOME nor HOME names an absolute folder";
    return std::nullopt;
}

/// Reports whether `file` holds exactly `bytes`. Only a file of the same size is read.
///
/// @param file the file
/// @param bytes the bytes it should hold
/// @return true when it holds them
bool holds(const fs::path& file, std::span<const uint8_t> bytes) {
    std::error_code error;
    const uintmax_t size = fs::file_size(file, error);
    if (error || size != bytes.size())
        return false;
    std::FILE* stream = oa::platform::open_file(file, "rb");
    if (stream == nullptr)
        return false;
    std::vector<uint8_t> held(bytes.size());
    const std::size_t read = held.empty() ? 0 : std::fread(held.data(), 1, held.size(), stream);
    // One byte more would mean the file grew since its size was read.
    uint8_t extra = 0;
    const bool ended = std::fread(&extra, 1, 1, stream) == 0;
    std::fclose(stream);
    return read == held.size() && ended && std::equal(held.begin(), held.end(), bytes.begin());
}

/// Writes `bytes` into `file` unless it holds them already: into a temporary file beside it
/// first, renamed over it once whole, so that a reader never sees half a file. The stamp of
/// the database tool that reads the file is removed before anything is written, so that a
/// stop at any point leaves the tool's run due.
///
/// @param file the file
/// @param bytes what it holds
/// @param stamp the stamp file of the tool that reads it; empty for none
/// @param[out] why why it could not be written, when it could not
/// @return what came of it
Written write_if_different(
    const fs::path& file, std::span<const uint8_t> bytes, const fs::path& stamp, std::string& why
) {
    if (holds(file, bytes))
        return Written::same;
    std::error_code error;
    if (!stamp.empty() && !fs::remove(stamp, error) && error) {
        why = "cannot remove " + detail::utf8_of(stamp) + ": " + error.message();
        return Written::failed;
    }
    fs::create_directories(file.parent_path(), error);
    if (error) {
        why = "cannot make " + detail::utf8_of(file.parent_path()) + ": " + error.message();
        return Written::failed;
    }
    fs::path temporary = file;
    temporary += "." + std::to_string(static_cast<long long>(getpid())) + ".tmp";
    std::FILE* stream = oa::platform::open_file(temporary, "wb");
    if (stream == nullptr) {
        why = "cannot write " + detail::utf8_of(temporary) + ": " + std::strerror(errno);
        return Written::failed;
    }
    const std::size_t wrote =
        bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), stream);
    const bool flushed = std::fflush(stream) == 0;
    const bool closed = std::fclose(stream) == 0;
    if (wrote != bytes.size() || !flushed || !closed) {
        why = "cannot write " + detail::utf8_of(temporary);
        fs::remove(temporary, error);
        return Written::failed;
    }
    fs::rename(temporary, file, error);
    if (error) {
        why = "cannot replace " + detail::utf8_of(file) + ": " + error.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Written::failed;
    }
    return Written::written;
}

/// Returns a text's bytes.
///
/// @param text the text
/// @return a view of its bytes
std::span<const uint8_t> bytes_of(const std::string& text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// What came of reading one file.
enum class Read : uint8_t {
    missing, ///< the file is not there
    read,    ///< the file was read
    failed,  ///< it could not be read; the reason is given
};

/// A file's text, and whether it was read.
struct TextFile {
    Read state{Read::missing}; ///< what came of the read
    std::string text{};        ///< the text, when it was read
};

/// Reads a whole file.
///
/// @param file the file
/// @param[out] why why it could not be read, when it could not
/// @return the text, or that the file is missing
TextFile read_text(const fs::path& file, std::string& why) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(file, error);
    // A path that is not there is reported as an error on some platforms, and as not_found
    // on others. Either way the file is missing.
    if (status.type() == fs::file_type::not_found || error == std::errc::no_such_file_or_directory)
        return {Read::missing, {}};
    if (error) {
        why = "cannot read " + detail::utf8_of(file) + ": " + error.message();
        return {Read::failed, {}};
    }
    if (!fs::exists(status))
        return {Read::missing, {}};
    std::FILE* stream = oa::platform::open_file(file, "rb");
    if (stream == nullptr) {
        why = "cannot read " + detail::utf8_of(file) + ": " + std::strerror(errno);
        return {Read::failed, {}};
    }
    std::string text;
    char buffer[4096];
    while (true) {
        const std::size_t got = std::fread(buffer, 1, sizeof buffer, stream);
        text.append(buffer, got);
        if (got < sizeof buffer)
            break;
    }
    const bool failed = std::ferror(stream) != 0;
    std::fclose(stream);
    if (failed) {
        why = "cannot read " + detail::utf8_of(file);
        return {Read::failed, {}};
    }
    return {Read::read, std::move(text)};
}

/// Removes `file` when it is there, and the stamp of the tool that reads it, so that the
/// tool's run is due. A file that is already absent is left so.
///
/// @param file the file
/// @param stamp the stamp file; empty for none
/// @param[out] why why it could not be removed, when it could not
/// @param[in,out] done the registration's record; a removal is a line
/// @return what came of it; written means the file was removed
Written remove_if_present(
    const fs::path& file, const fs::path& stamp, std::string& why, Registration& done
) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(file, error);
    // A path that is not there is reported as an error on some platforms, and as not_found
    // on others. Either way there is nothing to remove.
    if (status.type() == fs::file_type::not_found || error == std::errc::no_such_file_or_directory)
        return Written::same;
    if (error) {
        why = "cannot remove " + detail::utf8_of(file) + ": " + error.message();
        return Written::failed;
    }
    if (!fs::exists(status))
        return Written::same;
    error.clear();
    if (!stamp.empty() && !fs::remove(stamp, error) && error) {
        why = "cannot remove " + detail::utf8_of(stamp) + ": " + error.message();
        return Written::failed;
    }
    error.clear();
    // remove answers false both when the file is already gone and when it cannot be removed.
    // Only an error is a failure.
    const bool removed = fs::remove(file, error);
    if (error) {
        why = "cannot remove " + detail::utf8_of(file) + ": " + error.message();
        return Written::failed;
    }
    if (!removed)
        return Written::same;
    done.lines.push_back("removed " + detail::utf8_of(file));
    return Written::written;
}

/// The mimetype icon of one file type, in the hicolor theme.
///
/// @param hicolor_folder the hicolor folder
/// @param extension the extension, without its dot
/// @return the icon's path
fs::path type_icon(const fs::path& hicolor_folder, std::string_view extension) {
    return hicolor_folder / icon_size_folder / "mimetypes" /
           ("application-x-" + std::string(extension) + ".png");
}

/// Reports whether a MIME type belongs to a file type in `which`.
///
/// @param mime the MIME type
/// @param which the file types
/// @return true when one of them has that MIME type
bool mime_in(std::string_view mime, TypeSet which) {
    for (const FileType& type : file_types)
        if (which.contains(type.extension) && type.mime_type == mime)
            return true;
    return false;
}

/// Reports whether a MimeType value lists no type.
///
/// @param value the text after MimeType=, up to the line's end
/// @return true when it holds no type
bool mime_list_empty(std::string_view value) {
    std::size_t cursor = 0;
    while (cursor < value.size()) {
        const std::size_t semi = value.find(';', cursor);
        const std::size_t end = semi == std::string_view::npos ? value.size() : semi;
        if (end > cursor)
            return false;
        if (semi == std::string_view::npos)
            break;
        cursor = semi + 1;
    }
    return true;
}

/// Takes the MIME types in `which` out of a desktop entry's MimeType line.
///
/// @param entry the entry's text
/// @param which the file types to take out
/// @param[out] emptied true when the line lists no type afterwards, or never listed one
/// @return the entry's text, and whether it changed
std::pair<std::string, bool> desktop_without(std::string entry, TypeSet which, bool& emptied) {
    emptied = false;
    const std::string prefix = "MimeType=";
    const std::size_t line_start = entry.find(prefix);
    if (line_start == std::string::npos)
        return {std::move(entry), false};
    const std::size_t line_end = entry.find('\n', line_start);
    const std::size_t value_end = line_end == std::string::npos ? entry.size() : line_end;
    const std::size_t value_at = line_start + prefix.size();
    const std::string value = entry.substr(value_at, value_end - value_at);
    std::string kept;
    std::size_t cursor = 0;
    while (cursor < value.size()) {
        const std::size_t semi = value.find(';', cursor);
        const std::size_t end = semi == std::string::npos ? value.size() : semi;
        const std::string one = value.substr(cursor, end - cursor);
        if (!one.empty() && !mime_in(one, which)) {
            kept += one;
            kept += ';';
        }
        if (semi == std::string::npos)
            break;
        cursor = semi + 1;
    }
    emptied = mime_list_empty(kept);
    if (kept == value)
        return {std::move(entry), false};
    entry.replace(value_at, value_end - value_at, kept);
    return {std::move(entry), true};
}

/// Takes the MIME types in `which` out of a shared-mime-info package.
///
/// @param[in,out] package the package's text
/// @param which the file types to take out
/// @return true when a mime-type element was removed
bool drop_mime_blocks(std::string& package, TypeSet which) {
    bool changed = false;
    for (const FileType& type : file_types) {
        if (!which.contains(type.extension))
            continue;
        const std::string marker = "<mime-type type=\"" + std::string(type.mime_type) + "\"";
        const std::string close = "</mime-type>";
        while (true) {
            const std::size_t start = package.find(marker);
            if (start == std::string::npos)
                break;
            const std::size_t close_at = package.find(close, start);
            if (close_at == std::string::npos)
                break;
            std::size_t end = close_at + close.size();
            if (end < package.size() && package[end] == '\r')
                ++end;
            if (end < package.size() && package[end] == '\n')
                ++end;
            std::size_t begin = start;
            while (begin > 0 && (package[begin - 1] == ' ' || package[begin - 1] == '\t'))
                --begin;
            package.erase(begin, end - begin);
            changed = true;
        }
    }
    return changed;
}

/// Reports whether a package still names a MIME type.
///
/// @param package the package's text
/// @return true when a mime-type element remains
bool package_lists_type(std::string_view package) {
    return package.find("<mime-type ") != std::string_view::npos;
}

#if OA_PROCESS_SPAWNING
/// Joins a program and its arguments with spaces, for a line of the log.
///
/// @param arguments the program and its arguments
/// @return the command as one line
std::string command_line(const std::vector<std::string>& arguments) {
    std::string line;
    for (const std::string& argument : arguments) {
        if (!line.empty())
            line += ' ';
        line += argument;
    }
    return line;
}

/// Runs a tool found on PATH with its arguments, without a shell, its standard streams on
/// /dev/null, and waits for it, at most `limit_ms` milliseconds; a tool still running then is
/// killed. What came of it is added to `done`'s lines: a tool that is not installed is no
/// error.
///
/// @param arguments the tool and its arguments
/// @param limit_ms the most milliseconds it may run
/// @param[in,out] done the registration's record
/// @return true when it ran and ended with status 0, or ended with a status the system did
///         not keep
bool run_tool(const std::vector<std::string>& arguments, uint32_t limit_ms, Registration& done) {
    const std::string& tool = arguments.front();
    std::vector<char*> argument_list;
    argument_list.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argument_list.push_back(const_cast<char*>(argument.c_str()));
    argument_list.push_back(nullptr);

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    if (posix_spawn_file_actions_init(&actions) != 0) {
        done.lines.push_back("cannot start " + tool);
        return false;
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        done.lines.push_back("cannot start " + tool);
        return false;
    }
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    // The tool starts with no signal blocked and every signal's default action, whatever
    // the game set for itself.
    sigset_t no_signals{};
    sigset_t every_signal{};
    sigemptyset(&no_signals);
    sigfillset(&every_signal);
    posix_spawnattr_setsigmask(&attributes, &no_signals);
    posix_spawnattr_setsigdefault(&attributes, &every_signal);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t child = 0;
    const int started =
        posix_spawnp(&child, tool.c_str(), &actions, &attributes, argument_list.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (started == ENOENT) {
        done.lines.push_back(tool + " is not installed");
        return false;
    }
    if (started != 0) {
        done.lines.push_back("cannot start " + tool + ": " + std::strerror(started));
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
    int status = 0;
    while (true) {
        const pid_t ended = waitpid(child, &status, WNOHANG);
        if (ended == child)
            break;
        if (ended < 0 && errno == EINTR)
            continue;
        if (ended < 0) {
            // Children are reaped elsewhere when SIGCHLD is ignored: the tool has ended, and
            // counts as run, so that it is not run again at every start.
            done.lines.push_back("ran " + command_line(arguments) + "; its status is unknown");
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            done.lines.push_back(
                tool + " did not finish in " + std::to_string(limit_ms) + " ms and was stopped"
            );
            return false;
        }
        base::threads::sleep_ms(tool_poll_ms);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        done.lines.push_back("ran " + command_line(arguments));
        return true;
    }
    if (WIFEXITED(status))
        done.lines.push_back(tool + " ended with status " + std::to_string(WEXITSTATUS(status)));
    else if (WIFSIGNALED(status))
        done.lines.push_back(tool + " was ended by signal " + std::to_string(WTERMSIG(status)));
    else
        done.lines.push_back(tool + " ended abnormally");
    return false;
}
#endif

/// Runs a database tool whose run is due, where the build may start other programs, and
/// leaves its stamp once a run succeeds; without the stamp, a later start runs it again.
///
/// @param arguments the tool and its arguments
/// @param stamp the tool's stamp file
/// @param places whether and how long tools run
/// @param[in,out] done the registration's record
void run_database_tool(
    const std::vector<std::string>& arguments,
    const fs::path& stamp,
    const Places& places,
    Registration& done
) {
    if (!places.run_tools)
        return;
#if OA_PROCESS_SPAWNING
    if (!run_tool(arguments, places.tool_time_limit_ms, done))
        return;
    std::string why;
    if (write_if_different(stamp, {}, {}, why) == Written::failed)
        done.lines.push_back(why);
#else
    // No run is made, and a start that wrote nothing says nothing of it.
    static_cast<void>(stamp);
    if (done.changed)
        done.lines.push_back(
            "this build starts no other programs; " + arguments.front() + " was not run"
        );
#endif
}

/// Tells whether a database tool's run is due: its stamp is missing, as after a change to
/// the files it reads, or a run that failed, was stopped or was not made.
///
/// @param stamp the tool's stamp file
/// @return true when the stamp is missing
bool run_due(const fs::path& stamp) {
    std::error_code error;
    return !fs::exists(fs::symlink_status(stamp, error));
}

/// The folders and stamp files one data folder's registration uses.
struct DataPlaces {
    fs::path mime_folder{};
    fs::path applications_folder{};
    fs::path hicolor_folder{};
    fs::path package_file{};
    fs::path entry_file{};        ///< the desktop entry of the six the game installs or adds
    fs::path launch_entry_file{}; ///< the desktop entry of launch files
    fs::path mime_stamp{};
    fs::path applications_stamp{};
    fs::path icon_stamp{};
    std::string name{};
};

/// Names the files a registration writes under a data folder.
///
/// @param data the XDG data folder
/// @return the files
DataPlaces data_places(const fs::path& data) {
    DataPlaces named{};
    named.mime_folder = data / "mime";
    named.applications_folder = data / "applications";
    named.hicolor_folder = data / "icons" / "hicolor";
    named.name = std::string(desktop_id);
    const std::string stamp_name = "." + named.name + std::string(stamp_suffix);
    named.mime_stamp = named.mime_folder / stamp_name;
    named.applications_stamp = named.applications_folder / stamp_name;
    named.icon_stamp = named.hicolor_folder / stamp_name;
    named.package_file = named.mime_folder / "packages" / (named.name + ".xml");
    named.entry_file = named.applications_folder / (named.name + ".desktop");
    named.launch_entry_file =
        named.applications_folder / (std::string(launch_desktop_id) + ".desktop");
    return named;
}

/// Runs each database tool whose stamp is missing.
///
/// @param named the files
/// @param refresh_icons the icon cache may hide a new or removed icon
/// @param places whether and how long tools run
/// @param[in,out] done the registration's record
void run_due_tools(
    const DataPlaces& named, bool refresh_icons, const Places& places, Registration& done
) {
    if (run_due(named.mime_stamp))
        run_database_tool(
            {"update-mime-database", detail::utf8_of(named.mime_folder)},
            named.mime_stamp,
            places,
            done
        );
    if (run_due(named.applications_stamp))
        run_database_tool(
            {"update-desktop-database", detail::utf8_of(named.applications_folder)},
            named.applications_stamp,
            places,
            done
        );
    // A cache another program made of the user's hicolor icons hides a new icon until it is
    // made again; without one, the icons are read from their folders.
    std::error_code error;
    if (refresh_icons && run_due(named.icon_stamp) &&
        fs::exists(named.hicolor_folder / "icon-theme.cache", error))
        run_database_tool(
            {"gtk-update-icon-cache",
             "--force",
             "--ignore-theme-index",
             "--quiet",
             detail::utf8_of(named.hicolor_folder)},
            named.icon_stamp,
            places,
            done
        );
}

/// Removes the mimetype icons of the file types in `which`. The program's icon stays.
///
/// @param named the files
/// @param which the file types
/// @param[out] why why one could not be removed, when one could not
/// @param[in,out] done the registration's record
/// @return written when an icon was removed, failed when one could not be
Written
remove_type_icons(const DataPlaces& named, TypeSet which, std::string& why, Registration& done) {
    Written result = Written::same;
    for (const FileType& type : file_types) {
        if (!which.contains(type.extension))
            continue;
        const Written removed = remove_if_present(
            type_icon(named.hicolor_folder, type.extension), named.icon_stamp, why, done
        );
        if (removed == Written::failed)
            return Written::failed;
        if (removed == Written::written)
            result = Written::written;
    }
    return result;
}

/// The registration itself; register_xdg catches what it throws.
///
/// @param executable the program, absolute
/// @param icon_png the icon's PNG bytes; empty writes no icon
/// @param places where it writes
/// @param types the file types to register
/// @return what was done
Registration register_in(
    const fs::path& executable,
    std::span<const uint8_t> icon_png,
    const Places& places,
    TypeSet types
) {
    Registration done{};
    done.supported = true;
    if (set_value(flatpak_variable) || set_value(snap_variable)) {
        done.lines.push_back("skipped: a Flatpak or Snap package registers its own file types");
        return done;
    }
    if (!executable.is_absolute()) {
        done.error = "the program's path " + detail::utf8_of(executable) + " is not absolute";
        return done;
    }
    if (const auto temporary = temporary_folder_of(executable)) {
        done.lines.push_back(
            "skipped: " + detail::utf8_of(executable) + " lies in the temporary folder " +
            detail::utf8_of(*temporary)
        );
        return done;
    }
    // A path a desktop entry cannot hold registers nothing, and removes nothing.
    if (desktop_exec_argument(detail::utf8_of(executable)).empty()) {
        done.lines.push_back(
            "skipped: the program's path holds a control character or is not "
            "UTF-8, which a desktop entry cannot hold"
        );
        return done;
    }
    std::string why;
    const auto data = data_folder(places, why);
    if (!data) {
        done.error = why;
        return done;
    }
    const DataPlaces named = data_places(*data);

    // The icons first, so that the desktop entry finds its icon once it is read: the
    // program's, and one for each file type that stays on. A type left out loses its icon.
    // The program's icon stays even when every type is off.
    bool icon_changed = false;
    if (!icon_png.empty()) {
        std::vector<fs::path> icons;
        icons.push_back(named.hicolor_folder / icon_size_folder / "apps" / (named.name + ".png"));
        for (const FileType& type : file_types) {
            if (types.contains(type.extension))
                icons.push_back(type_icon(named.hicolor_folder, type.extension));
        }
        for (const fs::path& icon : icons) {
            const Written written = write_if_different(icon, icon_png, named.icon_stamp, why);
            if (written == Written::failed) {
                done.error = why;
                return done;
            }
            if (written == Written::written) {
                icon_changed = true;
                done.lines.push_back("wrote " + detail::utf8_of(icon));
            }
        }
    }
    const Written removed_icons = remove_type_icons(named, types.complement(), why, done);
    if (removed_icons == Written::failed) {
        done.error = why;
        return done;
    }
    if (removed_icons == Written::written)
        icon_changed = true;

    // Each file is written while it lists a type, and removed once it would list none: the
    // MIME package with none of the seven, the entry of the six with none of them, and the
    // launch file's entry without it.
    const auto keep = [&](const fs::path& file, const std::string& text, const fs::path& stamp) {
        if (text.empty())
            return remove_if_present(file, stamp, why, done);
        const Written written = write_if_different(file, bytes_of(text), stamp, why);
        if (written == Written::written)
            done.lines.push_back("wrote " + detail::utf8_of(file));
        return written;
    };
    const Written package = keep(
        named.package_file, types.none() ? std::string{} : mime_package(types), named.mime_stamp
    );
    if (package == Written::failed) {
        done.error = why;
        return done;
    }
    const Written desktop = keep(
        named.entry_file,
        types.installs_any() ? desktop_entry(executable, types) : std::string{},
        named.applications_stamp
    );
    if (desktop == Written::failed) {
        done.error = why;
        return done;
    }
    const Written launch_desktop = keep(
        named.launch_entry_file,
        types.launch ? launch_desktop_entry(executable) : std::string{},
        named.applications_stamp
    );
    if (launch_desktop == Written::failed) {
        done.error = why;
        return done;
    }

    done.changed = icon_changed || package == Written::written || desktop == Written::written ||
                   launch_desktop == Written::written;
    // Each tool runs after a change to the files it reads, and at each later start until a
    // run succeeds and leaves its stamp.
    run_due_tools(named, !icon_png.empty() || icon_changed, places, done);
    return done;
}

/// The withdrawal itself; withdraw_xdg catches what it throws. It edits the files that are
/// there, so a type an earlier registration left out is not put back.
///
/// @param types the file types to withdraw
/// @param places where it writes
/// @return what was done
Registration withdraw_in(TypeSet types, const Places& places) {
    Registration done{};
    done.supported = true;
    if (types.none())
        return done;
    if (set_value(flatpak_variable) || set_value(snap_variable)) {
        done.lines.push_back("skipped: a Flatpak or Snap package registers its own file types");
        return done;
    }
    std::string why;
    const auto data = data_folder(places, why);
    if (!data) {
        done.error = why;
        return done;
    }
    const DataPlaces named = data_places(*data);
    const Written removed_icons = remove_type_icons(named, types, why, done);
    if (removed_icons == Written::failed) {
        done.error = why;
        return done;
    }
    bool icons_changed = removed_icons == Written::written;

    // Each desktop entry loses the types withdrawn, and goes once it lists none.
    const auto withdraw_entry = [&](const fs::path& file) {
        const TextFile entry_read = read_text(file, why);
        if (entry_read.state == Read::failed)
            return Written::failed;
        if (entry_read.state != Read::read)
            return Written::same;
        bool emptied = false;
        const std::pair<std::string, bool> revised =
            desktop_without(entry_read.text, types, emptied);
        if (emptied)
            return remove_if_present(file, named.applications_stamp, why, done);
        if (!revised.second)
            return Written::same;
        const Written written =
            write_if_different(file, bytes_of(revised.first), named.applications_stamp, why);
        if (written == Written::written)
            done.lines.push_back("wrote " + detail::utf8_of(file));
        return written;
    };
    const Written desktop = withdraw_entry(named.entry_file);
    if (desktop == Written::failed) {
        done.error = why;
        return done;
    }
    const Written launch_desktop = withdraw_entry(named.launch_entry_file);
    if (launch_desktop == Written::failed) {
        done.error = why;
        return done;
    }

    const TextFile package_read = read_text(named.package_file, why);
    if (package_read.state == Read::failed) {
        done.error = why;
        return done;
    }
    Written package = Written::same;
    if (package_read.state == Read::read) {
        std::string package_text = package_read.text;
        const bool dropped = drop_mime_blocks(package_text, types);
        if (!package_lists_type(package_text))
            package = remove_if_present(named.package_file, named.mime_stamp, why, done);
        else if (dropped)
            package = write_if_different(
                named.package_file, bytes_of(package_text), named.mime_stamp, why
            );
        if (package == Written::failed) {
            done.error = why;
            return done;
        }
        if (package == Written::written && package_lists_type(package_text))
            done.lines.push_back("wrote " + detail::utf8_of(named.package_file));
    }

    done.changed = icons_changed || desktop == Written::written ||
                   launch_desktop == Written::written || package == Written::written;
    run_due_tools(named, icons_changed, places, done);
    return done;
}

} // namespace

Registration register_xdg(
    const fs::path& executable,
    std::span<const uint8_t> icon_png,
    const Places& places,
    const TypeSet types
) {
    try {
        return register_in(executable, icon_png, places, types);
    } catch (const std::exception& error) {
        Registration failed{};
        failed.supported = true;
        failed.error = std::string("the file types could not be registered: ") + error.what();
        return failed;
    }
}

Registration withdraw_xdg(const TypeSet types, const Places& places) {
    try {
        return withdraw_in(types, places);
    } catch (const std::exception& error) {
        Registration failed{};
        failed.supported = true;
        failed.error = std::string("the file types could not be withdrawn: ") + error.what();
        return failed;
    }
}

} // namespace oa::platform::file_types
