// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// iOS and Android have no trash. A file the player lets go is deleted, and
// the question says so.

#include "oa/platform/trash.hpp"

#include <filesystem>
#include <system_error>

namespace oa::platform {

bool system_has_trash() noexcept {
    return false;
}

TrashMove move_to_trash(const std::filesystem::path& file) {
    TrashMove out;
    out.place = TrashPlace::deleted;
    std::error_code failure;
    if (!std::filesystem::is_regular_file(file, failure) || failure) {
        out.error = "the file is not there";
        return out;
    }
    if (!std::filesystem::remove(file, failure) || failure) {
        out.error = failure ? failure.message() : "the file could not be deleted";
        return out;
    }
    out.moved = true;
    return out;
}

} // namespace oa::platform
