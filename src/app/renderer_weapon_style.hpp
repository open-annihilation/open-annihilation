// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/weapon_def.h"
#include "renderer_effect_sources.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace oa::app::renderer_weapon_style {

inline constexpr uint32_t emg_impact_ticks = 10;
inline constexpr uint32_t emg_muzzle_ticks = 3;
inline constexpr float emg_impact_merge_radius = 24;

/// Identify the installed EMG weapon family without changing other plasma weapons.
/// @param weapon canonical weapon definition
/// @return true for an EMG definition key, ignoring ASCII case
inline bool is_emg(const oa::WeaponDef& weapon) noexcept {
    const auto end = std::find(std::begin(weapon.key), std::end(weapon.key), '\0');
    std::string_view key(weapon.key, static_cast<std::size_t>(end - weapon.key));
    const auto matches = [&](std::string_view expected) {
        if (key.size() != expected.size())
            return false;
        for (std::size_t index = 0; index < key.size(); ++index) {
            const char letter = key[index];
            const char lower = letter >= 'A' && letter <= 'Z' ? letter + ('a' - 'A') : letter;
            if (lower != expected[index])
                return false;
        }
        return true;
    };
    return matches("emg") || matches("emg2") || matches("vtol_emg");
}

/// Apply compact amber lighting to a rapid EMG round, muzzle or impact.
/// @param[in,out] source light whose origin has already been assigned
inline void apply_emg_light(renderer_effect_sources::Source& source) noexcept {
    using renderer_effect_sources::Origin;
    source.red = 1;
    source.green = .68F;
    source.blue = .22F;
    source.heat = 0;
    if (source.origin == Origin::impact) {
        source.radius = 60;
        source.intensity = .5F;
    } else if (source.origin == Origin::muzzle) {
        source.radius = 36;
        source.intensity = .35F;
    } else {
        source.radius = 18;
        source.intensity = .12F;
    }
}

/// Combine nearby EMG impacts into one bounded hotspot, retaining its stable location.
/// @param[in,out] hotspot existing EMG impact light
/// @param impact incoming EMG impact light
/// @return true when the lights were close enough to combine
inline bool merge_emg_impact(
    renderer_effect_sources::Source& hotspot, const renderer_effect_sources::Source& impact
) noexcept {
    const float x = hotspot.x - impact.x;
    const float z = hotspot.z - impact.z;
    constexpr float height_tolerance = 16;
    if (x * x + z * z > emg_impact_merge_radius * emg_impact_merge_radius ||
        std::abs(hotspot.height - impact.height) > height_tolerance)
        return false;
    constexpr float light_increment = .035F;
    constexpr float maximum_light = .85F;
    constexpr float heat_increment = .025F;
    constexpr float maximum_heat = .3F;
    hotspot.intensity = std::min(maximum_light, hotspot.intensity + light_increment);
    hotspot.heat = std::min(maximum_heat, hotspot.heat + heat_increment);
    return true;
}

} // namespace oa::app::renderer_weapon_style
