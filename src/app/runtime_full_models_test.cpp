// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's model stage (runtime_full.hpp) on SDL's software renderer
// over a transparent render target, read back and compared pixel by pixel
// with the processor's raster of the same list, drawn through the model
// bridge as a band of the match frame draws it. Synthetic scenes: a flat
// square at rest and turned, a back-facing polygon left out, textured quads
// over fixed, animated and team textures with the running and the first
// frame, units with a depth plane whose polygons are ordered as the plane
// shows them, one a panel resting on a square drawn after it, a lit
// building's darkened and brightened rows and its flat colours through the
// shade table, a cloaked unit at half alpha over an opaque canvas, a
// building under construction with its bands and outline, units under the
// water line of the viewpoint's side and the other, a digger's clipped
// polygons, a polygon of the image key clearing what it covers of the
// picture, a carried unit composed into its carrier, vehicle and building
// shadows through the shadow target and straight into the frame, a
// projectile with its shadow sprite, a debris piece and a shatter fragment,
// the texture frames placed once and uploaded once and the pages emptied by
// a palette change, and a frame at zoom 2 against the raster enlarged. With
// --data, every unit model of the installed game at zoom 1, from a cached
// image without a depth plane and unlit, at rest and turned, and with one
// as a lit building, held to the bounds of runtime_full.hpp and the figures
// printed.
#include "runtime_full.hpp"

#include "oa/formats/objects3d.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/present/model/unit_supersampling.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/surface.hpp"
#include "oa/sim/effect_particles.hpp"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace card = oa::app::card;
namespace full = oa::app::full;
namespace draw = oa::present::model;
namespace fx = oa::sim::effect_particles;
using oa::app::DebrisDraw;
using oa::app::FragmentDraw;
using oa::app::ModelDraw;
using oa::app::ProjectileDraw;
using oa::app::WorldDrawKind;
using oa::app::WorldDrawList;
using oa::FixedVec3;
using oa::formats::objects3d::FixedVector3;
using oa::formats::objects3d::Model;
using oa::formats::objects3d::Object;
using oa::formats::objects3d::Primitive;
using oa::sim::model_runtime::Instance;
using oa::sim::model_runtime::PieceFlag;
using oa::sim::model_runtime::RotationWords;

constexpr int32_t unit = 0x10000;
constexpr uint32_t sampler_window = 0x10000;
constexpr uint8_t ink = 0x55;
constexpr uint8_t second_ink = 0x66;
constexpr uint8_t third_ink = 0x77;
constexpr uint8_t team_colour = 4;
constexpr uint16_t piece_cached = static_cast<uint16_t>(PieceFlag::cached);
/// The synthetic frame and where its unit stands.
constexpr int frame_width = 160;
constexpr int frame_height = 160;
constexpr int32_t unit_x = 80;
constexpr int32_t unit_z = 80;
/// Half the side of the square model, in world units.
constexpr int32_t half_side = 8;
/// The textured trapezoid's half widths at its narrow and wide ends and its
/// half depth, in world units: its sides slope half a pixel a row.
constexpr int32_t trapezoid_narrow_half = 5;
constexpr int32_t trapezoid_wide_half = 16;
constexpr int32_t trapezoid_half_depth = 11;
/// The step between the points the trapezoid's texture is read at, in
/// pixels of the target.
constexpr double trapezoid_sample_step = 0.25;
/// How far the card may draw the trapezoid's texels along a row from where
/// the walk puts them, in pixels of the target: a quarter of a pixel, and
/// the rounding of the card's arithmetic.
constexpr double trapezoid_shift_allowed = 0.26;
/// How far the card may draw them across a row from the walk's, in texels.
constexpr double trapezoid_across_allowed = 0.01;
/// A heading that slopes the square's edges, and a turn with bank and pitch.
constexpr RotationWords turned_heading{0, 0x0a00, 0};
constexpr RotationWords installed_turn{0x0300, 0x3000, 0x0200};
/// The size of the installed sweep's frame, and where its model stands.
constexpr int installed_width = 400;
constexpr int installed_height = 400;
constexpr int32_t installed_x = 200;
constexpr int32_t installed_z = 260;
/// Most pixels of a sloped edge may differ on a small model: a pixel a row;
/// and the pixels an edge steeper than a row a column may differ by more
/// than a pixel.
constexpr std::size_t edge_pixels_allowed = 40;
constexpr std::size_t steep_edge_pixels_allowed = 4;
/// The first SDL release whose software renderer samples a texture drawn
/// through a triangle at the texel each pixel's centre lands on. An older
/// one takes the texel beside it over parts of a triangle, so on it a
/// textured model is held to cover what the processor covers, not to its
/// texels.
constexpr int triangle_texels_version = SDL_VERSIONNUM(3, 4, 0);
/// Bounds on each installed model alone, looser than the sweep's.
constexpr double installed_model_far_coverage_share = 0.03;
constexpr std::size_t installed_model_far_coverage_pixels = 12;
constexpr double installed_model_far_share = 0.6;
/// Bounds on the whole sweep.
constexpr double installed_sweep_far_coverage_share = 0.005;
constexpr double installed_sweep_far_share = 0.25;
/// The lit mode's colours differ by the design: the shade table's snap
/// becomes a scaled colour; its coverage is held, its colours reported.
constexpr double installed_lit_far_share = 0.95;
constexpr std::size_t least_installed_models = 100;

/// A gray palette: entry i is (i, i, i).
oa::Palette gray_palette() {
    oa::Palette palette{};
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        palette.entries[i] = {
            static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0
        };
    return palette;
}

/// The palette as the band draw takes it: four bytes a colour.
oa::PaletteBytes palette_bytes(const oa::Palette& palette) {
    oa::PaletteBytes bytes{};
    for (std::size_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        bytes[i * 4] = palette.entries[i].r;
        bytes[i * 4 + 1] = palette.entries[i].g;
        bytes[i * 4 + 2] = palette.entries[i].b;
        bytes[i * 4 + 3] = 0;
    }
    return bytes;
}

/// The palette entry nearest a colour, as the model bridge maps a frame
/// colour: the first entry at the least squared distance.
uint8_t nearest_entry(const oa::Palette& palette, const std::array<uint8_t, 3>& colour) {
    uint8_t nearest = 0;
    int64_t least = INT64_MAX;
    for (std::size_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const auto& entry = palette.entries[i];
        const int64_t dr = int64_t{entry.r} - colour[0];
        const int64_t dg = int64_t{entry.g} - colour[1];
        const int64_t db = int64_t{entry.b} - colour[2];
        const int64_t distance = dr * dr + dg * dg + db * db;
        if (distance < least) {
            least = distance;
            nearest = static_cast<uint8_t>(i);
        }
    }
    return nearest;
}

/// A colour no entry of a palette has after the gamma, for the processor's
/// undrawn pixels, whose nearest entry a shadow darkens to another entry,
/// so that a shadow over it shows: a shadow over a colour that darkens to
/// itself changes no index, and the bridge writes nothing back.
std::array<uint8_t, 3>
key_colour(const oa::Palette& palette, float gamma, const draw::ModelDisplay& display) {
    std::set<uint32_t> used;
    for (const auto& entry : palette.entries)
        used.insert(
            (uint32_t{oa::present::gamma_channel(entry.r, gamma)} << 16) |
            (uint32_t{oa::present::gamma_channel(entry.g, gamma)} << 8) |
            oa::present::gamma_channel(entry.b, gamma)
        );
    for (uint32_t candidate = 0xc1c2c3U;; candidate += 0x30507U) {
        if (used.count(candidate & 0xffffffU) != 0)
            continue;
        const std::array<uint8_t, 3> colour{
            static_cast<uint8_t>(candidate >> 16),
            static_cast<uint8_t>(candidate >> 8),
            static_cast<uint8_t>(candidate)
        };
        const uint8_t index = nearest_entry(palette, colour);
        if (display.alpha.size() > index && display.alpha[index] != index)
            return colour;
    }
}

// ---------------------------------------------------------------------------
// Models

/// A primitive of corners in order, coloured or textured.
Primitive primitive(std::vector<uint16_t> corners, uint8_t colour, const char* texture) {
    Primitive result;
    result.vertex_indices = std::move(corners);
    if (texture != nullptr) {
        result.texture_name = texture;
        result.is_colored = 0;
    } else {
        result.color_index = colour;
        result.is_colored = 1;
    }
    return result;
}

/// A square of half_side on each side at a height, wound to face the
/// camera, or the other way.
Object square_object(int32_t height, uint8_t colour, const char* texture, bool facing = true) {
    Object object;
    const int32_t h = half_side * unit;
    const int32_t y = height * unit;
    object.vertices = {{-h, y, -h}, {h, y, -h}, {h, y, h}, {-h, y, h}};
    object.primitives.push_back(
        facing ? primitive({0, 3, 2, 1}, colour, texture) : primitive({0, 1, 2, 3}, colour, texture)
    );
    return object;
}

/// One square at height 0.
std::shared_ptr<Model> square_model(uint8_t colour = ink, bool facing = true) {
    auto model = std::make_shared<Model>();
    model->objects.push_back(square_object(0, colour, nullptr, facing));
    return model;
}

/// Two squares in one piece, the higher one listed first and smaller, so
/// that a depth plane shows it over the lower.
std::shared_ptr<Model> stacked_model() {
    auto model = std::make_shared<Model>();
    Object object = square_object(0, ink, nullptr);
    const int32_t h = 4 * unit;
    const int32_t y = 12 * unit;
    object.vertices.insert(object.vertices.end(), {{-h, y, -h}, {h, y, -h}, {h, y, h}, {-h, y, h}});
    object.primitives.insert(
        object.primitives.begin(), primitive({4, 7, 6, 5}, second_ink, nullptr)
    );
    model->objects.push_back(object);
    return model;
}

/// A square four units up, and a child piece over it: a panel rising from
/// the square's height inside its edge to eight units higher well past it,
/// so gently that where the two overlap the panel's depth is still the
/// square's. The panel's piece is drawn first and the square's last.
std::shared_ptr<Model> resting_model() {
    auto model = std::make_shared<Model>();
    model->objects.push_back(square_object(4, ink, nullptr));
    Object panel;
    const int32_t w = 4 * unit;
    const int32_t foot = 4 * unit;
    const int32_t tip = 44 * unit;
    const int32_t low = 4 * unit;
    const int32_t high = 12 * unit;
    panel.vertices = {{-w, low, foot}, {w, low, foot}, {w, high, tip}, {-w, high, tip}};
    panel.primitives.push_back(primitive({0, 3, 2, 1}, second_ink, nullptr));
    panel.parent = 0;
    model->objects[0].first_child = 1;
    model->objects.push_back(panel);
    return model;
}

/// A square eight units up and a smaller one at the ground, which the
/// digger clip takes away with everything at or below the ground.
std::shared_ptr<Model> digger_model() {
    auto model = std::make_shared<Model>();
    Object object = square_object(8, second_ink, nullptr);
    const int32_t h = 4 * unit;
    object.vertices.insert(object.vertices.end(), {{-h, 0, -h}, {h, 0, -h}, {h, 0, h}, {-h, 0, h}});
    object.primitives.push_back(primitive({4, 7, 6, 5}, ink, nullptr));
    model->objects.push_back(object);
    return model;
}

/// A square at the ground, a polygon of the image key four units up over
/// its northern half and a smaller square eight units up over its middle,
/// in that order: the key's polygon clears the square at the ground under
/// it, and the higher square shows over the key's.
std::shared_ptr<Model> cleared_model() {
    auto model = std::make_shared<Model>();
    Object object = square_object(0, ink, nullptr);
    const int32_t h = half_side * unit;
    const int32_t key_height = 4 * unit;
    const int32_t d = 4 * unit;
    const int32_t deck_height = 8 * unit;
    object.vertices.insert(
        object.vertices.end(),
        {{-h, key_height, -h},
         {h, key_height, -h},
         {h, key_height, 0},
         {-h, key_height, 0},
         {-d, deck_height, -d},
         {d, deck_height, -d},
         {d, deck_height, d},
         {-d, deck_height, d}}
    );
    object.primitives.push_back(primitive({4, 7, 6, 5}, draw::image_key, nullptr));
    object.primitives.push_back(primitive({8, 11, 10, 9}, second_ink, nullptr));
    model->objects.push_back(object);
    return model;
}

