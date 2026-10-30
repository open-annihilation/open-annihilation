// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The first-run pieces: the perspective blit (edges, opacity, clipping,
// nearest against filtered), the carousel's place as a function of time,
// and every component's states at Compact, Regular and Large, at 1x and at
// 150%. Hashes are compared, never pinned: two draws of one state match,
// and two states do not. The smooth draws read the bundled fonts beside
// the test.

#include "oa/formats/fnt.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/first_run.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/paint/painter.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;
namespace text_font = oa::platform::text_font;

constexpr int32_t kOneAndAHalf = 150;
constexpr kit::Colour kWhite{255, 255, 255};
constexpr kit::Colour kRed{255, 0, 0};
constexpr kit::Colour kBlue{0, 0, 255};

renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

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
        for (uint16_t y = 0; y < height; ++y)
            for (uint16_t x = 0; x < width; ++x)
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
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

struct Smooth {
    paint::Canvas pixels;
    paint::Painter painter;
    kit::Canvas canvas{};

    Smooth(int width, int height, text_font::FontStack* modern)
        : pixels(paint::make_canvas(width, height)), painter(pixels) {
        canvas.smooth.painter = &painter;
        canvas.smooth.fonts = modern;
        canvas.smooth.scale_percent = kOneAndAHalf;
        canvas.fonts = &fonts();
    }
};

uint64_t fnv(const uint8_t* bytes, std::size_t count) {
    uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

int ink_of(const renderer::Surface& surface) {
    int ink = 0;
    for (std::size_t index = 0; index + 2 < surface.rgb.size(); index += 3)
        if ((surface.rgb[index] | surface.rgb[index + 1] | surface.rgb[index + 2]) != 0)
            ++ink;
    return ink;
}

int ink_of(const paint::Canvas& pixels) {
    int ink = 0;
    for (int y = 0; y < pixels.height; ++y)
        for (int x = 0; x < pixels.width; ++x)
            if (paint::pixel_at(pixels, x, y).a != 0)
                ++ink;
    return ink;
}

bool pixel_is(
    const renderer::Surface& surface, int32_t x, int32_t y, uint8_t red, uint8_t green, uint8_t blue
) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(surface.width) ||
        y >= static_cast<int32_t>(surface.height))
        return false;
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return surface.rgb[at] == red && surface.rgb[at + 1] == green && surface.rgb[at + 2] == blue;
}

int column_ink(const renderer::Surface& surface, int32_t x) {
    int ink = 0;
    for (uint32_t y = 0; y < surface.height; ++y)
        if (!pixel_is(surface, x, static_cast<int32_t>(y), 0, 0, 0))
            ++ink;
    return ink;
}

void expect(bool ok, const char* what) {
    if (!ok)
        std::fprintf(stderr, "first-run: %s\n", what);
    OA_CHECK(ok);
}

kit::LanguageCardLook
language_look(kit::LanguageAvailability availability, bool hovered, bool held, bool disabled) {
    kit::LanguageCardLook look;
    look.letters = "EN";
    look.own_name = "English";
    look.english_name = "English";
    look.facts = "Built into the game";
    look.pill = "Built in";
    look.availability = availability;
    look.hovered = hovered;
    look.held = held;
    look.disabled = disabled;
    return look;
}

void perspective_keeps_edges_opacity_and_clip() {
    {
        auto surface = black(40, 30);
        kit::Canvas canvas = canvas_on(surface);
        kit::perspective_blit(
            canvas,
            {0, 0, 32, 24},
            {0, 0, 32, 24},
            kit::Perspective{40.F, 1.F},
            [](const kit::Canvas& off) { kit::fill_rect(off, {0, 0, 32, 24}, kWhite); }
        );
        expect(column_ink(surface, 0) > column_ink(surface, 31), "a turn shortens the far edge");
        expect(pixel_is(surface, 0, 0, 255, 255, 255), "the near edge keeps its full height");
        expect(pixel_is(surface, 31, 0, 0, 0, 0), "the far edge's corner is left clear");
        auto again = black(40, 30);
        kit::Canvas second = canvas_on(again);
        kit::perspective_blit(
            second,
            {0, 0, 32, 24},
            {0, 0, 32, 24},
            kit::Perspective{40.F, 1.F},
            [](const kit::Canvas& off) { kit::fill_rect(off, {0, 0, 32, 24}, kWhite); }
        );
        expect(
            fnv(surface.rgb.data(), surface.rgb.size()) == fnv(again.rgb.data(), again.rgb.size()),
            "two turns of one drawing differ"
        );
    }
    {
        auto surface = black(40, 24);
        kit::Canvas canvas = canvas_on(surface);
        kit::perspective_blit(
            canvas,
            {0, 0, 40, 20},
            {0, 0, 40, 20},
            kit::Perspective{24.F, 1.F},
            [](const kit::Canvas& off) {
                kit::fill_rect(off, {0, 0, 20, 20}, kRed);
                kit::fill_rect(off, {20, 0, 20, 20}, kBlue);
            }
        );
        bool mixed = false;
        for (std::size_t index = 0; index + 2 < surface.rgb.size(); index += 3)
            if (surface.rgb[index] > 0 && surface.rgb[index + 2] > 0)
                mixed = true;
        expect(!mixed, "nearest sampling mixes the two halves");
        expect(ink_of(surface) > 0, "the nearest copy drew nothing");
    }
    {
        auto surface = black(20, 20);
        kit::Canvas canvas = canvas_on(surface);
        kit::perspective_blit(
            canvas,
            {0, 0, 10, 10},
            {2, 2, 10, 10},
            kit::Perspective{0.F, 0.5F},
            [](const kit::Canvas& off) { kit::fill_rect(off, {0, 0, 10, 10}, kWhite); }
        );
        expect(pixel_is(surface, 7, 7, 128, 128, 128), "opacity 0.5 is not half of white");
        expect(pixel_is(surface, 0, 0, 0, 0, 0), "the copy painted outside its rectangle");
    }
    {
        auto surface = black(20, 12);
        kit::Canvas canvas = canvas_on(surface);
        kit::Canvas kept = kit::clipped(canvas, {0, 0, 8, 12});
        kit::perspective_blit(kept, {0, 0, 16, 8}, {0, 0, 16, 8}, {}, [](const kit::Canvas& off) {
            kit::fill_rect(off, {0, 0, 16, 8}, kWhite);
        });
        expect(pixel_is(surface, 1, 1, 255, 255, 255), "the clip dropped the near half");
        expect(pixel_is(surface, 12, 1, 0, 0, 0), "the clip let the far half through");
    }
    {
        auto surface = black(16, 16);
        kit::Canvas canvas = canvas_on(surface);
        int outer = 0;
        int inner = 0;
        kit::perspective_blit(canvas, {0, 0, 8, 8}, {0, 0, 8, 8}, {}, [&](const kit::Canvas& off) {
            ++outer;
            kit::fill_rect(off, {0, 0, 8, 8}, kWhite);
            kit::perspective_blit(off, {0, 0, 4, 4}, {0, 0, 4, 4}, {}, [&](const kit::Canvas&) {
                ++inner;
            });
        });
        expect(outer == 1 && inner == 0, "a nested blit drew into the buffer");
    }
    {
        auto first = black(16, 16);
        kit::Canvas leading = canvas_on(first);
        kit::perspective_blit(
            leading, {0, 0, 10, 10}, {0, 0, 10, 10}, {}, [](const kit::Canvas& off) {
                kit::fill_rect(off, {0, 0, 10, 10}, kWhite);
            }
        );
        auto second = black(16, 16);
        kit::Canvas following = canvas_on(second);
        kit::perspective_blit(following, {0, 0, 6, 6}, {0, 0, 6, 6}, {}, [](const kit::Canvas&) {});
        expect(ink_of(second) == 0, "a later blit kept the previous drawing");
    }
}

