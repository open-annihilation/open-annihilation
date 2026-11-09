// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Exact per-frame pose reuse, interpolated roots and local pieces, receiver
// identity and world rounding, complete-key misses, and bounded fallback.
#include "model_transform_cache.hpp"
#include "presentation_interpolation.hpp"

#include "oa/present/model/model_draw.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace {

thread_local bool fail_next_cache_allocation{};
thread_local size_t failed_cache_allocations{};

} // namespace

/// Fails one requested ordinary allocation while testing snapshot recovery.
void* operator new(size_t bytes) {
    if (fail_next_cache_allocation) {
        fail_next_cache_allocation = false;
        ++failed_cache_allocations;
        throw std::bad_alloc{};
    }
    if (void* result = std::malloc(bytes == 0 ? 1 : bytes))
        return result;
    throw std::bad_alloc{};
}

/// Routes ordinary array allocations through the same one-shot failure.
void* operator new[](size_t bytes) {
    return ::operator new(bytes);
}

/// Releases ordinary allocation storage.
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

/// Releases array allocation storage.
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

/// Releases sized ordinary allocation storage.
void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}

/// Releases sized array allocation storage.
void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

namespace {

using oa::app::ModelTransformCache;
using oa::formats::objects3d::FixedVector3;
using oa::formats::objects3d::Model;
using oa::formats::objects3d::Object;
using oa::sim::model_runtime::Instance;
using oa::sim::model_runtime::make_instance;
using oa::sim::model_runtime::RotationWords;

constexpr int32_t fixed = 0x10000;
constexpr uint32_t half_tick = oa::app::whole_tick / 2;

bool same(FixedVector3 a, FixedVector3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same(RotationWords a, RotationWords b) {
    return a.xy == b.xy && a.xz == b.xz && a.yz == b.yz;
}

std::shared_ptr<const Model> model_with(size_t pieces = 3, size_t vertices = 3) {
    auto model = std::make_shared<Model>();
    for (size_t index = 0; index < pieces; ++index) {
        Object object;
        object.name = index == 0 ? "base" : (index == 1 ? "turret" : "barrel");
        object.offset_from_parent = {
            static_cast<int32_t>(index) * fixed + 13, -fixed / 3, fixed / 7
        };
        object.parent =
            index == 0 ? oa::formats::objects3d::kNoObject : static_cast<uint32_t>(index - 1);
        if (index + 1 < pieces)
            object.first_child = static_cast<uint32_t>(index + 1);
        for (size_t vertex = 0; vertex < vertices; ++vertex)
            object.vertices.push_back(
                {static_cast<int32_t>(vertex) * fixed / 3 + 19,
                 static_cast<int32_t>(vertex % 3) * fixed - 5,
                 -static_cast<int32_t>(vertex) * fixed / 5 - 37}
            );
        model->objects.push_back(std::move(object));
    }
    return model;
}

void pose(Instance& instance, int16_t phase) {
    for (size_t index = 0; index < instance.pieces().size(); ++index) {
        auto& piece = instance.pieces()[index];
        piece.translation = {
            static_cast<int32_t>(phase) * 13,
            -static_cast<int32_t>(index) * 41,
            static_cast<int32_t>(phase) * -17
        };
        piece.rotation = {static_cast<int16_t>(phase + index), phase, static_cast<int16_t>(-phase)};
    }
}

Instance rebuilt_afresh(
    const Instance& posed, RotationWords root, std::span<const std::string_view> names = {}
) {
    auto fresh = make_instance(posed.model_handle(), 0, names);
    OA_CHECK(fresh.pieces().size() == posed.pieces().size());
    for (size_t index = 0; index < fresh.pieces().size(); ++index) {
        fresh.pieces()[index].translation = posed.pieces()[index].translation;
        fresh.pieces()[index].rotation = posed.pieces()[index].rotation;
    }
    fresh.rebuild_transforms(root);
    return fresh;
}

bool same_transforms(const Instance& a, const Instance& b) {
    if (a.pieces().size() != b.pieces().size())
        return false;
    for (size_t index = 0; index < a.pieces().size(); ++index) {
        const auto& first = a.pieces()[index];
        const auto& second = b.pieces()[index];
        if (!same(first.transformed_origin, second.transformed_origin) ||
            first.transform_marker != second.transform_marker ||
            first.transformed_vertices.size() != second.transformed_vertices.size())
            return false;
        for (size_t vertex = 0; vertex < first.transformed_vertices.size(); ++vertex)
            if (!same(first.transformed_vertices[vertex], second.transformed_vertices[vertex]))
                return false;
    }
    return true;
}

void check_unchanged(const Instance& instance, const Instance& before) {
    OA_CHECK(instance.owner_token() == before.owner_token());
    OA_CHECK(instance.model_handle() == before.model_handle());
    OA_CHECK(instance.root_piece() == before.root_piece());
    OA_CHECK(instance.transforms_dirty() == before.transforms_dirty());
    OA_CHECK(same_transforms(instance, before));
    for (size_t index = 0; index < instance.pieces().size(); ++index) {
        const auto& piece = instance.pieces()[index];
        const auto& saved = before.pieces()[index];
        OA_CHECK(same(piece.translation, saved.translation));
        OA_CHECK(same(piece.rotation, saved.rotation));
        OA_CHECK(piece.flags == saved.flags);
        OA_CHECK(piece.object_index == saved.object_index);
        OA_CHECK(piece.parent == saved.parent);
        OA_CHECK(piece.first_child == saved.first_child);
        OA_CHECK(piece.next_sibling == saved.next_sibling);
    }
}

void check_miss(ModelTransformCache& cache, Instance& receiver, RotationWords root) {
    const auto before = receiver;
    OA_CHECK(!cache.reuse(receiver, root));
    check_unchanged(receiver, before);
}

uint32_t world_bits(int32_t origin, int32_t point) {
    const float world_origin = static_cast<float>(origin) / fixed;
    const float local = static_cast<float>(point) / fixed;
    return std::bit_cast<uint32_t>(world_origin + local);
}

void test_reuse_keeps_owner_flags_and_world_rounding() {
    const auto model = model_with();
    const RotationWords root{123, -24001, -73};
    auto source = make_instance(model, 17);
    pose(source, 563);
    source.rebuild_transforms(root);
    ModelTransformCache cache;
    cache.begin_frame();
    cache.remember(source, root);
    OA_CHECK(cache.counts().stores == 1);

    constexpr std::array<oa::FixedVec3, 4> origins{{
        {0, fixed / 3, 17},
        {INT32_MAX - 113, -13 * fixed - 17, INT32_MIN + 471},
        {-19 * fixed - 37, -fixed / 7, 16383 * fixed + 32767},
        {12345 * fixed + 17, -17001 * fixed - 1, -fixed / 3},
    }};
    for (size_t owner = 0; owner < origins.size(); ++owner) {
        oa::Unit unit{};
        unit.position = origins[owner];
        unit.owner = static_cast<uint8_t>(owner);
        const auto record = unit;
        auto receiver = make_instance(model, reinterpret_cast<uintptr_t>(&unit));
        receiver.rebuild_transforms();
        pose(receiver, 563);
        receiver.pieces()[1].flags = 0;
        receiver.pieces()[2].flags =
            static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
        receiver.pieces()[1].transform_marker = 7;
        const auto oracle = rebuilt_afresh(receiver, root);
        const auto* vertices = receiver.pieces()[1].transformed_vertices.data();
        const auto capacity = receiver.pieces()[1].transformed_vertices.capacity();
        const auto storage = cache.storage_bytes();
        OA_CHECK(cache.reuse(receiver, root));
        OA_CHECK(same_transforms(receiver, oracle));
        OA_CHECK(receiver.owner_token() == reinterpret_cast<uintptr_t>(&unit));
        OA_CHECK(receiver.pieces()[1].flags == 0);
        OA_CHECK(receiver.pieces()[2].flags == 1);
        OA_CHECK(receiver.pieces()[1].transformed_vertices.data() == vertices);
        OA_CHECK(receiver.pieces()[1].transformed_vertices.capacity() == capacity);
        OA_CHECK(cache.storage_bytes() == storage);
        OA_CHECK(
            unit.position.x == record.position.x && unit.position.y == record.position.y &&
            unit.position.z == record.position.z && unit.owner == record.owner
        );
        for (size_t piece = 0; piece < receiver.pieces().size(); ++piece) {
            const auto& actual = receiver.pieces()[piece];
            const auto& expected = oracle.pieces()[piece];
            OA_CHECK(
                world_bits(unit.position.x, actual.transformed_origin.x) ==
                world_bits(record.position.x, expected.transformed_origin.x)
            );
            OA_CHECK(
                world_bits(unit.position.y, actual.transformed_origin.y) ==
                world_bits(record.position.y, expected.transformed_origin.y)
            );
            OA_CHECK(
                world_bits(unit.position.z, actual.transformed_origin.z) ==
                world_bits(record.position.z, expected.transformed_origin.z)
            );
            for (size_t vertex = 0; vertex < actual.transformed_vertices.size(); ++vertex) {
                const auto a = actual.transformed_vertices[vertex];
                const auto b = expected.transformed_vertices[vertex];
                OA_CHECK(world_bits(unit.position.x, a.x) == world_bits(record.position.x, b.x));
                OA_CHECK(world_bits(unit.position.y, a.y) == world_bits(record.position.y, b.y));
                OA_CHECK(world_bits(unit.position.z, a.z) == world_bits(record.position.z, b.z));
            }
        }
    }
    OA_CHECK(cache.counts().hits == origins.size());
    OA_CHECK(same_transforms(source, rebuilt_afresh(source, root)));
}

void test_unbuilt_and_stale_sources_fall_back() {
    const auto model = model_with();
    const RotationWords root{7, -1234, 18};
    ModelTransformCache cache;
    cache.begin_frame();
    auto source = make_instance(model);
    cache.remember(source, root);
    OA_CHECK(cache.counts().stores == 0);
    source.rebuild_transforms(root);
    ++source.pieces()[1].rotation.xz;
    // A direct piece change can leave the dirty flag clear. Geometry from
    // before that change still cannot satisfy the source's current pose.
    OA_CHECK(!source.transforms_dirty());
    cache.remember(source, root);
    auto receiver = make_instance(model, 42);
    receiver.rebuild_transforms();
    ++receiver.pieces()[1].rotation.xz;
    check_miss(cache, receiver, root);
    receiver.rebuild_transforms(root);
    OA_CHECK(same_transforms(receiver, rebuilt_afresh(receiver, root)));
    cache.begin_frame();
    cache.remember(receiver, {root.xy, static_cast<int16_t>(root.xz + 1), root.yz});
    check_miss(cache, source, {root.xy, static_cast<int16_t>(root.xz + 1), root.yz});
}

void test_interpolated_instances_and_generation() {
    const auto model = model_with();
    ModelTransformCache cache;
    cache.begin_frame();
    std::array<oa::Unit, 2> units{};
    for (uint32_t generation = 1; generation <= 2; ++generation) {
        auto& unit = units[generation - 1];
        unit.type_index = 1;
        unit.position = {fixed + 17, -fixed / 3, -21 * fixed - 5};
        unit.heading = 0xff00;
        auto instance = make_instance(model, reinterpret_cast<uintptr_t>(&unit));
        instance.rebuild_transforms();
        oa::app::UnitMotion motion;
        oa::app::observe_unit(motion, 10, generation, unit, instance);
        unit.position.x += 4 * fixed;
        unit.position.z -= 2 * fixed;
        unit.heading = 0x100;
        instance.pieces()[1].translation.x = 3 * fixed + 1;
        instance.pieces()[1].rotation.xz = 0x400;
        oa::app::observe_unit(motion, 11, generation, unit, instance);
        OA_CHECK(motion.continued && motion.moved && motion.pieces_moved);
        const auto simulation = instance;
        auto shown = instance;
        oa::Unit record = unit;
        oa::app::blend_unit_pose(motion.previous, motion.current, half_tick, record, shown);
        OA_CHECK(record.heading == 0);
        OA_CHECK(record.position.x == 3 * fixed + 17);
        OA_CHECK(shown.pieces()[1].rotation.xz == 0x200);
        const RotationWords root{record.bank, static_cast<int16_t>(record.heading), record.pitch};
        const auto oracle = rebuilt_afresh(shown, root);
        if (generation == 1) {
            check_miss(cache, shown, root);
            shown.rebuild_transforms(root);
            cache.remember(shown, root);
        } else {
            OA_CHECK(cache.reuse(shown, root));
        }
        OA_CHECK(same_transforms(shown, oracle));
        OA_CHECK(shown.owner_token() == reinterpret_cast<uintptr_t>(&unit));
        check_unchanged(instance, simulation);
    }
    OA_CHECK(cache.counts().hits == 1 && cache.counts().stores == 1);
}

void test_effective_root_threshold_and_wrap() {
    const auto model = model_with();
    for (const int16_t initial : std::array<int16_t, 2>{0, 32764}) {
        auto source = make_instance(model);
        oa::present::model::ModelState state;
        state.shift = {3, initial, -9};
        source.rebuild_transforms(state.shift);
        state.transforms_dirty = false;
        const oa::present::model::ModelRef ref{&source, nullptr, &state, nullptr, nullptr};
        ModelTransformCache cache;
        cache.begin_frame();
        cache.remember(source, state.shift);
        const auto before = state.shift;
        const auto plus = [](int16_t angle, uint16_t step) {
            return std::bit_cast<int16_t>(static_cast<uint16_t>(uint16_t(angle) + step));
        };
        oa::present::model::set_model_shift(ref, {3, plus(initial, 7), -9});
        OA_CHECK(same(state.shift, before));
        OA_CHECK(!state.transforms_dirty);
        auto receiver = make_instance(model, 99);
        receiver.rebuild_transforms();
        OA_CHECK(cache.reuse(receiver, state.shift));
        OA_CHECK(same_transforms(receiver, rebuilt_afresh(receiver, state.shift)));
        oa::present::model::set_model_shift(ref, {3, plus(initial, 8), -9});
        OA_CHECK(state.shift.xz == plus(initial, 8));
        OA_CHECK(state.transforms_dirty);
        check_miss(cache, receiver, state.shift);
        receiver.rebuild_transforms(state.shift);
        OA_CHECK(same_transforms(receiver, rebuilt_afresh(receiver, state.shift)));
    }
}

void test_complete_key_and_frame_invalidation() {
    const auto model = model_with();
    const RotationWords root{13, -900, 71};
    auto source = make_instance(model);
    pose(source, 117);
    source.rebuild_transforms(root);
    ModelTransformCache cache;
    cache.begin_frame();
    cache.remember(source, root);
    auto receiver = source;
    ++receiver.pieces()[1].translation.x;
    check_miss(cache, receiver, root);
    receiver = source;
    ++receiver.pieces()[2].rotation.yz;
    check_miss(cache, receiver, root);
    receiver = source;
    check_miss(cache, receiver, {root.xy, root.xz, static_cast<int16_t>(root.yz + 1)});
    auto other_model = make_instance(model_with());
    pose(other_model, 117);
    other_model.rebuild_transforms(root);
    check_miss(cache, other_model, root);
    constexpr std::array<std::string_view, 3> reordered{"barrel", "base", "turret"};
    auto reordered_instance = make_instance(model, 0, reordered);
    pose(reordered_instance, 117);
    reordered_instance.rebuild_transforms(root);
    OA_CHECK(reordered_instance.root_piece() != source.root_piece());
    check_miss(cache, reordered_instance, root);
    OA_CHECK(
        same_transforms(reordered_instance, rebuilt_afresh(reordered_instance, root, reordered))
    );
    receiver = source;
    receiver.pieces()[2].parent = oa::sim::model_runtime::kNoPiece;
    check_miss(cache, receiver, root);
    const auto retained = cache.storage_bytes();
    cache.begin_frame();
    OA_CHECK(
        cache.counts().hits == 0 && cache.counts().misses == 0 && cache.counts().stores == 0 &&
        cache.counts().overflows == 0
    );
    OA_CHECK(cache.storage_bytes() == retained);
    receiver = source;
    check_miss(cache, receiver, root);
    receiver.rebuild_transforms(root);
    cache.remember(receiver, root);
    OA_CHECK(cache.reuse(source, root));
    OA_CHECK(same_transforms(source, rebuilt_afresh(source, root)));
}

void test_admission_limits_and_many_unique_poses() {
    const auto model = model_with();
    auto first = make_instance(model);
    first.rebuild_transforms();
    auto second = first;
    pose(second, 1);
    second.rebuild_transforms();
    ModelTransformCache one_entry(1);
    one_entry.begin_frame();
    one_entry.remember(first, {});
    one_entry.remember(second, {});
    OA_CHECK(one_entry.counts().stores == 1 && one_entry.counts().overflows == 1);
    OA_CHECK(one_entry.reuse(first, {}));
    check_miss(one_entry, second, {});

    ModelTransformCache no_bytes(
        ModelTransformCache::entry_capacity,
        sizeof(ModelTransformCache) + first.transform_storage_bytes() - 1
    );
    no_bytes.begin_frame();
    no_bytes.remember(first, {});
    OA_CHECK(no_bytes.counts().stores == 0 && no_bytes.counts().overflows == 1);
    OA_CHECK(no_bytes.storage_bytes() == sizeof(ModelTransformCache));
    check_miss(no_bytes, first, {});

    const auto one_pose_bytes = sizeof(ModelTransformCache) + first.transform_storage_bytes();
    ModelTransformCache byte_limited(ModelTransformCache::entry_capacity, one_pose_bytes);
    byte_limited.begin_frame();
    byte_limited.remember(first, {});
    byte_limited.remember(second, {});
    OA_CHECK(byte_limited.counts().stores == 1 && byte_limited.counts().overflows == 1);
    OA_CHECK(byte_limited.storage_bytes() <= one_pose_bytes);
    OA_CHECK(byte_limited.reuse(first, {}));
    byte_limited.begin_frame();
    byte_limited.remember(second, {});
    OA_CHECK(byte_limited.counts().stores == 1);
    OA_CHECK(byte_limited.reuse(second, {}));
    OA_CHECK(byte_limited.storage_bytes() <= one_pose_bytes);

    ModelTransformCache cache;
    cache.begin_frame();
    auto too_many_vertices = make_instance(model_with(1, ModelTransformCache::vertex_limit + 1));
    too_many_vertices.rebuild_transforms();
    cache.remember(too_many_vertices, {});
    check_miss(cache, too_many_vertices, {});
    auto too_many_pieces = make_instance(model_with(ModelTransformCache::piece_limit + 1));
    too_many_pieces.rebuild_transforms();
    cache.remember(too_many_pieces, {});
    check_miss(cache, too_many_pieces, {});
    OA_CHECK(cache.counts().stores == 0);
    OA_CHECK(cache.storage_bytes() == sizeof(ModelTransformCache));

    constexpr int16_t unique_poses = 512;
    for (int16_t phase = 0; phase < unique_poses; ++phase) {
        auto receiver = make_instance(model, static_cast<uintptr_t>(phase + 1));
        receiver.rebuild_transforms();
        pose(receiver, phase);
        check_miss(cache, receiver, {});
        receiver.rebuild_transforms();
        OA_CHECK(same_transforms(receiver, rebuilt_afresh(receiver, {})));
        cache.remember(receiver, {});
        OA_CHECK(cache.storage_bytes() <= ModelTransformCache::storage_limit);
        OA_CHECK(cache.counts().stores <= ModelTransformCache::entry_capacity);
    }
    OA_CHECK(cache.counts().stores > 0);
    OA_CHECK(cache.counts().overflows >= unique_poses - ModelTransformCache::entry_capacity);
    cache.begin_frame();
    cache.remember(first, {});
    OA_CHECK(cache.reuse(first, {}));
    OA_CHECK(cache.storage_bytes() <= ModelTransformCache::storage_limit);
}

void test_failed_replacement_preserves_accounting_and_active_entry() {
    auto active = make_instance(model_with(3), 11);
    auto previous = make_instance(model_with(2), 22);
    auto replacement = make_instance(model_with(4), 33);
    const RotationWords root{31, -1729, 83};
    pose(active, 13);
    pose(previous, -37);
    pose(replacement, 91);
    active.rebuild_transforms(root);
    previous.rebuild_transforms(root);
    replacement.rebuild_transforms(root);
    const auto replacement_oracle = rebuilt_afresh(replacement, root);

    ModelTransformCache cache;
    cache.begin_frame();
    cache.remember(active, root);
    const auto active_bytes = cache.storage_bytes();
    cache.remember(previous, root);
    OA_CHECK(cache.storage_bytes() > active_bytes);
    OA_CHECK(cache.counts().stores == 2);

    cache.begin_frame();
    cache.remember(active, root);
    failed_cache_allocations = 0;
    fail_next_cache_allocation = true;
    bool threw = false;
    try {
        cache.remember(replacement, root);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_next_cache_allocation = false;
    OA_CHECK(threw && failed_cache_allocations == 1);
    OA_CHECK(cache.storage_bytes() == active_bytes);
    OA_CHECK(cache.counts().stores == 1);
    OA_CHECK(cache.reuse(active, root));
    OA_CHECK(same_transforms(active, rebuilt_afresh(active, root)));
    check_miss(cache, previous, root);
    check_miss(cache, replacement, root);

    cache.remember(replacement, root);
    OA_CHECK(cache.counts().stores == 2 && cache.counts().overflows == 0);
    OA_CHECK(cache.storage_bytes() == active_bytes + replacement.transform_storage_bytes());
    OA_CHECK(cache.storage_bytes() <= ModelTransformCache::storage_limit);
    OA_CHECK(cache.reuse(active, root));
    OA_CHECK(cache.reuse(replacement, root));
    OA_CHECK(same_transforms(replacement, replacement_oracle));
    OA_CHECK(active.owner_token() == 11 && replacement.owner_token() == 33);
    cache.begin_frame();
    cache.remember(previous, root);
    OA_CHECK(cache.reuse(previous, root));
    OA_CHECK(cache.storage_bytes() <= ModelTransformCache::storage_limit);
}

void test_heading_and_pitch_families_keep_exact_repeats() {
    constexpr size_t family_size = 32;
    constexpr int16_t root_step = 256;
    const auto model = model_with();
    for (const bool pitch : std::array<bool, 2>{false, true}) {
        ModelTransformCache cache;
        cache.begin_frame();
        auto source = make_instance(model, 61);
        pose(source, 117);
        std::array<bool, family_size> admitted{};
        const auto root_at = [&](size_t index) {
            const auto angle = static_cast<int16_t>(index * root_step);
            return pitch ? RotationWords{17, 0, angle} : RotationWords{17, angle, 0};
        };
        for (size_t index = 0; index < family_size; ++index) {
            const auto root = root_at(index);
            source.rebuild_transforms(root);
            const auto stored = cache.counts().stores;
            cache.remember(source, root);
            admitted[index] = cache.counts().stores != stored;
        }
        // A small family of distinct headings or pitches can coexist while
        // entries remain free. Probe collisions may still refuse a pose.
        const auto stored = cache.counts().stores;
        OA_CHECK(stored > 8 && stored <= family_size);
        OA_CHECK(stored < ModelTransformCache::entry_capacity);
        OA_CHECK(cache.counts().overflows == family_size - stored);
        OA_CHECK(cache.storage_bytes() <= ModelTransformCache::storage_limit);

        auto receiver = make_instance(model, 73);
        pose(receiver, 117);
        receiver.rebuild_transforms();
        for (size_t index = 0; index < family_size; ++index) {
            const auto root = root_at(index);
            const auto oracle = rebuilt_afresh(receiver, root);
            const auto before = receiver;
            const bool reused = cache.reuse(receiver, root);
            OA_CHECK(reused == admitted[index]);
            if (!reused) {
                check_unchanged(receiver, before);
                receiver.rebuild_transforms(root);
            }
            OA_CHECK(same_transforms(receiver, oracle));
            OA_CHECK(receiver.owner_token() == 73);
        }
        OA_CHECK(cache.counts().hits == stored);
        OA_CHECK(cache.counts().misses == family_size - stored);
        const auto unretained_root = root_at(family_size);
        check_miss(cache, receiver, unretained_root);
        receiver.rebuild_transforms(unretained_root);
        OA_CHECK(same_transforms(receiver, rebuilt_afresh(receiver, unretained_root)));
    }
}

} // namespace

int main() {
    test_reuse_keeps_owner_flags_and_world_rounding();
    test_interpolated_instances_and_generation();
    test_effective_root_threshold_and_wrap();
    test_complete_key_and_frame_invalidation();
    test_unbuilt_and_stale_sources_fall_back();
    test_admission_limits_and_many_unique_poses();
    test_failed_replacement_preserves_accounting_and_active_entry();
    test_heading_and_pitch_families_keep_exact_repeats();
    return oa::test::check_exit_status();
}
