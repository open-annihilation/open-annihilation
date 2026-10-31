// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Exercise persistent GPU resources, atlas revisions, resizing and invalid input without a window.
#include "renderer_live_gpu.hpp"
#include "renderer_effects.hpp"
#include "renderer_live_camera.hpp"
#include "renderer_water_appearance.hpp"
#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <limits>
#include <span>

namespace {
using namespace oa::app::renderer_live_gpu;
constexpr int skipped = 77;
constexpr uint32_t channels = 4;
constexpr uint32_t small_target = 8;
constexpr uint32_t large_target = 16;
constexpr uint32_t horizontal_translation = 4;
constexpr uint8_t full_channel = 255;
constexpr std::array<float, 12> classic_projection{1, 0, 0, 0, 0, -0.5f, 1, 0, 0, 1, 0.5f, 0};

/// Associate every vertex component with its inclusive accepted bounds.
struct VertexBound {
    float Vertex::* member{};
    float low{};
    float high{};
};

constexpr float coordinate_limit = 32768;
constexpr VertexBound vertex_bounds[]{
    {&Vertex::x, -coordinate_limit, coordinate_limit},
    {&Vertex::height, -coordinate_limit, coordinate_limit},
    {&Vertex::z, -coordinate_limit, coordinate_limit},
    {&Vertex::u, 0, 1},
    {&Vertex::v, 0, 1},
    {&Vertex::brightness, 0, 1},
    {&Vertex::opacity, 0, 1},
    {&Vertex::emission_r, 0, 1},
    {&Vertex::emission_g, 0, 1},
    {&Vertex::emission_b, 0, 1},
    {&Vertex::terrain_known, 0, 1},
    {&Vertex::terrain_grey, 0, 1},
    {&Vertex::red, 0, 1},
    {&Vertex::green, 0, 1},
    {&Vertex::blue, 0, 1}
};

/// Hold one triangle, one atlas texel and the state reused between frames.
struct Scene {
    Context context{};
    std::vector<Vertex> vertices{{0, 0, 0, 0, 0}, {8, 0, 0, 0, 0}, {0, 0, 8, 0, 0}};
    std::vector<uint8_t> atlas{full_channel, 0, 0, full_channel};
    std::array<float, 12> projection = classic_projection;
    std::vector<uint8_t> pixels{};
    std::string error{};

    /// Render the fixture with the requested revision and square target size.
    Result frame(uint64_t revision = 1, uint32_t size = small_target, uint32_t supersample = 1) {
        return render(
            context,
            vertices,
            atlas,
            1,
            1,
            revision,
            size,
            size,
            projection,
            pixels,
            error,
            supersample
        );
    }

    /// Compare one complete RGBA pixel only after checking the output bounds.
    bool pixel(uint32_t x, uint32_t y, uint32_t width, uint8_t red, uint8_t green) const {
        const size_t offset = (size_t(y) * width + x) * channels;
        return offset + channels <= pixels.size() && pixels[offset] == red &&
               pixels[offset + 1] == green && pixels[offset + 2] == 0 &&
               pixels[offset + 3] == full_channel;
    }
};

/// Reject malformed frames before creating a GPU, clearing stale output on failure.
void invalid_input() {
    Scene scene;
    const float infinity = std::numeric_limits<float>::infinity();
    for (const auto& bound : vertex_bounds) {
        const auto vertices_before = scene.vertices;
        for (float invalid :
             {std::numeric_limits<float>::quiet_NaN(),
              -infinity,
              infinity,
              std::nextafter(bound.low, -infinity),
              std::nextafter(bound.high, infinity)}) {
            scene.vertices[0].*bound.member = invalid;
            OA_CHECK(scene.frame() == Result::failed);
            OA_CHECK(scene.pixels.empty());
        }
        scene.vertices = vertices_before;
    }
    scene.pixels.assign(channels, full_channel);
    scene.projection[0] = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(scene.frame() == Result::failed);
    OA_CHECK(scene.pixels.empty());
    OA_CHECK(!scene.error.empty());
    scene.projection = classic_projection;
    scene.vertices[0].u = -1;
    OA_CHECK(scene.frame() == Result::failed);
    scene.vertices[0].u = 0;
    OA_CHECK(scene.frame(1, 0) == Result::failed);
    OA_CHECK(scene.frame(1, small_target, 0) == Result::failed);
    OA_CHECK(scene.frame(1, small_target, 3) == Result::failed);
    OA_CHECK(scene.frame(1, small_target, 32) == Result::failed);
    scene.vertices[0].opacity = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(scene.frame() == Result::failed);
    scene.vertices[0].opacity = 1;
    scene.vertices[0].brightness = 2;
    OA_CHECK(scene.frame() == Result::failed);
    scene.vertices[0].brightness = 1;
    for (float Vertex::* channel :
         {&Vertex::emission_r, &Vertex::emission_g, &Vertex::emission_b}) {
        for (float invalid : {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN()}) {
            scene.vertices[0].*channel = invalid;
            OA_CHECK(scene.frame() == Result::failed);
        }
        scene.vertices[0].*channel = 0;
    }
    scene.atlas.pop_back();
    OA_CHECK(scene.frame() == Result::failed);
    scene.atlas.push_back(full_channel);
    scene.vertices.pop_back();
    OA_CHECK(scene.frame() == Result::failed);
}

/// Preserve unchanged atlas rows and fall back to full uploads when the cached base is absent.
void atlas_row_updates() {
    Context partial_context;
    Context full_context;
    constexpr uint32_t atlas_size = 4;
    constexpr uint32_t resized_width = 8;
    const std::vector<Vertex> vertices{
        {0, 0, 0, 0, 0},
        {8, 0, 0, 1, 0},
        {0, 0, 8, 0, 1},
        {8, 0, 0, 1, 0},
        {8, 0, 8, 1, 1},
        {0, 0, 8, 0, 1}
    };
    std::vector<uint8_t> atlas(atlas_size * atlas_size * channels, full_channel);
    std::vector<uint8_t> partial_pixels;
    std::vector<uint8_t> full_pixels;
    std::string error;
    /// Render both upload paths with identical samples and compare every output pixel.
    const auto compare = [&](uint64_t revision, const AtlasUpdate* update, uint32_t width) {
        OA_CHECK(
            render(
                partial_context,
                vertices,
                atlas,
                width,
                atlas_size,
                revision,
                small_target,
                small_target,
                classic_projection,
                partial_pixels,
                error,
                2,
                update
            ) == Result::success
        );
        OA_CHECK(
            render(
                full_context,
                vertices,
                atlas,
                width,
                atlas_size,
                revision,
                small_target,
                small_target,
                classic_projection,
                full_pixels,
                error,
                2
            ) == Result::success
        );
        OA_CHECK(partial_pixels == full_pixels);
    };
    const AtlasUpdate first_update{0, 1, 2};
    compare(1, &first_update, atlas_size); // A newly created texture still needs every row.
    for (uint32_t x = 0; x < atlas_size; ++x) {
        const size_t offset = (atlas_size + x) * channels;
        atlas[offset] = uint8_t(x);
        atlas[offset + 1] = 0;
    }
    const AtlasUpdate middle_row{1, 1, 2};
    compare(2, &middle_row, atlas_size);
    const auto before = partial_pixels;
    atlas[0] = 0;
    compare(2, &middle_row, atlas_size); // Identical revisions retain the cached atlas.
    OA_CHECK(partial_pixels == before);
    atlas[0] = full_channel;
    const AtlasUpdate no_rows{2, 2, 2};
    compare(3, &no_rows, atlas_size);
    OA_CHECK(partial_pixels == before);
    atlas[0] = 0;
    const AtlasUpdate stale_update{1, 1, 2};
    compare(4, &stale_update, atlas_size); // A revision gap requires the changed first row too.
    OA_CHECK(partial_pixels != before);
    atlas.assign(resized_width * atlas_size * channels, full_channel);
    atlas[0] = 0;
    const AtlasUpdate resized_update{4, 1, 2};
    compare(5, &resized_update, resized_width);
    // A changed rectangle beside the changed rows is uploaded alone.
    const auto texel = [&](uint32_t x, uint32_t y) {
        return atlas.begin() + static_cast<std::ptrdiff_t>((y * resized_width + x) * channels);
    };
    *texel(5, 2) = 0;
    AtlasUpdate region_update{5, 0, 1};
    region_update.regions.push_back({5, 2, 1, 1});
    compare(6, &region_update, resized_width);
    // A texel beside every changed row and rectangle is not.
    *texel(6, 3) = 0;
    const auto render_atlas =
        [&](Context& context, uint64_t revision, const AtlasUpdate* update, auto& pixels) {
            return render(
                context,
                vertices,
                atlas,
                resized_width,
                atlas_size,
                revision,
                small_target,
                small_target,
                classic_projection,
                pixels,
                error,
                2,
                update
            );
        };
    AtlasUpdate beside{6, 0, 1};
    beside.regions = region_update.regions;
    OA_CHECK(render_atlas(partial_context, 7, &beside, partial_pixels) == Result::success);
    OA_CHECK(render_atlas(full_context, 7, nullptr, full_pixels) == Result::success);
    OA_CHECK(partial_pixels != full_pixels);
    AtlasUpdate outside_region{7, 0, 0};
    outside_region.regions.push_back({7, 3, 2, 1});
    for (const AtlasUpdate& invalid :
         {AtlasUpdate{7, 3, 2}, AtlasUpdate{7, 0, atlas_size + 1}, outside_region}) {
        OA_CHECK(
            render(
                partial_context,
                vertices,
                atlas,
                resized_width,
                atlas_size,
                8,
                small_target,
                small_target,
                classic_projection,
                partial_pixels,
                error,
                2,
                &invalid
            ) == Result::failed
        );
        OA_CHECK(partial_pixels.empty());
    }
}

/// Filter terrain mip chains without reading neighbouring materials or changing sprite texels.
void terrain_filtering() {
    Context context;
    constexpr uint32_t atlas_size = 64;
    constexpr uint32_t tile_size = 32;
    std::vector<uint8_t> atlas(atlas_size * atlas_size * channels, full_channel);
    for (uint32_t mip = 0, size = tile_size; size; ++mip, size /= 2) {
        const uint32_t top = mip == 0 ? 0 : atlas_size - 2 * size;
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const size_t offset = ((top + y) * atlas_size + x) * channels;
                atlas[offset] = mip ? 128 : ((x + y) % 2 ? 255 : 0);
                atlas[offset + 1] = mip ? 128 : ((x + y) % 2 ? 0 : 255);
                atlas[offset + 2] = 0;
                for (uint32_t channel = 0; channel < 3; ++channel)
                    atlas[offset + tile_size * channels + channel] = 80;
            }
        }
    }
    std::vector<Vertex> vertices{
        {0, 0, 0, 0, 0},
        {32, 0, 0, 0.5f, 0},
        {0, 0, 32, 0, 0.5f},
        {32, 0, 0, 0.5f, 0},
        {32, 0, 32, 0.5f, 0.5f},
        {0, 0, 32, 0, 0.5f}
    };
    std::vector<uint8_t> pixels;
    std::string error;
    for (const uint32_t supersample : {1u, 2u}) {
        for (const uint32_t output_size : {8u, 11u, 16u, 32u, 64u}) {
            auto projection = classic_projection;
            projection[0] = projection[6] = float(output_size) / tile_size;
            for (const float grey : {0.0f, 0.5f, 1.0f}) {
                for (auto& vertex : vertices) {
                    vertex.terrain_size = tile_size;
                    vertex.terrain_grey = grey;
                    vertex.brightness = 0; // Terrain uses known visibility, not legacy attenuation.
                }
                OA_CHECK(
                    render(
                        context,
                        vertices,
                        atlas,
                        atlas_size,
                        atlas_size,
                        1,
                        output_size,
                        output_size,
                        projection,
                        pixels,
                        error,
                        supersample
                    ) == Result::success
                );
                OA_CHECK(pixels.size() == output_size * output_size * channels);
                for (size_t offset = 0; offset + channels <= pixels.size(); offset += channels) {
                    const int red = pixels[offset];
                    const int green = pixels[offset + 1];
                    const int blue = pixels[offset + 2];
                    OA_CHECK(std::abs(blue - int(80 * grey)) <= 1);
                    if (output_size < tile_size) {
                        const int expected = int(128 * (1 - grey) + 80 * grey);
                        OA_CHECK(std::abs(red - expected) <= 1);
                        OA_CHECK(std::abs(green - expected) <= 1);
                    } else {
                        const int low = int(80 * grey);
                        const int high = int(255 * (1 - grey) + 80 * grey);
                        OA_CHECK(std::abs(red - low) <= 1 || std::abs(red - high) <= 1);
                        OA_CHECK(std::abs(green - low) <= 1 || std::abs(green - high) <= 1);
                    }
                }
            }
        }
    }
    for (auto& vertex : vertices) {
        vertex.terrain_known = 0;
        vertex.terrain_grey = 1;
    }
    OA_CHECK(
        render(
            context,
            vertices,
            atlas,
            atlas_size,
            atlas_size,
            1,
            tile_size,
            tile_size,
            classic_projection,
            pixels,
            error
        ) == Result::success
    );
    for (size_t offset = 0; offset + channels <= pixels.size(); offset += channels)
        OA_CHECK(pixels[offset] == 0 && pixels[offset + 1] == 0 && pixels[offset + 2] == 0);

    Scene scene;
    scene.atlas = {47, 127, 193, full_channel};
    OA_CHECK(scene.frame() == Result::success);
    OA_CHECK(scene.pixels.size() >= channels);
    if (scene.pixels.size() >= channels) {
        OA_CHECK(scene.pixels[0] == 47);
        OA_CHECK(scene.pixels[1] == 127);
        OA_CHECK(scene.pixels[2] == 193);
    }
}