/// A base square with a child barrel two units up, which a heading turns.
std::shared_ptr<Model> turret_model() {
    auto model = square_model();
    Object barrel;
    const int32_t w = 2 * unit;
    const int32_t y = 2 * unit;
    const int32_t near = 2 * unit;
    const int32_t far = 14 * unit;
    barrel.vertices = {{-w, y, near}, {w, y, near}, {w, y, far}, {-w, y, far}};
    barrel.primitives.push_back(primitive({0, 3, 2, 1}, second_ink, nullptr));
    barrel.parent = 0;
    model->objects[0].first_child = 1;
    model->objects.push_back(barrel);
    return model;
}

/// Textures of the library: a fixed one, an animated one and a team one,
/// each texel a function of its place and frame.
struct TextureFixture {
    std::vector<std::unique_ptr<oa::formats::gaf::Sequence>> sequences;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> texels;
    draw::TextureLibrary library;
    /// Texels kept at or under 127, so that the bright page's doubling
    /// clamps none of them.
    bool dark{};

    void add(const std::string& name, int frames, uint16_t width, uint16_t height, bool team) {
        auto sequence = std::make_unique<oa::formats::gaf::Sequence>();
        sequence->name = name;
        draw::TextureSequence entry;
        for (int i = 0; i < frames; ++i) {
            oa::formats::gaf::Frame frame;
            frame.width = width;
            frame.height = height;
            frame.duration = 1;
            sequence->frames.push_back(frame);
            auto pixels = std::make_unique<std::vector<uint8_t>>(
                sampler_window + static_cast<std::size_t>(width) * height, 0
            );
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    (*pixels)[static_cast<std::size_t>(y * width + x)] =
                        dark ? static_cast<uint8_t>((8 * x + 2 * y + 20 * i + 8) & 0x7f)
                             : static_cast<uint8_t>((16 * x + 4 * y + 40 * i + 32) & 0xff);
            oa::Sprite sprite{};
            sprite.width = width;
            sprite.height = height;
            sprite.data = pixels->data();
            entry.frames.push_back(sprite);
            texels.push_back(std::move(pixels));
        }
        entry.sequence = sequence.get();
        entry.team_archive = team;
        library.sequences.emplace(name, std::move(entry));
        sequences.push_back(std::move(sequence));
    }

    explicit TextureFixture(bool dark_texels = false) : dark(dark_texels) {
        add("plain", 1, 16, 16, false);
        add("blink", 3, 16, 16, false);
        add("logo", 10, 8, 8, true);
    }
};

/// Three textured squares side by side, over the fixed, the animated and
/// the team texture.
std::shared_ptr<Model> textured_model() {
    auto model = std::make_shared<Model>();
    Object object;
    const int32_t h = half_side * unit;
    const char* names[3] = {"plain", "blink", "logo"};
    for (int i = 0; i < 3; ++i) {
        const int32_t left = (i * 20 - 30) * unit;
        const auto first = static_cast<uint16_t>(object.vertices.size());
        object.vertices.insert(
            object.vertices.end(),
            {{left, 0, -h}, {left + 2 * h, 0, -h}, {left + 2 * h, 0, h}, {left, 0, h}}
        );
        object.primitives.push_back(primitive(
            {first,
             static_cast<uint16_t>(first + 3),
             static_cast<uint16_t>(first + 2),
             static_cast<uint16_t>(first + 1)},
            0,
            names[i]
        ));
    }
    model->objects.push_back(object);
    return model;
}

/// A textured quad facing the camera whose ends lie along rows, one end
/// narrower than the other, over the fixed texture.
std::shared_ptr<Model> trapezoid_model() {
    auto model = std::make_shared<Model>();
    Object object;
    const int32_t narrow = trapezoid_narrow_half * unit;
    const int32_t wide = trapezoid_wide_half * unit;
    const int32_t depth = trapezoid_half_depth * unit;
    object.vertices = {
        {-narrow, 0, -depth}, {narrow, 0, -depth}, {wide, 0, depth}, {-wide, 0, depth}
    };
    object.primitives.push_back(primitive({0, 3, 2, 1}, 0, "plain"));
    model->objects.push_back(object);
    return model;
}

/// The textured squares with a steep ramp beside them, rising 51 units
/// toward -x of the loaded orientation, so that its face turns away from the
/// light and its shade row is a dark one, where the squares' is a bright
/// one.
std::shared_ptr<Model> lit_model() {
    auto model = textured_model();
    Object& object = model->objects[0];
    const int32_t h = half_side * unit;
    const int32_t top = 51 * unit;
    const int32_t left = 36 * unit;
    const auto first = static_cast<uint16_t>(object.vertices.size());
    object.vertices.insert(
        object.vertices.end(),
        {{left, 0, h}, {left + 2 * h, top, h}, {left + 2 * h, top, -h}, {left, 0, -h}}
    );
    object.primitives.push_back(primitive(
        {first,
         static_cast<uint16_t>(first + 1),
         static_cast<uint16_t>(first + 2),
         static_cast<uint16_t>(first + 3)},
        0,
        "plain"
    ));
    return model;
}

// ---------------------------------------------------------------------------
// The canvas: SDL's software renderer over a surface, with a transparent
// render target the card's frame is drawn into.

/// A picture read back from the card's target: RGBA, alpha 0 where nothing
/// was drawn.
struct CardPicture {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgba;

    [[nodiscard]] const uint8_t* at(uint32_t x, uint32_t y) const {
        return rgba.data() + (std::size_t{y} * width + x) * 4U;
    }
};

/// The processor's picture: RGB24, the key colour where nothing was drawn.
struct ProcessorPicture {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgb;

    [[nodiscard]] const uint8_t* at(uint32_t x, uint32_t y) const {
        return rgb.data() + (std::size_t{y} * width + x) * 3U;
    }
};

struct Canvas {
    SDL_Surface* surface{};
    SDL_Renderer* renderer{};
    SDL_Texture* target{};
    int width{};
    int height{};

    Canvas(int canvas_width, int canvas_height) : width(canvas_width), height(canvas_height) {
        surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_XRGB8888);
        renderer = surface != nullptr ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        OA_CHECK(renderer != nullptr);
        target = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height
        );
        OA_CHECK(target != nullptr);
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    ~Canvas() {
        SDL_DestroyTexture(target);
        SDL_DestroyRenderer(renderer);
        SDL_DestroySurface(surface);
    }

    /// Clears the target to a colour, transparent by default.
    void clear(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0, uint8_t a = 0) const {
        OA_CHECK(SDL_SetRenderTarget(renderer, target));
        OA_CHECK(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE));
        OA_CHECK(SDL_SetRenderDrawColor(renderer, r, g, b, a));
        OA_CHECK(SDL_RenderClear(renderer));
        OA_CHECK(SDL_SetRenderTarget(renderer, nullptr));
    }

    /// Reads the target back.
    [[nodiscard]] CardPicture read() const {
        CardPicture picture;
        OA_CHECK(SDL_SetRenderTarget(renderer, target));
        SDL_Surface* read_back = SDL_RenderReadPixels(renderer, nullptr);
        OA_CHECK(SDL_SetRenderTarget(renderer, nullptr));
        OA_CHECK(read_back != nullptr);
        if (read_back == nullptr)
            return picture;
        SDL_Surface* rgba = SDL_ConvertSurface(read_back, SDL_PIXELFORMAT_RGBA32);
        OA_CHECK(rgba != nullptr);
        if (rgba != nullptr) {
            picture.width = static_cast<uint32_t>(rgba->w);
            picture.height = static_cast<uint32_t>(rgba->h);
            picture.rgba.resize(std::size_t{picture.width} * picture.height * 4U);
            for (uint32_t y = 0; y < picture.height; ++y)
                std::memcpy(
                    picture.rgba.data() + std::size_t{y} * picture.width * 4U,
                    static_cast<const uint8_t*>(rgba->pixels) +
                        std::size_t{y} * static_cast<std::size_t>(rgba->pitch),
                    std::size_t{picture.width} * 4U
                );
            SDL_DestroySurface(rgba);
        }
        SDL_DestroySurface(read_back);
        return picture;
    }
};

// ---------------------------------------------------------------------------
// The scene: a World, a model library and display, the planner's list, the
// processor's bridge and the card's stage.

/// One unit of the scene, with its instance and draw state.
struct SceneUnit {
    std::shared_ptr<const Model> model;
    Instance instance;
    draw::ModelState state;
    oa::Unit* unit{};
    uint16_t slot{};
};

struct Scene {
    oa::World* world = oa::world_create();
    oa::Palette palette;
    float gamma{1.0F};
    oa::PaletteBytes bytes{};
    std::array<uint8_t, 3> key{};
    draw::ModelLibrary library;
    draw::ModelDisplay display;
    draw::ModelRenderer renderer;
    draw::RgbBridge bridge;
    std::vector<draw::BridgeBand> bands;
    draw::SupersampleScratch supersample;
    std::vector<FixedVector3> debris_points;
    WorldDrawList list;
    std::deque<SceneUnit> units;
    int width{frame_width};
    int height{frame_height};
    int32_t ground{};
    std::vector<uint8_t> shadow_pixels;
    oa::Sprite shadow_sprite{};
    oa::formats::gaf::RenderedFrame shadow_frame;
    std::array<uint8_t, full::team_colour_players> team_colors{};
    uint32_t tick{};
    uint32_t build_pulse_lag{};

    explicit Scene(
        oa::Palette scene_palette = gray_palette(),
        draw::TextureLibrary textures = {},
        float scene_gamma = 1.0F
    )
        : palette(scene_palette), gamma(scene_gamma) {
        const oa::WorldCapacity capacity{16, 4, 0};
        oa::world_alloc_tables(world, &capacity);
        library.textures = std::move(textures);
        bytes = palette_bytes(palette);
        draw::build_model_display(display, palette);
        key = key_colour(palette, gamma, display);
        oa::present::bind_display(&display.context);
        renderer.world = world;
        renderer.origin_x = 0;
        renderer.origin_y = 0;
        renderer.user = this;
        renderer.ground_height = [](void* user, const FixedVec3&) {
            return static_cast<Scene*>(user)->ground;
        };
        renderer.model_of = [](void* user, const oa::Unit& record) {
            return static_cast<Scene*>(user)->model_of(record);
        };
        draw::init_composite_buffer(renderer);
        team_colors[0] = team_colour;
        renderer.team_colors[0] = team_colour;
        // The projectiles' shadow sprite: a 4x4 square of colour 0 about
        // its centre.
        shadow_pixels.assign(16, 0);
        shadow_sprite.width = 4;
        shadow_sprite.height = 4;
        shadow_sprite.origin_x = 2;
        shadow_sprite.origin_y = 2;
        shadow_sprite.key = 0xff;
        shadow_sprite.data = shadow_pixels.data();
        shadow_frame.width = 4;
        shadow_frame.height = 4;
        shadow_frame.origin_x = 2;
        shadow_frame.origin_y = 2;
        shadow_frame.transparency_index = 0xff;
        shadow_frame.pixels.assign(16, 0);
        shadow_frame.coverage.assign(16, 1);
    }

