// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A chip's state mark while shape cues are in effect. Each state draws its
// tone's mark, a filter draws none, and with the cues off a chip's width
// and pixels are today's.
#include "oa/formats/fnt.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/state_colours.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;
namespace vision = oa::present::colour_vision;

namespace {

struct RestoreMode {
    ~RestoreMode() { vision::set_active(vision::Mode::off, false); }
};

constexpr kit::Colour kBlack{0, 0, 0};

renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

/// The synthetic font of the settings pixel test, for both roles.
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

kit::Canvas canvas_on(renderer::Surface& surface) {
    return {&surface, {0, 0, 1, {}}, &fonts(), {}};
}

kit::Colour pixel(const renderer::Surface& surface, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

void expect(const renderer::Surface& surface, int32_t x, int32_t y, kit::Colour colour) {
    const kit::Colour got = pixel(surface, x, y);
    if (got != colour) {
        std::fprintf(
            stderr,
            "pixel %d,%d is #%02x%02x%02x, expected #%02x%02x%02x\n",
            x,
            y,
            got.r,
            got.g,
            got.b,
            colour.r,
            colour.g,
            colour.b
        );
        OA_CHECK(false);
    }
}

renderer::Surface draw(const kit::ChipLook& look) {
    const int32_t width = kit::chip_width(fonts(), look);
    const int32_t height = look.icon.has_value() ? kit::compact_metrics.state_icon_side
                                                 : kit::compact_metrics.chip_height;
    renderer::Surface surface = black(width + 4, height + 4);
    kit::draw_chip(canvas_on(surface), {0, 0, width, height}, look);
    return surface;
}

/// With the cues off, the pinned widths and the pinned GET pixels hold.
void cues_off_match_today() {
    vision::set_active(vision::Mode::off, false);
    OA_CHECK(kit::chip_width(fonts(), {"GET"}) == 22);
    OA_CHECK(kit::chip_width(fonts(), {"UPDATE"}) == 27);
    OA_CHECK(kit::chip_width(fonts(), {""}) == 12);
    OA_CHECK(kit::chip_width(fonts(), {"GET", kit::ChipState::filter}) == 22);
    OA_CHECK(kit::chip_width(fonts(), {"GET", kit::ChipState::tag}) == 22);

    renderer::Surface surface = black(40, 30);
    kit::draw_chip(canvas_on(surface), {10, 10, 18, 12}, {"GET", kit::ChipState::get});
    expect(surface, 10, 10, kBlack);
    expect(surface, 27, 21, kBlack);
    expect(surface, 19, 10, kit::colour::control_hover);
    expect(surface, 10, 16, kit::colour::control_hover);
    expect(surface, 11, 16, kBlack);
    expect(surface, 14, 11, kit::colour::text_secondary);
}

void marks_follow_the_tone() {
    vision::set_active(vision::Mode::off, true);
    const auto mark = [](kit::ChipState state, int32_t x, int32_t y, kit::Colour ink) {
        const renderer::Surface surface = draw({"GET", state});
        expect(surface, x, y, ink);
    };
    // The check, the warning and the cross sit where the chip centres an
    // 8-row mark. The "i" is centred in the room the chip adds.
    const kit::Colour ok = kit::state_ink(kit::StateTone::ok);
    mark(kit::ChipState::installed, 9, 3, ok);
    mark(kit::ChipState::playing, 9, 3, kit::colour::on_accent);
    mark(kit::ChipState::update, 6, 3, kit::state_ink(kit::StateTone::caution));
    mark(kit::ChipState::problem, 2, 3, kit::state_ink(kit::StateTone::danger));
    const kit::Colour note = kit::state_ink(kit::StateTone::information);
    mark(kit::ChipState::get, 4, 2, note);
    mark(kit::ChipState::online, 4, 2, note);

    const int32_t plain = kit::chip_width(fonts(), {"GET", kit::ChipState::get});
    vision::set_active(vision::Mode::off, false);
    const int32_t today = kit::chip_width(fonts(), {"GET", kit::ChipState::get});
    vision::set_active(vision::Mode::off, true);
    OA_CHECK(plain == today + 12);
}

/// A filter, a tag and an icon chip of either gain no mark and no room.
void a_filter_draws_no_mark() {
    const auto unchanged = [](kit::ChipLook look) {
        vision::set_active(vision::Mode::off, false);
        const int32_t width = kit::chip_width(fonts(), look);
        const renderer::Surface off = draw(look);
        vision::set_active(vision::Mode::off, true);
        OA_CHECK(kit::chip_width(fonts(), look) == width);
        const renderer::Surface on = draw(look);
        OA_CHECK(off.rgb == on.rgb);
    };
    unchanged({"GET", kit::ChipState::filter});
    unchanged({"GET", kit::ChipState::tag});
    kit::ChipLook filtered;
    filtered.state = kit::ChipState::filter;
    filtered.icon = kit::StateIcon::oa;
    unchanged(filtered);
    kit::ChipLook tagged;
    tagged.state = kit::ChipState::tag;
    tagged.icon = kit::StateIcon::bone;
    unchanged(tagged);
    // The gallery's maker's marks leave the state at get, and the circled
    // check is already the picture of its state. None of them gains a mark.
    kit::ChipLook oa;
    oa.icon = kit::StateIcon::oa;
    oa.text = "OA";
    unchanged(oa);
    kit::ChipLook bone;
    bone.icon = kit::StateIcon::bone;
    bone.text = "Bone";
    unchanged(bone);
    kit::ChipLook check;
    check.icon = kit::StateIcon::circled_check;
    unchanged(check);
}

} // namespace

int main() {
    const RestoreMode restore;
    cues_off_match_today();
    marks_follow_the_tone();
    a_filter_draws_no_mark();
    vision::set_active(vision::Mode::off, false);
    return oa::test::check_exit_status();
}
