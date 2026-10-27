// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog's controls, drawn through the canvas's primitives. The
// arithmetic is the dialog's, moved here unchanged. paint draws every role,
// the controls of controls.hpp among them.

#include "oa/ui/kit/components.hpp"

#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/first_run.hpp"
#include "oa/ui/kit/keys.hpp"

#include "oa/ui/frontend_renderer/artless.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace oa::ui::kit {

namespace renderer = oa::ui::frontend_renderer;

namespace {

/// The app icon the maker's mark draws, or empty so the badge stays a plate.
OpenAnnihilationMark installed_oa_mark{};

} // namespace

void set_open_annihilation_mark(OpenAnnihilationMark mark) {
    installed_oa_mark = mark;
}

const OpenAnnihilationMark& open_annihilation_mark() noexcept {
    return installed_oa_mark;
}

namespace {

/// The OA letters, 9 by 7, row by row.
constexpr std::array<uint8_t, 9 * 7> kSmallMarkBits{
    0, 1, 1, 0, 0, 0, 1, 1, 0, //
    1, 0, 0, 1, 0, 1, 0, 0, 1, //
    1, 0, 0, 1, 0, 1, 0, 0, 1, //
    1, 0, 0, 1, 0, 1, 1, 1, 1, //
    1, 0, 0, 1, 0, 1, 0, 0, 1, //
    1, 0, 0, 1, 0, 1, 0, 0, 1, //
    0, 1, 1, 0, 0, 1, 0, 0, 1, //
};
/// The OA letters, 12 by 9, row by row.
constexpr std::array<uint8_t, 12 * 9> kLargeMarkBits{
    0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, //
    1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, //
    1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, //
    1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, //
    1, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, //
    1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, //
    1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, //
    1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, //
    0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, //
};
/// The padlock, 5 by 7, row by row.
constexpr std::array<uint8_t, 5 * 7> kPadlockBits{
    0, 1, 1, 1, 0, //
    1, 0, 0, 0, 1, //
    1, 0, 0, 0, 1, //
    1, 1, 1, 1, 1, //
    1, 1, 0, 1, 1, //
    1, 1, 0, 1, 1, //
    1, 1, 1, 1, 1, //
};
/// An arrow pointing right, 5 by 5, row by row.
constexpr std::array<uint8_t, 5 * 5> kClosedArrowBits{
    0, 1, 0, 0, 0, //
    0, 1, 1, 0, 0, //
    0, 1, 1, 1, 0, //
    0, 1, 1, 0, 0, //
    0, 1, 0, 0, 0, //
};
/// An arrow pointing down, 5 by 5, row by row.
constexpr std::array<uint8_t, 5 * 5> kOpenArrowBits{
    0, 0, 0, 0, 0, //
    1, 1, 1, 1, 1, //
    0, 1, 1, 1, 0, //
    0, 0, 1, 0, 0, //
    0, 0, 0, 0, 0, //
};
/// A drop-down's arrow, 7 by 4, row by row.
constexpr std::array<uint8_t, 7 * 4> kChoiceArrowBits{
    1, 1, 1, 1, 1, 1, 1, //
    0, 1, 1, 1, 1, 1, 0, //
    0, 0, 1, 1, 1, 0, 0, //
    0, 0, 0, 1, 0, 0, 0, //
};

/// A search field's magnifier, 7 by 7, row by row.
constexpr std::array<uint8_t, 7 * 7> kMagnifierBits{
    0, 1, 1, 1, 0, 0, 0, //
    1, 0, 0, 0, 1, 0, 0, //
    1, 0, 0, 0, 1, 0, 0, //
    1, 0, 0, 0, 1, 0, 0, //
    0, 1, 1, 1, 1, 0, 0, //
    0, 0, 0, 0, 0, 1, 0, //
    0, 0, 0, 0, 0, 0, 1, //
};
/// A signing key, 7 by 7, row by row: the bow, then the shaft and its teeth.
constexpr std::array<uint8_t, 7 * 7> kKeyBits{
    0, 1, 1, 1, 0, 0, 0, //
    1, 0, 0, 0, 1, 0, 0, //
    1, 0, 1, 0, 1, 1, 1, //
    1, 0, 0, 0, 1, 0, 1, //
    0, 1, 1, 1, 0, 0, 1, //
    0, 0, 0, 0, 0, 1, 1, //
    0, 0, 0, 0, 0, 0, 1, //
};

/// The OA button's attention dot, 7 by 7, row by row: the whole disc, which
/// its ring's colour fills.
constexpr std::array<uint8_t, 7 * 7> kDotBits{
    0, 0, 1, 1, 1, 0, 0, //
    0, 1, 1, 1, 1, 1, 0, //
    1, 1, 1, 1, 1, 1, 1, //
    1, 1, 1, 1, 1, 1, 1, //
    1, 1, 1, 1, 1, 1, 1, //
    0, 1, 1, 1, 1, 1, 0, //
    0, 0, 1, 1, 1, 0, 0, //
};
/// The dot's face inside its one-point ring, 5 by 5, row by row, drawn one
/// point in from the disc's top left.
constexpr std::array<uint8_t, 5 * 5> kDotFaceBits{
    0, 1, 1, 1, 0, //
    1, 1, 1, 1, 1, //
    1, 1, 1, 1, 1, //
    1, 1, 1, 1, 1, //
    0, 1, 1, 1, 0, //
};

constexpr renderer::Mark kSmallMark{9, 7, kSmallMarkBits};
constexpr renderer::Mark kDot{7, 7, kDotBits};
constexpr renderer::Mark kDotFace{5, 5, kDotFaceBits};
/// The dot's ring, in points: its face starts this far in from the disc's edge.
constexpr int32_t kDotRing = 1;
static_assert(
    kDot.width == oa_button_dot_side && kDot.height == oa_button_dot_side &&
        kDotFace.width == oa_button_dot_side - 2 * kDotRing &&
        kDotFace.height == oa_button_dot_side - 2 * kDotRing,
    "the dot is oa_button_dot_side square, its face one ring in"
);
constexpr renderer::Mark kLargeMark{12, 9, kLargeMarkBits};
constexpr renderer::Mark kPadlock{5, 7, kPadlockBits};
constexpr renderer::Mark kClosedArrow{5, 5, kClosedArrowBits};
constexpr renderer::Mark kOpenArrow{5, 5, kOpenArrowBits};
constexpr renderer::Mark kChoiceArrow{7, 4, kChoiceArrowBits};
constexpr renderer::Mark kMagnifier{7, 7, kMagnifierBits};
constexpr renderer::Mark kKeyMark{7, 7, kKeyBits};
static_assert(
    kMagnifier.width == compact_metrics.magnifier_side &&
        kMagnifier.height == compact_metrics.magnifier_side,
    "the magnifier is magnifier_side square"
);

/// The chosen menu item's marker, the same mark the section list uses.
constexpr int32_t kChosenMarkerOffset = 5;
constexpr int32_t kChosenMarkerWidth = 2;
constexpr int32_t kChosenMarkerHeight = 8;
/// The columns a menu item keeps clear at its right.
constexpr int32_t kChoiceTextMargin = 4;

const renderer::Mark& bits_of(Mark mark) noexcept {
    switch (mark) {
    case Mark::oa_small:
        return kSmallMark;
    case Mark::oa_large:
        return kLargeMark;
    case Mark::padlock:
        return kPadlock;
    case Mark::arrow_closed:
        return kClosedArrow;
    case Mark::arrow_open:
        return kOpenArrow;
    case Mark::choice_arrow:
        return kChoiceArrow;
    case Mark::magnifier:
        return kMagnifier;
    case Mark::key:
        return kKeyMark;
    }
    return kSmallMark;
}

bool drawable(const Canvas& canvas) noexcept {
    return can_draw(canvas);
}

void paint_text(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink
) {
    box_text(canvas, role, text, box, align, ink);
}

/// A button's face, when it has one, its border and its caption.
struct ButtonColours {
    std::optional<Colour> face{};
    Colour border{};
    Colour caption{};
};

/// Returns a button's colours: the primary button lime with dark text, the
/// secondary raised with the strong edge, the quiet one a lime caption with
/// no face at rest, the inset one the list's face, the filled one deep green
/// with white text, and the danger one outlined in red. A button that is not
/// enabled has no face, the control border and the idle caption.
///
/// @param look the button
/// @return its face, when it has one, its border and its caption
ButtonColours button_colours(const ButtonLook& look) noexcept {
    if (look.style == ButtonStyle::accent) {
        Colour face = colour::accent;
        if (look.held)
            face = colour::accent_held;
        else if (look.hovered || look.bright)
            face = colour::accent_light;
        return {face, face, colour::on_accent};
    }
    const bool hovered = look.enabled && look.hovered;
    const bool held = look.enabled && look.held;
    if (!look.enabled)
        return {std::nullopt, colour::control_border, colour::switch_idle};
    switch (look.style) {
    case ButtonStyle::filled: {
        Colour face = colour::filled;
        if (held)
            face = colour::filled_held;
        else if (hovered)
            face = colour::filled_light;
        return {face, hovered ? colour::accent : colour::accent_held, colour::text};
    }
    case ButtonStyle::quiet: {
        std::optional<Colour> face;
        if (held)
            face = colour::list_selected;
        else if (hovered)
            face = colour::hover;
        return {
            face, face.value_or(colour::panel), hovered ? colour::accent_light : colour::accent
        };
    }
    case ButtonStyle::inset: {
        Colour face = colour::list;
        if (held)
            face = colour::band;
        else if (hovered)
            face = colour::hover;
        return {face, hovered ? colour::control_hover : colour::control_border, colour::text};
    }
    case ButtonStyle::danger: {
        std::optional<Colour> face;
        if (held || hovered)
            face = colour::danger_tint;
        return {face, colour::danger, colour::danger};
    }
    case ButtonStyle::accent:
    case ButtonStyle::plain:
        break;
    }
    Colour face = colour::hover;
    if (held)
        face = colour::panel;
    else if (hovered)
        face = colour::list_selected;
    return {face, hovered ? colour::control_hover : colour::strong_edge, colour::text};
}

/// Draws a control's rounded ground: its face, when it has one, inside a
/// one-point border, or the border alone. The border is the whole shape and
/// the face the shape one point in, so a partial pixel is one colour or the
/// other.
///
/// @param canvas where it is drawn
/// @param area the control
/// @param radius the corners' radius, in points
/// @param face the face; none leaves what is under the control
/// @param border the border's colour
void draw_rounded_ground(
    const Canvas& canvas,
    const Rect& area,
    int32_t radius,
    std::optional<Colour> face,
    Colour border
) {
    if (!face) {
        outline_corner_rect(canvas, area, radius, border);
        return;
    }
    if (*face == border || area.width <= 2 || area.height <= 2) {
        fill_corner_rect(canvas, area, radius, *face);
        return;
    }
    fill_corner_rect(canvas, area, radius, border);
    fill_corner_rect(
        canvas,
        {area.x + 1, area.y + 1, area.width - 2, area.height - 2},
        std::max(radius - 1, int32_t{0}),
        *face
    );
}

void draw_mark_square(const Canvas& canvas, int32_t side, Colour ink) {
    if (!drawable(canvas))
        return;
    const int32_t square_side =
        side * compact_metrics.button_square_numerator / compact_metrics.button_square_denominator;
    const int32_t square_offset = (side - square_side) / 2;
    const Rect square{square_offset, square_offset, square_side, square_side};
    outline_rect(canvas, square, ink);
    const renderer::Mark& mark =
        square_side - 2 >= kLargeMark.width + 2 * compact_metrics.large_mark_margin ? kLargeMark
                                                                                    : kSmallMark;
    draw_mark(
        canvas,
        mark,
        {square_offset + (square_side - mark.width) / 2,
         square_offset + (square_side - mark.height + 1) / 2},
        ink
    );
}

void add_control(
    DisplayList& list,
    const Rect& rect,
    ControlId id,
    std::string name,
    ControlKind kind,
    bool steps,
    bool checked,
    std::string text,
    bool enabled
) {
    Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = enabled;
    control.name = std::move(name);
    control.kind = kind;
    control.steps = steps;
    control.checked = checked;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

void paint_item(const Canvas& canvas, const Item& item) {
    Canvas drawn = canvas;
    if (item.clip.width > 0 && item.clip.height > 0)
        drawn = clipped(canvas, item.clip);
    if (!drawable(drawn))
        return;
    switch (item.role) {
    case Role::fill:
        fill_rect(drawn, item.rect, item.colour);
        break;
    case Role::blend:
        blend_rect(drawn, item.rect, item.colour, item.opacity);
        break;
    case Role::outline:
        outline_rect(drawn, item.rect, item.colour);
        break;
    case Role::bevel:
        bevel_rect(drawn, item.rect, item.colour, colour::edge_dark);
        break;
    case Role::rule:
        rule_rect(drawn, item.rect, item.colour);
        break;
    case Role::text:
        paint_text(drawn, item.font, item.text, item.rect, item.align, item.colour);
        break;
    case Role::picture:
        if (const auto* look = std::get_if<PictureLook>(&item.look))
            draw_picture(drawn, item.rect, look->picture);
        else
            draw_picture(drawn, item.rect, drawn.icon);
        break;
    case Role::mark:
        if (const auto* look = std::get_if<MarkLook>(&item.look))
            draw_mark(drawn, look->mark, {item.rect.x, item.rect.y}, look->colour);
        break;
    case Role::button:
        if (const auto* look = std::get_if<ButtonLook>(&item.look))
            draw_button(drawn, item.rect, *look);
        break;
    case Role::toggle:
        if (const auto* look = std::get_if<SwitchLook>(&item.look))
            draw_switch(drawn, item.rect, *look);
        break;
    case Role::levels:
        if (const auto* look = std::get_if<LevelsLook>(&item.look))
            draw_levels(drawn, item.rect, *look);
        break;
    case Role::slider:
        if (const auto* look = std::get_if<SliderLook>(&item.look))
            draw_slider(drawn, item.rect, *look);
        break;
    case Role::choice:
        if (const auto* look = std::get_if<ChoiceLook>(&item.look))
            draw_choice(drawn, item.rect, *look);
        break;
    case Role::choice_menu:
        if (const auto* look = std::get_if<ChoiceMenuLook>(&item.look))
            draw_choice_menu(drawn, item.rect, *look);
        break;
    case Role::lock:
        if (const auto* look = std::get_if<LockLook>(&item.look))
            draw_lock(drawn, item.rect, *look);
        break;
    case Role::scroll_bar:
        if (const auto* look = std::get_if<ScrollBarLook>(&item.look))
            draw_scroll_bar(drawn, item.rect, look->thumb, look->hot);
        break;
    case Role::focus_ring:
        if (const auto* look = std::get_if<FocusRingLook>(&item.look))
            draw_focus_ring(drawn, item.rect, look->around);
        break;
    case Role::oa_button:
        if (const auto* look = std::get_if<OaButtonLook>(&item.look))
            draw_oa_button(
                placed_at(drawn, {item.rect.x, item.rect.y}),
                look->side,
                look->state,
                OaButtonMark{look->dot, look->corner, look->count, look->turn}
            );
        break;
    case Role::oa_mark:
        draw_oa_mark(placed_at(drawn, {item.rect.x, item.rect.y}), item.rect.width);
        break;
    case Role::header:
        if (const auto* look = std::get_if<HeaderLook>(&item.look))
            draw_header(drawn, *look);
        break;
    case Role::footer_band:
        if (const auto* look = std::get_if<FooterBandLook>(&item.look))
            draw_footer_band(drawn, look->width, look->rule_row);
        break;
    case Role::nav:
        if (const auto* look = std::get_if<NavLook>(&item.look))
            draw_nav(drawn, *look);
        break;
    case Role::heading:
        if (const auto* look = std::get_if<HeadingLook>(&item.look))
            draw_heading(drawn, item.rect, look->text);
        break;
    case Role::row_frame:
        if (const auto* look = std::get_if<RowFrame>(&item.look))
            draw_row_frame(drawn, *look);
        break;
    case Role::locked_fade:
        draw_locked_fade(drawn, item.rect);
        break;
    case Role::list_row:
        if (const auto* look = std::get_if<ListRowLook>(&item.look))
            draw_list_row(drawn, item.rect, *look);
        break;
    case Role::chip:
        if (const auto* look = std::get_if<ChipLook>(&item.look))
            draw_chip(drawn, item.rect, *look);
        break;
    case Role::pill:
        if (const auto* look = std::get_if<PillLook>(&item.look))
            draw_pill(drawn, item.rect, *look);
        break;
    case Role::field:
        if (const auto* look = std::get_if<SearchLook>(&item.look))
            draw_search(drawn, item.rect, *look);
        break;
    case Role::tabs:
        if (const auto* look = std::get_if<TabsLook>(&item.look))
            draw_tabs(drawn, item.rect, *look);
        break;
    case Role::card:
        if (const auto* look = std::get_if<CardLook>(&item.look))
            draw_card(drawn, item.rect, *look);
        break;
    case Role::hover_card:
        if (const auto* look = std::get_if<HoverCardLook>(&item.look))
            draw_hover_card(drawn, item.rect, *look);
        break;
    case Role::link:
        if (const auto* look = std::get_if<LinkLook>(&item.look))
            draw_link(drawn, item.rect, *look);
        break;
    case Role::progress:
        if (const auto* look = std::get_if<ProgressLook>(&item.look))
            draw_progress(drawn, item.rect, look->done, look->whole, look->paused);
        break;
    case Role::language_card:
        if (const auto* look = std::get_if<LanguageCardLook>(&item.look))
            draw_language_card(drawn, item.rect, *look);
        break;
    case Role::carousel:
        if (const auto* look = std::get_if<CarouselLook>(&item.look))
            draw_carousel(drawn, item.rect, *look);
        break;
    case Role::step_indicator:
        if (const auto* look = std::get_if<StepLook>(&item.look))
            draw_step_indicator(drawn, item.rect, *look);
        break;
    case Role::setup_chrome:
        if (const auto* look = std::get_if<SetupChromeLook>(&item.look))
            draw_setup_chrome(placed_at(drawn, {item.rect.x, item.rect.y}), *look);
        break;
    case Role::choice_card:
        if (const auto* look = std::get_if<ChoiceCardLook>(&item.look))
            draw_choice_card(drawn, item.rect, *look);
        break;
    case Role::coach_mark:
        if (const auto* look = std::get_if<CoachMarkLook>(&item.look))
            draw_coach_mark(drawn, item.rect, *look);
        break;
    case Role::spotlight:
        if (const auto* look = std::get_if<SpotlightLook>(&item.look))
            draw_spotlight(drawn, item.rect, *look);
        break;
    case Role::picture_well:
        if (const auto* look = std::get_if<PictureWellLook>(&item.look))
            draw_picture_well(drawn, item.rect, *look);
        break;
    case Role::call_to_action:
        if (const auto* look = std::get_if<CallToActionLook>(&item.look))
            draw_call_to_action(drawn, item.rect, *look);
        break;
    case Role::package_badge:
        if (const auto* look = std::get_if<BadgeLook>(&item.look))
            draw_badge(drawn, item.rect, *look);
        break;
    case Role::grip:
        if (const auto* look = std::get_if<GripLook>(&item.look))
            draw_grip(drawn, item.rect, *look);
        break;
    case Role::keycap:
        if (const auto* look = std::get_if<KeycapLook>(&item.look))
            draw_keycap(drawn, item.rect, *look);
        break;
    case Role::chord_chip:
        if (const auto* look = std::get_if<ChordChipLook>(&item.look))
            draw_chord_chip(drawn, item.rect, *look);
        break;
    case Role::keyboard:
        if (const auto* look = std::get_if<KeyboardLook>(&item.look))
            draw_keyboard(drawn, item.rect, *look);
        break;
    case Role::mouse_picture:
        if (const auto* look = std::get_if<MouseLook>(&item.look))
            draw_mouse(drawn, item.rect, *look);
        break;
    case Role::hold_ring:
        if (const auto* look = std::get_if<HoldRingLook>(&item.look))
            draw_hold_ring(drawn, item.rect, *look);
        break;
    case Role::header_bar:
    case Role::badge:
    case Role::header_text:
    case Role::version:
    case Role::title:
    case Role::panel:
    case Role::icon:
    case Role::banner:
    case Role::row:
    case Role::divider:
    case Role::button_main:
    case Role::button_danger:
    case Role::progress_track:
    case Role::progress_fill:
    case Role::progress_busy:
    case Role::backdrop:
    case Role::sheet:
        // A screen drawn in the modern fonts draws these parts with its own
        // painter; the kit's canvas, in either form, draws nothing for them.
        break;
    }
}

// ---- State icons ----
//
// A state icon is drawn from rings and strokes worked out at the canvas's own
// pixels, so that it keeps its edges at every scale. Its design is a square
// of 24 units, each written here in quarters; the icon fills the shorter
// side of the rectangle it is drawn in, centred along the longer.

/// A state icon's square, in quarter units: 24 units.
constexpr int64_t kIconGrid = 96;
/// The samples across, and the samples down, each pixel a state icon's
/// coverage is counted from: 16 a pixel, so its edges take 17 shades.
constexpr int64_t kIconSamples = 4;
/// The most pixels across or down a state icon is drawn at; a larger
/// rectangle draws nothing.
constexpr int32_t kMostIconPixels = 1024;

/// A point of a state icon's design, in quarter units from its top left.
struct IconPoint {
    int64_t x{}; ///< across
    int64_t y{}; ///< down
};

/// A stroke of a state icon's design: a straight line with round ends.
struct IconStroke {
    IconPoint from{}; ///< one end
    IconPoint to{};   ///< the other end
};

/// The ring of the circled icons: its centre, and the radii of its outer and
/// inner edges, in quarter units: 2 units wide, its outer edge a unit in.
constexpr IconPoint kRingCentre{48, 48};
constexpr int64_t kRingOuter = 44;
constexpr int64_t kRingInner = 36;
/// Half a stroke's width, in quarter units: every stroke is 2.5 units wide.
constexpr int64_t kStrokeHalf = 5;

/// The check: down to the right, then up to the right.
constexpr std::array<IconStroke, 2> kCheckStrokes{{
    {{26, 50}, {42, 66}},
    {{42, 66}, {70, 34}},
}};
/// The up-arrow: its shaft, then its head's two sides.
constexpr std::array<IconStroke, 3> kUpArrowStrokes{{
    {{48, 70}, {48, 28}},
    {{30, 46}, {48, 28}},
    {{48, 28}, {66, 46}},
}};
/// The download arrow: its shaft and its head's two sides, then the tray
/// under it, its two sides and its bottom.
constexpr std::array<IconStroke, 6> kDownloadStrokes{{
    {{48, 12}, {48, 62}},
    {{26, 40}, {48, 62}},
    {{48, 62}, {70, 40}},
    {{14, 62}, {14, 82}},
    {{14, 82}, {82, 82}},
    {{82, 82}, {82, 62}},
}};
/// A signing key: a twelve-sided bow, a shaft to the right, and two teeth.
/// Every end stays kStrokeHalf inside the square, so the round ends are not
/// clipped. The bow's centre is (28, 48) and its radius is 16.
constexpr std::array<IconStroke, 15> kKeyStrokes{{
    {{44, 48}, {42, 56}},
    {{42, 56}, {36, 62}},
    {{36, 62}, {28, 64}},
    {{28, 64}, {20, 62}},
    {{20, 62}, {14, 56}},
    {{14, 56}, {12, 48}},
    {{12, 48}, {14, 40}},
    {{14, 40}, {20, 34}},
    {{20, 34}, {28, 32}},
    {{28, 32}, {36, 34}},
    {{36, 34}, {42, 40}},
    {{42, 40}, {44, 48}},
    {{40, 48}, {88, 48}},
    {{70, 48}, {70, 66}},
    {{82, 48}, {82, 76}},
}};

/// Reports whether a key stroke's round ends stay inside the icon square.
///
/// @return true when every end is kStrokeHalf inside the square
constexpr bool key_strokes_fit() noexcept {
    constexpr int64_t least = kStrokeHalf;
    constexpr int64_t most = kIconGrid - kStrokeHalf;
    for (const IconStroke& stroke : kKeyStrokes) {
        const IconPoint ends[] = {stroke.from, stroke.to};
        for (const IconPoint end : ends) {
            if (end.x < least || end.x > most || end.y < least || end.y > most)
                return false;
        }
    }
    return true;
}

static_assert(key_strokes_fit(), "the key glyph stays inside its square");

/// The statistics tag: two axes and three bars.
constexpr std::array<IconStroke, 5> kStatisticsStrokes{{
    {{12, 12}, {12, 84}},
    {{12, 84}, {84, 84}},
    {{32, 68}, {32, 52}},
    {{52, 68}, {52, 28}},
    {{72, 68}, {72, 40}},
}};
/// The economy tag: a closed bolt.
constexpr std::array<IconStroke, 6> kEconomyStrokes{{
    {{52, 8}, {12, 56}},
    {{12, 56}, {48, 56}},
    {{48, 56}, {44, 88}},
    {{44, 88}, {84, 40}},
    {{84, 40}, {48, 40}},
    {{48, 40}, {52, 8}},
}};
/// The commanders tag: a closed ten-point star.
constexpr std::array<IconStroke, 10> kCommandersStrokes{{
    {{48, 8}, {60, 32}},
    {{60, 32}, {86, 36}},
    {{86, 36}, {67, 54}},
    {{67, 54}, {72, 80}},
    {{72, 80}, {48, 68}},
    {{48, 68}, {24, 80}},
    {{24, 80}, {29, 54}},
    {{29, 54}, {10, 36}},
    {{10, 36}, {36, 32}},
    {{36, 32}, {48, 8}},
}};
/// The new-units tag: a square with square corners, and a plus.
constexpr std::array<IconStroke, 6> kNewUnitsStrokes{{
    {{12, 12}, {84, 12}},
    {{84, 12}, {84, 84}},
    {{84, 84}, {12, 84}},
    {{12, 84}, {12, 12}},
    {{48, 32}, {48, 64}},
    {{32, 48}, {64, 48}},
}};
/// The disable tag: a sixteen-sided ring of radius 9 units, and a slash.
constexpr std::array<IconStroke, 17> kDisableStrokes{{
    {{84, 48}, {81, 62}},
    {{81, 62}, {73, 73}},
    {{73, 73}, {62, 81}},
    {{62, 81}, {48, 84}},
    {{48, 84}, {34, 81}},
    {{34, 81}, {23, 73}},
    {{23, 73}, {15, 62}},
    {{15, 62}, {12, 48}},
    {{12, 48}, {15, 34}},
    {{15, 34}, {23, 23}},
    {{23, 23}, {34, 15}},
    {{34, 15}, {48, 12}},
    {{48, 12}, {62, 15}},
    {{62, 15}, {73, 23}},
    {{73, 23}, {81, 34}},
    {{81, 34}, {84, 48}},
    {{22, 22}, {74, 74}},
}};
/// The gameplay tag: a pole and a closed pennant.
constexpr std::array<IconStroke, 6> kGameplayStrokes{{
    {{20, 88}, {20, 12}},
    {{20, 16}, {68, 16}},
    {{68, 16}, {58, 34}},
    {{58, 34}, {68, 52}},
    {{68, 52}, {20, 52}},
    {{20, 52}, {20, 16}},
}};
/// The ai tag: a 12-unit square with square corners, and eight pins.
constexpr std::array<IconStroke, 12> kAiStrokes{{
    {{24, 24}, {72, 24}},
    {{72, 24}, {72, 72}},
    {{72, 72}, {24, 72}},
    {{24, 72}, {24, 24}},
    {{36, 8}, {36, 24}},
    {{60, 8}, {60, 24}},
    {{36, 72}, {36, 88}},
    {{60, 72}, {60, 88}},
    {{8, 36}, {24, 36}},
    {{8, 60}, {24, 60}},
    {{72, 36}, {88, 36}},
    {{72, 60}, {88, 60}},
}};
/// The maps tag: a closed folded outline and its two folds.
constexpr std::array<IconStroke, 10> kMapsStrokes{{
    {{12, 24}, {36, 16}},
    {{36, 16}, {60, 24}},
    {{60, 24}, {84, 16}},
    {{84, 16}, {84, 72}},
    {{84, 72}, {60, 80}},
    {{60, 80}, {36, 72}},
    {{36, 72}, {12, 80}},
    {{12, 80}, {12, 24}},
    {{36, 16}, {36, 72}},
    {{60, 24}, {60, 80}},
}};

/// Reports whether every tag stroke's round ends stay inside the icon square.
///
/// @return true when every end is kStrokeHalf inside the square
constexpr bool tag_strokes_fit() noexcept {
    constexpr int64_t least = kStrokeHalf;
    constexpr int64_t most = kIconGrid - kStrokeHalf;
    const std::array<std::span<const IconStroke>, 8> designs{
        kStatisticsStrokes,
        kEconomyStrokes,
        kCommandersStrokes,
        kNewUnitsStrokes,
        kDisableStrokes,
        kGameplayStrokes,
        kAiStrokes,
        kMapsStrokes,
    };
    for (const std::span<const IconStroke> design : designs) {
        for (const IconStroke& stroke : design) {
            const IconPoint ends[] = {stroke.from, stroke.to};
            for (const IconPoint end : ends) {
                if (end.x < least || end.x > most || end.y < least || end.y > most)
                    return false;
            }
        }
    }
    return true;
}

static_assert(tag_strokes_fit(), "every tag icon stays inside its square");

/// Returns a tag icon's strokes.
///
/// @param icon the icon
/// @return its strokes, in quarter units
std::span<const IconStroke> tag_strokes(TagIcon icon) noexcept {
    switch (icon) {
    case TagIcon::statistics:
        return kStatisticsStrokes;
    case TagIcon::economy:
        return kEconomyStrokes;
    case TagIcon::commanders:
        return kCommandersStrokes;
    case TagIcon::new_units:
        return kNewUnitsStrokes;
    case TagIcon::disable:
        return kDisableStrokes;
    case TagIcon::gameplay:
        return kGameplayStrokes;
    case TagIcon::ai:
        return kAiStrokes;
    case TagIcon::maps:
        return kMapsStrokes;
    }
    return {};
}

/// Returns a state icon's strokes.
///
/// @param icon the icon
/// @return its strokes, in quarter units
std::span<const IconStroke> strokes_of(StateIcon icon) noexcept {
    switch (icon) {
    case StateIcon::circled_check:
        return kCheckStrokes;
    case StateIcon::circled_up_arrow:
        return kUpArrowStrokes;
    case StateIcon::download_arrow:
        return kDownloadStrokes;
    case StateIcon::key:
        return kKeyStrokes;
    case StateIcon::oa:
    case StateIcon::bone:
        return {};
    }
    return {};
}

/// Tells whether a state icon has the ring round its strokes.
///
/// @param icon the icon
/// @return true for the circled icons
bool ringed(StateIcon icon) noexcept {
    return icon == StateIcon::circled_check || icon == StateIcon::circled_up_arrow;
}

/// Tells whether a point lies within a distance of a stroke.
///
/// @param point the point
/// @param stroke the stroke
/// @param half_width the distance, in the same units
/// @return true when the point is that near the stroke or its round ends
bool near_stroke(IconPoint point, const IconStroke& stroke, int64_t half_width) noexcept {
    const int64_t along_x = stroke.to.x - stroke.from.x;
    const int64_t along_y = stroke.to.y - stroke.from.y;
    const int64_t from_x = point.x - stroke.from.x;
    const int64_t from_y = point.y - stroke.from.y;
    const int64_t reach_squared = half_width * half_width;
    const int64_t length_squared = along_x * along_x + along_y * along_y;
    const int64_t projected = from_x * along_x + from_y * along_y;
    if (length_squared == 0 || projected <= 0)
        return from_x * from_x + from_y * from_y <= reach_squared;
    if (projected >= length_squared) {
        const int64_t to_x = point.x - stroke.to.x;
        const int64_t to_y = point.y - stroke.to.y;
        return to_x * to_x + to_y * to_y <= reach_squared;
    }
    const int64_t across = from_x * along_y - from_y * along_x;
    return across * across <= reach_squared * length_squared;
}

/// Returns how much of each pixel of a rectangle a state icon covers.
///
/// The icon fills a square of the rectangle's shorter side, centred along
/// the longer. Its design is scaled to that square, each place rounded to
/// an eighth of a pixel, and each pixel counts which of its 16 samples, at
/// the middles of a 4 by 4 grid, lie in the ring or within half a stroke's
/// width of a stroke. The arithmetic is in integers, so every machine draws
/// the same pixels.
///
/// @param icon the icon
/// @param columns the rectangle's pixels across, at most kMostIconPixels
/// @param rows the rectangle's pixels down, at most kMostIconPixels
/// @return columns times rows bytes, rows top to bottom: 0 uncovered to 255 covered
std::vector<uint8_t> state_icon_coverage(StateIcon icon, int32_t columns, int32_t rows) {
    std::vector<uint8_t> coverage(
        static_cast<std::size_t>(std::max(columns, 0)) * static_cast<std::size_t>(std::max(rows, 0))
    );
    const int32_t side = std::min(columns, rows);
    if (side <= 0)
        return coverage;
    const int32_t left = (columns - side) / 2;
    const int32_t top = (rows - side) / 2;
    // Places in eighths of a pixel of the square: a sample lies at an odd
    // eighth, the middle of its sixteenth of the pixel.
    const int64_t square = 2 * kIconSamples * side;
    const auto scaled = [square](int64_t quarters) {
        return (quarters * square + kIconGrid / 2) / kIconGrid;
    };
    const IconPoint centre{scaled(kRingCentre.x), scaled(kRingCentre.y)};
    const int64_t outer = scaled(kRingOuter);
    const int64_t inner = scaled(kRingInner);
    const int64_t half_width = scaled(kStrokeHalf);
    std::vector<IconStroke> strokes;
    for (const IconStroke& stroke : strokes_of(icon))
        strokes.push_back(
            {{scaled(stroke.from.x), scaled(stroke.from.y)},
             {scaled(stroke.to.x), scaled(stroke.to.y)}}
        );
    const bool ring = ringed(icon);
    const auto covers = [&](IconPoint point) {
        if (ring) {
            const int64_t across = point.x - centre.x;
            const int64_t down = point.y - centre.y;
            const int64_t distance_squared = across * across + down * down;
            if (distance_squared <= outer * outer && distance_squared >= inner * inner)
                return true;
        }
        for (const IconStroke& stroke : strokes)
            if (near_stroke(point, stroke, half_width))
                return true;
        return false;
    };
    constexpr int64_t samples = kIconSamples * kIconSamples;
    for (int32_t row = 0; row < side; ++row)
        for (int32_t column = 0; column < side; ++column) {
            int64_t inside = 0;
            for (int64_t down = 0; down < kIconSamples; ++down)
                for (int64_t across = 0; across < kIconSamples; ++across)
                    if (covers(
                            {2 * kIconSamples * column + 2 * across + 1,
                             2 * kIconSamples * row + 2 * down + 1}
                        ))
                        ++inside;
            const std::size_t at =
                static_cast<std::size_t>(top + row) * static_cast<std::size_t>(columns) +
                static_cast<std::size_t>(left + column);
            coverage[at] = static_cast<uint8_t>((inside * 255 + samples / 2) / samples);
        }
    return coverage;
}

/// An ×: two strokes, in the state icon's quarter units. The ends stay
/// inside the square so the round caps are not clipped. It is drawn, not
/// taken from a font, so a language pack's face cannot lack it.
constexpr std::array<IconStroke, 2> kCloseStrokes{{
    {{22, 22}, {74, 74}},
    {{74, 22}, {22, 74}},
}};

/// A circular arrow: an arc of straight strokes and an arrowhead, in the
/// state icon's quarter units. Turning it is a clockwise step of an eighth
/// of a turn, 90 degree steps exact and 45 degree steps at 181/256.
constexpr std::array<IconStroke, 12> kRefreshStrokes{{
    {{68, 65}, {57, 72}},
    {{57, 72}, {43, 74}},
    {{43, 74}, {31, 68}},
    {{31, 68}, {24, 57}},
    {{24, 57}, {22, 43}},
    {{22, 43}, {28, 31}},
    {{28, 31}, {39, 24}},
    {{39, 24}, {53, 22}},
    {{53, 22}, {65, 28}},
    {{65, 28}, {72, 39}},
    {{83, 45}, {76, 51}},
    {{76, 51}, {67, 51}},
}};

/// Turns a design point clockwise about the icon's centre.
///
/// @param point the point, in quarter units
/// @param eighths how far, in eighths of a turn; only the low three bits count
/// @return the turned point
IconPoint turned_icon_point(IconPoint point, uint8_t eighths) noexcept {
    int64_t across = point.x - kRingCentre.x;
    int64_t down = point.y - kRingCentre.y;
    const uint8_t step = static_cast<uint8_t>(eighths & 7u);
    if ((step & 1u) != 0u) {
        constexpr int64_t kCos45 = 181;
        constexpr int64_t kScale = 256;
        constexpr int64_t kHalf = kScale / 2;
        const int64_t turned_across = (across * kCos45 - down * kCos45 + kHalf) / kScale;
        const int64_t turned_down = (across * kCos45 + down * kCos45 + kHalf) / kScale;
        across = turned_across;
        down = turned_down;
    }
    const uint8_t quarters = static_cast<uint8_t>(step / 2u);
    for (uint8_t quarter = 0; quarter < quarters; ++quarter) {
        const int64_t turned_across = -down;
        down = across;
        across = turned_across;
    }
    return {across + kRingCentre.x, down + kRingCentre.y};
}

/// Returns how much of each pixel a set of strokes covers.
///
/// The same sampling as a state icon. The strokes are already in quarter
/// units of the 24-unit design. Every machine draws the same pixels.
///
/// @param design the strokes, in quarter units
/// @param columns the rectangle's pixels across, at most kMostIconPixels
/// @param rows the rectangle's pixels down, at most kMostIconPixels
/// @return columns times rows bytes, rows top to bottom: 0 uncovered to 255 covered
std::vector<uint8_t>
stroke_coverage(std::span<const IconStroke> design, int32_t columns, int32_t rows) {
    std::vector<uint8_t> coverage(
        static_cast<std::size_t>(std::max(columns, 0)) * static_cast<std::size_t>(std::max(rows, 0))
    );
    const int32_t side = std::min(columns, rows);
    if (side <= 0)
        return coverage;
    const int32_t origin_x = (columns - side) / 2;
    const int32_t origin_y = (rows - side) / 2;
    const int64_t square = 2 * kIconSamples * side;
    const auto scaled = [square](int64_t quarters) {
        return (quarters * square + kIconGrid / 2) / kIconGrid;
    };
    const int64_t half_width = scaled(kStrokeHalf);
    std::vector<IconStroke> strokes;
    strokes.reserve(design.size());
    for (const IconStroke& stroke : design)
        strokes.push_back(
            {{scaled(stroke.from.x), scaled(stroke.from.y)},
             {scaled(stroke.to.x), scaled(stroke.to.y)}}
        );
    constexpr int64_t samples = kIconSamples * kIconSamples;
    for (int32_t row = 0; row < side; ++row) {
        for (int32_t column = 0; column < side; ++column) {
            int64_t inside = 0;
            for (int64_t down = 0; down < kIconSamples; ++down) {
                for (int64_t across = 0; across < kIconSamples; ++across) {
                    const IconPoint sample{
                        2 * kIconSamples * column + 2 * across + 1,
                        2 * kIconSamples * row + 2 * down + 1,
                    };
                    for (const IconStroke& stroke : strokes) {
                        if (near_stroke(sample, stroke, half_width)) {
                            ++inside;
                            break;
                        }
                    }
                }
            }
            const std::size_t at =
                static_cast<std::size_t>(origin_y + row) * static_cast<std::size_t>(columns) +
                static_cast<std::size_t>(origin_x + column);
            coverage[at] = static_cast<uint8_t>((inside * 255 + samples / 2) / samples);
        }
    }
    return coverage;
}

/// Returns how much of each pixel a refresh arrow covers.
///
/// `eighths` turns the design clockwise before it is scaled.
///
/// @param columns the rectangle's pixels across, at most kMostIconPixels
/// @param rows the rectangle's pixels down, at most kMostIconPixels
/// @param eighths how far the arrow has turned, in eighths of a turn
/// @return columns times rows bytes, rows top to bottom: 0 uncovered to 255 covered
std::vector<uint8_t> refresh_icon_coverage(int32_t columns, int32_t rows, uint8_t eighths) {
    std::array<IconStroke, kRefreshStrokes.size()> turned{};
    for (std::size_t index = 0; index < kRefreshStrokes.size(); ++index) {
        const IconStroke& stroke = kRefreshStrokes[index];
        turned[index] = {
            turned_icon_point(stroke.from, eighths), turned_icon_point(stroke.to, eighths)
        };
    }
    return stroke_coverage(turned, columns, rows);
}

/// Returns how much of each pixel the refresh mark's check covers.
///
/// The same check as a state icon, without its ring, sampled the same way.
///
/// @param columns the rectangle's pixels across, at most kMostIconPixels
/// @param rows the rectangle's pixels down, at most kMostIconPixels
/// @return columns times rows bytes, rows top to bottom: 0 uncovered to 255 covered
std::vector<uint8_t> refresh_check_coverage(int32_t columns, int32_t rows) {
    return stroke_coverage(kCheckStrokes, columns, rows);
}

/// Returns how much of each pixel a close mark's × covers.
///
/// @param columns the rectangle's pixels across, at most kMostIconPixels
/// @param rows the rectangle's pixels down, at most kMostIconPixels
/// @return columns times rows bytes, rows top to bottom: 0 uncovered to 255 covered
std::vector<uint8_t> close_icon_coverage(int32_t columns, int32_t rows) {
    return stroke_coverage(kCloseStrokes, columns, rows);
}

/// Blends a coverage map at a share of its ink. A share of 0 draws nothing
/// and a share of 255 draws the map as it is.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param coverage the map; scaled in a copy
/// @param share how much of the ink to use, 0 to 255
/// @param ink the colour
void blend_share(
    const Canvas& canvas,
    const Rect& rect,
    std::span<const uint8_t> coverage,
    uint8_t share,
    Colour ink
) {
    if (share == 0)
        return;
    if (share == 255) {
        blend_coverage(canvas, rect, coverage, ink);
        return;
    }
    std::vector<uint8_t> scaled(coverage.begin(), coverage.end());
    for (uint8_t& covered : scaled) {
        const auto mixed = static_cast<uint16_t>(static_cast<uint16_t>(covered) * share);
        covered = static_cast<uint8_t>((mixed + 127u) / 255u);
    }
    blend_coverage(canvas, rect, scaled, ink);
}

/// The OA badge's plate, lighter than the list's face so the plate reads on it.
constexpr Colour kOaPlate{0x3c, 0x40, 0x38};
/// The bone's outline, dark enough to separate the off-white from the list.
constexpr Colour kBoneOutline{0x0c, 0x0a, 0x08};
/// The bone's fill.
constexpr Colour kBoneFill{0xf4, 0xef, 0xe0};

/// A design scaled into the sample space of one icon square.
struct DesignScale {
    int64_t square{}; ///< the square's samples across, in eighths of a pixel

    /// Scales a design coordinate into that space.
    ///
    /// @param quarters the coordinate, in quarter units
    /// @return the same place, in eighths of a pixel of the square
    [[nodiscard]] int64_t scaled(int64_t quarters) const noexcept {
        return (quarters * square + kIconGrid / 2) / kIconGrid;
    }
};

/// Tells whether a sample lies in a disc.
///
/// @param point the sample
/// @param centre the disc's centre
/// @param radius the radius
/// @return true when the sample is inside or on the disc
bool in_disc(IconPoint point, IconPoint centre, int64_t radius) noexcept {
    const int64_t across = point.x - centre.x;
    const int64_t down = point.y - centre.y;
    return across * across + down * down <= radius * radius;
}

/// Tells whether a sample lies in an axis-aligned rounded rectangle.
///
/// @param point the sample
/// @param left the left edge
/// @param top the top edge
/// @param right the right edge
/// @param bottom the bottom edge
/// @param radius the corner radius
/// @return true when the sample is inside
bool in_round_rect(
    IconPoint point, int64_t left, int64_t top, int64_t right, int64_t bottom, int64_t radius
) noexcept {
    if (point.x < left || point.x > right || point.y < top || point.y > bottom)
        return false;
    if (radius <= 0)
        return true;
    const int64_t inner_left = left + radius;
    const int64_t inner_right = right - radius;
    const int64_t inner_top = top + radius;
    const int64_t inner_bottom = bottom - radius;
    if (inner_left > inner_right || inner_top > inner_bottom)
        return true;
    if ((point.x >= inner_left && point.x <= inner_right) ||
        (point.y >= inner_top && point.y <= inner_bottom))
        return true;
    const int64_t corner_x = point.x < inner_left ? inner_left : inner_right;
    const int64_t corner_y = point.y < inner_top ? inner_top : inner_bottom;
    return in_disc(point, {corner_x, corner_y}, radius);
}

/// The cross product of AB and AP, for a point-in-triangle test.
///
/// @param from one end
/// @param to the other end
/// @param point the point
/// @return the cross product
int64_t cross_at(IconPoint from, IconPoint to, IconPoint point) noexcept {
    return (to.x - from.x) * (point.y - from.y) - (to.y - from.y) * (point.x - from.x);
}

/// Tells whether two cross products lie on the same side of an edge.
///
/// A zero lies on the edge and counts as inside.
///
/// @param left one cross product
/// @param right the other
/// @return true when they do not point opposite ways
bool same_side(int64_t left, int64_t right) noexcept {
    if (left == 0 || right == 0)
        return true;
    return (left > 0) == (right > 0);
}

/// Tells whether a sample lies in a triangle, edges included.
///
/// @param point the sample
/// @param a the first corner
/// @param b the second corner
/// @param c the third corner
/// @return true when the sample is inside
bool in_triangle(IconPoint point, IconPoint a, IconPoint b, IconPoint c) noexcept {
    const int64_t ab = cross_at(a, b, point);
    const int64_t bc = cross_at(b, c, point);
    const int64_t ca = cross_at(c, a, point);
    return same_side(ab, bc) && same_side(bc, ca);
}

/// Tells whether a sample lies on the OA badge's plate.
///
/// @param point the sample, in the scaled space
/// @param scale how the design was scaled
/// @return true on the plate
bool on_oa_plate(IconPoint point, const DesignScale& scale) noexcept {
    return in_round_rect(
        point,
        scale.scaled(4),
        scale.scaled(4),
        scale.scaled(92),
        scale.scaled(92),
        scale.scaled(14)
    );
}

/// Tells whether a sample lies on the OA badge's letters.
///
/// The O is a rounded ring. The A is a triangle with a counter and a bar.
///
/// @param point the sample, in the scaled space
/// @param scale how the design was scaled
/// @return true on a letter
bool on_oa_letters(IconPoint point, const DesignScale& scale) noexcept {
    const bool ring = in_round_rect(
                          point,
                          scale.scaled(12),
                          scale.scaled(26),
                          scale.scaled(40),
                          scale.scaled(70),
                          scale.scaled(10)
                      ) &&
                      !in_round_rect(
                          point,
                          scale.scaled(20),
                          scale.scaled(36),
                          scale.scaled(32),
                          scale.scaled(60),
                          scale.scaled(4)
                      );
    const bool body = in_triangle(
        point,
        {scale.scaled(70), scale.scaled(24)},
        {scale.scaled(48), scale.scaled(72)},
        {scale.scaled(92), scale.scaled(72)}
    );
    const bool counter = in_triangle(
        point,
        {scale.scaled(70), scale.scaled(42)},
        {scale.scaled(60), scale.scaled(64)},
        {scale.scaled(80), scale.scaled(64)}
    );
    const bool bar = point.x >= scale.scaled(54) && point.x <= scale.scaled(86) &&
                     point.y >= scale.scaled(50) && point.y <= scale.scaled(58);
    return ring || (body && !counter) || bar;
}

/// Tells whether a sample lies on a bone of the given thickness.
///
/// Two knobs at each end and a shaft between them. The outline is the
/// thicker bone; the fill is the thinner one drawn over it.
///
/// @param point the sample, in the scaled space
/// @param scale how the design was scaled
/// @param knob the knob radius, in quarter units
/// @param shaft_half half the shaft's thickness, in quarter units
/// @return true on the bone
bool on_bone(IconPoint point, const DesignScale& scale, int64_t knob, int64_t shaft_half) noexcept {
    const int64_t radius = scale.scaled(knob);
    const IconPoint knobs[] = {
        {scale.scaled(24), scale.scaled(34)},
        {scale.scaled(24), scale.scaled(62)},
        {scale.scaled(72), scale.scaled(34)},
        {scale.scaled(72), scale.scaled(62)},
    };
    for (const IconPoint centre : knobs)
        if (in_disc(point, centre, radius))
            return true;
    const int64_t half = scale.scaled(shaft_half);
    const int64_t centre = scale.scaled(48);
    return point.x >= scale.scaled(24) && point.x <= scale.scaled(72) && point.y >= centre - half &&
           point.y <= centre + half;
}

/// Returns how much of each pixel a filled maker's shape covers.
///
/// The same sampling as a stroke icon. `covers` is asked about each sample
/// in the scaled space.
///
/// @param columns the rectangle's pixels across, at most kMostIconPixels
/// @param rows the rectangle's pixels down, at most kMostIconPixels
/// @param covers whether a sample, and the scale, lie in the shape
/// @return columns times rows bytes, rows top to bottom: 0 uncovered to 255 covered
template <class Covers>
std::vector<uint8_t> sample_maker(int32_t columns, int32_t rows, Covers covers) {
    std::vector<uint8_t> coverage(
        static_cast<std::size_t>(std::max(columns, 0)) * static_cast<std::size_t>(std::max(rows, 0))
    );
    const int32_t side = std::min(columns, rows);
    if (side <= 0)
        return coverage;
    const int32_t left = (columns - side) / 2;
    const int32_t top = (rows - side) / 2;
    const DesignScale scale{2 * kIconSamples * side};
    constexpr int64_t samples = kIconSamples * kIconSamples;
    for (int32_t row = 0; row < side; ++row) {
        for (int32_t column = 0; column < side; ++column) {
            int64_t inside = 0;
            for (int64_t down = 0; down < kIconSamples; ++down) {
                for (int64_t across = 0; across < kIconSamples; ++across) {
                    const IconPoint sample{
                        2 * kIconSamples * column + 2 * across + 1,
                        2 * kIconSamples * row + 2 * down + 1,
                    };
                    if (covers(sample, scale))
                        ++inside;
                }
            }
            const std::size_t at =
                static_cast<std::size_t>(top + row) * static_cast<std::size_t>(columns) +
                static_cast<std::size_t>(left + column);
            coverage[at] = static_cast<uint8_t>((inside * 255 + samples / 2) / samples);
        }
    }
    return coverage;
}

/// Draws the OA badge or the bone in their own colours. `ink` is not used.
/// The badge is the app icon when one has been given, from the copy kept
/// for a mark this small, and the drawn plate otherwise.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param icon oa or bone
void draw_maker_icon(const Canvas& canvas, const Rect& rect, StateIcon icon) {
    const OpenAnnihilationMark& mark = open_annihilation_mark();
    if (icon == StateIcon::oa && renderer::picture_drawable(mark.full)) {
        const int32_t side = std::min(rect.width, rect.height);
        if (side <= 0)
            return;
        const Rect square{
            rect.x + (rect.width - side) / 2,
            rect.y + (rect.height - side) / 2,
            side,
            side,
        };
        const Point covered = covered_pixels(canvas, square);
        const bool prepared_fits = renderer::picture_drawable(mark.prepared) && covered.x > 0 &&
                                   covered.y > 0 &&
                                   covered.x <= static_cast<int32_t>(mark.prepared.width) &&
                                   covered.y <= static_cast<int32_t>(mark.prepared.height);
        draw_picture(canvas, square, prepared_fits ? mark.prepared : mark.full);
        return;
    }
    const Point pixels = covered_pixels(canvas, rect);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostIconPixels || pixels.y > kMostIconPixels)
        return;
    if (icon == StateIcon::oa) {
        const std::vector<uint8_t> plate =
            sample_maker(pixels.x, pixels.y, [](IconPoint point, const DesignScale& scale) {
                return on_oa_plate(point, scale);
            });
        const std::vector<uint8_t> letters =
            sample_maker(pixels.x, pixels.y, [](IconPoint point, const DesignScale& scale) {
                return on_oa_letters(point, scale);
            });
        blend_coverage(canvas, rect, plate, kOaPlate);
        blend_coverage(canvas, rect, letters, colour::brand);
        return;
    }
    const std::vector<uint8_t> outline =
        sample_maker(pixels.x, pixels.y, [](IconPoint point, const DesignScale& scale) {
            return on_bone(point, scale, 18, 13);
        });
    const std::vector<uint8_t> fill =
        sample_maker(pixels.x, pixels.y, [](IconPoint point, const DesignScale& scale) {
            return on_bone(point, scale, 10, 5);
        });
    blend_coverage(canvas, rect, outline, kBoneOutline);
    blend_coverage(canvas, rect, fill, kBoneFill);
}

} // namespace