/// Draw a kept ground from its own buffer as the frame would draw it whole,
/// uploading it only when its revision changes, and run the caller's work
/// once a frame while the card draws.
void kept_ground_frames() {
    Context kept_context;
    Context plain_context;
    constexpr uint32_t atlas_width = 2;
    // Texel 0 red, texel 1 green.
    const std::vector<uint8_t> atlas{
        full_channel, 0, 0, full_channel, 0, full_channel, 0, full_channel
    };
    std::vector<Vertex> vertices{
        {0, 0, 0, 0, 0},
        {8, 0, 0, 0, 0},
        {0, 0, 8, 0, 0},
        {8, 0, 0, 0, 0},
        {8, 0, 8, 0, 0},
        {0, 0, 8, 0, 0},
        {0, 0, 0, 1, 0},
        {4, 0, 0, 1, 0},
        {0, 0, 4, 1, 0}
    };
    std::vector<uint8_t> kept_pixels;
    std::vector<uint8_t> plain_pixels;
    std::string error;
    int calls{};
    const std::function<void()> count_call = [&calls] { ++calls; };
    const auto kept = [&](const KeptGround& ground) {
        return render(
            kept_context,
            vertices,
            atlas,
            atlas_width,
            1,
            1,
            small_target,
            small_target,
            classic_projection,
            kept_pixels,
            error,
            1,
            nullptr,
            nullptr,
            &ground,
            count_call
        );
    };
    const auto plain = [&] {
        OA_CHECK(
            render(
                plain_context,
                vertices,
                atlas,
                atlas_width,
                1,
                1,
                small_target,
                small_target,
                classic_projection,
                plain_pixels,
                error
            ) == Result::success
        );
    };
    const KeptGround ground{6, 7};
    OA_CHECK(kept(ground) == Result::success);
    OA_CHECK(calls == 1);
    plain();
    OA_CHECK(kept_pixels == plain_pixels);
    // The rest of the frame moves; the ground is held on the card.
    for (auto& vertex : std::span(vertices).subspan(6))
        vertex.x += horizontal_translation;
    OA_CHECK(kept(ground) == Result::success);
    OA_CHECK(calls == 2);
    plain();
    OA_CHECK(kept_pixels == plain_pixels);
    // Ground changed under the same revision is not uploaded: the card
    // draws the ground it holds.
    const auto held = kept_pixels;
    for (auto& vertex : std::span(vertices).first(6))
        vertex.u = 1;
    OA_CHECK(kept(ground) == Result::success);
    OA_CHECK(kept_pixels == held);
    // A new revision uploads it.
    OA_CHECK(kept(KeptGround{6, 8}) == Result::success);
    plain();
    OA_CHECK(kept_pixels == plain_pixels);
    OA_CHECK(kept_pixels != held);
    // Ground that is not kept is drawn with the frame; ground that is the
    // whole frame is drawn alone.
    OA_CHECK(kept(KeptGround{6, 0}) == Result::success);
    OA_CHECK(kept_pixels == plain_pixels);
    OA_CHECK(kept(KeptGround{9, 9}) == Result::success);
    OA_CHECK(kept_pixels == plain_pixels);
    OA_CHECK(calls == 6);
    // The ground must be whole triangles within the frame.
    OA_CHECK(kept(KeptGround{4, 10}) == Result::failed);
    OA_CHECK(kept(KeptGround{12, 10}) == Result::failed);
    OA_CHECK(kept_pixels.empty());
    OA_CHECK(calls == 6);
}

