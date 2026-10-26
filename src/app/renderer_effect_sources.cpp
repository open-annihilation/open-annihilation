// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_effect_sources.hpp"

#include <algorithm>

namespace oa::app::renderer_effect_sources {

bool observe(Timeline& timeline, uint32_t tick, std::size_t wreck_count) {
    const bool reset =
        !timeline.seen || tick < timeline.tick || wreck_count < timeline.wrecks.size();
    if (reset) {
        timeline.wrecks.assign(wreck_count, {});
        timeline.first_live = 0;
    } else {
        timeline.wrecks.resize(wreck_count, {tick, false});
    }
    timeline.tick = tick;
    timeline.seen = true;
    return reset;
}

std::size_t first_live_wreck(Timeline& timeline) noexcept {
    auto& first = timeline.first_live;
    first = std::min(first, timeline.wrecks.size());
    while (first < timeline.wrecks.size()) {
        const auto& wreck = timeline.wrecks[first];
        if (!wreck.retired && timeline.tick - wreck.birth < wreck_cooling_ticks)
            break;
        ++first;
    }
    return first;
}

void weapon_colour(
    Source& source,
    uint8_t color,
    uint8_t render_type,
    std::span<const uint8_t> palette,
    std::span<const uint8_t> interface_colours
) noexcept {
    constexpr uint8_t laser_render_type = 0;
    constexpr uint8_t lightning_render_type = 7;
    constexpr std::size_t entry_bytes = 4;
    constexpr float channel_scale = 255.0F;
    constexpr float minimum_colour = .15F;
    source.red = 1;
    source.green = .58F;
    source.blue = .18F;
    if (color == 0 || (render_type != laser_render_type && render_type != lightning_render_type) ||
        color >= interface_colours.size())
        return;
    const auto index = static_cast<std::size_t>(interface_colours[color]) * entry_bytes;
    if (index + 2 >= palette.size())
        return;
    const float red = palette[index] / channel_scale;
    const float green = palette[index + 1] / channel_scale;
    const float blue = palette[index + 2] / channel_scale;
    if (std::max({red, green, blue}) < minimum_colour)
        return;
    source.red = red;
    source.green = green;
    source.blue = blue;
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
