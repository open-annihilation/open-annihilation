// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/model/model_draw.hpp"
#include "oa/present/display.hpp"
#include "oa/present/surface.hpp"

#include <algorithm>
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
using oa::Sprite;
using oa::formats::objects3d::FixedVector3;

constexpr int32_t unit_fixed = 0x10000;
constexpr uint8_t ink = 0x55;
constexpr uint8_t ground = 200;

// A flat 16x16 square at height 0 with one coloured primitive, wound so
// that it faces the camera (the fill routines skip back faces).
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

constexpr uint8_t barrel_ink = 0x66;

// A square of a side in units with a 4x12 barrel two units up beside its
// centre, the barrel drawn every frame through the composite once the
// building is made a turret building (make_turret_building).
std::shared_ptr<oa::formats::objects3d::Model> wide_turret_model(int32_t side) {
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    oa::formats::objects3d::Object base;
    const int32_t h = side / 2 * unit_fixed;
    base.vertices = {{-h, 0, -h}, {h, 0, -h}, {h, 0, h}, {-h, 0, h}};
    oa::formats::objects3d::Primitive square;
    square.vertex_indices = {0, 3, 2, 1};
    square.color_index = ink;
    square.is_colored = 1;
    base.primitives.push_back(square);
    base.first_child = 1;
    oa::formats::objects3d::Object barrel;
    const int32_t w = 2 * unit_fixed;
    const int32_t y = 2 * unit_fixed;
    barrel.vertices = {
        {-w, y, 2 * unit_fixed},
        {w, y, 2 * unit_fixed},
        {w, y, 14 * unit_fixed},
        {-w, y, 14 * unit_fixed}
    };
    oa::formats::objects3d::Primitive bar;
    bar.vertex_indices = {0, 3, 2, 1};
    bar.color_index = barrel_ink;
    bar.is_colored = 1;
    barrel.primitives.push_back(bar);
    barrel.parent = 0;
    model->objects.push_back(base);
    model->objects.push_back(barrel);
    return model;
}