/// Reuse a real device across frames and release it before the SDL video subsystem.
int persistent_frames() {
    Scene scene;
    const auto first = scene.frame();
    if (first == Result::unavailable) {
        std::printf("SKIP: live GPU lifecycle: %s\n", scene.error.c_str());
        return skipped;
    }
    OA_CHECK(first == Result::success);
    if (first != Result::success) {
        std::fprintf(stderr, "live GPU frame: %s\n", scene.error.c_str());
        return oa::test::check_exit_status();
    }
    const auto initial_vertices = scene.vertices;
    for (const auto& bound : vertex_bounds) {
        for (float accepted : {bound.low, bound.high}) {
            scene.vertices[0].*bound.member = accepted;
            OA_CHECK(scene.frame() == Result::success);
        }
        scene.vertices = initial_vertices;
    }
    OA_CHECK(scene.frame() == Result::success);
    atlas_row_updates();
    kept_ground_frames();
    terrain_filtering();
    OA_CHECK(scene.pixels.size() == small_target * small_target * channels);
    OA_CHECK(scene.pixel(0, 0, small_target, full_channel, 0));
    OA_CHECK(scene.pixel(7, 7, small_target, 0, 0));

    scene.atlas = {0, full_channel, 0, full_channel};
    OA_CHECK(scene.frame() == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, full_channel, 0));
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, full_channel));

    OA_CHECK(scene.frame(2, large_target) == Result::success);
    OA_CHECK(scene.pixels.size() == large_target * large_target * channels);
    OA_CHECK(scene.pixel(0, 0, large_target, 0, full_channel));
    scene.projection[3] = float(horizontal_translation);
    OA_CHECK(scene.frame(2, large_target) == Result::success);
    OA_CHECK(scene.pixel(0, 0, large_target, 0, 0));
    OA_CHECK(scene.pixel(horizontal_translation, 0, large_target, 0, full_channel));

    scene.projection[0] = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(scene.frame(2) == Result::failed);
    OA_CHECK(scene.pixels.empty());
    scene.projection = classic_projection;
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, full_channel));
    const auto opaque_triangle = scene.vertices;
    for (auto& vertex : scene.vertices) {
        vertex.brightness = 0;
        vertex.emission_r = 0.5f;
        vertex.emission_g = 1;
        vertex.emission_b = 0.25f;
    }
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.pixels.size() >= channels);
    if (scene.pixels.size() >= channels) {
        OA_CHECK(scene.pixels[0] >= 127 && scene.pixels[0] <= 128);
        OA_CHECK(scene.pixels[1] == full_channel);
        OA_CHECK(scene.pixels[2] >= 63 && scene.pixels[2] <= 64);
    }
    for (auto& vertex : scene.vertices)
        vertex.brightness = 1;
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.pixels.size() > 1 && scene.pixels[1] == full_channel);
    scene.vertices = opaque_triangle;
    for (auto vertex : opaque_triangle) {
        vertex.brightness = 0;
        vertex.opacity = 0.5f;
        scene.vertices.push_back(vertex);
    }
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.pixels.size() > 1 && scene.pixels[1] >= 127 && scene.pixels[1] <= 128);
    scene.vertices = opaque_triangle;
    for (auto& vertex : scene.vertices) {
        vertex.brightness = 0.5f;
        vertex.opacity = 0.5f;
    }
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.pixels.size() > 1 && scene.pixels[1] >= 63 && scene.pixels[1] <= 64);
    scene.vertices = opaque_triangle;
    // Supersampling alone, as on a card without multisampling; the card's
    // own is multisampled_frames'.
    scene.context.multisampling_allowed = false;
    for (const uint32_t factor : {2u, 4u, 8u, 16u}) {
        OA_CHECK(scene.frame(2, small_target, factor) == Result::success);
        OA_CHECK(scene.context.actual_supersample == factor);
        OA_CHECK(scene.context.actual_multisample == 1);
        OA_CHECK(scene.pixels.size() == small_target * small_target * channels);
        OA_CHECK(scene.pixel(0, 0, small_target, 0, full_channel));
        bool partial_coverage = false;
        for (size_t offset = 1; offset < scene.pixels.size(); offset += channels)
            partial_coverage |= scene.pixels[offset] > 0 && scene.pixels[offset] < full_channel;
        OA_CHECK(partial_coverage);
    }
    // Compare every output channel with an independently downloaded high-resolution frame.
    // Translation and varying vertex colours exercise nonuniform samples and edge rounding.
    scene.atlas = {173, 91, 47, full_channel};
    scene.vertices[0].brightness = 0.2f;
    scene.vertices[1].brightness = 0.9f;
    scene.vertices[2].brightness = 0.6f;
    scene.projection[3] = 0.37f;
    scene.projection[7] = 0.61f;
    for (const uint32_t factor : {2u, 4u, 8u, 16u}) {
        OA_CHECK(scene.frame(3, small_target, factor) == Result::success);
        const auto resolved = scene.pixels;
        auto enlarged_projection = scene.projection;
        constexpr size_t screen_rows = 8;
        for (size_t component = 0; component < screen_rows; ++component)
            enlarged_projection[component] *= float(factor);
        const uint32_t enlarged_size = small_target * factor;
        OA_CHECK(
            render(
                scene.context,
                scene.vertices,
                scene.atlas,
                1,
                1,
                3,
                enlarged_size,
                enlarged_size,
                enlarged_projection,
                scene.pixels,
                scene.error
            ) == Result::success
        );
        OA_CHECK(resolved.size() == small_target * small_target * channels);
        OA_CHECK(scene.pixels.size() == enlarged_size * enlarged_size * channels);
        if (resolved.size() != small_target * small_target * channels ||
            scene.pixels.size() != enlarged_size * enlarged_size * channels)
            continue;
        const uint32_t samples = factor * factor;
        for (uint32_t y = 0; y < small_target; ++y) {
            for (uint32_t x = 0; x < small_target; ++x) {
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    uint32_t sum = 0;
                    for (uint32_t dy = 0; dy < factor; ++dy)
                        for (uint32_t dx = 0; dx < factor; ++dx)
                            sum += scene.pixels
                                       [((y * factor + dy) * enlarged_size + x * factor + dx) *
                                            channels +
                                        channel];
                    const int expected = int((sum + samples / 2) / samples);
                    const int actual = resolved[(y * small_target + x) * channels + channel];
                    OA_CHECK(actual >= expected - 1 && actual <= expected + 1);
                }
            }
        }
    }
    scene.vertices = opaque_triangle;
    scene.atlas = {0, full_channel, 0, full_channel};
    scene.projection = classic_projection;
    OA_CHECK(scene.frame(4) == Result::success);
    constexpr uint32_t wide_target = 2048;
    OA_CHECK(
        render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            2,
            wide_target,
            1,
            scene.projection,
            scene.pixels,
            scene.error,
            16
        ) == Result::success
    );
    OA_CHECK(scene.context.actual_supersample == 2);
    OA_CHECK(scene.pixels.size() == wide_target * channels);
    OA_CHECK(scene.frame(2) == Result::success);
    OA_CHECK(scene.context.actual_supersample == 1);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, full_channel));
    scene.atlas[3] = 0;
    OA_CHECK(scene.frame(5) == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, 0));
    return oa::test::check_exit_status();
}

/// Where a multisampled pixel's four samples lie in it, in eighths of the
/// pixel: the standard places, in the order of the coverage mask's bits.
constexpr std::array<std::array<uint32_t, 2>, 4> sample_eighths{{{3, 1}, {7, 3}, {1, 5}, {5, 7}}};

/// Compares a multisampled frame with the same vertices drawn without
/// anti-aliasing at eight times the size for each supersampled pixel and
/// half a pixel over, so that its pixels' centres fall on the samples: each
/// output channel must be the mean of its samples there, within tolerance.
/// @return whether the frames compare
bool matches_samples(
    Context& context,
    std::span<const Vertex> vertices,
    std::span<const uint8_t> atlas,
    uint32_t atlas_width,
    const std::array<float, 12>& projection,
    std::span<const uint8_t> multisampled,
    uint32_t supersample,
    int tolerance
) {
    const uint32_t enlargement = 8 * supersample;
    auto enlarged_projection = projection;
    constexpr size_t screen_rows = 8;
    for (size_t component = 0; component < screen_rows; ++component)
        enlarged_projection[component] *= float(enlargement);
    enlarged_projection[3] += 0.5f;
    enlarged_projection[7] += 0.5f;
    const uint32_t enlarged_size = small_target * enlargement;
    std::vector<uint8_t> enlarged;
    std::string error;
    if (render(
            context,
            vertices,
            atlas,
            atlas_width,
            1,
            7,
            enlarged_size,
            enlarged_size,
            enlarged_projection,
            enlarged,
            error
        ) != Result::success ||
        multisampled.size() != small_target * small_target * channels ||
        enlarged.size() != size_t(enlarged_size) * enlarged_size * channels)
        return false;
    bool matches = true;
    const uint32_t samples = supersample * supersample * uint32_t(sample_eighths.size());
    for (uint32_t y = 0; y < small_target; ++y) {
        for (uint32_t x = 0; x < small_target; ++x) {
            for (uint32_t channel = 0; channel < channels; ++channel) {
                uint32_t sum = 0;
                for (uint32_t row = 0; row < supersample; ++row)
                    for (uint32_t column = 0; column < supersample; ++column)
                        for (const auto& [across, down] : sample_eighths) {
                            const uint32_t enlarged_x = x * enlargement + column * 8 + across;
                            const uint32_t enlarged_y = y * enlargement + row * 8 + down;
                            sum += enlarged
                                [(size_t(enlarged_y) * enlarged_size + enlarged_x) * channels +
                                 channel];
                        }
                const int expected = int((sum + samples / 2) / samples);
                const int actual = multisampled[(y * small_target + x) * channels + channel];
                matches &= actual >= expected - tolerance && actual <= expected + tolerance;
            }
        }
    }
    return matches;
}

