// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The OA layer (OaLayer): one host for Open Annihilation's own screens on
// every screen of the game. It keeps a stack of screens (LayerScreen) above
// the game's picture, and a queue of those nobody asked for at that moment
// that wait for it to be free; maps the input to each by its placement,
// tells a finger from a mouse, maps the keys to the kit's, latches the key
// that closed a screen, lets the top modal screen take every input, darkens
// what lies under each opaque modal screen, and draws the screens in the
// window's own pixels before the software cursor, each opaque or laid over
// what lies under it by its pixels' opacity. On the front end it draws them
// at the scale kit::layer_viewport gives (the Interface size setting, or the
// Auto scale) and lays them out at the size class of the points left over:
// at a whole scale on the kit canvas's crisp form, and at 150% on its smooth
// form in the modern fonts. In a match the settings dialog takes a scale and
// size class of its own from the whole window, capped by the side column's
// scale (OaLayer::match_dialog). A window such as Settings or the Library
// moves by its title, and keeps its place for each size class
// (window_placement.hpp); its corners and a notice's are rounded, what lies
// behind showing there (round_layer_corners). OA's overlays over a match
// show where the player arranged them (oa_layer_overlays.hpp,
// oa_layer_arrange.cpp). oa_layer.cpp holds it, the
// settings' screen (Runtime::SettingsScreen), the screens of a notice
// (NoticeScreen) and a question (QuestionScreen), and the UI kit gallery
// (GalleryScreen), which covers the whole window over the settings.
#pragma once

#include "oa/app/automation_host.hpp"
#include "oa/app/runtime.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/gallery.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/screen_registry.hpp"
#include "oa/ui/screen_registry/kit.hpp"
#include "oa_layer_overlays.hpp"
#include "window_placement.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

class OaLayer;
class GalleryScreen;

/// What a screen of the OA layer needs to place itself.
struct LayerView {
    /// The layer shows over a match; otherwise over the front end's picture.
    /// Either way its screens are placed in the window's own pixels.
    bool match{};
    /// The screen of the game the layer shows over now.
    Screen game_screen{};
    /// The match's layout; null on the front end.
    const oa::ui::display_layout::MatchLayout* match_layout{};
    /// In a match, the screens fit the window's safe area rather than the
    /// side column's scale: the touch controls are on and the window is
    /// phone class.
    bool fit_safe_area{};
    /// The room the screens have, in points, with its size class and scale.
    /// On the front end: the window's canvas less its safe insets while the
    /// game has touch controls, divided by the scale (kit::frame_of), its
    /// class the Window size's where one is chosen
    /// (window_placement::class_under). In a match: the safe area, Compact
    /// at 100%, as the in-match Settings takes its own class and scale
    /// (OaLayer::match_dialog).
    oa::ui::kit::Frame frame{};
    /// The Window size in effect (window_placement::window_size_key): Auto,
    /// or the class the player chose, on the front end and in a match.
    window_placement::WindowSize window_size{window_placement::WindowSize::automatic};
    /// The whole number of window pixels a point takes: the step the
    /// Interface size setting in effect chooses, or for Auto the window
    /// canvas's Auto scale (kit::auto_scale), one step lower where that
    /// gives the room the class the Window size wants, no larger than lets
    /// Compact's settings dialog fit the room, and at least 1
    /// (kit::layer_viewport).
    /// At 150% it is 1, and scale_percent holds the fraction. 1 in a match.
    int32_t scale{1};
    /// The window pixels a point takes, in percent: scale times 100, or 150
    /// while the Interface size in effect is 150%, the modern fonts are on
    /// and Compact's dialog fits the room at one and a half. The screens are
    /// drawn on the kit canvas's crisp form at a whole scale and on its
    /// smooth form at 150%. 100 in a match.
    int32_t scale_percent{100};
    /// The Interface size in effect, in percent, as kit::layer_viewport
    /// takes it: 0 for Auto on whole steps, kit::auto_with_fraction_percent
    /// for Auto with 150% among the steps (the modern fonts are on and
    /// open), else 100 to 400, and 150 only while the modern fonts in
    /// effect are on and open. On the front end and in a match alike; in a
    /// match the settings dialog takes its scale from it
    /// (OaLayer::match_dialog).
    int32_t interface_percent{};
    /// The room in window pixels: the canvas less its safe insets on the
    /// front end, the safe area in a match. A centred screen centres in it.
    oa::ui::display_layout::Rect room{};
    /// Where the front end's picture, the 640 by 480 frame, is drawn in the
    /// window, in window pixels: what a screen anchored to a part of a 3.1c
    /// screen places itself by. The match's canvas in a match.
    oa::ui::display_layout::Rect picture{};
    /// The picture's own size, in its pixels: what input on the front end
    /// arrives in, and the picture's rectangle holds.
    oa::ui::display_layout::Point picture_size{};
};

/// Maps a rectangle of the 640 by 480 frame to the OA layer's points through
/// the picture's place in the window, the edges outward.
///
/// MORE MAPS… and MUTATORS… place from this, and the button check measures
/// with it.
///
/// @param seen what the layer's screens are placed on
/// @param frame_rect the rectangle, in the frame's pixels
/// @return the rectangle, in the layer's points
inline oa::ui::kit::Rect
frame_to_points(const LayerView& seen, const oa::ui::kit::Rect& frame_rect) {
    if (seen.picture_size.x <= 0 || seen.picture_size.y <= 0)
        return {};
    const int64_t percent = std::max(seen.scale_percent, int32_t{100});
    const auto& picture = seen.picture;
    const auto across = [&](int32_t column) {
        return picture.x + static_cast<int64_t>(column) * picture.width / seen.picture_size.x;
    };
    const auto down = [&](int32_t row) {
        return picture.y + static_cast<int64_t>(row) * picture.height / seen.picture_size.y;
    };
    const auto floor_points = [percent](int64_t pixels) {
        const int64_t scaled = pixels * 100;
        return static_cast<int32_t>(
            scaled >= 0 ? scaled / percent : -((-scaled + percent - 1) / percent)
        );
    };
    const auto ceil_points = [percent](int64_t pixels) {
        const int64_t scaled = pixels * 100;
        return static_cast<int32_t>(
            scaled >= 0 ? (scaled + percent - 1) / percent : -(-scaled / percent)
        );
    };
    const int32_t left = floor_points(across(frame_rect.x));
    const int32_t top = floor_points(down(frame_rect.y));
    const int32_t right = ceil_points(across(frame_rect.x + frame_rect.width));
    const int32_t bottom = ceil_points(down(frame_rect.y + frame_rect.height));
    return {left, top, right - left, bottom - top};
}

/// Where a screen of the OA layer shows.
struct LayerPlacement {
    /// Where the screen shows, in window pixels: points_width at the scale
    /// wide (kit::to_canvas), on the front end and for the in-match
    /// Settings beside the side column; on a phone, wherever the in-match
    /// Settings is fitted to the safe area. Empty while the screen does not
    /// show: it then neither draws nor takes input.
    oa::ui::display_layout::Rect shown{};
    int32_t points_width{};  ///< the screen's width in its own points, as draw draws it
    int32_t points_height{}; ///< the screen's height in its own points
    /// The scale the screen is drawn at as large as its place, in percent,
    /// which layer_point divides by: the view's on the front end, the
    /// in-match Settings' own in a match (OaLayer::match_dialog). 0 for a
    /// screen drawn at 1× and fitted onto a place of its own size, as the
    /// in-match Settings is on a phone, whose points map by the place's size.
    int32_t scale_percent{};
    /// The radius of the place's rounded corners, in the screen's points:
    /// the layer leaves what lies outside the rounded shape clear, so that
    /// what is behind the screen shows there, and draws the window's edge
    /// round each corner (round_layer_corners). Metrics::window_radius for a
    /// window that moves (window_placement::moves), Metrics::dialog_radius
    /// for a notice, a question and a work dialog, at the class each is laid
    /// out at; 0, square, for every other screen and for one that fills its
    /// whole room.
    int32_t corner_radius{};
};

/// A screen of the OA layer as drawn, before it is laid over what lies under
/// it (OaLayer::draw_screen).
///
/// Its colours are straight: not multiplied by their opacity. A pixel is
/// laid over what lies under it as (colour × opacity + under × (255 −
/// opacity)) / 255, src over; an opaque pixel replaces what lies under it,
/// and a clear one leaves it as it is.
struct LayerDrawing {
    /// The colours, row by row: points_width × points_height points times
    /// the scale.
    renderer::Surface picture{};
    /// Each pixel's opacity, one byte a pixel of picture, row by row: 0
    /// clear, 255 opaque. Empty for an opaque screen, every pixel of which
    /// is opaque.
    std::vector<uint8_t> opacity{};
};

/// What a screen of the OA layer did with an input, or with its frame's tick.
enum class LayerAnswer : uint8_t {
    /// The screen does not take it. Under a modal screen it counts as none;
    /// a screen that is not modal lets it go on to the screens below it and
    /// then to what lies under the layer.
    pass,
    none,   ///< taken; nothing the screen draws changed
    redraw, ///< taken; what the screen draws changed (its revision changed)
    close,  ///< taken, and the screen closes: the layer takes it off
};

/// One screen of the OA layer: a modal dialog such as Settings, a notice, a
/// card, a button. Every place and point is in the screen's own points, from
/// its top left corner; the layer maps them to where it shows
/// (LayerPlacement). No call of it names an SDL type.
class LayerScreen {
  public:

    LayerScreen() = default;
    LayerScreen(const LayerScreen&) = delete;
    LayerScreen& operator=(const LayerScreen&) = delete;

    /// Lets the screen go.
    virtual ~LayerScreen() = default;

    /// Returns the screen's name, by which the layer finds it.
    ///
    /// @return a short lower-case word, such as "settings"
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Returns where the screen shows.
    ///
    /// @param view what the screen is placed on
    /// @return its place; an empty shown rectangle while it does not show
    [[nodiscard]] virtual LayerPlacement placement(const LayerView& view) const = 0;

    /// Tells whether the screen takes every input while it shows, nothing
    /// under it seeing any.
    ///
    /// @return true for a modal screen
    [[nodiscard]] virtual bool modal() const = 0;

    /// Tells whether what lies under the screen darkens while it shows. A
    /// screen that is not opaque never darkens what lies under it, whatever
    /// it answers here.
    ///
    /// @return true for a backdrop
    [[nodiscard]] virtual bool backdrop() const = 0;

    /// Tells whether the screen covers every pixel of its place. One that
    /// does not, such as a card beside a badge with its arrow outside the
    /// card, is drawn on a clear surface each time, and what it draws is laid
    /// over what lies under it, the picture or the screens below, by each
    /// pixel's opacity (OaLayer::draw_screen); what it leaves clear shows
    /// what lies under it unchanged.
    ///
    /// @return true for a screen that covers its place, the default
    [[nodiscard]] virtual bool opaque() const { return true; }

    /// Draws the screen from its own top left corner at the canvas's scale,
    /// through the kit's draw functions on whichever form the canvas has: an
    /// opaque screen every pixel of its points_width × points_height points,
    /// over black; one that is not opaque only what it shows, leaving the
    /// rest as it finds it. The layer may draw a screen that is not opaque
    /// more than once for one picture.
    ///
    /// @param canvas where it draws: a picture of the screen's size at the
    ///     scale, crisp at a whole scale and smooth at 150%, the dialog's
    ///     fonts and the Open Annihilation icon
    virtual void draw(const oa::ui::kit::Canvas& canvas) const = 0;

    /// Lays the screen out for a view: Settings, a notice and a question take
    /// the view's size class on the front end. The layer calls it before it
    /// places, draws or hands input to the screen, and whenever the view's
    /// class may have changed. A screen of one layout ignores it.
    virtual void lay_out(const LayerView& /*view*/) {}

