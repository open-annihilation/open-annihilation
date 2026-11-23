// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "full_polygon_shape.hpp"

#include <limits>
#include <stdexcept>

namespace {

struct ShapeCorner {
    int32_t x{};
    int32_t y{};
    int32_t depth{};
};

struct ShapePolygon {
    size_t first_corner{};
    uint64_t corner_count{};
};

constexpr uint64_t shape_test_start = 0xcbf29ce484222325ULL;
constexpr uint64_t shape_test_multiplier = 0x100000001b3ULL;
constexpr unsigned shape_test_fold_shift = 32;

/// Retains the original push-back algorithm as an independent exact-key and digest reference.
uint64_t reference_shape_words(
    std::span<const ShapeCorner> corners,
    std::span<const ShapePolygon> polygons,
    std::vector<int64_t>& shape
) {
    uint64_t digest = shape_test_start;
    shape.clear();
    const auto fold = [&digest, &shape](int64_t value) {
        digest = (digest ^ static_cast<uint64_t>(value)) * shape_test_multiplier;
        shape.push_back(value);
    };
    fold(static_cast<int64_t>(polygons.size()));
    if (polygons.empty())
        return digest;
    const ShapeCorner& origin = corners[polygons.front().first_corner];
    for (const ShapePolygon& polygon : polygons) {
        fold(static_cast<int64_t>(polygon.corner_count));
        for (const ShapeCorner& corner :
             corners.subspan(polygon.first_corner, polygon.corner_count)) {
            fold(int64_t{corner.x} - origin.x);
            fold(int64_t{corner.y} - origin.y);
            fold(corner.depth);
        }
    }
    return digest ^ (digest >> shape_test_fold_shift);
}

/// Checks every normalized word as well as its digest, including retained-capacity reuse.
uint64_t check_shape_words(
    std::span<const ShapeCorner> corners,
    std::span<const ShapePolygon> polygons,
    std::vector<int64_t>& actual
) {
    namespace shapes = oa::app::full::polygon_shapes;
    std::vector<int64_t> expected;
    const auto expected_digest = reference_shape_words(corners, polygons, expected);
    const auto actual_digest = shapes::write(
        corners, polygons, actual, shape_test_start, shape_test_multiplier, shape_test_fold_shift
    );
    OA_CHECK(actual == expected);
    OA_CHECK(actual_digest == expected_digest);
    const auto needed = shapes::required_words(polygons, actual.max_size());
    OA_CHECK(needed && *needed == actual.size());
    return actual_digest;
}

/// The exact serializer retains empty, signed, translated, growing and overflowing inputs.
void test_polygon_shape_serialization() {
    namespace shapes = oa::app::full::polygon_shapes;
    const std::span<const ShapeCorner> no_corners;
    const std::span<const ShapePolygon> no_polygons;
    std::vector<int64_t> actual{83, -12, 190};
    const auto empty = check_shape_words(no_corners, no_polygons, actual);
    OA_CHECK(actual == std::vector<int64_t>{0});
    OA_CHECK(empty == 0xaf63bd4c8601b7dfULL);
    OA_CHECK(empty == shape_test_start * shape_test_multiplier);
    OA_CHECK(empty != (empty ^ (empty >> shape_test_fold_shift)));
    OA_CHECK(!shapes::required_words(no_polygons, 0));
    OA_CHECK(shapes::required_words(no_polygons, 1) == 1);
    const std::array<ShapeCorner, 1> origin{{{17, -23, 91}}};
    const std::array<ShapePolygon, 1> zero_corners{{{0, 0}}};
    check_shape_words(origin, zero_corners, actual);
    OA_CHECK(actual == (std::vector<int64_t>{1, 0}));
    OA_CHECK(!shapes::required_words(std::span<const ShapePolygon>{zero_corners}, 1));
    OA_CHECK(shapes::required_words(std::span<const ShapePolygon>{zero_corners}, 2) == 2);

    const auto least = std::numeric_limits<int32_t>::min();
    const auto most = std::numeric_limits<int32_t>::max();
    const std::array<ShapeCorner, 4> extremes{
        {{least, most, least}, {most, least, most}, {0, -1, -173}, {-1, 0, 217}}
    };
    const std::array<ShapePolygon, 2> extreme_polygons{{{0, 2}, {2, 2}}};
    check_shape_words(extremes, extreme_polygons, actual);
    OA_CHECK(actual[5] == 4294967295LL);
    OA_CHECK(actual[6] == -4294967295LL);
    const size_t exact = 1 + extreme_polygons.size() + 3 * extremes.size();
    OA_CHECK(!shapes::required_words(std::span<const ShapePolygon>{extreme_polygons}, exact - 1));
    OA_CHECK(
        shapes::required_words(std::span<const ShapePolygon>{extreme_polygons}, exact) == exact
    );
    const std::array<ShapePolygon, 2> sparse{{{3, 1}, {1, 1}}};
    check_shape_words(extremes, sparse, actual);
    OA_CHECK(
        actual == (std::vector<int64_t>{2, 1, 0, 0, 217, 1, 2147483648LL, -2147483648LL, most})
    );

    struct Count32Polygon {
        size_t first_corner{};
        uint32_t corner_count{};
    };

    const std::array<Count32Polygon, 1> count32{{{0, std::numeric_limits<uint32_t>::max()}}};
    constexpr uint64_t count32_words = 2 + 3ULL * std::numeric_limits<uint32_t>::max();
    if constexpr (std::numeric_limits<size_t>::max() >= count32_words) {
        const auto limit = static_cast<size_t>(count32_words);
        OA_CHECK(shapes::required_words(std::span<const Count32Polygon>{count32}, limit) == limit);
        OA_CHECK(!shapes::required_words(std::span<const Count32Polygon>{count32}, limit - 1));
    } else {
        OA_CHECK(!shapes::required_words(
            std::span<const Count32Polygon>{count32}, std::numeric_limits<size_t>::max()
        ));
    }

    std::array<ShapeCorner, 4> moving{{{-27, 16, -9}, {3, -42, 71}, {19, 8, -87}, {5, 0, 3}}};
    const auto digest = check_shape_words(moving, extreme_polygons, actual);
    const auto untranslated = actual;
    for (auto& corner : moving) {
        corner.x += 117;
        corner.y -= 269;
    }
    OA_CHECK(check_shape_words(moving, extreme_polygons, actual) == digest);
    OA_CHECK(actual == untranslated);
    for (auto& corner : moving)
        ++corner.depth;
    OA_CHECK(check_shape_words(moving, extreme_polygons, actual) != digest);
    OA_CHECK(actual != untranslated);

    std::vector<ShapeCorner> many;
    for (int32_t index = 0; index < 512; ++index)
        many.push_back({index * 7 - 513, 127 - index * 3, (index % 19) - 11});
    const std::array<ShapePolygon, 3> many_polygons{{{0, 257}, {257, 255}, {1, 7}}};
    check_shape_words(many, many_polygons, actual);
    const auto* storage = actual.data();
    const auto capacity = actual.capacity();
    check_shape_words(moving, extreme_polygons, actual);
    OA_CHECK(actual.data() == storage && actual.capacity() == capacity);
    check_shape_words(no_corners, no_polygons, actual);
    OA_CHECK(actual.data() == storage && actual.capacity() == capacity);
    check_shape_words(many, many_polygons, actual);
    OA_CHECK(actual.data() == storage && actual.capacity() == capacity);
    const std::array<ShapePolygon, 1> huge{{{0, std::numeric_limits<uint64_t>::max()}}};
    OA_CHECK(!shapes::required_words(
        std::span<const ShapePolygon>{huge}, std::numeric_limits<size_t>::max()
    ));
    const auto before = actual;
    bool length_rejected = false;
    try {
        (void)shapes::write(
            no_corners,
            std::span<const ShapePolygon>{huge},
            actual,
            shape_test_start,
            shape_test_multiplier,
            shape_test_fold_shift
        );
    } catch (const std::length_error&) {
        length_rejected = true;
    }
    OA_CHECK(length_rejected && actual == before);
    OA_CHECK(actual.data() == storage && actual.capacity() == capacity);
    std::vector<int64_t> refused{101, -207};
    const auto refused_before = refused;
    const auto* refused_storage = refused.data();
    const auto refused_capacity = refused.capacity();
    bool allocation_rejected = false;
    emission_allocation_count = 0;
    emission_allocation_bytes = 0;
    watch_emission_allocations = true;
    refuse_next_emission_allocation = true;
    try {
        (void)shapes::write(
            std::span<const ShapeCorner>{extremes},
            std::span<const ShapePolygon>{extreme_polygons},
            refused,
            shape_test_start,
            shape_test_multiplier,
            shape_test_fold_shift
        );
    } catch (const std::bad_alloc&) {
        allocation_rejected = true;
    }
    watch_emission_allocations = false;
    const bool refusal_consumed = !refuse_next_emission_allocation;
    refuse_next_emission_allocation = false;
    OA_CHECK(allocation_rejected && refusal_consumed);
    OA_CHECK(emission_allocation_count == 1 && emission_allocation_bytes != 0);
    OA_CHECK(refused == refused_before);
    OA_CHECK(refused.data() == refused_storage && refused.capacity() == refused_capacity);
}

/// Keeps a dense shared mesh beside the same ordered corners with separate source indices.
std::shared_ptr<Model> shape_frame_grid_model(bool separate_corners, bool beyond_limit = false) {
    auto model = std::make_shared<Model>();
    Object object;
    constexpr uint16_t columns = 17;
    constexpr uint16_t rows = 9;
    for (uint16_t row = 0; row < rows; ++row)
        for (uint16_t column = 0; column < columns; ++column)
            object.vertices.push_back(
                {(int32_t{column} - 8) * 2 * unit,
                 (column % 2 == 0 ? 0 : 6) * unit,
                 (int32_t{row} - 4) * 2 * unit}
            );
    for (uint16_t row = 0; row + 1 < rows; ++row)
        for (uint16_t column = 0; column + 1 < columns; ++column) {
            const auto first = static_cast<uint16_t>(row * columns + column);
            object.primitives.push_back(primitive(
                {first,
                 static_cast<uint16_t>(first + columns),
                 static_cast<uint16_t>(first + columns + 1),
                 static_cast<uint16_t>(first + 1)},
                (row + column) % 2 == 0 ? ink : second_ink,
                nullptr
            ));
        }
    const auto faces = object.primitives;
    object.primitives.insert(object.primitives.end(), faces.rbegin(), faces.rend());
    if (separate_corners) {
        const auto shared = object.vertices;
        object.vertices.clear();
        for (auto& face : object.primitives)
            for (auto& source : face.vertex_indices) {
                const auto point = shared[source];
                source = static_cast<uint16_t>(object.vertices.size());
                object.vertices.push_back(point);
            }
    }
    if (beyond_limit)
        object.vertices.resize(4097);
    model->objects.push_back(std::move(object));
    return model;
}

/// Emits through an actual supersampled target and reads its resolved picture.
CardPicture raster_shape_frame(Card& card, const Scene& scene, full::SceneView view) {
    constexpr uint32_t samples = 4;
    card.canvas.clear();
    card.frame.reset();
    card.stage.begin_frame();
    const auto target =
        card.executor.create_target(card.canvas.width, card.canvas.height, samples, true);
    OA_CHECK(target != card::TargetHandle{});
    if (target == card::TargetHandle{})
        return {};
    view.target = target;
    view.width = card.canvas.width;
    view.height = card.canvas.height;
    card::Batch clear;
    clear.operation = card::Operation::clear;
    clear.target = target;
    card.frame.batches.push_back(clear);
    card.stage.emit_frame(scene.inputs(false), view, card.executor, card.frame);
    card::Batch resolve;
    resolve.operation = card::Operation::resolve;
    resolve.source = target;
    resolve.destination = {0, 0, card.canvas.width, card.canvas.height};
    card.frame.batches.push_back(resolve);
    OA_CHECK(card.stage.upload(card.executor));
    OA_CHECK(card::check_frame(card.frame).empty());
    OA_CHECK(card.executor.execute(card.frame, card.canvas.target));
    auto picture = card.canvas.read();
    card.executor.destroy_target(target);
    return picture;
}

/// Lists a flat piece directly so wrapped origins need no viewport planning shortcut.
void plan_shape_piece(Scene& scene, SceneUnit& actor, bool flat) {
    if (!flat) {
        scene.plan_unit(actor);
        retain_planned_body(scene);
        OA_CHECK(scene.list.models.front().plan.model.from_image);
        OA_CHECK(actor.state.image.sprite.aux != nullptr);
        return;
    }
    ModelDraw drawn;
    drawn.model = scene.ref(actor);
    drawn.plan.linked.drawn = true;
    drawn.plan.linked.unlit = true;
    drawn.plan.model.from_image = false;
    scene.list.models.push_back(std::move(drawn));
    oa::app::add_world_draw(scene.list, WorldDrawKind::model, scene.list.models.size() - 1);
}

/// Complete frame and pixel words permit the same corpus against an ordinary object.
uint64_t shape_frame_signature(const card::CardFrame& frame, const CardPicture& picture) {
    uint64_t result = 14695981039346656037ULL;
    const auto word = [&](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            result ^= (value >> shift) & 255;
            result *= 1099511628211ULL;
        }
    };
    const auto rect = [&](const card::Rect& value) {
        word(static_cast<uint32_t>(value.x));
        word(static_cast<uint32_t>(value.y));
        word(static_cast<uint32_t>(value.width));
        word(static_cast<uint32_t>(value.height));
    };
    word(frame.vertices.size());
    for (const auto& vertex : frame.vertices)
        for (const auto value : vertex_words(vertex))
            word(value);
    word(frame.indices.size());
    for (const auto index : frame.indices)
        word(index);
    word(frame.batches.size());
    for (const auto& batch : frame.batches) {
        word(static_cast<uint8_t>(batch.operation));
        word(batch.target.value);
        word(batch.page.value);
        word(batch.level);
        word(static_cast<uint8_t>(batch.blend));
        word(static_cast<uint8_t>(batch.sampling));
        word(batch.scissored);
        rect(batch.scissor);
        word(batch.first_index);
        word(batch.index_count);
        word(batch.source.value);
        rect(batch.destination);
        word(std::bit_cast<uint32_t>(batch.shift_x));
        word(std::bit_cast<uint32_t>(batch.shift_y));
        rect(batch.source_part);
        for (const auto value : colour_words(batch.colour))
            word(value);
    }
    word(picture.width);
    word(picture.height);
    for (const auto value : picture.rgba)
        word(value);
    return result;
}

