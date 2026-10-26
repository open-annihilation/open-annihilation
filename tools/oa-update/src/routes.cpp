// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/routes.hpp"

#include "oa/update_helper/paths.hpp"

#include <algorithm>
#include <utility>

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;

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

} // namespace

std::vector<Operation>
plan_operations(const oa::app::update::Plan& plan, const fs::path& staged, std::string* why) {
    if (why != nullptr)
        why->clear();
    if (plan.route == oa::app::update::Route::linux_versions)
        return linux_operations(plan, staged, why);
    if (plan.route == oa::app::update::Route::check_folder)
        return folder_operations(plan, staged, why);
    set_why(why, "this route is not carried out");
    return {};
}

} // namespace oa::update_helper
