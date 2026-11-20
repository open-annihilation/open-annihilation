// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Whether a replay viewer prints its summary, gives the session up, or keeps
// watching, and whether a headless one sets READY. The inputs are facts;
// nothing here reads a session, a lobby or a clock.
#include <cstdint>

namespace oa::app::netgame {

/// How long a running match may hear nothing from every other player before
/// the viewer treats the session as dropped.
inline constexpr uint32_t replay_viewer_silence_ms = 60u * 1000u;

/// How long a headless viewer may sit in the battle room before the match
/// starts. Past this, the run ends with no summary.
inline constexpr uint32_t replay_viewer_room_limit_ms = 5u * 60u * 1000u;

/// How long after a READY send the bit may be sent again.
inline constexpr uint32_t replay_viewer_ready_gap_ms = 1000;

/// What a replay viewer does with the watch.
enum class ReplayViewerEnd : uint8_t {
    waiting, ///< keep watching
    summary, ///< the watched game ended after a record of it was applied
    dropped, ///< the session ended without a summary
};

/// The facts the end is decided from.
struct ReplayViewerWatch {
    bool received = false;         ///< a record of the replay was applied, or a unit was created
    bool local_rejected = false;   ///< this player was refused
    bool match_ended = false;      ///< the match's outcome is no longer ongoing
    bool host_in_table = true;     ///< the host the match started with is still seated
    bool session_lost = false;     ///< the session has been lost, or this machine has left it
    bool silence_measured = false; ///< the match is running, so silence counts
    uint32_t milliseconds_since_packet = 0; ///< since a packet from any other player
    /// The run is in the battle room and the match has not started, so the
    /// wait counts. A windowed run leaves this false.
    bool room_wait_measured = false;
    uint32_t milliseconds_in_battle_room = 0; ///< since this run reached the battle room
};

/// Decides a replay viewer's end from the watch.
///
/// A summary needs a record of the replay, and then the host refusing this
/// player, the match ending, or the host leaving the table. Silence of
/// `replay_viewer_silence_ms`, `replay_viewer_room_limit_ms` in the battle
/// room with no match, a lost session, a refusal or a host who left before
/// any record, or a match that ended before any record, is a drop. The room
/// wait does not print a summary, including when a record was already applied.
///
/// @param watch the facts
/// @return waiting, a summary, or a drop
[[nodiscard]] inline ReplayViewerEnd replay_viewer_end(const ReplayViewerWatch& watch) noexcept {
    const bool host_left = !watch.host_in_table;
    if (watch.received && (watch.local_rejected || watch.match_ended || host_left))
        return ReplayViewerEnd::summary;
    const bool silent =
        watch.silence_measured && watch.milliseconds_since_packet >= replay_viewer_silence_ms;
    const bool room_wait = watch.room_wait_measured &&
                           watch.milliseconds_in_battle_room >= replay_viewer_room_limit_ms;
    if (silent || room_wait || watch.session_lost || watch.local_rejected || watch.match_ended ||
        host_left)
        return ReplayViewerEnd::dropped;
    return ReplayViewerEnd::waiting;
}

/// What a headless replay viewer does with the READY bit on this frame.
enum class ReplayViewerReady : uint8_t {
    hold, ///< leave the bit as it is
    set,  ///< set the bit and send the player info
};

/// The facts a READY send is decided from.
struct ReplayViewerReadyFacts {
    bool headless = false;                ///< the run is `--headless-check`
    bool in_battle_room = false;          ///< the join has reached the battle room
    bool seated = false;                  ///< the local player is seated and not refused
    bool units_reported = false;          ///< this machine has reported every unit type
    bool host_map_known = false;          ///< the host's map is known here
    bool awaiting_mutators = false;       ///< the room is waiting for the host's mutators
    bool ready_set = false;               ///< the seated player's READY bit is set
    bool sent = false;                    ///< a READY send has already gone out
    uint32_t milliseconds_since_send = 0; ///< since that send; ignored until one has
};

/// Decides whether a headless replay viewer sets READY and sends player info.
///
/// It sets the bit only in a headless battle room, with the local player
/// seated and not refused, after this machine has reported its unit types,
/// while the host's map is known and the room is not waiting on mutators, and
/// only while the bit is clear. A bit the room cleared is set again, but not
/// twice within `replay_viewer_ready_gap_ms`. A windowed run never sets it.
///
/// @param facts the facts
/// @return set, or hold
[[nodiscard]] inline ReplayViewerReady
replay_viewer_ready(const ReplayViewerReadyFacts& facts) noexcept {
    if (!facts.headless || !facts.in_battle_room || !facts.seated || !facts.units_reported ||
        !facts.host_map_known || facts.awaiting_mutators || facts.ready_set)
        return ReplayViewerReady::hold;
    if (facts.sent && facts.milliseconds_since_send < replay_viewer_ready_gap_ms)
        return ReplayViewerReady::hold;
    return ReplayViewerReady::set;
}

} // namespace oa::app::netgame
