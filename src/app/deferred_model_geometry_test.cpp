// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Invisible draw recipes keep raw inputs and historical image metadata separate
// from retained vectors, with explicit ingress and external-effect resolution.
#include "deferred_model_geometry.hpp"
#include "oa/sim/model_runtime/model_host.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace {
namespace runtime = oa::sim::model_runtime;
namespace draw = oa::present::model;
using oa::formats::objects3d::FixedVector3;
constexpr int32_t fixed = 65536;
constexpr std::array<std::string_view, 4> order{"tip", "root", "arm", "detached"};

struct Host final : oa::sim::model_runtime::ModelHost {
    using ModelHost::ModelHost;

    void emit_sfx(uint32_t, int32_t) override {}

    void explode(uint32_t, int32_t) override {}

    int32_t get_unit_value(int32_t, int32_t, int32_t, int32_t, int32_t) override { return 0; }

    void set_unit_value(int32_t, int32_t) override {}

    void attach_unit(int32_t, int32_t, int32_t) override {}

    void drop_unit(int32_t) override {}

    void ignored_piece_op(uint32_t, uint32_t, uint32_t) override {}

    void dont_shadow(uint32_t) override {}

    uint32_t is_carrying_unit(uint32_t) override { return 0; }

    uint32_t carrier_unit_id() override { return 0; }

    uint32_t random_bounded(uint32_t) override { return 0; }
};

/// Builds a reordered hierarchy with a root sibling outside its transform walk.
std::shared_ptr<oa::formats::objects3d::Model> model() {
    auto source = std::make_shared<oa::formats::objects3d::Model>();
    source->objects.resize(4);
    const std::array names{"root", "arm", "tip", "detached"};
    for (size_t i = 0; i < source->objects.size(); ++i) {
        auto& object = source->objects[i];
        object.name = names[i];
        object.vertices = {{-fixed, 0, fixed}, {2 * fixed, fixed, 0}, {0, 3 * fixed, -fixed}};
        object.offset_from_parent = {int32_t(i) * 71, int32_t(i) * -93, int32_t(i) * 113};
    }
    source->objects[0].first_child = 1;
    source->objects[0].next_sibling = 3;
    source->objects[1].parent = 0;
    source->objects[1].first_child = 2;
    source->objects[2].parent = 1;
    return source;
}

/// Compares actual derived vectors, without rebuilding either operand.
bool same_geometry(const runtime::Instance& a, const runtime::Instance& b) {
    if (a.pieces().size() != b.pieces().size())
        return false;
    const auto same = [](FixedVector3 x, FixedVector3 y) {
        return x.x == y.x && x.y == y.y && x.z == y.z;
    };
    for (size_t i = 0; i < a.pieces().size(); ++i) {
        const auto& x = a.pieces()[i];
        const auto& y = b.pieces()[i];
        if (!same(x.transformed_origin, y.transformed_origin) ||
            x.transformed_vertices.size() != y.transformed_vertices.size())
            return false;
        for (size_t v = 0; v < x.transformed_vertices.size(); ++v)
            if (!same(x.transformed_vertices[v], y.transformed_vertices[v]))
                return false;
    }
    return true;
}

/// Checks retained geometry, current flags, wrapped inputs and partial dirty ingress.
void retained_pose_and_ingress() {
    auto instance = runtime::make_instance(model(), 7, order);
    Host host(instance);
    const runtime::RotationWords root_a{123, 2100, -77}, root_b{513, -14567, 221};
    instance.rebuild_transforms(root_a);
    const auto before = instance;
    const uint64_t before_revision = instance.materialization_revision();
    oa::app::DeferredModelGeometries table;
    table.units.resize(2);
    table.prepare(1, 5, instance);
    auto& kept = table.units[1];
    host.set_piece_position(0, 0, std::numeric_limits<int32_t>::max() - 37);
    host.set_piece_angle(2, 1, 0x8abc);
    OA_CHECK(kept.retain(instance, root_b));
    OA_CHECK(instance.materialization_revision() == before_revision);
    OA_CHECK(instance.transforms_dirty());
    OA_CHECK(same_geometry(instance, before));
    auto expected = instance;
    expected.rebuild_transforms(root_b);
    // Later script state must not be overwritten while previous geometry resolves.
    host.set_piece_position(0, 2, std::numeric_limits<int32_t>::min() + 91);
    host.set_piece_visible(2, 0);
    const auto raw = instance.pieces()[0].translation;
    const auto flags = instance.pieces()[2].flags;
    OA_CHECK(kept.resolve(instance));
    OA_CHECK(!kept.pending && instance.materialization_revision() > before_revision);
    OA_CHECK(instance.pieces()[0].translation.x == raw.x);
    OA_CHECK(instance.pieces()[0].translation.z == raw.z);
    OA_CHECK(instance.pieces()[2].flags == flags);
    OA_CHECK(instance.transforms_dirty());
    OA_CHECK(same_geometry(instance, expected));
    auto current = instance;
    current.rebuild_transforms(root_a);
    instance.rebuild_transforms(root_a, true);
    OA_CHECK(same_geometry(instance, current));
    OA_CHECK(!instance.transforms_dirty());
    OA_CHECK(table.bytes <= table.maximum_bytes);
}