Point mark_size(Mark mark) noexcept {
    const renderer::Mark& bits = bits_of(mark);
    return {bits.width, bits.height};
}

void draw_mark(const Canvas& canvas, Mark mark, Point at, Colour ink) {
    if (!drawable(canvas))
        return;
    draw_mark(canvas, bits_of(mark), at, ink);
}

void draw_state_icon(const Canvas& canvas, const Rect& rect, StateIcon icon, Colour ink) {
    if (!drawable(canvas))
        return;
    if (icon == StateIcon::oa || icon == StateIcon::bone) {
        draw_maker_icon(canvas, rect, icon);
        return;
    }
    const Point pixels = covered_pixels(canvas, rect);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostIconPixels || pixels.y > kMostIconPixels)
        return;
    const std::vector<uint8_t> coverage = state_icon_coverage(icon, pixels.x, pixels.y);
    blend_coverage(canvas, rect, coverage, ink);
}

void draw_tag_icon(const Canvas& canvas, const Rect& rect, TagIcon icon, Colour ink) {
    if (!drawable(canvas))
        return;
    const Point pixels = covered_pixels(canvas, rect);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostIconPixels || pixels.y > kMostIconPixels)
        return;
    blend_coverage(canvas, rect, stroke_coverage(tag_strokes(icon), pixels.x, pixels.y), ink);
}

