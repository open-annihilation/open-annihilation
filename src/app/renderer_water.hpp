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

/// Bounded coarse water-body metadata rebuilt once for each loaded match.
class SurfaceCache {
  public:

    /// Sample water connectivity and local fetch without retaining the map.
    void reset(const formats::tnt::Map& map);
    /// Local wave strength, zero outside water and reduced on small enclosed bodies.
    float fetch(float x, float z) const noexcept;
    /// fetch blended between the centres of the coarse cells around a point,
    /// so that it changes evenly from cell to cell; zero outside the map.
    float smooth_fetch(float x, float z) const noexcept;

  private:

    uint32_t width_{}, height_{}, stride_{1};
    float world_width_{}, world_height_{};
    std::vector<float> fetch_;
};
} // namespace oa::app::renderer_water
