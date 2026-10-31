// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/model_runtime/instance.hpp"

#include "oa/base/game_math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace oa::sim::model_runtime {
namespace {

bool equal_fold_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto ac = static_cast<unsigned char>(a[i]);
        auto bc = static_cast<unsigned char>(b[i]);
        if (ac >= 'A' && ac <= 'Z')
            ac = static_cast<unsigned char>(ac + ('a' - 'A'));
        if (bc >= 'A' && bc <= 'Z')
            bc = static_cast<unsigned char>(bc + ('a' - 'A'));
        if (ac != bc)
            return false;
    }
    return true;
}

// A coordinate outside 32 bits keeps its low 32 bits.
int32_t low32(int64_t value) noexcept {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
}

void rotate_pair(int32_t& a, int32_t& b, int16_t angle) {
    if (angle == 0)
        return;
    const auto rotated = base::game_math::rotate_pair(a, b, angle);
    // Each coordinate rounds to the nearest integer, ties to even.
    a = low32(std::llrint(rotated.first));
    b = low32(std::llrint(rotated.second));
}

void rotate(oa::formats::objects3d::FixedVector3& value, RotationWords words) {
    // The game's pair order and aliasing.
    rotate_pair(value.x, value.y, words.xy);
    rotate_pair(value.y, value.z, words.yz);
    rotate_pair(value.x, value.z, words.xz);
}

// The negation wraps: -2^31 stays -2^31.
int32_t negate(int32_t value) noexcept {
    return std::bit_cast<int32_t>(0U - std::bit_cast<uint32_t>(value));
}

} // namespace

oa::formats::objects3d::FixedVector3
rotate_vector(oa::formats::objects3d::FixedVector3 value, RotationWords words) {
    rotate(value, words);
    return value;
}

uint32_t count_linked_objects(const oa::formats::objects3d::Model& model, uint32_t object_index) {
    if (object_index >= model.objects.size())
        return 0;
    const auto& object = model.objects[object_index];
    // The count starts at 1, becomes child_count + 1 when the child link is
    // present, then adds the sibling chain.
    uint32_t count = 1;
    if (object.first_child != oa::formats::objects3d::kNoObject)
        count = count_linked_objects(model, object.first_child) + 1;
    if (object.next_sibling != oa::formats::objects3d::kNoObject)
        count += count_linked_objects(model, object.next_sibling);
    return count;
}

const char*
model_hierarchy_error(const std::shared_ptr<const oa::formats::objects3d::Model>& model) {
    if (!model || model->objects.empty())
        return "3DO model is empty";

    // Public callers may construct Models without going through load_3do.
    const auto count = model->objects.size();
    auto valid_link = [count](uint32_t link) { return link == kNoPiece || link < count; };
    for (std::size_t i = 0; i < count; ++i) {
        const auto& object = model->objects[i];
        if (!valid_link(object.parent) || !valid_link(object.first_child) ||
            !valid_link(object.next_sibling))
            return "3DO model contains an out-of-range hierarchy link";
        if (object.parent != kNoPiece && object.parent >= i)
            return "3DO model hierarchy is not parent-before-child preorder";
    }

    struct Pending {
        uint32_t object{};
        uint32_t expected_parent{};
    };

    std::vector<Pending> pending{{0, kNoPiece}};
    std::vector<uint8_t> topology_seen(count);
    while (!pending.empty()) {
        const auto [current, expected_parent] = pending.back();
        pending.pop_back();
        if (topology_seen[current] != 0)
            return "3DO model hierarchy contains a cycle or duplicate link";
        topology_seen[current] = 1;
        const auto& object = model->objects[current];
        if (object.parent != expected_parent)
            return "3DO model parent link disagrees with child/sibling topology";
        if (object.next_sibling != kNoPiece)
            pending.push_back({object.next_sibling, expected_parent});
        if (object.first_child != kNoPiece)
            pending.push_back({object.first_child, current});
    }
    if (std::find(topology_seen.begin(), topology_seen.end(), 0) != topology_seen.end())
        return "3DO model contains an unreachable object";
    return nullptr;
}

