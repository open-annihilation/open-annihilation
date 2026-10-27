// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch layer: the controls, the radial order menu, the sheets, the
// phone's build drawer and MORE sheet frames, the banner, the status pill,
// the tip and the placement bar, and with a gamepad the slim pad HUD, the
// button badges, the rings' aim and hints, the build ring with its 3.1c
// pictures, the group ring and the hold's progress, painted into a layer of
// their own at the display's pixels, composed over the CPU frame and
// presented in every tier (docs/touch-controls.md). The layer is drawn again
// only when the HUD state's revision, the layout of the controls, the
// window's size or its density changes, and only its painted rows are
// uploaded.
#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/paint/pad_glyphs.hpp"
#include "render_run.hpp"
#include "oa/ui/paint/painter.hpp"
#include "touch_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <functional>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

namespace hud = oa::ui::touch_hud;
namespace pad = oa::ui::pad_controls;
namespace paint = oa::ui::paint;
namespace pad_glyphs = oa::ui::paint::pad_glyphs;
namespace text_font = oa::platform::text_font;
namespace kit = oa::ui::kit;
using hud::Control;

// ---- Style: the approved settings mock-up's colours --------------------------------------

/// Panels and buttons: gunmetal at 85% opacity.
constexpr paint::Rgba panel_colour{
    kit::hud_colour::panel.r,
    kit::hud_colour::panel.g,
    kit::hud_colour::panel.b,
    kit::hud_colour::panel.a
};
/// Sheets and the radial's ring: darker and nearly opaque, over the battlefield.
constexpr paint::Rgba sheet_colour{
    kit::hud_colour::sheet.r,
    kit::hud_colour::sheet.g,
    kit::hud_colour::sheet.b,
    kit::hud_colour::sheet.a
};
/// An empty cell of the drawer or the MORE sheet, which a 3.1c gadget covers.
constexpr paint::Rgba cell_colour{
    kit::hud_colour::cell.r,
    kit::hud_colour::cell.g,
    kit::hud_colour::cell.b,
    kit::hud_colour::cell.a
};
/// A control a finger rests on: a lighter gunmetal.
constexpr paint::Rgba pressed_colour{
    kit::hud_colour::pressed.r,
    kit::hud_colour::pressed.g,
    kit::hud_colour::pressed.b,
    kit::hud_colour::pressed.a
};
/// The hairline round panels and buttons.
constexpr paint::Rgba edge_colour{
    kit::hud_colour::edge.r,
    kit::hud_colour::edge.g,
    kit::hud_colour::edge.b,
    kit::hud_colour::edge.a
};
/// Lit, latched and armed controls.
constexpr paint::Rgba lit_colour{
    kit::hud_colour::lit.r, kit::hud_colour::lit.g, kit::hud_colour::lit.b, kit::hud_colour::lit.a
};
/// A lit control a finger rests on.
constexpr paint::Rgba lit_pressed_colour{
    kit::hud_colour::lit_pressed.r,
    kit::hud_colour::lit_pressed.g,
    kit::hud_colour::lit_pressed.b,
    kit::hud_colour::lit_pressed.a
};
/// Labels and icons on a lit control.
constexpr paint::Rgba ink_colour{
    kit::hud_colour::ink.r, kit::hud_colour::ink.g, kit::hud_colour::ink.b, kit::hud_colour::ink.a
};
/// Labels and icons.
constexpr paint::Rgba label_colour{
    kit::hud_colour::label.r,
    kit::hud_colour::label.g,
    kit::hud_colour::label.b,
    kit::hud_colour::label.a
};
/// The quieter text of the drawer's caption and page dots.
constexpr paint::Rgba quiet_colour{
    kit::hud_colour::quiet.r,
    kit::hud_colour::quiet.g,
    kit::hud_colour::quiet.b,
    kit::hud_colour::quiet.a
};
/// SELF-DESTRUCT · HOLD's red: its edge, and its fill as the hold fills it.
constexpr paint::Rgba danger_colour{
    kit::hud_colour::danger.r,
    kit::hud_colour::danger.g,
    kit::hud_colour::danger.b,
    kit::hud_colour::danger.a
};
/// SELF-DESTRUCT · HOLD's panel before its hold fills it.
constexpr paint::Rgba danger_panel_colour{
    kit::hud_colour::danger_panel.r,
    kit::hud_colour::danger_panel.g,
    kit::hud_colour::danger_panel.b,
    kit::hud_colour::danger_panel.a
};
/// SELF-DESTRUCT · HOLD's label before its hold fills it.
constexpr paint::Rgba danger_label_colour{
    kit::hud_colour::danger_label.r,
    kit::hud_colour::danger_label.g,
    kit::hud_colour::danger_label.b,
    kit::hud_colour::danger_label.a
};
/// The lines between the radial's wedges.
constexpr paint::Rgba wedge_gap_colour{
    kit::hud_colour::wedge_gap.r,
    kit::hud_colour::wedge_gap.g,
    kit::hud_colour::wedge_gap.b,
    kit::hud_colour::wedge_gap.a
};
/// Greyed items show at this opacity.
constexpr float greyed_opacity = kit::hud_colour::greyed_opacity;

// ---- Sizes, in points ---------------------------------------------------------------------

constexpr float corner_points = 6.0F;       ///< corner radius of panels and buttons
constexpr float edge_points = 1.0F;         ///< hairline width
constexpr float lit_edge_points = 2.0F;     ///< the selected group's and the default wedge's edge
constexpr float large_icon_points = 18.0F;  ///< icon over a label on a tall button
constexpr float small_icon_points = 15.0F;  ///< icon over a label on a 44 pt button
constexpr float row_icon_points = 16.0F;    ///< icon beside a label
constexpr float label_points = 10.0F;       ///< a label under an icon on a tall button
constexpr float small_label_points = 9.0F;  ///< a label under an icon on a 44 pt button
constexpr float row_label_points = 11.0F;   ///< a label beside an icon
constexpr float sub_label_points = 7.0F;    ///< LATCHED, a group's unit count
constexpr float stack_gap_points = 3.0F;    ///< between an icon and its label
constexpr float row_gap_points = 6.0F;      ///< between an icon and its label beside it
constexpr float text_pad_points = 4.0F;     ///< kept clear inside a button's sides
constexpr float tall_button_points = 50.0F; ///< buttons this tall get the larger icon
constexpr float title_points = 11.5F;       ///< banner and drawer titles, the status selection
constexpr float hint_points = 9.5F;         ///< banner and status hints
constexpr float tip_points = 10.0F;         ///< tip bubble text
constexpr float pill_pad_points = 14.0F;    ///< text inset of the banner and the status pill
constexpr float dot_points = 5.0F;          ///< page dot diameter
constexpr float dot_gap_points = 4.0F;      ///< between page dots
constexpr float hint_glyph_points = 16.0F;  ///< a button's glyph in the pad's hints and chips
constexpr float hint_gap_points = 4.0F;     ///< between a glyph and its words
constexpr float hint_piece_points = 6.0F;   ///< between a hint's pieces
constexpr float least_text_points = 7.0F;   ///< the smallest a hint's words shrink to
constexpr float badge_points = 14.0F;       ///< a control's badge glyphs
constexpr float badge_inset_points = 2.0F;  ///< a badge from its control's corner
constexpr float ring_hint_points = 26.0F;   ///< a ring's hint pill
constexpr float ring_hint_gap = 8.0F;       ///< between a ring and its hint pill
constexpr float aim_dot_points = 4.0F;      ///< the pad's aim dot
constexpr float hold_ring_points = 22.0F;   ///< the pad's hold progress ring
constexpr float hold_stroke_points = 4.0F;  ///< its stroke
/// The track of the hold progress ring, before it fills.
constexpr float hold_track_opacity = 0.35F;
/// The cap height of the bundled fonts, as a share of the pixel size: used
/// to centre capitals on a row.
constexpr float cap_share = 0.73F;

/// Translates interface text into the game's language.
using Translate = std::function<std::string(std::string_view)>;

/// Returns the translation the controls' texts are drawn in: a text 3.1c's tables translate,
/// else one of the HUD's own words in the interface catalogue.
///
/// @param runtime the runtime, kept by the translation
/// @return the translation
Translate interface_translation(Runtime& runtime) {
    return [&runtime](std::string_view text) {
        std::string translated = runtime.translate_ui(text);
        if (translated == text)
            translated = std::string(oa::data::languages::interface_text(text));
        return translated;
    };
}

/// What the painting needs: the painter, the fonts, the layer's scale and the translation.
struct PaintContext {
    paint::Painter& painter;       ///< paints the layer
    text_font::FontStack* fonts{}; ///< the label fonts; null draws no text
    float scale_x{1.0F};           ///< layer pixels per layout pixel, across
    float scale_y{1.0F};           ///< layer pixels per layout pixel, down
    float point{1.0F};             ///< layer pixels per point
    const Translate& translate;    ///< interface text into the game's language
    const hud::HudState& state;    ///< what the controls show
    const hud::Frame& frame;       ///< where they are
    /// The HUD layer the build ring's 3.1c pictures are copied from; null when none shows.
    const renderer::Surface* hud_layer{};
    /// Where each build ring wedge's picture comes from and goes; null without the ring.
    const std::array<RingPicture, hud::build_ring_slot_count>* pictures{};
    /// The wedge a mouse-opened wheel aims, lit as a pressed wedge. The pad's aim is separate.
    std::optional<std::size_t> mouse_aim{};
};

/// How a control's icon and label are arranged inside it.
enum class Arrangement : uint8_t {
    stacked,   ///< the icon over the label
    row,       ///< the icon left of the label
    icon_only, ///< the icon alone, as large as fits
    text_only, ///< the label alone
};

/// The looks a control can show at once.
struct ControlLook {
    bool pressed{};  ///< a finger rests on it
    bool lit{};      ///< lit: armed, latched, on, the open sheet's button
    bool lit_edge{}; ///< lit by its edge and label only (the selected group's chip)
    bool greyed{};   ///< shown at 40%: it does nothing now
    bool latched{};  ///< marked LATCHED under its label
};

/// Returns a point's size in layer pixels.
///
/// @param context the painting
/// @param points points
/// @return layer pixels
float px(const PaintContext& context, float points) {
    return points * context.point;
}

/// Returns a font size in whole layer pixels.
///
/// @param context the painting
/// @param points the size in points
/// @return pixels per em, at least 6
int font_px(const PaintContext& context, float points) {
    return std::max(6, static_cast<int>(std::lround(px(context, points))));
}

/// Returns a layout rectangle in layer pixels.
///
/// @param context the painting
/// @param rect layout pixels
/// @return the same place in layer pixels
paint::Area area_of(const PaintContext& context, const hud::Rect& rect) {
    return {
        static_cast<float>(rect.x) * context.scale_x,
        static_cast<float>(rect.y) * context.scale_y,
        static_cast<float>(rect.width) * context.scale_x,
        static_cast<float>(rect.height) * context.scale_y
    };
}

/// Returns whether a rectangle holds no pixel.
///
/// @param rect the rectangle
/// @return true when its width or height is 0 or less
bool empty(const hud::Rect& rect) {
    return rect.width <= 0 || rect.height <= 0;
}

/// Returns whether one rectangle lies inside another, a pixel's slack allowed.
///
/// @param inner the rectangle tested
/// @param outer the rectangle it may lie in
/// @return true when it does
bool inside(const hud::Rect& inner, const hud::Rect& outer) {
    return !empty(outer) && inner.x >= outer.x - 1 && inner.y >= outer.y - 1 &&
           inner.x + inner.width <= outer.x + outer.width + 1 &&
           inner.y + inner.height <= outer.y + outer.height + 1;
}

/// Returns a colour greyed when an item does nothing now.
///
/// @param colour the colour
/// @param greyed whether the item is greyed
/// @return the colour, at 40% when greyed
paint::Rgba greyed_if(paint::Rgba colour, bool greyed) {
    return greyed ? paint::with_opacity(colour, greyed_opacity) : colour;
}