/// Take a 2x2 of the samples asked for as the card's 4x multisampling where
/// the card has it: a triangle's edges and a keyed edge across a triangle
/// are smoothed at the standard sample places, a frame supersampled beyond
/// that is reduced from the averaged samples, and a frame too wide to
/// supersample is still multisampled.
void multisampled_frames() {
    Scene scene;
    scene.atlas = {173, 91, 47, full_channel};
    scene.projection[3] = 0.37f;
    scene.projection[7] = 0.61f;
    OA_CHECK(scene.frame(1, small_target, 2) == Result::success);
    if (scene.context.actual_multisample != 4) {
        // A card without it supersamples, as multisampling_allowed false does.
        std::printf("live GPU: this card does not multisample 4 times; supersampled instead\n");
        OA_CHECK(scene.context.actual_multisample == 1 && scene.context.actual_supersample == 2);
        return;
    }
    OA_CHECK(scene.context.actual_supersample == 1);
    bool partial_coverage = false;
    for (size_t offset = 0; offset < scene.pixels.size(); offset += channels)
        partial_coverage |= scene.pixels[offset] > 0 && scene.pixels[offset] < 173;
    OA_CHECK(partial_coverage);
    OA_CHECK(matches_samples(
        scene.context, scene.vertices, scene.atlas, 1, scene.projection, scene.pixels, 1, 1
    ));
    // Asked for 4x4, the card multisamples a supersampled frame of half that.
    OA_CHECK(scene.frame(1, small_target, 4) == Result::success);
    OA_CHECK(scene.context.actual_multisample == 4 && scene.context.actual_supersample == 2);
    OA_CHECK(matches_samples(
        scene.context, scene.vertices, scene.atlas, 1, scene.projection, scene.pixels, 2, 2
    ));

    // A square across the frame whose texture is keyed clear past its middle
    // along a diagonal, inside both triangles: its edge is smoothed as a
    // triangle's is.
    const std::vector<uint8_t> keyed_atlas{0, full_channel, 0, full_channel, 0, 0, 0, 0};
    std::vector<Vertex> square{
        {0, 0, 0, 0, 0.5f},
        {8, 0, 0, 0.5f, 0.5f},
        {8, 0, 8, 1, 0.5f},
        {0, 0, 0, 0, 0.5f},
        {8, 0, 8, 1, 0.5f},
        {0, 0, 8, 0.5f, 0.5f}
    };
    std::vector<uint8_t> keyed;
    OA_CHECK(
        render(
            scene.context,
            square,
            keyed_atlas,
            2,
            1,
            2,
            small_target,
            small_target,
            scene.projection,
            keyed,
            scene.error,
            2
        ) == Result::success
    );
    bool keyed_partial = false;
    for (size_t offset = 1; offset < keyed.size(); offset += channels)
        keyed_partial |= keyed[offset] > 0 && keyed[offset] < full_channel;
    OA_CHECK(keyed_partial);
    OA_CHECK(matches_samples(scene.context, square, keyed_atlas, 2, scene.projection, keyed, 1, 1));

    // Too wide to draw at twice its size, a frame is still multisampled.
    constexpr uint32_t widest = 4096;
    OA_CHECK(
        render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            1,
            widest,
            1,
            scene.projection,
            scene.pixels,
            scene.error,
            2
        ) == Result::success
    );
    OA_CHECK(scene.context.actual_multisample == 4 && scene.context.actual_supersample == 1);
    OA_CHECK(scene.pixels.size() == widest * channels);
    // One with more pixels than a fourth of the largest supersampled target
    // is not, as it is not supersampled.
    OA_CHECK(
        render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            1,
            widest,
            widest / 4 + 1,
            scene.projection,
            scene.pixels,
            scene.error,
            2
        ) == Result::success
    );
    OA_CHECK(scene.context.actual_multisample == 1 && scene.context.actual_supersample == 1);
    scene.context.multisampling_allowed = false;
    OA_CHECK(scene.frame(1, small_target, 2) == Result::success);
    OA_CHECK(scene.context.actual_multisample == 1 && scene.context.actual_supersample == 2);
    // Off draws one sample a pixel whatever the card can.
    scene.context.multisampling_allowed = true;
    OA_CHECK(scene.frame(1, small_target, 1) == Result::success);
    OA_CHECK(scene.context.actual_multisample == 1 && scene.context.actual_supersample == 1);
}

/// Water animation remains deterministic, optional and unable to illuminate unknown ground.
void water_frames() {
    Scene scene;
    WaterEffects water{1, 1, 1, 1, 8, 0, 1};
    water.wind.strength = 1;
    const auto frame = [&] {
        return render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            1,
            small_target,
            small_target,
            scene.projection,
            scene.pixels,
            scene.error,
            1,
            nullptr,
            &water
        );
    };
    for (auto& vertex : scene.vertices) {
        vertex.terrain_size = -2;
        vertex.terrain_x = 2;
        vertex.terrain_y = 1;
    }
    OA_CHECK(frame() == Result::success);
    const auto first = scene.pixels;
    OA_CHECK(frame() == Result::success);
    OA_CHECK(first == scene.pixels);
    water.seconds = 1.25F;
    water.wind.drift_x = 12;
    OA_CHECK(frame() == Result::success);
    OA_CHECK(first != scene.pixels);
    for (auto& vertex : scene.vertices)
        vertex.brightness = 0;
    OA_CHECK(frame() == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, 0));
    water.detail = 0;
    for (auto& vertex : scene.vertices)
        vertex.brightness = 1;
    OA_CHECK(frame() == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, 0));
    water.detail = 1;
    water.ripples = water.coastal_waves = 0;
    OA_CHECK(frame() == Result::success);
    OA_CHECK(scene.pixel(0, 0, small_target, 0, 0));
    for (auto& vertex : scene.vertices) {
        vertex.terrain_size = 0;
        vertex.terrain_x = 0;
    }
    water.shimmer = 0;
    OA_CHECK(frame() == Result::success);
    const auto unshimmered = scene.pixels;
    water.shimmer = 1;
    OA_CHECK(frame() == Result::success);
    OA_CHECK(unshimmered != scene.pixels);
    water.seconds = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(frame() == Result::failed);
    OA_CHECK(scene.pixels.empty());
}