void draw_focus_ring(const Canvas& canvas, const Rect& control, bool around) {
    if (!drawable(canvas))
        return;
    const int32_t radius = control_radius_for(control.height);
    if (!around) {
        ring_corner_rect(canvas, control, radius, compact_metrics.focus_ring, colour::focus);
        return;
    }
    const int32_t out = compact_metrics.focus_inset + compact_metrics.focus_ring;
    ring_corner_rect(
        canvas, grown(control, out), radius + out, compact_metrics.focus_ring, colour::focus
    );
}

/// Returns the border of a button that draws a mark: the control border,
/// lighter under the pointer.
///
/// @param look the button
/// @return the border's colour
Colour mark_border(const ButtonLook& look) noexcept {
    return look.enabled && look.hovered ? colour::control_hover : colour::control_border;
}

/// Draws a refresh arrow in a button: the quiet look's face while the pointer
/// is over an enabled button, the plain look's disabled face while it cannot
/// be pressed, and the arrow in the text colour while it turns. A check eases
/// in over the arrow, in the accent, for as long as the look says it has shown.
///
/// @param canvas where it is drawn
/// @param area the button
/// @param look the mark, how far it has turned, and its state
void draw_refresh_button(const Canvas& canvas, const Rect& area, const ButtonLook& look) {
    ButtonLook face = look;
    face.style = !look.enabled && !look.turning ? ButtonStyle::plain : ButtonStyle::quiet;
    const ButtonColours colours = button_colours(face);
    draw_rounded_ground(
        canvas, area, control_radius_for(area.height), colours.face, mark_border(look)
    );
    const Colour ink = !look.enabled && !look.turning ? colour::switch_idle : colour::text;
    constexpr int32_t inset = 2;
    const Rect icon{
        area.x + inset,
        area.y + inset,
        std::max(area.width - 2 * inset, int32_t{0}),
        std::max(area.height - 2 * inset, int32_t{0}),
    };
    if (icon.width <= 0 || icon.height <= 0)
        return;
    const Point pixels = covered_pixels(canvas, icon);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostIconPixels || pixels.y > kMostIconPixels)
        return;
    const uint8_t ease = look.turning ? uint8_t{0} : refresh_check_ease(look.check_ms);
    if (ease < 255)
        blend_share(
            canvas,
            icon,
            refresh_icon_coverage(pixels.x, pixels.y, look.turn),
            static_cast<uint8_t>(255u - ease),
            ink
        );
    if (ease > 0)
        blend_share(canvas, icon, refresh_check_coverage(pixels.x, pixels.y), ease, colour::accent);
}

