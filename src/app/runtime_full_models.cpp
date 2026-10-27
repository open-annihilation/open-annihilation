// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's model stage (runtime_full.hpp): the planner's units, 3D
// features, projectiles, debris pieces and shatter fragments as triangles
// the card draws, and their shadows. A unit's polygons come from its model's
// mesh (model_meshes.hpp), each corner placed from the piece transform the
// planner rebuilt, with today's arithmetic for the path the processor draws
// the piece by: a cached piece as the cached image places it, a moving piece
// as the flat draw places it, a carried unit as its carrier composes it.
// A textured quad is drawn as strips across its rows, so that its texels
// run as the processor's walk runs them.
// Within a unit drawn with a depth plane the polygons are drawn in the order
// the plane shows them: of two that overlap, the one higher where they
// overlap goes later, and at the same height the one the processor draws
// later, as the plane keeps a later pixel of the same height; the rest
// lowest mean depth first. Where no order shows the plane's picture, as
// where two polygons cross or the overlaps go round in a circle, a polygon
// leaves out the parts that lie under polygons drawn before it. A unit
// without one keeps the processor's order. A polygon of a unit's picture
// filled with the image key, which the image leaves clear, draws nothing and
// cuts what it covers out of the polygons drawn before it. The palette's
// tables are approximated as the design says: the shade rows as a per-vertex
// multiplier with a bright page for the rows above unlit, the alpha table as
// alpha 0.5, the blue table as a halved colour and an additive lift, the
// nanoframe's bands per polygon with its outline as the processor finds its
// pixels. The texture frames are placed on sprite pages (sprite_pages.hpp)
// on first sight, in two variants: keyed, where the image key is transparent
// as in a cached image, and flat, where it is a colour as in a flat draw.
// Nothing here writes the match or the planner's state.
#include "runtime_full.hpp"
#include "renderer_water_shading.hpp"
#include "renderer_aircraft_shadow.hpp"

#include "oa/base/game_math.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/polygon.hpp"
#include "oa/sim/effect_particles.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace oa::app::full {

namespace {

namespace gw = oa::present::gpu_world;
namespace draw = oa::present::model;
namespace fx = oa::sim::effect_particles;
using oa::formats::objects3d::FixedVector3;
using oa::formats::objects3d::Model;
using oa::formats::objects3d::Object;
using oa::sim::model_runtime::PieceFlag;
using oa::sim::model_runtime::PieceState;

constexpr uint16_t piece_visible = static_cast<uint16_t>(PieceFlag::visible);
constexpr uint16_t piece_cached = static_cast<uint16_t>(PieceFlag::cached);
constexpr uint16_t piece_shaded = static_cast<uint16_t>(PieceFlag::shaded);

/// The multiplier a shade row scales a colour by; row 15, the unlit row, is
/// 1.03, which the normal page draws as 1.
constexpr float shade_row_scale = 0.06875F;
/// The first shade row whose multiplier is above 1: such a polygon draws
/// from the bright page with every multiplier halved.
constexpr int32_t first_bright_row = 16;
/// The alpha a cloaked unit, a shadow and the debug overlay's units draw
/// at: the alpha table's mix of two colours in equal parts.
constexpr float half_alpha = 0.5F;
/// Half the vertex's colour, which with half_alpha gives the 50% mix.
constexpr float half_colour = 0.5F;
/// Where a frame's corner lies within its corner texel. Drawn NEAREST at a
/// whole-number zoom, a hair inside the texel's edge, so that the texel
/// read at each pixel is the one today's walk reads, which floors the
/// interpolated texel coordinate; drawn LINEAR at another zoom, the texel's
/// centre, so that the filter blends the neighbours evenly about it.
constexpr float texel_edge = 1.0F / 64.0F;
constexpr float texel_centre = 0.5F;
/// The blue table halves a colour and lifts its blue channel by this much.
constexpr uint8_t underwater_blue_lift = 0x32;
constexpr float progress_scale = 255.0F;
/// The progress bands of the build effect and the span of each.
constexpr int32_t nano_band_first = 0xeb;
constexpr int32_t nano_band_second = 0xc8;
constexpr int32_t nano_band_third = 0x73;
constexpr int32_t nano_band_fourth = 0x1e;
constexpr int32_t nano_span_first = 0x14;
constexpr int32_t nano_span_second = 0x23;
constexpr int32_t nano_span_middle = 0x55;
constexpr int32_t depth_byte_max = 0xff;
/// The band below a threshold the effect colours.
constexpr int32_t nano_band_depth = 4;
/// The corners of a textured primitive the image fills.
constexpr size_t quad_corner_count = 4;
/// The most a textured quad's strips move its texels along a row from
/// where the processor's walk puts them, in pixels of the target. A strip
/// whose width changes by d pixels from its top to its bottom moves them
/// by up to d / 4, at its middle row.
constexpr float strip_shift_allowed = 0.25F;
/// The most a textured quad's two triangles may move its texels from where
/// the walk puts them, in pixels of the target, for the quad to be drawn as
/// them with no strips: half a pixel, within which no texel shows anywhere
/// but where the walk would show it. Two triangles move them by up to a
/// quarter of how far the quad is from a parallelogram (twist_of), which
/// shrinks with the zoom: over the installed game's units about a third of
/// the textured quads keep their strips at zoom 1, a fifth at 0.5, one in
/// sixteen at 0.25 and one in forty at the Full tier's floor of a sixth.
constexpr double two_triangle_shift_allowed = 0.5;
/// Where a pixel's centre lies within it, across and down.
constexpr float pixel_centre = 0.5F;
/// Past this many vertices or indices in the frame, textured quads are
/// drawn as their two triangles, so that a frame of many models stays
/// small.
constexpr size_t most_strip_frame_vertices = size_t{1} << 19;
constexpr size_t most_strip_frame_indices = size_t{1} << 21;
/// The empty map pixels kept round the plane a nanoframe's outline is
/// found on, past the rows' last pixels the outline reaches.
constexpr int32_t outline_plane_margin = 2;
/// The most map pixels a side of that plane takes; a model wider or
/// taller than that draws no outline.
constexpr int32_t most_outline_plane_side = 4096;
/// The least area two polygons must share, in square frame pixels at zoom
/// 1, for the depth plane's order to weigh them against each other.
constexpr double least_shared_area = 1.0 / 1024.0;
/// How far outside a fan triangle, as a share of its weights, a point may
/// lie and still be taken as inside it.
constexpr double fan_weight_tolerance = 1e-6;
/// What an interpolated depth may fall short of a whole depth by and still
/// be floored to it: the rounding of the interpolation, so that a point
/// whose depth is whole by its corners takes that depth.
constexpr double depth_rounding_allowance = 1e-6;
/// The least area a part of a polygon under one drawn before it takes to be
/// cut out of it (find_plane_cuts), in square frame pixels at zoom 1: a
/// quarter of a pixel. The plane decides the frame at zoom 1 pixel by pixel,
/// so that a smaller part holds a pixel's centre only now and then, and the
/// order's picture of it shows instead.
constexpr double least_cut_area = 0.25;
/// How far below a sloping polygon's depth a later sloping one may lie and
/// still show over it (shown_margin): half a depth, where the floors of the
/// two depths are as often the same as not.
constexpr double sloped_shown_margin = -0.5;
/// The 64-bit FNV-1a digest a unit's polygon shapes are folded into: its
/// start and its multiplier.
constexpr uint64_t shape_digest_start = 0xcbf29ce484222325ULL;
constexpr uint64_t shape_digest_multiplier = 0x100000001b3ULL;
/// The most units whose polygon order is kept from frame to frame; one
/// more forgets them all.
constexpr size_t most_kept_plane_orders = 4096;
/// The edge of the white page every solid fill draws from, so that each
/// renderer draws the fill with the batch's own blend.
constexpr uint32_t solid_page_edge = 2;
/// Frame numbers on the pages: the projectiles' shadow sprite, then the
/// texture frames as they are first seen.
constexpr uint64_t projectile_shadow_number = 0;
constexpr uint64_t first_texture_number = 1;
/// The mark on the number of a model's shadow as the game casts it
/// (game_shadow_of_model): the top bit, over a digest of its silhouette, so
/// that no texture frame's number meets it and a silhouette many units
/// share is one frame.
constexpr uint64_t game_shadow_number_mark = uint64_t{1} << 63;

/// Returns how a model's shadow as the game casts it reads its frame at a
/// zoom: as the sprite stage reads a feature's shadow frame
/// (sprite_sampling), nearest at a whole-number zoom, linear below 1 and
/// pixel-art above.
///
/// @param zoom window pixels per map pixel
/// @return the sampling
card::Sampling game_shadow_sampling(float zoom) noexcept {
    if (std::floor(zoom) == zoom)
        return card::Sampling::nearest;
    return zoom < 1.0F ? card::Sampling::linear : card::Sampling::pixel_art;
}

/// A frame's number doubled, plus its variant: the keyed frame, or the flat
/// one whose image-key texels are a colour.
constexpr uint64_t variants = 2;
constexpr uint64_t flat_variant = 1;
/// Memory the texture pages start with: the installed game's texture
/// library is 1.35 MB of indices, 753 frames.
constexpr size_t page_memory_limit = size_t{32} * 1024 * 1024;
/// The most the texture pages grow to where one frame's textures fill them,
/// as far as the growth hooks allow: a mod's library many times the game's.
constexpr size_t largest_page_memory_limit = size_t{512} * 1024 * 1024;

/// One corner of a polygon: a frame pixel at zoom 1, where on its frame the
/// corner lies and its colour.
struct Corner {
    int32_t x{};
    int32_t y{};
    float u{};
    float v{};
    card::Colour colour{};
    float world_height{}; ///< physical height, independent of cached piece depth bytes
    int32_t depth{};      ///< the depth plane's value at the corner; units' polygons alone
};

/// One polygon to emit, in the order and the look the processor draws it.
struct Polygon {
    uint32_t first_corner{};
    uint16_t corner_count{};
    /// Under water by the game's depth rule, with Underwater shimmer at 0%:
    /// tinted as the blue table tints, halved, then lifted.
    bool underwater{};
    bool water_geometry{};
    /// Its corners' world heights hold a plane over it (height_x, height_y,
    /// height_origin), which Underwater shimmer shades by where the polygon
    /// dips under the water and Naval reflections mirror its corners by.
    bool height_plane{};
    float height_x{};
    float height_y{};
    float height_origin{};
    bool bright{};    ///< drawn from the bright page
    bool flat_page{}; ///< the flat variant of its frame: the image key a colour
    /// The mesh whose fan splits the polygon, with the primitive; a null
    /// mesh fans from the first corner. A textured quad is drawn as strips
    /// across its rows instead where it is not a parallelogram on screen.
    const gw::ModelMesh* mesh{};
    uint32_t primitive{};
    const Sprite* texture{}; ///< null for a polygon filled with its corners' colours
    int32_t depth{};         ///< the depth plane's value, for the sort
    /// Filled with the image key at every corner. In a unit's picture, which
    /// leaves the key's pixels clear, it clears what it covers of the
    /// polygons under it and draws nothing (cut_under_clearing); drawn flat,
    /// it is a colour like any other.
    bool clears{};
    /// Cleared in part by polygons that clear over it: drawn as the pieces
    /// left of it, piece_count of the cut pieces from first_piece, and not
    /// at all when none are left.
    bool cut{};
    uint32_t first_piece{};
    uint32_t piece_count{};
    /// Where it lies under nearer polygons drawn before it, which it leaves
    /// out (cut_under_nearer): hole_count of the unit's holes from
    /// first_hole.
    uint32_t first_hole{};
    uint32_t hole_count{};
};

/// Recover physical height over a projected face for waterline clipping.
void water_plane(Polygon& polygon, std::span<const Corner> corners) {
    const auto& a = corners[0];
    for (uint32_t k = 1; k + 1 < corners.size(); ++k) {
        const auto& b = corners[k];
        const auto& c = corners[k + 1];
        const float bx = float(b.x) - a.x, by = float(b.y) - a.y;
        const float cx = float(c.x) - a.x, cy = float(c.y) - a.y;
        const float determinant = bx * cy - by * cx;
        if (std::abs(determinant) < 0.01F)
            continue;
        polygon.height_x =
            ((b.world_height - a.world_height) * cy - (c.world_height - a.world_height) * by) /
            determinant;
        polygon.height_y =
            (bx * (c.world_height - a.world_height) - cx * (b.world_height - a.world_height)) /
            determinant;
        polygon.height_origin = a.world_height - polygon.height_x * a.x - polygon.height_y * a.y;
        polygon.water_geometry = true;
        polygon.height_plane = true;
        break;
    }
}

/// A run of one row's pixels of a nanoframe's outline, frame pixels at zoom 1.
struct OutlineRun {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    card::Colour colour{};
};

/// A corner of a piece of a cut polygon: a point in frame pixels at zoom 1,
/// between whole pixels, with where on its frame it lies and its colour, as
/// the polygon's corners give them there.
struct CutPoint {
    double x{};
    double y{};
    double u{};
    double v{};
    card::Colour colour{};
};

/// One convex piece of a cut polygon: count of the cut points from first.
struct CutPiece {
    uint32_t first_point{};
    uint32_t point_count{};
};

/// The executor's page of one of the sprite pages' pages.
struct PageSlot {
    card::PageHandle handle{};
    uint32_t size{}; ///< the page's side when the handle was made
};

/// Where a frame's texels are on the executor's pages.
struct PlacedFrame {
    card::PageHandle page{};
    uint32_t page_size{};
    gw::TexelRect rect{};
};

/// A model's mesh, with the handle it was built for.
struct MeshEntry {
    gw::ModelMesh mesh;
    std::weak_ptr<const Model> owner;
    bool refused{};
};

/// How a polygon's colours are shaded: not at all, or by the shade rows of
/// the lit builder, lit or unlit.
enum class Shading : uint8_t {
    none, ///< the unshaded builder: colours as they are
    rows, ///< the shaded builder: a row per vertex, unlit_shade for an unlit piece
};

/// How a piece's corners are placed: as the cached image places them, as the
/// flat draw does, or as a carried unit's image is composed into its
/// carrier's.
struct Placement {
    bool flat{};         ///< the flat draw: each term floored after the unit's offset is added
    int32_t dx{};        ///< image and composed paths: added to the floored vertex column
    int32_t dy{};        ///< added to the floored vertex row
    int32_t depth_add{}; ///< composed path: the carried unit's height over its carrier
    int32_t flat_dx{};   ///< flat path: the unit's 16.16 offset from the camera
    int32_t flat_dz{};
    int32_t flat_y{}; ///< flat path: the unit's 16.16 height
    int32_t depth_base{};
};

/// The build effect's bands for one unit, as remap_depth_range takes them.
struct NanoBands {
    bool active{};
    uint8_t low{};       ///< the band's lowest depth
    uint8_t threshold{}; ///< the depth past the band's highest
    int32_t above{};
    int32_t below{};
    int32_t band{};
    uint8_t outline{}; ///< the outline's palette entry
};

int32_t hi(int32_t value) noexcept {
    return value >> 16;
}

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t fixed_of_pixels(int32_t pixels) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(pixels) << 16);
}

/// A level of a channel as the card's float.
float level(uint8_t value) noexcept {
    return static_cast<float>(value) / 255.0F;
}

/// A palette entry through the display gamma, as the pages hold it.
card::Colour palette_colour(const Palette& palette, float gamma, uint8_t index, float alpha) {
    const PaletteEntry& entry = palette.entries[index];
    return {
        level(oa::present::gamma_channel(entry.r, gamma)),
        level(oa::present::gamma_channel(entry.g, gamma)),
        level(oa::present::gamma_channel(entry.b, gamma)),
        alpha
    };
}

uint32_t def_flags(const draw::ModelRef& model) noexcept {
    return model.def != nullptr ? model.def->flags : 0;
}

bool is_digger(const draw::ModelRef& model) noexcept {
    return (def_flags(model) & OA_UNIT_DEF_FLAG_DIGGER) != 0;
}

bool is_building(const Unit& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_BUILDING) != 0;
}

int32_t vertex_depth_base(const draw::ModelRef& model) noexcept {
    return draw::model_depth_base + (is_digger(model) ? draw::digger_depth_bias : 0);
}

uint32_t first_primitive(const draw::PreparedObject& prepared) noexcept {
    return prepared.skips_first ? 1U : 0U;
}

/// The build effect's bands of a unit for a tick of its pulse, as
/// apply_build_effect sets them; inactive for a finished unit.
NanoBands nano_bands(uint32_t pulse_tick, const Unit& unit) {
    NanoBands bands;
    if (unit.build_remaining == 0.0F)
        return bands;
    const draw::BuildPulseColours colours = draw::build_pulse_colours(pulse_tick, unit.id);
    const int32_t color_a = colours.first;
    const int32_t color_b = colours.second;
    const int32_t progress = oa::base::game_math::truncate_low32(
        static_cast<double>(unit.build_remaining) * progress_scale
    );
    bands.active = true;
    bands.outline = static_cast<uint8_t>(color_b);
    if (progress > nano_band_first) {
        bands.threshold =
            static_cast<uint8_t>(((progress - nano_band_first) * depth_byte_max) / nano_span_first);
        bands.above = draw::remap_clear;
        bands.below = draw::remap_clear;
        bands.band = color_a;
    } else if (progress > nano_band_second) {
        bands.threshold = static_cast<uint8_t>(
            ((progress - nano_band_second) * depth_byte_max) / nano_span_second
        );
        bands.above = draw::remap_clear;
        bands.below = draw::remap_clear;
        bands.band = color_a;
    } else if (progress > nano_band_third) {
        bands.threshold = static_cast<uint8_t>(
            ((nano_band_third - progress) * depth_byte_max) / nano_span_middle - 1
        );
        bands.above = draw::remap_clear;
        bands.below = color_a;
        bands.band = color_b;
    } else if (progress > nano_band_fourth) {
        bands.threshold = static_cast<uint8_t>(
            ((nano_band_fourth - progress) * depth_byte_max) / nano_span_middle - 1
        );
        bands.above = color_a;
        bands.below = draw::remap_keep;
        bands.band = color_b;
    } else {
        bands.threshold = static_cast<uint8_t>((progress * depth_byte_max) / nano_band_fourth);
        bands.above = draw::remap_keep;
        bands.below = draw::remap_keep;
        bands.band = color_a;
    }
    // The band takes the 4 depths below the threshold.
    bands.low = bands.threshold < nano_band_depth
                    ? 0
                    : static_cast<uint8_t>(bands.threshold - nano_band_depth);
    return bands;
}

/// The bands of a model drawn with build bands of its own
/// (ModelState::build_bands), else its unit's (nano_bands).
NanoBands bands_of(uint32_t pulse_tick, const draw::ModelRef& model) {
    if (model.state == nullptr || !model.state->build_bands || model.unit == nullptr ||
        model.unit->build_remaining == 0.0F)
        return model.unit != nullptr ? nano_bands(pulse_tick, *model.unit) : NanoBands{};
    const draw::BuildEffectBands& own = *model.state->build_bands;
    NanoBands bands;
    bands.active = true;
    bands.low = own.band_low;
    bands.threshold = own.band_end;
    bands.above = own.above;
    bands.below = own.below;
    bands.band = own.band;
    bands.outline = own.outline;
    return bands;
}

/// What the bands make of a depth: remap_keep, remap_clear or a colour.
int32_t classify_depth(const NanoBands& bands, uint8_t depth) noexcept {
    int32_t value = bands.below;
    if (bands.low <= depth) {
        value = bands.above;
        if (depth < bands.threshold)
            value = bands.band;
    }
    return value;
}

/// Renders a texture frame of the library as the pages take a frame: its
/// indices with every texel covered, or, keyed, every texel but the image
/// key's.
void render_texture(const Sprite& sprite, bool flat, oa::formats::gaf::RenderedFrame& out) {
    out.width = sprite.width;
    out.height = sprite.height;
    out.origin_x = sprite.origin_x;
    out.origin_y = sprite.origin_y;
    out.transparency_index = draw::image_key;
    const auto size = static_cast<size_t>(sprite.width) * sprite.height;
    const auto* pixels = static_cast<const uint8_t*>(sprite.data);
    out.pixels.assign(pixels, pixels + size);
    out.coverage.assign(size, 1);
    if (!flat)
        for (size_t i = 0; i < size; ++i)
            if (out.pixels[i] == draw::image_key)
                out.coverage[i] = 0;
}

/// Doubles a palette's channels and clamps them: the bright page's palette.
Palette doubled(const Palette& palette) noexcept {
    Palette bright{};
    for (size_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const PaletteEntry& entry = palette.entries[i];
        bright.entries[i] = {
            static_cast<uint8_t>(std::min(255, 2 * int{entry.r})),
            static_cast<uint8_t>(std::min(255, 2 * int{entry.g})),
            static_cast<uint8_t>(std::min(255, 2 * int{entry.b})),
            entry.flags
        };
    }
    return bright;
}

/// Twice the signed area of a polygon in frame pixels, positive for the
/// winding today's fills draw: forward order runs down the right side.
int64_t winding(std::span<const Corner> corners) noexcept {
    int64_t sum = 0;
    for (size_t i = 0; i < corners.size(); ++i) {
        const Corner& a = corners[i];
        const Corner& b = corners[(i + 1) % corners.size()];
        sum += int64_t{a.x} * b.y - int64_t{b.x} * a.y;
    }
    return sum;
}

/// A point in frame pixels at zoom 1, between pixel centres.
struct PlanePoint {
    double x{};
    double y{};
};

