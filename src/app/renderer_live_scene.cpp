// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_live_scene.hpp"
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
#include <exception>
#include <cstring>
#include <limits>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>

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
constexpr uint64_t hash_multiplier = 1099511628211ULL;
constexpr std::size_t maximum_vertices = 4U * 1024U * 1024U;
constexpr uint32_t maximum_atlas_edge = 4096;

/// Signals a full atlas so the next attempt can discard old frame materials.
class AtlasFull : public std::runtime_error {
  public:

    AtlasFull() : std::runtime_error("experimental material atlas is full") {}
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
        (index >= sight.player_bits.size() || (sight.player_bits[index] & sight.viewer_bit) == 0))
        return Fog::black;
    const bool visible = index < sight.coverage.size() &&
                         sim::visibility_state::count_in_sight(
                             sight.coverage[index], sim::visibility_state::SightReading::player
                         );
    return sight.line_of_sight && !visible ? Fog::grey : Fog::seen;
}

/// Interpolates fog inside known ground and fades to black before unknown cells.
float brightness_at(const full::SightView& sight, float x, float y, float unseen = -1) {
    constexpr int32_t fog_edge = present::world_renderer::fog_cell_pixels;
    constexpr float edge = fog_edge;
    const float unseen_brightness = unseen >= 0 ? unseen : sight.dithered ? 0.5F : 0.35F;
    constexpr float last_cell_inset = 0.001F;
    if (sight.width > 0 && sight.height > 0) {
        x = std::clamp(x, 0.0F, sight.width * edge - last_cell_inset);
        y = std::clamp(y, 0.0F, sight.height * edge - last_cell_inset);
    }
    if (fog_at(sight, static_cast<int32_t>(std::floor(x)), static_cast<int32_t>(std::floor(y))) ==
        Fog::black)
        return 0;
    const auto sample = [&](int32_t column, int32_t row) {
        if (sight.width <= 0 || sight.height <= 0)
            return 1.0F;
        column = std::clamp(column, 0, sight.width - 1);
        row = std::clamp(row, 0, sight.height - 1);
        const auto fog = fog_at(
            sight,
            static_cast<int32_t>((column + 0.5F) * edge),
            static_cast<int32_t>((row + 0.5F) * edge)
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
            if (c < 0 || z < 0 || c >= sight.width || z >= sight.height ||
                fog_at(sight, c * fog_edge, z * fog_edge) != Fog::black)
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

/// Samples the canonical world grid at a bounded world coordinate.
float height_at(const formats::tnt::Map& map, int32_t x, int32_t y) {
    const auto column = std::clamp(x / cell_edge, 0, static_cast<int32_t>(map.attribute_width) - 1);
    const auto row = std::clamp(y / cell_edge, 0, static_cast<int32_t>(map.attribute_height) - 1);
    return map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column].height;
}

/// Returns the height the simulation gives the middle of a height cell, as
/// unit_movement::Terrain::height interpolates between the cell's corners,
/// in whole steps.
///
/// @param map the map
/// @param x the cell's left edge, in map pixels
/// @param y the cell's top edge
/// @return the height
float cell_middle_height(const formats::tnt::Map& map, int32_t x, int32_t y) {
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

/// Adds a triangle fan after checking the frame's memory budget.
void polygon(Scene& scene, std::span<const Vertex> vertices) {
    if (vertices.size() < 3)
        return;
    if (scene.vertices.size() + (vertices.size() - 2) * 3 > maximum_vertices)
        throw std::runtime_error("experimental scene exceeds its geometry budget");
    for (std::size_t corner = 1; corner + 1 < vertices.size(); ++corner)
        scene.vertices.insert(
            scene.vertices.end(), {vertices[0], vertices[corner], vertices[corner + 1]}
        );
}

/// Packs palette pixels with coverage and fog, sharing identical materials.
AtlasRect material(
    Scene& scene,
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
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || pixels.size() < count ||
        (!coverage.empty() && coverage.size() < count))
        throw std::runtime_error("experimental scene received an invalid material");
    uint64_t digest = hash_start;
    const auto hash = [&](uint64_t value) { digest = (digest ^ value) * hash_multiplier; };
    hash(width);
    hash(height);
    hash(static_cast<uint64_t>(transparent + 1));
    hash(static_cast<uint64_t>(fog));
    hash(light_mask);
    hash(alpha_coverage);
    if (light_mask)
        hash(static_cast<uint64_t>(flash_strength));
    for (const auto value : pixels.first(count))
        hash(value);
    for (const auto value : coverage)
        hash(value);
    // A palette change must invalidate the cached material colours too.
    for (const auto value : palette)
        hash(value);
    if (const auto found = scene.materials.find(digest); found != scene.materials.end())
        return found->second;
    if (width > scene.texture_width || height > scene.texture_height)
        throw std::runtime_error("experimental material is larger than its atlas");
    if (scene.next_x + width > scene.texture_width) {
        scene.next_x = 0;
        scene.next_y += scene.row_height;
        scene.row_height = 0;
    }
    if (scene.next_y + height > scene.texture_height)
        throw AtlasFull();
    const AtlasRect rect{scene.next_x, scene.next_y, width, height};
    for (uint32_t y = 0; y < height; ++y) {
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
    scene.next_x += width;
    scene.row_height = std::max(scene.row_height, height);
    scene.materials.emplace(digest, rect);
    ++scene.texture_revision;
    scene.atlas_update.first_row = std::min(scene.atlas_update.first_row, rect.y);
    scene.atlas_update.end_row = std::max(scene.atlas_update.end_row, rect.y + height);
    return rect;
}

/// One point on a terrain triangle, in painted coordinates and world height.
struct GroundPoint {
    float x{};
    float y{};
    float height{};
    float brightness{1};
};

/// Clips a terrain triangle against a sprite edge without changing its plane.
std::vector<GroundPoint>
clip_ground(std::span<const GroundPoint> source, bool horizontal, float edge, bool greater) {
    std::vector<GroundPoint> result;
    if (source.empty())
        return result;
    const auto coordinate = [horizontal](const GroundPoint& p) { return horizontal ? p.x : p.y; };
    auto previous = source.back();
    bool previous_inside = greater ? coordinate(previous) >= edge : coordinate(previous) <= edge;
    for (const auto& current : source) {
        const bool inside = greater ? coordinate(current) >= edge : coordinate(current) <= edge;
        if (inside != previous_inside) {
            const float fraction =
                (edge - coordinate(previous)) / (coordinate(current) - coordinate(previous));
            result.push_back(
                {std::lerp(previous.x, current.x, fraction),
                 std::lerp(previous.y, current.y, fraction),
                 std::lerp(previous.height, current.height, fraction),
                 std::lerp(previous.brightness, current.brightness, fraction)}
            );
            // Interpolation can round just outside the clipping plane. Keep
            // its known coordinate exact so atlas UVs remain inside the tile.
            (horizontal ? result.back().x : result.back().y) = edge;
        }
        if (inside)
            result.push_back(current);
        previous = current;
        previous_inside = inside;
    }
    return result;
}

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

/// Packs colour and classic grey palettes with independently clamped mip chains.
AtlasRect terrain_material(
    Scene& scene,
    const PaletteBytes& palette,
    const PaletteBytes& grey,
    std::span<const uint8_t> pixels,
    Fog fog
) {
    constexpr uint32_t packed_edge = 64;
    if (scene.next_x + packed_edge > scene.texture_width) {
        scene.next_x = 0;
        scene.next_y += scene.row_height;
        scene.row_height = 0;
    }
    if (scene.next_y + packed_edge > scene.texture_height)
        throw AtlasFull();
    const AtlasRect rect{scene.next_x, scene.next_y, packed_edge, packed_edge};
    const auto pixel_at = [&](uint32_t x, uint32_t y) {
        return scene.texture_rgba.data() +
               (static_cast<std::size_t>(rect.y + y) * scene.texture_width + rect.x + x) * channels;
    };
    for (uint32_t variant = 0; variant < 2; ++variant) {
        const auto& colours = variant == 0 ? palette : grey;
        const uint32_t origin = variant * tile_edge;
        for (uint32_t y = 0; y < tile_edge; ++y)
            for (uint32_t x = 0; x < tile_edge; ++x) {
                auto* out = pixel_at(origin + x, y);
                const auto index =
                    static_cast<std::size_t>(pixels[y * tile_edge + x]) * palette_entry_bytes;
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    const auto value = fog == Fog::black ? uint8_t{0} : colours[index + channel];
                    out[channel] = value;
                }
                out[3] = 255;
            }
        uint32_t row = tile_edge, edge = tile_edge / 2;
        for (; edge != 0; edge /= 2) {
            for (uint32_t y = 0; y < edge; ++y)
                for (uint32_t x = 0; x < edge; ++x) {
                    auto* out = pixel_at(origin + x, row + y);
                    for (uint32_t channel = 0; channel < channels; ++channel) {
                        uint32_t sum = 0;
                        const uint32_t factor = tile_edge / edge;
                        for (uint32_t dy = 0; dy < factor; ++dy)
                            for (uint32_t dx = 0; dx < factor; ++dx)
                                sum += pixel_at(origin + x * factor + dx, y * factor + dy)[channel];
                        const uint32_t count = factor * factor;
                        out[channel] = static_cast<uint8_t>((sum + count / 2) / count);
                    }
                }
            row += edge;
        }
    }
    scene.next_x += packed_edge;
    scene.row_height = std::max(scene.row_height, packed_edge);
    ++scene.texture_revision;
    scene.atlas_update.first_row = std::min(scene.atlas_update.first_row, rect.y);
    scene.atlas_update.end_row = std::max(scene.atlas_update.end_row, rect.y + packed_edge);
    return rect;
}

/// Emits terrain cells with tile UVs and a separate fog material per cell.
void terrain(const Inputs& in, Scene& scene) {
    const auto& map = *in.map;
    const int32_t left =
        std::clamp(in.left, 0, static_cast<int32_t>(map.attribute_width) * cell_edge) / cell_edge *
        cell_edge;
    const int32_t top =
        std::clamp(in.top, 0, static_cast<int32_t>(map.attribute_height) * cell_edge) / cell_edge *
        cell_edge;
    const int32_t right =
        std::clamp(in.right, 0, static_cast<int32_t>(map.attribute_width) * cell_edge);
    const int32_t bottom = std::clamp(
        in.bottom + maximum_terrain_row_shift,
        0,
        static_cast<int32_t>(map.attribute_height) * cell_edge
    );
    // Compare palette bytes once per pass and tile bytes once per visible key.
    // The retained copies detect edits even when the same map object is reused.
    if (scene.terrain_palette != *in.palette) {
        scene.terrain_materials.clear();
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
    constexpr std::size_t maximum_cached_tiles = 8192;
    // A tile's texels are reused by all four height cells and every repeated tile.
    std::unordered_map<uint64_t, AtlasRect> tiles;
    const auto tile_material = [&](int32_t tile_x, int32_t tile_y) {
        const auto tile = map.tile_indices.at(
            static_cast<std::size_t>(tile_y / tile_edge) * map.tile_width + tile_x / tile_edge
        );
        const auto fog = fog_at(in.sight, tile_x, tile_y) == Fog::black ? Fog::black : Fog::seen;
        const uint64_t key = static_cast<uint64_t>(tile) * 3 + static_cast<uint64_t>(fog);
        auto found = tiles.find(key);
        if (found == tiles.end()) {
            const auto start = static_cast<std::size_t>(tile) * formats::tnt::layout::tile_bytes;
            if (start + formats::tnt::layout::tile_bytes > map.tile_palette_indices.size())
                throw std::runtime_error("experimental terrain tile is out of bounds");
            const auto pixels = std::span(map.tile_palette_indices)
                                    .subspan(start, formats::tnt::layout::tile_bytes);
            auto cached = scene.terrain_materials.find(key);
            if (cached == scene.terrain_materials.end() ||
                std::memcmp(cached->second.pixels.data(), pixels.data(), pixels.size()) != 0) {
                const auto rectangle =
                    terrain_material(scene, *in.palette, scene.terrain_grey_palette, pixels, fog);
                if (scene.terrain_materials.size() >= maximum_cached_tiles)
                    scene.terrain_materials.clear();
                scene.aircraft_shadow_materials.clear();
                auto& entry = scene.terrain_materials[key];
                std::copy(pixels.begin(), pixels.end(), entry.pixels.begin());
                entry.rectangle = rectangle;
                cached = scene.terrain_materials.find(key);
            }
            found = tiles.emplace(key, cached->second.rectangle).first;
        }
        return found->second;
    };
    // Geometry follows world X/Z. Artwork is indexed by its classic projected
    // position and can cross several source tiles on one steep terrain face.
    const auto emit = [&](std::span<const Vertex> source) {
        for (std::size_t corner = 1; corner + 1 < source.size(); ++corner) {
            const std::array vertices{source[0], source[corner], source[corner + 1]};
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
                static_cast<int32_t>(map.tile_width) * tile_edge,
                static_cast<int32_t>(std::ceil(max_x / tile_edge)) * tile_edge
            );
            const int32_t last_y = std::min(
                static_cast<int32_t>(map.tile_height) * tile_edge,
                std::max(
                    first_y + tile_edge,
                    static_cast<int32_t>(std::ceil(max_y / tile_edge)) * tile_edge
                )
            );
            const auto emit_tile = [&](const GroundPolygon& clipped, int32_t tx, int32_t ty) {
                if (clipped.count < 3)
                    return;
                const auto rect = tile_material(tx, ty);
                std::array<Vertex, 8> output{};
                for (std::size_t index = 0; index < clipped.count; ++index) {
                    const auto& point = clipped.points[index];
                    output[index] = {
                        point.x,
                        point.height,
                        point.y + point.height * height_lift,
                        (rect.x + point.x - tx) / scene.texture_width,
                        (rect.y + point.y - ty) / scene.texture_height,
                        point.brightness
                    };
                    auto& vertex = output[index];
                    vertex.terrain_x = static_cast<float>(rect.x);
                    vertex.terrain_y = static_cast<float>(rect.y);
                    vertex.terrain_size = tile_edge;
                    vertex.terrain_known = point.brightness >= 1 ? 1
                                           : point.brightness <= 0
                                               ? 0
                                               : brightness_at(in.sight, point.x, point.y, 1);
                    const float grey_brightness = in.sight.dithered ? 0.5F : 0.35F;
                    vertex.terrain_grey =
                        vertex.terrain_known > 0
                            ? std::clamp(
                                  (vertex.terrain_known - point.brightness) /
                                      (vertex.terrain_known * (1 - grey_brightness)),
                                  0.0F,
                                  1.0F
                              )
                            : 0;
                }
                for (std::size_t index = 1; index + 1 < clipped.count; ++index) {
                    const auto& a = output[0];
                    const auto& b = output[index];
                    const auto& c = output[index + 1];
                    const float area = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
                    if (std::abs(area) > 0.000001F)
                        polygon(scene, std::array{a, b, c});
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
    const int32_t columns = (right - left + cell_edge - 1) / cell_edge + 1;
    const int32_t rows = (bottom - top + cell_edge - 1) / cell_edge + 1;
    const auto count = static_cast<std::size_t>(columns) * rows;
    std::vector<std::array<float, 2>> corner_samples;
    if (count <= maximum_vertices)
        corner_samples.assign(count, {0, -1});
    const auto sample = [&](int32_t x, int32_t y) {
        const auto index =
            static_cast<std::size_t>((y - top) / cell_edge) * columns + (x - left) / cell_edge;
        if (!corner_samples.empty() && corner_samples[index][1] >= 0)
            return corner_samples[index];
        const float height = height_at(map, x, y);
        const std::array result{
            height, brightness_at(in.sight, float(x), y - height * height_lift)
        };
        if (!corner_samples.empty())
            corner_samples[index] = result;
        return result;
    };
    const bool distant =
        std::isfinite(in.detail_scale) && in.detail_scale > 0 && in.detail_scale < 1;
    std::vector<bool> merged_columns(static_cast<std::size_t>(columns));
    for (int32_t y = top; y < bottom; y += cell_edge) {
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
                const float h = cell_middle_height(map, x, y);
                const float middle_x = static_cast<float>(x + cell_edge / 2);
                const float middle_y = static_cast<float>(y + cell_edge / 2);
                middle = Vertex{
                    middle_x,
                    h,
                    middle_y,
                    0,
                    0,
                    brightness_at(in.sight, middle_x, middle_y - h * height_lift)
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
            const int32_t divisions = distant && in.detail_scale <= 0.25F  ? 1
                                      : distant && in.detail_scale <= 0.5F ? 2
                                                                           : maximum_divisions;
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
                        brightness_at(in.sight, result.x, result.z - result.height * height_lift);
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
                        const std::array first{
                            point(column, row), point(column + 1, row), point(column, row + 1)
                        };
                        emit(first);
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
    scene.terrain_vertex_count = scene.vertices.size();
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
    Scene& scene,
    const AtlasRect& rect,
    float left,
    float top,
    float opacity,
    bool shadow,
    const FixedVec3* anchor = nullptr,
    float footprint_scale = 1,
    bool water_receiver = false,
    bool soft_edges = false
) {
    constexpr float surface_lift = 0.05F;
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
                std::array<Vertex, 8> corners{};
                for (std::size_t corner = 0; corner < clipped.count; ++corner) {
                    const auto& p = clipped.points[corner];
                    const float h = p.height + surface_lift;
                    corners[corner] = {
                        p.x,
                        h,
                        p.y + (anchor ? anchor_lift : p.height * height_lift),
                        (rect.x + (p.x - left) / footprint_scale) / scene.texture_width,
                        (rect.y + (p.y - top) / footprint_scale) / scene.texture_height,
                        shadow ? 1.0F
                               : brightness_at(
                                     in.sight,
                                     p.x,
                                     p.y + (anchor ? anchor_lift - p.height * height_lift : 0)
                                 ),
                        opacity
                    };
                    if (soft_edges)
                        corners[corner].terrain_size = -1;
                    move_with_swells(in, corners[corner]);
                }
                polygon(scene, std::span(corners).first(clipped.count));
            }
        }
}

/// Restores the prepared image's real silhouette without synthesising a shape.
void unit_shadows(const Inputs& in, Scene& scene) {
    if ((in.graphics_flags & model::graphics_shadows) == 0 || in.draws->shadow_level == 0)
        return;
    const float opacity = 0.5F * std::min(in.draws->shadow_level, model::shadow_full_level) /
                          model::shadow_full_level;
    for (const auto& draw : in.draws->models) {
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
        std::vector<uint8_t> pixels(count, image.key);
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
        std::vector<uint8_t> coverage(count);
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
        const bool aircraft = (flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0 && altitude > 0;
        if (aircraft && image.width <= 256 && image.height <= 256) {
            const uint32_t radius = renderer_aircraft_shadow::edge_radius(altitude);
            uint64_t key = hash_start;
            const auto hash = [&](uint64_t value) { key = (key ^ value) * hash_multiplier; };
            hash(image.width);
            hash(image.height);
            hash(radius);
            for (const auto value : coverage)
                hash(value);
            auto cached = scene.aircraft_shadow_materials.find(key);
            if (cached == scene.aircraft_shadow_materials.end()) {
                auto alpha =
                    renderer_aircraft_shadow::soften(coverage, image.width, image.height, radius);
                const uint32_t width = image.width + radius * 4;
                const uint32_t height = image.height + radius * 4;
                std::vector<uint8_t> black(alpha.size());
                const auto rectangle = material(
                    scene,
                    *in.palette,
                    width,
                    height,
                    black,
                    alpha,
                    -1,
                    Fog::black,
                    false,
                    FlashStrength::full,
                    true
                );
                if (scene.aircraft_shadow_materials.size() >= 256)
                    scene.aircraft_shadow_materials.clear();
                cached = scene.aircraft_shadow_materials.emplace(key, rectangle).first;
            }
            const float padding = static_cast<float>(radius) * 2.0F;
            const float left =
                position.x / fixed_scale + model::shadow_offset_x - (image.origin_x + padding);
            const float top =
                position.z / fixed_scale - receiver * height_lift - (image.origin_y + padding);
            decal(in, scene, cached->second, left, top, opacity, true, nullptr, 1, true, true);
        } else {
            const auto rect = material(
                scene, *in.palette, image.width, image.height, pixels, coverage, -1, Fog::black
            );
            decal(in, scene, rect, shadow_x, shadow_y, opacity, true);
        }
        ++scene.shadow_count;
    }
}

/// Splits ground geometry at a world map coordinate, retaining its material and fog.
std::vector<Vertex>
clip_foundation(std::span<const Vertex> source, bool horizontal, float edge, bool greater) {
    std::vector<Vertex> result;
    if (source.empty())
        return result;
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
                 previous.terrain_x,
                 previous.terrain_y,
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
    return result;
}

/// Levels only drawn ground structures' foundations, without changing map or unit records.
/// Ground decals share the split geometry and keep their small lift above the terrain.
void foundations(const Inputs& in, Scene& scene) {
    constexpr float blend_width = cell_edge;
    constexpr float ground_separation = 0.1F;
    constexpr float decal_lift = 0.05F;

    struct Foundation {
        float left{}, right{}, top{}, bottom{}, height{};
    };

    std::vector<Foundation> plots;
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
        const float left = centre_x - half_width;
        const float right = centre_x + half_width;
        const float top = centre_y - half_depth;
        const float bottom = centre_y + half_depth;
        plots.push_back({left, right, top, bottom, height});
    }
    if (plots.empty())
        return;
    std::sort(plots.begin(), plots.end(), [](const Foundation& a, const Foundation& b) {
        return std::array{a.left, a.right, a.top, a.bottom, a.height} <
               std::array{b.left, b.right, b.top, b.bottom, b.height};
    });
    // Index expanded footprints so each ground triangle visits only nearby
    // structures. Very large plots retain a bounded global fallback.
    constexpr float bin_edge = 128;
    constexpr std::size_t maximum_plot_bins = 64;
    const auto bin = [](float coordinate) {
        return static_cast<int32_t>(std::floor(coordinate / bin_edge));
    };
    const auto key = [](int32_t x, int32_t y) {
        return (uint64_t{static_cast<uint32_t>(y)} << 32) | static_cast<uint32_t>(x);
    };
    std::unordered_map<uint64_t, std::vector<std::size_t>> bins;
    std::vector<std::size_t> global;
    std::size_t references{};
    for (std::size_t index = 0; index < plots.size(); ++index) {
        const auto& plot = plots[index];
        const int32_t x0 = bin(plot.left - blend_width), x1 = bin(plot.right + blend_width);
        const int32_t y0 = bin(plot.top - blend_width), y1 = bin(plot.bottom + blend_width);
        const auto count = static_cast<uint64_t>(int64_t{x1} - x0 + 1) *
                           static_cast<uint64_t>(int64_t{y1} - y0 + 1);
        if (count > maximum_plot_bins || count + references > maximum_vertices) {
            global.push_back(index);
            continue;
        }
        references += static_cast<std::size_t>(count);
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x)
                bins[key(x, y)].push_back(index);
    }
    std::vector<std::size_t> indices;
    const auto nearby =
        [&](float min_x, float max_x, float min_y, float max_y) -> std::span<const std::size_t> {
        const int32_t x0 = bin(min_x), x1 = bin(max_x), y0 = bin(min_y), y1 = bin(max_y);
        // Most triangles occupy one bin. Its entries already follow plot order.
        if (x0 == x1 && y0 == y1 && global.empty()) {
            if (const auto found = bins.find(key(x0, y0)); found != bins.end())
                return found->second;
            return {};
        }
        indices.assign(global.begin(), global.end());
        const auto count = static_cast<uint64_t>(int64_t{x1} - x0 + 1) *
                           static_cast<uint64_t>(int64_t{y1} - y0 + 1);
        if (count > maximum_plot_bins) {
            indices.resize(plots.size());
            for (std::size_t i = 0; i < plots.size(); ++i)
                indices[i] = i;
            return indices;
        }
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x)
                if (const auto found = bins.find(key(x, y)); found != bins.end())
                    indices.insert(indices.end(), found->second.begin(), found->second.end());
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    };
    float plot_left = plots[0].left - blend_width, plot_right = plots[0].right + blend_width;
    float plot_top = plots[0].top - blend_width, plot_bottom = plots[0].bottom + blend_width;
    for (const auto& plot : plots) {
        plot_left = std::min(plot_left, plot.left - blend_width);
        plot_right = std::max(plot_right, plot.right + blend_width);
        plot_top = std::min(plot_top, plot.top - blend_width);
        plot_bottom = std::max(plot_bottom, plot.bottom + blend_width);
    }
    std::vector<std::array<std::size_t, 2>> affected_ranges;
    scene.foundation_vertices.swap(scene.vertices);
    const auto& previous = scene.foundation_vertices;
    scene.vertices.clear();
    scene.vertices.reserve(previous.size());
    const auto terrain_end = scene.terrain_vertex_count;
    std::size_t pending_begin{};
    // Most geometry lies outside foundations. Copy each untouched run once,
    // retaining the precise terrain/decal boundary even when the run crosses it.
    const auto copy_pending = [&](std::size_t end) {
        const auto count = end - pending_begin;
        if (scene.vertices.size() + count > maximum_vertices)
            throw std::runtime_error("experimental scene exceeds its geometry budget");
        if (pending_begin < terrain_end && end >= terrain_end)
            scene.terrain_vertex_count = scene.vertices.size() + terrain_end - pending_begin;
        scene.vertices.insert(
            scene.vertices.end(), previous.begin() + pending_begin, previous.begin() + end
        );
        pending_begin = end;
    };
    for (std::size_t first = 0; first + 2 < previous.size(); first += 3) {
        const auto triangle = std::span(previous).subspan(first, 3);
        float min_x = triangle[0].x, max_x = min_x;
        float min_y = triangle[0].z, max_y = min_y;
        for (const auto& vertex : triangle) {
            min_x = std::min(min_x, vertex.x);
            max_x = std::max(max_x, vertex.x);
            const float y = vertex.z;
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
        if (max_x <= plot_left || min_x >= plot_right || max_y <= plot_top ||
            min_y >= plot_bottom) {
            continue;
        }
        std::vector<std::vector<Vertex>> pieces;
        bool affected{};
        for (const auto plot_index : nearby(min_x, max_x, min_y, max_y)) {
            const auto& [left, right, top, bottom, height] = plots[plot_index];
            if (max_x <= left - blend_width || min_x >= right + blend_width ||
                max_y <= top - blend_width || min_y >= bottom + blend_width)
                continue;
            affected = true;
            if (pieces.empty())
                pieces.emplace_back(triangle.begin(), triangle.end());
            for (const bool horizontal : {true, false}) {
                const float low = horizontal ? left : top;
                const float high = horizontal ? right : bottom;
                for (const float edge : {low - blend_width, low, high, high + blend_width}) {
                    std::vector<std::vector<Vertex>> split;
                    for (auto& piece : pieces) {
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
                                split.push_back(std::move(piece));
                            continue;
                        }
                        auto before = clip_foundation(piece, horizontal, edge, false);
                        auto after = clip_foundation(piece, horizontal, edge, true);
                        if (before.size() >= 3)
                            split.push_back(std::move(before));
                        if (after.size() >= 3)
                            split.push_back(std::move(after));
                    }
                    pieces = std::move(split);
                }
            }
        }
        if (!affected)
            continue;
        copy_pending(first);
        const auto output_begin = scene.vertices.size();
        if (pieces.empty())
            polygon(scene, triangle);
        else
            for (const auto& piece : pieces)
                polygon(scene, piece);
        if (affected) {
            if (!affected_ranges.empty() && affected_ranges.back()[1] == output_begin)
                affected_ranges.back()[1] = scene.vertices.size();
            else
                affected_ranges.push_back({output_begin, scene.vertices.size()});
        }
        pending_begin = first + 3;
        if (pending_begin == terrain_end)
            scene.terrain_vertex_count = scene.vertices.size();
    }
    copy_pending(previous.size());
    // Blend once from the original terrain. An interior always wins over a
    // neighbouring skirt; tied skirts choose a stable height, not draw order.
    std::span<const std::size_t> candidates;
    for (const auto& range : affected_ranges)
        for (std::size_t index = range[0]; index < range[1]; ++index) {
            if (index % 3 == 0) {
                const auto& a = scene.vertices[index];
                const auto& b = scene.vertices[index + 1];
                const auto& c = scene.vertices[index + 2];
                const float ay = a.z;
                const float by = b.z;
                const float cy = c.z;
                candidates = nearby(
                    std::min({a.x, b.x, c.x}),
                    std::max({a.x, b.x, c.x}),
                    std::min({ay, by, cy}),
                    std::max({ay, by, cy})
                );
            }
            if (candidates.empty())
                continue;
            auto& vertex = scene.vertices[index];
            const float y = vertex.z;
            float strongest{}, target{};
            const std::size_t first = index / 3 * 3;
            float centre_x{}, centre_y{};
            for (std::size_t corner = first; corner < first + 3; ++corner) {
                centre_x += scene.vertices[corner].x / 3;
                centre_y += scene.vertices[corner].z / 3;
            }
            for (const auto plot_index : candidates) {
                const auto& plot = plots[plot_index];
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
            if (index >= scene.terrain_vertex_count)
                target += decal_lift;
            vertex.height = std::lerp(vertex.height, target, strongest);
        }
}

/// Rejects the back of a model face, including opposite faces sharing one plane.
bool back_facing(std::span<const Vertex> corners, const std::array<float, 3>& view) {
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

/// Draws each source polygon edge as a narrow camera-facing nanoframe ribbon.
void nanoframe(
    const Inputs& in,
    Scene& scene,
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
        cached = material(scene, palette, 1, 1, pixel, {}, -1, Fog::seen);
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
            result.u = (rect.x + 0.5F) / scene.texture_width;
            result.v = (rect.y + 0.5F) / scene.texture_height;
            result.opacity = std::min(1.0F, remaining * 4);
            result.red = result.green = result.blue = 1;
            return result;
        };
        const std::array ribbon{point(a, -1), point(b, -1), point(b, 1), point(a, 1)};
        polygon(scene, ribbon);
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
void wet_polygon(Scene& scene, const std::array<Vertex, 3>& triangle) {
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
    polygon(scene, {clipped.data(), count});
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
    Scene& scene,
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
        reflection_triangle(in, scene, first, depth + 1, remaining);
        reflection_triangle(in, scene, second, depth + 1, remaining);
        return;
    }
    for (auto& vertex : triangle) {
        vertex.terrain_x = water_depth(in, vertex.x, vertex.z);
        vertex.brightness *=
            brightness_at(in.sight, vertex.x, vertex.z - in.sea_level * height_lift);
    }
    const auto begin = scene.vertices.size();
    wet_polygon(scene, triangle);
    remaining -= scene.vertices.size() - begin;
}

/// Mirrors visible floating hulls onto water without a second scene render pass.
void naval_reflection(
    const Inputs& in, Scene& scene, std::span<const Vertex> source, std::size_t& budget
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
                in, scene, {clipped[0], clipped[corner], clipped[corner + 1]}, 0, budget
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

/// Uses the actual animated, rotated model vertices prepared for this frame.
void models(const Inputs& in, Scene& scene) {
    std::size_t reflection_budget = 24576;
    std::array<std::optional<AtlasRect>, 256> colors;
    std::unordered_map<const Sprite*, AtlasRect> textures;
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
    for (const auto& draw : in.draws->models) {
        if (!draw.plan.linked.drawn)
            continue;
        const std::size_t first_vertex = scene.vertices.size();
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
                            material(scene, white_palette, 1, 1, pixel, {}, -1, Fog::seen);
                    }
                    rect = *white_material;
                } else if (colored) {
                    auto& cached = colors[primitive.color];
                    if (!cached) {
                        const std::array pixel{primitive.color};
                        cached = material(scene, *in.palette, 1, 1, pixel, {}, -1, Fog::seen);
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
                                        scene,
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
                             scene.texture_width,
                         (rect.y + 0.5F + (colored ? 0 : v[index] * (rect.height - 1))) /
                             scene.texture_height,
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
                    constexpr std::size_t light_samples = 16;
                    constexpr float light_radius = 32.0F;
                    const auto& particles = in.draws->spatial_particles;
                    const auto stride = std::max<std::size_t>(1, particles.size() / light_samples);
                    for (std::size_t sample = 0, count = 0;
                         in.nanolathe_lighting && sample < particles.size() &&
                         count < light_samples;
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
                        const float dx = vertex.x - particle.position.x / fixed_scale;
                        const float dy = vertex.height - particle.position.y / fixed_scale;
                        const float dz = vertex.z - particle.position.z / fixed_scale;
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
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
                    polygon(scene, corners);
                if (remaining > 0)
                    nanoframe(in, scene, corners, remaining, frame_material);
            }
        }
        ++scene.model_count;
        if (translucent && scene.vertices.size() > first_vertex) {
            // Held back, to be drawn over the opaque scene.
            const std::size_t held = scene.translucent_vertices.size();
            const auto first = scene.vertices.begin() + static_cast<std::ptrdiff_t>(first_vertex);
            scene.translucent_vertices.insert(
                scene.translucent_vertices.end(), first, scene.vertices.end()
            );
            scene.vertices.erase(first, scene.vertices.end());
            if (!draw.on_map && unit->id != 0)
                scene.translucent_ranges.push_back(
                    {unit->id, held, scene.translucent_vertices.size() - held}
                );
        } else if (!draw.on_map && unit->id != 0 && scene.vertices.size() > first_vertex)
            scene.unit_ranges.push_back(
                {unit->id, first_vertex, scene.vertices.size() - first_vertex}
            );
        if (!reflected_faces.empty())
            naval_reflection(in, scene, reflected_faces, reflection_budget);
    }
}

/// Draws canonical 3DO shots and their child pieces at their presented simulation depth.
void projectiles(const Inputs& in, Scene& scene) {
    std::array<std::optional<AtlasRect>, 256> colors;
    std::unordered_map<const Sprite*, AtlasRect> textures;
    for (const auto& shot : in.draws->projectiles) {
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
                        cached = material(scene, *in.palette, 1, 1, pixel, {}, -1, Fog::seen);
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
                                        scene,
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
                             scene.texture_width,
                         (rect.y + 0.5F + (colored ? 0 : v[index] * (rect.height - 1))) /
                             scene.texture_height,
                         brightness_at(in.sight, x, z - height * height_lift)}
                    );
                }
                // Small weapon faces are two-sided in the classic projectile pass.
                polygon(scene, corners);
            }
        };
        object(shot.object, shot.prepared, shot.rotation);
        object(shot.child, shot.child_prepared, shot.child_rotation);
    }
}