// A building with a turret: the square as its base, and as the base's child
// a 4x12 barrel two units up that reaches from beside the base's centre
// towards one edge, so that a turn about the vertical axis moves it on
// screen.
std::shared_ptr<oa::formats::objects3d::Model> turret_model() {
    auto model = square_model();
    oa::formats::objects3d::Object barrel;
    const int32_t w = 2 * unit_fixed;
    const int32_t y = 2 * unit_fixed;
    const int32_t near = 2 * unit_fixed;
    const int32_t far = 14 * unit_fixed;
    barrel.vertices = {{-w, y, near}, {w, y, near}, {w, y, far}, {-w, y, far}};
    oa::formats::objects3d::Primitive primitive;
    primitive.vertex_indices = {0, 3, 2, 1};
    primitive.color_index = barrel_ink;
    primitive.is_colored = 1;
    barrel.primitives.push_back(primitive);
    barrel.parent = 0;
    model->objects[0].first_child = 1;
    model->objects.push_back(barrel);
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

struct Scene {
    oa::World* world = oa::world_create();
    std::shared_ptr<const oa::formats::objects3d::Model> model = square_model();
    oa::sim::model_runtime::Instance instance = oa::sim::model_runtime::make_instance(model);
    ModelLibrary library;
    ModelState state;
    ModelRenderer renderer;
    ModelDisplay display;
    oa::present::SurfaceBuffer screen = oa::present::create_surface(200, 200);
    oa::Unit* unit{};

    Scene() : Scene(square_model()) {}

    explicit Scene(std::shared_ptr<const oa::formats::objects3d::Model> shape)
        : model(std::move(shape)) {
        const oa::WorldCapacity capacity{4, 2, 0};
        oa::world_alloc_tables(world, &capacity);
        unit = &world->units[1];
        unit->def = oa::oa_ref_from_index(1);
        unit->position = {100 * unit_fixed, 0, 100 * unit_fixed};
        renderer.world = world;
        renderer.origin_x = 0;
        renderer.origin_y = 0;
        init_composite_buffer(renderer);
        build_model_display(display, gray_palette());
        oa::present::bind_display(&display.context);
        for (auto& p : screen.pixels)
            p = ground;
    }

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

    uint8_t pixel(int x, int y) const {
        return screen.pixels[static_cast<std::size_t>(y * 200 + x)];
    }
};

void test_bounds() {
    Scene scene;
    const ImageFrame frame = measure_model_bounds(scene.instance, nullptr);
    CHECK(frame.width == 20);
    CHECK(frame.height == 20);
    CHECK(frame.origin_x == 10);
    CHECK(frame.origin_y == 10);
    ModelBounds bounds{};
    expand_model_bounds(bounds, scene.instance, 3 * unit_fixed, 0, 0);
    CHECK(bounds.left == -7);
    CHECK(bounds.right == 13);
    CHECK(bounds.top == -10);
    CHECK(bounds.bottom == 10);
    // A lifted model rises on screen by half its height.
    const FixedVector3 lift{0, 8 * unit_fixed, 0};
    const ImageFrame lifted = measure_model_bounds(scene.instance, &lift);
    CHECK(lifted.origin_y == 14);
    const ImageFrame shadow = measure_shadow_bounds(scene.instance);
    CHECK(shadow.width == 20);
    CHECK(shadow.origin_x == 10);
}

// Plain finished units get a depth-less image; z-buffered or unfinished
// units get a depth plane, with diggers lifted by their bias.
void test_image_planes() {
    Scene scene;
    CHECK(prepare_model_image(scene.renderer, scene.ref(), false, pass_cached_pieces));
    const Sprite& image = scene.state.image.sprite;
    CHECK(image.aux == nullptr);
    CHECK(image.key == image_key);
    CHECK(image.width == 20 && image.origin_x == 10);
    const auto* pixels = static_cast<const uint8_t*>(image.data);
    CHECK(pixels[10 * 20 + 10] == ink);
    CHECK(pixels[0] == image_key);
    CHECK(pixels[1 * 20 + 1] == image_key);
    scene.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    CHECK(prepare_model_image(scene.renderer, scene.ref(), false, pass_cached_pieces));
    CHECK(scene.state.image.sprite.aux != nullptr);
    CHECK(
        static_cast<const uint8_t*>(scene.state.image.sprite.aux)[10 * 20 + 10] == model_depth_base
    );
    scene.world->unit_defs[1].flags = OA_UNIT_DEF_FLAG_DIGGER;
    CHECK(prepare_model_image(scene.renderer, scene.ref(), false, pass_cached_pieces));
    CHECK(
        static_cast<const uint8_t*>(scene.state.image.sprite.aux)[10 * 20 + 10] ==
        model_depth_base + digger_depth_bias
    );
    // Only moving pieces: nothing is drawn for a fully cached model.
    CHECK(prepare_model_image(scene.renderer, scene.ref(), false, pass_moving_pieces));
    CHECK(static_cast<const uint8_t*>(scene.state.image.sprite.data)[10 * 20 + 10] == image_key);
}

void test_linked_draw() {
    Scene scene;
    draw_linked_model(scene.renderer, &scene.screen.surface, scene.ref(), true);
    CHECK(scene.state.cache.has_image);
    CHECK(scene.state.cache.draws == 1);
    CHECK(scene.pixel(100, 100) == ink);
    CHECK(scene.pixel(92, 92) == ink);
    CHECK(scene.pixel(89, 100) == ground);
    CHECK(scene.pixel(100, 109) == ground);
    // The second draw reuses the cached image.
    const void* cached = scene.state.image.sprite.data;
    draw_linked_model(scene.renderer, &scene.screen.surface, scene.ref(), true);
    CHECK(scene.state.image.sprite.data == cached);
    CHECK(scene.state.cache.draws == 2);
}

// An aircraft's silhouette falls on the ground below it, five pixels right.
void test_vehicle_shadow_at_altitude() {
    Scene scene;
    scene.renderer.graphics_flags = graphics_shadows | graphics_vehicle_shadows;
    scene.unit->position.y = 40 * unit_fixed;
    draw_linked_model(scene.renderer, &scene.screen.surface, scene.ref(), true);
    CHECK(scene.pixel(100, 80) == ink);
    const uint8_t shaded = scene.pixel(106, 104);
    CHECK(shaded < ground);
    CHECK(shaded > 0);
    CHECK(scene.pixel(100, 115) == ground);
    // No-shadow types and disabled vehicle shadows cast nothing.
    Scene plain;
    plain.renderer.graphics_flags = graphics_shadows;
    plain.unit->position.y = 40 * unit_fixed;
    draw_linked_model(plain.renderer, &plain.screen.surface, plain.ref(), true);
    CHECK(plain.pixel(106, 104) == ground);
}

// A building's silhouette is cached as a row-RLE sprite less its own footprint.
void test_building_shadow() {
    Scene scene;
    scene.renderer.graphics_flags = graphics_shadows;
    scene.unit->flags |= OA_UNIT_FLAG_BUILDING;
    scene.unit->type_index = 1;
    draw_linked_model(scene.renderer, &scene.screen.surface, scene.ref(), false);
    CHECK(scene.state.shadow.sprite.encoding == OA_SPRITE_ROW_RLE);
    CHECK(scene.state.shadow.sprite.width == 20);
    CHECK(scene.pixel(100, 100) == ink);
    CHECK(scene.pixel(111, 100) < ground);
    CHECK(scene.pixel(119, 100) == ground);
}

/// One case of ground_shadow against the draw: how the scene is set up.
struct GroundShadowCase {
    const char* name{};
    uint16_t graphics_flags{};
    uint32_t def_flags{}; ///< the unit type's flags
    bool turret{};        ///< the turret model, a barrel two pixels over a square
    bool building{};      ///< the unit is a building
    bool z_buffer{};      ///< its image has a depth plane
    int32_t height{};     ///< its height above the map's floor, pixels
    uint8_t sea_level{};  ///< the map's sea level
    bool casts{};         ///< the draw casts a shadow
};

// ground_shadow names exactly the pixels draw_linked_model darkens for its
// shadow, where the model's own image does not cover them: a vehicle, an
// aircraft, a building, a vehicle half under the water and a digger, whose
// square lies under the water or the ground and whose barrel casts alone,
// and the cases that cast none.
void test_ground_shadow_matches_the_draw() {
    constexpr uint16_t both = graphics_shadows | graphics_vehicle_shadows;
    constexpr uint32_t none = 0;
    const GroundShadowCase cases[] = {
        {"vehicle", both, none, false, false, false, 0, 0, true},
        {"aircraft", both, OA_UNIT_DEF_FLAG_CAN_FLY, false, false, false, 40, 0, true},
        {"building", graphics_shadows, none, true, true, false, 0, 0, true},
        {"building with a depth plane", graphics_shadows, none, true, true, true, 0, 0, true},
        {"vehicle with a depth plane", both, none, true, false, true, 0, 0, true},
        {"vehicle half under water", both, none, true, false, true, 4, 5, true},
        {"digger", both, OA_UNIT_DEF_FLAG_DIGGER, true, false, true, 0, 0, true},
        {"shadows off", graphics_vehicle_shadows, none, false, false, false, 0, 0, false},
        {"vehicle shadows off", graphics_shadows, none, false, false, false, 0, 0, false},
        {"a type without a shadow",
         both,
         OA_UNIT_DEF_FLAG_NO_SHADOW,
         false,
         false,
         false,
         0,
         0,
         false},
        {"hovercraft", both, OA_UNIT_DEF_FLAG_CAN_HOVER, false, false, false, 0, 0, false},
    };
    for (const GroundShadowCase& shown : cases) {
        // The frame without shadows, then with: the pixels that changed are
        // the shadow's.
        const auto draw = [&](uint16_t flags, Scene& scene) {
            scene.renderer.graphics_flags = flags;
            scene.world->game.sea_level = shown.sea_level;
            scene.world->unit_defs[1].flags = shown.def_flags;
            scene.unit->position.y = shown.height * unit_fixed;
            if (shown.building) {
                scene.unit->flags |= OA_UNIT_FLAG_BUILDING;
                scene.unit->type_index = 1;
            }
            if (shown.z_buffer)
                scene.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
            draw_linked_model(scene.renderer, &scene.screen.surface, scene.ref(), !shown.building);
        };
        Scene plain(shown.turret ? turret_model() : square_model());
        draw(0, plain);
        Scene shaded(shown.turret ? turret_model() : square_model());
        draw(shown.graphics_flags, shaded);
        GroundShadow shadow;
        shadow.coverage.assign(3, 1);
        const bool cast =
            ground_shadow(shown.graphics_flags, shaded.world->game, shaded.ref(), 0, 0, 0, shadow);
        std::size_t changed = 0;
        std::size_t named = 0;
        std::size_t mismatched = 0;
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x) {
                const bool darkened = shaded.pixel(x, y) != plain.pixel(x, y);
                const int column = x - shadow.left;
                const int row = y - shadow.top;
                const bool covered =
                    cast && column >= 0 && row >= 0 && column < shadow.width &&
                    row < shadow.height &&
                    shadow.coverage[static_cast<std::size_t>(row * shadow.width + column)] != 0;
                // Where the model's own image lies, the shadow does not show.
                const bool seen = covered && plain.pixel(x, y) == ground;
                changed += darkened ? 1U : 0U;
                named += seen ? 1U : 0U;
                mismatched += darkened != seen ? 1U : 0U;
            }
        std::printf(
            "ground shadow, %s: %zu pixels darkened, %zu named, %zu apart\n",
            shown.name,
            changed,
            named,
            mismatched
        );
        CHECK(cast == shown.casts);
        CHECK(mismatched == 0);
        CHECK((changed != 0) == shown.casts);
        if (!cast)
            CHECK(shadow.coverage.empty() && shadow.width == 0 && shadow.height == 0);
    }
}

