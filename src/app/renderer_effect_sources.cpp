// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_effect_sources.hpp"

namespace oa::app::renderer_effect_sources {

bool observe(Timeline& timeline, uint32_t tick, std::size_t wreck_count) {
    const bool reset =
        !timeline.seen || tick < timeline.tick || wreck_count < timeline.wrecks.size();
    if (reset) {
        timeline.wrecks.assign(wreck_count, {});
    } else {
        timeline.wrecks.resize(wreck_count, {tick, false});
    }
    timeline.tick = tick;
    timeline.seen = true;
    return reset;
}

float fade(uint32_t birth, uint32_t tick, uint32_t duration) noexcept {
    if (tick < birth || duration == 0 || tick - birth >= duration)
        return 0;
    return 1.0F - static_cast<float>(tick - birth) / static_cast<float>(duration);
}

float wreck_heat(Timeline& timeline, std::size_t index, bool present, bool visible) {
    if (index >= timeline.wrecks.size())
        return 0;
    auto& wreck = timeline.wrecks[index];
    if (!present)
        wreck.retired = true;
    if (wreck.retired || !visible)
        return 0;
    const float strength = fade(wreck.birth, timeline.tick, wreck_cooling_ticks);
    return strength * strength;
}

} // namespace oa::app::renderer_effect_sources
