// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Linux and Steam Deck route. The first update turns a flat folder into
// versions/<old>, versions/<new>, a current link and the launcher. Later
// updates stage versions/<new> and a next link. The launcher swaps the link.
#include "oa/update_helper/routes.hpp"

#include "linux_launcher.hpp"
#include "oa/update_helper/paths.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;

/// Names a flat package may hold at the top of the install, besides whatever
/// the new package's own top folder lists. A player's own file is not here.
constexpr std::string_view known_names[] = {
    "ATTRIBUTIONS.md",
    "LICENSE",
    "brains",
    "fonts",
    "languages",
    "licenses",
    "oa-intro",
    "oa-tool",
    "oa-update",
    "open-annihilation",
    "registries",
    "steam-deck",
};

void set_why(std::string* why, std::string message) {
    if (why != nullptr)
        *why = std::move(message);
}

/// True when `name` is one path component and not `.` or `..`.
///
/// @param name the component
/// @return true when it can be a version folder's name
bool single_component(std::string_view name) {
    if (name.empty() || name == "." || name == "..")
        return false;
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos)
        return false;
    return true;
}

/// True when `path` is present, without following a link.
///
/// @param path the path
/// @return true when a directory entry is there
bool entry_exists(const fs::path& path) {
    std::error_code io;
    const fs::file_status status = fs::symlink_status(path, io);
    return !io && fs::exists(status);
}

/// True when `path` is a real directory, not a link.
///
/// @param path the path
/// @return true when it is a directory
bool real_directory(const fs::path& path) {
    std::error_code io;
    const fs::file_status status = fs::symlink_status(path, io);
    return !io && fs::is_directory(status);
}

/// The version in `open-annihilation-<version>-<package>`, or in the
/// release's own spelling, `open-annihilation-v<version>-<package>`.
///
/// @param top the staged top folder's name
/// @param package the plan's package id
/// @return the version, without the `v`, or empty when the name is not that shape
std::string version_from_top(std::string_view top, std::string_view package) {
    constexpr std::string_view prefix = "open-annihilation-";
    if (!top.starts_with(prefix) || !single_component(package))
        return {};
    const std::string suffix = std::string("-") + std::string(package);
    if (top.size() <= prefix.size() + suffix.size() || !top.ends_with(suffix))
        return {};
    std::string_view version =
        top.substr(prefix.size(), top.size() - prefix.size() - suffix.size());
    // The versions folder is named as the feed names the version, so the
    // next update finds it under that name.
    if (version.size() > 1 && version.front() == 'v' && version[1] >= '0' && version[1] <= '9')
        version.remove_prefix(1);
    if (!single_component(version))
        return {};
    return std::string(version);
}

/// The names of `folder`'s entries, sorted. A link is not followed.
///
/// @param folder the folder
/// @param[out] children the entries
/// @param[out] why why the folder could not be read; may be null
/// @return false when the folder could not be read
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

/// Adds `name` once, skipping the links and the versions folder.
///
/// @param names the names gathered so far
/// @param name the candidate
void consider_name(std::vector<std::string>& names, const std::string& name) {
    if (name == "current" || name == "next" || name == "versions")
        return;
    if (std::find(names.begin(), names.end(), name) != names.end())
        return;
    names.push_back(name);
}

/// One rename of an install entry into `versions/<old>/`.
///
/// @param from the install entry
/// @param to where it moves
/// @return the operation
Operation rename_to(const fs::path& from, const fs::path& to) {
    Operation operation;
    operation.kind = Operation::Kind::rename;
    operation.from = from;
    operation.to = to;
    return operation;
}

/// One copy of a tree. The destination must not exist.
///
/// @param from the source
/// @param to the destination
/// @return the operation
Operation copy_to(const fs::path& from, const fs::path& to) {
    Operation operation;
    operation.kind = Operation::Kind::copy_tree;
    operation.from = from;
    operation.to = to;
    return operation;
}

/// Moves `path` aside so it can be put back.
///
/// @param path the file or folder
/// @return the operation
Operation move_aside(const fs::path& path) {
    Operation operation;
    operation.kind = Operation::Kind::remove_tree;
    operation.path = path;
    return operation;
}

/// Makes `path` a link to `target`.
///
/// @param path where the link is made
/// @param target the relative target
/// @return the operation
Operation link_to(const fs::path& path, const std::string& target) {
    Operation operation;
    operation.kind = Operation::Kind::make_link;
    operation.path = path;
    operation.link_target = target;
    return operation;
}

} // namespace

std::string_view linux_launcher_script() noexcept {
    return linux_launcher_text;
}

