// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each colour-vision mode's health, build-site and resource slots, and the
// kit's ok, caution, danger and information colours. Off leaves both as
// they are today, and no mode moves the focus ring.
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/state_colours.hpp"
#include "oa/ui/kit/theme.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace kit = oa::ui::kit;
namespace vision = oa::present::colour_vision;

namespace {

struct RestoreMode {
    ~RestoreMode() { vision::set_active(vision::Mode::off, false); }
};

constexpr int kEntries = vision::kPaletteColours;
constexpr std::size_t kBytes = static_cast<std::size_t>(kEntries) * 4U;
constexpr std::size_t kGood = 10;
constexpr std::size_t kBad = 12;
constexpr std::size_t kMid = 14;
constexpr std::size_t kRefused = 4;
constexpr std::size_t kText = 15;

void plant(std::span<uint8_t> palette, int index, uint8_t red, uint8_t green, uint8_t blue) {
    const auto at = static_cast<std::size_t>(index) * 4U;
    palette[at] = red;
    palette[at + 1] = green;
    palette[at + 2] = blue;
}

/// A palette whose planted entries are the mode colours of §4.3, each the
/// only exact match of that sRGB value.
std::array<uint8_t, kBytes> sample_palette() {
    std::array<uint8_t, kBytes> palette{};
    plant(palette, 100, 0x7B, 0x97, 0xFF);
    plant(palette, 64, 0xFB, 0xFB, 0xD7);
    plant(palette, 212, 0xD3, 0x2B, 0x00);
    plant(palette, 226, 0x43, 0x6F, 0xEF);
    plant(palette, 193, 0xFB, 0xF3, 0x83);
    plant(palette, 80, 0xFB, 0xFB, 0xFB);
    plant(palette, 86, 0x9B, 0x9B, 0x9B);
    plant(palette, 90, 0x5B, 0x5B, 0x5B);
    // Tritanopia's consumed figure and achromatopsia's consumed figure.
    plant(palette, 211, 0xFF, 0x47, 0x00);
    plant(palette, 89, 0x6B, 0x6B, 0x6B);
    return palette;
}

std::array<uint8_t, 256> identity_slots() {
    std::array<uint8_t, 256> slots{};
    for (int index = 0; index < kEntries; ++index)
        slots[static_cast<std::size_t>(index)] = static_cast<uint8_t>(index);
    return slots;
}

void expect_slots(std::span<const uint8_t> slots, uint8_t good, uint8_t mid, uint8_t bad) {
    OA_CHECK(slots[kGood] == good);
    OA_CHECK(slots[kMid] == mid);
    OA_CHECK(slots[kBad] == bad);
    OA_CHECK(slots[kRefused] == bad);
    OA_CHECK(slots[kText] == static_cast<uint8_t>(kText));
    OA_CHECK(slots[0] == 0);
    OA_CHECK(slots[11] == 11);
}

void slots_follow_the_mode() {
    const std::array<uint8_t, kBytes> palette = sample_palette();
    std::array<uint8_t, 256> slots = identity_slots();
    slots[kGood] = 7;
    vision::paint_mark_slots(slots, palette, vision::Mode::off);
    OA_CHECK(slots[kGood] == 7);

    std::array<uint8_t, 4> too_few{1, 2, 3, 4};
    vision::paint_mark_slots(too_few, palette, vision::Mode::protanopia);
    OA_CHECK(too_few[0] == 1);

    slots = identity_slots();
    vision::paint_mark_slots(slots, std::span<const uint8_t>{}, vision::Mode::protanopia);
    OA_CHECK(slots[kGood] == static_cast<uint8_t>(kGood));

    const auto painted = [&](vision::Mode mode) {
        std::array<uint8_t, 256> table = identity_slots();
        vision::paint_mark_slots(table, palette, mode);
        return table;
    };
    expect_slots(painted(vision::Mode::protanopia), 100, 64, 212);
    expect_slots(painted(vision::Mode::deuteranopia), 100, 64, 212);
    expect_slots(painted(vision::Mode::tritanopia), 226, 193, 212);
    expect_slots(painted(vision::Mode::achromatopsia), 80, 86, 90);
}

void resource_figures_follow_the_mode() {
    const std::array<uint8_t, kBytes> palette = sample_palette();
    std::array<uint8_t, 256> held = identity_slots();
    held[kGood] = 233;
    held[kBad] = 211;
    vision::set_active(vision::Mode::off, false);
    vision::paint_mark_slots(held, palette, vision::Mode::off);
    vision::match_resource_figures(palette);
    OA_CHECK(held[kGood] == 233);
    OA_CHECK(held[kBad] == 211);
    OA_CHECK(held[kText] == static_cast<uint8_t>(kText));

    const auto check_mode =
        [&](vision::Mode mode, uint8_t good, uint8_t bad, uint8_t produced, uint8_t consumed) {
            std::array<uint8_t, 256> table = identity_slots();
            vision::set_active(mode, false);
            vision::paint_mark_slots(table, palette, mode);
            vision::match_resource_figures(palette);
            OA_CHECK(table[kGood] == good);
            OA_CHECK(table[kBad] == bad);
            OA_CHECK(table[kText] == static_cast<uint8_t>(kText));
            OA_CHECK(vision::resource_produced_index() == produced);
            OA_CHECK(vision::resource_consumed_index() == consumed);
        };
    check_mode(vision::Mode::protanopia, 100, 212, 226, 212);
    check_mode(vision::Mode::deuteranopia, 100, 212, 226, 212);
    check_mode(vision::Mode::tritanopia, 226, 212, 226, 211);
    check_mode(vision::Mode::achromatopsia, 80, 90, 100, 89);

    vision::set_active(vision::Mode::off, false);
    vision::match_resource_figures(std::span<const uint8_t>{});
    vision::paint_mark_slots(held, palette, vision::Mode::off);
    OA_CHECK(held[kGood] == 233);
    OA_CHECK(held[kBad] == 211);
}

void expect_rgb(kit::Colour ink, uint8_t red, uint8_t green, uint8_t blue) {
    OA_CHECK(ink == (kit::Colour{red, green, blue}));
}

void expect_tone(kit::StateTone tone, uint8_t red, uint8_t green, uint8_t blue) {
    const kit::Colour ink = kit::state_ink(tone);
    expect_rgb(ink, red, green, blue);
    OA_CHECK(kit::state_tint(tone) == kit::mixed(ink, kit::colour::panel, kit::badge_tint));
}

void state_colours_follow_the_mode() {
    vision::set_active(vision::Mode::off, false);
    OA_CHECK(kit::state_ink(kit::StateTone::ok) == kit::colour::accent);
    OA_CHECK(kit::state_ok_words() == kit::colour::accent_light);
    OA_CHECK(kit::state_tint(kit::StateTone::ok) == kit::colour::ok_tint);
    OA_CHECK(kit::state_ink(kit::StateTone::caution) == kit::colour::lock);
    OA_CHECK(kit::state_tint(kit::StateTone::caution) == kit::colour::caution_tint);
    OA_CHECK(kit::state_ink(kit::StateTone::danger) == kit::colour::danger);
    OA_CHECK(kit::state_tint(kit::StateTone::danger) == kit::colour::danger_tint);
    OA_CHECK(kit::state_ink(kit::StateTone::information) == kit::colour::information);
    OA_CHECK(kit::state_tint(kit::StateTone::information) == kit::colour::information_tint);

    const auto shared = [] {
        expect_tone(kit::StateTone::ok, 0x5A, 0xA9, 0xF0);
        expect_tone(kit::StateTone::caution, 0xFF, 0xE6, 0x80);
        expect_tone(kit::StateTone::danger, 0xF4, 0x51, 0x1E);
        expect_tone(kit::StateTone::information, 0xD6, 0xD6, 0xD6);
        OA_CHECK(kit::state_ok_words() == kit::state_ink(kit::StateTone::ok));
    };
    vision::set_active(vision::Mode::protanopia, false);
    shared();
    vision::set_active(vision::Mode::deuteranopia, false);
    shared();

    vision::set_active(vision::Mode::tritanopia, false);
    expect_tone(kit::StateTone::ok, 0x5A, 0xA9, 0xF0);
    expect_tone(kit::StateTone::caution, 0xE0, 0xB0, 0x4F);
    OA_CHECK(kit::state_ink(kit::StateTone::caution) == kit::colour::lock);
    OA_CHECK(kit::state_tint(kit::StateTone::caution) == kit::colour::caution_tint);
    expect_tone(kit::StateTone::danger, 0xF4, 0x51, 0x1E);
    expect_tone(kit::StateTone::information, 0xD6, 0xD6, 0xD6);
    OA_CHECK(kit::state_ok_words() == kit::state_ink(kit::StateTone::ok));

    vision::set_active(vision::Mode::achromatopsia, false);
    expect_tone(kit::StateTone::ok, 0xF2, 0xF2, 0xF2);
    expect_tone(kit::StateTone::caution, 0xBD, 0xBD, 0xBD);
    expect_tone(kit::StateTone::danger, 0x8A, 0x8A, 0x8A);
    expect_tone(kit::StateTone::information, 0xD9, 0xD9, 0xD9);
    OA_CHECK(kit::state_ok_words() == kit::state_ink(kit::StateTone::ok));
}

void the_focus_ring_stays() {
    const kit::Colour accent = kit::colour::accent;
    const kit::Colour ring = kit::colour::focus;
    OA_CHECK(ring == kit::colour::accent_light);
    OA_CHECK((accent == kit::Colour{0x9c, 0xcc, 0x3c}));
    OA_CHECK((ring == kit::Colour{0xb6, 0xe0, 0x5a}));
    for (const vision::Mode mode : {
             vision::Mode::off,
             vision::Mode::protanopia,
             vision::Mode::deuteranopia,
             vision::Mode::tritanopia,
             vision::Mode::achromatopsia,
         }) {
        vision::set_active(mode, false);
        OA_CHECK(kit::colour::accent == accent);
        OA_CHECK(kit::colour::focus == ring);
        OA_CHECK(kit::colour::focus == kit::colour::accent_light);
    }
}

} // namespace

int main() {
    const RestoreMode restore;
    slots_follow_the_mode();
    resource_figures_follow_the_mode();
    state_colours_follow_the_mode();
    the_focus_ring_stays();
    vision::set_active(vision::Mode::off, false);
    return oa::test::check_exit_status();
}