    ~Scene() {
        oa::present::bind_display(nullptr);
        oa::world_destroy(world);
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Adds a unit of a model in a slot, standing at a map point.
    SceneUnit& add_unit(std::shared_ptr<const Model> model, uint16_t slot, int32_t x, int32_t z) {
        units.push_back({model, oa::sim::model_runtime::make_instance(model), {}, nullptr, slot});
        SceneUnit& added = units.back();
        added.unit = &world->units[slot];
        *added.unit = {};
        added.unit->def = oa::oa_ref_from_index(1);
        added.unit->type_index = 1;
        added.unit->id = slot;
        added.unit->position = {x * unit, 0, z * unit};
        added.instance.rebuild_transforms();
        return added;
    }

    /// The model of a unit record, for the carried units.
    draw::ModelRef model_of(const oa::Unit& record) {
        for (SceneUnit& candidate : units)
            if (candidate.unit == &record)
                return ref(candidate);
        return {};
    }

    draw::ModelRef ref(SceneUnit& scene_unit) {
        return {
            &scene_unit.instance,
            &draw::prepare_model(library, scene_unit.model),
            &scene_unit.state,
            scene_unit.unit,
            oa::world_unit_def_of(world, scene_unit.unit)
        };
    }

    /// Starts a frame: the list emptied and the bridge laid over the
    /// processor's picture, as render_match_surface does before planning.
    void begin_frame(ProcessorPicture& picture, uint16_t graphics_flags) {
        oa::app::clear_world_draws(list);
        picture.width = static_cast<uint32_t>(width);
        picture.height = static_cast<uint32_t>(height);
        picture.rgb.resize(std::size_t{picture.width} * picture.height * 3U);
        for (std::size_t i = 0; i < picture.rgb.size(); i += 3) {
            picture.rgb[i] = key[0];
            picture.rgb[i + 1] = key[1];
            picture.rgb[i + 2] = key[2];
        }
        world->game.graphics_flags = graphics_flags;
        renderer.graphics_flags = graphics_flags;
        renderer.tick = tick;
        renderer.build_pulse_lag = build_pulse_lag;
        renderer.camera_x = 0;
        renderer.camera_y = 0;
        draw::bridge_begin(
            bridge,
            {picture.rgb.data(), width, height, width * 3},
            {0, 0, width - 1, height - 1},
            1.0F,
            display.palette
        );
    }

    /// Plans a unit for the frame, as the planner does, and adds its draw.
    void plan_unit(SceneUnit& scene_unit, bool idle = false) {
        ModelDraw drawn;
        drawn.model = ref(scene_unit);
        draw::plan_unit_supersampled(
            renderer,
            bridge,
            drawn.model,
            {0, 0, width - 1, height - 1},
            idle,
            draw::UnitSupersampling::off,
            drawn.plan
        );
        list.models.push_back(std::move(drawn));
        oa::app::add_world_draw(list, WorldDrawKind::model, list.models.size() - 1);
    }

    /// Draws the list as one band of the frame, as the match draws it, and
    /// applies the gamma, so that the picture compares with the card's.
    void raster_processor(ProcessorPicture& picture) {
        oa::app::add_world_draw(list, WorldDrawKind::commit_always, 0);
        draw::bridge_split(bridge, 1, bands);
        oa::app::WorldFrameDraw frame_draw{};
        frame_draw.target = {picture.rgb.data(), width, height, 0, 0, width, height, 0, height};
        frame_draw.palette = &bytes;
        frame_draw.scale = 1.0F;
        frame_draw.bridge = &bridge;
        frame_draw.display = &display;
        frame_draw.projectile_shadow = &shadow_sprite;
        frame_draw.debris_view = {0, 0, width - 1, height - 1};
        oa::app::draw_world_band(
            list, frame_draw, bands.front(), renderer, supersample, debris_points
        );
        draw::bridge_join_band(bridge, bands.front());
        if (gamma != 1.0F)
            for (std::size_t i = 0; i + 2 < picture.rgb.size(); i += 3) {
                uint8_t* pixel = picture.rgb.data() + i;
                if (pixel[0] == key[0] && pixel[1] == key[1] && pixel[2] == key[2])
                    continue;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    pixel[channel] = oa::present::gamma_channel(pixel[channel], gamma);
            }
    }

    /// Draws a unit's cached image two samples to a pixel, as the processor
    /// draws a unit finer, from its vertices snapped to the whole pixels the
    /// card places them on, and reads it at the odd samples: the processor's
    /// raster of the unit's polygons sampled at the pixel centres, where the
    /// card samples.
    void raster_processor_centred(SceneUnit& scene_unit, ProcessorPicture& picture) {
        picture.width = static_cast<uint32_t>(width);
        picture.height = static_cast<uint32_t>(height);
        picture.rgb.assign(std::size_t{picture.width} * picture.height * 3U, 0);
        for (std::size_t i = 0; i < picture.rgb.size(); i += 3) {
            picture.rgb[i] = key[0];
            picture.rgb[i + 1] = key[1];
            picture.rgb[i + 2] = key[2];
        }
        draw::ModelRenderer finer;
        draw::copy_renderer_settings(renderer, finer);
        finer.samples = 2;
        // The card floors x, floors the height to its even whole and takes
        // the whole of -z, so that the finer draw's half pixels land on the
        // card's whole ones.
        Instance snapped = scene_unit.instance;
        for (auto& piece : snapped.pieces())
            for (FixedVector3& v : piece.transformed_vertices) {
                v.x = static_cast<int32_t>(static_cast<uint32_t>(v.x >> 16) << 16);
                v.y = static_cast<int32_t>(static_cast<uint32_t>(v.y >> 17) << 17);
                const int32_t negated = static_cast<int32_t>(0U - static_cast<uint32_t>(v.z));
                const int32_t floored =
                    static_cast<int32_t>(static_cast<uint32_t>(negated >> 16) << 16);
                v.z = static_cast<int32_t>(0U - static_cast<uint32_t>(floored));
            }
        draw::ModelState state;
        const draw::ModelRef ref{
            &snapped,
            &draw::prepare_model(library, scene_unit.model),
            &state,
            scene_unit.unit,
            oa::world_unit_def_of(world, scene_unit.unit)
        };
        OA_CHECK(draw::prepare_model_image(finer, ref, false, draw::pass_cached_pieces));
        const oa::Sprite& image = state.image.sprite;
        if (image.data == nullptr)
            return;
        const oa::Unit& record = *scene_unit.unit;
        const int32_t image_x = record.position.x >> 16;
        const int32_t unit_y = (record.position.z >> 16) - ((record.position.y >> 16) >> 1);
        const auto* indices = static_cast<const uint8_t*>(image.data);
        for (int32_t y = 0; y < height; ++y)
            for (int32_t x = 0; x < width; ++x) {
                const int32_t sample_x = 2 * (x - image_x) + image.origin_x + 1;
                const int32_t sample_y = 2 * (y - unit_y) + image.origin_y + 1;
                if (sample_x < 0 || sample_y < 0 || sample_x >= image.width ||
                    sample_y >= image.height)
                    continue;
                const uint8_t index =
                    indices[static_cast<std::size_t>(sample_y) * image.width + sample_x];
                if (index == draw::image_key)
                    continue;
                uint8_t* pixel =
                    picture.rgb.data() + (std::size_t{static_cast<uint32_t>(y)} * picture.width +
                                          static_cast<uint32_t>(x)) *
                                             3U;
                const oa::PaletteEntry& entry = palette.entries[index];
                pixel[0] = oa::present::gamma_channel(entry.r, gamma);
                pixel[1] = oa::present::gamma_channel(entry.g, gamma);
                pixel[2] = oa::present::gamma_channel(entry.b, gamma);
            }
    }

    /// What the stage reads of the frame.
    [[nodiscard]] full::ModelFrameInputs inputs(bool with_shadow_sprite = true) const {
        full::ModelFrameInputs in;
        in.draws = &list;
        in.world = world;
        in.library = &library;
        in.display = &display;
        in.graphics_flags = world->game.graphics_flags;
        in.build_pulse_tick = tick - build_pulse_lag;
        in.moving_pieces_once_built = renderer.moving_pieces_once_built;
        in.team_colors = team_colors;
        in.projectile_shadow = with_shadow_sprite ? &shadow_frame : nullptr;
        return in;
    }
};

/// The card's side of a scene: an executor on a canvas and the stage.
struct Card {
    Canvas canvas;
    card::Executor executor;
    full::ModelStage stage;
    card::CardFrame frame;

    Card(const Scene& scene, float zoom = 1.0F, uint32_t texture_limit = 0)
        : canvas(
              static_cast<int>(std::ceil(static_cast<float>(scene.width) * zoom)),
              static_cast<int>(std::ceil(static_cast<float>(scene.height) * zoom))
          ) {
        OA_CHECK(executor.open(canvas.renderer, texture_limit));
        stage.set_palette(scene.palette, scene.gamma);
    }

    ~Card() { stage.close(executor); }

    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;

    /// Builds the frame of models from the scene's list, runs it and reads
    /// the target back.
    [[nodiscard]] CardPicture
    raster(const Scene& scene, float zoom = 1.0F, bool with_shadow_sprite = true) {
        full::SceneView view;
        view.zoom = zoom;
        view.camera_x = 0;
        view.camera_y = 0;
        view.width = canvas.width;
        view.height = canvas.height;
        canvas.clear();
        frame.reset();
        stage.emit_frame(scene.inputs(with_shadow_sprite), view, executor, frame);
        OA_CHECK(stage.upload(executor));
        OA_CHECK(card::check_frame(frame).empty());
        const bool executed = executor.execute(frame, canvas.target);
        OA_CHECK(executed);
        if (!executed)
            std::fprintf(stderr, "executor: %s\n", executor.error().c_str());
        return canvas.read();
    }
};

/// Compares the card's picture with the processor's.
full::ModelRasterComparison
compare(const CardPicture& by_card, const ProcessorPicture& by_processor, const Scene& scene) {
    OA_CHECK(by_card.width == by_processor.width);
    OA_CHECK(by_card.height == by_processor.height);
    return full::compare_model_rasters(
        by_card.rgba.data(), by_processor.rgb.data(), by_card.width, by_card.height, scene.key
    );
}

void print(const char* what, const full::ModelRasterComparison& c) {
    std::printf(
        "%s: drawn %zu, same %zu, phase %zu, edge colour %zu, far %zu, edge coverage %zu, far "
        "coverage %zu, shadowed %zu\n",
        what,
        c.drawn,
        c.same,
        c.phase,
        c.edge_colour,
        c.far,
        c.edge_coverage,
        c.far_coverage,
        c.shadowed
    );
}

/// Holds a flat-coloured scene exactly: every pixel both draw the same, and
/// no pixel drawn by one alone beyond a sloped edge's.
void check_exact(const char* what, const full::ModelRasterComparison& c, std::size_t edges = 0) {
    print(what, c);
    OA_CHECK(c.drawn > 0);
    OA_CHECK(c.far == 0);
    OA_CHECK(c.phase == 0);
    OA_CHECK(c.far_coverage == 0);
    OA_CHECK(c.edge_coverage <= edges);
}

/// Tells whether the SDL library the test runs on, which can be older than
/// the headers it was built with, samples a textured triangle's texels at
/// pixel centres.
///
/// @return true from triangle_texels_version on
bool card_samples_triangle_texels() {
    return SDL_GetVersion() >= triangle_texels_version;
}

/// Holds a textured scene: every pixel both draw the same or a texel of
/// phase apart, since the card interpolates the texels at pixel centres
/// where the processor's walk steps from the row's start; no pixel drawn
/// by one alone beyond a sloped edge's.
void check_textured(const char* what, const full::ModelRasterComparison& c, std::size_t edges = 0) {
    print(what, c);
    OA_CHECK(c.drawn > 0);
    OA_CHECK(c.far == 0 || !card_samples_triangle_texels());
    OA_CHECK(c.far_coverage == 0);
    OA_CHECK(c.edge_coverage <= edges);
}

/// Counts the pixels of a picture with a given alpha.
std::size_t pixels_with_alpha(const CardPicture& picture, uint8_t alpha) {
    std::size_t count = 0;
    for (std::size_t i = 3; i < picture.rgba.size(); i += 4)
        if (picture.rgba[i] == alpha)
            ++count;
    return count;
}

/// Tells whether the processor drew a pixel.
bool drawn_by_processor(const uint8_t* pixel, const std::array<uint8_t, 3>& key) {
    return pixel[0] != key[0] || pixel[1] != key[1] || pixel[2] != key[2];
}

/// Counts the processor's drawn pixels.
std::size_t pixels_drawn(const ProcessorPicture& picture, const std::array<uint8_t, 3>& key) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < picture.rgb.size(); i += 3)
        if (drawn_by_processor(picture.rgb.data() + i, key))
            ++count;
    return count;
}

/// Prints the first pixels the two pictures draw far apart, or one alone
/// with nothing of the other near, for a failed check.
void print_far(
    const CardPicture& by_card, const ProcessorPicture& by_processor, const Scene& scene
) {
    constexpr std::size_t most_printed = 12;
    std::size_t printed = 0;
    for (uint32_t y = 0; y < by_card.height && printed < most_printed; ++y)
        for (uint32_t x = 0; x < by_card.width && printed < most_printed; ++x) {
            const uint8_t* card_pixel = by_card.at(x, y);
            const uint8_t* processor_pixel = by_processor.at(x, y);
            const bool card_drawn = card_pixel[3] != 0;
            const bool processor_drawn = drawn_by_processor(processor_pixel, scene.key);
            if (card_drawn != processor_drawn ||
                (card_drawn && card_pixel[3] == 255 &&
                 std::abs(int{card_pixel[0]} - int{processor_pixel[0]}) >
                     full::model_raster_tolerance)) {
                std::printf(
                    "  (%u, %u): card %u %u %u a%u, processor %u %u %u\n",
                    x,
                    y,
                    card_pixel[0],
                    card_pixel[1],
                    card_pixel[2],
                    card_pixel[3],
                    processor_pixel[0],
                    processor_pixel[1],
                    processor_pixel[2]
                );
                ++printed;
            }
        }
}

