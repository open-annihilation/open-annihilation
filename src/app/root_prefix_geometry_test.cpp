// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Bounded final-root Q30 arithmetic, immutable prefix ownership and point-free boxes.
#include "root_prefix_geometry.hpp"

#include "oa/test/check.hpp"

#include <array>
#include <bit>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

namespace {

using oa::app::RootPrefixBounds;
using oa::app::RootPrefixPoint;
using oa::app::RootPrefixRecipe;
using oa::app::RootPrefixRotation;
using oa::formats::objects3d::Model;
using oa::formats::objects3d::Object;
using oa::sim::model_runtime::Instance;
using oa::sim::model_runtime::RotationWords;

constexpr int32_t fixed = oa::app::root_prefix_fixed_scale;
constexpr size_t packet_budget = size_t{128} * 1024;

/// Checks equality of every coordinate.
bool same(RootPrefixPoint first, RootPrefixPoint second) {
    return first.x == second.x && first.y == second.y && first.z == second.z;
}

/// Checks containment in an inclusive fixed-coordinate box.
bool contains(const RootPrefixBounds& box, RootPrefixPoint point) {
    return point.x >= box.low.x && point.x <= box.high.x && point.y >= box.low.y &&
           point.y <= box.high.y && point.z >= box.low.z && point.z <= box.high.z;
}

/// Checks source-instance fields that a rendering packet must not change.
void check_unchanged(const Instance& source, const Instance& before) {
    OA_CHECK(source.model_handle() == before.model_handle());
    OA_CHECK(source.owner_token() == before.owner_token());
    OA_CHECK(source.root_piece() == before.root_piece());
    OA_CHECK(source.transforms_dirty() == before.transforms_dirty());
    OA_CHECK(source.materialization_revision() == before.materialization_revision());
    OA_CHECK(source.pieces().size() == before.pieces().size());
    if (source.pieces().size() != before.pieces().size())
        return;
    for (size_t piece = 0; piece < source.pieces().size(); ++piece) {
        const auto& first = source.pieces()[piece];
        const auto& second = before.pieces()[piece];
        OA_CHECK(first.object_index == second.object_index);
        OA_CHECK(first.parent == second.parent);
        OA_CHECK(first.first_child == second.first_child);
        OA_CHECK(first.next_sibling == second.next_sibling);
        OA_CHECK(same(first.translation, second.translation));
        OA_CHECK(first.rotation.xy == second.rotation.xy);
        OA_CHECK(first.rotation.xz == second.rotation.xz);
        OA_CHECK(first.rotation.yz == second.rotation.yz);
        OA_CHECK(same(first.built_translation, second.built_translation));
        OA_CHECK(first.built_rotation.xy == second.built_rotation.xy);
        OA_CHECK(first.built_rotation.xz == second.built_rotation.xz);
        OA_CHECK(first.built_rotation.yz == second.built_rotation.yz);
        OA_CHECK(first.flags == second.flags);
        OA_CHECK(first.transform_marker == second.transform_marker);
        OA_CHECK(same(first.transformed_origin, second.transformed_origin));
        OA_CHECK(first.transformed_vertices.size() == second.transformed_vertices.size());
        if (first.transformed_vertices.size() != second.transformed_vertices.size())
            continue;
        for (size_t vertex = 0; vertex < first.transformed_vertices.size(); ++vertex)
            OA_CHECK(same(first.transformed_vertices[vertex], second.transformed_vertices[vertex]));
    }
}

/// Makes a small articulated model with a nonzero root offset.
std::shared_ptr<Model> model_with(size_t count = 3) {
    auto model = std::make_shared<Model>();
    constexpr std::array<std::string_view, 3> names{"base", "arm", "gun"};
    for (size_t index = 0; index < count; ++index) {
        Object object;
        object.name = names[index % names.size()];
        object.parent =
            index == 0 ? oa::formats::objects3d::kNoObject : static_cast<uint32_t>(index - 1);
        if (index + 1 < count)
            object.first_child = static_cast<uint32_t>(index + 1);
        object.offset_from_parent = {13 + static_cast<int32_t>(index) * fixed / 2, -29, 37};
        object.vertices = {
            {-fixed, -fixed / 2, fixed / 3},
            {fixed, -fixed / 2, fixed / 3},
            {fixed, fixed, -fixed / 3},
            {-fixed, fixed, -fixed / 3}
        };
        model->objects.push_back(std::move(object));
    }
    return model;
}

/// Makes an exact zero-XZ prefix in reordered COB piece order.
Instance zero_prefix(const std::shared_ptr<Model>& model) {
    constexpr std::array<std::string_view, 3> script_order{"gun", "base", "arm"};
    auto source = oa::sim::model_runtime::make_instance(model, 71, script_order);
    for (size_t piece = 0; piece < source.pieces().size(); ++piece) {
        auto& state = source.pieces()[piece];
        state.translation = {static_cast<int32_t>(piece) * 191, -37, 511};
        state.rotation = {static_cast<int16_t>(piece * 123), 313, -514};
    }
    source.rebuild_transforms({321, -313, 17});
    return source;
}

/// Checks all angle words against the native final plane turn at domain edges.
void test_native_error_bound() {
    constexpr int32_t limit = oa::app::root_prefix_coordinate_limit;
    constexpr std::array<std::array<int32_t, 2>, 12> points{
        {{-limit, -limit},
         {-limit, limit},
         {limit, -limit},
         {limit, limit},
         {limit, 0},
         {0, limit},
         {-limit, 0},
         {0, -limit},
         {limit - 1, limit / 3},
         {-limit + 1, -limit / 3},
         {1, -1},
         {-3, 5}}
    };
    int64_t greatest_error{};
    for (uint32_t word = 0; word <= std::numeric_limits<uint16_t>::max(); ++word) {
        const auto angle = std::bit_cast<int16_t>(static_cast<uint16_t>(word));
        const auto rotation = oa::app::prepare_root_prefix_rotation(angle);
        OA_CHECK(rotation.has_value());
        if (!rotation)
            continue;
        for (const auto& pair : points) {
            const RootPrefixPoint point{pair[0], 173, pair[1]};
            const auto native = oa::sim::model_runtime::rotate_vector(point, {0, angle, 0});
            const auto actual = oa::app::rotate_root_prefix_point(point, *rotation);
            const int64_t error_x = std::abs(int64_t{actual.x} - native.x);
            const int64_t error_z = std::abs(int64_t{actual.z} - native.z);
            greatest_error = std::max({greatest_error, error_x, error_z});
            OA_CHECK(error_x <= 1 && error_z <= 1);
            OA_CHECK(actual.y == point.y);
        }
    }
    // Quantization is a bounded visual policy rather than native arithmetic.
    OA_CHECK(greatest_error == 1);
}

/// Checks ties, signed products and wrapped placement independently of angle preparation.
void test_rounding_and_placement() {
    constexpr int32_t half = oa::app::root_prefix_rotation_scale / 2;
    constexpr RootPrefixRotation halve{0, half};
    constexpr std::array<int32_t, 6> input{1, 3, 5, -1, -3, -5};
    constexpr std::array<int32_t, 6> rounded{0, 2, 2, 0, -2, -2};
    for (size_t at = 0; at < input.size(); ++at) {
        const auto point = oa::app::rotate_root_prefix_point({input[at], 17, input[at]}, halve);
        OA_CHECK(point.x == rounded[at] && point.z == rounded[at] && point.y == 17);
    }
    const auto cancellation = oa::app::rotate_root_prefix_point({3, -71, -2}, {half, half});
    OA_CHECK(same(cancellation, {2, -71, 0}));
    const auto placed =
        oa::app::place_root_prefix_point({INT32_MAX, INT32_MAX, INT32_MIN}, {}, {1, 1, -1});
    OA_CHECK(same(placed, {INT32_MIN, INT32_MIN, INT32_MAX}));
    const RootPrefixRotation invalid{oa::app::root_prefix_rotation_scale + 1, 0};
    OA_CHECK(same(oa::app::rotate_root_prefix_point({1, 2, 3}, invalid), {}));
    OA_CHECK(same(oa::app::place_root_prefix_point({1, 2, 3}, invalid, {4, 5, 6}), {}));
    const auto before_mode = std::fegetround();
    OA_CHECK(std::fesetround(FE_DOWNWARD) == 0);
    OA_CHECK(!oa::app::prepare_root_prefix_rotation(1117));
    OA_CHECK(same(oa::app::rotate_root_prefix_point({3, 17, -3}, halve), {2, 17, -2}));
    OA_CHECK(std::fesetround(before_mode) == 0);
}

/// Checks exact root-offset signs and wrapping before the unplaced packet is made.
void test_root_placement() {
    auto model = model_with(1);
    model->objects[0].offset_from_parent = {INT32_MIN, INT32_MAX, INT32_MIN};
    auto source = oa::sim::model_runtime::make_instance(model);
    source.pieces()[0].translation = {1, 1, -1};
    const auto placed = oa::app::root_prefix_placement(source);
    OA_CHECK(placed.has_value());
    if (placed)
        OA_CHECK(same(*placed, {INT32_MIN + 1, INT32_MIN, INT32_MAX}));
    OA_CHECK(!oa::app::root_prefix_placement({}));
}

/// Checks owned point order, source immutability and agreement after arbitrary final turns.
void test_owned_recipe_and_boxes() {
    auto model = model_with();
    auto source = zero_prefix(model);
    OA_CHECK(source.root_piece() != 0);
    auto unchanged = source;
    const auto old_placement = oa::app::root_prefix_placement(source);
    OA_CHECK(old_placement.has_value());
    if (!old_placement)
        return;
    const auto geometry = oa::app::make_root_prefix_geometry(source, *old_placement, packet_budget);
    OA_CHECK(geometry != nullptr);
    if (!geometry)
        return;
    OA_CHECK(geometry->storage_bytes() <= packet_budget);
    OA_CHECK(geometry->objects.size() == source.pieces().size());
    OA_CHECK(geometry->piece_vertices.size() == source.pieces().size() + 1);
    OA_CHECK(geometry->points.size() == 12);
    for (size_t piece = 0; piece < source.pieces().size(); ++piece)
        OA_CHECK(geometry->objects[piece] == source.pieces()[piece].object_index);
    constexpr std::array<uint16_t, 8> turns{0, 1, 1117, 16384, 24001, 32768, 49152, 65535};
    for (const auto word : turns) {
        const auto angle = std::bit_cast<int16_t>(word);
        const auto rotation = oa::app::prepare_root_prefix_rotation(angle);
        OA_CHECK(rotation.has_value());
        if (!rotation)
            continue;
        auto native = source;
        auto& root = native.pieces()[native.root_piece()];
        root.translation = {71 * fixed, -13 * fixed, -81 * fixed};
        const RotationWords effective{
            321,
            std::bit_cast<int16_t>(
                static_cast<uint16_t>(word - static_cast<uint16_t>(root.rotation.xz))
            ),
            17
        };
        const auto placement = oa::app::root_prefix_placement(native);
        OA_CHECK(placement.has_value());
        if (!placement)
            continue;
        const RootPrefixRecipe recipe{geometry, *rotation, *placement};
        native.rebuild_transforms(effective);
        for (uint32_t piece = 0; piece < native.pieces().size(); ++piece) {
            const auto bounds = oa::app::root_prefix_piece_bounds(recipe, piece);
            OA_CHECK(bounds.has_value());
            for (uint32_t vertex = 0; vertex < native.pieces()[piece].transformed_vertices.size();
                 ++vertex) {
                const auto actual = oa::app::root_prefix_point(recipe, piece, vertex);
                const auto expected = native.pieces()[piece].transformed_vertices[vertex];
                OA_CHECK(std::abs(int64_t{actual.x} - expected.x) <= 1);
                OA_CHECK(std::abs(int64_t{actual.z} - expected.z) <= 1);
                OA_CHECK(actual.y == expected.y);
                if (bounds)
                    OA_CHECK(contains(*bounds, actual));
            }
        }
    }
    check_unchanged(source, unchanged);
    const auto saved_first = geometry->points.front();
    const std::weak_ptr<const Model> retained_model = geometry->model;
    unchanged = {};
    model.reset();
    source = {};
    OA_CHECK(!retained_model.expired());
    OA_CHECK(same(geometry->points.front(), saved_first));
    OA_CHECK(same(oa::app::root_prefix_point({}, 0, 0), {}));
    const RootPrefixRecipe valid{geometry, {}, {}};
    OA_CHECK(same(oa::app::root_prefix_point(valid, UINT32_MAX, 0), {}));
    OA_CHECK(same(oa::app::root_prefix_point(valid, 0, UINT32_MAX), {}));
    OA_CHECK(!oa::app::root_prefix_piece_bounds(valid, UINT32_MAX));
    OA_CHECK(!oa::app::root_prefix_piece_bounds({geometry, {}, {INT32_MAX, 0, 0}}, 0));
}

/// Checks bounded admission, stale inputs, empty pieces and allocation budgets.
void test_packet_refusals() {
    auto model = model_with();
    auto source = zero_prefix(model);
    const auto placement = oa::app::root_prefix_placement(source);
    OA_CHECK(placement.has_value());
    if (!placement)
        return;
    OA_CHECK(!oa::app::make_root_prefix_geometry({}, {}, packet_budget));
    OA_CHECK(!oa::app::make_root_prefix_geometry(source, *placement, 0));
    OA_CHECK(!oa::app::make_root_prefix_geometry(source, {1, 2, 3}, packet_budget));
    auto stale = source;
    ++stale.pieces()[0].translation.x;
    OA_CHECK(!oa::app::make_root_prefix_geometry(stale, *placement, packet_budget));
    auto short_points = source;
    short_points.pieces()[0].transformed_vertices.pop_back();
    OA_CHECK(!oa::app::make_root_prefix_geometry(short_points, *placement, packet_budget));
    auto outside = source;
    outside.pieces()[0].transformed_vertices[0].x =
        placement->x + oa::app::root_prefix_coordinate_limit + 1;
    OA_CHECK(!oa::app::make_root_prefix_geometry(outside, *placement, packet_budget));
    auto sibling_model = model_with(2);
    sibling_model->objects[0].first_child = oa::formats::objects3d::kNoObject;
    sibling_model->objects[0].next_sibling = 1;
    sibling_model->objects[1].parent = oa::formats::objects3d::kNoObject;
    const auto sibling = zero_prefix(sibling_model);
    const auto sibling_placement = oa::app::root_prefix_placement(sibling);
    OA_CHECK(sibling_placement.has_value());
    if (sibling_placement)
        OA_CHECK(!oa::app::make_root_prefix_geometry(sibling, *sibling_placement, packet_budget));
    auto empty_model = model_with();
    empty_model->objects[2].vertices.clear();
    const auto empty_piece = zero_prefix(empty_model);
    const auto empty_placement = oa::app::root_prefix_placement(empty_piece);
    OA_CHECK(empty_placement.has_value());
    if (!empty_placement)
        return;
    const auto geometry =
        oa::app::make_root_prefix_geometry(empty_piece, *empty_placement, packet_budget);
    OA_CHECK(geometry != nullptr);
    if (geometry) {
        const RootPrefixRecipe recipe{geometry, {}, {}};
        OA_CHECK(geometry->objects.front() == 2);
        OA_CHECK(!oa::app::root_prefix_piece_bounds(recipe, 0));
        OA_CHECK(same(oa::app::root_prefix_point(recipe, 0, 0), {}));
        const auto bytes = geometry->storage_bytes();
        OA_CHECK(
            oa::app::make_root_prefix_geometry(empty_piece, *empty_placement, bytes) != nullptr
        );
        OA_CHECK(!oa::app::make_root_prefix_geometry(empty_piece, *empty_placement, bytes - 1));
    }
}

} // namespace

int main() {
    OA_CHECK(std::fegetround() == FE_TONEAREST);
    test_native_error_bound();
    test_rounding_and_placement();
    test_root_placement();
    test_owned_recipe_and_boxes();
    test_packet_refusals();
    return oa::test::check_exit_status();
}
