// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_live_scene.hpp"
#include "bench_timers.hpp"
#include "renderer_live_camera.hpp"
#include "renderer_effect_sources.hpp"
#include "renderer_aircraft_shadow.hpp"
#include "renderer_water_appearance.hpp"
#include "renderer_water_surface.hpp"

#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/visibility_state.hpp"
#include "oa/present/rle.hpp"
#include "oa/present/gpu_world/model_meshes.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <exception>
#include <cstring>
#include <limits>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace oa::app::renderer_live_scene {
namespace {
using renderer_live_gpu::Vertex;
namespace model = present::model;
constexpr uint32_t channels = 4;
constexpr int32_t cell_edge = formats::tnt::layout::attribute_cell_pixels;
constexpr int32_t tile_edge = formats::tnt::layout::tile_edge_pixels;
constexpr float fixed_scale = 65536.0F;
constexpr float height_lift = 0.5F;
/// Ceiling of the greatest TNT height projected into painted map rows.
constexpr int32_t maximum_terrain_row_shift = 128;
constexpr uint64_t hash_start = 14695981039346656037ULL;
/// The multipliers and addend of the digest's rounds, odd numbers with
/// their bits well spread (those of XXH64).
constexpr uint64_t digest_prime_1 = 0x9E3779B185EBCA87ULL;
constexpr uint64_t digest_prime_2 = 0xC2B2AE3D27D4EB4FULL;
constexpr uint64_t digest_prime_4 = 0x85EBCA77C2B2AE63ULL;
constexpr uint64_t digest_prime_5 = 0x27D4EB2F165667C5ULL;
constexpr std::size_t maximum_vertices = renderer_live_gpu::largest_vertex_count;
constexpr uint32_t maximum_atlas_edge = 4096;
/// A terrain material's square in the atlas at full detail: the tile's 32
/// pixels in its own palette and in the grey one side by side, each with its
/// smaller levels under it.
constexpr uint32_t full_terrain_square = 2 * tile_edge;
/// The smallest square a frame packs a terrain tile into when its terrain
/// needs more room than the atlas has: the tile at 4 by 4 pixels.
constexpr uint32_t smallest_terrain_square = 8;
/// The bits of all four quarters of a split terrain square
/// (Scene::split_terrain_squares): the quarter at its corner's is the
/// lowest, then the one beside it, then the two under them.
constexpr uint8_t all_quarters = 0b1111;
/// The room the terrain leaves the other materials at least, and in
/// proportion to what they took when last measured: they are packed after
/// it, and a battle brings more into view.
constexpr uint64_t least_material_room = uint64_t{maximum_atlas_edge} * maximum_atlas_edge / 16;

/// The share of the atlas's rows a moving camera's terrain leaves the
/// other materials while it can make room of its own (MovingRoom): they
/// keep coming as the battle goes on, and the rows are theirs alone.
constexpr uint32_t moving_material_row_share = 4;
/// A moving camera's ground packed in the frame that takes it that finds
/// the atlas pressed, or that takes a ground that could not be packed
/// ahead, leaves at least this share of its tiles' count in spare squares
/// of the size it packs, for the grounds packed ahead of it, which cannot
/// take the kept ground's squares: about the tiles that come into view from
/// one ground to the next.
constexpr std::size_t ahead_room_share = 8;

/// The rows kept for the frame's own materials (Scene::frame_rows) are a
/// whole number of these.
constexpr uint32_t frame_row_step = 16;
/// The most height cells a side of a square of ground drawn as one, far
/// out over a large map.
constexpr int32_t coarsest_terrain_step = 8;
/// The frames a camera that has moved stands still for before it counts as
/// settled (Inputs::tiles_hold_while_moving): a pause in a drag is no stop.
constexpr uint32_t camera_settle_frames = 10;

/// A frame that needs more room than the renderer has: a camera that drew
/// can still draw.
class OutOfRoom : public std::runtime_error {
  public:

    using std::runtime_error::runtime_error;
};

/// Signals a full atlas so the next attempt can discard old frame materials.
class AtlasFull : public OutOfRoom {
  public:

    AtlasFull() : OutOfRoom("experimental material atlas is full") {}
};

/// A ground packed ahead of the frame that takes it that would have to
/// give back a square the kept ground draws: it is packed in that frame.
class AheadFull : public std::exception {
  public:

    [[nodiscard]] const char* what() const noexcept override {
        return "experimental ground drawn ahead needs the kept ground's room";
    }
};

/// More triangles than the frame's geometry budget.
class GeometryFull : public OutOfRoom {
  public:

    GeometryFull() : OutOfRoom("experimental scene exceeds its geometry budget") {}
};

/// Separates undiscovered ground from mapped ground outside current sight.
enum class Fog : uint8_t { seen, grey, black };

/// Reads mapping before line of sight, so unexplored cells stay black.
Fog fog_at(const full::SightView& sight, int32_t x, int32_t z) {
    constexpr int32_t fog_edge = present::world_renderer::fog_cell_pixels;
    const int32_t column = x / fog_edge;
    const int32_t row = z / fog_edge;
    if (x < 0 || z < 0 || column >= sight.width || row >= sight.height)
        return sight.mapping ? Fog::black : Fog::seen;
    const std::size_t index = static_cast<std::size_t>(row) * sight.width + column;
    if (sight.mapping &&
        (index >= sight.player_bits.size() || (sight.player_bits[index] & sight.mapped_bits) == 0))
        return Fog::black;
    const present::world_renderer::FogOptions options{
        sight.line_of_sight, sight.mapping, sight.shared_bits
    };
    const bool visible = present::world_renderer::cell_shown_in_sight(
        index < sight.coverage.size() ? sight.coverage[index] : uint16_t{0},
        index < sight.player_bits.size() ? sight.player_bits[index] : uint16_t{0},
        static_cast<uint16_t>(sight.mapped_bits & ~sight.shared_bits),
        options
    );
    return present::world_renderer::view_greys(options) && !visible ? Fog::grey : Fog::seen;
}

/// What the fog shows of each sight cell, as fog_at reads it, held for a
/// frame's ground: every point of it reads a dozen cells.
struct FogCells {
    std::span<const uint8_t> cells{}; ///< a Fog for each sight cell, row by row
    int32_t width{};                  ///< cells across
    int32_t height{};                 ///< cells down
    bool mapping{};                   ///< ground beyond the cells is never mapped
    bool dithered{};

    /// @param x a painted map column
    /// @param z a painted map row
    /// @return the fog there, as fog_at gives it
    [[nodiscard]] Fog at(int32_t x, int32_t z) const {
        constexpr int32_t fog_edge = present::world_renderer::fog_cell_pixels;
        const int32_t column = x / fog_edge;
        const int32_t row = z / fog_edge;
        if (x < 0 || z < 0 || column >= width || row >= height)
            return mapping ? Fog::black : Fog::seen;
        return static_cast<Fog>(cells[static_cast<std::size_t>(row) * width + column]);
    }
};

/// Interpolates fog inside known ground and fades to black before unknown
/// cells.
///
/// @param fog_at_point the fog at a painted map point, as fog_at gives it
/// @param width the sight's cells across
/// @param height its cells down
/// @param dithered the fog dithers ground out of sight
/// @param x a painted map column
/// @param y a painted map row
/// @param unseen the brightness of ground out of sight; below 0, the fog's
/// @return the brightness, 0 for unknown ground to 1 in sight
template <typename FogAt>
float fog_brightness(
    const FogAt& fog_at_point,
    int32_t width,
    int32_t height,
    bool dithered,
    float x,
    float y,
    float unseen
) {
    constexpr int32_t fog_edge = present::world_renderer::fog_cell_pixels;
    constexpr float edge = fog_edge;
    const float unseen_brightness = unseen >= 0 ? unseen : dithered ? 0.5F : 0.35F;
    constexpr float last_cell_inset = 0.001F;
    if (width > 0 && height > 0) {
        x = std::clamp(x, 0.0F, width * edge - last_cell_inset);
        y = std::clamp(y, 0.0F, height * edge - last_cell_inset);
    }
    if (fog_at_point(static_cast<int32_t>(std::floor(x)), static_cast<int32_t>(std::floor(y))) ==
        Fog::black)
        return 0;
    const auto sample = [&](int32_t column, int32_t row) {
        if (width <= 0 || height <= 0)
            return 1.0F;
        column = std::clamp(column, 0, width - 1);
        row = std::clamp(row, 0, height - 1);
        const auto fog = fog_at_point(
            static_cast<int32_t>((column + 0.5F) * edge), static_cast<int32_t>((row + 0.5F) * edge)
        );
        return fog == Fog::black ? 0.0F : fog == Fog::grey ? unseen_brightness : 1.0F;
    };
    const float grid_x = x / edge - 0.5F;
    const float grid_y = y / edge - 0.5F;
    const auto column = static_cast<int32_t>(std::floor(grid_x));
    const auto row = static_cast<int32_t>(std::floor(grid_y));
    const auto smooth = [](float value) { return value * value * (3 - 2 * value); };
    const float dx = smooth(grid_x - column);
    const float dy = smooth(grid_y - row);
    float value = std::lerp(
        std::lerp(sample(column, row), sample(column + 1, row), dx),
        std::lerp(sample(column, row + 1), sample(column + 1, row + 1), dx),
        dy
    );
    // Unknown cells retain solid black material. Fade the known side to zero
    // at their edges so interpolation never illuminates undiscovered ground.
    const auto home_x = static_cast<int32_t>(std::floor(x / edge));
    const auto home_y = static_cast<int32_t>(std::floor(y / edge));
    float edge_fade = 1;
    for (int32_t z = home_y - 1; z <= home_y + 1; ++z)
        for (int32_t c = home_x - 1; c <= home_x + 1; ++c) {
            if (c < 0 || z < 0 || c >= width || z >= height ||
                fog_at_point(c * fog_edge, z * fog_edge) != Fog::black)
                continue;
            const float distance_x = std::max({c * edge - x, x - (c + 1) * edge, 0.0F});
            const float distance_y = std::max({z * edge - y, y - (z + 1) * edge, 0.0F});
            edge_fade = std::min(
                edge_fade,
                std::clamp(std::hypot(distance_x, distance_y) / (edge * 0.5F), 0.0F, 1.0F)
            );
        }
    return value * edge_fade;
}

/// Interpolates fog inside known ground and fades to black before unknown cells.
float brightness_at(const full::SightView& sight, float x, float y, float unseen = -1) {
    return fog_brightness(
        [&sight](int32_t column, int32_t row) { return fog_at(sight, column, row); },
        sight.width,
        sight.height,
        sight.dithered,
        x,
        y,
        unseen
    );
}

/// brightness_at from the fog's cells held for the frame.
float brightness_at(const FogCells& fog, float x, float y, float unseen = -1) {
    return fog_brightness(
        [&fog](int32_t column, int32_t row) { return fog.at(column, row); },
        fog.width,
        fog.height,
        fog.dithered,
        x,
        y,
        unseen
    );
}

/// The ground's fog at a painted map point: how much of it is known, and how
/// far the known part blends toward the grey palette. The same figures the
/// terrain stores on each vertex, so a feature can grey with the ground.
struct GroundFog {
    float known{1};
    float grey{};
};

/// @param sight the frame's sight, or its fog's cells (FogCells)
/// @param dithered the fog dithers ground out of sight
/// @param painted_x a painted map column
/// @param painted_y a painted map row
/// @param brightness the point's brightness (brightness_at)
/// @return the ground's fog there
template <typename Sight>
GroundFog fog_of_ground(
    const Sight& sight, bool dithered, float painted_x, float painted_y, float brightness
) {
    GroundFog ground;
    ground.known = brightness >= 1   ? 1
                   : brightness <= 0 ? 0
                                     : brightness_at(sight, painted_x, painted_y, 1);
    const float grey_brightness = dithered ? 0.5F : 0.35F;
    ground.grey =
        ground.known > 0
            ? std::clamp(
                  (ground.known - brightness) / (ground.known * (1 - grey_brightness)), 0.0F, 1.0F
              )
            : 0;
    return ground;
}

GroundFog fog_of_ground(const Inputs& in, float painted_x, float painted_y, float brightness) {
    return fog_of_ground(in.sight, in.sight.dithered, painted_x, painted_y, brightness);
}

/// How many times a fog boundary splits a cell. Distant views keep fewer.
int32_t fog_divisions(const Inputs& in) {
    constexpr int32_t most = 4;
    const bool distant =
        std::isfinite(in.detail_scale) && in.detail_scale > 0 && in.detail_scale < 1;
    if (distant && in.detail_scale <= 0.25F)
        return 1;
    if (distant && in.detail_scale <= 0.5F)
        return 2;
    return most;
}

/// Samples the canonical world grid at a bounded world coordinate.
float height_at(const formats::tnt::Map& map, int32_t x, int32_t y) {
    const auto column = std::clamp(x / cell_edge, 0, static_cast<int32_t>(map.attribute_width) - 1);
    const auto row = std::clamp(y / cell_edge, 0, static_cast<int32_t>(map.attribute_height) - 1);
    return map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column].height;
}

/// Samples the heights a frame's ground is drawn from as height_at samples
/// the map's.
float height_at(const GroundSource& source, int32_t x, int32_t y) {
    const auto column = std::clamp(x / cell_edge, 0, static_cast<int32_t>(source.map_width) - 1);
    const auto row = std::clamp(y / cell_edge, 0, static_cast<int32_t>(source.map_height) - 1);
    return source.heights[static_cast<std::size_t>(row) * source.map_width + column];
}

/// Returns the height the simulation gives the middle of a height cell, as
/// unit_movement::Terrain::height interpolates between the cell's corners,
/// in whole steps.
///
/// @param map the map, or the heights a frame's ground is drawn from
/// @param x the cell's left edge, in map pixels
/// @param y the cell's top edge
/// @return the height
template <typename Heights>
float cell_middle_height(const Heights& map, int32_t x, int32_t y) {
    constexpr int32_t half = cell_edge / 2;
    const auto corner = [&](int32_t dx, int32_t dy) {
        return static_cast<int32_t>(height_at(map, x + dx, y + dy));
    };
    const int32_t top = corner(0, 0) + (corner(cell_edge, 0) - corner(0, 0)) * half / cell_edge;
    const int32_t bottom = corner(0, cell_edge) +
                           (corner(cell_edge, cell_edge) - corner(0, cell_edge)) * half / cell_edge;
    return static_cast<float>(top + (bottom - top) * half / cell_edge);
}

/// Tells whether a height cell's corners lie off one plane, so that two
/// triangles across one diagonal would leave the ground the simulation
/// interpolates between them: up to a quarter of the twist away at the
/// middle. Such a cell is drawn as four triangles about its middle, at the
/// simulation's height there, which leaves it a sixteenth of the twist away
/// at most.
///
/// @param top_left the cell's top left corner's height
/// @param top_right its top right corner's
/// @param bottom_right its bottom right corner's
/// @param bottom_left its bottom left corner's
/// @return true when the corners do not lie in one plane
bool twisted_cell(float top_left, float top_right, float bottom_right, float bottom_left) {
    return top_left - top_right + bottom_right - bottom_left != 0.0F;
}

/// Makes room in a list for count items: a quarter more when it must
/// grow, so that a list that grows a little from frame to frame is not
/// moved each frame.
///
/// @param[in,out] list the list
/// @param count the items it is to hold
template <typename Item>
void reserve_room(std::vector<Item>& list, std::size_t count) {
    if (list.capacity() < count)
        list.reserve(count + count / 4);
}

/// Adds a triangle fan after checking the frame's memory budget.
void polygon(Scene& scene, std::span<const Vertex> vertices) {
    if (vertices.size() < 3)
        return;
    if (scene.vertices.size() + (vertices.size() - 2) * 3 > maximum_vertices)
        throw GeometryFull();
    for (std::size_t corner = 1; corner + 1 < vertices.size(); ++corner)
        scene.vertices.insert(
            scene.vertices.end(), {vertices[0], vertices[corner], vertices[corner + 1]}
        );
}

/// A material a pass running beside the others did not find packed: only
/// the thread that builds the frame packs materials, in the passes' order,
/// so the pass stops at the draw that needs it (PassList::stopped).
class Unpacked : public std::exception {
  public:

    [[nodiscard]] const char* what() const noexcept override {
        return "experimental scene pass needs a material not yet packed";
    }
};

/// A scene pass at work: where it lists its triangles, and what it may do
/// with the atlas.
struct Pass {
    Scene& scene;   ///< the atlas and the frame's ground
    PassList& list; ///< where the pass lists its triangles
    /// The pass may pack materials: it runs on the thread that builds the
    /// frame, after the passes before it. Otherwise it only finds them, and
    /// one it does not find stops it (Unpacked).
    bool packing{};
    /// The frame's vertices listed before the list, which the geometry
    /// budget counts: none while the passes run at once, when the budget is
    /// held to as their lists are joined (PassList::peak).
    std::size_t listed_before{};
    /// The frame's ground triangles as the foundations leave them, which
    /// the cards standing up are lifted clear of.
    std::span<const Vertex> ground{};
};

/// Makes room in a pass's list for more vertices within the frame's
/// geometry budget, noting the most the list has held.
///
/// @param[in,out] pass the pass
/// @param count the vertices to be added
/// @return the list's vertices
std::vector<Vertex>& listing(Pass& pass, std::size_t count) {
    auto& list = pass.list;
    // The models the pass draws on the card count as the vertices they
    // would have listed.
    const std::size_t held = list.vertices.size() + list.cards.vertices + count;
    list.peak = std::max(list.peak, held);
    if (held > maximum_vertices - std::min(maximum_vertices, pass.listed_before))
        throw GeometryFull();
    return list.vertices;
}

/// Adds a triangle fan to a pass's list after checking the frame's memory budget.
void polygon(Pass& pass, std::span<const Vertex> vertices) {
    if (vertices.size() < 3)
        return;
    auto& listed = listing(pass, (vertices.size() - 2) * 3);
    for (std::size_t corner = 1; corner + 1 < vertices.size(); ++corner)
        listed.insert(listed.end(), {vertices[0], vertices[corner], vertices[corner + 1]});
}

/// Notes the draw a pass begins: should it stop in it (Unpacked), the draw
/// is listed again from the start.
///
/// @param[in,out] pass the pass
/// @param draw the draw's index among the pass's draws
void begin_draw(Pass& pass, std::size_t draw) {
    auto& list = pass.list;
    list.stopped = draw;
    list.draw_vertices = list.vertices.size();
    list.draw_translucent = list.translucent_vertices.size();
    list.draw_ranges = list.unit_ranges.size();
    mark_cards(list.cards);
}

/// Mixes eight bytes into a digest at once: every bit of the word reaches
/// every bit of the digest.
///
/// @param digest the digest so far
/// @param word the next eight bytes, or a value
/// @return the digest with the word mixed in
constexpr uint64_t mix_word(uint64_t digest, uint64_t word) {
    digest ^= std::rotl(word * digest_prime_2, 31) * digest_prime_1;
    return std::rotl(digest, 27) * digest_prime_1 + digest_prime_4;
}

/// Mixes bytes into a digest eight at a time, then the last few one by one.
///
/// @param digest the digest so far
/// @param bytes the bytes
/// @return the digest with the bytes mixed in
uint64_t mix_bytes(uint64_t digest, std::span<const uint8_t> bytes) {
    std::size_t at = 0;
    for (; at + sizeof(uint64_t) <= bytes.size(); at += sizeof(uint64_t)) {
        uint64_t word{};
        std::memcpy(&word, bytes.data() + at, sizeof(word));
        digest = mix_word(digest, word);
    }
    for (; at < bytes.size(); ++at)
        digest = std::rotl(digest ^ (bytes[at] * digest_prime_5), 11) * digest_prime_1;
    return digest;
}

/// @return the digest of a palette's bytes: the frame's own palette's
///     digest once taken this frame (Scene::frame_palette)
uint64_t palette_digest(const Scene& scene, const PaletteBytes& palette) {
    if (&palette == scene.frame_palette)
        return scene.frame_palette_digest;
    return mix_bytes(hash_start, palette);
}

/// @return the rows of the atlas the materials kept from frame to frame
///     may take: all but the frame's own (Scene::frame_rows)
uint32_t kept_rows(const Scene& scene) {
    return scene.texture_height - std::min(scene.frame_rows, scene.texture_height);
}

/// Notes a rectangle's rows among those the frame uploads
/// (Scene::atlas_update), and the row on either side: a filtered material
/// samples the texels one beyond its edge, which must be on the card as
/// they are here, though an emptied atlas leaves the rows it was not packed
/// in again unsent.
///
/// @param scene the atlas
/// @param[in,out] first_row the first row the frame uploads
/// @param[in,out] end_row the row after the last
/// @param rect the rectangle written
void note_rows(const Scene& scene, uint32_t& first_row, uint32_t& end_row, const AtlasRect& rect) {
    first_row = std::min(first_row, rect.y - std::min(rect.y, 1U));
    end_row = std::max(end_row, std::min(rect.y + rect.height + 1, scene.texture_height));
}

/// Writes a black material's texels: black, and opaque where covered or
/// as much as the coverage says.
///
/// @param[in,out] scene the atlas
/// @param rect where the material goes
/// @param pixels its palette indices, read where it has no coverage
/// @param coverage each texel's coverage, zero where clear, or none
/// @param transparent the index of a clear pixel where it has no coverage
/// @param alpha_coverage the coverage is each texel's alpha
void write_black(
    Scene& scene,
    const AtlasRect& rect,
    std::span<const uint8_t> pixels,
    std::span<const uint8_t> coverage,
    int32_t transparent,
    bool alpha_coverage
) {
    for (uint32_t y = 0; y < rect.height; ++y) {
        auto* row =
            scene.texture_rgba.data() +
            (static_cast<std::size_t>(rect.y + y) * scene.texture_width + rect.x) * channels;
        for (uint32_t x = 0; x < rect.width; ++x) {
            const auto source = static_cast<std::size_t>(y) * rect.width + x;
            const bool covered =
                coverage.empty() ? pixels[source] != transparent : coverage[source] != 0;
            row[x * channels] = row[x * channels + 1] = row[x * channels + 2] = 0;
            row[x * channels + 3] = static_cast<uint8_t>(
                alpha_coverage && !coverage.empty() ? coverage[source] : (covered ? 255 : 0)
            );
        }
    }
}

/// The digest material() looks a material up by. A black material
/// (Fog::black, no light mask) is black whatever the palette: it is shared
/// by every material of its size whose texels are covered alike, keyed on
/// its coverage alone where it has one.
uint64_t material_digest(
    const Scene& scene,
    const PaletteBytes& palette,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> pixels,
    std::span<const uint8_t> coverage,
    int32_t transparent,
    Fog fog,
    bool light_mask,
    FlashStrength flash_strength,
    bool alpha_coverage
) {
    const std::size_t count = static_cast<std::size_t>(width) * height;
    const bool black = fog == Fog::black && !light_mask;
    uint64_t digest = hash_start;
    digest = mix_word(digest, width);
    digest = mix_word(digest, height);
    digest = mix_word(digest, static_cast<uint64_t>(fog));
    digest = mix_word(digest, light_mask);
    digest = mix_word(digest, alpha_coverage);
    if (light_mask)
        digest = mix_word(digest, static_cast<uint64_t>(flash_strength));
    if (!black || coverage.empty()) {
        digest = mix_word(digest, static_cast<uint64_t>(transparent + 1));
        digest = mix_bytes(digest, pixels.first(count));
    }
    digest = mix_bytes(digest, coverage);
    // A palette change must invalidate the cached material colours too.
    if (!black)
        digest = mix_word(digest, palette_digest(scene, palette));
    return digest;
}

std::optional<AtlasRect> material_square(Scene& scene, uint32_t width, uint32_t height);

/// Packs palette pixels with coverage and fog, sharing identical materials
/// (material_digest), in the rows, or in a terrain square where a moving
/// camera's frame finds the rows full (Scene::moving_packing). A pass that
/// may not pack (Pass::packing) only finds them.
AtlasRect material(
    Pass& pass,
    const PaletteBytes& palette,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> pixels,
    std::span<const uint8_t> coverage,
    int32_t transparent,
    Fog fog,
    bool light_mask = false,
    FlashStrength flash_strength = FlashStrength::full,
    bool alpha_coverage = false
) {
    auto& scene = pass.scene;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || pixels.size() < count ||
        (!coverage.empty() && coverage.size() < count))
        throw std::runtime_error("experimental scene received an invalid material");
    const bool black = fog == Fog::black && !light_mask;
    if (!coverage.empty())
        coverage = coverage.first(count);
    const uint64_t digest = material_digest(
        scene,
        palette,
        width,
        height,
        pixels,
        coverage,
        transparent,
        fog,
        light_mask,
        flash_strength,
        alpha_coverage
    );
    if (const auto found = scene.materials.find(digest); found != scene.materials.end())
        return found->second;
    if (!pass.packing)
        throw Unpacked();
    ++pass.list.materials_packed;
    pass.list.texels_packed += uint64_t{width} * height;
    if (width > scene.texture_width || height > scene.texture_height)
        throw std::runtime_error("experimental material is larger than its atlas");
    uint32_t row_x = scene.next_x;
    uint32_t row_y = scene.next_y;
    uint32_t row_height = scene.row_height;
    if (row_x + width > scene.texture_width) {
        row_x = 0;
        row_y += row_height;
        row_height = 0;
    }
    // Past the rows, a moving camera's material takes a terrain square.
    std::optional<AtlasRect> square;
    if (row_y + height > kept_rows(scene)) {
        if (scene.moving_packing)
            square = material_square(scene, width, height);
        if (!square)
            throw AtlasFull();
    }
    const AtlasRect rect{square ? square->x : row_x, square ? square->y : row_y, width, height};
    if (black)
        write_black(scene, rect, pixels, coverage, transparent, alpha_coverage);
    for (uint32_t y = 0; y < height && !black; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto source = static_cast<std::size_t>(y) * width + x;
            const auto target =
                (static_cast<std::size_t>(rect.y + y) * scene.texture_width + rect.x + x) *
                channels;
            const auto color = static_cast<std::size_t>(pixels[source]) * palette_entry_bytes;
            const bool covered =
                coverage.empty() ? pixels[source] != transparent : coverage[source] != 0;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                uint8_t value = palette[color + channel];
                if (fog == Fog::grey)
                    value = static_cast<uint8_t>(
                        (uint32_t{palette[color]} + palette[color + 1] + palette[color + 2]) / 6
                    );
                if (fog == Fog::black)
                    value = 0;
                scene.texture_rgba[target + channel] = value;
            }
            scene.texture_rgba[target + 3] = static_cast<uint8_t>(
                alpha_coverage && !coverage.empty() ? coverage[source] : (covered ? 255 : 0)
            );
            // Transparent texels carry zero RGB so filtered effect edges interpolate premultiplied colour.
            if (!covered)
                std::fill_n(scene.texture_rgba.data() + target, 3, 0);
            if (light_mask) {
                // Flash bytes name light-table rows, not palette colours. The
                // spatial pass approximates relighting with a translucent mask.
                constexpr float maximum_flash_opacity = 0.55F;
                const int32_t row = pixels[source] - present::shade_ramp_base;
                const bool valid = covered && row >= 0 && row < present::ramp_table_rows;
                const int32_t strength = flash_strength == FlashStrength::reduced ? row / 2 : row;
                const float alpha =
                    valid ? maximum_flash_opacity * strength / (present::ramp_table_rows - 1)
                          : 0.0F;
                std::fill_n(scene.texture_rgba.data() + target, 3, 255);
                scene.texture_rgba[target + 3] = static_cast<uint8_t>(std::lround(alpha * 255));
            }
        }
    }
    scene.materials.emplace(digest, rect);
    scene.packed_material_area += uint64_t{width} * height;
    ++scene.texture_revision;
    if (square) {
        scene.atlas_update.regions.push_back({rect.x, rect.y, rect.width, rect.height});
        return rect;
    }
    scene.next_x = row_x + width;
    scene.next_y = row_y;
    scene.row_height = std::max(row_height, height);
    note_rows(scene, scene.atlas_update.first_row, scene.atlas_update.end_row, rect);
    return rect;
}

/// The digest a frame's own material is found by: material_digest of a
/// black material of its coverage, which no palette changes.
///
/// @param scene the frame
/// @param palette the frame's palette
/// @param width the material's width
/// @param height its height
/// @param coverage each texel's coverage, zero where clear
/// @param alpha_coverage the coverage is each texel's alpha
/// @return the digest
uint64_t frame_material_digest(
    const Scene& scene,
    const PaletteBytes& palette,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> coverage,
    bool alpha_coverage
) {
    return material_digest(
        scene,
        palette,
        width,
        height,
        coverage,
        coverage,
        -1,
        Fog::black,
        false,
        FlashStrength::full,
        alpha_coverage
    );
}

/// Writes a frame's own material into the rows kept for them, noting its
/// rows among those the frame uploads.
///
/// @param[in,out] scene the atlas
/// @param rect where the material goes
/// @param coverage each texel's coverage, zero where clear
/// @param alpha_coverage the coverage is each texel's alpha
void write_frame_material(
    Scene& scene, const AtlasRect& rect, std::span<const uint8_t> coverage, bool alpha_coverage
) {
    write_black(scene, rect, coverage, coverage, -1, alpha_coverage);
    ++scene.texture_revision;
    note_rows(scene, scene.atlas_update.frame_first_row, scene.atlas_update.frame_end_row, rect);
}

/// Packs a black material that changes from frame to frame, the shadow a
/// unit casts from its picture, into the rows kept for the frame's own
/// materials (Scene::frame_rows); with the other materials when those rows
/// are full or there are none.
///
/// @param[in,out] pass the pass packing it, which may stop here when the
///     material goes with the others (Unpacked)
/// @param[in,out] frame the frame's own materials packed so far
/// @param digest the material's digest (frame_material_digest)
/// @param width the material's width
/// @param height its height
/// @param coverage each texel's coverage, zero where clear
/// @param alpha_coverage the coverage is each texel's alpha; otherwise a
///     covered texel is opaque
/// @param[out] written the material is new in the rows: its texels are to
///     be written there (write_frame_material), as the caller does
/// @return the material's rectangle, good for this frame
AtlasRect frame_material(
    Pass& pass,
    FramePacking& frame,
    uint64_t digest,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> coverage,
    bool alpha_coverage,
    bool& written
) {
    auto& scene = pass.scene;
    written = false;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || coverage.size() < count)
        throw std::runtime_error("experimental scene received an invalid material");
    coverage = coverage.first(count);
    if (const auto found = frame.materials.find(digest); found != frame.materials.end())
        return found->second;
    frame.material_area += uint64_t{width} * height;
    uint32_t x = frame.next_x;
    uint32_t y = frame.next_y;
    uint32_t row_height = frame.row_height;
    if (x + width > scene.texture_width) {
        x = 0;
        y += row_height;
        row_height = 0;
    }
    if (width > scene.texture_width || y + height > scene.frame_rows ||
        scene.frame_rows >= scene.texture_height) {
        frame.spilled_area += uint64_t{width} * height;
        // Measured with the frame's own materials, not twice.
        const uint64_t packed = scene.packed_material_area;
        const auto rect = material(
            pass,
            *scene.frame_palette,
            width,
            height,
            coverage,
            coverage,
            -1,
            Fog::black,
            false,
            FlashStrength::full,
            alpha_coverage
        );
        scene.packed_material_area = packed;
        return rect;
    }
    ++pass.list.materials_packed;
    pass.list.texels_packed += uint64_t{width} * height;
    frame.next_x = x + width;
    frame.next_y = y;
    frame.row_height = std::max(row_height, height);
    const AtlasRect rect{x, kept_rows(scene) + y, width, height};
    frame.materials.emplace(digest, rect);
    // Each material's place follows from the sizes of those packed before
    // it this frame, all of them in its digest: one packed in the same
    // place by an earlier frame, after the same ones, is still there.
    const std::size_t index = frame.packed++;
    if (index < frame.order.size() && frame.order[index] == digest)
        return rect;
    frame.order.resize(index);
    frame.order.push_back(digest);
    written = true;
    return rect;
}

/// One point on a terrain triangle, in painted coordinates and world height.
struct GroundPoint {
    float x{};
    float y{};
    float height{};
    float brightness{1};
};

/// Keep triangle/rectangle intersections on the stack; at most seven corners survive.
struct GroundPolygon {
    std::array<GroundPoint, 8> points{};
    std::size_t count{};
};

/// Clip one convex triangle fragment without allocating transient storage.
GroundPolygon
clip_ground_fixed(const GroundPolygon& source, bool horizontal, float edge, bool greater) {
    GroundPolygon result{};
    if (source.count == 0)
        return result;
    const auto coordinate = [horizontal](const GroundPoint& p) { return horizontal ? p.x : p.y; };
    auto previous = source.points[source.count - 1];
    bool previous_inside = greater ? coordinate(previous) >= edge : coordinate(previous) <= edge;
    for (std::size_t index = 0; index < source.count; ++index) {
        const auto& current = source.points[index];
        const bool inside = greater ? coordinate(current) >= edge : coordinate(current) <= edge;
        if (inside != previous_inside) {
            const float t =
                (edge - coordinate(previous)) / (coordinate(current) - coordinate(previous));
            result.points[result.count++] = {
                std::lerp(previous.x, current.x, t),
                std::lerp(previous.y, current.y, t),
                std::lerp(previous.height, current.height, t),
                std::lerp(previous.brightness, current.brightness, t)
            };
            auto& intersection = result.points[result.count - 1];
            (horizontal ? intersection.x : intersection.y) = edge;
        }
        if (inside)
            result.points[result.count++] = current;
        previous = current;
        previous_inside = inside;
    }
    return result;
}

/// The squares a frame packs its terrain tiles into: full squares, unless
/// the tiles it draws need more room than the atlas has.
struct TerrainPlan {
    /// Each tile key's square where any is smaller than full.
    std::unordered_map<uint64_t, uint32_t> squares{};
    /// The square of a tile the survey did not foresee.
    uint32_t fallback{full_terrain_square};
    /// The larger of the two sizes, the nearest tiles' square: full where
    /// every tile fits.
    uint32_t larger{full_terrain_square};
    /// The camera is moving, or has not yet settled
    /// (Inputs::tiles_hold_while_moving): each tile keeps any square no
    /// more than one size smaller than its own, and a tile packed takes the
    /// size below the larger, which it keeps however near it comes.
    bool holding{};
    /// A camera that has moved has settled (Scene::moving_packing): each
    /// tile takes the plan's square, neither smaller nor larger, the detail
    /// it would have had had the camera never moved.
    bool exact{};

    /// @param key a tile key, as the terrain pass makes it
    /// @return the edge of the square the plan gives it
    [[nodiscard]] uint32_t square(uint64_t key) const {
        const auto found = squares.find(key);
        return found == squares.end() ? fallback : found->second;
    }

    /// @param key a tile key, as the terrain pass makes it
    /// @return the edge of the least square it may keep
    [[nodiscard]] uint32_t least(uint64_t key) const {
        const uint32_t edge = square(key);
        return holding ? std::max(edge / 2, smallest_terrain_square) : edge;
    }

    /// @param key a tile key, as the terrain pass makes it
    /// @return the edge of the square to pack it into
    [[nodiscard]] uint32_t packed(uint64_t key) const {
        return holding ? std::max(larger / 2, smallest_terrain_square) : square(key);
    }

    /// @param key a tile key, as the terrain pass makes it
    /// @param edge the edge of a square it is packed in
    /// @return true when it may keep that square
    [[nodiscard]] bool keeps(uint64_t key, uint32_t edge) const {
        return exact && !holding ? edge == square(key) : edge >= least(key);
    }
};

/// @return the spare squares of a terrain square's edge
///     (Scene::spare_terrain_squares)
std::vector<AtlasRect>& spare_terrain_squares(Scene& scene, uint32_t edge) {
    return scene.spare_terrain_squares.at(
        static_cast<std::size_t>(std::countr_zero(full_terrain_square / edge))
    );
}

/// @return a square's key in Scene::split_terrain_squares
uint64_t split_square_key(uint32_t x, uint32_t y, uint32_t edge) {
    return (uint64_t{x} << 32) | (uint64_t{y} << 8) | edge;
}

/// Finds the split square a terrain square smaller than full is a quarter
/// of: the one split square of twice its edge among the four that would
/// hold it.
///
/// @param scene the split squares
/// @param square the quarter
/// @return the split square's entry, or the end of the split squares; and
///     the quarter's bit in it
std::pair<std::unordered_map<uint64_t, uint8_t>::iterator, uint8_t>
split_parent(Scene& scene, const AtlasRect& square) {
    const uint32_t edge = square.width;
    for (uint32_t row = 0; row < 2; ++row)
        for (uint32_t column = 0; column < 2; ++column) {
            if (square.x < column * edge || square.y < row * edge)
                continue;
            const auto found = scene.split_terrain_squares.find(
                split_square_key(square.x - column * edge, square.y - row * edge, edge * 2)
            );
            if (found != scene.split_terrain_squares.end())
                return {found, static_cast<uint8_t>(1U << (row * 2 + column))};
        }
    return {scene.split_terrain_squares.end(), 0};
}

