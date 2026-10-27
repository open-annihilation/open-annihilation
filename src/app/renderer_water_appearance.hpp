// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace oa::app::renderer_water_appearance {
/// Shared water appearance for classic geometry and the live fragment shader.
inline constexpr float immersion_distance = 4.0F;
inline constexpr float tint_distance = 64.0F;
inline constexpr float tint_base = 0.20F;
inline constexpr float tint_depth = 0.45F;
inline constexpr float tint_max = 0.70F;
inline constexpr float tint_red = 0.30F;
inline constexpr float tint_green = 0.65F;
inline constexpr float tint_blue = 0.78F;
inline constexpr float absorption_base = 0.06F;
inline constexpr float absorption_depth = 0.10F;
inline constexpr float absorption_max = 0.60F;
inline constexpr float caustic_gain = 0.35F;
inline constexpr float caustic_bias = -0.08F;
inline constexpr float caustic_fade_start = 16.0F;
inline constexpr float caustic_fade_range = 104.0F;
inline constexpr float rim_peak_depth = 0.75F;
inline constexpr float rim_width = 2.0F;
inline constexpr float rim_strength = 0.18F;
inline constexpr float rim_red = 0.55F;
inline constexpr float rim_green = 0.85F;
inline constexpr float rim_blue = 1.0F;
inline constexpr float reflection_opacity = 0.23F;
inline constexpr float reflection_cap = 0.46F;
inline constexpr float reflection_brightness = 0.90F;
/// Coastal waves' foam at 100% where its cover is whole on open water at
/// full wind, before foam_cap holds it.
inline constexpr float foam_strength = 1.0F;
inline constexpr float foam_cap = 0.85F;
/// The share of foam_strength the most sheltered water keeps.
inline constexpr float foam_sheltered = 0.35F;
/// The foam's plain colour, where the colour of the ground under it is not
/// known.
inline constexpr float foam_red = 0.88F;
inline constexpr float foam_green = 0.94F;
inline constexpr float foam_blue = 0.97F;
/// The foam over ground of some colour: the share of the way from the
/// ground's lightness to white that the foam's lightness lies, and the share
/// of the ground's own colour (its difference from grey) the foam keeps, so
/// that it takes the shore's hue and is lighter than the ground in every
/// channel.
inline constexpr float foam_lift = 0.55F;
inline constexpr float foam_chroma = 0.9F;
} // namespace oa::app::renderer_water_appearance