Instance make_instance(
    std::shared_ptr<const oa::formats::objects3d::Model> model,
    uintptr_t owner_token,
    std::span<const std::string_view> script_piece_names
) {
    if (model_hierarchy_error(model) != nullptr) {
        Instance empty;
        empty.model_ = std::move(model);
        empty.owner_token_ = owner_token;
        return empty;
    }

    Instance instance;
    instance.model_ = std::move(model);
    instance.owner_token_ = owner_token;
    instance.pieces_.reserve(instance.model_->objects.size());
    for (uint32_t oi = 0; oi < instance.model_->objects.size(); ++oi) {
        const auto& object = instance.model_->objects[oi];
        PieceState piece;
        piece.object_index = oi;
        piece.flags =
            static_cast<uint16_t>(PieceFlag::cached) | static_cast<uint16_t>(PieceFlag::shaded);
        if (object.vertices.size() >= 3)
            piece.flags |= static_cast<uint16_t>(PieceFlag::visible);
        piece.transformed_vertices.reserve(object.vertices.size());
        for (const auto& vertex : object.vertices)
            piece.transformed_vertices.push_back({negate(vertex.x), vertex.y, negate(vertex.z)});
        instance.pieces_.push_back(std::move(piece));
    }

    // Binding scans only from the current script slot onward and swaps the
    // whole piece state, making the first N entries use COB piece order.
    for (std::size_t script_index = 0; script_index < script_piece_names.size(); ++script_index) {
        if (script_index >= instance.pieces_.size())
            continue;
        for (std::size_t candidate = script_index; candidate < instance.pieces_.size();
             ++candidate) {
            const auto object_index = instance.pieces_[candidate].object_index;
            if (equal_fold_ascii(
                    script_piece_names[script_index], instance.model_->objects[object_index].name
                )) {
                if (candidate != script_index)
                    std::swap(instance.pieces_[candidate], instance.pieces_[script_index]);
                break;
            }
        }
    }

    // Links are rebuilt after the reorder by matching model objects.
    std::vector<uint32_t> piece_for_object(instance.model_->objects.size(), kNoPiece);
    for (uint32_t pi = 0; pi < instance.pieces_.size(); ++pi)
        piece_for_object[instance.pieces_[pi].object_index] = pi;
    for (auto& piece : instance.pieces_) {
        const auto& object = instance.model_->objects[piece.object_index];
        if (object.parent != oa::formats::objects3d::kNoObject)
            piece.parent = piece_for_object[object.parent];
        if (object.first_child != oa::formats::objects3d::kNoObject)
            piece.first_child = piece_for_object[object.first_child];
        if (object.next_sibling != oa::formats::objects3d::kNoObject)
            piece.next_sibling = piece_for_object[object.next_sibling];
    }
    instance.root_piece_ = piece_for_object[0];
    return instance;
}

PieceState* Instance::find_piece(uint32_t index) noexcept {
    return index < pieces_.size() ? &pieces_[index] : nullptr;
}

const PieceState* Instance::find_piece(uint32_t index) const noexcept {
    return index < pieces_.size() ? &pieces_[index] : nullptr;
}

PieceState& Instance::piece_for_script_index(uint32_t index) {
    if (index < pieces_.size())
        return pieces_[index];
    spare_piece_ = {};
    return spare_piece_;
}

const PieceState& Instance::piece_for_script_index(uint32_t index) const {
    static const PieceState no_piece{};
    return index < pieces_.size() ? pieces_[index] : no_piece;
}

