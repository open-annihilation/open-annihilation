// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's launch block: how a launching program (an extension built on
// network play) asks the game to open a network game, which connection,
// host or join, the session and its options. The "-n" and "-h" switches
// write its connection fields too. The multiplayer screens, the battle room
// and the session read it, most of them only while a launch is active
// (LaunchLink::launch_active).
#pragma once

#include <cstddef>
#include <cstdint>

namespace oa::ui::frontend_multiplayer::launch {

inline constexpr std::size_t kProviderLabelBytes = 0x10;
inline constexpr std::size_t kLaunchSessionNameBytes = 0x40;
inline constexpr std::size_t kLaunchMissionBytes = 0x40;
inline constexpr std::size_t kLaunchPasswordBytes = 0x20;
inline constexpr std::size_t kLaunchUserNameBytes = 0x20;
inline constexpr std::size_t kLaunchAddressBytes = 0x40;

// LaunchBlock::connection_type: the transport the connection selection
// takes without asking; 0 leaves the choice to the player.
namespace connection_type {
inline constexpr int32_t none = 0;
inline constexpr int32_t tcpip = 1;
inline constexpr int32_t ipx = 2;
inline constexpr int32_t modem = 3;
inline constexpr int32_t serial = 4;
} // namespace connection_type

// LaunchBlock::faction: the side the launch assigned.
namespace faction {
inline constexpr int32_t none = 0;
inline constexpr int32_t arm = 1;
inline constexpr int32_t core = 2;
} // namespace faction

/// The most computer players a launch seats: every seat but the player's own.
inline constexpr int32_t kLaunchComputerLimit = 9;
/// A kebab-case id of at most 64 bytes, empty for the built-in player.
inline constexpr std::size_t kLaunchBrainIdBytes = 65;
/// The teams a launch names, 1 through 5, which the battle room shows as its five team icons.
inline constexpr int32_t kLaunchTeamFirst = 1;
inline constexpr int32_t kLaunchTeamLast = 5;
/// The colours a launch names, 1 through 10, in the order the battle room steps through them.
inline constexpr int32_t kLaunchColourFirst = 1;
inline constexpr int32_t kLaunchColourLast = 10;

/// LaunchComputer::difficulty: how well a computer player plays; none leaves the room's own.
namespace difficulty {
inline constexpr int32_t none = 0;
inline constexpr int32_t easy = 1;
inline constexpr int32_t medium = 2;
inline constexpr int32_t hard = 3;
} // namespace difficulty

/// The most unit restrictions a launch sets.
inline constexpr int32_t kLaunchRestrictionLimit = 256;
/// Room for a unit's FBI UnitName in a restriction, with its terminating NUL.
inline constexpr std::size_t kLaunchUnitNameBytes = 0x20;
/// LaunchRestriction::limit of a unit that may be built without a limit.
inline constexpr int32_t kLaunchNoLimit = -1;
/// The most a restriction lets a player build of one unit; the battle room's slider stops here.
inline constexpr int32_t kLaunchHighestLimit = 100;

/// One unit restriction a launch sets, as the battle room's Restrictions sets it.
struct LaunchRestriction {
    char unit[kLaunchUnitNameBytes]{}; ///< the unit's FBI UnitName, matched in any case
    int32_t limit{};                   ///< 0 to 100, or kLaunchNoLimit; 0 cannot be built
};

/// One computer player a launch seats in the battle room.
struct LaunchComputer {
    int32_t faction{};    ///< faction values; none leaves the battle room's side
    int32_t team{};       ///< 1 to 5; 0 seats it on no team
    int32_t color{};      ///< 1 to 10; 0 takes the next free colour
    int32_t difficulty{}; ///< difficulty values
    /// A kebab-case id of at most 64 bytes. Empty seats the built-in player.
    char brain[kLaunchBrainIdBytes]{};
};

// One launch. Texts are terminated within their fields.
struct LaunchBlock {
    // The transport to connect with (connection_type values); the connection
    // selection clears it once it has taken it.
    int32_t connection_type{};
    uint32_t hosting{};                 // 1: create the game; 0: join it
    int32_t launched_connection_type{}; // the launch's connection_type, kept; never read
    int32_t player_limit{};             // players the launch allows
    int32_t time_limit{};               // ? minutes; the score report passes it on
    int32_t pfs{};                      // ? meaning unresolved; the score report passes it on
    char provider_label[kProviderLabelBytes]{};   // the launcher's name for the match's menus
    char session_name[kLaunchSessionNameBytes]{}; // the game's name ("-h" writes it too)
    char mission[kLaunchMissionBytes]{};          // the map the host plays
    char password[kLaunchPasswordBytes]{};        // the game's password
    char user_name[kLaunchUserNameBytes]{};       // the player's name with the launcher
    uint32_t lock_options{};                      // 1: the battle room's option controls are locked
    int32_t max_units{};
    int32_t energy{};                    // starting energy
    int32_t metal{};                     // starting metal
    int32_t death_option{};              // 1 continues, 2 ends, 3 deathmatch on commander death
    int32_t los{};                       // 1 true, 2 circular, 3 permanent line of sight
    int32_t cheats{};                    // 2 allows the cheat codes
    int32_t location{};                  // 1 fixed start positions
    int32_t mapping{};                   // 1 the map starts unmapped
    int32_t watching{};                  // 2 allows watchers
    char address[kLaunchAddressBytes]{}; // the host's address ("-n1:" writes it too)
    int32_t tournament{};                // nonzero for a tournament game
    int32_t faction{};                   // faction values
    int32_t team{};
    int32_t color{};          // 1 to 10, the player's colour; 0 leaves it to the battle room
    uint32_t watcher{};       // 1: the player watches instead of playing
    int32_t computer_count{}; // computer players seated from computers, in order
    LaunchComputer computers[kLaunchComputerLimit]{};
    uint32_t closed{};           // 1: the host closes the game to more players
    int32_t restriction_count{}; // unit restrictions set from restrictions, in order
    LaunchRestriction restrictions[kLaunchRestrictionLimit]{};
};

} // namespace oa::ui::frontend_multiplayer::launch
