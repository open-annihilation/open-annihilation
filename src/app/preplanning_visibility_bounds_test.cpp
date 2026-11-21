// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The proof is checked against freshly rebuilt native vertices, never against
// another interval transform or the receiver's stale transformed buffers.
#include "preplanning_visibility_bounds.hpp"
#include "presentation_interpolation.hpp"

#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
thread_local bool refuse_allocation{};
thread_local std::size_t refused_allocations{};
} // namespace

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

/// Refuses a single allocation only inside the explicit recovery controls.
void* operator new(std::size_t bytes) {
    if (refuse_allocation) {
        refuse_allocation = false;
        ++refused_allocations;
        throw std::bad_alloc{};
    }
    if (void* result = std::malloc(bytes == 0 ? 1 : bytes))
        return result;
    throw std::bad_alloc{};
}

/// Routes array allocation through the same one-shot control.
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}

/// Releases ordinary storage.
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

/// Releases array storage.
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

/// Releases sized ordinary storage.
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

/// Releases sized array storage.
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
namespace proof = oa::app::preplanning_visibility;
namespace runtime = oa::sim::model_runtime;
namespace drawing = oa::present::model;
using oa::formats::objects3d::FixedVector3;
using oa::formats::objects3d::Model;
using runtime::Instance;
using runtime::RotationWords;
constexpr int32_t fixed = 65536;
constexpr uint16_t visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
constexpr uint16_t cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
constexpr std::array<std::string_view, 6> cob_order{
    "tip", "side_tip", "base", "detached", "lower", "side"
};

bool same(FixedVector3 a, FixedVector3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/// A branched hierarchy with an empty hidden parent, short pieces and a root sibling.
std::shared_ptr<const Model> branched_model() {
    auto model = std::make_shared<Model>();
    model->objects.resize(6);
    constexpr std::array names{"base", "lower", "tip", "side", "side_tip", "detached"};
    constexpr std::array<std::size_t, 6> counts{4, 0, 1, 2, 7, 3};
    for (std::size_t object = 0; object < model->objects.size(); ++object) {
        auto& part = model->objects[object];
        part.name = names[object];
        part.offset_from_parent = {
            static_cast<int32_t>(object) * fixed / 3 + 13,
            static_cast<int32_t>(object) * -fixed / 4 - 7,
            static_cast<int32_t>(object) * fixed / 5 + 19
        };
        for (std::size_t vertex = 0; vertex < counts[object]; ++vertex)
            part.vertices.push_back(
                {static_cast<int32_t>(vertex) * fixed / 2 - 2 * fixed + 3,
                 static_cast<int32_t>((vertex + object) % 3) * fixed - 11,
                 static_cast<int32_t>(vertex % 4) * -fixed / 3 + 23}
            );
    }
    model->objects[0].first_child = 1;
    model->objects[0].next_sibling = 5;
    model->objects[1].parent = 0;
    model->objects[1].first_child = 2;
    model->objects[1].next_sibling = 3;
    model->objects[2].parent = 1;
    model->objects[3].parent = 0;
    model->objects[3].first_child = 4;
    model->objects[4].parent = 3;
    return model;
}

/// Reconstructs only raw pose inputs in the same COB order, then uses native rebuild.
Instance native_oracle(
    const Instance& posed, RotationWords root, std::span<const std::string_view> names = {}
) {
    auto fresh = runtime::make_instance(posed.model_handle(), posed.owner_token(), names);
    OA_CHECK(fresh.pieces().size() == posed.pieces().size());
    for (std::size_t index = 0; index < fresh.pieces().size(); ++index) {
        OA_CHECK(fresh.pieces()[index].object_index == posed.pieces()[index].object_index);
        fresh.pieces()[index].translation = posed.pieces()[index].translation;
        fresh.pieces()[index].rotation = posed.pieces()[index].rotation;
        fresh.pieces()[index].flags = posed.pieces()[index].flags;
    }
    fresh.rebuild_transforms(root);
    return fresh;
}

/// Makes stale/reset geometry unusable as an accidental oracle or proof input.
void poison(Instance& instance) {
    for (std::size_t index = 0; index < instance.pieces().size(); ++index) {
        auto& piece = instance.pieces()[index];
        piece.transformed_vertices.clear();
        if (index % 2 != 0)
            piece.transformed_vertices.push_back({123456789, -987654321, 777777777});
        piece.transformed_origin = {-1, 2, -3};
        piece.transform_marker = 65535;
    }
}

/// The geometry-only query must not mutate raw or stale native instance state.
void unchanged(const Instance& instance, const Instance& saved) {
    OA_CHECK(instance.owner_token() == saved.owner_token());
    OA_CHECK(instance.transforms_dirty() == saved.transforms_dirty());
    OA_CHECK(instance.pieces().size() == saved.pieces().size());
    for (std::size_t index = 0; index < instance.pieces().size(); ++index) {
        const auto& a = instance.pieces()[index];
        const auto& b = saved.pieces()[index];
        OA_CHECK(same(a.translation, b.translation));
        OA_CHECK(
            a.rotation.xy == b.rotation.xy && a.rotation.xz == b.rotation.xz &&
            a.rotation.yz == b.rotation.yz
        );
        OA_CHECK(a.flags == b.flags && a.object_index == b.object_index);
        OA_CHECK(
            a.parent == b.parent && a.first_child == b.first_child &&
            a.next_sibling == b.next_sibling
        );
        OA_CHECK(a.transform_marker == b.transform_marker);
        OA_CHECK(same(a.transformed_origin, b.transformed_origin));
        OA_CHECK(a.transformed_vertices.size() == b.transformed_vertices.size());
        if (a.transformed_vertices.size() == b.transformed_vertices.size())
            for (std::size_t point = 0; point < a.transformed_vertices.size(); ++point)
                OA_CHECK(same(a.transformed_vertices[point], b.transformed_vertices[point]));
    }
}

/// Checks only the native visible vertices, including visible one/two-vertex pieces.
bool encloses(const proof::FixedBounds& box, const Instance& native) {
    bool nonempty{};
    for (const auto& piece : native.pieces())
        if ((piece.flags & visible) != 0)
            for (const auto& point : piece.transformed_vertices) {
                nonempty = true;
                OA_CHECK(box.low.x <= point.x && point.x <= box.high.x);
                OA_CHECK(box.low.y <= point.y && point.y <= box.high.y);
                OA_CHECK(box.low.z <= point.z && point.z <= box.high.z);
            }
    return nonempty;
}

/// Queries first, then builds a separate native receiver from raw fields alone.
bool check_pose(
    const proof::Geometry& geometry,
    Instance& posed,
    RotationWords root,
    std::span<const std::string_view> names = {},
    bool required = true
) {
    poison(posed);
    const auto saved = posed;
    proof::Scratch scratch;
    const auto box = proof::bound_instance(geometry, posed, root, scratch);
    unchanged(posed, saved);
    const auto native = native_oracle(posed, root, names);
    const auto completed = proof::bound_completed_instance(native, scratch);
    OA_CHECK(completed.has_value());
    if (completed)
        OA_CHECK(encloses(*completed, native));
    if (required)
        OA_CHECK(box.has_value());
    if (box)
        OA_CHECK(encloses(*box, native));
    return box.has_value();
}

/// Gives repeatable signed words without consuming simulation randomness.
uint32_t next_word(uint32_t& value) {
    value = value * 1664525U + 1013904223U;
    return value;
}

void native_hierarchy_and_pose_corpus() {
    const auto model = branched_model();
    const auto geometry = proof::prepare_geometry(model);
    OA_CHECK(geometry.model == model && geometry.objects.size() == model->objects.size());
    uint32_t random = 0x16b0a57U;
    for (const bool reordered : {false, true}) {
        const auto names = reordered ? std::span<const std::string_view>(cob_order)
                                     : std::span<const std::string_view>{};
        auto posed = runtime::make_instance(model, 19, names);
        for (unsigned sample = 0; sample < 96; ++sample) {
            for (auto& piece : posed.pieces()) {
                const auto coordinate = [&] {
                    return static_cast<int32_t>(next_word(random) % (32U * fixed)) - 16 * fixed;
                };
                piece.translation = {coordinate(), coordinate(), coordinate()};
                piece.rotation = {
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random)))
                };
                piece.flags = static_cast<uint16_t>(visible | (sample % 2 == 0 ? cached : 0));
                if (piece.object_index == 1 || (sample % 5 == 0 && piece.object_index == 3))
                    piece.flags = 0; // An invisible parent still transforms its visible children.
            }
            const RotationWords root{
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random)))
            };
            check_pose(geometry, posed, root, names);
        }
    }
    auto deep = std::make_shared<Model>();
    deep->objects.resize(24);
    for (std::size_t index = 0; index < deep->objects.size(); ++index) {
        auto& object = deep->objects[index];
        object.name = "deep" + std::to_string(index);
        object.vertices = {{-fixed, fixed / 2, 0}, {fixed, 0, fixed}, {0, -fixed, -fixed}};
        object.offset_from_parent = {31, -19, 11};
        if (index != 0)
            object.parent = static_cast<uint32_t>(index - 1);
        if (index + 1 < deep->objects.size())
            object.first_child = static_cast<uint32_t>(index + 1);
    }
    const auto deep_geometry = proof::prepare_geometry(deep);
    auto instance = runtime::make_instance(deep);
    check_pose(deep_geometry, instance, {});
    for (auto& piece : instance.pieces())
        piece.rotation = {8191, -16383, 2731};
    // Reboxing deep rotating chains may conservatively refuse; it must never underbound.
    check_pose(deep_geometry, instance, {32767, -32768, 16384}, {}, false);
}

