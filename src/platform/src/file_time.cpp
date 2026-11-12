// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/file_time.hpp"

#include <cstdint>
#include <filesystem>

namespace oa::platform {

int64_t file_seconds_since_1970(std::filesystem::file_time_type time) noexcept {
    return clock_seconds_since_1970<std::filesystem::file_time_type::clock>(time);
}

std::filesystem::file_time_type file_time_from_seconds(int64_t seconds) noexcept {
    return clock_time_from_seconds<std::filesystem::file_time_type::clock>(seconds);
}

} // namespace oa::platform
