// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A centred crop, and the guide that follows the one showing.
//
// oa-ui-photo-panel-crop-test

#include "oa/test/check.hpp"
#include "oa/ui/photo_panel/crop.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace panel = oa::ui::photo_panel;

/// One pixel of packed RGB.
std::vector<uint8_t>
pixel_at(const std::vector<uint8_t>& rgb, uint32_t width, uint32_t x, uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3U;
    return {rgb[at], rgb[at + 1U], rgb[at + 2U]};
}

} // namespace

int main() {
    panel::Guide guide = panel::Guide::off;
    for (int step = 0; step < static_cast<int>(panel::Guide::count); ++step)
        guide = panel::next_guide(guide);
    OA_CHECK(guide == panel::Guide::off);
    OA_CHECK(panel::next_guide(panel::Guide::crop_21_9) == panel::Guide::off);
    OA_CHECK(panel::guide_name(panel::Guide::thirds) == "Thirds");
    OA_CHECK(panel::guide_name(panel::Guide::crop_1_1) == "1:1");
    OA_CHECK(panel::guide_name(panel::Guide::off) == "Off");

    int width_parts = 0;
    int height_parts = 0;
    OA_CHECK(!panel::crop_ratio(panel::Guide::thirds, width_parts, height_parts));
    OA_CHECK(panel::crop_ratio(panel::Guide::crop_1_1, width_parts, height_parts));
    OA_CHECK(width_parts == 1 && height_parts == 1);

    constexpr uint32_t width = 8;
    constexpr uint32_t height = 4;
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3U;
            rgb[at] = static_cast<uint8_t>(x);
            rgb[at + 1U] = static_cast<uint8_t>(y);
            rgb[at + 2U] = 7;
        }
    }
    const std::vector<uint8_t> centre = pixel_at(rgb, width, 4, 2);
    uint32_t cropped_w = width;
    uint32_t cropped_h = height;
    std::vector<uint8_t> kept = rgb;
    panel::crop_centred(kept, cropped_w, cropped_h, panel::Guide::thirds);
    OA_CHECK(cropped_w == width && cropped_h == height && kept == rgb);

    panel::crop_centred(rgb, cropped_w, cropped_h, panel::Guide::crop_1_1);
    OA_CHECK(cropped_w == cropped_h && cropped_w == 4);
    OA_CHECK(pixel_at(rgb, cropped_w, cropped_w / 2U, cropped_h / 2U) == centre);

    const oa::ui::kit::Rect square = panel::crop_bounds(1280, 720, panel::Guide::crop_1_1);
    OA_CHECK(square.width == 720 && square.height == 720);
    OA_CHECK(square.x == 280 && square.y == 0);
    OA_CHECK(panel::crop_bounds(1280, 720, panel::Guide::off).width == 0);

    return oa::test::check_exit_status();
}
