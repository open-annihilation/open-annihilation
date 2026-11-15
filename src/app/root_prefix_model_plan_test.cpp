// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rendering recipes leave native model state untouched until a real ordinary
// consumer needs current vectors and image pixels, including owned copies.
#include "root_prefix_model_plan.hpp"

#include "oa/present/model/model_library.hpp"
#include "oa/sim/model_runtime/model_host.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

namespace app = oa::app;
namespace model = oa::present::model;
namespace runtime = oa::sim::model_runtime;
using oa::formats::objects3d::FixedVector3;
constexpr int32_t fixed = 65536;
constexpr uint8_t body_color = 17;
constexpr size_t packet_limit = 64 * 1024;

struct Host final : runtime::ModelHost {
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

/// Makes two ordinary colored pieces with no external content dependencies.
std::shared_ptr<oa::formats::objects3d::Model> make_model() {
    auto source = std::make_shared<oa::formats::objects3d::Model>();
    source->objects.resize(2);
    for (size_t index = 0; index < source->objects.size(); ++index) {
        auto& piece = source->objects[index];
        piece.name = index == 0 ? "base" : "arm";
        piece.offset_from_parent = {static_cast<int32_t>(index) * fixed, 0, 0};
        piece.vertices = {{-2 * fixed, 0, -fixed}, {2 * fixed, 0, -fixed}, {0, 3 * fixed, 2 * fixed}};
        oa::formats::objects3d::Primitive primitive;
        primitive.color_index = body_color;
        primitive.is_colored = 1;
        primitive.vertex_indices = {0, 1, 2};
        piece.primitives.push_back(primitive);
        primitive.vertex_indices = {0, 2, 1};
        piece.primitives.push_back(std::move(primitive));
    }
    source->objects[0].first_child = 1;
    source->objects[1].parent = 0;
    return source;
}

/// Compares native inputs, derived vectors, ownership and dirty/build evidence.
bool same_instance(const runtime::Instance& a, const runtime::Instance& b) {
    const auto same_point = [](FixedVector3 x, FixedVector3 y) {
        return x.x == y.x && x.y == y.y && x.z == y.z;
    };
    if (a.model_handle() != b.model_handle() || a.owner_token() != b.owner_token() ||
        a.root_piece() != b.root_piece() || a.transforms_dirty() != b.transforms_dirty() ||
        a.materialization_revision() != b.materialization_revision() ||
        a.pieces().size() != b.pieces().size())
        return false;
    for (size_t piece = 0; piece < a.pieces().size(); ++piece) {
        const auto& x = a.pieces()[piece];
        const auto& y = b.pieces()[piece];
        if (x.object_index != y.object_index || x.parent != y.parent ||
            x.first_child != y.first_child || x.next_sibling != y.next_sibling ||
            x.flags != y.flags || x.transform_marker != y.transform_marker ||
            !same_point(x.translation, y.translation) ||
            !same_point(x.built_translation, y.built_translation) ||
            x.rotation.xy != y.rotation.xy || x.rotation.xz != y.rotation.xz ||
            x.rotation.yz != y.rotation.yz || x.built_rotation.xy != y.built_rotation.xy ||
            x.built_rotation.xz != y.built_rotation.xz ||
            x.built_rotation.yz != y.built_rotation.yz ||
            !same_point(x.transformed_origin, y.transformed_origin) ||
            x.transformed_vertices.size() != y.transformed_vertices.size())
            return false;
        for (size_t point = 0; point < x.transformed_vertices.size(); ++point)
            if (!same_point(x.transformed_vertices[point], y.transformed_vertices[point]))
                return false;
    }
    return true;
}

/// Compares native geometry independently of the receiver's completed revision.
bool same_geometry(const runtime::Instance& a, const runtime::Instance& b) {
    if (a.pieces().size() != b.pieces().size())
        return false;
    for (size_t piece = 0; piece < a.pieces().size(); ++piece) {
        const auto& x = a.pieces()[piece];
        const auto& y = b.pieces()[piece];
        if (x.transformed_vertices.size() != y.transformed_vertices.size())
            return false;
        for (size_t point = 0; point < x.transformed_vertices.size(); ++point) {
            const auto left = x.transformed_vertices[point];
            const auto right = y.transformed_vertices[point];
            if (left.x != right.x || left.y != right.y || left.z != right.z)
                return false;
        }
    }
    return true;
}

struct Fixture {
    std::shared_ptr<oa::formats::objects3d::Model> source{make_model()};
    oa::World world{};
    oa::Unit unit{};
    oa::UnitDef definition{};
    runtime::Instance instance{};
    model::ModelLibrary library{};
    model::ModelState state{};
    model::ModelRenderer renderer{};
    model::RgbBridge bridge{};
    app::DeferredModelGeometry deferred{};