/// Takes a square of the atlas for a terrain material: a spare one of its
/// size when there is one; otherwise a full square from the rows as every
/// other material is packed, and a smaller one as a quarter of a square
/// split for its size, so that small squares waste no rows.
///
/// @param[in,out] scene the atlas and its spare squares
/// @param edge the square's edge: full_terrain_square or a power of two
///     down to smallest_terrain_square
/// @param rows whether a full square may be taken from the rows; otherwise
///     only spare squares are
/// @param left_rows the rows a square from the rows leaves the other
///     materials at least
/// @return the square, or none when there is no room left for it
std::optional<AtlasRect>
take_terrain_square(Scene& scene, uint32_t edge, bool rows, uint32_t left_rows = 0) {
    auto& spare = spare_terrain_squares(scene, edge);
    if (!spare.empty()) {
        const auto square = spare.back();
        spare.pop_back();
        if (edge < full_terrain_square) {
            const auto [parent, quarter] = split_parent(scene, square);
            if (parent != scene.split_terrain_squares.end())
                parent->second &= static_cast<uint8_t>(~quarter);
        }
        return square;
    }
    if (edge >= full_terrain_square) {
        if (!rows)
            return std::nullopt;
        uint32_t x = scene.next_x;
        uint32_t y = scene.next_y;
        uint32_t row_height = scene.row_height;
        if (x + full_terrain_square > scene.texture_width) {
            x = 0;
            y += row_height;
            row_height = 0;
        }
        if (y + full_terrain_square + left_rows > kept_rows(scene))
            return std::nullopt;
        scene.next_x = x + full_terrain_square;
        scene.next_y = y;
        scene.row_height = std::max(row_height, full_terrain_square);
        return AtlasRect{x, y, full_terrain_square, full_terrain_square};
    }
    const auto parent = take_terrain_square(scene, edge * 2, rows, left_rows);
    if (!parent)
        return std::nullopt;
    // Every quarter but the first is spare.
    constexpr uint8_t first_quarter = 1;
    scene.split_terrain_squares[split_square_key(parent->x, parent->y, parent->width)] =
        all_quarters & ~first_quarter;
    spare.push_back({parent->x + edge, parent->y + edge, edge, edge});
    spare.push_back({parent->x, parent->y + edge, edge, edge});
    spare.push_back({parent->x + edge, parent->y, edge, edge});
    return AtlasRect{parent->x, parent->y, edge, edge};
}

/// Gives a terrain material's square back, to be packed again: to the
/// spare squares of its size, or, when the other three quarters of the
/// square it was split from are spare too, that square whole, as far up as
/// that goes. No triangle the frame draws or the kept ground holds may
/// still sample it.
///
/// @param[in,out] scene the spare and split squares
/// @param square the square
void give_back_terrain_square(Scene& scene, AtlasRect square) {
    while (square.width < full_terrain_square) {
        auto& spare = spare_terrain_squares(scene, square.width);
        const auto [parent, quarter] = split_parent(scene, square);
        if (parent == scene.split_terrain_squares.end() ||
            (parent->second | quarter) != all_quarters) {
            if (parent != scene.split_terrain_squares.end())
                parent->second |= quarter;
            spare.push_back(square);
            return;
        }
        // The other three quarters, the spare squares of this edge inside
        // the split square, make it whole with this one.
        const uint32_t edge = square.width * 2;
        const AtlasRect whole{
            static_cast<uint32_t>(parent->first >> 32),
            static_cast<uint32_t>(parent->first >> 8) & 0xFFFFFFU,
            edge,
            edge
        };
        std::erase_if(spare, [&](const AtlasRect& quarter_square) {
            return quarter_square.x >= whole.x && quarter_square.x < whole.x + edge &&
                   quarter_square.y >= whole.y && quarter_square.y < whole.y + edge;
        });
        scene.split_terrain_squares.erase(parent);
        square = whole;
    }
    spare_terrain_squares(scene, full_terrain_square).push_back(square);
}

/// Drops the terrain materials that no terrain pass since the given one
/// has drawn, giving their squares back (give_back_terrain_square).
///
/// @param[in,out] scene the terrain materials and spare squares
/// @param since the pass (Scene::terrain_passes); a material last drawn by
///     an earlier one is dropped
/// @return true when any was dropped
bool drop_terrain_materials(Scene& scene, uint64_t since) {
    bool dropped = false;
    for (auto entry = scene.terrain_materials.begin(); entry != scene.terrain_materials.end();) {
        if (entry->second.drawn >= since) {
            ++entry;
            continue;
        }
        give_back_terrain_square(scene, entry->second.rectangle);
        entry = scene.terrain_materials.erase(entry);
        dropped = true;
    }
    return dropped;
}

/// How a moving camera's ground finds room for its tiles in the atlas
/// (terrain_square), when it replaces the kept ground: every tile it draws
/// is marked drawn by it before any is packed, so that any other tile may
/// give its square back. Once the rows it leaves the other materials are
/// all it could take, the tiles that have left the picture give their
/// squares back, then its own tiles held in larger squares than the moving
/// camera packs them into, to be packed again at that size, before it
/// takes those rows too.
struct MovingRoom {
    const TerrainPlan& plan;
    std::span<const uint64_t> tiles; ///< the ground's tile keys, in the order it draws them
    std::size_t next_tile{};         ///< the next of them that may give its square back
    /// The tiles that gave their squares back, to be packed again.
    std::vector<uint64_t> unpacked{};
    uint32_t material_rows{}; ///< the atlas rows left to the other materials
    /// The terrain pass that last dropped the tiles this ground does not draw.
    uint64_t dropped_unseen{};
    /// The ground is packed ahead of the frame that takes it, while the kept
    /// ground still draws, whose tiles keep their squares (AheadFull).
    bool ahead{};
    /// For a ground packed ahead, the tiles the kept ground draws.
    const std::unordered_set<uint64_t>* kept_tiles{};
};

/// Gives back the square of the next tile of a moving camera's ground held
/// in a larger square than the moving camera packs it into, for it to be
/// packed again at that size (MovingRoom::unpacked).
///
/// @param[in,out] scene the terrain materials and spare squares
/// @param[in,out] room the ground's tiles
/// @return true when a tile gave its square back
bool unpack_larger_tile(Scene& scene, MovingRoom& room) {
    while (room.next_tile < room.tiles.size()) {
        const uint64_t key = room.tiles[room.next_tile++];
        const auto cached = scene.terrain_materials.find(key);
        if (cached == scene.terrain_materials.end() ||
            cached->second.rectangle.width <= room.plan.packed(key) ||
            (room.kept_tiles && room.kept_tiles->contains(key)))
            continue;
        give_back_terrain_square(scene, cached->second.rectangle);
        scene.terrain_materials.erase(cached);
        room.unpacked.push_back(key);
        return true;
    }
    return false;
}

/// Takes whole spare terrain squares side by side, a block of them, from
/// the first spare square that is the block's top left corner.
///
/// @param[in,out] scene the spare squares
/// @param across the squares across
/// @param down the squares down
/// @return the block, or none
std::optional<AtlasRect> take_spare_block(Scene& scene, uint32_t across, uint32_t down) {
    auto& spare = spare_terrain_squares(scene, full_terrain_square);
    const auto place = [](uint32_t x, uint32_t y) { return (uint64_t{x} << 32) | y; };
    std::unordered_set<uint64_t> spare_places;
    for (const auto& square : spare)
        spare_places.insert(place(square.x, square.y));
    for (const auto& corner : spare) {
        bool whole = true;
        for (uint32_t row = 0; row < down && whole; ++row)
            for (uint32_t column = 0; column < across && whole; ++column)
                whole = spare_places.contains(place(
                    corner.x + column * full_terrain_square, corner.y + row * full_terrain_square
                ));
        if (!whole)
            continue;
        const AtlasRect block{
            corner.x, corner.y, across * full_terrain_square, down * full_terrain_square
        };
        std::erase_if(spare, [&](const AtlasRect& square) {
            return square.x >= block.x && square.x < block.x + block.width && square.y >= block.y &&
                   square.y < block.y + block.height;
        });
        return block;
    }
    return std::nullopt;
}

/// Takes terrain squares for a material that finds the rows full while the
/// camera moves (Scene::moving_packing): a spare square large enough, or a
/// block of whole spare squares side by side for a larger material, or the
/// same given back by the tiles neither the frame's ground nor a ground
/// packed ahead of it draws. They hold the material until the atlas is emptied.
///
/// @param[in,out] scene the spare squares and terrain materials
/// @param width the material's width
/// @param height its height
/// @return the square or block, or none when there is none for it
std::optional<AtlasRect> material_square(Scene& scene, uint32_t width, uint32_t height) {
    const uint32_t size = std::max(width, height);
    const auto take = [&]() -> std::optional<AtlasRect> {
        if (size > full_terrain_square)
            return take_spare_block(
                scene,
                (width + full_terrain_square - 1) / full_terrain_square,
                (height + full_terrain_square - 1) / full_terrain_square
            );
        uint32_t edge = smallest_terrain_square;
        while (edge < size)
            edge *= 2;
        return take_terrain_square(scene, edge, false);
    };
    auto square = take();
    if (!square && drop_terrain_materials(scene, scene.ground_pass))
        square = take();
    return square;
}

/// Takes a square of the atlas for a terrain material (take_terrain_square),
/// keeping the rows for the other materials while the terrain can make room
/// of its own: a spare square; else one given back by the tiles neither this
/// terrain pass nor the last has drawn; else one from the rows; else one
/// given back by the tiles this pass has not drawn yet. The frame draws none
/// of the tiles that give their squares back, nor does the kept ground,
/// which this pass draws again; none gives its square back while a kept
/// ground may still be taken (Scene::ground_kept). A moving camera's
/// ground (MovingRoom) leaves the other materials rows while it can make
/// room of its own: from the tiles it does not draw, then by packing its
/// tiles held in larger squares again at the moving camera's size. One
/// packed ahead, while the kept ground draws, gives back only squares
/// neither ground draws and leaves the other materials their rows
/// (AheadFull).
///
/// @param[in,out] scene the atlas, its spare squares and the terrain
///     materials
/// @param edge the square's edge
/// @param moving how a moving camera's ground finds room; none for a
///     settled camera's
/// @return the square
/// @throws AtlasFull when the atlas has no room left for it
/// @throws AheadFull when a ground packed ahead finds none without the kept
///     ground's
AtlasRect terrain_square(Scene& scene, uint32_t edge, MovingRoom* moving) {
    const uint64_t pass = scene.terrain_passes;
    auto square = take_terrain_square(scene, edge, false);
    // Tiles drawn by neither pass cannot be drawn by this one until it ends.
    if (!square && !scene.ground_kept && scene.stale_terrain_dropped != pass) {
        scene.stale_terrain_dropped = pass;
        if (drop_terrain_materials(scene, pass - 1))
            square = take_terrain_square(scene, edge, false);
    }
    // One packed ahead, while the kept ground draws, from the tiles neither
    // draws.
    if (moving && moving->ahead && !square && moving->dropped_unseen != pass) {
        moving->dropped_unseen = pass;
        if (drop_terrain_materials(scene, scene.ground_pass))
            square = take_terrain_square(scene, edge, false);
    }
    if (!square)
        square = take_terrain_square(scene, edge, true, moving ? moving->material_rows : 0);
    // A moving camera's ground, having marked every tile it draws, makes
    // room of its own before it takes the rows left to the other materials:
    // first from the tiles that have left the picture, then from its own
    // held in larger squares than it packs into; one packed ahead, from its
    // own the kept ground does not draw, and it leaves those rows.
    if (moving && !moving->ahead && !square && moving->dropped_unseen != pass) {
        moving->dropped_unseen = pass;
        if (drop_terrain_materials(scene, pass))
            square = take_terrain_square(scene, edge, false);
    }
    while (!square && moving && unpack_larger_tile(scene, *moving))
        square = take_terrain_square(scene, edge, false);
    if (!square && moving && !moving->ahead)
        square = take_terrain_square(scene, edge, true);
    if (!square && !scene.ground_kept && drop_terrain_materials(scene, pass))
        square = take_terrain_square(scene, edge, true);
    if (!square && moving && moving->ahead)
        throw AheadFull();
    if (!square)
        throw AtlasFull();
    return *square;
}

/// Packs colour and classic grey palettes with independently clamped mip
/// chains. A full square holds the tile's 32 pixels; a smaller one starts
/// its chain at a smaller level, each level averaged from the whole tile
/// as a full square's same level is.
///
/// @param[in,out] scene the atlas
/// @param palette the tile's colours
/// @param grey the colours of ground out of sight
/// @param pixels the tile's palette indices
/// @param fog black for undiscovered ground
/// @param edge the square's edge, full_terrain_square or smaller
/// @param moving how a moving camera's ground finds room (terrain_square)
/// @return the square, whose top left levels are edge / 2 pixels wide
AtlasRect terrain_material(
    Scene& scene,
    const PaletteBytes& palette,
    const PaletteBytes& grey,
    std::span<const uint8_t> pixels,
    Fog fog,
    uint32_t edge = full_terrain_square,
    MovingRoom* moving = nullptr
) {
    bench_timers::count_terrain_square();
    const std::array cursor{scene.next_x, scene.next_y};
    const AtlasRect rect = terrain_square(scene, edge, moving);
    const uint32_t detail = rect.width / 2;
    const auto pixel_at = [&](uint32_t x, uint32_t y) {
        return scene.texture_rgba.data() +
               (static_cast<std::size_t>(rect.y + y) * scene.texture_width + rect.x + x) * channels;
    };
    // The sums of each block of the tile's pixels, a channel at a time: a
    // level's blocks are made of four of the level above's, so each level
    // is summed from the last at a quarter of the work of summing the
    // pixels again. The sums are whole, so every level rounds exactly as
    // the mean of its pixels.
    constexpr std::size_t tile_pixels = static_cast<std::size_t>(tile_edge) * tile_edge;
    std::array<uint32_t, tile_pixels * channels> sums{};
    for (uint32_t variant = 0; variant < 2; ++variant) {
        const auto& colours = variant == 0 ? palette : grey;
        const uint32_t origin = variant * detail;
        for (std::size_t pixel = 0; pixel < tile_pixels; ++pixel) {
            const auto index = static_cast<std::size_t>(pixels[pixel]) * palette_entry_bytes;
            for (uint32_t channel = 0; channel < 3; ++channel)
                sums[pixel * channels + channel] =
                    fog == Fog::black ? uint32_t{0} : colours[index + channel];
            sums[pixel * channels + 3] = 255;
        }
        // The sums hold blocks of factor pixels a side, blocks of them across.
        auto blocks = static_cast<uint32_t>(tile_edge);
        uint32_t factor = 1;
        uint32_t row = 0;
        for (uint32_t level = detail; level != 0; level /= 2) {
            while (blocks > level) {
                const uint32_t half = blocks / 2;
                for (uint32_t y = 0; y < half; ++y)
                    for (uint32_t x = 0; x < half; ++x)
                        for (uint32_t channel = 0; channel < channels; ++channel) {
                            const auto at = [&](uint32_t column, uint32_t line) {
                                return sums
                                    [(static_cast<std::size_t>(line) * blocks + column) * channels +
                                     channel];
                            };
                            // Written in place: block (x, y) lies at or before
                            // the four it is summed from.
                            sums[(static_cast<std::size_t>(y) * half + x) * channels + channel] =
                                at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                                at(2 * x + 1, 2 * y + 1);
                        }
                blocks = half;
                factor *= 2;
            }
            const uint32_t count = factor * factor;
            for (uint32_t y = 0; y < level; ++y)
                for (uint32_t x = 0; x < level; ++x) {
                    auto* out = pixel_at(origin + x, row + y);
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        out[channel] = static_cast<uint8_t>(
                            (sums[(static_cast<std::size_t>(y) * level + x) * channels + channel] +
                             count / 2) /
                            count
                        );
                }
            row += level;
        }
    }
    ++scene.texture_revision;
    // A spare square, anywhere in the atlas, is uploaded alone; one cut from
    // the rows with the rows packed this frame.
    if (cursor == std::array{scene.next_x, scene.next_y}) {
        scene.atlas_update.regions.push_back({rect.x, rect.y, rect.width, rect.height});
        return rect;
    }
    note_rows(scene, scene.atlas_update.first_row, scene.atlas_update.end_row, rect);
    return rect;
}

/// One terrain tile a frame draws, and how near the camera it comes.
struct TileDemand {
    uint64_t key{};
    float nearest{}; ///< the greatest depth of its cells: nearer is greater
};

/// The tile key of a painted tile position: its tile, and black for
/// undiscovered ground, as the terrain pass packs it.
uint64_t terrain_tile_key(const Inputs& in, int32_t tile_x, int32_t tile_y) {
    const auto& map = *in.map;
    const auto tile = map.tile_indices.at(
        static_cast<std::size_t>(tile_y / tile_edge) * map.tile_width + tile_x / tile_edge
    );
    const auto fog = fog_at(in.sight, tile_x, tile_y) == Fog::black ? Fog::black : Fog::seen;
    return static_cast<uint64_t>(tile) * 3 + static_cast<uint64_t>(fog);
}

/// The painted rectangle the terrain pass draws, in map pixels: whole cells
/// from the left and top, and as far below as the highest ground lifts.
struct TerrainCrop {
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
};

/// @param map the map
/// @param left the painted rectangle's least map x
/// @param top its least painted row
/// @param right its greatest map x
/// @param bottom its greatest painted row
/// @return the terrain pass's crop of the painted rectangle
TerrainCrop terrain_crop(
    const formats::tnt::Map& map, int32_t left, int32_t top, int32_t right, int32_t bottom
) {
    const int32_t width = static_cast<int32_t>(map.attribute_width) * cell_edge;
    const int32_t height = static_cast<int32_t>(map.attribute_height) * cell_edge;
    return {
        std::clamp(left, 0, width) / cell_edge * cell_edge,
        std::clamp(top, 0, height) / cell_edge * cell_edge,
        std::clamp(right, 0, width),
        std::clamp(bottom + maximum_terrain_row_shift, 0, height)
    };
}

TerrainCrop terrain_crop(const Inputs& in) {
    return terrain_crop(*in.map, in.left, in.top, in.right, in.bottom);
}

/// How far beyond the picture's edges, in its pixels, the terrain pass still
/// draws ground: what lies near the picture, such as the ground under an
/// explosion's card that reaches into it, keeps its ground.
constexpr float screen_ground_margin = 128;

/// How much further beyond the picture's edges a moving camera's ground is
/// drawn, as a share of the picture's larger side: the frames after it take
/// that ground while their pictures move no further (Scene::ground_vertices).
constexpr float moving_ground_margin_share = 0.1F;

/// Tells whether a box of ground lies wholly beyond the picture, more than
/// margin pixels past one of its edges. Every triangle the terrain pass
/// draws in the box lies inside it, so none of them would show. Without the
/// picture's size (clip_width or clip_height zero) no ground is left out.
///
/// @param m the picture's projection
/// @param clip_width the picture's width
/// @param clip_height its height
/// @param left the box's least map x
/// @param top its least map z (row)
/// @param right its greatest map x
/// @param bottom its greatest map z
/// @param low the least height in the box
/// @param high the greatest
/// @param margin the pixels beyond the picture's edges still drawn
/// @return true when the box lies wholly beyond the picture
bool ground_off_screen(
    const std::array<float, 12>& m,
    uint32_t clip_width,
    uint32_t clip_height,
    float left,
    float top,
    float right,
    float bottom,
    float low,
    float high,
    float margin
) {
    if (clip_width == 0 || clip_height == 0)
        return false;
    // An affine row's least and greatest value over the box.
    const auto span = [&](std::size_t row) {
        const float x0 = m[row] * left, x1 = m[row] * right;
        const float h0 = m[row + 1] * low, h1 = m[row + 1] * high;
        const float z0 = m[row + 2] * top, z1 = m[row + 2] * bottom;
        return std::array{
            std::min(x0, x1) + std::min(h0, h1) + std::min(z0, z1) + m[row + 3],
            std::max(x0, x1) + std::max(h0, h1) + std::max(z0, z1) + m[row + 3]
        };
    };
    const auto across = span(0);
    const auto down = span(4);
    // A bound that is not a number fails every test, and its box is kept.
    return across[1] < -margin || across[0] > static_cast<float>(clip_width) + margin ||
           down[1] < -margin || down[0] > static_cast<float>(clip_height) + margin;
}

/// ground_off_screen for the frame's own picture.
bool ground_off_screen(
    const Inputs& in,
    float left,
    float top,
    float right,
    float bottom,
    float low,
    float high,
    float margin = screen_ground_margin
) {
    return ground_off_screen(
        in.clip_projection,
        in.clip_width,
        in.clip_height,
        left,
        top,
        right,
        bottom,
        low,
        high,
        margin
    );
}

/// The crop of a picture widened by margin pixels beyond its edges, past
/// screen_ground_margin: the painted rectangle in which any ground drawn
/// within that margin lies, as the runtime crops the picture itself.
///
/// @param map the map
/// @param projection the picture's projection
/// @param clip_width the picture's width
/// @param clip_height its height
/// @param margin the pixels the picture is widened by
/// @return the widened crop, or none when the picture cannot be bounded
std::optional<TerrainCrop> widened_bounds(
    const formats::tnt::Map& map,
    const std::array<float, 12>& projection,
    uint32_t clip_width,
    uint32_t clip_height,
    float margin
) {
    const float widening = screen_ground_margin + margin;
    // The picture widened: its origin moved up and left by the widening.
    auto widened = projection;
    widened[3] += widening;
    widened[7] += widening;
    renderer_live_camera::TerrainBounds bounds{};
    constexpr float crop_margin = 2 * cell_edge;
    if (!renderer_live_camera::visible_terrain_bounds(
            widened,
            static_cast<float>(clip_width) + 2 * widening,
            static_cast<float>(clip_height) + 2 * widening,
            crop_margin,
            bounds
        ))
        return std::nullopt;
    const auto width = static_cast<float>(map.attribute_width * cell_edge);
    const auto height = static_cast<float>(map.attribute_height * cell_edge);
    return terrain_crop(
        map,
        static_cast<int32_t>(std::clamp(std::floor(bounds.min_x), 0.0F, width)),
        static_cast<int32_t>(std::clamp(std::floor(bounds.min_y), 0.0F, height)),
        static_cast<int32_t>(std::clamp(std::ceil(bounds.max_x), 0.0F, width)),
        static_cast<int32_t>(std::clamp(std::ceil(bounds.max_y), 0.0F, height))
    );
}

/// The crop of the frame's picture widened by margin pixels
/// (widened_bounds), and at least the frame's own crop.
///
/// @param in the frame and its picture
/// @param margin the pixels the picture is widened by
/// @return the widened crop, or none when the picture cannot be bounded
std::optional<TerrainCrop> widened_crop(const Inputs& in, float margin) {
    const auto bounds =
        widened_bounds(*in.map, in.clip_projection, in.clip_width, in.clip_height, margin);
    if (!bounds)
        return std::nullopt;
    const auto& wide = *bounds;
    const auto crop = terrain_crop(in);
    return TerrainCrop{
        std::min(crop.left, wide.left),
        std::min(crop.top, wide.top),
        std::max(crop.right, wide.right),
        std::max(crop.bottom, wide.bottom)
    };
}

/// Surveys the tiles the terrain pass will draw: for each height cell of
/// the crop, every tile its painted rows reach, as the pass clips its
/// triangles to tiles. Returns nothing when even every painted tile
/// position in reach would fit in full squares.
///
/// @param in the frame
/// @param room the atlas area the terrain may take
/// @return each tile key once, nearest the camera first
std::vector<TileDemand> terrain_demand(const Inputs& in, uint64_t room) {
    const auto& map = *in.map;
    const auto crop = terrain_crop(in);
    std::vector<TileDemand> demand;
    if (crop.right <= crop.left || crop.bottom <= crop.top)
        return demand;
    constexpr uint64_t full_area = uint64_t{full_terrain_square} * full_terrain_square;
    const auto reach =
        static_cast<uint64_t>((crop.right - crop.left) / tile_edge + 2) *
        static_cast<uint64_t>((crop.bottom - crop.top + maximum_terrain_row_shift) / tile_edge + 2);
    if (reach * full_area <= room)
        return demand;
    const int32_t columns = static_cast<int32_t>(map.tile_width);
    const int32_t rows = static_cast<int32_t>(map.tile_height);
    constexpr float unreached = -std::numeric_limits<float>::infinity();
    std::vector<float> nearest(static_cast<std::size_t>(columns) * rows, unreached);
    const auto& m = in.clip_projection;
    for (int32_t y = crop.top; y < crop.bottom; y += cell_edge)
        for (int32_t x = crop.left; x < crop.right; x += cell_edge) {
            const std::array corners{
                height_at(map, x, y),
                height_at(map, x + cell_edge, y),
                height_at(map, x + cell_edge, y + cell_edge),
                height_at(map, x, y + cell_edge)
            };
            const auto [low, high] = std::minmax_element(corners.begin(), corners.end());
            const float top = static_cast<float>(y) - *high * height_lift;
            const float bottom = static_cast<float>(y + cell_edge) - *low * height_lift;
            const int32_t column = x / tile_edge;
            const int32_t first = std::max(0, static_cast<int32_t>(std::floor(top / tile_edge)));
            const int32_t last = std::min(
                rows, std::max(first + 1, static_cast<int32_t>(std::ceil(bottom / tile_edge)))
            );
            if (column >= columns)
                continue;
            const float middle = (*low + *high) * 0.5F;
            const float depth = m[8] * (static_cast<float>(x) + cell_edge * 0.5F) + m[9] * middle +
                                m[10] * (static_cast<float>(y) + cell_edge * 0.5F) + m[11];
            for (int32_t row = first; row < last; ++row) {
                auto& point = nearest[static_cast<std::size_t>(row) * columns + column];
                point = std::max(point, depth);
            }
        }
    std::unordered_map<uint64_t, std::size_t> found;
    for (int32_t row = 0; row < rows; ++row)
        for (int32_t column = 0; column < columns; ++column) {
            const float depth = nearest[static_cast<std::size_t>(row) * columns + column];
            if (depth == unreached)
                continue;
            const auto key = terrain_tile_key(in, column * tile_edge, row * tile_edge);
            const auto [entry, added] = found.try_emplace(key, demand.size());
            if (added)
                demand.push_back({key, depth});
            else
                demand[entry->second].nearest = std::max(demand[entry->second].nearest, depth);
        }
    std::sort(demand.begin(), demand.end(), [](const TileDemand& a, const TileDemand& b) {
        return a.nearest != b.nearest ? a.nearest > b.nearest : a.key < b.key;
    });
    return demand;
}

/// Chooses each surveyed tile's square so that the terrain fits the room:
/// every tile full where they all fit; otherwise the nearest tiles in the
/// larger of two neighbouring sizes and the rest in the smaller, at the
/// largest pair that fits, the smallest squares at the least.
///
/// @param demand the surveyed tiles, nearest first
/// @param room the atlas area the terrain may take
/// @param holding the camera is moving (TerrainPlan::holding)
/// @return the plan
TerrainPlan plan_terrain(std::span<const TileDemand> demand, uint64_t room, bool holding) {
    TerrainPlan plan;
    plan.holding = holding;
    const uint64_t count = demand.size();
    if (count * full_terrain_square * full_terrain_square <= room)
        return plan;
    for (uint32_t larger = full_terrain_square; larger > smallest_terrain_square; larger /= 2) {
        const uint32_t smaller = larger / 2;
        const uint64_t smaller_area = uint64_t{smaller} * smaller;
        const uint64_t larger_area = uint64_t{larger} * larger;
        if (count * smaller_area > room && smaller != smallest_terrain_square)
            continue;
        const uint64_t larger_count =
            count * smaller_area > room
                ? 0
                : std::min(count, (room - count * smaller_area) / (larger_area - smaller_area));
        plan.squares.reserve(demand.size());
        for (std::size_t index = 0; index < demand.size(); ++index)
            plan.squares.emplace(demand[index].key, index < larger_count ? larger : smaller);
        plan.fallback = smaller;
        plan.larger = larger;
        break;
    }
    return plan;
}

/// @return the atlas area a plan's tiles take
uint64_t planned_area(std::span<const TileDemand> demand, const TerrainPlan& plan) {
    uint64_t area = 0;
    for (const auto& tile : demand) {
        const uint64_t edge = plan.square(tile.key);
        area += edge * edge;
    }
    return area;
}

/// Makes source what the frame's ground is drawn from (GroundSource), but
/// for the crop and the picture, which the terrain pass notes when it draws
/// the ground (ground_covers).
///
/// @param[in,out] source what the last frame's ground was drawn from
/// @param in the frame
/// @param scene the atlas
/// @param step the height cells a side of each drawn square of ground
/// @return true when it was already so
bool note_ground_source(GroundSource& source, const Inputs& in, const Scene& scene, int32_t step) {
    const auto& map = *in.map;
    bool same = true;
    const auto note = [&same](auto& kept, const auto& now) {
        if (!(kept == now)) {
            kept = now;
            same = false;
        }
    };
    const auto note_values = [&same](auto& kept, const auto& now) {
        if (!std::equal(kept.begin(), kept.end(), now.begin(), now.end())) {
            kept.assign(now.begin(), now.end());
            same = false;
        }
    };
    note(source.map, in.map);
    note(source.map_width, map.attribute_width);
    note(source.map_height, map.attribute_height);
    note(source.tile_width, map.tile_width);
    note(source.tile_height, map.tile_height);
    note(source.tile_art_bytes, map.tile_palette_indices.size());
    const auto cells = static_cast<std::size_t>(map.attribute_width) * map.attribute_height;
    bool heights_same = source.heights.size() == cells;
    for (std::size_t cell = 0; cell < cells && heights_same; ++cell)
        heights_same = source.heights[cell] == map.attributes[cell].height;
    if (!heights_same) {
        source.heights.resize(cells);
        for (std::size_t cell = 0; cell < cells; ++cell)
            source.heights[cell] = map.attributes[cell].height;
        const auto [lowest, highest] =
            std::minmax_element(source.heights.begin(), source.heights.end());
        source.lowest = cells == 0 ? 0 : *lowest;
        source.highest = cells == 0 ? 0 : *highest;
        same = false;
    }
    note_values(source.tile_indices, map.tile_indices);
    note(source.step, step);
    note(
        source.distant, std::isfinite(in.detail_scale) && in.detail_scale > 0 && in.detail_scale < 1
    );
    note(source.fog_divisions, fog_divisions(in));
    note(source.texture_width, scene.texture_width);
    note(source.texture_height, scene.texture_height);
    // The ground reads the sight only as fog_at reads each sight cell, so
    // that a count of sight stamps changing in a cell still in sight
    // changes nothing drawn.
    note(source.sight_width, in.sight.width);
    note(source.sight_height, in.sight.height);
    note(source.mapping, in.sight.mapping);
    note(source.dithered, in.sight.dithered);
    constexpr int32_t fog_edge = present::world_renderer::fog_cell_pixels;
    const auto sight_cells = static_cast<std::size_t>(std::max(in.sight.width, 0)) *
                             static_cast<std::size_t>(std::max(in.sight.height, 0));
    if (source.fog.size() != sight_cells) {
        source.fog.assign(sight_cells, std::numeric_limits<uint8_t>::max());
        same = false;
    }
    for (int32_t row = 0; row < in.sight.height; ++row)
        for (int32_t column = 0; column < in.sight.width; ++column) {
            const auto fog =
                static_cast<uint8_t>(fog_at(in.sight, column * fog_edge, row * fog_edge));
            auto& kept = source.fog[static_cast<std::size_t>(row) * in.sight.width + column];
            if (kept != fog) {
                kept = fog;
                same = false;
            }
        }
    return same;
}

/// The furthest, in a picture's pixels, that any ground the picture shows
/// within screen_ground_margin has moved since a picture of the same size
/// drawn by another projection.
///
/// A box of ground the picture shows within screen_ground_margin holds a
/// point that lies, at its height, under the picture so widened. The
/// projection is affine, so such ground has moved furthest at one of the
/// widened picture's corners at the map's lowest or highest ground.
///
/// @param from the other picture's projection
/// @param to the picture's projection
/// @param clip_width the pictures' width
/// @param clip_height their height
/// @param lowest the map's lowest ground
/// @param highest its highest
/// @return the distance; infinity where it is not a number
float picture_shift(
    const std::array<float, 12>& from,
    const std::array<float, 12>& to,
    uint32_t clip_width,
    uint32_t clip_height,
    uint8_t lowest,
    uint8_t highest
) {
    constexpr float unbounded = std::numeric_limits<float>::infinity();
    const float right = static_cast<float>(clip_width) + screen_ground_margin;
    const float bottom = static_cast<float>(clip_height) + screen_ground_margin;
    float furthest = 0;
    for (const float height : {float(lowest), float(highest)})
        for (const float screen_y : {-screen_ground_margin, bottom})
            for (const float screen_x : {-screen_ground_margin, right}) {
                float x{}, z{};
                if (!renderer_live_camera::world_at_height(to, screen_x, screen_y, height, x, z))
                    return unbounded;
                const std::array point{x, height, z, 1.0F};
                for (const std::size_t row : {std::size_t{0}, std::size_t{4}}) {
                    float shift = 0;
                    for (std::size_t column = 0; column < point.size(); ++column)
                        shift += (to[row + column] - from[row + column]) * point[column];
                    // A shift that is not a number is unbounded.
                    if (!(std::abs(shift) <= furthest))
                        furthest = std::isnan(shift) ? unbounded : std::abs(shift);
                }
            }
    return furthest;
}

/// Tells whether the ground drawn from source holds every cell the frame's
/// picture shows: it was drawn for the same picture, or for one of the
/// same size whose margin (GroundSource::picture_margin) is wider than any
/// ground the frame's picture shows has moved in the picture since
/// (picture_shift). Moved no further than the margin, a box of ground the
/// frame shows was shown within the margin more by the kept ground's
/// picture, lies in its crop (widened_bounds), and was drawn.
///
/// @param source what the kept ground was drawn from
/// @param in the frame
/// @param crop the frame's crop
/// @return true when the frame may take the kept ground
bool ground_covers(const GroundSource& source, const Inputs& in, const TerrainCrop& crop) {
    if (source.clip_width != in.clip_width || source.clip_height != in.clip_height)
        return false;
    if (source.picture_margin == 0)
        return source.clip_projection == in.clip_projection &&
               source.crop == std::array{crop.left, crop.top, crop.right, crop.bottom};
    return picture_shift(
               source.clip_projection,
               in.clip_projection,
               in.clip_width,
               in.clip_height,
               source.lowest,
               source.highest
           ) <= source.picture_margin;
}

/// A ground revision no scene has had before.
uint64_t new_ground_revision() {
    static std::atomic<uint64_t> last{};
    return ++last;
}

/// Tells whether every tile the kept ground draws would come out of the
/// atlas where it did: still packed in its square, no smaller than the plan
/// lets it keep, from the map's art as it is now.
///
/// @param in the frame
/// @param scene the kept ground and the atlas
/// @param plan the square of each tile
/// @return true when none of its tiles would be packed again
bool ground_tiles_hold(const Inputs& in, const Scene& scene, const TerrainPlan& plan) {
    constexpr std::size_t tile_bytes = formats::tnt::layout::tile_bytes;
    const auto& art = in.map->tile_palette_indices;
    for (const auto& [key, rectangle] : scene.ground_tiles) {
        // A key is its tile's number by three, and its fog.
        const auto start = static_cast<std::size_t>(key / 3) * tile_bytes;
        const auto cached = scene.terrain_materials.find(key);
        if (cached == scene.terrain_materials.end() || !(cached->second.rectangle == rectangle) ||
            !plan.keeps(key, rectangle.width) || start + tile_bytes > art.size() ||
            std::memcmp(cached->second.pixels.data(), art.data() + start, tile_bytes) != 0)
            return false;
    }
    return true;
}

/// What a ground's rows are drawn from: its source, with the crop, the
/// picture and the margin it is drawn for (GroundSource), the fog of each
/// sight cell as the source holds it, and the pixels beyond the picture's
/// edges drawn.
struct GroundDraw {
    const GroundSource& source;
    FogCells cells{};
    float margin{};
};

/// @return what a ground drawn from source is drawn from
GroundDraw ground_draw(const GroundSource& source) {
    return {
        source,
        {source.fog, source.sight_width, source.sight_height, source.mapping, source.dithered},
        screen_ground_margin + source.picture_margin
    };
}

/// Fills in a terrain vertex's place in its tile's square of the atlas.
///
/// @param[in,out] vertex the vertex
/// @param rect the tile's square
/// @param x the vertex's map x
/// @param painted_y its painted row
/// @param tile_x the tile's left edge
/// @param tile_y its top edge
/// @param atlas_width the atlas's width
/// @param atlas_height its height
void place_in_square(
    Vertex& vertex,
    const AtlasRect& rect,
    float x,
    float painted_y,
    int32_t tile_x,
    int32_t tile_y,
    uint32_t atlas_width,
    uint32_t atlas_height
) {
    // A smaller square holds the tile at a fraction of its pixels.
    const bool full = rect.width == full_terrain_square;
    const float scale = static_cast<float>(rect.width) / full_terrain_square;
    vertex.u = (full ? rect.x + x - tile_x : rect.x + (x - tile_x) * scale) / atlas_width;
    vertex.v =
        (full ? rect.y + painted_y - tile_y : rect.y + (painted_y - tile_y) * scale) / atlas_height;
    vertex.terrain_x = static_cast<float>(rect.x);
    vertex.terrain_y = static_cast<float>(rect.y);
    vertex.terrain_size = static_cast<float>(rect.width / 2);
}

