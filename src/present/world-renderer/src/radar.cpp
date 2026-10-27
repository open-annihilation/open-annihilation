// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/world_renderer/world_radar.hpp"

#include "oa/present/blit.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/polygon.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/world_renderer/world_camera.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::present::world_renderer {

namespace {

int32_t high_word(oa_fixed value) noexcept {
    return static_cast<int16_t>(static_cast<uint32_t>(value) >> 16);
}

// Picture column of a map position.
int32_t radar_column(const Game& game, const FixedVec3& position) noexcept {
    return game.radar_width * high_word(position.x) / game.map_pixel_width;
}

// Picture row of a map position: its z less half its height, as the
// battlefield projection places it.
int32_t radar_row(const Game& game, const FixedVec3& position) noexcept {
    return (high_word(position.z) - (high_word(position.y) >> 1)) * game.radar_height /
           game.map_pixel_height;
}

// Picture radius of a map-pixel distance.
int32_t radar_radius(const Game& game, int32_t distance) noexcept {
    return game.radar_width * distance / game.map_pixel_width;
}

// Colour of a player, the frame index of its blip and shot icon.
int32_t player_color(const World& world, uint8_t player) noexcept {
    const Player* record = world_player(&world, player);
    const PlayerSetupInfo* info = record != nullptr ? world_player_info(&world, record) : nullptr;
    return info != nullptr ? info->color : -1;
}

void draw_sensor_rings(
    const Game& game, const UnitDef& def, ::oa::Surface& target, int32_t x, int32_t y
) noexcept {
    const auto sensor = game_ui_color(game, ui_color_sensor_range);
    const auto jammer = game_ui_color(game, ui_color_jammer_range);

    const struct {
        int16_t distance;
        uint8_t color;
    } rings[] = {
        {def.radar_distance, sensor},
        {def.sonar_distance, sensor},
        {def.radar_distance_jam, jammer},
        {def.sonar_distance_jam, jammer},
    };

    for (const auto& ring : rings)
        if (ring.distance != 0)
            ::oa::present::draw_range_ring(
                &target, x, y, radar_radius(game, ring.distance), ring.color
            );
}

void draw_interceptor_rings(
    const World& world, const Unit& unit, ::oa::Surface& target, int32_t x, int32_t y
) noexcept {
    const Game& game = world.game;
    const auto color = game_ui_color(game, ui_color_interceptor_range);
    for (const UnitWeapon& slot : unit.weapons) {
        const WeaponDef* weapon = world_weapon_def(&world, slot.def);
        if (weapon == nullptr || (weapon->flags & OA_WEAPON_FLAG_INTERCEPTOR) == 0)
            continue;
        const auto radius = radar_radius(game, weapon->coverage - interceptor_ring_inset);
        if (slot.stockpile == 0)
            ::oa::present::draw_range_ring(&target, x, y, radius, color);
        else
            ::oa::present::draw_dashed_range_ring(
                &target,
                x,
                y,
                radius,
                color,
                interceptor_ring_segments,
                game.radar_blink_flags & radar_flag_blink
            );
    }
}

void draw_projectiles(
    const World& world,
    ::oa::Surface& target,
    const RadarSprites& sprites,
    const RadarContactHost& host
) noexcept {
    const Game& game = world.game;
    if (world.projectiles == nullptr)
        return;
    const auto viewer = game.viewpoint_player;
    const auto count = game.projectile_count < OA_PROJECTILE_CAPACITY ? game.projectile_count
                                                                      : OA_PROJECTILE_CAPACITY;
    for (int32_t index = 0; index < count; ++index) {
        const Projectile& shot = world.projectiles[index];
        const WeaponDef* weapon = world_weapon_def(&world, shot.def);
        if (weapon == nullptr ||
            (host.projectile_hidden != nullptr && host.projectile_hidden(host.user, shot)))
            continue;
        const auto x = radar_column(game, shot.position);
        const auto y = radar_row(game, shot.position);
        const bool seen =
            host.point_visible != nullptr && host.point_visible(host.user, shot.position);
        if ((weapon->flags & radar_icon_weapon_flags) == 0) {
            if ((weapon->flags & OA_WEAPON_FLAG_NO_RADAR) == 0 &&
                (seen || shot.owner_index == viewer))
                ::oa::present::draw_point(&target, x, y, game_ui_color(game, ui_color_radar_marks));
            continue;
        }
        const Unit* source = world_unit(&world, shot.source);
        if (seen || (source != nullptr && source->owner_index == viewer))
            ::oa::present::draw_sprite(
                &target,
                ::oa::present::gaf_frame(
                    sprites.weapon_logos, player_color(world, shot.owner_index)
                ),
                x,
                y
            );
    }
}

/// Tells whether a unit's blip shows as the viewer's own: its owner is the
/// viewer or, when allied units are shown, its owner allies the viewer.
bool shown_as_own(const World& world, const Unit& unit, uint8_t viewer, bool allied) noexcept {
    if (!allied)
        return unit.owner_index == viewer;
    const Player* owner = world_unit_owner(&world, &unit);
    return owner != nullptr && viewer < sizeof owner->alliance && owner->alliance[viewer] != 0;
}

} // namespace