/// Dense geometry, view changes and spare source points retain every emitted word.
void test_polygon_shape_frame_references() {
    for (unsigned sample = 0; sample < 6; ++sample) {
        Scene scene;
        auto& shared =
            scene.add_unit(shape_frame_grid_model(false, sample == 5), 1, unit_x, unit_z);
        auto& separate = scene.add_unit(shape_frame_grid_model(true), 2, unit_x, unit_z);
        const bool flat = sample != 1;
        full::SceneView view;
        view.zoom = 1.0F;
        if (sample == 2) {
            view.origin_x = -7.25F;
            view.origin_y = 3.5F;
            view.offset = {0.375, 0.625};
            view.camera_x = -11;
            view.camera_y = -7;
        }
        if (sample == 3) {
            view.camera_x = 32760;
            const auto wrapped =
                std::bit_cast<int32_t>(0x80000000U + 32U * uint32_t{unit} + 0x8000U);
            shared.unit->position.x = separate.unit->position.x = wrapped;
        } else {
            shared.unit->position.x -= unit / 3;
            separate.unit->position.x -= unit / 3;
        }
        if (sample == 4) {
            scene.water_shimmer = 0.5F;
            scene.world->game.sea_level = 4;
        }
        for (auto* actor : {&shared, &separate}) {
            actor->unit->flags2 = flat ? 0 : OA_UNIT_FLAG2_Z_BUFFER;
            actor->unit->heading = 0x2300;
            actor->instance.pieces()[0].flags &=
                static_cast<uint16_t>(~static_cast<uint16_t>(PieceFlag::shaded));
            if (flat)
                actor->instance.pieces()[0].flags &= static_cast<uint16_t>(~piece_cached);
            else
                actor->instance.pieces()[0].flags |= piece_cached;
            actor->instance.rebuild_transforms({0, 0x2300, 0});
        }
        Card candidate(scene), reference(scene);
        candidate.stage.set_body_lod_enabled(true);
        reference.stage.set_body_lod_enabled(true);
        ProcessorPicture processor;
        scene.begin_frame(processor, 0);
        scene.list.shadow_level = 0;
        plan_shape_piece(scene, shared, flat);
        const auto actual = raster_shape_frame(candidate, scene, view);
        scene.begin_frame(processor, 0);
        scene.list.shadow_level = 0;
        plan_shape_piece(scene, separate, flat);
        const auto expected = raster_shape_frame(reference, scene, view);
        check_body_frame_suffix(candidate.frame, reference.frame, 0, 0);
        OA_CHECK(actual.rgba == expected.rgba);
        OA_CHECK(pixels_with_alpha(actual, 255) > 0);
        std::printf(
            "POLYGON_SHAPE_SIGNATURE references-%u %016llx\n",
            sample,
            static_cast<unsigned long long>(shape_frame_signature(candidate.frame, actual))
        );
    }
}