/// Returns how wide a line of text is drawn, in layer pixels.
///
/// @param context the painting
/// @param text the text
/// @param size_points its size
/// @return pixels; 0 without fonts
float text_width(const PaintContext& context, std::string_view text, float size_points) {
    if (context.fonts == nullptr || text.empty())
        return 0.0F;
    return static_cast<float>(
        paint::text_width(*context.fonts, text, font_px(context, size_points), true)
    );
}

/// Draws a line of capitals from a left edge with their middle on a row,
/// shortened to a width.
///
/// @param context the painting
/// @param text the text
/// @param left the pen's start, layer pixels
/// @param middle the row the capitals' middle lies on, layer pixels
/// @param size_points the text's size
/// @param colour its colour
/// @param max_width the widest it may be, layer pixels
/// @return the width drawn, layer pixels
float text_from(
    const PaintContext& context,
    std::string_view text,
    float left,
    float middle,
    float size_points,
    paint::Rgba colour,
    float max_width
) {
    if (context.fonts == nullptr || text.empty() || max_width <= 0.0F)
        return 0.0F;
    const int size = font_px(context, size_points);
    const std::string fitted =
        paint::fit_text(*context.fonts, text, size, true, static_cast<int>(max_width));
    if (fitted.empty())
        return 0.0F;
    const auto line = paint::draw_line(*context.fonts, fitted, size, true);
    const int baseline = static_cast<int>(std::lround(middle + cap_share * size * 0.5F));
    paint::paint_line(context.painter, line, static_cast<int>(std::lround(left)), baseline, colour);
    return static_cast<float>(line.coverage.advance);
}

/// Draws a line of capitals centred on a point, shortened to a width.
///
/// @param context the painting
/// @param text the text
/// @param centre_x the column its middle lies on, layer pixels
/// @param middle the row the capitals' middle lies on, layer pixels
/// @param size_points the text's size
/// @param colour its colour
/// @param max_width the widest it may be, layer pixels
void text_centred(
    const PaintContext& context,
    std::string_view text,
    float centre_x,
    float middle,
    float size_points,
    paint::Rgba colour,
    float max_width
) {
    if (context.fonts == nullptr || text.empty() || max_width <= 0.0F)
        return;
    const int size = font_px(context, size_points);
    const std::string fitted =
        paint::fit_text(*context.fonts, text, size, true, static_cast<int>(max_width));
    if (fitted.empty())
        return;
    const float width = static_cast<float>(paint::text_width(*context.fonts, fitted, size, true));
    text_from(context, fitted, centre_x - width * 0.5F, middle, size_points, colour, max_width + 1);
}

/// Returns the icon an order shows on the rail, the radial and the MORE sheet.
///
/// @param order the order
/// @return its icon
paint::Icon order_icon(hud::Order order) {
    switch (order) {
    case hud::Order::move:
        return paint::Icon::arrow;
    case hud::Order::attack:
        return paint::Icon::crosshair;
    case hud::Order::patrol:
        return paint::Icon::patrol;
    case hud::Order::guard:
        return paint::Icon::shield;
    case hud::Order::stop:
        return paint::Icon::square;
    case hud::Order::blast:
        return paint::Icon::star;
    case hud::Order::reclaim:
        return paint::Icon::tray_down;
    case hud::Order::repair:
        return paint::Icon::wrench;
    case hud::Order::capture:
        return paint::Icon::flag;
    case hud::Order::load:
        return paint::Icon::tray_up;
    case hud::Order::unload:
        return paint::Icon::tray_down;
    }
    return paint::Icon::none;
}

/// Returns the icon of a radial item.
///
/// @param item the item
/// @return its icon
paint::Icon radial_icon(hud::RadialItem item) {
    if (item == hud::RadialItem::info)
        return paint::Icon::info;
    if (item == hud::RadialItem::type)
        return paint::Icon::box_select;
    const auto order = hud::radial_order(item);
    return order ? order_icon(*order) : paint::Icon::none;
}

/// Returns the label of a radial item: its order's label, INFO or TYPE; the
/// repair wedge reads ASSIST when a plain tap would assist.
///
/// @param item the item
/// @param state what the controls show
/// @return the label, before translation
std::string_view radial_label(hud::RadialItem item, const hud::HudState& state) {
    if (item == hud::RadialItem::info)
        return "INFO";
    if (item == hud::RadialItem::type)
        return "TYPE";
    if (item == hud::RadialItem::repair && state.tap_action == hud::TapAction::assist)
        return "ASSIST";
    const auto order = hud::radial_order(item);
    return order ? hud::order_label(*order) : std::string_view{};
}

/// Returns the rail slot a control shows, when its index names one in use.
///
/// @param state what the controls show
/// @param index the slot
/// @return the slot; null beyond rail_count
const hud::RailSlot* rail_slot(const hud::HudState& state, uint8_t index) {
    if (index >= state.rail_count || index >= state.rail.size())
        return nullptr;
    return &state.rail[index];
}

/// Returns the icon a control shows.
///
/// @param control the control
/// @param state what the controls show
/// @return its icon; none for a label alone
paint::Icon control_icon(const hud::ControlRect& control, const hud::HudState& state) {
    switch (control.control) {
    case Control::queue:
        return paint::Icon::shift_arrow;
    case Control::add:
    case Control::group_store:
        return paint::Icon::plus;
    case Control::clear:
    case Control::place_cancel:
    case Control::banner_cancel:
        return paint::Icon::cross;
    case Control::select_menu:
        return paint::Icon::box_select;
    case Control::pause:
        return paint::Icon::pause;
    case Control::speed:
        return paint::Icon::gauge;
    case Control::chat:
        return paint::Icon::speech;
    case Control::centre:
        return paint::Icon::crosshair;
    case Control::follow:
        return paint::Icon::eye;
    case Control::next_unit:
        return paint::Icon::chevrons;
    case Control::info:
        return paint::Icon::info;
    case Control::menu:
        return paint::Icon::menu;
    case Control::build_drawer:
        return paint::Icon::grid;
    case Control::zoom_out:
        return paint::Icon::magnifier_minus;
    case Control::zoom_in:
        return paint::Icon::magnifier_plus;
    case Control::order_slot:
        if (const auto* slot = rail_slot(state, control.index))
            return slot->more ? paint::Icon::dots : order_icon(slot->order);
        return paint::Icon::none;
    case Control::more:
        return paint::Icon::dots;
    case Control::drawer_close:
    case Control::drawer_prev:
        return paint::Icon::chevron_left;
    case Control::drawer_next:
        return paint::Icon::chevron_right;
    case Control::more_item:
        return control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct)
                   ? paint::Icon::warning
                   : paint::Icon::info;
    case Control::none:
    case Control::times_five:
    case Control::group_chip:
    case Control::drawer_build_tab:
    case Control::drawer_orders_tab:
    case Control::menu_item:
    case Control::radial_item:
    case Control::radial_hub:
    case Control::sheet_outside:
    case Control::force:
    case Control::build_wedge:
    case Control::group_wedge:
        return paint::Icon::none;
    }
    return paint::Icon::none;
}

/// Returns a control's label, before translation: touch_hud's, else the
/// control's own name for the few the model leaves without one.
///
/// @param control the control
/// @param state what the controls show
/// @return the label
std::string_view control_text(const hud::ControlRect& control, const hud::HudState& state) {
    if (control.control == Control::menu_item)
        return hud::menu_item_label(state.sheet, control.index);
    const auto label = hud::control_label(control.control, control.index, state);
    if (!label.empty())
        return label;
    switch (control.control) {
    case Control::queue:
        return "QUEUE";
    case Control::add:
        return "ADD";
    case Control::times_five:
        return "x5";
    case Control::clear:
        return "CLEAR";
    case Control::select_menu:
        return "SELECT";
    case Control::group_store:
        return "STORE";
    case Control::pause:
        return "PAUSE";
    case Control::speed:
        return "SPEED";
    case Control::chat:
        return "CHAT";
    case Control::centre:
        return "CENTRE";
    case Control::follow:
        return "FOLLOW";
    case Control::next_unit:
        return "NEXT";
    case Control::info:
        return "INFO";
    case Control::menu:
        return "MENU";
    case Control::build_drawer:
        return state.builder_selected ? "BUILD" : "ORDERS";
    case Control::order_slot:
        if (const auto* slot = rail_slot(state, control.index))
            return slot->more ? std::string_view{"MORE"} : hud::order_label(slot->order);
        return {};
    case Control::more:
        return "MORE";
    case Control::drawer_build_tab:
        return "BUILD";
    case Control::drawer_orders_tab:
        return "ORDERS";
    case Control::place_cancel:
        return "CANCEL";
    case Control::more_item:
        return control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct)
                   ? std::string_view{"SELF-DESTRUCT \xC2\xB7 HOLD"}
                   : std::string_view{"INFO"};
    default:
        return {};
    }
}

/// Returns a control's label in the language shown: a word that stands for
/// two things is translated by the text that says which (hud::shown_label).
///
/// @param context the painting
/// @param control the control
/// @return the words to draw
std::string label_shown(const PaintContext& context, const hud::ControlRect& control) {
    return hud::shown_label(
        control_text(control, context.state),
        hud::control_label_lookup(control.control, control.index, context.state),
        context.translate
    );
}

/// Returns whether a finger rests on a control.
///
/// @param state what the controls show
/// @param control the control
/// @param index its index
/// @return true when one of the pressed entries names it
bool is_pressed(const hud::HudState& state, Control control, uint8_t index) {
    return std::any_of(state.pressed.begin(), state.pressed.end(), [&](const hud::Pressed& entry) {
        return entry.control != Control::none && entry.control == control && entry.index == index;
    });
}

/// Returns the looks a control shows now, from the HUD state.
///
/// @param control the control
/// @param state what the controls show
/// @return its looks
ControlLook look_of(const hud::ControlRect& control, const hud::HudState& state) {
    ControlLook look{};
    look.pressed = is_pressed(state, control.control, control.index);
    const auto latch = [&](hud::Latch which) {
        look.lit = state.latches.active(which);
        look.latched = state.latches.latched(which);
    };
    switch (control.control) {
    case Control::queue:
        // The pad HUD's chip is x5 over a build button, and lit by x5.
        latch(
            state.pad.hud && state.pad.over_build_button ? hud::Latch::times_five
                                                         : hud::Latch::queue
        );
        break;
    case Control::force:
        look.lit = state.pad.force_active || state.force_touch;
        break;
    case Control::add:
        latch(hud::Latch::add);
        break;
    case Control::times_five:
        latch(hud::Latch::times_five);
        break;
    case Control::select_menu:
        look.lit = state.sheet == hud::Sheet::select_menu;
        break;
    case Control::group_chip:
        look.lit_edge = control.index != 0 && state.selected_group == control.index;
        break;
    case Control::pause:
        look.lit = state.paused;
        break;
    case Control::speed:
        look.lit = state.sheet == hud::Sheet::speed;
        break;
    case Control::chat:
        look.lit = state.chat_open;
        break;
    case Control::follow:
        look.lit = state.following;
        break;
    case Control::info:
        look.greyed = !state.has_selection;
        break;
    case Control::menu:
        look.lit = state.menu_open || state.sheet == hud::Sheet::phone_menu;
        break;
    case Control::build_drawer:
        look.lit = state.sheet == hud::Sheet::drawer;
        break;
    case Control::order_slot:
        if (const auto* slot = rail_slot(state, control.index)) {
            look.lit = slot->more ? state.sheet == hud::Sheet::more : slot->lit;
            look.greyed = !slot->more && !slot->available;
        }
        break;
    case Control::more:
        look.lit = state.sheet == hud::Sheet::more;
        break;
    case Control::drawer_build_tab:
        look.lit = state.drawer_tab == hud::DrawerTab::build;
        break;
    case Control::drawer_orders_tab:
        look.lit = state.drawer_tab == hud::DrawerTab::orders;
        break;
    case Control::drawer_prev:
        look.greyed = state.drawer_page == 0;
        break;
    case Control::drawer_next:
        look.greyed = state.drawer_page + 1 >= state.drawer_pages;
        break;
    case Control::more_item:
        look.greyed =
            control.index == static_cast<uint8_t>(hud::MoreItem::info) && !state.has_selection;
        break;
    default:
        break;
    }
    return look;
}