/// Water ripples move the ground's own art under the water and lay no
/// colour of their own over it: every pixel keeps a colour of the art, dry
/// ground stays, the art travels with the map's flow, and calm water,
/// ripples at 0% and a distant view stay as drawn.
void ripple_art_motion() {
    Context context;
    constexpr uint32_t atlas_size = 64;
    constexpr uint32_t size = 128;
    constexpr uint32_t cell = 16;
    constexpr float sea = 20;
    // Stripes three texels wide across the whole atlas, its levels too.
    constexpr std::array<int, 3> dark{20, 40, 60};
    constexpr std::array<int, 3> bright{200, 180, 160};
    std::vector<uint8_t> atlas(atlas_size * atlas_size * channels, full_channel);
    for (uint32_t y = 0; y < atlas_size; ++y)
        for (uint32_t x = 0; x < atlas_size; ++x) {
            const auto& colour = (x / 3) % 2 != 0 ? bright : dark;
            for (uint32_t channel = 0; channel < 3; ++channel)
                atlas[(size_t(y) * atlas_size + x) * channels + channel] =
                    static_cast<uint8_t>(colour[channel]);
        }
    // Ground cells of half a tile each: under the sea left of x 64, dry
    // ground above it from there on.
    std::vector<Vertex> vertices;
    for (uint32_t row = 0; row < size / cell; ++row)
        for (uint32_t column = 0; column < size / cell; ++column) {
            const float height = column < 4 ? 0.0F : 30.0F;
            const float x = float(column * cell), z = float(row * cell);
            const float u = float((column % 2) * cell), v = float((row % 2) * cell);
            const auto corner = [&](float dx, float dz) {
                Vertex vertex{x + dx, height, z + dz, (u + dx) / atlas_size, (v + dz) / atlas_size};
                vertex.terrain_size = 32;
                return vertex;
            };
            const std::array corners{
                corner(0, 0), corner(cell, 0), corner(0, cell), corner(cell, cell)
            };
            vertices.insert(
                vertices.end(),
                {corners[0], corners[1], corners[2], corners[1], corners[3], corners[2]}
            );
        }
    WaterEffects water{};
    water.sea_level = sea;
    water.wind.strength = 1;
    std::vector<uint8_t> pixels;
    std::string error;
    const auto frame = [&] {
        OA_CHECK(
            render(
                context,
                vertices,
                atlas,
                atlas_size,
                atlas_size,
                1,
                size,
                size,
                classic_projection,
                pixels,
                error,
                1,
                nullptr,
                &water
            ) == Result::success
        );
        return pixels;
    };
    const auto still = frame();
    water.ripples = 1;
    const auto moved = frame();
    std::size_t changed{};
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            const size_t offset = (size_t(y) * size + x) * channels;
            bool differs = false;
            for (uint32_t channel = 0; channel < 3; ++channel)
                differs = differs || moved[offset + channel] != still[offset + channel];
            // Right of the wet ground's moved edge the dry ground stays.
            if (x >= 72)
                OA_CHECK(!differs);
            changed += differs;
            // Every pixel blends the art's two colours alone: no pale sheet.
            // The ground's own edge moves off the picture's, so it is left out.
            if (x >= 8 && x < 56 && y >= 8 && y < size - 8) {
                std::array<float, 3> share{};
                for (uint32_t channel = 0; channel < 3; ++channel)
                    share[channel] = float(moved[offset + channel] - dark[channel]) /
                                     float(bright[channel] - dark[channel]);
                OA_CHECK(std::abs(share[0] - share[1]) < 0.04F);
                OA_CHECK(std::abs(share[0] - share[2]) < 0.04F);
            }
        }
    OA_CHECK(changed >= size * 64 / 8);
    // The art travels downstream as the flow's travel grows, and turns with it.
    water.wind.drift_x = 18;
    const auto travelled = frame();
    OA_CHECK(travelled != moved);
    water.wind.drift_x = 0;
    water.wind.x = 0;
    water.wind.z = 1;
    OA_CHECK(frame() != moved);
    water.wind.x = 1;
    water.wind.z = 0;
    // The fine ripples move it further up close.
    water.fine_detail = 1;
    OA_CHECK(frame() != moved);
    water.fine_detail = 0;
    // Calm water, a distant view and 0% leave the art as drawn.
    water.wind.strength = 0;
    OA_CHECK(frame() == still);
    water.wind.strength = 1;
    water.detail = 0;
    OA_CHECK(frame() == still);
    water.detail = 1;
    water.ripples = 0;
    OA_CHECK(frame() == still);
}

/// Coastal waves' foam on the water's sheet shows plainly at 100%, as a
/// gentle wash at 30% and up to its cap at 200%, and fades out with distance.
void coastal_foam_strength() {
    Scene scene;
    for (auto& vertex : scene.vertices) {
        vertex.height = 0.05F;
        vertex.terrain_size = -2;
        vertex.terrain_x = 2;
        vertex.terrain_y = 1;
        // The sheet's vertices carry the foam's colour; its plain one here.
        vertex.red = oa::app::renderer_water_appearance::foam_red;
        vertex.green = oa::app::renderer_water_appearance::foam_green;
        vertex.blue = oa::app::renderer_water_appearance::foam_blue;
    }
    WaterEffects water{};
    water.wind.strength = 1;
    // A crest over water 2 map pixels deep.
    water.seconds = 5.2117F;
    const auto red = [&](float coast, float detail) {
        water.coastal_waves = coast;
        water.detail = detail;
        OA_CHECK(
            render(
                scene.context,
                scene.vertices,
                scene.atlas,
                1,
                1,
                1,
                small_target,
                small_target,
                scene.projection,
                scene.pixels,
                scene.error,
                1,
                nullptr,
                &water
            ) == Result::success
        );
        return scene.pixels.size() >= channels ? int{scene.pixels[0]} : -1;
    };
    const int gentle = red(0.3F, 1), plain = red(1, 1), strong = red(2, 1);
    OA_CHECK(gentle >= 40 && gentle <= 90);
    OA_CHECK(plain >= 150);
    OA_CHECK(strong >= plain && strong <= 200);
    OA_CHECK(red(1, 0.5F) < plain && red(1, 0.5F) >= plain / 2 - 2);
    OA_CHECK(red(1, 0) == 0);
}

/// Seabed shimmer stays below a subtle contrast budget over the map's baked artwork.
void terrain_shimmer_contrast() {
    Context context;
    constexpr uint32_t atlas_size = 64;
    constexpr uint32_t output_size = 32;
    std::vector<uint8_t> atlas(atlas_size * atlas_size * channels, 160);
    for (std::size_t offset = 3; offset < atlas.size(); offset += channels)
        atlas[offset] = full_channel;
    std::array<Vertex, 6> vertices{
        Vertex{0, 0, 0, 0, 0},
        Vertex{32, 0, 0, .5F, 0},
        Vertex{0, 0, 32, 0, .5F},
        Vertex{32, 0, 0, .5F, 0},
        Vertex{32, 0, 32, .5F, .5F},
        Vertex{0, 0, 32, 0, .5F}
    };
    for (auto& vertex : vertices)
        vertex.terrain_size = 32;
    WaterEffects water{0, 0, 1, 0, 12, 1.25F, 1};
    std::vector<uint8_t> pixels;
    std::string error;
    OA_CHECK(
        render(
            context,
            vertices,
            atlas,
            atlas_size,
            atlas_size,
            1,
            output_size,
            output_size,
            classic_projection,
            pixels,
            error,
            1,
            nullptr,
            &water
        ) == Result::success
    );
    bool changed{};
    for (std::size_t offset = 0; offset < pixels.size(); offset += channels) {
        OA_CHECK(pixels[offset] >= 158 && pixels[offset] <= 168);
        changed |= pixels[offset] != 160;
    }
    OA_CHECK(changed);
}

/// Every submerged face is tinted at either viewing direction; dry faces retain their palette.
void submerged_faces() {
    Scene scene;
    scene.atlas = {160, 160, 160, full_channel};
    scene.vertices.clear();
    const auto quad = [&](float x, float bottom, float top, float z) {
        const Vertex a{x, bottom, z};
        const Vertex b{x + 8, bottom, z};
        const Vertex c{x + 8, top, z};
        const Vertex d{x, top, z};
        scene.vertices.insert(scene.vertices.end(), {a, b, c, a, c, d});
    };
    quad(0, 0, 4, 8);
    quad(0, 4, 8, 8);
    quad(8, 20, 28, 20);
    WaterEffects water{0, 0, 0, 0, 16, 1.25F, 1};
    const auto frame = [&] {
        return render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            1,
            large_target,
            large_target,
            scene.projection,
            scene.pixels,
            scene.error,
            1,
            nullptr,
            &water
        );
    };
    for (const float facing : {1.0F, -1.0F}) {
        scene.projection = classic_projection;
        scene.projection[0] = facing;
        scene.projection[3] = facing < 0 ? static_cast<float>(large_target) : 0.0F;
        water.shimmer = 0;
        OA_CHECK(frame() == Result::success);
        const auto ordinary = scene.pixels;
        water.shimmer = 1;
        OA_CHECK(frame() == Result::success);
        const uint32_t wet_x = facing > 0 ? 3 : 12;
        const uint32_t dry_x = facing > 0 ? 12 : 3;
        for (const uint32_t row : {5U, 7U}) {
            const auto offset = (row * large_target + wet_x) * channels;
            OA_CHECK(ordinary[offset] == 160);
            OA_CHECK(scene.pixels[offset] < scene.pixels[offset + 2]);
        }
        const auto dry_offset = (8 * large_target + dry_x) * channels;
        OA_CHECK(ordinary[dry_offset] == 160);
        OA_CHECK(scene.pixels[dry_offset] == ordinary[dry_offset]);
    }
}