/// Draws the actual sight-gated particle stream at its interpolated world height.
void particles(const Inputs& in, Scene& scene) {
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
    for (const auto& particle : in.draws->spatial_particles) {
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
            found = colors.emplace(color, material(scene, palette, 1, 1, pixel, {}, -1, Fog::seen))
                        .first;
        }
        const auto rect = found->second;
        const float u = (rect.x + 0.5F) / scene.texture_width;
        const float v = (rect.y + 0.5F) / scene.texture_height;
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
            if (scene.vertices.size() + triangles.size() > maximum_vertices)
                throw std::runtime_error("experimental particle geometry exceeds its limit");
            scene.vertices.insert(scene.vertices.end(), triangles.begin(), triangles.end());
        };
        if (core_opacity > 0)
            shell(1.0F, core_opacity);
        if (glowing && halo_opacity > 0)
            shell(3.0F, 0.16F * halo_opacity * intensity);
        if (particle.emissive)
            ++scene.nano_particle_count;
        ++scene.particle_count;
    }
}

/// Finds the rendered ground, including leveled foundations, below a world point.
std::optional<float> rendered_ground(const Scene& scene, float x, float z) {
    std::optional<float> result;
    for (std::size_t i = 0; i + 2 < scene.terrain_vertex_count; i += 3) {
        const auto& a = scene.vertices[i];
        const auto& b = scene.vertices[i + 1];
        const auto& c = scene.vertices[i + 2];
        if (x < std::min({a.x, b.x, c.x}) || x > std::max({a.x, b.x, c.x}) ||
            z < std::min({a.z, b.z, c.z}) || z > std::max({a.z, b.z, c.z}))
            continue;
        const float determinant = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(determinant) < 0.000001F)
            continue;
        const float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / determinant;
        const float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / determinant;
        constexpr float edge_tolerance = 0.00001F;
        if (u < -edge_tolerance || v < -edge_tolerance || u + v > 1 + edge_tolerance)
            continue;
        const float height = u * a.height + v * b.height + (1 - u - v) * c.height;
        if (!result || height > *result)
            result = height;
    }
    return result;
}