void interpolated_raw_poses_use_the_existing_helpers() {
    const auto model = branched_model();
    const auto geometry = proof::prepare_geometry(model);
    auto previous = runtime::make_instance(model, 41, cob_order);
    auto current = runtime::make_instance(model, 41, cob_order);
    for (std::size_t index = 0; index < current.pieces().size(); ++index) {
        previous.pieces()[index].translation = {-9 * fixed, static_cast<int32_t>(index) * 37, 13};
        previous.pieces()[index].rotation = {30000, -31000, 123};
        current.pieces()[index].translation = {11 * fixed, -77, static_cast<int32_t>(index) * 101};
        current.pieces()[index].rotation = {-30000, 31000, -923};
        previous.pieces()[index].flags = current.pieces()[index].flags = visible | cached;
    }
    oa::Unit first{}, last{};
    first.position = {100 * fixed, 32 * fixed, 80 * fixed};
    last.position = {104 * fixed, 36 * fixed, 91 * fixed};
    first.heading = 65000;
    last.heading = 1100;
    first.bank = -193;
    last.pitch = 291;
    oa::app::UnitPose before, after;
    std::vector<int32_t> turns(current.pieces().size() * 3, 0);
    oa::app::capture_unit_pose(first, previous, before, turns);
    oa::app::capture_unit_pose(last, current, after, turns);
    // One axis makes two complete revolutions between identical endpoints.
    after.pieces[0].rotation.xy = before.pieces[0].rotation.xy;
    after.pieces[0].turn_per_tick[0] = 65536;
    for (const uint32_t fraction : {0U, 1U, 8192U, 32768U, 57344U, oa::app::whole_tick}) {
        auto resolved = runtime::make_instance(model, 41, cob_order);
        for (std::size_t index = 0; index < resolved.pieces().size(); ++index)
            resolved.pieces()[index].flags = current.pieces()[index].flags;
        auto record = last;
        oa::app::blend_unit_pose(before, after, fraction, record, resolved, 2);
        check_pose(
            geometry,
            resolved,
            {record.bank, static_cast<int16_t>(record.heading), record.pitch},
            cob_order
        );
        // A newly primed rig uses its current local pieces, with root-only blending.
        auto primed = runtime::make_instance(model, 41, cob_order);
        for (std::size_t index = 0; index < primed.pieces().size(); ++index)
            primed.pieces()[index].flags = current.pieces()[index].flags;
        oa::app::blend_unit_root_pose(before, after, fraction, record);
        oa::app::place_current_unit_pieces(after, primed);
        check_pose(
            geometry,
            primed,
            {record.bank, static_cast<int16_t>(record.heading), record.pitch},
            cob_order
        );
    }
}

/// Exact visible native vertices projected as the normal body and ordinary cast.
struct NativeVisibility {
    bool body_outside{}, shadow_outside{}, markers_outside{}, reservation_outside{};
};

NativeVisibility scan_native(
    const Instance& native, const oa::Unit& unit, const proof::FrameView& view, int32_t ground
) {
    float min_x = std::numeric_limits<float>::infinity(), min_y = min_x;
    float max_x = -min_x, max_y = -min_x;
    float shadow_left = min_x, shadow_top = min_x, shadow_right = -min_x, shadow_bottom = -min_x;
    std::array world_low{min_x, min_x, min_x}, world_high{-min_x, -min_x, -min_x};
    const std::array origin{
        unit.position.x / float(fixed),
        unit.position.y / float(fixed),
        unit.position.z / float(fixed)
    };
    const auto& p = view.projection;
    std::size_t points{};
    for (const auto& piece : native.pieces())
        if ((piece.flags & visible) != 0)
            for (const auto& point : piece.transformed_vertices) {
                ++points;
                const float x = origin[0] + point.x / float(fixed);
                const float height = origin[1] + point.y / float(fixed);
                const float z = origin[2] - point.z / float(fixed);
                const std::array world{x, height, z};
                for (std::size_t axis = 0; axis < world.size(); ++axis) {
                    world_low[axis] = std::min(world_low[axis], world[axis]);
                    world_high[axis] = std::max(world_high[axis], world[axis]);
                }
                const float across = p[0] * x + p[1] * height + p[2] * z + p[3];
                const float down = p[4] * x + p[5] * height + p[6] * z + p[7];
                min_x = std::min(min_x, across);
                max_x = std::max(max_x, across);
                min_y = std::min(min_y, down);
                max_y = std::max(max_y, down);
                const float laid_x =
                    origin[0] + (drawing::shadow_offset_x + std::floor(x - origin[0]));
                const float laid_y =
                    (origin[2] + -float(ground) * 0.5F) +
                    (std::floor(z - origin[2]) - std::floor(std::floor(height - origin[1]) * 0.5F));
                shadow_left = std::min(shadow_left, laid_x);
                shadow_right = std::max(shadow_right, laid_x);
                shadow_top = std::min(shadow_top, laid_y);
                shadow_bottom = std::max(shadow_bottom, laid_y);
            }
    OA_CHECK(points != 0);
    const float margin_x = std::abs(p[0]) + std::abs(p[1]) + std::abs(p[2]);
    const float margin_y = std::abs(p[4]) + std::abs(p[5]) + std::abs(p[6]);
    const float shadow_margin = 2 * view.shadow_scale + 1;
    // The ordinary marker uses this inverse-camera basis and the midpoint
    // of the actual posed vertices. No interval-bound center is used here.
    const std::array a{p[0], p[1], p[2]}, b{p[4], p[5], p[6]}, c{p[8], p[9], p[10]};
    std::array across{
        b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]
    };
    std::array down{
        c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]
    };
    const float determinant = a[0] * across[0] + a[1] * across[1] + a[2] * across[2];
    OA_CHECK(std::isfinite(determinant) && std::abs(determinant) >= 1e-12F);
    for (std::size_t axis = 0; axis < across.size(); ++axis) {
        across[axis] /= determinant;
        down[axis] /= determinant;
    }
    bool markers_outside = true;
    for (const float radius : {0.0F, 2.5F, 32.0F}) {
        std::array<float, 3> center{};
        for (std::size_t axis = 0; axis < center.size(); ++axis)
            center[axis] = (world_low[axis] + world_high[axis]) * 0.5F;
        center[1] += radius * (std::abs(across[1]) + std::abs(down[1])) + 0.25F;
        const float sx = p[0] * center[0] + p[1] * center[1] + p[2] * center[2] + p[3];
        const float sy = p[4] * center[0] + p[5] * center[1] + p[6] * center[2] + p[7];
        markers_outside &= sx + radius < 0 || sy + radius < 0 || sx - radius > view.width ||
                           sy - radius > view.height;
    }
    // The existing detail pass reserves no body only after this native
    // translation-first bbox/center/extent test. It is a separate obligation
    // from exact polygon or billboard visibility.
    bool reservation_outside = false;
    std::array<float, 3> world_center{};
    for (std::size_t axis = 0; axis < world_center.size(); ++axis)
        world_center[axis] = (world_low[axis] + world_high[axis]) * 0.5F;
    for (std::size_t row = 0; row < 2; ++row) {
        float extent{}, center = p[row * 4 + 3], margin{};
        for (std::size_t axis = 0; axis < world_low.size(); ++axis) {
            extent += std::abs(p[row * 4 + axis]) * (world_high[axis] - world_low[axis]);
            center += p[row * 4 + axis] * world_center[axis];
            margin += std::abs(p[row * 4 + axis]);
        }
        const float edge = row == 0 ? view.width : view.height;
        reservation_outside |=
            center + extent * 0.5F + margin < 0 || center - extent * 0.5F - margin > edge;
    }
    return {
        max_x + margin_x < 0 || min_x - margin_x > view.width || max_y + margin_y < 0 ||
            min_y - margin_y > view.height,
        shadow_right < view.shadow_bounds[0] - shadow_margin ||
            shadow_left > view.shadow_bounds[2] + shadow_margin ||
            shadow_bottom < view.shadow_bounds[1] - shadow_margin ||
            shadow_top > view.shadow_bounds[3] + shadow_margin,
        markers_outside,
        reservation_outside
    };
}