/// Draws an × in a button, on the same face as a refresh mark: the quiet
/// look while the pointer is over an enabled button, and the plain look's
/// disabled face while it cannot be pressed. The × is the text colour.
///
/// @param canvas where it is drawn
/// @param area the button
/// @param look the mark and its state
void draw_close_button(const Canvas& canvas, const Rect& area, const ButtonLook& look) {
    ButtonLook face = look;
    face.style = look.enabled ? ButtonStyle::quiet : ButtonStyle::plain;
    const ButtonColours colours = button_colours(face);
    draw_rounded_ground(
        canvas, area, control_radius_for(area.height), colours.face, mark_border(look)
    );
    const Colour ink = look.enabled ? colour::text : colour::switch_idle;
    constexpr int32_t inset = 2;
    const Rect icon{
        area.x + inset,
        area.y + inset,
        std::max(area.width - 2 * inset, int32_t{0}),
        std::max(area.height - 2 * inset, int32_t{0}),
    };
    if (icon.width <= 0 || icon.height <= 0)
        return;
    const Point pixels = covered_pixels(canvas, icon);
    if (pixels.x <= 0 || pixels.y <= 0 || pixels.x > kMostIconPixels || pixels.y > kMostIconPixels)
        return;
    blend_coverage(canvas, icon, close_icon_coverage(pixels.x, pixels.y), ink);
}