/// Checks that an effect's newer geometry supersedes the virtual draw's recipe.
void external_materialization_wins() {
    auto instance = runtime::make_instance(model(), 9, order);
    Host host(instance);
    instance.rebuild_transforms();
    oa::app::DeferredModelGeometries table;
    table.units.resize(2);
    table.prepare(1, 8, instance);
    auto& kept = table.units[1];
    host.set_piece_angle(0, 2, 4000);
    OA_CHECK(kept.retain(instance, {0, 8000, 0}));
    // SFX refreshes the actual current pose before reading muzzle/debris points.
    instance.rebuild_transforms({0, 8111, 0});
    const auto effect_geometry = instance;
    const auto revision = instance.materialization_revision();
    OA_CHECK(kept.resolve(instance));
    OA_CHECK(!kept.pending);
    OA_CHECK(instance.materialization_revision() == revision);
    OA_CHECK(same_geometry(instance, effect_geometry));
    // A new occupant of the same slot must never inherit a pending recipe.
    OA_CHECK(kept.retain(instance, {0, 9000, 0}));
    table.prepare(1, 9, instance);
    OA_CHECK(!kept.pending && kept.generation == 9);
}

/// Checks root-only deferral when script setters have left Instance clean.
void root_only_and_refusal() {
    auto instance = runtime::make_instance(model(), 11, order);
    instance.rebuild_transforms();
    oa::app::DeferredModelGeometries table;
    table.units.resize(2);
    table.prepare(1, 1, instance);
    auto& kept = table.units[1];
    OA_CHECK(!instance.transforms_dirty());
    OA_CHECK(kept.retain(instance, {213, 12777, -314}));
    auto expected = instance;
    expected.rebuild_transforms({213, 12777, -314});
    OA_CHECK(kept.resolve(instance));
    OA_CHECK(same_geometry(instance, expected));
    const auto before = instance;
    const auto revision = instance.materialization_revision();
    std::vector<runtime::TransformPose> wrong_size(1);
    OA_CHECK(!instance.rebuild_retained_transforms(wrong_size, {}, true));
    OA_CHECK(instance.materialization_revision() == revision);
    OA_CHECK(same_geometry(instance, before));
    // Admission refuses excessive recipes without allocating them.
    table.bytes = table.maximum_bytes;
    table.units.resize(3);
    table.prepare(2, 1, instance);
    OA_CHECK(table.units[2].poses.empty());
}

/// Checks exact no-rebuild prediction without changing cache/image history.
void image_metadata_gate() {
    auto instance = runtime::make_instance(model(), 13, order);
    instance.rebuild_transforms();
    draw::ModelState state;
    oa::Unit unit{};
    draw::ModelRef reference{&instance, nullptr, &state, &unit, nullptr};
    state.cache.draws = 7;
    state.deferred_image = draw::ImageFrame{12, 15, 4, 6};
    state.deferred_image_depth = true;
    state.image_builds = 19;
    const auto step = oa::app::deferred_model_cache_step(reference, true);
    OA_CHECK(step && !step->rebuild && step->unlit);
    OA_CHECK(state.cache.draws == 7 && !state.cache.has_image);
    OA_CHECK(state.image_builds == 19 && state.deferred_image->width == 12);
    state.cache.draws = 0;
    OA_CHECK(!oa::app::deferred_model_cache_step(reference, false));
    state.cache.draws = 7;
    state.image_unfinished = true;
    OA_CHECK(!oa::app::deferred_model_cache_step(reference, false));
    state.image_unfinished = false;
    state.finer = std::make_unique<draw::FinerModel>();
    OA_CHECK(!oa::app::deferred_model_cache_step(reference, false));
    state.finer.reset();
    state.build_bands.emplace();
    OA_CHECK(!oa::app::deferred_model_cache_step(reference, false));
    state.build_bands.reset();
    unit.attach_first_child = 1;
    OA_CHECK(!oa::app::deferred_model_cache_step(reference, false));
    unit.attach_first_child = 0;
    state.deferred_image.reset();
    OA_CHECK(!oa::app::deferred_model_cache_step(reference, false));
}

