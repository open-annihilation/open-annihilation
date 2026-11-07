// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Photo mode's state (Runtime::PhotoState): what it replaced and restores
// as it leaves, and the player's own view the under-attack notice keeps
// testing while the photo camera moves. runtime_photo_mode.cpp enters and
// leaves the mode. Leaving writes the player's follow back; nothing else
// here is written into the match.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/sim/messages.hpp"

#include <cstdint>
#include <vector>

namespace oa::app {

struct Runtime::PhotoState {
    /// What photo mode replaced, put back as it leaves.
    oa::ui::display_layout::MatchLayout layout{};
    int32_t camera_x{};
    int32_t camera_z{};
    float zoom{};
    float zoom_target{};
    ExactView exact{};
    ViewHold hold{};
    bool tracking{};
    uint16_t tracked_unit{};
    uint32_t follow_unit{}; ///< Game.follow_unit on entering
    bool live_kept{};       ///< the live renderer existed, so its turn is put back
    float live_yaw{};
    float live_pitch{};
    bool live_unlocked{};
    bool keys_overlay{};      ///< the keys overlay was open
    bool cursor_was_hidden{}; ///< frame_without_cursor_ on entering
    bool show_selection{};    ///< selection boxes over the bare battlefield
    bool show_health_bars{};  ///< health bars over the bare battlefield

    /// The player's camera and battlefield rectangle at entry. The on-screen
    /// test collects against these. The picture uses the photo camera, and
    /// the Game block keeps this one.
    uint32_t kept_camera_x{};
    uint32_t kept_camera_y{};
    oa::Rect32 kept_rect{};
    std::vector<uint16_t> kept_units{};
    uint32_t kept_count{};

    /// The message log's next write slot at entry, and the line that stood
    /// there, so a full ring can be told from a slot nothing has replaced.
    uint16_t chat_head{};
    oa::sim::messages::MessageLine chat_at_head{};

    /// How photo mode is playing a match on this machine. A shared match
    /// stays running: its commands are refused, and a held picture is separate.
    enum class Time : uint8_t {
        running, ///< the match's own clock
        frozen,  ///< the clock is held, as the game menu holds a local match
        slow,    ///< the clock is held and ticks are granted at a slow rate
    };
    Time time{Time::running};
    int32_t slow_speed{};            ///< 1..9 while slow; 0 otherwise
    uint64_t slow_interval_ns{};     ///< one granted tick, on frame_time_ns_
    uint64_t slow_origin_ns{};       ///< frame_time_ns_ when the slow pace started
    uint64_t slow_granted{};         ///< ticks granted since slow_origin_ns
    bool shutter{};                  ///< an eighth between ticks replaces the fraction
    int32_t shutter_eighth{};        ///< 0 the tick before, 8 the frozen tick
    bool picture_held{};             ///< a shared match shows the copied battlefield
    uint32_t held_width{};           ///< the copy's width
    uint32_t held_height{};          ///< the copy's height
    std::vector<uint8_t> held_rgb{}; ///< the copy, row by row, three bytes a pixel
    /// The camera when the picture was held. Letting go writes it back.
    /// Leaving photo mode still writes the camera from entering.
    bool camera_held{};
    int32_t held_camera_x{};
    int32_t held_camera_z{};
    float held_zoom{};
    float held_zoom_target{};
    ExactView held_exact{};
    ViewHold held_view{};
    bool held_tracking{};
    uint16_t held_tracked_unit{};
    uint32_t held_follow_unit{};
    bool held_live{}; ///< the live renderer existed when the picture was held
    float held_live_yaw{};
    float held_live_pitch{};
    bool held_live_unlocked{};
    uint16_t entry_speed{}; ///< a replay's speed on entering, for Run to put back

    /// Tells whether a unit is in the view photo mode kept: listed by the
    /// last collect against the player's camera, not the photo camera.
    ///
    /// @param context the PhotoState
    /// @param unit unit slot
    /// @return true when the unit is in the kept list
    [[nodiscard]] static bool unit_kept(const void* context, uint16_t unit) noexcept {
        const auto* state = static_cast<const PhotoState*>(context);
        if (state == nullptr)
            return false;
        const auto count = state->kept_count < state->kept_units.size()
                               ? state->kept_count
                               : static_cast<uint32_t>(state->kept_units.size());
        for (uint32_t index = 0; index < count; ++index)
            if (state->kept_units[index] == unit)
                return true;
        return false;
    }
};

} // namespace oa::app
