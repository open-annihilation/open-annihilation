// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_proof_backend.hpp"
#include "renderer_projection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace oa::app::renderer_proof {
namespace {
using renderer_proof_gpu::Result;
constexpr uint32_t maximum_edge = 4096;
constexpr size_t maximum_vertices = 3U * 1024U * 1024U;
constexpr float coordinate_limit = 32768.0F;
constexpr size_t channels = 4;
constexpr uint8_t opaque = 255;
constexpr double pixel_centre = 0.5;

/// Returns the signed twice-area of a triangle in screen space.
double edge(
    const renderer_projection::ScreenPoint& a,
    const renderer_projection::ScreenPoint& b,
    double x,
    double y
) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

/// Draws world triangles with an independent CPU depth buffer and nearest texture reads.
Result draw_cpu(void*, const Frame& frame, std::vector<uint8_t>& pixels, std::string& error) {
    pixels.clear();
    error.clear();
    if (!frame.width || !frame.height || !frame.texture_width || !frame.texture_height ||
        frame.width > maximum_edge || frame.height > maximum_edge ||
        frame.texture_width > maximum_edge || frame.texture_height > maximum_edge ||
        frame.vertices.empty() || frame.vertices.size() > maximum_vertices ||
        frame.vertices.size() % 3 != 0 ||
        frame.texture.size() != size_t{frame.texture_width} * frame.texture_height * channels) {
        error = "invalid proof frame dimensions or storage";
        return Result::failed;
    }
    for (const auto& v : frame.vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.height) || !std::isfinite(v.z) ||
            !std::isfinite(v.u) || !std::isfinite(v.v) || std::abs(v.x) > coordinate_limit ||
            std::abs(v.height) > coordinate_limit || std::abs(v.z) > coordinate_limit || v.u < 0 ||
            v.u > 1 || v.v < 0 || v.v > 1) {
            error = "invalid proof vertex";
            return Result::failed;
        }
    }
    pixels.resize(size_t{frame.width} * frame.height * channels);
    for (size_t i = 3; i < pixels.size(); i += channels)
        pixels[i] = opaque;
    std::vector<double> depths(
        size_t{frame.width} * frame.height, -std::numeric_limits<double>::infinity()
    );
    for (size_t i = 0; i < frame.vertices.size(); i += 3) {
        std::array<renderer_projection::ScreenPoint, 3> p{};
        for (size_t k = 0; k < p.size(); ++k) {
            const auto& v = frame.vertices[i + k];
            p[k] = *renderer_projection::project_classic({}, {v.x, v.height, v.z});
        }
        const double area = edge(p[0], p[1], p[2].x, p[2].y);
        if (area == 0)
            continue;
        const int left = static_cast<int>(
            std::clamp(std::floor(std::min({p[0].x, p[1].x, p[2].x})), 0.0, double(frame.width))
        );
        const int right = static_cast<int>(
            std::clamp(std::ceil(std::max({p[0].x, p[1].x, p[2].x})), 0.0, double(frame.width))
        );
        const int top = static_cast<int>(
            std::clamp(std::floor(std::min({p[0].y, p[1].y, p[2].y})), 0.0, double(frame.height))
        );
        const int bottom = static_cast<int>(
            std::clamp(std::ceil(std::max({p[0].y, p[1].y, p[2].y})), 0.0, double(frame.height))
        );
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const double a = edge(p[1], p[2], x + pixel_centre, y + pixel_centre) / area;
                const double b = edge(p[2], p[0], x + pixel_centre, y + pixel_centre) / area;
                const double c = edge(p[0], p[1], x + pixel_centre, y + pixel_centre) / area;
                if (a < 0 || b < 0 || c < 0)
                    continue;
                const size_t at = size_t{static_cast<uint32_t>(y)} * frame.width + x;
                const double depth = a * p[0].depth + b * p[1].depth + c * p[2].depth;
                if (depth < depths[at])
                    continue;
                const auto& v0 = frame.vertices[i];
                const auto& v1 = frame.vertices[i + 1];
                const auto& v2 = frame.vertices[i + 2];
                const double u = a * v0.u + b * v1.u + c * v2.u;
                const double v = a * v0.v + b * v1.v + c * v2.v;
                const auto tx = static_cast<uint32_t>(std::clamp(
                    std::floor(u * frame.texture_width), 0.0, double(frame.texture_width - 1)
                ));
                const auto ty = static_cast<uint32_t>(std::clamp(
                    std::floor(v * frame.texture_height), 0.0, double(frame.texture_height - 1)
                ));
                const size_t source = (size_t{ty} * frame.texture_width + tx) * channels;
                if (!frame.texture[source + 3])
                    continue;
                std::copy_n(frame.texture.data() + source, channels, pixels.data() + at * channels);
                depths[at] = depth;
            }
        }
    }
    return Result::success;
}

/// Submits the same prepared frame to SDL's offscreen Metal device.
Result draw_gpu(void*, const Frame& frame, std::vector<uint8_t>& pixels, std::string& error) {
    return renderer_proof_gpu::render(
        frame.vertices,
        frame.texture,
        frame.texture_width,
        frame.texture_height,
        frame.width,
        frame.height,
        0,
        0,
        pixels,
        error
    );
}

const std::array<RendererHooks, 2> builtins{
    {{nullptr, "cpu-depth-reference", false, draw_cpu},
     {nullptr, "experimental-metal-3d", true, draw_gpu}}
};
} // namespace

std::span<const RendererHooks> renderers() noexcept {
    return builtins;
}
} // namespace oa::app::renderer_proof