/// What the depth plane's order reads of one polygon: whether it draws, the
/// rectangle round its corners in frame pixels, and its lowest and highest
/// corner depth.
struct PolygonExtent {
    bool drawn{};
    int32_t left{};
    int32_t right{};
    int32_t top{};
    int32_t bottom{};
    int32_t lowest{};
    int32_t highest{};
};

/// A part of a polygon across which its depth runs along one plane: the
/// whole polygon where its corners lie on one, else a fan triangle from its
/// first corner, as depth_at walks them. Its corners are point_count plane
/// points from first_point, in the polygon's winding, and its depth at a
/// point (x, y) is x * along_x + y * along_y + at_origin.
struct DepthFace {
    uint32_t first_point{};
    uint32_t point_count{};
    double along_x{};
    double along_y{};
    double at_origin{};
};

/// A convex part of the frame, point_count of a list of plane points from
/// first_point, in the winding of the polygons it was cut from.
struct PlaneCut {
    uint32_t first_point{};
    uint32_t point_count{};
};

/// A hole placed on the surface (leave_out_holes): edge_count of the lines
/// along its edges from first_edge, and the box round it, in pixels of the
/// target.
struct HoleBox {
    uint32_t first_edge{};
    uint32_t edge_count{};
    double left{};
    double right{};
    double top{};
    double bottom{};
};

/// The line along an edge of a hole placed on the surface (leave_out_holes):
/// a point (x, y) lies inside it where across * y - down * x + offset is
/// above 0, on it where that is 0.
struct HoleEdge {
    double across{};
    double down{};
    double offset{};
};

/// Scratch of the order a unit's depth plane gives its polygons, kept from
/// unit to unit.
struct PlaneOrderScratch {
    std::vector<PolygonExtent> extents;
    std::vector<uint32_t> by_mean; ///< the polygons, lowest mean depth first
    std::vector<uint32_t> ranks;   ///< each polygon's place in by_mean
    std::vector<uint32_t> by_left; ///< the drawn polygons, leftmost first
    /// Each pair of overlapping polygons: the one drawn under, then the one
    /// drawn over it.
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    std::vector<uint32_t> first_over; ///< where each polygon's overs start in overs
    std::vector<uint32_t> next_over;  ///< where each polygon's next over goes in overs
    std::vector<uint32_t> overs;
    std::vector<uint32_t> unders_left; ///< each polygon's unders not yet placed
    std::vector<uint32_t> ready;       ///< ranks whose unders are all placed, a heap
    std::vector<uint8_t> placed;
    std::vector<PlanePoint> shared;
    std::vector<PlanePoint> clipped;
    std::vector<Polygon> ordered;
    std::vector<uint32_t> places; ///< each polygon's place in the drawing order
    std::vector<DepthFace> earlier_faces;
    std::vector<DepthFace> later_faces;
    std::vector<PlanePoint> earlier_points;
    std::vector<PlanePoint> later_points;
    std::vector<double> levels;
    /// The parts found to cut, each with the place of the polygon it cuts,
    /// and their points.
    std::vector<std::pair<uint32_t, PlaneCut>> found_cuts;
    std::vector<PlanePoint> found_points;
};

/// A unit's polygon order, kept while its polygons keep their shape.
struct KeptPlaneOrder {
    uint64_t shape{};            ///< the digest of the shape the order was found for
    std::vector<uint32_t> order; ///< the polygons by their place in the processor's order
    /// Where each polygon in drawing order lies under one drawn before it
    /// (find_plane_cuts): its cuts from first_cut[k] to first_cut[k + 1],
    /// their points placed from the first corner of the processor's first
    /// polygon.
    std::vector<uint32_t> first_cut;
    std::vector<PlaneCut> cuts;
    std::vector<PlanePoint> cut_points;
};

/// The cuts of a unit's polygons in drawing order (KeptPlaneOrder), with the
/// frame pixel their points are placed from.
struct PlaneCuts {
    std::span<const uint32_t> first_cut;
    std::span<const PlaneCut> cuts;
    std::span<const PlanePoint> points;
    PlanePoint origin{};
};

/// Twice the signed area of the triangle a point and an edge make, positive
/// when the point lies on the inner side of an edge of a polygon whose
/// winding is positive.
double edge_side(const PlanePoint& from, const PlanePoint& to, const PlanePoint& point) noexcept {
    return (to.x - from.x) * (point.y - from.y) - (to.y - from.y) * (point.x - from.x);
}

/// Returns a corner's place as a plane point, from an origin of whole frame
/// pixels, so that the same shape gives the same points wherever it lies.
PlanePoint plane_point(const Corner& corner, const Corner& origin) noexcept {
    return {
        static_cast<double>(int64_t{corner.x} - origin.x),
        static_cast<double>(int64_t{corner.y} - origin.y)
    };
}

/// Finds the depth a polygon's corners give a point, interpolated across
/// the fan triangle from the first corner that holds it.
///
/// @param corners the polygon's corners, of positive winding
/// @param origin the corner the point is placed from
/// @param point the point
/// @param[out] depth the interpolated depth, before it is floored
/// @return false when no fan triangle holds the point
bool depth_at(
    std::span<const Corner> corners, const Corner& origin, const PlanePoint& point, double& depth
) noexcept {
    const PlanePoint first = plane_point(corners[0], origin);
    for (size_t k = 1; k + 1 < corners.size(); ++k) {
        const PlanePoint second = plane_point(corners[k], origin);
        const PlanePoint third = plane_point(corners[k + 1], origin);
        const double area = edge_side(first, second, third);
        if (area == 0.0)
            continue;
        const double first_weight = edge_side(second, third, point) / area;
        const double second_weight = edge_side(third, first, point) / area;
        const double third_weight = edge_side(first, second, point) / area;
        if (first_weight < -fan_weight_tolerance || second_weight < -fan_weight_tolerance ||
            third_weight < -fan_weight_tolerance)
            continue;
        depth = first_weight * corners[0].depth + second_weight * corners[k].depth +
                third_weight * corners[k + 1].depth;
        return true;
    }
    return false;
}

/// Keeps the part of a convex polygon of plane points on the inner side of
/// the line along an edge of a polygon of positive winding.
///
/// @param shape the convex polygon's points, in their turn
/// @param from the edge's first corner
/// @param to its second
/// @param[out] kept the part on that side, its points in the same turn
void clip_to_edge(
    std::span<const PlanePoint> shape,
    const PlanePoint& from,
    const PlanePoint& to,
    std::vector<PlanePoint>& kept
) {
    kept.clear();
    for (size_t m = 0; m < shape.size(); ++m) {
        const PlanePoint& here = shape[m];
        const PlanePoint& next = shape[(m + 1) % shape.size()];
        const double here_side = edge_side(from, to, here);
        const double next_side = edge_side(from, to, next);
        if (here_side >= 0.0)
            kept.push_back(here);
        if ((here_side >= 0.0) != (next_side >= 0.0)) {
            const double along = here_side / (here_side - next_side);
            kept.push_back(
                {here.x + along * (next.x - here.x), here.y + along * (next.y - here.y)}
            );
        }
    }
}

/// Twice the signed area of a polygon of plane points, positive for
/// positive winding.
double twice_plane_area(std::span<const PlanePoint> shape) noexcept {
    double sum = 0.0;
    for (size_t m = 0; m < shape.size(); ++m) {
        const PlanePoint& here = shape[m];
        const PlanePoint& next = shape[(m + 1) % shape.size()];
        sum += here.x * next.y - next.x * here.y;
    }
    return sum;
}

/// Finds the centre of the area two polygons of positive winding share,
/// taking them as convex.
///
/// @param first one polygon's corners
/// @param second the other's
/// @param origin the corner the centre is placed from
/// @param[in,out] scratch the clipping scratch
/// @param[out] centre the centre of the shared area
/// @return false when they share less than least_shared_area
bool shared_centre(
    std::span<const Corner> first,
    std::span<const Corner> second,
    const Corner& origin,
    PlaneOrderScratch& scratch,
    PlanePoint& centre
) {
    auto& shared = scratch.shared;
    auto& clipped = scratch.clipped;
    shared.clear();
    for (const Corner& corner : second)
        shared.push_back(plane_point(corner, origin));
    for (size_t k = 0; k < first.size() && !shared.empty(); ++k) {
        clip_to_edge(
            shared,
            plane_point(first[k], origin),
            plane_point(first[(k + 1) % first.size()], origin),
            clipped
        );
        shared.swap(clipped);
    }
    if (shared.size() < 3)
        return false;
    double twice_area = 0.0;
    double x_sum = 0.0;
    double y_sum = 0.0;
    for (size_t m = 0; m < shared.size(); ++m) {
        const PlanePoint& here = shared[m];
        const PlanePoint& next = shared[(m + 1) % shared.size()];
        const double cross = here.x * next.y - next.x * here.y;
        twice_area += cross;
        x_sum += (here.x + next.x) * cross;
        y_sum += (here.y + next.y) * cross;
    }
    if (std::abs(twice_area) < 2.0 * least_shared_area)
        return false;
    centre = {x_sum / (3.0 * twice_area), y_sum / (3.0 * twice_area)};
    return true;
}

/// Returns a corner of a polygon as a cut point.
CutPoint cut_point_of(const Corner& corner) noexcept {
    return {
        static_cast<double>(corner.x),
        static_cast<double>(corner.y),
        static_cast<double>(corner.u),
        static_cast<double>(corner.v),
        corner.colour
    };
}

/// Returns the point a share of the way from one cut point to another, with
/// where on its frame it lies and its colour taken the same share of the way.
///
/// @param from the first point
/// @param to the second
/// @param along the share of the way, 0 at `from` and 1 at `to`
/// @return the point between them
CutPoint cut_point_between(const CutPoint& from, const CutPoint& to, double along) noexcept {
    const auto mix = [along](double first, double second) {
        return first + along * (second - first);
    };
    const auto mix_level = [&mix](float first, float second) {
        return static_cast<float>(mix(first, second));
    };
    return {
        mix(from.x, to.x),
        mix(from.y, to.y),
        mix(from.u, to.u),
        mix(from.v, to.v),
        {mix_level(from.colour.red, to.colour.red),
         mix_level(from.colour.green, to.colour.green),
         mix_level(from.colour.blue, to.colour.blue),
         mix_level(from.colour.alpha, to.colour.alpha)}
    };
}

/// Keeps the part of a convex polygon on one side of the line along an edge
/// of a polygon of positive winding.
///
/// @param polygon the convex polygon's points, in their turn
/// @param from the edge's first corner
/// @param to its second
/// @param inner true for the side the edge's polygon lies on, false for the other
/// @param[out] kept the part on that side, its points in the same turn;
///     fewer than three where nothing is
void keep_side(
    std::span<const CutPoint> polygon,
    const PlanePoint& from,
    const PlanePoint& to,
    bool inner,
    std::vector<CutPoint>& kept
) {
    kept.clear();
    const double side = inner ? 1.0 : -1.0;
    for (size_t m = 0; m < polygon.size(); ++m) {
        const CutPoint& here = polygon[m];
        const CutPoint& next = polygon[(m + 1) % polygon.size()];
        const double here_side = side * edge_side(from, to, {here.x, here.y});
        const double next_side = side * edge_side(from, to, {next.x, next.y});
        if (here_side >= 0.0)
            kept.push_back(here);
        if ((here_side >= 0.0) != (next_side >= 0.0))
            kept.push_back(cut_point_between(here, next, here_side / (here_side - next_side)));
    }
}

/// Twice the area of a polygon of cut points, in square frame pixels at
/// zoom 1, positive for positive winding.
double twice_cut_area(std::span<const CutPoint> polygon) noexcept {
    double sum = 0.0;
    for (size_t m = 0; m < polygon.size(); ++m) {
        const CutPoint& here = polygon[m];
        const CutPoint& next = polygon[(m + 1) % polygon.size()];
        sum += here.x * next.y - next.x * here.y;
    }
    return sum;
}

/// Digests the shape of a unit's polygons: how many corners each has, and
/// each corner's place from the first polygon's first corner and its depth,
/// all the order the depth plane gives them depends on.
///
/// @param corners the frame's corners
/// @param polygons the unit's polygons
/// @return the digest
uint64_t
polygon_shape(std::span<const Corner> corners, std::span<const Polygon> polygons) noexcept {
    uint64_t digest = shape_digest_start;
    const auto fold = [&digest](int64_t value) {
        digest = (digest ^ static_cast<uint64_t>(value)) * shape_digest_multiplier;
    };
    fold(static_cast<int64_t>(polygons.size()));
    if (polygons.empty())
        return digest;
    const Corner& origin = corners[polygons.front().first_corner];
    for (const Polygon& polygon : polygons) {
        fold(polygon.corner_count);
        for (const Corner& corner : corners.subspan(polygon.first_corner, polygon.corner_count)) {
            fold(int64_t{corner.x} - origin.x);
            fold(int64_t{corner.y} - origin.y);
            fold(corner.depth);
        }
    }
    return digest;
}

/// Returns how far below the depth of the earlier of two polygons, in the
/// processor's order, the later's may lie where the plane still shows the
/// later: the plane keeps a pixel whose floored depth is not below the
/// floored depth there. Over one lying flat at a whole depth the later
/// shows from that depth up, so by none; lying flat itself, at a whole
/// depth, it shows over all that lies less than a whole depth higher, so
/// by all but the rounding of the interpolation; over a slope, sloping, by
/// half a depth, between where the floors could take either.
///
/// @param earlier the earlier polygon's extent
/// @param later the later's
/// @return the margin, from 0 down to just above -1
double shown_margin(const PolygonExtent& earlier, const PolygonExtent& later) noexcept {
    const bool earlier_flat = earlier.lowest == earlier.highest;
    const bool later_flat = later.lowest == later.highest;
    if (earlier_flat && !later_flat)
        return -depth_rounding_allowance;
    if (later_flat && !earlier_flat)
        return depth_rounding_allowance - 1.0;
    return sloped_shown_margin;
}

/// Finds the order a unit's depth plane shows its polygons in.
///
/// The polygons come in the processor's order. Of two that overlap, the
/// later in the result is the one the plane shows at the centre of their
/// overlap: the higher there, or at the same height the one the processor
/// draws later, which the plane lets over a pixel of the same floored
/// height (shown_margin). Where the overlaps leave the order open, as
/// for polygons apart, the lower mean depth goes first and the processor's
/// order among equals; where they contradict one another, the polygon left
/// with the fewest left to go under it goes next, the lowest mean depth
/// among those. The same shape anywhere gives the same order.
/// Where two polygons cross, or the overlaps contradict one another, the
/// order cannot show the plane's picture alone, and find_plane_cuts finds
/// what each polygon must leave out of its own.
///
/// @param corners the frame's corners
/// @param polygons the unit's polygons, at least one
/// @param[in,out] scratch the scratch
/// @param[out] order the polygons by their place in `polygons`, in drawing order
void find_plane_order(
    std::span<const Corner> corners,
    std::span<const Polygon> polygons,
    PlaneOrderScratch& scratch,
    std::vector<uint32_t>& order
) {
    const auto count = static_cast<uint32_t>(polygons.size());
    const Corner& origin = corners[polygons.front().first_corner];
    const auto corners_of = [&](uint32_t index) {
        const Polygon& polygon = polygons[index];
        return corners.subspan(polygon.first_corner, polygon.corner_count);
    };
    auto& extents = scratch.extents;
    extents.assign(count, PolygonExtent{});
    for (uint32_t i = 0; i < count; ++i) {
        if (polygons[i].corner_count < 3)
            continue;
        const std::span<const Corner> own = corners_of(i);
        PolygonExtent& extent = extents[i];
        extent.drawn = winding(own) > 0;
        extent.left = extent.right = own[0].x;
        extent.top = extent.bottom = own[0].y;
        extent.lowest = extent.highest = own[0].depth;
        for (const Corner& corner : own) {
            extent.left = std::min(extent.left, corner.x);
            extent.right = std::max(extent.right, corner.x);
            extent.top = std::min(extent.top, corner.y);
            extent.bottom = std::max(extent.bottom, corner.y);
            extent.lowest = std::min(extent.lowest, corner.depth);
            extent.highest = std::max(extent.highest, corner.depth);
        }
    }
    auto& by_mean = scratch.by_mean;
    by_mean.resize(count);
    std::iota(by_mean.begin(), by_mean.end(), 0U);
    std::stable_sort(by_mean.begin(), by_mean.end(), [&](uint32_t a, uint32_t b) {
        return polygons[a].depth < polygons[b].depth;
    });
    auto& ranks = scratch.ranks;
    ranks.resize(count);
    for (uint32_t rank = 0; rank < count; ++rank)
        ranks[by_mean[rank]] = rank;
    // The drawn polygons by their left edge, so that each meets only those
    // starting left of its right edge.
    auto& by_left = scratch.by_left;
    by_left.clear();
    for (uint32_t i = 0; i < count; ++i)
        if (extents[i].drawn)
            by_left.push_back(i);
    std::sort(by_left.begin(), by_left.end(), [&](uint32_t a, uint32_t b) {
        return extents[a].left < extents[b].left;
    });
    auto& edges = scratch.edges;
    edges.clear();
    for (size_t first = 0; first < by_left.size(); ++first) {
        for (size_t second = first + 1; second < by_left.size(); ++second) {
            if (extents[by_left[second]].left >= extents[by_left[first]].right)
                break;
            const uint32_t earlier = std::min(by_left[first], by_left[second]);
            const uint32_t later = std::max(by_left[first], by_left[second]);
            const PolygonExtent& under = extents[earlier];
            const PolygonExtent& over = extents[later];
            if (under.bottom <= over.top || over.bottom <= under.top)
                continue;
            bool later_shows = false;
            if (under.highest <= over.lowest) {
                later_shows = true;
            } else if (over.highest < under.lowest) {
                later_shows = false;
            } else {
                PlanePoint centre;
                double earlier_depth = 0.0;
                double later_depth = 0.0;
                if (!shared_centre(
                        corners_of(earlier), corners_of(later), origin, scratch, centre
                    ) ||
                    !depth_at(corners_of(earlier), origin, centre, earlier_depth) ||
                    !depth_at(corners_of(later), origin, centre, later_depth))
                    continue;
                later_shows = later_depth - earlier_depth >= shown_margin(under, over);
            }
            edges.emplace_back(later_shows ? earlier : later, later_shows ? later : earlier);
        }
    }
    // Each polygon's overs, gathered by the polygon under them.
    auto& first_over = scratch.first_over;
    auto& overs = scratch.overs;
    auto& unders_left = scratch.unders_left;
    first_over.assign(count + 1, 0);
    unders_left.assign(count, 0);
    for (const auto& [under, over] : edges) {
        ++first_over[under + 1];
        ++unders_left[over];
    }
    for (uint32_t i = 0; i < count; ++i)
        first_over[i + 1] += first_over[i];
    overs.resize(edges.size());
    auto& next_over = scratch.next_over;
    next_over.assign(first_over.begin(), first_over.end() - 1);
    for (const auto& [under, over] : edges)
        overs[next_over[under]++] = over;
    auto& ready = scratch.ready;
    auto& placed = scratch.placed;
    ready.clear();
    placed.assign(count, 0);
    order.clear();
    // A heap of ranks whose top is the lowest.
    const std::greater<uint32_t> lowest_first;
    for (uint32_t i = 0; i < count; ++i)
        if (unders_left[i] == 0)
            ready.push_back(ranks[i]);
    std::make_heap(ready.begin(), ready.end(), lowest_first);
    uint32_t lowest_left = 0;
    while (order.size() < count) {
        uint32_t next = count;
        while (!ready.empty() && next == count) {
            std::pop_heap(ready.begin(), ready.end(), lowest_first);
            const uint32_t candidate = by_mean[ready.back()];
            ready.pop_back();
            if (placed[candidate] == 0)
                next = candidate;
        }
        if (next == count) {
            // The overlaps go round in a circle: of the polygons left, the
            // one with the fewest left to go under it, the lowest mean depth
            // among those.
            while (placed[by_mean[lowest_left]] != 0)
                ++lowest_left;
            for (uint32_t rank = lowest_left; rank < count; ++rank) {
                const uint32_t candidate = by_mean[rank];
                if (placed[candidate] == 0 &&
                    (next == count || unders_left[candidate] < unders_left[next]))
                    next = candidate;
            }
        }
        placed[next] = 1;
        order.push_back(next);
        for (uint32_t k = first_over[next]; k < first_over[next + 1]; ++k) {
            const uint32_t over = overs[k];
            if (placed[over] == 0 && unders_left[over] != 0 && --unders_left[over] == 0) {
                ready.push_back(ranks[over]);
                std::push_heap(ready.begin(), ready.end(), lowest_first);
            }
        }
    }
}