// ---------------------------------------------------------------------------
// Cases

// A flat square from its cached image, at rest and turned: every pixel the
// same colour as the processor's, every batch one draw of the frame's
// scissor, the counts of the stage.
void test_square() {
    Scene scene;
    SceneUnit& square = scene.add_unit(square_model(), 1, unit_x, unit_z);
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(square);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_exact("square at rest", compare(drawn, processor, scene));
    // The square covers 16 by 16 pixels about the unit; the colour is the
    // ink's palette entry.
    const uint8_t* inside = drawn.at(unit_x, unit_z);
    OA_CHECK(inside[0] == ink && inside[1] == ink && inside[2] == ink && inside[3] == 255);
    OA_CHECK(drawn.at(unit_x - half_side - 1, unit_z)[3] == 0);
    OA_CHECK(drawn.at(unit_x + half_side, unit_z)[3] == 0);
    OA_CHECK(pixels_with_alpha(drawn, 255) == 16 * 16);
    OA_CHECK(card.stage.counts().units == 1);
    OA_CHECK(card.stage.counts().polygons == 1);
    OA_CHECK(card.stage.counts().culled == 0);
    OA_CHECK(card.stage.counts().meshes_built == 1);
    OA_CHECK(card.stage.mesh_count() == 1);
    OA_CHECK(card.stage.mesh_bytes() > 0);
    for (const card::Batch& batch : card.frame.batches)
        if (batch.operation == card::Operation::draw) {
            OA_CHECK(batch.scissored);
            OA_CHECK(batch.target == card::TargetHandle{});
            OA_CHECK(batch.sampling == card::Sampling::nearest);
        }
    // Turned: the edges slope, and the pixels they cover may differ by one.
    square.unit->heading = static_cast<uint16_t>(turned_heading.xz);
    scene.begin_frame(processor, 0);
    scene.plan_unit(square);
    scene.raster_processor(processor);
    const CardPicture turned = card.raster(scene);
    check_exact("square turned", compare(turned, processor, scene), edge_pixels_allowed);
}

// A square wound away from the camera draws nothing on either side.
void test_back_face() {
    Scene scene;
    SceneUnit& square = scene.add_unit(square_model(ink, false), 1, unit_x, unit_z);
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(square);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    OA_CHECK(pixels_drawn(processor, scene.key) == 0);
    OA_CHECK(pixels_with_alpha(drawn, 0) == drawn.rgba.size() / 4);
    OA_CHECK(card.stage.counts().culled == 1);
    OA_CHECK(card.stage.counts().polygons == 0);
}

// Textured quads over a fixed, an animated and a team texture: the texels
// the processor fetches, the running frame on a moving piece and the first
// on an idle one, three frames placed on one page and uploaded once.
void test_textured() {
    TextureFixture textures;
    Scene scene(gray_palette(), std::move(textures.library));
    SceneUnit& quads = scene.add_unit(textured_model(), 1, unit_x, unit_z);
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(quads);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_textured("textured at rest", compare(drawn, processor, scene));
    OA_CHECK(card.stage.counts().frames_placed == 3);
    OA_CHECK(card.stage.pages().memory().pages == 1);
    OA_CHECK(card.stage.pages().memory().frames == 3);
    const uint64_t uploads = card.stage.counts().page_uploads;
    OA_CHECK(uploads >= 1);
    // The same frame again places and uploads nothing more.
    scene.begin_frame(processor, 0);
    scene.plan_unit(quads);
    scene.raster_processor(processor);
    std::ignore = card.raster(scene);
    OA_CHECK(card.stage.counts().frames_placed == 3);
    OA_CHECK(card.stage.counts().page_uploads == uploads);
    // The piece made a moving one shows the running frame of the animated
    // texture, stepped twice, and the first frame while the unit is idle.
    quads.instance.pieces()[0].flags &= static_cast<uint16_t>(~piece_cached);
    draw::step_texture_animations(scene.library);
    draw::step_texture_animations(scene.library);
    scene.begin_frame(processor, 0);
    scene.plan_unit(quads);
    scene.raster_processor(processor);
    const CardPicture running = card.raster(scene);
    check_textured("textured running frame", compare(running, processor, scene));
    // The flat variants of the three frames, and the running frame.
    OA_CHECK(card.stage.counts().frames_placed == 6);
    scene.begin_frame(processor, 0);
    scene.plan_unit(quads, true);
    scene.raster_processor(processor);
    const CardPicture idle = card.raster(scene);
    check_textured("textured idle", compare(idle, processor, scene));
    OA_CHECK(card.stage.counts().frames_placed == 7);
    // A texel equals its palette entry: the fixed texture's first texel,
    // which its first corner shows; the loaded orientation negates x and z,
    // so that corner lands at the top right of the rightmost square.
    const uint8_t* corner = drawn.at(unit_x + 29, unit_z - half_side);
    OA_CHECK(corner[3] == 255 && corner[0] == 32);
}

/// Finds the texture coordinates the card draws at a point of the target:
/// those of the last textured triangle drawn over it, in proportion to the
/// point's place in it.
///
/// @param frame the frame drawn
/// @param x the point, in pixels of the target
/// @param y
/// @param[out] u the texture coordinates there, 0 to 1 across the page
/// @param[out] v
/// @return false when no textured triangle covers the point
bool card_texture_at(const card::CardFrame& frame, double x, double y, double& u, double& v) {
    bool found = false;
    for (const card::Batch& batch : frame.batches) {
        if (batch.operation != card::Operation::draw || batch.page == card::PageHandle{})
            continue;
        for (uint32_t k = 0; k + 2 < batch.index_count; k += 3) {
            const card::Vertex& a = frame.vertices[frame.indices[batch.first_index + k]];
            const card::Vertex& b = frame.vertices[frame.indices[batch.first_index + k + 1]];
            const card::Vertex& c = frame.vertices[frame.indices[batch.first_index + k + 2]];
            const auto twice_area =
                [](double ax, double ay, double bx, double by, double cx, double cy) {
                    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                };
            const double area = twice_area(a.x, a.y, b.x, b.y, c.x, c.y);
            if (area == 0.0)
                continue;
            const double at_a = twice_area(x, y, b.x, b.y, c.x, c.y) / area;
            const double at_b = twice_area(a.x, a.y, x, y, c.x, c.y) / area;
            const double at_c = 1.0 - at_a - at_b;
            if (at_a < 0.0 || at_b < 0.0 || at_c < 0.0)
                continue;
            u = at_a * a.u + at_b * b.u + at_c * c.u;
            v = at_a * a.v + at_b * b.v + at_c * c.v;
            found = true;
        }
    }
    return found;
}

// A textured trapezoid at zoom 1 and zoom 4: wherever the card draws it,
// its texels lie where the processor's walk puts them, in proportion down
// its sloped sides and then along each row, to within a quarter of a pixel
// along the row; two triangles would bend them along their shared diagonal,
// 5.5 pixels at zoom 1. At zoom 1 it draws no pixel far from the processor's.
void test_textured_trapezoid() {
    TextureFixture textures;
    Scene scene(gray_palette(), std::move(textures.library));
    SceneUnit& trapezoid = scene.add_unit(trapezoid_model(), 1, unit_x, unit_z);
    for (const float zoom : {1.0F, 4.0F}) {
        Card card(scene, zoom);
        ProcessorPicture processor;
        scene.begin_frame(processor, 0);
        scene.plan_unit(trapezoid);
        scene.raster_processor(processor);
        const CardPicture drawn = card.raster(scene, zoom);
        OA_CHECK(card.stage.counts().polygons == 1);
        if (zoom == 1.0F) {
            const auto c = compare(drawn, processor, scene);
            print("textured trapezoid", c);
            OA_CHECK(c.far_coverage == 0);
        }
        // The trapezoid's corners: its textured vertices at its top and
        // bottom rows, the leftmost and the rightmost of each.
        std::vector<card::Vertex> textured;
        for (const card::Batch& batch : card.frame.batches)
            if (batch.operation == card::Operation::draw && batch.page != card::PageHandle{})
                for (uint32_t k = 0; k < batch.index_count; ++k)
                    textured.push_back(
                        card.frame.vertices[card.frame.indices[batch.first_index + k]]
                    );
        OA_CHECK(!textured.empty());
        if (textured.empty())
            continue;
        const auto [lowest, highest] = std::ranges::minmax(textured, {}, &card::Vertex::y);
        card::Vertex top_left = lowest;
        card::Vertex top_right = lowest;
        card::Vertex bottom_left = highest;
        card::Vertex bottom_right = highest;
        for (const card::Vertex& vertex : textured) {
            if (vertex.y == lowest.y && vertex.x < top_left.x)
                top_left = vertex;
            if (vertex.y == lowest.y && vertex.x > top_right.x)
                top_right = vertex;
            if (vertex.y == highest.y && vertex.x < bottom_left.x)
                bottom_left = vertex;
            if (vertex.y == highest.y && vertex.x > bottom_right.x)
                bottom_right = vertex;
        }
        OA_CHECK(top_right.x - top_left.x < bottom_right.x - bottom_left.x);
        const double page = card.stage.pages().pages()[0].size;
        double most_along = 0.0;
        double most_across = 0.0;
        std::size_t read = 0;
        for (double y = lowest.y + trapezoid_sample_step; y < highest.y;
             y += trapezoid_sample_step) {
            const double down = (y - lowest.y) / (static_cast<double>(highest.y) - lowest.y);
            const auto at_row = [down](float from, float to) { return from + down * (to - from); };
            const double left = at_row(top_left.x, bottom_left.x);
            const double right = at_row(top_right.x, bottom_right.x);
            const double left_u = at_row(top_left.u, bottom_left.u);
            const double left_v = at_row(top_left.v, bottom_left.v);
            const double row_u = at_row(top_right.u, bottom_right.u) - left_u;
            const double row_v = at_row(top_right.v, bottom_right.v) - left_v;
            const double row_squared = row_u * row_u + row_v * row_v;
            for (double x = left + trapezoid_sample_step; x < right; x += trapezoid_sample_step) {
                double u = 0.0;
                double v = 0.0;
                OA_CHECK(card_texture_at(card.frame, x, y, u, v));
                // The card's coordinates less the walk's, along the row's
                // texture coordinates and across them.
                const double share = (x - left) / (right - left);
                const double off_u = u - (left_u + share * row_u);
                const double off_v = v - (left_v + share * row_v);
                const double along = (off_u * row_u + off_v * row_v) / row_squared;
                most_along = std::max(most_along, std::fabs(along) * (right - left));
                most_across = std::max(
                    most_across, std::hypot(off_u - along * row_u, off_v - along * row_v) * page
                );
                ++read;
            }
        }
        std::printf(
            "textured trapezoid at zoom %.0f: %zu points, %.3f pixels along a row at most, %.3f "
            "texels across\n",
            static_cast<double>(zoom),
            read,
            most_along,
            most_across
        );
        OA_CHECK(read > 0);
        OA_CHECK(most_along <= trapezoid_shift_allowed);
        OA_CHECK(most_across <= trapezoid_across_allowed);
    }
}

// A unit with a depth plane: the higher square listed first draws over the
// lower, as the plane keeps it; the sort puts it last.
void test_depth_sorted() {
    Scene scene;
    SceneUnit& stacked = scene.add_unit(stacked_model(), 1, unit_x, unit_z);
    stacked.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(stacked);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_exact("depth sorted", compare(drawn, processor, scene));
    // The higher square lifts by half its height and shows over the lower.
    const uint8_t* top = drawn.at(unit_x, unit_z - 6);
    OA_CHECK(top[0] == second_ink && top[3] == 255);
    const uint8_t* ground = drawn.at(unit_x, unit_z + 6);
    OA_CHECK(ground[0] == ink && ground[3] == 255);
    OA_CHECK(card.stage.counts().polygons == 2);
}

// A unit with a depth plane whose panel rests on its square: the plane keeps
// the square, drawn later, where the two have the same depth, so the square
// shows whole under the panel's foot, and the panel past the square's edge.
void test_resting_on_depth_plane() {
    Scene scene;
    SceneUnit& resting = scene.add_unit(resting_model(), 1, unit_x, unit_z);
    resting.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(resting);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_exact("resting on the depth plane", compare(drawn, processor, scene));
    std::size_t square = 0;
    std::size_t panel = 0;
    for (std::size_t i = 0; i < drawn.rgba.size(); i += 4)
        if (drawn.rgba[i + 3] == 255) {
            square += drawn.rgba[i] == ink ? 1 : 0;
            panel += drawn.rgba[i] == second_ink ? 1 : 0;
        }
    OA_CHECK(square == static_cast<std::size_t>(4 * half_side * half_side));
    OA_CHECK(panel > 0);
    OA_CHECK(card.stage.counts().polygons == 2);
}

