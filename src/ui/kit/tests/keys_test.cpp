// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Keycaps, chord chips, keyboard and mouse pictures, and the hold ring.
// Geometry stays inside 91 by 25 quarters without overlaps. Drawings stay
// inside their rectangle at 1x, 2x, 3x and 150%, and a 2x drawing is not a
// 1x drawing doubled. Marks differ in shape, and a clash mark on its own
// paints the danger colour. Each tint's face reaches 4.5:1 against its ink.
// A reduced-motion ring snaps to quarters. At 2, 3 and 4 points a quarter
// no two keys touch, and every label that is drawn fits its key.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;

constexpr kit::Colour kBlack{0, 0, 0};

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

renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

kit::Canvas canvas_on(renderer::Surface& surface, int32_t scale = 1) {
    return {&surface, {0, 0, scale, {}}, &fonts(), {}};
}

kit::Colour pixel_of(const renderer::Surface& surface, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

bool inside(int32_t x, int32_t y, const kit::Rect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

struct Smooth {
    paint::Canvas pixels;
    paint::Painter painter;
    kit::Canvas canvas{};

    Smooth(int width, int height, int32_t scale_percent)
        : pixels(paint::make_canvas(width, height)), painter(pixels) {
        canvas.smooth.painter = &painter;
        canvas.smooth.scale_percent = scale_percent;
        canvas.fonts = &fonts();
    }
};

/// Tells whether two places share any quarter.
///
/// @param a a key
/// @param b another
/// @return true when they overlap
bool overlaps(const kit::KeyPlace& a, const kit::KeyPlace& b) noexcept {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/// Tells whether a name matches name_problem's rule.
///
/// @param name the name
/// @return true when a control of that name is sound
bool name_ok(std::string name) {
    kit::DisplayList list;
    kit::Control control;
    control.id = 1;
    control.name = std::move(name);
    list.controls.push_back(std::move(control));
    return kit::name_problem(list).empty();
}

template <class Draw>
void crisp_inside(Draw&& draw, int32_t part_w, int32_t part_h, int32_t scale) {
    constexpr int32_t margin = 2;
    const int32_t width = (part_w + margin * 2) * scale;
    const int32_t height = (part_h + margin * 2) * scale;
    const kit::Rect placed{margin, margin, part_w, part_h};
    const kit::Rect pixels{margin * scale, margin * scale, part_w * scale, part_h * scale};
    renderer::Surface first = black(width, height);
    renderer::Surface second = black(width, height);
    draw(canvas_on(first, scale), placed);
    draw(canvas_on(second, scale), placed);
    OA_CHECK(first.rgb == second.rgb);
    bool covered = false;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const kit::Colour got = pixel_of(first, x, y);
            if (!inside(x, y, pixels))
                OA_CHECK(got == kBlack);
            covered = covered || got != kBlack;
        }
    }
    OA_CHECK(covered);
}

template <class Draw>
void not_doubled(Draw&& draw, int32_t part_w, int32_t part_h) {
    renderer::Surface one = black(part_w, part_h);
    draw(canvas_on(one, 1), kit::Rect{0, 0, part_w, part_h});
    renderer::Surface two = black(part_w * 2, part_h * 2);
    draw(canvas_on(two, 2), kit::Rect{0, 0, part_w, part_h});
    bool doubled = true;
    for (int32_t y = 0; y < part_h && doubled; ++y) {
        for (int32_t x = 0; x < part_w && doubled; ++x) {
            const kit::Colour source = pixel_of(one, x, y);
            doubled = pixel_of(two, x * 2, y * 2) == source &&
                      pixel_of(two, x * 2 + 1, y * 2) == source &&
                      pixel_of(two, x * 2, y * 2 + 1) == source &&
                      pixel_of(two, x * 2 + 1, y * 2 + 1) == source;
        }
    }
    OA_CHECK(!doubled);
}

template <class Draw>
void smooth_inside(Draw&& draw, int32_t part_w, int32_t part_h) {
    constexpr int32_t percent = 150;
    constexpr int32_t margin = 2;
    const kit::Rect placed{margin, margin, part_w, part_h};
    const kit::Rect whole{0, 0, part_w + margin * 2, part_h + margin * 2};
    const kit::Rect pixels = kit::to_canvas(whole, percent);
    const kit::Rect box = kit::to_canvas(placed, percent);
    Smooth first(pixels.width + 2, pixels.height + 2, percent);
    Smooth second(pixels.width + 2, pixels.height + 2, percent);
    draw(first.canvas, placed);
    draw(second.canvas, placed);
    bool covered = false;
    for (int y = 0; y < first.pixels.height; ++y) {
        for (int x = 0; x < first.pixels.width; ++x) {
            const paint::Rgba got = paint::pixel_at(first.pixels, x, y);
            const paint::Rgba again = paint::pixel_at(second.pixels, x, y);
            OA_CHECK(got == again);
            const bool in =
                x >= box.x && y >= box.y && x < box.x + box.width && y < box.y + box.height;
            if (!in)
                OA_CHECK(got.a == 0);
            covered = covered || got.a != 0;
        }
    }
    OA_CHECK(covered);
}

template <class Draw>
void every_scale(Draw&& draw, int32_t part_w, int32_t part_h) {
    for (const int32_t scale : {1, 2, 3})
        crisp_inside(draw, part_w, part_h, scale);
    not_doubled(draw, part_w, part_h);
    smooth_inside(draw, part_w, part_h);
}

bool has_code(kit::KeyboardShape shape, std::string_view code) {
    for (const kit::KeyPlace& place : kit::keyboard_places(shape))
        if (place.code == code)
            return true;
    return false;
}

void geometry_holds() {
    const kit::Point size = kit::keyboard_size();
    OA_CHECK(size.x == 91);
    OA_CHECK(size.y == 25);
    for (const kit::KeyboardShape shape :
         {kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis}) {
        const std::span<const kit::KeyPlace> places = kit::keyboard_places(shape);
        OA_CHECK(!places.empty());
        std::set<std::string> codes;
        for (std::size_t index = 0; index < places.size(); ++index) {
            const kit::KeyPlace& place = places[index];
            OA_CHECK(place.w > 0 && place.h > 0);
            OA_CHECK(place.x >= 0 && place.y >= 0);
            OA_CHECK(place.x + place.w <= 91);
            OA_CHECK(place.y + place.h <= 25);
            OA_CHECK(codes.insert(std::string(place.code)).second);
            OA_CHECK(name_ok(kit::code_word(place.code)));
            OA_CHECK(name_ok("board." + kit::code_word(place.code)));
            for (std::size_t other = index + 1; other < places.size(); ++other)
                OA_CHECK(!overlaps(place, places[other]));
            if (index > 0) {
                const kit::KeyPlace& earlier = places[index - 1];
                const bool reading =
                    place.y > earlier.y || (place.y == earlier.y && place.x >= earlier.x);
                OA_CHECK(reading);
            }
        }
    }
    OA_CHECK(has_code(kit::KeyboardShape::iso, "IntlBackslash"));
    OA_CHECK(!has_code(kit::KeyboardShape::ansi, "IntlBackslash"));
    OA_CHECK(has_code(kit::KeyboardShape::jis, "IntlYen"));
    OA_CHECK(has_code(kit::KeyboardShape::jis, "IntlRo"));
    OA_CHECK(has_code(kit::KeyboardShape::jis, "NonConvert"));
    OA_CHECK(has_code(kit::KeyboardShape::jis, "Convert"));
    OA_CHECK(has_code(kit::KeyboardShape::jis, "KanaMode"));
    bool tall = false;
    for (const kit::KeyPlace& place : kit::keyboard_places(kit::KeyboardShape::iso))
        if (place.code == "Enter")
            tall = place.h > 4;
    OA_CHECK(tall);
}

void words_match_the_examples() {
    OA_CHECK(kit::code_word("KeyA") == "key-a");
    OA_CHECK(kit::code_word("Digit1") == "digit-1");
    OA_CHECK(kit::code_word("NumpadAdd") == "numpad-add");
    OA_CHECK(kit::code_word("F12") == "f12");
    OA_CHECK(kit::code_word("Numpad0") == "numpad-0");
    OA_CHECK(kit::code_word("IntlBackslash") == "intl-backslash");
    OA_CHECK(kit::code_word("ArrowLeft") == "arrow-left");
}

void keyboard_names_one_control_per_key() {
    for (const kit::KeyboardShape shape :
         {kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis}) {
        kit::KeyboardLook look;
        look.shape = shape;
        kit::DisplayList list;
        const kit::Rect rect{0, 0, 182, 50};
        kit::add_keyboard(list, rect, look, 10, "board");
        const std::span<const kit::KeyPlace> places = kit::keyboard_places(shape);
        OA_CHECK(list.controls.size() == places.size());
        OA_CHECK(list.items.size() == 1);
        OA_CHECK(list.items[0].role == kit::Role::keyboard);
        for (std::size_t index = 0; index < places.size(); ++index) {
            const kit::Control& control = list.controls[index];
            OA_CHECK(control.id == 10 + static_cast<kit::ControlId>(index));
            OA_CHECK(control.name == "board." + kit::code_word(places[index].code));
            OA_CHECK(control.rect.width > 0 && control.rect.height > 0);
        }
        OA_CHECK(kit::name_problem(list).empty());
    }
}

void mouse_names_its_buttons() {
    kit::MouseLook look;
    look.extra_buttons = true;
    kit::DisplayList list;
    kit::add_mouse(list, {0, 0, 80, 128}, look, 3, "pointer");
    const std::vector<std::string> names{
        "pointer.mouse-left",
        "pointer.mouse-right",
        "pointer.mouse-middle",
        "pointer.wheel",
        "pointer.mouse-back",
        "pointer.mouse-forward",
        "pointer.mouse-6",
        "pointer.mouse-7",
        "pointer.mouse-8",
    };
    OA_CHECK(list.controls.size() == names.size());
    for (std::size_t index = 0; index < names.size(); ++index) {
        OA_CHECK(list.controls[index].name == names[index]);
        OA_CHECK(list.controls[index].id == 3 + static_cast<kit::ControlId>(index));
    }
    OA_CHECK(kit::name_problem(list).empty());
    kit::MouseLook plain;
    kit::DisplayList fewer;
    kit::add_mouse(fewer, {0, 0, 80, 128}, plain, 1, "pointer");
    OA_CHECK(fewer.controls.size() == 6);
}

renderer::Surface draw_mark(kit::KeyMark mark) {
    kit::KeycapLook look;
    look.label = "A";
    look.mark = mark;
    renderer::Surface surface = black(16, 16);
    kit::draw_keycap(canvas_on(surface), {0, 0, 16, 16}, look);
    return surface;
}

int grey_of(kit::Colour ink) {
    return (static_cast<int>(ink.r) * 30 + static_cast<int>(ink.g) * 59 +
            static_cast<int>(ink.b) * 11) /
           100;
}

bool grey_differs(const renderer::Surface& a, const renderer::Surface& b) {
    if (a.rgb.size() != b.rgb.size() || a.width != b.width)
        return true;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            if (grey_of(pixel_of(a, static_cast<int32_t>(x), static_cast<int32_t>(y))) !=
                grey_of(pixel_of(b, static_cast<int32_t>(x), static_cast<int32_t>(y))))
                return true;
        }
    }
    return false;
}

