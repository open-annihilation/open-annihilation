// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a replay's playback tells the engine about the watch: the slot it is
// watched from, and each frame the income and shared cameras the playback
// bar's player list shows (runtime_replay_viewer.cpp). Network play's
// playback calls these as a replay starts and after each frame it plays.
// The header includes no other header of the engine's but oa/core/player.h,
// for the number of slots.
#pragma once

#include "oa/core/player.h"

#include <array>
#include <cstdint>

namespace oa::app {

class Runtime;

/// Income and a shared camera the player list shows for one slot. Not the world's.
struct ReplayShownSlot {
    bool income{};
    float metal{};
    float energy{};
    bool camera_seen{};
    bool camera_shared{};
    uint16_t camera_x{};
    uint16_t camera_y{};
};

/// Makes the slot a replay is watched from the match's local player, as the
/// replay starts. The watcher owns no units, so nothing can be ordered and no
/// unit is announced as its own.
///
/// @param runtime the running app
/// @param slot the watcher's slot; 0 when every slot belongs to a recorded player
void seat_replay_watcher(Runtime& runtime, uint8_t slot);

/// Copies what the player list shows. A playback calls this each frame.
///
/// @param runtime the running app
/// @param slots one entry per live slot
/// @param shares_camera the recording has shared a camera
void note_replay_watch(
    Runtime& runtime, const std::array<ReplayShownSlot, OA_PLAYER_COUNT>& slots, bool shares_camera
);

} // namespace oa::app