// A lit building: the squares facing the camera take the bright row their
// normals give under the light and draw from the bright page, the steep
// ramp a dark row, and a flat square's colour goes through the shade table.
void test_lit_building() {
    TextureFixture textures(true);
    Scene scene(gray_palette(), std::move(textures.library));
    SceneUnit& building = scene.add_unit(lit_model(), 1, unit_x, unit_z);
    building.unit->flags |= OA_UNIT_FLAG_BUILDING;
    building.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, draw::graphics_shading);
    scene.plan_unit(building);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    const auto lit = compare(drawn, processor, scene);
    print("lit building", lit);
    OA_CHECK(lit.drawn > 0);
    // The ramp's steep edges step more than a row a column, where the
    // card's centre sampling lands a pixel further than the processor's.
    OA_CHECK(lit.far_coverage <= steep_edge_pixels_allowed);
    // The rows scale the texels: the table snaps to the nearest gray, the
    // card multiplies, within the tolerance; the squares' row is a bright
    // one, so their frames went to the bright page.
    OA_CHECK(lit.far == 0 || !card_samples_triangle_texels());
    if (lit.far != 0)
        print_far(drawn, processor, scene);
    OA_CHECK(card.stage.bright_pages().memory().frames == 3);
    OA_CHECK(card.stage.pages().memory().frames == 1);
    std::printf(
        "lit building: %u frames on the pages, %u on the bright pages\n",
        card.stage.pages().memory().frames,
        card.stage.bright_pages().memory().frames
    );
    // The ramp's dark row leaves its texels under a tenth of their value.
    uint8_t darkest = 255;
    uint8_t brightest = 0;
    for (std::size_t i = 0; i < drawn.rgba.size(); i += 4)
        if (drawn.rgba[i + 3] == 255) {
            darkest = std::min(darkest, drawn.rgba[i]);
            brightest = std::max(brightest, drawn.rgba[i]);
        }
    OA_CHECK(darkest <= 12);
    OA_CHECK(brightest >= 128);
    // A flat square of a lit building takes the table's colour for its row:
    // the square's normal points down, which gives row 27.
    const int32_t row = oa::present::gpu_world::shade_row(
        0.0F, -1.0F, 0.0F, scene.renderer.light, scene.renderer.light_scale
    );
    OA_CHECK(row == 27);
    SceneUnit& flat = scene.add_unit(square_model(), 2, unit_x, unit_z);
    flat.unit->flags |= OA_UNIT_FLAG_BUILDING;
    flat.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    scene.begin_frame(processor, draw::graphics_shading);
    scene.plan_unit(flat);
    scene.raster_processor(processor);
    const CardPicture shaded = card.raster(scene);
    check_exact("lit flat square", compare(shaded, processor, scene));
    const uint8_t* pixel = shaded.at(unit_x, unit_z);
    const uint8_t expected =
        scene.display.shade[static_cast<std::size_t>(row) * OA_PALETTE_COLORS + ink];
    OA_CHECK(pixel[0] == expected && expected != ink);
    // Shading off, the same building draws its colours as they are; its
    // image is built again, as the game builds every image again when the
    // option changes.
    flat.state = {};
    scene.begin_frame(processor, 0);
    scene.plan_unit(flat);
    scene.raster_processor(processor);
    const CardPicture unlit = card.raster(scene);
    check_exact("unlit flat square", compare(unlit, processor, scene));
    OA_CHECK(unlit.at(unit_x, unit_z)[0] == ink);
}

// A cloaked unit at half alpha: over an opaque canvas each pixel is the
// mean of the ink and the canvas, as the alpha table's entry is in a gray
// palette.
void test_cloaked() {
    Scene scene;
    SceneUnit& square = scene.add_unit(square_model(), 1, unit_x, unit_z);
    square.unit->state_flags |= draw::unit_state_cloaked;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(square);
    scene.raster_processor(processor);
    // Over a transparent canvas the unit reads back at half alpha.
    const CardPicture translucent = card.raster(scene);
    OA_CHECK(pixels_with_alpha(translucent, 255) == 0);
    const uint8_t* half = translucent.at(unit_x, unit_z);
    OA_CHECK(half[3] >= 126 && half[3] <= 129);
    const auto c = compare(translucent, processor, scene);
    OA_CHECK(c.shadowed == 16 * 16 && c.far_coverage == 0 && c.edge_coverage == 0);
    // Over an opaque gray canvas: the mean, within a level of the table's.
    constexpr uint8_t canvas_gray = 0xc8;
    full::SceneView view;
    view.width = card.canvas.width;
    view.height = card.canvas.height;
    card.canvas.clear(canvas_gray, canvas_gray, canvas_gray, 255);
    card.frame.reset();
    card.stage.emit_frame(scene.inputs(), view, card.executor, card.frame);
    OA_CHECK(card.stage.upload(card.executor));
    OA_CHECK(card.executor.execute(card.frame, card.canvas.target));
    const CardPicture blended = card.canvas.read();
    const uint8_t* pixel = blended.at(unit_x, unit_z);
    const int mean = (int{ink} + int{canvas_gray}) / 2;
    OA_CHECK(std::abs(int{pixel[0]} - mean) <= 1);
    // The processor's pixel is the table's entry for the two colours, not
    // the ink.
    OA_CHECK(processor.at(unit_x, unit_z)[0] != ink);
}

// A building under construction: the bands colour its polygons by their
// depth, and the outline is the processor's, the first and the last pixel
// of each row a primitive spans. Half built, both pictures take the band's
// colour inside; near the start of building the bands clear the square and
// leave its outline's two sides alone.
void test_nanoframe() {
    for (const float remaining : {0.5F, 0.9F, 0.99F}) {
        Scene scene;
        SceneUnit& site = scene.add_unit(square_model(), 1, unit_x, unit_z);
        site.unit->flags |= OA_UNIT_FLAG_BUILDING;
        site.unit->build_remaining = remaining;
        scene.tick = 7;
        Card card(scene);
        ProcessorPicture processor;
        scene.begin_frame(processor, 0);
        scene.plan_unit(site);
        scene.raster_processor(processor);
        const CardPicture drawn = card.raster(scene);
        const auto c = compare(drawn, processor, scene);
        std::array<char, 48> name{};
        std::snprintf(name.data(), name.size(), "nanoframe, %.2f left to build", remaining);
        check_exact(name.data(), c);
        OA_CHECK(c.same == c.drawn);
        const uint8_t* inside = drawn.at(unit_x, unit_z);
        if (remaining == 0.5F) {
            // Inside, the band's colour, from the nano ramp; the processor's too.
            OA_CHECK(inside[3] == 255 && inside[0] >= 0xa0 && inside[0] <= 0xaf);
            OA_CHECK(processor.at(unit_x, unit_z)[0] == inside[0]);
        } else {
            // The square cleared, and the outline down its two sides alone,
            // one pixel a row: no row across its top or bottom.
            OA_CHECK(inside[3] == 0);
            OA_CHECK(c.drawn == 2 * 2 * half_side);
            OA_CHECK(drawn.at(unit_x, unit_z - half_side)[3] == 0);
        }
        // The outline is drawn as quads of its own, past the square's one
        // polygon where the bands leave it.
        OA_CHECK(card.stage.counts().polygons == (remaining == 0.5F ? 1U : 0U));
        OA_CHECK(card.frame.indices.size() > 6);
    }
}

// A nanoframe barely begun in a view zoomed out to a sixth: its outline's
// runs span a sixth of a screen pixel, and are drawn a screen pixel across
// and down, so that both of its sides still show on screen, with the
// square cleared between them.
void test_nanoframe_zoomed_out() {
    constexpr float zoom = 1.0F / 6.0F;
    Scene scene;
    SceneUnit& site = scene.add_unit(square_model(), 1, unit_x, unit_z);
    site.unit->flags |= OA_UNIT_FLAG_BUILDING;
    site.unit->build_remaining = 0.99F;
    scene.tick = 7;
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(site);
    Card card(scene, zoom);
    const CardPicture drawn = card.raster(scene, zoom);
    const auto middle_x = static_cast<uint32_t>(static_cast<float>(unit_x) * zoom);
    const auto middle_y = static_cast<uint32_t>(static_cast<float>(unit_z) * zoom);
    uint32_t left = 0;
    uint32_t right = 0;
    for (uint32_t y = 0; y < drawn.height; ++y)
        for (uint32_t x = 0; x < drawn.width; ++x)
            if (drawn.at(x, y)[3] != 0)
                ++(x < middle_x ? left : right);
    std::printf(
        "nanoframe barely begun at zoom 1/6: %u pixels left of its middle, %u right\n", left, right
    );
    OA_CHECK(left > 0 && right > 0);
    OA_CHECK(drawn.at(middle_x, middle_y)[3] == 0);
}

// The nanoframe's pulse in a view zoomed out, its clock lagging behind the
// match's tick: both pictures take the colours of the pulse's tick, the
// tick less the lag, the same as a frame at that tick with no lag.
void test_nanoframe_pulse_lag() {
    const auto inside_colour = [](uint32_t tick, uint32_t lag) {
        Scene scene;
        SceneUnit& site = scene.add_unit(square_model(), 1, unit_x, unit_z);
        site.unit->flags |= OA_UNIT_FLAG_BUILDING;
        site.unit->build_remaining = 0.5F;
        scene.tick = tick;
        scene.build_pulse_lag = lag;
        Card card(scene);
        ProcessorPicture processor;
        scene.begin_frame(processor, 0);
        scene.plan_unit(site);
        scene.raster_processor(processor);
        const CardPicture drawn = card.raster(scene);
        const uint8_t* inside = drawn.at(unit_x, unit_z);
        OA_CHECK(inside[3] == 255 && processor.at(unit_x, unit_z)[0] == inside[0]);
        return inside[0];
    };
    OA_CHECK(inside_colour(907, 900) == inside_colour(7, 0));
    OA_CHECK(inside_colour(7, 0) != inside_colour(13, 0));
}

// A mobile unit under construction whose moving pieces wait for it to be
// built, as a building's do: the bands colour it as they colour a
// building, in both pictures; without the rule it is drawn whole.
void test_mobile_nanoframe_once_built() {
    for (const bool once_built : {false, true}) {
        Scene scene;
        SceneUnit& mobile = scene.add_unit(square_model(), 1, unit_x, unit_z);
        mobile.unit->build_remaining = 0.5F;
        scene.tick = 7;
        scene.renderer.moving_pieces_once_built = once_built;
        Card card(scene);
        ProcessorPicture processor;
        scene.begin_frame(processor, 0);
        scene.plan_unit(mobile);
        scene.raster_processor(processor);
        const CardPicture drawn = card.raster(scene);
        const auto c = compare(drawn, processor, scene);
        print(once_built ? "mobile nanoframe once built" : "mobile unit unfinished", c);
        OA_CHECK(c.drawn > 0 && c.far_coverage == 0 && c.far == 0);
        const uint8_t* inside = drawn.at(unit_x, unit_z);
        OA_CHECK(inside[3] == 255 && processor.at(unit_x, unit_z)[0] == inside[0]);
        const bool banded = inside[0] >= 0xa0 && inside[0] <= 0xaf;
        OA_CHECK(banded == once_built);
    }
}

