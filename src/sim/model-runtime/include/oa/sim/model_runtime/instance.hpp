// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/formats/objects3d.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace oa::sim::model_runtime {

inline constexpr uint32_t kNoPiece = 0xffff'ffffU;

// Angle words cover one full turn across the signed 16-bit range. Names
// describe the coordinate pair each word rotates in rotate_vector; they do not
// assign yaw/pitch/roll semantics the game does not establish.
struct RotationWords {
    int16_t xy{};
    int16_t xz{};
    int16_t yz{};
};

/// Rotates a vector through the three angle-word pairs in the game's order.
///
/// The xy pair turns first, then yz, then xz, each on the already rotated
/// values; results round to nearest as the game's rotation does, and keep
/// their low 32 bits.
///
/// @param value signed 16.16 vector to rotate
/// @param words angle words, 65536 per turn
/// @return the rotated vector
[[nodiscard]] oa::formats::objects3d::FixedVector3
rotate_vector(oa::formats::objects3d::FixedVector3 value, RotationWords words);

enum class PieceFlag : uint16_t {
    visible = 0x0001,
    cached = 0x0002,
    shaded = 0x0004,
};

struct PieceState {
    uint32_t object_index{};
    oa::formats::objects3d::FixedVector3 translation{};
    RotationWords rotation{};
    oa::formats::objects3d::FixedVector3 transformed_origin{};
    std::vector<oa::formats::objects3d::FixedVector3> transformed_vertices;
    uint16_t transform_marker{};
    uint16_t flags{};
    uint32_t next_sibling{kNoPiece};
    uint32_t first_child{kNoPiece};
    uint32_t parent{kNoPiece};
    /// The translation and rotation the instance's last rebuild_transforms
    /// worked this piece's transforms out from; only the instance writes them.
    oa::formats::objects3d::FixedVector3 built_translation{};
    RotationWords built_rotation{};
};

/// A box in a model's space, inclusive at both corners.
struct PieceBox {
    oa::formats::objects3d::FixedVector3 low{};
    oa::formats::objects3d::FixedVector3 high{};
};

class Instance {
  public:

    /// Returns the model this instance was made from.
    [[nodiscard]] const oa::formats::objects3d::Model& model() const noexcept { return *model_; }

    /// Returns the handle through which this instance shares ownership of its model.
    ///
    /// @return the handle; null for a default-constructed instance
    [[nodiscard]] const std::shared_ptr<const oa::formats::objects3d::Model>&
    model_handle() const noexcept {
        return model_;
    }

    /// Returns the piece states, COB-bound pieces first.
    [[nodiscard]] std::span<const PieceState> pieces() const noexcept { return pieces_; }

    /// Returns the piece states, COB-bound pieces first.
    [[nodiscard]] std::span<PieceState> pieces() noexcept { return pieces_; }

    /// Returns the index of the piece holding the model's root object.
    [[nodiscard]] uint32_t root_piece() const noexcept { return root_piece_; }

    /// Returns the owner association given to make_instance.
    [[nodiscard]] uintptr_t owner_token() const noexcept { return owner_token_; }

    /// Returns whether a piece moved or turned since the last rebuild_transforms.
    [[nodiscard]] bool transforms_dirty() const noexcept { return transforms_dirty_; }

    /// Returns the piece a COB piece operand names, when the instance has it.
    ///
    /// COB piece operands index the reordered prefix make_instance produces.
    ///
    /// @param index COB piece index
    /// @return the piece state, or null for an index past the pieces
    [[nodiscard]] PieceState* find_piece(uint32_t index) noexcept;
    /// Returns the piece a COB piece operand names, when the instance has it.
    ///
    /// @param index COB piece index
    /// @return the piece state, or null for an index past the pieces
    [[nodiscard]] const PieceState* find_piece(uint32_t index) const noexcept;
    /// Returns the piece a COB piece operand names.
    ///
    /// COB piece operands index the reordered prefix make_instance produces.
    /// An index past the pieces gives a spare piece outside the model, cleared
    /// at each such call: what is written to it is drawn nowhere.
    ///
    /// @param index COB piece index
    /// @return the piece state
    [[nodiscard]] PieceState& piece_for_script_index(uint32_t index);
    /// Returns the piece a COB piece operand names.
    ///
    /// An index past the pieces gives a cleared piece outside the model.
    ///
    /// @param index COB piece index
    /// @return the piece state
    [[nodiscard]] const PieceState& piece_for_script_index(uint32_t index) const;

    /// Returns a piece's attachment point relative to the model origin.
    ///
    /// Adds each piece's local object offset and script translation, rotating the
    /// sum by each ancestor's angle words on the way up, the root's turned by
    /// root_rotation.
    ///
    /// A sum outside 32 bits keeps its low 32 bits, and a negated -2^31 stays
    /// -2^31.
    ///
    /// @param piece_index piece to locate; one past the pieces is at the origin
    /// @param root_rotation angle words added to the root piece's rotation
    /// @return signed 16.16 position; Z carries the game's final sign inversion
    [[nodiscard]] oa::formats::objects3d::FixedVector3
    attachment_position(uint32_t piece_index, RotationWords root_rotation = {}) const;