/// Returns how a control's icon and label are arranged, from its kind and size.
///
/// @param control the control
/// @param width_points its width in points
/// @param height_points its height in points
/// @param icon its icon
/// @param label its label
/// @return the arrangement
Arrangement arrangement_of(
    Control control,
    float width_points,
    float height_points,
    paint::Icon icon,
    std::string_view label
) {
    if (icon == paint::Icon::none)
        return Arrangement::text_only;
    if (label.empty())
        return Arrangement::icon_only;
    switch (control) {
    case Control::zoom_out:
    case Control::zoom_in:
    case Control::drawer_close:
    case Control::drawer_prev:
    case Control::drawer_next:
    case Control::banner_cancel:
        return Arrangement::icon_only;
    case Control::pause:
    case Control::menu:
        if (width_points < 56.0F && height_points < 56.0F)
            return Arrangement::icon_only;
        return width_points >= height_points * 1.3F ? Arrangement::row : Arrangement::stacked;
    case Control::group_store:
        if (label == "+" || width_points < 56.0F)
            return Arrangement::icon_only;
        break;
    case Control::build_drawer:
    case Control::place_cancel:
    case Control::more_item:
        return Arrangement::row;
    default:
        break;
    }
    return height_points >= 40.0F ? Arrangement::stacked : Arrangement::row;
}

/// Paints a button's face: its fill and edge for its looks.
///
/// @param context the painting
/// @param area the button, layer pixels
/// @param look its looks
void paint_face(const PaintContext& context, paint::Area area, const ControlLook& look) {
    const float radius = px(context, corner_points);
    paint::Rgba fill = panel_colour;
    if (look.lit)
        fill = look.pressed ? lit_pressed_colour : lit_colour;
    else if (look.pressed)
        fill = pressed_colour;
    context.painter.fill_rounded_rect(area, radius, fill);
    if (look.lit_edge)
        context.painter.outline_rounded_rect(
            area, radius, px(context, lit_edge_points), lit_colour
        );
    else if (!look.lit)
        context.painter.outline_rounded_rect(area, radius, px(context, edge_points), edge_colour);
}

/// Returns the colour a control's icon and label are drawn in.
///
/// @param look its looks
/// @return the colour
paint::Rgba content_colour(const ControlLook& look) {
    if (look.lit)
        return greyed_if(ink_colour, look.greyed);
    if (look.lit_edge)
        return lit_colour;
    return greyed_if(label_colour, look.greyed);
}

/// Paints an icon and a label inside an area, arranged as asked.
///
/// @param context the painting
/// @param area the area, layer pixels
/// @param arrangement how they are arranged
/// @param icon the icon
/// @param label the label, translated
/// @param sub_label a smaller line under the label (LATCHED); empty for none
/// @param colour their colour
/// @param text_points the label's size; 0 picks the arrangement's
void paint_content(
    const PaintContext& context,
    paint::Area area,
    Arrangement arrangement,
    paint::Icon icon,
    std::string_view label,
    std::string_view sub_label,
    paint::Rgba colour,
    float text_points = 0.0F
) {
    const float height_points = area.height / context.point;
    const float pad = px(context, text_pad_points);
    const float centre_x = area.x + area.width * 0.5F;
    const float centre_y = area.y + area.height * 0.5F;
    const float room = area.width - 2.0F * pad;
    // A sub-label that does not fit whole is left out: the look still says it.
    if (!sub_label.empty() && text_width(context, sub_label, sub_label_points) > room)
        sub_label = {};
    switch (arrangement) {
    case Arrangement::icon_only: {
        const float side = std::min(px(context, 22.0F), std::min(area.width, area.height) * 0.55F);
        context.painter.draw_icon(
            icon, paint::Area{centre_x - side * 0.5F, centre_y - side * 0.5F, side, side}, colour
        );
        return;
    }
    case Arrangement::text_only: {
        const float size = text_points > 0.0F ? text_points : row_label_points;
        if (sub_label.empty()) {
            text_centred(context, label, centre_x, centre_y, size, colour, room);
            return;
        }
        const float cap = cap_share * static_cast<float>(font_px(context, size));
        const float sub_cap = cap_share * static_cast<float>(font_px(context, sub_label_points));
        const float gap = px(context, stack_gap_points);
        const float top = centre_y - (cap + gap + sub_cap) * 0.5F;
        text_centred(context, label, centre_x, top + cap * 0.5F, size, colour, room);
        text_centred(
            context,
            sub_label,
            centre_x,
            top + cap + gap + sub_cap * 0.5F,
            sub_label_points,
            colour,
            room
        );
        return;
    }
    case Arrangement::row: {
        const float side = px(context, row_icon_points);
        const float gap = px(context, row_gap_points);
        const float size = text_points > 0.0F ? text_points : row_label_points;
        const float width = std::min(text_width(context, label, size), room - side - gap);
        const float left = centre_x - (side + gap + width) * 0.5F;
        context.painter.draw_icon(
            icon, paint::Area{left, centre_y - side * 0.5F, side, side}, colour
        );
        text_from(context, label, left + side + gap, centre_y, size, colour, width + 1.0F);
        return;
    }
    case Arrangement::stacked: {
        const bool tall = height_points >= tall_button_points;
        const float side = px(context, tall ? large_icon_points : small_icon_points);
        const float size =
            text_points > 0.0F ? text_points : (tall ? label_points : small_label_points);
        const float cap = cap_share * static_cast<float>(font_px(context, size));
        const float sub_cap =
            sub_label.empty() ? 0.0F
                              : cap_share * static_cast<float>(font_px(context, sub_label_points));
        const float gap = px(context, stack_gap_points);
        const float block = side + gap + cap + (sub_label.empty() ? 0.0F : gap + sub_cap);
        const float top = centre_y - block * 0.5F;
        context.painter.draw_icon(
            icon, paint::Area{centre_x - side * 0.5F, top, side, side}, colour
        );
        text_centred(context, label, centre_x, top + side + gap + cap * 0.5F, size, colour, room);
        if (!sub_label.empty())
            text_centred(
                context,
                sub_label,
                centre_x,
                top + side + gap + cap + gap + sub_cap * 0.5F,
                sub_label_points,
                colour,
                room
            );
        return;
    }
    }
}

/// Paints a pill: a panel whose ends are half circles.
///
/// @param context the painting
/// @param area the pill, layer pixels
/// @param edge the colour of its edge
/// @param edge_width its edge's width, layer pixels
void paint_pill(const PaintContext& context, paint::Area area, paint::Rgba edge, float edge_width) {
    const float radius = area.height * 0.5F;
    context.painter.fill_rounded_rect(area, radius, sheet_colour);
    context.painter.outline_rounded_rect(area, radius, edge_width, edge);
}

// ---- The gamepad's prompts --------------------------------------------------------------

/// Returns a hint piece's words: translated, and with no glyphs drawn (prompts Off) the
/// buttons' names before them.
///
/// @param context the painting
/// @param part the piece
/// @return the words to draw
std::string piece_words(const PaintContext& context, const hud::HintPart& part) {
    std::string words = hud::shown_label(part.text, part.lookup, context.translate, part.action);
    if (part.chord && !context.state.pad.badges) {
        const std::string names = hud::chord_words(*part.chord, context.state.pad.glyphs);
        words = words.empty() ? names : names + " " + words;
    }
    return words;
}

/// Returns how wide a pad hint is drawn: each piece's glyphs and words, the pieces apart.
///
/// @param context the painting
/// @param hint the hint
/// @param glyph the glyphs' height, layer pixels
/// @param text_points the words' size
/// @return layer pixels
float hint_width(
    const PaintContext& context, const hud::PadHint& hint, float glyph, float text_points
) {
    float width = 0.0F;
    const std::size_t count = std::min<std::size_t>(hint.count, hud::max_hint_parts);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& part = hint.parts[index];
        if (index > 0)
            width += px(context, hint_piece_points);
        const std::string words = piece_words(context, part);
        if (part.chord && context.state.pad.badges) {
            width += pad_glyphs::chord_width(*part.chord, context.state.pad.glyphs, glyph);
            if (!words.empty())
                width += px(context, hint_gap_points);
        }
        width += text_width(context, words, text_points);
    }
    return width;
}

/// Paints a pad hint left to right from a column with its middle on a row: each piece's
/// glyphs, then its words.
///
/// @param context the painting
/// @param hint the hint
/// @param left the column it starts at, layer pixels
/// @param middle the row its middle lies on, layer pixels
/// @param glyph the glyphs' height, layer pixels
/// @param text_points the words' size
/// @param colour the words' colour
void paint_hint(
    const PaintContext& context,
    const hud::PadHint& hint,
    float left,
    float middle,
    float glyph,
    float text_points,
    paint::Rgba colour
) {
    float pen = left;
    const std::size_t count = std::min<std::size_t>(hint.count, hud::max_hint_parts);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& part = hint.parts[index];
        if (index > 0)
            pen += px(context, hint_piece_points);
        const std::string words = piece_words(context, part);
        if (part.chord && context.state.pad.badges) {
            const float width =
                pad_glyphs::chord_width(*part.chord, context.state.pad.glyphs, glyph);
            pad_glyphs::draw_chord(
                context.painter,
                context.fonts,
                *part.chord,
                context.state.pad.glyphs,
                {pen, middle - glyph * 0.5F, width, glyph},
                ink_colour,
                label_colour
            );
            pen += width;
            if (!words.empty())
                pen += px(context, hint_gap_points);
        }
        const float words_width = text_width(context, words, text_points);
        text_from(context, words, pen, middle, text_points, colour, words_width + 1.0F);
        pen += words_width;
    }
}

/// Paints a pad hint centred in an area, at the largest size from the hint size down that fits
/// its width.
///
/// @param context the painting
/// @param hint the hint
/// @param area the area, layer pixels
/// @param colour the words' colour
void paint_hint_in(
    const PaintContext& context, const hud::PadHint& hint, paint::Area area, paint::Rgba colour
) {
    const float room = area.width - 2.0F * px(context, pill_pad_points);
    for (float size = hint_points; size >= least_text_points; size -= 0.5F) {
        const float glyph =
            std::min(px(context, hint_glyph_points * size / hint_points), area.height * 0.8F);
        const float width = hint_width(context, hint, glyph, size);
        if (width > room && size - 0.5F >= least_text_points)
            continue;
        paint_hint(
            context,
            hint,
            area.x + std::max(px(context, pill_pad_points), (area.width - width) * 0.5F),
            area.y + area.height * 0.5F,
            glyph,
            size,
            colour
        );
        return;
    }
}

/// A control's badge: the pad buttons that give it, and where they go.
struct Badge {
    pad::Chord chord{}; ///< the buttons
    paint::Area box{};  ///< the glyphs' place, layer pixels
    float band{};       ///< the strip at the control's top the badge takes, layer pixels
};

