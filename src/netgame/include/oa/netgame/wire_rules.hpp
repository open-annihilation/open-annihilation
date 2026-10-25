// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The rules a mod profile sets for network play: the version bytes the game
// presents and how it compares them, the private chat channel and what rides
// on it, the recorder's traffic and the receive-side rules of the match. A
// value-initialised WireRules is 3.1c: every byte on the wire, every record
// handled and every decision stays as 3.1c makes it.

#include <cstdint>

namespace oa::netgame {

/// How a joiner compares a game's major version byte with its own.
enum class VersionRule : uint8_t {
    at_least, ///< the game's major is at least the local one (3.1c)
    equal,    ///< the game's major equals the local one
};

/// How chat records whose text starts with a NUL byte are read.
enum class PrivateChannel : uint8_t {
    none,            ///< as ordinary chat (3.1c)
    sub_id_dispatch, ///< by sub-id: integrity checks (0x2b) and votes to reject (0x2c)
    integrity_only,  ///< only integrity checks (sub-id 0x2b); every other private line is dropped
};

/// When the integrity check challenges the other players.
enum class IntegrityCheck : uint8_t {
    off,                ///< never; private integrity lines are ignored (3.1c)
    single_challenge,   ///< once, at tick 180
    periodic_challenge, ///< at tick 180, again until tick 450 and then every 30 ticks until tick 3600
};

/// How a recording of ten players is watched.
enum class TenPlayerReplay : uint8_t {
    off,                ///< refused: the viewer needs a slot of its own (3.1c)
    watcher_view,       ///< the viewer takes no slot and sees every player's radar
    allied_fake_player, ///< the viewer takes no slot and is allied with every recorded player
};

/// The recorder protocol byte a plain peer sends: no recorder.
inline constexpr uint8_t recorder_protocol_plain = 0;
/// The recorder protocol version a recorder peer sends in every lobby setup block.
inline constexpr uint8_t recorder_protocol_current = 6;
/// The recording header version every recorder writes and every replayer reads.
inline constexpr uint8_t recording_header_version = 5;
/// The most weapon types 3.1c's records can name.
inline constexpr uint32_t wire_base_weapon_types = 256;
/// The most sides 3.1c plays.
inline constexpr uint8_t wire_base_sides = 5;

/// The network rules of one game; value-initialised it is 3.1c.
struct WireRules {
    uint8_t version_major{3}; ///< in every setup block and the session's user bytes
    uint8_t version_minor{1}; ///< in every setup block and the session's user bytes
    VersionRule version_rule{VersionRule::at_least};
    /// A session hosted while a launch is active adds 100 to its major and
    /// marks itself launch-only, and a joiner removes the 100 again.
    bool launch_version_bias{true};
    /// The version bytes presented to a replayer that plays a recording back.
    uint8_t replay_version_major{3};
    uint8_t replay_version_minor{1};
    PrivateChannel private_channel{PrivateChannel::none};
    IntegrityCheck integrity_check{IntegrityCheck::off};
    bool vote_reject{};                ///< a vote of every machine replaces the host's drop dialog
    uint16_t vote_seconds{60};         ///< how long a vote someone asked for stays open
    uint16_t timeout_vote_seconds{90}; ///< how long a vote on a silent player stays open
    /// While no remote player has been heard for this many milliseconds, the
    /// game steps at most once per period and ignores the Pause key; 0 is off.
    uint16_t lag_guard_ms{};
    /// The tick at which each machine sends its players' first units'
    /// positions, which receivers take for the commander; 0 is off.
    uint16_t commander_sync_tick{};
    bool keep_remote_colour{}; ///< an in-game setup block never changes a remote player's colour
    bool clear_session_password{}; ///< the host publishes its session with an empty password
    bool host_stays_watching{};    ///< a defeated host watches on instead of being asked to leave
    bool recorder_session_commands{}; ///< the recorder's session commands (.autopause, .ready, ...)
    /// The recorder protocol this machine presents in its lobby setup blocks;
    /// recorder_protocol_plain sends and reads no recorder traffic.
    uint8_t recorder_protocol{recorder_protocol_plain};
    /// A player whose recorder reports cheats is announced.
    bool recorder_cheat_notices{true};
    TenPlayerReplay ten_player_replay{TenPlayerReplay::off};
    uint8_t speed_min{1};  ///< the lowest game speed a received speed record may set
    uint8_t speed_max{20}; ///< the highest game speed a received speed record may set
    /// The host's .syncon and .syncoff lock and unlock the game speed for
    /// the game, on recorders (console.game-speed-range syncon).
    bool speed_lock{};
    /// The most weapon types this game's records name. Above
    /// wire_base_weapon_types, shots, interceptions and feature hits travel
    /// as the wide records.
    uint32_t weapon_types{wire_base_weapon_types};
    /// The most sides this game plays.
    uint8_t sides{wire_base_sides};

    /// Compares every field.
    bool operator==(const WireRules&) const = default;
};

/// Tells whether a game's major version byte lets the local machine join.
///
/// @param rules the local machine's rules
/// @param game_major the game's major byte, with any launch bias removed
/// @return true when the rule admits the game
[[nodiscard]] constexpr bool version_admits(const WireRules& rules, int32_t game_major) noexcept {
    const auto local = static_cast<int32_t>(static_cast<int8_t>(rules.version_major));
    return rules.version_rule == VersionRule::equal ? game_major == local : local <= game_major;
}

/// Tells whether shots, interceptions and feature hits travel as wide records.
///
/// The choice reads the profile's weapon-type ceiling, not the weapons loaded.
///
/// @param rules the game's rules
/// @return true when weapon_types is above wire_base_weapon_types
[[nodiscard]] constexpr bool wide_weapon_records(const WireRules& rules) noexcept {
    return rules.weapon_types > wire_base_weapon_types;
}

/// Tells whether the game can be played only between OA machines.
///
/// @param rules the game's rules
/// @return true when weapon types pass 256 or sides pass five
[[nodiscard]] constexpr bool oa_only(const WireRules& rules) noexcept {
    return rules.weapon_types > wire_base_weapon_types || rules.sides > wire_base_sides;
}

} // namespace oa::netgame
