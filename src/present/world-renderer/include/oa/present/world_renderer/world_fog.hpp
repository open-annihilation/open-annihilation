// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Fog-of-war edge grid and gray shade table. The battlefield is covered by a
// grid of 32-pixel tiles offset half a sight cell from the sight grid, so each
// tile's four corners sit on four sight-cell centres. Each tile holds two
// four-bit corner masks: one for never-mapped corners (drawn with the black
// FOG.GAF tiles) and one for corners outside current line of sight (drawn by
// graying the terrain through the gray FOG.GAF tiles).

#include "oa/present/surface.h"
#include "oa/sim/visibility_state.hpp"
#include "oa/present/world_renderer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::present::world_renderer {

inline constexpr int32_t fog_cell_pixels = 32;
// Corner bits of a fog tile mask.
inline constexpr uint8_t fog_corner_top_left = 0x01;
inline constexpr uint8_t fog_corner_top_right = 0x02;
inline constexpr uint8_t fog_corner_bottom_left = 0x04;
inline constexpr uint8_t fog_corner_bottom_right = 0x08;
// Every corner fogged: the whole tile is filled instead of masked.
inline constexpr uint8_t fog_mask_full = 0x0f;
// FOG.GAF holds four variants of each tile set, chosen per tile.
inline constexpr int32_t fog_tile_variants = 4;
// 16.16 zoom of an unscaled view.
inline constexpr uint32_t fog_zoom_one = 65536u;

struct FogOptions {
    bool line_of_sight{}; // gray tiles are built with LOS enabled, and see view_greys
    bool mapping{true};   // without mapping every cell counts as mapped
    /// Further players, as bits of the sight grid's mapped words, whose
    /// mapped cells show as explored besides the viewer's own: under allied
    /// vision (intel.allied-los-sharing) the players who ally the viewer
    /// (allied_mapped_players); 0 for the viewer's own alone, as in 3.1c.
    uint16_t shared_mapped{};
};

/// Returns the players whose explored ground a viewer is shown under allied
/// vision (intel.allied-los-sharing): each player in use, other than the
/// viewer, whose alliance row names the viewer, in the hack's direction
/// (a player who allies the viewer grants it its sight). Read from the
/// alliance rows as they stand, so that an alliance ended takes the
/// player's ground off the views.
///
/// @param game game block holding the players and their alliance rows
/// @param viewer the player whose view is drawn, 0..9; another gives none
/// @return a bit for each such player, as the sight grid's mapped words hold them
[[nodiscard]] uint16_t allied_mapped_players(const Game& game, uint8_t viewer) noexcept;

/// Tells whether the views grey explored ground out of sight: under the
/// line-of-sight rule, and under permanent line of sight where the views
/// show other players' explored ground (FogOptions::shared_mapped), so
/// that ground only they explored is told from the viewer's own.
///
/// @param options line-of-sight and mapping rules
/// @return true when a view greys explored ground out of sight
[[nodiscard]] bool view_greys(FogOptions options) noexcept;

/// Tells whether a view shows a sight cell in sight: under the
/// line-of-sight rule where the viewer's count of the stamps seeing it is
/// not zero, read as the player is shown it; under permanent line of sight
/// with other players' ground shown, where the viewer's own mapped bit is
/// set, so that ground only they explored shows out of sight, as ground
/// does under the other rules, and the viewer's own explored ground stays
/// clear, as in 3.1c; else everywhere.
///
/// @param count the viewer's count of the stamps seeing the cell
/// @param mapped the cell's mapped word, a bit for each player
/// @param own_bits the viewer's own bits of the mapped word
/// @param options line-of-sight and mapping rules
/// @return true when the view shows the cell in sight
[[nodiscard]] bool cell_shown_in_sight(
    uint16_t count, uint16_t mapped, uint16_t own_bits, FogOptions options
) noexcept;

