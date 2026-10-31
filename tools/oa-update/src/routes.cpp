// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/routes.hpp"

#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/route_windows.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;

/// The script osascript runs: one shell command, with administrator
/// privileges, from the values after the script. The first value is the
/// prompt; the others are the program and its arguments, each quoted by
/// the script. The command's output is the program's exit status.
constexpr std::string_view elevation_script[] = {
    "on run argv",
    "set command to \"\"",
    "repeat with i from 2 to count of argv",
    "set command to command & quoted form of (item i of argv) & \" \"",
    "end repeat",
    "do shell script command & \"> /dev/null 2>&1; echo $?\" with prompt (item 1 of argv) "
    "with administrator privileges",
    "end run",
};

/// The text without the spaces and line ends around it.
///
/// @param text the text
/// @return the trimmed text
std::string_view trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

#if !defined(_WIN32)
/// True when this process may write `path`.
///
/// @param path the file or folder
/// @return true when access allows writing
bool writable(const fs::path& path) {
    return !path.empty() && ::access(path.c_str(), W_OK) == 0;
}

/// True when `entry` may leave `folder`: a sticky folder lets only its own
/// owner, or the entry's owner, move an entry out.
///
/// @param folder the folder that holds the entry
/// @param entry the entry
/// @return true when the folder's sticky rule allows the move
bool may_leave(const fs::path& folder, const fs::path& entry) {
    struct stat folder_info{};
    if (::stat(folder.c_str(), &folder_info) != 0)
        return false;
    if ((folder_info.st_mode & S_ISVTX) == 0)
        return true;
    struct stat entry_info{};
    if (::lstat(entry.c_str(), &entry_info) != 0)
        return false;
    const uid_t user = ::geteuid();
    return folder_info.st_uid == user || entry_info.st_uid == user;
}
#endif

void set_why(std::string* why, std::string message) {
    if (why != nullptr)
        *why = std::move(message);
}

bool list_children(const fs::path& folder, std::vector<fs::path>& children, std::string* why) {
    std::error_code io;
    fs::directory_iterator it(folder, io);
    while (!io && it != fs::directory_iterator()) {
        children.push_back(it->path());
        it.increment(io);
    }
    if (io) {
        set_why(why, "a folder could not be read");
        return false;
    }
    std::sort(children.begin(), children.end(), [](const fs::path& left, const fs::path& right) {
        return utf8_of(left.filename()) < utf8_of(right.filename());
    });
    return true;
}

bool ends_with_app(const fs::path& path) {
    const std::string name = utf8_of(path.filename());
    return name.size() >= 4 && name.compare(name.size() - 4, 4, ".app") == 0;
}

std::vector<Operation>
folder_operations(const oa::app::update::Plan& plan, const fs::path& staged, std::string* why) {
    if (why != nullptr)
        why->clear();
    const fs::path install = path_from(plan.install);
    const fs::path previous = path_from(plan.updates) / "previous";
    std::error_code io;
    if (!fs::is_directory(install, io)) {
        set_why(why, "the install folder is missing");
        return {};
    }
    if (!fs::is_directory(staged, io)) {
        set_why(why, "the staging folder is missing");
        return {};
    }

    std::vector<fs::path> staged_children;
    if (!list_children(staged, staged_children, why))
        return {};
    if (staged_children.size() != 1 || !fs::is_directory(fs::symlink_status(staged_children[0]))) {
        set_why(why, "the staged package is not one folder");
        return {};
    }
    if (fs::is_symlink(fs::symlink_status(staged_children[0]))) {
        set_why(why, "the staged package is not one folder");
        return {};
    }

    std::vector<fs::path> install_children;
    if (!list_children(install, install_children, why))
        return {};
    std::vector<fs::path> top_children;
    if (!list_children(staged_children[0], top_children, why))
        return {};

    std::vector<Operation> operations;
    operations.reserve(install_children.size() + top_children.size());
    for (const fs::path& child : install_children) {
        Operation operation;
        operation.kind = Operation::Kind::rename;
        operation.from = child;
        operation.to = previous / child.filename();
        operations.push_back(std::move(operation));
    }
    for (const fs::path& child : top_children) {
        Operation operation;
        operation.kind = Operation::Kind::rename;
        operation.from = child;
        operation.to = install / child.filename();
        operations.push_back(std::move(operation));
    }
    return operations;
}