oa::formats::objects3d::FixedVector3
Instance::attachment_position(uint32_t piece_index, RotationWords root_rotation) const {
    oa::formats::objects3d::FixedVector3 result{};
    if (piece_index >= pieces_.size())
        return result;
    auto current = piece_index;
    while (current != kNoPiece) {
        const auto& piece = pieces_[current];
        const auto& object = model_->objects[piece.object_index];
        result.x = low32(
            static_cast<int64_t>(result.x) + negate(object.offset_from_parent.x) +
            piece.translation.x
        );
        result.y = low32(
            static_cast<int64_t>(result.y) + object.offset_from_parent.y + piece.translation.y
        );
        result.z = low32(
            static_cast<int64_t>(result.z) + negate(object.offset_from_parent.z) +
            piece.translation.z
        );
        if (piece.parent == kNoPiece)
            break;
        auto rotation = pieces_[piece.parent].rotation;
        if (pieces_[piece.parent].parent == kNoPiece) {
            rotation.xy = static_cast<int16_t>(rotation.xy + root_rotation.xy);
            rotation.xz = static_cast<int16_t>(rotation.xz + root_rotation.xz);
            rotation.yz = static_cast<int16_t>(rotation.yz + root_rotation.yz);
        }
        rotate(result, rotation);
        current = piece.parent;
    }
    result.z = negate(result.z);
    return result;
}

RotationWords Instance::turning_words(uint32_t piece_index, RotationWords root_rotation) const {
    auto rotation = pieces_[piece_index].rotation;
    if (pieces_[piece_index].parent == kNoPiece) {
        rotation.xy = static_cast<int16_t>(rotation.xy + root_rotation.xy);
        rotation.xz = static_cast<int16_t>(rotation.xz + root_rotation.xz);
        rotation.yz = static_cast<int16_t>(rotation.yz + root_rotation.yz);
    }
    return rotation;
}

bool Instance::under_root(uint32_t piece_index) const {
    auto top = piece_index;
    while (pieces_[top].parent != kNoPiece)
        top = pieces_[top].parent;
    return top == root_piece_;
}

namespace {
// Turns a point by a piece's turn and moves it by the piece's offset from its
// parent and script translation, into the parent's space.
void place_in_parent(
    oa::formats::objects3d::FixedVector3& point,
    RotationWords turn,
    const oa::formats::objects3d::Object& source,
    const PieceState& state
) {
    rotate(point, turn);
    point.x = low32(
        static_cast<int64_t>(point.x) + negate(source.offset_from_parent.x) + state.translation.x
    );
    point.y =
        low32(static_cast<int64_t>(point.y) + source.offset_from_parent.y + state.translation.y);
    point.z = low32(
        static_cast<int64_t>(point.z) + negate(source.offset_from_parent.z) + state.translation.z
    );
}

// The turns piece_box holds for a piece and its nearest ancestors; a deeper
// piece works out the turns of the levels past them for each vertex.
constexpr std::size_t kHeldTurns = 8;
} // namespace

PieceBox
Instance::piece_box(uint32_t piece_index, RotationWords root_rotation, PieceBox box) const {
    if (piece_index >= pieces_.size())
        return box;
    const bool transformed = under_root(piece_index);
    std::array<RotationWords, kHeldTurns> turns{};
    std::size_t held = 0;
    for (auto current = piece_index; transformed && current != kNoPiece && held < turns.size();
         current = pieces_[current].parent)
        turns[held++] = turning_words(current, root_rotation);
    for (const auto& vertex : model_->objects[pieces_[piece_index].object_index].vertices) {
        oa::formats::objects3d::FixedVector3 point{negate(vertex.x), vertex.y, negate(vertex.z)};
        std::size_t level = 0;
        for (auto current = piece_index; transformed && current != kNoPiece;
             current = pieces_[current].parent, ++level) {
            const auto& state = pieces_[current];
            place_in_parent(
                point,
                level < held ? turns[level] : turning_words(current, root_rotation),
                model_->objects[state.object_index],
                state
            );
        }
        box.low = {
            std::min(box.low.x, point.x), std::min(box.low.y, point.y), std::min(box.low.z, point.z)
        };
        box.high = {
            std::max(box.high.x, point.x),
            std::max(box.high.y, point.y),
            std::max(box.high.z, point.z)
        };
    }
    return box;
}