    /// Takes a pointer's move, press or release.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left; 0 for a move
    /// @param at the pointer, in the screen's points; it may lie outside the screen
    /// @param finger_reach how far a finger's press may lie from a control,
    ///     in the screen's points; 0 for a mouse
    /// @return what the screen did
    virtual LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, oa::ui::kit::Point at, int32_t finger_reach) = 0;

    /// Takes a pointer the same way pointer does, and how many presses landed
    /// as one. A screen that does not care drops the count. The layer calls
    /// this, not pointer, so one screen can tell a double-click.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left; 0 for a move
    /// @param at the pointer, in the screen's points; it may lie outside the screen
    /// @param finger_reach how far a finger's press may lie from a control,
    ///     in the screen's points; 0 for a mouse
    /// @param clicks how many presses landed as one; 1 for a single click
    /// @return what the screen did
    virtual LayerAnswer pointer_with_clicks(
        ScreenInputKind kind,
        uint8_t button,
        oa::ui::kit::Point at,
        int32_t finger_reach,
        uint8_t clicks
    ) {
        static_cast<void>(clicks);
        return pointer(kind, button, at, finger_reach);
    }

    /// Takes a key's press.
    ///
    /// @param pressed the key, as the kit names it (layer_key)
    /// @param sdl_key the key's SDL keycode
    /// @return what the screen did
    virtual LayerAnswer key(oa::ui::kit::Key pressed, uint32_t sdl_key) = 0;

    /// Takes a key's press before the layer maps it to the kit's keys: a
    /// screen's own shortcuts, such as the Library's Ctrl+F. The layer hands
    /// the press to key only when this answers pass.
    ///
    /// @return what the screen did; pass, as every screen without shortcuts answers
    virtual LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) {
        return LayerAnswer::pass;
    }

    /// Takes a key's release. The layer hands a screen every release it
    /// sees while the screen takes input, as the keys it pressed were.
    ///
    /// @return what the screen did; pass, as every screen that does not
    ///     follow a key's release answers
    virtual LayerAnswer key_up(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) {
        return LayerAnswer::pass;
    }

    /// Takes a gamepad action before it becomes a key, a click or anything
    /// else. The layer offers each action to the top screen, and a screen
    /// that answers other than pass took it, so it becomes no key. The
    /// argument is the action, as the gamepad controls name it.
    ///
    /// @return what the screen did; pass, as every screen without the hook answers
    virtual LayerAnswer pad_action(oa::ui::pad_controls::Action /*action*/) {
        return LayerAnswer::pass;
    }

    /// Takes the right stick's motion in a menu, in notches of the wheel,
    /// before it becomes a turn of the wheel. The argument is that turn;
    /// positive is towards the top.
    ///
    /// @return what the screen did; pass, as every screen without the hook answers
    virtual LayerAnswer pad_stick(float /*notches*/) { return LayerAnswer::pass; }

    /// Takes a turn of the wheel.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned; positive away from the player
    /// @return what the screen did
    virtual LayerAnswer wheel(oa::ui::kit::Point at, float notches) = 0;

    /// Takes text the player typed into the focused text field.
    ///
    /// @return what the screen did; pass unless the screen has a text field
    virtual LayerAnswer text(std::string_view /*utf8*/) { return LayerAnswer::pass; }

    /// Returns the focused text field, over which the system's text input
    /// stays started while the screen takes input.
    ///
    /// @return the field's rectangle, in the screen's points; none without one
    [[nodiscard]] virtual std::optional<oa::ui::kit::Rect> text_field() const {
        return std::nullopt;
    }

    /// Returns what the screen draws and where its controls are, at its own
    /// points, as automation lists them.
    ///
    /// @return the display list
    [[nodiscard]] virtual oa::ui::kit::DisplayList display_list() const = 0;

    /// Returns the screen's hover, press and focus, as automation lists them.
    ///
    /// @return the interaction
    [[nodiscard]] virtual oa::ui::kit::Interaction interaction() const = 0;

    /// Brings the screen up to date once a frame. A screen waiting in the
    /// layer's queue (OaLayer::show_when_free) is ticked too, and leaves the
    /// queue when it answers close.
    ///
    /// @return close when the screen closes; otherwise none or redraw
    virtual LayerAnswer tick() = 0;

    /// Returns a number that changes whenever what the screen draws changes.
    ///
    /// @return the revision
    [[nodiscard]] virtual uint64_t revision() const = 0;

    /// Tells whether what the screen draws changes only when its revision
    /// does, its animations and timed content among it, so that the front
    /// end keeps the screen's drawing while its revision, its place, its
    /// scale, its darkening and the fonts, language and text style it is
    /// drawn in hold. A screen that cannot promise it is drawn again each
    /// frame.
    ///
    /// @return false, the default; true for a screen that keeps to it
    [[nodiscard]] virtual bool revision_covers_drawing() const { return false; }

    /// Returns a number no other screen made in this run has, by which the
    /// layer tells a screen's kept drawing from another's.
    ///
    /// @return the screen's number, 1 or more
    [[nodiscard]] uint64_t identity() const noexcept { return identity_; }

    /// Tells the screen it leaves the layer, whatever took it off. A screen
    /// whose own model is still open closes it here, as its Cancel would.
    ///
    /// @param by_key a key's press closed it; that key is latched
    virtual void close(bool by_key) = 0;

  private:

    /// Returns the next screen's number.
    ///
    /// @return a number no screen made before has
    [[nodiscard]] static uint64_t next_identity() noexcept;

    uint64_t identity_{next_identity()}; ///< the screen's number
};

/// Maps a point to a screen's own points by its placement: for a screen
/// drawn at a scale as large as its place, on the front end and the in-match
/// Settings, floor((x - left) × 100 / scale_percent), the drawing's own
/// scale, 150% included; for a screen fitted onto a place of its own size
/// floor((x - shown.x) × points_width / shown.width). The same down.
///
/// @param placement where the screen shows
/// @param x the point's column, in window pixels
/// @param y the point's row
/// @return the point in the screen's points; it may lie outside the screen
[[nodiscard]] oa::ui::kit::Point
layer_point(const LayerPlacement& placement, float x, float y) noexcept;

/// Returns where a screen of a size shows centred in a view's room at the
/// view's scale: its size the points at the scale as the canvas's smooth
/// form rounds them (kit::to_canvas, width × scale at a whole scale), and
/// left = room's left + (room's width - that width) / 2, rounded down, and
/// the same down. The placement keeps the view's scale.
///
/// @param view what the screen is placed on
/// @param width_points the screen's width, in points
/// @param height_points the screen's height, in points
/// @return its place, in window pixels
[[nodiscard]] LayerPlacement
centred_placement(const LayerView& view, int32_t width_points, int32_t height_points) noexcept;

/// Draws on a canvas of a screen's size at a scale, as the OA layer draws
/// its screens: at a whole scale on the kit canvas's crisp form, the
/// placement's scale that whole number; at any other scale on its smooth
/// form, the modern fonts drawing its text, every pixel starting opaque at
/// the ground's grey. The picture is the points at the scale
/// (kit::to_canvas).
///
/// @param points_width the screen's width, in points
/// @param points_height the screen's height, in points
/// @param scale_percent the scale, in percent: 100, 150, 200 and so on
/// @param fonts the game's fonts the screens lay out and, crisp, draw in
/// @param modern_fonts the modern fonts the smooth form draws text in; null
///     draws no text there
/// @param icon the Open Annihilation icon; empty draws the OA mark
/// @param draw draws the screen on the canvas it is given
/// @param ground the grey every pixel starts at: 0, black, or 255, white,
///     over which a screen that is not opaque is drawn a second time
/// @return the picture, opaque
[[nodiscard]] renderer::Surface layer_drawing(
    int32_t points_width,
    int32_t points_height,
    int32_t scale_percent,
    const oa::ui::kit::Fonts* fonts,
    oa::platform::text_font::FontStack* modern_fonts,
    const oa::ui::frontend_renderer::RgbaPicture& icon,
    const std::function<void(const oa::ui::kit::Canvas&)>& draw,
    uint8_t ground = 0
);

/// Draws on a clear canvas of a screen's size at a scale, as the OA layer
/// draws a screen that is not opaque: it is drawn twice (layer_drawing),
/// over black and over white, and each pixel's opacity is what it leaves of
/// the white, 255 less the most any colour channel differs between the two;
/// its colour is the one over black divided by that opacity, so that the
/// colours stay straight. What it fills is opaque and keeps its colour
/// exactly, what it leaves alone is clear, and what it blends over takes the
/// blend's opacity.
///
/// @param points_width the width, in points
/// @param points_height the height, in points
/// @param scale_percent the scale, in percent: 100, 150, 200 and so on
/// @param fonts the game's fonts it lays out and, crisp, draws in
/// @param modern_fonts the modern fonts the smooth form draws text in; null
///     draws no text there
/// @param icon the Open Annihilation icon; empty draws the OA mark
/// @param draw draws on the canvas it is given
/// @return the drawing, its colours straight, with an opacity for each pixel
[[nodiscard]] LayerDrawing clear_drawing(
    int32_t points_width,
    int32_t points_height,
    int32_t scale_percent,
    const oa::ui::kit::Fonts* fonts,
    oa::platform::text_font::FontStack* modern_fonts,
    const oa::ui::frontend_renderer::RgbaPicture& icon,
    const std::function<void(const oa::ui::kit::Canvas&)>& draw
);

/// Rounds the corners of a screen's drawing as the layer shows it: in each
/// corner's square, what lies outside the rounded rectangle becomes clear,
/// a pixel the rounded edge cuts takes the share of it the shape covers, and
/// a ring a point thick inside the edge takes the window's edge colours, the
/// strong edge (kit::colour::edge_light) where the corner meets the top or
/// the left edge and the dark edge (kit::colour::edge_dark) where it meets
/// the bottom or the right, as kit::bevel_rect draws the straight edges. The
/// shape is the kit's (kit::corner_coverage): a step at 1×, a blended edge
/// at every other scale. A drawing with no opacity takes an opaque one
/// first. Nothing changes for a radius of 0 or less.
///
/// @param[in,out] drawing the screen's drawing, straight colours, as large
///     as its place
/// @param radius_points the corners' radius, in the screen's points
/// @param scale_percent the scale it is drawn at, in percent; 0 counts as 100
void round_layer_corners(LayerDrawing& drawing, int32_t radius_points, int32_t scale_percent);

/// A place in the window, in window pixels and their fractions.
struct WindowPosition {
    double x{}; ///< column
    double y{}; ///< row
};

/// Returns where an input's position lies in the window: on the front end,
/// where input arrives in the picture's pixels, through the picture's
/// rectangle (to the nearest 64th of a pixel, as the mapping back from the
/// picture leaves it); in a match as it is.
///
/// @param view what the screens are placed on
/// @param x the input's column, in the coordinates input arrives in
/// @param y the input's row
/// @return the position, in window pixels
[[nodiscard]] WindowPosition window_position(const LayerView& view, float x, float y) noexcept;

/// Tells whether a key's press pastes into a focused text field of the OA
/// layer: V with Ctrl, or on macOS with Cmd too.
///
/// @param sdl_key the SDL keycode
/// @param modifiers SDL_Keymod bits
/// @return true for the paste shortcut
[[nodiscard]] bool layer_paste(uint32_t sdl_key, uint16_t modifiers) noexcept;

/// Returns the key a press means to a screen of the OA layer: Enter, Escape,
/// the arrows, Space, Tab and Shift+Tab, Page Up and Page Down, Home and End,
/// Y and N, Backspace and Delete, and, outside a match, F1 as the
/// information key. In a match F1 is the game's own (unit information), so
/// it goes on to the game.
///
/// @param sdl_key the SDL keycode
/// @param modifiers SDL_Keymod bits
/// @param in_match the press comes in a match
/// @return the kit's key; nothing for a key no screen answers to
[[nodiscard]] std::optional<oa::ui::kit::Key>
layer_key(uint32_t sdl_key, uint16_t modifiers, bool in_match = false) noexcept;