void radar_init_surfaces(
    Game& game, RadarSurfaces& surfaces, const RadarSurfaceHost& host
) noexcept {
    if (host.build_picture != nullptr)
        host.build_picture(host.user, surfaces);
    const int32_t width = game.radar_width;
    const int32_t height = game.radar_height;
    if (host.create_surface != nullptr) {
        surfaces.final_image = host.create_surface(host.user, radar_final_name, width, height);
        surfaces.mapped = host.create_surface(host.user, radar_mapped_name, width, height);
    }
    game.radar_picture_rect = {
        game.radar_offset_x,
        game.radar_offset_y,
        width - 1 + game.radar_offset_x,
        height - 1 + game.radar_offset_y,
    };
    game.radar_blink_flags =
        static_cast<uint16_t>(game.radar_blink_flags | radar_flag_mapped_dirty);
    game.radar_blink_countdown = radar_blink_reload;
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags & ~radar_flag_blink);
}

void radar_build_picture(
    Game& game,
    RadarSurfaces& surfaces,
    const RadarPictureSource& source,
    const RadarSurfaceHost& host
) noexcept {
    const int32_t map_w = game.map_pixel_width;
    const int32_t map_h = game.map_pixel_height;
    if (map_w <= 0 || map_h <= 0 || host.create_surface == nullptr)
        return;
    int32_t width = radar_picture_size;
    int32_t height = radar_picture_size;
    if (map_w < map_h) {
        width = map_w * radar_picture_size / map_h;
        game.radar_offset_x = static_cast<int16_t>((radar_picture_size - width) / 2);
        game.radar_offset_y = 0;
    } else {
        height = map_h * radar_picture_size / map_w;
        game.radar_offset_x = 0;
        game.radar_offset_y = static_cast<int16_t>((radar_picture_size - height) / 2);
    }
    game.radar_width = static_cast<int16_t>(width);
    game.radar_height = static_cast<int16_t>(height);
    surfaces.picture = host.create_surface(host.user, radar_picture_name, width, height);
    if (surfaces.picture == nullptr || source.display == nullptr)
        return;
    Sprite halved{};
    ::oa::present::sprite_from_surface(halved, *surfaces.picture);
    if (source.minimap != nullptr) {
        Sprite minimap{};
        minimap.width = static_cast<uint16_t>(source.minimap_stride);
        minimap.height = static_cast<uint16_t>(height * 2);
        minimap.data = const_cast<uint8_t*>(source.minimap);
        ::oa::present::blend_downsample_sprite(*source.display, minimap, halved);
        return;
    }
    if (source.tile_map == nullptr || source.tile_pixels == nullptr)
        return;
    const int32_t temp_w = width * 2;
    const int32_t temp_h = height * 2;
    ::oa::Surface* temp = host.create_surface(host.user, radar_temp_name, temp_w, temp_h);
    if (temp == nullptr)
        return;
    const int32_t tiles_per_row = game.map_width / 2;
    for (int32_t y = 0; y < temp_h; ++y) {
        const int32_t map_y = map_h * y / temp_h;
        uint8_t* out = temp->pixels + y * temp->pitch;
        for (int32_t x = 0; x < temp_w; ++x) {
            const int32_t map_x = map_w * x / temp_w;
            int32_t tile =
                source.tile_map
                    [tiles_per_row * (map_y / radar_tile_pixels) + map_x / radar_tile_pixels];
            if (tile >= source.tile_count)
                tile = 0;
            out[x] =
                source.tile_pixels
                    [(tile * radar_tile_pixels + map_y % radar_tile_pixels) * radar_tile_pixels +
                     map_x % radar_tile_pixels];
        }
    }
    Sprite mosaic{};
    ::oa::present::sprite_from_surface(mosaic, *temp);
    ::oa::present::blend_downsample_sprite(*source.display, mosaic, halved);
    if (host.free_surface != nullptr)
        host.free_surface(host.user, temp);
}