struct FrameFixture {
    std::shared_ptr<const Model> model = branched_model();
    proof::Geometry geometry = proof::prepare_geometry(model);
    oa::Unit unit{};
    oa::UnitDef definition{};
    drawing::PreparedModel prepared{};
    drawing::ModelState state{};
    Instance instance =
        runtime::make_instance(model, reinterpret_cast<uintptr_t>(&unit), cob_order);
    proof::Scratch scratch{};
    proof::FrameView view{};
    proof::Eligibility gate{};
    RotationWords root{};

    FrameFixture() {
        unit.id = unit.type_index = 1;
        unit.position = {200 * fixed, 20 * fixed, 200 * fixed};
        view.projection = {1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0.5F, 0};
        view.shadow_bounds = {0, 0, 64, 64};
        view.width = view.height = 64;
        view.frame = 17;
        gate.enhanced = gate.local = gate.rig_resolved = gate.ground_resolved = true;
        gate.gpu_type_supported = gate.budget_safe = gate.native_geometry_resolved = true;
        gate.fog_seen = true;
        gate.ordinary_model_path = true;
        gate.frame = view.frame;
        gate.owner_token = instance.owner_token();
        gate.generation = gate.rig_generation = 7;
    }

    drawing::ModelRef ref() { return {&instance, &prepared, &state, &unit, &definition}; }

    bool reject() { return proof::rejection_proof(geometry, ref(), root, view, gate, scratch); }
};

void exact_native_body_and_shadow_oracle() {
    FrameFixture fixture;
    std::size_t accepted{};
    for (const bool translated : {false, true})
        for (const float shadow_scale : {1.0F, 4.0F})
            for (const int32_t ground : {0, 63}) {
                const int32_t origin = translated ? 7168 : 0;
                fixture.view.projection =
                    translated
                        ? std::array<
                              float,
                              12>{0, 0, 1, -float(origin), 1, -0.5F, 0, -float(origin), 0, 1, 0, 0}
                        : std::array<float, 12>{1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0.5F, 0};
                fixture.view.shadow_bounds = {
                    float(origin), float(origin), float(origin + 64), float(origin + 64)
                };
                fixture.view.shadow_scale = shadow_scale;
                fixture.gate.ground_height = ground;
                fixture.root = {2731, -8192, 511};
                for (const int32_t x : {-320, -20, -5, 0, 32, 64, 70, 320})
                    for (const int32_t z : {-320, -2, 24, 64, 320})
                        for (const uint32_t fraction : {0U, 1U, 16384U, 32768U, 65535U}) {
                            fixture.unit.position = {
                                (origin + x) * fixed + static_cast<int32_t>(fraction),
                                (ground + 20) * fixed,
                                (origin + z) * fixed - static_cast<int32_t>(fraction)
                            };
                            poison(fixture.instance);
                            const auto saved = fixture.instance;
                            const bool rejected = fixture.reject();
                            unchanged(fixture.instance, saved);
                            const auto native =
                                native_oracle(fixture.instance, fixture.root, cob_order);
                            const auto actual =
                                scan_native(native, fixture.unit, fixture.view, ground);
                            if (rejected) {
                                ++accepted;
                                OA_CHECK(
                                    actual.body_outside && actual.shadow_outside &&
                                    actual.markers_outside && actual.reservation_outside
                                );
                            }
                            if (!actual.body_outside || !actual.shadow_outside ||
                                !actual.markers_outside)
                                OA_CHECK(!rejected);
                        }
            }
    OA_CHECK(accepted != 0);
    fixture.view = FrameFixture{}.view;
    fixture.gate.ground_height = 0;
    fixture.root = {};
    // Both native body and laid shadow are outside, but the largest
    // supported budget marker still reaches the viewport from the left.
    fixture.unit.position = {-20 * fixed, 20 * fixed, 32 * fixed};
    const auto marker_native = native_oracle(fixture.instance, {}, cob_order);
    const auto marker_visibility = scan_native(marker_native, fixture.unit, fixture.view, 0);
    OA_CHECK(marker_visibility.body_outside && marker_visibility.shadow_outside);
    OA_CHECK(!marker_visibility.markers_outside);
    OA_CHECK(!fixture.reject());
    // A body above the viewport still casts directly onto visible ground.
    fixture.unit.position = {32 * fixed, 400 * fixed, 32 * fixed};
    const auto native = native_oracle(fixture.instance, {}, cob_order);
    const auto visibility = scan_native(native, fixture.unit, fixture.view, 0);
    OA_CHECK(visibility.body_outside && !visibility.shadow_outside);
    OA_CHECK(!fixture.reject());
    // Conversely the body is visible while a deliberately distant shadow crop is not.
    fixture.unit.position = {32 * fixed, 20 * fixed, 32 * fixed};
    fixture.view.shadow_bounds = {1000, 1000, 1064, 1064};
    OA_CHECK(!fixture.reject());
}

void interpolated_native_projection_oracle() {
    FrameFixture fixture;
    auto first = runtime::make_instance(fixture.model, fixture.instance.owner_token(), cob_order);
    auto last = runtime::make_instance(fixture.model, fixture.instance.owner_token(), cob_order);
    for (std::size_t index = 0; index < first.pieces().size(); ++index) {
        first.pieces()[index].translation = {-3 * fixed, static_cast<int32_t>(index) * 17, 123};
        last.pieces()[index].translation = {5 * fixed, -73, static_cast<int32_t>(index) * -91};
        first.pieces()[index].rotation = {30000, -31000, 993};
        last.pieces()[index].rotation = {-30000, 31000, -447};
        first.pieces()[index].flags = last.pieces()[index].flags = visible | cached;
        fixture.instance.pieces()[index].flags = visible | cached;
    }
    auto before_record = fixture.unit, after_record = fixture.unit;
    after_record.position.x += 7 * fixed;
    before_record.heading = 64000;
    after_record.heading = 1200;
    before_record.bank = -431;
    after_record.pitch = 293;
    oa::app::UnitPose before, after;
    oa::app::capture_unit_pose(before_record, first, before);
    oa::app::capture_unit_pose(after_record, last, after);
    after.pieces[0].rotation.xy = before.pieces[0].rotation.xy;
    after.pieces[0].turn_per_tick[0] = 65536;
    std::size_t rejected{};
    for (const uint32_t fraction : {0U, 1U, 8192U, 32768U, 57344U, oa::app::whole_tick}) {
        fixture.unit = after_record;
        oa::app::blend_unit_pose(before, after, fraction, fixture.unit, fixture.instance, 2);
        fixture.root = {
            fixture.unit.bank, static_cast<int16_t>(fixture.unit.heading), fixture.unit.pitch
        };
        poison(fixture.instance);
        const bool result = fixture.reject();
        const auto native = native_oracle(fixture.instance, fixture.root, cob_order);
        const auto actual =
            scan_native(native, fixture.unit, fixture.view, fixture.gate.ground_height);
        if (result) {
            ++rejected;
            OA_CHECK(
                actual.body_outside && actual.shadow_outside && actual.markers_outside &&
                actual.reservation_outside
            );
        }
    }
    OA_CHECK(rejected != 0);
}

