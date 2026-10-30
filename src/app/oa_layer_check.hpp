// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Helpers for the native checks of the OA layer's screens as the window
// shows them: the window frame expected while a screen shows over the front
// end, the pixels a presented frame differs in outside the software cursor's
// square, and pointer and finger events at a window pixel, so that a check
// presses a screen where the layer shows it at any window size; and a screen
// only the checks push (MarkScreen), opaque or not, with which the layer's
// own check (OaLayer::check_clear_screens) holds what the layer shows of a
// screen that is not opaque, and where the information key goes.
#pragma once

#include "oa_layer.hpp"

#include "oa/ui/display_layout.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace oa::app {

/// A window size a check shows a screen of the OA layer at, and the size
/// class and scale the layer gives the screen there.
struct LayerWindow {
    int32_t width{};                     ///< the window's width, in pixels
    int32_t height{};                    ///< the window's height, in pixels
    oa::ui::kit::SizeClass size_class{}; ///< the class the screen is laid out at
    int32_t scale{};                     ///< window pixels a point
};

/// The windows the notice and prompt checks show their screens at beyond
/// 640 by 480: 800 by 600, Regular at 1×, and 2560 by 1440, Large at 2×.
inline constexpr std::array<LayerWindow, 2> larger_layer_windows{{
    {800, 600, oa::ui::kit::SizeClass::regular, 1},
    {2560, 1440, oa::ui::kit::SizeClass::large, 2},
}};

/// Returns a size class's name, as the checks print it.
///
/// @param size_class the class
/// @return "Compact", "Regular" or "Large"
[[nodiscard]] std::string_view size_class_name(oa::ui::kit::SizeClass size_class) noexcept;

/// How far round the pointer the software cursor may draw, in the front end's
/// picture's pixels.
inline constexpr int32_t layer_cursor_reach = 40;

/// The pointer as the window shows it: where it rests, and how far round it
/// the software cursor may draw.
struct LayerPointer {
    oa::ui::display_layout::Point at{}; ///< where it rests, in window pixels
    int32_t reach{};                    ///< the cursor's reach round it, in window pixels
};

/// Returns the pointer resting at a point of the front end's picture as the
/// window shows it: the point through the picture's rectangle, and
/// layer_cursor_reach picture pixels as the picture's letterbox scale makes
/// them, rounded up.
///
/// @param view what the layer's screens are placed on (OaLayer::view)
/// @param picture_point the pointer, in the picture's pixels
/// @return the pointer, in window pixels
[[nodiscard]] LayerPointer
window_pointer(const LayerView& view, oa::ui::display_layout::Point picture_point) noexcept;

/// Returns the window frame expected while a screen of the OA layer shows
/// over the front end: the frame presented with no OA screen open, every
/// pixel darkened as a modal screen's backdrop darkens it (the backdrop's
/// colour blended at the main menu's opacity, once for each such screen),
/// with the screen's own drawing copied at its placement, its corners rounded
/// by the placement's radius as the layer rounds them (round_layer_corners).
///
/// The frames are compared at the display gamma the window shows; the
/// darkening is the frame's own, so the comparison holds where that gamma
/// leaves the picture's colours as they are, as the checks run.
///
/// @param closed the window's frame with no OA screen open
/// @param drawing the screen drawn at its size class and the view's scale,
///     as large as its placement
/// @param placement where the layer shows the screen, in window pixels
/// @param darkenings the modal screens with a backdrop that show
/// @return the expected frame, the size of the closed one
[[nodiscard]] oa::ui::frontend_renderer::Surface expected_layer_frame(
    const oa::ui::frontend_renderer::Surface& closed,
    const oa::ui::frontend_renderer::Surface& drawing,
    const LayerPlacement& placement,
    uint32_t darkenings = 1
);

