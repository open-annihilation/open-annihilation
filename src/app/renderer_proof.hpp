// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>

namespace oa::app::renderer_proof {
/// Captures a fixed installed-asset scene through the proof renderer boundary.
///
/// @param game_dir installed game data directory
/// @param snapshot output directory; empty selects local/reports/renderer-proof
/// @return 0 for a completed comparison, 77 for unavailable GPU, 1 for failure;
///     scene parity is reported separately and is not implied by completion
int run(const std::filesystem::path& game_dir, const std::filesystem::path& snapshot);
} // namespace oa::app::renderer_proof
