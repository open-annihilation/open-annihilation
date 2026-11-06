// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The BY1117a install the Boneyards' tests read: the 3.1c install
// OA_GAME_DIR names, whose by.ccx, Boneyards folder and base archives the
// Boneyards screens use; and the multiplayer install OA_LAUNCHER_GAME_DIR
// names, which the Boneyards launches from. The engine's network-play test
// installs (oa/test/netgame_installs.hpp) read an install's archives. A test
// without the install it needs prints one line saying what it skipped and
// exits with oa::test::kSkipExitCode, which ctest reports as skipped.
#pragma once

#include "oa/platform/system.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/netgame_installs.hpp"

#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <system_error>

namespace oa::boneyards_test {

// The archive of the BY1117a install that carries the Boneyards art.
inline constexpr const char* kBoneyardsArchive = "by.ccx";
// The environment variable naming the multiplayer install the Boneyards
// launches from.
inline constexpr const char* kLauncherGameDirVariable = "OA_LAUNCHER_GAME_DIR";

/// Returns the multiplayer install OA_LAUNCHER_GAME_DIR names.
///
/// @return the folder, or empty when the variable is unset, empty or names no directory
[[nodiscard]] inline std::filesystem::path launcher_install() {
    const auto named = oa::platform::environment_value(kLauncherGameDirVariable);
    if (!named || named->empty())
        return {};
    std::filesystem::path folder(*named);
    std::error_code error;
    return std::filesystem::is_directory(folder, error) ? folder : std::filesystem::path{};
}

/// Returns the BY1117a install OA_GAME_DIR names.
///
/// @return the folder, or empty when OA_GAME_DIR names none
[[nodiscard]] inline std::filesystem::path by_install() {
    return oa::test::game_directory();
}

/// Returns the BY1117a install OA_GAME_DIR names, or ends the test as skipped.
///
/// The install must hold by.ccx, totala1.hpi and every file in `files`.
///
/// @param what the cases that need the install, for the skip line
/// @param files further '/'-separated paths the cases read
/// @return the install
[[nodiscard]] inline std::filesystem::path
require_by_install(std::string_view what, std::initializer_list<const char*> files = {}) {
    auto root = oa::test::require_game_directory(what);
    if (!oa::test::netgame::holds(root, {kBoneyardsArchive, oa::test::netgame::kBaseArchive}) ||
        !oa::test::netgame::holds(root, files))
        oa::test::skip_test(
            what, "the install OA_GAME_DIR names is not BY1117a with its Boneyards files"
        );
    return root;
}

/// The coreprime.ccx in the mod folder a test is given on its command line.
///
/// The build writes that archive beside oamod.yaml. The test opens it with
/// its own reader. An empty path means the test was started without the folder.
///
/// @param argc argument count
/// @param argv arguments; argv[1] is the mod folder
/// @return the archive path, or empty when the folder was not given
[[nodiscard]] inline std::filesystem::path coreprime_archive(int argc, char** argv) {
    if (argc < 2 || argv == nullptr || argv[1] == nullptr || argv[1][0] == '\0')
        return {};
    return std::filesystem::path(argv[1]) / "coreprime.ccx";
}

} // namespace oa::boneyards_test
