// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Fog of war over the battlefield: the FOG.GAF tile sets, the gray table,
// and which map features stay drawn on ground outside line of sight.
#include "oa/app/runtime.hpp"

#include "oa/ui/console/game_fields.hpp"
#include "oa/core/map_plot.h"
#include "oa/sim/feature_runtime.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>

namespace oa::app {

void Runtime::apply_match_fog(
    oa::present::world_renderer::Surface& destination,
    int32_t camera_x,
    int32_t camera_y,
    int dest_x,
    int dest_y,
    int dest_w,
    int dest_h,
    float draw_scale,
    FogPasses passes
) {
    if (!match_ || dest_w <= 0 || dest_h <= 0)
        return;
    const auto started = std::chrono::steady_clock::now();
    // In the Full tier the graphics card draws the fog from the grid; the
    // frame keeps the grid and rasterises nothing, and a frame with no fog
    // keeps none.
    const bool card_fog = full_frame_drawn();
    const bool dithered =
        (match_->state().game.graphics_flags & init::preference_flags::dithered_fog) != 0;
    // The fog shows the view player's sight and mapped terrain.
    const auto viewer = match_view_player();
    std::span<const uint16_t> coverage;
    try {
        coverage = match_->player_coverage(viewer);
    } catch (const std::exception&) {
        if (card_fog)
            note_full_fog_grid({}, 0, 0, dithered, true);
        return;
    }
    const auto& sight = match_->sight();
    const bool los_on = match_line_of_sight_on();
    const bool mapping_on = match_mapping_on();
    if (coverage.empty() || sight.width <= 0 || sight.height <= 0 || (!los_on && !mapping_on)) {
        if (card_fog)
            note_full_fog_grid({}, 0, 0, dithered, true);
        return;
    }
    ensure_fog_frames();
    auto zoom_fp = static_cast<uint32_t>(
        std::lround(static_cast<double>(draw_scale <= 0.0F ? 1.0F : draw_scale) * 65536.0)
    );
    if (zoom_fp == 0)
        zoom_fp = 1;
    const oa::present::world_renderer::FogView view{
        dest_x, dest_y, dest_w, dest_h, camera_x, camera_y, zoom_fp
    };
    // The last frame's grid where the sight it reads is the same.
    bool changed = oa::present::world_renderer::update_fog_grid(
        sight,
        viewer,
        coverage,
        {los_on, mapping_on, allied_mapped_bits()},
        view.camera_x,
        view.camera_z,
        oa::present::world_renderer::fog_map_span(zoom_fp, dest_w),
        oa::present::world_renderer::fog_map_span(zoom_fp, dest_h),
        fog_grid_,
        fog_grid_source_
    );
    // One pass alone leaves the other's corners clear: the gray pass then
    // the black pass give the pixels both in one pass give, since the black
    // goes over the gray. The kept grid holds both.
    oa::present::world_renderer::FogGrid one_pass;
    const oa::present::world_renderer::FogGrid* drawn = &fog_grid_;
    if (passes != FogPasses::both) {
        one_pass = fog_grid_;
        for (auto& tile : one_pass.tiles) {
            if (passes == FogPasses::unseen)
                tile.unmapped = 0;
            else
                tile.unseen = 0;
        }
        drawn = &one_pass;
        changed = true;
    }
    const auto& grid = *drawn;
    // The fog pass reads the DitheredFog bit of the match's Game word.
    fog_shading_.dithered = dithered;
    if (card_fog) {
        note_full_fog_grid(grid, view.camera_x, view.camera_z, dithered, changed);
        phase_times_.fog += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - started
        )
                                .count();
        return;
    }
    oa::present::world_renderer::draw_fog_grid(
        destination, view, grid, fog_tiles_, fog_shading_, draw_pool_.get()
    );
    phase_times_.fog += std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - started
    )
                            .count();
}