/// Returns a control's badge, in its top right corner, once a pad was used and prompts show;
/// group chips show their button (the groups layer's L5 is held) only while that layer is held.
///
/// @param context the painting
/// @param control the control
/// @return the badge; none for a control with none or too narrow for it
std::optional<Badge> badge_for(const PaintContext& context, const hud::ControlRect& control) {
    const auto& pad_look = context.state.pad;
    if (!pad_look.badges || (control.control == Control::group_chip && !pad_look.groups_layer))
        return std::nullopt;
    auto chord = hud::control_badge(control.control, control.index, pad_look.map);
    if (!chord)
        return std::nullopt;
    if (control.control == Control::group_chip)
        chord->held = pad::PadButton::none;
    const auto area = area_of(context, control.rect);
    const float height = std::min(px(context, badge_points), area.height * 0.3F);
    const float width = pad_glyphs::chord_width(*chord, pad_look.glyphs, height);
    const float inset = px(context, badge_inset_points);
    if (width > area.width - 2.0F * inset || height <= 0.0F)
        return std::nullopt;
    Badge badge{};
    badge.chord = *chord;
    badge.box = {area.x + area.width - inset - width, area.y + inset, width, height};
    badge.band = height + inset;
    return badge;
}

/// Paints a control's badge.
///
/// @param context the painting
/// @param badge the badge
void paint_badge(const PaintContext& context, const Badge& badge) {
    pad_glyphs::draw_chord(
        context.painter,
        context.fonts,
        badge.chord,
        context.state.pad.glyphs,
        badge.box,
        ink_colour,
        label_colour
    );
}

/// Returns a control's area less the strip its badge takes: where its icon and label go.
///
/// @param area the control, layer pixels
/// @param badge its badge, if any
/// @return the area below the badge's strip
paint::Area below_badge(paint::Area area, const std::optional<Badge>& badge) {
    if (!badge)
        return area;
    return {area.x, area.y + badge->band, area.width, std::max(0.0F, area.height - badge->band)};
}

/// Paints one of the pad HUD's QUEUE, ADD and FORCE chips: its label and, with prompts, the
/// buttons that give it beside the label; lit while held or latched.
///
/// @param context the painting
/// @param control the chip
/// @param look its looks
void paint_pad_chip(
    const PaintContext& context, const hud::ControlRect& control, const ControlLook& look
) {
    const auto area = area_of(context, control.rect);
    paint_face(context, area, look);
    const auto& pad_look = context.state.pad;
    const std::string label = label_shown(context, control);
    const auto colour = content_colour(look);
    const auto chord = pad_look.badges
                           ? hud::control_badge(control.control, control.index, pad_look.map)
                           : std::nullopt;
    const float pad_px = px(context, text_pad_points);
    const float glyph = std::min(px(context, hint_glyph_points), area.height - 2.0F * pad_px);
    const float glyph_width =
        chord ? pad_glyphs::chord_width(*chord, pad_look.glyphs, glyph) : 0.0F;
    const float gap = chord ? px(context, row_gap_points) : 0.0F;
    const float room = area.width - 2.0F * pad_px - glyph_width - gap;
    const float label_width = std::min(text_width(context, label, row_label_points), room);
    const float left = area.x + (area.width - label_width - gap - glyph_width) * 0.5F;
    const float middle = area.y + area.height * 0.5F;
    text_from(context, label, left, middle, row_label_points, colour, label_width + 1.0F);
    if (chord)
        pad_glyphs::draw_chord(
            context.painter,
            context.fonts,
            *chord,
            pad_look.glyphs,
            {left + label_width + gap, middle - glyph * 0.5F, glyph_width, glyph},
            ink_colour,
            label_colour
        );
}

/// Paints a ring's hint pill under the ring, or over it when there is no room below.
///
/// @param context the painting
/// @param build_ring whether for the build ring, else the order ring
/// @param centre the ring's centre, layer pixels
/// @param outer the ring's outer radius, layer pixels
void paint_ring_hint(
    const PaintContext& context, bool build_ring, paint::Spot centre, float outer
) {
    const auto hint = hud::ring_hint(build_ring, context.state.pad.map);
    const float height = px(context, ring_hint_points);
    const float glyph = px(context, hint_glyph_points) * 0.85F;
    const float canvas_width = static_cast<float>(context.painter.canvas().width);
    const float canvas_height = static_cast<float>(context.painter.canvas().height);
    const float width = std::min(
        hint_width(context, hint, glyph, hint_points) + 2.0F * px(context, pill_pad_points),
        canvas_width - 2.0F * px(context, ring_hint_gap)
    );
    float top = centre.y + outer + px(context, ring_hint_gap);
    if (top + height > canvas_height - px(context, ring_hint_gap))
        top = centre.y - outer - px(context, ring_hint_gap) - height;
    const float left = std::clamp(
        centre.x - width * 0.5F,
        px(context, ring_hint_gap),
        std::max(px(context, ring_hint_gap), canvas_width - px(context, ring_hint_gap) - width)
    );
    const paint::Area area{left, top, width, height};
    paint_pill(context, area, edge_colour, px(context, edge_points));
    paint_hint_in(context, hint, area, label_colour);
}

/// Paints a group chip: its number large and its unit count small; the
/// selection's group lit by its edge and number.
///
/// @param context the painting
/// @param control the chip
/// @param look its looks
void paint_group_chip(
    const PaintContext& context, const hud::ControlRect& control, const ControlLook& look
) {
    const auto face = area_of(context, control.rect);
    paint_face(context, face, look);
    const auto badge = badge_for(context, control);
    if (badge)
        paint_badge(context, *badge);
    const auto area = below_badge(face, badge);
    const auto colour = content_colour(look);
    const uint8_t group = control.index;
    const uint16_t units =
        group < context.state.group_counts.size() ? context.state.group_counts[group] : uint16_t{0};
    const bool wide = area.width / context.point >= 64.0F;
    const float number_points = wide ? 17.0F : 15.0F;
    const std::string number = std::to_string(group);
    std::string count = std::to_string(units);
    if (wide && units == 1) {
        count = context.translate("1 UNIT");
    } else if (wide) {
        // The phrase is translated whole, with the count in its place.
        count = context.translate("{n} UNITS");
        if (const auto at = count.find("{n}"); at != std::string::npos)
            count.replace(at, std::string_view("{n}").size(), std::to_string(units));
    }
    const float cap = cap_share * static_cast<float>(font_px(context, number_points));
    const float sub_cap = cap_share * static_cast<float>(font_px(context, sub_label_points + 1.0F));
    const float gap = px(context, 5.0F);
    const float top = area.y + (area.height - (cap + gap + sub_cap)) * 0.5F;
    const float centre_x = area.x + area.width * 0.5F;
    const float room = area.width - 2.0F * px(context, text_pad_points);
    text_centred(context, number, centre_x, top + cap * 0.5F, number_points, colour, room);
    text_centred(
        context,
        count,
        centre_x,
        top + cap + gap + sub_cap * 0.5F,
        sub_label_points + 1.0F,
        look.lit_edge ? label_colour : colour,
        room
    );
}

/// Paints SELF-DESTRUCT · HOLD: a red-edged panel that fills red from the
/// left as the hold runs.
///
/// @param context the painting
/// @param control the item
/// @param look its looks
void paint_self_destruct(
    const PaintContext& context, const hud::ControlRect& control, const ControlLook& look
) {
    const auto area = area_of(context, control.rect);
    const float radius = px(context, corner_points);
    context.painter.fill_rounded_rect(
        area, radius, look.pressed ? pressed_colour : danger_panel_colour
    );
    const float progress = std::clamp(context.state.self_destruct_progress, 0.0F, 1.0F);
    if (progress > 0.0F) {
        // The fill keeps the panel's rounded corners: clip a full red face.
        const auto saved = context.painter.clip();
        const paint::Box fill{
            static_cast<int>(std::floor(area.x)),
            static_cast<int>(std::floor(area.y)),
            static_cast<int>(std::lround(area.width * progress)),
            static_cast<int>(std::ceil(area.height)) + 1
        };
        context.painter.set_clip(paint::intersect(saved, fill));
        context.painter.fill_rounded_rect(area, radius, danger_colour);
        context.painter.set_clip(saved);
    }
    context.painter.outline_rounded_rect(area, radius, px(context, edge_points), danger_colour);
    const auto label = label_shown(context, control);
    const auto colour = progress > 0.0F ? label_colour : danger_label_colour;
    paint_content(context, area, Arrangement::row, paint::Icon::warning, label, {}, colour, 10.0F);
}

/// Paints one control with its looks, icon and label.
///
/// @param context the painting
/// @param control the control
void paint_control(const PaintContext& context, const hud::ControlRect& control) {
    if (empty(control.rect))
        return;
    switch (control.control) {
    case Control::none:
    case Control::radial_item:
    case Control::radial_hub:
    case Control::sheet_outside:
        return; // the radial is painted whole; outside a sheet nothing shows
    default:
        break;
    }
    switch (control.control) {
    case Control::build_wedge:
    case Control::group_wedge:
        return; // the rings are painted whole
    default:
        break;
    }
    const auto look = look_of(control, context.state);
    if (control.control == Control::group_chip) {
        paint_group_chip(context, control, look);
        return;
    }
    if (context.state.pad.hud &&
        (control.control == Control::queue || control.control == Control::add ||
         control.control == Control::force)) {
        paint_pad_chip(context, control, look);
        return;
    }
    if (control.control == Control::more_item &&
        control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct)) {
        paint_self_destruct(context, control, look);
        return;
    }
    const auto area = area_of(context, control.rect);
    const auto icon = control_icon(control, context.state);
    auto label = label_shown(context, control);
    // The drawer slides out (BUILD ▸) and SELECT opens a menu (SELECT ▾), as
    // the mock-ups mark them, where the mark fits beside the label whole.
    const auto mark = control.control == Control::build_drawer  ? std::string_view{"\xE2\x96\xB8"}
                      : control.control == Control::select_menu ? std::string_view{"\xE2\x96\xBE"}
                                                                : std::string_view{};
    if (!mark.empty() && !label.empty() && label.find(mark) == std::string::npos) {
        const std::string marked = label + " " + std::string(mark);
        const float room =
            area_of(context, control.rect).width - 2.0F * px(context, text_pad_points);
        const float icon_room = control.control == Control::build_drawer
                                    ? px(context, row_icon_points + row_gap_points)
                                    : 0.0F;
        const float size =
            control.control == Control::build_drawer ? row_label_points
            : area_of(context, control.rect).height / context.point >= tall_button_points
                ? label_points
                : small_label_points;
        if (text_width(context, marked, size) <= room - icon_room)
            label = marked;
    }

    const float width_points = area.width / context.point;
    const float height_points = area.height / context.point;
    auto arrangement = arrangement_of(control.control, width_points, height_points, icon, label);
    // PAUSE and MENU beside their icon only while the label fits whole there (a larger Control
    // size narrows the room); else stacked, or the icon alone on a short button.
    if (arrangement == Arrangement::row &&
        (control.control == Control::pause || control.control == Control::menu) &&
        px(context, row_icon_points + row_gap_points) +
                text_width(context, label, row_label_points) >
            area.width - 2.0F * px(context, text_pad_points))
        arrangement = height_points >= 40.0F ? Arrangement::stacked : Arrangement::icon_only;
    if (control.control == Control::banner_cancel) {
        // The banner's ✕ sits on the banner: only a resting finger shows a face.
        if (look.pressed)
            context.painter.fill_circle(
                {area.x + area.width * 0.5F, area.y + area.height * 0.5F},
                std::min(area.width, area.height) * 0.45F,
                pressed_colour
            );
        const float side = std::min(px(context, 14.0F), std::min(area.width, area.height) * 0.6F);
        context.painter.draw_icon(
            icon,
            {area.x + (area.width - side) * 0.5F, area.y + (area.height - side) * 0.5F, side, side},
            label_colour
        );
        return;
    }
    paint_face(context, area, look);
    // A badge takes a strip at the top; the icon and label go under it.
    const auto badge = badge_for(context, control);
    if (badge)
        paint_badge(context, *badge);
    const auto content = below_badge(area, badge);
    float text_points = 0.0F;
    if (control.control == Control::times_five)
        text_points = 14.0F;
    else if (
        control.control == Control::drawer_build_tab ||
        control.control == Control::drawer_orders_tab
    )
        text_points = 10.5F;
    else if (control.control == Control::menu_item)
        text_points = 11.0F;
    const std::string sub_label = look.latched ? context.translate("LATCHED") : std::string{};
    paint_content(
        context, content, arrangement, icon, label, sub_label, content_colour(look), text_points
    );
}