void marks_differ_in_shape() {
    const renderer::Surface changed = draw_mark(kit::KeyMark::changed);
    const renderer::Surface clash = draw_mark(kit::KeyMark::clash);
    const renderer::Surface unbound = draw_mark(kit::KeyMark::unbound);
    const renderer::Surface fixed = draw_mark(kit::KeyMark::fixed);
    const renderer::Surface none = draw_mark(kit::KeyMark::none);
    const renderer::Surface* marks[] = {&changed, &clash, &unbound, &fixed, &none};
    for (std::size_t first = 0; first < 5; ++first)
        for (std::size_t second = first + 1; second < 5; ++second)
            OA_CHECK(marks[first]->rgb != marks[second]->rgb);
    OA_CHECK(grey_differs(clash, changed));
}

// A labelled keycap's dashed border differs even when the triangle covers
// nothing, so the mark is drawn on its own.
void clash_mark_paints_danger() {
    kit::KeycapLook look;
    look.mark_only = true;
    look.mark = kit::KeyMark::clash;
    renderer::Surface surface = black(14, 14);
    kit::draw_keycap(canvas_on(surface), {0, 0, 14, 14}, look);
    int painted = 0;
    for (uint32_t y = 0; y < surface.height; ++y) {
        for (uint32_t x = 0; x < surface.width; ++x) {
            const kit::Colour pixel =
                pixel_of(surface, static_cast<int32_t>(x), static_cast<int32_t>(y));
            if (pixel.r == kit::colour::danger.r && pixel.g == kit::colour::danger.g &&
                pixel.b == kit::colour::danger.b)
                ++painted;
        }
    }
    OA_CHECK(painted > 0);
}