void unknown_policy_and_identity_keep_the_normal_path() {
    FrameFixture fixture;
    OA_CHECK(fixture.reject());
    const auto original = fixture.gate;
    for (bool proof::Eligibility::* field :
         {&proof::Eligibility::enhanced,
          &proof::Eligibility::local,
          &proof::Eligibility::rig_resolved,
          &proof::Eligibility::ground_resolved,
          &proof::Eligibility::gpu_type_supported,
          &proof::Eligibility::budget_safe,
          &proof::Eligibility::native_geometry_resolved,
          &proof::Eligibility::fog_seen,
          &proof::Eligibility::ordinary_model_path}) {
        fixture.gate = original;
        fixture.gate.*field = false;
        OA_CHECK(!fixture.reject());
    }
    for (bool proof::Eligibility::* field :
         {&proof::Eligibility::selected,
          &proof::Eligibility::debug,
          &proof::Eligibility::probe,
          &proof::Eligibility::reflection,
          &proof::Eligibility::complex,
          &proof::Eligibility::snapshot,
          &proof::Eligibility::vertex_override}) {
        fixture.gate = original;
        fixture.gate.*field = true;
        OA_CHECK(!fixture.reject());
    }
    fixture.gate = original;
    fixture.gate.frame++;
    OA_CHECK(!fixture.reject());
    fixture.gate = original;
    fixture.gate.rig_generation++;
    OA_CHECK(!fixture.reject());
    fixture.gate = original;
    fixture.gate.owner_token++;
    OA_CHECK(!fixture.reject());
    fixture.gate = original;
    fixture.unit.flags |= OA_UNIT_FLAG_SELECTED;
    OA_CHECK(!fixture.reject());
    fixture.unit.flags = 0;
    const auto view = fixture.view;
    fixture.view.projection.fill(0);
    OA_CHECK(!fixture.reject());
    fixture.view = view;
    for (std::size_t column = 0; column < 3; ++column)
        fixture.view.projection[8 + column] = fixture.view.projection[column];
    OA_CHECK(!fixture.reject()); // The first two rows alone do not certify full rank.
    fixture.view = view;
    fixture.view.projection[0] = std::numeric_limits<float>::infinity();
    OA_CHECK(!fixture.reject());
    fixture.view = view;
    fixture.view.shadow_scale = 0;
    OA_CHECK(!fixture.reject());
}

void ownership_topology_wrap_and_allocation_refusals() {
    const auto model = branched_model();
    const auto geometry = proof::prepare_geometry(model);
    auto instance = runtime::make_instance(model, 1, cob_order);
    proof::Scratch scratch;
    const std::shared_ptr<const Model> unrelated(model.get(), [](const Model*) {});
    const auto foreign_geometry = proof::prepare_geometry(unrelated);
    OA_CHECK(!proof::bound_instance(foreign_geometry, instance, {}, scratch));
    instance.pieces()[0].object_index = instance.pieces()[1].object_index;
    OA_CHECK(!proof::bound_instance(geometry, instance, {}, scratch));
    instance = runtime::make_instance(model, 1, cob_order);
    instance.pieces()[0].parent = 99999;
    OA_CHECK(!proof::bound_instance(geometry, instance, {}, scratch));
    auto extreme = std::make_shared<Model>();
    extreme->objects.resize(1);
    extreme->objects[0].vertices = {
        {std::numeric_limits<int32_t>::min(), 0, std::numeric_limits<int32_t>::min()},
        {0, 0, 0},
        {fixed, 0, fixed}
    };
    const auto extreme_geometry = proof::prepare_geometry(extreme);
    auto wrapped = runtime::make_instance(extreme);
    check_pose(extreme_geometry, wrapped, {}); // Reset negation preserves INT32_MIN.
    wrapped.pieces()[0].translation.x = -1;
    OA_CHECK(!proof::bound_instance(extreme_geometry, wrapped, {}, scratch));
    wrapped.pieces()[0].translation.x = 0;
    OA_CHECK(!proof::bound_instance(extreme_geometry, wrapped, {0, 8192, 0}, scratch));
    auto invalid = std::make_shared<Model>(*model);
    invalid->objects[1].parent = 1;
    OA_CHECK(!proof::prepare_geometry(invalid).model);
    const auto before_failures = refused_allocations;
    refuse_allocation = true;
    const auto failed_geometry = proof::prepare_geometry(model);
    OA_CHECK(!refuse_allocation && refused_allocations == before_failures + 1);
    OA_CHECK(!failed_geometry.model);
    instance = runtime::make_instance(model, 1, cob_order);
    proof::Scratch cold;
    refuse_allocation = true;
    const auto failed_bound = proof::bound_instance(geometry, instance, {}, cold);
    OA_CHECK(!refuse_allocation && refused_allocations == before_failures + 2);
    OA_CHECK(!failed_bound);
    OA_CHECK(proof::bound_instance(geometry, instance, {}, cold).has_value());
    const auto saved_control = oa::base::float_precision::save_float_control();
    if (std::fesetround(FE_DOWNWARD) == 0) {
        OA_CHECK(!proof::bound_instance(geometry, instance, {}, cold));
        oa::base::float_precision::restore_float_control(saved_control);
    }
}

/// Completed bounds read the actual freshly built buffers, with no raw-box approximation.
void completed_native_geometry_and_refusals() {
    const auto model = branched_model();
    auto native = runtime::make_instance(model, 71, cob_order);
    for (auto& piece : native.pieces()) {
        piece.flags |= visible;
        piece.rotation = {2731, -8191, 511};
        piece.translation = {fixed / 3, -37, fixed / 7};
    }
    native.rebuild_transforms({32767, -32768, 123});
    proof::Scratch scratch;
    const auto bound = proof::bound_completed_instance(native, scratch);
    OA_CHECK(bound.has_value());
    proof::FixedBounds exact{
        {std::numeric_limits<int32_t>::max(),
         std::numeric_limits<int32_t>::max(),
         std::numeric_limits<int32_t>::max()},
        {std::numeric_limits<int32_t>::min(),
         std::numeric_limits<int32_t>::min(),
         std::numeric_limits<int32_t>::min()}
    };
    for (const auto& piece : native.pieces())
        if ((piece.flags & visible) != 0)
            for (const auto& point : piece.transformed_vertices) {
                exact.low.x = std::min(exact.low.x, point.x);
                exact.low.y = std::min(exact.low.y, point.y);
                exact.low.z = std::min(exact.low.z, point.z);
                exact.high.x = std::max(exact.high.x, point.x);
                exact.high.y = std::max(exact.high.y, point.y);
                exact.high.z = std::max(exact.high.z, point.z);
            }
    if (bound)
        OA_CHECK(same(bound->low, exact.low) && same(bound->high, exact.high));
    const auto refused = [&](const Instance& altered) {
        const auto before = altered;
        OA_CHECK(!proof::bound_completed_instance(altered, scratch));
        unchanged(altered, before);
    };
    auto altered = native;
    altered.take_pose(native);
    refused(altered); // Marked dirty even with identical raw locals.
    altered = native;
    ++altered.pieces()[0].translation.x;
    refused(altered); // Raw locals differ from the built locals without a dirty setter.
    altered = native;
    ++altered.pieces()[0].rotation.xy;
    refused(altered);
    altered = native;
    altered.pieces()[0].transformed_vertices.pop_back();
    refused(altered);
    altered = native;
    altered.pieces()[0].object_index = altered.pieces()[1].object_index;
    refused(altered);
    altered = native;
    altered.pieces()[0].parent = 99999;
    refused(altered);
    altered = native;
    altered.pieces()[0].transform_marker = 1;
    refused(altered);
    const auto allocation_count = refused_allocations;
    proof::Scratch cold;
    refuse_allocation = true;
    OA_CHECK(!proof::bound_completed_instance(native, cold));
    OA_CHECK(!refuse_allocation && refused_allocations == allocation_count + 1);
    OA_CHECK(proof::bound_completed_instance(native, cold).has_value());

    FrameFixture fixture;
    fixture.instance.rebuild_transforms(fixture.root);
    fixture.state.transforms_dirty = false;
    oa::app::ModelDraw completed;
    completed.model = fixture.ref();
    completed.plan.linked.drawn = true;
    completed.plan.model.from_image = true;
    completed.plan.model.ground = fixture.gate.ground_height;
    completed.shown_position = fixture.unit.position;
    const auto rejects = [&](const oa::app::ModelDraw& draw) {
        return proof::completed_rejection_proof(draw, fixture.view, fixture.gate, fixture.scratch);
    };
    OA_CHECK(rejects(completed));
    const auto visible_native =
        scan_native(fixture.instance, fixture.unit, fixture.view, fixture.gate.ground_height);
    OA_CHECK(
        visible_native.body_outside && visible_native.shadow_outside &&
        visible_native.markers_outside && visible_native.reservation_outside
    );
    auto bad = completed;
    bad.preplan_omitted = true;
    OA_CHECK(!rejects(bad));
    bad = completed;
    bad.plan.linked.drawn = false;
    OA_CHECK(!rejects(bad));
    bad = completed;
    bad.plan.level = drawing::UnitSupersampling::x2;
    OA_CHECK(!rejects(bad));
    bad = completed;
    bad.plan.model.from_image = false;
    OA_CHECK(!rejects(bad));
    bad = completed;
    ++bad.plan.model.ground;
    OA_CHECK(!rejects(bad));
    bad = completed;
    bad.plan.model.carried.emplace_back();
    OA_CHECK(!rejects(bad));
    bad = completed;
    bad.shown_vertices.emplace_back();
    OA_CHECK(!rejects(bad));
    bad = completed;
    bad.shown_position.reset();
    OA_CHECK(!rejects(bad));
    bad = completed;
    ++bad.shown_position->x;
    OA_CHECK(!rejects(bad));
    fixture.state.build_bands.emplace();
    OA_CHECK(!rejects(completed));
    fixture.state.build_bands.reset();
    fixture.state.finer = std::make_unique<drawing::FinerModel>();
    OA_CHECK(!rejects(completed));
    fixture.state.finer.reset();
    fixture.state.transforms_dirty = true;
    OA_CHECK(!rejects(completed));
}

