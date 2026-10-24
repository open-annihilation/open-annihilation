// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Loads one brain against a mounted game, in a check match that is never
// ticked, and reports whether it loaded, fitted and started.
#pragma once

#include "oa/app/brain_seats.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::app {

/// What a check loads the brain against.
struct BrainCheckRequest {
    /// The game folder, mounted the way the game mounts it.
    std::filesystem::path game_dir;
    /// A mod folder layered over the game. Empty checks the base game.
    std::filesystem::path mod_folder;
    /// The brain, already read from its folder or its package.
    InstalledBrain brain;
    /// The level and the options. An empty level selects the first level.
    BrainSeatChoice choice;
    /// The side whose commander the brain's seat holds. 0 is the first side.
    uint8_t side{};
};

/// What one of load, fits and start did.
struct PhaseReport {
    /// `ok`, `failed`, `none`, `not run`, or `does not fit: ` and the reason.
    std::string result;
    /// Instructions that phase used. 0 when it did not run. The count steps
    /// by 1,000, so a short phase can report 0.
    uint64_t instructions{};
    /// The reason `fits` returned, without the `does not fit: ` prefix.
    /// Empty when `fits` did not decline.
    std::string text;
};

/// What the check found. `seated` is true only when the brain took the seat.
struct BrainCheckReport {
    /// True when load, fits and start left the brain seated.
    bool seated{};
    /// The content bundle the brain reads, `totala` for this check.
    std::string game;
    /// The mod's id and version, or nothing when no mod folder was given.
    std::optional<std::pair<std::string, std::string>> mod;
    /// The level that was seated, or the one the choice named.
    std::string level;
    /// The load phase.
    PhaseReport load;
    /// The fits phase.
    PhaseReport fits;
    /// The start phase.
    PhaseReport start;
    /// A failure's sentence. Empty when the brain was seated or declined.
    std::string error;
    /// Lua bytes the seated brain holds after start. 0 when it was not seated.
    std::size_t lua_bytes{};
    /// Bytes of the network files the manifest names.
    std::size_t network_bytes{};
    /// One line per unit file that did not load. The check still runs.
    std::vector<std::string> warnings;
};

/// Loads `request` against its game and reports the seating.
///
/// Mounts the game, and the mod folder when one is given, the way the game
/// does. A manifest that does not fit is reported and nothing of the brain
/// runs. Otherwise builds a check match — the game's definitions, a flat
/// stand-in map, the brain's seat holding that side's commander — seats the
/// brain, and does not tick the match. Nothing is returned when the game or
/// the mod cannot be mounted, or the check match cannot be built; `problem`
/// then names the reason. The call does not throw.
///
/// @param request the game, the mod, the brain and the seat
/// @param[out] problem the reason, when nothing is returned
/// @return the report, or nothing when the check match cannot be built
[[nodiscard]] std::optional<BrainCheckReport>
check_brain_against_game(const BrainCheckRequest& request, std::string& problem);

/// Loads `request` and calls `inspect` on the live match before it is destroyed.
///
/// `inspect` may be null. It is called only when the match was built, after
/// the brain was seated or refused, and it must not throw. The rest is
/// `check_brain_against_game`.
///
/// @param request the game, the mod, the brain and the seat
/// @param[out] problem the reason, when nothing is returned
/// @param inspect called with the match and `context`; null skips the call
/// @param context pointer passed to `inspect`; may be null
/// @return the report, or nothing when the check match cannot be built
[[nodiscard]] std::optional<BrainCheckReport> check_brain_against_game(
    const BrainCheckRequest& request,
    std::string& problem,
    void (*inspect)(sim::match_runtime::Match& match, void* context),
    void* context
);

} // namespace oa::app