// Units under the water line: the viewpoint's own tinted, halved with the
// blue lift; another player's not drawn at all.
void test_underwater() {
    Scene scene;
    scene.world->game.sea_level = 10;
    scene.world->game.viewpoint_player = 0;
    SceneUnit& own = scene.add_unit(square_model(), 1, unit_x, unit_z);
    own.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    own.unit->owner_index = 0;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(own);
    scene.raster_processor(processor);
    const CardPicture tinted = card.raster(scene);
    // The gray palette's nearest entry to the halved and lifted colour is
    // a gray, so the colours differ by design; the coverage holds.
    const auto c = compare(tinted, processor, scene);
    print("underwater, own", c);
    OA_CHECK(c.drawn == 16 * 16 && c.far_coverage == 0 && c.edge_coverage == 0);
    const uint8_t* pixel = tinted.at(unit_x, unit_z);
    OA_CHECK(pixel[3] == 255);
    OA_CHECK(std::abs(int{pixel[0]} - ink / 2) <= 2);
    OA_CHECK(std::abs(int{pixel[1]} - ink / 2) <= 2);
    OA_CHECK(std::abs(int{pixel[2]} - (ink / 2 + 0x32)) <= 2);
    // The processor's pixel is tinted too: not the ink.
    OA_CHECK(processor.at(unit_x, unit_z)[0] != ink);
    // Another player's unit under the line is not drawn by either.
    SceneUnit& other = scene.add_unit(square_model(), 2, unit_x, unit_z);
    other.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    other.unit->owner_index = 1;
    scene.begin_frame(processor, 0);
    scene.plan_unit(other);
    scene.raster_processor(processor);
    const CardPicture hidden = card.raster(scene);
    OA_CHECK(pixels_drawn(processor, scene.key) == 0);
    OA_CHECK(pixels_with_alpha(hidden, 0) == hidden.rgba.size() / 4);
    // Above the line both draw the unit whole.
    other.unit->position.y = 20 * unit;
    scene.begin_frame(processor, 0);
    scene.plan_unit(other);
    scene.raster_processor(processor);
    check_exact("above the water", compare(card.raster(scene), processor, scene));
}

// A digger's polygons below the ground are clipped on both sides.
void test_digger() {
    Scene scene;
    scene.world->unit_defs[1].flags = OA_UNIT_DEF_FLAG_DIGGER;
    SceneUnit& digger = scene.add_unit(digger_model(), 1, unit_x, unit_z);
    digger.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(digger);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_exact("digger", compare(drawn, processor, scene));
    // Only the square above the ground shows, lifted four rows: 16 by 16
    // pixels of its ink.
    OA_CHECK(pixels_with_alpha(drawn, 255) == 16 * 16);
    OA_CHECK(drawn.at(unit_x, unit_z - 4)[0] == second_ink);
}

// A polygon of the image key clears what it covers of the unit's picture
// under it, so the ground shows there, from an image with a depth plane and
// without one; nothing of the key's colour is drawn.
void test_clearing() {
    for (const bool depth_plane : {true, false}) {
        Scene scene;
        SceneUnit& cleared = scene.add_unit(cleared_model(), 1, unit_x, unit_z);
        if (depth_plane)
            cleared.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
        Card card(scene);
        ProcessorPicture processor;
        scene.begin_frame(processor, 0);
        scene.plan_unit(cleared);
        scene.raster_processor(processor);
        const CardPicture drawn = card.raster(scene);
        check_exact(
            depth_plane ? "cleared, with a depth plane" : "cleared",
            compare(drawn, processor, scene)
        );
        // Under the key's polygon, beside the higher square: clear.
        OA_CHECK(!drawn_by_processor(processor.at(unit_x - 6, unit_z - 5), scene.key));
        OA_CHECK(drawn.at(unit_x - 6, unit_z - 5)[3] == 0);
        // The square at the ground south of the key's polygon, and the
        // higher square over the key's.
        OA_CHECK(drawn.at(unit_x, unit_z + 4)[0] == ink);
        OA_CHECK(drawn.at(unit_x, unit_z - 6)[0] == second_ink);
        std::size_t key_coloured = 0;
        for (std::size_t i = 0; i + 3 < drawn.rgba.size(); i += 4)
            if (drawn.rgba[i + 3] != 0 && drawn.rgba[i] == draw::image_key)
                ++key_coloured;
        OA_CHECK(key_coloured == 0);
    }
}

// A carried unit: composed into its carrier's depth image at its offset,
// its height over the carrier lifting it and ordering it in the plane; and
// drawn flat over a carrier without a depth plane.
void test_carried() {
    Scene scene;
    SceneUnit& carrier = scene.add_unit(square_model(), 1, unit_x, unit_z);
    carrier.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
    SceneUnit& cargo = scene.add_unit(square_model(second_ink), 2, unit_x + 4, unit_z);
    cargo.unit->position.y = 6 * unit;
    cargo.unit->attach_parent = oa::oa_ref_from_index(1);
    carrier.unit->attach_first_child = oa::oa_ref_from_index(2);
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(carrier);
    scene.plan_unit(cargo);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_exact("carried", compare(drawn, processor, scene));
    OA_CHECK(card.stage.counts().carried == 1);
    // The cargo, higher, shows over the carrier where they overlap.
    OA_CHECK(drawn.at(unit_x + 4, unit_z - 3)[0] == second_ink);
    // A plain carrier draws its cargo flat over itself.
    carrier.unit->flags2 = 0;
    carrier.state = {};
    scene.begin_frame(processor, 0);
    scene.plan_unit(carrier);
    scene.plan_unit(cargo);
    scene.raster_processor(processor);
    check_exact("carried by a plain carrier", compare(card.raster(scene), processor, scene));
}

// Shadows: a vehicle's silhouette and a building's sheared one, into the
// shadow target composed at half darkness, falling where the processor's
// fall; and straight into the frame where the target cannot be made.
void test_shadows() {
    Scene scene;
    SceneUnit& vehicle = scene.add_unit(square_model(), 1, unit_x, unit_z);
    vehicle.unit->position.y = 4 * unit;
    constexpr uint16_t flags = draw::graphics_shadows | draw::graphics_vehicle_shadows;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, flags);
    scene.plan_unit(vehicle);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    const auto c = compare(drawn, processor, scene);
    print("vehicle shadow", c);
    OA_CHECK(c.far == 0 && c.far_coverage == 0);
    OA_CHECK(card.stage.shadows_through_target());
    OA_CHECK(card.stage.counts().shadows == 1);
    OA_CHECK(card.stage.counts().target_creations == 1);
    // The silhouette lies five pixels right of the body and at the ground,
    // two rows below the lifted body: a half-dark pixel at its right edge,
    // in its last row.
    const uint32_t shade_x = unit_x + half_side + 4;
    const uint32_t shade_y = unit_z + half_side - 2;
    const uint8_t* shade = drawn.at(shade_x, shade_y);
    OA_CHECK(shade[3] >= 126 && shade[3] <= 129);
    OA_CHECK(shade[0] == 0);
    OA_CHECK(drawn_by_processor(processor.at(shade_x, shade_y), scene.key));
    bool resolved = false;
    for (const card::Batch& batch : card.frame.batches)
        if (batch.operation == card::Operation::resolve) {
            resolved = true;
            OA_CHECK(batch.blend == card::Blend::alpha_premultiplied);
            OA_CHECK(batch.destination.width == card.canvas.width);
        }
    OA_CHECK(resolved);
    // A second frame makes no second target.
    scene.begin_frame(processor, flags);
    scene.plan_unit(vehicle);
    scene.raster_processor(processor);
    std::ignore = card.raster(scene);
    OA_CHECK(card.stage.counts().target_creations == 1);
    // A building casts its cached pieces sheared by a quarter of their height.
    SceneUnit& tower = scene.add_unit(turret_model(), 2, unit_x, unit_z);
    tower.unit->flags |= OA_UNIT_FLAG_BUILDING;
    scene.begin_frame(processor, flags);
    scene.plan_unit(tower);
    scene.raster_processor(processor);
    const CardPicture sheared = card.raster(scene);
    const auto b = compare(sheared, processor, scene);
    print("building shadow", b);
    OA_CHECK(b.far == 0 && b.far_coverage == 0);
    OA_CHECK(card.stage.counts().shadows == 4);
    // Shadows off, none is cast.
    scene.begin_frame(processor, 0);
    scene.plan_unit(vehicle);
    scene.raster_processor(processor);
    const CardPicture plain = card.raster(scene);
    OA_CHECK(pixels_with_alpha(plain, 255) + pixels_with_alpha(plain, 0) == plain.rgba.size() / 4);
    OA_CHECK(card.stage.counts().shadows == 4);
    // Where the target exceeds the texture limit, the shadows go straight
    // into the frame.
    Scene wide;
    wide.width = 1100;
    wide.height = 64;
    SceneUnit& lone = wide.add_unit(square_model(), 1, unit_x, 40);
    lone.unit->position.y = 4 * unit;
    Card limited(wide, 1.0F, 1024);
    ProcessorPicture wide_processor;
    wide.begin_frame(wide_processor, flags);
    wide.plan_unit(lone);
    wide.raster_processor(wide_processor);
    const CardPicture direct = limited.raster(wide);
    OA_CHECK(!limited.stage.shadows_through_target());
    OA_CHECK(!limited.stage.error().empty());
    const auto d = compare(direct, wide_processor, wide);
    print("shadow without a target", d);
    OA_CHECK(d.far == 0 && d.far_coverage == 0 && d.shadowed > 0);
}

// An unfinished building near the start of building: the bands clear its
// image, and its shadow, cut by the image's place as the processor cuts it,
// lies only past the building, so that the ground shows unshaded through
// the image, in the card's picture as in the processor's.
void test_unfinished_building_shadow() {
    Scene scene;
    SceneUnit& tower = scene.add_unit(turret_model(), 2, unit_x, unit_z);
    tower.unit->flags |= OA_UNIT_FLAG_BUILDING;
    tower.unit->build_remaining = 0.9F;
    scene.tick = 7;
    constexpr uint16_t flags = draw::graphics_shadows | draw::graphics_vehicle_shadows;
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, flags);
    scene.plan_unit(tower);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    const auto c = compare(drawn, processor, scene);
    print("unfinished building shadow", c);
    OA_CHECK(card.stage.shadows_through_target());
    OA_CHECK(c.drawn > 0 && c.far == 0 && c.far_coverage == 0 && c.shadowed > 0);
    OA_CHECK(drawn.at(unit_x, unit_z)[3] == 0);
    OA_CHECK(!drawn_by_processor(processor.at(unit_x, unit_z), scene.key));
}

