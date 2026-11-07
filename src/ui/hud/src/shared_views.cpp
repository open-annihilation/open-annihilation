// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/shared_views.hpp"

#include "oa/present/colour_vision/colour_vision.hpp"

namespace oa::ui::hud {
namespace {

constexpr uint8_t kFirstSharingSlot = 1;

/// Places a map coordinate on the minimap, as a float truncated.
int32_t on_minimap(
    int32_t map, int32_t map_size, int32_t picture_size, int32_t picture_start, int32_t half
) noexcept {
    if (map_size == 0)
        return picture_start - half;
    const float placed =
        static_cast<float>(map) / static_cast<float>(map_size) * static_cast<float>(picture_size) +
        static_cast<float>(picture_start) - static_cast<float>(half);
    return static_cast<int32_t>(placed);
}

uint8_t player_color_number(const World& world, uint8_t slot) noexcept {
    if (slot >= OA_PLAYER_COUNT)
        return 0;
    const PlayerSetupInfo* info = world_player_info(&world, &world.game.players[slot]);
    const uint8_t color = info != nullptr ? info->color : uint8_t{0};
    if (color >= kPlayerDotColors.size())
        return 0;
    return color;
}

} // namespace

uint8_t stock_dot_color(const World& world, uint8_t slot) noexcept {
    return kPlayerDotColors[player_color_number(world, slot)];
}

uint8_t player_dot_color(const World& world, uint8_t slot) noexcept {
    const uint8_t color = player_color_number(world, slot);
    // Off is the table above, unchanged. A mode is the anchor matched to the palette in use.
    if (oa::present::colour_vision::active_mode() == oa::present::colour_vision::Mode::off)
        return kPlayerDotColors[color];
    return oa::present::colour_vision::player_dot(static_cast<int>(color));
}

uint8_t drawn_dot_color(uint8_t stored) noexcept {
    if (oa::present::colour_vision::active_mode() == oa::present::colour_vision::Mode::off)
        return stored;
    for (uint8_t color = 0; color < kPlayerDotColors.size(); ++color) {
        if (kPlayerDotColors[color] != stored)
            continue;
        return oa::present::colour_vision::player_dot(static_cast<int>(color));
    }
    return stored;
}

void take_reported_cameras(
    const World& world,
    const std::array<ReportedCamera, OA_PLAYER_COUNT>& reported,
    SharedPlayerViews& shared
) noexcept {
    const uint8_t local = world.game.local_player_index;
    const uint8_t local_index =
        local < OA_PLAYER_COUNT ? world.game.players[local].index : uint8_t{OA_PLAYER_COUNT};
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const ReportedCamera& camera = reported[slot];
        if (!camera.shared) {
            shared.shares_position[slot] = false;
            shared.camera_x[slot] = kUnknownCamera;
            shared.camera_y[slot] = kUnknownCamera;
            continue;
        }
        const Player& player = world.game.players[slot];
        shared.camera_x[slot] = camera.x;
        shared.camera_y[slot] = camera.y;
        shared.shares_position[slot] =
            slot != local && local_index < OA_PLAYER_COUNT && player.alliance[local_index] != 0;
    }
}

uint32_t camera_rectangles(
    const World& world,
    const SharedPlayerViews& shared,
    bool watching,
    const MinimapFrame& frame,
    std::array<CameraRectangle, OA_PLAYER_COUNT>& out
) noexcept {
    uint32_t count = 0;
    for (uint8_t slot = kFirstSharingSlot; slot < OA_PLAYER_COUNT; ++slot) {
        if (!shared.shares_position[slot] && !watching)
            continue;
        if (shared.camera_x[slot] == kUnknownCamera || world.game.players[slot].name[0] == '\0')
            continue;
        CameraRectangle& rectangle = out[count++];
        rectangle.slot = slot;
        rectangle.left = on_minimap(
            shared.camera_x[slot], frame.map_width, frame.width, frame.left, frame.view_width / 2
        );
        rectangle.top = on_minimap(
            shared.camera_y[slot], frame.map_height, frame.height, frame.top, frame.view_height / 2
        );
        rectangle.right = rectangle.left + frame.view_width;
        rectangle.bottom = rectangle.top + frame.view_height;
        rectangle.color = player_dot_color(world, slot);
    }
    return count;
}

} // namespace oa::ui::hud