void test_remap_depth_bands() {
    uint8_t pixels[6] = {9, 9, 9, 9, 9, image_key};
    uint8_t depth[6] = {1, 5, 6, 9, 10, 7};
    Sprite sprite{};
    sprite.width = 6;
    sprite.height = 1;
    sprite.key = image_key;
    sprite.data = pixels;
    sprite.aux = depth;
    remap_depth_bands(sprite, 10, remap_clear, 0x30, 0x40);
    CHECK(pixels[0] == 0x30);
    CHECK(pixels[1] == 0x30);
    CHECK(pixels[2] == 0x40);
    CHECK(pixels[3] == 0x40);
    CHECK(pixels[4] == image_key);
    CHECK(pixels[5] == image_key);
    uint8_t low[2] = {9, 9};
    uint8_t low_depth[2] = {0, 3};
    sprite.width = 2;
    sprite.data = low;
    sprite.aux = low_depth;
    remap_depth_bands(sprite, 2, remap_keep, remap_keep, 0x21);
    CHECK(low[0] == 0x21);
    CHECK(low[1] == 9);
}

void test_build_effect() {
    Scene scene;
    scene.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    CHECK(prepare_model_image(scene.renderer, scene.ref(), false, pass_all_pieces));
    Sprite& image = scene.state.image.sprite;
    CHECK(!apply_build_effect(scene.renderer, image, scene.ref()));
    scene.unit->build_remaining = 0.5F;
    CHECK(apply_build_effect(scene.renderer, image, scene.ref()));
    const auto* pixels = static_cast<const uint8_t*>(image.data);
    int nano = 0;
    for (int i = 0; i < 400; ++i)
        nano += pixels[i] >= 0xa0 && pixels[i] <= 0xaf ? 1 : 0;
    CHECK(nano > 0);
}