/// Gathers the faces of a polygon of positive winding across which its
/// depth runs along one plane (DepthFace): the whole polygon where every
/// corner lies on the plane of its first fan triangle, else each fan
/// triangle of area.
///
/// @param corners the polygon's corners
/// @param origin the corner the faces are placed from
/// @param[out] faces its faces
/// @param[out] points their corners
void depth_faces(
    std::span<const Corner> corners,
    const Corner& origin,
    std::vector<DepthFace>& faces,
    std::vector<PlanePoint>& points
) {
    faces.clear();
    points.clear();
    // The plane through three corners, as a face of them.
    const auto plane_of = [&](size_t a, size_t b, size_t c, DepthFace& face) {
        const PlanePoint pa = plane_point(corners[a], origin);
        const PlanePoint pb = plane_point(corners[b], origin);
        const PlanePoint pc = plane_point(corners[c], origin);
        const double twice_area = edge_side(pa, pb, pc);
        if (twice_area <= 0.0)
            return false;
        const double rise_b = static_cast<double>(corners[b].depth - corners[a].depth);
        const double rise_c = static_cast<double>(corners[c].depth - corners[a].depth);
        face.along_x = (rise_b * (pc.y - pa.y) - rise_c * (pb.y - pa.y)) / twice_area;
        face.along_y = (rise_c * (pb.x - pa.x) - rise_b * (pc.x - pa.x)) / twice_area;
        face.at_origin = corners[a].depth - face.along_x * pa.x - face.along_y * pa.y;
        return true;
    };
    for (size_t k = 1; k + 1 < corners.size(); ++k) {
        DepthFace face;
        if (!plane_of(0, k, k + 1, face))
            continue;
        if (faces.empty()) {
            bool flat = true;
            for (const Corner& corner : corners) {
                const PlanePoint point = plane_point(corner, origin);
                const double off =
                    face.along_x * point.x + face.along_y * point.y + face.at_origin - corner.depth;
                flat = flat && std::abs(off) <= depth_rounding_allowance;
            }
            if (flat) {
                face.first_point = 0;
                face.point_count = static_cast<uint32_t>(corners.size());
                for (const Corner& corner : corners)
                    points.push_back(plane_point(corner, origin));
                faces.push_back(face);
                return;
            }
        }
        face.first_point = static_cast<uint32_t>(points.size());
        face.point_count = 3;
        points.insert(
            points.end(),
            {plane_point(corners[0], origin),
             plane_point(corners[k], origin),
             plane_point(corners[k + 1], origin)}
        );
        faces.push_back(face);
    }
}

/// Returns the depth a face's plane gives a point, on the face or past it.
double depth_on(const DepthFace& face, const PlanePoint& point) noexcept {
    return face.along_x * point.x + face.along_y * point.y + face.at_origin;
}

/// Finds where each polygon, drawn in a depth plane's order
/// (find_plane_order), lies under one drawn before it, which a whole order
/// cannot show: where two polygons cross, the one drawn later shows over
/// the other on both sides of the line they cross along, and where the
/// overlaps contradict one another, a polygon is drawn over one the plane
/// shows over it. Of each two that overlap, the area they share is the
/// later's in the processor's order where its depth there is not below the
/// earlier's by more than their shown_margin, and the earlier's elsewhere;
/// the parts of it that are the polygon drawn first's, taken a face of each
/// at a time (depth_faces), are cuts of the polygon drawn second, which leaves
/// them out (cut_under_nearer), so that every point shows the polygon the
/// plane shows there whatever the order. Two whose every point the order
/// already shows as the plane does, the one wholly higher drawn later, are
/// not cut.
///
/// @param corners the frame's corners
/// @param polygons the unit's polygons in the processor's order, as find_plane_order took them
/// @param[in,out] scratch the scratch find_plane_order left its overlaps in
/// @param[in,out] entry the order found, whose cuts this sets
void find_plane_cuts(
    std::span<const Corner> corners,
    std::span<const Polygon> polygons,
    PlaneOrderScratch& scratch,
    KeptPlaneOrder& entry
) {
    const auto count = static_cast<uint32_t>(polygons.size());
    const Corner& origin = corners[polygons.front().first_corner];
    const auto corners_of = [&](uint32_t index) {
        const Polygon& polygon = polygons[index];
        return corners.subspan(polygon.first_corner, polygon.corner_count);
    };
    auto& places = scratch.places;
    places.assign(count, 0);
    for (uint32_t place = 0; place < entry.order.size(); ++place)
        places[entry.order[place]] = place;
    auto& found = scratch.found_cuts;
    auto& points = scratch.found_points;
    auto& shared = scratch.shared;
    auto& clipped = scratch.clipped;
    auto& levels = scratch.levels;
    found.clear();
    points.clear();
    for (const auto& [under, over] : scratch.edges) {
        const uint32_t earlier = std::min(under, over);
        const uint32_t later = std::max(under, over);
        const PolygonExtent& earlier_extent = scratch.extents[earlier];
        const PolygonExtent& later_extent = scratch.extents[later];
        const bool agrees_throughout = earlier_extent.highest <= later_extent.lowest ||
                                       later_extent.highest < earlier_extent.lowest;
        const bool drawn_in_order = places[under] < places[over];
        if (agrees_throughout && drawn_in_order)
            continue;
        const uint32_t first = drawn_in_order ? under : over;
        const uint32_t second = drawn_in_order ? over : under;
        const double margin = shown_margin(earlier_extent, later_extent);
        // Where the polygon drawn first shows: below the margin when it is
        // the earlier, at or above it when it is the later.
        const double first_side = first == later ? 1.0 : -1.0;
        depth_faces(corners_of(earlier), origin, scratch.earlier_faces, scratch.earlier_points);
        depth_faces(corners_of(later), origin, scratch.later_faces, scratch.later_points);
        for (const DepthFace& earlier_face : scratch.earlier_faces) {
            const std::span<const PlanePoint> earlier_points(
                scratch.earlier_points.data() + earlier_face.first_point, earlier_face.point_count
            );
            for (const DepthFace& later_face : scratch.later_faces) {
                const auto later_points =
                    std::span<const PlanePoint>(scratch.later_points)
                        .subspan(later_face.first_point, later_face.point_count);
                shared.assign(later_points.begin(), later_points.end());
                for (size_t k = 0; k < earlier_points.size() && shared.size() >= 3; ++k) {
                    clip_to_edge(
                        shared,
                        earlier_points[k],
                        earlier_points[(k + 1) % earlier_points.size()],
                        clipped
                    );
                    shared.swap(clipped);
                }
                if (shared.size() < 3 || twice_plane_area(shared) < 2.0 * least_shared_area)
                    continue;
                // The share is the first's where this level is not below 0;
                // it runs straight across the share, so it is cut along a line.
                levels.clear();
                bool any = false;
                for (const PlanePoint& point : shared) {
                    const double rise =
                        depth_on(later_face, point) - depth_on(earlier_face, point) - margin;
                    levels.push_back(first_side * rise);
                    any = any || levels.back() >= 0.0;
                }
                if (!any)
                    continue;
                clipped.clear();
                for (size_t m = 0; m < shared.size(); ++m) {
                    const size_t n = (m + 1) % shared.size();
                    if (levels[m] >= 0.0)
                        clipped.push_back(shared[m]);
                    if ((levels[m] >= 0.0) != (levels[n] >= 0.0)) {
                        const double along = levels[m] / (levels[m] - levels[n]);
                        clipped.push_back(
                            {shared[m].x + along * (shared[n].x - shared[m].x),
                             shared[m].y + along * (shared[n].y - shared[m].y)}
                        );
                    }
                }
                // Without the points the cuts doubled where they passed
                // through a corner.
                shared.clear();
                for (const PlanePoint& point : clipped)
                    if (shared.empty() || point.x != shared.back().x || point.y != shared.back().y)
                        shared.push_back(point);
                while (shared.size() > 1 && shared.back().x == shared.front().x &&
                       shared.back().y == shared.front().y)
                    shared.pop_back();
                clipped.swap(shared);
                if (clipped.size() < 3 || twice_plane_area(clipped) < 2.0 * least_cut_area)
                    continue;
                found.emplace_back(
                    places[second],
                    PlaneCut{
                        static_cast<uint32_t>(points.size()), static_cast<uint32_t>(clipped.size())
                    }
                );
                points.insert(points.end(), clipped.begin(), clipped.end());
            }
        }
    }
    // The cuts by the place of the polygon they cut, in the order found.
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    entry.first_cut.assign(count + 1, 0);
    entry.cuts.clear();
    entry.cut_points.clear();
    for (const auto& [place, cut] : found) {
        ++entry.first_cut[place + 1];
        entry.cuts.push_back({static_cast<uint32_t>(entry.cut_points.size()), cut.point_count});
        const auto from = points.begin() + static_cast<std::ptrdiff_t>(cut.first_point);
        entry.cut_points.insert(
            entry.cut_points.end(), from, from + static_cast<std::ptrdiff_t>(cut.point_count)
        );
    }
    for (uint32_t place = 0; place < count; ++place)
        entry.first_cut[place + 1] += entry.first_cut[place];
}

/// Orders a unit's polygons as its depth plane shows them (find_plane_order),
/// with the order kept for the unit while its polygons keep their shape.
///
/// The cuts found with the order (find_plane_cuts) are kept with it.
///
/// @param unit_key the unit's draw state, which the order is kept by
/// @param corners the frame's corners
/// @param[in,out] polygons the unit's polygons, reordered
/// @param[in,out] scratch the scratch
/// @param[in,out] kept the orders kept by unit
/// @return the cuts of the polygons in their new order, which the next call changes
PlaneCuts order_as_depth_plane(
    const void* unit_key,
    std::span<const Corner> corners,
    std::vector<Polygon>& polygons,
    PlaneOrderScratch& scratch,
    std::unordered_map<const void*, KeptPlaneOrder>& kept
) {
    if (polygons.empty())
        return {};
    const uint64_t shape = polygon_shape(corners, polygons);
    if (kept.size() >= most_kept_plane_orders && !kept.contains(unit_key))
        kept.clear();
    KeptPlaneOrder& entry = kept[unit_key];
    if (entry.order.size() != polygons.size() || entry.shape != shape) {
        find_plane_order(corners, polygons, scratch, entry.order);
        find_plane_cuts(corners, polygons, scratch, entry);
        entry.shape = shape;
    }
    const Corner& origin = corners[polygons.front().first_corner];
    auto& ordered = scratch.ordered;
    ordered.clear();
    for (const uint32_t index : entry.order)
        ordered.push_back(polygons[index]);
    polygons.swap(ordered);
    return {
        entry.first_cut,
        entry.cuts,
        entry.cut_points,
        {static_cast<double>(origin.x), static_cast<double>(origin.y)}
    };
}

/// Leaves out of a polygon's pieces what lies inside a convex part of the
/// frame of positive winding: each piece keeps its parts outside the part's
/// edges, taken one edge at a time, an edge of no length, where two of the
/// part's corners meet, dividing nothing.
///
/// @param[in,out] left the pieces, each convex
/// @param region the part's corners, in frame pixels at zoom 1
/// @param[in,out] kept scratch, the pieces left
/// @param[in,out] rest scratch, a piece's part inside the edges so far
/// @param[in,out] side scratch, the part on one side of an edge
void cut_away(
    std::vector<std::vector<CutPoint>>& left,
    std::span<const PlanePoint> region,
    std::vector<std::vector<CutPoint>>& kept,
    std::vector<CutPoint>& rest,
    std::vector<CutPoint>& side
) {
    kept.clear();
    for (const std::vector<CutPoint>& piece : left) {
        rest = piece;
        for (size_t k = 0; k < region.size() && rest.size() >= 3; ++k) {
            const PlanePoint& from = region[k];
            const PlanePoint& to = region[(k + 1) % region.size()];
            if (from.x == to.x && from.y == to.y)
                continue;
            keep_side(rest, from, to, false, side);
            if (side.size() >= 3 && twice_cut_area(side) >= 2.0 * least_shared_area)
                kept.push_back(side);
            keep_side(rest, from, to, true, side);
            rest.swap(side);
        }
    }
    left.swap(kept);
}

} // namespace

// ---------------------------------------------------------------------------
// The stage's state

struct ModelStage::Impl {
    Palette palette{};
    float gamma{1.0F};
    bool has_palette{};
    std::vector<PageSlot> slots;
    std::vector<PageSlot> bright_slots;
    /// Each texture frame's number on the pages, by the frame's address in
    /// the library, and the next number to give.
    std::unordered_map<const void*, uint64_t> numbers;
    uint64_t next_number{first_texture_number};
    /// Whether each texture frame has a texel of the image key, by the
    /// frame's address in the library.
    std::unordered_map<const void*, bool> keyed_texels;
    std::unordered_map<const Model*, MeshEntry> meshes;

    /// Cached soft-edged aircraft silhouettes; at most 256 pages of 256 square texels.
    struct AircraftShadow {
        card::PageHandle page{};
        uint32_t width{}, height{}, radius{}, image_builds{};
        const void* image_data{};
        uint64_t used_frame{}; ///< aircraft_shadow_frame when last drawn
    };

    std::unordered_map<uint16_t, AircraftShadow> aircraft_shadows;
    /// Frames begun, so that a full shadow cache hands the page drawn
    /// longest ago to a new aircraft.
    uint64_t aircraft_shadow_frame{};
    card::PageHandle solid_page{};
    card::TargetHandle shadow_target{};
    uint32_t shadow_width{};
    uint32_t shadow_height{};
    /// A model's shadow as the game casts it, and that shadow as a frame for
    /// the pages (game_shadow_of_model), kept from draw to draw.
    draw::GroundShadow game_shadow;
    oa::formats::gaf::RenderedFrame game_shadow_frame;
    // Scratch of a frame's emission, kept from frame to frame.
    std::vector<Corner> corners;
    std::vector<Polygon> polygons;
    PlaneOrderScratch plane_order;
    /// Each depth-plane unit's polygon order, by its draw state.
    std::unordered_map<const void*, KeptPlaneOrder> kept_plane_orders;
    /// The pieces of the unit's polygons that clearing cut, and their
    /// corners; with the scratch the cutting works in.
    std::vector<CutPiece> cut_pieces;
    std::vector<CutPoint> cut_points;
    std::vector<std::vector<CutPoint>> cut_left;
    std::vector<std::vector<CutPoint>> cut_kept;
    std::vector<CutPoint> cut_rest;
    std::vector<CutPoint> cut_side;
    std::vector<PlanePoint> cut_region;
    std::vector<std::vector<CutPoint>> cut_clears;
    /// The parts of the frame the unit's polygons leave out under nearer
    /// polygons (cut_under_nearer), their points in frame pixels at zoom 1.
    std::vector<PlaneCut> holes;
    std::vector<PlanePoint> hole_points;
    std::vector<OutlineRun> outline_runs;
    /// The primitives a nanoframe's outline is found from: their corners,
    /// and how many corners each takes and whether it fills the image.
    std::vector<oa::present::DepthVertex> outline_corners;
    std::vector<std::pair<uint32_t, bool>> outline_primitives;
    std::vector<oa::present::DepthVertex> outline_scratch;
    std::vector<uint8_t> outline_depth;
    std::vector<uint8_t> outline_marks;
    std::vector<gw::VertexNormal> normals;
    std::vector<FixedVector3> points;
    std::vector<gw::PixelPoint> projected;
    oa::formats::gaf::RenderedFrame rendered;

    /// Forgets the pages' and meshes' state, keeping the executor's handles
    /// for close or upload to free.
    void forget_contents() {
        numbers.clear();
        next_number = first_texture_number;
        keyed_texels.clear();
        meshes.clear();
    }
};

namespace {

/// Makes the executor's page of one of the sprite pages' pages, or makes it
/// again when the page was released and made again at another size, and
/// uploads it whole. The page at its old size, which batches of the frame
/// being built may name, is destroyed once the frame has run.
bool ensure_page_slot(
    card::Executor& executor,
    gw::SpritePages& pages,
    std::vector<PageSlot>& slots,
    uint32_t index,
    ModelStageCounts& counts,
    std::string& error
) {
    const auto all = pages.pages();
    if (index >= all.size())
        return false;
    const gw::Page& page = all[index];
    if (slots.size() <= index)
        slots.resize(index + 1);
    PageSlot& slot = slots[index];
    if (slot.handle != card::PageHandle{} && slot.size == page.size &&
        executor.page_alive(slot.handle))
        return true;
    if (slot.handle != card::PageHandle{})
        executor.retire_page(slot.handle);
    slot = {};
    if (page.size == 0)
        return false;
    const card::PageHandle made = executor.create_page({page.size, page.size, 1});
    if (made == card::PageHandle{}) {
        error = "model stage: cannot make a texture page: " + executor.error();
        return false;
    }
    if (!executor.update_page(made, 0, nullptr, page.texels.data(), page.size * gw::texel_bytes)) {
        error = "model stage: cannot upload a texture page: " + executor.error();
        executor.destroy_page(made);
        return false;
    }
    ++counts.page_uploads;
    pages.clear_dirty(index);
    slot = {made, page.size};
    return true;
}

/// Uploads the texels written to the pages since the last upload, and frees
/// the executor's pages of pages released since once the frame being built,
/// whose batches may name them, has run.
bool upload_pages(
    card::Executor& executor,
    gw::SpritePages& pages,
    std::vector<PageSlot>& slots,
    ModelStageCounts& counts,
    std::string& error
) {
    const auto all = pages.pages();
    bool uploaded = true;
    for (uint32_t index = 0; index < slots.size(); ++index) {
        PageSlot& slot = slots[index];
        if (slot.handle == card::PageHandle{})
            continue;
        if (index >= all.size() || all[index].size == 0 || all[index].size != slot.size) {
            executor.retire_page(slot.handle);
            slot = {};
            continue;
        }
        const gw::Page& page = all[index];
        if (page.dirty.empty())
            continue;
        const card::Rect part{page.dirty.x, page.dirty.y, page.dirty.width, page.dirty.height};
        const uint8_t* texels =
            page.texels.data() +
            (size_t{page.dirty.y} * page.size + size_t{page.dirty.x}) * gw::texel_bytes;
        if (!executor.update_page(slot.handle, 0, &part, texels, page.size * gw::texel_bytes)) {
            error = "model stage: cannot upload a texture page: " + executor.error();
            uploaded = false;
            continue;
        }
        ++counts.page_uploads;
        pages.clear_dirty(index);
    }
    return uploaded;
}

/// Frees the executor's pages of a page set.
void free_pages(card::Executor& executor, std::vector<PageSlot>& slots) noexcept {
    for (PageSlot& slot : slots)
        if (slot.handle != card::PageHandle{})
            executor.destroy_page(slot.handle);
    slots.clear();
}

/// The emission of one frame: the inputs, the view and the frame the
/// batches go to, with the stage's state.
class Emitter {
  public:

    Emitter(
        ModelStage::Impl& impl,
        gw::SpritePages& pages,
        gw::SpritePages& bright_pages,
        ModelStageCounts& counts,
        std::string& error,
        const ModelFrameInputs& inputs,
        const SceneView& view,
        card::Executor& executor,
        card::CardFrame& frame
    )
        : impl_(impl), pages_(pages), bright_pages_(bright_pages), counts_(counts), error_(error),
          in_(inputs), view_(view), executor_(executor), frame_(frame),
          scale_(pixels_per_map_pixel(view)), whole_scale_(std::floor(scale_) == scale_),
          texel_offset_(whole_scale_ ? texel_edge : texel_centre),
          camera_x_(fixed_of_pixels(view.camera_x)), camera_z_(fixed_of_pixels(view.camera_y)) {
        // The battlefield's rectangle in the target, and where the frame's
        // pixel (0, 0) lands in it: the view's offset moves every vertex by
        // its negative at the zoom, as the sprite stage moves its own.
        pan_x_ = -static_cast<float>(view.offset.x) * view.zoom * view.scale;
        pan_y_ = -static_cast<float>(view.offset.y) * view.zoom * view.scale;
        origin_x_ = view.origin_x + pan_x_;
        origin_y_ = view.origin_y + pan_y_;
        scissor_.x = static_cast<int32_t>(std::lround(view.origin_x));
        scissor_.y = static_cast<int32_t>(std::lround(view.origin_y));
        scissor_.width =
            static_cast<int32_t>(std::lround(static_cast<double>(view.width) * view.scale));
        scissor_.height =
            static_cast<int32_t>(std::lround(static_cast<double>(view.height) * view.scale));
        if (inputs.draws != nullptr) {
            map_scissor_ = shown_map_scissor(
                view,
                view.origin_x,
                view.origin_y,
                inputs.draws->shown_map_width,
                inputs.draws->shown_map_height,
                scissor_
            );
            shadow_map_scissor_ = shown_map_scissor(
                view,
                0.0F,
                0.0F,
                inputs.draws->shown_map_width,
                inputs.draws->shown_map_height,
                {0, 0, scissor_.width, scissor_.height}
            );
        }
    }

    /// Emits the frame's shadows and composes them.
    ///
    /// @param through_target set to whether the shadow target was used
    void shadows(bool& through_target);

    /// Emits one draw of the list.
    ///
    /// @param entry the draw
    void emit(const WorldDraw& entry);

  private:

    /// The target and scissor of the battlefield's own batches.
    struct Canvas {
        card::TargetHandle target{};
        bool scissored{};
        card::Rect scissor{};
        float origin_x{};
        float origin_y{};
    };

    /// Returns the battlefield as the bodies draw into it.
    [[nodiscard]] Canvas battlefield() const noexcept {
        return {view_.target, true, scissor_, origin_x_, origin_y_};
    }

    /// Returns the shadow target as the silhouettes draw into it: the
    /// battlefield's size at its own origin, panned as the battlefield is.
    [[nodiscard]] Canvas shadow_surface() const noexcept {
        return {impl_.shadow_target, false, {}, pan_x_, pan_y_};
    }

    /// Returns the battlefield, or the shadow target, which has no scissor
    /// of its own, as a feature's draws go into it: cut off where the map
    /// the view shows ends.
    ///
    /// @param surface the battlefield or the shadow target
    /// @return the surface, its scissor the part over the map
    [[nodiscard]] Canvas on_map(const Canvas& surface) const noexcept {
        Canvas narrowed = surface;
        narrowed.scissored = true;
        narrowed.scissor = surface.scissored ? map_scissor_ : shadow_map_scissor_;
        return narrowed;
    }

    /// Tells whether a feature's draws show anywhere: whether the map the
    /// view shows lies on the battlefield.
    ///
    /// @return false when the map lies wholly off it
    [[nodiscard]] bool map_shown() const noexcept {
        return map_scissor_.width > 0 && map_scissor_.height > 0;
    }