double channel(uint8_t value) {
    const double unit = static_cast<double>(value) / 255.0;
    if (unit <= 0.04045)
        return unit / 12.92;
    return std::pow((unit + 0.055) / 1.055, 2.4);
}

double luminance(kit::Colour ink) {
    return 0.2126 * channel(ink.r) + 0.7152 * channel(ink.g) + 0.0722 * channel(ink.b);
}

double contrast(kit::Colour a, kit::Colour b) {
    double lighter = luminance(a);
    double darker = luminance(b);
    if (lighter < darker)
        std::swap(lighter, darker);
    return (lighter + 0.05) / (darker + 0.05);
}

void tints_reach_contrast() {
    const kit::KeyTint tints[] = {
        kit::KeyTint::none,
        kit::KeyTint::orders,
        kit::KeyTint::pointer,
        kit::KeyTint::selection,
        kit::KeyTint::groups,
        kit::KeyTint::camera,
        kit::KeyTint::building,
        kit::KeyTint::game,
        kit::KeyTint::chat,
        kit::KeyTint::window,
        kit::KeyTint::hacks,
        kit::KeyTint::mod,
        kit::KeyTint::developer,
    };
    for (const kit::KeyTint tint : tints)
        OA_CHECK(contrast(kit::tint_face(tint), kit::tint_ink(tint)) >= 4.5);
}