/// Emits a ground's terrain cells from its first painted row to before its
/// end, with tile UVs and a separate fog material per cell, as the rows of
/// the whole crop are drawn one after another: rows of whole tiles, or of
/// the squares drawn far out, list the same triangles alone as among the
/// others.
///
/// @param draw what the ground is drawn from
/// @param first the first row: the crop's top, or the top of a row of tiles
///     (of squares far out) within it
/// @param end the row after the last, at most the crop's bottom
/// @param[in,out] samples the rows' height samples and their fog, held while
///     they are drawn
/// @param[in,out] merged_columns the columns of tiles drawn whole
/// @param tiles finds each tile drawn (tile(x, y)), places each vertex in it
///     (place(vertex, point, x, y, tile)) and lists each triangle
///     (triangle(triangle))
template <typename Tiles>
void draw_ground_rows(
    const GroundDraw& draw,
    int32_t first,
    int32_t end,
    std::vector<std::array<float, 2>>& samples,
    std::vector<bool>& merged_columns,
    Tiles& tiles
) {
    const auto& source = draw.source;
    const auto& cells = draw.cells;
    const int32_t left = source.crop[0];
    const int32_t right = source.crop[2];
    const int32_t bottom = source.crop[3];
    const float margin = draw.margin;
    // Whether a box of ground lies wholly beyond the picture.
    const auto off_screen = [&](float x0, float y0, float x1, float y1, float low, float high) {
        return ground_off_screen(
            source.clip_projection,
            source.clip_width,
            source.clip_height,
            x0,
            y0,
            x1,
            y1,
            low,
            high,
            margin
        );
    };
    // Geometry follows world X/Z. Artwork is indexed by its classic projected
    // position and can cross several source tiles on one steep terrain face.
    const auto emit = [&](std::span<const Vertex> polygon_corners) {
        for (std::size_t corner = 1; corner + 1 < polygon_corners.size(); ++corner) {
            const std::array vertices{
                polygon_corners[0], polygon_corners[corner], polygon_corners[corner + 1]
            };
            GroundPolygon points{};
            points.count = 3;
            float min_x = vertices[0].x, max_x = min_x;
            float min_y = vertices[0].z - vertices[0].height * height_lift, max_y = min_y;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const auto& v = vertices[i];
                points.points[i] = {v.x, v.z - v.height * height_lift, v.height, v.brightness};
                min_x = std::min(min_x, points.points[i].x);
                max_x = std::max(max_x, points.points[i].x);
                min_y = std::min(min_y, points.points[i].y);
                max_y = std::max(max_y, points.points[i].y);
            }
            const int32_t first_x =
                std::max(0, static_cast<int32_t>(std::floor(min_x / tile_edge)) * tile_edge);
            const int32_t first_y =
                std::max(0, static_cast<int32_t>(std::floor(min_y / tile_edge)) * tile_edge);
            const int32_t last_x = std::min(
                static_cast<int32_t>(source.tile_width) * tile_edge,
                static_cast<int32_t>(std::ceil(max_x / tile_edge)) * tile_edge
            );
            const int32_t last_y = std::min(
                static_cast<int32_t>(source.tile_height) * tile_edge,
                std::max(
                    first_y + tile_edge,
                    static_cast<int32_t>(std::ceil(max_y / tile_edge)) * tile_edge
                )
            );
            const auto emit_tile = [&](const GroundPolygon& clipped, int32_t tx, int32_t ty) {
                if (clipped.count < 3)
                    return;
                const auto tile = tiles.tile(tx, ty);
                std::array<Vertex, 8> output{};
                for (std::size_t index = 0; index < clipped.count; ++index) {
                    const auto& point = clipped.points[index];
                    output[index] = {
                        point.x,
                        point.height,
                        point.y + point.height * height_lift,
                        0,
                        0,
                        point.brightness
                    };
                    auto& vertex = output[index];
                    tiles.place(vertex, point, tx, ty, tile);
                    const auto ground =
                        fog_of_ground(cells, cells.dithered, point.x, point.y, point.brightness);
                    vertex.terrain_known = ground.known;
                    vertex.terrain_grey = ground.grey;
                }
                for (std::size_t index = 1; index + 1 < clipped.count; ++index) {
                    const auto& a = output[0];
                    const auto& b = output[index];
                    const auto& c = output[index + 1];
                    const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
                    if (std::abs(area) > 0.000001F)
                        tiles.triangle(std::array{a, b, c});
                }
            };
            if (min_x >= first_x && min_y >= first_y && max_x <= first_x + tile_edge &&
                max_y <= first_y + tile_edge && first_x < last_x && first_y < last_y) {
                emit_tile(points, first_x, first_y);
                continue;
            }
            for (int32_t ty = first_y; ty < last_y; ty += tile_edge)
                for (int32_t tx = first_x; tx < last_x; tx += tile_edge) {
                    auto clipped = clip_ground_fixed(points, true, float(tx), true);
                    clipped = clip_ground_fixed(clipped, true, float(tx + tile_edge), false);
                    clipped = clip_ground_fixed(clipped, false, float(ty), true);
                    clipped = clip_ground_fixed(clipped, false, float(ty + tile_edge), false);
                    emit_tile(clipped, tx, ty);
                }
        }
    };
    if (source.step > 1) {
        // Seen from this far out, a cell is a pixel or two: the ground is
        // drawn from every step-th height sample, each square as four
        // triangles about its middle sample, or two where all five lie
        // level and in even fog. Each still samples its own tiles' art.
        const int32_t edge = cell_edge * source.step;
        const int32_t map_width = static_cast<int32_t>(source.map_width) * cell_edge;
        const int32_t map_height = static_cast<int32_t>(source.map_height) * cell_edge;
        const auto ground = [&](int32_t x, int32_t y) {
            const float h = height_at(source, x, y);
            return Vertex{
                static_cast<float>(x),
                h,
                static_cast<float>(y),
                0,
                0,
                brightness_at(cells, float(x), y - h * height_lift)
            };
        };
        for (int32_t y = first / edge * edge; y < end; y += edge)
            for (int32_t x = left / edge * edge; x < right; x += edge) {
                const int32_t far_x = std::min(x + edge, map_width);
                const int32_t far_y = std::min(y + edge, map_height);
                const std::array heights{
                    height_at(source, x, y),
                    height_at(source, far_x, y),
                    height_at(source, far_x, far_y),
                    height_at(source, x, far_y),
                    height_at(source, (x + far_x) / 2, (y + far_y) / 2)
                };
                const auto [low, high] = std::minmax_element(heights.begin(), heights.end());
                if (off_screen(float(x), float(y), float(far_x), float(far_y), *low, *high))
                    continue;
                const std::array corners{
                    ground(x, y), ground(far_x, y), ground(far_x, far_y), ground(x, far_y)
                };
                const auto middle = ground((x + far_x) / 2, (y + far_y) / 2);
                const bool level =
                    std::all_of(corners.begin(), corners.end(), [&](const Vertex& v) {
                        return v.height == middle.height &&
                               std::abs(v.brightness - middle.brightness) < 0.001F;
                    });
                if (level) {
                    emit(corners);
                    continue;
                }
                for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                    const std::array fan{
                        middle, corners[corner], corners[(corner + 1) % corners.size()]
                    };
                    emit(fan);
                }
            }
        return;
    }
    // The height samples of the rows and of a tile's further down, which a
    // tile drawn whole reads.
    const int32_t columns = (right - left + cell_edge - 1) / cell_edge + 1;
    const int32_t rows = (end - first + tile_edge + cell_edge - 1) / cell_edge + 1;
    samples.assign(static_cast<std::size_t>(columns) * rows, {0, -1});
    const auto sample = [&](int32_t x, int32_t y) {
        const int32_t row = (y - first) / cell_edge;
        const int32_t column = (x - left) / cell_edge;
        const bool held = y >= first && x >= left && row < rows && column < columns;
        const auto index =
            static_cast<std::size_t>(row) * columns + static_cast<std::size_t>(column);
        if (held && samples[index][1] >= 0)
            return samples[index];
        const float height = height_at(source, x, y);
        const std::array result{height, brightness_at(cells, float(x), y - height * height_lift)};
        if (held)
            samples[index] = result;
        return result;
    };
    const bool distant = source.distant;
    // Whether the square of ground at x, y, edge pixels a side, lies wholly
    // beyond the picture, by the heights at every height sample in it.
    const auto square_off_screen = [&](int32_t x, int32_t y, int32_t edge) {
        float low = std::numeric_limits<float>::infinity();
        float high = -low;
        for (int32_t dy = 0; dy <= edge; dy += cell_edge)
            for (int32_t dx = 0; dx <= edge; dx += cell_edge) {
                const float height = height_at(source, x + dx, y + dy);
                low = std::min(low, height);
                high = std::max(high, height);
            }
        return off_screen(float(x), float(y), float(x + edge), float(y + edge), low, high);
    };
    merged_columns.assign(static_cast<std::size_t>(columns), false);
    for (int32_t y = first; y < end; y += cell_edge) {
        if (y % tile_edge == 0)
            std::fill(merged_columns.begin(), merged_columns.end(), false);
        for (int32_t x = left; x < right; x += cell_edge) {
            if (y % tile_edge != 0 && merged_columns[(x - left) / cell_edge])
                continue;
            const auto vertex = [&](int32_t dx, int32_t dy) {
                const auto values = sample(x + dx, y + dy);
                const float h = values[0];
                return Vertex{
                    static_cast<float>(x + dx), h, static_cast<float>(y + dy), 0, 0, values[1]
                };
            };
            // A complete flat or planar tile needs no internal height edges.
            // Keep every original cell wherever relief or fog changes within it.
            if (distant && x % tile_edge == 0 && y % tile_edge == 0 && x + tile_edge <= right &&
                y + tile_edge <= bottom) {
                // A tile wholly beyond the picture is left out, merged or
                // not: each of its cells is then left out in turn.
                if (square_off_screen(x, y, tile_edge))
                    continue;
                const auto a = sample(x, y), b = sample(x + tile_edge, y);
                const auto c = sample(x + tile_edge, y + tile_edge), d = sample(x, y + tile_edge);
                bool planar = !twisted_cell(a[0], b[0], c[0], d[0]);
                for (int32_t row = 0; row <= 2; ++row)
                    for (int32_t column = 0; column <= 2; ++column) {
                        const auto point = sample(x + column * cell_edge, y + row * cell_edge);
                        const float u = column * 0.5F, v = row * 0.5F;
                        const float height = u >= v ? a[0] + u * (b[0] - a[0]) + v * (c[0] - b[0])
                                                    : a[0] + v * (d[0] - a[0]) + u * (c[0] - d[0]);
                        planar = planar && std::abs(point[0] - height) < 0.00001F &&
                                 std::abs(point[1] - a[1]) < 0.00001F;
                    }
                if (planar) {
                    const std::array tile_corners{
                        vertex(0, 0),
                        vertex(tile_edge, 0),
                        vertex(tile_edge, tile_edge),
                        vertex(0, tile_edge)
                    };
                    emit(tile_corners);
                    merged_columns[(x - left) / cell_edge] = true;
                    merged_columns[(x - left) / cell_edge + 1] = true;
                    x += cell_edge;
                    continue;
                }
            }
            if (square_off_screen(x, y, cell_edge))
                continue;
            const std::array corners{
                vertex(0, 0),
                vertex(cell_edge, 0),
                vertex(cell_edge, cell_edge),
                vertex(0, cell_edge)
            };
            // A twisted cell is four triangles about its middle at the
            // simulation's height, so that the ground drawn stays where units
            // stand on it (twisted_cell).
            std::optional<Vertex> middle;
            if (twisted_cell(
                    corners[0].height, corners[1].height, corners[2].height, corners[3].height
                )) {
                const float h = cell_middle_height(source, x, y);
                const float middle_x = static_cast<float>(x + cell_edge / 2);
                const float middle_y = static_cast<float>(y + cell_edge / 2);
                middle = Vertex{
                    middle_x,
                    h,
                    middle_y,
                    0,
                    0,
                    brightness_at(cells, middle_x, middle_y - h * height_lift)
                };
            }
            const auto [minimum, maximum] = std::minmax_element(
                corners.begin(), corners.end(), [](const Vertex& a, const Vertex& b) {
                    return a.brightness < b.brightness;
                }
            );
            const bool even_fog =
                maximum->brightness - minimum->brightness < 0.001F &&
                (!middle || std::abs(middle->brightness - minimum->brightness) < 0.001F);
            if (even_fog && !middle) {
                emit(corners);
                continue;
            }
            if (even_fog) {
                for (std::size_t corner = 0; corner < corners.size(); ++corner) {
                    const std::array fan{
                        *middle, corners[corner], corners[(corner + 1) % corners.size()]
                    };
                    emit(fan);
                }
                continue;
            }
            // Refine only sight transitions, retaining each original terrain plane.
            constexpr int32_t maximum_divisions = 4;
            const int32_t divisions = source.fog_divisions;
            const auto triangle = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
                const auto make_point = [&](int32_t column, int32_t row) {
                    const float u = static_cast<float>(column) / divisions;
                    const float v = static_cast<float>(row) / divisions;
                    const auto blend = [u, v](float a, float b, float c) {
                        return a + u * (b - a) + v * (c - a);
                    };
                    Vertex result{
                        blend(a.x, b.x, c.x),
                        blend(a.height, b.height, c.height),
                        blend(a.z, b.z, c.z),
                        blend(a.u, b.u, c.u),
                        blend(a.v, b.v, c.v)
                    };
                    result.brightness =
                        brightness_at(cells, result.x, result.z - result.height * height_lift);
                    return result;
                };
                constexpr int32_t row_stride = maximum_divisions + 1;
                std::array<Vertex, row_stride * row_stride> points{};
                for (int32_t row = 0; row <= divisions; ++row)
                    for (int32_t column = 0; column <= divisions - row; ++column)
                        points[row * row_stride + column] = make_point(column, row);
                const auto point = [&](int32_t column, int32_t row) -> const Vertex& {
                    return points[row * row_stride + column];
                };
                for (int32_t row = 0; row < divisions; ++row)
                    for (int32_t column = 0; column < divisions - row; ++column) {
                        const std::array first_triangle{
                            point(column, row), point(column + 1, row), point(column, row + 1)
                        };
                        emit(first_triangle);
                        if (column + row + 1 < divisions) {
                            const std::array second{
                                point(column + 1, row),
                                point(column + 1, row + 1),
                                point(column, row + 1)
                            };
                            emit(second);
                        }
                    }
            };
            if (middle) {
                for (std::size_t corner = 0; corner < corners.size(); ++corner)
                    triangle(*middle, corners[corner], corners[(corner + 1) % corners.size()]);
            } else {
                triangle(corners[0], corners[1], corners[2]);
                triangle(corners[0], corners[2], corners[3]);
            }
        }
    }
}

/// Lists a ground's triangles straight into the frame, packing each tile
/// as the ground first draws it: the ground drawn row after row on the
/// thread that builds the frame, which gives up where the frame's geometry
/// budget runs out (GeometryFull) or a tile cannot be drawn, with the tiles
/// met before then packed.
class PackingTiles {
  public:

    /// @param in the frame
    /// @param[in,out] scene the atlas and terrain materials
    /// @param plan the square of each tile
    /// @param draw what the ground is drawn from
    /// @param[out] vertices where the triangles are listed, empty
    PackingTiles(
        const Inputs& in,
        Scene& scene,
        const TerrainPlan& plan,
        const GroundDraw& draw,
        std::vector<Vertex>& vertices
    )
        : in_(in), scene_(scene), plan_(plan), draw_(draw), vertices_(vertices) {}

    /// @return the square of the tile at a painted tile position, packed
    ///     now when this ground meets it first and has no square for it
    AtlasRect tile(int32_t tile_x, int32_t tile_y) {
        const auto& map = *in_.map;
        // Room for every tile of the largest map at least once in full and
        // once black, so that a wide view of one does not empty the cache
        // each frame.
        constexpr std::size_t maximum_cached_tiles = 32768;
        const auto tile = draw_.source.tile_indices.at(
            static_cast<std::size_t>(tile_y / tile_edge) * draw_.source.tile_width +
            tile_x / tile_edge
        );
        const auto fog = draw_.cells.at(tile_x, tile_y) == Fog::black ? Fog::black : Fog::seen;
        const uint64_t key = static_cast<uint64_t>(tile) * 3 + static_cast<uint64_t>(fog);
        // A tile's texels are reused by all four height cells and every
        // repeated tile: tiles_ holds each key's square once it is found.
        auto found = tiles_.find(key);
        if (found != tiles_.end())
            return found->second;
        const auto start = static_cast<std::size_t>(tile) * formats::tnt::layout::tile_bytes;
        if (start + formats::tnt::layout::tile_bytes > map.tile_palette_indices.size())
            throw std::runtime_error("experimental terrain tile is out of bounds");
        const auto pixels =
            std::span(map.tile_palette_indices).subspan(start, formats::tnt::layout::tile_bytes);
        // A tile packed larger in an earlier frame keeps its detail.
        auto& scene = scene_;
        auto cached = scene.terrain_materials.find(key);
        if (cached == scene.terrain_materials.end() ||
            !plan_.keeps(key, cached->second.rectangle.width) ||
            std::memcmp(cached->second.pixels.data(), pixels.data(), pixels.size()) != 0) {
            // The square it leaves is given back: nothing this pass has
            // drawn samples it, and the kept ground is drawn again.
            if (cached != scene.terrain_materials.end() && !scene.ground_kept) {
                give_back_terrain_square(scene, cached->second.rectangle);
                scene.terrain_materials.erase(cached);
            }
            const auto rectangle = terrain_material(
                scene, *in_.palette, scene.terrain_grey_palette, pixels, fog, plan_.packed(key)
            );
            if (scene.terrain_materials.size() >= maximum_cached_tiles && !scene.ground_kept)
                drop_terrain_materials(scene, scene.terrain_passes);
            scene.frame.aircraft_shadows.clear();
            auto& entry = scene.terrain_materials[key];
            std::copy(pixels.begin(), pixels.end(), entry.pixels.begin());
            entry.rectangle = rectangle;
            cached = scene.terrain_materials.find(key);
        }
        cached->second.drawn = scene.terrain_passes;
        found = tiles_.emplace(key, cached->second.rectangle).first;
        return found->second;
    }

    /// Places a vertex in its tile's square.
    void place(
        Vertex& vertex, const GroundPoint& point, int32_t tile_x, int32_t tile_y, AtlasRect square
    ) const {
        place_in_square(
            vertex,
            square,
            point.x,
            point.y,
            tile_x,
            tile_y,
            scene_.texture_width,
            scene_.texture_height
        );
    }

    /// Lists a triangle after checking the frame's geometry budget.
    void triangle(const std::array<Vertex, 3>& triangle) {
        if (vertices_.size() + triangle.size() > maximum_vertices)
            throw GeometryFull();
        vertices_.insert(vertices_.end(), triangle.begin(), triangle.end());
    }

    /// @return each tile key drawn, with its square
    [[nodiscard]] const std::unordered_map<uint64_t, AtlasRect>& tiles() const { return tiles_; }

  private:

    const Inputs& in_;
    Scene& scene_;
    const TerrainPlan& plan_;
    const GroundDraw& draw_;
    std::vector<Vertex>& vertices_;
    std::unordered_map<uint64_t, AtlasRect> tiles_;
};

/// A band of ground that holds more vertices than the frame may.
class BandFull : public std::exception {
  public:

    [[nodiscard]] const char* what() const noexcept override {
        return "experimental ground band exceeds the geometry budget";
    }
};

/// Lists a band of ground's triangles into the band, noting each tile it
/// draws for the thread that builds the frame to pack, and each vertex's
/// place in its tile for it to place (GroundBand).
class BandTiles {
  public:

    /// @param draw what the ground is drawn from
    /// @param[in,out] band the band, empty
    BandTiles(const GroundDraw& draw, GroundBand& band) : draw_(draw), band_(band) {}

    /// @return the place among the band's tiles of the tile at a painted
    ///     tile position
    uint32_t tile(int32_t tile_x, int32_t tile_y) {
        const auto& source = draw_.source;
        const auto tile = source.tile_indices.at(
            static_cast<std::size_t>(tile_y / tile_edge) * source.tile_width + tile_x / tile_edge
        );
        const auto fog = draw_.cells.at(tile_x, tile_y) == Fog::black ? Fog::black : Fog::seen;
        const uint64_t key = static_cast<uint64_t>(tile) * 3 + static_cast<uint64_t>(fog);
        // The cells of a row meet the same tile one after another.
        if (key == last_key_ && !band_.tiles.empty())
            return last_place_;
        const auto [entry, added] =
            band_.places.try_emplace(key, static_cast<uint32_t>(band_.tiles.size()));
        if (added) {
            if (static_cast<std::size_t>(tile) * formats::tnt::layout::tile_bytes +
                    formats::tnt::layout::tile_bytes >
                source.tile_art_bytes) {
                band_.places.erase(entry);
                throw std::runtime_error("experimental terrain tile is out of bounds");
            }
            band_.tiles.push_back(key);
        }
        last_key_ = key;
        last_place_ = entry->second;
        return entry->second;
    }

    /// Notes a vertex's place in its tile, to be placed in its square.
    static void
    place(Vertex& vertex, const GroundPoint& point, int32_t tile_x, int32_t tile_y, uint32_t tile) {
        vertex.u = static_cast<float>(tile_x);
        vertex.v = point.y;
        vertex.terrain_x = static_cast<float>(tile_y);
        vertex.terrain_size = static_cast<float>(tile);
    }

    /// Lists a triangle, unless the band would hold more than the frame may.
    void triangle(const std::array<Vertex, 3>& triangle) {
        auto& vertices = band_.vertices;
        if (vertices.size() + triangle.size() > maximum_vertices)
            throw BandFull();
        vertices.insert(vertices.end(), triangle.begin(), triangle.end());
    }

  private:

    const GroundDraw& draw_;
    GroundBand& band_;
    uint64_t last_key_{};
    uint32_t last_place_{};
};

/// Splits the rows of a ground's crop into its bands (GroundBand): a row of
/// tiles each, or a row of the squares drawn far out.
///
/// @param source what the ground is drawn from, with its crop
/// @param[in,out] bands the bands, as many as there are rows
void plan_ground_bands(const GroundSource& source, std::vector<GroundBand>& bands) {
    const int32_t top = source.crop[1];
    const int32_t bottom = source.crop[3];
    std::size_t count = 0;
    const auto add = [&](int32_t first, int32_t end) {
        if (bands.size() <= count)
            bands.resize(count + 1);
        bands[count].top = first;
        bands[count].bottom = std::min(end, bottom);
        ++count;
    };
    if (source.step > 1) {
        const int32_t edge = cell_edge * source.step;
        for (int32_t y = top / edge * edge; y < bottom; y += edge)
            add(y, y + edge);
    } else {
        for (int32_t y = top; y < bottom; y = (y / tile_edge + 1) * tile_edge)
            add(y, (y / tile_edge + 1) * tile_edge);
    }
    bands.resize(count);
}

/// Draws a band of ground into its list (GroundBand), on any thread: it
/// reads only what the ground is drawn from.
///
/// @param draw what the ground is drawn from
/// @param[in,out] band the band, its rows planned (plan_ground_bands)
/// @param timing the stage timers measure
void draw_ground_band(const GroundDraw& draw, GroundBand& band, bool timing) {
    const uint64_t start = timing ? bench_timers::now_ns() : 0;
    band.vertices.clear();
    band.tiles.clear();
    band.places.clear();
    band.squares.clear();
    band.failure = nullptr;
    band.overflow = false;
    BandTiles tiles(draw, band);
    try {
        draw_ground_rows(draw, band.top, band.bottom, band.samples, band.merged_columns, tiles);
    } catch (const BandFull&) {
        band.overflow = true;
    } catch (...) {
        band.failure = std::current_exception();
    }
    band.nanoseconds = timing ? bench_timers::now_ns() - start : 0;
}

/// Packs one tile of a ground, as PackingTiles::tile takes it: still
/// packed in a large enough square from the same art, or packed now, and
/// marked drawn by the terrain pass.
///
/// @param in the frame
/// @param[in,out] scene the atlas and terrain materials
/// @param plan the square of each tile
/// @param key the tile's key
/// @param room how a moving camera's ground finds room; none for a settled
///     camera's
/// @param pass the terrain pass that draws it
/// @return its square
AtlasRect pack_ground_tile(
    const Inputs& in,
    Scene& scene,
    const TerrainPlan& plan,
    uint64_t key,
    MovingRoom* room,
    uint64_t pass
) {
    constexpr std::size_t tile_bytes = formats::tnt::layout::tile_bytes;
    constexpr std::size_t maximum_cached_tiles = 32768;
    const auto pixels = std::span(in.map->tile_palette_indices)
                            .subspan(static_cast<std::size_t>(key / 3) * tile_bytes, tile_bytes);
    const auto fog = static_cast<Fog>(key % 3);
    auto cached = scene.terrain_materials.find(key);
    if (cached == scene.terrain_materials.end() ||
        !plan.keeps(key, cached->second.rectangle.width) ||
        std::memcmp(cached->second.pixels.data(), pixels.data(), pixels.size()) != 0) {
        if (cached != scene.terrain_materials.end() && !scene.ground_kept) {
            give_back_terrain_square(scene, cached->second.rectangle);
            scene.terrain_materials.erase(cached);
        }
        const auto rectangle = terrain_material(
            scene, *in.palette, scene.terrain_grey_palette, pixels, fog, plan.packed(key), room
        );
        if (scene.terrain_materials.size() >= maximum_cached_tiles && !scene.ground_kept)
            drop_terrain_materials(scene, scene.terrain_passes);
        scene.frame.aircraft_shadows.clear();
        auto& entry = scene.terrain_materials[key];
        std::copy(pixels.begin(), pixels.end(), entry.pixels.begin());
        entry.rectangle = rectangle;
        cached = scene.terrain_materials.find(key);
    }
    cached->second.drawn = pass;
    return cached->second.rectangle;
}

/// Places every vertex of a ground's bands in its tile's square and lists
/// them in order, each band on a drawing thread of its own.
///
/// @param in the frame
/// @param scene the atlas
/// @param[in,out] bands the ground's bands, drawn
/// @param squares each tile key's square
/// @param[out] vertices the ground's vertices; what it held before is
///     overwritten in place
void place_ground_bands(
    const Inputs& in,
    const Scene& scene,
    std::span<GroundBand> bands,
    const std::unordered_map<uint64_t, AtlasRect>& squares,
    std::vector<Vertex>& vertices
) {
    const auto atlas_width = scene.texture_width;
    const auto atlas_height = scene.texture_height;
    std::vector<std::size_t> starts(bands.size() + 1);
    for (std::size_t index = 0; index < bands.size(); ++index) {
        auto& band = bands[index];
        band.squares.resize(band.tiles.size());
        for (std::size_t tile = 0; tile < band.tiles.size(); ++tile)
            band.squares[tile] = squares.at(band.tiles[tile]);
        starts[index + 1] = starts[index] + band.vertices.size();
    }
    reserve_room(vertices, starts.back());
    vertices.resize(starts.back());
    platform::job_pool::run_bands(
        in.pool, static_cast<uint32_t>(bands.size()), [&](uint32_t index) {
            const auto& band = bands[index];
            auto listed = vertices.begin() + static_cast<std::ptrdiff_t>(starts[index]);
            for (auto vertex : band.vertices) {
                place_in_square(
                    vertex,
                    band.squares[static_cast<std::size_t>(vertex.terrain_size)],
                    vertex.x,
                    vertex.v,
                    static_cast<int32_t>(vertex.u),
                    static_cast<int32_t>(vertex.terrain_x),
                    atlas_width,
                    atlas_height
                );
                *listed++ = vertex;
            }
        }
    );
}

/// @param scene the atlas
/// @param left_rows the rows the terrain leaves the other materials
/// @return the full terrain squares the atlas's rows still hold for the
///     terrain (take_terrain_square)
std::size_t terrain_row_squares(const Scene& scene, uint32_t left_rows) {
    const uint32_t limit = kept_rows(scene) - std::min(kept_rows(scene), left_rows);
    std::size_t squares = 0;
    if (scene.next_y + full_terrain_square <= limit)
        squares += (scene.texture_width - std::min(scene.texture_width, scene.next_x)) /
                   full_terrain_square;
    const uint32_t below = scene.next_y + scene.row_height;
    if (below < limit)
        squares += std::size_t{(limit - below) / full_terrain_square} *
                   (scene.texture_width / full_terrain_square);
    return squares;
}

/// Packs the tiles a ground's bands draw, as drawing them row after row
/// packs them, and lists the bands' vertices, each placed in its tile's
/// square.
///
/// The tiles are taken in the order the ground first draws them, each as
/// PackingTiles::tile takes it (pack_ground_tile). The atlas then holds
/// what drawing the ground row after row leaves it holding. A moving
/// camera's ground (MovingRoom) marks its tiles drawn first. One that
/// finds the atlas pressed, having had to give back the squares of tiles
/// it does not draw or to pack its own smaller, or that was to have been
/// packed ahead, leaves room for the grounds packed ahead of it: it packs
/// more of its tiles smaller until spare squares of the size it packs, and
/// the rows it leaves them, would hold a share of its tiles
/// (ahead_room_share).
///
/// @param in the frame
/// @param[in,out] scene the atlas and terrain materials
/// @param plan the square of each tile
/// @param[in,out] bands the ground's bands, drawn
/// @param moving the ground is a moving camera's, which replaces the kept ground
/// @param leave_room a moving camera's ground leaves room for the grounds
///     packed ahead of it however much room it found
/// @param[out] vertices the ground's vertices; what it held before is
///     overwritten in place
/// @return each tile key drawn, with its square
std::vector<std::pair<uint64_t, AtlasRect>> pack_ground_tiles(
    const Inputs& in,
    Scene& scene,
    const TerrainPlan& plan,
    std::span<GroundBand> bands,
    bool moving,
    bool leave_room,
    std::vector<Vertex>& vertices
) {
    std::unordered_map<uint64_t, AtlasRect> squares;
    std::vector<uint64_t> order;
    for (const auto& band : bands)
        for (const uint64_t key : band.tiles)
            if (squares.emplace(key, AtlasRect{}).second)
                order.push_back(key);
    std::optional<MovingRoom> room;
    if (moving) {
        room.emplace(MovingRoom{plan, order});
        room->material_rows = scene.texture_height / moving_material_row_share;
        for (const uint64_t key : order)
            if (const auto cached = scene.terrain_materials.find(key);
                cached != scene.terrain_materials.end())
                cached->second.drawn = scene.terrain_passes;
    }
    MovingRoom* moving_room = room ? &*room : nullptr;
    for (const uint64_t key : order)
        squares[key] = pack_ground_tile(in, scene, plan, key, moving_room, scene.terrain_passes);
    // Tiles that gave their squares back after they were packed are packed
    // again, at the size a moving camera packs them.
    for (std::size_t index = 0; room && index < room->unpacked.size(); ++index)
        if (!scene.terrain_materials.contains(room->unpacked[index]))
            pack_ground_tile(
                in, scene, plan, room->unpacked[index], moving_room, scene.terrain_passes
            );
    if (room &&
        (leave_room || room->dropped_unseen == scene.terrain_passes || !room->unpacked.empty())) {
        const uint32_t edge = std::max(plan.larger / 2, smallest_terrain_square);
        const auto spare = [&] {
            std::size_t count = spare_terrain_squares(scene, edge).size();
            for (uint32_t larger = edge * 2; larger <= full_terrain_square; larger *= 2)
                count +=
                    spare_terrain_squares(scene, larger).size() * (larger / edge) * (larger / edge);
            const std::size_t quarters = full_terrain_square / edge;
            return count + terrain_row_squares(scene, room->material_rows) * quarters * quarters;
        };
        while (spare() < order.size() / ahead_room_share && unpack_larger_tile(scene, *room))
            pack_ground_tile(
                in, scene, plan, room->unpacked.back(), moving_room, scene.terrain_passes
            );
    }
    for (const uint64_t key : order)
        squares[key] = scene.terrain_materials.at(key).rectangle;
    place_ground_bands(in, scene, bands, squares, vertices);
    return {squares.begin(), squares.end()};
}

/// Draws a frame's ground (draw_ground_rows) in bands on the drawing
/// threads at once, packs their tiles and lists them in order: the same
/// triangles and the same atlas as drawing it row after row on the calling
/// thread, with any number of threads. A ground the frame's geometry budget
/// cannot hold, or that cannot be drawn, is drawn row after row instead, to
/// give up where that gives up.
///
/// @param in the frame
/// @param[in,out] scene the atlas, the terrain materials and the bands
/// @param plan the square of each tile
/// @param draw what the ground is drawn from, with the crop, the picture and
///     the margin it is drawn for
/// @param[out] vertices where the ground is listed
/// @param moving the ground is a moving camera's (pack_ground_tiles)
/// @return each tile key drawn, with its square
std::vector<std::pair<uint64_t, AtlasRect>> draw_ground(
    const Inputs& in,
    Scene& scene,
    const TerrainPlan& plan,
    const GroundDraw& draw,
    std::vector<Vertex>& vertices,
    bool moving
) {
    scene.ground_pass = ++scene.terrain_passes;
    auto& bands = scene.ground_bands;
    plan_ground_bands(draw.source, bands);
    platform::job_pool::run_bands(in.pool, static_cast<uint32_t>(bands.size()), [&](uint32_t band) {
        draw_ground_band(draw, bands[band], false);
    });
    std::size_t listed = 0;
    bool whole = true;
    for (const auto& band : bands) {
        listed += band.vertices.size();
        whole = whole && !band.overflow && !band.failure;
    }
    if (!whole || listed > maximum_vertices) {
        vertices.clear();
        PackingTiles tiles(in, scene, plan, draw, vertices);
        std::vector<std::array<float, 2>> samples;
        std::vector<bool> merged_columns;
        draw_ground_rows(
            draw, draw.source.crop[1], draw.source.crop[3], samples, merged_columns, tiles
        );
        return {tiles.tiles().begin(), tiles.tiles().end()};
    }
    return pack_ground_tiles(in, scene, plan, bands, moving, false, vertices);
}

/// The frames the ground drawn ahead of a moving camera (GroundAhead) is
/// drawn over, a share of its bands each.
constexpr std::size_t ground_ahead_frames = 4;
/// The most of its tiles a frame packs (pack_ground_ahead).
constexpr std::size_t ground_ahead_packs = 64;
/// The frames its plots are cut over, from the frame after the one that
/// places its vertices, a share of its bands each.
constexpr std::size_t ground_cut_frames = 4;
/// The frames it is sent to the card over once cut, a share of its
/// vertices each.
constexpr std::size_t ground_send_frames = 4;
/// The frames from the first of its bands to the frame that takes it.
constexpr std::size_t ground_ahead_pipeline =
    ground_ahead_frames + 1 + ground_cut_frames + ground_send_frames;

/// Tells whether two grounds' sources draw the same ground for the same
/// crop and picture: all but the crop and the picture alike.
///
/// @param a one ground's source
/// @param b the other's
/// @return true when they are drawn alike
bool drawn_alike(const GroundSource& a, const GroundSource& b) {
    return a.map == b.map && a.map_width == b.map_width && a.map_height == b.map_height &&
           a.tile_width == b.tile_width && a.tile_height == b.tile_height &&
           a.tile_art_bytes == b.tile_art_bytes && a.heights == b.heights &&
           a.tile_indices == b.tile_indices && a.step == b.step && a.distant == b.distant &&
           a.fog_divisions == b.fog_divisions && a.fog == b.fog && a.sight_width == b.sight_width &&
           a.sight_height == b.sight_height && a.mapping == b.mapping && a.dithered == b.dithered;
}

/// Starts drawing the ground ahead of a moving camera (GroundAhead) once
/// the kept ground would not cover the picture the camera is expected to
/// show after the frames it takes to be drawn, packed, cut and sent to the
/// card, and one more, moving on as it moved since the last frame: for the
/// picture expected half a margin further on than that, from what the kept
/// ground is drawn from, so that the camera comes to it with half its
/// margin behind it.
///
/// @param in the frame, which takes the kept ground
/// @param[in,out] scene the kept ground and the ground ahead
/// @param motion the frame's projection less the last frame's
void plan_ground_ahead(const Inputs& in, Scene& scene, const std::array<float, 12>& motion) {
    auto& ahead = scene.ground_ahead;
    const auto& source = scene.ground_source;
    const float margin = source.picture_margin;
    if (ahead.active || margin == 0)
        return;
    // The picture expected so many frames on.
    const auto expected = [&](float frames) {
        std::array<float, 12> picture{};
        for (std::size_t index = 0; index < picture.size(); ++index)
            picture[index] = in.clip_projection[index] + frames * motion[index];
        return picture;
    };
    const auto shift = [&](const std::array<float, 12>& from, const std::array<float, 12>& to) {
        return picture_shift(
            from, to, in.clip_width, in.clip_height, source.lowest, source.highest
        );
    };
    constexpr auto drawing_frames = static_cast<float>(ground_ahead_pipeline);
    if (shift(source.clip_projection, expected(drawing_frames + 1)) <= margin)
        return;
    const float pace = shift(expected(-1), in.clip_projection);
    if (!(pace > 0) || !std::isfinite(pace))
        return;
    const auto picture = expected(drawing_frames + margin / (2 * pace));
    if (!std::all_of(picture.begin(), picture.end(), [](float value) {
            return std::isfinite(value);
        }))
        return;
    const auto crop = widened_bounds(*in.map, picture, in.clip_width, in.clip_height, margin);
    if (!crop)
        return;
    ahead.source = source;
    ahead.source.crop = {crop->left, crop->top, crop->right, crop->bottom};
    ahead.source.clip_projection = picture;
    plan_ground_bands(ahead.source, ahead.bands);
    ahead.bands_drawn = 0;
    ahead.bands_a_frame = (ahead.bands.size() + ground_ahead_frames - 1) / ground_ahead_frames;
    ahead.pass = 0;
    ahead.kept_tiles.clear();
    ahead.bands_listed = 0;
    ahead.order.clear();
    ahead.squares.clear();
    ahead.tiles_packed = 0;
    ahead.unpacked.clear();
    ahead.next_unpack = 0;
    ahead.placed = ahead.packed_late = ahead.cut = ahead.cut_waits = false;
    ahead.cut_bands_done = 0;
    ahead.sent = 0;
    ahead.send_rest = false;
    ahead.active = !ahead.bands.empty();
}

