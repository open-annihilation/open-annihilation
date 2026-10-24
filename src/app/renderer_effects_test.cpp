// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Optional effects preserve hidden pixels, depth edges, alpha, and paused animation.
#include "renderer_effects.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <limits>
#include <vector>

namespace {
using namespace oa::app::renderer_effects;
constexpr int width = 24;
constexpr int height = 16;
constexpr int channels = 4;
constexpr int stride = width * channels + 8;
constexpr size_t pixel_count = width * height;

/// Include row padding and alpha so writes outside RGB are observable.
std::vector<uint8_t> picture() {
    std::vector<uint8_t> result(stride * height, 73);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = size_t(y * stride + x * channels);
            result[offset] = uint8_t(20 + x * 7);
            result[offset + 1] = uint8_t(30 + y * 5);
            result[offset + 2] = 40;
        }
    }
    return result;
}

/// Provide a compact warm source in the middle of the battlefield.
Source source() {
    return {12, 8, 0, 6, 10, 1, 0.4f, 0, 1, 1, 1, 0};
}

/// Wrap padded RGBA pixels with optional tightly packed visibility and depth.
Surface surface(
    std::vector<uint8_t>& pixels,
    std::span<const uint8_t> visibility = {},
    std::span<const float> depth = {}
) {
    return {pixels, width, height, stride, channels, visibility, depth};
}

/// Disabled effects, absent sources and rejected inputs are exact no-ops.
void unchanged_frames() {
    const auto original = picture();
    auto pixels = original;
    const std::array sources{source()};
    compose(surface(pixels), sources, {}, 0);
    OA_CHECK(pixels == original);
    compose(surface(pixels), {}, {true, true, true}, 0);
    OA_CHECK(pixels == original);
    auto invalid = source();
    invalid.screen_x = std::numeric_limits<float>::infinity();
    compose(surface(pixels), std::span(&invalid, 1), {true, true, true}, 0);
    OA_CHECK(pixels == original);
    auto bad_surface = surface(pixels);
    bad_surface.stride_bytes = 1;
    compose(bad_surface, sources, {true, true, true}, 0);
    OA_CHECK(pixels == original);
}

/// Lighting has finite support, keeps colour, and leaves alpha and row padding alone.
void light_support() {
    const auto original = picture();
    auto pixels = original;
    const std::array sources{source()};
    compose(surface(pixels), sources, {true, false, false}, 0);
    const size_t centre = 8 * stride + 12 * channels;
    OA_CHECK(pixels[centre] > original[centre]);
    OA_CHECK(pixels[centre + 1] > original[centre + 1]);
    OA_CHECK(pixels[centre + 2] == original[centre + 2]);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = size_t(y * stride + x * channels);
            OA_CHECK(pixels[offset + 3] == original[offset + 3]);
            if ((x - 12) * (x - 12) + (y - 8) * (y - 8) >= 36) {
                for (int channel = 0; channel < channels; ++channel)
                    OA_CHECK(
                        pixels[offset + size_t(channel)] == original[offset + size_t(channel)]
                    );
            }
        }
        for (int padding = width * channels; padding < stride; ++padding)
            OA_CHECK(pixels[size_t(y * stride + padding)] == 73);
    }
}

/// Unseen pixels and distant foreground cannot receive glow or distorted hidden content.
void visibility_and_depth() {
    const auto original = picture();
    auto pixels = original;
    std::array<uint8_t, pixel_count> visibility{};
    std::array<float, pixel_count> depth{};
    visibility.fill(255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = size_t(y * width + x);
            if (x >= 12)
                visibility[index] = 0;
            if (x < 12 && y >= 8)
                depth[index] = 100;
        }
    }
    const std::array sources{source()};
    compose(surface(pixels, visibility, depth), sources, {true, true, true}, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x < 12 && y < 8)
                continue;
            const auto offset = size_t(y * stride + x * channels);
            for (int channel = 0; channel < channels; ++channel)
                OA_CHECK(pixels[offset + size_t(channel)] == original[offset + size_t(channel)]);
        }
    }
    OA_CHECK(pixels != original);
    depth.fill(std::numeric_limits<float>::quiet_NaN());
    pixels = original;
    compose(surface(pixels, {}, depth), sources, {true, true, true}, 0);
    OA_CHECK(pixels == original);
}