    // Pages and meshes.
    [[nodiscard]] uint64_t frame_number(const void* key);
    [[nodiscard]] bool place_frame(
        gw::SpritePages& pages,
        std::vector<PageSlot>& slots,
        uint64_t frame_id,
        const Sprite* sprite,
        bool flat,
        const oa::formats::gaf::RenderedFrame* rendered,
        PlacedFrame& out
    );
    [[nodiscard]] bool
    place_texture(const Sprite& sprite, bool flat, bool bright, PlacedFrame& out);
    [[nodiscard]] bool ensure_solid_page();
    [[nodiscard]] const gw::ModelMesh* mesh_of(const draw::ModelRef& model);
    [[nodiscard]] const Sprite* frame_of(
        const gw::ModelMesh& mesh,
        const gw::MeshPrimitive& run,
        const draw::PreparedObject& prepared,
        bool first_frame,
        uint8_t team
    ) const;

    // Placing polygons.
    [[nodiscard]] gw::PixelPoint
    place_vertex(const FixedVector3& v, const Placement& placement) const;
    void add_piece_polygons(
        const draw::ModelRef& model,
        const gw::ModelMesh& mesh,
        const PieceState& state,
        const Placement& placement,
        bool first_frame,
        uint8_t team,
        Shading shading,
        float alpha,
        bool flat_page
    );
    void add_unit_pieces(
        const draw::ModelRef& model,
        const gw::ModelMesh& mesh,
        const Placement& placement,
        bool cached_pieces,
        bool moving_pieces,
        bool first_frame,
        uint8_t team,
        Shading shading,
        float alpha,
        bool flat_page
    );
    void apply_nanoframe(
        const NanoBands& bands,
        size_t first_polygon,
        const draw::ModelRef& model,
        const Placement& placement
    );
    void
    apply_water_and_digger(size_t first_polygon, int32_t lift_threshold, bool own, bool digger);
    void cut_under_nearer(const PlaneCuts& cuts);
    void cut_under_clearing(size_t first_polygon, size_t end_polygon);
    void pieces_of(const Polygon& polygon, std::vector<std::vector<CutPoint>>& pieces) const;
    void keep_pieces(Polygon& polygon, std::span<const std::vector<CutPoint>> pieces);
    void leave_out_holes(const Canvas& surface, const Polygon& polygon);
    void add_flat_object(
        const Object& object,
        const draw::PreparedObject& prepared,
        std::span<const gw::PixelPoint> projected,
        const Sprite* (*texture_of)(const draw::PreparedPrimitive&, uint8_t),
        uint8_t team,
        std::span<const FixedVector3> world_points = {}
    );

    // Emitting.
    /// Appends triangles to the frame, joining the last batch where it draws
    /// alike; `sampling` reads the page as given, else nearest at a
    /// whole-number scale and linear at any other.
    void append(
        const Canvas& surface,
        card::PageHandle page,
        card::Blend blend,
        std::span<const card::Vertex> vertices,
        std::span<const card::Index> indices,
        std::optional<card::Sampling> sampling = std::nullopt
    );
    void emit_polygons(const Canvas& surface);
    void emit_polygon(const Canvas& surface, const Polygon& polygon);
    /// Splits the finished face geometry at sea level before shading its wet part.
    void append_body(const Canvas& surface, const Polygon& polygon, card::PageHandle page);
    void emit_cut_polygon(const Canvas& surface, const Polygon& polygon, const PlacedFrame& placed);
    [[nodiscard]] card::Vertex body_vertex(
        const Canvas& surface,
        const Polygon& polygon,
        const PlacedFrame& placed,
        double x,
        double y,
        double u,
        double v,
        const card::Colour& colour
    ) const;
    void lift_under_water(const Canvas& surface);
    [[nodiscard]] bool has_keyed_texels(const Sprite& sprite);
    [[nodiscard]] bool walk_strips(const Canvas& surface, std::span<const Corner> corners);
    void emit_outline(const Canvas& surface);
    void emit_silhouettes(const Canvas& surface, card::Blend blend, const card::Colour& shade);

    // The kinds.
    void draw_model(const ModelDraw& drawn);
    void draw_projectile(const ProjectileDraw& shot);
    void draw_debris(const DebrisDraw& piece);
    void draw_fragment(const FragmentDraw& fragment);
    void shadow_of_model(
        const ModelDraw& drawn, const Canvas& surface, card::Blend blend, bool unfinished_pass
    );
    void shadow_of_projectile(const ProjectileDraw& shot, const Canvas& surface);
    /// Emits a model's shadow as the game casts it (New unit shadows Off):
    /// the processor's silhouette as a frame on the texture pages, one quad
    /// at the shadows' own darkness, over what the list drew before it.
    void game_shadow_of_model(const ModelDraw& drawn);
    /// Emits a cached, padded silhouette at the shadows' own darkness and
    /// place, its edge softened a little more at altitude.
    bool aircraft_shadow(
        const Unit& unit,
        const draw::ModelState& state,
        const Canvas& surface,
        int32_t x,
        int32_t y,
        float altitude
    );

    /// Returns the alpha the frame's shadows draw at: the alpha table's half
    /// mix at the game's own darkness, less as the list's shadow level falls.
    ///
    /// @return half_alpha times the shadow level's strength
    [[nodiscard]] float shadow_alpha() const noexcept {
        const uint32_t level =
            in_.draws != nullptr ? in_.draws->shadow_level : draw::shadow_full_level;
        return half_alpha * draw::shadow_level_strength(level);
    }