std::vector<Operation>
linux_operations(const oa::app::update::Plan& plan, const fs::path& staged, std::string* why) {
    if (why != nullptr)
        why->clear();
    const fs::path install = path_from(plan.install);
    std::error_code io;
    if (!fs::is_directory(install, io)) {
        set_why(why, "the install folder is missing");
        return {};
    }
    if (!fs::is_directory(staged, io)) {
        set_why(why, "the staging folder is missing");
        return {};
    }
    if (!single_component(plan.from_version)) {
        set_why(why, "the kept version's name cannot be used");
        return {};
    }

    std::vector<fs::path> staged_children;
    if (!list_children(staged, staged_children, why))
        return {};
    if (staged_children.size() != 1 || !real_directory(staged_children[0])) {
        set_why(why, "the staged package is not one folder");
        return {};
    }
    const fs::path staged_top = staged_children[0];
    const std::string version = version_from_top(utf8_of(staged_top.filename()), plan.package);
    if (version.empty()) {
        set_why(why, "the staged folder has no version");
        return {};
    }
    if (version == plan.from_version) {
        set_why(why, "the new version is the one already running");
        return {};
    }

    const fs::path versions = install / "versions";
    if (entry_exists(versions) && !real_directory(versions)) {
        set_why(why, "versions is not a folder");
        return {};
    }
    const fs::path current = install / "current";
    const fs::file_status current_status = fs::symlink_status(current, io);
    const bool current_exists = !io && fs::exists(current_status);
    const bool later = current_exists && fs::is_symlink(current_status);
    if (current_exists && !later) {
        set_why(why, "current is not a link");
        return {};
    }

    const fs::path old_dir = versions / path_from(plan.from_version);
    const fs::path new_dir = versions / path_from(version);
    if (entry_exists(new_dir)) {
        set_why(why, "the new version is already there");
        return {};
    }
    if (!later && entry_exists(old_dir)) {
        set_why(why, "the kept version is already there");
        return {};
    }
    if (later) {
        const fs::path pointed = fs::read_symlink(current, io);
        if (io || !single_component(utf8_of(pointed.filename()))) {
            set_why(why, "current could not be read");
            return {};
        }
    }

    std::vector<fs::path> top_children;
    if (!list_children(staged_top, top_children, why))
        return {};
    std::vector<std::string> names;
    for (const fs::path& child : top_children)
        consider_name(names, utf8_of(child.filename()));
    for (const std::string_view known : known_names)
        consider_name(names, std::string(known));
    std::sort(names.begin(), names.end());

    std::vector<Operation> operations;
    if (!later) {
        for (const std::string& name : names) {
            const fs::path from = install / path_from(name);
            if (!entry_exists(from))
                continue;
            operations.push_back(rename_to(from, old_dir / path_from(name)));
        }
        Operation launcher;
        launcher.kind = Operation::Kind::write_file;
        launcher.path = install / "open-annihilation.new";
        launcher.bytes = std::string(linux_launcher_text);
        launcher.executable = true;
        operations.push_back(std::move(launcher));
        operations.push_back(
            rename_to(install / "open-annihilation.new", install / "open-annihilation")
        );
        operations.push_back(link_to(current, std::string("versions/") + plan.from_version));
    }

    operations.push_back(copy_to(staged_top, new_dir));
    const fs::path staged_deck = staged_top / "steam-deck";
    if (real_directory(staged_deck)) {
        operations.push_back(move_aside(new_dir / "steam-deck"));
        // The first update already renamed a top-level steam-deck into the
        // kept version. A later update leaves it at the top, so replace it.
        const fs::path top_deck = install / "steam-deck";
        if (later && entry_exists(top_deck))
            operations.push_back(move_aside(top_deck));
        operations.push_back(copy_to(staged_deck, top_deck));
    }

    const std::string next_target = std::string("versions/") + version;
    const fs::path next = install / "next";
    const fs::file_status next_status = fs::symlink_status(next, io);
    if (!io && fs::is_symlink(next_status)) {
        Operation replace;
        replace.kind = Operation::Kind::replace_link;
        replace.path = next;
        replace.link_target = next_target;
        operations.push_back(std::move(replace));
    } else if (!io && fs::exists(next_status)) {
        operations.push_back(move_aside(next));
        operations.push_back(link_to(next, next_target));
    } else {
        operations.push_back(link_to(next, next_target));
    }

    std::vector<std::string> keep;
    keep.push_back(version);
    keep.push_back(plan.from_version);
    if (later) {
        const fs::path pointed = fs::read_symlink(current, io);
        keep.push_back(utf8_of(pointed.filename()));
    }
    if (real_directory(versions)) {
        std::vector<fs::path> present;
        if (!list_children(versions, present, why))
            return {};
        for (const fs::path& child : present) {
            if (!real_directory(child))
                continue;
            const std::string name = utf8_of(child.filename());
            if (std::find(keep.begin(), keep.end(), name) != keep.end())
                continue;
            operations.push_back(move_aside(child));
        }
    }
    return operations;
}

std::vector<Operation> linux_restore_operations(
    const oa::app::update::State& state, const fs::path& install, std::string* why
) {
    if (why != nullptr)
        why->clear();
    std::error_code io;
    if (!fs::is_directory(install, io)) {
        set_why(why, "the install folder is missing");
        return {};
    }
    const fs::path kept = path_from(state.kept_path);
    if (!real_directory(kept)) {
        set_why(why, "the kept folder is missing");
        return {};
    }
    const std::string name = utf8_of(kept.filename());
    if (!single_component(name)) {
        set_why(why, "the kept version's name cannot be used");
        return {};
    }
    const std::string target = std::string("versions/") + name;
    const fs::path current = install / "current";
    const fs::file_status status = fs::symlink_status(current, io);
    const bool exists = !io && fs::exists(status);
    std::vector<Operation> operations;
    if (exists && fs::is_symlink(status)) {
        Operation replace;
        replace.kind = Operation::Kind::replace_link;
        replace.path = current;
        replace.link_target = target;
        operations.push_back(std::move(replace));
    } else if (exists) {
        operations.push_back(move_aside(current));
        operations.push_back(link_to(current, target));
    } else {
        operations.push_back(link_to(current, target));
    }
    const fs::path starts = install / "update-starts";
    if (entry_exists(starts))
        operations.push_back(move_aside(starts));
    return operations;
}

} // namespace oa::update_helper
