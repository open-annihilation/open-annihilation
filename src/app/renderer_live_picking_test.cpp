// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_live_picking.hpp"
#include "oa/test/check.hpp"
#include "oa/platform/job_pool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {
namespace picking = oa::app::renderer_live_picking;
namespace camera = oa::app::renderer_live_camera;
namespace scene = oa::app::renderer_live_scene;
using Vertex = oa::app::renderer_live_gpu::Vertex;
constexpr std::size_t block_triangles = 32;
constexpr std::size_t band_blocks = 512;
using Point = std::array<float, 3>;
using Bounds = std::array<float, 4>;

/// Projects ordinary records and bounds each block independently of compact input.
Bounds reference_block(
    std::span<const Vertex> vertices,
    const camera::Matrix& m,
    std::size_t first,
    std::size_t end,
    std::span<Point> projected
) {
    constexpr float none = std::numeric_limits<float>::infinity();
    Bounds bounds{none, none, -none, -none};
    bool bounded = true;
    for (std::size_t index = first; index < end; ++index) {
        const auto& vertex = vertices[index];
        auto& point = projected[index];
        point = {
            m[0] * vertex.x + m[1] * vertex.height + m[2] * vertex.z + m[3],
            m[4] * vertex.x + m[5] * vertex.height + m[6] * vertex.z + m[7],
            m[8] * vertex.x + m[9] * vertex.height + m[10] * vertex.z + m[11]
        };
        if (point[0] < bounds[0])
            bounds[0] = point[0];
        if (point[1] < bounds[1])
            bounds[1] = point[1];
        if (point[0] > bounds[2])
            bounds[2] = point[0];
        if (point[1] > bounds[3])
            bounds[3] = point[1];
        if (index % 3 == 2 && bounded)
            bounded =
                camera::triangle_hit_bounded({projected[index - 2], projected[index - 1], point});
    }
    return bounded ? bounds : Bounds{-none, -none, none, none};
}

/// Owns the complete projection outputs, including the final mixed block.
struct Projected {
    std::vector<Point> points{};
    std::vector<Bounds> bounds{};
};

/// Runs every affected projection and bounds operation, on the usual block bands.
void project(
    const scene::Scene& input,
    const camera::Matrix& matrix,
    bool enabled,
    bool reference,
    Projected& output,
    oa::platform::job_pool::Pool* pool = nullptr
) {
    output.points.resize(input.vertices.size());
    const std::size_t triangles = input.vertices.size() / 3;
    const std::size_t blocks = (triangles + block_triangles - 1) / block_triangles;
    output.bounds.resize(blocks);
    const auto ground = picking::ground_coordinates(input, enabled);
    oa::platform::job_pool::run_bands(
        pool, static_cast<uint32_t>((blocks + band_blocks - 1) / band_blocks), [&](uint32_t band) {
            const auto first_block = std::size_t(band) * band_blocks;
            for (std::size_t block = first_block;
                 block < std::min(blocks, first_block + band_blocks);
                 ++block) {
                const auto first = block * block_triangles * 3;
                const auto end = std::min(triangles * 3, first + block_triangles * 3);
                output.bounds[block] =
                    reference ? reference_block(input.vertices, matrix, first, end, output.points)
                              : picking::project_block(
                                    input.vertices, ground, matrix, first, end, output.points
                                );
            }
        }
    );
}

/// Makes rough ground followed by non-ground triangles with distinct attributes.
void make(scene::Scene& input, std::size_t ground, std::size_t tail) {
    input.terrain_vertex_count = ground;
    input.ground_revision = input.ground_points_revision = 17;
    input.vertices.resize(ground + tail);
    input.ground_points.resize(ground);
    for (std::size_t index = 0; index < input.vertices.size(); ++index) {
        const auto triangle = index / 3;
        auto& vertex = input.vertices[index];
        vertex.x = static_cast<float>(triangle % 512 * 16 + (index % 3 == 1 ? 16 : 0));
        vertex.z = static_cast<float>(triangle / 512 * 16 + (index % 3 == 2 ? 16 : 0));
        vertex.height = static_cast<float>((triangle * 7 + index % 3 * 3) % 255);
        vertex.u = static_cast<float>(index % 31) / 32;
        vertex.v = static_cast<float>(index % 17) / 32;
        vertex.red = 0.2F;
        if (index < ground)
            input.ground_points[index] = {vertex.x, vertex.z, vertex.height};
    }
}

