// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::renderer_effect_sources {

enum class Kind { weapon, nanolathe, wreck };
enum class Origin { muzzle, projectile, impact, wreck, nano_stream, construction };

/// Describe a presentation light in world pixels, independent of camera and renderer.
struct Source {
    float x{}, height{}, z{};
    float red{}, green{}, blue{};
    float intensity{}, radius{}, glow{}, heat{};
    Kind kind{};
    Origin origin{};
};

inline constexpr uint32_t wreck_cooling_ticks = 900;
inline constexpr uint32_t muzzle_flash_ticks = 6;
inline constexpr std::size_t maximum_sources = 128;

/// Gently vary construction emission using simulation time, freezing when paused.
/// @param tick current simulation tick
/// @return brightness multiplier in [0.85,1.15], repeating every two game seconds
inline float construction_pulse(uint32_t tick) noexcept {
    constexpr uint32_t period_ticks = 60;
    constexpr float full_turn = 6.28318530718F;
    constexpr float amplitude = 0.15F;
    return 1.0F - amplitude * std::cos(full_turn * (tick % period_ticks) / period_ticks);
}

/// Keep the birth time and permanent retirement of append-only wreck entries.
struct WreckHeat {
    uint32_t birth{};
    bool retired{true};
};

/// Keep visual ages only; never write a simulation record or use wall time.
struct Timeline {
    bool seen{};
    uint32_t tick{};
    std::vector<WreckHeat> wrecks;
    /// Wrecks before this index never glow again: each is retired or cooled.
    std::size_t first_live{};
};

/// Observe a tick and append-only corpse count; seed existing wrecks cold on first view or rewind.
/// @param[in,out] timeline visual ages
/// @param tick simulation tick
/// @param wreck_count canonical append-only corpse count
/// @return true when history was reset
bool observe(Timeline& timeline, uint32_t tick, std::size_t wreck_count);

/// Return a wreck's cooling strength, retiring it permanently when its feature vanishes.
/// @param[in,out] timeline visual ages
/// @param index canonical corpse entry index
/// @param present true while this entry still identifies the feature at its cell
/// @param visible true while its world position is in current sight
/// @return heat in [0,1]; hidden wrecks still age
float wreck_heat(Timeline& timeline, std::size_t index, bool present, bool visible);

/// Moves past the leading wrecks that can no longer glow, retired or cooled.
/// Births never decrease along the list, so every wreck after the first one
/// still cooling is younger than it.
/// @param[in,out] timeline visual ages
/// @return the index of the first wreck that may still glow
std::size_t first_live_wreck(Timeline& timeline) noexcept;

/// Colour a weapon's light as its shot is drawn. A laser's and a lightning
/// bolt's `color` names an entry of the interface palette (GUIPAL.PAL), and
/// the beam is drawn in the battlefield palette's entry nearest it, not in
/// the battlefield entry of that number; plasma's picks its sprite. Plasma,
/// every other shot and a missing or near-black colour light warm, as
/// physical ammunition does.
/// @param[in,out] source light whose red, green and blue are set
/// @param color the weapon's `color`
/// @param render_type the weapon's `rendertype`
/// @param palette battlefield palette, four bytes an entry
/// @param interface_colours the battlefield entry nearest each interface entry
void weapon_colour(
    Source& source,
    uint8_t color,
    uint8_t render_type,
    std::span<const uint8_t> palette,
    std::span<const uint8_t> interface_colours
) noexcept;

/// Return the remaining strength of a transient light at simulation time.
/// @param birth source tick
/// @param tick current tick
/// @param duration lifetime in ticks
/// @return linear fade in [0,1], zero before birth or at expiry
float fade(uint32_t birth, uint32_t tick, uint32_t duration) noexcept;

} // namespace oa::app::renderer_effect_sources