    /// Starts with completed native geometry and an older image obligation.
    Fixture() {
        unit.id = 1;
        unit.type_index = 1;
        unit.position = {11 * fixed, 9 * fixed, 13 * fixed};
        instance = runtime::make_instance(source, reinterpret_cast<uintptr_t>(&unit));
        instance.rebuild_transforms();
        state.transforms_dirty = false;
        state.cache.draws = 7;
        state.deferred_image = model::ImageFrame{12, 15, 4, 6};
        state.image_builds = 19;
        deferred.instance = &instance;
        deferred.generation = 3;
        deferred.revision = instance.materialization_revision();
        renderer.world = &world;
        renderer.origin_x = renderer.origin_y = 0;
        model::init_composite_buffer(renderer);
    }

    /// Returns this fixture's canonical model and presentation state.
    model::ModelRef ref() {
        return {&instance, &model::prepare_model(library, source), &state, &unit, &definition};
    }
};

/// Builds a trusted exact prefix for one already resolved model pose.
app::RootPrefixRecipe recipe_for(const model::ModelRef& ref) {
    model::set_model_shift(
        ref, {ref.unit->bank, std::bit_cast<int16_t>(ref.unit->heading), ref.unit->pitch}
    );
    auto source = *ref.instance;
    const auto rotation = source.pieces()[source.root_piece()].rotation;
    auto zero = ref.state->shift;
    zero.xz = std::bit_cast<int16_t>(static_cast<uint16_t>(0U - static_cast<uint16_t>(rotation.xz)));
    source.rebuild_transforms(zero);
    const auto placement = app::root_prefix_placement(source);
    OA_CHECK(placement.has_value());
    const auto angle = std::bit_cast<int16_t>(static_cast<uint16_t>(
        static_cast<uint16_t>(rotation.xz) + static_cast<uint16_t>(ref.state->shift.xz)
    ));
    const auto turn = app::prepare_root_prefix_rotation(angle);
    OA_CHECK(turn.has_value());
    const auto geometry = placement
                              ? app::make_root_prefix_geometry(source, *placement, packet_limit)
                              : nullptr;
    OA_CHECK(geometry != nullptr);
    return {geometry, turn.value_or(app::RootPrefixRotation{}), placement.value_or(FixedVector3{})};
}

/// Keeps clean and dirty canonical instances untouched while publishing a draw.
void publication_preserves_canonical_state() {
    for (const bool script_moved : {false, true}) {
        Fixture fixture;
        auto ref = fixture.ref();
        fixture.unit.heading = 17003;
        Host host(fixture.instance);
        if (script_moved)
            host.set_piece_position(fixture.instance.root_piece(), 0, fixed + 19);
        const auto recipe = recipe_for(ref);
        const auto before = fixture.instance;
        const auto unit_before = fixture.unit;
        const auto builds = fixture.state.image_builds;
        app::ModelDraw draw;
        OA_CHECK(app::publish_root_prefix_draw(ref, ref, fixture.deferred, recipe, 7, true, draw));
        OA_CHECK(same_instance(fixture.instance, before));
        OA_CHECK(std::memcmp(&fixture.unit, &unit_before, sizeof(fixture.unit)) == 0);
        OA_CHECK(fixture.state.image_builds == builds);
        OA_CHECK(fixture.state.deferred_image && fixture.state.deferred_image->width == 12);
        OA_CHECK(fixture.deferred.fresh_geometry && fixture.deferred.fresh_original_images);
        OA_CHECK(!fixture.deferred.fresh_copy_images && !fixture.state.transforms_dirty);
        OA_CHECK(draw.root_prefix && draw.root_prefix->geometry == recipe.geometry);
        OA_CHECK(draw.shown_vertices.empty() && draw.plan.linked.drawn && draw.plan.linked.unlit);
        OA_CHECK(draw.plan.model.from_image && draw.plan.model.ground == 7);
    }
}

/// Resolves current raw geometry and genuine pixels when the prototype is refused.
void refusal_materializes_current_geometry() {
    Fixture fixture;
    auto ref = fixture.ref();
    fixture.unit.heading = 17003;
    Host host(fixture.instance);
    host.set_piece_position(fixture.instance.root_piece(), 0, fixed + 19);
    const auto recipe = recipe_for(ref);
    app::ModelDraw draw;
    OA_CHECK(app::publish_root_prefix_draw(ref, ref, fixture.deferred, recipe, 7, false, draw));
    host.set_piece_position(fixture.instance.root_piece(), 2, -2 * fixed + 37);
    auto expected = fixture.instance;
    expected.rebuild_transforms(fixture.state.shift);
    const auto revision = fixture.instance.materialization_revision();
    const auto builds = fixture.state.image_builds;
    OA_CHECK(app::materialize_root_prefix_draw(fixture.renderer, fixture.bridge, draw,
                                             &fixture.deferred));
    OA_CHECK(!draw.root_prefix && draw.plan.linked.drawn && draw.plan.model.from_image);
    OA_CHECK(fixture.instance.materialization_revision() > revision);
    OA_CHECK(!fixture.instance.transforms_dirty() && same_geometry(fixture.instance, expected));
    OA_CHECK(!fixture.state.deferred_image && fixture.state.image.sprite.data != nullptr);
    OA_CHECK(fixture.state.image_builds > builds && !fixture.state.image.pixels.empty());
    OA_CHECK(std::any_of(fixture.state.image.pixels.begin(), fixture.state.image.pixels.end(),
                        [](uint8_t value) { return value == body_color; }));
    OA_CHECK(!fixture.deferred.fresh_geometry && !fixture.deferred.fresh_original_images);
    OA_CHECK(fixture.deferred.revision == fixture.instance.materialization_revision());
    const auto materialized = fixture.instance;
    OA_CHECK(!app::materialize_root_prefix_draw(fixture.renderer, fixture.bridge, draw,
                                              &fixture.deferred));
    OA_CHECK(same_instance(fixture.instance, materialized));
}

/// A copy's ordinary refusal never satisfies its canonical original's debt.
void copied_refusal_preserves_original_obligations() {
    Fixture fixture;
    auto original = fixture.ref();
    Host original_host(fixture.instance);
    original_host.set_piece_position(fixture.instance.root_piece(), 0, fixed + 19);
    auto copy = fixture.instance;
    auto copy_unit = fixture.unit;
    copy_unit.heading = 24571;
    model::ModelState copy_state;
    model::ModelRef shown{&copy, original.prepared, &copy_state, &copy_unit, &fixture.definition};
    Host copy_host(copy);
    copy_host.set_piece_position(1, 1, fixed / 3);
    const auto recipe = recipe_for(shown);
    const auto original_before = fixture.instance;
    const auto copy_before = copy;
    app::ModelDraw draw;
    OA_CHECK(app::publish_root_prefix_draw(original, shown, fixture.deferred, recipe, 7, false, draw));
    OA_CHECK(same_instance(fixture.instance, original_before) && same_instance(copy, copy_before));
    OA_CHECK(fixture.deferred.fresh_geometry && fixture.deferred.fresh_original_images &&
             fixture.deferred.fresh_copy_images);
    copy_host.set_piece_position(1, 2, fixed / 5);
    auto expected = copy;
    expected.rebuild_transforms(copy_state.shift);
    OA_CHECK(app::materialize_root_prefix_draw(fixture.renderer, fixture.bridge, draw,
                                             &fixture.deferred));
    OA_CHECK(same_instance(fixture.instance, original_before));
    OA_CHECK(same_geometry(copy, expected) && copy_state.image.sprite.data != nullptr);
    OA_CHECK(fixture.deferred.fresh_geometry && fixture.deferred.fresh_original_images &&
             !fixture.deferred.fresh_copy_images);
    const auto current_recipe = recipe_for(original);
    OA_CHECK(app::publish_root_prefix_draw(original, original, fixture.deferred, current_recipe,
                                          7, false, draw));
    OA_CHECK(app::materialize_root_prefix_draw(fixture.renderer, fixture.bridge, draw,
                                             &fixture.deferred));
    OA_CHECK(!fixture.deferred.fresh_geometry && !fixture.deferred.fresh_original_images);
}

} // namespace

int main() {
    publication_preserves_canonical_state();
    refusal_materializes_current_geometry();
    copied_refusal_preserves_original_obligations();
    return oa::test::check_exit_status();
}