/// Moving and interpolated endpoints retain complete physical frame and readback signatures.
void test_polygon_shape_animated_frames() {
    for (const float zoom : {0.5F, 0.85F, 2.0F}) {
        Scene scene;
        auto model = body_lod_model();
        for (auto& object : model->objects) {
            const auto faces = object.primitives;
            for (unsigned repeat = 1; repeat < 4; ++repeat)
                object.primitives.insert(object.primitives.end(), faces.begin(), faces.end());
        }
        auto& actor = scene.add_unit(model, 1, unit_x, unit_z);
        actor.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
        actor.instance.pieces()[0].flags |= piece_cached;
        actor.instance.pieces()[1].flags &= static_cast<uint16_t>(~piece_cached);
        Card candidate(scene, zoom);
        candidate.stage.set_body_lod_enabled(true);
        full::SceneView view;
        view.zoom = zoom;
        ProcessorPicture processor;
        std::vector<uint8_t> first_pixels;
        for (unsigned pose = 0; pose < 3; ++pose) {
            actor.unit->heading = static_cast<uint16_t>(pose * 0x1300U);
            actor.instance.pieces()[1].translation.x = static_cast<int32_t>(pose) * unit / 3;
            actor.instance.rebuild_transforms({0, static_cast<int16_t>(actor.unit->heading), 0});
            ++scene.tick;
            scene.begin_frame(processor, 0);
            scene.list.shadow_level = 0;
            scene.plan_unit(actor);
            OA_CHECK(scene.list.models.back().plan.model.from_image);
            OA_CHECK(actor.state.image.sprite.aux != nullptr);
            if (pose == 2)
                for (auto& point : actor.instance.pieces()[1].transformed_vertices)
                    point.x += unit / 3;
            const auto picture = raster_shape_frame(candidate, scene, view);
            OA_CHECK(!candidate.frame.vertices.empty() && !candidate.frame.indices.empty());
            OA_CHECK(pixels_with_alpha(picture, 255) > 0);
            const auto& bodies = candidate.stage.body_lod_counts();
            if (zoom < 1.0F)
                OA_CHECK(bodies.body_lod_models == 1 && bodies.full_mesh_models == 0);
            else
                OA_CHECK(bodies.full_mesh_models == 1 && bodies.body_lod_models == 0);
            if (pose == 0)
                first_pixels = picture.rgba;
            else
                OA_CHECK(picture.rgba != first_pixels);
            std::printf(
                "POLYGON_SHAPE_SIGNATURE animated-%u-%u %016llx\n",
                unsigned(std::lround(zoom * 100)),
                pose,
                static_cast<unsigned long long>(shape_frame_signature(candidate.frame, picture))
            );
        }
    }
}