    ModelStage::Impl& impl_;
    gw::SpritePages& pages_;
    gw::SpritePages& bright_pages_;
    ModelStageCounts& counts_;
    std::string& error_;
    const ModelFrameInputs& in_;
    const SceneView& view_;
    card::Executor& executor_;
    card::CardFrame& frame_;
    float scale_{1.0F};
    bool whole_scale_{true};
    float texel_offset_{texel_edge}; ///< where a corner lies within its texel
    card::Rect scissor_{};           ///< the battlefield in the target, every batch's scissor
    card::Rect map_scissor_{};       ///< the part of scissor_ over the map the view shows
    /// The part of the shadow target over the map the view shows.
    card::Rect shadow_map_scissor_{};
    float origin_x_{}; ///< where the frame's pixel (0, 0) lands in the target
    float origin_y_{};
    float pan_x_{}; ///< the view's offset in target pixels, on every vertex
    float pan_y_{};
    int32_t camera_x_{}; ///< the camera, 16.16
    int32_t camera_z_{};
    std::vector<card::Vertex> vertices_;
    std::vector<card::Index> indices_;
    std::vector<card::Vertex> wet_vertices_;
    std::vector<card::Index> wet_indices_;
    /// A textured quad's strips, built beside its corners and triangles.
    std::vector<card::Vertex> strip_vertices_;
    std::vector<card::Index> strip_indices_;
    /// A polygon's triangles with its holes left out, and the scratch the
    /// cutting works in (leave_out_holes).
    std::vector<card::Vertex> hole_vertices_;
    std::vector<card::Index> hole_indices_;
    std::vector<card::Vertex> hole_points_;
    std::vector<uint32_t> hole_ends_;
    std::vector<card::Vertex> hole_next_points_;
    std::vector<uint32_t> hole_next_ends_;
    std::vector<card::Vertex> hole_rest_;
    std::vector<card::Vertex> hole_inner_;
    std::vector<double> hole_sides_;
    std::vector<HoleEdge> hole_edges_;
    std::vector<HoleBox> hole_boxes_;
};

uint64_t Emitter::frame_number(const void* key) {
    const auto found = impl_.numbers.find(key);
    if (found != impl_.numbers.end())
        return found->second;
    const uint64_t number = impl_.next_number++;
    impl_.numbers.emplace(key, number);
    return number;
}

bool Emitter::place_frame(
    gw::SpritePages& pages,
    std::vector<PageSlot>& slots,
    uint64_t frame_id,
    const Sprite* sprite,
    bool flat,
    const oa::formats::gaf::RenderedFrame* rendered,
    PlacedFrame& out
) {
    gw::FrameResult result = pages.find(frame_id, gw::DrawMode::opaque);
    if (result.status == gw::FrameStatus::not_held) {
        if (rendered == nullptr) {
            render_texture(*sprite, flat, impl_.rendered);
            rendered = &impl_.rendered;
        }
        result = pages.frame(frame_id, gw::DrawMode::opaque, *rendered);
        if (result.status == gw::FrameStatus::ok)
            ++counts_.frames_placed;
    }
    if (result.status != gw::FrameStatus::ok)
        return false;
    if (!ensure_page_slot(executor_, pages, slots, result.record.page, counts_, error_))
        return false;
    const PageSlot& slot = slots[result.record.page];
    out = {slot.handle, slot.size, result.record.rect};
    return true;
}

bool Emitter::place_texture(const Sprite& sprite, bool flat, bool bright, PlacedFrame& out) {
    if (sprite.data == nullptr || sprite.width == 0 || sprite.height == 0)
        return false;
    const uint64_t frame_id = frame_number(&sprite) * variants + (flat ? flat_variant : 0);
    return bright ? place_frame(
                        bright_pages_, impl_.bright_slots, frame_id, &sprite, flat, nullptr, out
                    )
                  : place_frame(pages_, impl_.slots, frame_id, &sprite, flat, nullptr, out);
}

bool Emitter::ensure_solid_page() {
    if (impl_.solid_page != card::PageHandle{} && executor_.page_alive(impl_.solid_page))
        return true;
    impl_.solid_page = executor_.create_page({solid_page_edge, solid_page_edge, 1});
    if (impl_.solid_page == card::PageHandle{}) {
        error_ = "model stage: cannot make the solid page: " + executor_.error();
        return false;
    }
    std::array<uint8_t, solid_page_edge * solid_page_edge * card::texel_bytes> white{};
    white.fill(255);
    if (!executor_.update_page(
            impl_.solid_page, 0, nullptr, white.data(), solid_page_edge * card::texel_bytes
        )) {
        error_ = "model stage: cannot upload the solid page: " + executor_.error();
        executor_.destroy_page(impl_.solid_page);
        impl_.solid_page = {};
        return false;
    }
    return true;
}

const gw::ModelMesh* Emitter::mesh_of(const draw::ModelRef& model) {
    if (model.instance == nullptr || model.prepared == nullptr || in_.library == nullptr)
        return nullptr;
    const Model& source = model.instance->model();
    MeshEntry& entry = impl_.meshes[&source];
    const auto& handle = model.instance->model_handle();
    const auto held = entry.owner.lock();
    if (held == nullptr || held.get() != &source) {
        entry.owner = handle;
        entry.refused = false;
        const gw::MeshBuildError error = gw::build_model_mesh(
            source, *model.prepared, in_.library->textures, impl_.palette, impl_.gamma, entry.mesh
        );
        if (error.message != nullptr) {
            entry.refused = true;
            entry.mesh = {};
        } else {
            ++counts_.meshes_built;
        }
    }
    return entry.refused ? nullptr : &entry.mesh;
}

const Sprite* Emitter::frame_of(
    const gw::ModelMesh& mesh,
    const gw::MeshPrimitive& run,
    const draw::PreparedObject& prepared,
    bool first_frame,
    uint8_t team
) const {
    if ((run.flags & gw::primitive_flag_textured) == 0 || run.texture == gw::no_texture ||
        run.texture >= mesh.textures.size())
        return nullptr;
    const gw::MeshTexture& texture = mesh.textures[run.texture];
    if (texture.sequence == nullptr)
        return nullptr;
    size_t index = 0;
    if (texture.kind == gw::TextureKind::team) {
        index = team;
    } else if (texture.kind == gw::TextureKind::animated && !first_frame) {
        if (run.prepared_index >= prepared.primitives.size())
            return nullptr;
        const auto& cursor = prepared.primitives[run.prepared_index].cursor;
        if (cursor.sequence == nullptr)
            return nullptr;
        index = cursor.frame_index;
    }
    if (index >= texture.sequence->frames.size())
        return nullptr;
    const Sprite& frame = texture.sequence->frames[index];
    return frame.data != nullptr ? &frame : nullptr;
}

gw::PixelPoint Emitter::place_vertex(const FixedVector3& v, const Placement& placement) const {
    if (placement.flat) {
        // Each term floored after the unit's offset is added, as the flat
        // draw does; the depth goes unread.
        return gw::pixel_of_model_point(
            FixedVector3{
                wrap_add(v.x, placement.flat_dx),
                wrap_add(v.y, placement.flat_y),
                wrap_sub(v.z, placement.flat_dz)
            },
            placement.depth_base
        );
    }
    gw::PixelPoint point = gw::pixel_of_model_point(v, placement.depth_base);
    point.x += placement.dx;
    point.y += placement.dy;
    point.depth += placement.depth_add;
    return point;
}

void Emitter::add_piece_polygons(
    const draw::ModelRef& model,
    const gw::ModelMesh& mesh,
    const PieceState& state,
    const Placement& placement,
    bool first_frame,
    uint8_t team,
    Shading shading,
    float alpha,
    bool flat_page
) {
    if (state.object_index >= mesh.pieces.size() ||
        state.object_index >= model.prepared->objects.size())
        return;
    const gw::MeshPiece& piece = mesh.pieces[state.object_index];
    const draw::PreparedObject& prepared = model.prepared->objects[state.object_index];
    const bool lit = shading == Shading::rows && (state.flags & piece_shaded) != 0;
    if (lit)
        gw::vertex_normals(
            model.instance->model().objects[state.object_index],
            prepared,
            state.transformed_vertices,
            impl_.normals
        );
    const uint8_t* shade_table =
        shading == Shading::rows && in_.display != nullptr &&
                in_.display->shade.size() >= static_cast<size_t>(oa::present::shade_table_size)
            ? in_.display->shade.data()
            : nullptr;
    const auto& points = state.transformed_vertices;
    for (uint32_t p = piece.first_primitive; p < piece.first_primitive + piece.primitive_count;
         ++p) {
        if (p >= mesh.primitives.size())
            break;
        const gw::MeshPrimitive& run = mesh.primitives[p];
        if (run.corner_count < 3)
            continue;
        const bool textured = (run.flags & gw::primitive_flag_textured) != 0;
        const Sprite* texture = nullptr;
        if (textured) {
            texture = frame_of(mesh, run, prepared, first_frame, team);
            if (texture == nullptr)
                continue;
        }
        Polygon polygon;
        polygon.first_corner = static_cast<uint32_t>(impl_.corners.size());
        polygon.corner_count = run.corner_count;
        polygon.mesh = &mesh;
        polygon.primitive = p;
        polygon.texture = texture;
        polygon.flat_page = flat_page;
        polygon.clears = !textured;
        int64_t depth_sum = 0;
        int32_t highest_row = 0;
        bool valid = true;
        bool submerged_corner = false;
        for (uint32_t k = 0; k < run.corner_count; ++k) {
            const uint32_t vertex_index = run.first_vertex + k;
            if (vertex_index >= mesh.vertices.size()) {
                valid = false;
                break;
            }
            const gw::MeshVertex& vertex = mesh.vertices[vertex_index];
            const uint16_t source = mesh.source_vertex[vertex_index];
            if (source >= points.size()) {
                valid = false;
                break;
            }
            const gw::PixelPoint placed = place_vertex(points[source], placement);
            Corner corner;
            corner.x = placed.x;
            corner.y = placed.y;
            corner.depth = placed.depth;
            if ((in_.water_shimmer > 0 || in_.vertex_heights != nullptr) && model.unit) {
                corner.world_height =
                    points[source].y / 65536.0F + model.unit->position.y / 65536.0F;
                submerged_corner = submerged_corner || corner.world_height < in_.water_sea_level;
            }
            corner.u = vertex.u;
            corner.v = vertex.v;
            depth_sum += placed.depth;
            int32_t row = draw::unlit_shade;
            if (lit && source < impl_.normals.size()) {
                const gw::VertexNormal& normal = impl_.normals[source];
                row =
                    gw::shade_row(normal.x, normal.y, normal.z, in_.light.data(), in_.light_scale);
            }
            highest_row = std::max(highest_row, row);
            if (textured) {
                // The row's multiplier, which the normal page clamps at 1;
                // the bright page's halving comes once the rows are known.
                const float multiplier =
                    shading == Shading::rows ? static_cast<float>(row) * shade_row_scale : 1.0F;
                corner.colour = {multiplier, multiplier, multiplier, alpha};
            } else if (shade_table != nullptr) {
                const uint8_t remapped = shade_table
                    [static_cast<size_t>(row) * OA_PALETTE_COLORS + vertex.palette_index];
                corner.colour = palette_colour(impl_.palette, impl_.gamma, remapped, alpha);
                polygon.clears = polygon.clears && remapped == draw::image_key;
            } else {
                corner.colour = {level(vertex.red), level(vertex.green), level(vertex.blue), alpha};
                polygon.clears = polygon.clears && vertex.palette_index == draw::image_key;
            }
            impl_.corners.push_back(corner);
        }
        if (!valid) {
            impl_.corners.resize(polygon.first_corner);
            continue;
        }
        polygon.depth = static_cast<int32_t>(depth_sum / run.corner_count);
        if (model.unit &&
            ((in_.water_shimmer > 0 && submerged_corner) || in_.vertex_heights != nullptr)) {
            water_plane(
                polygon,
                std::span(impl_.corners.data() + polygon.first_corner, polygon.corner_count)
            );
            // Shaded by the shimmer only where it dips under the water.
            polygon.water_geometry =
                polygon.water_geometry && in_.water_shimmer > 0 && submerged_corner;
        }
        if (textured) {
            polygon.bright = highest_row >= first_bright_row;
            for (uint32_t k = 0; k < run.corner_count; ++k) {
                card::Colour& colour = impl_.corners[polygon.first_corner + k].colour;
                const float multiplier =
                    std::min(1.0F, polygon.bright ? colour.red * half_colour : colour.red);
                colour.red = colour.green = colour.blue = multiplier;
            }
        }
        impl_.polygons.push_back(polygon);
    }
}

void Emitter::add_unit_pieces(
    const draw::ModelRef& model,
    const gw::ModelMesh& mesh,
    const Placement& placement,
    bool cached_pieces,
    bool moving_pieces,
    bool first_frame,
    uint8_t team,
    Shading shading,
    float alpha,
    bool flat_page
) {
    const auto pieces = model.instance->pieces();
    for (size_t n = pieces.size(); n != 0; --n) {
        const PieceState& state = pieces[n - 1];
        if ((state.flags & piece_visible) == 0)
            continue;
        const bool cached = (state.flags & piece_cached) != 0;
        if (!(cached ? cached_pieces : moving_pieces))
            continue;
        add_piece_polygons(
            model, mesh, state, placement, first_frame, team, shading, alpha, flat_page
        );
    }
}

void Emitter::apply_nanoframe(
    const NanoBands& bands,
    size_t first_polygon,
    const draw::ModelRef& model,
    const Placement& placement
) {
    if (!bands.active)
        return;
    for (size_t i = first_polygon; i < impl_.polygons.size(); ++i) {
        Polygon& polygon = impl_.polygons[i];
        // The bands leave the image key's pixels as they are.
        if (polygon.clears)
            continue;
        const int32_t value = classify_depth(bands, static_cast<uint8_t>(polygon.depth));
        if (value == draw::remap_keep)
            continue;
        if (value == draw::remap_clear) {
            polygon.corner_count = 0;
            continue;
        }
        polygon.texture = nullptr;
        for (uint32_t k = 0; k < polygon.corner_count; ++k) {
            card::Colour& colour = impl_.corners[polygon.first_corner + k].colour;
            colour = palette_colour(
                impl_.palette, impl_.gamma, static_cast<uint8_t>(value), colour.alpha
            );
        }
    }
    // The outline, as the processor draws it over the image: each visible
    // piece's primitives, cleared ones too, in the processor's order, the
    // first and the last pixel of each row a primitive spans, where nothing
    // nearer of the image lies. The image's depth plane is found from its
    // primitives on a plane of the model's own.
    impl_.outline_corners.clear();
    impl_.outline_primitives.clear();
    const Model& source = model.instance->model();
    const auto pieces = model.instance->pieces();
    for (size_t n = pieces.size(); n != 0; --n) {
        const PieceState& state = pieces[n - 1];
        if ((state.flags & piece_visible) == 0 || state.object_index >= source.objects.size() ||
            state.object_index >= model.prepared->objects.size())
            continue;
        const Object& object = source.objects[state.object_index];
        const draw::PreparedObject& prepared = model.prepared->objects[state.object_index];
        const auto& points = state.transformed_vertices;
        for (uint32_t i = first_primitive(prepared); i < prepared.primitives.size(); ++i) {
            const draw::PreparedPrimitive& primitive = prepared.primitives[i];
            if (primitive.source_index >= object.primitives.size())
                continue;
            const auto& corners = object.primitives[primitive.source_index].vertex_indices;
            if (corners.empty() || std::any_of(corners.begin(), corners.end(), [&](uint16_t c) {
                    return c >= points.size();
                }))
                continue;
            for (const uint16_t corner : corners) {
                const gw::PixelPoint placed = place_vertex(points[corner], placement);
                impl_.outline_corners.push_back({placed.x, placed.y, placed.depth});
            }
            const bool fills = (primitive.flags & draw::primitive_colored) != 0 ||
                               corners.size() == quad_corner_count;
            impl_.outline_primitives.emplace_back(static_cast<uint32_t>(corners.size()), fills);
        }
    }
    if (impl_.outline_corners.empty())
        return;
    int32_t left = impl_.outline_corners.front().x;
    int32_t right = left;
    int32_t top = impl_.outline_corners.front().y;
    int32_t bottom = top;
    for (const auto& corner : impl_.outline_corners) {
        left = std::min(left, corner.x);
        right = std::max(right, corner.x);
        top = std::min(top, corner.y);
        bottom = std::max(bottom, corner.y);
    }
    const int64_t wide = int64_t{right} - left + 1 + 2 * outline_plane_margin;
    const int64_t high = int64_t{bottom} - top + 1 + 2 * outline_plane_margin;
    if (wide > most_outline_plane_side || high > most_outline_plane_side)
        return;
    const int32_t shift_x = outline_plane_margin - left;
    const int32_t shift_y = outline_plane_margin - top;
    for (auto& corner : impl_.outline_corners) {
        corner.x += shift_x;
        corner.y += shift_y;
    }
    const auto size = static_cast<size_t>(wide * high);
    impl_.outline_depth.assign(size, 0);
    impl_.outline_marks.assign(size, 0);
    Sprite plane{};
    plane.width = static_cast<uint16_t>(wide);
    plane.height = static_cast<uint16_t>(high);
    plane.key = 0;
    plane.data = impl_.outline_marks.data();
    plane.aux = impl_.outline_depth.data();
    // The depth plane: every primitive the image fills, its marks left
    // unmarked.
    size_t first = 0;
    for (const auto& [count, fills] : impl_.outline_primitives) {
        if (fills && count >= 3)
            oa::present::fill_depth_polygon(
                plane, impl_.outline_corners.data() + first, static_cast<int32_t>(count), 0
            );
        first += count;
    }
    // The outline over it, each primitive closed by its first corner again.
    constexpr uint8_t marked = 1;
    first = 0;
    for (const auto& [count, fills] : impl_.outline_primitives) {
        impl_.outline_scratch.assign(
            impl_.outline_corners.begin() + static_cast<std::ptrdiff_t>(first),
            impl_.outline_corners.begin() + static_cast<std::ptrdiff_t>(first + count)
        );
        impl_.outline_scratch.push_back(impl_.outline_scratch.front());
        oa::present::outline_depth_polygon(
            plane,
            impl_.outline_scratch.data(),
            static_cast<int32_t>(impl_.outline_scratch.size()),
            marked
        );
        first += count;
    }
    const card::Colour colour = palette_colour(impl_.palette, impl_.gamma, bands.outline, 1.0F);
    for (int32_t y = 0; y < plane.height; ++y) {
        const uint8_t* row = impl_.outline_marks.data() + static_cast<size_t>(y) * plane.width;
        for (int32_t x = 0; x < plane.width;) {
            if (row[x] != marked) {
                ++x;
                continue;
            }
            const int32_t start = x;
            while (x < plane.width && row[x] == marked)
                ++x;
            impl_.outline_runs.push_back({start - shift_x, y - shift_y, x - start, colour});
        }
    }
}

void Emitter::apply_water_and_digger(
    size_t first_polygon, int32_t lift_threshold, bool own, bool digger
) {
    for (size_t i = first_polygon; i < impl_.polygons.size(); ++i) {
        Polygon& polygon = impl_.polygons[i];
        if (polygon.corner_count == 0)
            continue;
        const int32_t depth = static_cast<uint8_t>(polygon.depth);
        // Visibility keeps the game's depth rule. With Underwater shimmer
        // off, so does the colour, as 3.1c draws it; on, the colour comes
        // from the physical height instead (append_body).
        if (lift_threshold >= 0 && depth <= lift_threshold) {
            if (!own)
                polygon.corner_count = 0;
            else if (in_.water_shimmer <= 0)
                polygon.underwater = true;
        }
        if (digger && depth <= draw::digger_clip_depth)
            polygon.corner_count = 0;
    }
}

/// Returns the pieces of a polygon as clearing leaves it: the pieces cut
/// from it, or its corners whole; its holes (cut_under_nearer) are not cut.
///
/// @param polygon the polygon
/// @param[out] pieces its pieces
void Emitter::pieces_of(const Polygon& polygon, std::vector<std::vector<CutPoint>>& pieces) const {
    if (!polygon.cut) {
        pieces.resize(1);
        pieces[0].clear();
        for (const Corner& corner : std::span<const Corner>(
                 impl_.corners.data() + polygon.first_corner, polygon.corner_count
             ))
            pieces[0].push_back(cut_point_of(corner));
        return;
    }
    pieces.resize(polygon.piece_count);
    for (uint32_t p = 0; p < polygon.piece_count; ++p) {
        const CutPiece& piece = impl_.cut_pieces[polygon.first_piece + p];
        const auto first =
            impl_.cut_points.begin() + static_cast<std::ptrdiff_t>(piece.first_point);
        pieces[p].assign(first, first + static_cast<std::ptrdiff_t>(piece.point_count));
    }
}

/// Makes a polygon drawn as pieces cut from it, adding them to the unit's
/// cut pieces.
///
/// @param[in,out] polygon the polygon
/// @param pieces the pieces it keeps, none when nothing of it is left
void Emitter::keep_pieces(Polygon& polygon, std::span<const std::vector<CutPoint>> pieces) {
    polygon.cut = true;
    polygon.first_piece = static_cast<uint32_t>(impl_.cut_pieces.size());
    polygon.piece_count = static_cast<uint32_t>(pieces.size());
    for (const std::vector<CutPoint>& piece : pieces) {
        CutPiece kept_piece;
        kept_piece.first_point = static_cast<uint32_t>(impl_.cut_points.size());
        kept_piece.point_count = static_cast<uint32_t>(piece.size());
        impl_.cut_pieces.push_back(kept_piece);
        impl_.cut_points.insert(impl_.cut_points.end(), piece.begin(), piece.end());
    }
}

/// Gives a depth-plane unit's polygons, in drawing order, the holes they
/// leave out where the plane shows polygons drawn before them
/// (find_plane_cuts), so that those show through there; the holes are cut
/// out of the triangles each polygon is drawn as (leave_out_holes).
///
/// @param cuts the polygons' cuts, as order_as_depth_plane gave them
void Emitter::cut_under_nearer(const PlaneCuts& cuts) {
    auto& polygons = impl_.polygons;
    if (cuts.first_cut.size() != polygons.size() + 1)
        return;
    for (size_t i = 0; i < polygons.size(); ++i) {
        Polygon& polygon = polygons[i];
        polygon.first_hole = static_cast<uint32_t>(impl_.holes.size());
        polygon.hole_count = cuts.first_cut[i + 1] - cuts.first_cut[i];
        for (uint32_t c = cuts.first_cut[i]; c < cuts.first_cut[i + 1]; ++c) {
            const PlaneCut& cut = cuts.cuts[c];
            impl_.holes.push_back(
                {static_cast<uint32_t>(impl_.hole_points.size()), cut.point_count}
            );
            for (const PlanePoint& point : cuts.points.subspan(cut.first_point, cut.point_count))
                impl_.hole_points.push_back({point.x + cuts.origin.x, point.y + cuts.origin.y});
        }
    }
}

/// Clears from a unit's picture what its polygons of the image key cover, as
/// the processor's picture of the unit leaves the key's pixels clear, so
/// that the ground shows there: each polygon drawn before one that clears
/// and sharing area with it keeps only its pieces outside that one, and the
/// polygons that clear draw nothing. One that faces away clears nothing, as
/// it fills nothing, and one with holes under nearer polygons
/// (cut_under_nearer) clears nothing there. A model hides its pieces sunk
/// below the ground behind walls of the key so.
///
/// @param first_polygon the picture's first polygon, in drawing order
/// @param end_polygon one past its last
void Emitter::cut_under_clearing(size_t first_polygon, size_t end_polygon) {
    auto& polygons = impl_.polygons;
    end_polygon = std::min(end_polygon, polygons.size());
    const auto corners_of = [this](const Polygon& polygon) {
        return std::span<const Corner>(
            impl_.corners.data() + polygon.first_corner, polygon.corner_count
        );
    };
    const auto clearing = [&](const Polygon& polygon) {
        return polygon.clears && polygon.corner_count >= 3 && winding(corners_of(polygon)) > 0;
    };
    if (std::none_of(
            polygons.begin() + static_cast<std::ptrdiff_t>(first_polygon),
            polygons.begin() + static_cast<std::ptrdiff_t>(end_polygon),
            clearing
        ))
        return;
    auto& left = impl_.cut_left;
    auto& region = impl_.cut_region;
    // What each polygon that clears clears: its corners, or the pieces
    // left of it.
    std::vector<std::vector<CutPoint>>& clears = impl_.cut_clears;
    for (size_t i = first_polygon; i < end_polygon; ++i) {
        Polygon& polygon = polygons[i];
        if (polygon.clears || polygon.corner_count < 3)
            continue;
        const std::span<const Corner> corners = corners_of(polygon);
        if (winding(corners) <= 0)
            continue;
        bool cut = false;
        for (size_t j = i + 1; j < end_polygon && !(cut && left.empty()); ++j) {
            const Polygon& over = polygons[j];
            if (!clearing(over))
                continue;
            const std::span<const Corner> over_corners = corners_of(over);
            PlanePoint centre;
            if (!shared_centre(over_corners, corners, corners[0], impl_.plane_order, centre))
                continue;
            if (!cut) {
                pieces_of(polygon, left);
                cut = true;
            }
            // What it clears: where it shows, its pieces but its holes.
            pieces_of(over, clears);
            for (uint32_t h = over.first_hole; h < over.first_hole + over.hole_count; ++h) {
                const PlaneCut& hole = impl_.holes[h];
                cut_away(
                    clears,
                    std::span<const PlanePoint>(impl_.hole_points)
                        .subspan(hole.first_point, hole.point_count),
                    impl_.cut_kept,
                    impl_.cut_rest,
                    impl_.cut_side
                );
            }
            for (const std::vector<CutPoint>& clear : clears) {
                region.clear();
                for (const CutPoint& point : clear)
                    region.push_back({point.x, point.y});
                cut_away(left, region, impl_.cut_kept, impl_.cut_rest, impl_.cut_side);
            }
        }
        if (cut)
            keep_pieces(polygon, left);
    }
    for (size_t i = first_polygon; i < end_polygon; ++i)
        if (polygons[i].clears)
            polygons[i].corner_count = 0;
}

void Emitter::add_flat_object(
    const Object& object,
    const draw::PreparedObject& prepared,
    std::span<const gw::PixelPoint> projected,
    const Sprite* (*texture_of)(const draw::PreparedPrimitive&, uint8_t),
    uint8_t team,
    std::span<const FixedVector3> world_points
) {
    constexpr std::array<std::pair<float, float>, 4> quad_corners{
        std::pair{0.0F, 0.0F}, std::pair{1.0F, 0.0F}, std::pair{1.0F, 1.0F}, std::pair{0.0F, 1.0F}
    };
    for (uint32_t i = first_primitive(prepared); i < prepared.primitives.size(); ++i) {
        const draw::PreparedPrimitive& primitive = prepared.primitives[i];
        if (primitive.source_index >= object.primitives.size())
            continue;
        const auto& indices = object.primitives[primitive.source_index].vertex_indices;
        const bool colored = (primitive.flags & draw::primitive_colored) != 0;
        const Sprite* texture = nullptr;
        if (colored) {
            if (indices.size() < 3)
                continue;
        } else {
            if (indices.size() != quad_corners.size())
                continue;
            texture = texture_of(primitive, team);
            if (texture == nullptr)
                continue;
        }
        Polygon polygon;
        polygon.first_corner = static_cast<uint32_t>(impl_.corners.size());
        polygon.corner_count = static_cast<uint16_t>(indices.size());
        polygon.texture = texture;
        polygon.flat_page = true;
        bool valid = true;
        bool submerged = false;
        for (size_t k = 0; k < indices.size(); ++k) {
            if (indices[k] >= projected.size()) {
                valid = false;
                break;
            }
            const gw::PixelPoint& placed = projected[indices[k]];
            Corner corner;
            corner.x = placed.x;
            corner.y = placed.y;
            if (in_.water_shimmer > 0 && indices[k] < world_points.size()) {
                corner.world_height = world_points[indices[k]].y / 65536.0F;
                submerged = submerged || corner.world_height < in_.water_sea_level;
            }
            if (texture != nullptr) {
                corner.u = quad_corners[k].first;
                corner.v = quad_corners[k].second;
                corner.colour = {1.0F, 1.0F, 1.0F, 1.0F};
            } else {
                corner.colour = palette_colour(impl_.palette, impl_.gamma, primitive.color, 1.0F);
            }
            impl_.corners.push_back(corner);
        }
        if (!valid) {
            impl_.corners.resize(polygon.first_corner);
            continue;
        }
        if (submerged)
            water_plane(
                polygon,
                std::span(impl_.corners.data() + polygon.first_corner, polygon.corner_count)
            );
        impl_.polygons.push_back(polygon);
    }
}

void Emitter::append(
    const Canvas& surface,
    card::PageHandle page,
    card::Blend blend,
    std::span<const card::Vertex> vertices,
    std::span<const card::Index> indices,
    std::optional<card::Sampling> asked_sampling
) {
    const card::Sampling sampling =
        asked_sampling.value_or(whole_scale_ ? card::Sampling::nearest : card::Sampling::linear);
    const auto base = static_cast<card::Index>(frame_.vertices.size());
    const auto first = static_cast<card::Index>(frame_.indices.size());
    frame_.vertices.insert(frame_.vertices.end(), vertices.begin(), vertices.end());
    for (const card::Index index : indices)
        frame_.indices.push_back(base + index);
    const auto count = static_cast<uint32_t>(indices.size());
    if (!frame_.batches.empty()) {
        card::Batch& last = frame_.batches.back();
        if (last.operation == card::Operation::draw && last.target == surface.target &&
            last.page == page && last.level == 0 && last.blend == blend &&
            last.sampling == sampling && last.scissored == surface.scissored &&
            last.scissor.x == surface.scissor.x && last.scissor.y == surface.scissor.y &&
            last.scissor.width == surface.scissor.width &&
            last.scissor.height == surface.scissor.height &&
            last.first_index + last.index_count == first) {
            last.index_count += count;
            return;
        }
    }
    card::Batch batch;
    batch.operation = card::Operation::draw;
    batch.target = surface.target;
    batch.page = page;
    batch.level = 0;
    batch.blend = blend;
    batch.sampling = sampling;
    batch.scissored = surface.scissored;
    batch.scissor = surface.scissor;
    batch.first_index = first;
    batch.index_count = count;
    frame_.batches.push_back(batch);
}

/// Tells whether a texture frame has a texel of the image key, which the
/// frame's keyed variant leaves transparent. A frame is read once.
///
/// @param sprite the frame
/// @return true when one of its texels is the image key
bool Emitter::has_keyed_texels(const Sprite& sprite) {
    const auto [found, added] = impl_.keyed_texels.try_emplace(&sprite, false);
    if (added && sprite.data != nullptr) {
        const auto* pixels = static_cast<const uint8_t*>(sprite.data);
        const auto size = static_cast<size_t>(sprite.width) * sprite.height;
        found->second = std::find(pixels, pixels + size, draw::image_key) != pixels + size;
    }
    return found->second;
}

namespace {

/// Returns how far a quad is from a parallelogram: the length of its
/// first and third corners' sum less its second and fourth's, in map
/// pixels. Its two triangles move its texels from where the processor's
/// walk puts them by up to a quarter of that, at zoom 1.
///
/// @param corners the quad's four corners
/// @return the twist, 0 for a parallelogram
[[nodiscard]] double twist_of(std::span<const Corner> corners) noexcept {
    const auto across =
        static_cast<double>(corners[0].x) + corners[2].x - corners[1].x - corners[3].x;
    const auto down =
        static_cast<double>(corners[0].y) + corners[2].y - corners[1].y - corners[3].y;
    return std::hypot(across, down);
}

} // namespace

/// Cuts a textured quad into strips across its rows, so that its texels
/// run as the processor's walk runs them: in proportion down each side
/// from the topmost corner to the bottommost, then in proportion along
/// each row from the left side to the right. Two triangles would bend them
/// where they meet, unless the quad is a parallelogram.
///
/// @param surface the canvas the quad is drawn on
/// @param corners the quad's corners, which vertices_ holds placed on the
///     surface with their texture coordinates and colours
/// @return true with the strips in strip_vertices_ and strip_indices_, no
///     strip where every row is empty; false, with nothing built, for a
///     quad its two triangles draw as the walk does to within
///     two_triangle_shift_allowed of a pixel (a parallelogram, a quad small
///     at the zoom, or one of no height), for a quad with a side that turns
///     back up or runs along a row between two sloped edges, and once the
///     frame holds most_strip_frame_vertices or most_strip_frame_indices
bool Emitter::walk_strips(const Canvas& surface, std::span<const Corner> corners) {
    if (corners.size() != quad_corner_count || vertices_.size() != quad_corner_count ||
        frame_.vertices.size() >= most_strip_frame_vertices ||
        frame_.indices.size() >= most_strip_frame_indices)
        return false;
    const double twist = twist_of(corners);
    counts_.widest_twist = std::max(counts_.widest_twist, twist);
    if (twist * static_cast<double>(scale_) / 4.0 <= two_triangle_shift_allowed)
        return false;
    size_t top = 0;
    size_t bottom = 0;
    for (size_t k = 1; k < quad_corner_count; ++k) {
        if (corners[k].y < corners[top].y)
            top = k;
        if (corners[bottom].y < corners[k].y)
            bottom = k;
    }
    if (corners[top].y == corners[bottom].y)
        return false;

    // Each side's sloped edges from the top corner down to the bottom one:
    // the left side runs back through the corners, the right side forward.
    // An edge along a row has no rows of its own.
    struct Edge {
        size_t from{};
        size_t to{};
    };

    using Side = std::array<Edge, quad_corner_count>;
    const auto walk_side = [&](bool forward, Side& edges, size_t& count) {
        size_t reached = top;
        for (size_t k = top; k != bottom;) {
            const size_t next = forward ? (k + 1) % quad_corner_count
                                        : (k + quad_corner_count - 1) % quad_corner_count;
            if (corners[next].y < corners[k].y)
                return false;
            if (corners[k].y < corners[next].y) {
                if (count != 0 && reached != k)
                    return false;
                edges[count++] = {k, next};
                reached = next;
            }
            k = next;
        }
        return count != 0;
    };
    Side left_side{};
    Side right_side{};
    size_t left_count = 0;
    size_t right_count = 0;
    if (!walk_side(false, left_side, left_count) || !walk_side(true, right_side, right_count))
        return false;
    // Where an edge crosses a row of the target: its corners' places,
    // texture coordinates and colours, each in proportion along it.
    const auto on_edge = [&](const Edge& edge, float y) {
        const card::Vertex& from = vertices_[edge.from];
        const card::Vertex& to = vertices_[edge.to];
        const float t = (y - from.y) / (to.y - from.y);
        if (t <= 0.0F)
            return from;
        if (t >= 1.0F)
            return to;
        card::Vertex end;
        end.x = from.x + t * (to.x - from.x);
        end.y = y;
        end.u = from.u + t * (to.u - from.u);
        end.v = from.v + t * (to.v - from.v);
        end.colour.red = from.colour.red + t * (to.colour.red - from.colour.red);
        end.colour.green = from.colour.green + t * (to.colour.green - from.colour.green);
        end.colour.blue = from.colour.blue + t * (to.colour.blue - from.colour.blue);
        end.colour.alpha = from.colour.alpha + t * (to.colour.alpha - from.colour.alpha);
        return end;
    };
    strip_vertices_.clear();
    strip_indices_.clear();
    // Cuts the quad at a row, and adds the strip from the cut above unless
    // neither of its rows reaches right of its left end: the walk draws
    // nothing of a row whose right end is not past its left.
    const auto cut = [&](const Edge& on_left, const Edge& on_right, float y) {
        const auto below = static_cast<card::Index>(strip_vertices_.size());
        strip_vertices_.push_back(on_edge(on_left, y));
        strip_vertices_.push_back(on_edge(on_right, y));
        if (below == 0)
            return;
        const card::Index above = below - 2;
        if (strip_vertices_[above + 1].x <= strip_vertices_[above].x &&
            strip_vertices_[below + 1].x <= strip_vertices_[below].x)
            return;
        for (const card::Index index : {above, above + 1, below + 1, above, below + 1, below})
            strip_indices_.push_back(index);
    };
    // Between two corners' rows each side runs along one edge. A strip
    // there moves the texels along a row by a quarter of its height times
    // how much faster one edge slopes than the other, at most; the strips
    // are cut at every corner's row, and at rows of pixel centres as many
    // rows apart as keep that within strip_shift_allowed. More than a
    // strip's height outside the surface's scissor only the corners' rows
    // are cut, since nothing there is seen.
    const float view_top = surface.scissored ? static_cast<float>(surface.scissor.y) : 0.0F;
    const float view_bottom = surface.scissored
                                  ? view_top + static_cast<float>(surface.scissor.height)
                                  : static_cast<float>(card::largest_target_edge);
    size_t left = 0;
    size_t right = 0;
    cut(left_side[0], right_side[0], vertices_[top].y);
    while (left < left_count && right < right_count) {
        const Edge& left_edge = left_side[left];
        const Edge& right_edge = right_side[right];
        const auto slope = [&](const Edge& edge) {
            return static_cast<float>(corners[edge.to].x - corners[edge.from].x) /
                   static_cast<float>(corners[edge.to].y - corners[edge.from].y);
        };
        const float narrowing = std::fabs(slope(right_edge) - slope(left_edge));
        const int32_t lower_row = std::min(corners[left_edge.to].y, corners[right_edge.to].y);
        const float lower = surface.origin_y + static_cast<float>(lower_row) * scale_;
        if (narrowing > 0.0F) {
            const float rows = std::max(1.0F, std::floor(4.0F * strip_shift_allowed / narrowing));
            const float upper = std::max(strip_vertices_.back().y, view_top - rows);
            const float last = std::min(lower, view_bottom + rows);
            for (float y = std::floor(upper - pixel_centre) + 1.0F + pixel_centre; y < last;
                 y += rows)
                cut(left_edge, right_edge, y);
        }
        cut(left_edge, right_edge, lower);
        if (corners[left_edge.to].y == lower_row)
            ++left;
        if (corners[right_edge.to].y == lower_row)
            ++right;
    }
    return true;
}

/// Leaves out of the triangles vertices_ and indices_ hold the holes of a
/// polygon (cut_under_nearer), placed on the surface: each triangle keeps
/// its parts outside every hole, the vertices on the cuts taken in
/// proportion along its edges, so that a textured quad's strips keep their
/// texels. The parts are drawn as fans. An edge of no length, where two of a
/// hole's corners meet, divides nothing.
///
/// @param surface the canvas the polygon is drawn on
/// @param polygon the polygon, with its holes
void Emitter::leave_out_holes(const Canvas& surface, const Polygon& polygon) {
    const auto between = [](const card::Vertex& from, const card::Vertex& to, float along) {
        const auto mix = [along](float first, float second) {
            return first + along * (second - first);
        };
        card::Vertex point;
        point.x = mix(from.x, to.x);
        point.y = mix(from.y, to.y);
        point.u = mix(from.u, to.u);
        point.v = mix(from.v, to.v);
        point.colour = {
            mix(from.colour.red, to.colour.red),
            mix(from.colour.green, to.colour.green),
            mix(from.colour.blue, to.colour.blue),
            mix(from.colour.alpha, to.colour.alpha)
        };
        return point;
    };
    const auto twice_area = [](std::span<const card::Vertex> part) {
        double sum = 0.0;
        for (size_t m = 0; m < part.size(); ++m) {
            const card::Vertex& here = part[m];
            const card::Vertex& next = part[(m + 1) % part.size()];
            sum += static_cast<double>(here.x) * next.y - static_cast<double>(next.x) * here.y;
        }
        return sum;
    };
    const double least_area = 2.0 * least_shared_area * static_cast<double>(scale_ * scale_);
    // The holes placed on the surface: the lines along their edges, each a
    // side function across * y - down * x + offset, positive inside, an edge
    // of no length left out, and the box round each hole, so that a part
    // outside a hole's box is not cut by it.
    auto& edges = hole_edges_;
    auto& boxes = hole_boxes_;
    edges.clear();
    boxes.clear();
    for (uint32_t h = polygon.first_hole; h < polygon.first_hole + polygon.hole_count; ++h) {
        const PlaneCut& hole = impl_.holes[h];
        HoleBox box{static_cast<uint32_t>(edges.size()), 0};
        const auto placed = [&](uint32_t k) {
            const PlanePoint& point = impl_.hole_points[hole.first_point + k % hole.point_count];
            return PlanePoint{
                surface.origin_x + point.x * static_cast<double>(scale_),
                surface.origin_y + point.y * static_cast<double>(scale_)
            };
        };
        for (uint32_t k = 0; k < hole.point_count; ++k) {
            const PlanePoint from = placed(k);
            const PlanePoint to = placed(k + 1);
            box.left = k == 0 ? from.x : std::min(box.left, from.x);
            box.right = k == 0 ? from.x : std::max(box.right, from.x);
            box.top = k == 0 ? from.y : std::min(box.top, from.y);
            box.bottom = k == 0 ? from.y : std::max(box.bottom, from.y);
            if (from.x == to.x && from.y == to.y)
                continue;
            const double across = to.x - from.x;
            const double down = to.y - from.y;
            edges.push_back({across, down, down * from.x - across * from.y});
            ++box.edge_count;
        }
        boxes.push_back(box);
    }
    const auto outside = [](const HoleBox& box, std::span<const card::Vertex> part) {
        double left = part[0].x;
        double right = left;
        double top = part[0].y;
        double bottom = top;
        for (const card::Vertex& point : part) {
            left = std::min(left, static_cast<double>(point.x));
            right = std::max(right, static_cast<double>(point.x));
            top = std::min(top, static_cast<double>(point.y));
            bottom = std::max(bottom, static_cast<double>(point.y));
        }
        return right <= box.left || box.right <= left || bottom <= box.top || box.bottom <= top;
    };
    // The parts of a triangle left, each a run of points of a pool ending
    // where its end says, cut hole by hole from one pool into the other.
    auto& points = hole_points_;
    auto& ends = hole_ends_;
    auto& next_points = hole_next_points_;
    auto& next_ends = hole_next_ends_;
    auto& rest = hole_rest_;
    auto& inner = hole_inner_;
    auto& sides = hole_sides_;
    const auto keep_part = [&](std::span<const card::Vertex> part) {
        next_points.insert(next_points.end(), part.begin(), part.end());
        next_ends.push_back(static_cast<uint32_t>(next_points.size()));
    };
    hole_vertices_.clear();
    hole_indices_.clear();
    for (size_t t = 0; t + 2 < indices_.size(); t += 3) {
        const std::array<card::Vertex, 3> triangle{
            vertices_[indices_[t]], vertices_[indices_[t + 1]], vertices_[indices_[t + 2]]
        };
        // A triangle no hole's box reaches is kept whole.
        if (std::all_of(boxes.begin(), boxes.end(), [&](const HoleBox& box) {
                return outside(box, triangle);
            })) {
            const auto first = static_cast<card::Index>(hole_vertices_.size());
            hole_vertices_.insert(hole_vertices_.end(), triangle.begin(), triangle.end());
            for (card::Index k = 0; k < 3; ++k)
                hole_indices_.push_back(first + k);
            continue;
        }
        points.assign(triangle.begin(), triangle.end());
        ends.assign(1, 3);
        for (const HoleBox& box : boxes) {
            next_points.clear();
            next_ends.clear();
            uint32_t begin = 0;
            for (const uint32_t end : ends) {
                const std::span<const card::Vertex> part(points.data() + begin, end - begin);
                begin = end;
                if (outside(box, part)) {
                    keep_part(part);
                    continue;
                }
                // Each part outside an edge is kept, and the rest goes on to
                // the next edge; what lies inside every edge is the hole's.
                rest.assign(part.begin(), part.end());
                for (uint32_t e = box.first_edge; e < box.first_edge + box.edge_count; ++e) {
                    const HoleEdge& edge = edges[e];
                    sides.clear();
                    bool any_inside = false;
                    bool any_outside = false;
                    for (const card::Vertex& point : rest) {
                        const double side =
                            edge.across * point.y - edge.down * point.x + edge.offset;
                        sides.push_back(side);
                        any_inside = any_inside || side > 0.0;
                        any_outside = any_outside || side < 0.0;
                    }
                    if (!any_inside) {
                        keep_part(rest);
                        rest.clear();
                        break;
                    }
                    if (!any_outside)
                        continue;
                    // The part outside the edge, kept, and the part inside,
                    // a point on the line in both.
                    const auto outer_begin = next_points.size();
                    inner.clear();
                    for (size_t m = 0; m < rest.size(); ++m) {
                        const size_t n = (m + 1) % rest.size();
                        if (sides[m] <= 0.0)
                            next_points.push_back(rest[m]);
                        if (sides[m] >= 0.0)
                            inner.push_back(rest[m]);
                        if ((sides[m] < 0.0 && sides[n] > 0.0) ||
                            (sides[m] > 0.0 && sides[n] < 0.0)) {
                            const card::Vertex point = between(
                                rest[m],
                                rest[n],
                                static_cast<float>(sides[m] / (sides[m] - sides[n]))
                            );
                            next_points.push_back(point);
                            inner.push_back(point);
                        }
                    }
                    const std::span<const card::Vertex> outer(
                        next_points.data() + outer_begin, next_points.size() - outer_begin
                    );
                    if (outer.size() >= 3 && std::abs(twice_area(outer)) >= least_area)
                        next_ends.push_back(static_cast<uint32_t>(next_points.size()));
                    else
                        next_points.resize(outer_begin);
                    rest.swap(inner);
                }
            }
            std::swap(points, next_points);
            std::swap(ends, next_ends);
            if (ends.empty())
                break;
        }
        // Each part as a fan, without the points a cut doubled where it
        // passed through a corner, and without triangles of no area, which
        // a renderer may take for parts of a rectangle.
        uint32_t begin = 0;
        for (const uint32_t end : ends) {
            const std::span<const card::Vertex> part(points.data() + begin, end - begin);
            begin = end;
            const auto first = static_cast<card::Index>(hole_vertices_.size());
            for (const card::Vertex& point : part)
                if (hole_vertices_.size() == first || point.x != hole_vertices_.back().x ||
                    point.y != hole_vertices_.back().y)
                    hole_vertices_.push_back(point);
            while (hole_vertices_.size() > first + 1U &&
                   hole_vertices_.back().x == hole_vertices_[first].x &&
                   hole_vertices_.back().y == hole_vertices_[first].y)
                hole_vertices_.pop_back();
            const auto count = static_cast<card::Index>(hole_vertices_.size() - first);
            for (card::Index k = 1; k + 1 < count; ++k) {
                const card::Vertex& a = hole_vertices_[first];
                const card::Vertex& b = hole_vertices_[first + k];
                const card::Vertex& c = hole_vertices_[first + k + 1];
                if ((b.x - a.x) * (c.y - a.y) == (b.y - a.y) * (c.x - a.x))
                    continue;
                hole_indices_.push_back(first);
                hole_indices_.push_back(static_cast<card::Index>(first + k));
                hole_indices_.push_back(static_cast<card::Index>(first + k + 1));
            }
        }
    }
    std::swap(vertices_, hole_vertices_);
    std::swap(indices_, hole_indices_);
}

void Emitter::append_body(const Canvas& surface, const Polygon& polygon, card::PageHandle page) {
    if (polygon.hole_count != 0)
        leave_out_holes(surface, polygon);
    const auto height_of = [&](const card::Vertex& vertex) {
        const float x = (vertex.x - surface.origin_x) / scale_;
        const float y = (vertex.y - surface.origin_y) / scale_;
        return polygon.height_origin + polygon.height_x * x + polygon.height_y * y;
    };
    // Each vertex's world height beside the frame's vertices, where they are
    // asked for: NaN for the vertices other draws appended before these.
    const auto record_heights = [&](std::span<const card::Vertex> vertices) {
        if (in_.vertex_heights == nullptr)
            return;
        auto& heights = *in_.vertex_heights;
        heights.resize(frame_.vertices.size(), std::numeric_limits<float>::quiet_NaN());
        for (const card::Vertex& vertex : vertices)
            heights.push_back(
                polygon.height_plane ? height_of(vertex) : std::numeric_limits<float>::quiet_NaN()
            );
    };
    if (polygon.water_geometry && in_.water_shimmer > 0) {
        renderer_water_shading::split(
            vertices_,
            indices_,
            wet_vertices_,
            wet_indices_,
            [&](const card::Vertex& vertex) { return in_.water_sea_level - height_of(vertex); },
            [&](card::Vertex& vertex, float depth) {
                const float x = (vertex.x - surface.origin_x) / scale_;
                const float y = (vertex.y - surface.origin_y) / scale_;
                renderer_water_shading::apply(
                    vertex,
                    x + camera_x_ / 65536.0F,
                    y + camera_z_ / 65536.0F + (in_.water_sea_level - depth) * 0.5F,
                    depth,
                    in_.water_seconds,
                    in_.water_shimmer,
                    scale_
                );
            }
        );
        record_heights(wet_vertices_);
        append(surface, page, card::Blend::alpha, wet_vertices_, wet_indices_);
    } else {
        record_heights(vertices_);
        append(surface, page, card::Blend::alpha, vertices_, indices_);
    }
}

void Emitter::emit_polygon(const Canvas& surface, const Polygon& polygon) {
    if (polygon.corner_count < 3)
        return;
    const std::span<const Corner> corners(
        impl_.corners.data() + polygon.first_corner, polygon.corner_count
    );
    if (winding(corners) <= 0) {
        ++counts_.culled;
        return;
    }
    PlacedFrame placed;
    if (polygon.texture != nullptr &&
        !place_texture(*polygon.texture, polygon.flat_page, polygon.bright, placed)) {
        ++counts_.culled;
        return;
    }
    if (polygon.cut) {
        emit_cut_polygon(surface, polygon, placed);
        return;
    }
    vertices_.clear();
    for (const Corner& corner : corners)
        vertices_.push_back(body_vertex(
            surface, polygon, placed, corner.x, corner.y, corner.u, corner.v, corner.colour
        ));
    indices_.clear();
    if (polygon.mesh != nullptr && polygon.primitive < polygon.mesh->primitives.size()) {
        const gw::MeshPrimitive& run = polygon.mesh->primitives[polygon.primitive];
        for (uint32_t k = run.first_index; k < run.first_index + run.index_count; ++k) {
            if (k >= polygon.mesh->indices.size())
                break;
            const uint32_t corner = polygon.mesh->indices[k] - run.first_vertex;
            if (corner >= polygon.corner_count)
                continue;
            indices_.push_back(corner);
        }
    } else {
        for (uint32_t k = 1; k + 1 < polygon.corner_count; ++k) {
            indices_.push_back(0);
            indices_.push_back(k);
            indices_.push_back(k + 1);
        }
    }
    if (indices_.size() < 3)
        return;
    indices_.resize(indices_.size() - indices_.size() % 3);
    // A textured quad is drawn as strips across its rows. Where a strip's
    // end meets another polygon's edge between that edge's corners, the
    // card may leave a pixel to neither, so a quad that draws every pixel
    // it covers opaque has its two triangles drawn beneath the strips.
    if (polygon.texture != nullptr && walk_strips(surface, corners)) {
        if (strip_indices_.empty())
            return;
        ++counts_.strip_quads;
        const auto opaque_corner = [](const Corner& corner) { return corner.colour.alpha >= 1.0F; };
        const bool opaque = std::ranges::all_of(corners, opaque_corner) &&
                            (polygon.flat_page || !has_keyed_texels(*polygon.texture));
        if (opaque)
            append_body(surface, polygon, placed.page);
        std::swap(vertices_, strip_vertices_);
        std::swap(indices_, strip_indices_);
    }
    append_body(surface, polygon, placed.page);
    ++counts_.polygons;
    if (polygon.underwater)
        lift_under_water(surface);
}

/// Emits the pieces a polygon keeps where polygons that clear lie over it
/// (cut_under_clearing), each a fan from its first corner; a polygon with
/// none left draws nothing.
///
/// @param surface the canvas the polygon is drawn on
/// @param polygon the cut polygon
/// @param placed where its frame lies on its page, for a textured polygon
void Emitter::emit_cut_polygon(
    const Canvas& surface, const Polygon& polygon, const PlacedFrame& placed
) {
    vertices_.clear();
    indices_.clear();
    const uint32_t end_piece = std::min(
        polygon.first_piece + polygon.piece_count, static_cast<uint32_t>(impl_.cut_pieces.size())
    );
    for (uint32_t p = polygon.first_piece; p < end_piece; ++p) {
        const CutPiece& piece = impl_.cut_pieces[p];
        const auto first = static_cast<card::Index>(vertices_.size());
        for (uint32_t k = 0; k < piece.point_count; ++k) {
            const CutPoint& point = impl_.cut_points[piece.first_point + k];
            vertices_.push_back(body_vertex(
                surface, polygon, placed, point.x, point.y, point.u, point.v, point.colour
            ));
        }
        for (uint32_t k = 1; k + 1 < piece.point_count; ++k) {
            indices_.push_back(first);
            indices_.push_back(first + k);
            indices_.push_back(first + k + 1);
        }
    }
    if (indices_.empty())
        return;
    append_body(surface, polygon, placed.page);
    ++counts_.polygons;
    if (polygon.underwater)
        lift_under_water(surface);
}

/// Makes the vertex of a point of a polygon's body: placed on the surface,
/// its colour halved where the polygon is under water, and its place on
/// the polygon's frame turned into a place on the frame's page.
///
/// @param surface the canvas the polygon is drawn on
/// @param polygon the polygon
/// @param placed where its frame lies on its page, for a textured polygon
/// @param x the point across, in frame pixels at zoom 1
/// @param y the point down
/// @param u where on the frame it lies across, 0 to 1
/// @param v where down
/// @param colour its colour
/// @return the vertex
card::Vertex Emitter::body_vertex(
    const Canvas& surface,
    const Polygon& polygon,
    const PlacedFrame& placed,
    double x,
    double y,
    double u,
    double v,
    const card::Colour& colour
) const {
    card::Vertex vertex;
    vertex.x = surface.origin_x + static_cast<float>(x) * scale_;
    vertex.y = surface.origin_y + static_cast<float>(y) * scale_;
    vertex.colour = colour;
    if (polygon.underwater) {
        vertex.colour.red *= half_colour;
        vertex.colour.green *= half_colour;
        vertex.colour.blue *= half_colour;
    }
    if (polygon.texture != nullptr) {
        const auto page = static_cast<float>(placed.page_size);
        vertex.u = (static_cast<float>(placed.rect.x) + texel_offset_ +
                    static_cast<float>(u) * static_cast<float>(placed.rect.width - 1)) /
                   page;
        vertex.v = (static_cast<float>(placed.rect.y) + texel_offset_ +
                    static_cast<float>(v) * static_cast<float>(placed.rect.height - 1)) /
                   page;
    }
    return vertex;
}

/// Adds the blue table's lift once over the halved body just appended,
/// whose vertices and indices vertices_ and indices_ still hold.
///
/// @param surface the canvas the body was drawn on
void Emitter::lift_under_water(const Canvas& surface) {
    if (!ensure_solid_page())
        return;
    const card::Colour lift{
        0.0F, 0.0F, level(oa::present::gamma_channel(underwater_blue_lift, impl_.gamma)), 1.0F
    };
    for (card::Vertex& vertex : vertices_) {
        vertex.colour = lift;
        vertex.u = texel_centre;
        vertex.v = texel_centre;
    }
    append(surface, impl_.solid_page, card::Blend::additive, vertices_, indices_);
}

void Emitter::emit_polygons(const Canvas& surface) {
    for (const Polygon& polygon : impl_.polygons)
        emit_polygon(surface, polygon);
}

void Emitter::emit_outline(const Canvas& surface) {
    if (impl_.outline_runs.empty())
        return;
    // Zoomed out, a run of map pixels spans less than a screen pixel and
    // could fall between pixel centres, leaving a nanoframe barely begun
    // with nothing on screen: each run is then drawn at least a screen
    // pixel across and down.
    const float least = scale_ < 1.0F ? 1.0F : 0.0F;
    for (const OutlineRun& run : impl_.outline_runs) {
        const float x0 = surface.origin_x + static_cast<float>(run.x) * scale_;
        const float y0 = surface.origin_y + static_cast<float>(run.y) * scale_;
        const float x1 =
            std::max(surface.origin_x + static_cast<float>(run.x + run.width) * scale_, x0 + least);
        const float y1 =
            std::max(surface.origin_y + static_cast<float>(run.y + 1) * scale_, y0 + least);
        vertices_.clear();
        for (const auto& [x, y] :
             {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x1, y1}, std::pair{x0, y1}}) {
            card::Vertex vertex;
            vertex.x = x;
            vertex.y = y;
            vertex.colour = run.colour;
            vertices_.push_back(vertex);
        }
        constexpr std::array<card::Index, 6> quad{0, 1, 2, 0, 2, 3};
        append(surface, {}, card::Blend::alpha, vertices_, quad);
    }
    impl_.outline_runs.clear();
}