/// Makes the sibling fixture a single rooted tree without changing its COB order.
std::shared_ptr<const Model> far_model() {
    auto source = std::make_shared<Model>(*branched_model());
    source->objects[0].next_sibling = runtime::kNoPiece;
    source->objects[3].next_sibling = 5;
    source->objects[5].parent = 0;
    return source;
}

/// Validates the complete mapping and topology before computing any path radius.
std::optional<proof::FixedBounds> separate_far_bound(
    const proof::Geometry& geometry, const Instance& instance, proof::Scratch& scratch
) {
    constexpr std::size_t maximum_pieces = 256;
    constexpr uint64_t coordinate_limit = std::numeric_limits<int32_t>::max();
    const auto& handle = instance.model_handle();
    if (!geometry.model || !handle || geometry.model.get() != handle.get() ||
        geometry.model.owner_before(handle) || handle.owner_before(geometry.model) ||
        geometry.objects.size() != geometry.model->objects.size() ||
        !oa::base::float_precision::rounds_to_double() || std::fegetround() != FE_TONEAREST ||
        !oa::base::float_precision::float_control_matches(
            oa::base::float_precision::program_float_control().saved
        ))
        return std::nullopt;
    const auto pieces = instance.pieces();
    if (pieces.empty() || pieces.size() != geometry.objects.size() ||
        pieces.size() > maximum_pieces)
        return std::nullopt;
    auto& mapping = scratch.piece_for_object;
    mapping.assign(pieces.size(), runtime::kNoPiece);
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto object = pieces[index].object_index;
        if (object >= pieces.size() || mapping[object] != runtime::kNoPiece)
            return std::nullopt;
        mapping[object] = static_cast<uint32_t>(index);
    }
    const auto linked_piece = [&](uint32_t object) {
        return object == runtime::kNoPiece || object >= pieces.size() ? runtime::kNoPiece
                                                                      : mapping[object];
    };
    if (instance.root_piece() != mapping[0])
        return std::nullopt;
    for (const auto& piece : pieces) {
        const auto& object = geometry.model->objects[piece.object_index];
        if ((object.parent != runtime::kNoPiece && object.parent >= piece.object_index) ||
            (object.parent != runtime::kNoPiece && object.parent >= pieces.size()) ||
            (object.first_child != runtime::kNoPiece && object.first_child >= pieces.size()) ||
            (object.next_sibling != runtime::kNoPiece && object.next_sibling >= pieces.size()) ||
            piece.parent != linked_piece(object.parent) ||
            piece.first_child != linked_piece(object.first_child) ||
            piece.next_sibling != linked_piece(object.next_sibling))
            return std::nullopt;
    }
    if (geometry.model->objects[0].next_sibling != runtime::kNoPiece)
        return std::nullopt;
    const auto magnitude = [](int32_t word) -> uint64_t {
        return word < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(word))
                        : static_cast<uint64_t>(word);
    };
    auto& paths = scratch.far_radius_path;
    paths.resize(pieces.size());
    uint64_t radius{};
    bool nonempty{};
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto& object = geometry.model->objects[index];
        const auto& piece = pieces[mapping[index]];
        const auto& raw = geometry.objects[index];
        if (!raw.radius_ready || (index != 0 && object.parent == runtime::kNoPiece))
            return std::nullopt;
        const uint64_t parent = object.parent == runtime::kNoPiece ? 0 : paths[object.parent];
        const uint64_t translation =
            magnitude(object.offset_from_parent.x) + magnitude(object.offset_from_parent.y) +
            magnitude(object.offset_from_parent.z) + magnitude(piece.translation.x) +
            magnitude(piece.translation.y) + magnitude(piece.translation.z);
        const uint64_t added = translation + 6;
        if (added >= coordinate_limit || parent >= coordinate_limit - added)
            return std::nullopt;
        paths[index] = parent + added;
        if ((piece.flags & visible) == 0 || !raw.nonempty)
            continue;
        if (raw.l1_radius >= coordinate_limit - paths[index])
            return std::nullopt;
        radius = std::max(radius, raw.l1_radius + paths[index]);
        nonempty = true;
    }
    if (!nonempty)
        return std::nullopt;
    const auto bound = static_cast<int32_t>(radius);
    return proof::FixedBounds{{-bound, -bound, -bound}, {bound, bound, bound}};
}

/// Compares exact boxes and refusals with separate topology and radius passes.
void far_mapping_and_radius_reference_parity() {
    const auto source = far_model();
    const auto geometry = proof::prepare_geometry(source);
    proof::Scratch scratch, reference_scratch;
    const auto compare = [&](const proof::Geometry& bounds, const Instance& instance) {
        const auto expected = separate_far_bound(bounds, instance, reference_scratch);
        const auto actual = proof::bound_far_instance(bounds, instance, scratch);
        OA_CHECK(actual.has_value() == expected.has_value());
        if (actual && expected) {
            OA_CHECK(same(actual->low, expected->low));
            OA_CHECK(same(actual->high, expected->high));
        }
    };
    uint32_t words = 0x6943a17dU;
    for (const bool reordered : {false, true}) {
        const auto names = reordered ? std::span<const std::string_view>(cob_order)
                                     : std::span<const std::string_view>{};
        const auto original = runtime::make_instance(source, 31, names);
        compare(geometry, original);
        for (unsigned sample = 0; sample < 128; ++sample) {
            auto posed = original;
            for (auto& piece : posed.pieces()) {
                const auto coordinate = [&] {
                    return static_cast<int32_t>(next_word(words) % (64U * fixed)) - 32 * fixed;
                };
                piece.translation = {coordinate(), coordinate(), coordinate()};
                piece.rotation = {
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(words))),
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(words))),
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(words)))
                };
                piece.flags = sample % 8 == 0 || (next_word(words) & 1U) == 0 ? 0 : visible;
            }
            compare(geometry, posed);
        }
        for (std::size_t index = 0; index < original.pieces().size(); ++index) {
            auto changed = original;
            changed.pieces()[index].parent = 999;
            compare(geometry, changed);
            changed = original;
            changed.pieces()[index].first_child = static_cast<uint32_t>(index);
            compare(geometry, changed);
            changed = original;
            changed.pieces()[index].next_sibling = static_cast<uint32_t>(index);
            compare(geometry, changed);
            changed = original;
            changed.pieces()[index].object_index = runtime::kNoPiece;
            compare(geometry, changed);
            changed = original;
            changed.pieces()[index].object_index =
                original.pieces()[(index + 1) % original.pieces().size()].object_index;
            compare(geometry, changed);
            for (const int32_t coordinate :
                 {std::numeric_limits<int32_t>::min(),
                  std::numeric_limits<int32_t>::max(),
                  std::numeric_limits<int32_t>::max() / 2}) {
                changed = original;
                changed.pieces()[index].translation = {coordinate, coordinate, coordinate};
                compare(geometry, changed);
            }
            auto missing = geometry;
            missing.objects[index].radius_ready = false;
            compare(missing, original);
            missing = geometry;
            missing.objects[index].l1_radius = std::numeric_limits<uint64_t>::max();
            compare(missing, original);
            missing = geometry;
            missing.objects[index].nonempty = false;
            compare(missing, original);
        }
        auto root_mismatch = original;
        const auto root = root_mismatch.root_piece();
        const auto other = (root + 1) % root_mismatch.pieces().size();
        std::swap(root_mismatch.pieces()[root], root_mismatch.pieces()[other]);
        compare(geometry, root_mismatch);
        auto hidden = original;
        for (auto& piece : hidden.pieces())
            piece.flags = 0;
        compare(geometry, hidden);
        auto missing = geometry;
        missing.objects.pop_back();
        compare(missing, original);
        missing = geometry;
        missing.model.reset();
        compare(missing, original);
        const auto control = oa::base::float_precision::save_float_control();
        for (const int rounding : {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
            if (std::fesetround(rounding) == 0)
                compare(geometry, original);
            oa::base::float_precision::restore_float_control(control);
        }
    }
    const auto sibling = runtime::make_instance(branched_model(), 31, cob_order);
    compare(proof::prepare_geometry(sibling.model_handle()), sibling);
    compare(geometry, Instance{});
}

