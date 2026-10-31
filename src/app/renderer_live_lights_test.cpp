// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The card's lights table holds the lights compose() draws, in its order and
// at its strengths, and every tile a light changes lists it.
#include "renderer_live_lights.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {
using namespace oa::app;
using renderer_live_lights::pack;
using renderer_live_lights::Table;
using renderer_live_lights::texels_per_light;
using renderer_live_lights::tile_edge;
using renderer_live_lights::words_per_tile;
constexpr uint32_t width = 150;
constexpr uint32_t height = 90;
constexpr renderer_effects::Settings every_effect{true, true, true};

/// A warm light of radius 10 at a column and row.
renderer_effects::Source light_at(float x, float y) {
    return {x, y, 0, 10, 10, 1, 0.5F, 0.25F, 1, 1, 1, 0.5F, 0};
}

/// The table's float f of light n.
float field(const Table& table, uint32_t light, uint32_t field) {
    return table.lights[std::size_t(light) * texels_per_light * 4 + field];
}

/// Whether tile column, row lists light n.
bool listed(const Table& table, uint32_t column, uint32_t row, uint32_t light) {
    const auto word =
        table.tiles[(std::size_t(row) * table.tiles_across + column) * words_per_tile + light / 32];
    return (word >> (light % 32) & 1U) != 0;
}

/// Switches off, no sources, a clock that is not finite or a picture of no
/// size leave the table empty.
void nothing_to_draw() {
    Table table;
    const std::vector sources{light_at(20, 20)};
    pack(sources, {}, 0, width, height, table);
    OA_CHECK(table.count == 0 && table.lights.empty() && table.tiles.empty());
    pack({}, every_effect, 0, width, height, table);
    OA_CHECK(table.count == 0);
    pack(sources, every_effect, std::numeric_limits<double>::infinity(), width, height, table);
    OA_CHECK(table.count == 0);
    pack(sources, every_effect, 0, 0, height, table);
    OA_CHECK(table.count == 0);
    pack(sources, every_effect, 0, width, height, table);
    OA_CHECK(table.count == 1);
    OA_CHECK(table.tiles_across == (width + tile_edge - 1) / tile_edge);
    OA_CHECK(table.tiles_down == (height + tile_edge - 1) / tile_edge);
}

/// The first max_sources sources are drawn in their order, less those
/// compose() rejects, those wholly outside the picture and those that add
/// nothing.
void order_and_rejection() {
    std::vector<renderer_effects::Source> sources;
    for (uint32_t index = 0; index < renderer_effects::max_sources + 5; ++index)
        sources.push_back(light_at(float(index % width), 45));
    sources[3].screen_x = std::numeric_limits<float>::quiet_NaN();
    sources[4].radius_pixels = 0;
    sources[5].screen_x = -11; // its reach ends left of the picture
    sources[6].screen_y = height + 10;
    sources[7].screen_x = -9; // its reach crosses the picture's first column
    sources[8].light = sources[8].glow = 0;
    sources[8].heat = 0.2F; // too little to distort
    Table table;
    pack(sources, every_effect, 0, width, height, table);
    OA_CHECK(table.count == renderer_effects::max_sources - 5);
    uint32_t light = 0;
    for (uint32_t index = 0; index < renderer_effects::max_sources; ++index) {
        if (index >= 3 && index <= 8 && index != 7)
            continue;
        OA_CHECK(field(table, light, 0) == sources[index].screen_x);
        ++light;
    }
    OA_CHECK(light == table.count);
    OA_CHECK(table.lights.size() == std::size_t(table.count) * texels_per_light * 4);
}

/// Strengths are bounded as compose() bounds them, and each effect's switch
/// zeroes it.
void strengths_and_switches() {
    auto source = light_at(40, 40);
    source.red = 3;
    source.blue = -1;
    source.light = 5;
    source.glow = 2.5F;
    source.heat = 7;
    source.radius_pixels = 400;
    source.radius_y_pixels = 300;
    const std::vector sources{source};
    Table table;
    pack(sources, every_effect, 0, width, height, table);
    OA_CHECK(table.count == 1);
    OA_CHECK(field(table, 0, 4) == renderer_effects::max_radius);
    OA_CHECK(field(table, 0, 5) == renderer_effects::max_radius);
    OA_CHECK(field(table, 0, 7) == 2);  // heat
    OA_CHECK(field(table, 0, 8) == 1);  // red
    OA_CHECK(field(table, 0, 10) == 0); // blue
    OA_CHECK(field(table, 0, 11) == 2); // light
    OA_CHECK(field(table, 0, 12) == 2); // glow
    pack(sources, {false, true, true}, 0, width, height, table);
    OA_CHECK(field(table, 0, 11) == 0 && field(table, 0, 12) == 2 && field(table, 0, 7) == 2);
    pack(sources, {true, false, true}, 0, width, height, table);
    OA_CHECK(field(table, 0, 11) == 2 && field(table, 0, 12) == 0 && field(table, 0, 7) == 2);
    pack(sources, {true, true, false}, 0, width, height, table);
    OA_CHECK(field(table, 0, 11) == 2 && field(table, 0, 12) == 2 && field(table, 0, 7) == 0);
    // A round source's vertical radius is its radius.
    source = light_at(40, 40);
    source.radius_y_pixels = 0;
    pack(std::vector{source}, every_effect, 0, width, height, table);
    OA_CHECK(field(table, 0, 5) == 10);
}