/// Returns the kind the automation endpoint lists a control or a part of a
/// screen of the OA layer as: a button, a link and a row of buttons' button
/// are buttons; a switch, a strip and a row of buttons are labels, their
/// parts (a switch's halves, a strip's levels) check boxes; a drop-down is a
/// button and an open one's items check boxes; a slider and a scroll bar
/// are sliders; a tab and a list's row are check boxes; a text field and a
/// list are themselves; and an area is an area, but one neither enabled nor
/// focusable is text a journey reads, a label.
///
/// @param entry the control or part, as kit::automation_parts lists it
/// @return its kind
[[nodiscard]] AutomationControlKind
automation_kind(const oa::ui::kit::AutomationEntry& entry) noexcept;

/// The host of Open Annihilation's own screens over every screen of the
/// game (Runtime::oa_layer).
///
/// Input reaches it through one overlay at z 100 on every screen
/// (Runtime::register_oa_layer): the latched key first, then the top modal
/// screen that shows takes every event; with none, each screen that shows
/// sees it from the top, and the first that answers other than pass takes
/// it; in a match the in-game OA button then takes its own; what nobody
/// takes goes on to the overlays and screens under the layer.
///
/// A screen somebody asked for joins at once (push); one nobody asked for
/// at that moment, such as a notice or an install's question, waits in a
/// queue, first in, first out (show_when_free), so that two never show at
/// once. The first waiting screen is pushed while the layer has no modal
/// screen up, no frontend message box is up, and no match is being played.
/// A registered request joins that same queue and is never held back because
/// nobody is watching. A question a screen asks about its own content is
/// pushed over that screen and is not queued.
///
/// On the front end the screens are laid out at the size class of the
/// window's canvas and drawn at its whole scale (view), centred in the
/// window; each opaque modal screen with a backdrop darkens the frame
/// itself, the window round it and every screen under it (darken_front_end,
/// present), and the screens are copied into the window in its own pixels
/// (present), before the software cursor; without a renderer, and for
/// frame_without_cursor, they are composed into the frame instead
/// (compose_front_end). A screen that is not opaque (LayerScreen::opaque) is
/// laid over what lies under it by its pixels' opacity wherever it goes. In
/// a match the layer is one picture of the window's size: the in-game OA
/// button, the backdrop and the screens, drawn again when what it shows
/// changes (refresh_match); Settings is laid out and drawn there at the
/// class and scale match_dialog gives.
///
/// A window that moves (window_placement::moves) shows where the player put
/// it: its title, the window's header, drags it, snapped to the room's
/// centre and safe edges and kept on screen; a finger holds the title
/// first; a double-click on the title puts it back in the centre; Alt and an
/// arrow move the window that takes the keys, by 8 points, or 32 with Shift.
/// Each window keeps its place for each size class in the preferences
/// (window_placement::place_key), as an offset from its centre in points.
/// Notices and questions stay centred. A window's corners take its class's
/// window radius and a notice's, a question's and a work dialog's its
/// dialog radius (LayerPlacement::corner_radius).
///
/// While Settings' Move overlays is on, the keys overlay, the chat line and
/// the latch chips each show at the place the player gave them for the size
/// class in effect (overlay_shown), kept in the preferences
/// (overlay_placement::place_key); Settings' Arrange… opens Arrange
/// overlays (open_arrange_overlays), a picture of the match where they are
/// placed. While it is off they show where they lay themselves out.
class OaLayer {
  public:

    /// The in-game OA Settings button's left column in the in-game menu's
    /// column, in source pixels: Resume's left edge.
    static constexpr int32_t match_button_source_x = 14;
    /// The in-game OA Settings button's top row in the in-game menu's
    /// column, in source pixels: under Resume, clear of its frame.
    static constexpr int32_t match_button_source_y = 455;
    /// The in-game OA Settings button's width, in source pixels: Resume's.
    static constexpr int32_t match_button_source_width = 96;
    /// The in-game OA Settings button's height, in source pixels: what the
    /// column leaves under Resume.
    static constexpr int32_t match_button_source_height = 24;

    /// Makes an empty layer.
    ///
    /// @param runtime the runtime it hosts screens for
    explicit OaLayer(Runtime& runtime) noexcept;

    /// Answers each registered prompt still queued or shown with −1, lets the
    /// screens and those waiting go without closing the others, and destroys
    /// the textures. Calling drop_pending_prompts first, and again here, is safe.
    ~OaLayer();

    OaLayer(const OaLayer&) = delete;
    OaLayer& operator=(const OaLayer&) = delete;

    /// Puts a screen on top of the stack at once: a screen somebody asked
    /// for, such as Settings, or a question a screen asks of its own over
    /// that screen.
    ///
    /// @param screen the screen; null is ignored
    void push(std::unique_ptr<LayerScreen> screen);

    /// Shows a screen nobody asked for at this moment, such as a notice or an
    /// install's question, once the layer is free: the screens wait first in,
    /// first out, and the first is pushed while no modal screen shows, no
    /// frontend message box is up and no match is being played, at once,
    /// after an input or a tick, or when a screen is closed (close); one at
    /// a time while each shows.
    ///
    /// @param screen the screen; null is ignored
    void show_when_free(std::unique_ptr<LayerScreen> screen);

    /// Takes a screen off the stack, or out of the queue, telling it it
    /// closes, and pushes the next waiting screen if the layer is then free.
    /// A screen asked while it takes an input or its tick leaves once that
    /// is done, as if it had answered close.
    ///
    /// @param screen the screen; one the layer does not hold is ignored
    void close(const LayerScreen* screen);

    /// Takes the top screen off, telling it it closes.
    void close_top();

    /// Takes off, from the top, every screen above the one of a name; with no
    /// such screen, nothing.
    ///
    /// @param name the screen's name
    void close_above(std::string_view name);

    /// Brings the topmost screen of a name back to the top: every screen above
    /// it is closed (close), a screen that is taking an input or its tick once
    /// that is done. With no such screen, nothing.
    ///
    /// @param name the screen's name
    void return_to(std::string_view name);

    /// Returns the top screen.
    ///
    /// @return the screen; null while the stack is empty
    [[nodiscard]] LayerScreen* top() const noexcept;

    /// Returns the topmost screen of a name on the stack.
    ///
    /// @param name the screen's name
    /// @return the screen; null when none has the name
    [[nodiscard]] LayerScreen* find(std::string_view name) const noexcept;

    /// Returns the first screen of a name that waits in the queue.
    ///
    /// @param name the screen's name
    /// @return the screen; null when none waits
    [[nodiscard]] LayerScreen* waiting(std::string_view name) const noexcept;

    /// Tells whether a screen is on the stack.
    ///
    /// @param screen the screen
    /// @return true while it is
    [[nodiscard]] bool holds(const LayerScreen* screen) const noexcept;

    /// Returns the fonts the layer's screens are measured and drawn in: the
    /// settings dialog's (Runtime::engine_settings_fonts), at the size class
    /// the screens are laid out at now (kit::Fonts::size_class), so that in
    /// the modern look each class's text takes that class's steps on the
    /// type scale. The class is the view's when the layer last laid its
    /// screens out (lay_out_screens): on the front end the class Settings,
    /// the Library and the other screens take, and in a match Compact, the
    /// class of the overlays and the notices there. The in-match Settings
    /// and the gallery, which a match lays out at a class of their own, are
    /// measured and drawn at Compact's type.
    ///
    /// @return the fonts; null when they cannot be loaded
    [[nodiscard]] const oa::ui::kit::Fonts* screen_fonts() const;

    /// Returns what the screens are placed on now: the window over the front
    /// end's picture, with the scale and size class of the window's canvas
    /// (its size in pixels, the window points it holds and, while the game
    /// has touch controls, its safe insets) at the Interface size in effect,
    /// or the match's canvas. Without a renderer the canvas is the picture
    /// itself.
    ///
    /// @return the view
    [[nodiscard]] LayerView view() const;

    /// Returns the whole scale the front end's screens are drawn at now.
    ///
    /// @return window pixels a point; 1 at 150% and in a match
    [[nodiscard]] int32_t screen_scale() const { return view().scale; }

    /// Returns the scale the front end's screens are drawn at now, in percent.
    ///
    /// @return 100, 150, 200 and so on; 100 in a match
    [[nodiscard]] int32_t screen_scale_percent() const { return view().scale_percent; }

    /// Returns the modern fonts the screens draw their text in at 150%: the
    /// bundled fonts beside the game, opened the first time they are asked
    /// for, with the faces the language shown adds (Runtime::follow_language_fonts).
    ///
    /// @return the fonts; null when they cannot be opened, and 150% is then
    ///     not in effect
    [[nodiscard]] oa::platform::text_font::FontStack* smooth_fonts() const;

    /// Returns the size class the front end's screens are laid out at now.
    ///
    /// @return the class; Compact in a match
    [[nodiscard]] oa::ui::kit::SizeClass screen_class() const { return view().frame.size_class; }

    /// Returns the Window size in effect, as the preferences hold it
    /// (window_placement::window_size_key).
    ///
    /// @return the size; Auto when none is chosen
    [[nodiscard]] window_placement::WindowSize window_size() const;

    /// Chooses the Window size, which overrides the size class OA's windows
    /// are laid out at, keeps it in the preferences and writes them, and
    /// lays every screen out again for it.
    ///
    /// @param size the size; Auto erases the preference
    void set_window_size(window_placement::WindowSize size);

    /// Puts every window back in its centre: forgets each window's place at
    /// every size class (window_placement::forget_places), writes the
    /// preferences, and ends a drag under way.
    ///
    /// @return true when a window had a place to forget
    bool reset_window_places();

    /// Returns the offset from its centre at which a window that moves shows
    /// now, at the class it is laid out at: a drag's while one is under way,
    /// else the one remembered.
    ///
    /// @param name the window's screen name
    /// @return the offset, in points; 0, 0 when it shows centred
    [[nodiscard]] oa::ui::kit::Point window_offset(std::string_view name) const;

    /// Where OA's overlays over a match keep to, and what their places are
    /// kept at (overlay_room).
    struct OverlayRoom {
        /// The match's canvas: 0, 0 and its width and height, in canvas pixels.
        oa::ui::display_layout::Rect window{};
        /// The overlays' area (Runtime::overlay_area): the battlefield, or
        /// the part of it the touch controls leave clear, in canvas pixels.
        oa::ui::display_layout::Rect area{};
        /// The scale the places are measured at, in percent: the in-match
        /// windows' (match_dialog), or 100 where that is fitted.
        int32_t scale_percent{100};
        /// The size class the places are kept for: the in-match windows'.
        oa::ui::kit::SizeClass size_class{oa::ui::kit::SizeClass::compact};
        /// Returns the area in points at the scale, from its own top left
        /// corner.
        ///
        /// @return 0, 0 and the area's width and height in points, rounded down
        [[nodiscard]] oa::ui::kit::Rect points() const noexcept;
    };

    /// One overlay as Arrange overlays shows it.
    struct ArrangedOverlay {
        std::string_view name{}; ///< the overlay's name (overlay_placement::overlays)
        /// Where it lays itself out with no place of its own, in canvas
        /// pixels: as large as it is now, or for one that shows nothing now
        /// (no chip on, no line typed) as large as it would be.
        oa::ui::display_layout::Rect unplaced{};
    };

    /// Returns where OA's overlays keep to now: in a match, the overlays'
    /// area at the in-match windows' scale and size class.
    ///
    /// @return the room; nothing off a match or while the area is empty
    [[nodiscard]] std::optional<OverlayRoom> overlay_room() const;

    /// Returns the room Arrange overlays pictures: in a match, overlay_room;
    /// on the front end, the battlefield of a match laid out at the window's
    /// canvas (Runtime::match_canvas_point), at the class and scale the
    /// in-match windows take there.
    ///
    /// @return the room; nothing while the window or the area is empty
    [[nodiscard]] std::optional<OverlayRoom> arranging_room() const;

    /// Tells whether Settings' Move overlays is on: the overlays show at the
    /// places the player gave them (overlay_shown).
    ///
    /// @return true while it is on
    [[nodiscard]] bool overlays_move() const;

