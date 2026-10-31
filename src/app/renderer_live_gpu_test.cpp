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
#include <vector>
#include <utility>

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

/// Send the next kept ground to the card a slice a frame while the kept
/// ground draws, and take its buffer, unchecked and not sent again, in the
/// frame whose kept ground has its revision; a slice that fails its check,
/// or slices that leave a gap, leave the frame's ground sent with it.
void kept_ground_sent_ahead() {
    Context context;
    Context plain_context;
    constexpr uint32_t atlas_width = 2;
    // Texel 0 red, texel 1 green.
    const std::vector<uint8_t> atlas{
        full_channel, 0, 0, full_channel, 0, full_channel, 0, full_channel
    };
    // Two triangles of red ground, then a small green one.
    const std::vector<Vertex> red{
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
    // The same ground green, the next kept ground.
    auto green = red;
    for (auto& vertex : std::span(green).first(6))
        vertex.u = 1;
    const auto next = std::span<const Vertex>(green).first(6);
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> plain_pixels;
    std::string error;
    const auto frame = [&](const std::vector<Vertex>& vertices, const KeptGround& ground) {
        return render(
            context,
            vertices,
            atlas,
            atlas_width,
            1,
            1,
            small_target,
            small_target,
            classic_projection,
            pixels,
            error,
            1,
            nullptr,
            nullptr,
            &ground
        );
    };
    const auto plain = [&](const std::vector<Vertex>& vertices) {
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
        return plain_pixels;
    };
    const auto red_pixels = plain(red);
    const auto green_pixels = plain(green);
    OA_CHECK(red_pixels != green_pixels);
    const auto sending = [&](uint64_t revision, std::size_t first, std::size_t count) {
        return KeptGround{6, 1, next, revision, first, count};
    };
    // The kept ground draws while the next is sent, a triangle a frame.
    OA_CHECK(frame(red, KeptGround{6, 1}) == Result::success);
    OA_CHECK(frame(red, sending(2, 0, 3)) == Result::success);
    OA_CHECK(pixels == red_pixels);
    OA_CHECK(frame(red, sending(2, 3, 3)) == Result::success);
    OA_CHECK(pixels == red_pixels);
    // The frame that takes it draws what was sent, not its own vertices
    // under the same revision.
    OA_CHECK(frame(red, KeptGround{6, 2}) == Result::success);
    OA_CHECK(pixels == green_pixels);
    OA_CHECK(frame(red, KeptGround{6, 2}) == Result::success);
    OA_CHECK(pixels == green_pixels);
    // A slice that fails its check: the frame's own ground is sent with it.
    auto invalid = green;
    invalid[4].u = 2;
    OA_CHECK(frame(green, KeptGround{6, 2, invalid, 3, 0, 3}) == Result::success);
    OA_CHECK(frame(green, KeptGround{6, 2, invalid, 3, 3, 3}) == Result::success);
    OA_CHECK(pixels == green_pixels);
    OA_CHECK(frame(red, KeptGround{6, 3}) == Result::success);
    OA_CHECK(pixels == red_pixels);
    // Slices out of order leave a gap: the frame's ground is sent.
    OA_CHECK(frame(red, KeptGround{6, 3, next, 4, 0, 3}) == Result::success);
    OA_CHECK(frame(red, KeptGround{6, 3, next, 4, 0, 3}) == Result::success);
    OA_CHECK(frame(green, KeptGround{6, 4}) == Result::success);
    OA_CHECK(pixels == green_pixels);
    OA_CHECK(frame(red, KeptGround{6, 4, next, 5, 3, 3}) == Result::success);
    OA_CHECK(frame(red, KeptGround{6, 5}) == Result::success);
    OA_CHECK(pixels == red_pixels);
    // Sent whole, then taken with an atlas of another size: the frame's
    // ground is sent.
    OA_CHECK(frame(red, KeptGround{6, 5, next, 6, 0, 6}) == Result::success);
    const std::vector<uint8_t> wider{
        full_channel,
        0,
        0,
        full_channel,
        0,
        full_channel,
        0,
        full_channel,
        0,
        0,
        0,
        full_channel,
        0,
        0,
        0,
        full_channel
    };
    const auto wider_frame = [&](Context& on, std::vector<uint8_t>& out, const KeptGround* ground) {
        return render(
            on,
            red,
            wider,
            atlas_width * 2,
            1,
            2,
            small_target,
            small_target,
            classic_projection,
            out,
            error,
            1,
            nullptr,
            nullptr,
            ground
        );
    };
    const KeptGround taken{6, 6};
    OA_CHECK(wider_frame(context, pixels, &taken) == Result::success);
    OA_CHECK(wider_frame(plain_context, plain_pixels, nullptr) == Result::success);
    OA_CHECK(pixels == plain_pixels);
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
    kept_ground_sent_ahead();
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

/// A frame of model draws on the card (ModelDraws), and the vertices its
/// triangles give the scene's fragment stage, as the frame's own.
struct ModelFrame {
    std::vector<uint32_t> corners{};
    std::vector<ModelInstance> instances{};
    std::vector<float> positions{};
    std::vector<uint32_t> piece_frames{};
    std::vector<float> textures{};
    std::vector<uint32_t> indices{};
    std::vector<ModelRun> runs{};
    std::vector<ShadowCast> casts{};
    std::vector<uint32_t> cast_indices{};
    std::vector<ModelRun> cast_runs{};
    ShadowMap shadow_map{};
    std::size_t shadow_vertex{};
    std::size_t shadow_ground{};

    /// The draws, borrowing the frame.
    [[nodiscard]] ModelDraws draws(uint64_t revision) const {
        return {
            corners,
            revision,
            instances,
            positions,
            piece_frames,
            textures,
            indices,
            runs,
            casts,
            cast_indices,
            cast_runs,
            shadow_map,
            shadow_vertex,
            shadow_ground
        };
    }

    /// The vertex a corner of an instance gives: its place, the atlas place
    /// its piece's frame and its corner take, and the instance's brightness.
    [[nodiscard]] Vertex vertex(uint32_t instance, uint32_t corner) const {
        const auto& drawn = instances[instance];
        const uint32_t place = corners[2 * (drawn.corners + corner)];
        const uint32_t texture = corners[2 * (drawn.corners + corner) + 1];
        const uint32_t at = 3 * (drawn.positions + (place & 0xffffU));
        const bool first = piece_frames[drawn.pieces + (place >> model_piece_shift)] != 0;
        const uint32_t slot = first ? texture & 0x3fffU : (texture >> model_texture_bits) & 0x3fffU;
        const uint32_t polygon_corner = (texture >> model_polygon_corner_shift) & 3U;
        const float* places = textures.data() + 4 * (drawn.textures + slot);
        Vertex made{};
        made.x = positions[at];
        made.height = positions[at + 1];
        made.z = positions[at + 2];
        made.u = places[polygon_corner == 1 || polygon_corner == 2 ? 2 : 0];
        made.v = places[polygon_corner >= 2 ? 3 : 1];
        made.brightness = drawn.brightness;
        return made;
    }

    /// The frame's vertices with each run's triangles in their place.
    [[nodiscard]] std::vector<Vertex> expanded(std::span<const Vertex> frame) const {
        std::vector<Vertex> out;
        std::size_t next = 0;
        for (const auto& run : runs) {
            out.insert(
                out.end(),
                frame.begin() + std::ptrdiff_t(next),
                frame.begin() + std::ptrdiff_t(run.vertex)
            );
            next = run.vertex;
            for (uint32_t at = 0; at < run.index_count; ++at) {
                const uint32_t index = indices[run.first_index + at];
                out.push_back(
                    vertex(run.first_instance + (index >> model_corner_bits), index & 0xffffU)
                );
            }
        }
        out.insert(out.end(), frame.begin() + std::ptrdiff_t(next), frame.end());
        return out;
    }
};

/// Adds a box of six faces, two pieces of three faces each, as a type's
/// triangle corners: every face a fan of two triangles from its first
/// corner, its first and running textures two of three.
void add_box_type(ModelFrame& frame) {
    constexpr std::array<std::array<uint32_t, 4>, 6> faces{
        {{0, 1, 2, 3}, {7, 6, 5, 4}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}}
    };
    for (uint32_t face = 0; face < faces.size(); ++face)
        for (const uint32_t corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
            frame.corners.push_back(faces[face][corner] | (face / 3) << model_piece_shift);
            frame.corners.push_back(
                face % 3 | ((face + 1) % 3) << model_texture_bits |
                corner << model_polygon_corner_shift
            );
        }
}

/// Model draws render() draws on the card give the picture their triangles
/// give as the frame's own vertices, to the byte: in the frame's order
/// among its vertices, before and after a half transparent face and a kept
/// ground, with one sample, multisampled and supersampled, and in the
/// surface the lights reach. Corners added under the same revision are
/// added to those the card keeps; a new revision replaces them. Draws that
/// do not hold together are refused, as are any where the card may not
/// draw them.
void card_models() {
    constexpr uint32_t size = 40;
    constexpr uint32_t atlas_width = 4;
    constexpr uint32_t atlas_height = 2;
    const std::vector<uint8_t> atlas{200, 40,  40,  255, 40,  200, 40,  255, 40,  40,  200,
                                     255, 200, 200, 40,  255, 40,  200, 200, 255, 200, 40,
                                     200, 255, 120, 120, 120, 255, 250, 250, 250, 255};
    ModelFrame models;
    add_box_type(models);
    // Boxes of two sizes about the frame, turned and lifted, each with its
    // own textures: a texel, two across or two down.
    constexpr uint32_t box_count = 600;
    constexpr uint32_t corners_a_box = 36;
    for (uint32_t box = 0; box < box_count; ++box) {
        const float x = 3.25F + float(box * 7 % 33);
        const float z = 4.5F + float(box * 13 % 31);
        const float h = float(box % 5) * 0.75F;
        const float side = box % 2 == 0 ? 2.5F : 4.125F;
        const float turn = float(box) * 0.37F;
        const float across = std::cos(turn) * side;
        const float down = std::sin(turn) * side;
        ModelInstance instance;
        instance.corners = 0;
        instance.corner_count = corners_a_box;
        instance.positions = uint32_t(models.positions.size() / 3);
        instance.position_count = 8;
        instance.pieces = uint32_t(models.piece_frames.size());
        instance.piece_count = 2;
        instance.textures = uint32_t(models.textures.size() / 4);
        instance.texture_count = 3;
        instance.brightness = box % 3 == 0 ? 1.0F : 0.55F + float(box % 7) * 0.05F;
        for (const float top : {h, h + side})
            for (const auto& [dx, dz] :
                 {std::pair{0.0F, 0.0F},
                  std::pair{across, down},
                  std::pair{across - down, down + across},
                  std::pair{-down, across}})
                models.positions.insert(models.positions.end(), {x + dx, top, z + dz});
        models.piece_frames.insert(models.piece_frames.end(), {box % 2, (box / 2) % 2});
        for (uint32_t slot = 0; slot < 3; ++slot) {
            const uint32_t texel = (box + slot) % 8;
            const float u0 = (float(texel % atlas_width) + 0.5F) / atlas_width;
            const float v0 = (float(texel / atlas_width) + 0.5F) / atlas_height;
            const bool wide = slot == 1 && texel % atlas_width < atlas_width - 1;
            const bool tall = slot == 2 && texel / atlas_width == 0;
            models.textures.insert(
                models.textures.end(),
                {u0, v0, wide ? u0 + 1.0F / atlas_width : u0, tall ? v0 + 1.0F / atlas_height : v0}
            );
        }
        models.instances.push_back(instance);
    }
    // The frame: ground, then boxes, a half transparent face over them, and
    // the rest of the boxes over that.
    std::vector<Vertex> frame;
    const auto quad =
        [&](
            float left, float right, float front, float back, float height, float u, float opacity
        ) {
            Vertex a{left, height, front, u, 0.25F};
            a.opacity = opacity;
            Vertex b = a, c = a, d = a;
            b.x = c.x = right;
            c.z = d.z = back;
            frame.insert(frame.end(), {a, b, d, b, c, d});
        };
    quad(0, 40, 0, 48, 0, 0.875F, 1);
    quad(0, 40, 0, 48, 0.25F, 0.625F, 1);
    const std::size_t ground = frame.size();
    // The faces drawn: every face but one in five.
    const auto add_run = [&](uint32_t first, uint32_t last, std::size_t vertex) {
        ModelRun run{vertex, uint32_t(models.indices.size()), 0, first};
        for (uint32_t box = first; box < last; ++box)
            for (uint32_t corner = 0; corner < corners_a_box; ++corner)
                if ((box + corner / 6) % 5 != 0)
                    models.indices.push_back((box - first) << model_corner_bits | corner);
        run.index_count = uint32_t(models.indices.size()) - run.first_index;
        models.runs.push_back(run);
    };
    add_run(0, 150, ground);
    quad(6, 30, 8, 36, 3, 0.375F, 0.5F);
    add_run(150, 150 + model_run_instances, frame.size());
    add_run(150 + model_run_instances, box_count, frame.size());
    quad(20, 36, 2, 20, 1, 0.125F, 0.6F);
    const auto expanded = models.expanded(frame);
    OA_CHECK(expanded.size() > frame.size() + 3 * 2000);
    Context context;
    Context reference;
    std::vector<uint8_t> drawn;
    std::vector<uint8_t> expected;
    std::string error;
    const auto both = [&](uint32_t supersample,
                          const KeptGround* kept,
                          const Lights* lights,
                          const ModelFrame& drawn_models,
                          uint64_t revision) {
        const auto draws = drawn_models.draws(revision);
        const auto vertices = drawn_models.expanded(frame);
        const bool on_card = render(
                                 context,
                                 frame,
                                 atlas,
                                 atlas_width,
                                 atlas_height,
                                 1,
                                 size,
                                 size,
                                 classic_projection,
                                 drawn,
                                 error,
                                 supersample,
                                 nullptr,
                                 nullptr,
                                 kept,
                                 {},
                                 lights,
                                 &draws
                             ) == Result::success;
        const KeptGround reference_kept{kept ? kept->vertex_count : 0, kept ? kept->revision : 0};
        const bool from_vertices = render(
                                       reference,
                                       vertices,
                                       atlas,
                                       atlas_width,
                                       atlas_height,
                                       1,
                                       size,
                                       size,
                                       classic_projection,
                                       expected,
                                       error,
                                       supersample,
                                       nullptr,
                                       nullptr,
                                       &reference_kept,
                                       {},
                                       lights
                                   ) == Result::success;
        return on_card && from_vertices && !drawn.empty() && drawn == expected;
    };
    using oa::app::renderer_effects::Source;
    const std::vector<Source> sources{
        {12, 20, 9, 10, 8, 1, 0.6F, 0.2F, 1.5F, 1, 0, 0},
        {28, 14, 6, 9, 6, 0.3F, 0.6F, 1, 2, 0, 0, 0}
    };
    const Lights lights{sources, {true, true, false}, 1.5};
    for (const bool multisampling : {true, false})
        for (const uint32_t supersample : {1U, 2U, 4U}) {
            context.multisampling_allowed = reference.multisampling_allowed = multisampling;
            OA_CHECK(both(supersample, nullptr, nullptr, models, 5));
            const KeptGround kept{ground, 3};
            OA_CHECK(both(supersample, &kept, nullptr, models, 5));
            OA_CHECK(both(supersample, &kept, &lights, models, 5));
            OA_CHECK(context.lights_drawn == reference.lights_drawn);
        }
    context.multisampling_allowed = reference.multisampling_allowed = true;
    // The models change the picture.
    std::vector<uint8_t> bare;
    OA_CHECK(
        render(
            reference,
            frame,
            atlas,
            atlas_width,
            atlas_height,
            1,
            size,
            size,
            classic_projection,
            bare,
            error,
            2
        ) == Result::success
    );
    OA_CHECK(bare != expected);
    // A second type, a triangle, added under the same revision: the boxes'
    // corners stay on the card, the triangle's go up.
    auto grown = models;
    const auto triangle_corners = uint32_t(grown.corners.size() / 2);
    for (const uint32_t corner : {0U, 1U, 2U}) {
        grown.corners.push_back(corner);
        grown.corners.push_back(1 | 1 << model_texture_bits | corner << model_polygon_corner_shift);
    }
    ModelInstance triangle = grown.instances[3];
    triangle.corners = triangle_corners;
    triangle.corner_count = 3;
    grown.instances.push_back(triangle);
    grown.runs.back().index_count += 3;
    grown.indices.insert(
        grown.indices.end(),
        {(box_count - 150 - model_run_instances) << model_corner_bits | 0U,
         (box_count - 150 - model_run_instances) << model_corner_bits | 1U,
         (box_count - 150 - model_run_instances) << model_corner_bits | 2U}
    );
    {
        const auto grown_expanded = grown.expanded(frame);
        const auto grown_draws = grown.draws(5);
        OA_CHECK(
            render(
                context,
                frame,
                atlas,
                atlas_width,
                atlas_height,
                1,
                size,
                size,
                classic_projection,
                drawn,
                error,
                2,
                nullptr,
                nullptr,
                nullptr,
                {},
                nullptr,
                &grown_draws
            ) == Result::success
        );
        OA_CHECK(
            render(
                reference,
                grown_expanded,
                atlas,
                atlas_width,
                atlas_height,
                1,
                size,
                size,
                classic_projection,
                expected,
                error,
                2
            ) == Result::success
        );
        OA_CHECK(drawn == expected);
    }
    // Other corners under a new revision replace them.
    auto turned = models;
    for (std::size_t at = 1; at < turned.corners.size(); at += 2) {
        const uint32_t first = turned.corners[at] & 0x3fffU;
        turned.corners[at] = (turned.corners[at] & ~0x3fffU) | (first + 1) % 3;
    }
    OA_CHECK(both(2, nullptr, nullptr, turned, 6));
    const auto turned_picture = drawn;
    OA_CHECK(both(2, nullptr, nullptr, models, 5));
    OA_CHECK(drawn != turned_picture);

    // Draws that do not hold together, each refused with the frame.
    const auto refused = [&](const ModelFrame& broken) {
        const auto broken_draws = broken.draws(5);
        drawn.assign(1, 1);
        return render(
                   context,
                   frame,
                   atlas,
                   atlas_width,
                   atlas_height,
                   1,
                   size,
                   size,
                   classic_projection,
                   drawn,
                   error,
                   1,
                   nullptr,
                   nullptr,
                   nullptr,
                   {},
                   nullptr,
                   &broken_draws
               ) == Result::failed &&
               drawn.empty() && !error.empty();
    };
    auto broken = models;
    broken.indices[4] = 35U | 1U << model_corner_bits;
    broken.indices[5] = 36U;
    OA_CHECK(refused(broken));
    broken = models;
    broken.indices[0] = model_run_instances << model_corner_bits;
    OA_CHECK(refused(broken));
    broken = models;
    broken.runs[0].vertex = frame.size() + 1;
    OA_CHECK(refused(broken));
    broken = models;
    std::swap(broken.runs[0], broken.runs[1]);
    OA_CHECK(refused(broken));
    broken = models;
    broken.positions[7] = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(refused(broken));
    broken = models;
    broken.textures[2] = 1.5F;
    OA_CHECK(refused(broken));
    broken = models;
    broken.instances[2].position_count = 7;
    OA_CHECK(refused(broken));
    broken = models;
    broken.instances[2].texture_count = 2;
    OA_CHECK(refused(broken));
    broken = models;
    broken.instances[9].positions = uint32_t(broken.positions.size() / 3);
    OA_CHECK(refused(broken));
    broken = models;
    broken.piece_frames[1] = 2;
    OA_CHECK(refused(broken));
    broken = models;
    broken.instances[1].brightness = 1.25F;
    OA_CHECK(refused(broken));
    broken = models;
    broken.runs[0].index_count -= 1;
    OA_CHECK(refused(broken));
    context.models_allowed = false;
    OA_CHECK(!draws_models(context));
    OA_CHECK(refused(models));
    context.models_allowed = true;
    OA_CHECK(draws_models(context));
    OA_CHECK(both(1, nullptr, nullptr, models, 5));
}

/// The light the shadows' map leaves at each texel's centre, as the card
/// casts it: each cast's triangles laid as its kind lays them from their
/// corners' whole pixels from the model's place, a texel taking a cast's
/// light once, where no cast at its depth or nearer took it, and not where
/// the corners' height above the base does not pass the cast's.
std::vector<float> expected_shadow_map(const ModelFrame& models, const ShadowMap& map) {
    std::vector<float> light(std::size_t{map.width} * map.height, 1.0F);
    std::vector<float> depth(light.size(), 1.0F);
    const auto add = [&](const ShadowCast& cast, std::span<const uint32_t> corners) {
        const auto& instance = models.instances[cast.instance];
        std::array<std::array<float, 3>, 3> laid{};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const uint32_t place = models.corners[2 * (instance.corners + corners[corner])];
            const float* point =
                models.positions.data() + 3 * (instance.positions + (place & 0xffffU));
            // The whole pixels from the model's place.
            const float whole_x = std::floor(point[0] - cast.across);
            const float whole_y = std::floor(point[1] - cast.base);
            const float whole_z = std::floor(point[2] - cast.down);
            const float across =
                cast.across + (cast.kind == shadow_cast_cut ? whole_x : whole_x + 5);
            const float down = cast.down - cast.ground_half + whole_z;
            const float lean = std::floor(whole_y * 0.25F);
            laid[corner] = cast.kind == shadow_cast_sheared
                               ? std::array{across + lean, down - lean, whole_y}
                               : std::array{across, down - std::floor(whole_y * 0.5F), whole_y};
        }
        for (uint32_t y = 0; y < map.height; ++y)
            for (uint32_t x = 0; x < map.width; ++x) {
                const float px = map.left + (float(x) + 0.5F) * map.scale;
                const float py = map.top + (float(y) + 0.5F) * map.scale;
                const auto& [a, b, c] = laid;
                // Rows run down: a triangle whose corners run anticlockwise
                // casts nothing.
                const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                if (!(area > 0))
                    continue;
                const float wb = ((px - a[0]) * (c[1] - a[1]) - (py - a[1]) * (c[0] - a[0])) / area;
                const float wc = ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) / area;
                const float wa = 1 - wb - wc;
                if (wa < 0 || wb < 0 || wc < 0 ||
                    wa * a[2] + wb * b[2] + wc * c[2] <= cast.least_height)
                    continue;
                const std::size_t at = std::size_t{y} * map.width + x;
                if (!(cast.depth < depth[at]))
                    continue;
                depth[at] = cast.depth;
                light[at] *= cast.light;
            }
    };
    for (const auto& run : models.cast_runs)
        for (uint32_t first = 0; first + 2 < run.index_count; first += 3) {
            const uint32_t index = models.cast_indices[run.first_index + first];
            const auto& cast = models.casts[run.first_instance + (index >> model_corner_bits)];
            std::array<uint32_t, 3> corners{};
            for (uint32_t corner = 0; corner < 3; ++corner)
                corners[corner] = models.cast_indices[run.first_index + first + corner] & 0xffffU;
            add(cast, corners);
        }
    return light;
}