void ring_snaps_to_quarters() {
    constexpr kit::Rect box{0, 0, 32, 32};
    renderer::Surface reduced = black(32, 32);
    renderer::Surface steady = black(32, 32);
    kit::draw_hold_ring(canvas_on(reduced), box, kit::HoldRingLook{30, 100, true});
    kit::draw_hold_ring(canvas_on(steady), box, kit::HoldRingLook{25, 100, false});
    OA_CHECK(reduced.rgb == steady.rgb);
    renderer::Surface full_motion = black(32, 32);
    kit::draw_hold_ring(canvas_on(full_motion), box, kit::HoldRingLook{30, 100, false});
    OA_CHECK(full_motion.rgb != reduced.rgb);
}

void parts_stay_inside_at_every_scale() {
    every_scale(
        [](const kit::Canvas& canvas, const kit::Rect& rect) {
            kit::KeycapLook look;
            look.label = "A";
            look.sub = "go";
            look.tint = kit::KeyTint::orders;
            look.letter = "O";
            look.mark = kit::KeyMark::changed;
            kit::draw_keycap(canvas, rect, look);
        },
        24,
        16
    );
    every_scale(
        [](const kit::Canvas& canvas, const kit::Rect& rect) {
            kit::ChordChipLook look;
            look.keys = {"A", "B"};
            look.gesture = "hold";
            look.mark = kit::KeyMark::clash;
            kit::draw_chord_chip(canvas, rect, look);
        },
        48,
        16
    );
    for (const kit::KeyboardShape shape :
         {kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis}) {
        every_scale(
            [shape](const kit::Canvas& canvas, const kit::Rect& rect) {
                kit::KeyboardLook look;
                look.shape = shape;
                kit::draw_keyboard(canvas, rect, look);
            },
            91,
            25
        );
    }
    every_scale(
        [](const kit::Canvas& canvas, const kit::Rect& rect) {
            kit::MouseLook look;
            look.extra_buttons = true;
            kit::draw_mouse(canvas, rect, look);
        },
        40,
        64
    );
    every_scale(
        [](const kit::Canvas& canvas, const kit::Rect& rect) {
            kit::draw_hold_ring(canvas, rect, kit::HoldRingLook{2, 3, false});
        },
        32,
        32
    );
}

