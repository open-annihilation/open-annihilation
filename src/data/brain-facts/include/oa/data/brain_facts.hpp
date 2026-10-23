// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The facts of one game, owned outright, so a copy can cross to the thread
// that seats a brain. Text and lists live here, not under the simulation.
#pragma once

#include "oa/core/game_state.h"
#include "oa/data/mod_profile.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::data::brain_facts {

/// Whether the session is a skirmish, a campaign mission or a network game.
enum class GameMode : uint8_t {
    skirmish, ///< a game on this machine
    campaign, ///< a campaign mission
    network,  ///< a network game
};

/// One side, in SIDEDATA order.
struct SideFacts {
    std::string name{};      ///< the side's name
    std::string prefix{};    ///< the side's unit prefix
    std::string commander{}; ///< the side's commander unit
};

/// One mutator option, as the loadout stores it.
struct OptionFacts {
    /// How the option's value is stored.
    enum class Kind : uint8_t {
        toggle, ///< on or off
        number, ///< a number
        choice, ///< one of a fixed list, held as text
    };

    std::string id{};   ///< the option's id
    Kind kind{};        ///< which value is set
    bool toggle{};      ///< a toggle's value
    double number{};    ///< a number's value
    std::string text{}; ///< a choice's text
};

/// One mutator in the order it is applied.
struct MutatorFacts {
    std::string id{};                   ///< the mutator's id
    std::string version{};              ///< the mutator's version
    std::vector<OptionFacts> options{}; ///< its options, in the loadout's order
};

/// The mod the game plays, or the base game when no mod is loaded.
struct ModFacts {
    /// The mod's id. The base game is `ta-3.1c`.
    std::string id{"ta-3.1c"};
    /// The name players see. The base game is `Total Annihilation 3.1c`.
    std::string name{"Total Annihilation 3.1c"};
    /// The mod's version. The base game is `3.1c`.
    std::string version{"3.1c"};
    /// The sides of this match, in SIDEDATA order.
    std::vector<SideFacts> sides{};
    /// The effective profile, already resolved. Empty when the game plays
    /// 3.1c's rules with no profile in hand.
    std::optional<data::mod_profile::ModProfile> profile{};
};

/// The facts a brain reads as `game`. A default value is the base game with
/// nothing switched on.
struct GameFacts {
    /// The content bundle id: `totala` or `totala-demo`.
    std::string base{"totala"};
    /// Expansion ids that are on, in the built-in order. The base game is
    /// not among them.
    std::vector<std::string> expansions{};
    /// The engine's version text, such as `0.8.6`. Empty until the app sets it.
    std::string engine{};
    /// Skirmish, campaign or network.
    GameMode mode{};
    /// OA_DIFFICULTY_EASY, MEDIUM or HARD.
    int32_t difficulty{OA_DIFFICULTY_MEDIUM};
    /// The mod, or the base game.
    ModFacts mod{};
    /// The mutators in apply order. Empty when the match applied none.
    std::vector<MutatorFacts> mutators{};
};

/// Names the mod a profile plays, and keeps a copy of that profile.
///
/// A null profile and a profile whose id is `base-game` are the base game:
/// id `ta-3.1c`, name `Total Annihilation 3.1c`, version `3.1c`. Any other
/// profile keeps its own id, name and version. A non-null profile is copied
/// either way, so the facts do not point back at the caller.
///
/// @param profile the effective profile, or null when the game plays 3.1c
/// @return the mod's facts, owned outright
[[nodiscard]] ModFacts mod_facts_of(const data::mod_profile::ModProfile* profile);

} // namespace oa::data::brain_facts