    /// Returns where an overlay shows over the match: while Move overlays is
    /// on (overlays_move), where it lays itself out, moved to the place the
    /// player gave it at the size class in effect
    /// (overlay_placement::rect_at), as large as it is and kept in the
    /// overlays' area. With no place, or while Move overlays is off, it shows
    /// where it lays itself out, pixel for pixel.
    ///
    /// @param overlay the overlay's name (overlay_placement::overlays)
    /// @param unplaced where it lays itself out, in canvas pixels
    /// @return where it shows, in canvas pixels
    [[nodiscard]] oa::ui::display_layout::Rect
    overlay_shown(std::string_view overlay, const oa::ui::display_layout::Rect& unplaced) const;

    /// Returns the overlays Arrange overlays moves and where each lays
    /// itself out over the match now (ArrangedOverlay).
    ///
    /// @return the keys overlay, the chat line and the latch chips; empty off a match
    [[nodiscard]] std::vector<ArrangedOverlay> arranged_overlays() const;

    /// Returns the overlays Arrange overlays moves and where each lays
    /// itself out in a room: as the match shows them now, or with Arrange
    /// overlays' samples (the keys overlay's short list, keys_overlay_sample,
    /// and a latched Queue's chip).
    ///
    /// @param room the room
    /// @param samples the samples' sizes, else the match's overlays'
    /// @return the keys overlay, the chat line and the latch chips, in canvas pixels
    [[nodiscard]] std::vector<ArrangedOverlay>
    arranged_overlays_in(const OverlayRoom& room, bool samples) const;

    /// Lays out Arrange overlays' keys overlay: the first few keys of a match
    /// being played, laid out beside the side column of a room as the match
    /// lays the panel out.
    ///
    /// @param room the room
    /// @return the panel, its position in points from the overlays' area's
    ///     top left corner; empty without fonts
    [[nodiscard]] oa::ui::key_editor::OverlayLayout
    keys_overlay_sample(const OverlayRoom& room) const;

    /// Returns where the keys overlay lays itself out over the match now,
    /// whether or not it is open: its panel as it would be laid out at the
    /// in-match windows' class and scale, before any place of the player's.
    /// [runtime_keys_overlay.cpp]
    ///
    /// @return the panel, in canvas pixels; empty off a match or without fonts
    [[nodiscard]] oa::ui::display_layout::Rect keys_overlay_unplaced() const;

    /// Returns an overlay's place at a size class.
    ///
    /// @param overlay the overlay's name
    /// @param size_class the class
    /// @return the place; nothing while it has none
    [[nodiscard]] std::optional<overlay_placement::OverlayPlace>
    overlay_place(std::string_view overlay, oa::ui::kit::SizeClass size_class) const;

    /// Keeps an overlay's place at a size class, writes the preferences and
    /// lays the screens out again for it.
    ///
    /// @param overlay the overlay's name
    /// @param size_class the class
    /// @param place the place
    /// @return true when the place changed
    bool place_overlay(
        std::string_view overlay,
        oa::ui::kit::SizeClass size_class,
        const overlay_placement::OverlayPlace& place
    );

    /// Forgets an overlay's place at a size class, so that it shows where it
    /// lays itself out there again, and writes the preferences.
    ///
    /// @param overlay the overlay's name
    /// @param size_class the class
    /// @return true when it had a place
    bool forget_overlay_place(std::string_view overlay, oa::ui::kit::SizeClass size_class);

    /// Puts every overlay back where it lays itself out: forgets every
    /// overlay's place at every size class (overlay_placement::forget_places)
    /// and writes the preferences.
    ///
    /// @return true when an overlay had a place to forget
    bool reset_overlay_places();

    /// Returns every overlay's place at every size class, as the preferences
    /// hold them (the keys that start with overlay_placement::place_key_start).
    ///
    /// @return the keys and their values
    [[nodiscard]] std::map<std::string, std::string> kept_overlay_places() const;

    /// Puts every overlay's place back as kept_overlay_places returned them:
    /// forgets the places there are now, keeps those, and writes the
    /// preferences.
    ///
    /// @param kept the places
    void restore_overlay_places(const std::map<std::string, std::string>& kept);

    /// Opens Arrange overlays over Settings (Accessibility's Arrange…):
    /// modal, as large as the window, a picture of the match in the kit's
    /// look (arranging_room) with the overlays' samples, a handle on each to
    /// drag on the grid, and a card with Reset, Cancel and Done
    /// (oa_layer_arrange.cpp).
    ///
    /// @return true when it opened; false while it is open, without fonts or
    ///     with no room to picture
    bool open_arrange_overlays();

    /// Returns where the topmost screen of a name shows now, moved where the
    /// player put it (shown_placement).
    ///
    /// @param name the screen's name
    /// @return its place, in window pixels; empty when no such screen shows
    [[nodiscard]] LayerPlacement placement_of(std::string_view name) const;

    /// Tells whether a screen shows on the front end or in a match.
    ///
    /// @param match true for a match, false for the front end
    /// @return true while a screen shows there
    [[nodiscard]] bool shows(bool match) const;

    /// Tells whether a modal screen shows on the front end or in a match, so
    /// that nothing under the layer sees input.
    ///
    /// @param match true for a match, false for the front end
    /// @return true while a modal screen shows there
    [[nodiscard]] bool modal_shown(bool match) const;

    /// Takes one input, as the layer's overlay does.
    ///
    /// @param input the input, in the picture's pixels on the front end and
    ///     the window's in a match
    /// @return true when the layer took it
    bool take_input(const ScreenInput& input);

    /// Offers a gamepad action to the screens that show before it becomes a
    /// key or a click: the top modal screen alone when one shows, otherwise
    /// each from the top until one answers other than pass. A screen that
    /// answers other than pass took it.
    ///
    /// @param action the action
    /// @return true when a screen took it
    bool take_pad_action(oa::ui::pad_controls::Action action);

    /// Offers the right stick's motion in a menu, in notches of the wheel,
    /// before it becomes a turn of the wheel, to the screens that show as
    /// take_pad_action offers an action.
    ///
    /// @param notches the turn; positive towards the top
    /// @return true when a screen took it
    bool take_pad_stick(float notches);

    /// Carries out a screen's answer: close takes it off, latching the key
    /// whose press closed it. take_input does this for its own events; a
    /// check that hands a screen an event of its own does it here.
    ///
    /// @param screen the screen, on the layer
    /// @param answer what it answered
    /// @param key_down the key whose press it answered; 0 for anything else
    void take_answer(LayerScreen& screen, LayerAnswer answer, uint32_t key_down);

    /// Ticks every screen once a frame, taking off those that close, and
    /// keeps the system's text input over the focused text field.
    void tick();

    /// Starts the system's text input again over the focused text field the
    /// layer keeps it started over, as Space or a pad's A on a field asks, so
    /// that an on-screen keyboard the player put away shows again. Without
    /// such a field it does nothing: the field the input lands on starts it
    /// once the input is taken.
    void restart_text_input();

    /// Returns the key that closed a screen until it is released.
    ///
    /// @return the SDL keycode; 0 for none
    [[nodiscard]] uint32_t latched_key() const noexcept { return latched_key_; }

    /// Latches a key as closing a screen does: its presses do nothing until
    /// it is released. A question whose answer to a key leaves it on the
    /// layer latches that key, so that the key held answers nothing more.
    ///
    /// @param sdl_key the SDL keycode
    void latch_key(uint32_t sdl_key) noexcept { latched_key_ = sdl_key; }

    /// Forgets the latched key, as its release would.
    void forget_latched_key() noexcept { latched_key_ = 0; }

    /// Draws the match's layer again at its next refresh: for a change made
    /// to a screen's model from outside its events.
    void redraw() noexcept { ++revision_; }

    /// Lays every screen out again for the size in effect.
    ///
    /// A size chosen in Settings applies before the next input, so the open
    /// dialog and every OA screen are drawn at it on this frame. The layer
    /// also lays them out as it takes input and as it ticks.
    void lay_out_for_size();

    /// Asks for the window's read-back (the frame a check captures as it is
    /// presented) to hold the whole window on the front end, the letterbox
    /// round the picture with it, rather than the picture's area alone: what
    /// a check compares the screens and their backdrop with, as they are
    /// drawn in the window's own pixels. Off, the read-back is the picture's.
    ///
    /// @param whole true for the whole window
    void read_whole_window(bool whole) noexcept { whole_window_ = whole; }

    /// Tells whether the window's read-back holds the whole window on the
    /// front end (read_whole_window).
    ///
    /// @return true for the whole window
    [[nodiscard]] bool reads_whole_window() const noexcept { return whole_window_; }

    /// Darkens the front end's frame under the modal screens with a
    /// backdrop: the frame blended with the backdrop's colour at the menu's
    /// opacity once for each that shows.
    ///
    /// @param[in,out] frame the composed frame
    void darken_front_end(renderer::Surface& frame) const;

    /// Draws each screen that shows on the front end into the frame, at its
    /// place mapped through the picture's rectangle, darkened once for each
    /// modal screen with a backdrop that shows above it: the picture the
    /// player sees, for checks and runs without a renderer. Each pixel of the
    /// frame takes the screen's pixel at its centre in the window; where the
    /// picture fills the window, as without a renderer and in a 640 by 480
    /// window, that is the screen drawn at 1× at its place.
    ///
    /// @param[in,out] frame the composed frame
    void compose_front_end(renderer::Surface& frame) const;

    /// Draws the match's layer again when what it shows changed: the
    /// in-game OA Settings button while the in-game menu's column shows,
    /// the backdrop
    /// under a modal screen, and the screens that show, each darkened as
    /// the button is once for each modal screen with a backdrop above it.
    /// Each screen is drawn at its placement's scale (Settings at
    /// match_dialog's, on the kit canvas's crisp form at a whole scale and
    /// its smooth form at 150%) as large as its place and copied into the
    /// layer pixel for pixel; one with no scale of its own (the phone's
    /// fitted Settings) is drawn at 1× and fitted to its place (stamp).
    ///
    /// @return true when the layer shows anything
    bool refresh_match();

    /// Draws the match's layer over a composed match frame, at the display gamma.
    ///
    /// @param[in,out] frame the composed frame, at the window's size
    void compose_match(renderer::Surface& frame);

    /// Draws the layer into the window, in the window's own pixels
    /// (RenderState::use_window_pixels), before the software cursor: on the
    /// front end each OA badge in the battle room (the logo scaled into its
    /// CD rectangle, the rules dot crisp on it) and each screen drawn at the
    /// view's scale and copied to its place pixel for pixel, and, under a
    /// modal screen with a backdrop, the window outside the picture darkened
    /// as the picture is; in a match the match's layer, as the match's other
    /// layers are drawn.
    ///
    /// Throws PresentError when SDL cannot make the layer's texture.
    ///
    /// @param picture_area where the front end's picture lies in the window
    ///     (SDL_GetRenderLogicalPresentationRect); null in a match
    void present(const SDL_FRect* picture_area);

    /// Tells whether this frame's OA badges are drawn here, at the window's
    /// own pixels, rather than into the 640 by 480 frame.
    ///
    /// True when a renderer will present the front end and the logo can be
    /// drawn. False in a match, without a renderer, and when the logo cannot
    /// be read: the frame then keeps the badge it draws itself.
    ///
    /// @return true when present draws the badges
    [[nodiscard]] bool draws_presence_badges() noexcept;

    /// Returns the layer's texture, in tiles beyond the renderer's limit.
    ///
    /// @return the texture; none before the first present
    [[nodiscard]] const TiledTexture& texture() const noexcept { return texture_; }

    /// Destroys the layer's texture; the next present makes it again.
    void destroy_textures() noexcept;

