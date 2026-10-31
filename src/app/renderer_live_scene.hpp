// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "renderer_live_gpu.hpp"
#include "renderer_live_models.hpp"
#include "renderer_water.hpp"
#include "runtime_full.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/platform/job_pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
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
    uint64_t drawn{}; ///< the last terrain pass to draw it (Scene::terrain_passes)
};

/// What a frame's ground is drawn from, beside the tiles' art and their
/// squares in the atlas: the map's heights and tiles, the crop, the
/// picture, the detail and the fog. Two frames drawn from the same, with
/// the same squares, draw the same ground; so do two whose pictures differ
/// by no more than the margin the ground was drawn with.
struct GroundSource {
    const formats::tnt::Map* map{};
    uint32_t map_width{};   ///< height cells across
    uint32_t map_height{};  ///< height cells down
    uint32_t tile_width{};  ///< tiles across
    uint32_t tile_height{}; ///< tiles down
    /// The bytes of the tiles' art, which every tile drawn lies within.
    std::size_t tile_art_bytes{};
    std::vector<uint8_t> heights{};
    uint8_t lowest{};  ///< the least of heights
    uint8_t highest{}; ///< the greatest of heights
    std::vector<uint16_t> tile_indices{};
    /// The painted rectangle drawn, in map pixels.
    std::array<int32_t, 4> crop{};
    int32_t step{}; ///< height cells a side of each square of ground drawn
    bool distant{}; ///< planar tiles are drawn whole
    int32_t fog_divisions{};
    /// The picture the ground was drawn for.
    std::array<float, 12> clip_projection{};
    uint32_t clip_width{};
    uint32_t clip_height{};
    /// How much further beyond the picture's edges, in its pixels, the
    /// ground was drawn, for the pictures of the frames after it while the
    /// camera moves: none for the picture's own ground.
    float picture_margin{};
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

/// One band of a frame's ground, rows of it drawn apart from the others on
/// a drawing thread, into a list of its own: a row of tiles, or a row of
/// the squares drawn far out. Until its tiles are packed and the band is
/// placed in the atlas, each vertex holds in place of its atlas coordinates
/// its tile's left edge (u), its painted row (v), its tile's top edge
/// (terrain_x) and its tile's place in tiles (terrain_size), each a whole
/// number below 2^24 but the row, so that a float holds every one exactly.
struct GroundBand {
    int32_t top{};    ///< the first painted row of the band's cells
    int32_t bottom{}; ///< the row after its last
    std::vector<renderer_live_gpu::Vertex> vertices{};
    /// The tile keys the band draws, each once, in the order it first draws them.
    std::vector<uint64_t> tiles{};
    std::unordered_map<uint64_t, uint32_t> places{}; ///< each key's place in tiles
    std::vector<AtlasRect> squares{};                ///< each tile's square, once packed
    /// The band's height samples and their fog, reused from frame to frame.
    std::vector<std::array<float, 2>> samples{};
    std::vector<bool> merged_columns{}; ///< the columns of tiles drawn whole
    std::exception_ptr failure{};       ///< a failure, raised in the bands' order
    bool overflow{};                    ///< it held more vertices than a frame may
    uint64_t nanoseconds{};             ///< for the bench: the time drawing it took
};

/// The ground of a moving camera drawn ahead of it: band by band on the
/// drawing threads, over the frames before the kept ground's margin runs
/// out, for the picture the camera is expected to show by then, from the
/// same as the kept ground. Once every band is drawn it is taken as the
/// kept ground, as soon as it covers the frame's picture.
struct GroundAhead {
    bool active{}; ///< a ground is being drawn ahead
    /// What it is drawn from, with the crop, the picture and the margin it
    /// is drawn for.
    GroundSource source{};
    std::vector<GroundBand> bands{};
    std::size_t bands_drawn{};   ///< the bands drawn so far, from the first
    std::size_t bands_a_frame{}; ///< the bands each frame draws
};

/// A structure's plot, the ground levelled under it: the map rectangle it
/// covers, in map pixels across and world rows down, and the height the
/// ground lies at inside it.
struct Foundation {
    float left{};
    float right{};
    float top{};
    float bottom{};
    float height{};

