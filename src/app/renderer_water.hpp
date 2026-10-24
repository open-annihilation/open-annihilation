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

  private:

    uint32_t width_{}, height_{}, stride_{1};
    float world_width_{}, world_height_{};
    std::vector<float> fetch_;
};
} // namespace oa::app::renderer_water