/// Tells whether two key rectangles keep at least a point between them.
///
/// @param left a key
/// @param right another
/// @return true when they do not share an edge or a corner
bool kept_apart(const kit::Rect& left, const kit::Rect& right) noexcept {
    if (left.width <= 0 || left.height <= 0 || right.width <= 0 || right.height <= 0)
        return true;
    const bool apart_x = left.x + left.width < right.x || right.x + right.width < left.x;
    const bool apart_y = left.y + left.height < right.y || right.y + right.height < left.y;
    return apart_x || apart_y;
}

bool same_ink(kit::Colour pixel, kit::Colour ink) noexcept {
    return pixel.r == ink.r && pixel.g == ink.g && pixel.b == ink.b;
}

int count_colour(const renderer::Surface& surface, kit::Colour ink) {
    int count = 0;
    for (uint32_t y = 0; y < surface.height; ++y) {
        for (uint32_t x = 0; x < surface.width; ++x) {
            if (same_ink(pixel_of(surface, static_cast<int32_t>(x), static_cast<int32_t>(y)), ink))
                ++count;
        }
    }
    return count;
}

int count_in(const renderer::Surface& surface, const kit::Rect& rect, kit::Colour ink) {
    int count = 0;
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= surface.width ||
                static_cast<uint32_t>(y) >= surface.height)
                continue;
            if (same_ink(pixel_of(surface, x, y), ink))
                ++count;
        }
    }
    return count;
}

const kit::Control* key_named(const kit::DisplayList& list, std::string_view name) {
    for (const kit::Control& control : list.controls) {
        if (control.name == name)
            return &control;
    }
    return nullptr;
}

void draw_board(renderer::Surface& surface, const kit::KeyboardLook& look, int32_t quarter) {
    const int32_t wide = 91 * quarter;
    const int32_t high = 25 * quarter;
    kit::draw_keyboard(canvas_on(surface), {0, 0, wide, high}, look);
}