    /// Appends what the layer shows to the automation endpoint's controls
    /// (docs/automation.md, "OA's own screens"): the OA button while it
    /// shows, named oa.button (in the in-game menu the OA Settings button,
    /// its text its words), and on the main menu the Settings button beside
    /// it, named oa.settings-button, each with no dialog and enabled while no
    /// modal screen shows; then each screen that shows, from the
    /// top down to the first modal one, each control and part of its display
    /// list (kit::automation_parts) named oa. and its name, of the kind
    /// automation_kind gives, its dialog oa. and the screen's name.
    ///
    /// Each carries its exact rectangle in the window's pixels, as present
    /// draws it there (AutomationControl::window_pixels), and as its canvas
    /// rectangle that rectangle's corners mapped back through the canvas's
    /// placement, the top left rounded down and the bottom right up; without
    /// a renderer the two are the same.
    ///
    /// @param[in,out] controls the list they are appended to
    void automation_controls(std::vector<AutomationControl>& controls) const;

    /// Returns how far the nearest pressable control of a screen that shows
    /// lies from a point, in the same canvas pixels a finger's claim uses.
    ///
    /// A control is pressable when it is enabled, shown and has an area. A
    /// point inside one is no distance. A modal screen that shows answers
    /// for its own controls only. A screen that does not show answers
    /// nothing. Nothing farther than `radius` is answered.
    ///
    /// @param x canvas pixels
    /// @param y canvas pixels
    /// @param radius how far a control may lie, in canvas pixels
    /// @return that distance, or nothing when none is within radius
    [[nodiscard]] std::optional<float> nearest_control(float x, float y, float radius) const;

    /// The Library button and the Settings button beside it, in the window's
    /// pixels, mapped the way automation lists them. During a match the
    /// canvas positions are mapped the same way, even when the main menu's
    /// buttons are not drawn.
    struct MenuButtonWindows {
        oa::ui::display_layout::Rect library{};  ///< the Library button
        oa::ui::display_layout::Rect settings{}; ///< the Settings button
    };

    /// Returns both OA buttons in the window's pixels.
    ///
    /// @return the buttons
    [[nodiscard]] MenuButtonWindows menu_button_windows() const;

    /// Returns how many times the main menu's picture is darkened: once for
    /// each opaque modal screen with a backdrop that shows. A match is not
    /// darkened.
    ///
    /// @return the count
    [[nodiscard]] uint32_t front_darkenings() const;

    /// Drops the drawings the front end keeps of the screens whose revision
    /// covers what they draw, so that the next present draws every screen
    /// again. A check that reads what a frame draws, as the text-fit check
    /// reads the texts the kit records while they are drawn, calls it before
    /// that frame.
    void forget_kept_drawings() noexcept;

    /// Returns the number of the drawing the front end keeps of a screen, as
    /// the last present left it, by which a check tells whether a present
    /// drew the screen again.
    ///
    /// @param screen the screen
    /// @return the drawing's number; 0 when none is kept
    [[nodiscard]] uint64_t kept_drawing(const LayerScreen& screen) const noexcept;

    /// The layer's overlay's input callback (OverlayDesc::event): take_input.
    ///
    /// @param context the screen context, with its input and the runtime as its host
    /// @return 1 when the layer took the input
    static int overlay_event(ScreenContext* context, void* /*state*/);

    /// The layer's overlay's frame callback (OverlayDesc::tick): tick.
    ///
    /// @param context the screen context, with the runtime as its host
    static void overlay_tick(ScreenContext* context, void* /*state*/);

    /// The layer's overlay's drawing (OverlayDesc::draw), on the front end
    /// alone: darken_front_end, then, without a renderer or while the frame
    /// is drawn without the cursor (Runtime::frame_without_cursor),
    /// compose_front_end.
    ///
    /// @param context the screen context, with the frame and the runtime as its host
    static void overlay_draw(ScreenContext* context, void* /*state*/);

    /// Tells whether the in-game OA button shows: the in-game menu's column
    /// shows and the dialog's fonts are present.
    ///
    /// @param runtime the runtime
    /// @return true while the button shows
    [[nodiscard]] static bool match_button_shown(Runtime& runtime);

    /// Tells whether the match's screens and the OA button fit the window's
    /// safe area rather than the side column's scale: the touch controls are
    /// on and the window is phone class, whose side column is no longer
    /// drawn at a scale of its own.
    ///
    /// @param runtime the runtime
    /// @return true on a phone with touch controls
    [[nodiscard]] static bool match_fits_safe_area(const Runtime& runtime);

    /// Returns the window's safe area on the match's canvas.
    ///
    /// @param match_layout the match's layout
    /// @return the canvas less its safe-area insets, in window pixels
    [[nodiscard]] static oa::ui::display_layout::Rect
    match_safe_area(const oa::ui::display_layout::MatchLayout& match_layout) noexcept;

    /// Returns where the OA Settings button shows on the match's canvas:
    /// under Resume, as wide as it from its left edge (match_button_source_x
    /// and the rest), in the side column at the column's own scale, which a side-column page
    /// taller than the window makes smaller than the bars' (so the button
    /// stays in the in-game menu at every window size); fitted to the safe
    /// area, where a placed region showing that part of the column puts it,
    /// else in the column as the 640x480 frame fitted to the safe area from
    /// its top left corner places it.
    ///
    /// @param match_layout the match's layout
    /// @param fit the button fits the safe area (match_fits_safe_area)
    /// @return the button's rectangle, in window pixels
    [[nodiscard]] static oa::ui::display_layout::Rect match_button_rect(
        const oa::ui::display_layout::MatchLayout& match_layout, bool fit = false
    ) noexcept;

    /// Draws the in-game OA Settings button as the match's layer shows it,
    /// in the in-game menu's own art as OA's parts in 3.1c's rooms are drawn
    /// (oa/ui/frontend_renderer/room_parts.hpp): the game's BUTTONS0 plate at
    /// the button's source size, the small engraved OA stamp at its left end
    /// and "OA Settings" in the menu's caption font (the label font, or cut,
    /// where that does not fit), sunk while pressed, darkened as the screen
    /// is under a modal screen.
    ///
    /// @param resources the in-game menu's art (Runtime::match_hud_); null
    ///     draws the game's plain button face
    /// @param look how the button looks: idle, hovered or pressed
    /// @param darkened a modal screen with a backdrop shows over the screen
    /// @return the face, at its source size, clear where no part of it is drawn
    [[nodiscard]] static LayerDrawing match_button_face(
        const oa::ui::frontend_renderer::ScreenResources* resources,
        oa::ui::engine_settings::ButtonLook look,
        bool darkened
    );

    /// Returns where the settings dialog shows unmoved on the match's
    /// canvas, and the size class and scale it is laid out and drawn at
    /// there (window_placement::match_window).
    ///
    /// The scale is the layer's on the match's canvas (kit::layer_viewport
    /// of its pixels, density and safe insets at the Interface size: its
    /// step, 150%, or for Auto the canvas height's, one step lower where
    /// that gives the class the Window size wants), and above the side
    /// column's scale (MatchLayout::scale, which HUD scaling caps) the
    /// largest whole step at or below that scale, and at least 1. The class
    /// comes from the points of the window's whole safe area at the scale,
    /// or the Window size held to what fits it: the dialog may cover the side
    /// column. It is centred in the safe area, or centred across in the area
    /// right of the column where that holds it. A 640 by 480 window is
    /// Compact at 1× at (144, 78), as before; 800 by 600 Regular at 1×;
    /// 1024 by 768, 1280 by 720, 1366 by 768 and 1280 by 800 Large at 1×;
    /// 1920 by 1080 Large at 1×, or at 1.5× while 150% is offered; 2560 by
    /// 1440 Large at 2×.
    ///
    /// Fitted to a phone's safe area, Compact is drawn at 1× and fitted to
    /// the largest scale that shows it whole there (min(safe width /
    /// dialog_width, safe height / dialog_height)), centred in it, with a
    /// scale of 0.
    ///
    /// @param match_layout the match's layout
    /// @param interface_percent the Interface size in effect, in percent
    ///     (LayerView::interface_percent): 0 for Auto
    /// @param fit the dialog fits the safe area (match_fits_safe_area)
    /// @param size the Window size in effect (LayerView::window_size)
    /// @return the dialog's rectangle, class and scale
    [[nodiscard]] static MatchDialogPlace match_dialog(
        const oa::ui::display_layout::MatchLayout& match_layout,
        int32_t interface_percent,
        bool fit = false,
        window_placement::WindowSize size = window_placement::WindowSize::automatic
    ) noexcept;

    /// Returns the frame the UI kit gallery takes in a match: the match's
    /// canvas less its safe insets, at the layer's scale on it
    /// (window_placement::match_viewport at Auto's Window size), and the
    /// size class of the canvas's points at that scale (kit::frame_of). It
    /// covers the whole canvas: a 640 by 480 window is Compact at 1×, 1280
    /// by 720 Large at 1× and 1920 by 1080 Large at 1× while 150% is not
    /// offered, and Large at 1.5× while it is.
    ///
    /// @param match_layout the match's layout
    /// @param interface_percent the Interface size in effect, in percent
    ///     (LayerView::interface_percent): 0 for Auto
    /// @return the frame, its area in points from the canvas's safe insets
    [[nodiscard]] static oa::ui::kit::Frame match_gallery(
        const oa::ui::display_layout::MatchLayout& match_layout, int32_t interface_percent
    ) noexcept;

    /// Opens the UI kit gallery on top of the stack, over the settings
    /// dialog that asked for it: modal, covering the whole window on the
    /// front end or in a match, whichever the layer shows over now
    /// (GalleryScreen).
    ///
    /// @return the gallery's screen, which the layer holds
    GalleryScreen& open_gallery();

    /// Returns the settings pages registered for this runtime.
    ///
    /// @return the registry; the caller keeps each page's rows alive
    [[nodiscard]] SettingsRegistry& registered_settings() noexcept { return settings_pages_; }

    /// Opens a kit screen the runtime's ScreenRegistry holds
    /// (kit_screen_register) by name on this layer: modal, over a backdrop,
    /// filling the front end's room, laid out from its descriptor and drawn
    /// with the kit. An unknown name opens nothing.
    ///
    /// @param name the screen's name
    /// @return true when the screen was opened
    bool open_registered_screen(std::string_view name);

    /// Queues a registered notice on this layer's one queue. Its word is
    /// "registered", so it does not take the engine notice's name.
    ///
    /// @param request the notice; its strings are copied
    /// @return an id that is never 0; 0 when the request is malformed
    uint32_t queue_notice(const NoticeRequest& request);

    /// Queues a registered prompt on this layer's one queue. Its word is
    /// "prompt". answered is called once, with the button pressed, or with
    /// −1 from drop_pending_prompts when the prompt is still queued or shown.
    ///
    /// @param request the prompt; its strings are copied
    /// @return an id that is never 0; 0 when the request is malformed
    uint32_t queue_prompt(const PromptRequest& request);

    /// Answers every registered prompt still queued or shown with −1, once,
    /// and takes it off. The runtime's end calls this. Calling it twice is safe.
    void drop_pending_prompts();

    /// Copies a surface into a rectangle of a layer of straight colours and
    /// opacity, each layer pixel taking the surface pixel it lands on
    /// (nearest): fully opaque, or, with the surface's opacity, laid over
    /// the layer's pixel (src over, LayerDrawing). The match's layer fits the
    /// OA button's face to its place so, and a screen drawn at 1× for a
    /// place of another size, as the phone's fitted Settings is; a screen
    /// drawn as large as its place is copied pixel for pixel instead.
    ///
    /// @param[in,out] rgba the layer, 4 bytes a pixel: red, green, blue and opacity
    /// @param width the layer's width
    /// @param height the layer's height
    /// @param source the surface
    /// @param rect where the surface lands, in layer pixels; clipped to the layer
    /// @param opacity each surface pixel's opacity (LayerDrawing::opacity);
    ///     empty for an opaque surface
    static void stamp(
        std::vector<uint8_t>& rgba,
        int32_t width,
        int32_t height,
        const renderer::Surface& source,
        const oa::ui::display_layout::Rect& rect,
        std::span<const uint8_t> opacity = {}
    );