void radar_free_surfaces(RadarSurfaces& surfaces, const RadarSurfaceHost& host) noexcept {
    if (host.free_surface != nullptr) {
        host.free_surface(host.user, surfaces.picture);
        host.free_surface(host.user, surfaces.mapped);
        host.free_surface(host.user, surfaces.final_image);
    }
    surfaces.picture = nullptr;
    surfaces.mapped = nullptr;
    surfaces.final_image = nullptr;
}

void radar_read_sight(
    const sim::visibility_state::PlayerSightGrid& sight,
    uint8_t viewer,
    std::span<const uint16_t> coverage,
    FogOptions options,
    uint16_t explored_bit,
    int32_t grid_width,
    int32_t grid_height,
    std::vector<uint16_t>& sight_bits,
    std::vector<uint8_t>& covered
) {
    const auto cells = static_cast<std::size_t>(std::max(grid_width, 0)) *
                       static_cast<std::size_t>(std::max(grid_height, 0));
    sight_bits.assign(cells, options.mapping ? uint16_t{0} : explored_bit);
    covered.assign(cells, options.line_of_sight ? uint8_t{0} : uint8_t{1});
    if ((!options.mapping && !options.line_of_sight) || sight.width <= 0 || sight.height <= 0)
        return;
    const auto viewer_bit = static_cast<uint16_t>(1U << (viewer & 0x1fU));
    for (int32_t row = 0; row < grid_height; ++row) {
        const auto sight_row =
            static_cast<std::size_t>(static_cast<int64_t>(row) * sight.height / grid_height);
        for (int32_t column = 0; column < grid_width; ++column) {
            const auto sight_column =
                static_cast<std::size_t>(static_cast<int64_t>(column) * sight.width / grid_width);
            const auto index = sight_row * static_cast<std::size_t>(sight.width) + sight_column;
            const auto cell = static_cast<std::size_t>(row) * static_cast<std::size_t>(grid_width) +
                              static_cast<std::size_t>(column);
            if (options.mapping && index < sight.player_bits.size() &&
                (sight.player_bits[index] & viewer_bit) != 0)
                sight_bits[cell] = explored_bit;
            if (options.line_of_sight && index < coverage.size() &&
                sim::visibility_state::count_in_sight(
                    coverage[index], sim::visibility_state::SightReading::player
                ))
                covered[cell] = 1;
        }
    }
}

void radar_fill_mapped(
    Game& game, const RadarSurfaces& surfaces, const RadarMapInputs& inputs
) noexcept {
    if ((game.radar_blink_flags & radar_flag_mapped_dirty) == 0)
        return;
    game.radar_blink_flags =
        static_cast<uint16_t>(game.radar_blink_flags & ~radar_flag_mapped_dirty);
    if (surfaces.mapped == nullptr || surfaces.picture == nullptr || inputs.sight_grid == nullptr ||
        inputs.coverage == nullptr || inputs.gray_table == nullptr)
        return;
    const auto viewer_bit = static_cast<uint16_t>(1U << (game.viewpoint_player & 0x1f));
    const auto unexplored = game_ui_color(game, ui_color_unexplored);
    const int32_t width = game.radar_width;
    const int32_t height = game.radar_height;
    // The sight grid is sampled at half the map-cell resolution.
    const auto grid_w = game.map_width / 2;
    const auto grid_h = game.map_height;
    const uint8_t* source = surfaces.picture->pixels;
    uint8_t* target = surfaces.mapped->pixels;
    int32_t row_acc = 0;
    for (int32_t row = 0; row < height; ++row) {
        int32_t col_acc = 0;
        for (int32_t col = 0; col < width; ++col) {
            const auto cell = (row_acc / height) * grid_w + col_acc / width;
            auto color = unexplored;
            if ((inputs.sight_grid[cell] & viewer_bit) != 0)
                color = inputs.coverage[cell] == 0 ? inputs.gray_table[*source] : *source;
            *target++ = color;
            ++source;
            col_acc += grid_w;
        }
        row_acc += grid_h / 2;
    }
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags | radar_flag_redraw);
}