void smooth_copy_is_filtered(text_font::FontStack* modern) {
    Smooth smooth(48, 36, modern);
    kit::perspective_blit(
        smooth.canvas, {0, 0, 20, 16}, {0, 0, 20, 16}, {}, [](const kit::Canvas& off) {
            kit::fill_rect(off, {0, 0, 20, 8}, kRed);
            kit::fill_rect(off, {0, 8, 20, 8}, kBlue);
        }
    );
    bool mixed = false;
    for (int y = 0; y < smooth.pixels.height; ++y) {
        for (int x = 0; x < smooth.pixels.width; ++x) {
            const paint::Rgba pixel = paint::pixel_at(smooth.pixels, x, y);
            if (pixel.r > 0 && pixel.b > 0)
                mixed = true;
        }
    }
    expect(mixed, "the smooth copy has no mixed pixel");
    expect(ink_of(smooth.pixels) > 0, "the smooth copy drew nothing");
    Smooth again(48, 36, modern);
    kit::perspective_blit(
        again.canvas, {0, 0, 20, 16}, {0, 0, 20, 16}, {}, [](const kit::Canvas& off) {
            kit::fill_rect(off, {0, 0, 20, 8}, kRed);
            kit::fill_rect(off, {0, 8, 20, 8}, kBlue);
        }
    );
    expect(
        fnv(smooth.pixels.rgba.data(), smooth.pixels.rgba.size()) ==
            fnv(again.pixels.rgba.data(), again.pixels.rgba.size()),
        "two smooth copies differ"
    );
}