/// Coordinates, sliver bounds and triangle hits remain bit-exact in every mode.
void exact_projection() {
    scene::Scene input;
    constexpr std::size_t ground = 513 * block_triangles * 3 + 3, tail = 4 * 3;
    make(input, ground, tail);
    // Signed zero, a near-collinear face and unusable coordinates preserve
    // the original arithmetic and unbounded-block fallback.
    input.vertices[0].x = -0.0F;
    input.vertices[1].x = std::nextafter(0.0F, 1.0F);
    input.vertices[2].height = std::numeric_limits<float>::quiet_NaN();
    input.vertices[3].z = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < ground; ++index) {
        const auto& v = input.vertices[index];
        input.ground_points[index] = {v.x, v.z, v.height};
    }
    Projected actual, expected;
    oa::platform::job_pool::Pool pool{3};
    const int previous_rounding = std::fegetround();
    for (const int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        OA_CHECK(std::fesetround(rounding) == 0);
        for (const float pitch : {-53.0F, -32.0F, 0.0F, 20.0F}) {
            camera::State state{};
            state.unlocked = true;
            state.yaw_degrees = 20;
            state.pitch_degrees = pitch;
            for (const float zoom : {0.25F, 0.500001F, 2.25F}) {
                camera::Matrix matrix{};
                OA_CHECK(camera::projection(state, {250, 30, 250, 100, 75, zoom}, matrix));
                project(input, matrix, false, true, expected);
                for (auto* threads : {static_cast<oa::platform::job_pool::Pool*>(nullptr), &pool}) {
                    if (threads != nullptr && rounding != FE_TONEAREST)
                        continue;
                    project(input, matrix, true, false, actual, threads);
                    OA_CHECK(actual.points.size() == expected.points.size());
                    OA_CHECK(
                        std::memcmp(
                            actual.points.data(),
                            expected.points.data(),
                            actual.points.size() * sizeof(Point)
                        ) == 0
                    );
                    OA_CHECK(
                        std::memcmp(
                            actual.bounds.data(),
                            expected.bounds.data(),
                            actual.bounds.size() * sizeof(Bounds)
                        ) == 0
                    );
                    for (const std::size_t first : {std::size_t{6}, ground - 3, ground}) {
                        const float x = actual.points[first][0], y = actual.points[first][1];
                        std::array<float, 3> a{}, b{};
                        float da{}, db{};
                        const bool ha = camera::triangle_hit(
                            {actual.points[first],
                             actual.points[first + 1],
                             actual.points[first + 2]},
                            x,
                            y,
                            a,
                            da
                        );
                        const bool hb = camera::triangle_hit(
                            {expected.points[first],
                             expected.points[first + 1],
                             expected.points[first + 2]},
                            x,
                            y,
                            b,
                            db
                        );
                        OA_CHECK(
                            ha == hb && std::memcmp(a.data(), b.data(), sizeof(a)) == 0 &&
                            std::bit_cast<uint32_t>(da) == std::bit_cast<uint32_t>(db)
                        );
                    }
                }
            }
        }
    }
    OA_CHECK(std::fesetround(previous_rounding) == 0);
    // Invalid optional coordinate storage must not enter the projection.
    const camera::Matrix matrix{1, 0, 0, 0, 0, -0.5F, 1, 0, 0, 1, 0.5F, 0};
    const auto check_fallback = [&](bool enabled) {
        OA_CHECK(picking::ground_coordinates(input, enabled).empty());
        project(input, matrix, false, true, expected);
        project(input, matrix, enabled, false, actual);
        OA_CHECK(
            std::memcmp(
                actual.points.data(), expected.points.data(), actual.points.size() * sizeof(Point)
            ) == 0
        );
        OA_CHECK(
            std::memcmp(
                actual.bounds.data(), expected.bounds.data(), actual.bounds.size() * sizeof(Bounds)
            ) == 0
        );
    };
    for (auto& coordinate : input.ground_points)
        coordinate = {100000, 100000, 100000};
    check_fallback(false);
    input.ground_points_revision = 0;
    check_fallback(true);
    input.ground_points_revision = 18;
    check_fallback(true);
    input.ground_points_revision = 17;
    input.ground_revision = 0;
    check_fallback(true);
    input.ground_revision = 17;
    input.ground_points.pop_back();
    check_fallback(true);
    input.ground_points.resize(ground);
    input.terrain_vertex_count = input.vertices.size() + 3;
    check_fallback(true);
}