bool preferences_name(const fs::path& path) {
    return utf8_of(path.filename()) == "preferences.conf";
}

Operation rename_op(const fs::path& from, const fs::path& to) {
    Operation operation;
    operation.kind = Operation::Kind::rename;
    operation.from = from;
    operation.to = to;
    return operation;
}

Operation remove_op(const fs::path& path) {
    Operation operation;
    operation.kind = Operation::Kind::remove_tree;
    operation.path = path;
    return operation;
}

bool entry_there(const fs::path& path) {
    std::error_code io;
    return fs::exists(fs::symlink_status(path, io)) && !io;
}

std::vector<Operation>
folder_restore(const oa::app::update::State& state, const fs::path& install, std::string* why) {
    const fs::path kept = path_from(state.kept_path);
    std::error_code io;
    if (!fs::is_directory(install, io) || io) {
        set_why(why, "the install folder is missing");
        return {};
    }
    if (!fs::is_directory(kept, io) || io) {
        set_why(why, "the kept folder is missing");
        return {};
    }
    std::vector<fs::path> kept_children;
    if (!list_children(kept, kept_children, why))
        return {};
    std::vector<Operation> operations;
    for (const fs::path& child : kept_children) {
        if (preferences_name(child))
            continue;
        const fs::path destination = install / child.filename();
        if (destination == kept)
            continue;
        // A file the new version added, and that the kept copy does not
        // name, stays. Saves and the player's own files are not in this folder.
        if (entry_there(destination))
            operations.push_back(remove_op(destination));
        operations.push_back(rename_op(child, destination));
    }
    return operations;
}

std::vector<Operation>
macos_restore(const oa::app::update::State& state, const fs::path& install, std::string* why) {
    const fs::path kept_path = path_from(state.kept_path);
    const fs::path kept = ends_with_app(kept_path) ? kept_path : kept_path / install.filename();
    std::error_code io;
    if (!fs::is_directory(kept, io) || io || fs::is_symlink(fs::symlink_status(kept))) {
        set_why(why, "the kept bundle is missing");
        return {};
    }
    std::vector<Operation> operations;
    if (entry_there(install))
        operations.push_back(remove_op(install));
    operations.push_back(rename_op(kept, install));
    return operations;
}

} // namespace

bool macos_staged_application(const fs::path& staged, fs::path& application, std::string* why) {
    std::vector<fs::path> staged_children;
    if (!list_children(staged, staged_children, why))
        return false;
    if (staged_children.size() != 1 || !fs::is_directory(fs::symlink_status(staged_children[0])) ||
        fs::is_symlink(fs::symlink_status(staged_children[0]))) {
        set_why(why, "the staged package is not one folder");
        return false;
    }
    std::vector<fs::path> top_children;
    if (!list_children(staged_children[0], top_children, why))
        return false;
    const fs::path* found = nullptr;
    for (const fs::path& child : top_children) {
        if (!ends_with_app(child))
            continue;
        if (fs::is_symlink(fs::symlink_status(child)) ||
            !fs::is_directory(fs::symlink_status(child))) {
            set_why(why, "the staged package is not one application");
            return false;
        }
        if (found != nullptr) {
            set_why(why, "the staged package is not one application");
            return false;
        }
        found = &child;
    }
    if (found == nullptr) {
        set_why(why, "the staged package is not one application");
        return false;
    }
    application = *found;
    return true;
}

MacosDecision
macos_may_swap(const BundleSignature& running, const BundleSignature& staged, bool test_key) {
    MacosDecision decision;
    const bool running_ad_hoc = running.team.empty();
    const bool staged_ad_hoc = staged.team.empty();
    if (running_ad_hoc || staged_ad_hoc) {
        if (test_key && running_ad_hoc && staged_ad_hoc && running.valid && staged.valid) {
            decision.go = true;
            return decision;
        }
        decision.why = test_key && running_ad_hoc && staged_ad_hoc
                           ? "the new bundle's signature is not valid"
                           : "an ad hoc signed build does not update in place";
        return decision;
    }
    if (!running.valid || !staged.valid) {
        decision.why = "the new bundle's signature is not valid";
        return decision;
    }
    if (running.team != staged.team) {
        decision.why = "the new bundle is signed by another team";
        return decision;
    }
    if (!staged.notarized) {
        decision.why = "the new bundle is not notarized";
        return decision;
    }
    decision.go = true;
    return decision;
}

