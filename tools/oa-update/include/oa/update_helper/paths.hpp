// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Paths as the plan stores them: UTF-8, with forward slashes.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace oa::update_helper {

/// A path as UTF-8 with forward slashes.
///
/// @param path the path
/// @return its spelling
[[nodiscard]] inline std::string utf8_of(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

/// A path from UTF-8.
///
/// @param text the spelling
/// @return the path
[[nodiscard]] inline std::filesystem::path path_from(std::string_view text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())
    );
}

} // namespace oa::update_helper