// Shadows fading as the view zooms out (set_frame_shadows): a vehicle's
// silhouette and a projectile's shadow sprite, on the card and on the
// processor, at the game's darkness at zoom 1, at a quarter of the frame's
// darkness (half the game's) at zoom 0.5, and not drawn at all at zoom
// 0.25, where the card neither clears nor composes the shadow target.
void test_faded_shadows() {
    Scene scene;
    SceneUnit& vehicle = scene.add_unit(square_model(), 1, unit_x, unit_z);
    vehicle.unit->position.y = 4 * unit;
    const std::shared_ptr<const Model> model = square_model(third_ink);
    const draw::PreparedModel& prepared = draw::prepare_model(scene.library, model);
    constexpr uint16_t flags = draw::graphics_shadows | draw::graphics_vehicle_shadows;
    draw::ShadowTable table;
    Card card(scene);
    ProcessorPicture processor;
    // The vehicle's silhouette's last row, at its right edge; the
    // projectile's shadow, under a shot lifted well clear of it.
    const uint32_t shade_x = unit_x + half_side + 4;
    const uint32_t shade_y = unit_z + half_side - 2;
    constexpr uint32_t shot_x = 30;
    constexpr uint32_t shot_z = 130;
    const auto frame_at = [&](float zoom) {
        scene.begin_frame(processor, flags);
        oa::app::set_frame_shadows(
            scene.list, scene.renderer, table, scene.display, &scene.shadow_sprite, zoom
        );
        scene.plan_unit(vehicle);
        ProjectileDraw shot;
        shot.position = {
            static_cast<int32_t>(shot_x) * unit, 40 * unit, static_cast<int32_t>(shot_z) * unit
        };
        shot.shadow = true;
        shot.x = shot_x;
        shot.shadow_y = shot_z;
        shot.object = &model->objects[0];
        shot.prepared = &prepared.objects[0];
        shot.region = {0, 0, scene.width - 1, scene.height - 1};
        scene.list.projectiles.push_back(shot);
        oa::app::add_world_draw(scene.list, WorldDrawKind::projectile, 0);
        scene.raster_processor(processor);
        return card.raster(scene);
    };
    const auto brightness = [](const uint8_t* pixel) { return pixel[0] + pixel[1] + pixel[2]; };
    const auto resolves = [&]() {
        std::size_t count = 0;
        for (const card::Batch& batch : card.frame.batches)
            count += batch.operation == card::Operation::resolve ? 1U : 0U;
        return count;
    };
    // Zoom 1: the game's own shadows.
    const CardPicture full = frame_at(1.0F);
    OA_CHECK(scene.list.shadow_level == draw::shadow_full_level);
    OA_CHECK(full.at(shade_x, shade_y)[3] >= 126 && full.at(shade_x, shade_y)[3] <= 129);
    OA_CHECK(full.at(shot_x, shot_z)[3] >= 126 && full.at(shot_x, shot_z)[3] <= 129);
    OA_CHECK(card.stage.counts().shadows == 2);
    OA_CHECK(resolves() == 1);
    const ProcessorPicture full_processor = processor;
    const uint8_t* dark = full_processor.at(shade_x, shade_y);
    const uint8_t* dark_shot = full_processor.at(shot_x, shot_z);
    OA_CHECK(drawn_by_processor(dark, scene.key));
    OA_CHECK(drawn_by_processor(dark_shot, scene.key));
    // Zoom 0.5: half the game's darkness, a quarter alpha on the card, and
    // between the ground and the game's shadow on the processor.
    const CardPicture half = frame_at(0.5F);
    OA_CHECK(scene.list.shadow_level == draw::shadow_full_level / 2);
    OA_CHECK(half.at(shade_x, shade_y)[3] >= 62 && half.at(shade_x, shade_y)[3] <= 66);
    OA_CHECK(half.at(shot_x, shot_z)[3] >= 62 && half.at(shot_x, shot_z)[3] <= 66);
    OA_CHECK(card.stage.counts().shadows == 4);
    const uint8_t* light = processor.at(shade_x, shade_y);
    const uint8_t* light_shot = processor.at(shot_x, shot_z);
    OA_CHECK(drawn_by_processor(light, scene.key));
    OA_CHECK(brightness(light) > brightness(dark));
    OA_CHECK(brightness(light) < brightness(scene.key.data()));
    OA_CHECK(brightness(light_shot) > brightness(dark_shot));
    OA_CHECK(brightness(light_shot) < brightness(scene.key.data()));
    // The bodies are as at zoom 1.
    OA_CHECK(std::memcmp(processor.at(unit_x, unit_z), full_processor.at(unit_x, unit_z), 3) == 0);
    // Zoom 0.25: no shadow at all, and no work for one.
    const CardPicture none = frame_at(0.25F);
    OA_CHECK(scene.list.shadow_level == 0);
    OA_CHECK(none.at(shade_x, shade_y)[3] == 0);
    OA_CHECK(none.at(shot_x, shot_z)[3] == 0);
    OA_CHECK(card.stage.counts().shadows == 4);
    OA_CHECK(resolves() == 0);
    OA_CHECK(!card.stage.shadows_through_target());
    OA_CHECK(!drawn_by_processor(processor.at(shade_x, shade_y), scene.key));
    OA_CHECK(!drawn_by_processor(processor.at(shot_x, shot_z), scene.key));
    OA_CHECK(std::memcmp(processor.at(unit_x, unit_z), full_processor.at(unit_x, unit_z), 3) == 0);
    // Zooming back in brings them back as they were.
    const CardPicture again = frame_at(1.0F);
    OA_CHECK(again.rgba == full.rgba);
    OA_CHECK(processor.rgb == full_processor.rgb);
}

// A projectile and its shadow sprite, a debris piece and a shatter
// fragment, flat as the processor draws them.
void test_flat_objects() {
    Scene scene;
    const std::shared_ptr<const Model> model = square_model(third_ink);
    scene.add_unit(model, 1, unit_x, unit_z);
    const draw::PreparedModel& prepared = draw::prepare_model(scene.library, model);
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    // The projectile, lifted 10 units, with its shadow at the ground.
    ProjectileDraw shot;
    shot.position = {30 * unit, 10 * unit, 30 * unit};
    shot.shadow = true;
    shot.x = 30;
    shot.shadow_y = 30;
    shot.rotation = {0, 0x0400, 0};
    shot.object = &model->objects[0];
    shot.prepared = &prepared.objects[0];
    shot.region = {0, 0, scene.width - 1, scene.height - 1};
    scene.list.projectiles.push_back(shot);
    oa::app::add_world_draw(scene.list, WorldDrawKind::projectile, 0);
    // The debris piece, spinning.
    DebrisDraw piece;
    piece.object = &model->objects[0];
    piece.prepared = &prepared.objects[0];
    piece.spin = {0x0300, 0x0900, 0x0100};
    piece.origin = {100 * unit, 6 * unit, 40 * unit};
    piece.team = team_colour;
    piece.region = {0, 0, scene.width - 1, scene.height - 1};
    scene.list.debris.push_back(piece);
    oa::app::add_world_draw(scene.list, WorldDrawKind::debris, 0);
    // The fragment: a slab four units a side, coloured as the primitive it
    // broke from.
    fx::ShatterFragment fragment{};
    fragment.live = true;
    const int32_t s = 4 * unit;
    const std::array<FixedVec3, fx::fragment_point_count> corners{
        FixedVec3{-s, -s, -s},
        FixedVec3{s, -s, -s},
        FixedVec3{s, -s, s},
        FixedVec3{-s, -s, s},
        FixedVec3{-s, s, s},
        FixedVec3{s, s, s},
        FixedVec3{s, s, -s},
        FixedVec3{-s, s, -s}
    };
    std::copy(corners.begin(), corners.end(), std::begin(fragment.points));
    fragment.look.flags = draw::primitive_colored;
    fragment.look.color = second_ink;
    FragmentDraw shatter;
    shatter.position = {120 * unit, 8 * unit, 110 * unit};
    shatter.fragment = &fragment;
    shatter.primitive = &prepared.objects[0].primitives[0];
    shatter.spin = {0x0200, 0x0500, 0x0300};
    shatter.region = {0, 0, scene.width - 1, scene.height - 1};
    scene.list.fragments.push_back(shatter);
    oa::app::add_world_draw(scene.list, WorldDrawKind::fragment, 0);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    const auto c = compare(drawn, processor, scene);
    print("flat objects", c);
    OA_CHECK(c.drawn > 0 && c.far == 0 && c.far_coverage == 0);
    OA_CHECK(c.edge_coverage <= 3 * edge_pixels_allowed);
    OA_CHECK(card.stage.counts().projectiles == 1);
    OA_CHECK(card.stage.counts().debris == 1);
    OA_CHECK(card.stage.counts().fragments == 1);
    OA_CHECK(card.stage.counts().shadows == 1);
    OA_CHECK(card.stage.shadows_through_target());
    // The projectile's body over its shadow: the body at full alpha.
    OA_CHECK(drawn.at(30, 25)[3] == 255 && drawn.at(30, 25)[0] == third_ink);
    // Debris beyond the visible rectangle is culled on its origin.
    scene.begin_frame(processor, 0);
    piece.origin = {(scene.width + 10) * unit, 0, 40 * unit};
    scene.list.debris.push_back(piece);
    oa::app::add_world_draw(scene.list, WorldDrawKind::debris, 0);
    scene.raster_processor(processor);
    const CardPicture culled = card.raster(scene, 1.0F, false);
    OA_CHECK(pixels_with_alpha(culled, 0) == culled.rgba.size() / 4);
    OA_CHECK(card.stage.counts().debris == 1);
}

// The pages: emptied by a palette or gamma change and placed again, the
// executor's pages freed by close, and the frames through a gamma.
void test_pages_and_gamma() {
    TextureFixture textures;
    Scene scene(gray_palette(), std::move(textures.library), 1.4F);
    SceneUnit& quads = scene.add_unit(textured_model(), 1, unit_x, unit_z);
    Card card(scene);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(quads);
    scene.raster_processor(processor);
    const CardPicture drawn = card.raster(scene);
    check_textured("textured through gamma 1.4", compare(drawn, processor, scene));
    const uint8_t* corner = drawn.at(unit_x + 29, unit_z - half_side);
    OA_CHECK(corner[0] == oa::present::gamma_channel(32, 1.4F));
    OA_CHECK(card.stage.counts().frames_placed == 3);
    OA_CHECK(card.executor.counts().pages_alive >= 1);
    // Another gamma empties the pages: the frames are placed again.
    card.stage.set_palette(scene.palette, 1.0F);
    OA_CHECK(card.stage.pages().memory().frames == 0);
    card.stage.set_palette(scene.palette, 1.0F);
    scene.gamma = 1.0F;
    scene.key = key_colour(scene.palette, 1.0F, scene.display);
    scene.begin_frame(processor, 0);
    scene.plan_unit(quads);
    scene.raster_processor(processor);
    check_textured("textured after the change", compare(card.raster(scene), processor, scene));
    OA_CHECK(card.stage.counts().frames_placed == 6);
    OA_CHECK(card.stage.counts().meshes_built == 2);
    // Closed, nothing of the stage's is left on the executor.
    card.stage.close(card.executor);
    OA_CHECK(card.executor.counts().pages_alive == 0);
    OA_CHECK(card.executor.counts().targets_alive == 0);
    OA_CHECK(card.stage.mesh_count() == 0);
}

// At zoom 2 the frame is the zoom-1 raster enlarged; at a zoom that is no
// whole number the draws sample linear and land at the zoom; the smooth-pan
// offset moves every vertex.
void test_zoom() {
    Scene scene;
    SceneUnit& square = scene.add_unit(square_model(), 1, unit_x, unit_z);
    square.unit->heading = static_cast<uint16_t>(turned_heading.xz);
    ProcessorPicture processor;
    scene.begin_frame(processor, 0);
    scene.plan_unit(square);
    scene.raster_processor(processor);
    Card doubled(scene, 2.0F);
    const CardPicture drawn = doubled.raster(scene, 2.0F);
    ProcessorPicture enlarged;
    enlarged.width = processor.width * 2;
    enlarged.height = processor.height * 2;
    enlarged.rgb.resize(std::size_t{enlarged.width} * enlarged.height * 3U);
    for (uint32_t y = 0; y < enlarged.height; ++y)
        for (uint32_t x = 0; x < enlarged.width; ++x)
            std::memcpy(
                enlarged.rgb.data() + (std::size_t{y} * enlarged.width + x) * 3U,
                processor.at(x / 2, y / 2),
                3
            );
    const auto c = full::compare_model_rasters(
        drawn.rgba.data(), enlarged.rgb.data(), enlarged.width, enlarged.height, scene.key
    );
    print("zoom 2", c);
    // The enlarged raster steps two pixels along a sloped edge where the
    // card's edge runs straight.
    OA_CHECK(c.drawn > 0 && c.far == 0 && c.far_coverage <= edge_pixels_allowed);
    OA_CHECK(drawn.at(2 * unit_x, 2 * unit_z)[0] == ink);
    Card between(scene, 1.5F);
    const CardPicture scaled = between.raster(scene, 1.5F);
    OA_CHECK(scaled.at(120, 120)[0] == ink && scaled.at(120, 120)[3] == 255);
    for (const card::Batch& batch : between.frame.batches)
        if (batch.operation == card::Operation::draw)
            OA_CHECK(batch.sampling == card::Sampling::linear);
    full::SceneView view;
    // The battlefield four pixels right in the target, and the view half a
    // map pixel past the camera, which at zoom 2 moves every vertex a pixel
    // left: three pixels right in all.
    view.origin_x = 4.0F;
    view.offset.x = 0.5;
    view.zoom = 2.0F;
    view.width = doubled.canvas.width;
    view.height = doubled.canvas.height;
    doubled.canvas.clear();
    doubled.frame.reset();
    doubled.stage.emit_frame(scene.inputs(), view, doubled.executor, doubled.frame);
    OA_CHECK(doubled.stage.upload(doubled.executor));
    OA_CHECK(doubled.executor.execute(doubled.frame, doubled.canvas.target));
    const CardPicture panned = doubled.canvas.read();
    // The turned square's left corner moved three pixels right.
    uint32_t first_drawn = 0;
    uint32_t first_panned = 0;
    for (uint32_t x = 0; x < drawn.width; ++x)
        if (drawn.at(x, 2 * unit_z)[3] != 0) {
            first_drawn = x;
            break;
        }
    for (uint32_t x = 0; x < panned.width; ++x)
        if (panned.at(x, 2 * unit_z)[3] != 0) {
            first_panned = x;
            break;
        }
    OA_CHECK(first_panned == first_drawn + 3);
}

// The bounds helper and the comparison's shares.
void test_bounds() {
    full::ModelRasterComparison c;
    OA_CHECK(!full::within_bounds(c, {}));
    c.drawn = 1000;
    c.same = 900;
    c.far = 100;
    OA_CHECK(c.far_share() == 0.1);
    OA_CHECK(full::within_bounds(c, {}));
    c.far_coverage = 3;
    OA_CHECK(full::within_bounds(c, {}));
    c.far_coverage = 50;
    OA_CHECK(c.far_coverage_share() == 0.05);
    OA_CHECK(!full::within_bounds(c, {}));
    c.far_coverage = 0;
    c.far = 400;
    OA_CHECK(!full::within_bounds(c, {}));
}