void draw_button(const Canvas& canvas, const Rect& area, const ButtonLook& look) {
    if (!drawable(canvas))
        return;
    if (look.mark == ButtonMark::refresh) {
        draw_refresh_button(canvas, area, look);
        return;
    }
    if (look.mark == ButtonMark::close) {
        draw_close_button(canvas, area, look);
        return;
    }
    const ButtonColours colours = button_colours(look);
    const int32_t radius =
        look.style == ButtonStyle::filled ? filled_radius(area) : control_radius_for(area.height);
    // A quiet button at rest has no face and no border: its caption is the button.
    const bool bare = look.style == ButtonStyle::quiet && look.enabled && !colours.face;
    if (!bare)
        draw_rounded_ground(canvas, area, radius, colours.face, colours.border);
    if (look.oa_mark) {
        const ButtonMarkParts parts = button_mark_parts(area);
        if (parts.mark.width > 0)
            draw_state_icon(canvas, parts.mark, StateIcon::oa, colours.caption);
        if (parts.caption.width > 0)
            paint_text(
                canvas, look.font, look.caption, parts.caption, Align::left, colours.caption
            );
        return;
    }
    paint_text(canvas, look.font, look.caption, area, Align::centre, colours.caption);
}

int32_t filled_radius(const Rect& area) noexcept {
    return std::max(std::min(area.width, area.height) / 4, int32_t{2});
}