MacosLaunchDecision macos_launch_decision(const MacosLaunchSeen& seen) {
    MacosLaunchDecision decision;
    if (seen.callback_started && seen.callback_pid > 0) {
        decision.launch = MacosLaunch::started;
        decision.pid = seen.callback_pid;
        return decision;
    }
    if (seen.application_listed && seen.listed_pid > 0) {
        decision.launch = MacosLaunch::started;
        decision.pid = seen.listed_pid;
        return decision;
    }
    if (seen.version_confirmed) {
        decision.launch = MacosLaunch::started;
        return decision;
    }
    if (seen.callback_finished && !seen.callback_started)
        decision.launch = MacosLaunch::failed;
    return decision;
}

std::vector<Operation> macos_bundle_operations(
    const oa::app::update::Plan& plan, const fs::path& staged, MacosVolume volume, std::string* why
) {
    if (why != nullptr)
        why->clear();
    if (plan.installer) {
        if (plan.file.empty()) {
            set_why(why, "the installer is missing");
            return {};
        }
        Operation open;
        open.kind = Operation::Kind::run_and_wait;
        open.program = "/usr/bin/open";
        open.arguments.push_back(plan.file);
        return {std::move(open)};
    }

    const fs::path install = path_from(plan.install);
    std::error_code io;
    if (!fs::is_directory(install, io) || io || fs::is_symlink(fs::symlink_status(install))) {
        set_why(why, "the installed bundle is missing");
        return {};
    }
    fs::path application;
    if (!macos_staged_application(staged, application, why))
        return {};

    const fs::path previous = macos_kept_bundle(path_from(plan.updates), install);
    std::vector<Operation> operations;
    if (fs::exists(previous, io) && !io) {
        Operation remove_kept;
        remove_kept.kind = Operation::Kind::remove_tree;
        remove_kept.path = previous;
        operations.push_back(std::move(remove_kept));
    }
    if (volume == MacosVolume::exdev) {
        Operation copy_old;
        copy_old.kind = Operation::Kind::copy_tree;
        copy_old.from = install;
        copy_old.to = previous;
        operations.push_back(std::move(copy_old));
        Operation drop_old;
        drop_old.kind = Operation::Kind::remove_tree;
        drop_old.path = install;
        operations.push_back(std::move(drop_old));
        Operation copy_new;
        copy_new.kind = Operation::Kind::copy_tree;
        copy_new.from = application;
        copy_new.to = install;
        operations.push_back(std::move(copy_new));
        return operations;
    }
    Operation move_old;
    move_old.kind = Operation::Kind::rename;
    move_old.from = install;
    move_old.to = previous;
    operations.push_back(std::move(move_old));
    Operation move_new;
    move_new.kind = Operation::Kind::rename;
    move_new.from = application;
    move_new.to = install;
    operations.push_back(std::move(move_new));
    return operations;
}

bool macos_swap_needs_elevation(const MacosSwapAccess& access) noexcept {
    return !access.parent_writable || !access.install_movable || !access.kept_movable;
}

MacosSwapAccess macos_swap_access(const fs::path& install, const fs::path& kept) {
    MacosSwapAccess access;
#if defined(_WIN32)
    (void)install;
    (void)kept;
    access.parent_writable = true;
    access.install_movable = true;
#else
    const fs::path parent = install.parent_path();
    access.parent_writable = writable(parent) && may_leave(parent, install);
    access.install_movable = writable(install);
    struct stat kept_info{};
    if (::lstat(kept.c_str(), &kept_info) == 0)
        access.kept_movable = writable(kept) && may_leave(kept.parent_path(), kept);
#endif
    return access;
}

