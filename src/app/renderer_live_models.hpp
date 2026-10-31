// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The units and features the turned 3D view draws from their types' models
// on the graphics card (renderer_live_gpu::ModelDraws), as the scene's models
// pass lists them: each type's polygons, made into triangle corners once and
// kept on the card, and each frame's instances, their vertices as placed,
// the textures their teams draw and the triangles they show.

#include "renderer_live_gpu.hpp"
#include "oa/formats/objects3d.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace oa::app::renderer_live_scene {

/// A texture a model type's polygons draw with: a palette colour, or the
/// frame present::model::primitive_texture gives a primitive drawing its
/// first frame or its running one. A frame finds it in the atlas once for
/// each team that draws the type.
struct CardSlot {
    /// The primitive whose frame it is; null for a colour.
    const present::model::PreparedPrimitive* primitive{};
    bool first_frame{}; ///< the primitive's first frame, not its running one
    uint8_t color{};    ///< the palette colour, for a colour
};

/// A polygon of a model type, as the models pass draws it: the piece and
/// object primitive it is, its first triangle corner among its type's, and
/// the textures it draws on its piece's first frame and its running one.
struct CardPolygon {
    uint32_t piece{};     ///< the instance's piece
    uint32_t primitive{}; ///< its index in the object's primitives
    uint32_t record{};    ///< its first triangle corner, counted from its type's first
    uint32_t corners{};   ///< its corners, three or more
    uint16_t first_slot{};
    uint16_t running_slot{};
    bool colored{};  ///< filled with a palette colour
    uint8_t color{}; ///< that colour
};

/// A model type the card keeps: a model, with its pieces in an instance's
/// order, and the textures its primitives were prepared with.
struct CardType {
    /// The model, by its handle's ownership: another model made where a
    /// freed one was never passes for it.
    std::weak_ptr<const formats::objects3d::Model> model{};
    const present::model::PreparedModel* prepared{};
    std::vector<uint32_t> objects{};        ///< each piece's object, in the instance's order
    std::vector<uint32_t> piece_vertices{}; ///< each piece's first vertex, then their count
    std::vector<CardPolygon> polygons{};    ///< in drawing order, piece by piece
    std::vector<uint32_t> piece_polygons{}; ///< each piece's first polygon, then their count
    std::vector<CardSlot> slots{};
    uint32_t first_record{}; ///< its first triangle corner in CardTypes::records
    uint32_t record_count{};
    /// The card draws the model; one it cannot is kept, its pieces alone,
    /// so that a frame finds that at once.
    bool drawable{};
};

/// The model types the card keeps, and their triangle corners, as
/// renderer_live_gpu::ModelDraws::corners holds them.
struct CardTypes {
    std::vector<CardType> types{};
    /// The types made from each model, by its address.
    std::unordered_map<const formats::objects3d::Model*, std::vector<uint32_t>> by_model{};
    std::vector<uint32_t> records{}; ///< two words a triangle corner, type after type
    /// Changes whenever records are made afresh: never the same in two
    /// scenes. Zero before any type is made.
    uint64_t revision{};
    /// The frame's model draws' types, by their place in the draw list,
    /// found before the scene's passes run (-1 for none), which the passes
    /// read and never change.
    std::vector<int32_t> draw_types{};
};

/// A type and team whose textures a frame has found: where they start in
/// ModelCards::textures and ModelCards::slot_states.
struct CardTable {
    uint32_t type{};
    uint8_t team{};
    uint32_t first{};
};

/// A frame's model draw on the card, for the pick: the unit it draws, where
/// it falls among the frame's vertices and the indices of its triangles.
struct CardUnit {
    uint16_t unit_id{};     ///< 0 for a feature or a unit the pick never takes
    std::size_t vertex{};   ///< the frame's vertex it is drawn before
    uint32_t instance{};    ///< its instance in ModelCards::instances
    uint32_t type{};        ///< its type in CardTypes::types
    uint32_t first_index{}; ///< its first index in ModelCards::indices
    uint32_t index_count{}; ///< its indices, three a triangle
    /// The bounds of the corners it draws through the frame's camera, in
    /// its picture's pixels: those the pick takes from vertices drawn. Left
    /// past right where it draws none.
    float left{1};
    float top{1};
    float right{};
    float bottom{};
    /// Whether the pick finds every point of its triangles within those
    /// bounds widened as renderer_live_camera::triangle_hit widens a
    /// triangle's; false where one is a sliver, which the pick tests
    /// wherever the pointer is.
    bool bounded{true};
};

/// The sizes of a frame's model draws as a draw began, which a draw that
/// stops for a material not yet packed goes back to.
struct CardMark {
    std::size_t instances{};
    std::size_t positions{};
    std::size_t piece_frames{};
    std::size_t indices{};
    std::size_t runs{};
    std::size_t units{};
    std::size_t casts{};
    std::size_t cast_indices{};
    std::size_t cast_runs{};
    uint32_t cast_depths{};
    std::size_t vertices{};
};

