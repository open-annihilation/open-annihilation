// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oa/formats/tnt.hpp"
#include <array>
#include <span>
#include <vector>

namespace oa::app::renderer_water {
/// Stable map-derived surface flow and water-texture travel in world pixels.
struct Wind {
    float x{1}, z{};            ///< Fixed unit direction in the horizontal world plane.
    float strength{.55F};       ///< Moderate visual flow, independent of simulation wind changes.
    float drift_x{}, drift_z{}; ///< Texture travel at the current simulation tick.
};

/// Presentation-only map flow; no simulation changes or random samples.
struct WindState {
    Wind value{};
    /// Derive a stable heading from bounded map metadata, without consuming random state.
    void reset(const formats::tnt::Map& map) noexcept;
    /// Absolute tick phase makes repeated frames, reloads and seeks reproducible.
    void advance(uint32_t now) noexcept;
};

/// The map's sea as the keys of its OTA's GlobalHeader describe it, which
/// decides which of OA's water effects it shows.
struct Sea {
    /// lavaworld=1: the sea is lava, which the game draws with lava splashes
    /// and lava explosions, and which shows none of OA's water effects.
    bool lava{};
    /// nosealeveltrigger=1: shots and debris that reach the sea vanish
    /// without a splash, so no shot stirs it with rings or splashes.
    bool swallows_shots{};
};

/// How strongly OA draws each water effect: its slider's percent over 100,
/// 0 to 2, 0 leaving it out.
struct Levels {
    float ripples{};     ///< Water ripples, and the rings and splashes of shots
    float coast{};       ///< Coastal waves
    float shimmer{};     ///< Underwater shimmer
    float reflections{}; ///< Naval reflections
};

/// Gives the strengths a sea shows the water effects at: none on lava, as
/// chosen on any other sea.
/// @param sea the map's sea
/// @param chosen the strengths the settings choose
/// @return all zero on lava, else `chosen`
[[nodiscard]] Levels shown_levels(const Sea& sea, const Levels& chosen) noexcept;

/// Tells whether shots and naval guns stir the sea with rings and splashes:
/// not on a sea that swallows shots, nor on lava, which takes the game's
/// own lava splashes instead.
/// @param sea the map's sea
/// @return true where the rings and splashes are drawn
[[nodiscard]] bool stirred_by_shots(const Sea& sea) noexcept;

/// Distinguish small naval firing and hull-hit rings from open-water splashes.
enum class Kind { firing, hit, water_impact };

/// A visible, presentation-only water disturbance in world pixels and simulation ticks.
struct Disturbance {
    float x{}, z{};
    uint32_t birth{};
    float strength{1};
    Kind kind{Kind::water_impact};
};

/// Surface-ring eligibility from the actual weapon-event height, never its projected image.
/// Submerged firing/hits produce no surface disturbance; water weapons require proven entry.
/// A detonating projectile crossing the surface may overshoot the plane within its last tick.
float surface_strength(
    Kind kind, float height, float sea, bool water_weapon, bool crossed_surface
) noexcept;

inline constexpr uint32_t lifetime_ticks = 60;

/// Fixed storage prevents sustained bombardment from growing presentation work.
struct History {
    static constexpr std::size_t capacity = 128;
    std::array<Disturbance, capacity> entries{};
    std::size_t count{};
    uint32_t tick{};
    bool seen{};
    /// Expire old rings and discard history when simulation time moves backwards.
    void advance(uint32_t now) noexcept;
    /// Retain a currently visible event, coalescing nearby events from the same burst.
    void add(Disturbance event, bool visible) noexcept;
    /// Contiguous retained events, oldest first.
    std::span<const Disturbance> active() const noexcept;
};

/// Gives the colour coastal waves' foam takes over ground of a colour: the
/// ground's colour lightened towards white by
/// renderer_water_appearance::foam_lift, keeping foam_chroma of its colour.
/// @param ground the ground's red, green and blue, each 0 to 1
/// @return the foam's red, green and blue, each lighter than the ground's
///         unless that is already white
[[nodiscard]] std::array<float, 3> foam_tint(const std::array<float, 3>& ground) noexcept;

/// Bounded coarse water-body metadata rebuilt once for each loaded match.
class SurfaceCache {
  public:

    /// Sample water connectivity and local fetch without retaining the map,
    /// and with the game's palette the colour the map's picture gives the
    /// coastal waves' foam (foam_colour).
    /// @param map the match's map
    /// @param palette the game's palette: 256 colours of red, green and blue
    ///        bytes; empty or short leaves the foam its plain colour
    void reset(const formats::tnt::Map& map, std::span<const uint8_t> palette = {});
    /// Local wave strength, zero outside water and reduced on small enclosed bodies.
    float fetch(float x, float z) const noexcept;
    /// fetch blended between the centres of the coarse cells around a point,
    /// so that it changes evenly from cell to cell; zero outside the map.
    float smooth_fetch(float x, float z) const noexcept;
    /// Gives the colour of the coastal waves' foam over a point of the map's
    /// picture: foam_tint of the picture's colour around it, averaged over
    /// the cells about the point and blended between their centres.
    /// @param x map pixels across the picture
    /// @param y the picture's row, map pixels down
    /// @return red, green and blue, each 0 to 1; the foam's plain colour
    ///         (renderer_water_appearance::foam_red and the others) where
    ///         the picture's colours are not known
    std::array<float, 3> foam_colour(float x, float y) const noexcept;

  private:

    /// Averages the map's picture over cells and keeps the foam's colour
    /// for each.
    /// @param map the match's map
    /// @param palette the game's palette, as reset() takes it
    void reset_foam(const formats::tnt::Map& map, std::span<const uint8_t> palette);

    uint32_t width_{}, height_{}, stride_{1};
    float world_width_{}, world_height_{};
    std::vector<float> fetch_;
    /// The foam's colour in cells of foam_cell_ picture pixels a side,
    /// foam_width_ across; empty without a palette.
    uint32_t foam_width_{}, foam_height_{};
    float foam_cell_{16};
    std::vector<std::array<uint8_t, 3>> foam_;
};
} // namespace oa::app::renderer_water
