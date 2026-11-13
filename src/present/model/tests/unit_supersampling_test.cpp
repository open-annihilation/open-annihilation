// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced anti-aliasing of units: the levels and the samples each draws
// with, Off drawing exactly as without the setting, a fixed composite counted
// in the game's pixels at every level, the coverage of a unit's edges, and
// the finer image kept with the game's.

#include "oa/present/model/unit_supersampling.hpp"
#include "oa/present/display.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using namespace oa::present::model;

constexpr int32_t unit_fixed = 0x10000;
constexpr uint8_t ink = 0x55;
constexpr uint8_t ground = 200;
constexpr int32_t frame_side = 200;

// A flat 16x16 square at height 0 with one coloured primitive, wound so
// that it faces the camera.
std::shared_ptr<oa::formats::objects3d::Model> square_model() {
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    oa::formats::objects3d::Object object;
    const int32_t h = 8 * unit_fixed;
    object.vertices = {{-h, 0, -h}, {h, 0, -h}, {h, 0, h}, {-h, 0, h}};
    oa::formats::objects3d::Primitive primitive;
    primitive.vertex_indices = {0, 3, 2, 1};
    primitive.color_index = ink;
    primitive.is_colored = 1;
    object.primitives.push_back(primitive);
    model->objects.push_back(object);
    return model;
}

oa::Palette gray_palette() {
    oa::Palette palette{};
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        palette.entries[i] = {
            static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0
        };
    return palette;
}

// One unit over a 200x200 RGB frame of palette colour `ground`, drawn
// through a model bridge at a scale of 1.
struct Scene {
    oa::World* world = oa::world_create();
    std::shared_ptr<const oa::formats::objects3d::Model> model = square_model();
    oa::sim::model_runtime::Instance instance = oa::sim::model_runtime::make_instance(model);
    ModelLibrary library;
    ModelState state;
    ModelRenderer renderer;
    ModelDisplay display;
    std::vector<uint8_t> rgb =
        std::vector<uint8_t>(static_cast<std::size_t>(frame_side * frame_side * 3), ground);
    RgbBridge bridge;
    SupersampleScratch scratch;
    oa::Unit* unit{};