/// Crossing faces retain exactly their dry pixels and show a submerged contact on every piece.
void crossing_waterline() {
    Scene scene;
    scene.atlas = {160, 160, 160, full_channel};
    scene.vertices.clear();
    for (float x : {0.0F, 4.0F}) {
        const Vertex a{x, 0, 16}, b{x + 4, 0, 16};
        const Vertex c{x + 4, 32, 16}, d{x, 32, 16};
        scene.vertices.insert(scene.vertices.end(), {a, b, c, a, c, d});
    }
    WaterEffects water{0, 0, 0, 0, 16, 1.25F, 1};
    const auto frame = [&] {
        return render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            1,
            large_target,
            large_target,
            scene.projection,
            scene.pixels,
            scene.error,
            1,
            nullptr,
            &water
        );
    };
    for (float facing : {1.0F, -1.0F}) {
        scene.projection = classic_projection;
        scene.projection[0] = facing;
        scene.projection[3] = facing < 0 ? static_cast<float>(large_target) : 0.0F;
        water.shimmer = 0;
        OA_CHECK(frame() == Result::success);
        const auto baseline = scene.pixels;
        water.shimmer = 1;
        OA_CHECK(frame() == Result::success);
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < large_target; ++x)
                for (uint32_t channel = 0; channel < channels; ++channel)
                    OA_CHECK(
                        scene.pixels[(y * large_target + x) * channels + channel] ==
                        baseline[(y * large_target + x) * channels + channel]
                    );
        for (uint32_t x : {1U, 5U}) {
            const uint32_t column = facing > 0 ? x : large_target - 1 - x;
            for (uint32_t y : {8U, 13U}) {
                const auto offset = (y * large_target + column) * channels;
                OA_CHECK(baseline[offset] == 160);
                OA_CHECK(scene.pixels[offset] < scene.pixels[offset + 2]);
            }
        }
    }
}

/// Palette-remapped model corners multiply RGB without changing alpha coverage.
void material_modulation() {
    Scene scene;
    scene.atlas = {255, 255, 255, 255};
    for (auto& vertex : scene.vertices) {
        vertex.red = 0.25F;
        vertex.green = 0.5F;
        vertex.blue = 0.75F;
    }
    OA_CHECK(scene.frame() == Result::success);
    OA_CHECK(std::abs(int(scene.pixels[0]) - 64) <= 1);
    OA_CHECK(std::abs(int(scene.pixels[1]) - 128) <= 1);
    OA_CHECK(std::abs(int(scene.pixels[2]) - 191) <= 1);
    OA_CHECK(scene.pixels[3] == 255);
}

/// Tall underwater optical sprites use event depth even where the card crosses the surface.
void underwater_billboard_depth() {
    Scene scene;
    scene.atlas = {160, 160, 160, 255};
    scene.vertices.clear();
    for (float x : {0.0F, 8.0F}) {
        Vertex a{x, 0, 16}, b{x + 8, 0, 16};
        Vertex c{x + 8, 32, 16}, d{x, 32, 16};
        for (auto* vertex : {&a, &b, &c, &d}) {
            vertex->terrain_size = x == 0 ? -5.0F : -1.0F;
            vertex->terrain_x = x == 0 ? 8.0F : 0.0F;
        }
        scene.vertices.insert(scene.vertices.end(), {a, b, c, a, c, d});
    }
    WaterEffects water{0, 0, 0, 0, 16, 1.25F, 1};
    const auto frame = [&] {
        return render(
            scene.context,
            scene.vertices,
            scene.atlas,
            1,
            1,
            1,
            large_target,
            large_target,
            scene.projection,
            scene.pixels,
            scene.error,
            1,
            nullptr,
            &water
        );
    };
    OA_CHECK(frame() == Result::success);
    const auto baseline = scene.pixels;
    water.shimmer = 1;
    OA_CHECK(frame() == Result::success);
    for (uint32_t row : {1U, 7U, 12U}) {
        const auto offset = (row * large_target + 3) * channels;
        OA_CHECK(scene.pixels[offset] < scene.pixels[offset + 2]);
        OA_CHECK(scene.pixels[offset] < baseline[offset]);
    }
    for (uint32_t row = 0; row < 8; ++row)
        for (uint32_t column = 8; column < large_target; ++column)
            for (uint32_t channel = 0; channel < channels; ++channel) {
                const auto offset = (row * large_target + column) * channels + channel;
                OA_CHECK(scene.pixels[offset] == baseline[offset]);
            }
}

/// The light scene's atlas: four texels of ground in two colours, a wall's,
/// a clear one, a face's and white.
constexpr uint32_t light_atlas_width = 8;

/// A light scene's quad: x from left to right and z from near to far at a
/// height, or upright at z from the ground to a height; its atlas columns
/// run from first_texel at its left to last_texel at its right.
struct LightQuad {
    float left{};
    float right{};
    float near{};
    float far{};
    float height{};
    bool upright{};
    float first_texel{};
    float last_texel{};
    float brightness{1};
    float opacity{1};
};

/// The light scene's triangles: striped ground with a fogged strip and an
/// unseen one, a wall standing on it, and faces just above it that are
/// translucent, half opaque (opaque to the lights) and keyed clear.
std::vector<Vertex> light_scene() {
    const std::array<LightQuad, 7> quads{
        {{0, 48, 0, 36, 0, false, 0, 4},
         {0, 48, 36, 42, 0, false, 0, 4, 0.35F},
         {0, 48, 42, 48, 0, false, 0, 4, 0},
         {10, 22, 24, 24, 12, true, 4.5F, 4.5F},
         {28, 40, 6, 18, 2, false, 6.5F, 6.5F, 1, 0.4F},
         {4, 8, 30, 36, 2, false, 6.5F, 6.5F, 0.5F, 0.6F},
         {28, 40, 26, 34, 2, false, 5.5F, 5.5F}}
    };
    std::vector<Vertex> vertices;
    for (const auto& quad : quads) {
        const auto corner = [&](float x, float along) {
            Vertex vertex{};
            vertex.x = x;
            vertex.height = quad.upright ? along : quad.height;
            vertex.z = quad.upright ? quad.near : along;
            vertex.u = (quad.first_texel + (x - quad.left) / (quad.right - quad.left) *
                                               (quad.last_texel - quad.first_texel)) /
                       float(light_atlas_width);
            vertex.v = 0.5F;
            vertex.brightness = quad.brightness;
            vertex.opacity = quad.opacity;
            return vertex;
        };
        const float low = quad.upright ? 0 : quad.near;
        const float high = quad.upright ? quad.height : quad.far;
        const Vertex a = corner(quad.left, low), b = corner(quad.right, low);
        const Vertex c = corner(quad.right, high), d = corner(quad.left, high);
        vertices.insert(vertices.end(), {a, b, d, b, c, d});
    }
    return vertices;
}

/// The surface the processor's raster finds under each pixel's centre: the
/// nearest face whose opacity, and its texel's alpha times that, reach one
/// half, with its depth and visibility, as renderer_effects::Surface takes
/// them.
void reference_surfaces(
    std::span<const Vertex> vertices,
    std::span<const uint8_t> atlas,
    uint32_t atlas_width,
    uint32_t size,
    const std::array<float, 12>& m,
    std::vector<uint8_t>& visibility,
    std::vector<float>& depths
) {
    visibility.assign(std::size_t(size) * size, 0);
    depths.assign(std::size_t(size) * size, -std::numeric_limits<float>::infinity());
    for (std::size_t first = 0; first + 2 < vertices.size(); first += 3) {
        std::array<std::array<float, 3>, 3> triangle{};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& v = vertices[first + corner];
            triangle[corner] = {
                m[0] * v.x + m[1] * v.height + m[2] * v.z + m[3],
                m[4] * v.x + m[5] * v.height + m[6] * v.z + m[7],
                m[8] * v.x + m[9] * v.height + m[10] * v.z + m[11]
            };
        }
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                std::array<float, 3> weights{};
                float depth{};
                const auto pixel = std::size_t(y) * size + x;
                if (!oa::app::renderer_live_camera::triangle_hit(
                        triangle, float(x) + 0.5F, float(y) + 0.5F, weights, depth
                    ) ||
                    depth <= depths[pixel])
                    continue;
                float u{}, opacity{}, brightness{};
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    u += vertices[first + corner].u * weights[corner];
                    opacity += vertices[first + corner].opacity * weights[corner];
                    brightness += vertices[first + corner].brightness * weights[corner];
                }
                const auto column =
                    uint32_t(std::clamp(u * float(atlas_width), 0.0F, float(atlas_width - 1)));
                const float alpha = atlas[column * channels + 3];
                if (opacity * 255 < 128 || alpha * opacity < 128)
                    continue;
                depths[pixel] = depth;
                visibility[pixel] = uint8_t(std::clamp(brightness, 0.0F, 1.0F) * 255);
            }
    }
}