// How many times a colour of the pulse comes back to the bright end of the
// ramp over `ticks` ticks from `start`: the times it reaches 0xa0 from
// another colour.
int bright_returns(uint32_t start, uint32_t ticks, uint32_t id, bool second) {
    int returns = 0;
    uint8_t before = 0;
    for (uint32_t tick = start; tick <= start + ticks; ++tick) {
        const BuildPulseColours colours = build_pulse_colours(tick, id);
        const uint8_t colour = second ? colours.second : colours.first;
        if (tick != start && colour == 0xa0 && before != 0xa0)
            ++returns;
        before = colour;
    }
    return returns;
}

// The pulse's colours at a tick, as 3.1c sets them: each walks 0xa0..0xaf
// and back, the first 33 steps every 30 ticks, the second 57, each unit from
// its own place.
void test_build_pulse_colours() {
    // Tick 0: the waves start at the id mixed with 5 and 9.
    CHECK(build_pulse_colours(0, 0).first == 0xa5);
    CHECK(build_pulse_colours(0, 0).second == 0xa9);
    // Tick 30: 33 and 57 steps on, at steps 38 and 66, both on their way
    // down the ramp; step 16 turns back up from black.
    CHECK(build_pulse_colours(30, 0).first == 0xa6);
    CHECK(build_pulse_colours(30, 0).second == 0xa2);
    CHECK(build_pulse_colours(10, 0).first == 0xaf - 0x0);
    CHECK(build_pulse_colours(13, 0).first == 0xaf - 0x3);
    // Another id starts elsewhere.
    CHECK(build_pulse_colours(0, 3).first == 0xa6);
    // Every colour is on the ramp, and a colour moves at most two steps of
    // it from one tick to the next.
    for (uint32_t tick = 0; tick < 2000; ++tick) {
        const BuildPulseColours now = build_pulse_colours(tick, 41);
        const BuildPulseColours next = build_pulse_colours(tick + 1, 41);
        CHECK(now.first >= 0xa0 && now.first <= 0xaf);
        CHECK(now.second >= 0xa0 && now.second <= 0xaf);
        CHECK(std::abs(int{next.first} - int{now.first}) <= 2);
        CHECK(std::abs(int{next.second} - int{now.second}) <= 2);
    }
    // The rate: over 960 ticks, 32 seconds at 30 ticks a second, the first
    // colour comes back to bright green 33 times and the second 57, whatever
    // the unit and wherever the count starts.
    for (const uint32_t id : {0U, 7U, 300U})
        for (const uint32_t start : {0U, 17U, 54000U}) {
            CHECK(bright_returns(start, 960, id, false) == 33);
            CHECK(bright_returns(start, 960, id, true) == 57);
        }
}