void Emitter::emit_silhouettes(
    const Canvas& surface, card::Blend blend, const card::Colour& shade
) {
    if (!ensure_solid_page())
        return;
    for (const Polygon& polygon : impl_.polygons) {
        // The image's pixels of the key cast no shadow.
        if (polygon.corner_count < 3 || polygon.clears)
            continue;
        const std::span<const Corner> corners(
            impl_.corners.data() + polygon.first_corner, polygon.corner_count
        );
        if (winding(corners) <= 0)
            continue;
        vertices_.clear();
        for (const Corner& corner : corners) {
            card::Vertex vertex;
            vertex.x = surface.origin_x + static_cast<float>(corner.x) * scale_;
            vertex.y = surface.origin_y + static_cast<float>(corner.y) * scale_;
            vertex.colour = shade;
            vertex.u = texel_centre;
            vertex.v = texel_centre;
            vertices_.push_back(vertex);
        }
        indices_.clear();
        for (uint32_t k = 1; k + 1 < polygon.corner_count; ++k) {
            indices_.push_back(0);
            indices_.push_back(k);
            indices_.push_back(k + 1);
        }
        append(surface, impl_.solid_page, blend, vertices_, indices_);
        ++counts_.shadows;
    }
}

// ---------------------------------------------------------------------------
// The kinds