/// Rounds the corners of a screen a check drew straight into an expected
/// frame, as the layer shows them (round_layer_corners): outside each
/// quarter circle the frame shows what it held before the screen was drawn,
/// and along it the window's edge. A placement with no corner radius
/// changes nothing.
///
/// @param[in,out] frame the expected frame, the screen drawn in it at its place
/// @param before the same frame before the screen was drawn in it
/// @param placement where the screen shows in the frame, with its corners' radius
void round_drawn_screen(
    oa::ui::frontend_renderer::Surface& frame,
    const oa::ui::frontend_renderer::Surface& before,
    const LayerPlacement& placement
);

/// Counts the pixels in which a presented window frame differs from the
/// expected one, every pixel of the window compared but those in the square
/// round the pointer where the software cursor draws.
///
/// @param presented the window's frame as presented
/// @param expected the frame expected (expected_layer_frame)
/// @param pointer where the pointer rests (window_pointer)
/// @return the pixels that differ; every pixel when the frames' sizes differ
[[nodiscard]] std::size_t layer_differences(
    const oa::ui::frontend_renderer::Surface& presented,
    const oa::ui::frontend_renderer::Surface& expected,
    const LayerPointer& pointer
) noexcept;

/// Returns the window pixel in the middle of one of a screen's points,
/// through the screen's placement: left + point × scale + scale / 2, and
/// the same down.
///
/// @param placement where the layer shows the screen
/// @param point the point, in the screen's points
/// @return the window pixel
[[nodiscard]] oa::ui::display_layout::Point
layer_window_pixel(const LayerPlacement& placement, oa::ui::kit::Point point) noexcept;

/// Returns a mouse event at the middle of a window pixel, in the window's
/// coordinates (its points), as SDL reports the pointer there.
///
/// @param window the window
/// @param sdl_renderer its renderer, whose output size gives the window's pixels
/// @param type SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN or SDL_EVENT_MOUSE_BUTTON_UP
/// @param pixel the window pixel
/// @param button the button pressed or released; 0 for a motion
/// @return the event, to dispatch
[[nodiscard]] SDL_Event window_pointer_event(
    SDL_Window* window,
    SDL_Renderer* sdl_renderer,
    SDL_EventType type,
    oa::ui::display_layout::Point pixel,
    uint8_t button
) noexcept;

/// Returns a finger's event at the middle of a window pixel, its place a
/// share of the window's size, as SDL reports a finger there.
///
/// @param window the window
/// @param sdl_renderer its renderer, whose output size gives the window's pixels
/// @param type SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_MOTION or SDL_EVENT_FINGER_UP
/// @param pixel the window pixel
/// @param touch the touch device the finger is on
/// @param finger the finger
/// @return the event, to dispatch
[[nodiscard]] SDL_Event window_finger_event(
    SDL_Window* window,
    SDL_Renderer* sdl_renderer,
    SDL_EventType type,
    oa::ui::display_layout::Point pixel,
    SDL_TouchID touch,
    SDL_FingerID finger
) noexcept;

/// The share of each pixel a mark screen's veil blends its colour over, in
/// 256ths: half.
inline constexpr uint32_t mark_veil_opacity = 128;

/// How a check's mark screen (MarkScreen) shows and answers.
struct MarkLook {
    std::string name{"check-mark"}; ///< its name on the layer
    bool opaque{};                  ///< it covers its whole place
    bool modal{};                   ///< it takes every input
    bool backdrop{};                ///< it asks for a backdrop
    bool in_match{};                ///< it shows in a match too; otherwise on the front end alone
    /// Its place, in points from the top left corner of the view's room;
    /// empty covers the whole room.
    oa::ui::kit::Rect area{};
    oa::ui::kit::Rect mark{};                ///< the rectangle it fills, in its own points
    oa::ui::frontend_renderer::Rgb colour{}; ///< the mark's colour
    /// A rectangle it blends the mark's colour over at mark_veil_opacity, in
    /// its own points; empty for none.
    oa::ui::kit::Rect veil{};
    bool takes_info{}; ///< it takes the information key; otherwise it lets it go on
};

