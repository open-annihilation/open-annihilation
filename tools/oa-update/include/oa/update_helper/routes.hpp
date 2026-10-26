// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turning a plan into journal operations. This build carries out the plain
// folder route and the two Windows routes. The other routes produce none.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace oa::update_helper {

/// The operations that put a staged package in place of `plan.install`.
///
/// The plain folder moves the install's top-level entries into
/// `<updates>/previous/`, then the staged top folder's entries into the
/// install. The Windows routes build their own lists. The caller adds the
/// preferences copy and the state file for the plain folder.
///
/// @param plan the plan; its route is the plain folder
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set, or when there was nothing to move
[[nodiscard]] std::vector<Operation> plan_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

} // namespace oa::update_helper