// The colours a run of frames shows: one sample a frame, the game's tick
// moving on 30 times a second whatever the frame rate, with the build
// effect's clock moved on at every frame.
std::vector<uint8_t> pulse_by_tick(uint32_t frames_per_second, float zoom, uint32_t ticks) {
    BuildPulseClock clock;
    std::vector<uint8_t> by_tick(ticks + 1, 0);
    std::vector<bool> seen(ticks + 1, false);
    const uint32_t frames = ticks * frames_per_second / 30;
    for (uint32_t frame = 0; frame <= frames; ++frame) {
        const uint32_t tick = frame * 30 / frames_per_second;
        const uint32_t lag = advance_build_pulse(clock, tick, zoom);
        const uint8_t colour = build_pulse_colours(tick - lag, 11).second;
        // Every frame of a tick shows the same colour.
        CHECK(!seen[tick] || by_tick[tick] == colour);
        seen[tick] = true;
        by_tick[tick] = colour;
    }
    return by_tick;
}

// The pulse runs by the game's ticks, not the frames drawn: at 30, 60, 144
// and 240 frames a second the colours tick by tick are the same, at zoom 1
// and zoomed out.
void test_build_pulse_frame_rate() {
    for (const float zoom : {1.0F, 2.0F, 0.5F, 0.25F}) {
        const std::vector<uint8_t> at_30 = pulse_by_tick(30, zoom, 600);
        for (const uint32_t rate : {60U, 144U, 240U})
            CHECK(pulse_by_tick(rate, zoom, 600) == at_30);
    }
    // At zoom 1 they are 3.1c's.
    const std::vector<uint8_t> at_one = pulse_by_tick(144, 1.0F, 600);
    for (uint32_t tick = 0; tick <= 600; ++tick)
        CHECK(at_one[tick] == build_pulse_colours(tick, 11).second);
}

// The pulse's rate by zoom: 3.1c's at zoom 1 and in, the zoom's share of
// it zoomed out, and a quarter of it from four times out.
void test_build_pulse_rate() {
    CHECK(build_pulse_rate(1.0F) == 1.0F);
    CHECK(build_pulse_rate(4.0F) == 1.0F);
    CHECK(build_pulse_rate(0.5F) == 0.5F);
    CHECK(build_pulse_rate(0.25F) == 0.25F);
    CHECK(build_pulse_rate(1.0F / 6.0F) == build_pulse_least_rate);
    // Zoom 1 and in: no lag, tick for tick.
    BuildPulseClock clock;
    for (uint32_t tick = 100; tick < 400; ++tick)
        CHECK(advance_build_pulse(clock, tick, 1.0F) == 0);
    // Half way out, the pulse moves one tick for every two of the game's.
    uint32_t lag = 0;
    for (uint32_t tick = 400; tick <= 2400; tick += 2)
        lag = advance_build_pulse(clock, tick, 0.5F);
    CHECK(lag == 1000);
    // Ticks seen together count as ticks seen one by one.
    BuildPulseClock together;
    BuildPulseClock one_by_one;
    advance_build_pulse(together, 0, 0.3F);
    advance_build_pulse(one_by_one, 0, 0.3F);
    advance_build_pulse(together, 77, 0.3F);
    for (uint32_t tick = 1; tick <= 77; ++tick)
        advance_build_pulse(one_by_one, tick, 0.3F);
    CHECK(together.lag == one_by_one.lag && together.lag_fraction == one_by_one.lag_fraction);
    // Four times out and beyond, a quarter of the rate.
    BuildPulseClock far;
    advance_build_pulse(far, 0, 1.0F / 6.0F);
    CHECK(advance_build_pulse(far, 400, 1.0F / 6.0F) == 300);
    // Back at zoom 1 the lag goes at once: the pulse is 3.1c's again.
    CHECK(advance_build_pulse(far, 401, 1.0F) == 0);
    // A game loaded at an earlier tick starts the clock again.
    CHECK(advance_build_pulse(far, 801, 0.5F) == 200);
    CHECK(advance_build_pulse(far, 50, 0.5F) == 0);
    CHECK(advance_build_pulse(far, 52, 0.5F) == 1);
}

// The effect draws the pulse's colours at the renderer's tick less its lag.
void test_build_effect_lag() {
    const auto drawn_at = [](uint32_t tick, uint32_t lag) {
        Scene scene;
        scene.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
        CHECK(prepare_model_image(scene.renderer, scene.ref(), false, pass_all_pieces));
        scene.unit->build_remaining = 0.9F;
        Sprite& image = scene.state.image.sprite;
        scene.renderer.tick = tick;
        scene.renderer.build_pulse_lag = lag;
        CHECK(apply_build_effect(scene.renderer, image, scene.ref()));
        const auto* pixels = static_cast<const uint8_t*>(image.data);
        return std::vector<uint8_t>(
            pixels, pixels + static_cast<std::size_t>(image.width) * image.height
        );
    };
    const std::vector<uint8_t> lagging = drawn_at(500, 120);
    CHECK(lagging == drawn_at(380, 0));
    // The pulse moves the colours: the frame 9 ticks on differs.
    CHECK(lagging != drawn_at(389, 0));
}