/// Checks the integer rounding margin at every angle word and large coordinates.
void far_plane_rounding_margin() {
    const auto squared = [](FixedVector3 point) {
        const auto square = [](int32_t word) {
            const auto value = static_cast<int64_t>(word);
            return static_cast<uint64_t>(value * value);
        };
        return square(point.x) + square(point.y) + square(point.z);
    };
    for (uint32_t word = 0; word < 65536; ++word) {
        const auto angle = std::bit_cast<int16_t>(static_cast<uint16_t>(word));
        for (const int32_t radius : {1, fixed + 13, std::numeric_limits<int32_t>::max() - 128}) {
            const auto plane = runtime::rotate_vector({radius, 0, 0}, {angle, 0, 0});
            const auto padded = static_cast<uint64_t>(radius) + 2;
            OA_CHECK(squared(plane) <= padded * padded);
        }
        const auto radius = std::numeric_limits<int32_t>::max() - 128;
        const auto turned = runtime::rotate_vector(
            {radius, 0, 0},
            {angle,
             std::bit_cast<int16_t>(static_cast<uint16_t>(word * 3)),
             std::bit_cast<int16_t>(static_cast<uint16_t>(word * 7))}
        );
        const auto padded = static_cast<uint64_t>(radius) + 6;
        OA_CHECK(squared(turned) <= padded * padded);
        // Pythagorean pairs exercise both products and cancellation near the
        // signed-coordinate limit, rather than only one nonzero input.
        const int32_t pair_scale = radius / 5;
        for (const int32_t sign : {-1, 1}) {
            const auto pair =
                runtime::rotate_vector({sign * 3 * pair_scale, 4 * pair_scale, 0}, {angle, 0, 0});
            const auto pair_bound = uint64_t{5} * pair_scale + 2;
            OA_CHECK(squared(pair) <= pair_bound * pair_bound);
        }
        const int32_t triple_scale = radius / 3;
        const auto triple = runtime::rotate_vector(
            {triple_scale, -2 * triple_scale, 2 * triple_scale},
            {angle,
             std::bit_cast<int16_t>(static_cast<uint16_t>(word * 3)),
             std::bit_cast<int16_t>(static_cast<uint16_t>(word * 7))}
        );
        const auto triple_bound = uint64_t{3} * triple_scale + 6;
        OA_CHECK(squared(triple) <= triple_bound * triple_bound);
    }
}

/// Compares raw integer-radius cubes with separate native hierarchical rebuilds.
void far_hierarchy_and_wrap_domains() {
    const auto source = far_model();
    const auto geometry = proof::prepare_geometry(source);
    proof::Scratch scratch;
    uint32_t random = 0x31568a21U;
    for (const bool reordered : {false, true}) {
        const auto names = reordered ? std::span<const std::string_view>(cob_order)
                                     : std::span<const std::string_view>{};
        auto posed = runtime::make_instance(source, 31, names);
        for (unsigned sample = 0; sample < 128; ++sample) {
            for (auto& piece : posed.pieces()) {
                const auto coordinate = [&] {
                    return static_cast<int32_t>(next_word(random) % (32U * fixed)) - 16 * fixed;
                };
                piece.translation = {coordinate(), coordinate(), coordinate()};
                piece.rotation = {
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                    std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random)))
                };
                piece.flags = piece.object_index == 1 ? 0 : visible;
            }
            const RotationWords root{
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random))),
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(random)))
            };
            poison(posed);
            const auto before = posed;
            const auto box = proof::bound_far_instance(geometry, posed, scratch);
            unchanged(posed, before);
            OA_CHECK(box.has_value());
            if (box)
                OA_CHECK(encloses(*box, native_oracle(posed, root, names)));
        }
    }
    auto sibling = runtime::make_instance(branched_model(), 0, cob_order);
    OA_CHECK(!proof::bound_far_instance(
        proof::prepare_geometry(sibling.model_handle()), sibling, scratch
    ));
    auto posed = runtime::make_instance(source, 0, cob_order);
    posed.pieces()[posed.root_piece()].translation.x = std::numeric_limits<int32_t>::min();
    OA_CHECK(!proof::bound_far_instance(geometry, posed, scratch));
    posed = runtime::make_instance(source, 0, cob_order);
    posed.pieces()[0].parent = 999;
    OA_CHECK(!proof::bound_far_instance(geometry, posed, scratch));
    const auto saved_rounding = std::fegetround();
    std::fesetround(FE_UPWARD);
    OA_CHECK(!proof::bound_far_instance(geometry, runtime::make_instance(source), scratch));
    std::fesetround(saved_rounding);
    proof::Scratch cold;
    auto allocation_pose = runtime::make_instance(source);
    refuse_allocation = true;
    OA_CHECK(!proof::bound_far_instance(geometry, allocation_pose, cold));
    OA_CHECK(!refuse_allocation);
    cold.piece_for_object.reserve(allocation_pose.pieces().size());
    refuse_allocation = true;
    OA_CHECK(!proof::bound_far_instance(geometry, allocation_pose, cold));
    OA_CHECK(!refuse_allocation);
    auto missing_radius = geometry;
    missing_radius.objects.front().radius_ready = false;
    OA_CHECK(!proof::bound_far_instance(missing_radius, allocation_pose, scratch));
    posed = runtime::make_instance(source, 0, cob_order);
    constexpr int32_t half_limit = std::numeric_limits<int32_t>::max() / 2;
    posed.pieces()[posed.root_piece()].translation.x = half_limit;
    for (auto& piece : posed.pieces())
        if (piece.object_index == 2)
            piece.translation.x = half_limit;
    OA_CHECK(!proof::bound_far_instance(geometry, posed, scratch));
}