    [[nodiscard]] bool operator==(const Foundation&) const = default;
};

/// Emitted triangle range belonging to a visible canonical unit.
struct UnitRange {
    uint16_t unit_id{};
    std::size_t first_vertex{};
    std::size_t vertex_count{};
};

/// The bucket a ground triangle is listed in, the one holding its least
/// place along both axes, and how many buckets further its bounds reach.
struct GroundBucket {
    uint16_t bucket{}; ///< counted row by row
    uint8_t columns{}; ///< the further buckets along the first axis, at most 255
    uint8_t rows{};    ///< the further buckets along the second axis, at most 255
};

/// The memory a frame lists its ground's triangles in by place, for the
/// effect cards' ground queries; the scene keeps it so that each frame
/// reuses it.
struct GroundIndexMemory {
    /// Each triangle's least and greatest place along the index's first
    /// axis, then along its second.
    std::vector<std::array<float, 4>> bounds{};
    std::vector<GroundBucket> buckets{}; ///< each triangle's bucket
    /// Where each bucket's triangles begin in triangles, and after the last
    /// bucket the end of triangles.
    std::vector<uint32_t> starts{};
    std::vector<uint32_t> triangles{}; ///< the triangles of every bucket
    /// The triangles reaching too many buckets to be listed in one, which
    /// every query tests.
    std::vector<uint32_t> oversized{};
};

/// The frame's own materials, packed into the rows the atlas keeps for them
/// (Scene::frame_rows): the shadows units cast from their pictures, packed
/// afresh each frame.
struct FramePacking {
    uint32_t next_x{};
    uint32_t next_y{}; ///< counted from the first of Scene::frame_rows
    uint32_t row_height{};
    /// The materials packed this frame, by digest.
    std::unordered_map<uint64_t, AtlasRect> materials{};
    /// The digests of the materials the rows hold, in the order they were
    /// packed: a frame that packs the same ones in the same order finds
    /// each where it is and writes none of them again.
    std::vector<uint64_t> order{};
    std::size_t packed{}; ///< the materials packed into the rows this frame
    /// The area of this frame's own materials so far; the rows are sized
    /// for the last whole frame's (Scene::last_frame_material_area).
    uint64_t material_area{};
    /// The area of this frame's own materials packed with the other
    /// materials for want of room in the rows.
    uint64_t spilled_area{};
    /// The frame's blurred aircraft silhouettes, by their picture and blur.
    std::unordered_map<uint64_t, AtlasRect> aircraft_shadows{};
};

/// One unit's shadow, read from its picture before the frame's own
/// materials are packed: the material it casts, and where it lies.
struct ShadowPicture {
    uint32_t width{};  ///< the material's width
    uint32_t height{}; ///< its height
    /// The material's digest, as the atlas finds materials by.
    uint64_t digest{};
    /// For a soft aircraft shadow, its silhouette and blur, which the frame
    /// finds its blurred materials by (FramePacking::aircraft_shadows).
    uint64_t aircraft_key{};
    /// Where its material's coverage, or alpha, starts in its band's
    /// ShadowBand::texels.
    std::size_t texels{};
    float left{}; ///< the shadow's left edge, in map pixels
    float top{};  ///< its top edge, in world rows
    /// An aircraft's soft shadow, which lies on the water under it.
    bool soft{};
    AtlasRect rectangle{}; ///< its material in the atlas, once packed
};

/// The shadows of one band of the models' draws, in their order.
struct ShadowBand {
    std::vector<ShadowPicture> pictures{};
    /// Every shadow's material, its coverage or its alpha, one after another.
    std::vector<uint8_t> texels{};
    std::vector<uint8_t> pixels{}; ///< a picture decoded, reused from shadow to shadow
    /// A failure reading the band's pictures, raised in the passes' order.
    std::exception_ptr failure{};
    uint64_t nanoseconds{}; ///< for the bench: the time reading them took
};

/// A frame's own material that a frame packed while its passes ran at
/// once, and whose texels are written once the passes before are joined.
struct FrameWrite {
    AtlasRect rectangle{};
    std::size_t band{};   ///< the ShadowBand holding its coverage
    std::size_t texels{}; ///< where its coverage starts in that band's texels
    bool alpha_coverage{};
};

/// The triangles one scene pass lists, apart from the other passes' so that
/// the passes can run at once, each on a drawing thread of its own; build()
/// joins the lists afterwards in the order the passes run in, one after
/// another. The scene keeps every list, so that each frame reuses it.
struct PassList {
    std::vector<renderer_live_gpu::Vertex> vertices{};
    /// The units' ranges, counted from the list's first vertex.
    std::vector<UnitRange> unit_ranges{};
    /// The half-transparent units held back (Scene::translucent_vertices),
    /// their ranges counted from the first of them.
    std::vector<renderer_live_gpu::Vertex> translucent_vertices{};
    std::vector<UnitRange> translucent_ranges{};
    /// The list with the structures' plots cut into it, for what lies on
    /// the ground, cut as far as cut_from.
    std::vector<renderer_live_gpu::Vertex> cut_vertices{};
    std::size_t cut_from{};     ///< the vertices of the list cut so far
    bool cut{};                 ///< a plot reached the vertices cut so far
    uint64_t cut_nanoseconds{}; ///< for the bench: the time cutting them took
    std::size_t peak{};         ///< the most vertices the list held at once
    uint32_t drawn{};           ///< the shadows, models, cards or particles listed
    uint32_t nano_particles{};
    /// The first of its draws the pass did not list, for want of a material
    /// not yet packed: the thread that builds the frame lists them once the
    /// passes before it are joined, and packs it. The pass's end once it
    /// has listed them all.
    std::size_t stopped{};
    /// The list's sizes as its draw began, which a pass stopped in it goes
    /// back to.
    std::size_t draw_vertices{};
    std::size_t draw_translucent{};
    std::size_t draw_ranges{};
    std::exception_ptr failure{}; ///< a failure, raised in the passes' order
    /// The models the pass draws on the card (Inputs::models_on_card), whose
    /// sizes go back with the list's when a draw stops.
    ModelCards cards{};
    /// For the bench: the time the pass took on the threads it ran on, and
    /// the materials it packed and their texels.
    uint64_t nanoseconds{};
    uint64_t materials_packed{};
    uint64_t texels_packed{};
};

/// The memory the passes after the terrain run in, kept by the scene so
/// that each frame reuses it.
struct PassLists {
    /// The ground with the plots cut into it, by band of its triangles.
    std::vector<PassList> ground{};
    PassList ground_cards{}; ///< the effect cards laid on the ground
    /// The units' shadows read from their pictures, and drawn, by band of
    /// the models' draws.
    std::vector<ShadowBand> shadow_bands{};
    std::vector<PassList> shadows{};
    /// The frame's own materials as the shadows pack them before the
    /// scene takes them (Scene::frame), and the texels they write then;
    /// the materials the packing packed, for the bench.
    FramePacking frame{};
    std::vector<FrameWrite> frame_writes{};
    PassList shadow_packing{};
    PassList models{};
    PassList projectiles{};
    PassList cards{}; ///< the effect cards standing up
    PassList particles{};
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
    /// Rows at the atlas's foot kept for the materials that change from
    /// frame to frame, the shadows units cast from their pictures: packed
    /// afresh each frame, they never fill the rows the ground and the other
    /// materials keep. None until the atlas is first emptied at its
    /// largest, which sizes them for the frame's own materials.
    uint32_t frame_rows{};
    /// The frame's own materials packed into frame_rows.
    FramePacking frame{};
    /// The area of the last whole frame's own materials, which frame_rows
    /// are sized for.
    uint64_t last_frame_material_area{};
    std::unordered_map<uint64_t, AtlasRect> materials{};
    std::unordered_map<uint64_t, TerrainMaterial> terrain_materials{};
    uint64_t terrain_passes{}; ///< the terrain passes drawn since the scene began
    /// The last terrain pass to drop the terrain materials neither it nor
    /// the pass before drew, giving their squares back.
    uint64_t stale_terrain_dropped{};
    /// Atlas squares free for terrain materials: those given back by the
    /// tiles packed in them, and the unused quarters of squares split for
    /// smaller ones. The full, 32, 16 and 8 texel squares, in that order.
    std::array<std::vector<AtlasRect>, 4> spare_terrain_squares{};
    /// The squares split into quarters for smaller terrain materials, each
    /// by its corner and edge, with a bit set for each of its quarters that
    /// is spare: a square whose four quarters are spare is whole again.
    std::unordered_map<uint64_t, uint8_t> split_terrain_squares{};
    /// The atlas area of the materials other than terrain, the frame's own
    /// among them, all packed by the last frame that started from an
    /// emptied atlas; the terrain leaves them this much room and more.
    uint64_t material_area{};
    /// The area the frame being built has packed for materials other than
    /// terrain and its own (FramePacking::material_area).
    uint64_t packed_material_area{};
    /// The most squares of ground a frame draws before it draws them from
    /// fewer height samples: lowered when a frame's triangles do not fit.
    uint64_t terrain_square_limit{400'000};
    /// The atlas area the terrain of the last frame that halved its room
    /// fitted in, and the number of tiles that frame surveyed: a frame that
    /// surveys as many plans its terrain in that room from the start, so
    /// that it finds every tile and material where the last frame packed
    /// them. None (no tiles) once a frame fits in the room it starts with.
    uint64_t terrain_room{};
    std::size_t terrain_room_tiles{};
    /// A moving camera (Inputs::tiles_hold_while_moving) has packed the
    /// atlas since it was last emptied: a material that finds no room left
    /// in the rows takes a spare terrain square, or one the frame's ground
    /// does not draw, rather than the atlas being emptied for it. A camera
    /// that has not moved packs the atlas as it always has.
    bool moving_packing{};
    /// The camera of the last frame built: its clip_projection and picture
    /// size, none before a frame with a picture.
    std::array<float, 12> camera_projection{};
    uint32_t camera_width{};
    uint32_t camera_height{};
    /// The frames still to be built before the camera, which has moved,
    /// counts as settled: none once it has stood still that long.
    uint32_t camera_settling_frames{};
    /// The last frame could not be built for lack of room: its materials
    /// would not fit the atlas at its largest, or its triangles the geometry
    /// budget. Any other failure leaves it false.
    bool out_of_room{};
    /// The palette of the frame being built and the digest of its bytes,
    /// taken once a frame for every material drawn in it; null outside a
    /// build.
    const PaletteBytes* frame_palette{};
    uint64_t frame_palette_digest{};
    PaletteBytes terrain_palette{};
    PaletteBytes terrain_grey_palette{};
    std::size_t terrain_vertex_count{};
    /// The ground's triangles listed along world x and z, and along the
    /// effect cards' axes.
    std::array<GroundIndexMemory, 2> ground_index_memory{};
    /// What the last frame's ground was drawn from.
    GroundSource ground_source{};
    /// The ground of the last frame drawn from the same as the frame before
    /// it, or drawn beyond a moving camera's picture, while ground_kept: a
    /// frame drawn from the same again, whose picture it covers and whose
    /// tiles keep their squares, takes it rather than drawing it again.
    std::vector<renderer_live_gpu::Vertex> ground_vertices{};
    /// The tile keys that ground draws, each with its square.
    std::vector<std::pair<uint64_t, AtlasRect>> ground_tiles{};
    bool ground_kept{};
    /// The bands of a ground drawn within the frame, reused from frame to frame.
    std::vector<GroundBand> ground_bands{};
    /// The ground drawn ahead of a moving camera, to take over from the
    /// kept ground before its margin runs out.
    GroundAhead ground_ahead{};
    /// The revision of the kept ground, which a frame that takes it takes too.
    uint64_t kept_ground_revision{};
    /// The revision of the frame's ground, its first terrain_vertex_count
    /// vertices: a new one, never had by any scene before, whenever they may
    /// differ from the last frame's (renderer_live_gpu::KeptGround).
    uint64_t ground_revision{};
    /// The kept ground with the plots cut into it, the ground a frame that
    /// takes the kept ground of cut_source and has the same plots takes,
    /// with its revision, rather than cutting them into it again.
    std::vector<renderer_live_gpu::Vertex> cut_ground{};
    std::vector<Foundation> cut_plots{}; ///< sorted, as foundations cuts them
    uint64_t cut_source{};               ///< the kept ground's revision; 0 for none
    uint64_t cut_revision{};
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
    /// The lists of the passes after the terrain.
    PassLists passes{};
    /// The model types the card keeps (Inputs::models_on_card), and the
    /// frame's models drawn from them: each run among the vertices before
    /// its ModelRun::vertex, which the pick takes too.
    CardTypes card_types{};
    ModelCards cards{};
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
    /// The frames follow the player's camera, not a recording's: while it
    /// moves, and for a few frames after, each terrain tile is packed one
    /// square size smaller than the plan gives it and keeps any square at
    /// least that large, so that the atlas is not packed again as tiles come
    /// nearer and new ones into view. Once the camera has settled, every
    /// tile takes the plan's square again.
    bool tiles_hold_while_moving{};
    /// The drawing threads the passes after the terrain run on, at once;
    /// none runs them one after another on the calling thread. Either way
    /// the frame is the same.
    platform::job_pool::Pool* pool{};
    /// The card draws units and features from their types' models
    /// (renderer_live_gpu::draws_models): each model draw it can serve goes
    /// to Scene::cards rather than to the vertices, drawing what its
    /// vertices would have, in their place. A model under construction,
    /// cloaked or under the debug overlay, reflected in the water, shaded as
    /// a building, lit by nanolathe particles, or of a model the card cannot
    /// hold stays in the vertices.
    bool models_on_card{};
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
/// After the terrain, the passes (the cards on the ground, the shadows, the
/// foundations, the models, the projectiles, the standing cards and the
/// particles) each list their triangles apart, on the drawing threads at
/// once, and their lists are joined in that order: the vertices, their
/// order and the atlas are those the passes give one after another, with
/// any number of threads. Only the calling thread packs materials, in the
/// passes' order: a pass that needs one not yet packed stops there, and the
/// calling thread lists the rest of it once the passes before it are in.
///
/// @param inputs borrowed map, palette, visibility and prepared draw list
/// @param[in,out] scene reusable geometry and bounded material atlas
/// @param[out] error explanation when the frame cannot be built
/// @return true when the frame can be rendered
[[nodiscard]] bool build(const Inputs& inputs, Scene& scene, std::string& error);

} // namespace oa::app::renderer_live_scene