uint32_t radar_compose_final(
    World& world,
    const RadarSurfaces& surfaces,
    const RadarSprites& sprites,
    const RadarContactHost& host,
    std::span<RadarHotUnit> hot_units
) noexcept {
    Game& game = world.game;
    uint32_t listed = 0;
    if (surfaces.final_image == nullptr || surfaces.mapped == nullptr ||
        game.map_pixel_width == 0 || game.map_pixel_height == 0)
        return listed;
    ::oa::Surface& target = *surfaces.final_image;
    ::oa::present::blit_surface(&target, surfaces.mapped, 0, 0);
    const auto full_radar = (game.console_flags & console_flag_full_radar) != 0;
    const bool limited = (game.visibility_flags & visibility_flags_radar_limited) != 0;
    const auto viewer = game.viewpoint_player;
    const bool blink = (game.radar_blink_flags & radar_flag_blink) != 0;
    for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
        const Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        if (!full_radar && limited && (unit.flags & radar_contact_flags) == 0 &&
            !shown_as_own(world, unit, viewer, host.allied_units_shown))
            continue;
        const auto x = radar_column(game, unit.position);
        const auto y = radar_row(game, unit.position);
        if (unit.damage_countdown == 0 || blink)
            ::oa::present::draw_sprite(
                &target,
                ::oa::present::gaf_frame(sprites.unit_logos, player_color(world, unit.owner_index)),
                x,
                y
            );
        if (unit.id == game.cursor_unit_id)
            ::oa::present::draw_sprite(
                &target, ::oa::present::gaf_frame(sprites.cursor_logo, 0), x, y
            );
        const UnitDef* def = world_unit_def_of(&world, &unit);
        if ((unit.flags & OA_UNIT_FLAG_SELECTED) != 0 && def != nullptr) {
            if ((unit.state_flags & OA_UNIT_STATE_ACTIVE) != 0 ||
                (def->abilities & OA_UNIT_DEF_ABILITY_ON_OFFABLE) == 0)
                draw_sensor_rings(game, *def, target, x, y);
            if ((def->flags & OA_UNIT_DEF_FLAG_ANTI_WEAPONS) != 0)
                draw_interceptor_rings(world, unit, target, x, y);
        }
        if (listed < hot_units.size()) {
            hot_units[listed] = {unit.id, game.radar_offset_x + x, game.radar_offset_y + y};
            ++listed;
        }
    }
    draw_projectiles(world, target, sprites, host);
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags | radar_flag_redraw);
    return listed;
}

void radar_draw(Game& game, const RadarSurfaces& surfaces, ::oa::Surface& target) noexcept {
    if ((game.radar_blink_flags & radar_flag_redraw) == 0)
        return;
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags & ~radar_flag_redraw);
    if (surfaces.final_image == nullptr)
        return;
    ::oa::present::blit_surface(
        &target, surfaces.final_image, game.radar_offset_x, game.radar_offset_y
    );
    // A view past the map's edges, or wider than the map, is marked as far
    // as the radar's picture reaches.
    Rect32 marked = game.radar_view_rect;
    const int32_t left = game.radar_offset_x;
    const int32_t top = game.radar_offset_y;
    const int32_t right = left + std::max<int32_t>(game.radar_width, 1) - 1;
    const int32_t bottom = top + std::max<int32_t>(game.radar_height, 1) - 1;
    marked.x1 = std::clamp(marked.x1, left, right);
    marked.x2 = std::clamp(marked.x2, left, right);
    marked.y1 = std::clamp(marked.y1, top, bottom);
    marked.y2 = std::clamp(marked.y2, top, bottom);
    ::oa::present::draw_rect_outline(&target, marked, game_ui_color(game, ui_color_radar_marks));
}

void radar_step_blink(Game& game) noexcept {
    if (game.radar_blink_countdown > 0) {
        game.radar_blink_countdown = static_cast<int16_t>(game.radar_blink_countdown - 1);
        return;
    }
    game.radar_blink_countdown = radar_blink_reload;
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags ^ radar_flag_blink);
}

} // namespace oa::present::world_renderer