/// Draws bands of the ground ahead of a moving camera, on the drawing
/// threads at once, from the first not yet drawn.
///
/// @param in the frame
/// @param[in,out] ahead the ground ahead
/// @param count the bands to draw
/// @param timing the stage timers measure
void draw_ground_ahead(const Inputs& in, GroundAhead& ahead, std::size_t count, bool timing) {
    const auto draw = ground_draw(ahead.source);
    const std::size_t first = ahead.bands_drawn;
    platform::job_pool::run_bands(in.pool, static_cast<uint32_t>(count), [&](uint32_t band) {
        draw_ground_band(draw, ahead.bands[first + band], timing);
    });
    ahead.bands_drawn += count;
}

/// @return whether every band of the ground ahead was drawn whole, within
///     the frame's geometry budget
bool ground_ahead_whole(const GroundAhead& ahead) {
    std::size_t listed = 0;
    for (const auto& band : ahead.bands) {
        if (band.overflow || band.failure)
            return false;
        listed += band.vertices.size();
    }
    return listed <= maximum_vertices;
}

/// @return the vertices of a ground drawn ahead that the card takes once it
///     is the kept ground: the cut ground where its plots were cut into it,
///     else the ground
std::span<const Vertex> ahead_card_vertices(const GroundAhead& ahead) {
    return ahead.cut_plots.empty() ? std::span<const Vertex>(ahead.vertices)
                                   : std::span<const Vertex>(ahead.cut_vertices);
}

/// Packs a share of the tiles of the ground drawn ahead of a moving camera,
/// in a frame that still takes the kept ground, without giving back any
/// square the kept ground draws (MovingRoom::ahead), and places its
/// vertices once every band is drawn and every tile packed. It lists the
/// tiles of the bands drawn since, in the order the ground first draws
/// them, marking those already packed drawn by its terrain pass; then it
/// packs at most ground_ahead_packs of them, those packed again first, as
/// pack_ground_tile does. A ground that would have to give back the kept
/// ground's room is packed in the frame that takes it
/// (GroundAhead::packed_late).
///
/// @param in the frame
/// @param[in,out] scene the atlas and the ground ahead
/// @param plan the square of each tile
void pack_ground_ahead(const Inputs& in, Scene& scene, const TerrainPlan& plan) {
    constexpr std::size_t tile_bytes = formats::tnt::layout::tile_bytes;
    auto& ahead = scene.ground_ahead;
    if (ahead.pass == 0) {
        ahead.pass = ++scene.terrain_passes;
        for (const auto& tile : scene.ground_tiles)
            ahead.kept_tiles.insert(tile.first);
    }
    for (; ahead.bands_listed < ahead.bands_drawn; ++ahead.bands_listed)
        for (const uint64_t key : ahead.bands[ahead.bands_listed].tiles)
            if (ahead.squares.emplace(key, AtlasRect{}).second) {
                ahead.order.push_back(key);
                if (const auto cached = scene.terrain_materials.find(key);
                    cached != scene.terrain_materials.end())
                    cached->second.drawn = ahead.pass;
            }
    MovingRoom room{plan, ahead.order};
    room.material_rows = scene.texture_height / moving_material_row_share;
    room.ahead = true;
    room.kept_tiles = &ahead.kept_tiles;
    room.next_tile = ahead.next_unpack;
    const auto& art = in.map->tile_palette_indices;
    std::size_t packs = 0;
    try {
        while (packs < ground_ahead_packs) {
            if (!ahead.unpacked.empty()) {
                const uint64_t key = ahead.unpacked.back();
                ahead.unpacked.pop_back();
                if (!scene.terrain_materials.contains(key)) {
                    pack_ground_tile(in, scene, plan, key, &room, ahead.pass);
                    ++packs;
                }
            } else if (ahead.tiles_packed < ahead.order.size()) {
                const uint64_t key = ahead.order[ahead.tiles_packed];
                const auto start = static_cast<std::size_t>(key / 3) * tile_bytes;
                const auto cached = scene.terrain_materials.find(key);
                if (cached != scene.terrain_materials.end()) {
                    if (plan.keeps(key, cached->second.rectangle.width) &&
                        start + tile_bytes <= art.size() &&
                        std::memcmp(cached->second.pixels.data(), art.data() + start, tile_bytes) ==
                            0) {
                        ++ahead.tiles_packed;
                        continue;
                    }
                    if (ahead.kept_tiles.contains(key))
                        throw AheadFull();
                    give_back_terrain_square(scene, cached->second.rectangle);
                    scene.terrain_materials.erase(cached);
                }
                pack_ground_tile(in, scene, plan, key, &room, ahead.pass);
                ++ahead.tiles_packed;
                ++packs;
            } else {
                break;
            }
            ahead.unpacked.insert(ahead.unpacked.end(), room.unpacked.begin(), room.unpacked.end());
            room.unpacked.clear();
        }
    } catch (const AheadFull&) {
        ahead.packed_late = true;
        return;
    }
    ahead.next_unpack = room.next_tile;
    if (ahead.bands_drawn < ahead.bands.size() || ahead.tiles_packed < ahead.order.size() ||
        !ahead.unpacked.empty())
        return;
    for (auto& [key, square] : ahead.squares) {
        const auto cached = scene.terrain_materials.find(key);
        if (cached == scene.terrain_materials.end()) {
            ahead.packed_late = true;
            return;
        }
        square = cached->second.rectangle;
    }
    place_ground_bands(in, scene, ahead.bands, ahead.squares, ahead.vertices);
    ahead.tiles.assign(ahead.squares.begin(), ahead.squares.end());
    ahead.revision = new_ground_revision();
    ahead.placed = true;
    ahead.cut_waits = true;
}

/// Tells whether the camera, moving on as it moved since the last frame, is
/// expected to show a picture past the kept ground's margin in the next.
///
/// @param in the frame, which takes the kept ground
/// @param scene the kept ground
/// @param motion the frame's projection less the last frame's
/// @return true when it is
bool kept_ground_runs_out(
    const Inputs& in, const Scene& scene, const std::array<float, 12>& motion
) {
    const auto& source = scene.ground_source;
    std::array<float, 12> next{};
    for (std::size_t index = 0; index < next.size(); ++index)
        next[index] = in.clip_projection[index] + motion[index];
    return !(
        picture_shift(
            source.clip_projection,
            next,
            in.clip_width,
            in.clip_height,
            source.lowest,
            source.highest
        ) <= source.picture_margin
    );
}

/// Tells whether every tile a ground packed ahead draws is still where it
/// was packed, from the map's art as it is now, and in a square the plan
/// lets it keep, as ground_tiles_hold tells of the kept ground.
bool ground_ahead_holds(const Inputs& in, const Scene& scene, const TerrainPlan& plan) {
    constexpr std::size_t tile_bytes = formats::tnt::layout::tile_bytes;
    const auto& art = in.map->tile_palette_indices;
    for (const auto& [key, rectangle] : scene.ground_ahead.tiles) {
        const auto start = static_cast<std::size_t>(key / 3) * tile_bytes;
        const auto cached = scene.terrain_materials.find(key);
        if (cached == scene.terrain_materials.end() || !(cached->second.rectangle == rectangle) ||
            !plan.keeps(key, rectangle.width) || start + tile_bytes > art.size() ||
            std::memcmp(cached->second.pixels.data(), art.data() + start, tile_bytes) != 0)
            return false;
    }
    return true;
}

/// Takes the ground drawn ahead of a moving camera as the kept ground, its
/// crop and picture the kept ground's: packed and placed ahead, with its
/// cut kept for the frame's plots where it was cut with the same; or its
/// tiles packed now (pack_ground_tiles) and its bands listed.
///
/// @param in the frame
/// @param[in,out] scene the kept ground and the ground ahead
/// @param plan the square of each tile
void take_ground_ahead(const Inputs& in, Scene& scene, const TerrainPlan& plan) {
    auto& ahead = scene.ground_ahead;
    auto& source = scene.ground_source;
    ahead.active = false;
    source.crop = ahead.source.crop;
    source.clip_projection = ahead.source.clip_projection;
    source.clip_width = ahead.source.clip_width;
    source.clip_height = ahead.source.clip_height;
    source.picture_margin = ahead.source.picture_margin;
    if (ahead.placed && ground_ahead_holds(in, scene, plan)) {
        std::swap(scene.ground_vertices, ahead.vertices);
        scene.ground_tiles = std::move(ahead.tiles);
        scene.ground_kept = true;
        scene.ground_revision = scene.kept_ground_revision = ahead.revision;
        scene.ground_pass = ahead.pass;
        scene.terrain_vertex_count = scene.ground_vertices.size();
        // The cut, kept as the foundations keep theirs (run_passes), which
        // take it when the frame's plots are the same.
        if (ahead.cut && !ahead.cut_plots.empty()) {
            std::swap(scene.cut_ground, ahead.cut_vertices);
            std::swap(scene.cut_plots, ahead.cut_plots);
            scene.cut_source = ahead.revision;
            scene.cut_revision = ahead.cut_revision;
        }
        return;
    }
    scene.ground_pass = ++scene.terrain_passes;
    scene.ground_kept = false;
    scene.ground_revision = new_ground_revision();
    auto drawn = pack_ground_tiles(
        in, scene, plan, ahead.bands, true, ahead.packed_late, scene.ground_vertices
    );
    scene.ground_tiles = std::move(drawn);
    scene.ground_kept = true;
    scene.kept_ground_revision = scene.ground_revision;
    scene.terrain_vertex_count = scene.ground_vertices.size();
}

/// Emits the frame's ground (draw_ground), or takes a kept ground: the kept
/// ground again when it was drawn from the same, covers the frame's picture
/// and its tiles keep their squares, or the ground drawn ahead of a moving
/// camera once it covers the picture. A kept ground it leaves in
/// Scene::ground_vertices, for the passes after it to list as the
/// foundations leave it (run_passes). While the camera moves
/// (TerrainPlan::holding) the ground is drawn beyond the picture by a
/// margin and kept, so that the frames after it take it until their
/// pictures move past the margin, and the next is drawn ahead of the camera
/// on the drawing threads (plan_ground_ahead) to take over before then;
/// otherwise it is kept once two frames running are drawn from the same,
/// so that a recording's moving camera does not copy its ground every
/// frame.
///
/// @param in the frame
/// @param[in,out] scene the geometry, the atlas and the kept ground
/// @param plan the square of each tile
/// @param step the height cells a side of each drawn square of ground
/// @param camera_still the frame has a picture, which the camera drew the
///     last frame from too
/// @param widen the ground may be drawn beyond the picture's margin
/// @param motion the frame's projection less the last frame's
/// @return true when the frame's ground is a kept ground, not yet listed
///     in Scene::vertices; its vertices are counted in
///     Scene::terrain_vertex_count either way
bool terrain(
    const Inputs& in,
    Scene& scene,
    const TerrainPlan& plan,
    int32_t step,
    bool camera_still,
    bool widen,
    const std::array<float, 12>& motion
) {
    // Compare palette bytes once per pass and tile bytes once per visible key.
    // The retained copies detect edits even when the same map object is reused.
    if (scene.terrain_palette != *in.palette) {
        // No tile keeps its square, so the kept ground is drawn again, and
        // any ground drawn ahead is drawn afresh.
        scene.ground_kept = false;
        scene.ground_ahead.active = false;
        drop_terrain_materials(scene, std::numeric_limits<uint64_t>::max());
        scene.terrain_palette = *in.palette;
        const auto entries = present::palette_from_bytes(*in.palette);
        std::array<uint8_t, OA_PALETTE_COLORS> gray_levels{};
        present::world_renderer::build_gray_levels(entries, gray_levels);
        auto& grey_palette = scene.terrain_grey_palette;
        grey_palette = *in.palette;
        for (std::size_t index = 0; index < OA_PALETTE_COLORS; ++index) {
            const auto& colour = entries.entries[index];
            const auto level = (uint32_t(colour.r) + colour.g + colour.b) / 3;
            const auto from = std::size_t(gray_levels[level]) * palette_entry_bytes;
            std::copy_n(
                in.palette->begin() + from,
                palette_entry_bytes,
                grey_palette.begin() + index * palette_entry_bytes
            );
        }
    }
    auto& source = scene.ground_source;
    const auto crop = terrain_crop(in);
    const bool same_source = note_ground_source(source, in, scene, step);
    const bool same_picture = ground_covers(source, in, crop);
    // A moving camera's ground reaches beyond the picture for the frames
    // after it.
    const bool moving = plan.holding && widen && in.clip_width != 0 && in.clip_height != 0;
    // The ground ahead is the kept ground's successor, drawn from the same.
    auto& ahead = scene.ground_ahead;
    if (ahead.active && (!moving || !drawn_alike(ahead.source, source)))
        ahead.active = false;
    // A settled camera's ground reaches no further than its picture's.
    const bool kept = same_source && same_picture && scene.ground_kept &&
                      ground_tiles_hold(in, scene, plan) &&
                      (source.picture_margin == 0 || (plan.holding && widen));
    // Past the kept ground's margin before the ground ahead is done, the
    // rest of it is drawn now.
    if (ahead.active && !kept && ahead.bands_drawn < ahead.bands.size())
        draw_ground_ahead(in, ahead, ahead.bands.size() - ahead.bands_drawn, false);
    if (ahead.active && ahead.bands_drawn == ahead.bands.size()) {
        const bool whole = ground_ahead_whole(ahead);
        // While the kept ground covers the picture, the ground ahead waits
        // until it is cut and sent to the card.
        const bool ready =
            !kept || ahead.packed_late ||
            (ahead.placed && ahead.cut && ahead.sent == ahead_card_vertices(ahead).size());
        if (whole && ready && ground_covers(ahead.source, in, crop)) {
            take_ground_ahead(in, scene, plan);
            return true;
        }
        if (!whole || !kept)
            ahead.active = false;
    }
    if (kept) {
        scene.terrain_vertex_count = scene.ground_vertices.size();
        scene.ground_revision = scene.kept_ground_revision;
        // The ground ahead's tiles, packed as its bands are drawn, and its
        // vertices placed.
        if (ahead.active && ahead.bands_drawn != 0 && !ahead.placed && !ahead.packed_late)
            pack_ground_ahead(in, scene, plan);
        ahead.send_rest = ahead.active && moving && kept_ground_runs_out(in, scene, motion);
        if (moving)
            plan_ground_ahead(in, scene, motion);
        return true;
    }
    ahead.active = false;
    scene.ground_kept = false;
    scene.ground_revision = new_ground_revision();
    float margin = 0;
    std::optional<TerrainCrop> widened;
    if (moving) {
        margin = static_cast<float>(std::max(in.clip_width, in.clip_height)) *
                 moving_ground_margin_share;
        widened = widened_crop(in, margin);
    }
    if (!widened)
        margin = 0;
    const auto drawn = widened.value_or(crop);
    // Was the ground last drawn for this very picture?
    const bool drawn_before = margin == 0 && source.picture_margin == 0 && same_picture;
    source.crop = {drawn.left, drawn.top, drawn.right, drawn.bottom};
    source.clip_projection = in.clip_projection;
    source.clip_width = in.clip_width;
    source.clip_height = in.clip_height;
    source.picture_margin = margin;
    if (margin == 0 && !(same_source && (drawn_before || camera_still))) {
        draw_ground(in, scene, plan, ground_draw(source), scene.vertices, moving);
        scene.terrain_vertex_count = scene.vertices.size();
        return false;
    }
    scene.ground_tiles =
        draw_ground(in, scene, plan, ground_draw(source), scene.ground_vertices, moving);
    scene.ground_kept = true;
    scene.kept_ground_revision = scene.ground_revision;
    scene.terrain_vertex_count = scene.ground_vertices.size();
    return true;
}

/// Drapes a cutout over the exact terrain triangles, preserving its painted footprint.
/// Moves a corner lying under the water by the swells the vertex stage
/// moves the ground under it by (shaders/live_scene_vertex.glsl), so that
/// what lies on the sea bed moves with the water's art; a corner standing
/// out of the water stays.
void move_with_swells(const Inputs& in, Vertex& vertex) {
    const float depth = float(in.sea_level) - vertex.height;
    if (in.sea_level <= 0 || !(depth > 0) || !(in.water.ripples > 0) || !(in.water.detail > 0))
        return;
    const auto offset =
        renderer_water_surface::art_offset(vertex.x, vertex.z + depth * 0.5F, in.water.wind, 0);
    const float share = std::clamp(in.water.ripples, 0.0F, 2.0F) *
                        std::clamp(in.water.detail, 0.0F, 1.0F) *
                        renderer_water_surface::art_depth_weight(depth);
    vertex.x += offset[0] * share;
    vertex.z += offset[1] * share;
}

void decal(
    const Inputs& in,
    Pass& pass,
    const AtlasRect& rect,
    float left,
    float top,
    float opacity,
    bool shadow,
    const FixedVec3* anchor = nullptr,
    float footprint_scale = 1,
    bool water_receiver = false,
    bool soft_edges = false,
    bool scenery = false,
    const AtlasRect* grey = nullptr
) {
    constexpr float surface_lift = 0.05F;
    const auto& scene = pass.scene;
    if (in.detail_scale > 0 && in.detail_scale < 1) {
        const float diameter =
            std::min(rect.width, rect.height) * in.detail_scale * footprint_scale;
        opacity *= std::clamp((diameter - 0.25F) / 0.75F, 0.0F, 1.0F);
        if (opacity == 0)
            return;
    }
    const float right = left + rect.width * footprint_scale;
    const float bottom = top + rect.height * footprint_scale;
    const auto& map = *in.map;
    // A canonical feature anchor selects its world footprint even where two
    // surfaces overlap in the original painted projection. Shadow images without
    // an anchor retain their projected silhouette.
    const float anchor_lift = anchor ? anchor->y / fixed_scale * height_lift : 0;
    const float first_world_y = top + anchor_lift;
    const float last_world_y = bottom + (anchor ? anchor_lift : maximum_terrain_row_shift);
    const int32_t first_x = std::max(
        {0,
         static_cast<int32_t>(std::floor(left / cell_edge)) * cell_edge,
         static_cast<int32_t>(std::floor(float(in.left) / cell_edge)) * cell_edge}
    );
    const int32_t first_y = std::max(
        {0,
         static_cast<int32_t>(std::floor(first_world_y / cell_edge)) * cell_edge,
         static_cast<int32_t>(std::floor(float(in.top) / cell_edge)) * cell_edge}
    );
    const float last_x = std::min(
        {right,
         static_cast<float>(map.attribute_width * cell_edge),
         std::ceil(float(in.right) / cell_edge) * cell_edge}
    );
    const float last_y = std::min(
        {last_world_y,
         static_cast<float>(map.attribute_height * cell_edge),
         std::ceil(float(in.bottom + maximum_terrain_row_shift) / cell_edge) * cell_edge}
    );
    for (int32_t y = first_y; y < last_y; y += cell_edge)
        for (int32_t x = first_x; x < last_x; x += cell_edge) {
            const auto placed = [&](float dx, float dy, float ground_height) {
                const float height =
                    water_receiver ? std::max(ground_height, float(in.sea_level)) : ground_height;
                return GroundPoint{
                    static_cast<float>(x) + dx,
                    static_cast<float>(y) + dy - (anchor ? anchor_lift : height * height_lift),
                    height
                };
            };
            const auto point = [&](int32_t dx, int32_t dy) {
                return placed(float(dx), float(dy), height_at(map, x + dx, y + dy));
            };
            // The terrain's own triangles: about the middle of a twisted cell.
            const std::array cell{
                point(0, 0), point(cell_edge, 0), point(cell_edge, cell_edge), point(0, cell_edge)
            };
            std::array<GroundPoint, 12> triangles{};
            std::size_t triangle_points = 6;
            if (twisted_cell(cell[0].height, cell[1].height, cell[2].height, cell[3].height)) {
                constexpr float half = cell_edge / 2;
                const GroundPoint middle = placed(half, half, cell_middle_height(map, x, y));
                for (std::size_t corner = 0; corner < cell.size(); ++corner) {
                    triangles[corner * 3] = middle;
                    triangles[corner * 3 + 1] = cell[corner];
                    triangles[corner * 3 + 2] = cell[(corner + 1) % cell.size()];
                }
                triangle_points = triangles.size();
            } else {
                triangles = {cell[0], cell[1], cell[2], cell[0], cell[2], cell[3]};
            }
            for (std::size_t first = 0; first < triangle_points; first += 3) {
                GroundPolygon clipped{};
                clipped.count = 3;
                std::copy_n(triangles.begin() + first, 3, clipped.points.begin());
                clipped = clip_ground_fixed(clipped, true, left, true);
                clipped = clip_ground_fixed(clipped, true, right, false);
                clipped = clip_ground_fixed(clipped, false, top, true);
                clipped = clip_ground_fixed(clipped, false, bottom, false);
                if (clipped.count < 3)
                    continue;
                std::array<Vertex, 8> corners{};
                for (std::size_t corner = 0; corner < clipped.count; ++corner) {
                    const auto& p = clipped.points[corner];
                    const float h = p.height + surface_lift;
                    const float painted_y =
                        p.y + (anchor ? anchor_lift - p.height * height_lift : 0);
                    corners[corner] = {
                        p.x,
                        h,
                        p.y + (anchor ? anchor_lift : p.height * height_lift),
                        (rect.x + (p.x - left) / footprint_scale) / scene.texture_width,
                        (rect.y + (p.y - top) / footprint_scale) / scene.texture_height,
                        shadow ? 1.0F : brightness_at(in.sight, p.x, painted_y),
                        opacity
                    };
                    // A map feature keeps the ground's sampling. Its grey image
                    // shares the colour image's place in the frame, and the
                    // fragment stage reads that from terrain_x and terrain_y.
                    if (scenery) {
                        corners[corner].terrain_size = soft_edges ? -7.0F : -6.0F;
                        if (grey != nullptr) {
                            corners[corner].terrain_x =
                                (grey->x + (p.x - left) / footprint_scale) / scene.texture_width;
                            corners[corner].terrain_y =
                                (grey->y + (p.y - top) / footprint_scale) / scene.texture_height;
                            const auto ground =
                                fog_of_ground(in, p.x, painted_y, corners[corner].brightness);
                            corners[corner].terrain_known = ground.known;
                            corners[corner].terrain_grey = ground.grey;
                        }
                    } else if (soft_edges) {
                        corners[corner].terrain_size = -1;
                    }
                }
                float low_brightness = corners[0].brightness;
                float high_brightness = low_brightness;
                for (std::size_t corner = 1; corner < clipped.count; ++corner) {
                    low_brightness = std::min(low_brightness, corners[corner].brightness);
                    high_brightness = std::max(high_brightness, corners[corner].brightness);
                }
                // Even fog keeps the polygon the ground pass has always emitted.
                // A boundary is split as the terrain under the feature is, in
                // map space, before the swells move the sea bed.
                const int32_t divisions = fog_divisions(in);
                if (grey == nullptr || high_brightness - low_brightness < 0.001F ||
                    divisions <= 1) {
                    for (std::size_t corner = 0; corner < clipped.count; ++corner)
                        move_with_swells(in, corners[corner]);
                    polygon(pass, std::span(corners).first(clipped.count));
                    continue;
                }
                const auto sample_point = [&](const Vertex& a,
                                              const Vertex& b,
                                              const Vertex& c,
                                              int32_t column,
                                              int32_t row) {
                    const float u = static_cast<float>(column) / divisions;
                    const float v = static_cast<float>(row) / divisions;
                    const auto blend = [u, v](float first, float second, float third) {
                        return first + u * (second - first) + v * (third - first);
                    };
                    Vertex result{
                        blend(a.x, b.x, c.x),
                        blend(a.height, b.height, c.height),
                        blend(a.z, b.z, c.z),
                        blend(a.u, b.u, c.u),
                        blend(a.v, b.v, c.v),
                        1.0F,
                        blend(a.opacity, b.opacity, c.opacity),
                        blend(a.emission_r, b.emission_r, c.emission_r),
                        blend(a.emission_g, b.emission_g, c.emission_g),
                        blend(a.emission_b, b.emission_b, c.emission_b),
                        blend(a.terrain_x, b.terrain_x, c.terrain_x),
                        blend(a.terrain_y, b.terrain_y, c.terrain_y),
                        a.terrain_size
                    };
                    const float painted_x = result.x;
                    const float painted_y = result.z - (result.height - surface_lift) * height_lift;
                    result.brightness = brightness_at(in.sight, painted_x, painted_y);
                    const auto ground = fog_of_ground(in, painted_x, painted_y, result.brightness);
                    result.terrain_known = ground.known;
                    result.terrain_grey = ground.grey;
                    move_with_swells(in, result);
                    return result;
                };
                constexpr int32_t row_stride = 5;
                for (std::size_t fan = 1; fan + 1 < clipped.count; ++fan) {
                    const Vertex& a = corners[0];
                    const Vertex& b = corners[fan];
                    const Vertex& c = corners[fan + 1];
                    std::array<Vertex, row_stride * row_stride> points{};
                    for (int32_t row = 0; row <= divisions; ++row)
                        for (int32_t column = 0; column <= divisions - row; ++column)
                            points[static_cast<std::size_t>(row * row_stride + column)] =
                                sample_point(a, b, c, column, row);
                    for (int32_t row = 0; row < divisions; ++row)
                        for (int32_t column = 0; column < divisions - row; ++column) {
                            const auto at = [&](int32_t point_column, int32_t point_row) {
                                return points[static_cast<std::size_t>(
                                    point_row * row_stride + point_column
                                )];
                            };
                            polygon(
                                pass,
                                std::array{
                                    at(column, row), at(column + 1, row), at(column, row + 1)
                                }
                            );
                            if (column + row + 1 < divisions)
                                polygon(
                                    pass,
                                    std::array{
                                        at(column + 1, row),
                                        at(column + 1, row + 1),
                                        at(column, row + 1)
                                    }
                                );
                        }
                }
            }
        }
}

/// The models' draws each band of the shadow pass reads and draws.
constexpr std::size_t shadow_band_draws = 32;

/// Reads the shadows a band of the models' draws casts from their units'
/// pictures, restoring each picture's real silhouette without synthesising
/// a shape: each shadow's material and where it lies, before the frame's
/// own materials are packed (pack_shadows).
///
/// @param in the frame
/// @param scene the frame, for the materials' digests
/// @param[out] band the band's shadows, read afresh
/// @param first the band's first draw in WorldDrawList::models
/// @param last the draw after its last
void read_shadows(
    const Inputs& in, const Scene& scene, ShadowBand& band, std::size_t first, std::size_t last
) {
    band.pictures.clear();
    band.texels.clear();
    band.failure = nullptr;
    if ((in.graphics_flags & model::graphics_shadows) == 0 || in.draws->shadow_level == 0)
        return;
    for (std::size_t index = first; index < last; ++index) {
        const auto& draw = in.draws->models[index];
        const auto& ref = draw.model;
        const Unit* unit = ref.unit;
        if (draw.stand_in >= 0 &&
            static_cast<std::size_t>(draw.stand_in) < in.draws->stand_ins.size())
            unit = &in.draws->stand_ins[static_cast<std::size_t>(draw.stand_in)];
        if (!draw.plan.linked.drawn || !draw.plan.model.from_image || unit == nullptr ||
            ref.state == nullptr)
            continue;
        const uint32_t flags = ref.def ? ref.def->flags : 0;
        if ((flags & OA_UNIT_DEF_FLAG_NO_SHADOW) != 0)
            continue;
        const bool digger = (flags & OA_UNIT_DEF_FLAG_DIGGER) != 0;
        const bool building = (unit->flags & OA_UNIT_FLAG_BUILDING) != 0 && !digger;
        if (!building && !digger &&
            ((in.graphics_flags & model::graphics_vehicle_shadows) == 0 ||
             (flags & (OA_UNIT_DEF_FLAG_FLOATER | OA_UNIT_DEF_FLAG_CAN_HOVER)) != 0))
            continue;
        if (building && unit->type_index == 0 && unit->position.y / fixed_scale < in.sea_level)
            continue;
        const auto& source = building ? ref.state->shadow : ref.state->image;
        const auto& image = source.sprite;
        const auto count = static_cast<std::size_t>(image.width) * image.height;
        if (image.data == nullptr || count == 0 || count > maximum_vertices)
            continue;
        const auto position = draw.shown_position.value_or(unit->position);
        const float shadow_x = position.x / fixed_scale + model::shadow_offset_x - image.origin_x;
        const float shadow_y =
            position.z / fixed_scale - draw.plan.model.ground * height_lift - image.origin_y;
        if (shadow_x + image.width <= std::floor(float(in.left) / cell_edge) * cell_edge ||
            shadow_y + image.height <= std::floor(float(in.top) / cell_edge) * cell_edge ||
            shadow_x >= std::ceil(float(in.right) / cell_edge) * cell_edge ||
            shadow_y >= std::ceil(float(in.bottom) / cell_edge) * cell_edge)
            continue;
        auto& pixels = band.pixels;
        pixels.assign(count, image.key);
        if (image.encoding == OA_SPRITE_RAW) {
            if (source.pixels.size() < count)
                continue;
            std::copy_n(static_cast<const uint8_t*>(image.data), count, pixels.begin());
        } else if (image.encoding == OA_SPRITE_ROW_RLE) {
            const Rect32 rectangle{0, 0, image.width - 1, image.height - 1};
            if (!present::decode_rle_rows(
                    pixels.data(), image.width, rectangle, source.pixels, rectangle
                ))
                continue;
        } else
            continue;
        // The coverage goes after the band's other materials.
        const std::size_t start = band.texels.size();
        band.texels.resize(start + count);
        const auto coverage = std::span(band.texels).subspan(start, count);
        const int32_t unit_height = position.y / 65536;
        const int32_t lift = std::max(0, in.sea_level - unit_height);
        const int32_t threshold = digger ? model::model_depth_base + model::digger_depth_bias
                                         : model::model_depth_base + lift;
        const auto* depth = static_cast<const uint8_t*>(image.aux);
        for (std::size_t pixel = 0; pixel < count; ++pixel)
            coverage[pixel] = pixels[pixel] != image.key &&
                              (building || depth == nullptr || (!digger && lift == 0) ||
                               depth[pixel] > static_cast<uint8_t>(threshold));
        const float receiver = std::max(
            height_at(*in.map, position.x / 65536, position.z / 65536), float(in.sea_level)
        );
        const float altitude = std::max(0.0F, position.y / fixed_scale - receiver);
        const bool aircraft =
            in.new_unit_shadows && (flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0 && altitude > 0;
        ShadowPicture picture;
        picture.texels = start;
        if (aircraft && image.width <= 256 && image.height <= 256) {
            const uint32_t radius = renderer_aircraft_shadow::edge_radius(altitude);
            uint64_t key = hash_start;
            key = mix_word(key, image.width);
            key = mix_word(key, image.height);
            key = mix_word(key, radius);
            key = mix_bytes(key, coverage);
            // The blurred silhouette takes the coverage's place.
            const auto alpha =
                renderer_aircraft_shadow::soften(coverage, image.width, image.height, radius);
            band.texels.resize(start);
            band.texels.insert(band.texels.end(), alpha.begin(), alpha.end());
            const float padding = static_cast<float>(radius) * 2.0F;
            picture.width = image.width + radius * 4;
            picture.height = image.height + radius * 4;
            picture.aircraft_key = key;
            picture.left =
                position.x / fixed_scale + model::shadow_offset_x - (image.origin_x + padding);
            picture.top =
                position.z / fixed_scale - receiver * height_lift - (image.origin_y + padding);
            picture.soft = true;
            picture.digest = frame_material_digest(
                scene, *in.palette, picture.width, picture.height, alpha, true
            );
        } else {
            picture.width = image.width;
            picture.height = image.height;
            picture.left = shadow_x;
            picture.top = shadow_y;
            picture.digest = frame_material_digest(
                scene, *in.palette, picture.width, picture.height, coverage, false
            );
        }
        band.pictures.push_back(picture);
    }
}

/// Packs a band's shadows' materials among the frame's own, in the band's
/// order (frame_material): an aircraft's soft shadow cast before by the
/// same silhouette, blurred alike, takes the same material.
///
/// @param[in,out] pass the shadow pass: one that may not pack the other
///     materials stops at a shadow whose material goes with them, or that
///     fails
/// @param[in,out] frame the frame's own materials packed so far
/// @param[in,out] band the band, each shadow packed given its rectangle
/// @param band_index the band's place among the pass's bands
/// @param first the first shadow to pack
/// @param last the shadow after the last
/// @param[out] writes the materials new in the rows, whose texels are
///     written once the passes before are joined; null writes them now
/// @return the first shadow not packed, last once all are
std::size_t pack_shadows(
    Pass& pass,
    FramePacking& frame,
    ShadowBand& band,
    std::size_t band_index,
    std::size_t first,
    std::size_t last,
    std::vector<FrameWrite>* writes
) {
    for (std::size_t index = first; index < last; ++index) {
        auto& picture = band.pictures[index];
        const auto texels =
            std::span<const uint8_t>(band.texels)
                .subspan(picture.texels, static_cast<std::size_t>(picture.width) * picture.height);
        if (picture.soft) {
            const auto found = frame.aircraft_shadows.find(picture.aircraft_key);
            if (found != frame.aircraft_shadows.end()) {
                picture.rectangle = found->second;
                continue;
            }
        }
        // A shadow that stops the pass is packed again from the start, by
        // the thread that builds the frame, which raises any failure in it
        // in the passes' order.
        const uint64_t material_area = frame.material_area;
        const uint64_t spilled_area = frame.spilled_area;
        bool written{};
        try {
            picture.rectangle = frame_material(
                pass,
                frame,
                picture.digest,
                picture.width,
                picture.height,
                texels,
                picture.soft,
                written
            );
        } catch (...) {
            if (pass.packing)
                throw;
            frame.material_area = material_area;
            frame.spilled_area = spilled_area;
            return index;
        }
        if (picture.soft)
            frame.aircraft_shadows.emplace(picture.aircraft_key, picture.rectangle);
        if (!written)
            continue;
        if (writes != nullptr)
            writes->push_back({picture.rectangle, band_index, picture.texels, picture.soft});
        else
            write_frame_material(pass.scene, picture.rectangle, texels, picture.soft);
    }
    return last;
}

/// Lays a band's packed shadows on the ground under their units.
///
/// @param in the frame
/// @param[in,out] pass the shadow pass, listing the band
/// @param band the band, its shadows packed
/// @param first the first shadow to lay
/// @param last the shadow after the last
void draw_shadows(
    const Inputs& in, Pass& pass, const ShadowBand& band, std::size_t first, std::size_t last
) {
    const float opacity = 0.5F * std::min(in.draws->shadow_level, model::shadow_full_level) /
                          model::shadow_full_level;
    for (std::size_t index = first; index < last; ++index) {
        const auto& picture = band.pictures[index];
        if (picture.soft)
            decal(
                in,
                pass,
                picture.rectangle,
                picture.left,
                picture.top,
                opacity,
                true,
                nullptr,
                1,
                true,
                true
            );
        else
            decal(in, pass, picture.rectangle, picture.left, picture.top, opacity, true);
        ++pass.list.drawn;
    }
}

/// Splits ground geometry at a world map coordinate, retaining its material and fog.
///
/// @param source a convex piece of ground
/// @param horizontal split at a map column, rather than a world row
/// @param edge the column or row
/// @param greater keep the side at or past the edge, rather than before it
/// @param[out] result the piece's side of the edge, emptied first, and left
///     empty when it has no area; its storage is reused
void clip_foundation(
    std::span<const Vertex> source,
    bool horizontal,
    float edge,
    bool greater,
    std::vector<Vertex>& result
) {
    result.clear();
    if (source.empty())
        return;
    const auto coordinate = [horizontal](const Vertex& vertex) {
        return horizontal ? vertex.x : vertex.z;
    };
    auto previous = source.back();
    bool previous_inside = greater ? coordinate(previous) >= edge : coordinate(previous) <= edge;
    for (const auto& current : source) {
        const bool inside = greater ? coordinate(current) >= edge : coordinate(current) <= edge;
        if (inside != previous_inside) {
            const float t =
                (edge - coordinate(previous)) / (coordinate(current) - coordinate(previous));
            result.push_back(
                {std::lerp(previous.x, current.x, t),
                 std::lerp(previous.height, current.height, t),
                 std::lerp(previous.z, current.z, t),
                 std::lerp(previous.u, current.u, t),
                 std::lerp(previous.v, current.v, t),
                 std::lerp(previous.brightness, current.brightness, t),
                 std::lerp(previous.opacity, current.opacity, t),
                 std::lerp(previous.emission_r, current.emission_r, t),
                 std::lerp(previous.emission_g, current.emission_g, t),
                 std::lerp(previous.emission_b, current.emission_b, t),
                 std::lerp(previous.terrain_x, current.terrain_x, t),
                 std::lerp(previous.terrain_y, current.terrain_y, t),
                 previous.terrain_size,
                 std::lerp(previous.terrain_known, current.terrain_known, t),
                 std::lerp(previous.terrain_grey, current.terrain_grey, t)}
            );
        }
        if (inside)
            result.push_back(current);
        previous = current;
        previous_inside = inside;
    }
    double area{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& a = result[i];
        const auto& b = result[(i + 1) % result.size()];
        area += double(a.x) * (b.z) - double(b.x) * (a.z);
    }
    if (std::abs(area) < 0.000001)
        result.clear();
}

/// How far below a structure's height, in map pixels, the ground under it
/// goes to keep its base piece in view. A base piece reaching deeper is sunk
/// into the ground by design, and the ground still covers what lies deeper.
constexpr float deepest_base = 1;

/// How far above a structure's height, in map pixels, its base piece's
/// corners count as standing on the ground: the floor plates of the game's
/// structures lie up to about a map pixel above it. Higher parts of the base
/// may overhang the ground around without levelling it.
constexpr float base_reach = 2;

/// Where a structure's base piece, the root every other piece of its model
/// hangs from, meets the ground.
struct BasePiece {
    float depth{}; ///< its lowest corner below the structure's height, 0 to deepest_base
    /// The map rectangle its corners up to base_reach cover, in map pixels
    /// across and world rows down; empty, left above right, when it has none.
    float left{std::numeric_limits<float>::infinity()};
    float right{-std::numeric_limits<float>::infinity()};
    float top{std::numeric_limits<float>::infinity()};
    float bottom{-std::numeric_limits<float>::infinity()};
};

/// Measures a structure's base piece as the models pass draws it: every face
/// but the selection plate, at the piece's pose this frame. The other pieces
/// move (guns, doors, hatches) and may sink below the ground by design, so
/// they leave the ground as it is.
///
/// @param draw the structure's draw, with its model
/// @param x the map column its model stands on, in map pixels
/// @param z the world row its model stands on
/// @return the base piece; at no depth and empty when the model draws none
BasePiece base_piece(const ModelDraw& draw, float x, float z) {
    BasePiece result;
    const auto& reference = draw.model;
    if (reference.instance == nullptr || reference.prepared == nullptr)
        return result;
    const auto pieces = reference.instance->pieces();
    const auto root = reference.instance->root_piece();
    if (root >= pieces.size())
        return result;
    const auto& piece = pieces[root];
    const auto& objects = reference.instance->model().objects;
    if ((piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::visible)) == 0 ||
        piece.object_index >= objects.size() ||
        piece.object_index >= reference.prepared->objects.size())
        return result;
    const auto& points =
        root < draw.shown_vertices.size() ? draw.shown_vertices[root] : piece.transformed_vertices;
    const auto& object = objects[piece.object_index];
    for (const auto& primitive : reference.prepared->objects[piece.object_index].primitives) {
        if (static_cast<int32_t>(primitive.source_index) == object.selection_primitive ||
            primitive.source_index >= object.primitives.size())
            continue;
        const auto& source = object.primitives[primitive.source_index];
        if (source.vertex_indices.size() < 3)
            continue;
        for (const auto index : source.vertex_indices) {
            if (index >= points.size())
                continue;
            const auto& point = points[index];
            const float below = -(point.y / fixed_scale);
            result.depth = std::max(result.depth, std::min(deepest_base, below));
            if (-below > base_reach)
                continue;
            const float corner_x = x + point.x / fixed_scale;
            const float corner_z = z - point.z / fixed_scale;
            result.left = std::min(result.left, corner_x);
            result.right = std::max(result.right, corner_x);
            result.top = std::min(result.top, corner_z);
            result.bottom = std::max(result.bottom, corner_z);
        }
    }
    return result;
}