/// Checks that current-pose freezes never replay an older pose or validate stale images.
void current_pose_obligations() {
    auto instance = runtime::make_instance(model(), 17, order);
    Host host(instance);
    instance.rebuild_transforms();
    oa::app::DeferredModelGeometries table;
    table.units.resize(2);
    table.prepare(1, 3, instance, false);
    auto& kept = table.units[1];
    OA_CHECK(kept.poses.empty() && table.bytes == 0);
    const auto old = instance;
    const auto revision = instance.materialization_revision();
    host.set_piece_angle(0, 1, 0x4567);
    kept.freeze_current(instance, true, true);
    OA_CHECK(kept.fresh_geometry && kept.fresh_original_images && kept.fresh_copy_images);
    OA_CHECK(!kept.pending && kept.poses.empty());
    OA_CHECK(instance.materialization_revision() == revision && same_geometry(instance, old));
    // Later raw state is authoritative when a visible consumer actually rebuilds.
    host.set_piece_position(2, 0, 3 * fixed);
    const runtime::RotationWords root{13, 2711, -42};
    auto expected = instance;
    expected.rebuild_transforms(root);
    instance.rebuild_transforms(root, true);
    kept.observe_fresh(instance);
    OA_CHECK(!kept.fresh_geometry && kept.fresh_original_images && kept.fresh_copy_images);
    OA_CHECK(same_geometry(instance, expected));
    // Fresh SFX geometry does not make an old image current.
    kept.freeze_current(instance, false, false);
    OA_CHECK(!kept.fresh_geometry && kept.fresh_original_images && kept.fresh_copy_images);
    table.prepare(1, 4, instance, false);
    OA_CHECK(!kept.fresh_geometry && !kept.fresh_original_images && !kept.fresh_copy_images);
}

/// Checks that visible copies leave original image and geometry obligations separate.
void original_only_obligations() {
    auto instance = runtime::make_instance(model(), 23, order);
    Host host(instance);
    instance.rebuild_transforms();
    oa::app::DeferredModelGeometries table;
    table.units.resize(2);
    table.prepare(1, 7, instance, false);
    auto& kept = table.units[1];
    const auto old = instance;
    const auto revision = instance.materialization_revision();
    host.set_piece_angle(0, 1, 0x3210);
    kept.freeze_original(instance, true);
    OA_CHECK(kept.fresh_geometry && kept.fresh_original_images && !kept.fresh_copy_images);
    OA_CHECK(instance.transforms_dirty() && instance.materialization_revision() == revision);
    OA_CHECK(same_geometry(instance, old));
    // An effect rebuild satisfies geometry alone; the original still needs image ingress.
    instance.rebuild_transforms({13, -2111, 42});
    kept.observe_fresh(instance);
    OA_CHECK(!kept.fresh_geometry && kept.fresh_original_images && !kept.fresh_copy_images);
    kept.freeze_original(instance, false);
    OA_CHECK(!kept.fresh_geometry && kept.fresh_original_images && !kept.fresh_copy_images);
    // Existing copy obligations are never cancelled by another original-only draw.
    kept.fresh_copy_images = true;
    kept.freeze_original(instance, false);
    OA_CHECK(kept.fresh_original_images && kept.fresh_copy_images);
    table.prepare(1, 8, instance, false);
    OA_CHECK(!kept.fresh_geometry && !kept.fresh_original_images && !kept.fresh_copy_images);
}