/// A unit's shadow cast on the card darkens the ground under it as the
/// game lays its picture: shadow_offset_x right of it and at the ground's
/// height, by its light; another overlapping it darkens the overlap again,
/// a building's sheared silhouette darkens none of the building's own
/// picture where it stands, a cast whose corners do not pass its height
/// darkens nothing, and nothing but the ground under the shadows changes,
/// with one sample and multisampled. A map an earlier frame made larger
/// lays the same shadows. A face whose corners run anticlockwise as laid
/// casts nothing. Casts that do not hold together are refused.
void card_shadows() {
    constexpr uint32_t size = 48;
    const std::vector<uint8_t> atlas{200, 160, 120, 255, 90, 180, 60, 255};
    std::vector<Vertex> frame;
    const auto quad = [&](float left, float right, float front, float back, float u) {
        Vertex a{left, 0, front, u, 0.5F};
        Vertex b = a, c = a, d = a;
        b.x = c.x = right;
        c.z = d.z = back;
        frame.insert(frame.end(), {a, b, d, b, c, d});
    };
    // The ground, then a card standing on it, which no shadow darkens.
    quad(0, 48, 0, 48, 0.25F);
    const std::size_t ground = frame.size();
    Vertex standing{40, 0, 40, 0.75F, 0.5F};
    Vertex top = standing;
    top.height = 6;
    Vertex side = standing;
    side.x = 46;
    frame.insert(frame.end(), {standing, side, top});
    ModelFrame models;
    add_box_type(models);
    // Each box's place, from which its corners' whole pixels are counted.
    std::vector<std::array<float, 2>> places;
    const auto box = [&](float x, float z, float side_length, float height) {
        places.push_back({x, z});
        ModelInstance instance;
        instance.corners = 0;
        instance.corner_count = 36;
        instance.positions = uint32_t(models.positions.size() / 3);
        instance.position_count = 8;
        instance.pieces = uint32_t(models.piece_frames.size());
        instance.piece_count = 2;
        instance.textures = uint32_t(models.textures.size() / 4);
        instance.texture_count = 3;
        for (const float level : {0.0F, height})
            for (const auto& [dx, dz] :
                 {std::pair{0.0F, 0.0F},
                  std::pair{side_length, 0.0F},
                  std::pair{side_length, side_length},
                  std::pair{0.0F, side_length}})
                models.positions.insert(models.positions.end(), {x + dx, level, z + dz});
        models.piece_frames.insert(models.piece_frames.end(), {0, 0});
        models.textures.insert(models.textures.end(), 12, 0.5F);
        models.instances.push_back(instance);
    };
    box(4.3F, 6.2F, 7.4F, 6.1F);
    box(9.1F, 9.7F, 6.6F, 3.3F);
    box(20.6F, 24.4F, 8.2F, 9.7F);
    box(30.2F, 4.9F, 5.3F, 4.4F);
    const auto cast = [&](uint32_t instance, uint32_t kind, float light, float least, float depth) {
        const auto at = uint32_t(models.casts.size());
        models.casts.push_back(
            {instance,
             kind,
             0.0F,
             0.0F,
             least,
             light,
             depth,
             places[instance][0],
             places[instance][1]}
        );
        if (models.cast_runs.empty())
            models.cast_runs.push_back({0, 0, 0, 0});
        for (uint32_t corner = 0; corner < 36; ++corner)
            models.cast_indices.push_back(at << model_corner_bits | corner);
        models.cast_runs.back().index_count += 36;
    };
    constexpr float every = -1e30F;
    cast(0, shadow_cast_picture, 0.5F, every, 0.9F);
    cast(1, shadow_cast_picture, 0.5F, every, 0.8F);
    cast(2, shadow_cast_cut, 1.0F, every, 0.7F);
    cast(2, shadow_cast_sheared, 0.5F, every, 0.7F);
    // Its corners never pass this height.
    cast(3, shadow_cast_picture, 0.5F, 100.0F, 0.6F);
    models.shadow_map = {0, -8, 1, 64, 64};
    models.shadow_vertex = ground;
    models.shadow_ground = ground;
    const auto expected_light = expected_shadow_map(models, models.shadow_map);
    Context context;
    Context plain_context;
    std::vector<uint8_t> shadowed;
    std::vector<uint8_t> plain;
    std::vector<uint8_t> single;
    std::string error;
    const auto draws = models.draws(9);
    for (const uint32_t supersample : {1U, 2U}) {
        OA_CHECK(
            render(
                context,
                frame,
                atlas,
                2,
                1,
                1,
                size,
                size,
                classic_projection,
                shadowed,
                error,
                supersample,
                nullptr,
                nullptr,
                nullptr,
                {},
                nullptr,
                &draws
            ) == Result::success
        );
        OA_CHECK(
            render(
                plain_context,
                frame,
                atlas,
                2,
                1,
                1,
                size,
                size,
                classic_projection,
                plain,
                error,
                supersample
            ) == Result::success
        );
        std::size_t darkened = 0;
        std::size_t wrong = 0;
        for (uint32_t y = 0; y < size; ++y)
            for (uint32_t x = 0; x < size; ++x) {
                const std::size_t at = (std::size_t{y} * size + x) * channels;
                // The ground under the pixel's centre, as the shadows' map
                // spans it (the decals' lift is a fortieth of a row).
                const auto texel_x = uint32_t(float(x) + 0.5F - models.shadow_map.left);
                const auto texel_y = uint32_t(float(y) + 0.525F - models.shadow_map.top);
                const float light = expected_light[std::size_t{texel_y} * 64 + texel_x];
                const bool standing_card = x >= 40 && y >= 37 && y < 40;
                bool close = true;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float want =
                        standing_card ? plain[at + channel] : plain[at + channel] * light;
                    close = close && std::abs(float(shadowed[at + channel]) - want) <=
                                         (supersample == 1 && light == 1 ? 0.0F : 2.5F);
                }
                darkened += light < 1 && !standing_card;
                // A pixel on a shadow's edge may take the texel on either
                // side of it; multisampled, a mean of both.
                wrong += close ? 0 : 1;
            }
        OA_CHECK(darkened > 150);
        OA_CHECK(wrong <= (supersample == 1 ? 6U : 60U));
        if (supersample == 1)
            single = shadowed;
    }
    // A frame whose map needs more texels makes the texture larger; the
    // next frame's shadows fall on the texels they fell on before.
    auto larger = models;
    larger.shadow_map = {0, -8, 1, 1500, 1100};
    const auto larger_draws = larger.draws(9);
    for (const auto* drawn : {&larger_draws, &draws})
        OA_CHECK(
            render(
                context,
                frame,
                atlas,
                2,
                1,
                1,
                size,
                size,
                classic_projection,
                shadowed,
                error,
                1,
                nullptr,
                nullptr,
                nullptr,
                {},
                nullptr,
                drawn
            ) == Result::success
        );
    OA_CHECK(shadowed == single);
    // A face whose corners run anticlockwise as laid, as a model's
    // underside does, casts nothing; one whose corners run clockwise
    // darkens the ground under it. The box's first face, its bottom, runs
    // clockwise as laid, and its second, its top, anticlockwise.
    for (const bool clockwise : {true, false}) {
        auto facing = models;
        facing.casts.assign(1, models.casts[0]);
        facing.cast_indices.clear();
        for (uint32_t corner = 0; corner < 6; ++corner)
            facing.cast_indices.push_back(clockwise ? corner : 6 + corner);
        facing.cast_runs = {{0, 0, 6, 0}};
        const auto facing_light = expected_shadow_map(facing, facing.shadow_map);
        const bool casts = std::any_of(facing_light.begin(), facing_light.end(), [](float light) {
            return light < 1;
        });
        OA_CHECK(casts == clockwise);
        const auto facing_draws = facing.draws(9);
        OA_CHECK(
            render(
                context,
                frame,
                atlas,
                2,
                1,
                1,
                size,
                size,
                classic_projection,
                shadowed,
                error,
                1,
                nullptr,
                nullptr,
                nullptr,
                {},
                nullptr,
                &facing_draws
            ) == Result::success
        );
        OA_CHECK(
            render(
                plain_context,
                frame,
                atlas,
                2,
                1,
                1,
                size,
                size,
                classic_projection,
                plain,
                error,
                1
            ) == Result::success
        );
        // Under the middle of the face.
        const std::size_t middle = (std::size_t{9} * size + 12) * channels;
        OA_CHECK((shadowed[middle] < plain[middle]) == clockwise);
        if (!clockwise)
            OA_CHECK(shadowed == plain);
    }
    // The overlap is darker than either shadow alone, the building's own
    // picture is not darkened where it stands, the clipped cast darkens
    // nothing.
    const auto light_at = [&](float x, float y) {
        return expected_light[std::size_t(y - models.shadow_map.top) * 64 + std::size_t(x)];
    };
    OA_CHECK(std::abs(light_at(15.5F, 12.5F) - 0.25F) < 0.01F);
    OA_CHECK(light_at(23.5F, 25.5F) == 1.0F);
    OA_CHECK(light_at(32.5F, 6.5F) == 1.0F);
    // Casts that do not hold together are refused.
    const auto refused = [&](const ModelFrame& broken) {
        const auto broken_draws = broken.draws(9);
        shadowed.assign(1, 1);
        return render(
                   context,
                   frame,
                   atlas,
                   2,
                   1,
                   1,
                   size,
                   size,
                   classic_projection,
                   shadowed,
                   error,
                   1,
                   nullptr,
                   nullptr,
                   nullptr,
                   {},
                   nullptr,
                   &broken_draws
               ) == Result::failed &&
               shadowed.empty();
    };
    auto broken = models;
    broken.casts[1].instance = 9;
    OA_CHECK(refused(broken));
    broken = models;
    broken.casts[2].kind = 3;
    OA_CHECK(refused(broken));
    broken = models;
    broken.casts[0].light = 1.5F;
    OA_CHECK(refused(broken));
    broken = models;
    broken.cast_indices[0] = 5U << model_corner_bits;
    OA_CHECK(refused(broken));
    broken = models;
    broken.shadow_map.scale = 3;
    OA_CHECK(refused(broken));
    broken = models;
    broken.shadow_vertex = frame.size() + 1;
    OA_CHECK(refused(broken));
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
        card_models();
        card_shadows();
    }
    SDL_Quit();
    return result != 0 ? result : oa::test::check_exit_status();
}