/// Heat never pulls a hidden pixel or a different depth layer across its boundary.
void heat_boundaries() {
    std::vector<uint8_t> pixels(stride * height, 0);
    std::array<uint8_t, pixel_count> visibility{};
    std::array<float, pixel_count> depth{};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = size_t(y * width + x);
            visibility[index] = x < 12 ? 255 : 0;
            depth[index] = x < 12 ? 0 : 5;
            for (int channel = 0; channel < 3; ++channel)
                pixels[size_t(y * stride + x * channels + channel)] = x < 12 ? 20 : 230;
        }
    }
    const auto original = pixels;
    const std::array sources{source()};
    compose(surface(pixels, visibility), sources, {false, false, true}, 0);
    OA_CHECK(pixels == original);
    compose(surface(pixels, {}, depth), sources, {false, false, true}, 0);
    OA_CHECK(pixels == original);
}

/// Heat alone moves existing colour, repeats at a paused tick, and changes on later ticks.
void paused_heat() {
    const auto original = picture();
    auto first = original;
    auto repeated = original;
    auto later = original;
    const std::array sources{source()};
    compose(surface(first), sources, {false, false, true}, 0);
    compose(surface(repeated), sources, {false, false, true}, 0);
    compose(surface(later), sources, {false, false, true}, 0.2);
    OA_CHECK(first != original);
    OA_CHECK(first == repeated);
    OA_CHECK(first != later);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = size_t(y * stride + x * channels);
            OA_CHECK(first[offset + 1] == original[offset + 1]);
            OA_CHECK(first[offset + 2] == original[offset + 2]);
        }
    }
}

/// Fractional heat moves smoothly across integer pixel boundaries without altering alpha.
void fractional_heat() {
    const auto original = picture();
    auto first = original;
    auto later = original;
    auto heat = source();
    heat.phase = 0.27F - 8 * 0.55F;
    compose(surface(first), {&heat, 1}, {false, false, true}, 0);
    compose(surface(later), {&heat, 1}, {false, false, true}, 0.002);
    constexpr size_t centre = 8 * stride + 12 * channels;
    OA_CHECK(first[centre] > original[centre]);
    OA_CHECK(first[centre] < original[centre + channels]);
    OA_CHECK(later[centre] > original[centre]);
    OA_CHECK(first[centre] - later[centre] <= 1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto offset = size_t(y * stride + x * channels);
            OA_CHECK(first[offset + 3] == original[offset + 3]);
            OA_CHECK(later[offset + 3] == original[offset + 3]);
        }
        for (int padding = width * channels; padding < stride; ++padding) {
            OA_CHECK(first[size_t(y * stride + padding)] == original[size_t(y * stride + padding)]);
            OA_CHECK(later[size_t(y * stride + padding)] == original[size_t(y * stride + padding)]);
        }
    }
}

/// Glow can be enabled independently, and the RGB surface path matches RGBA RGB channels.
void independent_glow_and_rgb() {
    const auto original = picture();
    auto rgba = original;
    std::vector<uint8_t> rgb(pixel_count * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel)
                rgb[size_t((y * width + x) * 3 + channel)] =
                    original[size_t(y * stride + x * channels + channel)];
        }
    }
    const std::array sources{source()};
    compose(surface(rgba), sources, {false, true, false}, 0);
    compose({rgb, width, height, width * 3, 3}, sources, {false, true, false}, 0);
    OA_CHECK(rgba != original);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel)
                OA_CHECK(
                    rgb[size_t((y * width + x) * 3 + channel)] ==
                    rgba[size_t(y * stride + x * channels + channel)]
                );
        }
    }
}

