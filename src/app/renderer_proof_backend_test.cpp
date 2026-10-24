// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Proof backends preserve depth order, transparent cutouts and malformed-input rejection.
#include "renderer_proof_backend.hpp"
#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
using oa::app::renderer_proof::Frame;
using oa::app::renderer_proof::RendererHooks;
using oa::app::renderer_proof_gpu::Result;
using oa::app::renderer_proof_gpu::Vertex;
constexpr uint32_t image_edge = 4;
constexpr uint32_t channels = 4;
constexpr uint8_t opaque = 255;
constexpr int skipped = 77;
constexpr float front_height = 10;
constexpr std::array<uint8_t, 16> atlas{
    opaque, 0, 0, opaque, opaque, 0, 0, 0, 0, opaque, 0, opaque, 0, opaque, 0, opaque
};

/// Append a square that projects to the image, with a selected horizontal atlas interval.
void append_quad(std::vector<Vertex>& vertices, float height, float start_u, float end_u) {
    constexpr float height_projection = 0.5f;
    const float z = height * height_projection;
    const std::array<Vertex, 6> square{
        {{0, height, z, start_u, 0},
         {image_edge, height, z, end_u, 0},
         {0, height, z + image_edge, start_u, 1},
         {image_edge, height, z, end_u, 0},
         {image_edge, height, z + image_edge, end_u, 1},
         {0, height, z + image_edge, start_u, 1}}
    };
    vertices.insert(vertices.end(), square.begin(), square.end());
}

/// Make overlapping foreground and background surfaces; the foreground has a cutout.
std::vector<Vertex> overlapping_vertices(bool front_first) {
    constexpr float half_atlas = 0.5f;
    std::vector<Vertex> vertices;
    if (front_first) {
        append_quad(vertices, front_height, 0, half_atlas);
        append_quad(vertices, 0, half_atlas, 1);
    } else {
        append_quad(vertices, 0, half_atlas, 1);
        append_quad(vertices, front_height, 0, half_atlas);
    }
    return vertices;
}

/// Require a red left half and a green background visible through the right cutout.
void check_pixels(const std::vector<uint8_t>& pixels) {
    OA_CHECK(pixels.size() == image_edge * image_edge * channels);
    if (pixels.size() != image_edge * image_edge * channels)
        return;
    for (size_t at = 0; at < pixels.size(); at += channels) {
        const bool foreground = (at / channels) % image_edge < image_edge / 2;
        OA_CHECK(pixels[at] == (foreground ? opaque : 0));
        OA_CHECK(pixels[at + 1] == (foreground ? 0 : opaque));
        OA_CHECK(pixels[at + 2] == 0);
        OA_CHECK(pixels[at + 3] == opaque);
    }
}

/// Depth and cutout results must be independent of draw order and triangle winding.
Result check_depth_and_cutout(const RendererHooks& renderer) {
    std::vector<uint8_t> first_pixels;
    for (const bool front_first : {true, false}) {
        auto vertices = overlapping_vertices(front_first);
        for (const bool reverse_winding : {false, true}) {
            if (reverse_winding) {
                for (size_t at = 0; at < vertices.size(); at += 3)
                    std::swap(vertices[at], vertices[at + 1]);
            }
            const Frame frame{vertices, atlas, 4, 1, image_edge, image_edge};
            std::vector<uint8_t> pixels;
            std::string error;
            const auto result = renderer.draw(renderer.context, frame, pixels, error);
            if (result == Result::unavailable) {
                std::fprintf(stderr, "%s unavailable: %s\n", renderer.name, error.c_str());
                return result;
            }
            OA_CHECK(result == Result::success);
            OA_CHECK(error.empty());
            if (result != Result::success) {
                std::fprintf(stderr, "%s failed: %s\n", renderer.name, error.c_str());
                return result;
            }
            check_pixels(pixels);
            if (first_pixels.empty())
                first_pixels = pixels;
            else
                OA_CHECK(first_pixels == pixels);
        }
    }
    return Result::success;
}

/// Invalid input must fail with a reason and clear any previous output.
void require_rejected(const RendererHooks& renderer, const Frame& frame) {
    std::vector<uint8_t> pixels{opaque};
    std::string error;
    OA_CHECK(renderer.draw(renderer.context, frame, pixels, error) == Result::failed);
    OA_CHECK(pixels.empty());
    OA_CHECK(!error.empty());
}

/// Exercise dimensions, texture storage, incomplete triangles and invalid coordinates.
void check_malformed(const RendererHooks& renderer) {
    auto vertices = overlapping_vertices(true);
    const Frame valid{vertices, atlas, 4, 1, image_edge, image_edge};
    auto frame = valid;
    frame.width = 0;
    require_rejected(renderer, frame);
    frame = valid;
    frame.height = std::numeric_limits<uint32_t>::max();
    require_rejected(renderer, frame);
    frame = valid;
    frame.texture_width = std::numeric_limits<uint32_t>::max();
    require_rejected(renderer, frame);
    frame = valid;
    frame.texture = std::span<const uint8_t>(atlas).first(atlas.size() - 1);
    require_rejected(renderer, frame);
    frame = valid;
    frame.vertices = {};
    require_rejected(renderer, frame);
    frame = valid;
    frame.vertices = std::span<const Vertex>(vertices).first(vertices.size() - 1);
    require_rejected(renderer, frame);
    frame = valid;
    vertices[0].x = std::numeric_limits<float>::quiet_NaN();
    require_rejected(renderer, frame);
    vertices[0].x = std::numeric_limits<float>::max();
    require_rejected(renderer, frame);
    vertices[0].x = 0;
    vertices[0].u = -1;
    require_rejected(renderer, frame);
    vertices[0].u = 0;
    vertices[0].v = std::numeric_limits<float>::infinity();
    require_rejected(renderer, frame);
}
} // namespace

/// Run CPU cases by default, or real offscreen GPU cases without creating a window.
int main(int argc, char** argv) {
    const bool gpu = argc == 2 && std::strcmp(argv[1], "--gpu") == 0;
    if (argc != 1 && !gpu) {
        std::fprintf(stderr, "usage: renderer-proof-backend-test [--gpu]\n");
        return 1;
    }
    const auto renderers = oa::app::renderer_proof::renderers();
    OA_CHECK(renderers.size() == 2);
    if (renderers.size() != 2)
        return oa::test::check_exit_status();
    OA_CHECK(!renderers[0].accelerated);
    OA_CHECK(renderers[1].accelerated);
    for (const auto& renderer : renderers) {
        OA_CHECK(renderer.draw != nullptr);
        if (!renderer.draw)
            return oa::test::check_exit_status();
        check_malformed(renderer);
    }
    if (oa::test::failed_checks())
        return oa::test::check_exit_status();
    if (gpu && !SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "GPU proof video unavailable: %s\n", SDL_GetError());
        return skipped;
    }
    const auto result = check_depth_and_cutout(renderers[gpu ? 1 : 0]);
    if (gpu)
        SDL_Quit();
    if (result == Result::unavailable)
        return gpu ? skipped : 1;
    return oa::test::check_exit_status();
}