void Runtime::ensure_fog_frames() {
    if (fog_frames_ready_)
        return;
    static constexpr std::array<std::string_view, 4> gray{"Gray1", "Gray2", "Gray3", "Gray4"};
    static constexpr std::array<std::string_view, 4> black{"Black1", "Black2", "Black3", "Black4"};
    if (match_fog_.sequences.empty())
        append_gaf_file(match_fog_, "anims/FOG.GAF");
    constexpr auto kTile = oa::present::world_renderer::fog_cell_pixels;
    fog_tiles_.art.assign(2 * oa::present::world_renderer::FogTileSet::tiles_per_bank, {});
    for (const auto bank :
         {oa::present::world_renderer::FogTileSet::black,
          oa::present::world_renderer::FogTileSet::gray}) {
        for (int32_t variant = 0; variant < oa::present::world_renderer::fog_tile_variants;
             ++variant) {
            const auto name = bank == oa::present::world_renderer::FogTileSet::black
                                  ? black[static_cast<std::size_t>(variant)]
                                  : gray[static_cast<std::size_t>(variant)];
            const auto* sequence = gaf_sequence(match_fog_, name);
            if (sequence == nullptr)
                continue;
            const auto count = std::min<std::size_t>(
                oa::present::world_renderer::fog_mask_full - 1, sequence->frames.size()
            );
            for (std::size_t frame = 0; frame < count; ++frame) {
                const auto rendered = oa::formats::gaf::render_normal(sequence->frames[frame]);
                if (!rendered.ok())
                    continue;
                const auto& image = *rendered.frame;
                auto& art = fog_tiles_.at(bank, variant, static_cast<uint8_t>(frame + 1));
                // The sprite drawer puts a frame at (tile - origin), so tile texel
                // (x, y) shows frame pixel (x + origin_x, y + origin_y).
                for (int32_t ty = 0; ty < kTile; ++ty) {
                    const int32_t fy = ty + image.origin_y;
                    if (fy < 0 || fy >= image.height)
                        continue;
                    for (int32_t tx = 0; tx < kTile; ++tx) {
                        const int32_t fx = tx + image.origin_x;
                        if (fx < 0 || fx >= image.width)
                            continue;
                        const auto offset = static_cast<std::size_t>(fy) * image.width +
                                            static_cast<std::size_t>(fx);
                        if (offset >= image.coverage.size() || image.coverage[offset] == 0 ||
                            offset >= image.pixels.size() ||
                            image.pixels[offset] == image.transparency_index)
                            continue;
                        const auto texel = static_cast<std::size_t>(ty * kTile + tx);
                        art.opaque[texel] = 1;
                        art.index[texel] = image.pixels[offset];
                    }
                }
            }
        }
    }
    // The gray table depends only on an entry's (r+g+b)/3, so it is kept per
    // level and applies to any RGB the world surface holds.
    oa::Palette palette{};
    for (std::size_t i = 0; i < std::size(palette.entries); ++i)
        palette.entries[i] = {
            match_palette_[i * 4U], match_palette_[i * 4U + 1U], match_palette_[i * 4U + 2U], 0
        };
    std::array<uint8_t, OA_PALETTE_COLORS> levels{};
    oa::present::world_renderer::build_gray_levels(palette, levels);
    for (std::size_t level = 0; level < levels.size(); ++level)
        fog_shading_.gray_levels[level] = {
            palette.entries[levels[level]].r,
            palette.entries[levels[level]].g,
            palette.entries[levels[level]].b
        };
    for (std::size_t index = 0; index < OA_PALETTE_COLORS; ++index)
        fog_shading_.palette_rgb[index] = palette_rgb(static_cast<uint8_t>(index));
    ensure_ui_colors();
    fog_shading_.unmapped_rgb = palette_rgb(ui_colors_[0]);
    fog_shading_.dither_rgb = palette_rgb(0);
    fog_frames_ready_ = true;
}