    /// Checks, for --check-engine-settings, screens that are not opaque and
    /// the information key (oa_layer_check.cpp). On the main menu in a 640
    /// by 480 and a 2560 by 1440 window, such a screen changes no pixel of
    /// the window, nor of the frame without the cursor, but those it draws,
    /// alone, over an opaque screen and under one, and darkens nothing; the
    /// cursor shows above it; opaque screens draw as before; automation
    /// lists its controls; the cursor's square matches the one drawn in the
    /// picture with no screen open; F1
    /// reaches a screen that takes the information key, goes on past one
    /// that does not, and changes nothing in Settings. In a match F1 goes on
    /// to the game, and such a screen changes no pixel of the match's
    /// picture but those it draws. The match's layer lays a pixel over its
    /// own by its opacity, its colours straight.
    /// Settings, on a 640 by 480 main menu, moves 60 by 40 points by a drag
    /// on its title and keeps that place in the preferences, a double-click
    /// on the title centres it again, Alt and Right then Alt, Shift and Down
    /// move it 8 and 32 points, and reset_window_places centres it.
    ///
    /// Throws std::runtime_error naming the first thing that failed.
    void check_clear_screens();

  private:

    /// What the match's layer shows, at which size: it is drawn again only
    /// when this changes.
    struct MatchLook {
        int32_t width{};  ///< the layer's width, in window pixels
        int32_t height{}; ///< the layer's height, in window pixels
        double scale{};   ///< the side column's scale
        /// The OA button's rectangle, x, y, width and height, in window
        /// pixels: it moves with the safe area on a phone (fit_safe_area).
        std::array<int32_t, 4> button{};
        /// Each screen's rectangle, x, y, width and height, in window pixels,
        /// and its corners' radius, in its points.
        std::vector<int32_t> placed{};
        /// Each screen's revision, in the stack's order.
        std::vector<uint64_t> revisions{};
        bool button_shown{};   ///< the OA button shows
        bool darkened{};       ///< a modal screen with a backdrop shows over the darkened screen
        uint8_t button_look{}; ///< the button's oa::ui::engine_settings::ButtonLook
        uint64_t revision{};   ///< the layer's own revision when it was drawn
        bool operator==(const MatchLook&) const = default;
    };

    /// One screen of the front end as the window shows it.
    struct FrontPiece {
        /// The part of it the window shows, in window pixels: for a screen
        /// that is not opaque, only the part that holds what it drew.
        oa::ui::display_layout::Rect window{};
        oa::ui::display_layout::Rect shown{}; ///< where it shows, in window pixels
        /// Its drawing at the view's scale, darkened by the screens above it.
        std::shared_ptr<const LayerDrawing> drawn{};
        /// The number of its drawing (front_drawing_count_): two pieces with
        /// the same number hold the same drawing.
        uint64_t drawing{};
    };

    /// Everything but the screen itself that a front-end screen's drawing
    /// is drawn from.
    struct DrawingInputs {
        /// Its place's size, in window pixels: 0, 0, width and height. Where
        /// it shows does not change what it draws, so a window moved keeps
        /// its drawing.
        std::array<int32_t, 4> shown{};
        int32_t points_width{};     ///< its width, in its points
        int32_t points_height{};    ///< its height, in its points
        int32_t placed_percent{};   ///< its placement's scale, in percent
        int32_t scale_percent{};    ///< the scale it is drawn at, in percent
        uint32_t darkenings{};      ///< the modal screens with a backdrop above it
        int32_t corner_radius{};    ///< its place's corners' radius, in its points
        bool opaque{};              ///< it covers its place
        const void* fonts{};        ///< the game's fonts it is drawn in
        const void* modern_fonts{}; ///< the modern fonts it is drawn in; null for none
        const void* icon{};         ///< the icon's pixels
        std::size_t icon_bytes{};   ///< the icon's bytes
        uint32_t icon_width{};      ///< the icon's columns
        uint32_t icon_height{};     ///< the icon's rows
        const void* language{};     ///< the interface's language
        uint64_t languages{};       ///< the languages' registry generation
        uint64_t interface_words{}; ///< the interface's words' generation (interface_generation)
        bool modern_text{};         ///< game text is drawn in the modern fonts
        bool outline{};             ///< modern text is outlined
        bool shadow{};              ///< modern text casts a shadow
        bool background{};          ///< modern text lies on a shaded box
        int32_t text_size{};        ///< the modern text's size, in percent
        bool utf8{};                ///< game text may hold UTF-8
        /// Tells whether two sets of inputs are the same.
        ///
        /// @return true when every input is the same
        bool operator==(const DrawingInputs&) const = default;
    };

    /// A front-end screen's drawing, kept while what it was drawn from holds.
    struct KeptDrawing {
        uint64_t screen{};                           ///< the screen's identity
        uint64_t revision{};                         ///< its revision when it was drawn
        DrawingInputs inputs{};                      ///< what else it was drawn from
        std::shared_ptr<const LayerDrawing> drawn{}; ///< the drawing, darkened
        oa::ui::display_layout::Rect drawn_part{};   ///< the part that holds what it drew
        uint64_t drawing{};                          ///< the drawing's number
    };

    /// What the front end's part of the layer shows in the window.
    struct FrontLook {
        int32_t picture_width{};          ///< the picture's width, in its pixels
        int32_t picture_height{};         ///< the picture's height, in its pixels
        std::array<float, 4> area{};      ///< the picture's rectangle in the window
        std::vector<FrontPiece> pieces{}; ///< the screens that show, bottom first
    };

    /// Returns the top modal screen that shows.
    ///
    /// @param seen what the screens are placed on
    /// @return the screen; null when none shows
    [[nodiscard]] LayerScreen* top_modal(const LayerView& seen) const;

    /// Returns where a screen shows: its own placement, and for a window
    /// that moves that placement moved to the window's offset and kept on
    /// screen (window_placement::shown_at). Every place the layer draws,
    /// hands input to and lists for automation comes from here.
    ///
    /// @param screen the screen
    /// @param seen what the screens are placed on
    /// @return its place, in window pixels
    [[nodiscard]] LayerPlacement
    shown_placement(const LayerScreen& screen, const LayerView& seen) const;

    /// Returns the room a window that moves keeps to and where it shows
    /// unmoved, when it moves now.
    ///
    /// @param screen the screen
    /// @param seen what the screens are placed on
    /// @param own the screen's own placement
    /// @return the room; nothing for a screen that does not move now: not a
    ///     window that moves, not shown, or fitted to a place of its own size
    [[nodiscard]] std::optional<window_placement::WindowRoom>
    window_room(const LayerScreen& screen, const LayerView& seen, const LayerPlacement& own) const;

    /// Returns the size class a window's place is remembered for: the
    /// in-match Settings' class in a match, the view's on the front end.
    ///
    /// @param seen what the screens are placed on
    /// @return the class
    [[nodiscard]] oa::ui::kit::SizeClass window_class(const LayerView& seen) const;

    /// Takes the input that moves a window: a press on a window's title,
    /// which starts a drag or, double-clicked, puts the window back in its
    /// centre; the moves and release of that press; and Alt with an arrow
    /// for the top modal screen.
    ///
    /// @param seen what the screens are placed on
    /// @param input the input
    /// @return true when the input moved, or may yet move, a window
    bool take_window_move(const LayerView& seen, const ScreenInput& input);

    /// Remembers a window's offset at a class in the preferences, writing
    /// them when it changed, and draws the match's layer again.
    ///
    /// @param window the window's screen name
    /// @param size_class the class it is laid out at
    /// @param offset its offset from its centre, in points
    void remember_window(
        std::string_view window, oa::ui::kit::SizeClass size_class, oa::ui::kit::Point offset
    );

    /// Writes the preferences after a window's place or the Window size
    /// changed, telling of a failure once.
    void write_window_preferences();

    /// Tells whether a screen waits in the queue.
    ///
    /// @param screen the screen
    /// @return true while it does
    [[nodiscard]] bool queued(const LayerScreen* screen) const noexcept;

    /// Returns, for each screen of the stack, bottom first, how many modal
    /// screens with a backdrop show above it: each darkens it once.
    ///
    /// @param seen what the screens are placed on
    /// @return the counts, one for each screen of the stack
    [[nodiscard]] std::vector<uint32_t> backdrops_above(const LayerView& seen) const;

    /// Hands one input to a screen at its place; a screen that asked to be
    /// closed while it took the input (close) answers close.
    ///
    /// @param screen the screen
    /// @param seen what the screen is placed on
    /// @param input the input
    /// @return what the screen answered
    LayerAnswer deliver(LayerScreen& screen, const LayerView& seen, const ScreenInput& input);

    /// Hands one input to a screen at its place, as deliver does, without
    /// looking for a close asked meanwhile.
    ///
    /// @param screen the screen
    /// @param seen what the screen is placed on
    /// @param input the input
    /// @return what the screen answered
    LayerAnswer hand_input(LayerScreen& screen, const LayerView& seen, const ScreenInput& input);

    /// Carries out a screen's answer to a gamepad action or the right stick.
    ///
    /// @param screen the top screen
    /// @param answer what it answered
    /// @return true when it took the input
    bool finish_pad(LayerScreen& screen, LayerAnswer answer);

    /// Takes a screen off, telling it it closes.
    ///
    /// @param screen the screen
    /// @param by_key a key's press closed it
    void remove(const LayerScreen* screen, bool by_key);

    /// Takes a screen out of the queue, telling it it closes.
    ///
    /// @param screen the screen
    void drop_waiting(const LayerScreen* screen);

    /// Pushes the waiting screens, first in first, while no modal screen
    /// shows, no frontend message box is open and no match is being played.
    void show_waiting();

    /// Returns the next id for a registered notice or prompt. Never 0.
    ///
    /// @return the id
    uint32_t take_request_id() noexcept;

    /// Takes the input the in-game OA Settings button answers to: the
    /// shortcut, Ctrl+F2 while the profile offers the mod options, and the
    /// pointer over the button, whose release opens Settings.
    ///
    /// @param input the input, in window pixels
    /// @return true when it was taken
    bool take_match_button(const ScreenInput& input);

    /// Keeps the system's text input started over the focused text field of
    /// the screen taking input, mapped to the coordinates input arrives in,
    /// and stops it once no screen has one.
    void sync_text_input();

    /// Lays every screen out for the view now, and every one waiting
    /// (LayerScreen::lay_out), first handing the screens' fonts the view's
    /// size class (screen_fonts).
    void lay_out_screens();

    /// Draws a screen from its own top left corner at a scale, never drawn
    /// at another and enlarged (layer_drawing): an opaque screen on a
    /// picture cleared to black, its corners then rounded by its placement's
    /// radius (round_layer_corners); one that is not opaque on a surface
    /// cleared to clear (clear_drawing).
    ///
    /// @param screen the screen
    /// @param placed where it shows
    /// @param scale_percent the scale, in percent
    /// @return its drawing, points_width × points_height at the scale
    [[nodiscard]] LayerDrawing draw_screen(
        const LayerScreen& screen, const LayerPlacement& placed, int32_t scale_percent
    ) const;

    /// Draws the front end's screens into the window.
    ///
    /// @param area where the picture lies in the window, in window pixels
    void present_front_end(const SDL_FRect& area);

    /// Draws the battle room's OA badges into the window, under the screens:
    /// the logo filtered into each CD rectangle's window pixels, and the
    /// rules dot as solid window pixels on top. Nothing while the battle
    /// room shows no badge, or the logo cannot be drawn.
    ///
    /// Throws PresentError when SDL cannot make the badges' texture.
    ///
    /// @param seen the front end's view
    /// @param output_width the window's width, in pixels
    /// @param output_height the window's height, in pixels
    void present_presence_badges(const LayerView& seen, int output_width, int output_height);

    /// Draws the match's layer into the window, as the match's other layers
    /// laid out 1:1 are drawn (one_to_one_scale_mode).
    void present_match();