/// Keeps a near-ground explosion's covered card above every intersected terrain plane.
/// The translation leaves its UVs and shape intact; airborne effects retain their altitude.
float surface_effect_lift(
    const Scene& scene,
    const formats::gaf::RenderedFrame& frame,
    float x,
    float height,
    float z,
    const std::array<float, 3>& right,
    const std::array<float, 3>& up
) {
    constexpr float ground_near_distance = 4;
    constexpr float clearance = 0.1F;
    const auto ground = rendered_ground(scene, x, z);
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
    const float determinant = right[0] * up[2] - right[2] * up[0];
    if (std::abs(determinant) < 0.000001F)
        return 0;
    float lift{};
    for (std::size_t i = 0; i + 2 < scene.terrain_vertex_count; i += 3) {
        std::array<GroundPoint, 3> local{};
        for (std::size_t corner = 0; corner < local.size(); ++corner) {
            const auto& vertex = scene.vertices[i + corner];
            const float dx = vertex.x - x;
            const float dz = vertex.z - z;
            local[corner] = {
                (dx * up[2] - dz * up[0]) / determinant,
                (right[0] * dz - right[2] * dx) / determinant,
                vertex.height
            };
        }
        if (std::max({local[0].x, local[1].x, local[2].x}) < min_across ||
            std::min({local[0].x, local[1].x, local[2].x}) > max_across ||
            std::max({local[0].y, local[1].y, local[2].y}) < min_above ||
            std::min({local[0].y, local[1].y, local[2].y}) > max_above)
            continue;
        auto clipped = clip_ground(local, true, min_across, true);
        clipped = clip_ground(clipped, true, max_across, false);
        clipped = clip_ground(clipped, false, min_above, true);
        clipped = clip_ground(clipped, false, max_above, false);
        // Both surfaces are planar over each clipped polygon, so its corners
        // include the greatest clearance needed, even for an interior crest.
        for (const auto& point : clipped)
            lift = std::max(lift, point.height + clearance - (height + up[1] * point.y));
    }
    return lift;
}