void carousel_place_is_a_function_of_time() {
    expect(kit::carousel_centre_milli(0, 1000, 0) == 0, "a move starts away from its origin");
    expect(kit::carousel_centre_milli(0, 1000, 250) == 750, "halfway is not three quarters");
    expect(
        kit::carousel_centre_milli(0, 1000, kit::carousel_move_ms) == 1000, "the move never arrives"
    );
    expect(kit::carousel_turn_degrees == 18, "the turn is not 18 degrees");
    expect(kit::carousel_move_ms == 500, "the move is not half a second");
    expect(kit::carousel_visible_side == 2, "the carousel shows other than two cards a side");

    kit::CarouselModel split{5, 0, 0, 0};
    kit::carousel_go(split, 1);
    kit::carousel_tick(split, 10);
    kit::carousel_tick(split, 10);
    kit::CarouselModel once{5, 0, 0, 0};
    kit::carousel_go(once, 1);
    kit::carousel_tick(once, 20);
    expect(
        split.elapsed_ms == once.elapsed_ms &&
            kit::carousel_index_milli(split) == kit::carousel_index_milli(once),
        "two slices do not add as one"
    );

    kit::CarouselModel many{5, 0, 0, 0};
    kit::carousel_go(many, 1);
    for (int step = 0; step < 30; ++step)
        kit::carousel_tick(many, 16);
    kit::CarouselModel whole{5, 0, 0, 0};
    kit::carousel_go(whole, 1);
    kit::carousel_tick(whole, 480);
    expect(many.elapsed_ms == 480 && whole.elapsed_ms == 480, "thirty frames are not one slice");
    expect(
        kit::carousel_index_milli(many) == kit::carousel_index_milli(whole),
        "the place depends on how the time was sliced"
    );
    kit::carousel_tick(many, 1000);
    expect(many.elapsed_ms == kit::carousel_move_ms, "the move runs past its time");

    const kit::CarouselGeometry fitted = kit::carousel_fit(1280, kit::large_metrics);
    expect(
        fitted.card_width == 232 && fitted.card_height == 304 && fitted.step == 236,
        "Large in a 1280-point room is not the design's card and step"
    );
    const kit::CarouselGeometry narrow = kit::carousel_fit(200, kit::large_metrics);
    expect(
        narrow.card_width == 40 && narrow.step == narrow.card_width,
        "a narrow room does not shrink the faces apart"
    );

    const kit::Rect wide{0, 0, 1280, 220};
    const std::vector<kit::CarouselSlot> turned =
        kit::carousel_places(wide, fitted, 5, 2000, false);
    const auto slot_of = [](const std::vector<kit::CarouselSlot>& slots,
                            int32_t index) -> const kit::CarouselSlot* {
        for (const kit::CarouselSlot& slot : slots)
            if (slot.index == index)
                return &slot;
        return nullptr;
    };
    const kit::CarouselSlot* centre = slot_of(turned, 2);
    const kit::CarouselSlot* beside = slot_of(turned, 3);
    const kit::CarouselSlot* further = slot_of(turned, 4);
    expect(
        centre != nullptr && beside != nullptr && further != nullptr, "a card in range is missing"
    );
    if (centre != nullptr && beside != nullptr && further != nullptr) {
        expect(
            centre->opacity == 256 && centre->degrees == 0, "the centre card is not flat and full"
        );
        expect(
            beside->opacity == 256 && beside->degrees == 18,
            "one step out is not 18 degrees and fully readable"
        );
        expect(
            further->opacity == 256 && further->degrees == 36,
            "two steps out are not 36 degrees and fully readable"
        );
    }
    {
        kit::CarouselModel settled{5, 1, 0, 0};
        kit::carousel_settle(settled);
        expect(
            kit::carousel_index_milli(settled) == 1000,
            "settling does not put the centre on the chosen card"
        );
        expect(settled.elapsed_ms == kit::carousel_move_ms, "settling leaves a move running");
        const std::vector<kit::CarouselSlot> resting =
            kit::carousel_places(wide, fitted, 5, kit::carousel_index_milli(settled), false, true);
        const kit::CarouselSlot* chosen = slot_of(resting, settled.selected);
        expect(chosen != nullptr && chosen->centre, "the settled card is not the centre");
        if (chosen != nullptr) {
            const int32_t band_mid = wide.x + wide.width / 2;
            const int32_t face_mid = chosen->face.x + chosen->face.width / 2;
            expect(face_mid == band_mid, "the settled card is not centred on the band");
            expect(
                !resting.empty() && chosen == &resting.back(),
                "the settled card is not drawn in front"
            );
        }
    }

    for (std::size_t first = 0; first < turned.size(); ++first) {
        for (std::size_t second = first + 1; second < turned.size(); ++second) {
            const kit::Rect shared = kit::intersect(turned[first].face, turned[second].face);
            expect(shared.width <= 0 || shared.height <= 0, "two carousel faces overlap");
        }
    }

    const std::vector<kit::CarouselSlot> flat = kit::carousel_places(wide, fitted, 5, 2000, true);
    for (const kit::CarouselSlot& slot : flat) {
        expect(slot.degrees == 0, "reduced motion still turns a card");
        expect(
            slot.face.width == fitted.card_width && slot.face.height == fitted.card_height,
            "reduced motion shrinks a card"
        );
        const int32_t centred = wide.y + (wide.height - fitted.card_height) / 2;
        expect(slot.face.y == centred, "reduced motion drops a card");
    }

    const kit::Rect cramped{0, 0, 200, 80};
    const std::vector<kit::CarouselSlot> shrunk =
        kit::carousel_places(cramped, narrow, 5, 2000, false);
    for (std::size_t first = 0; first < shrunk.size(); ++first) {
        for (std::size_t second = first + 1; second < shrunk.size(); ++second) {
            const kit::Rect shared = kit::intersect(shrunk[first].face, shrunk[second].face);
            expect(shared.width <= 0 || shared.height <= 0, "a narrow carousel's faces overlap");
        }
    }

    kit::CarouselModel model{5, 2, 2000, kit::carousel_move_ms};
    expect(
        kit::carousel_command(model, kit::CarouselCommand{false, false, true}) ==
            kit::CarouselEvent::picked,
        "Enter does not pick the centre"
    );
    expect(model.selected == 2, "picking the centre moves away from it");
    expect(
        kit::carousel_command(model, kit::carousel_key(kit::Key::right)) ==
            kit::CarouselEvent::moved,
        "Right does not move to the next card"
    );
    expect(model.selected == 3, "Right moved somewhere other than the next card");

    const kit::CarouselSlot* side = slot_of(turned, 0);
    expect(side != nullptr, "the card two steps left is missing");
    if (side != nullptr) {
        const kit::Point at{
            side->face.x + side->face.width / 2, side->face.y + side->face.height / 2
        };
        kit::CarouselModel pressed{5, 2, 2000, kit::carousel_move_ms};
        kit::CarouselDrag drag;
        expect(
            kit::carousel_pointer(pressed, turned, {}, {}, drag, kit::PointerPhase::down, at) ==
                kit::CarouselEvent::none,
            "a press answers before the finger comes up"
        );
        expect(
            kit::carousel_pointer(pressed, turned, {}, {}, drag, kit::PointerPhase::up, at) ==
                kit::CarouselEvent::moved,
            "a press on a side card does not centre it"
        );
        expect(pressed.selected == 0, "a side card centred a different card");
        expect(
            kit::carousel_pointer(pressed, turned, {}, {}, drag, kit::PointerPhase::down, at) ==
                kit::CarouselEvent::none,
            "a second press answers before the finger comes up"
        );
        expect(
            kit::carousel_pointer(pressed, turned, {}, {}, drag, kit::PointerPhase::up, at) ==
                kit::CarouselEvent::picked,
            "a second press on the card just centred does not pick it"
        );
        expect(pressed.selected == 0, "picking the card just centred moved away from it");
    }
    {
        kit::CarouselModel swiped{5, 2, 2000, kit::carousel_move_ms};
        kit::CarouselDrag drag;
        const kit::Point from{640, 100};
        const kit::Point to{from.x - 40, from.y};
        std::ignore =
            kit::carousel_pointer(swiped, turned, {}, {}, drag, kit::PointerPhase::down, from);
        std::ignore =
            kit::carousel_pointer(swiped, turned, {}, {}, drag, kit::PointerPhase::move, to);
        expect(
            kit::carousel_pointer(swiped, turned, {}, {}, drag, kit::PointerPhase::up, to) ==
                kit::CarouselEvent::moved,
            "a swipe to the left does not move on"
        );
        expect(
            swiped.selected == 3, "a swipe to the left selected something other than the next card"
        );
    }
}

