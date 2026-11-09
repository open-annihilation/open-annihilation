// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A unit carrying a building and a mobile unit, planned once
// (plan_unit_supersampled) and drawn from the plan through the bridge whole
// and in 1 to 7 bands (draw_planned_unit), gives the frame a draw of the
// whole (draw_linked_model into the bridge) gives, frame after frame: with
// the carrier's image with a depth plane, where the carried units' images
// are built and composed into its composite (the building's through the
// composite itself when buildings are drawn anti-aliased), and without one,
// where they are drawn flat.

#include "oa/present/model/rgb_bridge.hpp"
#include "oa/present/model/unit_supersampling.hpp"
#include "oa/present/display.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
constexpr uint8_t ground = 200;
constexpr int32_t frame_side = 160;
constexpr int32_t frames = 4;
constexpr int32_t most_bands = 7;
// Unit slots: the carrier, the carried building and the carried mobile unit.
constexpr uint32_t carrier_slot = 1;
constexpr uint32_t building_slot = 2;
constexpr uint32_t mobile_slot = 3;

// A square `side` units across at `height`, in palette index `color`, wound
// to face the camera, with a smaller square of `top_color` above it as its
// child piece, so that the model has two pieces.
std::shared_ptr<oa::formats::objects3d::Model>
two_piece_model(int32_t side, int32_t height, uint8_t color, uint8_t top_color) {
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    const auto square = [](int32_t half, int32_t y, uint8_t ink) {
        oa::formats::objects3d::Object object;
        object.vertices = {{-half, y, -half}, {half, y, -half}, {half, y, half}, {-half, y, half}};
        oa::formats::objects3d::Primitive primitive;
        primitive.vertex_indices = {0, 3, 2, 1};
        primitive.color_index = ink;
        primitive.is_colored = 1;
        object.primitives.push_back(primitive);
        return object;
    };
    model->objects.push_back(square(side * unit_fixed / 2, height * unit_fixed, color));
    auto top = square(side * unit_fixed / 4, (height + 3) * unit_fixed, top_color);
    top.parent = 0;
    model->objects[0].first_child = 1;
    model->objects.push_back(top);
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

// One carrier and the two units it carries over an RGB frame of palette
// colour `ground`, drawn through a model bridge at a scale of 1.
struct Scene {
    oa::World* world = oa::world_create();
    std::vector<std::shared_ptr<const oa::formats::objects3d::Model>> models;
    std::vector<oa::sim::model_runtime::Instance> instances;
    std::vector<ModelState> states;
    ModelLibrary library;
    ModelRenderer renderer;
    ModelDisplay display;
    std::vector<uint8_t> rgb =
        std::vector<uint8_t>(static_cast<std::size_t>(frame_side * frame_side * 3), ground);
    RgbBridge bridge;
    SupersampleScratch scratch;

    /// Sets up the scene.
    ///
    /// @param depth_image the carrier draws from an image with a depth plane
    /// @param anti_alias buildings' images are drawn anti-aliased
    Scene(bool depth_image, bool anti_alias) {
        const oa::WorldCapacity capacity{8, 2, 0};
        oa::world_alloc_tables(world, &capacity);
        models.push_back(two_piece_model(24, 0, 0x40, 0x48));
        models.push_back(two_piece_model(10, 6, 0x60, 0x68));
        models.push_back(two_piece_model(8, 9, 0x80, 0x88));
        for (const auto& model : models)
            instances.push_back(oa::sim::model_runtime::make_instance(model));
        states.resize(models.size());
        oa::Unit& carrier = world->units[carrier_slot];
        oa::Unit& building = world->units[building_slot];
        oa::Unit& mobile = world->units[mobile_slot];
        for (oa::Unit* unit : {&carrier, &building, &mobile})
            unit->def = oa::oa_ref_from_index(1);
        carrier.position = {80 * unit_fixed, 0, 80 * unit_fixed};
        if (depth_image)
            carrier.flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
        carrier.attach_first_child = oa::oa_ref_from_index(building_slot);
        building.attach_parent = oa::oa_ref_from_index(carrier_slot);
        building.attach_next = oa::oa_ref_from_index(mobile_slot);
        building.flags |= OA_UNIT_FLAG_BUILDING;
        building.type_index = 1;
        building.position = {76 * unit_fixed, 4 * unit_fixed, 78 * unit_fixed};
        mobile.attach_parent = oa::oa_ref_from_index(carrier_slot);
        mobile.position = {87 * unit_fixed, 2 * unit_fixed, 84 * unit_fixed};
        mobile.build_remaining = 0.25F;
        renderer.world = world;
        renderer.origin_x = 0;
        renderer.origin_y = 0;
        renderer.graphics_flags = anti_alias ? graphics_anti_alias : uint16_t{0};
        renderer.user = this;
        renderer.model_of = [](void* user, const oa::Unit& unit) {
            return static_cast<Scene*>(user)->ref(unit);
        };
        init_composite_buffer(renderer);
        build_model_display(display, gray_palette());
        oa::present::bind_display(&display.context);
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    ~Scene() {
        oa::present::bind_display(nullptr);
        oa::world_destroy(world);
    }

    /// Returns a unit's model.
    ///
    /// @param unit one of the scene's units
    /// @return its model, or a null instance for any other record
    ModelRef ref(const oa::Unit& unit) {
        const auto slot = static_cast<std::size_t>(&unit - world->units);
        if (slot < carrier_slot || slot > mobile_slot)
            return {};
        const std::size_t index = slot - carrier_slot;
        return {
            &instances[index],
            &prepare_model(library, models[index]),
            &states[index],
            &world->units[slot],
            oa::world_unit_def_of(world, &world->units[slot])
        };
    }

    /// Returns the carrier's model.
    ///
    /// @return its model
    ModelRef carrier() { return ref(world->units[carrier_slot]); }

    /// Moves the carried units' top pieces and the carrier a little, as a
    /// tick's scripts would, before a frame.
    ///
    /// @param frame the frame about to be drawn
    void step(int32_t frame) {
        for (auto& instance : instances)
            instance.pieces()[1].rotation.xz = static_cast<int16_t>(frame * 0x1800);
        world->units[carrier_slot].position.x += unit_fixed / 2;
        for (std::size_t index = 0; index < states.size(); ++index) {
            states[index].transforms_dirty = true;
            note_piece_changes(ref(world->units[carrier_slot + index]));
        }
    }

    /// Starts a frame's drawing through the bridge.
    void begin() {
        bridge_begin(
            bridge,
            {rgb.data(), frame_side, frame_side, frame_side * 3},
            {0, 0, frame_side - 1, frame_side - 1},
            1.0F,
            display.palette
        );
    }
};

// Rect32 is packed: aligned here, since the bridge takes its fields by reference.
alignas(4) constexpr oa::Rect32 whole_frame{0, 0, frame_side - 1, frame_side - 1};
alignas(4) constexpr oa::Rect32 unit_region{56, 56, 112, 112};

// The carrier drawn whole, from a plan and in bands, frame after frame,
// gives the same bytes.
void planned_bands_draw_as_whole() {
    for (const bool depth_image : {true, false}) {
        for (const bool anti_alias : {false, true}) {
            Scene whole(depth_image, anti_alias);
            Scene planned(depth_image, anti_alias);
            bool all_equal = true;
            for (int32_t frame = 0; frame < frames; ++frame) {
                whole.step(frame);
                planned.step(frame);
                whole.begin();
                bridge_open(whole.bridge, whole_frame);
                draw_linked_model(whole.renderer, &whole.bridge.surface, whole.carrier(), false);
                bridge_end(whole.bridge);
                planned.begin();
                SupersampledUnitPlan plan;
                plan_unit_supersampled(
                    planned.renderer,
                    planned.bridge,
                    planned.carrier(),
                    whole_frame,
                    false,
                    UnitSupersampling::off,
                    plan
                );
                const std::vector<uint8_t> start = planned.rgb;
                for (int32_t count = 1; count <= most_bands; ++count) {
                    planned.rgb = start;
                    planned.begin();
                    std::vector<BridgeBand> bands;
                    bridge_split(planned.bridge, count, bands);
                    for (BridgeBand& band : bands) {
                        bool holds = false;
                        draw_planned_unit(
                            planned.renderer,
                            planned.bridge,
                            &band,
                            planned.scratch,
                            planned.carrier(),
                            plan,
                            holds
                        );
                        bridge_end(planned.bridge, band);
                    }
                    all_equal = all_equal && planned.rgb == whole.rgb;
                }
                // The carried units' images are built as carried, and the
                // carried building's through the composite when anti-aliased.
                CHECK(plan.model.from_image);
                if (depth_image) {
                    CHECK(plan.model.carried.size() == 2);
                    CHECK(
                        plan.model.carried.size() == 2 && plan.model.carried[0].image.sprite.data
                    );
                    CHECK(
                        plan.model.carried.size() == 2 &&
                        plan.model.carried[0].built_in_composite == anti_alias
                    );
                    CHECK(
                        plan.model.carried.size() == 2 && !plan.model.carried[1].built_in_composite
                    );
                }
            }
            CHECK(all_equal);
            if (!all_equal)
                std::fprintf(
                    stderr,
                    "depth image %d, anti-aliasing %d: the planned bands differ\n",
                    depth_image ? 1 : 0,
                    anti_alias ? 1 : 0
                );
            // Something was drawn: the frame is not bare ground.
            bool drawn = false;
            for (const uint8_t byte : whole.rgb)
                drawn = drawn || byte != ground;
            CHECK(drawn);
        }
    }
}

/// Checks whole-frame parity for tight regions, including carried pieces at band edges.
void tight_plans_draw_as_whole() {
    for (const auto level :
         {UnitSupersampling::off, UnitSupersampling::x2, UnitSupersampling::x4}) {
        for (const bool depth_image : {true, false}) {
            for (const bool anti_alias : {false, true}) {
                Scene whole(depth_image, anti_alias);
                Scene planned(depth_image, anti_alias);
                for (int32_t frame = 0; frame < frames; ++frame) {
                    whole.step(frame);
                    planned.step(frame);
                    whole.begin();
                    SupersampledUnitPlan full_plan;
                    plan_unit_supersampled(
                        whole.renderer,
                        whole.bridge,
                        whole.carrier(),
                        unit_region,
                        false,
                        level,
                        full_plan
                    );
                    bool holds = false;
                    draw_planned_unit(
                        whole.renderer,
                        whole.bridge,
                        nullptr,
                        whole.scratch,
                        whole.carrier(),
                        full_plan,
                        holds
                    );
                    bridge_end(whole.bridge);
                    planned.begin();
                    SupersampledUnitPlan plan;
                    plan_unit_supersampled(
                        planned.renderer,
                        planned.bridge,
                        planned.carrier(),
                        unit_region,
                        false,
                        level,
                        plan
                    );
                    CHECK(plan.level == level);
                    const std::vector<uint8_t> start = planned.rgb;
                    for (int32_t count = 1; count <= most_bands; ++count) {
                        planned.rgb = start;
                        planned.begin();
                        std::vector<BridgeBand> bands;
                        bridge_split(planned.bridge, count, bands);
                        for (auto& band : bands) {
                            holds = false;
                            draw_planned_unit(
                                planned.renderer,
                                planned.bridge,
                                &band,
                                planned.scratch,
                                planned.carrier(),
                                plan,
                                holds
                            );
                            bridge_end(planned.bridge, band);
                        }
                        CHECK(planned.rgb == whole.rgb);
                    }
                }
            }
        }
    }
}

/// Checks that off-band draws skip workspaces while retaining required flushes.
void off_band_draw_preserves_workspaces_and_flushes() {
    constexpr uint8_t workspace_ink = 165;
    constexpr uint8_t bridge_ink = 44;
    constexpr uint8_t direct_ink = 77;
    constexpr int32_t changed_x = 8;
    constexpr int32_t changed_y = 8;
    alignas(4) constexpr oa::Rect32 pending_region{0, 0, 31, 31};
    for (const auto level : {UnitSupersampling::off, UnitSupersampling::x2}) {
        Scene scene(true, true);
        scene.begin();
        SupersampledUnitPlan plan;
        plan_unit_supersampled(
            scene.renderer, scene.bridge, scene.carrier(), unit_region, false, level, plan
        );
        CHECK(plan.linked.drawn);
        CHECK(plan.level == level);
        std::vector<BridgeBand> bands;
        bridge_split(scene.bridge, 4, bands);
        BridgeBand& band = bands.front();
        CHECK(band.end_row <= unit_region.y1);
        CHECK(level == UnitSupersampling::off || band.end_row <= plan.covered.y1);
        bridge_open(scene.bridge, band, pending_region);
        band.surface.pixels[changed_y * band.surface.pitch + changed_x] = bridge_ink;
        const auto pixel = static_cast<std::size_t>((changed_y * frame_side + changed_x) * 3);
        CHECK(scene.rgb[pixel] == ground);
        std::fill(
            scene.renderer.composite.pixels.begin(),
            scene.renderer.composite.pixels.end(),
            workspace_ink
        );
        const auto before = scene.renderer.composite.pixels;
        const auto samples_before = scene.scratch.region.samples;
        bool holds = true;
        draw_planned_unit(
            scene.renderer, scene.bridge, &band, scene.scratch, scene.carrier(), plan, holds
        );
        CHECK(scene.renderer.composite.pixels == before);
        CHECK(scene.scratch.region.samples == samples_before);
        CHECK(holds == (level == UnitSupersampling::off));
        if (level == UnitSupersampling::off) {
            CHECK(!band.open_tiles.empty());
            CHECK(scene.rgb[pixel] == ground);
            bridge_end(scene.bridge, band);
            CHECK(scene.rgb[pixel] == bridge_ink);
        } else {
            CHECK(band.open_tiles.empty());
            CHECK(scene.rgb[pixel] == bridge_ink);
            for (std::size_t channel = 0; channel < 3; ++channel)
                scene.rgb[pixel + channel] = direct_ink;
            bridge_end(scene.bridge, band);
            CHECK(scene.rgb[pixel] == direct_ink);
        }
    }
}

/// A draw whose cached pixels wait keeps the same image state and carried units.
void metadata_plans_keep_cache_and_cargo() {
    for (const bool depth_image : {false, true}) {
        for (const bool anti_alias : {false, true}) {
            Scene pixels(depth_image, anti_alias);
            Scene metadata(depth_image, anti_alias);
            for (int32_t frame = 0; frame < frames; ++frame) {
                for (Scene* scene : {&pixels, &metadata}) {
                    scene->world->units[carrier_slot].build_remaining = frame == 0 ? 0.25F : 0;
                    const auto cached =
                        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
                    const auto visible =
                        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
                    if (frame == 2)
                        scene->instances[0].pieces()[1].flags &= static_cast<uint16_t>(~cached);
                    if (frame == frames - 1)
                        scene->instances[0].pieces()[0].flags &= static_cast<uint16_t>(~visible);
                    if (frame == frames - 1)
                        scene->world->units[building_slot].flags |=
                            unit_flag_attached_without_piece;
                    scene->step(frame);
                    scene->begin();
                }
                SupersampledUnitPlan reference;
                SupersampledUnitPlan planned;
                plan_unit_supersampled(
                    pixels.renderer,
                    pixels.bridge,
                    pixels.carrier(),
                    whole_frame,
                    false,
                    UnitSupersampling::off,
                    reference
                );
                plan_unit_supersampled(
                    metadata.renderer,
                    metadata.bridge,
                    metadata.carrier(),
                    whole_frame,
                    false,
                    UnitSupersampling::off,
                    planned,
                    ModelImagePlan::metadata
                );
                CHECK(planned.linked.drawn == reference.linked.drawn);
                CHECK(planned.linked.unlit == reference.linked.unlit);
                CHECK(planned.model.from_image == reference.model.from_image);
                CHECK(planned.model.ground == reference.model.ground);
                CHECK(planned.model.carried.size() == reference.model.carried.size());
                for (std::size_t index = 0; index < reference.model.carried.size(); ++index) {
                    if (index >= planned.model.carried.size())
                        break;
                    CHECK(
                        reference.model.carried[index].unit - pixels.world->units ==
                        planned.model.carried[index].unit - metadata.world->units
                    );
                    CHECK(planned.model.carried[index].image.pixels.empty());
                    CHECK(planned.model.carried[index].image.sprite.data == nullptr);
                }
                const auto& original = pixels.states[0];
                const auto& deferred = metadata.states[0];
                CHECK(deferred.cache.draws == original.cache.draws);
                CHECK(deferred.cache.has_image == original.cache.has_image);
                CHECK(deferred.cache.image_has_mask == original.cache.image_has_mask);
                CHECK(deferred.image_builds == original.image_builds);
                CHECK(deferred.image_unfinished == original.image_unfinished);
                CHECK(deferred.image.pixels.empty());
                CHECK(deferred.image.sprite.data == nullptr);
                CHECK(deferred.shadow.sprite.data == nullptr);
                CHECK(
                    deferred.deferred_image.has_value() == (original.image.sprite.data != nullptr)
                );
                if (deferred.deferred_image) {
                    const auto& image = original.image.sprite;
                    CHECK(deferred.deferred_image->width == image.width);
                    CHECK(deferred.deferred_image->height == image.height);
                    CHECK(deferred.deferred_image->origin_x == image.origin_x);
                    CHECK(deferred.deferred_image->origin_y == image.origin_y);
                    CHECK(deferred.deferred_image_depth == (image.aux != nullptr));
                }
                for (std::size_t index = 1; index < pixels.states.size(); ++index) {
                    const auto& child = pixels.states[index];
                    const auto& deferred_child = metadata.states[index];
                    CHECK(deferred_child.image_builds == child.image_builds);
                    CHECK(deferred_child.image_unfinished == child.image_unfinished);
                    CHECK(deferred_child.image.pixels.empty());
                    CHECK(deferred_child.image.sprite.data == nullptr);
                    CHECK(
                        deferred_child.deferred_image.has_value() ==
                        (child.image.sprite.data != nullptr)
                    );
                }
            }
        }
    }
}

/// An ordinary pass after a metadata pass fills its pixels before drawing, including shadows.
void deferred_pixels_materialize_before_an_ordinary_draw() {
    alignas(4) constexpr oa::Rect32 outside_frame{
        frame_side, frame_side, 2 * frame_side, 2 * frame_side
    };
    for (const bool depth_image : {false, true}) {
        for (const bool anti_alias : {false, true}) {
            for (const bool shadows : {false, true}) {
                Scene pixels(depth_image, anti_alias);
                Scene metadata(depth_image, anti_alias);
                pixels.begin();
                metadata.begin();
                SupersampledUnitPlan reference;
                SupersampledUnitPlan deferred;
                plan_unit_supersampled(
                    pixels.renderer,
                    pixels.bridge,
                    pixels.carrier(),
                    outside_frame,
                    false,
                    UnitSupersampling::off,
                    reference
                );
                plan_unit_supersampled(
                    metadata.renderer,
                    metadata.bridge,
                    metadata.carrier(),
                    outside_frame,
                    false,
                    UnitSupersampling::off,
                    deferred,
                    ModelImagePlan::metadata
                );
                CHECK(metadata.states[0].deferred_image.has_value());
                CHECK(metadata.states[0].image.sprite.data == nullptr);
                // A failed backend repeats the frame's planning in the ordinary mode.
                // Enabling shadows also requires the same pixels before any draw reads them.
                if (shadows) {
                    pixels.renderer.graphics_flags |= graphics_shadows | graphics_vehicle_shadows;
                    metadata.renderer.graphics_flags |= graphics_shadows | graphics_vehicle_shadows;
                }
                plan_unit_supersampled(
                    pixels.renderer,
                    pixels.bridge,
                    pixels.carrier(),
                    whole_frame,
                    false,
                    UnitSupersampling::off,
                    reference
                );
                plan_unit_supersampled(
                    metadata.renderer,
                    metadata.bridge,
                    metadata.carrier(),
                    whole_frame,
                    false,
                    UnitSupersampling::off,
                    deferred
                );
                CHECK(!metadata.states[0].deferred_image.has_value());
                CHECK(metadata.states[0].image.sprite.data != nullptr);
                CHECK(
                    (metadata.states[0].image.sprite.aux != nullptr) ==
                    (pixels.states[0].image.sprite.aux != nullptr)
                );
                CHECK(metadata.states[0].cache.draws == pixels.states[0].cache.draws);
                bool holds = false;
                draw_planned_unit(
                    pixels.renderer,
                    pixels.bridge,
                    nullptr,
                    pixels.scratch,
                    pixels.carrier(),
                    reference,
                    holds
                );
                bridge_end(pixels.bridge);
                holds = false;
                draw_planned_unit(
                    metadata.renderer,
                    metadata.bridge,
                    nullptr,
                    metadata.scratch,
                    metadata.carrier(),
                    deferred,
                    holds
                );
                bridge_end(metadata.bridge);
                CHECK(metadata.rgb == pixels.rgb);
                CHECK(std::any_of(metadata.rgb.begin(), metadata.rgb.end(), [](uint8_t byte) {
                    return byte != ground;
                }));
            }
        }
    }
}

/// Moving pieces cannot change the old cached frame when its deferred pixels are filled.
void deferred_pixels_keep_the_cached_frame() {
    constexpr int32_t moving_piece_offset = 64 * unit_fixed;
    const auto cached = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    for (const bool depth_image : {false, true}) {
        Scene pixels(depth_image, false);
        Scene metadata(depth_image, false);
        for (Scene* scene : {&pixels, &metadata}) {
            scene->world->units[carrier_slot].attach_first_child = 0;
            scene->instances[0].pieces()[1].flags &= static_cast<uint16_t>(~cached);
            note_piece_changes(scene->carrier());
            scene->begin();
        }
        SupersampledUnitPlan reference;
        SupersampledUnitPlan deferred;
        plan_unit_supersampled(
            pixels.renderer,
            pixels.bridge,
            pixels.carrier(),
            whole_frame,
            false,
            UnitSupersampling::off,
            reference
        );
        plan_unit_supersampled(
            metadata.renderer,
            metadata.bridge,
            metadata.carrier(),
            whole_frame,
            false,
            UnitSupersampling::off,
            deferred,
            ModelImagePlan::metadata
        );
        const auto original_frame = pixels.states[0].image.sprite;
        const auto original_pixels = pixels.states[0].image.pixels;
        for (Scene* scene : {&pixels, &metadata}) {
            scene->instances[0].pieces()[1].translation.x = moving_piece_offset;
            scene->states[0].transforms_dirty = true;
            note_piece_changes(scene->carrier());
        }
        plan_unit_supersampled(
            pixels.renderer,
            pixels.bridge,
            pixels.carrier(),
            whole_frame,
            false,
            UnitSupersampling::off,
            reference
        );
        plan_unit_supersampled(
            metadata.renderer,
            metadata.bridge,
            metadata.carrier(),
            whole_frame,
            false,
            UnitSupersampling::off,
            deferred
        );
        const auto current_frame = measure_model_bounds(pixels.instances[0], nullptr);
        CHECK(current_frame.width > original_frame.width);
        CHECK(pixels.states[0].image.pixels == original_pixels);
        const auto& image = metadata.states[0].image.sprite;
        CHECK(image.data != nullptr);
        CHECK(!metadata.states[0].deferred_image.has_value());
        CHECK(image.width == original_frame.width);
        CHECK(image.height == original_frame.height);
        CHECK(image.origin_x == original_frame.origin_x);
        CHECK(image.origin_y == original_frame.origin_y);
        CHECK((image.aux != nullptr) == depth_image);
        CHECK(metadata.states[0].image.pixels == original_pixels);
        bool holds = false;
        draw_planned_unit(
            pixels.renderer,
            pixels.bridge,
            nullptr,
            pixels.scratch,
            pixels.carrier(),
            reference,
            holds
        );
        bridge_end(pixels.bridge);
        holds = false;
        draw_planned_unit(
            metadata.renderer,
            metadata.bridge,
            nullptr,
            metadata.scratch,
            metadata.carrier(),
            deferred,
            holds
        );
        bridge_end(metadata.bridge);
        CHECK(metadata.rgb == pixels.rgb);
        CHECK(std::any_of(metadata.rgb.begin(), metadata.rgb.end(), [](uint8_t byte) {
            return byte != ground;
        }));
    }
}

/// An image above the ordinary allocation bound has no logical image either.
void metadata_rejects_an_oversized_image() {
    constexpr int32_t oversized_side = 8192;
    for (const auto mode : {ModelImagePlan::pixels, ModelImagePlan::metadata}) {
        Scene scene(true, false);
        scene.models[0] = two_piece_model(oversized_side, 0, 64, 72);
        scene.instances[0] = oa::sim::model_runtime::make_instance(scene.models[0]);
        scene.begin();
        SupersampledUnitPlan plan;
        plan_unit_supersampled(
            scene.renderer,
            scene.bridge,
            scene.carrier(),
            whole_frame,
            false,
            UnitSupersampling::off,
            plan,
            mode
        );
        CHECK(plan.linked.drawn);
        CHECK(!plan.model.from_image);
        CHECK(plan.model.carried.empty());
        CHECK(!scene.states[0].cache.has_image);
        CHECK(!scene.states[0].deferred_image.has_value());
        CHECK(scene.states[0].image.sprite.data == nullptr);
        CHECK(scene.states[0].image_builds == 0);
    }
}

/// An all-hidden model still owns the ordinary four-pixel-margin image.
void metadata_keeps_an_all_hidden_image() {
    constexpr int32_t empty_image_side = 4;
    for (const auto mode : {ModelImagePlan::pixels, ModelImagePlan::metadata}) {
        Scene scene(false, false);
        scene.world->units[carrier_slot].attach_first_child = 0;
        for (auto& piece : scene.instances[0].pieces())
            piece.flags = 0;
        scene.begin();
        SupersampledUnitPlan plan;
        plan_unit_supersampled(
            scene.renderer,
            scene.bridge,
            scene.carrier(),
            whole_frame,
            false,
            UnitSupersampling::off,
            plan,
            mode
        );
        CHECK(plan.model.from_image);
        CHECK(scene.states[0].cache.has_image);
        if (mode == ModelImagePlan::metadata) {
            CHECK(scene.states[0].deferred_image.has_value());
            if (scene.states[0].deferred_image) {
                CHECK(scene.states[0].deferred_image->width == empty_image_side);
                CHECK(scene.states[0].deferred_image->height == empty_image_side);
            }
        } else {
            CHECK(scene.states[0].image.sprite.width == empty_image_side);
            CHECK(scene.states[0].image.sprite.height == empty_image_side);
        }
    }
}

} // namespace

int main() {
    planned_bands_draw_as_whole();
    tight_plans_draw_as_whole();
    off_band_draw_preserves_workspaces_and_flushes();
    metadata_plans_keep_cache_and_cargo();
    deferred_pixels_materialize_before_an_ordinary_draw();
    deferred_pixels_keep_the_cached_frame();
    metadata_rejects_an_oversized_image();
    metadata_keeps_an_all_hidden_image();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("planned draws: ok");
    return EXIT_SUCCESS;
}
