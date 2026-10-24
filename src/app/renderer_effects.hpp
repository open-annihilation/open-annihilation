// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <array>
#include <span>

namespace oa::app::renderer_effects {

/// Select independent presentation effects without changing simulation state.
struct Settings {
    bool coloured_lighting{};
    bool wreck_glow{};
    bool heat_sizzle{};
};

/// Describe a visible source after camera projection; RGB is in [0,1] and effect strengths are in [0,2].
struct Source {
    float screen_x{};
    float screen_y{};
    float depth{}; ///< larger is nearer
    float radius_pixels{};
    float depth_radius{}; ///< positive depth reach; used only with a depth surface
    float red{};
    float green{};
    float blue{};
    float light{};
    float glow{}; ///< hot wreck emission, controlled separately from lighting
    float heat{};
    float phase{};           ///< stable source phase in radians
    float radius_y_pixels{}; ///< vertical projected reach; zero uses radius_pixels
};

/// Describe battlefield pixels only; exclude HUD and command overlays.
struct Surface {
    std::span<uint8_t> pixels{};
    int width{};
    int height{};
    int stride_bytes{};
    int channels{3}; ///< packed RGB or RGBA; alpha is preserved
    std::span<const uint8_t>
        visibility{};               ///< optional width*height attenuation, zero protects pixels
    std::span<const float> depth{}; ///< optional width*height camera depth; nonfinite is empty
};

/// Fade subpixel light footprints and reduce heat as the camera zooms out.
/// @param radius_pixels radius at displayed layout resolution, before supersampling
/// @param zoom world-to-layout scale
/// @return light and heat strength multipliers, respectively
std::array<float, 2> detail_strength(float radius_pixels, float zoom) noexcept;

/// Composite at most 128 visible sources, with radii bounded to 256 pixels.
/// Source visibility must be checked before projection. Visibility prevents both
/// sampling and writing hidden pixels; depth limits light reach and rejects shimmer
/// across silhouettes. Disabled effects and invalid surfaces leave pixels unchanged.
/// @param surface mutable battlefield and optional tightly packed masks
/// @param sources visible projected sources, finite values and effect strengths in [0,2]
/// @param settings independent light, wreck glow and distortion switches
/// @param simulation_seconds simulation time, held constant while paused
void compose(
    Surface surface, std::span<const Source> sources, Settings settings, double simulation_seconds
);

} // namespace oa::app::renderer_effects
