// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The megamap of ui.megamap: the whole map drawn over the battlefield's
// rectangle with small pictures of its indestructible features,
// player-coloured category icons and sensor rings, and the downscaled
// terrain both it and the enhanced minimap draw. Clicks on it give ordinary
// orders at the map point they stand for.
#pragma once

#include "oa/core/world.h"
#include "oa/data/defs/categories.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::hud {

/// Where the megamap shows the map: scaled to fit the view, its aspect
/// kept, centred with bars on the long sides.
struct MegamapLayout {
    int32_t left{};      ///< picture's left column on the canvas
    int32_t top{};       ///< its top row
    int32_t width{};     ///< its width in pixels
    int32_t height{};    ///< its height
    int32_t map_width{}; ///< the map in map pixels
    int32_t map_height{};
};

/// Fits the map into the view.
///
/// @param view_left the view's left column
/// @param view_top its top row
/// @param view_width its width
/// @param view_height its height
/// @param map_width the map's width in map pixels
/// @param map_height its height
/// @return the layout; empty for an empty map or view
[[nodiscard]] MegamapLayout megamap_layout(
    int32_t view_left,
    int32_t view_top,
    int32_t view_width,
    int32_t view_height,
    int32_t map_width,
    int32_t map_height
) noexcept;

/// Places a map point on the megamap.
[[nodiscard]] std::array<int32_t, 2>
megamap_point(const MegamapLayout& layout, int32_t map_x, int32_t map_z) noexcept;

/// Returns the map point a megamap pixel stands for, or none off the picture.
[[nodiscard]] std::optional<std::array<int32_t, 2>>
megamap_map_point(const MegamapLayout& layout, int32_t x, int32_t y) noexcept;

/// Reads one map pixel's palette index.
struct TerrainSource {
    void* user{};
    uint8_t (*pixel)(void* user, int32_t map_x, int32_t map_z){};
};

/// The palette entries a picture may take, with each entry's colour in the
/// OKLab space, where distance follows how different two colours look.
struct PerceptualPalette {
    std::vector<uint8_t> entries;          ///< palette indices, ascending
    std::vector<std::array<float, 3>> lab; ///< each entry's lightness and its two colour axes
};

/// Makes the perceptual palette of the palette entries some pixels use.
///
/// @param palette the game palette, four bytes an entry
/// @param pixels palette indices, such as the map's tiles; none takes every entry
/// @return the entries the pixels use, ascending, with their OKLab colours
[[nodiscard]] PerceptualPalette
perceptual_palette(std::span<const uint8_t> palette, std::span<const uint8_t> pixels);

/// Returns the entry of a perceptual palette that looks nearest a colour:
/// the least squared distance in OKLab, the first entry winning a tie.
///
/// @param palette the entries to choose from; not empty
/// @param r red, 0 to 255
/// @param g green
/// @param b blue
/// @return the palette index
[[nodiscard]] uint8_t
nearest_perceptual(const PerceptualPalette& palette, int32_t r, int32_t g, int32_t b);

/// Downscales terrain to a picture: each picture pixel takes the mean
/// colour of the map pixels it covers, truncated, then the nearest palette
/// entry or, dithered, alternates in a checkerboard between the two nearest.
///
/// @param source the map's pixels
/// @param map_width map pixels across
/// @param map_height map pixels down
/// @param width picture width
/// @param height picture height
/// @param palette the game palette, four bytes an entry
/// @param dither dither between the two nearest entries
/// @param perceptual the entries to choose from, nearest as they look
///        (nearest_perceptual); null takes every entry of `palette`, nearest
///        by squared distance in red, green and blue
/// @return the picture's palette indices, rows `width` apart
[[nodiscard]] std::vector<uint8_t> downscale_terrain(
    const TerrainSource& source,
    int32_t map_width,
    int32_t map_height,
    int32_t width,
    int32_t height,
    std::span<const uint8_t> palette,
    bool dither,
    const PerceptualPalette* perceptual
);

/// Returns the palette entry nearest a colour by squared distance in red,
/// green and blue over all 256 entries, the first entry winning a tie.
///
/// @param palette the game palette, four bytes an entry
/// @param r red, 0 to 255
/// @param g green
/// @param b blue
/// @return the palette index
[[nodiscard]] uint8_t
nearest_palette_entry(std::span<const uint8_t> palette, int32_t r, int32_t g, int32_t b);