/// Where the frame's drawn structures level the ground: their plots,
/// sorted, with an index of them by place so that each triangle cut visits
/// only the structures near it. Very large plots retain a bounded global
/// fallback.
struct Plots {
    std::vector<Foundation> plots{};
    /// The plots that reach each bin of the map, in plot order, by
    /// plot_bin_key.
    std::unordered_map<uint64_t, std::vector<std::size_t>> bins{};
    std::vector<std::size_t> global{}; ///< the plots every triangle visits
    /// The bounds of every plot with the ground blended around it.
    float left{};
    float right{};
    float top{};
    float bottom{};
};

/// The ground rises back to the terrain over this many map pixels around a plot.
constexpr float plot_blend_width = cell_edge;

/// A plot bin's edge, in map pixels.
constexpr float plot_bin_edge = 128;

/// The most bins one plot is listed in; a larger one is visited by every triangle.
constexpr std::size_t maximum_plot_bins = 64;

/// @param coordinate a map column, or a world row
/// @return the bin holding it along its axis
int32_t plot_bin(float coordinate) {
    return static_cast<int32_t>(std::floor(coordinate / plot_bin_edge));
}

/// @param x a bin along the map's columns
/// @param y a bin along its world rows
/// @return the bin's key in Plots::bins
uint64_t plot_bin_key(int32_t x, int32_t y) {
    return (uint64_t{static_cast<uint32_t>(y)} << 32) | static_cast<uint32_t>(x);
}

/// Lists the plots of the frame's drawn structures on the ground
/// (foundations), without changing map or unit records.
///
/// @param in the frame
/// @param[out] plots the plots, sorted and indexed; none when no structure
///     levels the ground
void plan_plots(const Inputs& in, Plots& plots) {
    plots = {};
    for (const auto& draw : in.draws->models) {
        const auto* unit = draw.model.unit;
        if (draw.stand_in >= 0) {
            if (static_cast<std::size_t>(draw.stand_in) >= in.draws->stand_ins.size())
                continue;
            unit = &in.draws->stand_ins[static_cast<std::size_t>(draw.stand_in)];
        }
        const auto* definition = draw.model.def;
        constexpr uint32_t excluded = OA_UNIT_DEF_FLAG_CAN_FLY | OA_UNIT_DEF_FLAG_CAN_HOVER |
                                      OA_UNIT_DEF_FLAG_FLOATER | OA_UNIT_DEF_FLAG_DIGGER;
        if (!draw.plan.linked.drawn || draw.on_map || unit == nullptr ||
            (unit->flags & OA_UNIT_FLAG_BUILDING) == 0 || unit->footprint_x <= 0 ||
            unit->footprint_z <= 0 || (definition && (definition->flags & excluded) != 0))
            continue;
        const float height = unit->position.y / fixed_scale;
        if (height < in.sea_level)
            continue;
        const float centre_x = unit->position.x / fixed_scale;
        const float centre_y = unit->position.z / fixed_scale;
        if (fog_at(
                in.sight,
                static_cast<int32_t>(centre_x),
                static_cast<int32_t>(centre_y - height * height_lift)
            ) == Fog::black)
            continue;
        // Canonical footprints already include the building's facing.
        const float half_width = unit->footprint_x * cell_edge * 0.5F;
        const float half_depth = unit->footprint_z * cell_edge * 0.5F;
        // The base piece where the models pass draws the model.
        const auto shown = draw.shown_position.value_or(unit->position);
        const auto footing = base_piece(draw, shown.x / fixed_scale, shown.z / fixed_scale);
        const float left = std::min(centre_x - half_width, footing.left);
        const float right = std::max(centre_x + half_width, footing.right);
        const float top = std::min(centre_y - half_depth, footing.top);
        const float bottom = std::max(centre_y + half_depth, footing.bottom);
        plots.plots.push_back({left, right, top, bottom, height - footing.depth});
    }
    auto& sorted = plots.plots;
    if (sorted.empty())
        return;
    std::sort(sorted.begin(), sorted.end(), [](const Foundation& a, const Foundation& b) {
        return std::array{a.left, a.right, a.top, a.bottom, a.height} <
               std::array{b.left, b.right, b.top, b.bottom, b.height};
    });
    std::size_t references{};
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        const auto& plot = sorted[index];
        const int32_t x0 = plot_bin(plot.left - plot_blend_width);
        const int32_t x1 = plot_bin(plot.right + plot_blend_width);
        const int32_t y0 = plot_bin(plot.top - plot_blend_width);
        const int32_t y1 = plot_bin(plot.bottom + plot_blend_width);
        const auto count = static_cast<uint64_t>(int64_t{x1} - x0 + 1) *
                           static_cast<uint64_t>(int64_t{y1} - y0 + 1);
        if (count > maximum_plot_bins || count + references > maximum_vertices) {
            plots.global.push_back(index);
            continue;
        }
        references += static_cast<std::size_t>(count);
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x)
                plots.bins[plot_bin_key(x, y)].push_back(index);
    }
    plots.left = sorted[0].left - plot_blend_width;
    plots.right = sorted[0].right + plot_blend_width;
    plots.top = sorted[0].top - plot_blend_width;
    plots.bottom = sorted[0].bottom + plot_blend_width;
    for (const auto& plot : sorted) {
        plots.left = std::min(plots.left, plot.left - plot_blend_width);
        plots.right = std::max(plots.right, plot.right + plot_blend_width);
        plots.top = std::min(plots.top, plot.top - plot_blend_width);
        plots.bottom = std::max(plots.bottom, plot.bottom + plot_blend_width);
    }
}

/// Finds the plots whose bins an area reaches, in plot order.
///
/// @param plots the frame's plots
/// @param[in,out] indices memory for the plots of an area of several bins
/// @param min_x the area's least map column
/// @param max_x its greatest
/// @param min_y its least world row
/// @param max_y its greatest
/// @return the plots, good until indices changes
std::span<const std::size_t> nearby_plots(
    const Plots& plots,
    std::vector<std::size_t>& indices,
    float min_x,
    float max_x,
    float min_y,
    float max_y
) {
    const int32_t x0 = plot_bin(min_x), x1 = plot_bin(max_x);
    const int32_t y0 = plot_bin(min_y), y1 = plot_bin(max_y);
    // Most triangles occupy one bin. Its entries already follow plot order.
    if (x0 == x1 && y0 == y1 && plots.global.empty()) {
        if (const auto found = plots.bins.find(plot_bin_key(x0, y0)); found != plots.bins.end())
            return found->second;
        return {};
    }
    indices.assign(plots.global.begin(), plots.global.end());
    const auto count =
        static_cast<uint64_t>(int64_t{x1} - x0 + 1) * static_cast<uint64_t>(int64_t{y1} - y0 + 1);
    if (count > maximum_plot_bins) {
        indices.resize(plots.plots.size());
        for (std::size_t i = 0; i < plots.plots.size(); ++i)
            indices[i] = i;
        return indices;
    }
    for (int32_t y = y0; y <= y1; ++y)
        for (int32_t x = x0; x <= x1; ++x)
            if (const auto found = plots.bins.find(plot_bin_key(x, y)); found != plots.bins.end())
                indices.insert(indices.end(), found->second.begin(), found->second.end());
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    return indices;
}

/// What a thread cutting plots keeps from triangle to triangle and from
/// frame to frame, so that cutting allocates almost nothing.
struct PlotCutting {
    std::vector<std::size_t> indices{}; ///< nearby_plots' memory
    /// The pieces of the triangle being cut, and those of its next split.
    std::vector<std::vector<Vertex>> pieces{};
    std::vector<std::vector<Vertex>> split{};
    /// The vertices listed for the triangles a plot reached.
    std::vector<std::array<std::size_t, 2>> affected_ranges{};
};

/// Cuts the frame's plots into triangles of the ground, or of what lies on
/// it, and levels them there (foundations). Each triangle a plot reaches is
/// split at the plot's edges and the blend's, and its pieces take its
/// place; the others are copied as they are. Each triangle is cut alone,
/// so a run of triangles cut in parts gives the triangles it gives whole.
///
/// The ground under a plot lies flat a little below the structure's
/// height, or below its base piece's lowest corner where that is lower, so
/// that no part of the base is hidden at any angle; it rises back to the
/// terrain over one cell around. Ground decals share the split geometry and
/// keep their small lift above the levelled ground.
///
/// @param plots the frame's plots, at least one
/// @param source the triangles
/// @param decals the triangles lie on the ground, rather than being it
/// @param[in,out] out where the triangles go, after what it holds already:
///     a whole number of triangles
/// @return true when a plot reached any of the triangles
bool cut_plots(
    const Plots& plots, std::span<const Vertex> source, bool decals, std::vector<Vertex>& out
) {
    constexpr float blend_width = plot_blend_width;
    constexpr float ground_separation = 0.1F;
    constexpr float decal_lift = 0.05F;
    thread_local PlotCutting cutting;
    auto& indices = cutting.indices;
    auto& pieces = cutting.pieces;
    auto& split = cutting.split;
    auto& affected_ranges = cutting.affected_ranges;
    affected_ranges.clear();
    const auto& sorted = plots.plots;
    bool reached{};
    std::size_t piece_count{};
    std::size_t split_count{};
    const auto next_split = [&split, &split_count]() -> std::vector<Vertex>& {
        if (split_count == split.size())
            split.emplace_back();
        return split[split_count++];
    };
    const auto fan = [&out](std::span<const Vertex> corners) {
        if (corners.size() < 3)
            return;
        for (std::size_t corner = 1; corner + 1 < corners.size(); ++corner)
            out.insert(out.end(), {corners[0], corners[corner], corners[corner + 1]});
    };
    std::size_t pending_begin{};
    // Most geometry lies outside foundations. Copy each untouched run once.
    const auto copy_pending = [&](std::size_t end) {
        out.insert(out.end(), source.begin() + pending_begin, source.begin() + end);
        pending_begin = end;
    };
    for (std::size_t first = 0; first + 2 < source.size(); first += 3) {
        const auto triangle = source.subspan(first, 3);
        float min_x = triangle[0].x, max_x = min_x;
        float min_y = triangle[0].z, max_y = min_y;
        for (const auto& vertex : triangle) {
            min_x = std::min(min_x, vertex.x);
            max_x = std::max(max_x, vertex.x);
            const float y = vertex.z;
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
        if (max_x <= plots.left || min_x >= plots.right || max_y <= plots.top ||
            min_y >= plots.bottom) {
            continue;
        }
        piece_count = 0;
        bool affected{};
        for (const auto plot_index : nearby_plots(plots, indices, min_x, max_x, min_y, max_y)) {
            const auto& [left, right, top, bottom, height] = sorted[plot_index];
            if (max_x <= left - blend_width || min_x >= right + blend_width ||
                max_y <= top - blend_width || min_y >= bottom + blend_width)
                continue;
            affected = true;
            if (piece_count == 0) {
                if (pieces.empty())
                    pieces.emplace_back();
                pieces.front().assign(triangle.begin(), triangle.end());
                piece_count = 1;
            }
            for (const bool horizontal : {true, false}) {
                const float low = horizontal ? left : top;
                const float high = horizontal ? right : bottom;
                for (const float edge : {low - blend_width, low, high, high + blend_width}) {
                    split_count = 0;
                    for (std::size_t index = 0; index < piece_count; ++index) {
                        auto& piece = pieces[index];
                        float minimum = std::numeric_limits<float>::infinity();
                        float maximum = -minimum;
                        double area{};
                        for (std::size_t i = 0; i < piece.size(); ++i) {
                            const auto& a = piece[i];
                            const auto& b = piece[(i + 1) % piece.size()];
                            const float coordinate = horizontal ? a.x : a.z;
                            minimum = std::min(minimum, coordinate);
                            maximum = std::max(maximum, coordinate);
                            area += double(a.x) * (b.z) - double(b.x) * (a.z);
                        }
                        if (minimum >= edge || maximum <= edge) {
                            // Neither half needs new vertices. Keep the same area
                            // rejection as clipping, including degenerate borders.
                            if (std::abs(area) >= 0.000001)
                                std::swap(next_split(), piece);
                            continue;
                        }
                        for (const bool greater : {false, true}) {
                            auto& side = next_split();
                            clip_foundation(piece, horizontal, edge, greater, side);
                            if (side.size() < 3)
                                --split_count;
                        }
                    }
                    std::swap(pieces, split);
                    piece_count = split_count;
                }
            }
        }
        if (!affected)
            continue;
        reached = true;
        copy_pending(first);
        const auto output_begin = out.size();
        if (piece_count == 0)
            fan(triangle);
        else
            for (std::size_t index = 0; index < piece_count; ++index)
                fan(pieces[index]);
        if (!affected_ranges.empty() && affected_ranges.back()[1] == output_begin)
            affected_ranges.back()[1] = out.size();
        else
            affected_ranges.push_back({output_begin, out.size()});
        pending_begin = first + 3;
    }
    copy_pending(source.size());
    // Blend once from the original terrain. An interior always wins over a
    // neighbouring skirt; tied skirts choose a stable height, not draw order.
    std::span<const std::size_t> candidates;
    for (const auto& range : affected_ranges)
        for (std::size_t index = range[0]; index < range[1]; ++index) {
            if (index % 3 == 0) {
                const auto& a = out[index];
                const auto& b = out[index + 1];
                const auto& c = out[index + 2];
                const float ay = a.z;
                const float by = b.z;
                const float cy = c.z;
                candidates = nearby_plots(
                    plots,
                    indices,
                    std::min({a.x, b.x, c.x}),
                    std::max({a.x, b.x, c.x}),
                    std::min({ay, by, cy}),
                    std::max({ay, by, cy})
                );
            }
            if (candidates.empty())
                continue;
            auto& vertex = out[index];
            const float y = vertex.z;
            float strongest{}, target{};
            const std::size_t first = index / 3 * 3;
            float centre_x{}, centre_y{};
            for (std::size_t corner = first; corner < first + 3; ++corner) {
                centre_x += out[corner].x / 3;
                centre_y += out[corner].z / 3;
            }
            for (const auto plot_index : candidates) {
                const auto& plot = sorted[plot_index];
                if (centre_x > plot.left && centre_x < plot.right && centre_y > plot.top &&
                    centre_y < plot.bottom) {
                    strongest = 1;
                    target = plot.height;
                    break;
                }
                const float distance = std::max(
                    {plot.left - vertex.x,
                     vertex.x - plot.right,
                     plot.top - y,
                     y - plot.bottom,
                     0.0F}
                );
                const float blend = std::clamp(1 - distance / blend_width, 0.0F, 1.0F);
                if (blend > strongest || (blend == strongest && plot.height < target)) {
                    strongest = blend;
                    target = plot.height;
                }
            }
            if (strongest == 0)
                continue;
            target -= ground_separation;
            if (decals)
                target += decal_lift;
            vertex.height = std::lerp(vertex.height, target, strongest);
        }
    return reached;
}

/// Rejects the back of a model face, including opposite faces sharing one
/// plane, from its corners' x, height and z.
template <typename Corner>
bool back_facing_corners(std::span<const Corner> corners, const std::array<float, 3>& view) {
    if (view == std::array<float, 3>{})
        return false;
    std::array<float, 3> normal{};
    for (std::size_t index = 0; index < corners.size(); ++index) {
        const auto& a = corners[index];
        const auto& b = corners[(index + 1) % corners.size()];
        normal[0] += (a.height - b.height) * (a.z + b.z);
        normal[1] += (a.z - b.z) * (a.x + b.x);
        normal[2] += (a.x - b.x) * (a.height + b.height);
    }
    // Source faces wind clockwise when seen from outside in world coordinates.
    return normal[0] * view[0] + normal[1] * view[1] + normal[2] * view[2] >= 0;
}

/// Rejects the back of a model face, including opposite faces sharing one plane.
bool back_facing(std::span<const Vertex> corners, const std::array<float, 3>& view) {
    return back_facing_corners(corners, view);
}

/// Draws each source polygon edge as a narrow camera-facing nanoframe ribbon.
void nanoframe(
    const Inputs& in,
    Pass& pass,
    std::span<const Vertex> corners,
    float remaining,
    std::optional<AtlasRect>& cached
) {
    auto palette = *in.palette;
    palette[0] = 48;
    palette[1] = 224;
    palette[2] = 80;
    constexpr std::array<uint8_t, 1> pixel{0};
    if (!cached)
        cached = material(pass, palette, 1, 1, pixel, {}, -1, Fog::seen);
    const auto rect = *cached;
    auto view = in.view_direction;
    if (view == std::array<float, 3>{})
        view = {0, 1, height_lift};
    constexpr float half_width = 0.35F;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto& a = corners[i];
        const auto& b = corners[(i + 1) % corners.size()];
        const std::array edge{b.x - a.x, b.height - a.height, b.z - a.z};
        std::array side{
            edge[1] * view[2] - edge[2] * view[1],
            edge[2] * view[0] - edge[0] * view[2],
            edge[0] * view[1] - edge[1] * view[0]
        };
        const float length = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        if (length < 0.0001F)
            continue;
        for (auto& value : side)
            value *= half_width / length;
        const auto point = [&](const Vertex& end, float sign) {
            auto result = end;
            result.x += side[0] * sign;
            result.height += side[1] * sign;
            result.z += side[2] * sign;
            result.u = (rect.x + 0.5F) / pass.scene.texture_width;
            result.v = (rect.y + 0.5F) / pass.scene.texture_height;
            result.opacity = std::min(1.0F, remaining * 4);
            result.red = result.green = result.blue = 1;
            return result;
        };
        const std::array ribbon{point(a, -1), point(b, -1), point(b, 1), point(a, 1)};
        polygon(pass, ribbon);
    }
}

/// Interpolates a water cutout edge while retaining its atlas material.
Vertex water_edge(const Vertex& a, const Vertex& b, float t) {
    auto out = a;
    out.x = std::lerp(a.x, b.x, t);
    out.height = std::lerp(a.height, b.height, t);
    out.z = std::lerp(a.z, b.z, t);
    out.u = std::lerp(a.u, b.u, t);
    out.v = std::lerp(a.v, b.v, t);
    out.brightness = std::lerp(a.brightness, b.brightness, t);
    out.red = std::lerp(a.red, b.red, t);
    out.green = std::lerp(a.green, b.green, t);
    out.blue = std::lerp(a.blue, b.blue, t);
    out.opacity = std::lerp(a.opacity, b.opacity, t);
    out.terrain_x = std::lerp(a.terrain_x, b.terrain_x, t);
    out.terrain_y = std::lerp(a.terrain_y, b.terrain_y, t);
    return out;
}

/// Clips a triangle to positive water depth, including the shoreline intersection.
///
/// @param[in,out] listing the scene, or a pass, the triangle is listed in
/// @param triangle the triangle
template <typename Listing>
void wet_polygon(Listing& listing, const std::array<Vertex, 3>& triangle) {
    std::array<Vertex, 6> clipped{};
    std::size_t count{};
    auto previous = triangle.back();
    for (const auto& current : triangle) {
        const bool inside = current.terrain_x > 0;
        if (inside != (previous.terrain_x > 0)) {
            const float t = previous.terrain_x / (previous.terrain_x - current.terrain_x);
            clipped[count] = water_edge(previous, current, t);
            clipped[count++].terrain_x = 0;
        }
        if (inside)
            clipped[count++] = current;
        previous = current;
    }
    polygon(listing, {clipped.data(), count});
}

/// Samples the height grid continuously so reflected triangles cannot bridge a beach.
float water_depth(const Inputs& in, float x, float z) {
    if (x < 0 || z < 0 || x >= in.map->attribute_width * cell_edge ||
        z >= in.map->attribute_height * cell_edge)
        return 0;
    const float left = std::floor(x / cell_edge) * cell_edge;
    const float top = std::floor(z / cell_edge) * cell_edge;
    const float tx = (x - left) / cell_edge;
    const float tz = (z - top) / cell_edge;
    const auto sample = [&](int dx, int dz) {
        return height_at(*in.map, static_cast<int32_t>(left) + dx, static_cast<int32_t>(top) + dz);
    };
    return in.sea_level - std::lerp(
                              std::lerp(sample(0, 0), sample(cell_edge, 0), tx),
                              std::lerp(sample(0, cell_edge), sample(cell_edge, cell_edge), tx),
                              tz
                          );
}

/// Subdivides only large reflected faces before clipping against shallow coast relief.
void reflection_triangle(
    const Inputs& in,
    Pass& pass,
    std::array<Vertex, 3> triangle,
    uint32_t depth,
    std::size_t& remaining
) {
    if (remaining < 6)
        return;
    float longest{};
    std::size_t edge{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& a = triangle[i];
        const auto& b = triangle[(i + 1) % 3];
        const float length = std::hypot(a.x - b.x, a.z - b.z);
        if (length > longest) {
            longest = length;
            edge = i;
        }
    }
    if (longest > cell_edge && depth < 5) {
        const auto middle = water_edge(triangle[edge], triangle[(edge + 1) % 3], 0.5F);
        auto first = triangle;
        auto second = triangle;
        first[edge] = middle;
        second[(edge + 1) % 3] = middle;
        reflection_triangle(in, pass, first, depth + 1, remaining);
        reflection_triangle(in, pass, second, depth + 1, remaining);
        return;
    }
    for (auto& vertex : triangle) {
        vertex.terrain_x = water_depth(in, vertex.x, vertex.z);
        vertex.brightness *=
            brightness_at(in.sight, vertex.x, vertex.z - in.sea_level * height_lift);
    }
    const auto begin = pass.list.vertices.size();
    wet_polygon(pass, triangle);
    remaining -= pass.list.vertices.size() - begin;
}

/// Mirrors visible floating hulls onto water without a second scene render pass.
void naval_reflection(
    const Inputs& in, Pass& pass, std::span<const Vertex> source, std::size_t& budget
) {
    const float elevation = std::abs(in.view_direction[1]);
    const float horizon = std::clamp((elevation - 0.01F) / 0.14F, 0.0F, 1.0F);
    const float angle_fade = horizon * horizon * (3 - 2 * horizon);
    if (in.water.reflections <= 0 || in.water.detail <= 0 || angle_fade <= 0)
        return;
    const float dx = in.view_direction[0] / in.view_direction[1];
    const float dz = in.view_direction[2] / in.view_direction[1];
    for (std::size_t index = 0; index + 2 < source.size() && budget >= 6; index += 3) {
        const auto triangle = source.subspan(index, 3);
        std::array<Vertex, 6> clipped{};
        std::size_t count{};
        auto previous = triangle.back();
        for (const auto& current : triangle) {
            const bool inside = current.height >= in.sea_level;
            if (inside != (previous.height >= in.sea_level)) {
                const float t =
                    (in.sea_level - previous.height) / (current.height - previous.height);
                clipped[count++] = water_edge(previous, current, t);
            }
            if (inside)
                clipped[count++] = current;
            previous = current;
        }
        for (std::size_t corner = 0; corner < count; ++corner) {
            auto& vertex = clipped[corner];
            const float altitude = std::max(0.0F, vertex.height - in.sea_level);
            // Intersect the ray from the mirrored point with the water plane.
            // One world-space altitude shift gives the full doubled screen displacement.
            const float travel = std::hypot(dx, dz) * altitude;
            const float travel_fade = std::clamp((384.0F - travel) / 128.0F, 0.0F, 1.0F);
            const float bounded_altitude = travel > 384 ? altitude * 384 / travel : altitude;
            vertex.x += dx * bounded_altitude;
            vertex.z += dz * bounded_altitude;
            vertex.x += std::sin(vertex.z * 0.12F + in.water.seconds * 1.3F) * 0.65F;
            vertex.height = in.sea_level + 0.12F;
            vertex.opacity *=
                std::min(
                    renderer_water_appearance::reflection_cap,
                    renderer_water_appearance::reflection_opacity * in.water.reflections
                ) *
                in.water.detail * angle_fade * travel_fade *
                std::clamp(1 - altitude / 100, 0.0F, 1.0F);
            vertex.brightness *= renderer_water_appearance::reflection_brightness;
            vertex.emission_r = vertex.emission_g = vertex.emission_b = 0;
            vertex.terrain_size = -3;
            vertex.terrain_x = vertex.terrain_y = 0;
        }
        for (std::size_t corner = 1; corner + 1 < count; ++corner)
            reflection_triangle(
                in, pass, {clipped[0], clipped[corner], clipped[corner + 1]}, 0, budget
            );
    }
}

/// Tells whether a piece's animated textures show their first frame, as the
/// classic tiers draw the piece (the processor's cached image, and the Full
/// tier's model stage): a piece the unit's image holds always does, and an
/// unfinished unit's whole depth image does while its moving pieces wait
/// for it to be built; a moving piece shows the running frame, unless the
/// unit is drawn unlit over an image without a depth plane. A unit drawn
/// without an image shows the first frame while it is unlit.
///
/// @param draw the unit's planned draw
/// @param unit the unit it draws
/// @param cached the piece is one the unit's image holds
/// @param moving_pieces_once_built Inputs::moving_pieces_once_built
/// @return true for the first frame, false for the running one
bool shows_first_frame(
    const ModelDraw& draw, const Unit& unit, bool cached, bool moving_pieces_once_built
) {
    const auto* state = draw.model.state;
    const bool image =
        draw.plan.model.from_image && state != nullptr && state->image.sprite.data != nullptr;
    if (!image)
        return draw.plan.linked.unlit;
    const bool depth_image = state->image.sprite.aux != nullptr;
    if (depth_image && unit.build_remaining != 0.0F)
        return (unit.flags & OA_UNIT_FLAG_BUILDING) != 0 || moving_pieces_once_built;
    if (cached)
        return true;
    return !depth_image && draw.plan.linked.unlit;
}

/// The atlas places of the colours and frames the models pass has found
/// this frame, which its draws on the card and in the vertices share.
struct ModelMaterials {
    std::array<std::optional<AtlasRect>, 256>& colors;
    std::unordered_map<const Sprite*, AtlasRect>& textures;
};

/// Counts the vertices a model drawn on the card would have listed against
/// the frame's geometry budget, as listing() counts them.
///
/// @param[in,out] pass the models pass
/// @param count the vertices
void count_on_card(Pass& pass, std::size_t count) {
    auto& list = pass.list;
    const std::size_t held = list.vertices.size() + list.cards.vertices + count;
    list.peak = std::max(list.peak, held);
    if (held > maximum_vertices - std::min(maximum_vertices, pass.listed_before))
        throw GeometryFull();
    list.cards.vertices += count;
}

/// Returns where a type's textures for a team start in the frame's model
/// draws, made unfound the first time the frame draws the type for it.
///
/// @param[in,out] cards the frame's model draws
/// @param type the type's place in the types kept
/// @param slots its textures
/// @param team the team colour it draws with
/// @return the first texture's place
uint32_t card_table(ModelCards& cards, uint32_t type, std::size_t slots, uint8_t team) {
    for (auto table = cards.tables.rbegin(); table != cards.tables.rend(); ++table)
        if (table->type == type && table->team == team)
            return table->first;
    const auto first = static_cast<uint32_t>(cards.slot_states.size());
    cards.slot_states.resize(cards.slot_states.size() + slots, 0);
    cards.textures.resize(cards.textures.size() + 4 * slots);
    cards.tables.push_back({type, team, first});
    return first;
}

/// Finds a texture of a type in the atlas for a team, as the models pass
/// finds a polygon's: its colour's material, or its frame's, which a frame
/// that is not drawn leaves drawn nowhere. A pass that may not pack stops
/// at a material not yet packed (Unpacked).
///
/// @param in the frame
/// @param[in,out] pass the models pass
/// @param[in,out] materials the places the pass has found
/// @param slot the texture
/// @param at its place in the frame's model draws
/// @param team the team colour it draws with
void find_card_slot(
    const Inputs& in,
    Pass& pass,
    ModelMaterials& materials,
    const CardSlot& slot,
    uint32_t at,
    uint8_t team
) {
    auto& cards = pass.list.cards;
    const bool colored = slot.primitive == nullptr;
    AtlasRect rect{};
    if (colored) {
        auto& cached = materials.colors[slot.color];
        if (!cached) {
            const std::array pixel{slot.color};
            cached = material(pass, *in.palette, 1, 1, pixel, {}, -1, Fog::seen);
        }
        rect = *cached;
    } else {
        const auto* frame = model::primitive_texture(*slot.primitive, slot.first_frame, team);
        if (frame == nullptr || frame->data == nullptr || frame->encoding != OA_SPRITE_RAW) {
            cards.slot_states[at] = 2;
            return;
        }
        auto found = materials.textures.find(frame);
        if (found == materials.textures.end())
            found = materials.textures
                        .emplace(
                            frame,
                            material(
                                pass,
                                *in.palette,
                                frame->width,
                                frame->height,
                                {static_cast<const uint8_t*>(frame->data),
                                 static_cast<std::size_t>(frame->width) * frame->height},
                                {},
                                -1,
                                Fog::seen
                            )
                        )
                        .first;
        rect = found->second;
    }
    // A polygon's first and third corners' places, as the vertices take them.
    constexpr std::array<float, 4> u{0, 1, 1, 0};
    constexpr std::array<float, 4> v{0, 0, 1, 1};
    float* place = cards.textures.data() + std::size_t{at} * 4;
    for (const std::size_t index : {std::size_t{0}, std::size_t{2}}) {
        *place++ = (rect.x + 0.5F + (colored ? 0 : u[index] * (rect.width - 1))) /
                   pass.scene.texture_width;
        *place++ = (rect.y + 0.5F + (colored ? 0 : v[index] * (rect.height - 1))) /
                   pass.scene.texture_height;
    }
    cards.slot_states[at] = 1;
}