// ---------------------------------------------------------------------------
// The installed game

/// The unit models units/*.fbi name, case-folded.
std::vector<std::string> installed_unit_models(const oa::AssetStore& assets) {
    std::set<std::string> names;
    for (const std::string& path : assets.list_effective_in_mount_order("units", ".fbi")) {
        const auto bytes = assets.load_file_contents(path);
        if (!bytes || bytes->empty() || bytes->size() > oa::formats::tdf::max_input_bytes)
            continue;
        oa::formats::tdf::Document document;
        oa::formats::tdf::document_init(&document);
        oa::formats::tdf::ParseError error;
        if (oa::formats::tdf::parse_text(
                &document,
                reinterpret_cast<const char*>(bytes->data()),
                static_cast<uint32_t>(bytes->size()),
                true,
                &error
            )) {
            const auto* info = oa::formats::tdf::find_child(document.root, "UNITINFO");
            const char* name =
                info != nullptr ? oa::formats::tdf::find_value(info, "Objectname") : nullptr;
            if ((name == nullptr || *name == '\0') && info != nullptr)
                name = oa::formats::tdf::find_value(info, "UnitName");
            if (name != nullptr && *name != '\0') {
                std::string folded(name);
                for (char& c : folded)
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c - 'A' + 'a');
                names.insert(folded);
            }
        }
        oa::formats::tdf::document_free(&document);
    }
    return {names.begin(), names.end()};
}

/// Prints the rows of two pictures over a model's extent, the card's and
/// the processor's values side by side, for a diagnosis.
void dump(const CardPicture& by_card, const ProcessorPicture& by_processor, const Scene& scene) {
    uint32_t left = by_card.width;
    uint32_t right = 0;
    uint32_t top = by_card.height;
    uint32_t bottom = 0;
    for (uint32_t y = 0; y < by_card.height; ++y)
        for (uint32_t x = 0; x < by_card.width; ++x)
            if (by_card.at(x, y)[3] != 0 || drawn_by_processor(by_processor.at(x, y), scene.key)) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    if (right < left)
        return;
    for (uint32_t y = top; y <= bottom; ++y) {
        std::printf("row %3u card:", y);
        for (uint32_t x = left; x <= right; ++x)
            std::printf(" %3u", by_card.at(x, y)[3] != 0 ? by_card.at(x, y)[0] : 0U);
        std::printf("\n        proc:");
        for (uint32_t x = left; x <= right; ++x)
            std::printf(
                " %3u",
                drawn_by_processor(by_processor.at(x, y), scene.key) ? by_processor.at(x, y)[0] : 0U
            );
        std::printf("\n");
    }
}

/// The sums of a mode of the sweep.
struct ModeTotals {
    const char* name{};
    full::ModelRasterComparison sum{};
    std::size_t models{};
    std::size_t failed{};
    std::string worst_far;
    double worst_far_share{};
    std::string worst_coverage;
    double worst_coverage_share{};
};

void add(ModeTotals& totals, const std::string& name, const full::ModelRasterComparison& c) {
    totals.sum.drawn += c.drawn;
    totals.sum.same += c.same;
    totals.sum.phase += c.phase;
    totals.sum.edge_colour += c.edge_colour;
    totals.sum.far += c.far;
    totals.sum.edge_coverage += c.edge_coverage;
    totals.sum.far_coverage += c.far_coverage;
    totals.sum.shadowed += c.shadowed;
    ++totals.models;
    if (c.far_share() > totals.worst_far_share) {
        totals.worst_far_share = c.far_share();
        totals.worst_far = name;
    }
    if (c.far_coverage_share() > totals.worst_coverage_share) {
        totals.worst_coverage_share = c.far_coverage_share();
        totals.worst_coverage = name;
    }
}

void print(const ModeTotals& totals) {
    const auto& c = totals.sum;
    const double both = static_cast<double>(c.same + c.phase + c.edge_colour + c.far);
    const double drawn = static_cast<double>(c.drawn);
    std::printf(
        "%s: %zu models, %zu outside the bounds; %zu pixels drawn; of the pixels both draw "
        "%.1f%% the same, %.1f%% a texel of phase, %.1f%% another colour at an edge, %.1f%% "
        "far; coverage differs at %.2f%% of the drawn pixels within a pixel of an edge and "
        "%.3f%% beyond; worst far %s %.1f%%, worst coverage %s %.2f%%\n",
        totals.name,
        totals.models,
        totals.failed,
        c.drawn,
        both > 0.0 ? 100.0 * static_cast<double>(c.same) / both : 0.0,
        both > 0.0 ? 100.0 * static_cast<double>(c.phase) / both : 0.0,
        both > 0.0 ? 100.0 * static_cast<double>(c.edge_colour) / both : 0.0,
        100.0 * c.far_share(),
        drawn > 0.0 ? 100.0 * static_cast<double>(c.edge_coverage) / drawn : 0.0,
        100.0 * c.far_coverage_share(),
        totals.worst_far.c_str(),
        100.0 * totals.worst_far_share,
        totals.worst_coverage.c_str(),
        100.0 * totals.worst_coverage_share
    );
}

/// Every unit model of the installation, at zoom 1, in three modes, against
/// the processor's raster sampled at the pixel centres; the band raster,
/// sampled at the pixel corners, is reported beside it.
void test_installed_unit_models(oa::AssetStore& assets, const char* dumped) {
    const auto pal = oa::test::read_game_file(assets, "palettes/palette.pal");
    OA_CHECK(!pal.empty());
    const oa::Palette palette = oa::present::palette_from_bytes(pal);
    Scene scene(palette, draw::load_texture_library(assets));
    scene.width = installed_width;
    scene.height = installed_height;
    scene.tick = 100;
    Card card(scene);
    const std::vector<std::string> names = installed_unit_models(assets);
    std::printf("units: %zu model names\n", names.size());
    std::array<ModeTotals, 3> modes{};
    modes[0].name = "plain image, unlit, at rest";
    modes[1].name = "plain image, unlit, turned";
    modes[2].name = "depth image, lit building, at rest";
    std::array<ModeTotals, 3> corner_modes{};
    corner_modes[0].name = "against the band raster, at rest";
    corner_modes[1].name = "against the band raster, turned";
    corner_modes[2].name = "against the band raster, lit building";
    const full::ModelRasterBounds model_bounds{
        installed_model_far_coverage_share,
        installed_model_far_coverage_pixels,
        installed_model_far_share
    };
    const full::ModelRasterBounds lit_bounds{
        installed_model_far_coverage_share, installed_model_far_coverage_pixels, 1.0
    };
    std::size_t loaded = 0;
    std::size_t missing = 0;
    std::size_t refused = 0;
    std::vector<std::shared_ptr<const Model>> kept;
    ProcessorPicture processor;
    ProcessorPicture centred;
    uint16_t slot = 1;
    for (const std::string& name : names) {
        const auto bytes = assets.load_file_contents("objects3d/" + name + ".3do");
        if (!bytes || bytes->empty()) {
            ++missing;
            continue;
        }
        auto loaded_model = oa::formats::objects3d::load_3do(std::as_bytes(std::span(*bytes)));
        if (!loaded_model.ok()) {
            ++missing;
            continue;
        }
        ++loaded;
        auto model = std::make_shared<const Model>(std::move(*loaded_model.value));
        kept.push_back(model);
        for (std::size_t mode = 0; mode < modes.size(); ++mode) {
            // Each run in a fresh slot, so that no draw state carries over.
            scene.units.clear();
            SceneUnit& drawn_unit = scene.add_unit(model, slot, installed_x, installed_z);
            slot = static_cast<uint16_t>(slot % 8 + 1);
            if (drawn_unit.instance.pieces().empty()) {
                ++refused;
                break;
            }
            uint16_t flags = 0;
            if (mode == 1) {
                drawn_unit.unit->bank = installed_turn.xy;
                drawn_unit.unit->heading = static_cast<uint16_t>(installed_turn.xz);
                drawn_unit.unit->pitch = installed_turn.yz;
            } else if (mode == 2) {
                drawn_unit.unit->flags |= OA_UNIT_FLAG_BUILDING;
                drawn_unit.unit->flags2 |= OA_UNIT_FLAG2_Z_BUFFER;
                flags = draw::graphics_shading;
            }
            scene.begin_frame(processor, flags);
            scene.plan_unit(drawn_unit);
            scene.raster_processor(processor);
            scene.raster_processor_centred(drawn_unit, centred);
            const CardPicture picture = card.raster(scene);
            const auto c = compare(picture, centred, scene);
            add(modes[mode], name, c);
            add(corner_modes[mode], name, compare(picture, processor, scene));
            if (dumped != nullptr && name == dumped) {
                std::printf(
                    "%s, %s: the card over the centred raster\n", name.c_str(), modes[mode].name
                );
                dump(picture, centred, scene);
                print_far(picture, centred, scene);
            }
            const bool within = full::within_bounds(c, mode == 2 ? lit_bounds : model_bounds);
            if (!within) {
                ++modes[mode].failed;
                std::fprintf(
                    stderr,
                    "%s, %s: drawn %zu, far %zu (%.1f%%), far coverage %zu (%.2f%%)\n",
                    name.c_str(),
                    modes[mode].name,
                    c.drawn,
                    c.far,
                    100.0 * c.far_share(),
                    c.far_coverage,
                    100.0 * c.far_coverage_share()
                );
            }
        }
        OA_CHECK(card.stage.error().empty());
    }
    std::printf(
        "models: %zu loaded, %zu missing, %zu refused; %zu meshes, %zu bytes; %u pages, %u "
        "frames, %zu texel bytes; bright pages %u, %u frames\n",
        loaded,
        missing,
        refused,
        card.stage.mesh_count(),
        card.stage.mesh_bytes(),
        card.stage.pages().memory().pages,
        card.stage.pages().memory().frames,
        card.stage.pages().memory().page_bytes,
        card.stage.bright_pages().memory().pages,
        card.stage.bright_pages().memory().frames
    );
    OA_CHECK(loaded >= least_installed_models);
    // How far two triangles would move a textured quad's texels from where
    // the walk puts them, at most, for the zooms the strips are cut at and
    // the one past which they are not.
    const double twist = card.stage.counts().widest_twist;
    std::printf(
        "models: %llu of %llu polygons drawn as strips; the widest twist is %.1f map pixels, "
        "two triangles moving texels by %.2f pixels at zoom 1, %.2f at 0.5 and %.2f at 0.25\n",
        static_cast<unsigned long long>(card.stage.counts().strip_quads),
        static_cast<unsigned long long>(card.stage.counts().polygons),
        twist,
        twist / 4.0,
        twist / 8.0,
        twist / 16.0
    );
    for (const ModeTotals& totals : corner_modes)
        print(totals);
    for (const ModeTotals& totals : modes) {
        print(totals);
        OA_CHECK(totals.failed == 0);
        OA_CHECK(totals.sum.far_coverage_share() <= installed_sweep_far_coverage_share);
    }
    OA_CHECK(modes[0].sum.far_share() <= installed_sweep_far_share);
    OA_CHECK(modes[1].sum.far_share() <= installed_sweep_far_share);
    OA_CHECK(modes[2].sum.far_share() <= installed_lit_far_share);
}

} // namespace

int main(int argc, char** argv) {
    if (!SDL_Init(0)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (oa::test::game_data_requested(argc, argv)) {
        oa::AssetStore assets = oa::test::require_game_assets("the installed unit models");
        // A model named after --data is dumped, for a diagnosis.
        test_installed_unit_models(assets, argc > 2 ? argv[2] : nullptr);
    } else {
        test_square();
        test_back_face();
        test_textured();
        test_textured_trapezoid();
        test_depth_sorted();
        test_resting_on_depth_plane();
        test_lit_building();
        test_cloaked();
        test_nanoframe();
        test_nanoframe_zoomed_out();
        test_nanoframe_pulse_lag();
        test_mobile_nanoframe_once_built();
        test_underwater();
        test_digger();
        test_clearing();
        test_carried();
        test_shadows();
        test_unfinished_building_shadow();
        test_faded_shadows();
        test_flat_objects();
        test_pages_and_gamma();
        test_zoom();
        test_bounds();
    }
    SDL_Quit();
    return oa::test::check_exit_status();
}