void test_shift_threshold() {
    Scene scene;
    const ModelRef model = scene.ref();
    scene.state.transforms_dirty = false;
    scene.state.cache.draws = 5;
    set_model_shift(model, {0, 7, 0});
    CHECK(!scene.state.transforms_dirty);
    CHECK(scene.state.shift.xz == 0);
    set_model_shift(model, {0, 8, 0});
    CHECK(scene.state.transforms_dirty);
    CHECK(scene.state.shift.xz == 8);
    CHECK(scene.state.cache.draws == 0);
    scene.state.transforms_dirty = false;
    set_model_shift(model, {0, 8, -8});
    CHECK(scene.state.transforms_dirty);
}

void test_piece_changes() {
    Scene scene;
    const ModelRef model = scene.ref();
    note_piece_changes(model);
    scene.state.cache.draws = 3;
    scene.instance.pieces()[0].rotation.xz = 100;
    note_piece_changes(model);
    CHECK(scene.state.cache.draws == 0);
    CHECK(prepare_model_image(scene.renderer, model, false, pass_cached_pieces));
    scene.instance.pieces()[0].flags &= static_cast<uint16_t>(~0x2u);
    note_piece_changes(model);
    CHECK(scene.state.image.sprite.data == nullptr);
}

constexpr uint16_t piece_cached_flag =
    static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);

// Makes the scene's unit a shaded, shadowed turret building whose barrel
// (piece 1) is drawn per frame, as a turret's Create script leaves it.
void make_turret_building(Scene& scene) {
    scene.renderer.graphics_flags = graphics_shadows | graphics_shading;
    scene.unit->flags |= OA_UNIT_FLAG_BUILDING;
    scene.unit->type_index = 1;
    scene.instance.pieces()[1].flags &= static_cast<uint16_t>(~piece_cached_flag);
}

// Turns the barrel to `heading` and draws the scene on bare ground, as a
// frame does. The script's turn marks the instance's transforms dirty; the
// test marks the draw state's.
void draw_turret(Scene& scene, int16_t heading) {
    scene.instance.pieces()[1].rotation.xz = heading;
    scene.state.transforms_dirty = true;
    std::fill(scene.screen.pixels.begin(), scene.screen.pixels.end(), ground);
    oa::present::bind_display(&scene.display.context);
    const ModelRef model = scene.ref();
    note_piece_changes(model);
    draw_linked_model(scene.renderer, &scene.screen.surface, model, false);
}

// A turret building drawn while it was unfinished, then finished and its
// barrel turned over several draws, draws each time as a building drawn
// afresh in the same state: the image built while it was unfinished held the
// barrel, which must not stay behind where it pointed then.
void test_finished_turret_draws_as_fresh() {
    Scene built(turret_model());
    make_turret_building(built);
    built.unit->build_remaining = 0.5F;
    draw_turret(built, 0);
    CHECK(built.state.image_unfinished);
    CHECK(built.state.image.sprite.aux != nullptr);
    built.unit->build_remaining = 0.0F;
    built.unit->flags |= OA_UNIT_FLAG_CONSTRUCTION_DIRTY;
    for (const int16_t heading :
         {int16_t{0}, int16_t{0x2000}, int16_t{0x4000}, static_cast<int16_t>(0x8000)}) {
        draw_turret(built, heading);
        Scene fresh(turret_model());
        make_turret_building(fresh);
        draw_turret(fresh, heading);
        CHECK(built.screen.pixels == fresh.screen.pixels);
    }
    // Half a turn on, the barrel reaches beyond the base's far edge and
    // nothing is left beyond its near edge, where it pointed at first.
    CHECK(!built.state.image_unfinished);
    CHECK(built.state.image.sprite.aux == nullptr);
    CHECK(built.pixel(100, 88) == barrel_ink);
    CHECK(built.pixel(100, 111) == ground);
}

// ui.interface-fixes nanoframe-raster: an unfinished mobile unit's moving
// pieces are drawn plain over its nanoframe image every frame in 3.1c; under
// the fix only once it is built, as a building's are.
void test_mobile_nanoframe_moving_pieces() {
    const auto draw = [](bool fix, float remaining) {
        auto scene = std::make_unique<Scene>(turret_model());
        make_turret_building(*scene);
        scene->unit->flags &= ~OA_UNIT_FLAG_BUILDING;
        scene->unit->build_remaining = remaining;
        scene->renderer.moving_pieces_once_built = fix;
        draw_turret(*scene, 0);
        return scene;
    };
    const auto base = draw(false, 0.5F);
    const auto fixed = draw(true, 0.5F);
    CHECK(base->pixel(100, 108) == barrel_ink);
    CHECK(fixed->pixel(100, 108) != barrel_ink);
    // Built, both draw the barrel.
    CHECK(draw(true, 0.0F)->screen.pixels == draw(false, 0.0F)->screen.pixels);
}