ButtonMarkParts button_mark_parts(const Rect& area) noexcept {
    const int32_t pad = std::max(area.height / 5, int32_t{2});
    const int32_t side = area.height - 2 * pad;
    if (side <= 0 || area.width < side + 3 * pad)
        return {};
    ButtonMarkParts parts;
    parts.mark = {area.x + pad, area.y + pad, side, side};
    const int32_t caption_x = parts.mark.x + side + pad;
    parts.caption = {caption_x, area.y, area.x + area.width - pad - caption_x, area.height};
    if (parts.caption.width <= 0)
        parts.caption = {};
    return parts;
}

Rect switch_track(const Rect& area) noexcept {
    const int32_t width = std::min(area.width, compact_metrics.switch_track_width);
    const int32_t height = std::min(area.height, compact_metrics.switch_height);
    return {area.x + area.width - width, area.y + (area.height - height) / 2, width, height};
}

void draw_switch(const Canvas& canvas, const Rect& area, const SwitchLook& look) {
    if (!drawable(canvas))
        return;
    const Rect track = switch_track(area);
    if (track.width <= 0 || track.height <= 0)
        return;
    // The track is a pill: lime while On, the selected surface while Off,
    // the hovered border's grey while On is locked.
    Colour ground = colour::off_selected;
    if (look.on)
        ground = look.locked ? colour::control_hover : colour::accent;
    if (look.on && look.hovered && !look.locked)
        ground = colour::accent_light;
    const int32_t radius = track.height / 2;
    if (look.on) {
        fill_round_rect(canvas, track, radius, ground);
    } else {
        fill_round_rect(
            canvas, track, radius, look.hovered ? colour::control_hover : colour::control_border
        );
        if (track.width > 2 && track.height > 2)
            fill_round_rect(
                canvas,
                {track.x + 1, track.y + 1, track.width - 2, track.height - 2},
                std::max(radius - 1, int32_t{0}),
                ground
            );
    }
    // The round knob, two points in: at the right while On, the left while Off.
    constexpr int32_t kKnobInset = 2;
    const int32_t knob = std::max(track.height - 2 * kKnobInset, int32_t{0});
    if (knob <= 0)
        return;
    const int32_t knob_x =
        look.on ? track.x + track.width - kKnobInset - knob : track.x + kKnobInset;
    fill_round_rect(
        canvas,
        {knob_x, track.y + kKnobInset, knob, knob},
        knob / 2,
        look.on ? colour::on_accent : colour::text_secondary
    );
}

void draw_levels(const Canvas& canvas, const Rect& area, const LevelsLook& look) {
    if (!drawable(canvas))
        return;
    const int32_t radius = control_radius_for(area.height);
    draw_rounded_ground(
        canvas,
        area,
        radius,
        colour::well,
        look.hovered ? colour::control_hover : colour::control_border
    );
    for (std::size_t index = 0; index < look.captions.size(); ++index) {
        const Rect segment{
            area.x + 1 + static_cast<int32_t>(index) * look.level_width,
            area.y + 1,
            look.level_width,
            area.height - 2,
        };
        const bool selected = index == look.chosen;
        if (selected)
            fill_corner_rect(
                canvas,
                segment,
                std::max(radius - 1, int32_t{0}),
                look.locked ? colour::control_hover : colour::accent
            );
        paint_text(
            canvas,
            FontRole::small,
            look.captions[index],
            segment,
            Align::centre,
            selected ? colour::on_accent : colour::button_text
        );
        if (index >= look.offered)
            blend_rect(canvas, segment, colour::panel, locked_fade);
    }
}

std::size_t
level_at(const Rect& area, std::size_t levels, int32_t level_width, int32_t column) noexcept {
    if (levels == 0 || level_width <= 0)
        return 0;
    const int32_t along = std::max(column - area.x - 1, int32_t{0}) / level_width;
    return std::min(static_cast<std::size_t>(along), levels - 1);
}

void draw_slider(const Canvas& canvas, const Rect& area, const SliderLook& look) {
    if (!drawable(canvas))
        return;
    const Rect track{
        area.x, area.y + compact_metrics.track_offset, area.width, compact_metrics.track_height
    };
    const int32_t track_radius = track.height / 2;
    fill_round_rect(canvas, track, track_radius, colour::control_border);
    const int32_t knob = knob_column(area, look.stop, look.stops);
    if (knob > track.x)
        fill_round_rect(
            canvas,
            {track.x, track.y, knob - track.x, track.height},
            track_radius,
            look.locked ? colour::control_hover : colour::accent
        );
    const int32_t travel = area.width - compact_metrics.knob_width;
    if (look.stops > 1 && travel / (look.stops - 1) >= compact_metrics.least_stop_spacing) {
        for (int32_t index = 0; index < look.stops; ++index) {
            fill_rect(
                canvas,
                {knob_column(area, index, look.stops),
                 area.y + compact_metrics.stop_offset,
                 1,
                 compact_metrics.stop_height},
                colour::control_border
            );
        }
    }
    const Rect knob_rect{
        knob - compact_metrics.knob_width / 2,
        area.y,
        compact_metrics.knob_width,
        compact_metrics.knob_height
    };
    Colour face = colour::text;
    if (look.locked)
        face = colour::control_hover;
    else if (look.hovered)
        face = colour::accent_light;
    const int32_t knob_radius = compact_metrics.knob_width / 2;
    fill_corner_rect(canvas, knob_rect, knob_radius, colour::on_accent);
    if (knob_rect.width > 2 && knob_rect.height > 2)
        fill_corner_rect(
            canvas,
            {knob_rect.x + 1, knob_rect.y + 1, knob_rect.width - 2, knob_rect.height - 2},
            std::max(knob_radius - 1, int32_t{0}),
            face
        );
}

int32_t knob_column(const Rect& track, int32_t stop, int32_t stops) noexcept {
    const int32_t first = track.x + compact_metrics.knob_width / 2;
    const int32_t travel = track.width - compact_metrics.knob_width;
    if (stops < 2)
        return first;
    return first +
           (travel * std::clamp(stop, int32_t{0}, stops - 1) + (stops - 1) / 2) / (stops - 1);
}

int32_t stop_at(const Rect& track, int32_t column, int32_t stops) noexcept {
    const int32_t first = track.x + compact_metrics.knob_width / 2;
    const int32_t travel = track.width - compact_metrics.knob_width;
    if (stops < 2 || travel <= 0)
        return 0;
    const int32_t along = std::clamp(column - first, int32_t{0}, travel);
    return (along * (stops - 1) + travel / 2) / travel;
}

void draw_choice(const Canvas& canvas, const Rect& area, const ChoiceLook& look) {
    if (!drawable(canvas))
        return;
    Colour border = colour::control_border;
    if (look.open)
        border = colour::accent;
    else if (look.hovered)
        border = colour::control_hover;
    draw_rounded_ground(canvas, area, control_radius_for(area.height), colour::well, border);
    const Rect text_box{
        area.x + compact_metrics.choice_text_inset,
        area.y,
        area.width - compact_metrics.choice_text_inset - compact_metrics.choice_arrow_room,
        area.height,
    };
    paint_text(
        clipped(canvas, text_box), FontRole::regular, look.text, text_box, Align::left, colour::text
    );
    draw_mark(
        canvas,
        kChoiceArrow,
        {area.x + area.width - compact_metrics.choice_arrow_room +
             (compact_metrics.choice_arrow_room - compact_metrics.choice_arrow_width) / 2,
         area.y + (area.height - compact_metrics.choice_arrow_height) / 2},
        look.open ? colour::accent : colour::text_secondary
    );
}

namespace {

/// Returns the row of one choice, when the menu names its rows.
///
/// @param look the menu
/// @param item the choice, from 0
/// @return the row; null for an ordinary menu, or an item past the rows
const ChoiceMenuRow* menu_row(const ChoiceMenuLook& look, int32_t item) noexcept {
    if (item < 0 || static_cast<std::size_t>(item) >= look.items.size())
        return nullptr;
    return &look.items[static_cast<std::size_t>(item)];
}

/// Draws one menu row's label and, to its right, its aside, with a gap
/// between them. An aside that would meet the label is clipped to the room
/// left of that gap.
///
/// @param canvas where it is drawn
/// @param box the row
/// @param label the row's text
/// @param aside the right-aligned text; empty for none
/// @param ink the text's colour
void draw_menu_text(
    const Canvas& canvas,
    const Rect& box,
    std::string_view label,
    std::string_view aside,
    Colour ink
) {
    const int32_t inner_left = box.x + compact_metrics.choice_item_text_inset;
    const int32_t inner_right = box.x + box.width - kChoiceTextMargin;
    int32_t label_right = inner_right;
    if (!aside.empty()) {
        // Eight points between the label and the aside, and eight kept for
        // the label, so a long aside is clipped before it meets the label.
        constexpr int32_t kGap = 8;
        constexpr int32_t kMinLabel = 8;
        int32_t aside_width = 0;
        if (canvas.fonts != nullptr)
            aside_width = text_width(*canvas.fonts, FontRole::regular, aside);
        int32_t room = inner_right - inner_left - kMinLabel - kGap;
        if (room < 1)
            room = 1;
        if (aside_width > room)
            aside_width = room;
        const int32_t aside_left = inner_right - aside_width;
        label_right = aside_left - kGap;
        const Rect aside_box{aside_left, box.y, aside_width, box.height};
        paint_text(
            clipped(canvas, aside_box), FontRole::regular, aside, aside_box, Align::right, ink
        );
    }
    const Rect caption{
        inner_left, box.y, std::max(label_right - inner_left, int32_t{0}), box.height
    };
    paint_text(clipped(canvas, caption), FontRole::regular, label, caption, Align::left, ink);
}

} // namespace