void choice_and_coach_place_as_they_are_told() {
    expect(kit::choice_step(1, 3, kit::Key::up) == 0, "Up did not move toward the first choice");
    expect(
        kit::choice_step(1, 3, kit::Key::left) == 0, "Left did not move toward the first choice"
    );
    expect(kit::choice_step(1, 3, kit::Key::down) == 2, "Down did not move toward the last choice");
    expect(
        kit::choice_step(1, 3, kit::Key::right) == 2, "Right did not move toward the last choice"
    );
    expect(kit::choice_step(0, 3, kit::Key::up) == 0, "Up left the first choice");
    expect(kit::choice_step(2, 3, kit::Key::down) == 2, "Down left the last choice");
    expect(kit::choice_step(1, 3, kit::Key::enter) == 1, "Enter moved between choices");
    expect(kit::choice_step(4, 0, kit::Key::down) == 0, "an empty group did not stay empty");

    const kit::Rect room{0, 0, 200, 100};
    const kit::Rect target{160, 30, 40, 20};
    const kit::Rect card = kit::place_coach_mark(room, target, 40, 30);
    expect(kit::wholly_in(card, room), "the coach card leaves the room");
    const kit::Rect shared = kit::intersect(card, target);
    expect(shared.width <= 0 || shared.height <= 0, "the coach card covers its target");
    expect(
        card.x + card.width <= target.x, "the coach card did not flip to the left of the target"
    );
    expect(kit::coach_gap == 8, "the coach gap is not 8 points");
    expect(kit::picture_well_inset == 6, "the picture well's inset is not 6 points");
    expect(kit::call_to_action_period_ms == 1800, "the pulse is not 1.8 seconds");
    expect(kit::choice_card_height() == 42, "a choice card is not 42 points tall");
    expect(kit::step_indicator_height() == 17, "a step indicator is not 17 points tall");
}

template <class Draw>
std::pair<uint64_t, int> crisp_draw(int32_t width, int32_t height, Draw&& draw) {
    auto surface = black(width, height);
    kit::Canvas canvas = canvas_on(surface);
    draw(canvas);
    return {fnv(surface.rgb.data(), surface.rgb.size()), ink_of(surface)};
}

template <class Draw>
std::pair<uint64_t, int>
smooth_draw(int width, int height, text_font::FontStack* modern, Draw&& draw) {
    Smooth smooth(width, height, modern);
    draw(smooth.canvas);
    return {fnv(smooth.pixels.rgba.data(), smooth.pixels.rgba.size()), ink_of(smooth.pixels)};
}

void states_differ(const char* name, uint64_t idle, uint64_t other) {
    expect(idle != 0 && other != 0 && idle != other, name);
}