/// Draws a model on the card where it can (Inputs::models_on_card): its
/// type's triangles that its vertices would have listed, in their order,
/// with the textures they would have taken, from its vertices placed as
/// they would have been. The caller has found it drawn whole, unlit by its
/// build, uncloaked, unreflected and unshaded.
///
/// @param in the frame
/// @param[in,out] pass the models pass
/// @param[in,out] materials the places the pass has found
/// @param draw the model's draw
/// @param unit the unit or stand-in it draws
/// @param place its world place: x, height and z
/// @param brightness its light from the fog
/// @param image it draws from its picture
/// @param lights the nanolathe particles that light units' vertices
/// @param[in,out] corners room for a polygon's corners
/// @param[in,out] points room for its corners through the camera
/// @return false where the card cannot draw it, having added nothing
bool draw_on_card(
    const Inputs& in,
    Pass& pass,
    ModelMaterials& materials,
    const ModelDraw& draw,
    const Unit& unit,
    const std::array<float, 3>& place,
    float brightness,
    bool image,
    std::span<const std::array<float, 3>> lights,
    std::vector<CardCorner>& corners,
    std::vector<std::array<float, 3>>& points
) {
    auto& cards = pass.list.cards;
    const auto& reference = draw.model;
    if (cards.instances.size() >= renderer_live_gpu::largest_model_instance_count)
        return false;
    const int32_t found =
        card_type(pass.scene.card_types, *reference.instance, *reference.prepared);
    if (found < 0)
        return false;
    const auto type_index = static_cast<uint32_t>(found);
    const auto& type = pass.scene.card_types.types[type_index];
    const auto pieces = reference.instance->pieces();
    // Its vertices, placed as its vertices would be.
    const auto [x, h, z] = place;
    const std::size_t first_position = cards.positions.size();
    std::array<float, 3> low{x, h, z};
    std::array<float, 3> high{x, h, z};
    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        const auto& points = piece < draw.shown_vertices.size()
                                 ? draw.shown_vertices[piece]
                                 : pieces[piece].transformed_vertices;
        if (points.size() != type.piece_vertices[piece + 1] - type.piece_vertices[piece]) {
            cards.positions.resize(first_position);
            return false;
        }
        for (const auto& point : points) {
            const std::array world{
                x + point.x / fixed_scale, h + point.y / fixed_scale, z - point.z / fixed_scale
            };
            for (std::size_t axis = 0; axis < world.size(); ++axis) {
                low[axis] = std::min(low[axis], world[axis]);
                high[axis] = std::max(high[axis], world[axis]);
            }
            cards.positions.insert(cards.positions.end(), world.begin(), world.end());
        }
    }
    // A nanolathe particle within a light's reach of a vertex would light
    // it; such a model stays in the vertices. A particle that far from the
    // box around the vertices reaches none of them, as the vertices'
    // distances are worked out.
    constexpr float light_radius = 32.0F;
    for (const auto& light : lights) {
        std::array<float, 3> gap{};
        for (std::size_t axis = 0; axis < gap.size(); ++axis)
            gap[axis] = std::max({0.0F, low[axis] - light[axis], light[axis] - high[axis]});
        if (gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2] >= light_radius * light_radius)
            continue;
        for (std::size_t at = first_position; at < cards.positions.size(); at += 3) {
            const float dx = cards.positions[at] - light[0];
            const float dy = cards.positions[at + 1] - light[1];
            const float dz = cards.positions[at + 2] - light[2];
            if (dx * dx + dy * dy + dz * dz < light_radius * light_radius) {
                cards.positions.resize(first_position);
                return false;
            }
        }
    }
    // Each piece's first frame or its running one.
    const bool first_cached = shows_first_frame(draw, unit, true, in.moving_pieces_once_built);
    const bool first_moving = shows_first_frame(draw, unit, false, in.moving_pieces_once_built);
    const auto first_piece = static_cast<uint32_t>(cards.piece_frames.size());
    for (const auto& piece : pieces)
        cards.piece_frames.push_back(
            (piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::cached)) != 0
                ? first_cached
                : first_moving
        );
    const uint8_t team = in.team_colors[unit.owner_index % in.team_colors.size()];
    const uint32_t table = card_table(cards, type_index, type.slots.size(), team);
    const auto instance = static_cast<uint32_t>(cards.instances.size());
    cards.instances.push_back(
        {type.first_record,
         type.record_count,
         static_cast<uint32_t>(first_position / 3),
         type.piece_vertices.back(),
         first_piece,
         static_cast<uint32_t>(pieces.size()),
         table,
         static_cast<uint32_t>(type.slots.size()),
         brightness}
    );
    // A run takes the models drawn one after another among the vertices,
    // as many as its indices can name.
    const std::size_t vertex = pass.list.vertices.size();
    if (cards.runs.empty() || cards.runs.back().vertex != vertex ||
        instance - cards.runs.back().first_instance >= renderer_live_gpu::model_run_instances)
        cards.runs.push_back({vertex, static_cast<uint32_t>(cards.indices.size()), 0, instance});
    auto& run = cards.runs.back();
    const uint32_t named = (instance - run.first_instance) << renderer_live_gpu::model_corner_bits;
    const auto first_index = static_cast<uint32_t>(cards.indices.size());
    const auto& source = reference.instance->model();
    const float* positions = cards.positions.data() + first_position;
    // The bounds the pick takes, through the camera as it projects vertices.
    const auto& camera = in.clip_projection;
    std::array<float, 4> bounds{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()
    };
    bool bounded = true;
    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        if ((pieces[piece].flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::visible)) ==
            0)
            continue;
        const auto& object = source.objects[type.objects[piece]];
        const float* piece_positions = positions + std::size_t{type.piece_vertices[piece]} * 3;
        const bool first = cards.piece_frames[first_piece + piece] != 0;
        for (uint32_t at = type.piece_polygons[piece]; at < type.piece_polygons[piece + 1]; ++at) {
            const auto& polygon = type.polygons[at];
            // A polygon of the image key is clear in a unit's picture.
            if (polygon.colored && image && polygon.color == model::image_key)
                continue;
            const uint32_t slot = table + (first ? polygon.first_slot : polygon.running_slot);
            if (cards.slot_states[slot] == 0)
                find_card_slot(in, pass, materials, type.slots[slot - table], slot, team);
            if (cards.slot_states[slot] != 1)
                continue;
            const auto& indices = object.primitives[polygon.primitive].vertex_indices;
            corners.clear();
            for (const uint16_t index : indices) {
                const float* point = piece_positions + std::size_t{index} * 3;
                corners.push_back({point[0], point[1], point[2]});
            }
            if (back_facing_corners(std::span<const CardCorner>(corners), in.view_direction))
                continue;
            const std::size_t count = (polygon.corners - 2) * 3;
            count_on_card(pass, count);
            for (uint32_t corner = 0; corner < count; ++corner)
                cards.indices.push_back(named | (polygon.record + corner));
            points.clear();
            for (const auto& corner : corners) {
                const float across = camera[0] * corner.x + camera[1] * corner.height +
                                     camera[2] * corner.z + camera[3];
                const float down = camera[4] * corner.x + camera[5] * corner.height +
                                   camera[6] * corner.z + camera[7];
                bounds = {
                    std::min(bounds[0], across),
                    std::min(bounds[1], down),
                    std::max(bounds[2], across),
                    std::max(bounds[3], down)
                };
                points.push_back({across, down, 0});
            }
            for (std::size_t fan = 1; bounded && fan + 1 < points.size(); ++fan)
                bounded = renderer_live_camera::triangle_hit_bounded(
                    {points[0], points[fan], points[fan + 1]}
                );
        }
    }
    const auto index_count = static_cast<uint32_t>(cards.indices.size() - first_index);
    run.index_count += index_count;
    const bool picked = !draw.on_map && unit.id != 0;
    auto& drawn = cards.units.emplace_back();
    drawn.unit_id = picked ? unit.id : uint16_t{0};
    drawn.vertex = vertex;
    drawn.instance = instance;
    drawn.type = type_index;
    drawn.first_index = first_index;
    drawn.index_count = index_count;
    if (index_count != 0) {
        drawn.left = bounds[0];
        drawn.top = bounds[1];
        drawn.right = bounds[2];
        drawn.bottom = bounds[3];
        drawn.bounded = bounded;
    }
    return true;
}

/// Uses the actual animated, rotated model vertices prepared for this frame.
///
/// @param in the frame
/// @param[in,out] pass the models pass
/// @param first the first draw listed, in WorldDrawList::models
/// @param last the draw after the last
void models(const Inputs& in, Pass& pass, std::size_t first, std::size_t last) {
    auto& list = pass.list;
    std::size_t reflection_budget = 24576;
    std::array<std::optional<AtlasRect>, 256> colors;
    std::unordered_map<const Sprite*, AtlasRect> textures;
    ModelMaterials materials{colors, textures};
    std::vector<CardCorner> card_corners;
    std::vector<std::array<float, 3>> card_points;
    std::unordered_map<const Sprite*, AtlasRect> bright_textures;
    PaletteBytes bright_palette = *in.palette;
    for (auto& channel : bright_palette)
        channel = static_cast<uint8_t>(std::min(255, 2 * int(channel)));
    PaletteBytes white_palette{};
    white_palette[0] = white_palette[1] = white_palette[2] = 255;
    std::optional<AtlasRect> white_material;
    std::vector<present::gpu_world::VertexNormal> normals;
    const bool shade_ready =
        in.shade_table.size() >= 32 * 256 && std::isfinite(in.light_scale) && in.light_scale > 0 &&
        std::all_of(
            in.light.begin(), in.light.end(), [](float value) { return std::isfinite(value); }
        ) &&
        std::any_of(in.light.begin(), in.light.end(), [](float value) { return value != 0; });
    std::optional<AtlasRect> frame_material;
    const auto& projection = in.clip_projection;
    const bool screen_bounds = in.clip_width > 0 && in.clip_height > 0 &&
                               std::all_of(projection.begin(), projection.end(), [](float value) {
                                   return std::isfinite(value);
                               });
    // The nanolathe particles that light units' vertices: the emissive ones
    // in sight among an even sample of at most 16 of the frame's particles,
    // the same for every vertex, in map pixels.
    constexpr std::size_t light_samples = 16;
    constexpr float light_radius = 32.0F;
    std::vector<std::array<float, 3>> nanolathe_lights;
    if (in.nanolathe_lighting) {
        const auto& particles = in.draws->spatial_particles;
        const auto stride = std::max<std::size_t>(1, particles.size() / light_samples);
        for (std::size_t sample = 0, count = 0; sample < particles.size() && count < light_samples;
             sample += stride, ++count) {
            const auto& particle = particles[sample];
            if (!particle.emissive ||
                fog_at(
                    in.sight,
                    particle.position.x / 65536,
                    static_cast<int32_t>(
                        (particle.position.z - particle.position.y * 0.5F) / fixed_scale
                    )
                ) != Fog::seen)
                continue;
            nanolathe_lights.push_back(
                {particle.position.x / fixed_scale,
                 particle.position.y / fixed_scale,
                 particle.position.z / fixed_scale}
            );
        }
    }
    for (std::size_t draw_index = first; draw_index < last; ++draw_index) {
        begin_draw(pass, draw_index);
        const auto& draw = in.draws->models[draw_index];
        if (!draw.plan.linked.drawn)
            continue;
        const std::size_t first_vertex = list.vertices.size();
        const auto& reference = draw.model;
        const Unit* unit = reference.unit;
        if (draw.stand_in >= 0 &&
            static_cast<std::size_t>(draw.stand_in) < in.draws->stand_ins.size())
            unit = &in.draws->stand_ins[static_cast<std::size_t>(draw.stand_in)];
        if (unit == nullptr || reference.instance == nullptr || reference.prepared == nullptr)
            continue;
        // A cloaked unit, or every unit under the debug overlay, is blended
        // at half its opacity, as the classic view blends its picture.
        constexpr float translucent_opacity = 0.5F;
        const bool translucent =
            in.translucent_units || (unit->state_flags & model::unit_state_cloaked) != 0;
        const auto position = draw.shown_position.value_or(unit->position);
        const float x = position.x / fixed_scale;
        const float h = position.y / fixed_scale;
        const float z = position.z / fixed_scale;
        const auto fog =
            fog_at(in.sight, static_cast<int32_t>(x), static_cast<int32_t>(z - h * height_lift));
        if (fog == Fog::black)
            continue;
        const bool reflects =
            !draw.on_map && reference.def &&
            (reference.def->flags & OA_UNIT_DEF_FLAG_FLOATER) != 0 &&
            (reference.def->flags & (OA_UNIT_DEF_FLAG_CAN_FLY | OA_UNIT_DEF_FLAG_CAN_HOVER)) == 0 &&
            water_depth(in, x, z) > 1 && unit->build_remaining == 0 && in.water.reflections > 0 &&
            in.water.detail > 0 && reflection_budget >= 6;
        // Bound the actual animated geometry in the same screen space as the GPU.
        // Without a projection the painted crop only guarantees heights 0..255.
        float min_x = std::numeric_limits<float>::infinity();
        float min_y = min_x, max_x = -min_x, max_y = -min_x;
        bool bounded = true;
        std::size_t bounds_piece = 0;
        for (const auto& piece : reference.instance->pieces()) {
            const auto& points = bounds_piece < draw.shown_vertices.size()
                                     ? draw.shown_vertices[bounds_piece]
                                     : piece.transformed_vertices;
            ++bounds_piece;
            if ((piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::visible)) == 0)
                continue;
            for (const auto& point : points) {
                const float height = h + point.y / fixed_scale;
                constexpr float maximum_ground_height = 255;
                if (!screen_bounds && (height < 0 || height > maximum_ground_height)) {
                    bounded = false;
                    break;
                }
                const float world_x = x + point.x / fixed_scale;
                const float world_z = z - point.z / fixed_scale;
                const float bound_x = screen_bounds
                                          ? projection[0] * world_x + projection[1] * height +
                                                projection[2] * world_z + projection[3]
                                          : world_x;
                const float bound_y = screen_bounds
                                          ? projection[4] * world_x + projection[5] * height +
                                                projection[6] * world_z + projection[7]
                                          : world_z - height * height_lift;
                if (!std::isfinite(bound_x) || !std::isfinite(bound_y)) {
                    bounded = false;
                    break;
                }
                min_x = std::min(min_x, bound_x);
                max_x = std::max(max_x, bound_x);
                min_y = std::min(min_y, bound_y);
                max_y = std::max(max_y, bound_y);
                // A hull outside the viewport can still cast an on-screen reflection.
                if (reflects && height >= in.sea_level) {
                    const float mirrored_height = 2 * in.sea_level - height;
                    const float reflected_x =
                        screen_bounds ? projection[0] * world_x + projection[1] * mirrored_height +
                                            projection[2] * world_z + projection[3]
                                      : world_x;
                    const float reflected_y =
                        screen_bounds ? projection[4] * world_x + projection[5] * mirrored_height +
                                            projection[6] * world_z + projection[7]
                                      : world_z - mirrored_height * height_lift;
                    min_x = std::min(min_x, reflected_x);
                    max_x = std::max(max_x, reflected_x);
                    min_y = std::min(min_y, reflected_y);
                    max_y = std::max(max_y, reflected_y);
                }
            }
            if (!bounded)
                break;
        }
        constexpr float nanoframe_margin = 1;
        const float margin_x =
            screen_bounds ? nanoframe_margin * (std::abs(projection[0]) + std::abs(projection[1]) +
                                                std::abs(projection[2]))
                          : nanoframe_margin;
        const float margin_y =
            screen_bounds ? nanoframe_margin * (std::abs(projection[4]) + std::abs(projection[5]) +
                                                std::abs(projection[6]))
                          : nanoframe_margin;
        const float left = screen_bounds ? 0.0F : static_cast<float>(in.left);
        const float top = screen_bounds ? 0.0F : static_cast<float>(in.top);
        const float right = screen_bounds ? float(in.clip_width) : in.right;
        const float bottom = screen_bounds ? float(in.clip_height) : in.bottom;
        if (bounded && (max_x + margin_x < left || min_x - margin_x > right ||
                        max_y + margin_y < top || min_y - margin_y > bottom))
            continue;
        const float brightness = brightness_at(in.sight, x, z - h * height_lift);
        // The card draws a model whose vertices would all be drawn whole,
        // unlit by its build and unshaded, from its type's model.
        if (in.models_on_card && !translucent && !reflects &&
            std::clamp(unit->build_remaining, 0.0F, 1.0F) == 0) {
            const bool image =
                draw.plan.model.from_image && reference.state && reference.state->image.sprite.data;
            const bool shaded = shade_ready && image &&
                                (unit->flags & OA_UNIT_FLAG_BUILDING) != 0 &&
                                (in.graphics_flags & model::graphics_shading) != 0;
            if (!shaded && draw_on_card(
                               in,
                               pass,
                               materials,
                               draw,
                               *unit,
                               {x, h, z},
                               brightness,
                               image,
                               nanolathe_lights,
                               card_corners,
                               card_points
                           )) {
                ++list.drawn;
                continue;
            }
        }
        const std::array reflected_view{
            in.view_direction[0], -in.view_direction[1], in.view_direction[2]
        };
        std::vector<Vertex> reflected_faces;
        std::size_t piece_index = 0;
        for (const auto& piece : reference.instance->pieces()) {
            const auto& points = piece_index < draw.shown_vertices.size()
                                     ? draw.shown_vertices[piece_index]
                                     : piece.transformed_vertices;
            ++piece_index;
            if ((piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::visible)) == 0)
                continue;
            const auto& object = reference.instance->model().objects.at(piece.object_index);
            const auto& prepared = reference.prepared->objects.at(piece.object_index);
            const bool image =
                draw.plan.model.from_image && reference.state && reference.state->image.sprite.data;
            const bool rows =
                shade_ready && image && (unit->flags & OA_UNIT_FLAG_BUILDING) != 0 &&
                (in.graphics_flags & model::graphics_shading) != 0 &&
                (unit->build_remaining > 0 ||
                 (piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::cached)) != 0);
            const bool lit =
                rows &&
                (piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::shaded)) != 0;
            const bool first_frame = shows_first_frame(
                draw,
                *unit,
                (piece.flags & static_cast<uint16_t>(sim::model_runtime::PieceFlag::cached)) != 0,
                in.moving_pieces_once_built
            );
            if (lit)
                present::gpu_world::vertex_normals(object, prepared, points, normals);

            for (const auto& primitive : prepared.primitives) {
                if (static_cast<int32_t>(primitive.source_index) == object.selection_primitive)
                    continue;
                const auto& source = object.primitives.at(primitive.source_index);
                if (source.vertex_indices.size() < 3)
                    continue;
                const bool colored = (primitive.flags & model::primitive_colored) != 0;
                std::vector<int32_t> shade_rows(source.vertex_indices.size(), model::unlit_shade);
                if (lit)
                    for (std::size_t i = 0; i < source.vertex_indices.size(); ++i) {
                        const auto vertex = source.vertex_indices[i];
                        if (vertex < normals.size()) {
                            const auto& normal = normals[vertex];
                            shade_rows[i] = present::gpu_world::shade_row(
                                normal.x, normal.y, normal.z, in.light.data(), in.light_scale
                            );
                        }
                    }
                const bool bright = !colored && rows &&
                                    *std::max_element(shade_rows.begin(), shade_rows.end()) >= 16;
                const bool shaded_color = colored && rows && in.shade_table.size() >= 32 * 256;
                // A polygon of the image key at every corner is clear in a
                // unit's picture: a wall that hides the pieces sunk below the
                // ground, which the terrain hides here, so it draws nothing.
                // Drawn flat, it is a colour like any other.
                if (colored && image &&
                    std::all_of(shade_rows.begin(), shade_rows.end(), [&](int32_t row) {
                        return (shaded_color ? in.shade_table[row * 256 + primitive.color]
                                             : primitive.color) == model::image_key;
                    }))
                    continue;
                AtlasRect rect{};
                if (shaded_color) {
                    if (!white_material) {
                        const std::array<uint8_t, 1> pixel{0};
                        white_material =
                            material(pass, white_palette, 1, 1, pixel, {}, -1, Fog::seen);
                    }
                    rect = *white_material;
                } else if (colored) {
                    auto& cached = colors[primitive.color];
                    if (!cached) {
                        const std::array pixel{primitive.color};
                        cached = material(pass, *in.palette, 1, 1, pixel, {}, -1, Fog::seen);
                    }
                    rect = *cached;
                } else {
                    const auto* frame = model::primitive_texture(
                        primitive,
                        first_frame,
                        in.team_colors[unit->owner_index % in.team_colors.size()]
                    );
                    if (frame == nullptr || frame->data == nullptr ||
                        frame->encoding != OA_SPRITE_RAW || source.vertex_indices.size() != 4)
                        continue;
                    // primitive_texture already selects animation and team colour.
                    // Its frame pointer is stable only for this build's read-only pass.
                    auto& texture_cache = bright ? bright_textures : textures;
                    auto found = texture_cache.find(frame);
                    if (found == texture_cache.end())
                        found =
                            texture_cache
                                .emplace(
                                    frame,
                                    material(
                                        pass,
                                        bright ? bright_palette : *in.palette,
                                        frame->width,
                                        frame->height,
                                        {static_cast<const uint8_t*>(frame->data),
                                         static_cast<std::size_t>(frame->width) * frame->height},
                                        {},
                                        -1,
                                        Fog::seen
                                    )
                                )
                                .first;
                    rect = found->second;
                }
                constexpr std::array<float, 4> u{0, 1, 1, 0};
                constexpr std::array<float, 4> v{0, 0, 1, 1};
                std::vector<Vertex> corners;
                corners.reserve(source.vertex_indices.size());
                for (std::size_t index = 0; index < source.vertex_indices.size(); ++index) {
                    const auto& point = points.at(source.vertex_indices[index]);
                    corners.push_back(
                        {x + point.x / fixed_scale,
                         h + point.y / fixed_scale,
                         z - point.z / fixed_scale,
                         (rect.x + 0.5F + (colored ? 0 : u[index] * (rect.width - 1))) /
                             pass.scene.texture_width,
                         (rect.y + 0.5F + (colored ? 0 : v[index] * (rect.height - 1))) /
                             pass.scene.texture_height,
                         brightness}
                    );
                    auto& corner = corners.back();
                    if (shaded_color) {
                        const auto palette_index =
                            in.shade_table[shade_rows[index] * 256 + primitive.color];
                        corner.red = (*in.palette)[palette_index * palette_entry_bytes] / 255.0F;
                        corner.green =
                            (*in.palette)[palette_index * palette_entry_bytes + 1] / 255.0F;
                        corner.blue =
                            (*in.palette)[palette_index * palette_entry_bytes + 2] / 255.0F;
                    } else if (!colored && rows) {
                        corner.brightness *= std::clamp(
                            shade_rows[index] * 0.06875F * (bright ? 0.5F : 1.0F), 0.0F, 1.0F
                        );
                    }
                }
                const float remaining = std::clamp(unit->build_remaining, 0.0F, 1.0F);
                const float detail = 1 - remaining;
                if (reflects && !back_facing(corners, reflected_view)) {
                    for (std::size_t corner = 1; corner + 1 < corners.size() &&
                                                 reflected_faces.size() + 3 <= reflection_budget;
                         ++corner)
                        reflected_faces.insert(
                            reflected_faces.end(),
                            {corners[0], corners[corner], corners[corner + 1]}
                        );
                }
                if (remaining == 0 && back_facing(corners, in.view_direction))
                    continue;
                for (auto& vertex : corners) {
                    const bool construction = in.nanolathe_lighting && unit->build_remaining > 0;
                    float glow = construction
                                     ? 0.65F * remaining *
                                           renderer_effect_sources::construction_pulse(in.tick)
                                     : 0.0F;
                    for (const auto& light : nanolathe_lights) {
                        const float dx = vertex.x - light[0];
                        const float dy = vertex.height - light[1];
                        const float dz = vertex.z - light[2];
                        const float distance_squared = dx * dx + dy * dy + dz * dz;
                        // A light a radius or more away adds nothing.
                        if (distance_squared >= light_radius * light_radius)
                            continue;
                        const float distance = std::sqrt(distance_squared);
                        glow = std::max(glow, 0.5F * std::max(0.0F, 1 - distance / light_radius));
                    }
                    const float visible_glow =
                        glow * vertex.brightness * std::clamp(in.nanolathe_intensity, 0.0F, 2.0F);
                    vertex.emission_r = std::min(1.0F, visible_glow * 0.12F);
                    vertex.emission_g = std::min(1.0F, visible_glow);
                    vertex.emission_b = std::min(1.0F, visible_glow * 0.25F);
                    vertex.opacity = detail * detail * (3 - 2 * detail);
                    if (translucent)
                        vertex.opacity *= translucent_opacity;
                }
                if (detail > 0)
                    polygon(pass, corners);
                if (remaining > 0)
                    nanoframe(in, pass, corners, remaining, frame_material);
            }
        }
        ++list.drawn;
        if (translucent && list.vertices.size() > first_vertex) {
            // Held back, to be drawn over the opaque scene.
            const std::size_t held = list.translucent_vertices.size();
            const auto begin = list.vertices.begin() + static_cast<std::ptrdiff_t>(first_vertex);
            list.translucent_vertices.insert(
                list.translucent_vertices.end(), begin, list.vertices.end()
            );
            list.vertices.erase(begin, list.vertices.end());
            if (!draw.on_map && unit->id != 0)
                list.translucent_ranges.push_back(
                    {unit->id, held, list.translucent_vertices.size() - held}
                );
        } else if (!draw.on_map && unit->id != 0 && list.vertices.size() > first_vertex)
            list.unit_ranges.push_back(
                {unit->id, first_vertex, list.vertices.size() - first_vertex}
            );
        if (!reflected_faces.empty())
            naval_reflection(in, pass, reflected_faces, reflection_budget);
    }
}

/// Draws canonical 3DO shots and their child pieces at their presented simulation depth.
///
/// @param in the frame
/// @param[in,out] pass the projectiles pass
/// @param first the first shot listed, in WorldDrawList::projectiles
/// @param last the shot after the last
void projectiles(const Inputs& in, Pass& pass, std::size_t first, std::size_t last) {
    std::array<std::optional<AtlasRect>, 256> colors;
    std::unordered_map<const Sprite*, AtlasRect> textures;
    for (std::size_t shot_index = first; shot_index < last; ++shot_index) {
        begin_draw(pass, shot_index);
        const auto& shot = in.draws->projectiles[shot_index];
        const float x = in.camera_x + shot.position.x / fixed_scale;
        const float height = shot.position.y / fixed_scale;
        const float z = in.camera_y + shot.position.z / fixed_scale;
        if (fog_at(
                in.sight, static_cast<int32_t>(x), static_cast<int32_t>(z - height * height_lift)
            ) != Fog::seen)
            continue;
        const auto object = [&](const formats::objects3d::Object* source,
                                const model::PreparedObject* prepared,
                                sim::model_runtime::RotationWords rotation) {
            if (!source || !prepared)
                return;
            std::vector<formats::objects3d::FixedVector3> points;
            points.reserve(source->vertices.size());
            for (const auto& point : source->vertices)
                points.push_back(
                    sim::model_runtime::rotate_vector(
                        {std::bit_cast<int32_t>(0U - static_cast<uint32_t>(point.x)),
                         point.y,
                         std::bit_cast<int32_t>(0U - static_cast<uint32_t>(point.z))},
                        rotation
                    )
                );
            for (const auto& primitive : prepared->primitives) {
                if (primitive.source_index >= source->primitives.size() ||
                    static_cast<int32_t>(primitive.source_index) == source->selection_primitive)
                    continue;
                const auto& face = source->primitives[primitive.source_index];
                const bool colored = (primitive.flags & model::primitive_colored) != 0;
                if (face.vertex_indices.size() < 3 ||
                    (!colored && face.vertex_indices.size() != 4) ||
                    std::any_of(
                        face.vertex_indices.begin(),
                        face.vertex_indices.end(),
                        [&](uint16_t index) { return index >= points.size(); }
                    ))
                    continue;
                AtlasRect rect{};
                if (colored) {
                    auto& cached = colors[primitive.color];
                    if (!cached) {
                        const std::array pixel{primitive.color};
                        cached = material(pass, *in.palette, 1, 1, pixel, {}, -1, Fog::seen);
                    }
                    rect = *cached;
                } else {
                    const auto* frame = model::primitive_texture(primitive, false, 0);
                    if (!frame || !frame->data || frame->encoding != OA_SPRITE_RAW)
                        continue;
                    auto found = textures.find(frame);
                    if (found == textures.end())
                        found =
                            textures
                                .emplace(
                                    frame,
                                    material(
                                        pass,
                                        *in.palette,
                                        frame->width,
                                        frame->height,
                                        {static_cast<const uint8_t*>(frame->data),
                                         static_cast<std::size_t>(frame->width) * frame->height},
                                        {},
                                        -1,
                                        Fog::seen
                                    )
                                )
                                .first;
                    rect = found->second;
                }
                constexpr std::array<float, 4> u{0, 1, 1, 0}, v{0, 0, 1, 1};
                std::vector<Vertex> corners;
                corners.reserve(face.vertex_indices.size());
                for (std::size_t index = 0; index < face.vertex_indices.size(); ++index) {
                    const auto& point = points[face.vertex_indices[index]];
                    corners.push_back(
                        {x + point.x / fixed_scale,
                         height + point.y / fixed_scale,
                         z - point.z / fixed_scale,
                         (rect.x + 0.5F + (colored ? 0 : u[index] * (rect.width - 1))) /
                             pass.scene.texture_width,
                         (rect.y + 0.5F + (colored ? 0 : v[index] * (rect.height - 1))) /
                             pass.scene.texture_height,
                         brightness_at(in.sight, x, z - height * height_lift)}
                    );
                }
                // Small weapon faces are two-sided in the classic projectile pass.
                polygon(pass, corners);
            }
        };
        object(shot.object, shot.prepared, shot.rotation);
        object(shot.child, shot.child_prepared, shot.child_rotation);
    }
}

/// Draws the actual sight-gated particle stream at its interpolated world height.
///
/// @param in the frame
/// @param[in,out] pass the particles pass
/// @param first the first particle listed, in WorldDrawList::spatial_particles
/// @param last the particle after the last
void particles(const Inputs& in, Pass& pass, std::size_t first, std::size_t last) {
    auto& list = pass.list;
    constexpr std::array<std::array<float, 3>, 6> axes{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}
    };
    constexpr std::array<uint8_t, 24> faces{0, 2, 4, 4, 2, 1, 1, 2, 5, 5, 2, 0,
                                            0, 4, 3, 4, 1, 3, 1, 5, 3, 5, 0, 3};
    constexpr std::array<uint8_t, 1> pixel{0};
    std::unordered_map<uint32_t, AtlasRect> colors;
    const auto& projection = in.clip_projection;
    const bool screen_bounds = in.clip_width > 0 && in.clip_height > 0 &&
                               std::all_of(projection.begin(), projection.end(), [](float value) {
                                   return std::isfinite(value);
                               });
    for (std::size_t particle_index = first; particle_index < last; ++particle_index) {
        begin_draw(pass, particle_index);
        const auto& particle = in.draws->spatial_particles[particle_index];
        const float x = particle.position.x / fixed_scale;
        const float h = particle.position.y / fixed_scale;
        const float z = particle.position.z / fixed_scale;
        if (fog_at(in.sight, static_cast<int32_t>(x), static_cast<int32_t>(z - h * height_lift)) !=
            Fog::seen)
            continue;
        const float intensity = std::clamp(in.nanolathe_intensity, 0.0F, 2.0F);
        const bool glowing = particle.emissive && in.nanolathe_lighting && intensity > 0;
        const bool distant =
            std::isfinite(in.detail_scale) && in.detail_scale > 0 && in.detail_scale < 1;
        const float core_opacity =
            distant ? std::clamp((2 * in.detail_scale - 0.25F) / 0.75F, 0.0F, 1.0F) : 1;
        const float halo_opacity =
            distant ? std::clamp((6 * in.detail_scale - 0.5F) / 1.5F, 0.0F, 1.0F) : 1;
        if (core_opacity == 0 && (!glowing || halo_opacity == 0))
            continue;
        if (screen_bounds) {
            const float screen_x =
                projection[0] * x + projection[1] * h + projection[2] * z + projection[3];
            const float screen_y =
                projection[4] * x + projection[5] * h + projection[6] * z + projection[7];
            const float radius = glowing ? 3.0F : 1.0F;
            // The octahedron extrema are its six axis vertices; include the glow shell.
            const float reach_x =
                radius *
                std::max(
                    {std::abs(projection[0]), std::abs(projection[1]), std::abs(projection[2])}
                );
            const float reach_y =
                radius *
                std::max(
                    {std::abs(projection[4]), std::abs(projection[5]), std::abs(projection[6])}
                );
            if (std::isfinite(screen_x) && std::isfinite(screen_y) &&
                (screen_x + reach_x < 0 || screen_y + reach_y < 0 ||
                 screen_x - reach_x > in.clip_width || screen_y - reach_y > in.clip_height))
                continue;
        }
        const uint32_t color = (uint32_t{particle.color[0]} << 16) |
                               (uint32_t{particle.color[1]} << 8) | particle.color[2];
        auto found = colors.find(color);
        if (found == colors.end()) {
            auto palette = *in.palette;
            std::copy(particle.color.begin(), particle.color.end(), palette.begin());
            found = colors.emplace(color, material(pass, palette, 1, 1, pixel, {}, -1, Fog::seen))
                        .first;
        }
        const auto rect = found->second;
        const float u = (rect.x + 0.5F) / pass.scene.texture_width;
        const float v = (rect.y + 0.5F) / pass.scene.texture_height;
        const auto shell = [&](float radius, float opacity) {
            std::array<Vertex, faces.size()> triangles;
            for (std::size_t index = 0; index < faces.size(); ++index) {
                const auto& axis = axes[faces[index]];
                triangles[index] = {
                    x + axis[0] * radius,
                    h + axis[1] * radius,
                    z + axis[2] * radius,
                    u,
                    v,
                    glowing ? std::max(0.0F, 1 - intensity) : 1.0F,
                    opacity,
                    glowing ? std::min(1.0F, particle.color[0] / 255.0F * intensity) : 0.0F,
                    glowing ? std::min(1.0F, particle.color[1] / 255.0F * intensity) : 0.0F,
                    glowing ? std::min(1.0F, particle.color[2] / 255.0F * intensity) : 0.0F
                };
            }
            auto& listed = listing(pass, triangles.size());
            listed.insert(listed.end(), triangles.begin(), triangles.end());
        };
        if (core_opacity > 0)
            shell(1.0F, core_opacity);
        if (glowing && halo_opacity > 0)
            shell(3.0F, 0.16F * halo_opacity * intensity);
        if (particle.emissive)
            ++list.nano_particles;
        ++list.drawn;
    }
}

/// The frame's ground triangles listed by place along two axes of the
/// ground plane, so that a query of a point or of an area tests only the
/// triangles that may reach it rather than every triangle of the ground.
/// Each triangle is listed once, in the square bucket holding its least
/// place along both axes, and a query looks as many buckets before its area
/// as a listed triangle reaches past its own. The few triangles that reach
/// further are tested by every query instead, so that one stray triangle
/// cannot widen them all. Listed once foundations has cut the ground, it
/// stays true while the frame adds its other geometry after the ground.
class GroundIndex {
  public:

    /// Two axes of the ground plane: a world point x, z lies at
    /// axes[0] * x + axes[1] * z along the first and axes[2] * x + axes[3] * z
    /// along the second.
    using Axes = std::array<double, 4>;

    /// Lists the ground's triangles by their bounds.
    ///
    /// @param axes the axes the bounds lie along
    /// @param[in,out] memory the index's memory, holding each triangle's
    ///        bounds along the axes in drawing order
    /// @param reach the greatest distance of a ground vertex from the world's
    ///        origin along x or z, in map pixels
    GroundIndex(const Axes& axes, GroundIndexMemory& memory, double reach)
        : axes_(axes), memory_(&memory), reach_(reach) {
        const auto& bounds = memory.bounds;
        for (const auto& bound : bounds) {
            low_[0] = std::min(low_[0], double{bound[0]});
            high_[0] = std::max(high_[0], double{bound[1]});
            low_[1] = std::min(low_[1], double{bound[2]});
            high_[1] = std::max(high_[1], double{bound[3]});
        }
        auto& oversized = memory.oversized;
        oversized.clear();
        if (bounds.empty())
            return;
        const auto count = [&](std::size_t axis) {
            return std::floor((high_[axis] - low_[axis]) / edge_) + 1;
        };
        while (count(0) * count(1) > static_cast<double>(maximum_buckets))
            edge_ *= 2;
        inverse_edge_ = 1 / edge_;
        columns_ = static_cast<std::size_t>(count(0));
        rows_ = static_cast<std::size_t>(count(1));
        auto& listed = memory.buckets;
        auto& starts = memory.starts;
        auto& triangles = memory.triangles;
        listed.resize(bounds.size());
        // The triangles by the most buckets they reach past their own along
        // either axis; the last count holds those reaching further still.
        std::array<std::size_t, listed_reach_limit + 2> reaches{};
        const auto further = [](uint16_t last, uint16_t first) {
            return static_cast<uint8_t>(std::min(last - first, 255));
        };
        for (std::size_t triangle = 0; triangle < bounds.size(); ++triangle) {
            const auto& bound = bounds[triangle];
            const uint16_t column = bucket(bound[0], 0, columns_);
            const uint16_t row = bucket(bound[2], 1, rows_);
            const auto& entry = listed[triangle] = {
                static_cast<uint16_t>(row * columns_ + column),
                further(bucket(bound[1], 0, columns_), column),
                further(bucket(bound[3], 1, rows_), row)
            };
            ++reaches[std::min<std::size_t>(
                std::max(entry.columns, entry.rows), listed_reach_limit + 1
            )];
        }
        // The least reach a listed triangle may have that leaves few enough
        // for every query to test.
        std::size_t reach_limit{};
        std::size_t reaching_further = bounds.size() - reaches[0];
        while (reach_limit < listed_reach_limit && reaching_further > oversized_limit)
            reaching_further -= reaches[++reach_limit];
        starts.assign(columns_ * rows_ + 1, 0);
        for (std::size_t triangle = 0; triangle < bounds.size(); ++triangle) {
            const auto& entry = listed[triangle];
            if (std::max(entry.columns, entry.rows) > reach_limit) {
                oversized.push_back(static_cast<uint32_t>(triangle));
                continue;
            }
            listed_reach_[0] = std::max<std::size_t>(listed_reach_[0], entry.columns);
            listed_reach_[1] = std::max<std::size_t>(listed_reach_[1], entry.rows);
            ++starts[entry.bucket + 1];
        }
        for (std::size_t index = 1; index < starts.size(); ++index)
            starts[index] += starts[index - 1];
        triangles.resize(starts.back());
        // Each bucket's start moves on as it is filled, to the next one's.
        for (std::size_t triangle = 0; triangle < bounds.size(); ++triangle) {
            const auto& entry = listed[triangle];
            if (std::max(entry.columns, entry.rows) <= reach_limit)
                triangles[starts[entry.bucket]++] = static_cast<uint32_t>(triangle);
        }
        for (std::size_t index = starts.size() - 1; index > 0; --index)
            starts[index] = starts[index - 1];
        starts[0] = 0;
    }

    /// @return the axes the triangles are listed along
    [[nodiscard]] const Axes& axes() const { return axes_; }

    /// @return the greatest distance of a ground vertex from the world's
    ///         origin along x or z, in map pixels
    [[nodiscard]] double reach() const { return reach_; }

    /// Places a world point on the axes.
    ///
    /// @param x the point's world x, in map pixels
    /// @param z its world z, in map pixels
    /// @return its place along the first axis and along the second
    [[nodiscard]] std::array<double, 2> along(double x, double z) const {
        return {axes_[0] * x + axes_[1] * z, axes_[2] * x + axes_[3] * z};
    }

