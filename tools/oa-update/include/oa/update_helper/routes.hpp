// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turning a plan into journal operations. This build carries out the plain
// folder route, the two Windows routes and the Linux versions folder. The
// other routes produce none.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// The operations that put a staged package in place for `plan.route`.
///
/// `check-folder` moves the install's top-level entries into
/// `<updates>/previous/`, then the staged top folder's entries into the
/// install. The Windows routes build their own lists. `linux-versions`
/// converts a flat folder into `versions/` on the first update, then stages
/// the new version beside a `next` link. The caller adds the preferences
/// copy and the state file for the plain folder and the Linux route.
///
/// @param plan the plan
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set, or when there was nothing to move
[[nodiscard]] std::vector<Operation> plan_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// The operations for a Linux or Steam Deck install.
///
/// @param plan the plan; its route is the versions folder
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation> linux_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// The launcher script a Linux install runs, the same bytes the package ships.
///
/// @return the script
[[nodiscard]] std::string_view linux_launcher_script() noexcept;

} // namespace oa::update_helper