/// A full physical model retains its complete frame with Enhanced Mode disabled.
void test_polygon_shape_disabled_control() {
    Scene scene;
    auto& actor = scene.add_unit(shape_frame_grid_model(false), 1, unit_x, unit_z);
    actor.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    actor.unit->heading = 0x2300;
    actor.instance.pieces()[0].flags |= piece_cached;
    actor.instance.rebuild_transforms({0, 0x2300, 0});
    Card ordinary(scene), enhanced(scene);
    ordinary.stage.set_body_lod_enabled(false);
    enhanced.stage.set_body_lod_enabled(true);
    full::SceneView view;
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.list.shadow_level = 0;
    scene.plan_unit(actor);
    retain_planned_body(scene);
    OA_CHECK(scene.list.models.front().plan.model.from_image);
    OA_CHECK(actor.state.image.sprite.aux != nullptr);
    const auto actual = raster_shape_frame(ordinary, scene, view);
    scene.begin_frame(processor, 0);
    scene.list.shadow_level = 0;
    scene.plan_unit(actor);
    retain_planned_body(scene);
    OA_CHECK(scene.list.models.front().plan.model.from_image);
    OA_CHECK(actor.state.image.sprite.aux != nullptr);
    const auto expected = raster_shape_frame(enhanced, scene, view);
    check_body_frame_suffix(ordinary.frame, enhanced.frame, 0, 0);
    OA_CHECK(actual.rgba == expected.rgba);
    OA_CHECK(pixels_with_alpha(actual, 255) > 0);
    for (const auto* stage : {&ordinary.stage, &enhanced.stage}) {
        const auto& counts = stage->body_lod_counts();
        OA_CHECK(counts.full_mesh_models == 1 && counts.body_lod_models == 0);
    }
    std::printf(
        "POLYGON_SHAPE_SIGNATURE enhanced-off %016llx\n",
        static_cast<unsigned long long>(shape_frame_signature(ordinary.frame, actual))
    );
}

} // namespace