/// Tells whether the megamap draws a feature (ui.megamap feature-blobs):
/// only one that is indestructible and not reclaimable, such as a rock, a
/// vent or a metal patch made of features. Trees, wrecks and other features
/// that can be destroyed or reclaimed are not drawn.
[[nodiscard]] bool megamap_draws_feature(const FeatureDef& def) noexcept;

/// Palette indices of the mark a drawn feature takes when it has no picture.
inline constexpr uint8_t feature_mark_metal = 0xfe;
inline constexpr uint8_t feature_mark_spire = 0xd3;
inline constexpr uint8_t feature_mark_other = 0xa5;

/// Pixels across and down the mark a drawn feature without a picture takes.
inline constexpr int32_t feature_mark_size = 3;

/// Returns the colour of a drawn feature's mark: features worth metal, a
/// feature described as "Spire" in any case, and the rest.
[[nodiscard]] uint8_t feature_mark_color(const FeatureDef& def) noexcept;

/// A picture shrunk in full colour: four bytes a pixel, red, green, blue
/// and opacity (0 transparent to 255 opaque).
struct ShrunkPicture {
    int32_t width{};
    int32_t height{};
    std::vector<uint8_t> rgba; ///< rows `width` apart
};

/// Shrinks a palette picture so that its longer side becomes `longest`
/// pixels, the other in proportion, rounded to the nearest pixel and at
/// least 1. Each pixel covers a box of the source; it takes the mean
/// colour of the box's opaque pixels, truncated, and an opacity of their
/// share of the box, 0 to 255, rounded.
///
/// @param pixels the picture's palette indices, rows `width` apart
/// @param width the picture's width
/// @param height its height
/// @param transparent the index not drawn
/// @param palette the game palette, four bytes an entry
/// @param longest the shrunk picture's longer side in pixels, at least 1
/// @return the shrunk picture; empty for an empty picture
[[nodiscard]] ShrunkPicture shrink_picture(
    std::span<const uint8_t> pixels,
    int32_t width,
    int32_t height,
    uint8_t transparent,
    std::span<const uint8_t> palette,
    int32_t longest
);

/// Lays a shrunk picture over a palette picture: each pixel's colour mixed
/// with the one under it by its opacity, truncated, then the nearest entry
/// of the whole palette (nearest_palette_entry). Transparent pixels and
/// those off the canvas leave it as it is.
///
/// @param[in,out] canvas the palette picture, rows `canvas_width` apart
/// @param canvas_width its width
/// @param canvas_height its height
/// @param picture the shrunk picture
/// @param left the canvas column of the picture's left edge
/// @param top the canvas row of its top edge
/// @param palette the game palette, four bytes an entry
void blend_picture(
    std::span<uint8_t> canvas,
    int32_t canvas_width,
    int32_t canvas_height,
    const ShrunkPicture& picture,
    int32_t left,
    int32_t top,
    std::span<const uint8_t> palette
);

/// Where and how large a drawn feature's picture goes on the megamap's
/// picture, whose columns and rows are the layout's.
struct FeatureSpot {
    int32_t x{};       ///< column of the footprint's centre
    int32_t y{};       ///< row of the footprint's centre, raised by half the ground's height
    int32_t longest{}; ///< the picture's longer side in pixels
};

/// Places a drawn feature on the megamap's picture. Its footprint's centre
/// is raised by half the height of the ground under its first cell, then
/// scaled to the picture; the
/// picture's longer side is the frame's scaled as the map's width is,
/// rounded to the nearest pixel and at least 2. A frame without a size
/// counts as 32 pixels. Scaled in single precision, truncated.
///
/// @param layout the megamap's layout
/// @param cell_x the feature's first cell column
/// @param cell_z its first cell row
/// @param footprint_x its footprint in cells across, at least 1
/// @param footprint_z its footprint in cells down, at least 1
/// @param ground_height the height of the ground under its first cell
/// @param frame_longest the longer side of its picture's first frame, in pixels
/// @return the spot, in the picture's own columns and rows
[[nodiscard]] FeatureSpot megamap_feature_spot(
    const MegamapLayout& layout,
    int32_t cell_x,
    int32_t cell_z,
    int32_t footprint_x,
    int32_t footprint_z,
    int32_t ground_height,
    int32_t frame_longest
) noexcept;

