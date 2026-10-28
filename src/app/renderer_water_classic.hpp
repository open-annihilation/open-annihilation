// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "runtime_full.hpp"
#include "full_terrain.hpp"
#include "oa/formats/tnt.hpp"
#include "renderer_water.hpp"
#include <array>
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
    /// Water ripples' motion of the water's art this frame (art_motion),
    /// which moves what lies on the sea bed with it; null for none.
    const full_terrain::ArtMotion* art{};
    std::span<const renderer_water::Disturbance> disturbances{};
    /// Each of the frame's vertices' world height, in map pixels, where the
    /// model stage drew it at one (full::ModelFrameInputs::vertex_heights),
    /// NaN elsewhere: the heights a ship's reflection mirrors.
    std::span<const float> vertex_heights{};
    /// Vertices already spent on reflections in this scene.
    std::size_t reflected_vertices{};
    /// True only for currently visible projected map pixels.
    std::function<bool(float, float)> visible;
    /// The part of the map's picture the game shows, map pixels across and
    /// down from its top left (Runtime::shown_map_size): the foam, rings,
    /// splashes and reflections end at its edges, as the terrain does.
    /// Either zero leaves them to the view's edges alone.
    std::array<float, 2> shown{};
};

/// Water ripples' motion of the map's own water art over the terrain's
/// picture from map pixel left, top to right, bottom: at each point of a
/// grid 8 to 16 window pixels apart, the broad swells and, up close, the
/// fine ripples of renderer_water_surface::art_offset at the water plane
/// the picture shows there, at the ripples' percent, none at the shore and
/// on land and all of it in deep water, faded out as the view zooms out
/// to 0.25. full_terrain draws the terrain's passes again moved by it.
/// Empty where the ripples are off or nothing would move.
full_terrain::ArtMotion
art_motion(const Inputs& in, double left, double top, double right, double bottom);

/// Tells whether a draw lies on the sea bed: a feature lying on the ground,
/// or a shadow, below the sea. The Full presenter draws these before
/// surface(), so that the water's foam, rings and splashes lie over them as
/// the ripples' moved art lies under them.
bool on_sea_bed(const Inputs& in, const WorldDrawList& draws, const WorldDraw& draw);

/// Tells whether surface() can draw anything, and so whether what lies on
/// the sea bed is drawn before it.
bool surface_drawn(const Inputs& in);

/// Append the coast's foam and the shots' rings and splashes, over what
/// lies on the sea bed and under everything else, before the fog passes.
void surface(card::CardFrame& frame, const Inputs& in);

/// Move a sprite under the water with the ripples' moved art (Inputs::art),
/// shade submerged geometry and place a reflected naval silhouette before
/// its original. A feature's frame (SpriteDraw::on_map) is the map's own
/// art, which already shows it under the water: it moves with the art and
/// keeps its colours, as the terrain does; models and effects under the
/// water take Underwater shimmer.
void objects(
    card::CardFrame& frame,
    Inputs& in,
    const WorldDrawList& draws,
    const WorldDraw& draw,
    std::size_t first_vertex,
    std::size_t first_index
);
} // namespace oa::app::renderer_water_classic