void Emitter::draw_model(const ModelDraw& drawn) {
    // A feature draws only over the map the view shows.
    if (drawn.on_map && !map_shown())
        return;
    const Canvas canvas = drawn.on_map ? on_map(battlefield()) : battlefield();
    draw::ModelRef model = drawn.model;
    if (drawn.stand_in >= 0 && static_cast<size_t>(drawn.stand_in) < in_.draws->stand_ins.size())
        model.unit = &in_.draws->stand_ins[static_cast<size_t>(drawn.stand_in)];
    if (model.instance == nullptr || model.prepared == nullptr || model.state == nullptr ||
        model.unit == nullptr || in_.world == nullptr)
        return;
    const draw::SupersampledUnitPlan& plan = drawn.plan;
    if (!plan.linked.drawn)
        return;
    const gw::ModelMesh* mesh = mesh_of(model);
    if (mesh == nullptr)
        return;
    const Unit& unit = *model.unit;
    const Game& game = in_.world->game;
    impl_.corners.clear();
    impl_.polygons.clear();
    impl_.cut_pieces.clear();
    impl_.cut_points.clear();
    impl_.holes.clear();
    impl_.hole_points.clear();
    impl_.outline_runs.clear();
    const int32_t dx = wrap_sub(unit.position.x, camera_x_);
    const int32_t dz = wrap_sub(unit.position.z, camera_z_);
    const int32_t image_x = hi(dx);
    const int32_t unit_y = hi(dz) - (hi(unit.position.y) >> 1);
    const int32_t base = vertex_depth_base(model);
    const bool from_image = plan.model.from_image && model.state->image.sprite.data != nullptr;
    const bool depth_image = from_image && model.state->image.sprite.aux != nullptr;
    const bool translucent =
        (unit.state_flags & draw::unit_state_cloaked) != 0 || game.debug_overlay != 0;
    const float body_alpha = translucent ? half_alpha : 1.0F;
    const bool unfinished = unit.build_remaining != 0.0F;
    const bool building = is_building(unit);
    const Shading image_shading = building && (in_.graphics_flags & draw::graphics_shading) != 0
                                      ? Shading::rows
                                      : Shading::none;
    const uint8_t team = in_.team_colors[unit.owner_index % team_colour_players];
    const Placement image_placement{false, image_x, unit_y, 0, 0, 0, 0, base};
    const auto flat_placement = [&](const Unit& placed, int32_t depth_base) {
        return Placement{
            true,
            0,
            0,
            0,
            wrap_sub(placed.position.x, camera_x_),
            wrap_sub(placed.position.z, camera_z_),
            placed.position.y,
            depth_base
        };
    };
    ++counts_.units;
    if (!from_image) {
        // No cached image: every visible piece flat, as draw_model draws it.
        add_unit_pieces(
            model,
            *mesh,
            flat_placement(unit, base),
            true,
            true,
            plan.linked.unlit,
            team,
            Shading::none,
            1.0F,
            true
        );
        emit_polygons(canvas);
        return;
    }
    if (!depth_image) {
        // The image's cached pieces, blended whole when translucent; the
        // moving pieces flat over it; then the carried units flat.
        add_unit_pieces(
            model, *mesh, image_placement, true, false, true, team, image_shading, body_alpha, false
        );
        cut_under_clearing(0, impl_.polygons.size());
        add_unit_pieces(
            model,
            *mesh,
            flat_placement(unit, base),
            false,
            true,
            plan.linked.unlit,
            team,
            Shading::none,
            1.0F,
            true
        );
        for (const draw::CarriedDraw& carried : plan.model.carried) {
            if (carried.model.instance == nullptr || carried.model.prepared == nullptr ||
                carried.model.unit == nullptr)
                continue;
            const gw::ModelMesh* carried_mesh = mesh_of(carried.model);
            if (carried_mesh == nullptr)
                continue;
            ++counts_.carried;
            add_unit_pieces(
                carried.model,
                *carried_mesh,
                flat_placement(*carried.model.unit, vertex_depth_base(carried.model)),
                true,
                true,
                plan.linked.unlit,
                in_.team_colors[carried.model.unit->owner_index % team_colour_players],
                Shading::none,
                1.0F,
                true
            );
        }
        emit_polygons(canvas);
        return;
    }
    // A depth image: the image's pieces, the moving pieces drawn again over
    // it and the carried units composed into it are one picture whose depth
    // plane the order of its polygons stands in for (order_as_depth_plane),
    // with holes in each polygon where it lies under one drawn before it
    // (cut_under_nearer).
    if (unfinished) {
        // An unfinished unit's image holds every piece; a mobile one draws
        // them again with the running frames, unless its moving pieces wait
        // for it to be built as a building's do.
        const bool framed = building || in_.moving_pieces_once_built;
        add_unit_pieces(
            model,
            *mesh,
            image_placement,
            true,
            true,
            framed,
            team,
            image_shading,
            body_alpha,
            false
        );
        if (framed)
            apply_nanoframe(bands_of(in_.build_pulse_tick, model), 0, model, image_placement);
    } else {
        add_unit_pieces(
            model, *mesh, image_placement, true, false, true, team, image_shading, body_alpha, false
        );
        add_unit_pieces(
            model,
            *mesh,
            image_placement,
            false,
            true,
            false,
            team,
            Shading::none,
            body_alpha,
            false
        );
    }
    for (const draw::CarriedDraw& carried : plan.model.carried) {
        if (carried.model.instance == nullptr || carried.model.prepared == nullptr ||
            carried.model.unit == nullptr)
            continue;
        const gw::ModelMesh* carried_mesh = mesh_of(carried.model);
        if (carried_mesh == nullptr)
            continue;
        ++counts_.carried;
        const Unit& child = *carried.model.unit;
        const int32_t cx = wrap_sub(child.position.x, unit.position.x);
        const int32_t cy = wrap_sub(child.position.y, unit.position.y);
        const int32_t cz = wrap_sub(child.position.z, unit.position.z);
        const Placement composed{
            false,
            image_x + hi(cx),
            unit_y + hi(cz) - (hi(cy) >> 1),
            hi(cy),
            0,
            0,
            0,
            vertex_depth_base(carried.model)
        };
        const Shading carried_shading =
            is_building(child) && (in_.graphics_flags & draw::graphics_shading) != 0
                ? Shading::rows
                : Shading::none;
        const size_t first_polygon = impl_.polygons.size();
        add_unit_pieces(
            carried.model,
            *carried_mesh,
            composed,
            true,
            true,
            true,
            in_.team_colors[child.owner_index % team_colour_players],
            carried_shading,
            body_alpha,
            false
        );
        apply_nanoframe(
            bands_of(in_.build_pulse_tick, carried.model), first_polygon, carried.model, composed
        );
    }
    const int32_t lift = static_cast<int32_t>(game.sea_level) - hi(unit.position.y);
    const bool own = (unit.flags & OA_UNIT_FLAG_VIEWPOINT_OWNED) != 0 ||
                     unit.owner_index == game.viewpoint_player;
    apply_water_and_digger(
        0, lift > 0 ? int32_t{static_cast<uint8_t>(lift + base)} : -1, own, is_digger(model)
    );
    cut_under_nearer(order_as_depth_plane(
        model.state, impl_.corners, impl_.polygons, impl_.plane_order, impl_.kept_plane_orders
    ));
    cut_under_clearing(0, impl_.polygons.size());
    emit_polygons(canvas);
    emit_outline(canvas);
}

namespace {

/// The frame a projectile's or fragment's primitive shows: the running
/// frame of an animated texture, else the fixed frame, the team colour
/// unread.
const Sprite* cursor_texture(const draw::PreparedPrimitive& primitive, uint8_t) {
    if ((primitive.flags & draw::primitive_animated) == 0)
        return primitive.frame != nullptr && primitive.frame->data != nullptr ? primitive.frame
                                                                              : nullptr;
    if (primitive.texture == nullptr || primitive.cursor.sequence == nullptr ||
        primitive.cursor.frame_index >= primitive.texture->frames.size())
        return nullptr;
    const Sprite* sprite = &primitive.texture->frames[primitive.cursor.frame_index];
    return sprite->data != nullptr ? sprite : nullptr;
}

/// The frame a debris piece's primitive shows: the running frame, the team
/// colour's for a team texture.
const Sprite* debris_texture(const draw::PreparedPrimitive& primitive, uint8_t team) {
    return draw::primitive_texture(primitive, false, team);
}

} // namespace

void Emitter::draw_projectile(const ProjectileDraw& shot) {
    if (shot.object == nullptr || shot.prepared == nullptr)
        return;
    ++counts_.projectiles;
    impl_.corners.clear();
    impl_.polygons.clear();
    const auto place_object = [&](const Object& object,
                                  const draw::PreparedObject& prepared,
                                  oa::sim::model_runtime::RotationWords rotation) {
        impl_.projected.clear();
        impl_.points.clear();
        for (const FixedVector3& v : object.vertices) {
            const FixedVector3 p = oa::sim::model_runtime::rotate_vector(
                {wrap_sub(0, v.x), v.y, wrap_sub(0, v.z)}, rotation
            );
            if (in_.water_shimmer > 0)
                impl_.points.push_back({0, wrap_add(p.y, shot.position.y), 0});
            impl_.projected.push_back(
                gw::pixel_of_model_point(
                    FixedVector3{
                        wrap_add(p.x, shot.position.x),
                        wrap_add(p.y, shot.position.y),
                        wrap_sub(p.z, shot.position.z)
                    },
                    0
                )
            );
        }
        add_flat_object(object, prepared, impl_.projected, cursor_texture, 0, impl_.points);
    };
    place_object(*shot.object, *shot.prepared, shot.rotation);
    if (shot.child != nullptr && shot.child_prepared != nullptr)
        place_object(*shot.child, *shot.child_prepared, shot.child_rotation);
    emit_polygons(battlefield());
}

void Emitter::draw_debris(const DebrisDraw& piece) {
    if (piece.object == nullptr || piece.prepared == nullptr)
        return;
    const int32_t dx = wrap_sub(piece.origin.x, camera_x_);
    const int32_t dz = wrap_sub(piece.origin.z, camera_z_);
    const int32_t x = hi(dx);
    const int32_t y = hi(dz) - (hi(piece.origin.y) >> 1);
    // Culled on its origin within the visible map rectangle, as today.
    const auto visible_w =
        static_cast<int32_t>(std::ceil(static_cast<float>(scissor_.width) / scale_));
    const auto visible_h =
        static_cast<int32_t>(std::ceil(static_cast<float>(scissor_.height) / scale_));
    if (x < 0 || x > visible_w - 1 || y < 0 || y > visible_h - 1)
        return;
    ++counts_.debris;
    impl_.corners.clear();
    impl_.polygons.clear();
    impl_.projected.clear();
    const Object& object = *piece.object;
    for (const FixedVector3& v : object.vertices) {
        const FixedVector3 p = oa::sim::model_runtime::rotate_vector(
            {wrap_sub(0, v.x), v.y, wrap_sub(0, v.z)}, piece.spin
        );
        // Each vertex is lifted by half of (vertex y + origin y), the
        // origin's world height rather than its screen row, as today.
        impl_.projected.push_back(
            gw::pixel_of_model_point(
                FixedVector3{wrap_add(p.x, dx), wrap_add(p.y, piece.origin.y), wrap_sub(p.z, dz)}, 0
            )
        );
    }
    add_flat_object(object, *piece.prepared, impl_.projected, debris_texture, piece.team);
    emit_polygons(battlefield());
}

void Emitter::draw_fragment(const FragmentDraw& fragment) {
    if (fragment.fragment == nullptr || fragment.primitive == nullptr)
        return;
    ++counts_.fragments;
    // The slab: eight points, six faces over the game's fragment corner
    // table, no selection primitive, every face carrying the look of the
    // primitive the fragment broke from.
    static const Object slab = [] {
        Object object;
        object.vertices.resize(fx::fragment_point_count);
        for (const auto& corners : fx::fragment_faces) {
            oa::formats::objects3d::Primitive face;
            face.vertex_indices.assign(std::begin(corners), std::end(corners));
            object.primitives.push_back(face);
        }
        return object;
    }();
    const fx::ShatterFragment& shattered = *fragment.fragment;
    draw::PreparedPrimitive look = *fragment.primitive;
    look.flags = shattered.look.flags;
    look.color = static_cast<uint8_t>(shattered.look.color);
    constexpr uint32_t shown_bits =
        draw::primitive_colored | draw::primitive_animated | draw::primitive_team;
    if ((look.flags & shown_bits) == draw::primitive_team &&
        (fragment.primitive->flags & draw::primitive_animated) != 0)
        look.frame = draw::primitive_texture(*fragment.primitive, false, shattered.look.team_color);
    draw::PreparedObject faces;
    faces.skips_first = false;
    faces.primitives.assign(fx::fragment_face_count, look);
    for (uint32_t i = 0; i < fx::fragment_face_count; ++i)
        faces.primitives[i].source_index = i;
    impl_.corners.clear();
    impl_.polygons.clear();
    impl_.projected.clear();
    for (uint32_t i = 0; i < fx::fragment_point_count; ++i) {
        const FixedVector3 p = oa::sim::model_runtime::rotate_vector(
            {shattered.points[i].x, shattered.points[i].y, shattered.points[i].z}, fragment.spin
        );
        impl_.projected.push_back(
            gw::pixel_of_model_point(
                FixedVector3{
                    wrap_add(p.x, fragment.position.x),
                    wrap_add(p.y, fragment.position.y),
                    wrap_sub(p.z, fragment.position.z)
                },
                0
            )
        );
    }
    add_flat_object(slab, faces, impl_.projected, cursor_texture, 0);
    emit_polygons(battlefield());
}

bool Emitter::aircraft_shadow(
    const Unit& unit,
    const draw::ModelState& state,
    const Canvas& surface,
    int32_t x,
    int32_t y,
    float altitude
) {
    const Sprite& image = state.image.sprite;
    constexpr uint32_t edge = 256;
    constexpr std::size_t capacity = 256;
    const uint32_t radius = renderer_aircraft_shadow::edge_radius(altitude);
    const uint32_t width = image.width, height = image.height;
    const uint32_t padding = radius * 2;
    if (width == 0 || height == 0 || width + padding * 2 > edge || height + padding * 2 > edge ||
        image.data == nullptr)
        return false;
    auto found = impl_.aircraft_shadows.find(unit.id);
    if (found == impl_.aircraft_shadows.end()) {
        if (impl_.aircraft_shadows.size() >= capacity) {
            // A full cache gives the page drawn longest ago, never one drawn
            // this frame, to this aircraft; it is drawn afresh.
            auto oldest = impl_.aircraft_shadows.end();
            for (auto entry = impl_.aircraft_shadows.begin(); entry != impl_.aircraft_shadows.end();
                 ++entry)
                if (entry->second.used_frame < impl_.aircraft_shadow_frame &&
                    (oldest == impl_.aircraft_shadows.end() ||
                     entry->second.used_frame < oldest->second.used_frame))
                    oldest = entry;
            if (oldest == impl_.aircraft_shadows.end())
                return false;
            auto node = impl_.aircraft_shadows.extract(oldest);
            node.key() = unit.id;
            node.mapped() = {.page = node.mapped().page};
            found = impl_.aircraft_shadows.insert(std::move(node)).position;
        } else {
            found = impl_.aircraft_shadows.try_emplace(unit.id).first;
        }
    }
    auto& cached = found->second;
    cached.used_frame = impl_.aircraft_shadow_frame;
    if (cached.page == card::PageHandle{} || cached.image_builds != state.image_builds ||
        cached.image_data != image.data || cached.width != width || cached.height != height ||
        cached.radius != radius) {
        std::vector<uint8_t> mask(static_cast<std::size_t>(width) * height);
        const auto* pixels = static_cast<const uint8_t*>(image.data);
        for (std::size_t i = 0; i < mask.size(); ++i)
            mask[i] = pixels[i] != draw::image_key;
        if (cached.page == card::PageHandle{})
            cached.page = executor_.create_page({edge, edge, 1});
        if (cached.page == card::PageHandle{})
            return false;
        const auto alpha = renderer_aircraft_shadow::soften(mask, width, height, radius);
        std::vector<uint8_t> rgba(edge * edge * card::texel_bytes);
        const uint32_t columns = width + padding * 2, rows = height + padding * 2;
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t col = 0; col < columns; ++col)
                rgba[(row * edge + col) * card::texel_bytes + 3] = alpha[row * columns + col];
        if (!executor_.update_page(cached.page, 0, nullptr, rgba.data(), edge * card::texel_bytes))
            return false;
        cached.width = width;
        cached.height = height;
        cached.radius = radius;
        cached.image_builds = state.image_builds;
        cached.image_data = image.data;
        ++counts_.page_uploads;
    }
    const float centre_x = static_cast<float>(x - image.origin_x) + width * 0.5F;
    const float centre_y = static_cast<float>(y - image.origin_y) + height * 0.5F;
    const float half_width = width * 0.5F + padding;
    const float half_height = height * 0.5F + padding;
    vertices_.clear();
    for (const auto& [sx, sy] : {std::pair{0, 0}, {1, 0}, {1, 1}, {0, 1}}) {
        card::Vertex vertex;
        vertex.x = surface.origin_x + (centre_x + (sx * 2 - 1) * half_width) * scale_;
        vertex.y = surface.origin_y + (centre_y + (sy * 2 - 1) * half_height) * scale_;
        vertex.u = static_cast<float>(sx * (width + padding * 2)) / edge;
        vertex.v = static_cast<float>(sy * (height + padding * 2)) / edge;
        vertex.colour = {1, 1, 1, shadow_alpha()};
        vertices_.push_back(vertex);
    }
    constexpr std::array<card::Index, 6> quad{0, 1, 2, 0, 2, 3};
    append(surface, cached.page, card::Blend::alpha, vertices_, quad);
    frame_.batches.back().sampling = card::Sampling::linear;
    ++counts_.shadows;
    return true;
}