/// Elevated weapon and nanolathe light reaches both terrain and raised unit surfaces.
void terrain_and_units() {
    const auto original = picture();
    std::array<uint8_t, pixel_count> visibility{};
    std::array<float, pixel_count> depths{};
    visibility.fill(255);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            depths[size_t(y * width + x)] = float(y) * 0.5F - 3;
    // Terrain at height -2.4 and an emitter at height 4: screen y = z - height/2,
    // camera depth = height + z/2. The ground is inside the world-space radius
    // even though its camera-depth difference equals that radius. A raised
    // model occupies the adjacent pixel.
    depths[8 * width + 13] = 9;
    depths[7 * width + 12] = 100;
    visibility[8 * width + 11] = 0;
    for (const auto colour : {std::array{1.0F, 0.4F, 0.0F}, std::array{0.1F, 1.0F, 0.24F}}) {
        auto pixels = original;
        constexpr float projection_scale = 1.11803399F;
        const Source emitter{
            12,
            8,
            9,
            8,
            8 * projection_scale,
            colour[0],
            colour[1],
            colour[2],
            1,
            0,
            0,
            0,
            8 * projection_scale
        };
        compose(surface(pixels, visibility, depths), {&emitter, 1}, {true, false, false}, 0);
        const int channel = colour[0] == 1 ? 0 : 1;
        OA_CHECK(
            pixels[8 * stride + 12 * channels + channel] >
            original[8 * stride + 12 * channels + channel]
        );
        OA_CHECK(
            pixels[8 * stride + 13 * channels + channel] >
            original[8 * stride + 13 * channels + channel]
        );
        OA_CHECK(
            pixels[7 * stride + 12 * channels + channel] ==
            original[7 * stride + 12 * channels + channel]
        );
        OA_CHECK(
            pixels[8 * stride + 11 * channels + channel] ==
            original[8 * stride + 11 * channels + channel]
        );
    }
}

/// Percent levels scale energy and displacement without expanding a source's footprint.
void intensity_levels() {
    const auto original = picture();
    constexpr size_t centre = 8 * stride + 12 * channels;
    auto light = source();
    light.red = 0.2F;
    light.green = 0.1F;
    int previous_light = original[centre];
    int previous_glow = original[centre];
    for (int level = 0; level <= 4; ++level) {
        const float multiplier = level * 0.5F;
        light.light = multiplier;
        light.glow = multiplier;
        auto lit = original;
        auto glowing = original;
        compose(surface(lit), {&light, 1}, {true, false, false}, 0);
        compose(surface(glowing), {&light, 1}, {false, true, false}, 0);
        if (level == 0) {
            OA_CHECK(lit == original);
            OA_CHECK(glowing == original);
        } else {
            OA_CHECK(lit[centre] > previous_light);
            OA_CHECK(glowing[centre] > previous_glow);
        }
        if (level == 2) {
            OA_CHECK(lit[centre] == original[centre] + 22);
            OA_CHECK(glowing[centre] == original[centre] + 30);
        }
        previous_light = lit[centre];
        previous_glow = glowing[centre];
        OA_CHECK(lit[0] == original[0]);
        OA_CHECK(glowing[0] == original[0]);
        auto heat = source();
        heat.heat = multiplier;
        // Place the centre on the positive crest of the wave, so each 50% adds one pixel.
        heat.phase = 1.57079632679F - 8 * 0.55F;
        auto distorted = original;
        compose(surface(distorted), {&heat, 1}, {false, false, true}, 0);
        OA_CHECK(distorted[centre] == original[centre + level * channels]);
        if (level == 0)
            OA_CHECK(distorted == original);
    }
}

/// Tiny footprints disappear continuously; heat fades sooner than coloured light.
void distance_detail() {
    OA_CHECK(detail_strength(1, 1)[0] == 0);
    OA_CHECK(detail_strength(8, 1)[0] == 1);
    float previous = 0;
    for (int step = 0; step <= 70; ++step) {
        const float light = detail_strength(1 + step * 0.1F, 1)[0];
        OA_CHECK(light >= previous && light - previous < 0.025F);
        previous = light;
    }
    OA_CHECK(detail_strength(20, 0.35F)[1] == 0);
    OA_CHECK(detail_strength(20, 0.7F)[1] > 0);
    OA_CHECK(detail_strength(20, 0.7F)[1] < detail_strength(20, 1)[1]);
    auto pixels = picture();
    const auto original = pixels;
    auto tiny_heat = source();
    tiny_heat.heat = 0.24F;
    compose(surface(pixels), {&tiny_heat, 1}, {false, false, true}, 0);
    OA_CHECK(pixels == original);
}
} // namespace

int main() {
    intensity_levels();
    terrain_and_units();
    distance_detail();
    unchanged_frames();
    light_support();
    visibility_and_depth();
    heat_boundaries();
    paused_heat();
    fractional_heat();
    independent_glow_and_rgb();
    return oa::test::check_exit_status();
}