/// Lifts sprites into upright cutouts preserving their classic pixel placement.
void sprites(const Inputs& in, Scene& scene, bool ground_pass) {
    // Frame pixels and palette are immutable during this pass; the cache never
    // survives a build, so animated or recoloured frames are hashed again.
    std::unordered_map<const formats::gaf::RenderedFrame*, std::array<std::optional<AtlasRect>, 4>>
        prepared;
    std::vector<bool> light_masks(in.draws->sprites.size());
    for (const auto& draw : in.draws->draws)
        if (draw.kind == WorldDrawKind::lit_sprite && draw.index < light_masks.size())
            light_masks[draw.index] = true;
    for (std::size_t index = 0; index < in.draws->sprites.size(); ++index) {
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
                scene,
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
        if (ground_pass) {
            if (sprite.shadow && in.draws->shadow_level == 0)
                continue;
            const float opacity =
                sprite.shadow ? 0.5F * std::min(in.draws->shadow_level, model::shadow_full_level) /
                                    model::shadow_full_level
                              : 1.0F;
            decal(
                in,
                scene,
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
                sprite.world_position ? &*sprite.world_position : nullptr
            );
            ++scene.sprite_count;
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
                ? surface_effect_lift(scene, frame, origin_x, origin_h, origin_z, right, up)
                : 0;
        const auto vertex = [&](float dx, float dy) {
            const float across = (dx - frame.origin_x) * sprite.effect_scale;
            const float above = (frame.origin_y - dy) * sprite.effect_scale;
            const bool facing = sprite.world_position.has_value() && !sprite.on_map;
            Vertex result{
                origin_x + (facing ? right[0] * across + up[0] * above : across),
                origin_h + surface_lift + (facing ? up[1] * above : above / height_lift),
                origin_z + (facing ? right[2] * across + up[2] * above : 0),
                (rect.x + dx) / scene.texture_width,
                (rect.y + dy) / scene.texture_height,
                brightness_at(in.sight, static_cast<float>(x), static_cast<float>(y)),
                sprite.effect_opacity
            };
            if (sprite.soft_effect || submerged_effect) {
                result.u =
                    (rect.x + std::clamp(dx, 0.5F, float(rect.width) - 0.5F)) / scene.texture_width;
                result.v = (rect.y + std::clamp(dy, 0.5F, float(rect.height) - 0.5F)) /
                           scene.texture_height;
                result.terrain_size = -1;
            }
            if (submerged_effect) {
                result.terrain_size = -5;
                result.terrain_x = in.sea_level - origin_h;
            } else if (!facing) {
                move_with_swells(in, result);
            }
            return result;
        };
        const std::array corners{
            vertex(0, 0),
            vertex(frame.width, 0),
            vertex(frame.width, frame.height),
            vertex(0, frame.height)
        };
        polygon(scene, corners);
        ++scene.sprite_count;
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
} // namespace

bool build(const Inputs& inputs, Scene& scene, std::string& error) {
    scene.atlas_update = {scene.texture_revision, scene.texture_height, 0};
    try {
        if (inputs.map == nullptr || inputs.palette == nullptr || inputs.draws == nullptr ||
            inputs.map->attribute_width == 0 || inputs.map->attribute_height == 0)
            throw std::runtime_error("experimental renderer has no prepared match scene");
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
        // Retry once with only this frame's materials, growing to the bounded
        // maximum when the smaller atlas has filled. Old map and animation
        // variants therefore cannot exhaust the cache permanently.
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            scene.vertices.clear();
            scene.unit_ranges.clear();
            scene.translucent_vertices.clear();
            scene.translucent_ranges.clear();
            scene.shadow_count = 0;
            scene.model_count = 0;
            scene.sprite_count = 0;
            scene.particle_count = 0;
            scene.nano_particle_count = 0;
            scene.texture_rgba.resize(
                static_cast<std::size_t>(scene.texture_width) * scene.texture_height * channels
            );
            try {
                terrain(inputs, scene);
                sprites(inputs, scene, true);
                unit_shadows(inputs, scene);
                foundations(inputs, scene);
                models(inputs, scene);
                projectiles(inputs, scene);
                sprites(inputs, scene, false);
                if (!inputs.draws->spatial_particles.empty())
                    particles(inputs, scene);
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
                break;
            } catch (const AtlasFull&) {
                if (attempt != 0)
                    throw;
                scene.texture_width = maximum_atlas_edge;
                scene.texture_height = maximum_atlas_edge;
                // Old atlas coordinates are discarded, including when already
                // at maximum size. Preserve the original base revision while
                // replacing every row of the texture on the next submission.
                scene.atlas_update.first_row = 0;
                scene.atlas_update.end_row = scene.texture_height;
                scene.materials.clear();
                scene.terrain_materials.clear();
                scene.aircraft_shadow_materials.clear();
                scene.next_x = 0;
                scene.next_y = 0;
                scene.row_height = 0;
                ++scene.texture_revision;
            }
        }
        if (scene.atlas_update.first_row > scene.atlas_update.end_row) {
            scene.atlas_update.first_row = 0;
            scene.atlas_update.end_row = 0;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        scene.vertices.clear();
        scene.unit_ranges.clear();
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