void Emitter::shadow_of_model(
    const ModelDraw& drawn, const Canvas& surface, card::Blend blend, bool unfinished_pass
) {
    draw::ModelRef model = drawn.model;
    if (drawn.stand_in >= 0 && static_cast<size_t>(drawn.stand_in) < in_.draws->stand_ins.size())
        model.unit = &in_.draws->stand_ins[static_cast<size_t>(drawn.stand_in)];
    if (model.instance == nullptr || model.prepared == nullptr || model.state == nullptr ||
        model.unit == nullptr || in_.world == nullptr)
        return;
    const draw::SupersampledUnitPlan& plan = drawn.plan;
    const bool from_image = plan.model.from_image && model.state->image.sprite.data != nullptr;
    if (!plan.linked.drawn || !from_image)
        return;
    const uint32_t flags = def_flags(model);
    if ((in_.graphics_flags & draw::graphics_shadows) == 0 ||
        (flags & OA_UNIT_DEF_FLAG_NO_SHADOW) != 0)
        return;
    const gw::ModelMesh* mesh = mesh_of(model);
    if (mesh == nullptr)
        return;
    const Unit& unit = *model.unit;
    const Game& game = in_.world->game;
    const bool depth_image = model.state->image.sprite.aux != nullptr;
    const bool vehicle_shadow =
        (in_.graphics_flags & draw::graphics_vehicle_shadows) != 0 &&
        (flags & (OA_UNIT_DEF_FLAG_FLOATER | OA_UNIT_DEF_FLAG_CAN_HOVER)) == 0;
    const int32_t unit_height = hi(unit.position.y);
    const bool submerged_standin =
        unit.type_index == 0 && static_cast<int16_t>(unit_height) < game.sea_level;
    const bool digger = (flags & OA_UNIT_DEF_FLAG_DIGGER) != 0;
    const bool building = is_building(unit);
    // An unfinished building's shadow is taken out where its image lies,
    // which the shadow target alone allows; those shadows are drawn in a
    // pass before the others, so that taking one out leaves the others.
    const bool cut = blend == card::Blend::none && building && !digger && !submerged_standin &&
                     unit.build_remaining != 0.0F;
    if (cut != unfinished_pass)
        return;
    const int32_t dx = wrap_sub(unit.position.x, camera_x_);
    const int32_t dz = wrap_sub(unit.position.z, camera_z_);
    const int32_t shadow_x = hi(dx) + draw::shadow_offset_x;
    const int32_t shadow_y = hi(dz) - (plan.model.ground >> 1);
    // An aircraft's shadow lies where every unit's does, at the ground's
    // place, the bed's under water, as the processor lays it; only its
    // soft edge follows the height above the ground or water under it.
    const int32_t receiver = std::max<int32_t>(plan.model.ground, game.sea_level);
    const float altitude = static_cast<float>(unit_height - receiver);
    if ((flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0 && vehicle_shadow && altitude > 0 &&
        aircraft_shadow(unit, *model.state, surface, shadow_x, shadow_y, altitude))
        return;
    const int32_t base = vertex_depth_base(model);
    const uint8_t team = in_.team_colors[unit.owner_index % team_colour_players];
    impl_.corners.clear();
    impl_.polygons.clear();
    // The image's silhouette: the pieces the image holds, placed as the
    // image is placed, at the shadow's place.
    const auto image_silhouette = [&](int32_t clear_at_or_below) {
        const bool unfinished = unit.build_remaining != 0.0F;
        add_unit_pieces(
            model,
            *mesh,
            Placement{false, shadow_x, shadow_y, 0, 0, 0, 0, base},
            true,
            unfinished,
            true,
            team,
            Shading::none,
            1.0F,
            false
        );
        if (clear_at_or_below >= 0)
            for (Polygon& polygon : impl_.polygons)
                if (static_cast<uint8_t>(polygon.depth) <= clear_at_or_below)
                    polygon.corner_count = 0;
    };
    // The building's silhouette: the cached pieces sheared by a quarter of
    // their height, which leans the shadow away from the light.
    const auto building_silhouette = [&]() {
        if (submerged_standin)
            return;
        const auto pieces = model.instance->pieces();
        for (size_t n = pieces.size(); n != 0; --n) {
            const PieceState& state = pieces[n - 1];
            if ((state.flags & piece_visible) == 0 || (state.flags & piece_cached) == 0 ||
                state.object_index >= mesh->pieces.size())
                continue;
            const gw::MeshPiece& piece = mesh->pieces[state.object_index];
            const auto& points = state.transformed_vertices;
            for (uint32_t p = piece.first_primitive;
                 p < piece.first_primitive + piece.primitive_count && p < mesh->primitives.size();
                 ++p) {
                const gw::MeshPrimitive& run = mesh->primitives[p];
                if (run.corner_count < 3)
                    continue;
                Polygon polygon;
                polygon.first_corner = static_cast<uint32_t>(impl_.corners.size());
                polygon.corner_count = run.corner_count;
                bool valid = true;
                for (uint32_t k = 0; k < run.corner_count; ++k) {
                    const uint32_t vertex_index = run.first_vertex + k;
                    if (vertex_index >= mesh->source_vertex.size() ||
                        mesh->source_vertex[vertex_index] >= points.size()) {
                        valid = false;
                        break;
                    }
                    const FixedVector3& v = points[mesh->source_vertex[vertex_index]];
                    const int32_t lean = hi(v.y) >> 2;
                    Corner corner;
                    corner.x = hi(v.x) + lean + shadow_x;
                    corner.y = hi(wrap_sub(0, v.z)) - lean + shadow_y;
                    impl_.corners.push_back(corner);
                }
                if (!valid) {
                    impl_.corners.resize(polygon.first_corner);
                    continue;
                }
                impl_.polygons.push_back(polygon);
            }
        }
    };
    if (!depth_image) {
        if (building && !digger)
            building_silhouette();
        else if (vehicle_shadow)
            image_silhouette(-1);
    } else if (digger) {
        image_silhouette(static_cast<uint8_t>(base));
    } else if (building) {
        building_silhouette();
    } else if (vehicle_shadow) {
        const int32_t lift = static_cast<int32_t>(game.sea_level) - unit_height;
        image_silhouette(lift > 0 ? int32_t{static_cast<uint8_t>(base + lift)} : -1);
    }
    emit_silhouettes(surface, blend, {0.0F, 0.0F, 0.0F, shadow_alpha()});
    // The processor cuts a building's shadow by the mask of its whole
    // image, so that the shadow lies only past the building; over an
    // unfinished building, whose image the bands clear, the ground shows
    // there unshaded. The image's polygons at the shadow's place, moved
    // back by the shadow's offset, clear the target again.
    if (cut) {
        impl_.corners.clear();
        impl_.polygons.clear();
        add_unit_pieces(
            model,
            *mesh,
            Placement{false, shadow_x - draw::shadow_offset_x, shadow_y, 0, 0, 0, 0, base},
            true,
            true,
            true,
            team,
            Shading::none,
            1.0F,
            false
        );
        emit_silhouettes(surface, card::Blend::none, {0.0F, 0.0F, 0.0F, 0.0F});
    }
}

void Emitter::shadow_of_projectile(const ProjectileDraw& shot, const Canvas& surface) {
    if (!shot.shadow || in_.projectile_shadow == nullptr || in_.projectile_shadow->width == 0 ||
        in_.projectile_shadow->height == 0)
        return;
    PlacedFrame placed;
    if (!place_frame(
            pages_,
            impl_.slots,
            projectile_shadow_number * variants,
            nullptr,
            false,
            in_.projectile_shadow,
            placed
        ))
        return;
    const oa::formats::gaf::RenderedFrame& sprite = *in_.projectile_shadow;
    const float left = surface.origin_x + static_cast<float>(shot.x - sprite.origin_x) * scale_;
    const float top =
        surface.origin_y + static_cast<float>(shot.shadow_y - sprite.origin_y) * scale_;
    const float width = static_cast<float>(sprite.width) * scale_;
    const float height = static_cast<float>(sprite.height) * scale_;
    const auto page = static_cast<float>(placed.page_size);
    const float u0 = static_cast<float>(placed.rect.x) / page;
    const float v0 = static_cast<float>(placed.rect.y) / page;
    const float u1 = static_cast<float>(placed.rect.x + placed.rect.width) / page;
    const float v1 = static_cast<float>(placed.rect.y + placed.rect.height) / page;
    const card::Colour shade{0.0F, 0.0F, 0.0F, shadow_alpha()};
    vertices_.clear();
    for (const auto& [x, y, u, v] :
         {std::tuple{left, top, u0, v0},
          std::tuple{left + width, top, u1, v0},
          std::tuple{left + width, top + height, u1, v1},
          std::tuple{left, top + height, u0, v1}}) {
        card::Vertex vertex;
        vertex.x = x;
        vertex.y = y;
        vertex.u = u;
        vertex.v = v;
        vertex.colour = shade;
        vertices_.push_back(vertex);
    }
    constexpr std::array<card::Index, 6> quad{0, 1, 2, 0, 2, 3};
    append(surface, placed.page, card::Blend::alpha, vertices_, quad);
    ++counts_.shadows;
}

void Emitter::game_shadow_of_model(const ModelDraw& drawn) {
    if (in_.draws == nullptr || !shadows_drawn(*in_.draws) || in_.world == nullptr ||
        (drawn.on_map && !map_shown()))
        return;
    draw::ModelRef model = drawn.model;
    if (drawn.stand_in >= 0 && static_cast<size_t>(drawn.stand_in) < in_.draws->stand_ins.size())
        model.unit = &in_.draws->stand_ins[static_cast<size_t>(drawn.stand_in)];
    if (model.instance == nullptr || model.state == nullptr || model.unit == nullptr ||
        !drawn.plan.linked.drawn || !drawn.plan.model.from_image)
        return;
    draw::GroundShadow& shadow = impl_.game_shadow;
    if (!draw::ground_shadow(
            in_.graphics_flags,
            in_.world->game,
            model,
            camera_x_,
            camera_z_,
            drawn.plan.model.ground,
            shadow
        ) ||
        shadow.width > UINT16_MAX || shadow.height > UINT16_MAX)
        return;
    // The silhouette as a frame of colour 0, which the pages hold in the
    // palette's colour 0 as the processor's alpha table mixes it, numbered
    // by a digest of its shape so that a frame is placed once for every
    // unit that casts it.
    uint64_t digest = shape_digest_start;
    const auto fold = [&digest](uint64_t value) {
        digest = (digest ^ value) * shape_digest_multiplier;
    };
    fold(static_cast<uint64_t>(shadow.width));
    fold(static_cast<uint64_t>(shadow.height));
    bool any = false;
    for (const uint8_t covered : shadow.coverage) {
        fold(covered);
        any = any || covered != 0;
    }
    if (!any)
        return;
    oa::formats::gaf::RenderedFrame& frame = impl_.game_shadow_frame;
    frame.width = static_cast<uint16_t>(shadow.width);
    frame.height = static_cast<uint16_t>(shadow.height);
    frame.origin_x = 0;
    frame.origin_y = 0;
    frame.transparency_index = 0;
    frame.pixels.assign(shadow.coverage.size(), 0);
    // The silhouette's bytes go to the frame, and the frame's last ones back
    // to the scratch the next shadow overwrites.
    frame.coverage.swap(shadow.coverage);
    PlacedFrame placed;
    if (!place_frame(
            pages_,
            impl_.slots,
            game_shadow_number_mark | (digest >> 1),
            nullptr,
            false,
            &frame,
            placed
        ))
        return;
    const Canvas surface = drawn.on_map ? on_map(battlefield()) : battlefield();
    const float left = surface.origin_x + static_cast<float>(shadow.left) * scale_;
    const float top = surface.origin_y + static_cast<float>(shadow.top) * scale_;
    const float width = static_cast<float>(shadow.width) * scale_;
    const float height = static_cast<float>(shadow.height) * scale_;
    const auto page = static_cast<float>(placed.page_size);
    const float u0 = static_cast<float>(placed.rect.x) / page;
    const float v0 = static_cast<float>(placed.rect.y) / page;
    const float u1 = static_cast<float>(placed.rect.x + placed.rect.width) / page;
    const float v1 = static_cast<float>(placed.rect.y + placed.rect.height) / page;
    // Under the premultiplied blend a colour and alpha of the shadows'
    // alpha mix that much of colour 0 into what is under it, as a
    // feature's translucent shadow frame is drawn.
    const float alpha = shadow_alpha();
    const card::Colour shade{alpha, alpha, alpha, alpha};
    vertices_.clear();
    for (const auto& [x, y, u, v] :
         {std::tuple{left, top, u0, v0},
          std::tuple{left + width, top, u1, v0},
          std::tuple{left + width, top + height, u1, v1},
          std::tuple{left, top + height, u0, v1}}) {
        card::Vertex vertex;
        vertex.x = x;
        vertex.y = y;
        vertex.u = u;
        vertex.v = v;
        vertex.colour = shade;
        vertices_.push_back(vertex);
    }
    constexpr std::array<card::Index, 6> quad{0, 1, 2, 0, 2, 3};
    append(
        surface,
        placed.page,
        card::Blend::alpha_premultiplied,
        vertices_,
        quad,
        game_shadow_sampling(view_.zoom)
    );
    ++counts_.shadows;
}

void Emitter::shadows(bool& through_target) {
    through_target = false;
    // A frame drawn too far out for shadows draws none, and neither clears
    // nor composes the shadow target.
    if (in_.draws == nullptr || !shadows_drawn(*in_.draws))
        return;
    const auto width = static_cast<uint32_t>(std::max(0, scissor_.width));
    const auto height = static_cast<uint32_t>(std::max(0, scissor_.height));
    if (width == 0 || height == 0)
        return;
    if (impl_.shadow_target != card::TargetHandle{} &&
        (impl_.shadow_width != width || impl_.shadow_height != height ||
         !executor_.target_alive(impl_.shadow_target))) {
        executor_.destroy_target(impl_.shadow_target);
        impl_.shadow_target = {};
    }
    if (impl_.shadow_target == card::TargetHandle{}) {
        impl_.shadow_target = executor_.create_target(width, height, 1);
        if (impl_.shadow_target == card::TargetHandle{})
            error_ = "model stage: cannot make the shadow target: " + executor_.error();
        else
            ++counts_.target_creations;
        impl_.shadow_width = width;
        impl_.shadow_height = height;
    }
    through_target = impl_.shadow_target != card::TargetHandle{};
    const Canvas surface = through_target ? shadow_surface() : battlefield();
    // Into the target, a silhouette replaces what is there, so overlapping
    // silhouettes darken once; straight into the battlefield each darkens
    // by itself.
    const card::Blend blend = through_target ? card::Blend::none : card::Blend::alpha;
    if (through_target) {
        card::Batch clear;
        clear.operation = card::Operation::clear;
        clear.target = impl_.shadow_target;
        clear.colour = {0.0F, 0.0F, 0.0F, 0.0F};
        frame_.batches.push_back(clear);
    }
    // The shadow sprites first, blended, then the silhouettes over them.
    // With New unit shadows off, each shot casts its own with its draw
    // (emit), as each model does.
    for (const WorldDraw& entry : in_.draws->draws)
        if (in_.new_unit_shadows && entry.kind == WorldDrawKind::projectile &&
            entry.index < in_.draws->projectiles.size())
            shadow_of_projectile(in_.draws->projectiles[entry.index], surface);
    // A feature's shadow falls only over the map the view shows. With New
    // unit shadows off, each model casts its own with its draw instead
    // (game_shadow_of_model).
    for (const bool unfinished_pass : {true, false})
        for (const WorldDraw& entry : in_.draws->draws) {
            if (!in_.new_unit_shadows || entry.kind != WorldDrawKind::model ||
                entry.index >= in_.draws->models.size())
                continue;
            const ModelDraw& drawn = in_.draws->models[entry.index];
            if (drawn.on_map && !map_shown())
                continue;
            shadow_of_model(
                drawn, drawn.on_map ? on_map(surface) : surface, blend, unfinished_pass
            );
        }
    if (!through_target)
        return;
    card::Batch compose;
    compose.operation = card::Operation::resolve;
    compose.target = view_.target;
    compose.source = impl_.shadow_target;
    compose.destination = scissor_;
    compose.blend = card::Blend::alpha_premultiplied;
    compose.scissored = true;
    compose.scissor = scissor_;
    frame_.batches.push_back(compose);
}

void Emitter::emit(const WorldDraw& entry) {
    if (in_.draws == nullptr)
        return;
    const WorldDrawList& list = *in_.draws;
    switch (entry.kind) {
    case WorldDrawKind::model:
        if (entry.index < list.models.size()) {
            if (!in_.new_unit_shadows)
                game_shadow_of_model(list.models[entry.index]);
            draw_model(list.models[entry.index]);
        }
        break;
    case WorldDrawKind::projectile:
        if (entry.index < list.projectiles.size()) {
            // A shot's shadow sprite, as the game draws it, just before the
            // shot, over what the list drew before it.
            if (!in_.new_unit_shadows && shadows_drawn(list))
                shadow_of_projectile(list.projectiles[entry.index], battlefield());
            draw_projectile(list.projectiles[entry.index]);
        }
        break;
    case WorldDrawKind::debris:
        if (entry.index < list.debris.size())
            draw_debris(list.debris[entry.index]);
        break;
    case WorldDrawKind::fragment:
        if (entry.index < list.fragments.size())
            draw_fragment(list.fragments[entry.index]);
        break;
    case WorldDrawKind::commit:
    case WorldDrawKind::commit_always:
    case WorldDrawKind::pixel_square:
    case WorldDrawKind::sprite:
    case WorldDrawKind::blended_sprite:
    case WorldDrawKind::lit_sprite:
    case WorldDrawKind::line:
    case WorldDrawKind::selection_line:
    case WorldDrawKind::lens:
        break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// ModelStage

float pixels_per_map_pixel(const SceneView& view) noexcept {
    return view.zoom * view.scale;
}

ModelStage::ModelStage()
    : impl_(std::make_unique<Impl>()),
      pages_(
          gw::Limits{
              gw::default_page_size, gw::max_page_size, page_memory_limit, largest_page_memory_limit
          }
      ),
      bright_pages_(
          gw::Limits{
              gw::default_page_size, gw::max_page_size, page_memory_limit, largest_page_memory_limit
          }
      ) {
}

ModelStage::~ModelStage() = default;

void ModelStage::set_palette(const Palette& palette, float gamma) {
    if (impl_->has_palette && gamma == impl_->gamma &&
        std::memcmp(&palette, &impl_->palette, sizeof(Palette)) == 0)
        return;
    impl_->palette = palette;
    impl_->gamma = gamma;
    impl_->has_palette = true;
    pages_.set_palette(palette, gamma);
    bright_pages_.set_palette(doubled(palette), gamma);
    impl_->forget_contents();
}

void ModelStage::close(card::Executor& executor) noexcept {
    free_pages(executor, impl_->slots);
    free_pages(executor, impl_->bright_slots);
    if (impl_->solid_page != card::PageHandle{})
        executor.destroy_page(impl_->solid_page);
    impl_->solid_page = {};
    for (auto& [id, shadow] : impl_->aircraft_shadows) {
        (void)id;
        if (shadow.page != card::PageHandle{})
            executor.destroy_page(shadow.page);
    }
    impl_->aircraft_shadows.clear();
    if (impl_->shadow_target != card::TargetHandle{})
        executor.destroy_target(impl_->shadow_target);
    impl_->shadow_target = {};
    impl_->shadow_width = 0;
    impl_->shadow_height = 0;
    pages_.clear();
    bright_pages_.clear();
    pages_.reset_memory_limit();
    bright_pages_.reset_memory_limit();
    impl_->forget_contents();
    impl_->kept_plane_orders.clear();
}

void ModelStage::begin_frame() noexcept {
    pages_.begin_frame();
    bright_pages_.begin_frame();
    ++impl_->aircraft_shadow_frame;
}

void ModelStage::set_growth_hooks(const gw::GrowthHooks& hooks) noexcept {
    pages_.set_growth_hooks(hooks);
    bright_pages_.set_growth_hooks(hooks);
}

void ModelStage::emit_shadows(
    const ModelFrameInputs& inputs,
    const SceneView& view,
    card::Executor& executor,
    card::CardFrame& frame
) {
    Emitter emitter(*impl_, pages_, bright_pages_, counts_, error_, inputs, view, executor, frame);
    emitter.shadows(shadows_through_target_);
}

void ModelStage::emit_draw(
    const ModelFrameInputs& inputs,
    const SceneView& view,
    const WorldDraw& entry,
    card::Executor& executor,
    card::CardFrame& frame
) {
    Emitter emitter(*impl_, pages_, bright_pages_, counts_, error_, inputs, view, executor, frame);
    emitter.emit(entry);
}

void ModelStage::emit_frame(
    const ModelFrameInputs& inputs,
    const SceneView& view,
    card::Executor& executor,
    card::CardFrame& frame
) {
    Emitter emitter(*impl_, pages_, bright_pages_, counts_, error_, inputs, view, executor, frame);
    emitter.shadows(shadows_through_target_);
    if (inputs.draws == nullptr)
        return;
    for (const WorldDraw& entry : inputs.draws->draws)
        emitter.emit(entry);
}

bool ModelStage::upload(card::Executor& executor) {
    const bool pages = upload_pages(executor, pages_, impl_->slots, counts_, error_);
    const bool bright = upload_pages(executor, bright_pages_, impl_->bright_slots, counts_, error_);
    return pages && bright;
}

std::size_t ModelStage::mesh_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& [model, entry] : impl_->meshes)
        if (!entry.refused)
            bytes += gw::mesh_bytes(entry.mesh);
    return bytes;
}

std::size_t ModelStage::mesh_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [model, entry] : impl_->meshes)
        if (!entry.refused)
            ++count;
    return count;
}

// ---------------------------------------------------------------------------
// Checks

double ModelRasterComparison::far_coverage_share() const noexcept {
    return drawn == 0 ? 0.0 : static_cast<double>(far_coverage) / static_cast<double>(drawn);
}

double ModelRasterComparison::far_share() const noexcept {
    const std::size_t both = same + phase + edge_colour + far;
    return both == 0 ? 0.0 : static_cast<double>(far) / static_cast<double>(both);
}

ModelRasterComparison compare_model_rasters(
    const uint8_t* card_rgba,
    const uint8_t* processor_rgb,
    uint32_t width,
    uint32_t height,
    const std::array<uint8_t, 3>& key
) {
    ModelRasterComparison result;
    const auto card_at = [&](uint32_t x, uint32_t y) {
        return card_rgba + (std::size_t{y} * width + x) * 4U;
    };
    const auto processor_at = [&](uint32_t x, uint32_t y) {
        return processor_rgb + (std::size_t{y} * width + x) * 3U;
    };
    const auto processor_drawn = [&](uint32_t x, uint32_t y) {
        const uint8_t* pixel = processor_at(x, y);
        return pixel[0] != key[0] || pixel[1] != key[1] || pixel[2] != key[2];
    };
    const auto close = [](const uint8_t* a, const uint8_t* b) {
        for (std::size_t channel = 0; channel < 3; ++channel)
            if (std::abs(int{a[channel]} - int{b[channel]}) > model_raster_tolerance)
                return false;
        return true;
    };
    // Whether a picture draws a colour, or anything, within one pixel of a
    // point.
    const auto processor_near = [&](uint32_t x, uint32_t y, const uint8_t* colour) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = static_cast<int>(x) + dx;
                const int ny = static_cast<int>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                    ny >= static_cast<int>(height))
                    continue;
                const auto ux = static_cast<uint32_t>(nx);
                const auto uy = static_cast<uint32_t>(ny);
                if (!processor_drawn(ux, uy))
                    continue;
                if (colour == nullptr || close(processor_at(ux, uy), colour))
                    return true;
            }
        return false;
    };
    // Whether a picture draws every pixel within one pixel of a point: a
    // point that is not within a pixel of its edge.
    const auto processor_near_everywhere = [&](uint32_t x, uint32_t y) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = static_cast<int>(x) + dx;
                const int ny = static_cast<int>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                    ny >= static_cast<int>(height))
                    continue;
                if (!processor_drawn(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny)))
                    return false;
            }
        return true;
    };
    const auto card_near_everywhere = [&](uint32_t x, uint32_t y) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = static_cast<int>(x) + dx;
                const int ny = static_cast<int>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                    ny >= static_cast<int>(height))
                    continue;
                if (card_at(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny))[3] == 0)
                    return false;
            }
        return true;
    };
    const auto card_near = [&](uint32_t x, uint32_t y, const uint8_t* colour) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = static_cast<int>(x) + dx;
                const int ny = static_cast<int>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                    ny >= static_cast<int>(height))
                    continue;
                const uint8_t* pixel =
                    card_at(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny));
                if (pixel[3] == 0)
                    continue;
                if (colour == nullptr || (pixel[3] == 255 && close(pixel, colour)))
                    return true;
            }
        return false;
    };
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* card = card_at(x, y);
            const bool card_drawn = card[3] != 0;
            const bool drawn = processor_drawn(x, y);
            if (!card_drawn && !drawn)
                continue;
            ++result.drawn;
            if (card_drawn && drawn) {
                if (card[3] != 255) {
                    ++result.shadowed;
                    continue;
                }
                const uint8_t* processor = processor_at(x, y);
                if (close(card, processor))
                    ++result.same;
                else if (processor_near(x, y, card) && card_near(x, y, processor))
                    ++result.phase;
                else if (!processor_near_everywhere(x, y) || !card_near_everywhere(x, y))
                    ++result.edge_colour;
                else
                    ++result.far;
                continue;
            }
            const bool near = card_drawn ? processor_near(x, y, nullptr) : card_near(x, y, nullptr);
            if (near)
                ++result.edge_coverage;
            else
                ++result.far_coverage;
        }
    }
    return result;
}

bool within_bounds(
    const ModelRasterComparison& comparison, const ModelRasterBounds& bounds
) noexcept {
    if (comparison.drawn == 0)
        return false;
    const bool coverage = comparison.far_coverage <= bounds.far_coverage_pixels ||
                          comparison.far_coverage_share() <= bounds.far_coverage_share;
    return coverage && comparison.far_share() <= bounds.far_share;
}

} // namespace oa::app::full