void every_component_at_every_class_and_scale(text_font::FontStack* modern) {
    const kit::Metrics* classes[] = {
        &kit::compact_metrics, &kit::regular_metrics, &kit::large_metrics
    };
    uint64_t compact_card = 0;
    uint64_t large_card = 0;
    for (const kit::Metrics* metrics : classes) {
        const int32_t card_w = metrics->language_card_width;
        const int32_t card_h = metrics->language_card_height;
        const int32_t wide = card_w + 16;
        const int32_t tall = card_h + 24;
        const auto draw_language = [&](const kit::Canvas& canvas,
                                       kit::LanguageAvailability availability,
                                       bool hovered,
                                       bool held,
                                       bool disabled) {
            kit::draw_language_card(
                canvas, {2, 2, card_w, card_h}, language_look(availability, hovered, held, disabled)
            );
        };
        const auto idle = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::built_in, false, false, false);
        });
        const auto hovered = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::built_in, true, false, false);
        });
        const auto held = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::built_in, true, true, false);
        });
        const auto active = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::active, false, false, false);
        });
        const auto failed = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::failed, false, false, false);
        });
        const auto offline = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::offline, false, false, false);
        });
        const auto disabled = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::built_in, false, false, true);
        });
        expect(idle.second > 0, "a language card drew nothing at 1x");
        states_differ("language hovered matches idle at 1x", idle.first, hovered.first);
        states_differ("language held matches idle at 1x", idle.first, held.first);
        states_differ("language active matches built in at 1x", idle.first, active.first);
        states_differ("language failed matches built in at 1x", idle.first, failed.first);
        states_differ("language offline matches built in at 1x", idle.first, offline.first);
        states_differ("language disabled matches idle at 1x", idle.first, disabled.first);
        const auto again = crisp_draw(wide, tall, [&](const kit::Canvas& canvas) {
            draw_language(canvas, kit::LanguageAvailability::built_in, false, false, false);
        });
        expect(again.first == idle.first, "a language card is not the same twice");

        const int smooth_w = wide * 2;
        const int smooth_h = tall * 2;
        const auto smooth_idle =
            smooth_draw(smooth_w, smooth_h, modern, [&](const kit::Canvas& canvas) {
                draw_language(canvas, kit::LanguageAvailability::built_in, false, false, false);
            });
        const auto smooth_hovered =
            smooth_draw(smooth_w, smooth_h, modern, [&](const kit::Canvas& canvas) {
                draw_language(canvas, kit::LanguageAvailability::built_in, true, false, false);
            });
        const auto smooth_active =
            smooth_draw(smooth_w, smooth_h, modern, [&](const kit::Canvas& canvas) {
                draw_language(canvas, kit::LanguageAvailability::active, false, false, false);
            });
        expect(
            smooth_idle.second > idle.second, "a language card at 150% is not larger than at 1x"
        );
        states_differ(
            "language hovered matches idle at 150%", smooth_idle.first, smooth_hovered.first
        );
        states_differ(
            "language active matches built in at 150%", smooth_idle.first, smooth_active.first
        );
        if (metrics == &kit::compact_metrics)
            compact_card = idle.first;
        if (metrics == &kit::large_metrics)
            large_card = idle.first;

        kit::CarouselLook carousel;
        carousel.card_width = std::min(card_w, int32_t{80});
        carousel.card_height = std::min(card_h, int32_t{56});
        carousel.step = carousel.card_width + 8;
        carousel.centre_milli = 2000;
        for (int index = 0; index < 5; ++index) {
            kit::CarouselCard card;
            card.letters = "EN";
            card.own_name = "English";
            card.english_name = "English";
            card.facts = "Built into the game";
            card.pill = "Built in";
            carousel.cards.push_back(card);
        }
        const int32_t carousel_w = carousel.step * 4 + carousel.card_width + 8;
        const int32_t carousel_h = carousel.card_height + kit::carousel_pager_band + 8;
        const auto turned = crisp_draw(carousel_w, carousel_h, [&](const kit::Canvas& canvas) {
            kit::draw_carousel(canvas, {0, 0, carousel_w, carousel_h}, carousel);
        });
        carousel.reduced_motion = true;
        const auto slid = crisp_draw(carousel_w, carousel_h, [&](const kit::Canvas& canvas) {
            kit::draw_carousel(canvas, {0, 0, carousel_w, carousel_h}, carousel);
        });
        expect(turned.second > 0, "a carousel drew nothing at 1x");
        states_differ("a flat carousel matches a turned one at 1x", turned.first, slid.first);
        const auto smooth_turned =
            smooth_draw(carousel_w * 2, carousel_h * 2, modern, [&](const kit::Canvas& canvas) {
                carousel.reduced_motion = false;
                kit::draw_carousel(canvas, {0, 0, carousel_w, carousel_h}, carousel);
            });
        expect(smooth_turned.second > turned.second, "a carousel at 150% is not larger than at 1x");

        kit::StepLook step;
        step.name = "LANGUAGE";
        step.index = 1;
        step.count = 3;
        const auto step_one = crisp_draw(180, 24, [&](const kit::Canvas& canvas) {
            kit::draw_step_indicator(canvas, {0, 0, 160, kit::step_indicator_height()}, step);
        });
        step.index = 3;
        const auto step_three = crisp_draw(180, 24, [&](const kit::Canvas& canvas) {
            kit::draw_step_indicator(canvas, {0, 0, 160, kit::step_indicator_height()}, step);
        });
        expect(step_one.second > 0, "a step indicator drew nothing");
        states_differ("step 3 matches step 1", step_one.first, step_three.first);
        const auto smooth_step = smooth_draw(360, 48, modern, [&](const kit::Canvas& canvas) {
            step.index = 1;
            kit::draw_step_indicator(canvas, {0, 0, 160, kit::step_indicator_height()}, step);
        });
        expect(
            smooth_step.second > step_one.second,
            "a step indicator at 150% is not larger than at 1x"
        );

        kit::SetupChromeLook chrome;
        chrome.width = 280;
        chrome.height = 62;
        chrome.page = "Language";
        chrome.step_name = "LANGUAGE";
        chrome.step_index = 1;
        chrome.step_count = 3;
        const auto setup = crisp_draw(280, 62, [&](const kit::Canvas& canvas) {
            kit::draw_setup_chrome(canvas, chrome);
        });
        chrome.next_hovered = true;
        const auto setup_hover = crisp_draw(280, 62, [&](const kit::Canvas& canvas) {
            kit::draw_setup_chrome(canvas, chrome);
        });
        expect(setup.second > 0, "the setup chrome drew nothing");
        states_differ("a hovered Next matches the setup at rest", setup.first, setup_hover.first);
        const auto smooth_setup = smooth_draw(560, 140, modern, [&](const kit::Canvas& canvas) {
            chrome.next_hovered = false;
            kit::draw_setup_chrome(canvas, chrome);
        });
        expect(
            smooth_setup.second > setup.second, "the setup chrome at 150% is not larger than at 1x"
        );
        chrome.next_style = kit::ButtonStyle::plain;
        const auto setup_plain = crisp_draw(280, 62, [&](const kit::Canvas& canvas) {
            kit::draw_setup_chrome(canvas, chrome);
        });
        states_differ(
            "a plain right-hand button matches an accent one", setup.first, setup_plain.first
        );

        kit::ChoiceCardLook choice;
        choice.title = "Ridge pack";
        choice.hint = "A choice.";
        const kit::Rect choice_rect{0, 0, 180, kit::choice_card_height()};
        const auto choice_idle = crisp_draw(180, 48, [&](const kit::Canvas& canvas) {
            kit::draw_choice_card(canvas, choice_rect, choice);
        });
        choice.hovered = true;
        const auto choice_hovered = crisp_draw(180, 48, [&](const kit::Canvas& canvas) {
            kit::draw_choice_card(canvas, choice_rect, choice);
        });
        choice.hovered = false;
        choice.chosen = true;
        const auto choice_chosen = crisp_draw(180, 48, [&](const kit::Canvas& canvas) {
            kit::draw_choice_card(canvas, choice_rect, choice);
        });
        choice.chosen = false;
        choice.disabled = true;
        const auto choice_disabled = crisp_draw(180, 48, [&](const kit::Canvas& canvas) {
            kit::draw_choice_card(canvas, choice_rect, choice);
        });
        expect(choice_idle.second > 0, "a choice card drew nothing");
        states_differ("a hovered choice matches idle", choice_idle.first, choice_hovered.first);
        states_differ("a chosen choice matches idle", choice_idle.first, choice_chosen.first);
        states_differ("a disabled choice matches idle", choice_idle.first, choice_disabled.first);
        const auto smooth_choice = smooth_draw(360, 96, modern, [&](const kit::Canvas& canvas) {
            kit::ChoiceCardLook rest;
            rest.title = "Ridge pack";
            rest.hint = "A choice.";
            kit::draw_choice_card(canvas, choice_rect, rest);
        });
        expect(
            smooth_choice.second > choice_idle.second,
            "a choice card at 150% is not larger than at 1x"
        );

        kit::CoachMarkLook coach;
        coach.number = 1;
        coach.title = "Library";
        coach.body = "Open it from the main menu.";
        coach.target = {150, 20, 24, 16};
        const auto coach_hash = crisp_draw(200, 80, [&](const kit::Canvas& canvas) {
            kit::draw_coach_mark(canvas, {8, 8, 120, 48}, coach);
        });
        expect(coach_hash.second > 0, "a coach mark drew nothing");
        const auto smooth_coach = smooth_draw(400, 160, modern, [&](const kit::Canvas& canvas) {
            kit::draw_coach_mark(canvas, {8, 8, 120, 48}, coach);
        });
        expect(
            smooth_coach.second > coach_hash.second, "a coach mark at 150% is not larger than at 1x"
        );

        auto spot_surface = black(48, 48);
        kit::Canvas spot = canvas_on(spot_surface);
        kit::fill_rect(spot, {0, 0, 48, 48}, kWhite);
        kit::draw_spotlight(spot, {0, 0, 48, 48}, kit::SpotlightLook{{12, 12, 16, 16}});
        expect(pixel_is(spot_surface, 20, 20, 255, 255, 255), "the spotlight dimmed its cut-out");
        expect(
            !pixel_is(spot_surface, 2, 2, 255, 255, 255), "the spotlight left the layer undimmed"
        );
        Smooth smooth_spot(96, 96, modern);
        kit::fill_rect(smooth_spot.canvas, {0, 0, 48, 48}, kWhite);
        kit::draw_spotlight(
            smooth_spot.canvas, {0, 0, 48, 48}, kit::SpotlightLook{{12, 12, 16, 16}}
        );
        expect(
            ink_of(smooth_spot.pixels) > ink_of(spot_surface),
            "a spotlight at 150% is not larger than at 1x"
        );

        kit::PictureWellLook well;
        well.caption = "Library, Active";
        well.stand_in = true;
        const auto pictured = crisp_draw(80, 56, [&](const kit::Canvas& canvas) {
            kit::draw_picture_well(canvas, {0, 0, 72, 48}, well);
        });
        well.stand_in = false;
        const auto empty_well = crisp_draw(80, 56, [&](const kit::Canvas& canvas) {
            kit::draw_picture_well(canvas, {0, 0, 72, 48}, well);
        });
        expect(pictured.second > 0, "a picture well drew nothing");
        states_differ("a stand-in matches an empty well", pictured.first, empty_well.first);
        const auto smooth_well = smooth_draw(160, 112, modern, [&](const kit::Canvas& canvas) {
            well.stand_in = true;
            kit::draw_picture_well(canvas, {0, 0, 72, 48}, well);
        });
        expect(
            smooth_well.second > pictured.second, "a picture well at 150% is not larger than at 1x"
        );

        kit::CallToActionLook call;
        call.caption = "Start playing";
        call.time_ms = 0;
        const auto pulse = crisp_draw(120, 40, [&](const kit::Canvas& canvas) {
            kit::draw_call_to_action(canvas, {8, 8, 80, 17}, call);
        });
        call.time_ms = 400;
        const auto later = crisp_draw(120, 40, [&](const kit::Canvas& canvas) {
            kit::draw_call_to_action(canvas, {8, 8, 80, 17}, call);
        });
        call.time_ms = 0;
        call.reduced_motion = true;
        const auto steady = crisp_draw(120, 40, [&](const kit::Canvas& canvas) {
            kit::draw_call_to_action(canvas, {8, 8, 80, 17}, call);
        });
        call.reduced_motion = false;
        call.enabled = false;
        const auto stopped = crisp_draw(120, 40, [&](const kit::Canvas& canvas) {
            kit::draw_call_to_action(canvas, {8, 8, 80, 17}, call);
        });
        expect(pulse.second > 0, "a call to action drew nothing");
        states_differ("the pulse looks the same at two times", pulse.first, later.first);
        states_differ("reduced motion matches the pulse", pulse.first, steady.first);
        states_differ("a disabled call to action matches the pulse", pulse.first, stopped.first);
        const auto smooth_call = smooth_draw(240, 80, modern, [&](const kit::Canvas& canvas) {
            kit::CallToActionLook rest;
            rest.caption = "Start playing";
            rest.time_ms = 400;
            kit::draw_call_to_action(canvas, {8, 8, 80, 17}, rest);
        });
        expect(
            smooth_call.second > pulse.second, "a call to action at 150% is not larger than at 1x"
        );
    }
    expect(
        compact_card != 0 && large_card != 0 && compact_card != large_card,
        "Compact and Large language cards match"
    );
}

} // namespace