struct FogTile {
    uint8_t unmapped{}; // black FOG.GAF mask
    uint8_t unseen{};   // gray FOG.GAF mask
};

struct FogGrid {
    int32_t width{}; // tiles
    int32_t height{};
    // Sight cell whose centre is the top-left corner of tile (0, 0).
    int32_t first_cell_x{};
    int32_t first_cell_z{};
    // Screen offset of tile (0, 0) from the camera origin (-31..0).
    int32_t offset_x{};
    int32_t offset_z{};
    // Tile-set variant phase: variant = (row + column + phase) & 3.
    int32_t variant_phase{};
    std::vector<FogTile> tiles{};

    /// Returns the tile at a grid column and row.
    ///
    /// @param column tile column, 0..width-1
    /// @param row tile row, 0..height-1
    /// @return the tile; the position is not checked
    [[nodiscard]] const FogTile& at(int32_t column, int32_t row) const noexcept {
        return tiles
            [static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(column)];
    }
};

// One 32x32 FOG.GAF tile: the palette index of each texel and whether the
// texel covers the ground at all.
struct FogTileArt {
    std::array<uint8_t, fog_cell_pixels * fog_cell_pixels> index{};
    std::array<uint8_t, fog_cell_pixels * fog_cell_pixels> opaque{};
};

// The FOG.GAF tile sets: black tiles for never-mapped corners and gray tiles
// for corners outside line of sight, four variants of each, one tile per
// partial corner mask 1..14.
struct FogTileSet {
    enum Bank : uint8_t { black = 0, gray = 1 };

    static constexpr std::size_t tiles_per_bank =
        static_cast<std::size_t>(fog_tile_variants) * (fog_mask_full - 1);

    std::vector<FogTileArt> art; // empty until loaded

    /// Returns whether both tile banks are loaded.
    [[nodiscard]] bool loaded() const noexcept { return art.size() == 2 * tiles_per_bank; }

    /// Returns the tile art for a bank, variant and partial corner mask.
    ///
    /// @param bank black or gray tiles
    /// @param variant tile-set variant, 0..3
    /// @param mask partial corner mask, 1..14
    /// @return the tile art; the arguments are not checked
    [[nodiscard]] FogTileArt& at(Bank bank, int32_t variant, uint8_t mask) {
        return art
            [static_cast<std::size_t>(bank) * tiles_per_bank +
             static_cast<std::size_t>(variant) * (fog_mask_full - 1) + (mask - 1U)];
    }

    /// Returns the tile art for a bank, variant and partial corner mask.
    ///
    /// @param bank black or gray tiles
    /// @param variant tile-set variant, 0..3
    /// @param mask partial corner mask, 1..14
    /// @return the tile art; the arguments are not checked
    [[nodiscard]] const FogTileArt& at(Bank bank, int32_t variant, uint8_t mask) const {
        return art
            [static_cast<std::size_t>(bank) * tiles_per_bank +
             static_cast<std::size_t>(variant) * (fog_mask_full - 1) + (mask - 1U)];
    }
};

// Colours the fog draws with on an RGB world surface.
struct FogShading {
    // RGB of the gray-table entry for each (r+g+b)/3 level of a grayed pixel.
    std::array<std::array<uint8_t, 3>, OA_PALETTE_COLORS> gray_levels{};
    // RGB of each palette index, for the black tile texels.
    std::array<std::array<uint8_t, 3>, OA_PALETTE_COLORS> palette_rgb{};
    // Solid fill of a tile whose four corners were never mapped (UI colour 0).
    std::array<uint8_t, 3> unmapped_rgb{};
    // Dithered fog clears every other grayed pixel to palette index 0 instead.
    std::array<uint8_t, 3> dither_rgb{};
    bool dithered{};
    // Dithered fog drawn as the even tone its every other pixel averages to:
    // each pixel out of sight half way to the dither colour. For a picture
    // whose pixels are enlarged or resampled after the fog, where a dither
    // of single pixels would grow into blocks or break into a pattern.
    bool dither_even{};
};