    /// Returns a box grown to hold one piece's vertices, transformed as
    /// rebuild_transforms transforms them, from the pieces' translations and
    /// rotations alone, without changing the instance.
    ///
    /// Coordinates wrap at 32 bits, as in attachment_position.
    ///
    /// @param piece_index piece whose vertices the box takes in; one past the
    ///        pieces leaves the box as it is
    /// @param root_rotation angle words added to the root piece's rotation
    /// @param box the box to grow
    /// @return the grown box
    /// @quirk A root-level sibling of the root piece adds its untransformed
    ///        vertices, as rebuild_transforms leaves them.
    [[nodiscard]] PieceBox
    piece_box(uint32_t piece_index, RotationWords root_rotation, PieceBox box) const;

    /// Rebuilds every piece's transformed vertices for the renderer.
    ///
    /// Resets every piece's vertices to the model's (X and Z negated) and transforms
    /// the pieces under the instance root through their ancestors, the root turned
    /// by root_rotation, then clears transforms_dirty.
    ///
    /// A piece is worked out again only when its transforms could differ from
    /// those the last rebuild left: when root_rotation, or the translation or
    /// rotation of the piece or of an ancestor, differs from what that rebuild
    /// worked from. Any other piece keeps its transforms, which the same
    /// arithmetic would give again; the transform markers are cleared either
    /// way. A copy of the instance keeps what its transforms were worked out
    /// from, so the copy rebuilds as the original would.
    ///
    /// Coordinates wrap at 32 bits, as in attachment_position.
    ///
    /// @param root_rotation angle words added to the root piece's rotation
    /// @quirk Root-level siblings of the root piece are reset but not transformed, as in 3.1c.
    void rebuild_transforms(RotationWords root_rotation = {});

    /// Takes another instance's pose, as a copy of it would: its model, its
    /// pieces with their translations, rotations, flags and transform
    /// markers, and its owner association. Where both instances hold the same
    /// model's pieces in the same order and with the same links, this one
    /// keeps its own transforms and what they were worked out from, so that
    /// the next rebuild_transforms works out again only the pieces whose
    /// pose differs from them; otherwise it takes the other's transforms
    /// too. Either way the transforms are marked dirty.
    ///
    /// @param from the instance whose pose to take; must not be this one
    void take_pose(const Instance& from);

  private:

    /// Returns the words a piece turns its points by: its rotation, plus
    /// root_rotation at the top of the hierarchy.
    [[nodiscard]] RotationWords
    turning_words(uint32_t piece_index, RotationWords root_rotation) const;
    /// Tells whether a piece hangs under the instance root, which the
    /// transforms walk; a root-level sibling does not.
    [[nodiscard]] bool under_root(uint32_t piece_index) const;
    /// Tells whether a piece and its ancestors have the translations and
    /// rotations the last rebuild_transforms worked from.
    [[nodiscard]] bool posed_as_built(uint32_t piece_index) const;

    friend class ModelHost;
    /// Makes a model's instance; declared below.
    friend Instance make_instance(
        std::shared_ptr<const oa::formats::objects3d::Model> model,
        uintptr_t owner_token,
        std::span<const std::string_view> script_piece_names
    );
    std::shared_ptr<const oa::formats::objects3d::Model> model_;
    std::vector<PieceState> pieces_;
    PieceState spare_piece_{}; // what piece_for_script_index gives for an index past the pieces
    uint32_t root_piece_{kNoPiece};
    uintptr_t owner_token_{};
    bool transforms_dirty_{true};
    /// The transforms have been rebuilt, so that each piece's built
    /// translation and rotation and built_root_ say what they were worked
    /// out from.
    bool transforms_built_{};
    /// The root rotation the last rebuild_transforms turned the root by.
    RotationWords built_root_{};
};

/// Counts a model object, its first_child subtree and its next_sibling chain.
///
/// A missing link adds nothing. The instance is sized from the count at the
/// model root.
///
/// @param model the model
/// @param object_index object to start from; one past the objects counts 0
/// @return the number of objects reached
[[nodiscard]] uint32_t
count_linked_objects(const oa::formats::objects3d::Model& model, uint32_t object_index);

/// Says what keeps a model from being instanced.
///
/// A model is instanced when it holds objects whose links stay inside it,
/// name each parent before its children, and reach every object exactly
/// once from the root through child and sibling links, each child's parent
/// link naming the object it hangs from.
///
/// @param model the model
/// @return a static description of the first fault, or null when the model can be instanced
[[nodiscard]] const char*
model_hierarchy_error(const std::shared_ptr<const oa::formats::objects3d::Model>& model);

/// Makes a model's instance.
///
/// One piece state per object counted from the root, in the model's
/// depth-first (child-before-sibling) order, each cached and shaded, and visible
/// with three or more vertices. Given script piece names, the matching pieces
/// are then moved, case-insensitively, into COB order and the links rebuilt.
///
/// A model that model_hierarchy_error refuses gives an instance with no pieces
/// and no root piece, which keeps the model and the owner token.
///
/// @param model the model; null gives an instance whose model() must not be read
/// @param owner_token opaque owner association kept on the instance
/// @param script_piece_names COB piece names; names beyond the piece count are ignored
/// @return the instance
[[nodiscard]] Instance make_instance(
    std::shared_ptr<const oa::formats::objects3d::Model> model,
    uintptr_t owner_token = 0,
    std::span<const std::string_view> script_piece_names = {}
);

} // namespace oa::sim::model_runtime
