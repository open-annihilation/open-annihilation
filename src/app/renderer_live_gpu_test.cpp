// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Exercise persistent GPU resources, atlas revisions, resizing and invalid input without a window.
#include "renderer_live_gpu.hpp"
#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <limits>

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
    float Vertex::* member;
    float low;
    float high;
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
    {&Vertex::terrain_grey, 0, 1}
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
        const auto original_vertices = scene.vertices;
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
        scene.vertices = original_vertices;
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
    const auto compare =
        [&](uint64_t revision, const AtlasUpdate* update, uint32_t width = atlas_size) {
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
    compare(1, &first_update); // A newly created texture still needs every row.
    for (uint32_t x = 0; x < atlas_size; ++x) {
        const size_t offset = (atlas_size + x) * channels;
        atlas[offset] = uint8_t(x);
        atlas[offset + 1] = 0;
    }
    const AtlasUpdate middle_row{1, 1, 2};
    compare(2, &middle_row);
    const auto before = partial_pixels;
    atlas[0] = 0;
    compare(2, &middle_row); // Identical revisions retain the cached atlas.
    OA_CHECK(partial_pixels == before);
    atlas[0] = full_channel;
    const AtlasUpdate no_rows{2, 2, 2};
    compare(3, &no_rows);
    OA_CHECK(partial_pixels == before);
    atlas[0] = 0;
    const AtlasUpdate stale_update{1, 1, 2};
    compare(4, &stale_update); // A revision gap requires the changed first row too.
    OA_CHECK(partial_pixels != before);
    atlas.assign(resized_width * atlas_size * channels, full_channel);
    atlas[0] = 0;
    const AtlasUpdate resized_update{4, 1, 2};
    compare(5, &resized_update, resized_width);
    for (const AtlasUpdate invalid : {AtlasUpdate{5, 3, 2}, AtlasUpdate{5, 0, atlas_size + 1}}) {
        OA_CHECK(
            render(
                partial_context,
                vertices,
                atlas,
                resized_width,
                atlas_size,
                6,
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
    for (const uint32_t factor : {2u, 4u, 8u, 16u}) {
        OA_CHECK(scene.frame(2, small_target, factor) == Result::success);
        OA_CHECK(scene.context.actual_supersample == factor);
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
    SDL_Quit();
    return result;
}