namespace {
bool same_words(RotationWords a, RotationWords b) noexcept {
    return a.xy == b.xy && a.xz == b.xz && a.yz == b.yz;
}

bool same_vector(
    const oa::formats::objects3d::FixedVector3& a, const oa::formats::objects3d::FixedVector3& b
) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
} // namespace

bool Instance::posed_as_built(uint32_t piece_index) const {
    for (auto current = piece_index; current != kNoPiece; current = pieces_[current].parent) {
        const auto& piece = pieces_[current];
        if (!same_vector(piece.translation, piece.built_translation) ||
            !same_words(piece.rotation, piece.built_rotation))
            return false;
    }
    return true;
}

void Instance::rebuild_transforms(RotationWords root_rotation) {
    // The transforms are a function of the root rotation and of the
    // translations and rotations from each piece up to the root alone, so a
    // piece whose inputs are those of the last rebuild keeps what it left. A
    // root-level sibling's reset vertices depend on none of them.
    const bool built = transforms_built_ && same_words(built_root_, root_rotation);
    for (uint32_t index = 0; index < pieces_.size(); ++index) {
        auto& piece = pieces_[index];
        piece.transform_marker = 0;
        if (built && (!under_root(index) || posed_as_built(index)))
            continue;
        const auto& object = model_->objects[piece.object_index];
        piece.transformed_vertices.clear();
        piece.transformed_vertices.reserve(object.vertices.size());
        for (const auto& vertex : object.vertices)
            piece.transformed_vertices.push_back({negate(vertex.x), vertex.y, negate(vertex.z)});
        piece.transformed_origin = {};
        // The transform walk starts at the instance root. Root-level siblings
        // are reset but are outside that traversal.
        if (!under_root(index))
            continue;
        for (auto current = index; current != kNoPiece; current = pieces_[current].parent) {
            const auto& state = pieces_[current];
            const auto& source = model_->objects[state.object_index];
            // Every point of the piece turns by the same words.
            const RotationWords turn = turning_words(current, root_rotation);
            place_in_parent(piece.transformed_origin, turn, source, state);
            for (auto& vertex : piece.transformed_vertices)
                place_in_parent(vertex, turn, source, state);
        }
    }
    for (auto& piece : pieces_) {
        piece.built_translation = piece.translation;
        piece.built_rotation = piece.rotation;
    }
    transforms_built_ = true;
    built_root_ = root_rotation;
    transforms_dirty_ = false;
}

void Instance::take_pose(const Instance& from) {
    bool same_pieces = model_ == from.model_ && root_piece_ == from.root_piece_ &&
                       pieces_.size() == from.pieces_.size();
    for (std::size_t index = 0; same_pieces && index < pieces_.size(); ++index) {
        const auto& mine = pieces_[index];
        const auto& theirs = from.pieces_[index];
        same_pieces = mine.object_index == theirs.object_index && mine.parent == theirs.parent &&
                      mine.first_child == theirs.first_child &&
                      mine.next_sibling == theirs.next_sibling;
    }
    if (!same_pieces) {
        *this = from;
        transforms_dirty_ = true;
        return;
    }
    for (std::size_t index = 0; index < pieces_.size(); ++index) {
        auto& mine = pieces_[index];
        const auto& theirs = from.pieces_[index];
        mine.translation = theirs.translation;
        mine.rotation = theirs.rotation;
        mine.transform_marker = theirs.transform_marker;
        mine.flags = theirs.flags;
    }
    spare_piece_ = from.spare_piece_;
    owner_token_ = from.owner_token_;
    transforms_dirty_ = true;
}

} // namespace oa::sim::model_runtime