void small_keyboard_keys_stay_apart() {
    const kit::KeyboardShape shapes[] = {
        kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis
    };
    for (const int32_t quarter : {2, 3, 4}) {
        for (const kit::KeyboardShape shape : shapes) {
            kit::KeyboardLook look;
            look.shape = shape;
            kit::DisplayList list;
            const kit::Rect picture{0, 0, 91 * quarter, 25 * quarter};
            kit::add_keyboard(list, picture, look, 1, "board");
            OA_CHECK(list.controls.size() == kit::keyboard_places(shape).size());
            for (std::size_t index = 0; index < list.controls.size(); ++index) {
                for (std::size_t other = index + 1; other < list.controls.size(); ++other)
                    OA_CHECK(kept_apart(list.controls[index].rect, list.controls[other].rect));
            }
            const kit::Control* key_a = key_named(list, "board.key-a");
            OA_CHECK(key_a != nullptr);
            if (key_a != nullptr) {
                OA_CHECK(key_a->rect.width == 4 * quarter - 2);
                OA_CHECK(key_a->rect.height == 4 * quarter - 2);
            }

            renderer::Surface surface = black(picture.width, picture.height);
            kit::begin_text_fit_frame();
            draw_board(surface, look, quarter);
            for (const kit::TextFitRecord& record : kit::text_fit_records()) {
                OA_CHECK(!record.wider);
                OA_CHECK(!record.across);
            }
            const int letters = count_colour(surface, kit::colour::text);
            if (quarter < 4)
                OA_CHECK(letters == 0);
            else
                OA_CHECK(letters > 0);
        }
    }

    for (const int32_t quarter : {2, 3}) {
        kit::KeyboardLook changed;
        changed.shape = kit::KeyboardShape::ansi;
        kit::KeyboardKey key;
        key.code = "KeyA";
        key.cap.mark = kit::KeyMark::changed;
        changed.keys.push_back(key);
        kit::KeyboardKey clash;
        clash.code = "KeyS";
        clash.cap.mark = kit::KeyMark::clash;
        changed.keys.push_back(std::move(clash));
        kit::KeyboardKey unbound;
        unbound.code = "KeyD";
        unbound.cap.mark = kit::KeyMark::unbound;
        changed.keys.push_back(std::move(unbound));
        kit::KeyboardKey fixed;
        fixed.code = "KeyF";
        fixed.cap.mark = kit::KeyMark::fixed;
        changed.keys.push_back(std::move(fixed));

        const int32_t wide = 91 * quarter;
        const int32_t high = 25 * quarter;
        renderer::Surface surface = black(wide, high);
        kit::begin_text_fit_frame();
        draw_board(surface, changed, quarter);
        for (const kit::TextFitRecord& record : kit::text_fit_records()) {
            OA_CHECK(!record.wider);
            OA_CHECK(!record.across);
        }

        kit::DisplayList list;
        kit::add_keyboard(list, {0, 0, wide, high}, changed, 1, "board");
        const kit::Control* key_a = key_named(list, "board.key-a");
        const kit::Control* key_s = key_named(list, "board.key-s");
        const kit::Control* key_d = key_named(list, "board.key-d");
        const kit::Control* key_f = key_named(list, "board.key-f");
        const kit::Control* key_g = key_named(list, "board.key-g");
        OA_CHECK(key_a != nullptr && key_s != nullptr && key_d != nullptr && key_f != nullptr);
        OA_CHECK(key_g != nullptr);
        if (key_a == nullptr || key_s == nullptr || key_d == nullptr || key_f == nullptr ||
            key_g == nullptr)
            continue;
        OA_CHECK(count_in(surface, key_a->rect, kit::colour::accent) > 0);
        OA_CHECK(count_in(surface, key_g->rect, kit::colour::accent) == 0);
        OA_CHECK(count_in(surface, key_s->rect, kit::colour::danger) > 0);
        OA_CHECK(count_in(surface, key_g->rect, kit::colour::danger) == 0);
        OA_CHECK(count_in(surface, key_d->rect, kit::colour::quiet) > 0);
        OA_CHECK(count_in(surface, key_g->rect, kit::colour::quiet) == 0);
        OA_CHECK(count_in(surface, key_f->rect, kit::colour::text) > 0);
        OA_CHECK(count_in(surface, key_g->rect, kit::colour::text) == 0);
        // The letter is drawn in on_accent on the filled key, and only when
        // it fits. At 2 points a quarter the capitals are taller than the key.
        const int letters = count_colour(surface, kit::colour::on_accent);
        if (quarter == 3)
            OA_CHECK(letters > 0);
        else
            OA_CHECK(letters == 0);
    }

    kit::KeyboardLook at_four;
    at_four.shape = kit::KeyboardShape::ansi;
    kit::KeyboardKey still;
    still.code = "KeyA";
    still.cap.mark = kit::KeyMark::changed;
    at_four.keys.push_back(std::move(still));
    renderer::Surface four = black(91 * 4, 25 * 4);
    draw_board(four, at_four, 4);
    kit::DisplayList fours;
    kit::add_keyboard(fours, {0, 0, 91 * 4, 25 * 4}, at_four, 1, "board");
    const kit::Control* key_a = key_named(fours, "board.key-a");
    OA_CHECK(key_a != nullptr);
    if (key_a != nullptr) {
        const kit::Colour middle = pixel_of(
            four, key_a->rect.x + key_a->rect.width / 2, key_a->rect.y + key_a->rect.height / 2
        );
        OA_CHECK(middle == kit::colour::list);
    }
}

void paint_matches_the_draws() {
    const auto same = [](const kit::DisplayList& list, auto draw, int32_t width, int32_t height) {
        renderer::Surface direct = black(width, height);
        renderer::Surface painted = black(width, height);
        draw(canvas_on(direct));
        kit::paint(canvas_on(painted), list);
        OA_CHECK(direct.rgb == painted.rgb);
    };
    {
        kit::KeycapLook look;
        look.label = "A";
        look.tint = kit::KeyTint::camera;
        kit::DisplayList list;
        const kit::Rect rect{1, 1, 20, 16};
        kit::add_keycap(list, rect, look, kit::no_control, {});
        same(
            list, [&](const kit::Canvas& canvas) { kit::draw_keycap(canvas, rect, look); }, 24, 20
        );
    }
    {
        kit::ChordChipLook look;
        look.add = true;
        look.none_text = "caption";
        kit::DisplayList list;
        const kit::Rect rect{1, 1, 80, 16};
        kit::add_chord_chip(list, rect, look, kit::no_control, {});
        same(
            list,
            [&](const kit::Canvas& canvas) { kit::draw_chord_chip(canvas, rect, look); },
            90,
            20
        );
    }
    {
        kit::KeyboardLook look;
        look.shape = kit::KeyboardShape::ansi;
        kit::DisplayList list;
        const kit::Rect rect{0, 0, 91, 25};
        kit::add_keyboard(list, rect, look, kit::no_control, {});
        same(
            list, [&](const kit::Canvas& canvas) { kit::draw_keyboard(canvas, rect, look); }, 91, 25
        );
    }
    {
        kit::MouseLook look;
        kit::DisplayList list;
        const kit::Rect rect{0, 0, 40, 64};
        kit::add_mouse(list, rect, look, kit::no_control, {});
        same(list, [&](const kit::Canvas& canvas) { kit::draw_mouse(canvas, rect, look); }, 40, 64);
    }
    {
        kit::HoldRingLook look{1, 3, true};
        kit::DisplayList list;
        const kit::Rect rect{0, 0, 28, 28};
        kit::add_hold_ring(list, rect, look);
        same(
            list,
            [&](const kit::Canvas& canvas) { kit::draw_hold_ring(canvas, rect, look); },
            28,
            28
        );
    }
}