/// Returns the row of a shrunk feature picture's top edge: the spot's row
/// less the frame's origin row scaled to the shrunk height and rounded, or
/// less the whole shrunk height for a frame without a height.
///
/// @param spot_y the spot's row (FeatureSpot::y)
/// @param origin_y the frame's origin row, in frame pixels
/// @param frame_height the frame's height
/// @param shrunk_height the shrunk picture's height
/// @return the top row
[[nodiscard]] int32_t feature_picture_top(
    int32_t spot_y, int32_t origin_y, int32_t frame_height, int32_t shrunk_height
) noexcept;

/// The icon settings of the megamap's icon file ([Option] of iconcfg.ini).
struct IconOptions {
    uint8_t fill_color{0};        ///< replaced by the owner's dot colour
    uint8_t transparent_color{9}; ///< not drawn
    uint8_t selected_color{0x59}; ///< drawn only while the unit is selected
    uint8_t hover_color{0x54};    ///< the selected colour's stand-in under the pointer
    bool circle_hover{};          ///< a ring marks the hovered unit instead
    bool default_icons{true};     ///< the built-in icon set instead of [Icon]
};

/// One [Icon] line: a unit category word (lower case) and its picture file.
struct IconLine {
    std::string category;
    std::string file;
};

/// The megamap's icon file, read.
struct IconConfig {
    IconOptions options{};
    std::vector<IconLine> lines; ///< category lines, in file order
    std::string unknown_file;    ///< "Unknow": a seen unit no line matches
    std::string nothing_file;    ///< "Nothing": a unit out of sight
    std::string nuke_file;       ///< "NukeIcon"
};

/// Reads an icon file's text: its [Option] numbers and switches and its
/// [Icon] lines, keys without case, values up to a ';'.
///
/// @param text the file
/// @param category_limit category lines kept (ui.megamap icon-categories)
/// @return the settings
[[nodiscard]] IconConfig parse_icon_config(std::string_view text, uint32_t category_limit);

/// The mask of a line's category word, matched without case.
///
/// A category no unit carries is null, which matches nothing. The lookup never
/// adds a category: adding one can move the masks earlier lines point at.
///
/// @param registry the unit table's category registry
/// @param line one [Icon] line
/// @return the category's mask, or null when no unit carries that word
[[nodiscard]] const data::defs::CategoryMask*
icon_category_mask(const data::defs::CategoryRegistry& registry, const IconLine& line) noexcept;

/// Which icon a unit takes.
enum class IconChoice : uint8_t {
    category, ///< the first line whose category holds the unit's type
    unknown,  ///< seen, but no line matches
    nothing,  ///< out of sight
};

/// Chooses a unit's icon: out of sight, the "Nothing" icon; else the first
/// category line holding its type, or the "Unknow" icon.
///
/// @param visible the viewer sees the unit
/// @param type_id its type
/// @param categories each line's category mask, in file order; null entries match nothing
/// @param[out] line the matching line's index for IconChoice::category
/// @return the choice
[[nodiscard]] IconChoice choose_icon(
    bool visible,
    uint16_t type_id,
    std::span<const data::defs::CategoryMask* const> categories,
    std::size_t& line
) noexcept;

/// Recolours one icon pixel for a unit: the fill colour takes the owner's
/// dot colour; the selected colour shows while selected, takes the hover
/// colour under the pointer and is transparent otherwise.
///
/// @param options icon settings
/// @param pixel the icon's palette index
/// @param dot_color the owner's dot colour
/// @param selected the unit is selected
/// @param hovered the pointer is on it
/// @return the palette index, or none for a transparent pixel
[[nodiscard]] std::optional<uint8_t> icon_pixel(
    const IconOptions& options, uint8_t pixel, uint8_t dot_color, bool selected, bool hovered
) noexcept;

/// One sensor ring on the megamap.
struct MegamapRing {
    int32_t radius{}; ///< map pixels
    uint8_t color{};
};

/// The sensor and anti-nuke rings a unit shows: radar, sonar, radar jammer,
/// sonar jammer and interceptor coverage, each when its range reaches the
/// ring's minimum (ui.megamap ring-minimums, in that order) and is above 0.
///
/// @param world the unit's weapons and the UI colours
/// @param unit the unit
/// @param def its type
/// @param minimums the five minimums
/// @param[out] out the rings
/// @return how many were written
uint32_t megamap_rings(
    const World& world,
    const Unit& unit,
    const UnitDef& def,
    const std::array<int32_t, 5>& minimums,
    std::array<MegamapRing, 5>& out
) noexcept;

} // namespace oa::ui::hud