    Scene() {
        const oa::WorldCapacity capacity{4, 2, 0};
        // Every case draws this unit, so without its World none can run.
        if (world == nullptr || oa::world_alloc_tables(world, &capacity) == 0) {
            std::fprintf(
                stderr, "%s:%d: the scene's World was not allocated\n", __FILE__, __LINE__
            );
            std::exit(EXIT_FAILURE);
        }
        unit = &world->units[1];
        unit->def = oa::oa_ref_from_index(1);
        unit->position = {100 * unit_fixed, 0, 100 * unit_fixed};
        renderer.world = world;
        renderer.origin_x = 0;
        renderer.origin_y = 0;
        init_composite_buffer(renderer);
        build_model_display(display, gray_palette());
        oa::present::bind_display(&display.context);
        bridge_begin(
            bridge,
            {rgb.data(), frame_side, frame_side, frame_side * 3},
            {0, 0, frame_side - 1, frame_side - 1},
            1.0F,
            display.palette
        );
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    ~Scene() {
        oa::present::bind_display(nullptr);
        oa::world_destroy(world);
    }

    ModelRef ref() {
        return {
            &instance,
            &prepare_model(library, model),
            &state,
            unit,
            oa::world_unit_def_of(world, unit)
        };
    }

    // Draws the unit at a level and writes the bridge back.
    void draw(UnitSupersampling level) {
        bool holds = false;
        draw_unit_supersampled(
            renderer,
            bridge,
            scratch,
            ref(),
            {0, 0, frame_side - 1, frame_side - 1},
            true,
            level,
            holds
        );
        CHECK(holds == (level == UnitSupersampling::off));
        bridge_end(bridge);
    }

    // The red channel of a frame pixel (the palette is gray).
    int level_at(int x, int y) const {
        return rgb[static_cast<std::size_t>((y * frame_side + x) * 3)];
    }
};

constexpr UnitSupersampling finer_levels[] = {
    UnitSupersampling::x2,
    UnitSupersampling::x4,
    UnitSupersampling::x8,
    UnitSupersampling::x16,
};

void levels_and_factors_agree() {
    for (const uint32_t factor : {1U, 2U, 4U, 8U, 16U}) {
        const auto level = unit_supersampling_from_factor(factor);
        CHECK(level.has_value());
        CHECK(level && supersampling_factor(*level) == factor);
    }
    for (const uint32_t factor : {0U, 3U, 5U, 6U, 7U, 9U, 32U})
        CHECK(!unit_supersampling_from_factor(factor).has_value());
}

// A region too large for its level draws at the highest level that fits.
void levels_fit_the_region() {
    CHECK(fitting_supersampling({0, 0, 99, 99}, UnitSupersampling::x16) == UnitSupersampling::x16);
    CHECK(fitting_supersampling({0, 0, 99, 99}, UnitSupersampling::x4) == UnitSupersampling::x4);
    // 2000 x 2000 pixels take 16M samples at x2 and more at x4.
    CHECK(
        fitting_supersampling({0, 0, 1999, 1999}, UnitSupersampling::x16) == UnitSupersampling::x2
    );
    CHECK(
        fitting_supersampling({0, 0, 4999, 4999}, UnitSupersampling::x16) == UnitSupersampling::off
    );
    CHECK(fitting_supersampling({5, 5, 4, 4}, UnitSupersampling::x4) == UnitSupersampling::off);
}

// Off draws exactly what drawing into the bridge did before the setting,
// shadows and all, and lets go of a finer image.
void off_draws_as_without() {
    for (const bool building : {false, true}) {
        Scene plain;
        Scene through;
        for (Scene* scene : {&plain, &through}) {
            scene->renderer.graphics_flags = graphics_shadows | graphics_vehicle_shadows;
            scene->unit->position.x += unit_fixed / 3;
            scene->unit->position.y = 20 * unit_fixed;
            if (building)
                scene->unit->flags |= OA_UNIT_FLAG_BUILDING;
        }
        through.draw(UnitSupersampling::x4);
        CHECK(through.state.finer != nullptr);
        through.rgb = plain.rgb;
        through.draw(UnitSupersampling::off);
        CHECK(through.state.finer == nullptr);
        bridge_open(plain.bridge, {0, 0, frame_side - 1, frame_side - 1});
        draw_linked_model(plain.renderer, &plain.bridge.surface, plain.ref(), true);
        bridge_end(plain.bridge);
        CHECK(plain.rgb == through.rgb);
        CHECK(plain.level_at(100, 90) == ink);
    }
}

/// Requires a verified geometry source before deferring cached shadow pixels.
void metadata_shadow_source() {
    const oa::Rect32 region{0, 0, frame_side - 1, frame_side - 1};
    for (const uint32_t depth : {0U, static_cast<uint32_t>(OA_UNIT_FLAG2_Z_BUFFER)}) {
        Scene pixels;
        Scene ordinary;
        Scene verified;
        Scene unknown;
        for (Scene* scene : {&pixels, &ordinary, &verified, &unknown}) {
            scene->renderer.graphics_flags = graphics_shadows | graphics_vehicle_shadows;
            scene->unit->id = 1;
            scene->unit->type_index = 1;
            scene->unit->position.x += unit_fixed / 3;
            scene->unit->position.y = 20 * unit_fixed;
            scene->unit->flags2 |= depth;
        }
        SupersampledUnitPlan pixel_plan;
        SupersampledUnitPlan ordinary_plan;
        SupersampledUnitPlan verified_plan;
        oa::present::bind_display(&pixels.display.context);
        plan_unit_supersampled(
            pixels.renderer,
            pixels.bridge,
            pixels.ref(),
            region,
            true,
            UnitSupersampling::off,
            pixel_plan
        );
        oa::present::bind_display(&ordinary.display.context);
        // The existing metadata request keeps its default cached-image source.
        plan_unit_supersampled(
            ordinary.renderer,
            ordinary.bridge,
            ordinary.ref(),
            region,
            true,
            UnitSupersampling::off,
            ordinary_plan,
            ModelImagePlan::metadata
        );
        CHECK(ordinary.state.image.sprite.data != nullptr);
        CHECK(!ordinary.state.deferred_image.has_value());
        CHECK(ordinary.state.image.pixels == pixels.state.image.pixels);
        constexpr auto unknown_source = static_cast<ModelShadowSource>(255);
        SupersampledUnitPlan unknown_plan;
        oa::present::bind_display(&unknown.display.context);
        plan_unit_supersampled(
            unknown.renderer,
            unknown.bridge,
            unknown.ref(),
            region,
            true,
            UnitSupersampling::off,
            unknown_plan,
            ModelImagePlan::metadata,
            unknown_source
        );
        CHECK(unknown.state.image.sprite.data != nullptr);
        CHECK(!unknown.state.deferred_image.has_value());
        CHECK(unknown.state.image.pixels == pixels.state.image.pixels);
        oa::present::bind_display(&verified.display.context);
        plan_unit_supersampled(
            verified.renderer,
            verified.bridge,
            verified.ref(),
            region,
            true,
            UnitSupersampling::off,
            verified_plan,
            ModelImagePlan::metadata,
            ModelShadowSource::verified_geometry
        );
        CHECK(verified_plan.level == UnitSupersampling::off);
        CHECK(verified_plan.linked.drawn && verified_plan.model.from_image);
        CHECK(verified.state.image.sprite.data == nullptr && verified.state.image.pixels.empty());
        CHECK(verified.state.deferred_image.has_value());
        CHECK(verified.state.image_builds == pixels.state.image_builds);
        if (verified.state.deferred_image) {
            const auto& frame = *verified.state.deferred_image;
            const auto& image = pixels.state.image.sprite;
            CHECK(frame.width == image.width && frame.height == image.height);
            CHECK(frame.origin_x == image.origin_x && frame.origin_y == image.origin_y);
            CHECK(verified.state.deferred_image_depth == (image.aux != nullptr));
        }
        // A later processor draw materializes the retained frame without
        // changing its logical build count or cached pixels.
        oa::present::bind_display(&pixels.display.context);
        plan_unit_supersampled(
            pixels.renderer,
            pixels.bridge,
            pixels.ref(),
            region,
            true,
            UnitSupersampling::off,
            pixel_plan
        );
        oa::present::bind_display(&verified.display.context);
        plan_unit_supersampled(
            verified.renderer,
            verified.bridge,
            verified.ref(),
            region,
            true,
            UnitSupersampling::off,
            verified_plan
        );
        CHECK(verified.state.image.sprite.data != nullptr);
        CHECK(!verified.state.deferred_image.has_value());
        CHECK(verified.state.image_builds == pixels.state.image_builds);
        CHECK(verified.state.cache.draws == pixels.state.cache.draws);
        CHECK(verified.state.image.pixels == pixels.state.image.pixels);
        CHECK((verified.state.image.sprite.aux != nullptr) == (depth != 0));
        for (const auto& item :
             {std::pair{&pixels, &pixel_plan}, std::pair{&verified, &verified_plan}}) {
            auto& scene = *item.first;
            oa::present::bind_display(&scene.display.context);
            bool holds{};
            draw_planned_unit(
                scene.renderer,
                scene.bridge,
                nullptr,
                scene.scratch,
                scene.ref(),
                *item.second,
                holds
            );
            CHECK(holds);
            bridge_end(scene.bridge);
        }
        CHECK(verified.rgb == pixels.rgb);
        CHECK(pixels.level_at(100, 90) == ink);
        CHECK(pixels.level_at(106, 104) < ground);
    }
    // Sampled tiers continue to build both images from real pixels even
    // when the caller supplies the geometry-source capability.
    for (const UnitSupersampling level : finer_levels) {
        Scene pixels;
        Scene verified;
        for (Scene* scene : {&pixels, &verified}) {
            scene->renderer.graphics_flags = graphics_shadows | graphics_vehicle_shadows;
            scene->unit->position.y = 20 * unit_fixed;
        }
        SupersampledUnitPlan pixel_plan;
        SupersampledUnitPlan verified_plan;
        oa::present::bind_display(&pixels.display.context);
        plan_unit_supersampled(
            pixels.renderer, pixels.bridge, pixels.ref(), region, true, level, pixel_plan
        );
        oa::present::bind_display(&verified.display.context);
        plan_unit_supersampled(
            verified.renderer,
            verified.bridge,
            verified.ref(),
            region,
            true,
            level,
            verified_plan,
            ModelImagePlan::metadata,
            ModelShadowSource::verified_geometry
        );
        CHECK(verified_plan.level == level);
        CHECK(verified.state.image.sprite.data != nullptr);
        CHECK(!verified.state.deferred_image.has_value());
        CHECK(verified.state.image.pixels == pixels.state.image.pixels);
        CHECK(verified.state.finer != nullptr && pixels.state.finer != nullptr);
        if (verified.state.finer && pixels.state.finer) {
            CHECK(verified.state.finer->samples == supersampling_factor(level));
            CHECK(verified.state.finer->state.image.sprite.data != nullptr);
            CHECK(
                verified.state.finer->state.image.pixels == pixels.state.finer->state.image.pixels
            );
        }
    }
}

// A model whose edges fall on pixel boundaries covers whole pixels, so every
// level draws it, and its shadow, exactly as Off does: from an image without
// a depth plane, and from one with a depth plane through the composite.
void whole_pixels_draw_as_off() {
    for (const uint32_t depth : {0U, static_cast<uint32_t>(OA_UNIT_FLAG2_Z_BUFFER)}) {
        Scene off;
        off.renderer.graphics_flags = graphics_shadows | graphics_vehicle_shadows;
        off.unit->position.y = 40 * unit_fixed;
        off.unit->flags2 |= depth;
        off.draw(UnitSupersampling::off);
        CHECK(off.level_at(100, 80) == ink);
        CHECK(off.level_at(106, 104) < ground);
        CHECK((off.state.image.sprite.aux != nullptr) == (depth != 0));
        for (const UnitSupersampling level : finer_levels) {
            Scene finer;
            finer.renderer.graphics_flags = off.renderer.graphics_flags;
            finer.unit->position.y = off.unit->position.y;
            finer.unit->flags2 |= depth;
            finer.draw(level);
            CHECK(finer.rgb == off.rgb);
        }
    }
}

// A profile that keeps the composite's size (limits.model-composite with
// clamp-oversize) counts that size in the game's pixels at every level: a
// unit whose image fits it draws whole, as Off draws it, and a larger one is
// cut where Off cuts it, from its image's top left. A mod that keeps 1280
// pixels and gives its commanders a ground quad of the image key 108 pixels
// across saw them cut at 16x, where the composite held 80 of the game's
// pixels: turned across a slope, a commander kept only its top left part.
void fixed_composite_counts_game_pixels() {
    for (const int32_t side : {48, 12}) {
        const bool fits = side == 48;
        for (const uint32_t depth : {0U, static_cast<uint32_t>(OA_UNIT_FLAG2_Z_BUFFER)}) {
            const auto stage = [&](Scene& scene) {
                scene.renderer.composite_limits = {side, side, true};
                CHECK(init_composite_buffer(scene.renderer));
                // A cut image's silhouette is not drawn at Off, so the cut
                // case draws no shadow.
                scene.renderer.graphics_flags =
                    fits ? graphics_shadows | graphics_vehicle_shadows : 0;
                scene.unit->position.y = 40 * unit_fixed;
                scene.unit->flags2 |= depth;
            };
            Scene off;
            stage(off);
            off.draw(UnitSupersampling::off);
            CHECK(off.level_at(100, 80) == ink);
            // Only a depth image draws through the composite.
            CHECK((off.level_at(106, 86) == ink) == (fits || depth == 0));
            for (const UnitSupersampling level : finer_levels) {
                Scene finer;
                stage(finer);
                finer.draw(level);
                CHECK(finer.rgb == off.rgb);
                // Off after a finer draw cuts as Off does, however far the
                // finer draw grew the composite.
                finer.rgb.assign(finer.rgb.size(), ground);
                finer.draw(UnitSupersampling::off);
                CHECK(finer.rgb == off.rgb);
            }
        }
    }
}

// Half a pixel off the grid, the square's left and right columns are half
// covered and blend half and half with the ground; the rest is the palette
// colour or the ground exactly, and the same draw gives the same pixels.
void edges_blend_by_coverage() {
    std::vector<uint8_t> first;
    for (int run = 0; run < 2; ++run) {
        Scene scene;
        scene.unit->position.x += unit_fixed / 2;
        scene.draw(UnitSupersampling::x4);
        const int half = (ink * 8 + ground * 8 + 8) / 16;
        CHECK(scene.level_at(100, 100) == ink);
        CHECK(scene.level_at(93, 100) == ink);
        CHECK(scene.level_at(92, 100) == half);
        CHECK(scene.level_at(108, 100) == half);
        CHECK(scene.level_at(91, 100) == ground);
        CHECK(scene.level_at(109, 100) == ground);
        CHECK(scene.level_at(100, 92) == ink);
        CHECK(scene.level_at(100, 91) == ground);
        if (run == 0)
            first = scene.rgb;
        else
            CHECK(scene.rgb == first);
    }
}

// The finer image is built again exactly when the game's image is, and when
// the level changes.
void finer_image_follows_the_cache() {
    Scene scene;
    scene.draw(UnitSupersampling::x4);
    CHECK(scene.state.finer != nullptr);
    FinerModel& finer = *scene.state.finer;
    CHECK(finer.samples == 4);
    CHECK(finer.image_builds == scene.state.image_builds);
    CHECK(finer.state.image.sprite.width == 4 * scene.state.image.sprite.width);
    const uint32_t builds = scene.state.image_builds;
    const uint32_t finer_builds = finer.state.image_builds;
    scene.draw(UnitSupersampling::x4);
    CHECK(scene.state.image_builds == builds);
    CHECK(finer.state.image_builds == finer_builds);
    // A cache reset builds both again.
    scene.state.cache.draws = 0;
    scene.draw(UnitSupersampling::x4);
    CHECK(scene.state.image_builds == builds + 1);
    CHECK(finer.image_builds == builds + 1);
    CHECK(finer.state.image_builds == finer_builds + 1);
    // Another level builds the finer image alone.
    scene.draw(UnitSupersampling::x2);
    CHECK(scene.state.image_builds == builds + 1);
    CHECK(finer.samples == 2);
    CHECK(finer.state.image.sprite.width == 2 * scene.state.image.sprite.width);
}

// An unfinished building draws its build effect at every level: its bands
// and outline in the nano colours.
void unfinished_units_draw_their_frame() {
    for (const UnitSupersampling level : {UnitSupersampling::off, UnitSupersampling::x4}) {
        Scene scene;
        scene.unit->flags |= OA_UNIT_FLAG_BUILDING;
        scene.unit->type_index = 1;
        scene.unit->build_remaining = 0.5F;
        scene.draw(level);
        CHECK(scene.state.image.sprite.aux != nullptr);
        int nano = 0;
        for (int y = 85; y < 115; ++y)
            for (int x = 85; x < 115; ++x)
                nano += scene.level_at(x, y) >= 0xa0 && scene.level_at(x, y) <= 0xaf ? 1 : 0;
        CHECK(nano > 0);
    }
}

// A carried unit draws nothing of its own: its carrier draws it.
void carried_unit_draws_nothing() {
    Scene scene;
    scene.unit->attach_parent = oa::oa_ref_from_index(2);
    const std::vector<uint8_t> before = scene.rgb;
    scene.draw(UnitSupersampling::x4);
    CHECK(scene.rgb == before);
    CHECK(scene.state.finer == nullptr);
}

} // namespace

int main() {
    levels_and_factors_agree();
    levels_fit_the_region();
    off_draws_as_without();
    metadata_shadow_source();
    whole_pixels_draw_as_off();
    fixed_composite_counts_game_pixels();
    edges_blend_by_coverage();
    finer_image_follows_the_cache();
    unfinished_units_draw_their_frame();
    carried_unit_draws_nothing();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("unit supersampling: ok");
    return EXIT_SUCCESS;
}