/// The main block alone: its keys and no other, scaled to the picture on
/// their own, each control keeping the number its key has on the whole
/// keyboard, and at a details pane's width every legend drawn whole.
void main_block_draws_its_keys_alone() {
    for (const kit::KeyboardShape shape :
         {kit::KeyboardShape::ansi, kit::KeyboardShape::iso, kit::KeyboardShape::jis}) {
        kit::KeyboardLook look;
        look.shape = shape;
        look.main_block = true;
        OA_CHECK(kit::keyboard_size(look).x == 60 && kit::keyboard_size(look).y == 12);
        kit::KeyboardLook whole = look;
        whole.main_block = false;
        OA_CHECK(kit::keyboard_size(whole).x == 91 && kit::keyboard_size(whole).y == 25);

        // 300 points across is five points a quarter.
        const kit::Rect picture{0, 0, 300, 60};
        kit::DisplayList list;
        kit::add_keyboard(list, picture, look, 10, "board");
        const std::span<const kit::KeyPlace> places = kit::keyboard_places(shape);
        std::size_t drawn = 0;
        for (std::size_t index = 0; index < places.size(); ++index) {
            const bool inside = kit::keyboard_draws(places[index], look);
            OA_CHECK(kit::keyboard_draws(places[index], whole));
            const std::string name = "board." + kit::code_word(places[index].code);
            const kit::Control* control = key_named(list, name);
            OA_CHECK((control != nullptr) == inside);
            if (control == nullptr)
                continue;
            ++drawn;
            OA_CHECK(control->id == 10 + static_cast<kit::ControlId>(index));
            OA_CHECK(kit::wholly_in(control->rect, picture));
        }
        OA_CHECK(drawn == list.controls.size() && drawn > 30);
        for (const std::string_view absent :
             {"Escape", "F1", "Space", "Digit1", "Backquote", "ArrowUp", "Numpad1"})
            OA_CHECK(key_named(list, "board." + kit::code_word(absent)) == nullptr);
        for (const std::string_view present :
             {"Tab", "KeyQ", "CapsLock", "KeyA", "KeyZ", "ShiftLeft", "ShiftRight", "Enter"})
            OA_CHECK(key_named(list, "board." + kit::code_word(present)) != nullptr);
        const kit::Control* key_a = key_named(list, "board." + kit::code_word("KeyA"));
        OA_CHECK(key_a != nullptr && key_a->rect.width == 4 * 5 - 2);
        for (std::size_t index = 0; index < list.controls.size(); ++index)
            for (std::size_t other = index + 1; other < list.controls.size(); ++other)
                OA_CHECK(kept_apart(list.controls[index].rect, list.controls[other].rect));

        renderer::Surface surface = black(picture.width, picture.height);
        kit::begin_text_fit_frame();
        kit::draw_keyboard(canvas_on(surface), picture, look);
        for (const kit::TextFitRecord& record : kit::text_fit_records()) {
            OA_CHECK(!record.wider);
            OA_CHECK(!record.across);
        }
        OA_CHECK(count_colour(surface, kit::colour::text) > 0);
    }
}