/// Checks native offset signs and proves the complete rejection result independently.
void far_projection_and_translation_signs() {
    auto source = std::make_shared<Model>();
    source->objects.resize(1);
    source->objects[0].vertices = {{0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    source->objects[0].offset_from_parent.x = 10 * fixed;
    auto posed = runtime::make_instance(source);
    posed.pieces()[0].translation.x = -10 * fixed;
    proof::Scratch scratch;
    const auto box = proof::bound_far_instance(proof::prepare_geometry(source), posed, scratch);
    OA_CHECK(box && box->low.x <= -20 * fixed);
    if (box)
        OA_CHECK(encloses(*box, native_oracle(posed, {})));
    source->objects[0].vertices[0].x = std::numeric_limits<int32_t>::min();
    OA_CHECK(!proof::bound_far_instance(
        proof::prepare_geometry(source), runtime::make_instance(source), scratch
    ));

    FrameFixture fixture;
    fixture.model = far_model();
    fixture.geometry = proof::prepare_geometry(fixture.model);
    fixture.instance = runtime::make_instance(fixture.model, fixture.gate.owner_token, cob_order);
    size_t accepted{};
    for (const int32_t x : {-1600, -800, 32, 800, 1600})
        for (const int32_t z : {-1600, -800, 32, 800, 1600}) {
            fixture.unit.position.x = x * fixed;
            fixture.unit.position.z = z * fixed;
            if (proof::far_rejection_proof(
                    fixture.geometry, fixture.ref(), fixture.view, fixture.gate, fixture.scratch
                )) {
                const auto native = native_oracle(fixture.instance, fixture.root, cob_order);
                const auto visible_native =
                    scan_native(native, fixture.unit, fixture.view, fixture.gate.ground_height);
                OA_CHECK(
                    visible_native.body_outside && visible_native.markers_outside &&
                    visible_native.shadow_outside && visible_native.reservation_outside
                );
                ++accepted;
            }
        }
    OA_CHECK(accepted != 0);
    fixture.unit.position.x = fixture.unit.position.z = 32 * fixed;
    OA_CHECK(!proof::far_rejection_proof(
        fixture.geometry, fixture.ref(), fixture.view, fixture.gate, fixture.scratch
    ));
}

/// Compares the combined call with independent, separately validated legacy wrappers.
bool compare_raw_proofs(
    FrameFixture& fixture, proof::Scratch& combined, proof::Scratch& separate, bool try_far = true
) {
    const auto saved = fixture.instance;
    const auto revision = fixture.instance.materialization_revision();
    const bool expected =
        (try_far && proof::far_rejection_proof(
                        fixture.geometry, fixture.ref(), fixture.view, fixture.gate, separate
                    )) ||
        proof::rejection_proof(
            fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, separate
        );
    const bool actual = proof::raw_rejection_proof(
        fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, combined, try_far
    );
    OA_CHECK(actual == expected);
    unchanged(fixture.instance, saved);
    OA_CHECK(fixture.instance.materialization_revision() == revision);
    return actual;
}

/// Starts each comparison with independent cold scratch storage.
bool compare_raw_proofs(FrameFixture& fixture, bool try_far = true) {
    proof::Scratch combined, separate;
    return compare_raw_proofs(fixture, combined, separate, try_far);
}

/// Replaces only immutable model data and the instance that owns the fixture's unit.
void use_model(FrameFixture& fixture, std::shared_ptr<const Model> model) {
    fixture.model = std::move(model);
    fixture.geometry = proof::prepare_geometry(fixture.model);
    fixture.instance = runtime::make_instance(fixture.model, fixture.gate.owner_token);
    for (auto& piece : fixture.instance.pieces())
        piece.flags = visible;
    poison(fixture.instance);
}

/// Makes a shallow valid hierarchy beyond the far path's bounded piece count.
std::shared_ptr<const Model> many_piece_model(bool root_siblings) {
    auto model = std::make_shared<Model>();
    constexpr uint32_t count = 257;
    model->objects.resize(count);
    for (uint32_t index = 0; index < count; ++index) {
        auto& object = model->objects[index];
        object.name = "piece" + std::to_string(index);
        object.vertices = {{0, 0, 0}, {fixed, fixed, 0}, {0, 0, fixed}};
        if (root_siblings) {
            if (index + 1 < count)
                object.next_sibling = index + 1;
        } else if (index == 0) {
            object.first_child = 1;
        } else {
            object.parent = 0;
            if (index + 1 < count)
                object.next_sibling = index + 1;
        }
    }
    return model;
}

/// Keeps exact fallback after a valid far cube fails the complete screen predicate.
void combined_raw_far_and_exact_fallbacks() {
    FrameFixture fixture;
    use_model(fixture, far_model());
    proof::Scratch far;
    OA_CHECK(proof::bound_far_instance(fixture.geometry, fixture.instance, far));
    OA_CHECK(
        proof::far_rejection_proof(fixture.geometry, fixture.ref(), fixture.view, fixture.gate, far)
    );
    OA_CHECK(compare_raw_proofs(fixture));
    OA_CHECK(compare_raw_proofs(fixture, false));

    auto displaced = std::make_shared<Model>();
    displaced->objects.resize(1);
    displaced->objects[0].vertices = {
        {1000 * fixed, 0, 0}, {1001 * fixed, fixed, 0}, {1000 * fixed, 0, fixed}
    };
    use_model(fixture, displaced);
    proof::Scratch exact;
    OA_CHECK(proof::bound_far_instance(fixture.geometry, fixture.instance, far));
    OA_CHECK(!proof::far_rejection_proof(
        fixture.geometry, fixture.ref(), fixture.view, fixture.gate, far
    ));
    OA_CHECK(
        proof::rejection_proof(
            fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, exact
        )
    );
    OA_CHECK(compare_raw_proofs(fixture));
    const auto native = native_oracle(fixture.instance, fixture.root);
    const auto actual = scan_native(native, fixture.unit, fixture.view, fixture.gate.ground_height);
    OA_CHECK(
        actual.body_outside && actual.shadow_outside && actual.markers_outside &&
        actual.reservation_outside
    );

    use_model(fixture, branched_model());
    OA_CHECK(!proof::bound_far_instance(fixture.geometry, fixture.instance, far));
    OA_CHECK(compare_raw_proofs(fixture));
    for (const bool siblings : {false, true}) {
        use_model(fixture, many_piece_model(siblings));
        OA_CHECK(fixture.instance.pieces().size() == 257);
        OA_CHECK(!proof::bound_far_instance(fixture.geometry, fixture.instance, far));
        OA_CHECK(compare_raw_proofs(fixture));
        OA_CHECK(compare_raw_proofs(fixture, false));
    }

    use_model(fixture, far_model());
    fixture.geometry.objects.front().radius_ready = false;
    OA_CHECK(!proof::bound_far_instance(fixture.geometry, fixture.instance, far));
    OA_CHECK(compare_raw_proofs(fixture));
    auto cancellation = std::make_shared<Model>();
    cancellation->objects.resize(1);
    cancellation->objects[0].vertices = {{0, 0, 0}, {fixed, fixed, 0}, {0, 0, fixed}};
    constexpr int32_t half_limit = std::numeric_limits<int32_t>::max() / 2;
    cancellation->objects[0].offset_from_parent.x = half_limit;
    use_model(fixture, cancellation);
    fixture.instance.pieces()[0].translation.x = half_limit;
    OA_CHECK(!proof::bound_far_instance(fixture.geometry, fixture.instance, far));
    OA_CHECK(compare_raw_proofs(fixture));
}

/// Never carries successful validation, geometry or visibility across raw calls.
void combined_raw_scratch_progression_and_poses() {
    FrameFixture fixture;
    use_model(fixture, far_model());
    proof::Scratch combined, separate;
    OA_CHECK(compare_raw_proofs(fixture, combined, separate));
    const auto original_instance = fixture.instance;
    const auto original_geometry = fixture.geometry;
    ++fixture.gate.owner_token;
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    --fixture.gate.owner_token;
    const std::shared_ptr<const Model> foreign(fixture.model.get(), [](const Model*) {});
    fixture.geometry = proof::prepare_geometry(foreign);
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    fixture.geometry = original_geometry;
    fixture.instance.pieces()[0].object_index = fixture.instance.pieces()[1].object_index;
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    fixture.instance = original_instance;
    fixture.instance.pieces()[0].parent = 99999;
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    fixture.instance = original_instance;
    fixture.instance.pieces()[0].next_sibling = 0;
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    fixture.instance = original_instance;
    fixture.geometry.objects.pop_back();
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    fixture.geometry = original_geometry;
    OA_CHECK(compare_raw_proofs(fixture, combined, separate));

    uint32_t words = 0x73a4c19U;
    for (unsigned pose = 0; pose < 6; ++pose) {
        for (auto& piece : fixture.instance.pieces()) {
            const auto coordinate = [&] {
                return static_cast<int32_t>(next_word(words) % (128U * fixed)) - 64 * fixed;
            };
            piece.translation = {coordinate(), coordinate(), coordinate()};
            piece.rotation = {
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(words))),
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(words))),
                std::bit_cast<int16_t>(static_cast<uint16_t>(next_word(words)))
            };
        }
        fixture.root = {32767, -32768, static_cast<int16_t>(pose * 2731)};
        for (const int32_t x : {-800, 32, 800})
            for (const int32_t z : {-800, 32, 800}) {
                fixture.unit.position = {x * fixed, 20 * fixed, z * fixed};
                compare_raw_proofs(fixture, combined, separate);
                compare_raw_proofs(fixture, combined, separate, false);
            }
    }

    auto simple = std::make_shared<Model>();
    simple->objects.resize(1);
    simple->objects[0].vertices = {{0, 0, 0}, {fixed, fixed, 0}, {0, 0, fixed}};
    use_model(fixture, simple);
    fixture.root = {};
    fixture.unit.position = {200 * fixed, 20 * fixed, 32 * fixed};
    OA_CHECK(compare_raw_proofs(fixture, combined, separate));
    fixture.instance.pieces()[0].translation.x = -168 * fixed;
    OA_CHECK(!compare_raw_proofs(fixture, combined, separate));
    fixture.instance.pieces()[0].translation.x = std::numeric_limits<int32_t>::max();
    compare_raw_proofs(fixture, combined, separate);
}