void draw_choice_menu(const Canvas& canvas, const Rect& menu, const ChoiceMenuLook& look) {
    if (!drawable(canvas))
        return;
    draw_rounded_ground(
        canvas, menu, compact_metrics.control_radius, colour::list, colour::control_hover
    );
    const int32_t count = static_cast<int32_t>(look.shown.size());
    for (int32_t place = 0; place < count; ++place) {
        const int32_t item = look.first + place;
        if (look.total >= 0 && item >= look.total)
            break;
        const Rect box = choice_item(menu, place);
        const ChoiceMenuRow* row = menu_row(look, item);
        // A divider is a section heading: no selection, no marker, no focus.
        if (row != nullptr && row->divider) {
            const Rect heading{
                box.x + compact_metrics.choice_item_text_inset,
                box.y,
                std::max(box.width - compact_metrics.choice_item_text_inset - kChoiceTextMargin, 0),
                box.height,
            };
            draw_heading(canvas, heading, look.shown[static_cast<std::size_t>(place)]);
            continue;
        }
        const bool choosable = row == nullptr || row->choosable;
        Colour text = choosable ? colour::list_text : colour::quiet;
        if (choosable && item == look.chosen) {
            fill_rect(canvas, box, colour::list_selected);
            fill_rect(
                canvas,
                {box.x + kChosenMarkerOffset,
                 box.y + (box.height - kChosenMarkerHeight) / 2,
                 kChosenMarkerWidth,
                 kChosenMarkerHeight},
                colour::accent
            );
            text = colour::text;
        }
        if (item == look.marked) {
            if (item != look.chosen || !choosable)
                fill_rect(canvas, box, colour::hover);
            if (choosable)
                text = colour::text;
            if (look.focus_shown)
                ring_corner_rect(
                    canvas,
                    box,
                    control_radius_for(box.height),
                    compact_metrics.focus_ring,
                    colour::focus
                );
        }
        const std::string_view aside =
            row != nullptr ? std::string_view{row->aside} : std::string_view{};
        if (aside.empty()) {
            const Rect caption{
                box.x + compact_metrics.choice_item_text_inset,
                box.y,
                box.width - compact_metrics.choice_item_text_inset - kChoiceTextMargin,
                box.height,
            };
            paint_text(
                clipped(canvas, caption),
                FontRole::regular,
                look.shown[static_cast<std::size_t>(place)],
                caption,
                Align::left,
                text
            );
        } else {
            draw_menu_text(canvas, box, look.shown[static_cast<std::size_t>(place)], aside, text);
        }
    }
    const int32_t window =
        shown_choices(static_cast<std::size_t>(std::max(look.total, int32_t{0})));
    if (look.total > window) {
        const int32_t travel = menu.height - 2;
        const int32_t height = std::max(travel * window / look.total, 4);
        const int32_t top =
            menu.y + 1 + (travel - height) * look.first / std::max(look.total - window, 1);
        fill_rect(canvas, {menu.x + menu.width - 3, top, 2, height}, colour::control_hover);
    }
}

int32_t shown_choices(std::size_t choices) noexcept {
    const auto most = static_cast<std::size_t>(compact_metrics.most_shown_choices);
    return static_cast<int32_t>(std::min(choices, most));
}

Rect choice_menu(const Rect& field, std::size_t choices, int32_t footer_line) noexcept {
    const int32_t height = shown_choices(choices) * compact_metrics.choice_item_height + 2;
    int32_t top = field.y + field.height;
    const int32_t body_top = compact_metrics.edge + compact_metrics.header_height + 1;
    if (top + height > footer_line)
        top = std::max(field.y - height, body_top);
    return {field.x, top, field.width, height};
}

Rect choice_item(const Rect& menu, int32_t place) noexcept {
    return {
        menu.x + 1,
        menu.y + 1 + place * compact_metrics.choice_item_height,
        menu.width - 2,
        compact_metrics.choice_item_height
    };
}

int32_t choice_item_at(const Rect& menu, int32_t x, int32_t y) noexcept {
    if (x < menu.x || y < menu.y || x >= menu.x + menu.width || y >= menu.y + menu.height)
        return -1;
    const int32_t local = y - (menu.y + 1);
    if (local < 0)
        return -1;
    const int32_t place = local / compact_metrics.choice_item_height;
    const Rect box = choice_item(menu, place);
    if (x < box.x || y < box.y || x >= box.x + box.width || y >= box.y + box.height)
        return -1;
    return place;
}

int32_t choice_mark_toward(const ChoiceMenuLook& look, int32_t marked, int32_t step) noexcept {
    const int32_t last = std::max(look.total, int32_t{0}) - 1;
    if (last < 0)
        return 0;
    const auto divider = [&look](int32_t index) noexcept {
        return index >= 0 && static_cast<std::size_t>(index) < look.items.size() &&
               look.items[static_cast<std::size_t>(index)].divider;
    };
    if (step == 0)
        return std::clamp(marked, int32_t{0}, last);
    const int32_t direction = step > 0 ? 1 : -1;
    int32_t target = std::clamp(marked + step, int32_t{0}, last);
    if (!divider(target))
        return target;
    const int32_t skipped = target + direction;
    if (skipped < 0 || skipped > last || divider(skipped))
        return std::clamp(marked, int32_t{0}, last);
    return skipped;
}

void draw_lock(const Canvas& canvas, const Rect& area, const LockLook& look) {
    if (!drawable(canvas))
        return;
    const int32_t width = measure_text(canvas, FontRole::small, look.text);
    const int32_t left =
        area.x + area.width - width - compact_metrics.padlock_gap - compact_metrics.padlock_width;
    draw_mark(
        canvas,
        kPadlock,
        {left, area.y + (area.height - compact_metrics.padlock_height) / 2},
        colour::lock
    );
    const Rect text_area{
        left + compact_metrics.padlock_width + compact_metrics.padlock_gap,
        area.y,
        width,
        area.height
    };
    paint_text(canvas, FontRole::small, look.text, text_area, Align::left, colour::lock);
}

void draw_scroll_bar(const Canvas& canvas, const Rect& well, const Rect& thumb, bool hot) {
    if (!drawable(canvas))
        return;
    // Rounded at both ends whichever way the bar runs.
    fill_corner_rect(canvas, well, std::min(well.width, well.height) / 2, colour::well);
    fill_corner_rect(
        canvas,
        thumb,
        std::min(thumb.width, thumb.height) / 2,
        hot ? colour::text_tertiary : colour::control_hover
    );
}

namespace {

/// Draws the OA button's face at the canvas's origin: the bevelled square,
/// and the icon inset or the OA mark.
///
/// @param canvas where it is drawn; its icon is the button's
/// @param side the square's side, in points
/// @param state how it looks
void draw_oa_button_face(const Canvas& canvas, int32_t side, OaButtonState state) {
    const Rect whole{0, 0, side, side};
    Colour face = colour::panel;
    if (state == OaButtonState::hovered)
        face = colour::hover;
    else if (state == OaButtonState::pressed)
        face = colour::band;
    fill_rect(canvas, whole, face);
    if (state == OaButtonState::pressed)
        bevel_rect(canvas, whole, colour::edge_dark, colour::edge_light);
    else
        bevel_rect(canvas, whole, colour::edge_light, colour::edge_dark);
    if (renderer::picture_drawable(canvas.icon)) {
        if (state == OaButtonState::hovered)
            outline_rect(canvas, grown(whole, -1), colour::accent);
        const int32_t pressed_in =
            state == OaButtonState::pressed ? compact_metrics.button_icon_press : 0;
        const int32_t icon_side = side - 2 * compact_metrics.button_icon_inset;
        draw_picture(
            canvas,
            {compact_metrics.button_icon_inset + pressed_in,
             compact_metrics.button_icon_inset + pressed_in,
             icon_side,
             icon_side},
            canvas.icon
        );
        return;
    }
    draw_mark_square(
        canvas, side, state == OaButtonState::idle ? colour::brand : colour::brand_light
    );
}

/// Draws the attention dot over the OA button's top right corner
/// (oa_button_dot): the disc in the outline token, then its face one ring
/// in, in the warning token.
///
/// @param canvas where the button is drawn
/// @param side the button's side, in points
void draw_attention_dot(const Canvas& canvas, int32_t side) {
    const Rect dot = oa_button_dot(side);
    draw_mark(canvas, kDot, {dot.x, dot.y}, colour::control_border);
    draw_mark(canvas, kDotFace, {dot.x + kDotRing, dot.y + kDotRing}, colour::lock);
}

/// One spinner place's centre, in 256ths of the badge from its top left.
struct SpinnerCentre {
    int32_t x{};
    int32_t y{};
};

/// The spinner's orbit and its dots, in 256ths of the badge. The diagonal
/// places sit at 45 degrees (181/256, the nearest eighth of the cosine).
constexpr int32_t kSpinnerOrbit = 56;
constexpr int32_t kSpinnerDot = 40;
constexpr int32_t kSpinnerDiag = (kSpinnerOrbit * 181 + 128) / 256;
constexpr std::array<SpinnerCentre, 8> kSpinnerCentres{{
    {128, 128 - kSpinnerOrbit},
    {128 + kSpinnerDiag, 128 - kSpinnerDiag},
    {128 + kSpinnerOrbit, 128},
    {128 + kSpinnerDiag, 128 + kSpinnerDiag},
    {128, 128 + kSpinnerOrbit},
    {128 - kSpinnerDiag, 128 + kSpinnerDiag},
    {128 - kSpinnerOrbit, 128},
    {128 - kSpinnerDiag, 128 - kSpinnerDiag},
}};

/// Draws the badge's spinner: eight dots on a circle, three of them lit,
/// the brightest the place `turn` names and the two behind it dimmer.
/// Each pixel's share is counted from 16 samples in integers and drawn
/// through blend_coverage, so a turn looks the same at every scale and on
/// every machine.
///
/// @param canvas where the button is drawn
/// @param badge the badge's rectangle
/// @param turn which place is brightest, wrapped over the eight
void draw_oa_spinner(const Canvas& canvas, const Rect& badge, uint32_t turn) {
    const Point pixels = covered_pixels(canvas, badge);
    if (pixels.x <= 0 || pixels.y <= 0)
        return;
    const int32_t places = oa_button_spinner_places;
    const int32_t head = static_cast<int32_t>(turn % static_cast<uint32_t>(places));
    std::array<uint8_t, 8> lit{};
    lit[static_cast<std::size_t>(head)] = 255;
    lit[static_cast<std::size_t>((head + places - 1) % places)] = 170;
    lit[static_cast<std::size_t>((head + places - 2) % places)] = 85;

    constexpr int32_t kSamples = 4;
    constexpr int32_t kRadiusSquared = kSpinnerDot * kSpinnerDot;
    const std::size_t pixel_count =
        static_cast<std::size_t>(pixels.x) * static_cast<std::size_t>(pixels.y);
    std::vector<uint8_t> coverage(pixel_count, 0);
    const int64_t across = static_cast<int64_t>(pixels.x) * kSamples * 2;
    const int64_t down = static_cast<int64_t>(pixels.y) * kSamples * 2;
    for (int32_t row = 0; row < pixels.y; ++row) {
        for (int32_t column = 0; column < pixels.x; ++column) {
            int32_t sum = 0;
            for (int32_t sample_y = 0; sample_y < kSamples; ++sample_y) {
                for (int32_t sample_x = 0; sample_x < kSamples; ++sample_x) {
                    const int64_t along_x =
                        (static_cast<int64_t>(column) * kSamples + sample_x) * 2 + 1;
                    const int64_t along_y =
                        (static_cast<int64_t>(row) * kSamples + sample_y) * 2 + 1;
                    const int32_t px = static_cast<int32_t>(along_x * 256 / across);
                    const int32_t py = static_cast<int32_t>(along_y * 256 / down);
                    int32_t best = 0;
                    for (int32_t place = 0; place < places; ++place) {
                        const uint8_t level = lit[static_cast<std::size_t>(place)];
                        if (level == 0)
                            continue;
                        const SpinnerCentre centre =
                            kSpinnerCentres[static_cast<std::size_t>(place)];
                        const int32_t dx = px - centre.x;
                        const int32_t dy = py - centre.y;
                        if (dx * dx + dy * dy <= kRadiusSquared)
                            best = std::max(best, static_cast<int32_t>(level));
                    }
                    sum += best;
                }
            }
            const int32_t samples = kSamples * kSamples;
            coverage
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(pixels.x) +
                 static_cast<std::size_t>(column)] =
                    static_cast<uint8_t>((sum + samples / 2) / samples);
        }
    }
    blend_coverage(canvas, badge, coverage, colour::text);
}