/// A frame's model draws on the card, as one scene pass lists them, and
/// then the frame, for render() and the pick.
struct ModelCards {
    std::vector<renderer_live_gpu::ModelInstance> instances{};
    std::vector<float> positions{};
    std::vector<uint32_t> piece_frames{};
    /// The textures of each type and team the frame draws, four atlas places
    /// a slot, in CardTable order.
    std::vector<float> textures{};
    /// Each slot's state: 0 not yet found, 1 drawn, 2 drawn nowhere (no frame).
    std::vector<uint8_t> slot_states{};
    std::vector<CardTable> tables{};
    std::vector<uint32_t> indices{};
    std::vector<renderer_live_gpu::ModelRun> runs{};
    std::vector<CardUnit> units{};
    /// The vertices the frame's vertices would hold for the triangles drawn
    /// here, which the geometry budget counts as if they were.
    std::size_t vertices{};
    /// The units' shadows cast from their instances into the shadows' map
    /// (renderer_live_gpu::ShadowCast), their triangles and runs, and the
    /// depths given out so far, a picture and the silhouette it is taken out
    /// of sharing one.
    std::vector<renderer_live_gpu::ShadowCast> casts{};
    std::vector<uint32_t> cast_indices{};
    std::vector<renderer_live_gpu::ModelRun> cast_runs{};
    uint32_t cast_depths{};
    /// The shadows' map, and where the shadows lie among the frame's
    /// vertices, once the frame is built.
    renderer_live_gpu::ShadowMap shadow_map{};
    std::size_t shadow_vertex{};
    std::size_t shadow_ground{};
    /// The instance of the draw the pass is listing, once it has one; -1
    /// before.
    int64_t draw_instance{-1};
    CardMark mark{}; ///< the sizes as the pass's draw began
};

/// A model draw's triangle corner, as the card draws it: its world place and
/// its atlas place.
struct CardCorner {
    float x{};
    float height{};
    float z{};
    float u{};
    float v{};
};

/// Returns a type for an instance's model and pieces, made and its corners
/// added to the records when there is none yet; none where the card cannot
/// draw the model: a vertex index past its object's vertices, or more
/// corners, vertices, pieces or textures than a type holds.
///
/// @param[in,out] types the types kept
/// @param instance the instance drawn
/// @param prepared the model's textures as prepared
/// @return the type's place in types.types, or -1
[[nodiscard]] int32_t card_type(
    CardTypes& types,
    const sim::model_runtime::Instance& instance,
    const present::model::PreparedModel& prepared
);

/// The most triangle corners the types keep: past it, a frame forgets them
/// all before it draws (trim_card_types).
inline constexpr std::size_t card_records_kept = std::size_t{1} << 20;

/// Forgets every type, and the models they hold, once their triangle
/// corners pass card_records_kept: the frame about to draw makes those it
/// draws afresh, under a new revision.
///
/// @param[in,out] types the types kept
void trim_card_types(CardTypes& types);

/// Empties a frame's model draws, keeping their memory.
///
/// @param[out] cards the frame's model draws
void clear_cards(ModelCards& cards) noexcept;

/// Notes the sizes of a frame's model draws as a draw begins (ModelCards::mark).
///
/// @param[in,out] cards the frame's model draws
void mark_cards(ModelCards& cards) noexcept;

/// Takes a frame's model draws back to their sizes as the draw last marked
/// began. The textures found stay.
///
/// @param[in,out] cards the frame's model draws
void unmark_cards(ModelCards& cards) noexcept;

/// Returns a triangle corner of a frame's model draw as the card draws it.
///
/// @param types the types kept
/// @param cards the frame's model draws
/// @param unit the draw
/// @param index one of its indices
/// @return the corner
[[nodiscard]] CardCorner card_corner(
    const CardTypes& types, const ModelCards& cards, const CardUnit& unit, uint32_t index
) noexcept;

/// Returns the frame's vertex a corner of a model draw gives the scene's
/// fragment stage: its place, the instance's brightness and a model's every
/// other attribute, as the card's models' vertex stage gives it.
///
/// @param types the types kept
/// @param cards the frame's model draws
/// @param unit the draw
/// @param index one of its indices
/// @return the vertex
[[nodiscard]] renderer_live_gpu::Vertex card_vertex(
    const CardTypes& types, const ModelCards& cards, const CardUnit& unit, uint32_t index
) noexcept;

/// Returns the frame's model draws as render() takes them.
///
/// @param types the types kept
/// @param cards the frame's model draws
/// @return the draws, borrowing both
[[nodiscard]] renderer_live_gpu::ModelDraws
card_draws(const CardTypes& types, const ModelCards& cards) noexcept;

} // namespace oa::app::renderer_live_scene