MovedOwner moved_tree_owner(const fs::path& updates, const fs::path& install, const fs::path& to) {
    const fs::path normal = to.lexically_normal();
    if (!install.empty() && normal == install.lexically_normal())
        return MovedOwner::install;
    if (updates.empty())
        return MovedOwner::unchanged;
    const fs::path inside = normal.lexically_relative(updates.lexically_normal());
    if (inside.empty() || inside == "." || *inside.begin() == "..")
        return MovedOwner::unchanged;
    return MovedOwner::player;
}

fs::path macos_kept_bundle(const fs::path& updates, const fs::path& install) {
    return updates / "previous" / install.filename();
}

std::vector<std::string> macos_elevation_command(
    std::string_view prompt, const fs::path& helper, const std::vector<std::string>& arguments
) {
    std::vector<std::string> command{"/usr/bin/osascript"};
    for (const std::string_view line : elevation_script) {
        command.emplace_back("-e");
        command.emplace_back(line);
    }
    // The prompt comes first: osascript reads options until the first value
    // that is not one, and the helper's arguments begin with "--".
    command.emplace_back(prompt);
    command.push_back(utf8_of(helper));
    command.insert(command.end(), arguments.begin(), arguments.end());
    return command;
}

ElevatedRelaunch macos_elevation_outcome(
    bool started, int status, std::string_view output, std::string_view errors
) {
    ElevatedRelaunch result;
    if (!started) {
        result.error = "the administrator's permission could not be asked";
        return result;
    }
    if (status != 0) {
        // -128 is the prompt's Cancel, whatever language the system shows.
        if (errors.find("(-128)") != std::string_view::npos) {
            result.cancelled = true;
            return result;
        }
        result.error = "the administrator's permission could not be asked";
        const std::string_view said = trimmed(errors);
        if (!said.empty())
            result.error += ": " + std::string(said.substr(0, said.find('\n')));
        return result;
    }
    const std::string_view text = trimmed(output);
    int code = 0;
    const std::from_chars_result parsed =
        std::from_chars(text.data(), text.data() + text.size(), code);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        result.error = "the copy with an administrator's rights did not say how it ended";
        return result;
    }
    result.ran = true;
    result.code = code;
    return result;
}

#if !defined(__APPLE__)
std::vector<Operation>
macos_checked_restore(const oa::app::update::Plan& plan, bool test_key, std::string* why) {
    (void)plan;
    (void)test_key;
    set_why(why, "this route is not carried out");
    return {};
}

std::vector<Operation> macos_checked_operations(
    const oa::app::update::Plan& plan, const fs::path& staged, bool test_key, std::string* why
) {
    (void)plan;
    (void)staged;
    (void)test_key;
    set_why(why, "this route is not carried out");
    return {};
}
#endif

std::vector<Operation>
plan_operations(const oa::app::update::Plan& plan, const fs::path& staged, std::string* why) {
    if (why != nullptr)
        why->clear();
    if (plan.route == oa::app::update::Route::windows_installer)
        return windows_installer_operations(plan, staged, why);
    if (plan.route == oa::app::update::Route::windows_folder)
        return windows_folder_operations(plan, staged, why);
    if (plan.route == oa::app::update::Route::linux_versions)
        return linux_operations(plan, staged, why);
    if (plan.route == oa::app::update::Route::check_folder)
        return folder_operations(plan, staged, why);
    set_why(why, "this route is not carried out");
    return {};
}

std::vector<Operation> restore_operations(
    const oa::app::update::State& state,
    const fs::path& install,
    std::string_view bad_version,
    std::string* why
) {
    if (why != nullptr)
        why->clear();
    if (state.kept_version.empty()) {
        set_why(why, "nothing is kept");
        return {};
    }
    switch (state.kept_route) {
    case oa::app::update::Route::check_folder:
        return folder_restore(state, install, why);
    case oa::app::update::Route::macos_bundle:
        return macos_restore(state, install, why);
    case oa::app::update::Route::windows_installer:
        return windows_installer_restore(state, install, bad_version, why);
    case oa::app::update::Route::windows_folder:
        return windows_folder_restore(state, install, why);
    case oa::app::update::Route::linux_versions:
        return linux_restore_operations(state, install, why);
    }
    set_why(why, "this route is not carried out");
    return {};
}

} // namespace oa::update_helper
