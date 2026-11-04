// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed expansion's folder, when this run has it turned on.
#pragma once

#include "oa/platform/preferences.hpp"

#include <filesystem>
#include <string_view>

namespace oa::app {

/// Returns the expansion folder this run reads ahead of the game folder.
///
/// The folder is `Expansions/<base>/<id>` under `user_folder`, matched
/// without regard to ASCII case. The path uses the spelling stored on disk.
/// It is returned when the expansion is named by `--content` or by the
/// stored choice, and the folder's content record names every one of its
/// files. A run with no stored choice returns nothing, so the game folder
/// stands alone. Empty when `user_folder` is empty, the choice cannot be
/// read, or the copy is not there.
///
/// @param user_folder the player's folder
/// @param values the preferences the stored choice is read from
/// @param content the `--content` value; empty when the run names none
/// @return the folder, or empty
[[nodiscard]] std::filesystem::path installed_expansion_folder(
    const std::filesystem::path& user_folder,
    const oa::platform::preferences::Values& values,
    std::string_view content
);

} // namespace oa::app