    /// Calls visit with the first vertex of each triangle whose bounds along
    /// the axes overlap an area, once each and in no set order.
    ///
    /// @param first_low the area's least place along the first axis
    /// @param first_high its greatest place along the first axis
    /// @param second_low its least place along the second axis
    /// @param second_high its greatest place along the second axis
    /// @param visit called with the index of a triangle's first vertex
    template <typename Visit>
    void visit(
        double first_low, double first_high, double second_low, double second_high, Visit&& visit
    ) const {
        if (columns_ == 0 || !(first_high >= low_[0] && first_low <= high_[0] &&
                               second_high >= low_[1] && second_low <= high_[1]))
            return;
        // A triangle whose bounds overlap the area ends in a bucket at or
        // after the area's first, as a larger place is never in an earlier
        // bucket, and is listed at most its reach before that, at or before
        // the area's last.
        const std::size_t low_column = bucket(first_low, 0, columns_);
        const std::size_t low_row = bucket(second_low, 1, rows_);
        const std::size_t first_column = low_column - std::min(low_column, listed_reach_[0]);
        const std::size_t first_row = low_row - std::min(low_row, listed_reach_[1]);
        const std::size_t last_column = bucket(first_high, 0, columns_);
        const std::size_t last_row = bucket(second_high, 1, rows_);
        const auto& bounds = memory_->bounds;
        const auto overlapping = [&](uint32_t triangle) {
            const auto& bound = bounds[triangle];
            if (bound[1] >= first_low && bound[0] <= first_high && bound[3] >= second_low &&
                bound[2] <= second_high)
                visit(std::size_t{triangle} * 3);
        };
        const auto& starts = memory_->starts;
        const auto& triangles = memory_->triangles;
        for (std::size_t row = first_row; row <= last_row; ++row) {
            const std::size_t row_start = row * columns_;
            for (std::size_t entry = starts[row_start + first_column];
                 entry < starts[row_start + last_column + 1];
                 ++entry)
                overlapping(triangles[entry]);
        }
        for (const uint32_t triangle : memory_->oversized)
            overlapping(triangle);
    }

  private:

    /// The buckets' edge to begin with, along either axis.
    static constexpr double first_edge = 32;
    /// The most buckets an index holds; a wider ground takes larger ones.
    static constexpr std::size_t maximum_buckets = std::size_t{1} << 16;
    /// The most buckets past its own a listed triangle may reach.
    static constexpr std::size_t listed_reach_limit = 15;
    /// The most triangles every query tests rather than listing them, unless
    /// more reach past listed_reach_limit.
    static constexpr std::size_t oversized_limit = 32;

    /// Finds the bucket that holds a place, never one before a smaller place's.
    ///
    /// @param place a place along an axis
    /// @param axis 0 for the first axis, 1 for the second
    /// @param count the buckets along that axis
    /// @return the bucket holding the place, or the nearest end one
    [[nodiscard]] uint16_t bucket(double place, std::size_t axis, std::size_t count) const {
        const double offset = (place - low_[axis]) * inverse_edge_;
        if (!(offset > 0))
            return 0;
        const double last = static_cast<double>(count - 1);
        return static_cast<uint16_t>(offset < last ? offset : last);
    }

    Axes axes_{};
    GroundIndexMemory* memory_{};
    double reach_{};
    std::array<double, 2> low_{
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()
    };
    std::array<double, 2> high_{
        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()
    };
    double edge_{first_edge};
    double inverse_edge_{1 / first_edge};
    std::size_t columns_{};
    std::size_t rows_{};
    /// The most buckets past its own a listed triangle reaches along the
    /// first axis and along the second. Buckets are counted row by row, and
    /// each bucket's triangles are listed in drawing order, a triangle
    /// numbering its first vertex divided by three.
    std::array<std::size_t, 2> listed_reach_{};
};

/// The ground indices of a sprite pass, listed together when first needed.
struct GroundIndices {
    bool listed{}; ///< list_ground has run for the pass
    /// The axes card lists along: the camera-facing cards' across and above.
    GroundIndex::Axes card_axes{};
    /// The triangles along world x and z; none when a ground vertex lies
    /// nowhere finite, and a query then tests every triangle itself.
    std::optional<GroundIndex> world{};
    /// The triangles along the cards' axes; none as for world, or when the
    /// cards have no axes.
    std::optional<GroundIndex> card{};
};

/// Lists the frame's ground triangles along world x and z and along the
/// cards' axes, reading each ground vertex once.
///
/// @param[in,out] scene the frame; the lists are kept in its
///        ground_index_memory
/// @param ground the frame's ground triangles, cut by foundations
/// @param card_axes the cards' across and above, or none when they have none
/// @param[out] indices the pass's ground indices, listed afresh
void list_ground(
    Scene& scene,
    std::span<const Vertex> ground,
    const GroundIndex::Axes* card_axes,
    GroundIndices& indices
) {
    indices.listed = true;
    indices.world.reset();
    indices.card.reset();
    if (card_axes != nullptr)
        indices.card_axes = *card_axes;
    const std::size_t triangles = ground.size() / 3;
    auto& world_bounds = scene.ground_index_memory[0].bounds;
    auto& card_bounds = scene.ground_index_memory[1].bounds;
    world_bounds.resize(triangles);
    card_bounds.resize(card_axes != nullptr ? triangles : 0);
    constexpr float none = std::numeric_limits<float>::infinity();
    double reach{};
    for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
        std::array<float, 4> world{none, -none, none, -none};
        std::array<double, 4> card{none, -none, none, -none};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& vertex = ground[triangle * 3 + corner];
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.z) ||
                !std::isfinite(vertex.height))
                return;
            world = {
                std::min(world[0], vertex.x),
                std::max(world[1], vertex.x),
                std::min(world[2], vertex.z),
                std::max(world[3], vertex.z)
            };
            if (card_axes != nullptr) {
                const auto& axes = *card_axes;
                const double across = axes[0] * vertex.x + axes[1] * vertex.z;
                const double above = axes[2] * vertex.x + axes[3] * vertex.z;
                card = {
                    std::min(card[0], across),
                    std::max(card[1], across),
                    std::min(card[2], above),
                    std::max(card[3], above)
                };
            }
        }
        reach = std::max(
            {reach,
             std::abs(double{world[0]}),
             std::abs(double{world[1]}),
             std::abs(double{world[2]}),
             std::abs(double{world[3]})}
        );
        world_bounds[triangle] = world;
        if (card_axes != nullptr)
            card_bounds[triangle] = {
                static_cast<float>(card[0]),
                static_cast<float>(card[1]),
                static_cast<float>(card[2]),
                static_cast<float>(card[3])
            };
    }
    indices.world.emplace(GroundIndex::Axes{1, 0, 0, 1}, scene.ground_index_memory[0], reach);
    if (card_axes != nullptr)
        indices.card.emplace(*card_axes, scene.ground_index_memory[1], reach);
}

/// Finds the rendered ground's height on one triangle below a world point.
///
/// @param ground the frame's ground triangles
/// @param first the index of the triangle's first vertex
/// @param x the point's world x, in map pixels
/// @param z its world z, in map pixels
/// @return the height there, or nothing when the triangle does not hold the point
std::optional<float>
ground_on_triangle(std::span<const Vertex> ground, std::size_t first, float x, float z) {
    const auto& a = ground[first];
    const auto& b = ground[first + 1];
    const auto& c = ground[first + 2];
    if (x < std::min({a.x, b.x, c.x}) || x > std::max({a.x, b.x, c.x}) ||
        z < std::min({a.z, b.z, c.z}) || z > std::max({a.z, b.z, c.z}))
        return std::nullopt;
    const float determinant = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::abs(determinant) < 0.000001F)
        return std::nullopt;
    const float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / determinant;
    const float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / determinant;
    constexpr float edge_tolerance = 0.00001F;
    if (u < -edge_tolerance || v < -edge_tolerance || u + v > 1 + edge_tolerance)
        return std::nullopt;
    return u * a.height + v * b.height + (1 - u - v) * c.height;
}

/// Finds the rendered ground, including leveled foundations, below a world point.
///
/// The highest triangle holding the point gives it, whatever order they are
/// tested in.
///
/// @param ground the frame's ground triangles
/// @param indices the pass's ground indices, listed
/// @param x the point's world x, in map pixels
/// @param z its world z, in map pixels
/// @return the ground's height there, or nothing off the ground
std::optional<float>
rendered_ground(std::span<const Vertex> ground, GroundIndices& indices, float x, float z) {
    std::optional<float> result;
    const auto test = [&](std::size_t first) {
        const auto height = ground_on_triangle(ground, first, x, z);
        if (height && (!result || *height > *result))
            result = height;
    };
    if (indices.world && std::isfinite(x) && std::isfinite(z)) {
        // A triangle holds the point only when its bounds, its corners'
        // least and greatest x and z, hold it.
        const auto place = indices.world->along(x, z);
        indices.world->visit(place[0], place[0], place[1], place[1], test);
    } else {
        for (std::size_t first = 0; first + 2 < ground.size(); first += 3)
            test(first);
    }
    return result;
}

/// Keeps a near-ground explosion's covered card above every intersected terrain plane.
/// The translation leaves its UVs and shape intact; airborne effects retain their altitude.
float surface_effect_lift(
    Scene& scene,
    std::span<const Vertex> ground_triangles,
    GroundIndices& indices,
    const formats::gaf::RenderedFrame& frame,
    float x,
    float height,
    float z,
    const std::array<float, 3>& right,
    const std::array<float, 3>& up
) {
    constexpr float ground_near_distance = 4;
    constexpr float clearance = 0.1F;
    const float determinant = right[0] * up[2] - right[2] * up[0];
    const bool facing = std::abs(determinant) >= 0.000001F;
    // The card's own axes: a ground vertex lies at its place along them less
    // the card origin's, as the test below works it out.
    const double inverse = 1.0 / static_cast<double>(determinant);
    const GroundIndex::Axes axes{
        up[2] * inverse, -up[0] * inverse, -right[2] * inverse, right[0] * inverse
    };
    if (!indices.listed || (facing && indices.card_axes != axes))
        list_ground(scene, ground_triangles, facing ? &axes : nullptr, indices);
    const auto ground = rendered_ground(ground_triangles, indices, x, z);
    if (!ground || height > *ground + ground_near_distance)
        return 0;
    int32_t left = frame.width, top = frame.height, right_pixel{}, bottom{};
    for (int32_t row = 0; row < frame.height; ++row)
        for (int32_t column = 0; column < frame.width; ++column) {
            const auto pixel = static_cast<std::size_t>(row) * frame.width + column;
            const bool covered =
                frame.coverage.empty()
                    ? pixel < frame.pixels.size() && frame.pixels[pixel] != frame.transparency_index
                    : pixel < frame.coverage.size() && frame.coverage[pixel] != 0;
            if (covered) {
                left = std::min(left, column);
                top = std::min(top, row);
                right_pixel = std::max(right_pixel, column + 1);
                bottom = std::max(bottom, row + 1);
            }
        }
    if (left >= right_pixel || top >= bottom)
        return 0;
    const float min_across = static_cast<float>(left - frame.origin_x);
    const float max_across = static_cast<float>(right_pixel - frame.origin_x);
    const float min_above = static_cast<float>(frame.origin_y - bottom);
    const float max_above = static_cast<float>(frame.origin_y - top);
    if (!facing)
        return 0;
    float lift{};
    const auto test = [&](std::size_t first) {
        GroundPolygon local{};
        local.count = 3;
        for (std::size_t corner = 0; corner < local.count; ++corner) {
            const auto& vertex = ground_triangles[first + corner];
            const float dx = vertex.x - x;
            const float dz = vertex.z - z;
            local.points[corner] = {
                (dx * up[2] - dz * up[0]) / determinant,
                (right[0] * dz - right[2] * dx) / determinant,
                vertex.height
            };
        }
        const auto& points = local.points;
        if (std::max({points[0].x, points[1].x, points[2].x}) < min_across ||
            std::min({points[0].x, points[1].x, points[2].x}) > max_across ||
            std::max({points[0].y, points[1].y, points[2].y}) < min_above ||
            std::min({points[0].y, points[1].y, points[2].y}) > max_above)
            return;
        auto clipped = clip_ground_fixed(local, true, min_across, true);
        clipped = clip_ground_fixed(clipped, true, max_across, false);
        clipped = clip_ground_fixed(clipped, false, min_above, true);
        clipped = clip_ground_fixed(clipped, false, max_above, false);
        // Both surfaces are planar over each clipped polygon, so its corners
        // include the greatest clearance needed, even for an interior crest.
        for (std::size_t corner = 0; corner < clipped.count; ++corner) {
            const auto& point = clipped.points[corner];
            lift = std::max(lift, point.height + clearance - (height + up[1] * point.y));
        }
    };
    if (!indices.card) {
        for (std::size_t first = 0; first + 2 < ground_triangles.size(); first += 3)
            test(first);
        return lift;
    }
    // The test above rounds each place by less than a 2^-21 part of the
    // reach times the axis's scale, and the index's bounds by less than a
    // 2^-23 part; the area is widened by a 2^-16 part, so that every
    // triangle the test would keep is visited.
    const double reach =
        std::max({indices.card->reach(), std::abs(double{x}), std::abs(double{z})});
    constexpr double rounding_share = 0x1p-16;
    constexpr double least_margin = 0x1p-8;
    const double across_margin =
        (std::abs(axes[0]) + std::abs(axes[1])) * reach * rounding_share + least_margin;
    const double above_margin =
        (std::abs(axes[2]) + std::abs(axes[3])) * reach * rounding_share + least_margin;
    const auto origin = indices.card->along(x, z);
    indices.card->visit(
        origin[0] + min_across - across_margin,
        origin[0] + max_across + across_margin,
        origin[1] + min_above - above_margin,
        origin[1] + max_above + above_margin,
        test
    );
    return lift;
}

/// Lifts sprites into upright cutouts preserving their classic pixel placement.
///
/// @param in the frame
/// @param[in,out] pass the pass of the cards laid on the ground, or of
///     those standing up
/// @param ground_pass the cards laid on the ground, rather than those
///     standing up
/// @param first the first card listed, in WorldDrawList::sprites
/// @param last the card after the last
void sprites(const Inputs& in, Pass& pass, bool ground_pass, std::size_t first, std::size_t last) {
    auto& scene = pass.scene;
    // Frame pixels and palette are immutable during this pass; the cache never
    // survives a build, so animated or recoloured frames are hashed again.
    std::unordered_map<const formats::gaf::RenderedFrame*, std::array<std::optional<AtlasRect>, 4>>
        prepared;
    std::unordered_map<const formats::gaf::RenderedFrame*, std::optional<AtlasRect>> grey_frames;
    // The ground is cut and stays as it is for the whole pass.
    GroundIndices ground_indices;
    std::vector<bool> light_masks(in.draws->sprites.size());
    std::vector<bool> blended_sprites(in.draws->sprites.size());
    for (const auto& draw : in.draws->draws) {
        if (draw.index >= light_masks.size())
            continue;
        if (draw.kind == WorldDrawKind::lit_sprite)
            light_masks[draw.index] = true;
        if (draw.kind == WorldDrawKind::blended_sprite)
            blended_sprites[draw.index] = true;
    }
    for (std::size_t index = first; index < last; ++index) {
        begin_draw(pass, index);
        const auto& sprite = in.draws->sprites[index];
        const bool light_mask = light_masks[index];
        const bool submerged_effect = sprite.world_position && !sprite.on_map &&
                                      sprite.world_position->y / fixed_scale < in.sea_level;
        if (sprite.frame == nullptr || (sprite.terrain_decal || sprite.shadow ||
                                        (light_mask && !submerged_effect)) != ground_pass)
            continue;
        const auto& frame = *sprite.frame;
        const int32_t x = in.camera_x + sprite.place.x;
        const int32_t y = in.camera_y + sprite.place.y;
        const auto fog = fog_at(in.sight, x, y);
        if ((fog == Fog::black || (light_mask && fog != Fog::seen)) && !ground_pass)
            continue;
        if (ground_pass) {
            if (sprite.shadow && in.draws->shadow_level == 0)
                continue;
            const float left = float(x) - frame.origin_x;
            const float top = float(y) - frame.origin_y;
            const float right = left + frame.width;
            const float bottom = top + frame.height;
            const float crop_left = std::floor(float(in.left) / cell_edge) * cell_edge;
            const float crop_top = std::floor(float(in.top) / cell_edge) * cell_edge;
            const float crop_right = std::ceil(float(in.right) / cell_edge) * cell_edge;
            const float crop_bottom = std::ceil(float(in.bottom) / cell_edge) * cell_edge;
            if (right <= crop_left || bottom <= crop_top || left >= crop_right ||
                top >= crop_bottom)
                continue;
            // A black origin can overlap mapped ground. Match decal's cell fog
            // tests rather than discarding its still-visible edge.
            bool mapped = false;
            const int32_t first_x = static_cast<int32_t>(
                std::max({0.0F, crop_left, std::floor(left / cell_edge) * cell_edge})
            );
            const int32_t first_y = static_cast<int32_t>(
                std::max({0.0F, crop_top, std::floor(top / cell_edge) * cell_edge})
            );
            const float last_x =
                std::min({right, crop_right, float(in.map->attribute_width * cell_edge)});
            const float last_y =
                std::min({bottom, crop_bottom, float(in.map->attribute_height * cell_edge)});
            for (int32_t row = first_y; row < last_y && !mapped; row += cell_edge)
                for (int32_t column = first_x; column < last_x; column += cell_edge)
                    if (fog_at(in.sight, column, row) != Fog::black) {
                        mapped = true;
                        break;
                    }
            if (!mapped)
                continue;
        }
        constexpr unsigned shadow_material = 1;
        constexpr unsigned light_material = 2;
        const unsigned variant =
            (sprite.shadow ? shadow_material : 0) | (light_mask ? light_material : 0);
        auto& cached = prepared[sprite.frame][variant];
        if (!cached)
            cached = material(
                pass,
                *in.palette,
                frame.width,
                frame.height,
                frame.pixels,
                frame.coverage,
                frame.transparency_index,
                sprite.shadow ? Fog::black : Fog::seen,
                light_mask,
                in.draws->flash_strength
            );
        const auto rect = *cached;
        // Scenery keeps its colours under the water. An opaque frame also
        // greys with the ground; a blended frame does not, as the classic
        // view leaves a blended feature's fog to the cell it is drawn in.
        const bool scenery = sprite.on_map && !sprite.shadow && !light_mask;
        const bool ground_fog = scenery && !blended_sprites[index];
        std::optional<AtlasRect> grey_rect;
        if (ground_fog) {
            auto& cached_grey = grey_frames[sprite.frame];
            if (!cached_grey)
                cached_grey = material(
                    pass,
                    scene.terrain_grey_palette,
                    frame.width,
                    frame.height,
                    frame.pixels,
                    frame.coverage,
                    frame.transparency_index,
                    Fog::seen
                );
            grey_rect = cached_grey;
        }
        if (ground_pass) {
            if (sprite.shadow && in.draws->shadow_level == 0)
                continue;
            const float opacity =
                sprite.shadow ? 0.5F * std::min(in.draws->shadow_level, model::shadow_full_level) /
                                    model::shadow_full_level
                              : 1.0F;
            decal(
                in,
                pass,
                rect,
                (sprite.world_position ? sprite.world_position->x / fixed_scale : x) -
                    frame.origin_x,
                (sprite.world_position
                     ? (sprite.world_position->z - sprite.world_position->y * height_lift) /
                           fixed_scale
                     : y) -
                    frame.origin_y,
                opacity,
                sprite.shadow,
                sprite.world_position ? &*sprite.world_position : nullptr,
                1,
                false,
                false,
                scenery,
                grey_rect ? &*grey_rect : nullptr
            );
            ++pass.list.drawn;
            continue;
        }
        const float ground = height_at(*in.map, x, y);
        const float origin_x = sprite.world_position ? sprite.world_position->x / fixed_scale : x;
        const float origin_h =
            sprite.world_position ? sprite.world_position->y / fixed_scale : ground;
        const float origin_z = sprite.world_position ? sprite.world_position->z / fixed_scale
                                                     : y + ground * height_lift;
        // Effects face the camera at their actual altitude. Scenery keeps its upright card.
        auto direction = in.view_direction;
        float horizontal = std::hypot(direction[0], direction[2]);
        if (horizontal < 0.0001F) {
            direction = {0, 1, height_lift};
            horizontal = height_lift;
        }
        const float length = std::hypot(horizontal, direction[1]);
        const std::array right{direction[2] / horizontal, 0.0F, -direction[0] / horizontal};
        const float vertical_scale = std::sqrt(1 + height_lift * height_lift);
        const std::array up{
            direction[1] * right[2] / length / vertical_scale,
            horizontal / length / vertical_scale,
            -direction[1] * right[0] / length / vertical_scale
        };
        const float surface_lift =
            sprite.surface_effect && sprite.world_position && !submerged_effect
                ? surface_effect_lift(
                      scene,
                      pass.ground,
                      ground_indices,
                      frame,
                      origin_x,
                      origin_h,
                      origin_z,
                      right,
                      up
                  )
                : 0;
        const auto vertex = [&](float dx, float dy) {
            const float across = (dx - frame.origin_x) * sprite.effect_scale;
            const float above = (frame.origin_y - dy) * sprite.effect_scale;
            const bool facing = sprite.world_position.has_value() && !sprite.on_map;
            const float sample_x =
                sprite.soft_effect ? std::clamp(dx, 0.5F, float(rect.width) - 0.5F) : dx;
            const float sample_y =
                sprite.soft_effect ? std::clamp(dy, 0.5F, float(rect.height) - 0.5F) : dy;
            Vertex result{
                origin_x + (facing ? right[0] * across + up[0] * above : across),
                origin_h + surface_lift + (facing ? up[1] * above : above / height_lift),
                origin_z + (facing ? right[2] * across + up[2] * above : 0),
                (rect.x + (sprite.soft_effect || submerged_effect ? sample_x : dx)) /
                    scene.texture_width,
                (rect.y + (sprite.soft_effect || submerged_effect ? sample_y : dy)) /
                    scene.texture_height,
                brightness_at(in.sight, static_cast<float>(x), static_cast<float>(y)),
                sprite.effect_opacity
            };
            if ((sprite.soft_effect || submerged_effect) && !scenery)
                result.terrain_size = -1;
            if (submerged_effect) {
                result.terrain_size = -5;
                result.terrain_x = in.sea_level - origin_h;
            } else if (scenery) {
                result.terrain_size = sprite.soft_effect ? -7.0F : -6.0F;
                if (ground_fog && grey_rect) {
                    result.terrain_x = (grey_rect->x + sample_x) / scene.texture_width;
                    result.terrain_y = (grey_rect->y + sample_y) / scene.texture_height;
                    const float painted_x = result.x;
                    const float painted_y = result.z - result.height * height_lift;
                    result.brightness = brightness_at(in.sight, painted_x, painted_y);
                    const auto ground = fog_of_ground(in, painted_x, painted_y, result.brightness);
                    result.terrain_known = ground.known;
                    result.terrain_grey = ground.grey;
                }
                if (!facing)
                    move_with_swells(in, result);
            } else if (!facing) {
                move_with_swells(in, result);
            }
            return result;
        };
        const std::array corners{
            vertex(0, 0),
            vertex(static_cast<float>(frame.width), 0),
            vertex(static_cast<float>(frame.width), static_cast<float>(frame.height)),
            vertex(0, static_cast<float>(frame.height))
        };
        float low_brightness = corners[0].brightness;
        float high_brightness = low_brightness;
        for (const auto& point : corners) {
            low_brightness = std::min(low_brightness, point.brightness);
            high_brightness = std::max(high_brightness, point.brightness);
        }
        // A frame standing in even fog stays one card. A fog boundary splits
        // it on the map, as the ground under it is split.
        if (!ground_fog || high_brightness - low_brightness < 0.001F) {
            polygon(pass, corners);
        } else {
            const int step = std::max(1, cell_edge / fog_divisions(in));
            for (int top_row = 0; top_row < frame.height; top_row += step) {
                const int bottom_row = std::min(static_cast<int>(frame.height), top_row + step);
                for (int left_column = 0; left_column < frame.width; left_column += step) {
                    const int right_column =
                        std::min(static_cast<int>(frame.width), left_column + step);
                    polygon(
                        pass,
                        std::array{
                            vertex(static_cast<float>(left_column), static_cast<float>(top_row)),
                            vertex(static_cast<float>(right_column), static_cast<float>(top_row)),
                            vertex(
                                static_cast<float>(right_column), static_cast<float>(bottom_row)
                            ),
                            vertex(static_cast<float>(left_column), static_cast<float>(bottom_row))
                        }
                    );
                }
            }
        }
        ++pass.list.drawn;
    }
}

/// Draws expanding rings and brief droplets from already visibility-gated weapon events.
void water_disturbances(const Inputs& in, Scene& scene) {
    if (in.water.detail <= 0 || in.water.ripples <= 0)
        return;
    constexpr uint32_t lifetime = renderer_water::lifetime_ticks;
    constexpr uint32_t segments = 24;
    constexpr float circle = 6.283185307F;
    std::size_t emitted{};
    const float intensity = std::clamp(in.water.ripples, 0.0F, 2.0F) * in.water.detail;
    for (auto event = in.water_disturbances.rbegin();
         event != in.water_disturbances.rend() && emitted < 32;
         ++event) {
        const auto age = in.tick - event->birth;
        if (age >= lifetime || water_depth(in, event->x, event->z) <= 0 ||
            fog_at(
                in.sight,
                static_cast<int32_t>(event->x),
                static_cast<int32_t>(event->z - in.sea_level * height_lift)
            ) != Fog::seen)
            continue;
        ++emitted;
        const float strength = std::clamp(event->strength, 0.2F, 2.0F);
        const float remaining = 1 - static_cast<float>(age) / lifetime;
        const float opacity = std::min(0.7F, 0.32F * strength * intensity * remaining * remaining);
        const float radius = 2 + age * (0.45F + strength * 0.15F);
        const float width = 1.1F + strength * 0.55F;
        const auto point = [&](float x, float z, float height, float alpha) {
            Vertex vertex{};
            vertex.x = x;
            vertex.z = z;
            vertex.height = height;
            vertex.opacity = alpha;
            vertex.brightness = brightness_at(in.sight, x, z - in.sea_level * height_lift);
            vertex.terrain_size = -4;
            vertex.terrain_x = water_depth(in, x, z);
            return vertex;
        };
        const auto ring_point = [&](float angle, float distance, float alpha) {
            return point(
                event->x + std::cos(angle) * distance,
                event->z + std::sin(angle) * distance,
                in.sea_level + 0.2F,
                alpha
            );
        };
        for (uint32_t segment = 0; segment < segments; ++segment) {
            const float a = circle * segment / segments;
            const float b = circle * (segment + 1) / segments;
            for (const float side : {-1.0F, 1.0F}) {
                const auto inner_a = ring_point(a, radius, opacity);
                const auto inner_b = ring_point(b, radius, opacity);
                const auto outer_a = ring_point(a, std::max(0.0F, radius + side * width), 0);
                const auto outer_b = ring_point(b, std::max(0.0F, radius + side * width), 0);
                wet_polygon(scene, {inner_a, inner_b, outer_a});
                wet_polygon(scene, {outer_a, inner_b, outer_b});
            }
        }
        if (age >= 16 || event->kind == renderer_water::Kind::firing)
            continue;
        const float time = age / 16.0F;
        const float right_length = std::hypot(in.clip_projection[0], in.clip_projection[2]);
        const float right_x = right_length > 0.001F ? in.clip_projection[0] / right_length : 1;
        const float right_z = right_length > 0.001F ? in.clip_projection[2] / right_length : 0;
        const float height = std::sin(time * 3.141592654F) * (8 + strength * 8);
        for (uint32_t drop = 0; drop < 6; ++drop) {
            const float angle = circle * drop / 6;
            const float x = event->x + std::cos(angle) * (1 + time * 7 * strength);
            const float z = event->z + std::sin(angle) * (1 + time * 7 * strength);
            const float half_width = 0.45F + strength * 0.2F;
            const float bottom = in.sea_level + std::max(0.3F, height - 3);
            const float top = in.sea_level + height + 0.5F;
            const float alpha = opacity * (1 - time);
            const auto a =
                point(x - half_width * right_x, z - half_width * right_z, bottom, alpha * 0.5F);
            const auto b =
                point(x + half_width * right_x, z + half_width * right_z, bottom, alpha * 0.5F);
            const auto c = point(x, z, top, alpha);
            wet_polygon(scene, {a, b, c});
        }
    }
}

/// Adds a transparent water sheet for the coastal waves only over the
/// already visible wet ground triangles of the shallows, where the
/// fragment stage's foam lies: none over deep water, so that however far
/// the view zooms out the budget holds the whole shore. Water ripples need
/// none: they move the ground's own art (shaders/live_scene_vertex.glsl).
void water_surface(const Inputs& in, Scene& scene) {
    if (in.water.detail <= 0 || in.water.coastal_waves <= 0)
        return;
    // The foam fades out by this depth, in map pixels.
    constexpr float foam_depth = 24;
    constexpr std::size_t vertex_budget = 196608;
    const auto begin = scene.vertices.size();
    for (std::size_t index = 0; index + 2 < scene.terrain_vertex_count &&
                                scene.vertices.size() - begin + 6 <= vertex_budget;
         index += 3) {
        std::array<Vertex, 3> triangle{
            scene.vertices[index], scene.vertices[index + 1], scene.vertices[index + 2]
        };
        if (std::all_of(
                triangle.begin(),
                triangle.end(),
                [&](const Vertex& vertex) { return vertex.height >= in.sea_level; }
            ) ||
            std::all_of(
                triangle.begin(),
                triangle.end(),
                [&](const Vertex& vertex) { return vertex.height <= in.sea_level - foam_depth; }
            ) ||
            std::all_of(triangle.begin(), triangle.end(), [](const Vertex& vertex) {
                return vertex.brightness == 0;
            }))
            continue;
        // Ground drawn beyond the picture for a moving camera's next frames
        // (Scene::ground_vertices) has no foam, so that it never takes the
        // budget from the picture's.
        if (scene.ground_source.picture_margin != 0) {
            const auto [least_x, greatest_x] =
                std::minmax({triangle[0].x, triangle[1].x, triangle[2].x});
            const auto [least_z, greatest_z] =
                std::minmax({triangle[0].z, triangle[1].z, triangle[2].z});
            const auto [low, high] =
                std::minmax({triangle[0].height, triangle[1].height, triangle[2].height});
            if (ground_off_screen(in, least_x, least_z, greatest_x, greatest_z, low, high))
                continue;
        }
        for (auto& vertex : triangle) {
            // The foam takes the colour of the picture the ground under it
            // shows (SurfaceCache::foam_colour); the fragment stage draws it
            // in the vertex's material colour.
            const auto foam = in.water_surface
                                  ? in.water_surface->foam_colour(
                                        vertex.x, vertex.z - vertex.height * height_lift
                                    )
                                  : std::array<float, 3>{
                                        renderer_water_appearance::foam_red,
                                        renderer_water_appearance::foam_green,
                                        renderer_water_appearance::foam_blue
                                    };
            vertex.red = foam[0];
            vertex.green = foam[1];
            vertex.blue = foam[2];
            vertex.terrain_x = in.sea_level - vertex.height;
            vertex.terrain_y = in.water_surface ? in.water_surface->fetch(vertex.x, vertex.z) : 1;
            vertex.terrain_size = -2;
            vertex.height = in.sea_level + 0.05F;
            vertex.emission_r = vertex.emission_g = vertex.emission_b = 0;
        }
        wet_polygon(scene, triangle);
    }
}

/// The ground's triangles each band of the plots' cut into it takes.
constexpr std::size_t ground_band_triangles = 8192;

/// The deferred frame materials each band of their writing takes.
constexpr std::size_t frame_write_band = 16;

/// Lists bands' vertices one after another, each band copied on a drawing
/// thread of its own.
///
/// @param in the frame
/// @param bands the bands, each with its vertices
/// @param[out] vertices the bands' vertices, in order; what it held before
///     is overwritten in place
template <typename Band>
void list_bands(const Inputs& in, std::span<Band> bands, std::vector<Vertex>& vertices) {
    std::vector<std::size_t> starts(bands.size() + 1);
    for (std::size_t band = 0; band < bands.size(); ++band)
        starts[band + 1] = starts[band] + bands[band].vertices.size();
    reserve_room(vertices, starts.back());
    vertices.resize(starts.back());
    platform::job_pool::run_bands(in.pool, static_cast<uint32_t>(bands.size()), [&](uint32_t band) {
        std::copy(
            bands[band].vertices.begin(),
            bands[band].vertices.end(),
            vertices.begin() + static_cast<std::ptrdiff_t>(starts[band])
        );
    });
}

/// Empties a pass's list for the frame, keeping its memory.
///
/// @param[out] list the list
/// @param end the pass's draws, which it has listed none of
void clear_list(PassList& list, std::size_t end) {
    list.vertices.clear();
    list.unit_ranges.clear();
    list.translucent_vertices.clear();
    list.translucent_ranges.clear();
    list.cut_vertices.clear();
    list.cut_from = 0;
    list.cut = false;
    list.cut_nanoseconds = 0;
    list.peak = 0;
    list.drawn = 0;
    list.nano_particles = 0;
    list.stopped = end;
    list.draw_vertices = 0;
    list.draw_translucent = 0;
    list.draw_ranges = 0;
    clear_cards(list.cards);
    list.failure = nullptr;
    list.nanoseconds = 0;
    list.materials_packed = 0;
    list.texels_packed = 0;
}

/// The bench's clock, while the stage timers measure.
///
/// @param timing the stage timers measure
/// @return nanoseconds since the clock's epoch, or 0
uint64_t pass_clock(bool timing) {
    return timing ? bench_timers::now_ns() : 0;
}

/// The bands of the cut of the ground drawn ahead a frame cuts.
struct AheadCut {
    std::size_t first{}; ///< the first of them
    std::size_t count{}; ///< how many
};

/// Plans the frame's share of the cut of the ground drawn ahead of a moving
/// camera, once it is placed (GroundAhead::cut_bands): bands of the
/// triangles the terrain's cut takes (ground_band_triangles), a share of
/// them a frame over ground_cut_frames frames, with the frame's plots; a
/// change of plots starts it again, and no plots leave the ground uncut.
///
/// @param[in,out] ahead the ground ahead
/// @param plots the frame's plots
/// @return the bands to cut this frame
AheadCut plan_ground_ahead_cut(GroundAhead& ahead, const Plots& plots) {
    if (!ahead.active || !ahead.placed || ahead.cut)
        return {};
    if (ahead.cut_waits) {
        ahead.cut_waits = false;
        return {};
    }
    if (plots.plots.empty()) {
        ahead.cut_plots.clear();
        ahead.cut_vertices.clear();
        ahead.cut_revision = ahead.revision;
        ahead.cut = true;
        return {};
    }
    if (ahead.cut_bands_done != 0 && ahead.cut_plots != plots.plots)
        ahead.cut_bands_done = 0;
    if (ahead.cut_bands_done == 0)
        ahead.cut_plots = plots.plots;
    const std::size_t bands =
        (ahead.vertices.size() / 3 + ground_band_triangles - 1) / ground_band_triangles;
    if (ahead.cut_bands.size() < bands)
        ahead.cut_bands.resize(bands);
    const std::size_t share = (bands + ground_cut_frames - 1) / ground_cut_frames;
    return {ahead.cut_bands_done, std::min(share, bands - ahead.cut_bands_done)};
}

/// Cuts the frame's plots into a band of the ground drawn ahead, on any
/// thread, as the terrain's cut cuts a band of the kept ground.
///
/// @param[in,out] ahead the ground ahead
/// @param plots the frame's plots, the ones its cut was begun with
/// @param index the band
/// @param timing the stage timers measure
void cut_ground_ahead_band(GroundAhead& ahead, const Plots& plots, std::size_t index, bool timing) {
    auto& band = ahead.cut_bands[index];
    const uint64_t began = pass_clock(timing);
    band.vertices.clear();
    band.failure = nullptr;
    const std::size_t first = index * ground_band_triangles * 3;
    const auto triangles =
        std::span<const Vertex>(ahead.vertices)
            .subspan(first, std::min(ground_band_triangles * 3, ahead.vertices.size() - first));
    try {
        band.cut = cut_plots(plots, triangles, false, band.vertices);
    } catch (...) {
        band.failure = std::current_exception();
    }
    band.nanoseconds = pass_clock(timing) - began;
}

/// Counts the bands of the ground drawn ahead cut this frame and, once
/// every band is, lists the cut ground and gives it the revision a cut
/// takes: a new one where a plot reached the ground, else the ground's.
/// A band that failed leaves the cut to the frame that takes the ground.
///
/// @param in the frame
/// @param[in,out] ahead the ground ahead
/// @param share the bands cut this frame
/// @return for the bench: the time cutting them took on their threads
uint64_t finish_ground_ahead_cut(const Inputs& in, GroundAhead& ahead, const AheadCut& share) {
    uint64_t nanoseconds = 0;
    if (share.count == 0)
        return nanoseconds;
    for (std::size_t index = share.first; index < share.first + share.count; ++index)
        nanoseconds += ahead.cut_bands[index].nanoseconds;
    ahead.cut_bands_done += share.count;
    const std::size_t bands =
        (ahead.vertices.size() / 3 + ground_band_triangles - 1) / ground_band_triangles;
    if (ahead.cut_bands_done < bands)
        return nanoseconds;
    const auto cut_bands = std::span(ahead.cut_bands).first(bands);
    bool reached = false;
    for (const auto& band : cut_bands) {
        if (band.failure) {
            ahead.cut_plots.clear();
            ahead.cut_vertices.clear();
            ahead.cut_revision = ahead.revision;
            ahead.cut = true;
            return nanoseconds;
        }
        reached = reached || band.cut;
    }
    list_bands(in, cut_bands, ahead.cut_vertices);
    ahead.cut_revision = reached ? new_ground_revision() : ahead.revision;
    ahead.cut = true;
    return nanoseconds;
}

