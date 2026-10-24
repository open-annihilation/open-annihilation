// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "runtime_full.hpp"
#include "oa/formats/tnt.hpp"
#include "renderer_water.hpp"
#include <functional>

namespace oa::app::renderer_water_classic {

/// Water presentation through the existing Full scene, without a frame readback.
struct Inputs {
    const formats::tnt::Map* map{};
    full::SceneView view{};
    double seconds{};
    renderer_water::Wind wind{};
    float ripples{};
    float coast{};
    float shimmer{};
    float reflections{};
    const renderer_water::SurfaceCache* water_surface{};
    std::span<const renderer_water::Disturbance> disturbances{};
    /// Vertices already spent on reflections in this scene.
    std::size_t reflected_vertices{};
    /// True only for currently visible projected map pixels.
    std::function<bool(float, float)> visible;
};

/// Append bounded water highlights before objects and the fog passes.
void surface(card::CardFrame& frame, const Inputs& in);

/// Shade submerged geometry and place a reflected naval silhouette before its original.
void objects(
    card::CardFrame& frame,
    Inputs& in,
    const WorldDrawList& draws,
    const WorldDraw& draw,
    std::size_t first_vertex,
    std::size_t first_index
);
} // namespace oa::app::renderer_water_classic