/// Paints a title and a hint side by side, centred between two columns:
/// the title in white, the hint in green, the hint shortened first.
///
/// @param context the painting
/// @param title the title, translated
/// @param hint the hint, translated
/// @param left the leftmost column, layer pixels
/// @param right the rightmost column, layer pixels
/// @param middle the row their capitals' middle lies on
void paint_title_and_hint(
    const PaintContext& context,
    std::string_view title,
    std::string_view hint,
    float left,
    float right,
    float middle
) {
    const float room = right - left;
    if (room <= 0.0F)
        return;
    const float gap = px(context, 10.0F);
    const float title_width = std::min(text_width(context, title, title_points), room);
    const float hint_room = room - title_width - (title.empty() ? 0.0F : gap);
    float hint_width = 0.0F;
    std::string fitted_hint;
    if (!hint.empty() && hint_room > px(context, 24.0F) && context.fonts != nullptr) {
        fitted_hint = paint::fit_text(
            *context.fonts, hint, font_px(context, hint_points), true, static_cast<int>(hint_room)
        );
        hint_width = text_width(context, fitted_hint, hint_points);
    }
    const float total =
        title_width + (fitted_hint.empty() || title.empty() ? 0.0F : gap) + hint_width;
    float pen = left + (room - total) * 0.5F;
    if (!title.empty()) {
        text_from(context, title, pen, middle, title_points, label_colour, title_width + 1.0F);
        pen += title_width + gap;
    }
    if (!fitted_hint.empty())
        text_from(context, fitted_hint, pen, middle, hint_points, lit_colour, hint_width + 1.0F);
}

/// Paints the armed-order or placement banner, under its ✕.
///
/// @param context the painting
void paint_banner(const PaintContext& context) {
    const auto& state = context.state;
    const auto& frame = context.frame;
    if (!state.banner.shown || empty(frame.banner))
        return;
    const auto area = area_of(context, frame.banner);
    paint_pill(context, area, lit_colour, px(context, 1.5F));
    float right = area.x + area.width - px(context, pill_pad_points);
    for (uint8_t i = 0; i < frame.control_count && i < frame.controls.size(); ++i) {
        const auto& control = frame.controls[i];
        if (control.control == Control::banner_cancel && inside(control.rect, frame.banner))
            right = std::min(right, area_of(context, control.rect).x - px(context, 4.0F));
    }
    paint_title_and_hint(
        context,
        hud::banner_title(state, context.translate),
        context.translate(state.banner.hint),
        area.x + px(context, pill_pad_points),
        right,
        area.y + area.height * 0.5F
    );
}

/// Paints the phone's status pill: the selection and what a tap does.
///
/// @param context the painting
void paint_status(const PaintContext& context) {
    const auto& state = context.state;
    const auto& frame = context.frame;
    if (empty(frame.status) || (state.banner.shown && !empty(frame.banner)))
        return;
    const auto area = area_of(context, frame.status);
    paint_pill(context, area, edge_colour, px(context, edge_points));
    // The pad HUD's pill: what R2 and L2 give now, the buttons' glyphs among the words.
    if (state.pad.hud) {
        paint_hint_in(
            context,
            hud::pad_status_hint(
                state.tap_action,
                state.enemy_action,
                state.armed_or_placing,
                state.right_click_interface,
                state.pad.map
            ),
            area,
            label_colour
        );
        return;
    }
    const std::string hint =
        hud::status_hint(state.tap_action, state.enemy_action, context.translate);
    paint_title_and_hint(
        context,
        context.translate(state.selection_text),
        hint,
        area.x + px(context, pill_pad_points),
        area.x + area.width - px(context, pill_pad_points),
        area.y + area.height * 0.5F
    );
}

/// Paints the tip bubble a long press shows, its text wrapped to fit.
///
/// @param context the painting
void paint_tip(const PaintContext& context) {
    const auto& state = context.state;
    const auto& frame = context.frame;
    if (empty(frame.tip) || state.tip.until_ms == 0 || state.tip.text.empty())
        return;
    const auto area = area_of(context, frame.tip);
    const float radius = px(context, corner_points);
    context.painter.fill_rounded_rect(area, radius, sheet_colour);
    context.painter.outline_rounded_rect(area, radius, px(context, edge_points), lit_colour);
    if (context.fonts == nullptr)
        return;
    const std::string text = context.translate(state.tip.text);
    const float pad = px(context, 8.0F);
    const int room = static_cast<int>(area.width - 2.0F * pad);
    // The largest size whose lines fit the bubble's height, down to 7 pt.
    for (float size = tip_points; size >= 7.0F; size -= 1.0F) {
        const int pixel_size = font_px(context, size);
        const auto measure = [&](std::string_view shown) {
            return paint::text_width(*context.fonts, shown, pixel_size, true);
        };
        kit::WrapRules rules;
        rules.shorten_word = [&](std::string_view word) {
            return paint::fit_text(*context.fonts, word, pixel_size, true, room);
        };
        const auto lines = kit::wrap(text, room, measure, rules);
        const float line_height = static_cast<float>(pixel_size) * 1.25F;
        const float block = line_height * static_cast<float>(lines.size());
        if (block > area.height - pad && size > 7.0F)
            continue;
        float middle = area.y + (area.height - block) * 0.5F + line_height * 0.5F;
        for (const auto& line : lines) {
            if (middle > area.y + area.height)
                break;
            text_centred(
                context,
                line,
                area.x + area.width * 0.5F,
                middle,
                size,
                label_colour,
                static_cast<float>(room)
            );
            middle += line_height;
        }
        return;
    }
}

/// Returns the widest stretch of a band, between two columns, that no
/// rectangle crossing the band covers.
///
/// @param left the band's left column, layout pixels
/// @param right the band's right column
/// @param top the band's top row
/// @param bottom the band's bottom row
/// @param taken the rectangles that may cover parts of it
/// @return the stretch's left and right columns; equal when the band is covered
std::pair<int, int>
widest_gap(int left, int right, int top, int bottom, const std::vector<hud::Rect>& taken) {
    std::vector<std::pair<int, int>> covered;
    for (const auto& rect : taken)
        if (rect.y < bottom && rect.y + rect.height > top)
            covered.emplace_back(rect.x, rect.x + rect.width);
    std::sort(covered.begin(), covered.end());
    std::pair<int, int> best{left, left};
    int from = left;
    for (const auto& [start, end] : covered) {
        if (start > from && std::min(start, right) - from > best.second - best.first)
            best = {from, std::min(start, right)};
        from = std::max(from, end);
    }
    if (right - from > best.second - best.first)
        best = {from, right};
    return best;
}

/// Paints page dots, the shown page's filled, centred on a point.
///
/// @param context the painting
/// @param centre_x the column their middle lies on, layer pixels
/// @param middle the row their middle lies on, layer pixels
void paint_page_dots(const PaintContext& context, float centre_x, float middle) {
    const auto pages = context.state.drawer_pages;
    const float diameter = px(context, dot_points);
    const float gap = px(context, dot_gap_points);
    const float width = static_cast<float>(pages) * diameter + static_cast<float>(pages - 1) * gap;
    float x = centre_x - width * 0.5F + diameter * 0.5F;
    for (uint8_t page = 0; page < pages; ++page, x += diameter + gap) {
        if (page == context.state.drawer_page)
            context.painter.fill_circle({x, middle}, diameter * 0.5F, label_colour);
        else
            context.painter.outline_circle(
                {x, middle}, diameter * 0.5F - px(context, 0.6F), px(context, 1.2F), quiet_colour
            );
    }
}

/// Paints the open sheet's panel, and for the drawer and the MORE sheet the
/// empty cells the 3.1c gadgets are placed over, the drawer's title and its
/// page dots.
///
/// @param context the painting
void paint_sheet(const PaintContext& context) {
    const auto& state = context.state;
    const auto& frame = context.frame;
    if (state.sheet == hud::Sheet::none || empty(frame.sheet))
        return;
    const auto area = area_of(context, frame.sheet);
    const float radius = px(context, corner_points);
    context.painter.fill_rounded_rect(area, radius, sheet_colour);
    context.painter.outline_rounded_rect(area, radius, px(context, edge_points), edge_colour);
    const auto cell = [&](const hud::Rect& rect) {
        if (empty(rect))
            return;
        const auto at = area_of(context, rect);
        context.painter.fill_rounded_rect(at, radius, cell_colour);
        context.painter.outline_rounded_rect(at, radius, px(context, edge_points), edge_colour);
    };
    if (state.sheet == hud::Sheet::more) {
        for (uint8_t i = 0; i < frame.more_cell_count && i < frame.more_cells.size(); ++i)
            cell(frame.more_cells[i]);
        return;
    }
    if (state.sheet != hud::Sheet::drawer)
        return;
    for (uint8_t i = 0; i < frame.drawer_cell_count && i < frame.drawer_cells.size(); ++i)
        cell(frame.drawer_cells[i]);

    // The header: the band above the cells. Its controls stand in rows; the
    // title goes in the first row with room beside them, the page dots in
    // the first row with room left after it; else both go under the cells.
    const auto& sheet = frame.sheet;
    const auto& grid = frame.drawer_grid;
    const float layout_point = context.point / context.scale_x; // layout pixels per point
    const int inset = static_cast<int>(std::lround(8.0F * layout_point));
    const int band_top = sheet.y;
    const int band_bottom = !empty(grid) && grid.y > band_top
                                ? grid.y
                                : sheet.y + static_cast<int>(std::lround(44.0F * layout_point));
    std::vector<hud::Rect> taken;
    std::vector<std::pair<int, int>> rows; // top and bottom of each row of header controls
    for (std::size_t i = 0; i < std::min<std::size_t>(frame.control_count, frame.controls.size());
         ++i) {
        const auto& rect = frame.controls[i].rect;
        if (!inside(rect, sheet) || rect.y >= band_bottom || empty(rect))
            continue;
        taken.push_back(rect);
        const int top = rect.y;
        const int bottom = rect.y + rect.height;
        auto row = std::find_if(rows.begin(), rows.end(), [&](const std::pair<int, int>& r) {
            return top < r.second && bottom > r.first;
        });
        if (row == rows.end())
            rows.emplace_back(top, bottom);
        else
            *row = {std::min(row->first, top), std::max(row->second, bottom)};
    }
    if (rows.empty())
        rows.emplace_back(band_top, band_bottom);
    std::sort(rows.begin(), rows.end());
    const auto room_in = [&](const std::pair<int, int>& row) {
        return widest_gap(
            sheet.x + inset, sheet.x + sheet.width - inset, row.first, row.second, taken
        );
    };
    // Under the cells: from the last row of cells (or the grid) to the panel's foot.
    int cells_bottom = empty(grid) ? sheet.y + sheet.height : grid.y + grid.height;
    if (frame.drawer_cell_count > 0) {
        cells_bottom = sheet.y;
        for (uint8_t i = 0; i < frame.drawer_cell_count && i < frame.drawer_cells.size(); ++i)
            cells_bottom =
                std::max(cells_bottom, frame.drawer_cells[i].y + frame.drawer_cells[i].height);
    }
    const float below_grid = static_cast<float>(sheet.y + sheet.height - cells_bottom);
    const float caption_middle =
        (static_cast<float>(cells_bottom) + std::min(below_grid, 28.0F * layout_point) * 0.5F) *
        context.scale_y;
    const bool caption_room = below_grid >= 16.0F * layout_point;
    const std::string title = context.translate(
        state.drawer_title.empty() ? std::string_view{"BUILD"}
                                   : std::string_view{state.drawer_title}
    );
    bool title_drawn = false;
    bool caption_used = false;
    for (const auto& row : rows) {
        const auto gap = room_in(row);
        const float gap_width = static_cast<float>(gap.second - gap.first) * context.scale_x;
        if (gap_width < px(context, 56.0F))
            continue;
        const float margin = px(context, 6.0F);
        const float drawn = text_from(
                                context,
                                title,
                                static_cast<float>(gap.first) * context.scale_x + margin,
                                static_cast<float>(row.first + row.second) * 0.5F * context.scale_y,
                                title_points,
                                label_colour,
                                gap_width - margin
                            ) +
                            margin;
        taken.push_back(
            {gap.first,
             row.first,
             static_cast<int>(std::ceil(drawn / context.scale_x)) + inset,
             row.second - row.first}
        );
        title_drawn = true;
        break;
    }
    if (!title_drawn && caption_room) {
        text_from(
            context,
            title,
            area.x + px(context, 8.0F),
            caption_middle,
            hint_points,
            quiet_colour,
            area.width * 0.6F
        );
        caption_used = true;
    }
    if (state.drawer_pages <= 1)
        return;
    const float dots_width =
        static_cast<float>(state.drawer_pages) * px(context, dot_points) +
        static_cast<float>(state.drawer_pages - 1) * px(context, dot_gap_points);
    // The dots go in the lowest row with room: beside the tabs and x5.
    for (auto row_at = rows.rbegin(); row_at != rows.rend(); ++row_at) {
        const auto& row = *row_at;
        const auto gap = room_in(row);
        if (static_cast<float>(gap.second - gap.first) * context.scale_x <
            dots_width + px(context, 4.0F))
            continue;
        paint_page_dots(
            context,
            static_cast<float>(gap.first + gap.second) * 0.5F * context.scale_x,
            static_cast<float>(row.first + row.second) * 0.5F * context.scale_y
        );
        return;
    }
    if (caption_room) {
        const float centre_x = caption_used
                                   ? area.x + area.width - px(context, 8.0F) - dots_width * 0.5F
                                   : area.x + area.width * 0.5F;
        paint_page_dots(context, centre_x, caption_middle);
    }
}

