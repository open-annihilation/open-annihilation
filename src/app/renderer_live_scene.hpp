// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "renderer_live_gpu.hpp"
#include "renderer_water.hpp"
#include "runtime_full.hpp"
#include "oa/formats/tnt.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::app::renderer_live_scene {

/// A material rectangle in the experimental renderer's bounded atlas.
struct AtlasRect {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};

    [[nodiscard]] bool operator==(const AtlasRect&) const = default;
};

/// Exact source bytes backing a reusable terrain atlas rectangle.
struct TerrainMaterial {
    std::array<uint8_t, formats::tnt::layout::tile_bytes> pixels{};
    AtlasRect rectangle{};
};

/// What a frame's ground is drawn from, beside the tiles' art and their
/// squares in the atlas: the map's heights and tiles, the crop, the
/// picture, the detail and the fog. Two frames drawn from the same, with
/// the same squares, draw the same ground.
struct GroundSource {
    const formats::tnt::Map* map{};
    uint32_t map_width{};  ///< height cells across
    uint32_t map_height{}; ///< height cells down
    uint32_t tile_width{}; ///< tiles across
    std::vector<uint8_t> heights{};
    std::vector<uint16_t> tile_indices{};
    /// The painted rectangle drawn, in map pixels.
    std::array<int32_t, 4> crop{};
    int32_t step{}; ///< height cells a side of each square of ground drawn
    bool distant{}; ///< planar tiles are drawn whole
    int32_t fog_divisions{};
    std::array<float, 12> clip_projection{};
    uint32_t clip_width{};
    uint32_t clip_height{};
    uint32_t texture_width{};
    uint32_t texture_height{};
    /// What the fog shows of each sight cell: in sight, out of sight or
    /// never mapped, the ground's whole dependence on the sight.
    std::vector<uint8_t> fog{};
    int32_t sight_width{};
    int32_t sight_height{};
    bool mapping{}; ///< ground beyond the sight cells is never mapped
    bool dithered{};

    [[nodiscard]] bool operator==(const GroundSource&) const = default;
};

/// Emitted triangle range belonging to a visible canonical unit.
struct UnitRange {
    uint16_t unit_id{};
    std::size_t first_vertex{};
    std::size_t vertex_count{};
};