/// Runs a pass on a drawing thread, beside the other passes, into its own
/// list: emptied first, then listed until the pass ends, stops for a
/// material not yet packed (PassList::stopped, the list as that draw
/// began) or fails (PassList::failure).
///
/// @param scene the frame, read alone
/// @param[out] list the pass's list
/// @param end the pass's draws
/// @param timing the stage timers measure
/// @param draws lists the pass's draws from the first, given the pass
template <typename Draws>
void run_beside(Scene& scene, PassList& list, std::size_t end, bool timing, Draws&& draws) {
    clear_list(list, end);
    const uint64_t start = pass_clock(timing);
    Pass pass{scene, list, false, 0};
    try {
        draws(pass);
        list.stopped = end;
    } catch (const Unpacked&) {
        list.vertices.resize(list.draw_vertices);
        list.translucent_vertices.resize(list.draw_translucent);
        list.unit_ranges.resize(list.draw_ranges);
        unmark_cards(list.cards);
    } catch (...) {
        list.failure = std::current_exception();
    }
    list.nanoseconds += pass_clock(timing) - start;
}

/// Cuts the frame's plots into what a pass has laid on the ground since
/// the last cut (PassList::cut_vertices).
///
/// @param plots the frame's plots, at least one
/// @param[in,out] list the pass's list
/// @param timing the stage timers measure
void cut_list(const Plots& plots, PassList& list, bool timing) {
    const uint64_t start = pass_clock(timing);
    const auto added = std::span<const Vertex>(list.vertices).subspan(list.cut_from);
    list.cut = cut_plots(plots, added, true, list.cut_vertices) || list.cut;
    list.cut_from = list.vertices.size();
    list.cut_nanoseconds += pass_clock(timing) - start;
}

/// Takes a pass's list into the frame once the passes before it are in:
/// holds the frame to its geometry budget, raises the pass's failure, and
/// lists the draws it stopped at, packing the materials they need, as the
/// pass would have one after the others.
///
/// @param scene the frame
/// @param[in,out] list the pass's list
/// @param end the pass's draws
/// @param[in,out] listed the frame's vertices before the list, then after it
/// @param timing the stage timers measure
/// @param draws lists the pass's draws from a first one, given the pass
///     and that draw
template <typename Draws>
void take_list(
    Scene& scene, PassList& list, std::size_t end, std::size_t& listed, bool timing, Draws&& draws
) {
    if (listed + list.peak > maximum_vertices)
        throw GeometryFull();
    if (list.failure)
        std::rethrow_exception(list.failure);
    if (list.stopped < end) {
        const uint64_t start = pass_clock(timing);
        Pass pass{scene, list, true, listed};
        draws(pass, list.stopped);
        list.stopped = end;
        list.nanoseconds += pass_clock(timing) - start;
    }
    listed += list.vertices.size();
}

/// Adds what the passes took to the bench's stage timers when a frame's
/// passes are done with, built or not.
class PassTimes {
  public:

    /// @param scene the frame, whose lists hold the passes' times
    /// @param timing the stage timers measure
    PassTimes(const Scene& scene, bool timing) : scene_(scene), timing_(timing) {}

    PassTimes(const PassTimes&) = delete;
    PassTimes& operator=(const PassTimes&) = delete;

    /// Adds the passes' times and the materials they packed.
    ~PassTimes() {
        if (!timing_)
            return;
        using bench_timers::Stage;
        const auto& passes = scene_.passes;
        const auto add = [](Stage stage, const PassList& list) {
            bench_timers::add_nanoseconds(stage, list.nanoseconds);
            bench_timers::count_packed_materials(stage, list.materials_packed, list.texels_packed);
        };
        add(Stage::ground_sprites, passes.ground_cards);
        add(Stage::shadows, passes.shadow_packing);
        for (std::size_t band = 0; band < shadow_bands; ++band) {
            add(Stage::shadows, passes.shadows[band]);
            bench_timers::add_nanoseconds(Stage::shadows, passes.shadow_bands[band].nanoseconds);
            bench_timers::add_nanoseconds(Stage::foundations, passes.shadows[band].cut_nanoseconds);
        }
        for (std::size_t band = 0; band < ground_bands; ++band)
            bench_timers::add_nanoseconds(Stage::foundations, passes.ground[band].nanoseconds);
        bench_timers::add_nanoseconds(Stage::foundations, passes.ground_cards.cut_nanoseconds);
        bench_timers::add_nanoseconds(Stage::foundations, foundations + ground_listing);
        add(Stage::models, passes.models);
        add(Stage::projectiles, passes.projectiles);
        add(Stage::sprites, passes.cards);
        add(Stage::particles, passes.particles);
        bench_timers::add_nanoseconds(Stage::water, joining);
        bench_timers::add_nanoseconds(Stage::terrain, ground_ahead);
        bench_timers::add_nanoseconds(Stage::foundations, ahead_cut);
    }

    std::size_t ground_bands{}; ///< the ground's bands cut this frame
    std::size_t shadow_bands{}; ///< the shadows' bands read this frame
    uint64_t foundations{};     ///< the plots' planning and the cut ground's taking
    uint64_t ground_listing{};  ///< the kept ground's listing beside the passes
    uint64_t joining{};         ///< the lists' joining
    uint64_t ground_ahead{};    ///< the bands of the ground drawn ahead of a moving camera
    uint64_t ahead_cut{};       ///< the plots' cut of the ground drawn ahead

  private:

    const Scene& scene_;
    bool timing_{};
};

/// Writes the frame's own materials the shadows packed while the passes
/// ran at once, now that the passes before them are in, on the drawing
/// threads: each into its own rectangle.
///
/// @param in the frame
/// @param[in,out] scene the atlas
void write_frame_materials(const Inputs& in, Scene& scene) {
    const auto& writes = scene.passes.frame_writes;
    const auto& bands = scene.passes.shadow_bands;
    const auto texels = [&](const FrameWrite& write) {
        const auto& rect = write.rectangle;
        return std::span<const uint8_t>(bands[write.band].texels)
            .subspan(write.texels, static_cast<std::size_t>(rect.width) * rect.height);
    };
    platform::job_pool::run_bands(
        in.pool,
        platform::job_pool::bands_of_rows(static_cast<uint32_t>(writes.size()), frame_write_band),
        [&](uint32_t band) {
            const std::size_t first = std::size_t{band} * frame_write_band;
            const std::size_t last = std::min(writes.size(), first + frame_write_band);
            for (std::size_t index = first; index < last; ++index)
                write_black(
                    scene,
                    writes[index].rectangle,
                    texels(writes[index]),
                    texels(writes[index]),
                    -1,
                    writes[index].alpha_coverage
                );
        }
    );
    for (const auto& write : writes) {
        ++scene.texture_revision;
        note_rows(
            scene,
            scene.atlas_update.frame_first_row,
            scene.atlas_update.frame_end_row,
            write.rectangle
        );
    }
}

/// Builds what the frame draws after its terrain: the cards laid on the
/// ground, the units' shadows, the foundations, the models, the
/// projectiles, the standing cards and the particles, each pass listing
/// its own triangles (PassList) on the drawing threads at once, then their
/// lists joined in that order after the terrain.
///
/// The passes read the frame and the atlas and change neither. The shadows'
/// materials are packed among the frame's own beforehand, on the calling
/// thread, in their order. The lists are then taken in the passes' order:
/// each held to the geometry budget, its failure raised, the frame's own
/// materials written, the plots' cut ground taken, and the draws of a pass
/// that stopped for a material not yet packed listed now, after the passes
/// before it, packing what they need. Every step is the one the passes
/// make one after another, and an exception leaves the scene where they
/// would have left it.
///
/// The bands of the ground drawn ahead of a moving camera (GroundAhead)
/// due this frame run among the passes, after them, and its cut with the
/// frame's plots beside the terrain's (cut_ground_ahead).
///
/// @param in the frame
/// @param[in,out] scene the frame, its terrain listed (Scene::vertices)
/// @param ground_kept the frame's terrain is the kept ground, not yet
///     listed (terrain)
/// @param ahead_bands the bands of the ground ahead to draw
void run_passes(const Inputs& in, Scene& scene, bool ground_kept, std::size_t ahead_bands) {
    auto& passes = scene.passes;
    const auto& draws = *in.draws;
    const bool timing = bench_timers::timers.on;
    trim_card_types(scene.card_types);
    for (auto* list :
         {&passes.ground_cards,
          &passes.shadow_packing,
          &passes.models,
          &passes.projectiles,
          &passes.cards,
          &passes.particles})
        clear_list(*list, 0);
    PassTimes times(scene, timing);
    uint64_t start = pass_clock(timing);
    // The structures' plots, and the ground they are cut into: the cut kept
    // from the frame before when it has the same plots in the same kept
    // ground, or the terrain cut now.
    Plots plots;
    plan_plots(in, plots);
    const bool plotted = !plots.plots.empty();
    const auto source = scene.ground_revision;
    const bool kept = scene.ground_kept && source == scene.kept_ground_revision;
    const bool cut_before =
        plotted && kept && scene.cut_source == source && scene.cut_plots == plots.plots;
    const std::size_t terrain_count = scene.terrain_vertex_count;
    const auto& ground = ground_kept ? scene.ground_vertices : scene.vertices;
    times.ground_bands = plotted && !cut_before ? (terrain_count / 3 + ground_band_triangles - 1) /
                                                      ground_band_triangles
                                                : 0;
    times.shadow_bands = (draws.models.size() + shadow_band_draws - 1) / shadow_band_draws;
    const std::size_t ground_bands = times.ground_bands;
    const std::size_t shadow_bands = times.shadow_bands;
    if (passes.ground.size() < ground_bands)
        passes.ground.resize(ground_bands);
    if (passes.shadows.size() < shadow_bands) {
        passes.shadows.resize(shadow_bands);
        passes.shadow_bands.resize(shadow_bands);
    }
    for (std::size_t band = 0; band < shadow_bands; ++band)
        clear_list(passes.shadows[band], 0);
    const auto ahead_cut = plan_ground_ahead_cut(scene.ground_ahead, plots);
    times.foundations += pass_clock(timing) - start;
    // The terrain's cut, the shadows read from the units' pictures, and the
    // cut of the ground drawn ahead.
    platform::job_pool::run_bands(
        in.pool,
        static_cast<uint32_t>(ground_bands + shadow_bands + ahead_cut.count),
        [&](uint32_t band) {
            if (band >= ground_bands + shadow_bands) {
                cut_ground_ahead_band(
                    scene.ground_ahead,
                    plots,
                    ahead_cut.first + band - ground_bands - shadow_bands,
                    timing
                );
                return;
            }
            if (band < ground_bands) {
                auto& list = passes.ground[band];
                clear_list(list, 0);
                const uint64_t began = pass_clock(timing);
                const std::size_t first = band * ground_band_triangles * 3;
                const auto triangles = std::span<const Vertex>(ground).subspan(
                    first, std::min(ground_band_triangles * 3, terrain_count - first)
                );
                try {
                    list.cut = cut_plots(plots, triangles, false, list.vertices);
                } catch (...) {
                    list.failure = std::current_exception();
                }
                list.nanoseconds = pass_clock(timing) - began;
                return;
            }
            const std::size_t index = band - ground_bands;
            auto& shadow = passes.shadow_bands[index];
            const uint64_t began = pass_clock(timing);
            try {
                read_shadows(
                    in,
                    scene,
                    shadow,
                    index * shadow_band_draws,
                    std::min(draws.models.size(), (index + 1) * shadow_band_draws)
                );
            } catch (...) {
                shadow.failure = std::current_exception();
            }
            shadow.nanoseconds = pass_clock(timing) - began;
        }
    );
    times.ahead_cut += finish_ground_ahead_cut(in, scene.ground_ahead, ahead_cut);
    // The ground as the foundations leave it: the cut kept, the kept ground
    // cut now, which is kept, or the ground kept, is listed beside the
    // passes, which find it where it is kept.
    start = pass_clock(timing);
    bool ground_cut{};
    std::span<const Vertex> cut_ground = ground;
    const bool cut_now = kept && ground_bands != 0;
    if (cut_before) {
        cut_ground = scene.cut_ground;
    } else if (cut_now) {
        // The cut is kept for the frames after; until the passes are in it
        // is kept for none.
        scene.cut_source = 0;
        list_bands(in, std::span(passes.ground).first(ground_bands), scene.cut_ground);
        for (std::size_t band = 0; band < ground_bands; ++band)
            ground_cut |= passes.ground[band].cut;
        cut_ground = scene.cut_ground;
    } else if (ground_bands != 0) {
        scene.vertices.clear();
        for (std::size_t band = 0; band < ground_bands; ++band) {
            const auto& cut = passes.ground[band];
            scene.vertices.insert(scene.vertices.end(), cut.vertices.begin(), cut.vertices.end());
            ground_cut |= cut.cut;
        }
        cut_ground = scene.vertices;
    }
    scene.terrain_vertex_count = cut_ground.size();
    const bool list_ground_beside = cut_before || cut_now || (ground_kept && ground_bands == 0);
    times.foundations += pass_clock(timing) - start;
    // The shadows' materials packed among the frame's own, as far as one
    // that goes with the other materials.
    start = pass_clock(timing);
    auto& frame = passes.frame;
    frame.next_x = frame.next_y = frame.row_height = 0;
    frame.materials.clear();
    frame.order.assign(scene.frame.order.begin(), scene.frame.order.end());
    frame.packed = 0;
    frame.material_area = frame.spilled_area = 0;
    frame.aircraft_shadows.clear();
    passes.frame_writes.clear();
    std::size_t stop_band = shadow_bands;
    std::size_t stop_shadow{};
    {
        Pass packing{scene, passes.shadow_packing, false, 0};
        for (std::size_t band = 0; band < shadow_bands; ++band) {
            auto& shadow = passes.shadow_bands[band];
            const std::size_t packed = shadow.failure ? 0
                                                      : pack_shadows(
                                                            packing,
                                                            frame,
                                                            shadow,
                                                            band,
                                                            0,
                                                            shadow.pictures.size(),
                                                            &passes.frame_writes
                                                        );
            if (shadow.failure || packed < shadow.pictures.size()) {
                stop_band = band;
                stop_shadow = packed;
                break;
            }
        }
    }
    passes.shadow_packing.nanoseconds += pass_clock(timing) - start;
    // Every pass at once, the longest first, then the ground ahead.
    const auto shadows_drawn = [&](std::size_t band) {
        return band < stop_band    ? passes.shadow_bands[band].pictures.size()
               : band == stop_band ? stop_shadow
                                   : 0;
    };
    constexpr uint32_t first_shadow_band = 4;
    const auto pass_bands = static_cast<uint32_t>(first_shadow_band + shadow_bands + 2);
    const auto bands = static_cast<uint32_t>(pass_bands + ahead_bands);
    const std::size_t cards = draws.sprites.size();
    auto& ahead = scene.ground_ahead;
    const auto ahead_draw = ground_draw(ahead.source);
    const std::size_t first_ahead = ahead.bands_drawn;
    platform::job_pool::run_bands(in.pool, bands, [&](uint32_t band) {
        if (band >= pass_bands) {
            draw_ground_band(ahead_draw, ahead.bands[first_ahead + band - pass_bands], timing);
        } else if (band == 0) {
            run_beside(scene, passes.models, draws.models.size(), timing, [&](Pass& pass) {
                models(in, pass, 0, draws.models.size());
            });
        } else if (band == 1) {
            run_beside(scene, passes.cards, cards, timing, [&](Pass& pass) {
                pass.ground = cut_ground;
                sprites(in, pass, false, 0, cards);
            });
        } else if (band == 3) {
            if (!list_ground_beside)
                return;
            const uint64_t began = pass_clock(timing);
            scene.vertices.assign(cut_ground.begin(), cut_ground.end());
            times.ground_listing = pass_clock(timing) - began;
        } else if (band == 2) {
            auto& list = passes.ground_cards;
            run_beside(scene, list, cards, timing, [&](Pass& pass) {
                sprites(in, pass, true, 0, cards);
            });
            if (plotted && !list.failure)
                cut_list(plots, list, timing);
        } else if (band < first_shadow_band + shadow_bands) {
            const std::size_t index = band - first_shadow_band;
            auto& list = passes.shadows[index];
            const auto drawn = shadows_drawn(index);
            run_beside(scene, list, 0, timing, [&](Pass& pass) {
                draw_shadows(in, pass, passes.shadow_bands[index], 0, drawn);
            });
            if (plotted && !list.failure)
                cut_list(plots, list, timing);
        } else if (band == first_shadow_band + shadow_bands) {
            const std::size_t shots = draws.projectiles.size();
            run_beside(scene, passes.projectiles, shots, timing, [&](Pass& pass) {
                projectiles(in, pass, 0, shots);
            });
        } else {
            const std::size_t count = draws.spatial_particles.size();
            run_beside(scene, passes.particles, count, timing, [&](Pass& pass) {
                particles(in, pass, 0, count);
            });
        }
    });
    for (std::size_t band = 0; band < ahead_bands; ++band)
        times.ground_ahead += ahead.bands[first_ahead + band].nanoseconds;
    ahead.bands_drawn += ahead_bands;
    // The lists taken in the passes' order, from the terrain's end as the
    // passes before the foundations found it.
    std::size_t listed = terrain_count;
    take_list(scene, passes.ground_cards, cards, listed, timing, [&](Pass& pass, std::size_t from) {
        sprites(in, pass, true, from, cards);
    });
    if (plotted && passes.ground_cards.cut_from < passes.ground_cards.vertices.size())
        cut_list(plots, passes.ground_cards, timing);
    // The shadows: their bands as drawn, then the frame's own materials as
    // packed, then the shadows from the one that stopped the packing.
    std::size_t shadows_listed = listed;
    for (std::size_t band = 0; band < shadow_bands; ++band) {
        const auto& list = passes.shadows[band];
        if (shadows_listed + list.peak > maximum_vertices)
            throw GeometryFull();
        if (list.failure)
            std::rethrow_exception(list.failure);
        shadows_listed += list.vertices.size();
    }
    start = pass_clock(timing);
    std::swap(scene.frame.next_x, frame.next_x);
    std::swap(scene.frame.next_y, frame.next_y);
    std::swap(scene.frame.row_height, frame.row_height);
    std::swap(scene.frame.materials, frame.materials);
    std::swap(scene.frame.order, frame.order);
    std::swap(scene.frame.packed, frame.packed);
    std::swap(scene.frame.material_area, frame.material_area);
    std::swap(scene.frame.spilled_area, frame.spilled_area);
    std::swap(scene.frame.aircraft_shadows, frame.aircraft_shadows);
    write_frame_materials(in, scene);
    if (stop_band < shadow_bands) {
        auto& list = passes.shadows[stop_band];
        std::size_t before = listed;
        for (std::size_t band = 0; band < stop_band; ++band)
            before += passes.shadows[band].vertices.size();
        Pass pass{scene, list, true, before};
        for (std::size_t band = stop_band; band < shadow_bands; ++band) {
            auto& shadow = passes.shadow_bands[band];
            if (shadow.failure)
                std::rethrow_exception(shadow.failure);
            for (std::size_t index = band == stop_band ? stop_shadow : 0;
                 index < shadow.pictures.size();
                 ++index) {
                pack_shadows(pass, scene.frame, shadow, band, index, index + 1, nullptr);
                draw_shadows(in, pass, shadow, index, index + 1);
            }
        }
        if (plotted)
            cut_list(plots, list, timing);
    }
    passes.shadow_packing.nanoseconds += pass_clock(timing) - start;
    for (std::size_t band = 0; band < shadow_bands; ++band)
        listed += passes.shadows[band].vertices.size();
    // The foundations: the cut ground and what lies on it, held to the
    // budget, then the cut kept for the frames after.
    if (plotted) {
        start = pass_clock(timing);
        for (std::size_t band = 0; band < ground_bands; ++band)
            if (passes.ground[band].failure)
                std::rethrow_exception(passes.ground[band].failure);
        listed = scene.terrain_vertex_count + passes.ground_cards.cut_vertices.size();
        for (std::size_t band = 0; band < shadow_bands; ++band)
            listed += passes.shadows[band].cut_vertices.size();
        if (listed > maximum_vertices)
            throw GeometryFull();
        // A plot cut into the ground makes it new.
        if (cut_before)
            scene.ground_revision = scene.cut_revision;
        else if (ground_cut)
            scene.ground_revision = new_ground_revision();
        if (kept && !cut_before) {
            if (!cut_now)
                scene.cut_ground.assign(
                    scene.vertices.begin(),
                    scene.vertices.begin() + static_cast<std::ptrdiff_t>(scene.terrain_vertex_count)
                );
            scene.cut_plots = std::move(plots.plots);
            scene.cut_source = source;
            scene.cut_revision = scene.ground_revision;
        }
        times.foundations += pass_clock(timing) - start;
    }
    const std::size_t models_first = listed;
    take_list(
        scene,
        passes.models,
        draws.models.size(),
        listed,
        timing,
        [&](Pass& pass, std::size_t from) { models(in, pass, from, draws.models.size()); }
    );
    listed += passes.models.cards.vertices;
    take_list(
        scene,
        passes.projectiles,
        draws.projectiles.size(),
        listed,
        timing,
        [&](Pass& pass, std::size_t from) { projectiles(in, pass, from, draws.projectiles.size()); }
    );
    take_list(scene, passes.cards, cards, listed, timing, [&](Pass& pass, std::size_t from) {
        pass.ground = cut_ground;
        sprites(in, pass, false, from, cards);
    });
    take_list(
        scene,
        passes.particles,
        draws.spatial_particles.size(),
        listed,
        timing,
        [&](Pass& pass, std::size_t from) {
            particles(in, pass, from, draws.spatial_particles.size());
        }
    );
    // The lists joined after the ground in the passes' order.
    start = pass_clock(timing);
    const auto laid = [plotted](const PassList& list) -> const std::vector<Vertex>& {
        return plotted ? list.cut_vertices : list.vertices;
    };
    reserve_room(scene.vertices, listed);
    const auto join = [&scene](const std::vector<Vertex>& vertices) {
        scene.vertices.insert(scene.vertices.end(), vertices.begin(), vertices.end());
    };
    join(laid(passes.ground_cards));
    uint32_t shadows{};
    for (std::size_t band = 0; band < shadow_bands; ++band) {
        join(laid(passes.shadows[band]));
        shadows += passes.shadows[band].drawn;
    }
    join(passes.models.vertices);
    for (auto range : passes.models.unit_ranges) {
        range.first_vertex += models_first;
        scene.unit_ranges.push_back(range);
    }
    // The models drawn on the card, among the models' vertices.
    auto& on_card = passes.models.cards;
    for (auto& run : on_card.runs)
        run.vertex += models_first;
    for (auto& unit : on_card.units)
        unit.vertex += models_first;
    std::swap(scene.cards, on_card);
    std::swap(scene.translucent_vertices, passes.models.translucent_vertices);
    std::swap(scene.translucent_ranges, passes.models.translucent_ranges);
    join(passes.projectiles.vertices);
    join(passes.cards.vertices);
    join(passes.particles.vertices);
    scene.shadow_count = shadows;
    scene.model_count = passes.models.drawn;
    scene.sprite_count = passes.ground_cards.drawn + passes.cards.drawn;
    scene.particle_count = passes.particles.drawn;
    scene.nano_particle_count = passes.particles.nano_particles;
    times.joining += pass_clock(timing) - start;
}

/// Notes the frame's kept ground for the card (Scene::card_ground), and,
/// once the ground drawn ahead of a moving camera is cut, the share of the
/// vertices the card will take from it sent this frame: whole triangles,
/// over ground_send_frames frames, or all that are left in a frame after
/// which the camera is expected to pass the kept ground's margin.
///
/// @param[in,out] scene the frame and the ground ahead
void card_ground(Scene& scene) {
    scene.card_ground = {scene.terrain_vertex_count, scene.ground_revision};
    auto& ahead = scene.ground_ahead;
    if (!ahead.active || !ahead.placed || !ahead.cut)
        return;
    const auto next = ahead_card_vertices(ahead);
    const std::size_t triangles = next.size() / 3;
    const std::size_t share = (triangles + ground_send_frames - 1) / ground_send_frames * 3;
    auto& card = scene.card_ground;
    card.next = next;
    card.next_revision = ahead.cut_plots.empty() ? ahead.revision : ahead.cut_revision;
    card.next_first = ahead.sent;
    card.next_count =
        ahead.send_rest ? next.size() - ahead.sent : std::min(share, next.size() - ahead.sent);
    ahead.sent += card.next_count;
}

/// Gives the frame's own materials more rows (Scene::frame_rows) when some
/// of this frame's found no room in theirs and were packed with the other
/// materials, where they stay until the atlas is emptied: as many more as
/// would hold twice their area, from the foot of the other materials' rows,
/// as far as those have not been packed. The frames after it pack their
/// own materials afresh in the rows as moved.
///
/// @param[in,out] scene the atlas
void grow_frame_rows(Scene& scene) {
    if (scene.frame.spilled_area == 0 || scene.frame_rows == 0)
        return;
    const uint64_t step_area = uint64_t{scene.texture_width} * frame_row_step;
    const uint64_t wanted = (scene.frame.spilled_area * 2 + step_area - 1) / step_area;
    const uint32_t packed = std::min(scene.next_y + scene.row_height, kept_rows(scene));
    const uint32_t free_steps = (kept_rows(scene) - packed) / frame_row_step;
    const uint32_t most =
        maximum_atlas_edge / 2 - std::min(scene.frame_rows, maximum_atlas_edge / 2);
    const auto rows =
        std::min<uint64_t>({wanted * frame_row_step, uint64_t{free_steps} * frame_row_step, most});
    if (rows == 0)
        return;
    scene.frame_rows += static_cast<uint32_t>(rows);
    scene.frame.order.clear();
}
} // namespace

bool build(const Inputs& inputs, Scene& scene, std::string& error) {
    auto& update = scene.atlas_update;
    update.previous_revision = scene.texture_revision;
    update.first_row = update.frame_first_row = scene.texture_height;
    update.end_row = update.frame_end_row = 0;
    update.regions.clear();
    scene.out_of_room = false;
    scene.frame_palette = nullptr;
    try {
        if (inputs.map == nullptr || inputs.palette == nullptr || inputs.draws == nullptr ||
            inputs.map->attribute_width == 0 || inputs.map->attribute_height == 0)
            throw std::runtime_error("experimental renderer has no prepared match scene");
        scene.frame_palette_digest = mix_bytes(hash_start, *inputs.palette);
        scene.frame_palette = inputs.palette;
        const auto& map = *inputs.map;
        if (map.attribute_width > formats::tnt::limit::attribute_dimension ||
            map.attribute_height > formats::tnt::limit::attribute_dimension ||
            map.attributes.size() <
                static_cast<std::size_t>(map.attribute_width) * map.attribute_height ||
            map.tile_width < (map.attribute_width + 1) / 2 ||
            map.tile_height < (map.attribute_height + 1) / 2 ||
            map.tile_indices.size() < static_cast<std::size_t>(map.tile_width) * map.tile_height)
            throw std::runtime_error("experimental renderer received incomplete terrain");
        if (inputs.left > inputs.right || inputs.top > inputs.bottom)
            throw std::runtime_error("experimental renderer received a reversed terrain crop");
        if (scene.texture_width == 0 || scene.texture_height == 0 ||
            scene.texture_width > maximum_atlas_edge || scene.texture_height > maximum_atlas_edge)
            throw std::runtime_error("experimental renderer received an invalid atlas size");
        // A camera that has moved since the last frame holds the tiles in
        // their squares until it has stood still for camera_settle_frames
        // frames. A move is from one picture to another.
        const bool camera_known = scene.camera_width != 0 && scene.camera_height != 0 &&
                                  inputs.clip_width != 0 && inputs.clip_height != 0;
        const bool camera_moved =
            camera_known &&
            (scene.camera_projection != inputs.clip_projection ||
             scene.camera_width != inputs.clip_width || scene.camera_height != inputs.clip_height);
        if (camera_moved)
            scene.camera_settling_frames = camera_settle_frames;
        else if (scene.camera_settling_frames != 0)
            --scene.camera_settling_frames;
        // How the picture moved since the last frame, which the ground drawn
        // ahead of a moving camera expects it to go on moving by.
        std::array<float, 12> motion{};
        if (camera_moved && scene.camera_width == inputs.clip_width &&
            scene.camera_height == inputs.clip_height)
            for (std::size_t index = 0; index < motion.size(); ++index)
                motion[index] = inputs.clip_projection[index] - scene.camera_projection[index];
        scene.camera_projection = inputs.clip_projection;
        scene.camera_width = inputs.clip_width;
        scene.camera_height = inputs.clip_height;
        const bool holding = inputs.tiles_hold_while_moving && scene.camera_settling_frames != 0;
        // Once a moving camera has packed the atlas, it holds what no
        // settled camera's would until it is next emptied.
        scene.moving_packing = scene.moving_packing || holding;
        // The terrain takes the atlas at its largest, less room for the other
        // materials: in full squares while every tile it draws fits so, and
        // otherwise the tiles nearest the camera in the larger squares that fit.
        constexpr uint64_t atlas_area = uint64_t{maximum_atlas_edge} * maximum_atlas_edge;
        const uint64_t starting_room =
            atlas_area -
            std::min(atlas_area, std::max(least_material_room, scene.material_area / 2 * 3));
        bench_timers::Span surveying(bench_timers::Stage::survey);
        const auto demand = terrain_demand(inputs, starting_room);
        // A view whose terrain last had to halve its room, surveying as many
        // tiles, starts in the room it fitted (Scene::terrain_room) rather
        // than emptying the atlas twice more to find it again.
        uint64_t room = starting_room;
        if (!demand.empty() && demand.size() == scene.terrain_room_tiles)
            room = std::min(room, scene.terrain_room);
        auto plan = plan_terrain(demand, room, holding);
        plan.exact = scene.moving_packing;
        surveying.stop();
        // Far out over a large map, the ground is drawn from fewer height
        // samples, as few as keep its triangles within the frame's
        // geometry budget: fewer than the last frame that ran out found
        // too many, and twice as few each time this frame runs out.
        const auto crop = terrain_crop(inputs);
        const uint64_t cells = static_cast<uint64_t>((crop.right - crop.left) / cell_edge + 1) *
                               static_cast<uint64_t>((crop.bottom - crop.top) / cell_edge + 1);
        int32_t step = 1;
        while (cells / (static_cast<uint64_t>(step) * step) > scene.terrain_square_limit &&
               step < coarsest_terrain_step)
            step *= 2;
        // Retry with only this frame's materials, growing to the bounded
        // maximum when the smaller atlas has filled. Old map and animation
        // variants therefore cannot exhaust the cache permanently. A frame
        // whose materials still do not fit gives its terrain half the room,
        // and then a quarter, before the frame is given up.
        constexpr uint32_t attempts = 4;
        bool emptied = scene.next_x == 0 && scene.next_y == 0 && scene.materials.empty() &&
                       scene.terrain_materials.empty();
        // A moving camera's ground may reach beyond the picture's margin
        // until its triangles do not fit.
        bool widen = true;
        for (uint32_t attempt = 0; attempt < attempts;) {
            scene.vertices.clear();
            scene.unit_ranges.clear();
            scene.translucent_vertices.clear();
            scene.translucent_ranges.clear();
            clear_cards(scene.cards);
            scene.shadow_count = 0;
            scene.model_count = 0;
            scene.sprite_count = 0;
            scene.particle_count = 0;
            scene.nano_particle_count = 0;
            scene.packed_material_area = 0;
            scene.frame.next_x = 0;
            scene.frame.next_y = 0;
            scene.frame.row_height = 0;
            scene.frame.materials.clear();
            scene.frame.packed = 0;
            scene.frame.material_area = 0;
            scene.frame.spilled_area = 0;
            scene.frame.aircraft_shadows.clear();
            scene.texture_rgba.resize(
                static_cast<std::size_t>(scene.texture_width) * scene.texture_height * channels
            );
            // How much of the geometry budget the ground took, once drawn.
            std::size_t ground_vertices = maximum_vertices;
            try {
                using bench_timers::Span;
                using bench_timers::Stage;
                bool ground_kept{};
                {
                    const Span timed(Stage::terrain);
                    ground_kept = terrain(
                        inputs, scene, plan, step, camera_known && !camera_moved, widen, motion
                    );
                }
                ground_vertices = scene.terrain_vertex_count;
                const auto& ahead = scene.ground_ahead;
                run_passes(
                    inputs,
                    scene,
                    ground_kept,
                    ahead.active
                        ? std::min(ahead.bands_a_frame, ahead.bands.size() - ahead.bands_drawn)
                        : 0
                );
                const Span water_and_rest(Stage::water);
                // The half-transparent units over everything opaque, under
                // the water's surface.
                for (auto range : scene.translucent_ranges) {
                    range.first_vertex += scene.vertices.size();
                    scene.unit_ranges.push_back(range);
                }
                scene.vertices.insert(
                    scene.vertices.end(),
                    scene.translucent_vertices.begin(),
                    scene.translucent_vertices.end()
                );
                water_surface(inputs, scene);
                water_disturbances(inputs, scene);
                // Every other material this frame draws was packed into the
                // emptied atlas, so their area is what they need.
                if (emptied)
                    scene.material_area = scene.packed_material_area + scene.frame.material_area;
                scene.last_frame_material_area = scene.frame.material_area;
                if (room < starting_room) {
                    scene.terrain_room = room;
                    scene.terrain_room_tiles = demand.size();
                } else {
                    scene.terrain_room_tiles = 0;
                }
                break;
            } catch (const GeometryFull&) {
                // A frame tried again draws its ground afresh.
                scene.ground_ahead.active = false;
                // The ground beyond a moving camera's picture goes first.
                if (widen && scene.ground_source.picture_margin != 0) {
                    widen = false;
                    scene.ground_kept = false;
                    continue;
                }
                // Fewer height samples help only where the ground takes
                // most of the budget.
                if (step >= coarsest_terrain_step || ground_vertices < maximum_vertices / 2)
                    throw;
                scene.terrain_square_limit = std::min(
                    scene.terrain_square_limit, cells / (static_cast<uint64_t>(step) * step) / 2
                );
                step *= 2;
            } catch (const AtlasFull&) {
                scene.ground_ahead.active = false;
                scene.moving_packing = holding;
                bench_timers::count_atlas_emptied();
                if (++attempt == attempts)
                    throw;
                // The rows kept for the frame's own materials hold half as
                // much again as a frame packs, as the terrain leaves the
                // other materials room.
                const uint64_t frame_need =
                    std::max(scene.last_frame_material_area, scene.frame.material_area);
                const auto frame_rows = std::min(
                    maximum_atlas_edge / 2,
                    static_cast<uint32_t>(
                        (frame_need / 2 * 3 + uint64_t{maximum_atlas_edge} * frame_row_step - 1) /
                        (uint64_t{maximum_atlas_edge} * frame_row_step)
                    ) * frame_row_step
                );
                scene.frame_rows = frame_rows;
                scene.frame.order.clear();
                // An emptied atlas at its largest could not hold the plan.
                // Otherwise the atlas is emptied for it, and the frame plans
                // from the room it would start with had none been
                // remembered, as the frames before it did.
                if (emptied && scene.texture_width == maximum_atlas_edge &&
                    scene.texture_height == maximum_atlas_edge) {
                    room = std::min(room, planned_area(demand, plan)) / 2;
                    plan = plan_terrain(demand, room, holding);
                } else if (room != starting_room) {
                    room = starting_room;
                    plan = plan_terrain(demand, room, holding);
                }
                plan.exact = scene.moving_packing;
                emptied = true;
                scene.texture_width = maximum_atlas_edge;
                scene.texture_height = maximum_atlas_edge;
                // Old atlas coordinates are discarded, including when already
                // at maximum size. The frame packs everything it draws afresh
                // from the first row, and only what it writes is uploaded,
                // against the original base revision: no triangle samples
                // the rest, which may hold texels written here by the
                // attempts before and never sent (note_rows).
                update.first_row = update.frame_first_row = scene.texture_height;
                update.end_row = update.frame_end_row = 0;
                update.regions.clear();
                scene.materials.clear();
                scene.terrain_materials.clear();
                scene.frame.aircraft_shadows.clear();
                for (auto& spare : scene.spare_terrain_squares)
                    spare.clear();
                scene.split_terrain_squares.clear();
                scene.next_x = 0;
                scene.next_y = 0;
                scene.row_height = 0;
                ++scene.texture_revision;
            }
        }
        grow_frame_rows(scene);
        card_ground(scene);
        if (scene.atlas_update.first_row > scene.atlas_update.end_row) {
            scene.atlas_update.first_row = 0;
            scene.atlas_update.end_row = 0;
        }
        if (scene.atlas_update.frame_first_row > scene.atlas_update.frame_end_row) {
            scene.atlas_update.frame_first_row = 0;
            scene.atlas_update.frame_end_row = 0;
        }
        scene.frame_palette = nullptr;
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        scene.frame_palette = nullptr;
        scene.card_ground = {};
        scene.out_of_room = dynamic_cast<const OutOfRoom*>(&exception) != nullptr;
        scene.vertices.clear();
        scene.unit_ranges.clear();
        clear_cards(scene.cards);
        scene.shadow_count = 0;
        scene.terrain_vertex_count = 0;
        scene.model_count = 0;
        scene.sprite_count = 0;
        scene.particle_count = 0;
        scene.nano_particle_count = 0;
        error = exception.what();
        return false;
    }
}
} // namespace oa::app::renderer_live_scene