/// Paints the pad's aim dot: where the thumb aims inside an open ring.
///
/// @param context the painting
/// @param at canvas pixels
void paint_aim_dot(const PaintContext& context, hud::Point at) {
    const paint::Spot spot{
        static_cast<float>(at.x) * context.scale_x, static_cast<float>(at.y) * context.scale_y
    };
    context.painter.fill_circle(spot, px(context, aim_dot_points), label_colour);
    context.painter.outline_circle(
        spot, px(context, aim_dot_points), px(context, 1.0F), ink_colour
    );
}

/// Paints the open radial order menu: twelve wedges round the QUEUE hub, the
/// greyed ones at 40%, the plain tap's item ringed, the pressed one lit.
///
/// @param context the painting
void paint_radial(const PaintContext& context) {
    const auto& state = context.state;
    if (!state.radial)
        return;
    const auto& radial = *state.radial;
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float slot_angle = 2.0F * pi / static_cast<float>(hud::radial_slot_count);
    const paint::Spot centre{
        static_cast<float>(radial.centre.x) * context.scale_x,
        static_cast<float>(radial.centre.y) * context.scale_y
    };
    const float inner = static_cast<float>(radial.inner_radius) * context.scale_x;
    const float outer = static_cast<float>(radial.outer_radius) * context.scale_x;
    if (outer <= inner || inner <= 0.0F)
        return;
    // The ring, then each wedge's lit or ringed look, then the gaps between them. The wedge the
    // pad aims at, and the wedge a mouse-opened wheel aims at, are lit as a pressed one.
    const auto& pad_look = state.pad;
    const auto aimed = [&](std::size_t slot) {
        const bool pad_aims =
            pad_look.ring_by_pad && pad_look.radial_aim && *pad_look.radial_aim == slot;
        const bool mouse_aims = context.mouse_aim && *context.mouse_aim == slot;
        return pad_aims || mouse_aims;
    };
    context.painter.fill_sector(centre, 0.0F, outer, 0.0F, pi, sheet_colour);
    for (std::size_t slot = 0; slot < radial.wedges.size(); ++slot) {
        const auto& wedge = radial.wedges[slot];
        const float middle = static_cast<float>(slot) * slot_angle;
        const bool pressed =
            wedge.available &&
            (aimed(slot) ||
             is_pressed(state, Control::radial_item, static_cast<uint8_t>(wedge.item)));
        if (pressed)
            context.painter.fill_sector(
                centre, inner, outer, middle, slot_angle * 0.5F, lit_colour
            );
        else if (wedge.default_item)
            context.painter.outline_sector(
                centre,
                inner,
                outer,
                middle,
                slot_angle * 0.5F,
                px(context, lit_edge_points),
                lit_colour
            );
    }
    for (std::size_t slot = 0; slot < radial.wedges.size(); ++slot) {
        const float angle = (static_cast<float>(slot) + 0.5F) * slot_angle;
        const paint::Spot from{
            centre.x + std::sin(angle) * inner, centre.y - std::cos(angle) * inner
        };
        const paint::Spot to{
            centre.x + std::sin(angle) * outer, centre.y - std::cos(angle) * outer
        };
        context.painter.stroke_line(from, to, px(context, 1.5F), wedge_gap_colour);
    }
    context.painter.outline_circle(
        centre, outer - px(context, 0.5F), px(context, 1.0F), edge_colour
    );
    // Each wedge's icon over its label. A lit wedge is lettered in the
    // accent's own ink.
    const float side = px(context, 16.0F);
    const float size_points = 8.5F;
    const float least_points = sub_label_points;
    const float cap = cap_share * static_cast<float>(font_px(context, size_points));
    const float gap = px(context, 3.0F);
    const paint::Rgba on_accent_colour{
        kit::colour::on_accent.r,
        kit::colour::on_accent.g,
        kit::colour::on_accent.b,
        kit::colour::on_accent.a
    };
    // Where a row of a wedge's caption may run: from the label's column
    // outwards, as far as each side keeps clear of the wedge's edges and its
    // two arcs.
    const float clear = px(context, 2.0F);
    const float step = std::max(px(context, 0.5F), 0.5F);
    const auto in_wedge = [&](float middle, float at_x, float row) {
        const float dx = at_x - centre.x;
        const float dy = row - centre.y;
        const float radius = std::hypot(dx, dy);
        if (radius < inner + clear || radius > outer - clear)
            return false;
        float off = std::atan2(dx, -dy) - middle;
        while (off > pi)
            off -= 2.0F * pi;
        while (off < -pi)
            off += 2.0F * pi;
        const float apart = slot_angle * 0.5F - std::abs(off);
        return apart > 0.0F && radius * std::sin(apart) >= clear;
    };
    const auto row_reach = [&](float middle, float from_x, float row, float direction) {
        float reach = 0.0F;
        while (reach < outer && in_wedge(middle, from_x + direction * (reach + step), row))
            reach += step;
        return reach;
    };
    for (std::size_t slot = 0; slot < radial.wedges.size(); ++slot) {
        const auto& wedge = radial.wedges[slot];
        const bool pressed =
            wedge.available &&
            (aimed(slot) ||
             is_pressed(state, Control::radial_item, static_cast<uint8_t>(wedge.item)));
        paint::Rgba colour = wedge.default_item ? lit_colour : label_colour;
        if (pressed)
            colour = on_accent_colour;
        colour = greyed_if(colour, !wedge.available);
        const float x = static_cast<float>(wedge.label.x) * context.scale_x;
        const float y = static_cast<float>(wedge.label.y) * context.scale_y;
        const float top = y - (side + gap + cap) * 0.5F;
        context.painter.draw_icon(
            radial_icon(wedge.item), paint::Area{x - side * 0.5F, top, side, side}, colour
        );
        // The caption stays inside its wedge: its room is what both its top
        // and its bottom rows keep clear, it sits in the middle of that room,
        // and a caption wider than the room steps down in size, then is
        // shortened.
        const float middle = static_cast<float>(slot) * slot_angle;
        const float caption_middle = top + side + gap + cap * 0.5F;
        const float upper = caption_middle - cap * 0.5F;
        const float lower = caption_middle + cap * 0.5F;
        const float left =
            x - std::min(row_reach(middle, x, upper, -1.0F), row_reach(middle, x, lower, -1.0F));
        const float right =
            x + std::min(row_reach(middle, x, upper, 1.0F), row_reach(middle, x, lower, 1.0F));
        const float room = right - left;
        const std::string caption = context.translate(radial_label(wedge.item, state));
        float fitted_points = size_points;
        while (fitted_points > least_points && context.fonts != nullptr &&
               static_cast<float>(
                   paint::text_width(*context.fonts, caption, font_px(context, fitted_points), true)
               ) > room)
            fitted_points -= 0.5F;
        text_centred(
            context, caption, (left + right) * 0.5F, caption_middle, fitted_points, colour, room
        );
    }
    // The hub: QUEUE for the next pick, lit once tapped.
    const float hub = inner - px(context, 3.0F);
    const bool hub_pressed = is_pressed(state, Control::radial_hub, 0);
    paint::Rgba hub_fill = panel_colour;
    if (radial.queue_hub)
        hub_fill = hub_pressed ? lit_pressed_colour : lit_colour;
    else if (hub_pressed)
        hub_fill = pressed_colour;
    context.painter.fill_circle(centre, hub, hub_fill);
    if (!radial.queue_hub)
        context.painter.outline_circle(centre, hub, px(context, edge_points), edge_colour);
    const auto hub_colour = radial.queue_hub ? ink_colour : label_colour;
    const float hub_side = px(context, 15.0F);
    const float hub_top = centre.y - (hub_side + gap + cap) * 0.5F;
    context.painter.draw_icon(
        paint::Icon::shift_arrow,
        paint::Area{centre.x - hub_side * 0.5F, hub_top, hub_side, hub_side},
        hub_colour
    );
    text_centred(
        context,
        context.translate("QUEUE"),
        centre.x,
        hub_top + hub_side + gap + cap * 0.5F,
        size_points,
        hub_colour,
        hub * 1.8F
    );
    // Where the order is given, when the ring had to move off it.
    const float anchor_x = static_cast<float>(radial.anchor.x) * context.scale_x;
    const float anchor_y = static_cast<float>(radial.anchor.y) * context.scale_y;
    if (std::hypot(anchor_x - centre.x, anchor_y - centre.y) > hub) {
        context.painter.outline_circle(
            {anchor_x, anchor_y}, px(context, 7.0F), px(context, 2.0F), label_colour
        );
        context.painter.fill_circle({anchor_x, anchor_y}, px(context, 2.0F), label_colour);
    }
    // A ring the pad opened: where the thumb aims, and how to give, arm or close.
    if (pad_look.ring_by_pad) {
        paint_aim_dot(context, pad_look.aim_dot);
        paint_ring_hint(context, false, centre, outer);
    }
}

/// Returns the icon a build ring wedge shows when it has no 3.1c picture.
///
/// @param kind what the wedge is
/// @return its icon
paint::Icon build_wedge_icon(hud::BuildWedgeKind kind) {
    switch (kind) {
    case hud::BuildWedgeKind::empty:
        return paint::Icon::none;
    case hud::BuildWedgeKind::build:
        return paint::Icon::grid;
    case hud::BuildWedgeKind::prev:
        return paint::Icon::chevron_left;
    case hud::BuildWedgeKind::next:
        return paint::Icon::chevron_right;
    case hud::BuildWedgeKind::fire_orders:
        return paint::Icon::crosshair;
    case hud::BuildWedgeKind::move_orders:
        return paint::Icon::arrow;
    case hud::BuildWedgeKind::on_off:
        return paint::Icon::power;
    case hud::BuildWedgeKind::cloak:
        return paint::Icon::cloak;
    case hud::BuildWedgeKind::info:
        return paint::Icon::info;
    case hud::BuildWedgeKind::self_destruct:
        return paint::Icon::warning;
    }
    return paint::Icon::none;
}