/// Presentation geometry and cached pixels; no simulation state is retained.
struct Scene {
    std::vector<renderer_live_gpu::Vertex> vertices{};
    std::vector<UnitRange> unit_ranges{};
    std::vector<uint8_t> texture_rgba{};
    uint32_t texture_width{2048};
    uint32_t texture_height{2048};
    uint64_t texture_revision{};
    renderer_live_gpu::AtlasUpdate atlas_update{};
    uint32_t next_x{};
    uint32_t next_y{};
    uint32_t row_height{};
    std::unordered_map<uint64_t, AtlasRect> materials{};
    std::unordered_map<uint64_t, TerrainMaterial> terrain_materials{};
    /// Unused quarters of the atlas squares split for smaller terrain
    /// materials: the 32, 16 and 8 texel squares, in that order.
    std::array<std::vector<AtlasRect>, 3> spare_terrain_squares{};
    /// The atlas area of the materials other than terrain, all packed into an
    /// emptied atlas by the last frame that started from one; the terrain
    /// leaves them this much room and more.
    uint64_t material_area{};
    /// The area the frame being built has packed for materials other than
    /// terrain.
    uint64_t packed_material_area{};
    /// The most squares of ground a frame draws before it draws them from
    /// fewer height samples: lowered when a frame's triangles do not fit.
    uint64_t terrain_square_limit{400'000};
    /// The last frame could not be built for lack of room: its materials
    /// would not fit the atlas at its largest, or its triangles the geometry
    /// budget. Any other failure leaves it false.
    bool out_of_room{};
    std::unordered_map<uint64_t, AtlasRect>
        aircraft_shadow_materials{}; ///< bounded blurred silhouettes
    PaletteBytes terrain_palette{};
    PaletteBytes terrain_grey_palette{};
    std::size_t terrain_vertex_count{};
    /// What the last frame's ground was drawn from.
    GroundSource ground_source{};
    /// The ground of the last frame drawn from the same as the frame before
    /// it, while ground_kept: a frame drawn from the same again, whose tiles
    /// keep their squares, takes it rather than drawing it again.
    std::vector<renderer_live_gpu::Vertex> ground_vertices{};
    /// The tile keys that ground draws, each with its square.
    std::vector<std::pair<uint64_t, AtlasRect>> ground_tiles{};
    bool ground_kept{};
    uint32_t model_count{};
    uint32_t sprite_count{};
    uint32_t shadow_count{};
    uint32_t particle_count{};
    uint32_t nano_particle_count{};
    /// Reuse the previous ground buffer while splitting presentation foundations.
    std::vector<renderer_live_gpu::Vertex> foundation_vertices{};
    /// Half-transparent units held back while the opaque scene is built, then
    /// drawn over it so that their depth hides nothing drawn after them;
    /// their ranges count from the start of this buffer until then.
    std::vector<renderer_live_gpu::Vertex> translucent_vertices{};
    std::vector<UnitRange> translucent_ranges{};
};

/// Read-only frame inputs. The planner owns all model and sprite references.
struct Inputs {
    const formats::tnt::Map* map{};
    const PaletteBytes* palette{};
    const WorldDrawList* draws{};
    full::SightView sight{};
    uint16_t graphics_flags{};
    std::span<const uint8_t> shade_table{}; ///< classic indexed building shade rows
    std::array<float, 3> light{};           ///< canonical model light, independent of camera
    float light_scale{};                    ///< canonical normal-to-shade-row scale
    int32_t sea_level{};
    float detail_scale{
        1
    }; ///< pixels per world pixel; values below one enable distant detail reduction
    renderer_live_gpu::WaterEffects water{};             ///< optional bounded water presentation
    const renderer_water::SurfaceCache* water_surface{}; ///< cached local water fetch
    std::span<const renderer_water::Disturbance> water_disturbances{}; ///< visible shot events
    bool nanolathe_lighting{true}; ///< construction emission, particle halos and stream lighting
    float nanolathe_intensity{
        1
    }; ///< emission multiplier in [0,2], independent of construction progress
    /// Direction toward the camera, in x,height,z order; zero disables face culling.
    std::array<float, 3> view_direction{};
    /// Affine GPU screen x, screen y and depth rows over world x,height,z,1.
    /// A zero viewport disables screen culling and uses the painted crop instead.
    std::array<float, 12> clip_projection{};
    uint32_t clip_width{};
    uint32_t clip_height{};
    std::array<uint8_t, full::team_colour_players> team_colors{};
    /// Painted map rectangle needed by the camera, clipped to the map.
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
    /// The planner's map origin, added back to its sprite placements.
    int32_t camera_x{};
    int32_t camera_y{};
    uint32_t tick{}; ///< simulation time for construction emission; unchanged when paused
    /// An unfinished mobile unit waits to be built before its moving pieces
    /// show their running texture frames, as a building does
    /// (ModelRenderer::moving_pieces_once_built).
    bool moving_pieces_once_built{};
    /// Every unit drawn half transparent, as the debug overlay draws them;
    /// a cloaked unit is drawn so whatever this says.
    bool translucent_units{};
    /// An aircraft's shadow has a soft edge and lies on the water under it
    /// (New unit shadows); false lays the game's silhouette where the game
    /// does, on the ground or the sea bed, as every other unit's.
    bool new_unit_shadows{true};
};

/// Rebuilds live terrain, visible models and GAF cutouts from a frame's plan.
///
/// Terrain follows the canonical world height grid and samples artwork at its
/// classic projected coordinates. GAF
/// scenery uses upright cutouts; effect sprites face the camera at their
/// original altitude. Light-table flashes follow the ground. Low features and
/// cached unit shadow silhouettes follow the terrain triangles. Fog attenuation
/// is interpolated within mapped ground; undiscovered ground stays black.
/// Construction and nanolathe particles emit green light. Unit transforms retain
/// the planner's interpolated pose; the canonical simulation records are untouched.
/// Models are drawn without the classic shading and palette blending.
///
/// @param inputs borrowed map, palette, visibility and prepared draw list
/// @param[in,out] scene reusable geometry and bounded material atlas
/// @param[out] error explanation when the frame cannot be built
/// @return true when the frame can be rendered
[[nodiscard]] bool build(const Inputs& inputs, Scene& scene, std::string& error);

} // namespace oa::app::renderer_live_scene