/// Keeps every policy, frame, generation, camera and shadow refusal unchanged.
void combined_raw_policy_camera_and_shadow_refusals() {
    FrameFixture fixture;
    const auto gate = fixture.gate;
    for (bool proof::Eligibility::* field :
         {&proof::Eligibility::enhanced,
          &proof::Eligibility::local,
          &proof::Eligibility::rig_resolved,
          &proof::Eligibility::ground_resolved,
          &proof::Eligibility::gpu_type_supported,
          &proof::Eligibility::budget_safe,
          &proof::Eligibility::native_geometry_resolved,
          &proof::Eligibility::fog_seen,
          &proof::Eligibility::ordinary_model_path}) {
        fixture.gate = gate;
        fixture.gate.*field = false;
        OA_CHECK(!compare_raw_proofs(fixture));
    }
    for (bool proof::Eligibility::* field :
         {&proof::Eligibility::selected,
          &proof::Eligibility::debug,
          &proof::Eligibility::probe,
          &proof::Eligibility::reflection,
          &proof::Eligibility::complex,
          &proof::Eligibility::snapshot,
          &proof::Eligibility::vertex_override}) {
        fixture.gate = gate;
        fixture.gate.*field = true;
        OA_CHECK(!compare_raw_proofs(fixture));
    }
    fixture.gate = gate;
    ++fixture.gate.frame;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.gate = gate;
    fixture.gate.generation = 0;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.gate = gate;
    ++fixture.gate.rig_generation;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.gate = gate;
    fixture.unit.flags |= OA_UNIT_FLAG_SELECTED;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.unit.flags = 0;
    fixture.unit.state_flags |= drawing::unit_state_cloaked;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.unit.state_flags = 0;
    const auto view = fixture.view;
    fixture.view.width = 0;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    fixture.view.projection.fill(0);
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    for (std::size_t column = 0; column < 3; ++column)
        fixture.view.projection[8 + column] = fixture.view.projection[column];
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    fixture.view.projection[0] = std::numeric_limits<float>::infinity();
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    fixture.view.shadow_bounds[0] = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    fixture.view.shadow_scale = 0;
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    fixture.unit.position = {32 * fixed, 400 * fixed, 32 * fixed};
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.unit.position = {-20 * fixed, 20 * fixed, 32 * fixed};
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.unit.position = {32 * fixed, 20 * fixed, 32 * fixed};
    fixture.view.shadow_bounds = {1000, 1000, 1064, 1064};
    OA_CHECK(!compare_raw_proofs(fixture));
    fixture.view = view;
    fixture.unit.position = {200 * fixed, 20 * fixed, 200 * fixed};
    const auto control = oa::base::float_precision::save_float_control();
    for (const int rounding : {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
        if (std::fesetround(rounding) == 0)
            OA_CHECK(!compare_raw_proofs(fixture));
        oa::base::float_precision::restore_float_control(control);
    }
}

/// Retries failed far validation exactly as the separate wrappers do in one call.
void combined_raw_allocation_refusal_progression() {
    FrameFixture fixture;
    use_model(fixture, far_model());
    const auto saved = fixture.instance;
    const auto revision = fixture.instance.materialization_revision();
    for (const bool mapping_warm : {false, true}) {
        proof::Scratch combined, separate;
        if (mapping_warm) {
            combined.piece_for_object.reserve(fixture.instance.pieces().size());
            separate.piece_for_object.reserve(fixture.instance.pieces().size());
        }
        auto failures = refused_allocations;
        refuse_allocation = true;
        const bool expected =
            proof::far_rejection_proof(
                fixture.geometry, fixture.ref(), fixture.view, fixture.gate, separate
            ) ||
            proof::rejection_proof(
                fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, separate
            );
        OA_CHECK(!refuse_allocation && refused_allocations == failures + 1);
        failures = refused_allocations;
        refuse_allocation = true;
        const bool actual = proof::raw_rejection_proof(
            fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, combined
        );
        OA_CHECK(!refuse_allocation && refused_allocations == failures + 1);
        OA_CHECK(expected && actual == expected);
        OA_CHECK(compare_raw_proofs(fixture, combined, separate));
        unchanged(fixture.instance, saved);
        OA_CHECK(fixture.instance.materialization_revision() == revision);
    }
    proof::Scratch combined, separate;
    auto failures = refused_allocations;
    refuse_allocation = true;
    const bool expected = proof::rejection_proof(
        fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, separate
    );
    OA_CHECK(!refuse_allocation && refused_allocations == failures + 1);
    failures = refused_allocations;
    refuse_allocation = true;
    const bool actual = proof::raw_rejection_proof(
        fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, combined, false
    );
    OA_CHECK(!refuse_allocation && refused_allocations == failures + 1);
    OA_CHECK(!expected && actual == expected);
    OA_CHECK(compare_raw_proofs(fixture, combined, separate, false));

    // The far piece cap refuses before allocation. Its exact fallback gets
    // the sole refusal, unlike a failed allocation inside eligible far work.
    FrameFixture oversized;
    use_model(oversized, many_piece_model(false));
    const auto large_saved = oversized.instance;
    const auto large_revision = oversized.instance.materialization_revision();
    proof::Scratch large_combined, large_separate;
    failures = refused_allocations;
    refuse_allocation = true;
    const bool large_expected =
        proof::far_rejection_proof(
            oversized.geometry, oversized.ref(), oversized.view, oversized.gate, large_separate
        ) ||
        proof::rejection_proof(
            oversized.geometry,
            oversized.ref(),
            oversized.root,
            oversized.view,
            oversized.gate,
            large_separate
        );
    OA_CHECK(!refuse_allocation && refused_allocations == failures + 1);
    failures = refused_allocations;
    refuse_allocation = true;
    const bool large_actual = proof::raw_rejection_proof(
        oversized.geometry,
        oversized.ref(),
        oversized.root,
        oversized.view,
        oversized.gate,
        large_combined
    );
    OA_CHECK(!refuse_allocation && refused_allocations == failures + 1);
    OA_CHECK(!large_expected && large_actual == large_expected);
    OA_CHECK(compare_raw_proofs(oversized, large_combined, large_separate));
    unchanged(oversized.instance, large_saved);
    OA_CHECK(oversized.instance.materialization_revision() == large_revision);

    fixture.gate.selected = true;
    failures = refused_allocations;
    refuse_allocation = true;
    const bool gated = proof::raw_rejection_proof(
        fixture.geometry, fixture.ref(), fixture.root, fixture.view, fixture.gate, combined
    );
    const bool unused = refuse_allocation;
    refuse_allocation = false;
    OA_CHECK(!gated && unused && refused_allocations == failures);
    unchanged(fixture.instance, saved);
    OA_CHECK(fixture.instance.materialization_revision() == revision);
}
} // namespace

int main() {
    native_hierarchy_and_pose_corpus();
    interpolated_raw_poses_use_the_existing_helpers();
    exact_native_body_and_shadow_oracle();
    interpolated_native_projection_oracle();
    unknown_policy_and_identity_keep_the_normal_path();
    ownership_topology_wrap_and_allocation_refusals();
    completed_native_geometry_and_refusals();
    far_plane_rounding_margin();
    far_hierarchy_and_wrap_domains();
    far_mapping_and_radius_reference_parity();
    far_projection_and_translation_signs();
    combined_raw_far_and_exact_fallbacks();
    combined_raw_scratch_progression_and_poses();
    combined_raw_policy_camera_and_shadow_refusals();
    combined_raw_allocation_refusal_progression();
    return oa::test::check_exit_status();
}