/// Paints the open build ring: eight wedges round the hub, the one the pad aims at lit, each
/// with its 3.1c picture (its caption carries a factory's count), PREV and NEXT and the
/// standing orders' marks, the greyed ones at 40%, and the ring's hint under it.
///
/// @param context the painting
void paint_build_ring(const PaintContext& context) {
    const auto& state = context.state;
    if (!state.build_ring)
        return;
    const auto& ring = *state.build_ring;
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float slot_angle = 2.0F * pi / static_cast<float>(hud::build_ring_slot_count);
    const paint::Spot centre{
        static_cast<float>(ring.centre.x) * context.scale_x,
        static_cast<float>(ring.centre.y) * context.scale_y
    };
    const float inner = static_cast<float>(ring.inner_radius) * context.scale_x;
    const float outer = static_cast<float>(ring.outer_radius) * context.scale_x;
    if (outer <= inner || inner <= 0.0F)
        return;
    const auto& aim = state.pad.build_aim;
    context.painter.fill_sector(centre, 0.0F, outer, 0.0F, pi, sheet_colour);
    for (std::size_t slot = 0; slot < ring.wedges.size(); ++slot) {
        const auto& wedge = ring.wedges[slot];
        if (!aim || *aim != slot || wedge.kind == hud::BuildWedgeKind::empty)
            continue;
        const float middle = static_cast<float>(slot) * slot_angle;
        if (wedge.available)
            context.painter.fill_sector(
                centre, inner, outer, middle, slot_angle * 0.5F, lit_colour
            );
        else
            context.painter.outline_sector(
                centre,
                inner,
                outer,
                middle,
                slot_angle * 0.5F,
                px(context, lit_edge_points),
                greyed_if(lit_colour, true)
            );
    }
    for (std::size_t slot = 0; slot < ring.wedges.size(); ++slot) {
        const float angle = (static_cast<float>(slot) + 0.5F) * slot_angle;
        context.painter.stroke_line(
            {centre.x + std::sin(angle) * inner, centre.y - std::cos(angle) * inner},
            {centre.x + std::sin(angle) * outer, centre.y - std::cos(angle) * outer},
            px(context, 1.5F),
            wedge_gap_colour
        );
    }
    context.painter.outline_circle(
        centre, outer - px(context, 0.5F), px(context, 1.0F), edge_colour
    );
    // Each wedge's picture, or its mark and label.
    const auto* hud_layer = context.hud_layer;
    for (std::size_t slot = 0; slot < ring.wedges.size(); ++slot) {
        const auto& wedge = ring.wedges[slot];
        if (wedge.kind == hud::BuildWedgeKind::empty)
            continue;
        const bool lit = wedge.available && aim && *aim == slot;
        const float opacity = wedge.available ? 1.0F : greyed_opacity;
        const RingPicture* picture =
            context.pictures != nullptr ? &(*context.pictures)[slot] : nullptr;
        if (picture != nullptr && !empty(picture->source) && hud_layer != nullptr) {
            const auto at = area_of(context, picture->canvas);
            context.painter.draw_rgb(
                hud_layer->rgb,
                static_cast<int>(hud_layer->width),
                static_cast<int>(hud_layer->height),
                {picture->source.x,
                 picture->source.y,
                 picture->source.width,
                 picture->source.height},
                at,
                opacity
            );
            if (lit)
                context.painter.outline_rounded_rect(
                    at, px(context, 2.0F), px(context, lit_edge_points), label_colour
                );
            continue;
        }
        const auto box = area_of(context, wedge.picture);
        const float side = px(context, 18.0F);
        const float size_points = 8.5F;
        const float cap = cap_share * static_cast<float>(font_px(context, size_points));
        const float gap = px(context, 3.0F);
        const paint::Rgba colour = greyed_if(lit ? ink_colour : label_colour, !wedge.available);
        const float top = box.y + (box.height - side - gap - cap) * 0.5F;
        context.painter.draw_icon(
            build_wedge_icon(wedge.kind),
            {box.x + (box.width - side) * 0.5F, top, side, side},
            colour
        );
        std::string label(
            hud::control_label(Control::build_wedge, static_cast<uint8_t>(slot), state)
        );
        if (label.empty() && wedge.queued > 0)
            label = std::to_string(wedge.queued);
        const auto lookup =
            hud::control_label_lookup(Control::build_wedge, static_cast<uint8_t>(slot), state);
        text_centred(
            context,
            hud::shown_label(label, lookup, context.translate),
            box.x + box.width * 0.5F,
            top + side + gap + cap * 0.5F,
            size_points,
            colour,
            (outer - inner) * 1.1F
        );
    }
    // The hub closes the ring: nothing is given there.
    const float hub = inner - px(context, 3.0F);
    context.painter.fill_circle(centre, hub, panel_colour);
    context.painter.outline_circle(centre, hub, px(context, edge_points), edge_colour);
    text_centred(
        context,
        context.translate(ring.standing_orders ? "ORDERS" : "BUILD"),
        centre.x,
        centre.y,
        8.5F,
        quiet_colour,
        hub * 1.8F
    );
    paint_ring_hint(context, true, centre, outer);
}

/// Paints the group ring while the groups layer is held: nine wedges with their group numbers
/// and sizes, the aimed one lit, the selection's group ringed, empty groups greyed.
///
/// @param context the painting
void paint_group_ring(const PaintContext& context) {
    const auto& state = context.state;
    if (!state.pad.group_ring)
        return;
    const auto& ring = *state.pad.group_ring;
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float slot_angle = 2.0F * pi / static_cast<float>(hud::group_ring_slot_count);
    const paint::Spot centre{
        static_cast<float>(ring.centre.x) * context.scale_x,
        static_cast<float>(ring.centre.y) * context.scale_y
    };
    const float inner = static_cast<float>(ring.inner_radius) * context.scale_x;
    const float outer = static_cast<float>(ring.outer_radius) * context.scale_x;
    if (outer <= inner || inner <= 0.0F)
        return;
    context.painter.fill_sector(centre, inner, outer, 0.0F, pi, sheet_colour);
    for (std::size_t slot = 0; slot < hud::group_ring_slot_count; ++slot) {
        const auto group = static_cast<uint8_t>(slot + 1);
        const float middle = static_cast<float>(slot) * slot_angle;
        if (ring.aim && *ring.aim == group)
            context.painter.fill_sector(
                centre, inner, outer, middle, slot_angle * 0.5F, lit_colour
            );
        else if (state.selected_group == group)
            context.painter.outline_sector(
                centre,
                inner,
                outer,
                middle,
                slot_angle * 0.5F,
                px(context, lit_edge_points),
                lit_colour
            );
    }
    for (std::size_t slot = 0; slot < hud::group_ring_slot_count; ++slot) {
        const float angle = (static_cast<float>(slot) + 0.5F) * slot_angle;
        context.painter.stroke_line(
            {centre.x + std::sin(angle) * inner, centre.y - std::cos(angle) * inner},
            {centre.x + std::sin(angle) * outer, centre.y - std::cos(angle) * outer},
            px(context, 1.5F),
            wedge_gap_colour
        );
    }
    context.painter.outline_circle(
        centre, outer - px(context, 0.5F), px(context, 1.0F), edge_colour
    );
    const float radius = (inner + outer) * 0.5F;
    const float number_points = 12.0F;
    const float count_points = sub_label_points;
    const float cap = cap_share * static_cast<float>(font_px(context, number_points));
    const float sub_cap = cap_share * static_cast<float>(font_px(context, count_points));
    const float gap = px(context, 2.0F);
    for (std::size_t slot = 0; slot < hud::group_ring_slot_count; ++slot) {
        const auto group = static_cast<uint8_t>(slot + 1);
        const float angle = static_cast<float>(slot) * slot_angle;
        const float x = centre.x + std::sin(angle) * radius;
        const float y = centre.y - std::cos(angle) * radius;
        const uint16_t units = group < state.group_counts.size() ? state.group_counts[group] : 0;
        const bool lit = ring.aim && *ring.aim == group;
        const paint::Rgba colour = greyed_if(lit ? ink_colour : label_colour, units == 0);
        const float top = y - (cap + (units != 0 ? gap + sub_cap : 0.0F)) * 0.5F;
        text_centred(
            context,
            std::to_string(group),
            x,
            top + cap * 0.5F,
            number_points,
            colour,
            outer - inner
        );
        if (units != 0)
            text_centred(
                context,
                std::to_string(units),
                x,
                top + cap + gap + sub_cap * 0.5F,
                count_points,
                colour,
                outer - inner
            );
    }
}

/// Paints a timed pad hold's progress (self-destruct): a ring that fills clockwise from the top.
///
/// @param context the painting
void paint_hold_progress(const PaintContext& context) {
    const auto& pad_look = context.state.pad;
    if (pad_look.hold_progress <= 0.0F)
        return;
    constexpr float pi = std::numbers::pi_v<float>;
    const paint::Spot centre{
        static_cast<float>(pad_look.hold_point.x) * context.scale_x,
        static_cast<float>(pad_look.hold_point.y) * context.scale_y
    };
    const float radius = px(context, hold_ring_points);
    const float width = px(context, hold_stroke_points);
    const float progress = std::clamp(pad_look.hold_progress, 0.0F, 1.0F);
    context.painter.outline_circle(
        centre, radius, width, paint::with_opacity(danger_colour, hold_track_opacity)
    );
    context.painter.stroke_arc(centre, radius, progress * pi, progress * pi, width, danger_colour);
}

/// Paints the ring round the SELECT ▾ item the D-pad marks.
///
/// @param context the painting
void paint_sheet_focus(const PaintContext& context) {
    const auto& state = context.state;
    if (state.sheet != hud::Sheet::select_menu || state.sheet_focus < 0)
        return;
    const auto& frame = context.frame;
    for (std::size_t i = 0; i < std::min<std::size_t>(frame.control_count, frame.controls.size());
         ++i) {
        const auto& control = frame.controls[i];
        if (control.control != Control::menu_item ||
            control.index != static_cast<uint8_t>(state.sheet_focus))
            continue;
        context.painter.outline_rounded_rect(
            area_of(context, control.rect),
            px(context, corner_points),
            px(context, lit_edge_points),
            lit_colour
        );
        return;
    }
}

/// Paints every touch control the frame lays out, in the order they stack:
/// the status pill and the banner, the controls on the battlefield, the
/// open sheet and its controls with the D-pad's focus, the radial menu, the
/// pad's build and group rings and its hold, and the tip on top.
///
/// @param context the painting
void paint_controls(const PaintContext& context) {
    const auto& frame = context.frame;
    paint_status(context);
    paint_banner(context);
    const bool sheet_open = context.state.sheet != hud::Sheet::none && !empty(frame.sheet);
    // The sheet's own controls go over its panel; every other control, even
    // one the panel covers (the phone's left column under the drawer), under it.
    const auto in_sheet = [&](const hud::ControlRect& control) {
        switch (control.control) {
        case Control::menu_item:
        case Control::more_item:
        case Control::drawer_build_tab:
        case Control::drawer_orders_tab:
        case Control::drawer_close:
        case Control::drawer_prev:
        case Control::drawer_next:
            return true;
        case Control::times_five:
            return sheet_open && context.state.sheet == hud::Sheet::drawer &&
                   inside(control.rect, frame.sheet);
        default:
            return false;
        }
    };
    const std::size_t count = std::min<std::size_t>(frame.control_count, frame.controls.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& control = frame.controls[i];
        // A control the open sheet covers whole stays hidden under it.
        if (!in_sheet(control) && !(sheet_open && inside(control.rect, frame.sheet)))
            paint_control(context, control);
    }
    paint_sheet(context);
    for (std::size_t i = 0; i < count; ++i)
        if (in_sheet(frame.controls[i]))
            paint_control(context, frame.controls[i]);
    paint_sheet_focus(context);
    paint_radial(context);
    paint_build_ring(context);
    paint_group_ring(context);
    paint_hold_progress(context);
    paint_tip(context);
}