/// Rows in the own-name band whose red is above the panel.
int name_rows(
    const renderer::Surface& surface, int32_t left, int32_t right, int32_t top, int32_t bottom
) {
    int rows = 0;
    for (int32_t y = top; y < bottom; ++y) {
        bool bright = false;
        for (int32_t x = left; x < right; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
            if (surface.rgb[at] > 40)
                bright = true;
        }
        if (bright)
            ++rows;
    }
    return rows;
}

void own_name_shrinks_to_fit_and_the_active_card_has_a_ring() {
    const int32_t card_w = kit::large_metrics.language_card_width;
    const int32_t card_h = kit::large_metrics.language_card_height;
    const int32_t origin = 4;
    const int32_t pad_top = 30 * card_h / 304;
    const int32_t badge = 88 * card_w / 232;
    const int32_t gap = 10 * card_h / 304;
    const int32_t name_h = 27 * card_h / 304;
    const int32_t name_top = origin + pad_top + badge + gap;
    auto draw = [&](std::string name, kit::LanguageAvailability availability) {
        auto surface = black(card_w + 16, card_h + 16);
        kit::Canvas canvas = canvas_on(surface);
        kit::LanguageCardLook look = language_look(availability, false, false, false);
        look.own_name = std::move(name);
        look.english_name.clear();
        look.facts.clear();
        look.pill.clear();
        kit::draw_language_card(canvas, {origin, origin, card_w, card_h}, look);
        return surface;
    };
    const renderer::Surface short_name = draw("W", kit::LanguageAvailability::built_in);
    kit::begin_text_fit_frame();
    const renderer::Surface long_name =
        draw(std::string(80, 'W'), kit::LanguageAvailability::built_in);
    // The card's border is bright on every row, so the count stays in the
    // text. A long name that the large size cannot fit wraps at the font's
    // own size inside the box, so it draws and is not recorded as wider.
    const int32_t side = 18 * card_w / 232;
    const int32_t text_left = origin + side;
    const int32_t text_right = origin + card_w - side;
    const int short_rows =
        name_rows(short_name, text_left, text_right, name_top, name_top + name_h);
    const int long_rows = name_rows(long_name, text_left, text_right, name_top, name_top + name_h);
    expect(short_rows > 0, "a short own name drew nothing");
    expect(long_rows > 0, "a long own name drew nothing");
    bool own_name_wider = false;
    for (const kit::TextFitRecord& record : kit::text_fit_records())
        own_name_wider = own_name_wider || record.wider;
    expect(!own_name_wider, "a long own name was drawn wider than its box");

    const int32_t badge_x = origin + side + (card_w - 2 * side - badge) / 2;
    const int32_t badge_y = origin + pad_top;
    expect(
        pixel_is(short_name, badge_x + 2, badge_y + 2, 0x5b, 0x63, 0x52),
        "the badge is not centred under the top padding"
    );
    expect(
        pixel_is(short_name, origin + 4, badge_y + 2, 0x1d, 0x21, 0x19),
        "the badge still sits at the top left"
    );

    const renderer::Surface active = draw("W", kit::LanguageAvailability::active);
    expect(
        pixel_is(active, origin, origin - 1, 0x9c, 0xcc, 0x3c), "an active card has no accent ring"
    );
    const renderer::Surface built_in = draw("W", kit::LanguageAvailability::built_in);
    expect(pixel_is(built_in, origin, origin - 1, 0, 0, 0), "a built-in card drew the active ring");
}