/// Draws the OA button's bottom-right badge: a dark pill in the outline
/// token, then the count or the spinner. A button that cannot hold it
/// draws nothing.
///
/// @param canvas where the button is drawn
/// @param side the button's side, in points
/// @param mark the badge
void draw_oa_button_corner(const Canvas& canvas, int32_t side, const OaButtonMark& mark) {
    if (mark.corner == OaButtonCorner::none)
        return;
    const std::string text =
        mark.corner == OaButtonCorner::count ? oa_button_count_text(mark.count) : std::string{};
    const int32_t columns =
        mark.corner == OaButtonCorner::count ? measure_text(canvas, FontRole::small, text) : 0;
    const Rect badge = oa_button_badge(side, columns);
    if (badge.width <= 0 || badge.height <= 0)
        return;
    fill_rect(canvas, badge, colour::band);
    outline_rect(canvas, badge, colour::control_border);
    if (mark.corner == OaButtonCorner::spinner) {
        draw_oa_spinner(canvas, badge, mark.turn);
        return;
    }
    box_text(canvas, FontRole::small, text, badge, Align::centre, colour::text);
}

} // namespace

void draw_spinner(const Canvas& canvas, const Rect& area, uint32_t turn) {
    draw_oa_spinner(canvas, area, turn);
}

void draw_oa_button(
    const Canvas& canvas, int32_t side, OaButtonState state, const OaButtonMark& mark
) {
    if (!drawable(canvas))
        return;
    draw_oa_button_face(canvas, side, state);
    draw_oa_button_corner(canvas, side, mark);
    if (mark.dot)
        draw_attention_dot(canvas, side);
}

void draw_oa_button(const Canvas& canvas, int32_t side, OaButtonState state, bool dot) {
    OaButtonMark mark;
    mark.dot = dot;
    draw_oa_button(canvas, side, state, mark);
}

namespace {

/// Draws the icon, shifted when pressed, and the rounded ring. The mark and
/// the badge are drawn by the caller, so a crisp picture can keep its own
/// fonts for the count.
///
/// @param painter where the icon and the ring are drawn
/// @param percent canvas pixels a point, in percent
/// @param origin the button's origin on that canvas
/// @param side the square's side, in points
/// @param state how it looks
/// @param focused a ring is drawn at rest
/// @param icon the button's icon; empty draws no icon
void paint_menu_icon_face(
    oa::ui::paint::Painter& painter,
    int32_t percent,
    Point origin,
    int32_t side,
    OaButtonState state,
    bool focused,
    const renderer::RgbaPicture& icon
) {
    if (percent <= 0)
        percent = 100;
    const Rect placed = to_canvas({origin.x, origin.y, side, side}, percent);
    if (placed.width <= 0 || placed.height <= 0)
        return;
    const int32_t nudge =
        state == OaButtonState::pressed
            ? std::max(
                  int32_t{1}, to_canvas({0, 0, compact_metrics.button_icon_press, 0}, percent).width
              )
            : 0;
    if (renderer::picture_drawable(icon)) {
        const int32_t drawn_width = std::max(int32_t{1}, placed.width - nudge);
        const int32_t drawn_height = std::max(int32_t{1}, placed.height - nudge);
        painter.draw_rgba(
            icon.pixels,
            static_cast<int>(icon.width),
            static_cast<int>(icon.height),
            {static_cast<int>(placed.x + nudge),
             static_cast<int>(placed.y + nudge),
             static_cast<int>(drawn_width),
             static_cast<int>(drawn_height)}
        );
    }
    if (state == OaButtonState::idle && !focused)
        return;
    const Colour ink = state == OaButtonState::pressed ? colour::accent_held : colour::accent;
    const int32_t span = std::min(placed.width, placed.height);
    const float radius =
        static_cast<float>(span) * static_cast<float>(menu_icon_corner_256) / 256.0F;
    const int32_t thick = std::max(int32_t{2}, to_canvas({0, 0, 2, 0}, percent).width);
    painter.outline_rounded_rect(
        {static_cast<float>(placed.x),
         static_cast<float>(placed.y),
         static_cast<float>(placed.width),
         static_cast<float>(placed.height)},
        radius,
        static_cast<float>(thick),
        {ink.r, ink.g, ink.b, ink.a}
    );
}

} // namespace

void draw_menu_icon_button(
    const Canvas& canvas, int32_t side, OaButtonState state, bool focused, const OaButtonMark& mark
) {
    if (side <= 0)
        return;
    oa::ui::paint::Painter* painter = canvas.smooth.painter;
    if (painter != nullptr) {
        if (canvas.smooth.clip && canvas.smooth.clip->empty())
            return;
        if (canvas.smooth.clip)
            painter->set_clip(*canvas.smooth.clip);
        else
            painter->reset_clip();
        const int32_t percent = canvas.smooth.scale_percent > 0 ? canvas.smooth.scale_percent : 100;
        if (!renderer::picture_drawable(canvas.icon))
            draw_oa_mark(canvas, side);
        paint_menu_icon_face(
            *painter, percent, canvas.smooth.origin, side, state, focused, canvas.icon
        );
    } else if (canvas.surface != nullptr && canvas.placement.scale >= 1) {
        // The 640 picture has no painter. The icon is filtered into the
        // square and blended over the menu, so a clear corner stays the menu.
        if (!renderer::picture_drawable(canvas.icon))
            draw_oa_mark(canvas, side);
        const int32_t pixel_scale = canvas.placement.scale;
        const int32_t pixels_wide = side * pixel_scale;
        oa::ui::paint::Canvas layer = oa::ui::paint::make_canvas(pixels_wide, pixels_wide);
        oa::ui::paint::Painter layer_painter(layer);
        paint_menu_icon_face(
            layer_painter, pixel_scale * 100, {0, 0}, side, state, focused, canvas.icon
        );
        const renderer::RgbaPicture painted{
            static_cast<uint32_t>(pixels_wide), static_cast<uint32_t>(pixels_wide), layer.rgba
        };
        renderer::Placement placed{canvas.placement.x, canvas.placement.y, 1};
        if (canvas.placement.clip.width > 0 && canvas.placement.clip.height > 0) {
            placed.clip = {
                canvas.placement.clip.x * pixel_scale,
                canvas.placement.clip.y * pixel_scale,
                canvas.placement.clip.width * pixel_scale,
                canvas.placement.clip.height * pixel_scale
            };
        }
        renderer::draw_picture(*canvas.surface, placed, {0, 0, pixels_wide, pixels_wide}, painted);
    } else {
        return;
    }
    draw_oa_button_corner(canvas, side, mark);
    if (mark.dot)
        draw_attention_dot(canvas, side);
}

void draw_oa_mark(const Canvas& canvas, int32_t side) {
    if (!drawable(canvas) || side <= 0)
        return;
    if (renderer::picture_drawable(canvas.icon)) {
        draw_picture(canvas, {0, 0, side, side}, canvas.icon);
        return;
    }
    draw_mark_square(canvas, side, colour::brand);
}

void draw_header_mark(const Canvas& canvas, const Rect& place) {
    if (!drawable(canvas))
        return;
    if (renderer::picture_drawable(canvas.icon)) {
        draw_picture(canvas, place, canvas.icon);
        return;
    }
    const int32_t side = compact_metrics.mark_square;
    const Rect square{
        place.x + (place.width - side) / 2,
        place.y + (place.height - side) / 2,
        side,
        side,
    };
    outline_rect(canvas, square, colour::brand);
    draw_mark(
        canvas,
        kSmallMark,
        {square.x + (square.width - kSmallMark.width) / 2,
         square.y + (square.height - kSmallMark.height + 1) / 2},
        colour::brand
    );
}

void add_button(
    DisplayList& list, const Rect& rect, const ButtonLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::button;
    item.rect = rect;
    item.control = id;
    item.text = look.caption;
    item.state.hovered = look.hovered;
    item.state.pressed = look.held;
    item.state.disabled = !look.enabled;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list,
        rect,
        id,
        std::move(name),
        ControlKind::button,
        false,
        false,
        look.caption,
        look.enabled
    );
}

void add_switch(
    DisplayList& list, const Rect& rect, const SwitchLook& look, ControlId id, std::string name
) {
    const std::string text = look.on ? look.on_caption : look.off_caption;
    Item item;
    item.role = Role::toggle;
    item.rect = rect;
    item.control = id;
    item.text = text;
    item.state.hovered = look.hovered;
    item.state.on = look.on;
    item.state.locked = look.locked;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list, rect, id, std::move(name), ControlKind::toggle, true, look.on, text, !look.locked
    );
}

void add_levels(
    DisplayList& list, const Rect& rect, const LevelsLook& look, ControlId id, std::string name
) {
    const std::string text =
        look.chosen < look.captions.size() ? look.captions[look.chosen] : std::string{};
    Item item;
    item.role = Role::levels;
    item.rect = rect;
    item.control = id;
    item.text = text;
    item.state.hovered = look.hovered;
    item.state.locked = look.locked;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list, rect, id, std::move(name), ControlKind::levels, true, false, text, !look.locked
    );
}

void add_slider(
    DisplayList& list, const Rect& rect, const SliderLook& look, ControlId id, std::string name
) {
    const std::string text = std::to_string(look.stop);
    Item item;
    item.role = Role::slider;
    item.rect = rect;
    item.control = id;
    item.text = text;
    item.state.hovered = look.hovered;
    item.state.locked = look.locked;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(
        list, rect, id, std::move(name), ControlKind::slider, true, false, text, !look.locked
    );
}

void add_choice(
    DisplayList& list, const Rect& rect, const ChoiceLook& look, ControlId id, std::string name
) {
    Item item;
    item.role = Role::choice;
    item.rect = rect;
    item.control = id;
    item.text = look.text;
    item.state.hovered = look.hovered;
    item.look = look;
    list.items.push_back(std::move(item));
    add_control(list, rect, id, std::move(name), ControlKind::choice, true, false, look.text, true);
}

void add_focus_ring(DisplayList& list, const Rect& control, bool around) {
    Item item;
    item.role = Role::focus_ring;
    item.rect = control;
    item.look = FocusRingLook{around};
    list.items.push_back(std::move(item));
}

void add_lock(DisplayList& list, const Rect& area, const LockLook& look) {
    Item item;
    item.role = Role::lock;
    item.rect = area;
    item.text = look.text;
    item.look = look;
    list.items.push_back(std::move(item));
}

void add_mark(DisplayList& list, Point at, Mark mark, Colour ink) {
    Item item;
    item.role = Role::mark;
    item.rect = {at.x, at.y, 0, 0};
    item.colour = ink;
    item.look = MarkLook{mark, ink};
    list.items.push_back(std::move(item));
}

void paint(const Canvas& canvas, const DisplayList& list) {
    text_fit_begin_paint();
    for (const Item& item : list.items)
        paint_item(canvas, item);
}

} // namespace oa::ui::kit
