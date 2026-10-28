// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The radar over the match's Game block: surfaces, blips, the per-tick
// compose and mapped refresh.
#include "oa/app/runtime.hpp"

#include "oa/present/gaf_sprites.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/weapon_execution/weapon_keys.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

namespace oa::app {

namespace {

// Ticks between a player slot's deadlines, at which the
// viewpoint slot refreshes the mapped radar image.
constexpr uint32_t player_update_period = 30;
constexpr char fx_path[] = "anims/FX.GAF";

oa::Surface* radar_host_create(void*, const char*, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return nullptr;
    auto* surface = new oa::Surface{};
    auto* pixels =
        new uint8_t[static_cast<std::size_t>(width) * static_cast<std::size_t>(height)]();
    oa::present::init_surface(*surface, width, height, width, pixels);
    return surface;
}

void radar_host_free(void*, oa::Surface* surface) {
    if (surface == nullptr)
        return;
    delete[] surface->pixels;
    delete surface;
}

oa::present::world_renderer::RadarSurfaceHost radar_host() {
    oa::present::world_renderer::RadarSurfaceHost host;
    host.create_surface = radar_host_create;
    host.free_surface = radar_host_free;
    return host;
}

} // namespace

void Runtime::RadarState::release() {
    oa::present::world_renderer::radar_free_surfaces(surfaces, radar_host());
    radar_host_free(nullptr, well);
    well = nullptr;
    built_for = nullptr;
    hot_unit_count = 0;
    game_picture.clear();
}

Runtime::RadarState::~RadarState() {
    release();
}

void Runtime::load_radar_sprites() {
    auto& radar = radar_state_;
    if (radar.fx.sequences.empty()) {
        try {
            const auto status = oa::present::relocate_gaf(assets_.read(fx_path).bytes, radar.fx);
            if (status != oa::present::GafStatus::ok)
                std::cerr << fx_path << ": " << oa::present::gaf_status_text(status) << '\n';
        } catch (const std::exception& error) {
            std::cerr << fx_path << " unavailable: " << error.what() << '\n';
        }
    }
    radar.sprites.unit_logos = oa::present::find_gaf_sequence(radar.fx, "radlogo");
    radar.sprites.cursor_logo = oa::present::find_gaf_sequence(radar.fx, "radlogohigh");
    radar.sprites.weapon_logos = oa::present::find_gaf_sequence(radar.fx, "nuclogo");
}

void Runtime::ensure_radar_surfaces() {
    namespace wr = oa::present::world_renderer;
    auto& radar = radar_state_;
    if (!match_ || !selected_tnt_)
        return;
    auto& world = match_->state();
    if (radar.built_for == &world)
        return;
    radar.release();
    auto& game = world.game;
    if (game.map_pixel_width <= 0 || game.map_pixel_height <= 0)
        return;
    // The session display's pair-mix and gray tables, both built from
    // PALETTE.PAL, the match palette.
    if (const auto* gray = display_.context.gray_table; gray != nullptr)
        std::copy_n(gray, radar.gray_table.size(), radar.gray_table.begin());
    const auto& map = *selected_tnt_;
    wr::RadarPictureSource source;
    source.display = &display_.context;
    const auto tiles = static_cast<std::size_t>(map.tile_width) * map.tile_height;
    if (map.attribute_width == map.tile_width * 2U && map.tile_indices.size() >= tiles &&
        map.tile_palette_indices.size() >= static_cast<std::size_t>(map.tile_count) * 1024U) {
        source.tile_map = map.tile_indices.data();
        source.tile_count = static_cast<int32_t>(map.tile_count);
        source.tile_pixels = map.tile_palette_indices.data();
    }
    const auto edge = static_cast<uint32_t>(2 * wr::radar_picture_size);
    if (map.minimap.has_value() && map.minimap->width >= edge && map.minimap->height >= edge &&
        map.minimap->palette_indices.size() >=
            static_cast<std::size_t>(map.minimap->width) * map.minimap->height) {
        source.minimap = map.minimap->palette_indices.data();
        source.minimap_stride = static_cast<int32_t>(map.minimap->width);
    }
    const auto host = radar_host();
    wr::radar_build_picture(game, radar.surfaces, source, host);
    enhance_radar_picture();
    wr::radar_init_surfaces(game, radar.surfaces, host);
    radar.well =
        radar_host_create(nullptr, nullptr, wr::radar_picture_size, wr::radar_picture_size);
    if (radar.surfaces.mapped == nullptr || radar.surfaces.final_image == nullptr ||
        radar.well == nullptr) {
        radar.release();
        return;
    }
    load_radar_sprites();
    radar.hot_units.assign(world.unit_slot_count, {});
    radar.built_for = &world;
    radar.tick = match_->simulation().tick;
    const auto* viewer = oa::world_player(&world, game.viewpoint_player);
    radar.viewer_deadline = viewer != nullptr ? viewer->next_economy_tick : 0;
    radar.reset_sight = false;
    refresh_radar_mapped();
    compose_radar_final();
}

void Runtime::refresh_radar_mapped() {
    namespace wr = oa::present::world_renderer;
    auto& radar = radar_state_;
    auto& game = match_->state().game;
    // The radar fill walks a 32-pixel cell grid; the match sight grid is
    // resampled onto it and read as the battlefield's fog reads it: the
    // view player's mapped cells and sight, so that the radar shows explored
    // exactly the ground the battlefield does not keep black. Disabled
    // mapping or line of sight reads as fully explored or seen.
    const int grid_w = std::max(1, game.map_width / 2);
    const int grid_h = std::max(1, game.map_height / 2);
    const bool los_on = match_line_of_sight_on();
    const auto viewer = match_view_player();
    // A viewer outside the player table has no live coverage.
    std::span<const uint16_t> live{};
    if (los_on && viewer < OA_PLAYER_COUNT)
        live = match_->player_coverage(viewer);
    wr::radar_read_sight(
        match_->sight(),
        viewer,
        live,
        {los_on, match_mapping_on()},
        static_cast<uint16_t>(1u << (game.viewpoint_player & 0x1fu)),
        grid_w,
        grid_h,
        radar.sight_bits,
        radar.coverage
    );
    // The match's sight grids keep no dirty mark for the mapped image, so
    // every refresh redraws it.
    game.radar_blink_flags =
        static_cast<uint16_t>(game.radar_blink_flags | wr::radar_flag_mapped_dirty);
    wr::radar_fill_mapped(
        game,
        radar.surfaces,
        {radar.sight_bits.data(), radar.coverage.data(), radar.gray_table.data()}
    );
}

void Runtime::compose_radar_final() {
    auto& radar = radar_state_;
    oa::present::world_renderer::RadarContactHost host{};
    host.user = this;
    host.point_visible = [](void* user, const oa::FixedVec3& position) {
        auto& self = *static_cast<Runtime*>(user);
        try {
            return self.match_->point_shown(
                self.match_->state().game.viewpoint_player,
                {static_cast<uint32_t>(position.x),
                 static_cast<uint32_t>(position.y),
                 static_cast<uint32_t>(position.z)}
            );
        } catch (const std::exception&) {
            return false;
        }
    };
    host.projectile_hidden = [](void* user, const oa::Projectile& shot) {
        const auto& self = *static_cast<Runtime*>(user);
        const auto& world = self.match_->state();
        const auto* weapon = oa::world_weapon_def(&world, shot.def);
        return weapon != nullptr &&
               oa::sim::weapon_execution::silent_ownerless_shot(
                   shot, *weapon, self.match_->rules_view().weapon(oa::oa_weapon_def_id(weapon))
               );
    };
    host.allied_units_shown = ui_rules().allied_unit_display.enabled;
    radar.hot_unit_count = oa::present::world_renderer::radar_compose_final(
        match_->state(), radar.surfaces, radar.sprites, host, radar.hot_units
    );
}

void Runtime::run_radar_ticks() {
    namespace wr = oa::present::world_renderer;
    auto& radar = radar_state_;
    auto& world = match_->state();
    auto& game = world.game;
    if (radar.reset_sight) {
        // A sight reset fills the mapped image and composes at once.
        radar.reset_sight = false;
        refresh_radar_mapped();
        compose_radar_final();
    }
    const uint32_t now = match_->simulation().tick;
    if (now <= radar.tick) {
        radar.tick = now;
        return;
    }
    const auto ticks = now - radar.tick;
    const auto* viewer = oa::world_player(&world, game.viewpoint_player);
    const auto deadline = viewer != nullptr ? viewer->next_economy_tick : radar.viewer_deadline;
    const bool refreshed = deadline != radar.viewer_deadline;
    const bool refreshed_now = refreshed && (ticks == 1 || deadline - player_update_period == now);
    for (uint32_t step = 1; step < ticks; ++step)
        wr::radar_step_blink(game);
    if (refreshed && !refreshed_now)
        refresh_radar_mapped();
    compose_radar_final();
    if (refreshed_now)
        refresh_radar_mapped();
    wr::radar_step_blink(game);
    radar.tick = now;
    radar.viewer_deadline = deadline;
}

} // namespace oa::app