/// Heat's phase at the frame's time is compose()'s, within a turn.
void heat_phase() {
    auto source = light_at(40, 40);
    source.phase = 2.5F;
    Table table;
    for (const double seconds : {0.0, 3.25, 119.5, 120.25, 9876.5}) {
        pack(std::vector{source}, every_effect, seconds, width, height, table);
        const float time = float(std::fmod(seconds, renderer_effects::animation_period));
        for (const float row : {0.0F, 17.0F, 89.0F}) {
            const float expected = std::sin(
                row * renderer_effects::heat_spatial_frequency -
                time * renderer_effects::heat_temporal_frequency + source.phase
            );
            const float phase = field(table, 0, 6);
            OA_CHECK(std::abs(phase) <= 6.2832F);
            OA_CHECK(
                std::abs(
                    std::sin(row * renderer_effects::heat_spatial_frequency + phase) - expected
                ) < 1e-4F
            );
        }
    }
}

/// Every pixel compose() changes for a light lies in a tile that lists it,
/// and a tile lists a light only where its rectangle crosses the tile.
void tiles_cover_the_reach() {
    const std::vector<renderer_effects::Source> sources{
        light_at(0, 0),
        light_at(31.5F, 31.5F),
        light_at(32, 70),
        light_at(149, 89),
        light_at(150, 45),
        light_at(70.25F, 12.75F)
    };
    Table table;
    pack(sources, every_effect, 0, width, height, table);
    OA_CHECK(table.count == sources.size());
    for (uint32_t light = 0; light < table.count; ++light) {
        // A grey picture, lit by the light alone, against the plain one.
        std::vector<uint8_t> plain(std::size_t(width) * height * 3, 100);
        auto lit = plain;
        renderer_effects::compose(
            {lit, int(width), int(height), int(width * 3), 3},
            std::span(&sources[light], 1),
            {true, true, false},
            0
        );
        bool changed_any = false;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto offset = (std::size_t(y) * width + x) * 3;
                if (std::equal(&lit[offset], &lit[offset] + 3, &plain[offset]))
                    continue;
                changed_any = true;
                OA_CHECK(listed(table, x / tile_edge, y / tile_edge, light));
            }
        OA_CHECK(changed_any);
        const auto& source = sources[light];
        const auto left = uint32_t(std::max(0.0F, source.screen_x - 10));
        const auto right = uint32_t(std::min(float(width - 1), source.screen_x + 10));
        const auto top = uint32_t(std::max(0.0F, source.screen_y - 10));
        const auto bottom = uint32_t(std::min(float(height - 1), source.screen_y + 10));
        for (uint32_t row = 0; row < table.tiles_down; ++row)
            for (uint32_t column = 0; column < table.tiles_across; ++column) {
                const bool crosses = column * tile_edge <= right &&
                                     (column + 1) * tile_edge > left && row * tile_edge <= bottom &&
                                     (row + 1) * tile_edge > top;
                OA_CHECK(listed(table, column, row, light) == crosses);
            }
    }
}

/// The pass's block holds compose()'s constants and the picture's layout.
void pass_parameters() {
    const auto parameters = renderer_live_lights::parameters(width, height);
    OA_CHECK(parameters[0] == renderer_effects::light_energy);
    OA_CHECK(parameters[1] == renderer_effects::glow_energy);
    OA_CHECK(
        parameters[2] ==
        renderer_effects::glow_radius_fraction * renderer_effects::glow_radius_fraction
    );
    OA_CHECK(parameters[3] == renderer_effects::silhouette_depth_fraction);
    OA_CHECK(parameters[4] == float(width - 1) && parameters[5] == float(height - 1));
    OA_CHECK(parameters[6] == renderer_effects::heat_spatial_frequency);
    OA_CHECK(parameters[7] == renderer_effects::max_displacement);
    OA_CHECK(parameters[8] == float(tile_edge));
}

} // namespace

int main() {
    nothing_to_draw();
    order_and_rejection();
    strengths_and_switches();
    heat_phase();
    tiles_cover_the_reach();
    pass_parameters();
    return oa::test::check_exit_status();
}