    Runtime& runtime_; ///< the runtime it hosts screens for
    /// The press on a window's title that moves it; none while no title is held.
    std::optional<window_placement::TitleDrag> drag_{};
    /// The screen drag_ moves; null while no title is held.
    const LayerScreen* dragged_{};
    /// A failure to write a window's place was told once.
    bool window_write_failure_told_{};
    /// The screens, bottom first.
    std::vector<std::unique_ptr<LayerScreen>> screens_{};
    /// The screens waiting for the layer to be free, first in first.
    std::vector<std::unique_ptr<LayerScreen>> queue_{};
    /// The screen an input or its tick is handed to now; null between.
    const LayerScreen* calling_{};
    /// close asked for calling_ while it was called: it leaves once the
    /// call is done.
    bool close_asked_{};
    /// The key whose press closed a screen, until it is released; 0 for
    /// none. Its presses do nothing until then, so that a held key never
    /// reaches what lies under the layer.
    uint32_t latched_key_{};
    /// The field the system's text input was started over, in the
    /// coordinates input arrives in; none while the layer has not started it.
    std::optional<oa::ui::display_layout::Rect> text_field_{};
    bool whole_window_{}; ///< the front end's read-back holds the whole window (read_whole_window)
    bool button_hovered_{}; ///< the pointer is over the in-game OA Settings button
    bool button_pressed_{}; ///< a press on the in-game OA Settings button is held
    /// Counts the changes to what the match's layer shows that MatchLook
    /// cannot see: the button's hover and press, screens coming and going.
    uint64_t revision_{};
    std::optional<MatchLook> drawn_{}; ///< what rgba_ holds; nothing before the first draw
    /// The match's layer at the window's size, 4 bytes a pixel (red, green,
    /// blue, opacity), before the display gamma.
    std::vector<uint8_t> rgba_{};
    oa::ui::display_layout::Rect bounds_{}; ///< the part of the match's layer that is not clear
    std::optional<MatchLook> uploaded_{};   ///< the match's layer the texture holds
    /// The front end's screens the texture holds, at their places; none
    /// while it holds the match's layer or nothing.
    std::optional<FrontLook> front_uploaded_{};
    /// The drawings of the front end's screens that promise their revision
    /// covers what they draw, from the last present; a screen that no
    /// longer shows loses its own.
    std::vector<KeptDrawing> front_drawings_{};
    uint64_t front_drawing_count_{}; ///< counts the front end's drawings, numbering each
    oa::ui::display_layout::Rect
        front_bounds_{}; ///< the window pixels the front end's texture covers
    std::array<uint8_t, 256> uploaded_gamma_{}; ///< the gamma table the texture was uploaded at
    /// The layer's texture, in tiles beyond the renderer's limit; none
    /// before the first present.
    TiledTexture texture_;
    /// The battle room's OA badges, one window pixel each; none before the
    /// first frame that draws one.
    TiledTexture badge_texture_;
    /// The window pixels badge_texture_ covers.
    oa::ui::display_layout::Rect badge_bounds_{};

    /// One badge the texture was drawn for, in canvas pixels.
    struct PresenceBadgeDrawn {
        int32_t slot{};            ///< the row's slot
        int32_t x{}, y{};          ///< the badge's left and top, canvas pixels
        int32_t width{}, height{}; ///< the badge's size, canvas pixels
        bool rules_differ{};       ///< the badge carries the dot
        bool operator==(const PresenceBadgeDrawn&) const = default;
    };

    /// The badges, picture and logo the texture holds; empty before the first.
    std::vector<PresenceBadgeDrawn> badges_drawn_{};
    oa::ui::display_layout::Rect badges_picture_{}; ///< the picture the badges were mapped through
    oa::ui::display_layout::Point badges_picture_size_{}; ///< that picture's own size
    uint32_t badges_icon_width_{};            ///< the logo's width when the texture was drawn
    uint32_t badges_icon_height_{};           ///< the logo's height when the texture was drawn
    std::array<uint8_t, 256> badges_gamma_{}; ///< the gamma table the badges were uploaded at
    bool badges_uploaded_{};                  ///< badge_texture_ holds badges_drawn_
    /// The modern fonts the smooth form draws in (smooth_fonts); null before
    /// they are opened, or when they cannot be.
    mutable std::unique_ptr<oa::platform::text_font::FontStack> smooth_fonts_{};
    mutable bool smooth_fonts_tried_{}; ///< smooth_fonts tried to open them
    /// The fonts generation of the language's faces the modern fonts hold
    /// (Runtime::follow_language_fonts); empty before they took any.
    mutable std::optional<uint64_t> smooth_fonts_generation_{};
    /// The size class the screens were last laid out at, which their fonts
    /// take (screen_fonts).
    oa::ui::kit::SizeClass fonts_class_{oa::ui::kit::SizeClass::compact};
    /// Settings pages registered for this runtime.
    SettingsRegistry settings_pages_{};
    /// The next id queue_notice and queue_prompt hand out. Never 0.
    uint32_t next_request_{1};
    /// One registered prompt still to answer.
    struct RegisteredPrompt;
    /// Registered prompts still queued or shown. drop_pending_prompts answers each.
    std::vector<std::shared_ptr<RegisteredPrompt>> prompts_{};
};

/// A notice of Open Annihilation's own (kit::Notice) as a screen of the OA
/// layer: modal over a backdrop, laid out at the view's size class and
/// centred in the window at its scale over one screen of the game, its open
/// button and OK answered as its host says. A host that tells the player something nobody asked for at
/// that moment shows it with OaLayer::show_when_free.
class NoticeScreen final : public LayerScreen {
  public:

    /// The name a notice has on the layer unless it gives a word of its own
    /// (kit::Notice::word).
    static constexpr std::string_view screen_name = oa::ui::kit::notice_word;

    /// What the notice's host does.
    struct Host {
        /// Opens the notice's folder when its open button is pressed, and
        /// returns why it could not, which the notice shows in amber under
        /// its text; empty when it opened. Unset, the button opens nothing.
        std::function<std::string()> open{};
        /// What OK, Enter or Escape closing the notice does besides, such as
        /// its sound; may be unset.
        std::function<void()> ok{};
        /// Tells, once a frame, whether the notice must close, as when the
        /// screen it shows over goes; unset, it never must.
        std::function<bool()> tick{};
    };

    /// Makes the screen of a notice.
    ///
    /// @param layer the layer it shows on, whose fonts it is drawn in
    /// @param shown_over the screen of the game it shows over; it shows on no other
    /// @param told the notice
    /// @param answers what its host does
    NoticeScreen(OaLayer& layer, Screen shown_over, oa::ui::kit::Notice told, Host answers);

    /// Returns the notice as it shows.
    ///
    /// @return the notice
    [[nodiscard]] const oa::ui::kit::Notice& notice() const noexcept { return notice_; }

    /// Returns the screen of the game the notice shows over.
    ///
    /// @return the screen
    [[nodiscard]] Screen over() const noexcept { return over_; }

    /// Shows the notice on any menu screen. It still shows in no match.
    void show_on_any_menu() noexcept { any_menu_ = true; }

    /// Returns the notice's word (kit::notice_word_of): "notice" unless it
    /// gives its own.
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override {
        return oa::ui::kit::notice_word_of(notice_);
    }

    /// Returns where the notice shows: centred in the view's room at its
    /// scale, its size class's notice width wide and as tall as its text
    /// makes it at that width, over its own screen of the game.
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty in a match, over another screen unless it shows
    ///     on any menu, or without fonts
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override;

    /// Lays the notice out at the view's size class (kit::Notice::size_class).
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override;

    /// Tells that the notice takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the notice darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Draws the notice in the settings dialog's look, its texts in the
    /// language shown (oa::ui::engine_settings::draw_notice).
    ///
    /// @param canvas where it draws
    void draw(const oa::ui::kit::Canvas& canvas) const override;

    /// Takes a pointer's move, press or release: a finger's press takes the
    /// nearer button within reach.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the notice's points
    /// @param finger_reach a finger's reach in the notice's points; 0 for a mouse
    /// @return close when OK closed it; otherwise none or redraw
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, oa::ui::kit::Point at, int32_t finger_reach
    ) override;

    /// Takes a key's press: Enter and Escape close the notice, Space presses
    /// the marked button, and the arrows and Tab move the mark.
    ///
    /// @param pressed the key
    /// @return close when the key closed it; otherwise none or redraw
    LayerAnswer key(oa::ui::kit::Key pressed, uint32_t /*sdl_key*/) override;

    /// Takes a turn of the wheel, which does nothing under the notice.
    ///
    /// @return none
    LayerAnswer wheel(oa::ui::kit::Point /*at*/, float /*notches*/) override {
        return LayerAnswer::none;
    }

    /// Returns the notice's display list (kit::notice_list), its texts in
    /// the language shown.
    ///
    /// @return the list
    [[nodiscard]] oa::ui::kit::DisplayList display_list() const override;

    /// Returns the notice's hover, press and mark.
    ///
    /// @return the interaction
    [[nodiscard]] oa::ui::kit::Interaction interaction() const override;

    /// Asks the host whether the notice must close.
    ///
    /// @return close when it must; otherwise none
    LayerAnswer tick() override;

    /// Returns the notice's revision, which counts the changes to its look.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that every change to what the screen draws moves its revision.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Returns the notice's height in its fonts.
    ///
    /// @return the height, in points
    [[nodiscard]] int32_t shown_height() const;

    /// Carries out what an event on the notice asks: the open button's
    /// folder, OK's closing.
    ///
    /// @param action what the event asked
    /// @return what the screen answers
    LayerAnswer take(oa::ui::kit::NoticeAction action);

    OaLayer& layer_;             ///< the layer it shows on
    Screen over_{};              ///< the screen of the game it shows over
    oa::ui::kit::Notice notice_; ///< the notice
    Host host_;                  ///< what its host does
    uint64_t revision_{};        ///< counts the changes to its look
    bool any_menu_{};            ///< shows on any menu, never in a match
};

/// A work dialog of Open Annihilation's own (kit::WorkDialog) as a screen of
/// the OA layer: modal over a backdrop, centred in the window at its scale
/// over one screen of the game, its spinner turning. It has no buttons and
/// takes every input, doing nothing with it, until its host closes it.
class WorkScreen final : public LayerScreen {
  public:

    /// The name a work dialog has on the layer.
    static constexpr std::string_view screen_name = "work";

    /// What the work dialog's host does.
    struct Host {
        /// Tells, once a frame, whether the dialog must close, as when the
        /// work is done; unset, it never must.
        std::function<bool()> tick{};
    };

    /// Makes the screen of a work dialog.
    ///
    /// @param layer the layer it shows on, whose fonts it is placed in
    /// @param shown_over the screen of the game it shows over; it shows on no other
    /// @param title the header, as shown
    /// @param line the one line, as shown
    /// @param answers what its host does
    WorkScreen(
        OaLayer& layer, Screen shown_over, std::string title, std::string line, Host answers
    );

    /// Returns the dialog's name.
    ///
    /// @return screen_name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns where the dialog shows: centred in the view's room at its
    /// scale, its size class's notice width wide and as tall as its line
    /// makes it, over its own screen of the game.
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty in a match, over another screen, or without fonts
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override;

    /// Lays the dialog out at the view's size class.
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override;

    /// Tells that the dialog takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the dialog darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Draws the dialog (kit::draw_work_dialog).
    ///
    /// @param canvas where it draws
    void draw(const oa::ui::kit::Canvas& canvas) const override;

    /// Takes a pointer, which does nothing under the dialog.
    ///
    /// @return none
    LayerAnswer pointer(
        ScreenInputKind /*kind*/,
        uint8_t /*button*/,
        oa::ui::kit::Point /*at*/,
        int32_t /*finger_reach*/
    ) override {
        return LayerAnswer::none;
    }

    /// Takes a key, which does nothing under the dialog.
    ///
    /// @return none
    LayerAnswer key(oa::ui::kit::Key /*pressed*/, uint32_t /*sdl_key*/) override {
        return LayerAnswer::none;
    }