/// Final terrain and foundation-cut coordinates feed the same screen queries.
void produced_scene_projection() {
    oa::formats::tnt::Map map{};
    map.attribute_width = map.attribute_height = 16;
    map.tile_width = map.tile_height = 8;
    map.tile_count = 1;
    map.attributes.resize(256);
    map.tile_indices.assign(64, 0);
    map.tile_palette_indices.assign(32 * 32, 1);
    for (std::size_t index = 0; index < map.attributes.size(); ++index)
        map.attributes[index].height = static_cast<uint8_t>((index * 7) % 40);
    oa::PaletteBytes palette{};
    palette[oa::palette_entry_bytes] = 120;
    palette[oa::palette_entry_bytes + 1] = 60;
    palette[oa::palette_entry_bytes + 2] = 30;
    oa::Unit building{}, marker{};
    constexpr int32_t fixed = 65536;
    building.flags = OA_UNIT_FLAG_BUILDING;
    building.footprint_x = building.footprint_z = 2;
    building.position = {96 * fixed, 24 * fixed, 96 * fixed};
    marker.id = 19;
    marker.position = {128 * fixed, 40 * fixed, 128 * fixed};
    oa::app::WorldDrawList draws{};
    draws.unit_markers = {{&marker, marker.position}};
    scene::Inputs in{};
    in.map = &map;
    in.palette = &palette;
    in.draws = &draws;
    in.right = in.bottom = 256;
    in.clip_width = 200;
    in.clip_height = 150;
    in.sight.mapping = in.sight.line_of_sight = false;
    in.tiles_hold_while_moving = in.ground_holds_contained_views = true;
    in.owner_colors[0] = {255, 80, 20};
    scene::Scene input;
    Projected actual, expected;
    camera::State state{};
    state.unlocked = true;
    state.yaw_degrees = 20;
    std::string error;
    for (int frame = 0; frame < 10; ++frame) {
        state.pitch_degrees = frame < 2 ? -53 : -32;
        in.detail_scale = frame < 3 ? 0.25F : 0.6F;
        OA_CHECK(
            camera::projection(state, {128, 20, 128, 100, 75, in.detail_scale}, in.clip_projection)
        );
        if (frame == 4) {
            draws.models.resize(1);
            draws.models[0].model.unit = &building;
            draws.models[0].plan.linked.drawn = true;
        }
        if (frame == 5)
            building.position.y += fixed;
        if (frame == 8)
            ++map.attributes[100].height;
        OA_CHECK(scene::build(in, input, error));
        if (frame == 7)
            input.ground_points_revision = 0;
        if (frame == 2 || frame == 4 || frame == 5 || frame == 8)
            OA_CHECK(!picking::ground_coordinates(input, true).empty());
        project(input, in.clip_projection, false, true, expected);
        project(input, in.clip_projection, true, false, actual);
        OA_CHECK(
            std::memcmp(
                actual.points.data(), expected.points.data(), actual.points.size() * sizeof(Point)
            ) == 0
        );
        OA_CHECK(
            std::memcmp(
                actual.bounds.data(), expected.bounds.data(), actual.bounds.size() * sizeof(Bounds)
            ) == 0
        );
        OA_CHECK(input.unit_ranges.size() == 1 && input.unit_ranges[0].unit_id == marker.id);
    }
}

/// Times complete projection/bounds kernels, never just copies or raw bandwidth.
void component_net() {
    scene::Scene input;
    make(input, 2199522, 120000);
    Projected output;
    camera::Matrix matrix{};
    camera::State state{};
    state.unlocked = true;
    state.yaw_degrees = 20;
    state.pitch_degrees = -32;
    OA_CHECK(camera::projection(state, {5120, 0, 7291, 960, 509, 0.25F}, matrix));
    std::array<std::vector<double>, 3> times;
    for (int repeat = 0; repeat < 14; ++repeat)
        for (int position = 0; position < 3; ++position) {
            const int path = (position + repeat) % 3;
            const auto start = std::chrono::steady_clock::now();
            project(input, matrix, path == 2, path == 0, output);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            if (repeat >= 2)
                times[path].push_back(ms);
        }
    for (auto& samples : times)
        std::sort(samples.begin(), samples.end());
    const auto median = [](const auto& samples) {
        return (samples[samples.size() / 2 - 1] + samples[samples.size() / 2]) / 2;
    };
    std::printf(
        "{\"scope\":\"complete single-thread production projection+block bounds kernel, synthetic "
        "rendered coordinates; not whole "
        "draw/"
        "nativeFPS\",\"ground_vertices\":%zu,\"tail_vertices\":%zu,\"samples_per_path\":%zu,"
        "\"original_vertex_median_ms\":%.6f,\"factored_vertex_median_ms\":%.6f,\"compact_median_"
        "ms\":%.6f,\"checksum\":%.9g}\n",
        input.terrain_vertex_count,
        input.vertices.size() - input.terrain_vertex_count,
        times[0].size(),
        median(times[0]),
        median(times[1]),
        median(times[2]),
        double(output.points[100][0])
    );
}
} // namespace

/// Checks exact camera projection before an explicitly requested component measurement.
int main(int argc, char** argv) {
    exact_projection();
    produced_scene_projection();
    if (oa::test::check_exit_status() != 0)
        return oa::test::check_exit_status();
    if (argc == 2 && std::string_view(argv[1]) == "--net")
        component_net();
    return oa::test::check_exit_status();
}