/// Returns whether two layouts of the controls are the same.
///
/// @param a a frame
/// @param b another frame
/// @return true when every control and region matches
bool same_frame(const hud::Frame& a, const hud::Frame& b) {
    const auto same = [](const hud::Rect& x, const hud::Rect& y) {
        return x.x == y.x && x.y == y.y && x.width == y.width && x.height == y.height;
    };
    if (a.control_count != b.control_count || a.drawer_cell_count != b.drawer_cell_count ||
        a.more_cell_count != b.more_cell_count)
        return false;
    for (std::size_t i = 0; i < std::min<std::size_t>(a.control_count, a.controls.size()); ++i) {
        const auto& x = a.controls[i];
        const auto& y = b.controls[i];
        if (x.control != y.control || x.index != y.index || !same(x.rect, y.rect))
            return false;
    }
    for (std::size_t i = 0; i < a.drawer_cells.size(); ++i)
        if (!same(a.drawer_cells[i], b.drawer_cells[i]))
            return false;
    for (std::size_t i = 0; i < a.more_cells.size(); ++i)
        if (!same(a.more_cells[i], b.more_cells[i]))
            return false;
    return same(a.minimap, b.minimap) && same(a.resources, b.resources) &&
           same(a.status, b.status) && same(a.banner, b.banner) && same(a.tip, b.tip) &&
           same(a.sheet, b.sheet) && same(a.drawer_grid, b.drawer_grid) &&
           same(a.more_grid, b.more_grid) && same(a.placement_bar, b.placement_bar) &&
           same(a.panel_sheet, b.panel_sheet) && same(a.clear, b.clear);
}

} // namespace

bool TouchDrawAccess::refresh_layer(Runtime& runtime) {
    // The touch controls, the pad HUD a gamepad brought, or a wheel a mouse button opened.
    // With neither the controls nor the pad HUD on, that wheel is the only thing drawn.
    if (runtime.touch_ == nullptr || runtime.screen_ != Screen::match)
        return false;
    const bool controls = runtime.touch_controls_active() || runtime.touch_->hud.pad.hud;
    const bool mouse_wheel =
        runtime.order_wheel_mouse_open() && runtime.touch_->hud.radial.has_value();
    if (!controls && !mouse_wheel)
        return false;
    auto& state = *runtime.touch_;
    auto& layer = state.layer;
    const auto& match = runtime.match_layout_;
    if (match.width <= 0 || match.height <= 0)
        return false;
    // The full controls need a laid-out frame. The wheel alone does not.
    if (controls && !state.frame_ready)
        return false;
    double density = runtime.match_display_density();
    if (!std::isfinite(density) || density <= 0.0)
        density = 1.0;
    TouchLayer::Look look{};
    look.revision = state.hud.revision;
    look.width = match.width;
    look.height = match.height;
    look.layer_width = std::max(1, static_cast<int32_t>(std::lround(match.width * density)));
    look.layer_height = std::max(1, static_cast<int32_t>(std::lround(match.height * density)));
    look.px_per_point = state.viewport.px_per_point > 0.0F ? state.viewport.px_per_point : 1.0F;
    if (layer.drawn == look && same_frame(layer.drawn_frame, state.frame))
        return !layer.bounds.empty();

    if (!layer.fonts_tried) {
        layer.fonts_tried = true;
        try {
            layer.fonts = text_font::FontStack::open(text_font::bundled_font_directory());
        } catch (const std::exception&) {
            layer.fonts.reset();
        }
    }
    if (layer.canvas.width != look.layer_width || layer.canvas.height != look.layer_height) {
        layer.canvas = paint::make_canvas(look.layer_width, look.layer_height);
        layer.bounds = {};
    }
    paint::Painter painter(layer.canvas);
    painter.clear_box(layer.bounds);
    const Translate translate = interface_translation(runtime);
    const float scale_x = static_cast<float>(look.layer_width) / static_cast<float>(look.width);
    const float scale_y = static_cast<float>(look.layer_height) / static_cast<float>(look.height);
    // The build ring's pictures, from the HUD layer as the phone drawer's regions show them.
    std::array<RingPicture, hud::build_ring_slot_count> pictures{};
    if (state.hud.build_ring)
        pictures = PhoneHudAccess::build_ring_pictures(runtime, *state.hud.build_ring);
    const PaintContext context{
        painter,
        layer.fonts.get(),
        scale_x,
        scale_y,
        look.px_per_point * scale_x,
        translate,
        state.hud,
        state.frame,
        state.hud.build_ring ? &runtime.match_hud_cpu_ : nullptr,
        state.hud.build_ring ? &pictures : nullptr,
        runtime.order_wheel_mouse_aim()
    };
    if (controls)
        paint_controls(context);
    else
        paint_radial(context);
    layer.bounds = painter.painted();
    layer.drawn = look;
    layer.drawn_frame = state.frame;
    layer.drawn_revision = look.revision;
    layer.uploaded.reset();
    return !layer.bounds.empty();
}

std::string TouchDrawAccess::banner_title(Runtime& runtime) {
    if (runtime.touch_ == nullptr)
        return {};
    return hud::banner_title(runtime.touch_->hud, interface_translation(runtime));
}

void TouchDrawAccess::forget_textures(Runtime& runtime) noexcept {
    if (runtime.touch_ == nullptr)
        return;
    auto& layer = runtime.touch_->layer;
    layer.texture.reset();
    layer.uploaded.reset();
    layer.texture_renderer = nullptr;
}

uint32_t TouchDrawAccess::device_resets(const Runtime& runtime) noexcept {
    return runtime.render_run_ ? runtime.render_run_->resets_handled : 0U;
}

void Runtime::compose_touch_layer(renderer::Surface& frame) {
    if (!TouchDrawAccess::refresh_layer(*this))
        return;
    const auto& layer = touch_->layer;
    const auto width = static_cast<int32_t>(frame.width);
    const auto height = static_cast<int32_t>(frame.height);
    if (!layer.drawn || layer.drawn->width != width || layer.drawn->height != height ||
        frame.rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U)
        return;
    const auto& canvas = layer.canvas;
    const auto& bounds = layer.bounds;
    const bool one_to_one = canvas.width == width && canvas.height == height;
    const double across = static_cast<double>(canvas.width) / width;
    const double down = static_cast<double>(canvas.height) / height;
    // The frame's pixels the painted box reaches.
    const int32_t left = std::max(0, static_cast<int32_t>(std::floor(bounds.x / across)));
    const int32_t top = std::max(0, static_cast<int32_t>(std::floor(bounds.y / down)));
    const int32_t right =
        std::min(width, static_cast<int32_t>(std::ceil((bounds.x + bounds.width) / across)));
    const int32_t bottom =
        std::min(height, static_cast<int32_t>(std::ceil((bounds.y + bounds.height) / down)));
    for (int32_t row = top; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column) {
            // The layer pixel, or the average of the layer pixels, under this frame pixel,
            // its colour counted by its opacity.
            uint32_t alpha = 0;
            std::array<uint32_t, 3> colour{};
            if (one_to_one) {
                const auto* source = canvas.rgba.data() +
                                     (static_cast<std::size_t>(row) * canvas.width + column) * 4U;
                alpha = source[3];
                for (std::size_t channel = 0; channel < 3; ++channel)
                    colour[channel] = source[channel];
            } else {
                const int32_t x0 = static_cast<int32_t>(std::floor(column * across));
                const int32_t y0 = static_cast<int32_t>(std::floor(row * down));
                const int32_t x1 = std::min(
                    canvas.width,
                    std::max(x0 + 1, static_cast<int32_t>(std::floor((column + 1) * across)))
                );
                const int32_t y1 = std::min(
                    canvas.height,
                    std::max(y0 + 1, static_cast<int32_t>(std::floor((row + 1) * down)))
                );
                uint64_t weighted_alpha = 0;
                std::array<uint64_t, 3> weighted{};
                for (int32_t y = y0; y < y1; ++y)
                    for (int32_t x = x0; x < x1; ++x) {
                        const auto* source = canvas.rgba.data() +
                                             (static_cast<std::size_t>(y) * canvas.width + x) * 4U;
                        weighted_alpha += source[3];
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            weighted[channel] += static_cast<uint64_t>(source[channel]) * source[3];
                    }
                const auto samples =
                    static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
                alpha = static_cast<uint32_t>((weighted_alpha + samples / 2U) / samples);
                if (weighted_alpha != 0)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        colour[channel] = static_cast<uint32_t>(
                            (weighted[channel] + weighted_alpha / 2U) / weighted_alpha
                        );
            }
            if (alpha == 0)
                continue;
            auto* target = frame.rgb.data() +
                           (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(column)) *
                               3U;
            // As the settings layer goes over the composed frame.
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const unsigned shown =
                    gamma_identity_ ? colour[channel] : gamma_table_[colour[channel]];
                target[channel] =
                    static_cast<uint8_t>((shown * alpha + target[channel] * (255U - alpha)) / 255U);
            }
        }
}

void Runtime::present_touch_layer() {
    if (sdl_.renderer == nullptr || !TouchDrawAccess::refresh_layer(*this))
        return;
    auto& layer = touch_->layer;
    const auto& drawn = *layer.drawn;
    if (layer.texture_renderer != sdl_.renderer) {
        // Made on a renderer since replaced: its textures went with it.
        layer.texture.reset();
        layer.uploaded.reset();
        layer.texture_renderer = sdl_.renderer;
    }
    if (layer.texture.ensure(
            sdl_.renderer,
            SDL_PIXELFORMAT_RGBA32,
            drawn.layer_width,
            drawn.layer_height,
            render_texture_limit(),
            SDL_BLENDMODE_BLEND
        ))
        layer.uploaded.reset();
    const uint32_t resets = TouchDrawAccess::device_resets(*this);
    const auto& bounds = layer.bounds;
    if (layer.uploaded != layer.drawn || layer.uploaded_gamma != gamma_table_ ||
        layer.texture_resets != resets) {
        // Only the painted rows: the draw below reads no other.
        const auto first_row = static_cast<uint32_t>(bounds.y);
        const auto end_row = static_cast<uint32_t>(bounds.y + bounds.height);
        const std::size_t pitch = static_cast<std::size_t>(drawn.layer_width) * 4U;
        const uint8_t* pixels = layer.canvas.rgba.data();
        if (!gamma_identity_) {
            layer.corrected.resize(layer.canvas.rgba.size());
            const std::size_t from = first_row * pitch;
            const std::size_t bytes = (end_row - first_row) * pitch;
            std::copy_n(layer.canvas.rgba.data() + from, bytes, layer.corrected.data() + from);
            apply_gamma_rgb(layer.corrected.data() + from, bytes / 4U, 4);
            pixels = layer.corrected.data();
        }
        layer.texture.update(pixels, static_cast<int>(pitch), 4, first_row, end_row);
        layer.uploaded = layer.drawn;
        layer.uploaded_gamma = gamma_table_;
        layer.texture_resets = resets;
    }
    const float across = static_cast<float>(drawn.layer_width) / static_cast<float>(drawn.width);
    const float down = static_cast<float>(drawn.layer_height) / static_cast<float>(drawn.height);
    const SDL_FRect source{
        static_cast<float>(bounds.x),
        static_cast<float>(bounds.y),
        static_cast<float>(bounds.width),
        static_cast<float>(bounds.height)
    };
    const SDL_FRect destination{
        source.x / across, source.y / down, source.w / across, source.h / down
    };
    // The layer holds the display's pixels, laid over the layout's.
    draw_one_to_one(sdl_.renderer, layer.texture, &source, &destination, one_to_one_scale_mode());
}

} // namespace oa::app
