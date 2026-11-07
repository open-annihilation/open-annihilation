// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What other players' machines tell this one about their players' views
// (ui.camera-sharing, ui.resource-panel): who shares their map position and
// where their cameras are. Network play fills it from the cameras other
// machines send in recorder traffic; a game played alone leaves it empty. The camera rectangles the minimap
// draws for them, and the dot colours the visual rules draw players in.
#pragma once

#include "oa/core/world.h"

#include <array>
#include <cstdint>

namespace oa::ui::hud {

/// A camera place no machine has reported.
inline constexpr int32_t kUnknownCamera = -1;

/// What other machines report about their players' views.
struct SharedPlayerViews {
    /// Whether each player slot shares its map position with this machine.
    std::array<bool, OA_PLAYER_COUNT> shares_position{};
    /// The centre of each slot's camera in map pixels, kUnknownCamera until reported.
    std::array<int32_t, OA_PLAYER_COUNT> camera_x{
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
    };
    std::array<int32_t, OA_PLAYER_COUNT> camera_y{
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
        kUnknownCamera,
    };
};

/// A camera another player's machine reported over the network.
struct ReportedCamera {
    bool shared{}; ///< reported, and not turned off since
    int32_t x{};   ///< the camera's centre in map pixels
    int32_t y{};
};

/// Takes the cameras other machines reported into the shared views.
///
/// A slot whose camera is shared has it placed at the reported centre, and
/// shares its position with this machine when it is not the local player
/// and allies the local player. A slot that never reported one, or turned
/// sharing off, has no camera and shares nothing.
///
/// @param world players and their alliances
/// @param reported each slot's camera, by slot
/// @param[in,out] shared the views; every slot's position and sharing change
void take_reported_cameras(
    const World& world,
    const std::array<ReportedCamera, OA_PLAYER_COUNT>& reported,
    SharedPlayerViews& shared
) noexcept;

/// The palette index a player's colour is drawn in on the minimap, the
/// megamap, the far view and camera rectangles, by the colour's number
/// (PlayerSetupInfo.color), while colour vision is off. A mode uses its own
/// indices, still read through player_dot_color. A whiteboard record keeps
/// this table (`stock_dot_color`) and a mode maps it only where it draws.
inline constexpr std::array<uint8_t, OA_PLAYER_COUNT> kPlayerDotColors{
    227, 212, 80, 235, 108, 219, 208, 93, 130, 67
};

/// Returns a player's dot colour for this machine's drawing.
///
/// @param world players and their setup records
/// @param slot the player's slot
/// @return the palette index; the first colour for an unknown slot
[[nodiscard]] uint8_t player_dot_color(const World& world, uint8_t slot) noexcept;

/// The game's own dot index for a player colour, whatever mode is on.
///
/// A recording and a byte sent to another machine use this index.
///
/// @param world players and their setup records
/// @param slot the player's slot
/// @return the palette index; the first colour for an unknown slot
[[nodiscard]] uint8_t stock_dot_color(const World& world, uint8_t slot) noexcept;

/// The index a stored dot is drawn in on this machine.
///
/// Off leaves the index unchanged. A mode maps a stored game dot onto that
/// colour's anchor and leaves every other index as it is.
///
/// @param stored the index a record holds
/// @return the index to draw
[[nodiscard]] uint8_t drawn_dot_color(uint8_t stored) noexcept;

/// One camera rectangle on the minimap, in screen pixels.
struct CameraRectangle {
    uint8_t slot{};
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
    uint8_t color{};
};

/// Where the minimap shows the map, and the view's size on it.
struct MinimapFrame {
    int32_t left{};       ///< the map picture's left column
    int32_t top{};        ///< its top row
    int32_t width{};      ///< its width in pixels
    int32_t height{};     ///< its height
    int32_t map_width{};  ///< the map's width in map pixels
    int32_t map_height{}; ///< its height
    int32_t view_width{}; ///< the view's rectangle on the minimap, in pixels
    int32_t view_height{};
};

/// The camera rectangles the minimap draws: one for each slot from 1 to 9
/// that shares its position (any slot, for a watcher), has reported a
/// camera and has a name, the size of the view and centred on the reported
/// camera, in the slot's dot colour.
///
/// @param world players
/// @param shared what other machines report
/// @param watching the local player watches
/// @param frame the minimap's map picture
/// @param[out] out the rectangles
/// @return how many were written
uint32_t camera_rectangles(
    const World& world,
    const SharedPlayerViews& shared,
    bool watching,
    const MinimapFrame& frame,
    std::array<CameraRectangle, OA_PLAYER_COUNT>& out
) noexcept;

} // namespace oa::ui::hud