void carousel_wraps_one_step_and_a_double_click_picks() {
    kit::CarouselModel first{5, 0, 0, kit::carousel_move_ms};
    expect(
        kit::carousel_command(first, kit::CarouselCommand{true, false, false}) ==
            kit::CarouselEvent::moved,
        "left from the first card does not move"
    );
    expect(first.selected == 4, "left from the first card did not select the last");
    const int32_t left_start = kit::carousel_index_milli(first);
    expect(left_start - 4 * 1000 == 1000, "left from the first card is not one step");
    kit::carousel_tick(first, 20);
    expect(
        kit::carousel_index_milli(first) < left_start,
        "the move from the first card does not go left"
    );

    const kit::CarouselGeometry fitted = kit::carousel_fit(1280, kit::large_metrics);
    const kit::Rect wide{0, 0, 1280, 220};
    const std::vector<kit::CarouselSlot> leaving =
        kit::carousel_places(wide, fitted, 5, left_start, false, true);
    const auto slot_of = [](const std::vector<kit::CarouselSlot>& slots,
                            int32_t index) -> const kit::CarouselSlot* {
        for (const kit::CarouselSlot& slot : slots)
            if (slot.index == index)
                return &slot;
        return nullptr;
    };
    const kit::CarouselSlot* last_card = slot_of(leaving, 4);
    const kit::CarouselSlot* first_card = slot_of(leaving, 0);
    expect(last_card != nullptr && first_card != nullptr, "the wrap does not show both ends");
    if (last_card != nullptr && first_card != nullptr) {
        expect(last_card->face.x < first_card->face.x, "the last card is not left of the first");
        expect(first_card->centre, "the first card is not the centre as the move starts");
    }

    kit::CarouselModel last{5, 4, 4 * 1000, kit::carousel_move_ms};
    expect(
        kit::carousel_command(last, kit::CarouselCommand{false, true, false}) ==
            kit::CarouselEvent::moved,
        "right from the last card does not move"
    );
    expect(last.selected == 0, "right from the last card did not select the first");
    const int32_t right_start = kit::carousel_index_milli(last);
    expect(0 - right_start == 1000, "right from the last card is not one step");
    kit::carousel_tick(last, 20);
    expect(
        kit::carousel_index_milli(last) > right_start,
        "the move from the last card does not go right"
    );

    kit::CarouselModel only{1, 0, 0, 0};
    expect(
        kit::carousel_command(only, kit::CarouselCommand{true, false, false}) ==
            kit::CarouselEvent::none,
        "one card moved left"
    );
    expect(
        kit::carousel_command(only, kit::CarouselCommand{false, true, false}) ==
            kit::CarouselEvent::none,
        "one card moved right"
    );
    expect(only.selected == 0, "one card changed its selection");

    kit::CarouselModel pair_first{2, 0, 0, kit::carousel_move_ms};
    expect(
        kit::carousel_command(pair_first, kit::CarouselCommand{true, false, false}) ==
            kit::CarouselEvent::moved,
        "left from the first of two cards does not move"
    );
    expect(pair_first.selected == 1, "left from the first of two cards did not select the last");
    const int32_t pair_left = kit::carousel_index_milli(pair_first);
    expect(pair_left - 1000 == 1000, "left from the first of two cards is not one step");
    kit::carousel_tick(pair_first, 20);
    expect(
        kit::carousel_index_milli(pair_first) < pair_left,
        "the move from the first of two cards does not go left"
    );

    kit::CarouselModel pair_last{2, 1, 1000, kit::carousel_move_ms};
    expect(
        kit::carousel_command(pair_last, kit::CarouselCommand{false, true, false}) ==
            kit::CarouselEvent::moved,
        "right from the last of two cards does not move"
    );
    expect(pair_last.selected == 0, "right from the last of two cards did not select the first");
    const int32_t pair_right = kit::carousel_index_milli(pair_last);
    expect(0 - pair_right == 1000, "right from the last of two cards is not one step");
    kit::carousel_tick(pair_last, 20);
    expect(
        kit::carousel_index_milli(pair_last) > pair_right,
        "the move from the last of two cards does not go right"
    );

    const std::vector<kit::CarouselSlot> turned =
        kit::carousel_places(wide, fitted, 5, 2000, false, true);
    const kit::CarouselSlot* side = slot_of(turned, 0);
    expect(side != nullptr, "a side card is missing");
    if (side != nullptr) {
        const kit::Point at{
            side->face.x + side->face.width / 2, side->face.y + side->face.height / 2
        };
        kit::CarouselModel pressed{5, 2, 2000, kit::carousel_move_ms};
        kit::CarouselDrag drag;
        expect(
            kit::carousel_pointer(pressed, turned, {}, {}, drag, kit::PointerPhase::down, at, 2) ==
                kit::CarouselEvent::none,
            "a double-click answers before the finger comes up"
        );
        expect(
            kit::carousel_pointer(pressed, turned, {}, {}, drag, kit::PointerPhase::up, at, 2) ==
                kit::CarouselEvent::picked,
            "a double-click on a side card does not pick it"
        );
        expect(pressed.selected == 0, "a double-click picked a different card");
    }
}