/// The card adds lights as renderer_effects::compose() adds them to the
/// picture the card drew without them, each pixel's surface the processor's
/// raster would find: light falls off with depth, stops at what stands in
/// front, passes translucent and keyed faces, lights a half-opaque face and
/// is lessened by fog; heat moves pixels within a surface. So drawn with one
/// sample a pixel, multisampled, supersampled, and multisampled on a
/// supersampled target, whose surfaces' edges here lie between pixels. The
/// first 128 lights alone are drawn, and a card not allowed leaves them to
/// the caller.
void card_lights() {
    constexpr uint32_t size = 48;
    constexpr uint32_t atlas_width = light_atlas_width;
    const std::vector<uint8_t> atlas{90,  80,  70,  255, 150, 120, 80,  255, 90,  80,  70,
                                     255, 150, 120, 80,  255, 60,  90,  120, 255, 200, 200,
                                     200, 0,   150, 60,  60,  255, 255, 255, 255, 255};
    const auto vertices = light_scene();
    std::vector<uint8_t> visibility;
    std::vector<float> depths;
    reference_surfaces(vertices, atlas, atlas_width, size, classic_projection, visibility, depths);
    using oa::app::renderer_effects::Source;
    std::vector<Source> sources{
        // At the wall's foot, its depth the ground's there.
        {16, 22, 11, 14, 8, 1, 0.6F, 0.2F, 1.5F, 1, 0, 0},
        // Over the translucent face.
        {34, 12, 6, 10, 6, 0.3F, 0.6F, 1, 2, 0, 0, 0},
        // Over the fogged and unseen ground and the keyed face.
        {24, 38, 19, 12, 8, 1, 1, 0.5F, 1, 1, 0, 0},
        // On the half-opaque face, and past the picture's corner.
        {6, 33, 18, 7, 6, 0.5F, 1, 0.5F, 2, 2, 0, 0},
        {46, 4, 2, 9, 8, 1, 0.2F, 0.2F, 2, 0, 0, 0, 12}
    };
    Context context;
    std::vector<uint8_t> pixels;
    std::string error;
    const auto frame = [&](const Lights* lights, uint32_t supersample) {
        OA_CHECK(
            render(
                context,
                vertices,
                atlas,
                atlas_width,
                1,
                1,
                size,
                size,
                classic_projection,
                pixels,
                error,
                supersample,
                nullptr,
                nullptr,
                nullptr,
                {},
                lights
            ) == Result::success
        );
        return pixels;
    };
    // The pixels whose channels differ by more than some steps: the card's
    // arithmetic may round each light's step the other way.
    const auto beyond_steps = [](const std::vector<uint8_t>& a,
                                 const std::vector<uint8_t>& b,
                                 int steps) {
        if (a.size() != b.size())
            return std::size_t(a.size() + b.size());
        std::size_t count = 0;
        for (std::size_t offset = 0; offset < a.size(); offset += channels) {
            bool far_off = false;
            for (std::size_t channel = 0; channel < channels; ++channel)
                far_off |= std::abs(int(a[offset + channel]) - int(b[offset + channel])) > steps;
            count += far_off ? 1 : 0;
        }
        return count;
    };
    const auto beyond_a_step = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        return beyond_steps(a, b, 1);
    };
    const auto composed = [&](const std::vector<uint8_t>& plain,
                              const Lights& lights,
                              std::span<const uint8_t> pixel_visibility,
                              std::span<const float> pixel_depths) {
        auto expected = plain;
        oa::app::renderer_effects::compose(
            {expected,
             int(size),
             int(size),
             int(size * channels),
             int(channels),
             pixel_visibility,
             pixel_depths},
            lights.sources,
            lights.settings,
            lights.simulation_seconds
        );
        return expected;
    };

    struct Sampling {
        uint32_t requested{};
        bool multisampling{};
        uint32_t supersample{}; ///< drawn
        uint32_t multisample{}; ///< drawn
    };

    for (const auto& sampling :
         {Sampling{1, true, 1, 1},
          Sampling{2, true, 1, 4},
          Sampling{2, false, 2, 1},
          Sampling{4, true, 2, 4}}) {
        context.multisampling_allowed = sampling.multisampling;
        const uint32_t supersample = sampling.requested;
        const auto plain = frame(nullptr, supersample);
        OA_CHECK(!context.lights_drawn);
        const Lights lights{sources, {true, true, false}, 2.5};
        const auto lit = frame(&lights, supersample);
        OA_CHECK(context.lights_drawn);
        // A card without multisampling supersamples instead.
        OA_CHECK(
            context.actual_supersample == sampling.supersample || context.actual_multisample == 1
        );
        OA_CHECK(
            context.actual_multisample == sampling.multisample || context.actual_multisample == 1
        );
        const auto expected = composed(plain, lights, visibility, depths);
        OA_CHECK(expected != plain);
        OA_CHECK(beyond_a_step(lit, expected) == 0);
        // The ground before the wall lit, the wall's top, nearer than the
        // light's depth reach, left alone, the ground under the translucent
        // face lit, the fogged ground lit less and the unseen not at all.
        const auto at = [&](uint32_t x, uint32_t y) {
            return std::size_t(y * size + x) * channels;
        };
        OA_CHECK(lit[at(16, 26)] > plain[at(16, 26)] + 20);
        OA_CHECK(lit[at(16, 18)] == plain[at(16, 18)]);
        OA_CHECK(lit[at(34, 12) + 2] > plain[at(34, 12) + 2] + 20);
        OA_CHECK(lit[at(24, 39)] > plain[at(24, 39)]);
        OA_CHECK(lit[at(24, 45)] == plain[at(24, 45)]);
        // Heat moves the picture's pixels along their rows, as the processor's.
        auto heated_sources = sources;
        heated_sources[0].heat = 1;
        heated_sources[3].heat = 2;
        heated_sources[3].phase = 1.25F;
        const Lights heated{heated_sources, {true, true, true}, 7.75};
        const auto shimmer = frame(&heated, supersample);
        const auto expected_shimmer = composed(plain, heated, visibility, depths);
        OA_CHECK(expected_shimmer != expected);
        OA_CHECK(beyond_a_step(shimmer, expected_shimmer) <= 2);
    }
    context.multisampling_allowed = true;
    // The first 128 lights alone are drawn; a pixel takes up to a dozen of
    // them, each of whose steps may round the other way.
    std::vector<Source> many;
    for (uint32_t light = 0; light < oa::app::renderer_effects::max_sources + 2; ++light)
        many.push_back(
            {float(light % 40) + 4, float(light / 40) * 6 + 4, 2, 3, 6, 1, 1, 1, 1, 0, 0, 0}
        );
    many.back() = {30, 30, 15.25F, 4, 6, 1, 1, 1, 2, 0, 0, 0};
    const auto plain = frame(nullptr, 1);
    const Lights crowded{many, {true, false, false}, 0};
    const auto crowded_frame = frame(&crowded, 1);
    OA_CHECK(beyond_steps(crowded_frame, composed(plain, crowded, visibility, depths), 12) == 0);
    const auto beyond_the_first = std::size_t(30 * size + 30) * channels;
    OA_CHECK(crowded_frame[beyond_the_first] == plain[beyond_the_first]);
    // Lights that reach no pixel leave the picture as drawn.
    const std::vector<Source> away{{-30, 10, 0, 5, 5, 1, 1, 1, 1, 1, 1, 0}};
    const Lights none_in_view{away, {true, true, true}, 0};
    OA_CHECK(frame(&none_in_view, 1) == plain);
    OA_CHECK(context.lights_drawn);
    // A card not allowed to add them leaves them to the caller.
    context.lights_allowed = false;
    const Lights lights{sources, {true, true, false}, 2.5};
    OA_CHECK(frame(&lights, 1) == plain);
    OA_CHECK(!context.lights_drawn);
}

/// An overlay canvas's colours as compose_overlay() takes them, ARGB8888
/// words, 0 where the canvas holds its key, and its bands with an opaque
/// pixel.
void overlay_words(
    const std::vector<uint8_t>& canvas,
    const std::array<uint8_t, 3>& key,
    uint32_t band_rows,
    uint32_t width,
    std::vector<uint8_t>& words,
    std::vector<uint8_t>& bands
) {
    const std::size_t pixels = canvas.size() / 3;
    const uint32_t height = uint32_t(pixels / width);
    words.assign(pixels * channels, 0);
    bands.assign((height + band_rows - 1) / band_rows, 0);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const uint8_t* at = canvas.data() + pixel * 3;
        if (std::equal(key.begin(), key.end(), at))
            continue;
        words[pixel * channels] = at[2];
        words[pixel * channels + 1] = at[1];
        words[pixel * channels + 2] = at[0];
        words[pixel * channels + 3] = full_channel;
        bands[pixel / width / band_rows] = 1;
    }
}