// The zoomed map view the fog is drawn over: destination rectangle on the
// surface, the map pixel under its top-left corner and the 16.16 zoom of the
// terrain DDA (destination pixel d shows map pixel floor(d * 65536 / zoom)).
struct FogView {
    int32_t dest_x{};
    int32_t dest_y{};
    int32_t dest_width{};
    int32_t dest_height{};
    int32_t camera_x{};
    int32_t camera_z{};
    uint32_t zoom_fp{fog_zoom_one};
};

/// Returns the map pixels shown by `count` destination pixels at a zoom.
///
/// @param zoom_fp 16.16 zoom; 0 is treated as 1
/// @param count destination pixels
/// @return (count - 1) * 65536 / zoom_fp + 1, or 0 for no pixels
[[nodiscard]] int32_t fog_map_span(uint32_t zoom_fp, int32_t count) noexcept;

/// Builds the fog tile masks for a view whose top-left map pixel is the camera.
///
/// Corners off the map are left clear, then the masks of the tiles straddling
/// the map border are copied outward so the border shows no fog edge. The
/// straddling tile is found from the border cell for any view size and zoom;
/// 3.1c copies the corners of the first and last-but-one tile, which straddle
/// the border only when the view is a whole number of tiles wide.
///
/// @param sight the match's sight grid, whose cells hold a mapped bit for each player
/// @param viewer the player whose view the fog shows: its mapped bit is read, with the bits
///        options.shared_mapped names
/// @param coverage the viewer's per-cell count of the stamps seeing it, read as the player is
///        shown it (sim::visibility_state::SightReading::player)
/// @param options line-of-sight and mapping rules
/// @param camera_x camera map-pixel X
/// @param camera_z camera map-pixel Z (screen row)
/// @param view_width view width in map pixels
/// @param view_height view height in map pixels
/// @return the fog grid
[[nodiscard]] FogGrid build_fog_grid(
    const sim::visibility_state::PlayerSightGrid& sight,
    uint8_t viewer,
    std::span<const uint16_t> coverage,
    FogOptions options,
    int32_t camera_x,
    int32_t camera_z,
    int32_t view_width,
    int32_t view_height
);

/// What a fog grid was last built from (update_fog_grid): the view, the
/// rules and the viewer, and for each sight cell the grid reads, row by
/// row, whether it lies on the map, is in sight and is mapped by the
/// viewer.
struct FogGridSource {
    bool built{}; ///< a grid was built from it
    int32_t camera_x{};
    int32_t camera_z{};
    int32_t view_width{};
    int32_t view_height{};
    int32_t sight_width{};
    int32_t sight_height{};
    uint8_t viewer{};
    FogOptions options{};
    std::vector<uint8_t>
        cells; ///< fog_source_on_map, fog_source_in_sight and fog_source_mapped bits
};

/// A sight cell of a FogGridSource lies on the map.
inline constexpr uint8_t fog_source_on_map = 1;
/// A sight cell of a FogGridSource is shown in sight (cell_shown_in_sight).
inline constexpr uint8_t fog_source_in_sight = 2;
/// A sight cell of a FogGridSource is mapped by the viewer.
inline constexpr uint8_t fog_source_mapped = 4;

/// Builds a fog grid as build_fog_grid does, unless the grid holds one
/// built from the same source already: the same view, rules and viewer,
/// and every sight cell the grid reads on or off the map, in or out of
/// sight and mapped or not as it was. Reading the cells costs a fraction
/// of building the grid, which the frames between two of the
/// simulation's updates of sight would otherwise build alike.
///
/// @param sight the match's sight grid, as build_fog_grid reads it
/// @param viewer the player whose view the fog shows
/// @param coverage the viewer's per-cell count of the stamps seeing it
/// @param options line-of-sight and mapping rules
/// @param camera_x camera map-pixel X
/// @param camera_z camera map-pixel Z (screen row)
/// @param view_width view width in map pixels
/// @param view_height view height in map pixels
/// @param[in,out] grid the grid last built from `source`, built again where the source changed
/// @param[in,out] source what the grid was built from, brought up to date
/// @return true when the grid was built again, false when it was kept
bool update_fog_grid(
    const sim::visibility_state::PlayerSightGrid& sight,
    uint8_t viewer,
    std::span<const uint16_t> coverage,
    FogOptions options,
    int32_t camera_x,
    int32_t camera_z,
    int32_t view_width,
    int32_t view_height,
    FogGrid& grid,
    FogGridSource& source
);