/// Draws a 100-unit turret building with a composite of the given limits.
///
/// @param limits the composite's size and clamp
/// @return the scene, drawn once
std::unique_ptr<Scene> draw_wide_turret(const oa::data::limits::ModelComposite& limits) {
    auto scene = std::make_unique<Scene>(wide_turret_model(100));
    scene->renderer.composite_limits = limits;
    CHECK(init_composite_buffer(scene->renderer));
    make_turret_building(*scene);
    // Unshaded and without a shadow, so that the model alone draws, in ink,
    // and with a depth plane, which draws through the composite.
    scene->renderer.graphics_flags = 0;
    scene->unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    draw_turret(*scene, 0);
    return scene;
}

// The composite starts at its limits' size: 600x600 as in 3.1c, up to the
// largest a profile may name. Without the clamp a smaller composite grows to
// fit the model and draws it whole; with it the model keeps the composite's
// width and height from its top left.
void test_composite_limits() {
    const auto base = draw_wide_turret({});
    CHECK(
        base->renderer.composite.pixels.size() == std::size_t{2} * composite_side * composite_side
    );
    CHECK(base->pixel(60, 60) == ink && base->pixel(140, 140) == ink);

    const auto grown = draw_wide_turret({64, 64, false});
    CHECK(grown->screen.pixels == base->screen.pixels);

    const auto cut = draw_wide_turret({64, 64, true});
    CHECK(cut->renderer.composite.pixels.size() == std::size_t{2} * 64 * 64);
    CHECK(cut->pixel(60, 60) == ink);
    CHECK(cut->pixel(140, 140) == ground);

    constexpr int32_t largest = oa::data::limits::highest_composite_side;
    for (const bool clamp : {false, true}) {
        const auto wide = draw_wide_turret({largest, largest, clamp});
        CHECK(wide->renderer.composite.pixels.size() == std::size_t{2} * largest * largest);
        CHECK(wide->renderer.composite.sprite.data != nullptr);
        CHECK(wide->screen.pixels == base->screen.pixels);
    }

    // A renderer of its own takes the same limits.
    ModelRenderer copy;
    copy_renderer_settings(cut->renderer, copy);
    CHECK(
        copy.composite_limits.clamp_oversize &&
        copy.composite.pixels.size() == std::size_t{2} * 64 * 64
    );
}

// A unit under construction larger than a composite that keeps its size
// (clamp-oversize) draws its build effect where the cut leaves it: the
// outline writes nothing past the cut's width and height. The composite's
// planes are given room past the cut, which must keep what it held.
void test_cut_composite_build_outline() {
    constexpr int32_t side = 64;
    constexpr std::size_t plane = std::size_t{side} * side;
    constexpr std::size_t room = 4 * plane;
    constexpr uint8_t room_fill = 0xEE;
    auto scene = std::make_unique<Scene>(wide_turret_model(100));
    scene->renderer.composite_limits = {side, side, true};
    CHECK(init_composite_buffer(scene->renderer));
    auto& composite = scene->renderer.composite;
    composite.pixels.assign(2 * (plane + room), 0);
    std::fill_n(composite.pixels.begin() + plane, room, room_fill);
    composite.sprite.data = composite.pixels.data();
    composite.sprite.aux = composite.pixels.data() + plane + room;
    make_turret_building(*scene);
    scene->renderer.graphics_flags = 0;
    scene->unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    scene->unit->build_remaining = 0.5F;
    draw_turret(*scene, 0);
    CHECK(composite.sprite.width == side && composite.sprite.height == side);
    const auto colour_room = composite.pixels.begin() + plane;
    CHECK(std::all_of(colour_room, colour_room + room, [](uint8_t value) {
        return value == room_fill;
    }));
    const auto depth_room = composite.pixels.begin() + 2 * plane + room;
    CHECK(std::all_of(depth_room, depth_room + room, [](uint8_t value) { return value == 0; }));
    CHECK(scene->pixel(140, 140) == ground);
}

void test_sparse_depth() {
    uint8_t source_depth[16];
    for (int i = 0; i < 16; ++i)
        source_depth[i] = static_cast<uint8_t>(i);
    Sprite source{};
    source.width = 4;
    source.height = 4;
    source.aux = source_depth;
    uint8_t target_depth[4] = {};
    Sprite target{};
    target.width = 2;
    target.height = 2;
    target.aux = target_depth;
    copy_sparse_depth(source, target);
    CHECK(target_depth[0] == 0);
    CHECK(target_depth[1] == 2);
    CHECK(target_depth[2] == 8);
    CHECK(target_depth[3] == 10);
}