/// A picture left on the card is the picture read back, and the overlay the
/// card adds to it, then the display gamma, is what the processor adds
/// (renderer_live_overlay::compose): the canvas's own pixels, blends and
/// lines of text reduced to the palette, at every way of drawing, with and
/// without the lights. Frames left on the card one after another, nothing
/// waited for, each hold their own picture; a band the canvas painted and no
/// longer paints shows the picture again. A frame whose lights the caller
/// adds is read back; a context drawing on another's device leaves the
/// device to it.
void picture_on_card() {
    namespace overlay = oa::app::renderer_live_overlay;
    constexpr uint32_t size = 48;
    constexpr uint32_t band_rows = 4;
    const std::vector<uint8_t> atlas{90,  80,  70,  255, 150, 120, 80,  255, 90,  80,  70,
                                     255, 150, 120, 80,  255, 60,  90,  120, 255, 200, 200,
                                     200, 0,   150, 60,  60,  255, 255, 255, 255, 255};
    auto vertices = light_scene();
    using oa::app::renderer_effects::Source;
    const std::vector<Source> sources{
        {16, 22, 11, 14, 8, 1, 0.6F, 0.2F, 1.5F, 1, 0, 0},
        {24, 38, 19, 12, 8, 1, 1, 0.5F, 1, 1, 0, 0}
    };
    const Lights lights{sources, {true, true, false}, 2.5};
    Context context;
    std::vector<uint8_t> pixels;
    std::string error;
    const auto frame = [&](bool keep, const Lights* given, uint32_t supersample) {
        context.keep_on_card = keep;
        const auto result = render(
            context,
            vertices,
            atlas,
            light_atlas_width,
            1,
            1,
            size,
            size,
            classic_projection,
            pixels,
            error,
            supersample,
            nullptr,
            nullptr,
            nullptr,
            {},
            given
        );
        OA_CHECK(result == Result::success);
        return pixels;
    };
    // A palette of grey steps and a few colours, and a gamma that lifts the
    // darks.
    std::vector<uint8_t> palette(256 * overlay::palette_entry_bytes);
    for (uint32_t entry = 0; entry < 256; ++entry)
        for (uint32_t channel = 0; channel < 3; ++channel)
            palette[entry * 4 + channel] = entry < 200 ? uint8_t(entry * 255 / 199)
                                                       : uint8_t((entry * 37 + channel * 91) % 256);
    std::array<uint8_t, 256> gamma{};
    for (uint32_t level = 0; level < 256; ++level)
        gamma[level] = uint8_t(std::lround(std::pow(level / 255.0, 0.8) * 255));
    const std::array<uint8_t, 3> key{1, 0, 0};
    std::vector<uint8_t> canvas(std::size_t(size) * size * 3);
    const auto clear = [&] {
        for (std::size_t at = 0; at < canvas.size(); at += 3)
            std::copy(key.begin(), key.end(), canvas.begin() + std::ptrdiff_t(at));
    };
    const auto paint = [&](uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint8_t level) {
        for (uint32_t y = top; y < top + h; ++y)
            for (uint32_t x = left; x < left + w; ++x)
                std::fill_n(canvas.begin() + std::ptrdiff_t((y * size + x) * 3), 3, level);
    };
    std::vector<overlay::Operation> operations{
        {4, 4, 30, 12, overlay::Kind::blend, {0, 0, 0}, 96, 0},
        {6, 6, 20, 1, overlay::Kind::shadow, {}, 0, 1},
        {6, 7, 20, 1, overlay::Kind::outline, {}, 0, 1},
        {8, 7, 9, 1, overlay::Kind::letter, {250, 240, 10}, 170, 1},
        {10, 20, 12, 3, overlay::Kind::outline, {}, 0, 2},
        {11, 21, 10, 1, overlay::Kind::letter, {20, 200, 250}, 60, 2},
        {-5, 40, 70, 30, overlay::Kind::blend, {200, 30, 30}, 300, 0},
        {30, 18, 14, 14, overlay::Kind::blend, {255, 255, 255}, 40, 0},
        {32, 25, 8, 1, overlay::Kind::letter, {0, 0, 0}, 254, 3}
    };
    const auto expected_overlay = [&](const std::vector<uint8_t>& rgba) {
        std::vector<uint8_t> rgb(std::size_t(size) * size * 3);
        for (std::size_t pixel = 0; pixel < rgb.size() / 3; ++pixel)
            std::copy_n(
                rgba.begin() + std::ptrdiff_t(pixel * 4), 3, rgb.begin() + std::ptrdiff_t(pixel * 3)
            );
        overlay::compose(rgb, size, size, canvas, key, operations, palette);
        std::vector<uint8_t> shown(rgba.size());
        for (std::size_t pixel = 0; pixel < rgb.size() / 3; ++pixel) {
            for (std::size_t channel = 0; channel < 3; ++channel)
                shown[pixel * 4 + channel] = gamma[rgb[pixel * 3 + channel]];
            shown[pixel * 4 + 3] = full_channel;
        }
        return shown;
    };
    overlay::Packed packed;
    std::vector<uint8_t> words;
    std::vector<uint8_t> bands;
    const auto add_overlay = [&]() {
        overlay::pack(operations, size, size, packed);
        overlay_words(canvas, key, band_rows, size, words, bands);
        const Overlay painted{words, bands, band_rows, &packed, palette, &gamma};
        OA_CHECK(compose_overlay(context, painted, error) == Result::success);
        OA_CHECK(composed(context) != nullptr);
        std::vector<uint8_t> shown;
        OA_CHECK(read_composed(context, shown, error) == Result::success);
        return shown;
    };
    // Nothing on the card yet: no overlay.
    {
        overlay::pack(operations, size, size, packed);
        clear();
        overlay_words(canvas, key, band_rows, size, words, bands);
        const Overlay painted{words, bands, band_rows, &packed, palette, &gamma};
        OA_CHECK(compose_overlay(context, painted, error) == Result::failed);
    }
    for (const uint32_t supersample : {1U, 2U, 4U})
        for (const Lights* given : {static_cast<const Lights*>(nullptr), &lights}) {
            const auto read = frame(false, given, supersample);
            OA_CHECK(!context.kept_on_card && !read.empty());
            OA_CHECK(frame(true, given, supersample).empty());
            OA_CHECK(context.kept_on_card);
            std::vector<uint8_t> picture;
            OA_CHECK(read_picture(context, picture, error) == Result::success);
            OA_CHECK(picture == read);
            clear();
            paint(0, 0, 3, 3, 120);
            paint(20, 6, 4, 2, 30);
            paint(40, 30, 6, 6, 250);
            const auto expected = expected_overlay(read);
            OA_CHECK(add_overlay() == expected);
            // The bands painted at the last overlay and clear now show the
            // picture again.
            clear();
            paint(10, 44, 5, 2, 60);
            OA_CHECK(add_overlay() == expected_overlay(read));
        }
    // Frames left on the card one after another, nothing waited for between
    // them, each overlaid on its own picture.
    std::vector<std::vector<uint8_t>> pictures;
    for (int shift = 0; shift < 3; ++shift) {
        for (auto& vertex : vertices)
            vertex.x += 1;
        pictures.push_back(frame(false, &lights, 2));
    }
    for (auto& vertex : vertices)
        vertex.x -= 3;
    clear();
    paint(5, 5, 2, 2, 90);
    for (int shift = 0; shift < 3; ++shift) {
        for (auto& vertex : vertices)
            vertex.x += 1;
        OA_CHECK(frame(true, &lights, 2).empty());
        OA_CHECK(add_overlay() == expected_overlay(pictures[std::size_t(shift)]));
    }
    // Lights the caller adds take the picture back.
    context.lights_allowed = false;
    OA_CHECK(!frame(true, &lights, 1).empty());
    OA_CHECK(!context.kept_on_card);
    context.lights_allowed = true;
    // A context on another's device draws the same picture and leaves the
    // device to it.
    SDL_GPUDevice* const shared = device(context);
    OA_CHECK(shared != nullptr);
    const auto own = frame(false, &lights, 1);
    {
        Context borrowing;
        borrowing.shared_device = shared;
        OA_CHECK(device(borrowing) == shared);
        std::vector<uint8_t> borrowed;
        OA_CHECK(
            render(
                borrowing,
                vertices,
                atlas,
                light_atlas_width,
                1,
                1,
                size,
                size,
                classic_projection,
                borrowed,
                error,
                1,
                nullptr,
                nullptr,
                nullptr,
                {},
                &lights
            ) == Result::success
        );
        OA_CHECK(borrowed == own);
    }
    OA_CHECK(frame(false, &lights, 1) == own);
}

} // namespace

/// Run validation on every platform and skip real rendering only when no device is available.
int main() {
    invalid_input();
    if (oa::test::check_exit_status() != 0)
        return oa::test::check_exit_status();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SKIP: live GPU video initialization: %s\n", SDL_GetError());
        return skipped;
    }
    const int result = persistent_frames();
    if (result == 0) {
        multisampled_frames();
        material_modulation();
        water_frames();
        submerged_faces();
        crossing_waterline();
        underwater_billboard_depth();
        terrain_shimmer_contrast();
        ripple_art_motion();
        coastal_foam_strength();
        card_lights();
        picture_on_card();
    }
    SDL_Quit();
    return result != 0 ? result : oa::test::check_exit_status();
}
