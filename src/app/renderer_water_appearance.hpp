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
} // namespace oa::app::renderer_water_appearance