bool Runtime::feature_hidden_by_fog(uint16_t feature_index, int32_t cell_x, int32_t cell_z) {
    if (!match_ || !selected_tnt_ || feature_index >= selected_tnt_->features.size())
        return false;
    if (feature_fog_rules_.map != &*selected_tnt_ ||
        feature_fog_rules_.hidden_under_gray.size() != selected_tnt_->features.size()) {
        feature_fog_rules_.map = &*selected_tnt_;
        feature_fog_rules_.hidden_under_gray.assign(selected_tnt_->features.size(), 0);
        feature_fog_rules_.footprints.assign(selected_tnt_->features.size(), {});
        for (std::size_t index = 0; index < selected_tnt_->features.size(); ++index) {
            const auto* feature = find_catalog_feature(selected_tnt_->features[index].name);
            if (feature == nullptr)
                continue;
            feature_fog_rules_.footprints[index] = {
                feature->terrain.footprint_x, feature->terrain.footprint_z
            };
            if (feature->terrain.no_draw_under_gray)
                feature_fog_rules_.hidden_under_gray[index] = 1;
        }
    }
    if (feature_fog_rules_.hidden_under_gray[feature_index] == 0)
        return false;
    const auto& spatial = match_->spatial();
    if (cell_x >= 0 && cell_z >= 0 && spatial.terrain_width != 0) {
        const auto plot = static_cast<std::size_t>(cell_z) * spatial.terrain_width +
                          static_cast<std::size_t>(cell_x);
        const auto& ignore_los = ui_rules().map_features_ignore_los;
        oa::present::world_renderer::FeatureOwnerRule rule{};
        rule.enabled = ignore_los.enabled;
        rule.map_owner = static_cast<uint8_t>(ignore_los.feature_owner);
        // A feature counts as the map's while its plot holds the feature the
        // map put there; the plots keep 3.1c's owner slot, which the match
        // hashes and saves.
        const auto map_plot = static_cast<std::size_t>(cell_z) * selected_tnt_->attribute_width +
                              static_cast<std::size_t>(cell_x);
        const bool placed_by_map =
            rule.enabled && prepared_map_ && map_plot < prepared_map_->collision_plots.size() &&
            prepared_map_->collision_plots[map_plot].feature_word == feature_index;
        if (plot < spatial.plots.size() &&
            oa::present::world_renderer::feature_drawn_without_sight(
                static_cast<uint8_t>(
                    (spatial.plots[plot].flags & OA_PLOT_FLAG_PLAYER_FEATURE_MASK) >>
                    oa::sim::feature_runtime::plot_player_shift
                ),
                placed_by_map,
                static_cast<uint8_t>(match_view_player()),
                rule
            ))
            return false;
    }
    uint8_t plot_height = 0;
    if (cell_x >= 0 && cell_z >= 0 &&
        static_cast<uint32_t>(cell_x) < selected_tnt_->attribute_width) {
        const auto attribute = static_cast<std::size_t>(cell_z) * selected_tnt_->attribute_width +
                               static_cast<std::size_t>(cell_x);
        if (attribute < selected_tnt_->attributes.size())
            plot_height = selected_tnt_->attributes[attribute].height;
    }
    const auto& footprint = feature_fog_rules_.footprints[feature_index];
    try {
        if (match_->cell_or_footprint_corner_shown(
                static_cast<uint8_t>(match_view_player()),
                static_cast<int16_t>(cell_x),
                static_cast<int16_t>(cell_z),
                footprint[0],
                footprint[1],
                plot_height
            ))
            return false;
        // Without the line-of-sight rule a feature shows on mapped ground,
        // and under allied vision the view is shown the ground its allies
        // mapped too (allied_mapped_bits): a feature at the cell or its
        // footprint's far corner on such ground shows, as that ground does.
        const auto allies = allied_mapped_bits();
        if (allies == 0 || (match_->state().game.visibility_flags &
                            oa::sim::visibility_state::update_sight_grid) != 0)
            return true;
        // The sight cell under a point of the feature's plot: a cell is 32
        // world units, and a point is raised by half its height, as the
        // shown test reads it.
        constexpr int32_t units_per_cell = 16;
        constexpr int32_t sight_cell_shift = 5;
        const auto& grid = match_->sight();
        const auto mapped_by_allies = [&](int32_t x, int32_t z) {
            const auto column = static_cast<int16_t>(x * units_per_cell) >> sight_cell_shift;
            const auto row =
                (static_cast<int16_t>(z * units_per_cell) - (plot_height >> 1)) >> sight_cell_shift;
            if (column < 0 || row < 0 || column >= grid.width || row >= grid.height)
                return false;
            const auto index =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(grid.width) +
                static_cast<std::size_t>(column);
            return index < grid.player_bits.size() && (grid.player_bits[index] & allies) != 0;
        };
        if (mapped_by_allies(cell_x, cell_z) ||
            mapped_by_allies(cell_x + footprint[0], cell_z + footprint[1]))
            return false;
        return true;
    } catch (const std::exception&) {
        return true;
    }
}

bool Runtime::match_mapping_on() const {
    if (match_ && watched_sight_ != WatchedSight::game)
        return watched_sight_ == WatchedSight::player;
    return match_ &&
           (match_->state().game.visibility_flags & oa::ui::console::visibility_flag::mapping) != 0;
}

bool Runtime::match_line_of_sight_on() const {
    if (match_ && watched_sight_ != WatchedSight::game)
        return watched_sight_ == WatchedSight::player;
    return match_ && (match_->state().game.visibility_flags &
                      oa::ui::console::visibility_flag::line_of_sight) != 0;
}

} // namespace oa::app