void test_projectile_and_debris() {
    Scene scene;
    const PreparedModel& prepared = prepare_model(scene.library, scene.model);
    const oa::formats::objects3d::Object& object = scene.model->objects[0];
    draw_projectile_model(
        scene.renderer,
        &scene.screen.surface,
        {50 * unit_fixed, 0, 60 * unit_fixed},
        object,
        prepared.objects[0],
        {}
    );
    CHECK(scene.pixel(50, 60) == ink);
    CHECK(scene.pixel(50, 70) == ground);
    std::vector<FixedVector3> points;
    const oa::Rect32 view{0, 0, 199, 199};
    draw_rotated_debris(
        scene.renderer,
        &scene.screen.surface,
        view,
        object,
        prepared.objects[0],
        {},
        {150 * unit_fixed, 0, 150 * unit_fixed},
        0,
        points
    );
    CHECK(scene.pixel(150, 150) == ink);
    const oa::Rect32 elsewhere{0, 0, 20, 20};
    draw_rotated_debris(
        scene.renderer,
        &scene.screen.surface,
        elsewhere,
        object,
        prepared.objects[0],
        {},
        {30 * unit_fixed, 0, 150 * unit_fixed},
        0,
        points
    );
    CHECK(scene.pixel(30, 150) == ground);
}

// A fragment slab facing the camera: a 16x16 quad one unit up (points 0..3,
// loaded axes) over its reversed copy one unit down.
oa::sim::effect_particles::ShatterFragment slab_fragment() {
    oa::sim::effect_particles::ShatterFragment fragment{};
    const int32_t h = 8 * unit_fixed;
    const oa::FixedVec3 top[]{
        {h, unit_fixed, h}, {h, unit_fixed, -h}, {-h, unit_fixed, -h}, {-h, unit_fixed, h}
    };
    for (uint32_t i = 0; i < 4; ++i) {
        fragment.points[i] = top[i];
        fragment.points[7 - i] = {top[i].x, -unit_fixed, top[i].z};
    }
    fragment.live = true;
    return fragment;
}

void test_shatter_fragment() {
    Scene scene;
    auto fragment = slab_fragment();
    fragment.look.flags = primitive_colored;
    fragment.look.color = ink;
    draw_shatter_fragment(
        scene.renderer,
        &scene.screen.surface,
        {50 * unit_fixed, 0, 60 * unit_fixed},
        fragment,
        PreparedPrimitive{},
        {}
    );
    CHECK(scene.pixel(50, 60) == ink);
    CHECK(scene.pixel(50, 75) == ground);
    // A team texture: ten solid frames; the fragment shows the owner's.
    constexpr int side = 16;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(10 * side * side + 4096), 0);
    TextureSequence texture;
    for (int frame = 0; frame < 10; ++frame) {
        Sprite sprite{};
        sprite.width = side;
        sprite.height = side;
        sprite.encoding = OA_SPRITE_RAW;
        sprite.data = pixels.data() + frame * side * side;
        std::fill_n(
            pixels.data() + frame * side * side, side * side, static_cast<uint8_t>(100 + frame)
        );
        texture.frames.push_back(sprite);
    }
    PreparedPrimitive team{};
    team.flags = primitive_animated | primitive_team;
    team.texture = &texture;
    fragment.look.flags = primitive_team;
    fragment.look.team_color = 3;
    draw_shatter_fragment(
        scene.renderer,
        &scene.screen.surface,
        {120 * unit_fixed, 0, 60 * unit_fixed},
        fragment,
        team,
        {}
    );
    CHECK(scene.pixel(120, 60) == 103);
    // Turned half a turn about the vertical axis the top still faces up.
    fragment.look.team_color = 5;
    draw_shatter_fragment(
        scene.renderer,
        &scene.screen.surface,
        {120 * unit_fixed, 0, 140 * unit_fixed},
        fragment,
        team,
        {0, static_cast<int16_t>(0x8000), 0}
    );
    CHECK(scene.pixel(120, 140) == 105);
}

} // namespace

int main() {
    test_mobile_nanoframe_moving_pieces();
    test_bounds();
    test_image_planes();
    test_linked_draw();
    test_vehicle_shadow_at_altitude();
    test_building_shadow();
    test_ground_shadow_matches_the_draw();
    test_remap_depth_bands();
    test_build_effect();
    test_build_pulse_colours();
    test_build_pulse_frame_rate();
    test_build_pulse_rate();
    test_build_effect_lag();
    test_shift_threshold();
    test_piece_changes();
    test_finished_turret_draws_as_fresh();
    test_composite_limits();
    test_cut_composite_build_outline();
    test_sparse_depth();
    test_projectile_and_debris();
    test_shatter_fragment();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("model draw tests passed");
    return EXIT_SUCCESS;
}
