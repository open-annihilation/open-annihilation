// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lights the live renderer's card adds to its picture
// (renderer_live_gpu.cpp), laid out as its lights pass reads them: the
// lights renderer_effects::compose() would draw, in its order, and for each
// tile of the picture the lights that reach it.
#pragma once

#include "renderer_effects.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::renderer_live_lights {

/// RGBA float texels a light takes in the lights' table, one row a light.
inline constexpr uint32_t texels_per_light = 4;

/// The edge of the square tiles of the picture each of which lists the
/// lights reaching it, in pixels.
inline constexpr uint32_t tile_edge = 32;

/// Words of a tile's list of lights: a bit a light.
inline constexpr uint32_t words_per_tile = renderer_effects::max_sources / 32;

/// The lights renderer_effects::compose() draws over a picture.
struct Table {
    /// texels_per_light RGBA texels a light, in compose()'s order: its
    /// centre's column, row and depth, and its depth reach; its radius
    /// across and down, its heat's phase at the frame's time and its heat's
    /// strength, zero where it does not distort; its red, green and blue and
    /// its light's strength, zero without coloured lighting; its glow's
    /// strength, zero without wreck glow, and three zeroes.
    std::vector<float> lights{};
    uint32_t count{}; ///< lights in the table, at most renderer_effects::max_sources
    uint32_t tiles_across{};
    uint32_t tiles_down{};
    /// words_per_tile words a tile, row after row of tiles: bit b of word w
    /// set for each light 32w + b whose reach crosses the tile.
    std::vector<uint32_t> tiles{};
};

/// Lays out the lights compose() would draw over a picture: the first
/// renderer_effects::max_sources sources, less those it rejects or that
/// lie wholly outside the picture or add nothing, in the same order. Light,
/// glow and heat each take compose()'s strengths and switches.
///
/// @param sources the projected sources, as compose() takes them
/// @param settings compose()'s switches
/// @param simulation_seconds compose()'s clock
/// @param width the picture's width, from 1 through 16384
/// @param height the picture's height, from 1 through 16384
/// @param[out] table the lights; none where compose() would change nothing
void pack(
    std::span<const renderer_effects::Source> sources,
    renderer_effects::Settings settings,
    double simulation_seconds,
    uint32_t width,
    uint32_t height,
    Table& table
);

/// The lights pass's uniform block: compose()'s energies and reach rules,
/// the picture's last column and row, and the tiles' edge.
///
/// @param width the picture's width
/// @param height the picture's height
/// @return twelve floats, as three vec4s
std::array<float, 12> parameters(uint32_t width, uint32_t height);

} // namespace oa::app::renderer_live_lights
