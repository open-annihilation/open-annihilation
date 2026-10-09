// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The XDG registration: the MIME package, the desktop entry and the icons in the user's data
// folder, and the database tools run after a change, without a shell, and at later starts
// until a run succeeds.

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
/// The MIME type's icon name: its type with the '/' a '-'.
constexpr std::string_view type_icon_name = "application-x-oamod";
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

/// The registration itself; register_xdg catches what it throws.
///
/// @param executable the program, absolute
/// @param icon_png the icon's PNG bytes; empty writes no icon
/// @param places where it writes
/// @return what was done
Registration
register_in(const fs::path& executable, std::span<const uint8_t> icon_png, const Places& places) {
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
    const std::string entry = desktop_entry(executable);
    if (entry.empty()) {
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
    const fs::path mime_folder = *data / "mime";
    const fs::path applications_folder = *data / "applications";
    const fs::path hicolor_folder = *data / "icons" / "hicolor";
    const std::string name(desktop_id);
    const std::string stamp_name = "." + name + std::string(stamp_suffix);
    const fs::path mime_stamp = mime_folder / stamp_name;
    const fs::path applications_stamp = applications_folder / stamp_name;
    const fs::path icon_stamp = hicolor_folder / stamp_name;

    // The icons first, so that the desktop entry finds its icon once it is read.
    bool icon_written = false;
    if (!icon_png.empty()) {
        const fs::path icons[] = {
            hicolor_folder / icon_size_folder / "apps" / (name + ".png"),
            hicolor_folder / icon_size_folder / "mimetypes" /
                (std::string(type_icon_name) + ".png"),
        };
        for (const fs::path& icon : icons) {
            const Written written = write_if_different(icon, icon_png, icon_stamp, why);
            if (written == Written::failed) {
                done.error = why;
                return done;
            }
            if (written == Written::written) {
                icon_written = true;
                done.lines.push_back("wrote " + detail::utf8_of(icon));
            }
        }
    }
    const fs::path package_file = mime_folder / "packages" / (name + ".xml");
    const Written package =
        write_if_different(package_file, bytes_of(mime_package()), mime_stamp, why);
    if (package == Written::failed) {
        done.error = why;
        return done;
    }
    if (package == Written::written)
        done.lines.push_back("wrote " + detail::utf8_of(package_file));
    const fs::path entry_file = applications_folder / (name + ".desktop");
    const Written desktop =
        write_if_different(entry_file, bytes_of(entry), applications_stamp, why);
    if (desktop == Written::failed) {
        done.error = why;
        return done;
    }
    if (desktop == Written::written)
        done.lines.push_back("wrote " + detail::utf8_of(entry_file));

    done.changed = icon_written || package == Written::written || desktop == Written::written;
    // Each tool runs after a change to the files it reads, and at each later start until a
    // run succeeds and leaves its stamp.
    if (run_due(mime_stamp))
        run_database_tool(
            {"update-mime-database", detail::utf8_of(mime_folder)}, mime_stamp, places, done
        );
    if (run_due(applications_stamp))
        run_database_tool(
            {"update-desktop-database", detail::utf8_of(applications_folder)},
            applications_stamp,
            places,
            done
        );
    // A cache another program made of the user's hicolor icons hides a new icon until it is
    // made again; without one, the icons are read from their folders.
    std::error_code error;
    if (!icon_png.empty() && run_due(icon_stamp) &&
        fs::exists(hicolor_folder / "icon-theme.cache", error))
        run_database_tool(
            {"gtk-update-icon-cache",
             "--force",
             "--ignore-theme-index",
             "--quiet",
             detail::utf8_of(hicolor_folder)},
            icon_stamp,
            places,
            done
        );
    return done;
}

} // namespace

Registration
register_xdg(const fs::path& executable, std::span<const uint8_t> icon_png, const Places& places) {
    try {
        return register_in(executable, icon_png, places);
    } catch (const std::exception& error) {
        Registration failed{};
        failed.supported = true;
        failed.error = std::string("the file types could not be registered: ") + error.what();
        return failed;
    }
}

} // namespace oa::platform::file_types