/// Fonts whose regular glyphs are each wider than a key's legend box at
/// four points a quarter, and whose small glyphs fit it.
///
/// @return the fonts
const kit::Fonts& wide_regular_fonts() {
    static const kit::Fonts loaded = [] {
        const auto made = [](uint16_t width) {
            renderer::TextFont font;
            font.font.nominal_height = 8;
            constexpr uint8_t ink_index = 1;
            for (int byte = 0; byte < 256; ++byte) {
                constexpr uint16_t height = 8;
                const auto count = static_cast<std::size_t>(width) * height;
                font.font.glyphs[static_cast<std::size_t>(byte)] = oa::formats::fnt::Glyph{
                    width,
                    height,
                    0,
                    0,
                    std::vector<uint8_t>(count, ink_index),
                    std::vector<uint8_t>(count, 1)
                };
            }
            font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
            return font;
        };
        return kit::Fonts{made(12), made(5), {}, {}};
    }();
    return loaded;
}

/// The main block keeps every one-letter legend, a changed key's among them,
/// where the regular font is too wide for its key: it draws it in the small
/// font, whole. The whole keyboard leaves those legends out, as before.
void main_block_keeps_every_legend() {
    kit::KeyboardLook look;
    look.main_block = true;
    for (const std::string_view code : {"KeyQ", "KeyW"}) {
        kit::KeyboardKey key;
        key.code = std::string(code);
        key.cap.held = true;
        look.keys.push_back(std::move(key));
    }
    // 240 points across is four points a quarter: a letter's key is 14
    // points, its legend box 10, and a regular glyph 12.
    const kit::Rect picture{0, 0, 240, 48};
    renderer::Surface surface = black(picture.width, picture.height);
    const kit::Canvas canvas{&surface, {0, 0, 1, {}}, &wide_regular_fonts(), {}};
    kit::draw_keyboard(canvas, picture, look);
    kit::DisplayList list;
    kit::add_keyboard(list, picture, look, 1, "board");
    // A key's legend is ink of its own colour inside it: the text colour on a
    // free key, the ink for the accent on a changed one.
    const auto inked = [](const renderer::Surface& drawn, const kit::Rect& key, kit::Colour ink) {
        for (int32_t y = key.y; y < key.y + key.height; ++y)
            for (int32_t x = key.x; x < key.x + key.width; ++x)
                if (pixel_of(drawn, x, y) == ink)
                    return true;
        return false;
    };
    for (const kit::KeyPlace& place : kit::keyboard_places(look.shape)) {
        if (!kit::keyboard_draws(place, look) || place.label.size() != 1)
            continue;
        const kit::Control* key = key_named(list, "board." + kit::code_word(place.code));
        OA_CHECK(key != nullptr);
        if (key == nullptr)
            continue;
        const bool changed = place.code == "KeyQ" || place.code == "KeyW";
        OA_CHECK(inked(surface, key->rect, changed ? kit::colour::on_accent : kit::colour::text));
    }

    // The whole keyboard at the same size leaves a wide legend out, as before.
    kit::KeyboardLook whole = look;
    whole.main_block = false;
    const kit::Rect board{0, 0, 91 * 4, 25 * 4};
    renderer::Surface whole_surface = black(board.width, board.height);
    const kit::Canvas whole_canvas{&whole_surface, {0, 0, 1, {}}, &wide_regular_fonts(), {}};
    kit::draw_keyboard(whole_canvas, board, whole);
    kit::DisplayList whole_list;
    kit::add_keyboard(whole_list, board, whole, 1, "board");
    const kit::Control* key_e = key_named(whole_list, "board." + kit::code_word("KeyE"));
    OA_CHECK(key_e != nullptr && !inked(whole_surface, key_e->rect, kit::colour::text));
}

} // namespace

int main() {
    geometry_holds();
    words_match_the_examples();
    keyboard_names_one_control_per_key();
    mouse_names_its_buttons();
    marks_differ_in_shape();
    clash_mark_paints_danger();
    tints_reach_contrast();
    ring_snaps_to_quarters();
    parts_stay_inside_at_every_scale();
    small_keyboard_keys_stay_apart();
    main_block_draws_its_keys_alone();
    main_block_keeps_every_legend();
    paint_matches_the_draws();
    std::puts("ui-kit-keys passed");
    return oa::test::check_exit_status();
}