/// Checks the copy revision distinguishes compatible raw poses from layout replacement.
void compatible_copy_revision() {
    const auto source = model();
    constexpr uintptr_t owner = 29;
    auto original = runtime::make_instance(source, owner, order);
    original.rebuild_transforms();
    auto copy = runtime::make_instance(source, owner, order);
    OA_CHECK(copy.materialization_revision() == 0);
    copy.rebuild_transforms();
    const auto revision = copy.materialization_revision();
    OA_CHECK(revision != 0);
    Host host(original);
    host.set_piece_angle(0, 1, 0x4567);
    copy.take_pose(original);
    OA_CHECK(copy.materialization_revision() == revision && copy.transforms_dirty());
    copy.rebuild_transforms({19, 8111, -37});
    auto eager = original;
    eager.rebuild_transforms({19, 8111, -37});
    OA_CHECK(same_geometry(copy, eager));
    OA_CHECK(original.transforms_dirty());
    for (uint32_t runtime::PieceState::* link :
         {&runtime::PieceState::object_index,
          &runtime::PieceState::parent,
          &runtime::PieceState::first_child,
          &runtime::PieceState::next_sibling}) {
        auto changed = copy;
        changed.pieces()[0].*link ^= 1;
        const auto before = changed.materialization_revision();
        changed.take_pose(original);
        OA_CHECK(changed.materialization_revision() != before);
    }
    for (auto changed :
         {runtime::make_instance(model(), owner, order), runtime::make_instance(source, owner)}) {
        const auto before = changed.materialization_revision();
        changed.take_pose(original);
        OA_CHECK(changed.materialization_revision() != before);
    }
}

/// Checks that optional ingress allocation has both frame and match limits.
void snapshot_warm_budget() {
    oa::app::DeferredModelGeometries table;
    draw::ModelState state;
    size_t room = table.snapshot_warm_frame_limit;
    OA_CHECK(table.prewarm_snapshots(state, 15, room));
    OA_CHECK(state.pieces.empty() && state.pieces.capacity() >= 15);
    const auto used = table.snapshot_warm_frame_limit - room;
    OA_CHECK(used == table.snapshot_warm_bytes);
    OA_CHECK(!table.prewarm_snapshots(state, 15, room));
    draw::ModelState another;
    size_t exhausted{};
    OA_CHECK(!table.prewarm_snapshots(another, 15, exhausted));
    table.snapshot_warm_bytes = table.snapshot_warm_limit;
    OA_CHECK(!table.prewarm_snapshots(another, 15, room));
    OA_CHECK(another.pieces.empty() && another.pieces.capacity() == 0);
}

/// Checks bounded ingress buffer reuse without discarding current root or revisions.
void fresh_image_invalidation() {
    draw::ModelState state;
    state.shift = {17, -211, 41};
    state.transforms_dirty = true;
    state.image_builds = 23;
    state.cache.draws = 9;
    state.cache.has_image = state.cache.image_has_mask = true;
    state.image.pixels.resize(4096);
    state.image.sprite.data = state.image.pixels.data();
    state.image.sprite.width = state.image.sprite.height = 32;
    state.shadow.pixels.resize(65537);
    state.shadow.sprite.data = state.shadow.pixels.data();
    state.deferred_image = draw::ImageFrame{12, 13, 3, 4};
    state.deferred_image_depth = state.image_unfinished = true;
    state.pieces.resize(4);
    state.finer = std::make_unique<draw::FinerModel>();
    const auto image_capacity = state.image.pixels.capacity();
    const auto piece_capacity = state.pieces.capacity();
    oa::app::invalidate_model_images(state);
    OA_CHECK(state.cache.draws == 0 && !state.cache.has_image && !state.cache.image_has_mask);
    OA_CHECK(!state.image.sprite.data && !state.shadow.sprite.data && !state.deferred_image);
    OA_CHECK(!state.deferred_image_depth && !state.image_unfinished && !state.finer);
    OA_CHECK(state.image.pixels.empty() && state.image.pixels.capacity() == image_capacity);
    OA_CHECK(state.shadow.pixels.empty() && state.shadow.pixels.capacity() == 0);
    OA_CHECK(state.pieces.empty() && state.pieces.capacity() == piece_capacity);
    OA_CHECK(state.shift.xy == 17 && state.shift.xz == -211 && state.shift.yz == 41);
    OA_CHECK(state.transforms_dirty && state.image_builds == 23);
}
} // namespace

int main() {
    retained_pose_and_ingress();
    external_materialization_wins();
    root_only_and_refusal();
    image_metadata_gate();
    current_pose_obligations();
    original_only_obligations();
    compatible_copy_revision();
    fresh_image_invalidation();
    snapshot_warm_budget();
    return oa::test::check_exit_status();
}