/// The keys that reached a check's mark screen.
struct MarkSeen {
    int32_t infos{};      ///< presses of the information key
    uint32_t info_key{};  ///< the SDL keycode the last of them came from
    int32_t other_keys{}; ///< presses of every other key
};

/// A screen of the OA layer only the checks push, on the front end and, as
/// its look says, in a match: it fills one small rectangle of a colour and
/// may blend that colour over another, draws nothing else, lists the
/// rectangle to automation as a button named mark, and notes the keys it is
/// given, taking the information key or letting it go on. Every pointer
/// event, turn of the wheel and other key goes on.
class MarkScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param look how it shows and answers
    /// @param seen where it notes the keys that reach it
    MarkScreen(MarkLook look, std::shared_ptr<MarkSeen> seen);

    /// Returns how it shows and answers.
    ///
    /// @return the look
    [[nodiscard]] const MarkLook& look() const noexcept { return look_; }

    /// Returns its name.
    ///
    /// @return the look's name
    [[nodiscard]] std::string_view name() const override { return look_.name; }

    /// Returns its place at the view's scale: its area from the room's top
    /// left corner, or the whole room in whole points.
    ///
    /// @param view what it is placed on
    /// @return its place; empty in a match unless its look shows it there
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override;

    /// Tells whether it takes every input.
    ///
    /// @return the look's modal
    [[nodiscard]] bool modal() const override { return look_.modal; }

    /// Tells whether it asks for a backdrop.
    ///
    /// @return the look's backdrop
    [[nodiscard]] bool backdrop() const override { return look_.backdrop; }

    /// Tells whether it covers its whole place.
    ///
    /// @return the look's opaque
    [[nodiscard]] bool opaque() const override { return look_.opaque; }

    /// Fills its mark with its colour, then blends the colour over its veil.
    ///
    /// @param canvas where it draws
    void draw(const oa::ui::kit::Canvas& canvas) const override;

    /// Lets every pointer event go on.
    ///
    /// @return pass
    LayerAnswer pointer(
        ScreenInputKind /*kind*/, uint8_t /*button*/, oa::ui::kit::Point /*at*/, int32_t /*reach*/
    ) override {
        return LayerAnswer::pass;
    }

    /// Notes a key: the information key is taken or goes on as the look
    /// says; every other key goes on.
    ///
    /// @param pressed the key
    /// @param sdl_key its SDL keycode
    /// @return none for the information key it takes; otherwise pass
    LayerAnswer key(oa::ui::kit::Key pressed, uint32_t sdl_key) override;

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(oa::ui::kit::Point /*at*/, float /*notches*/) override {
        return LayerAnswer::pass;
    }

    /// Returns its one control: the mark, a button named mark.
    ///
    /// @return the list
    [[nodiscard]] oa::ui::kit::DisplayList display_list() const override;

    /// Returns no hover, press or focus.
    ///
    /// @return the interaction
    [[nodiscard]] oa::ui::kit::Interaction interaction() const override { return {}; }

    /// Changes nothing.
    ///
    /// @return none
    LayerAnswer tick() override { return LayerAnswer::none; }

    /// Returns 0: it always draws the same.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return 0; }

    /// Has nothing to close.
    void close(bool /*by_key*/) override {}

  private:

    MarkLook look_;                  ///< how it shows and answers
    std::shared_ptr<MarkSeen> seen_; ///< where it notes the keys that reach it
};

/// Returns where a rectangle of a screen's points lies in the window: its
/// place's left and top plus the rectangle's own times the scale the place
/// gives a point.
///
/// @param placement where the layer shows the screen
/// @param points the rectangle, in the screen's points
/// @return the rectangle, in window pixels
[[nodiscard]] oa::ui::display_layout::Rect
layer_window_rect(const LayerPlacement& placement, const oa::ui::kit::Rect& points) noexcept;

} // namespace oa::app
