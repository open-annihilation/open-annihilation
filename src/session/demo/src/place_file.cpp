// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/place_file.hpp"

#include "oa/platform/file_copy.hpp"

#include <string>
#include <system_error>

namespace oa::session::demo {

namespace {

namespace fs = std::filesystem;

/// The most names tried for a file before one is said not to be free.
constexpr int most_names_tried = 10000;

/// Names the nth file of a name: the name itself for the first, then the stem with " (2)", " (3)"
/// and so on before the extension.
///
/// @param to the name wanted
/// @param copy 1 for the name itself
/// @return the path
fs::path numbered(const fs::path& to, int copy) {
    if (copy <= 1)
        return to;
    fs::path stem = to.stem();
    stem += " (" + std::to_string(copy) + ")";
    stem += to.extension();
    return to.parent_path() / stem;
}

/// Renames a file, through the hook when there is one.
///
/// @param from the file
/// @param to its new name
/// @param hooks the stand-ins
/// @param[out] failure why it could not
void rename_file(
    const fs::path& from, const fs::path& to, const PlaceHooks& hooks, std::error_code& failure
) {
    if (hooks.rename != nullptr)
        hooks.rename(hooks.context, from, to, failure);
    else
        fs::rename(from, to, failure);
}

/// Links a file to a second name, through the hook when there is one.
///
/// @param from the file
/// @param to the second name
/// @param hooks the stand-ins
/// @param[out] failure why it could not
void link_file(
    const fs::path& from, const fs::path& to, const PlaceHooks& hooks, std::error_code& failure
) {
    if (hooks.link != nullptr)
        hooks.link(hooks.context, from, to, failure);
    else
        fs::create_hard_link(from, to, failure);
}

/// Words the reason a file could not be moved.
///
/// @param failure why
/// @return the sentence
std::string refusal(const std::error_code& failure) {
    return "the file cannot be moved into place: " + failure.message();
}

} // namespace

bool place_file(
    const fs::path& from,
    const fs::path& to,
    bool replace,
    fs::path& placed,
    std::string& error,
    const PlaceHooks& hooks
) {
    std::error_code failure;
    fs::create_directories(to.parent_path(), failure);
    failure.clear();
    if (replace) {
        fs::remove(to, failure);
        failure.clear();
        rename_file(from, to, hooks, failure);
        if (!failure) {
            placed = to;
            return true;
        }
        // Another volume, or a file system that refuses a rename: the bytes are copied.
        failure = oa::platform::copy_file_contents(from, to, oa::platform::CopyOnto::replace);
        if (failure) {
            error = refusal(failure);
            return false;
        }
        fs::remove(from, failure);
        placed = to;
        return true;
    }
    for (int copy = 1; copy < most_names_tried; ++copy) {
        const fs::path candidate = numbered(to, copy);
        std::error_code exists_error;
        if (fs::exists(candidate, exists_error))
            continue;
        // A link is made only where the name is free, whoever else is making it.
        link_file(from, candidate, hooks, failure);
        if (!failure) {
            fs::remove(from, failure);
            placed = candidate;
            return true;
        }
        if (failure == std::errc::file_exists)
            continue;
        // Another volume, or a file system with no links: the bytes are copied to a file made
        // exclusively, so that a name taken in the meantime is refused and the next is tried.
        failure =
            oa::platform::copy_file_contents(from, candidate, oa::platform::CopyOnto::must_be_new);
        if (!failure) {
            fs::remove(from, failure);
            placed = candidate;
            return true;
        }
        if (failure == std::errc::file_exists)
            continue;
        error = refusal(failure);
        return false;
    }
    error = "no name for the file is free";
    return false;
}

} // namespace oa::session::demo