/// The two cards two steps out are drawn with no pill. The three nearer keep one.
void outer_cards_leave_the_pill_off() {
    kit::CarouselLook carousel;
    carousel.card_width = 80;
    carousel.card_height = 56;
    carousel.step = 88;
    carousel.centre_milli = 2000;
    carousel.wrap = true;
    for (int index = 0; index < 5; ++index) {
        kit::CarouselCard card;
        card.letters = "EN";
        card.own_name = "English";
        card.english_name = "English";
        card.facts = "Built into the game";
        card.pill = "Built in";
        card.availability =
            index == 2 ? kit::LanguageAvailability::active : kit::LanguageAvailability::built_in;
        carousel.cards.push_back(std::move(card));
    }
    const int32_t carousel_w = carousel.step * 4 + carousel.card_width + 8;
    const int32_t carousel_h = carousel.card_height + kit::carousel_pager_band + 8;
    const kit::Rect rect{0, 0, carousel_w, carousel_h};
    const std::vector<kit::CarouselSlot> slots = kit::carousel_places(
        kit::carousel_band(rect),
        {carousel.card_width, carousel.card_height, carousel.step},
        5,
        carousel.centre_milli,
        false,
        true
    );
    int outer = 0;
    int nearer = 0;
    int32_t centre_index = -1;
    int32_t side_index = -1;
    for (const kit::CarouselSlot& slot : slots) {
        if (slot.distance_milli >= kit::carousel_visible_side * 1000) {
            ++outer;
        } else {
            ++nearer;
            if (slot.centre)
                centre_index = slot.index;
            else if (side_index < 0)
                side_index = slot.index;
        }
    }
    expect(outer == 2 && nearer == 3, "a resting carousel is not two outer cards and three nearer");
    expect(centre_index >= 0 && side_index >= 0, "the centre or a side card is missing");
    if (centre_index < 0 || side_index < 0)
        return;

    const auto draw = [&](const kit::CarouselLook& look) {
        const auto drawn = crisp_draw(carousel_w, carousel_h, [&](const kit::Canvas& canvas) {
            kit::draw_carousel(canvas, rect, look);
        });
        return drawn.first;
    };
    const uint64_t kept = draw(carousel);
    kit::CarouselLook stripped = carousel;
    for (const kit::CarouselSlot& placed : slots) {
        if (placed.distance_milli >= kit::carousel_visible_side * 1000)
            stripped.cards[static_cast<std::size_t>(placed.index)].pill.clear();
    }
    expect(draw(stripped) == kept, "an outer card still draws its pill");

    kit::CarouselLook centre = carousel;
    centre.cards[static_cast<std::size_t>(centre_index)].pill.clear();
    states_differ("the centre card leaves its pill off", kept, draw(centre));
    kit::CarouselLook side = carousel;
    side.cards[static_cast<std::size_t>(side_index)].pill.clear();
    states_differ("a card one step out leaves its pill off", kept, draw(side));

    carousel.reduced_motion = true;
    stripped.reduced_motion = true;
    expect(draw(stripped) == draw(carousel), "reduced motion still draws an outer pill");
}

void smooth_pill_keeps_its_words(text_font::FontStack* modern) {
    Smooth smooth(160, 40, modern);
    smooth.canvas.fonts = nullptr;
    const int32_t active =
        kit::pill_width_of(smooth.canvas, kit::PillLook{"Active", kit::PillState::playing});
    const int32_t built_in =
        kit::pill_width_of(smooth.canvas, kit::PillLook{"Built in", kit::PillState::neutral});
    expect(active > 24, "Active is only as wide as the pill is tall");
    expect(built_in > active, "Built in is not wider than Active");
}

int main() {
    perspective_keeps_edges_opacity_and_clip();
    carousel_place_is_a_function_of_time();
    carousel_wraps_one_step_and_a_double_click_picks();
    choice_and_coach_place_as_they_are_told();
    own_name_shrinks_to_fit_and_the_active_card_has_a_ring();
    auto modern = text_font::FontStack::open(text_font::bundled_font_directory());
    expect(modern != nullptr, "the bundled fonts are not beside the test");
    // The filtered blit draws no glyphs. A missing font stack still has to
    // exercise a smooth canvas, or the filter is never checked.
    smooth_copy_is_filtered(modern.get());
    smooth_pill_keeps_its_words(modern.get());
    outer_cards_leave_the_pill_off();
    every_component_at_every_class_and_scale(modern.get());
    return oa::test::check_exit_status();
}