    /// Takes a turn of the wheel, which does nothing under the dialog.
    ///
    /// @return none
    LayerAnswer wheel(oa::ui::kit::Point /*at*/, float /*notches*/) override {
        return LayerAnswer::none;
    }

    /// Returns the dialog's display list (kit::work_dialog_list).
    ///
    /// @return the list
    [[nodiscard]] oa::ui::kit::DisplayList display_list() const override;

    /// Returns the dialog's interaction: nothing hovered, pressed or focused.
    ///
    /// @return the interaction
    [[nodiscard]] oa::ui::kit::Interaction interaction() const override { return {}; }

    /// Turns the spinner with the time it has shown, and asks the host
    /// whether the dialog must close.
    ///
    /// @return close when it must; redraw when the spinner turned; otherwise none
    LayerAnswer tick() override;

    /// Returns the dialog's revision, which counts the changes to its look.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    OaLayer& layer_;                 ///< the layer it shows on
    Screen over_{};                  ///< the screen of the game it shows over
    oa::ui::kit::WorkDialog dialog_; ///< the dialog
    Host host_;                      ///< what its host does
    uint32_t began_ms_{};            ///< when it was made, on the platform's clock
    uint64_t revision_{};            ///< counts the changes to its look
};

/// A question of Open Annihilation's own (kit::Question) as a screen of the
/// OA layer: modal over a backdrop, laid out at the view's size class and
/// centred in the window at its scale over one screen of the game, its
/// answers handed to its host. A question
/// nobody asked for at that moment, such as an install's, is shown with
/// OaLayer::show_when_free; one a screen asks of its own is pushed over it at
/// once (OaLayer::push). Its host may change the question while it shows
/// (question, changed).
class QuestionScreen final : public LayerScreen {
  public:

    /// The name a question has on the layer unless it gives a word of its
    /// own (kit::Question::word).
    static constexpr std::string_view screen_name = oa::ui::kit::question_word;

    /// What the question's host does.
    struct Host {
        /// Takes an answer: the button pressed, from 0. The host may change
        /// the question, or close it (OaLayer::close), meanwhile. Unset, every
        /// answer closes the question.
        ///
        /// Returns true when the answer is the question's last and it
        /// closes; false keeps it, as the host left it.
        std::function<bool(int32_t button)> answer{};
        /// Tells, once a frame, whether the question must close or be set
        /// aside, as when the screen it shows over goes; unset, it never must.
        std::function<bool()> tick{};
        /// What the question's leaving the layer, or its queue, does,
        /// whatever took it off; may be unset.
        std::function<void(const QuestionScreen& leaving)> closed{};
    };

    /// Makes the screen of a question.
    ///
    /// @param layer the layer it shows on, whose fonts it is drawn in, and
    ///     which latches the key that answered it
    /// @param shown_over the screen of the game it shows over; it shows on no other
    /// @param asked the question
    /// @param answers what its host does
    QuestionScreen(OaLayer& layer, Screen shown_over, oa::ui::kit::Question asked, Host answers);

    /// Returns the question, which its host may change; it then calls changed.
    ///
    /// @return the question
    [[nodiscard]] oa::ui::kit::Question& question() noexcept { return question_; }

    /// Returns the question as it shows.
    ///
    /// @return the question
    [[nodiscard]] const oa::ui::kit::Question& question() const noexcept { return question_; }

    /// Tells the screen its host changed the question: it keeps the size
    /// class the layer lays it out at, whatever the host put in its place.
    void changed() noexcept {
        question_.size_class = size_class_;
        ++revision_;
    }

    /// Returns the screen of the game the question shows over.
    ///
    /// @return the screen
    [[nodiscard]] Screen over() const noexcept { return over_; }

    /// Shows the question on any menu screen. It still shows in no match.
    void show_on_any_menu() noexcept { any_menu_ = true; }

    /// Returns the question's word (kit::question_word_of): "prompt" unless
    /// it gives its own.
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override {
        return oa::ui::kit::question_word_of(question_);
    }

    /// Returns where the question shows: centred in the view's room at its
    /// scale, its size class's notice width wide and as tall as its text
    /// makes it at that width, over its own screen of the game.
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty in a match, over another screen unless it shows
    ///     on any menu, or without fonts
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override;

    /// Lays the question out at the view's size class
    /// (kit::Question::size_class).
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override;

    /// Tells that the question takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the question darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Draws the question in a notice's look
    /// (oa::ui::engine_settings::draw_prompt).
    ///
    /// @param canvas where it draws
    void draw(const oa::ui::kit::Canvas& canvas) const override;

    /// Takes a pointer's move, press or release: a finger's press takes the
    /// nearest button within reach, and a release over the button pressed
    /// answers it.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the question's points
    /// @param finger_reach a finger's reach in the question's points; 0 for a mouse
    /// @return close when an answer closed it; otherwise none or redraw
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, oa::ui::kit::Point at, int32_t finger_reach
    ) override;

    /// Takes a key's press: Enter and Space answer the marked button, Escape
    /// and N the cancel button, Y the primary one; the arrows and Tab move
    /// the mark. The key that answered is latched (OaLayer::latch_key).
    ///
    /// @param pressed the key
    /// @param sdl_key the key's SDL keycode
    /// @return close when the answer closed it; otherwise none or redraw
    LayerAnswer key(oa::ui::kit::Key pressed, uint32_t sdl_key) override;

    /// Takes a turn of the wheel, which does nothing under the question.
    ///
    /// @return none
    LayerAnswer wheel(oa::ui::kit::Point /*at*/, float /*notches*/) override {
        return LayerAnswer::none;
    }

    /// Returns the question's display list (kit::question_list).
    ///
    /// @return the list
    [[nodiscard]] oa::ui::kit::DisplayList display_list() const override;

    /// Returns the question's hover, press and mark.
    ///
    /// @return the interaction
    [[nodiscard]] oa::ui::kit::Interaction interaction() const override;

    /// Asks the host whether the question must close or be set aside.
    ///
    /// @return close when it must; otherwise none
    LayerAnswer tick() override;

    /// Returns the question's revision, which counts the changes to its look.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that every change to what the screen draws moves its revision.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Tells the host the question leaves (Host::closed).
    void close(bool /*by_key*/) override;

  private:

    /// Returns the question's height in its fonts.
    ///
    /// @return the height, in points
    [[nodiscard]] int32_t shown_height() const;

    /// Carries out what an event on the question asks: an answer goes to
    /// the host, and the key that gave it is latched.
    ///
    /// @param outcome what the event did
    /// @param sdl_key the key whose press it was; 0 for a pointer
    /// @return what the screen answers
    LayerAnswer take(oa::ui::kit::QuestionAnswer outcome, uint32_t sdl_key);

    OaLayer& layer_;                 ///< the layer it shows on
    Screen over_{};                  ///< the screen of the game it shows over
    oa::ui::kit::Question question_; ///< the question
    Host host_;                      ///< what its host does
    uint64_t revision_{};            ///< counts the changes to its look
    /// The size class the layer lays the question out at (lay_out).
    oa::ui::kit::SizeClass size_class_{oa::ui::kit::SizeClass::compact};
    bool any_menu_{}; ///< shows on any menu, never in a match
};

/// The UI kit gallery (kit::gallery_layout) as a screen of the OA layer:
/// modal over a backdrop, covering the whole window. On the front end it
/// takes the view's scale and size class (the Interface size, or the Auto
/// scale); in a match the scale and class OaLayer::match_gallery gives.
/// Its model is its own: nothing it does reads or changes a setting.
/// Escape and its CLOSE take it off, back to the settings dialog under it,
/// and it leaves with that dialog.
class GalleryScreen final : public LayerScreen {
  public:

    /// The gallery's name on the layer; automation names its dialog
    /// oa.gallery.
    static constexpr std::string_view screen_name = "gallery";

    /// Makes the gallery's screen.
    ///
    /// @param layer the layer it shows on, whose fonts it is laid out and
    ///     drawn in
    /// @param in_match it was opened in a match; otherwise on the front end
    GalleryScreen(OaLayer& layer, bool in_match) noexcept;

    /// Returns "gallery".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns where the gallery shows: on the front end the view's room at
    /// its scale, the frame's area wide and tall in points, centred; in a
    /// match match_gallery's frame at its scale, centred in the canvas's
    /// safe area.
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty where it was not opened, or without fonts
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override;

    /// Tells that the gallery takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the gallery darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the gallery out for the frame the view gives it, when that
    /// changes.
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override;

    /// Draws the gallery with the kit (kit::draw_gallery).
    ///
    /// @param canvas where it draws
    void draw(const oa::ui::kit::Canvas& canvas) const override;

    /// Takes a pointer's move, press or release (kit::gallery_act).
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button; only the left presses
    /// @param at the pointer, in the gallery's points
    /// @param finger_reach a finger's reach in the gallery's points; 0 for a mouse
    /// @return close when CLOSE closed it; otherwise none or redraw
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, oa::ui::kit::Point at, int32_t finger_reach
    ) override;

    /// Takes a key's press (kit::gallery_act); the information key does
    /// nothing here.
    ///
    /// @param pressed the key
    /// @return close when Escape closed it; otherwise none or redraw
    LayerAnswer key(oa::ui::kit::Key pressed, uint32_t /*sdl_key*/) override;

    /// Takes a turn of the wheel, which scrolls the sections or the scroll list.
    ///
    /// @param at the pointer, in the gallery's points
    /// @param notches how far the wheel turned
    /// @return redraw when it scrolled; otherwise none
    LayerAnswer wheel(oa::ui::kit::Point at, float notches) override;

    /// Takes text typed into the gallery's search field.
    ///
    /// @param utf8 the text
    /// @return redraw when the field took it; otherwise none
    LayerAnswer text(std::string_view utf8) override;

    /// Returns the search field while it has the focus.
    ///
    /// @return its rectangle, in the gallery's points; none otherwise
    [[nodiscard]] std::optional<oa::ui::kit::Rect> text_field() const override;

    /// Returns the gallery's display list, as automation lists it.
    ///
    /// @return the list
    [[nodiscard]] oa::ui::kit::DisplayList display_list() const override { return layout_.list; }

    /// Returns the gallery's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] oa::ui::kit::Interaction interaction() const override {
        return model_.interaction;
    }

    /// Closes the gallery once the settings dialog under it has gone.
    ///
    /// @return close then; otherwise none
    LayerAnswer tick() override;

    /// Returns the gallery's revision, which counts the changes to its look.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that every change to what the screen draws moves its revision.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

    /// Returns the frame the gallery is laid out for: its size in points,
    /// its size class and its scale.
    ///
    /// @return the frame; an empty area before it is first laid out
    [[nodiscard]] const oa::ui::kit::Frame& frame() const noexcept { return frame_; }

    /// Returns the gallery as laid out now.
    ///
    /// @return the layout
    [[nodiscard]] const oa::ui::kit::GalleryLayout& layout() const noexcept { return layout_; }

    /// Returns the gallery's model.
    ///
    /// @return the model
    [[nodiscard]] const oa::ui::kit::GalleryModel& model() const noexcept { return model_; }

  private:

    /// Returns the frame a view gives the gallery.
    ///
    /// @param view what the screen is placed on
    /// @return the frame, its area from the gallery's own origin; an empty
    ///     area where the gallery does not show
    [[nodiscard]] oa::ui::kit::Frame frame_for(const LayerView& view) const;

    /// Hands one event to the gallery and lays it out again when it changed.
    ///
    /// @param event the event
    /// @return what the screen answers
    LayerAnswer take(const oa::ui::kit::GalleryEvent& event);

    OaLayer& layer_;                      ///< the layer it shows on
    bool in_match_{};                     ///< opened in a match; otherwise on the front end
    oa::ui::kit::GalleryModel model_{};   ///< its own model
    oa::ui::kit::Frame frame_{};          ///< the frame it is laid out for
    oa::ui::kit::GalleryLayout layout_{}; ///< the layout of model_ for frame_
    uint64_t revision_{};                 ///< counts the changes to its look
};

} // namespace oa::app
