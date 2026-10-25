// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Director mode's state (Runtime::DirectorState): what it presents and
// where its sounds go, the director's camera, what it replaced and restores
// as it leaves, every player's unit announcements with the director's own
// random stream, and the graphics card's renderer when the Full tier draws
// the frames (runtime_director_full.cpp). runtime_director_view.cpp enters
// and leaves the mode;
// the frame drawing (runtime_match_render.cpp) and the point sounds
// (runtime_console_sound.cpp) read it.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/audio/unit_announcements.hpp"
#include "oa/core/world.h"
#include "oa/media/director.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/ui/display_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace oa::app {

/// No renderer on a graphics card could be made for director frames drawn
/// by the Full tier: SDL's video or its GPU renderer did not start, as where
/// the machine has no graphics device SDL can use. A check skips on it; a
/// render stops with it.
class DirectorCardUnavailable : public std::runtime_error {
  public:

    using std::runtime_error::runtime_error;
};

struct Runtime::DirectorState {
    /// The linear congruential sequence of the director's random stream,
    /// x = x * random_multiplier + random_increment, drawing bits 16..30.
    static constexpr uint32_t random_multiplier = 214013;
    static constexpr uint32_t random_increment = 2531011;
    static constexpr uint32_t random_shift = 16;
    static constexpr uint32_t random_mask = 0x7fff;

    /// What reached the sound hooks and the announcement listener.
    struct Tally {
        uint64_t point_sounds{};         ///< the match's point sounds
        uint64_t unplaced_sounds{};      ///< sound-table sounds played unplaced
        uint64_t announcements_heard{};  ///< requests the listener heard
        uint64_t announcements_queued{}; ///< of them, taken by a player's queue
        uint64_t announcements_played{}; ///< presented with a sound and played
        uint32_t speaking_players{};     ///< bit n: player n's units asked to speak
    };

    /// The renderer with no window that the Full tier draws a director's
    /// frames on when its presentation asks for the graphics card
    /// (runtime_director_full.cpp), and what it replaced.
    struct Card {
        SDL_Renderer* renderer{}; ///< null while the processor draws
        /// The texture each frame is presented into and read back from, at
        /// the layout's size.
        SDL_Texture* frame{};
        int frame_width{};
        int frame_height{};
        bool video_started{};          ///< SDL's video was started for the renderer
        SDL_Renderer* kept_renderer{}; ///< the runtime's renderer before, put back
        /// The driver of the graphics card the renderer draws with, as SDL
        /// names it, which the run manifest names.
        std::string driver{};
    };

    DirectorPresentation presentation{};
    DirectorSoundHooks sounds{};
    Card card{};
    oa::media::director::EngineView view{};
    bool view_set{}; ///< set_director_view has set `view`
    /// The view being drawn as the script places it, with the camera's turn:
    /// a turned frame on the graphics card is drawn from its exact centre.
    oa::media::director::View aim{};
    bool live_turned{}; ///< the live renderer's camera was turned for a frame

    // What director mode replaced, put back as it leaves.
    oa::ui::display_layout::MatchLayout layout{};
    int32_t camera_x{};
    int32_t camera_z{};
    float zoom{};
    float zoom_target{};
    bool tracking{};
    uint16_t tracked_unit{};
    oa::sim::match_runtime::Match::PointSoundHook point_sound{};
    oa::sim::match_runtime::Match::NamedSoundHook named_sound{};

    /// Each player's announcements, queued as the game queues the viewing
    /// player's; index is the speaking unit's player.
    std::array<oa::audio::game_audio::AnnouncementQueue, OA_PLAYER_COUNT> announcements{};
    uint32_t random_state{kFixedRandomSeed};
    /// The frame's channel values after the default display gamma.
    std::array<uint8_t, 256> gamma_table{};
    bool gamma_identity{true}; ///< gamma_table leaves every value as it is
    Tally tally{};

    /// Draws the next number of the director's random stream.
    ///
    /// @return 0 to 32767
    [[nodiscard]] uint16_t next_random() noexcept {
        random_state = random_state * random_multiplier + random_increment;
        return static_cast<uint16_t>((random_state >> random_shift) & random_mask);
    }

    /// Sends a sound to the sound hooks.
    ///
    /// @param sound the sound; dropped when the hooks have no play function
    void send(const DirectorSound& sound) const {
        if (sounds.play != nullptr)
            sounds.play(sounds.context, sound);
    }

    /// Tells whether a unit is in the bound view: whether the point under it,
    /// at its x and z, lies in the rectangle from Game.camera_x and camera_y
    /// that view_cells_width by view_cells_height cells cover, edges
    /// included, the test by which the match plays a point sound near.
    ///
    /// @param context the match's World
    /// @param unit unit slot
    /// @return true when the unit is live and in the view
    [[nodiscard]] static bool unit_in_view(const void* context, uint16_t unit) {
        const auto& world = *static_cast<const oa::World*>(context);
        const oa::Unit* record = oa::world_unit_at(&world, unit);
        if (record == nullptr || record->type_index == 0)
            return false;
        namespace wr = oa::present::world_renderer;
        const auto& game = world.game;
        const auto camera_x = static_cast<int32_t>(game.camera_x);
        const auto camera_y = static_cast<int32_t>(game.camera_y);
        const int32_t x = static_cast<int16_t>(record->position.x >> 16);
        const int32_t z = static_cast<int16_t>(record->position.z >> 16);
        return x >= camera_x && z >= camera_y &&
               x <= camera_x + game.view_cells_width * OA_MAP_CELL_PIXELS &&
               z <= camera_y + game.view_cells_height * OA_MAP_CELL_PIXELS;
    }
};

} // namespace oa::app