/// Draws the fog grid over the view.
///
/// Wholly never-mapped tiles are filled, wholly unseen tiles grayed (or
/// dithered), and partial tiles drawn through their FOG.GAF masks, gray before
/// black. Each row of the grid is a band that draws only the surface rows
/// under it, on the pool's threads when one is given; every row is the same
/// whichever thread draws it.
///
/// @param[in,out] surface RGB world surface
/// @param view destination rectangle, camera and zoom
/// @param grid fog grid from build_fog_grid
/// @param tiles FOG.GAF tile art; without it partial tiles are skipped
/// @param shading gray levels, palette and fill colours
/// @param pool threads to draw the grid's rows on; null draws them on the calling thread
void draw_fog_grid(
    Surface& surface,
    const FogView& view,
    const FogGrid& grid,
    const FogTileSet& tiles,
    const FogShading& shading,
    platform::job_pool::Pool* pool = nullptr
);

/// Finds the nearest palette entry to each gray level among entries of similar brightness.
///
/// This is the search the game's gray table makes for an entry whose
/// (r+g+b)/3 is L.
///
/// @param palette the game palette
/// @param[out] levels palette index for each level (L, L, L)
void build_gray_levels(const Palette& palette, std::array<uint8_t, OA_PALETTE_COLORS>& levels);

/// Plot owner slot of a feature placed by the map loader in 3.1c.
inline constexpr uint8_t map_feature_owner = 10;
/// Plot owner slot of a feature drawn whatever the line of sight
/// (ui.map-features-ignore-los).
inline constexpr uint8_t sight_free_feature_owner = 11;

/// How the owner slot of a map-placed feature decides whether it is drawn
/// outside line of sight (ui.map-features-ignore-los); the default is 3.1c's.
struct FeatureOwnerRule {
    /// The rule applies: the map's features take `map_owner`, and a feature
    /// whose owner is sight_free_feature_owner is drawn without a sight test.
    bool enabled{};
    /// The owner slot the map loader gives its features (0-15).
    uint8_t map_owner{map_feature_owner};
};

/// Tells whether a feature that hides under the gray fog is drawn without its
/// sight test.
///
/// The feature's plot owner slot is the one its plot records, or, for a feature the
/// map placed while the rule applies, the rule's map owner. A feature whose
/// owner is the viewer is drawn; under the rule, so is one whose owner is
/// sight_free_feature_owner. A map owner of a player slot therefore shows the map's
/// features to that player alone, and map_feature_owner leaves 3.1c's sight test.
///
/// @param plot_owner the owner slot the feature's plot records
/// @param placed_by_map the map placed the feature and it has not been replaced
/// @param viewer the viewpoint player
/// @param rule the rule in force
/// @return true when the feature is drawn whatever the sight
[[nodiscard]] constexpr bool feature_drawn_without_sight(
    uint8_t plot_owner, bool placed_by_map, uint8_t viewer, const FeatureOwnerRule& rule
) noexcept {
    const uint8_t owner = rule.enabled && placed_by_map && plot_owner == map_feature_owner
                              ? static_cast<uint8_t>(rule.map_owner & 0xfu)
                              : plot_owner;
    return owner == viewer || (rule.enabled && owner == sight_free_feature_owner);
}

} // namespace oa::present::world_renderer
