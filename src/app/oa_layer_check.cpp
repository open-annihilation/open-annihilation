// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Helpers for the native checks of the OA layer's screens as the window
// shows them (oa_layer_check.hpp), the checks' mark screen, and the layer's
// own check of screens that are not opaque, of the information key and of
// Settings moved by its title (OaLayer::check_clear_screens), which
// --check-engine-settings runs.

#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/runtime.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace artless = oa::ui::frontend_renderer;

/// Returns the window's pixels per point of its own: its renderer's output
/// size over its size.
///
/// @param window the window
/// @param sdl_renderer its renderer
/// @return the density; 1 when either size is unknown
float pixel_density(SDL_Window* window, SDL_Renderer* sdl_renderer) noexcept {
    int window_width = 0;
    int window_height = 0;
    int output_width = 0;
    int output_height = 0;
    if (window == nullptr || sdl_renderer == nullptr ||
        !SDL_GetWindowSize(window, &window_width, &window_height) ||
        !SDL_GetRenderOutputSize(sdl_renderer, &output_width, &output_height) ||
        window_width <= 0 || output_width <= 0)
        return 1.0F;
    return static_cast<float>(output_width) / static_cast<float>(window_width);
}

} // namespace

std::string_view size_class_name(oa::ui::kit::SizeClass size_class) noexcept {
    switch (size_class) {
    case oa::ui::kit::SizeClass::regular:
        return "Regular";
    case oa::ui::kit::SizeClass::large:
        return "Large";
    case oa::ui::kit::SizeClass::compact:
        break;
    }
    return "Compact";
}

LayerPointer
window_pointer(const LayerView& view, oa::ui::display_layout::Point picture_point) noexcept {
    if (view.picture_size.x <= 0 || view.picture_size.y <= 0)
        return {picture_point, layer_cursor_reach};
    const double across =
        static_cast<double>(view.picture.width) / static_cast<double>(view.picture_size.x);
    const double down =
        static_cast<double>(view.picture.height) / static_cast<double>(view.picture_size.y);
    return {
        {view.picture.x + static_cast<int32_t>(std::floor((picture_point.x + 0.5) * across)),
         view.picture.y + static_cast<int32_t>(std::floor((picture_point.y + 0.5) * down))},
        static_cast<int32_t>(std::ceil(layer_cursor_reach * std::max(across, down)))
    };
}

artless::Surface expected_layer_frame(
    const artless::Surface& closed,
    const artless::Surface& drawing,
    const LayerPlacement& placement,
    uint32_t darkenings
) {
    artless::Surface expected = closed;
    // The backdrop over the whole window, as it darkens the picture.
    for (uint32_t pass = 0; pass < darkenings; ++pass)
        artless::blend_source_rect(
            expected,
            {0, 0, 1},
            {0, 0, static_cast<int32_t>(expected.width), static_cast<int32_t>(expected.height)},
            oa::ui::kit::rgb(oa::ui::kit::colour::backdrop),
            oa::ui::kit::menu_backdrop_opacity
        );
    // The drawing at its place, pixel for pixel, its corners rounded as the
    // layer rounds them: what lies outside them shows the frame behind.
    LayerDrawing rounded;
    rounded.picture = drawing;
    round_layer_corners(rounded, placement.corner_radius, placement.scale_percent);
    const auto& shown = placement.shown;
    const auto drawn_width = static_cast<int32_t>(drawing.width);
    const auto drawn_height = static_cast<int32_t>(drawing.height);
    const int32_t left = std::max(shown.x, 0);
    const int32_t top = std::max(shown.y, 0);
    const int32_t right = std::min(
        {shown.x + shown.width, shown.x + drawn_width, static_cast<int32_t>(closed.width)}
    );
    const int32_t bottom = std::min(
        {shown.y + shown.height, shown.y + drawn_height, static_cast<int32_t>(closed.height)}
    );
    for (int32_t row = top; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column) {
            const std::size_t source = static_cast<std::size_t>(row - shown.y) * drawing.width +
                                       static_cast<std::size_t>(column - shown.x);
            if (!rounded.opacity.empty() && rounded.opacity[source] == 0)
                continue;
            const auto* from = rounded.picture.rgb.data() + source * 3U;
            auto* to = expected.rgb.data() + (static_cast<std::size_t>(row) * expected.width +
                                              static_cast<std::size_t>(column)) *
                                                 3U;
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
        }
    return expected;
}

void round_drawn_screen(
    artless::Surface& frame, const artless::Surface& before, const LayerPlacement& placement
) {
    const auto& shown = placement.shown;
    if (placement.corner_radius <= 0 || shown.width <= 0 || shown.height <= 0 ||
        before.width != frame.width || before.height != frame.height || shown.x < 0 ||
        shown.y < 0 || shown.x + shown.width > static_cast<int32_t>(frame.width) ||
        shown.y + shown.height > static_cast<int32_t>(frame.height))
        return;
    // The screen's rectangle, rounded as the layer rounds it.
    LayerDrawing drawn;
    drawn.picture.width = static_cast<uint32_t>(shown.width);
    drawn.picture.height = static_cast<uint32_t>(shown.height);
    drawn.picture.rgb.resize(static_cast<std::size_t>(shown.width) * shown.height * 3U);
    for (int32_t row = 0; row < shown.height; ++row)
        std::copy_n(
            frame.rgb.data() + (static_cast<std::size_t>(shown.y + row) * frame.width +
                                static_cast<std::size_t>(shown.x)) *
                                   3U,
            static_cast<std::size_t>(shown.width) * 3U,
            drawn.picture.rgb.data() + static_cast<std::size_t>(row) * shown.width * 3U
        );
    round_layer_corners(drawn, placement.corner_radius, placement.scale_percent);
    for (int32_t row = 0; row < shown.height; ++row)
        for (int32_t column = 0; column < shown.width; ++column) {
            const std::size_t source =
                static_cast<std::size_t>(row) * shown.width + static_cast<std::size_t>(column);
            const std::size_t target = (static_cast<std::size_t>(shown.y + row) * frame.width +
                                        static_cast<std::size_t>(shown.x + column)) *
                                       3U;
            const uint8_t* from = drawn.opacity.empty() || drawn.opacity[source] != 0
                                      ? drawn.picture.rgb.data() + source * 3U
                                      : before.rgb.data() + target;
            std::copy_n(from, 3U, frame.rgb.data() + target);
        }
}

std::size_t layer_differences(
    const artless::Surface& presented, const artless::Surface& expected, const LayerPointer& pointer
) noexcept {
    if (presented.width != expected.width || presented.height != expected.height ||
        presented.rgb.size() != expected.rgb.size())
        return static_cast<std::size_t>(expected.width) * expected.height;
    std::size_t differing = 0;
    for (uint32_t y = 0; y < presented.height; ++y)
        for (uint32_t x = 0; x < presented.width; ++x) {
            const auto column = static_cast<int32_t>(x);
            const auto row = static_cast<int32_t>(y);
            if (column >= pointer.at.x - pointer.reach && column < pointer.at.x + pointer.reach &&
                row >= pointer.at.y - pointer.reach && row < pointer.at.y + pointer.reach)
                continue;
            const std::size_t at = (static_cast<std::size_t>(y) * presented.width + x) * 3U;
            if (presented.rgb[at] != expected.rgb[at] ||
                presented.rgb[at + 1] != expected.rgb[at + 1] ||
                presented.rgb[at + 2] != expected.rgb[at + 2])
                ++differing;
        }
    return differing;
}

oa::ui::display_layout::Point
layer_window_pixel(const LayerPlacement& placement, oa::ui::kit::Point point) noexcept {
    const auto& shown = placement.shown;
    if (placement.points_width <= 0 || placement.points_height <= 0)
        return {shown.x, shown.y};
    // A point's window pixels, and the one in their middle.
    const auto middle = [](int32_t start, int32_t length, int32_t points, int32_t at) {
        const int64_t first = int64_t{at} * length / points;
        const int64_t past = (int64_t{at} + 1) * length / points;
        return start + static_cast<int32_t>((first + past) / 2);
    };
    return {
        middle(shown.x, shown.width, placement.points_width, point.x),
        middle(shown.y, shown.height, placement.points_height, point.y)
    };
}

SDL_Event window_pointer_event(
    SDL_Window* window,
    SDL_Renderer* sdl_renderer,
    SDL_EventType type,
    oa::ui::display_layout::Point pixel,
    uint8_t button
) noexcept {
    const float density = pixel_density(window, sdl_renderer);
    const float x = (static_cast<float>(pixel.x) + 0.5F) / density;
    const float y = (static_cast<float>(pixel.y) + 0.5F) / density;
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.windowID = window != nullptr ? SDL_GetWindowID(window) : 0;
        event.motion.x = x;
        event.motion.y = y;
    } else {
        event.button.windowID = window != nullptr ? SDL_GetWindowID(window) : 0;
        event.button.button = button;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = 1;
        event.button.x = x;
        event.button.y = y;
    }
    return event;
}

SDL_Event window_finger_event(
    SDL_Window* window,
    SDL_Renderer* sdl_renderer,
    SDL_EventType type,
    oa::ui::display_layout::Point pixel,
    SDL_TouchID touch,
    SDL_FingerID finger
) noexcept {
    const float density = pixel_density(window, sdl_renderer);
    int window_width = 1;
    int window_height = 1;
    if (window == nullptr || !SDL_GetWindowSize(window, &window_width, &window_height) ||
        window_width <= 0 || window_height <= 0) {
        window_width = 1;
        window_height = 1;
    }
    SDL_Event event{};
    event.type = type;
    event.tfinger.touchID = touch;
    event.tfinger.fingerID = finger;
    event.tfinger.x =
        (static_cast<float>(pixel.x) + 0.5F) / density / static_cast<float>(window_width);
    event.tfinger.y =
        (static_cast<float>(pixel.y) + 0.5F) / density / static_cast<float>(window_height);
    event.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0F : 1.0F;
    event.tfinger.windowID = window != nullptr ? SDL_GetWindowID(window) : 0;
    return event;
}

// ---------------------------------------------------------------------------------------------
// The checks' mark screen

MarkScreen::MarkScreen(MarkLook look, std::shared_ptr<MarkSeen> seen)
    : look_(std::move(look)), seen_(std::move(seen)) {
}

LayerPlacement MarkScreen::placement(const LayerView& view) const {
    if (view.match && !look_.in_match)
        return {};
    const int32_t point_pixels = std::max(view.scale, int32_t{1});
    if (look_.area.width <= 0 || look_.area.height <= 0) {
        // The whole room, in whole points.
        const int32_t width = view.room.width / point_pixels;
        const int32_t height = view.room.height / point_pixels;
        return {
            {view.room.x, view.room.y, width * point_pixels, height * point_pixels}, width, height
        };
    }
    return {
        {view.room.x + look_.area.x * point_pixels,
         view.room.y + look_.area.y * point_pixels,
         look_.area.width * point_pixels,
         look_.area.height * point_pixels},
        look_.area.width,
        look_.area.height
    };
}

void MarkScreen::draw(const oa::ui::kit::Canvas& canvas) const {
    if (canvas.surface == nullptr)
        return;
    const auto& mark = look_.mark;
    artless::fill_source_rect(
        *canvas.surface, canvas.placement, {mark.x, mark.y, mark.width, mark.height}, look_.colour
    );
    const auto& veil = look_.veil;
    if (veil.width > 0 && veil.height > 0)
        artless::blend_source_rect(
            *canvas.surface,
            canvas.placement,
            {veil.x, veil.y, veil.width, veil.height},
            look_.colour,
            mark_veil_opacity
        );
}

LayerAnswer MarkScreen::key(oa::ui::kit::Key pressed, uint32_t sdl_key) {
    if (pressed != oa::ui::kit::Key::info) {
        ++seen_->other_keys;
        return LayerAnswer::pass;
    }
    ++seen_->infos;
    seen_->info_key = sdl_key;
    return look_.takes_info ? LayerAnswer::none : LayerAnswer::pass;
}

oa::ui::kit::DisplayList MarkScreen::display_list() const {
    oa::ui::kit::DisplayList list;
    oa::ui::kit::Control mark;
    mark.id = 1;
    mark.rect = look_.mark;
    mark.name = "mark";
    mark.kind = oa::ui::kit::ControlKind::button;
    mark.focusable = false;
    list.controls.push_back(std::move(mark));
    return list;
}

oa::ui::display_layout::Rect
layer_window_rect(const LayerPlacement& placement, const oa::ui::kit::Rect& points) noexcept {
    const auto& shown = placement.shown;
    if (placement.points_width <= 0 || placement.points_height <= 0)
        return {shown.x, shown.y, 0, 0};
    const int32_t across = shown.width / placement.points_width;
    const int32_t down = shown.height / placement.points_height;
    return {
        shown.x + points.x * across,
        shown.y + points.y * down,
        points.width * across,
        points.height * down
    };
}

// ---------------------------------------------------------------------------------------------
// The layer's own check of screens that are not opaque, and of the information key

namespace {

namespace layout = oa::ui::display_layout;
namespace kit = oa::ui::kit;

/// Throws when a condition fails.
///
/// @param ok the condition
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("OA layer check: " + std::string(what));
}

/// A point of the main menu's picture over none of its buttons, where the
/// pointer rests while the check reads frames back.
constexpr layout::Point kCheckPointer{4, 240};

/// A window the check shows the main menu in, and the scale the layer gives
/// a point there.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< the window's height, in pixels
    int32_t scale{};  ///< window pixels a point
};

/// The windows: the picture's own 640 by 480, and 2560 by 1440 at 2×, where
/// Auto stays at 2× for Large.
constexpr std::array<CheckWindow, 2> kCheckWindows{{{640, 480, 1}, {2560, 1440, 2}}};

/// The colour of the marks of the screens that are not opaque.
constexpr artless::Rgb kClearColour{0xe0, 0x38, 0xb0};
/// The colour of the opaque screen's mark.
constexpr artless::Rgb kBoxColour{0x38, 0xc8, 0xe8};

/// The opaque screen's size, in points.
constexpr kit::Point kBoxSize{48, 32};

/// How far a pixel a veil blends may differ, each channel, from the veil
/// blended straight over what lies under it: its opacity is read back to
/// the nearest 255th, and the window blends it in its own arithmetic.
constexpr int32_t kVeilTolerance = 2;

/// A screen the check expects the layer to show, and where.
struct ExpectedScreen {
    const MarkScreen* screen{}; ///< the screen
    LayerPlacement placed{};    ///< where it shows, in window pixels
    int32_t scale{};            ///< window pixels a point
};

/// Draws a screen straight onto a picture, as the layer means it to look:
/// an opaque screen over black across its place, one that is not opaque
/// over what the picture holds.
///
/// @param[in,out] picture the picture
/// @param expected the screen and its place
/// @param left the picture column its place starts at
/// @param first_row the picture row its place starts at
void draw_straight(
    artless::Surface& picture, const ExpectedScreen& expected, int32_t left, int32_t first_row
) {
    const LayerPlacement& placed = expected.placed;
    kit::Canvas canvas{};
    canvas.surface = &picture;
    canvas.placement = {
        left, first_row, expected.scale, {0, 0, placed.points_width, placed.points_height}
    };
    if (expected.screen->opaque())
        artless::fill_source_rect(
            picture,
            canvas.placement,
            {0, 0, placed.points_width, placed.points_height},
            artless::Rgb{0, 0, 0}
        );
    expected.screen->draw(canvas);
}

/// Returns the window pixel under the centre of a pixel of the front end's
/// picture, as the layer stamps its screens onto the picture.
///
/// @param view where the picture lies in the window
/// @param column the picture pixel's column
/// @param row the picture pixel's row
/// @return the window pixel
layout::Point picture_centre(const LayerView& view, int32_t column, int32_t row) noexcept {
    const auto along = [](int32_t pixel, int32_t start, int32_t length, int32_t size) {
        return start + static_cast<int32_t>(
                           (static_cast<int64_t>(2 * pixel + 1) * length) / (2 * int64_t{size})
                       );
    };
    return {
        along(column, view.picture.x, view.picture.width, view.picture_size.x),
        along(row, view.picture.y, view.picture.height, view.picture_size.y)
    };
}

/// Tells whether a rectangle holds a pixel.
///
/// @param rect the rectangle
/// @param pixel the pixel
/// @return true inside
bool holds_pixel(const layout::Rect& rect, layout::Point pixel) noexcept {
    return pixel.x >= rect.x && pixel.y >= rect.y && pixel.x < rect.x + rect.width &&
           pixel.y < rect.y + rect.height;
}

/// A frame the check expects, and the pixels of it a veil blends.
struct ExpectedFrame {
    artless::Surface picture{};     ///< the frame
    std::vector<uint8_t> blended{}; ///< one byte a pixel: nonzero where a veil blends
};

/// Returns the window expected with screens on the layer: the window with
/// none, each screen drawn straight over it at its place, bottom first.
///
/// @param closed the window with no screen on the layer
/// @param screens the screens, bottom first
/// @return the window expected
ExpectedFrame
expected_window(const artless::Surface& closed, const std::vector<ExpectedScreen>& screens) {
    ExpectedFrame expected{closed, {}};
    expected.blended.assign(static_cast<std::size_t>(closed.width) * closed.height, 0);
    for (const ExpectedScreen& screen : screens) {
        draw_straight(expected.picture, screen, screen.placed.shown.x, screen.placed.shown.y);
        const layout::Rect veil = layer_window_rect(screen.placed, screen.screen->look().veil);
        for (int32_t row = std::max(veil.y, 0);
             row < std::min(veil.y + veil.height, static_cast<int32_t>(closed.height));
             ++row)
            for (int32_t column = std::max(veil.x, 0);
                 column < std::min(veil.x + veil.width, static_cast<int32_t>(closed.width));
                 ++column)
                expected.blended
                    [static_cast<std::size_t>(row) * closed.width +
                     static_cast<std::size_t>(column)] = 1;
    }
    return expected;
}

/// Returns the frame without the cursor expected with screens on the
/// layer: the frame with none, and each pixel whose centre lies in a
/// screen's place with the screens drawn straight over it there, bottom
/// first.
///
/// @param closed the frame with no screen on the layer
/// @param view where the picture lies in the window
/// @param screens the screens, bottom first
/// @return the frame expected
ExpectedFrame expected_picture(
    const artless::Surface& closed,
    const LayerView& view,
    const std::vector<ExpectedScreen>& screens
) {
    ExpectedFrame expected{closed, {}};
    expected.blended.assign(static_cast<std::size_t>(closed.width) * closed.height, 0);
    artless::Surface probe;
    probe.width = 1;
    probe.height = 1;
    probe.rgb.assign(3U, 0);
    for (int32_t row = 0; row < static_cast<int32_t>(closed.height); ++row)
        for (int32_t column = 0; column < static_cast<int32_t>(closed.width); ++column) {
            const layout::Point centre = picture_centre(view, column, row);
            const std::size_t at =
                static_cast<std::size_t>(row) * closed.width + static_cast<std::size_t>(column);
            uint8_t* pixel = expected.picture.rgb.data() + at * 3U;
            for (const ExpectedScreen& screen : screens) {
                if (!holds_pixel(screen.placed.shown, centre))
                    continue;
                // The screen drawn onto the one pixel under that centre.
                std::copy(pixel, pixel + 3, probe.rgb.begin());
                draw_straight(
                    probe,
                    screen,
                    screen.placed.shown.x - centre.x,
                    screen.placed.shown.y - centre.y
                );
                std::copy(probe.rgb.begin(), probe.rgb.end(), pixel);
                if (holds_pixel(
                        layer_window_rect(screen.placed, screen.screen->look().veil), centre
                    ))
                    expected.blended[at] = 1;
            }
        }
    return expected;
}

/// Counts the pixels in which a frame differs from the one expected:
/// exactly, but by more than kVeilTolerance where a veil blends, every pixel
/// compared but those in the square round the pointer.
///
/// @param presented the frame as read back
/// @param expected the frame expected
/// @param pointer where the pointer rests; a reach of 0 leaves out nothing
/// @return the pixels that differ; every pixel when the sizes differ
std::size_t differences(
    const artless::Surface& presented, const ExpectedFrame& expected, const LayerPointer& pointer
) {
    const artless::Surface& wanted = expected.picture;
    if (presented.width != wanted.width || presented.height != wanted.height ||
        presented.rgb.size() != wanted.rgb.size())
        return static_cast<std::size_t>(wanted.width) * wanted.height;
    std::size_t differing = 0;
    for (uint32_t y = 0; y < presented.height; ++y)
        for (uint32_t x = 0; x < presented.width; ++x) {
            const auto column = static_cast<int32_t>(x);
            const auto row = static_cast<int32_t>(y);
            if (column >= pointer.at.x - pointer.reach && column < pointer.at.x + pointer.reach &&
                row >= pointer.at.y - pointer.reach && row < pointer.at.y + pointer.reach)
                continue;
            const std::size_t at = static_cast<std::size_t>(y) * presented.width + x;
            const int32_t allowed = expected.blended[at] != 0 ? kVeilTolerance : 0;
            for (std::size_t channel = 0; channel < 3U; ++channel)
                if (std::abs(
                        int32_t{presented.rgb[at * 3U + channel]} -
                        int32_t{wanted.rgb[at * 3U + channel]}
                    ) > allowed) {
                    ++differing;
                    break;
                }
        }
    return differing;
}

/// Counts the pixels of the cursor's square in which two window frames differ.
///
/// @param with_screen the window while an OA screen shows
/// @param closed the window with no OA screen open
/// @param pointer where the pointer rests, and how far the cursor may draw
/// @return the pixels of the square that differ; every pixel of it when the
///     frames' sizes differ
std::size_t cursor_square_differences(
    const artless::Surface& with_screen, const artless::Surface& closed, const LayerPointer& pointer
) noexcept {
    if (with_screen.width != closed.width || with_screen.height != closed.height ||
        with_screen.rgb.size() != closed.rgb.size())
        return static_cast<std::size_t>(pointer.reach) * 2U *
               static_cast<std::size_t>(pointer.reach) * 2U;
    std::size_t differing = 0;
    const int32_t left = std::max(pointer.at.x - pointer.reach, 0);
    const int32_t top = std::max(pointer.at.y - pointer.reach, 0);
    const int32_t right =
        std::min(pointer.at.x + pointer.reach, static_cast<int32_t>(with_screen.width));
    const int32_t bottom =
        std::min(pointer.at.y + pointer.reach, static_cast<int32_t>(with_screen.height));
    for (int32_t row = top; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column) {
            const std::size_t at = (static_cast<std::size_t>(row) * with_screen.width +
                                    static_cast<std::size_t>(column)) *
                                   3U;
            if (with_screen.rgb[at] != closed.rgb[at] ||
                with_screen.rgb[at + 1] != closed.rgb[at + 1] ||
                with_screen.rgb[at + 2] != closed.rgb[at + 2])
                ++differing;
        }
    return differing;
}

/// Checks that a match's layer takes a screen that is not opaque by its
/// opacity, src over, its colours straight; and an opaque one as before.
void check_clear_stamp() {
    constexpr int32_t stamp_side = 4;
    constexpr std::array<std::array<uint8_t, 4>, stamp_side> under_columns{{
        {0, 0, 0, 0},         // clear
        {90, 90, 90, 255},    // opaque grey
        {16, 18, 14, 128},    // half dark, as the backdrop
        {250, 250, 250, 255}, // opaque white
    }};
    constexpr artless::Rgb colour{200, 40, 120};
    // The opacity of each row of the source: clear, opaque, half and a quarter.
    constexpr std::array<uint8_t, stamp_side> row_opacity{0, 255, 128, 64};
    std::vector<uint8_t> layer_pixels(static_cast<std::size_t>(stamp_side * stamp_side) * 4U);
    artless::Surface source;
    source.width = static_cast<uint32_t>(stamp_side);
    source.height = static_cast<uint32_t>(stamp_side);
    std::vector<uint8_t> opacity(static_cast<std::size_t>(stamp_side * stamp_side));
    for (int32_t row = 0; row < stamp_side; ++row)
        for (int32_t column = 0; column < stamp_side; ++column) {
            const auto at = static_cast<std::size_t>(row * stamp_side + column);
            std::copy(
                under_columns[static_cast<std::size_t>(column)].begin(),
                under_columns[static_cast<std::size_t>(column)].end(),
                layer_pixels.begin() + static_cast<std::ptrdiff_t>(at * 4U)
            );
            source.rgb.insert(source.rgb.end(), colour.begin(), colour.end());
            opacity[at] = row_opacity[static_cast<std::size_t>(row)];
        }
    const layout::Rect whole{0, 0, stamp_side, stamp_side};

    std::vector<uint8_t> laid = layer_pixels;
    OaLayer::stamp(laid, stamp_side, stamp_side, source, whole, opacity);
    for (std::size_t at = 0; at < opacity.size(); ++at) {
        const uint8_t* under = layer_pixels.data() + at * 4U;
        const uint8_t* now = laid.data() + at * 4U;
        const double over = opacity[at] / 255.0;
        const double kept = under[3] / 255.0 * (1.0 - over);
        const double together = over + kept;
        require(
            std::abs(now[3] - std::lround(together * 255.0)) <= 1,
            "a match's layer does not take a pixel's opacity laid over its own"
        );
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const double straight =
                together > 0.0 ? (colour[channel] * over + under[channel] * kept) / together
                               : under[channel];
            require(
                std::abs(now[channel] - std::lround(straight)) <= 1,
                "a match's layer does not lay a screen's colour over its own, straight"
            );
        }
    }
    // Over a clear pixel the colour stays as it is: never multiplied by its
    // opacity.
    require(
        laid[(2 * stamp_side) * 4U] == colour[0] && laid[(2 * stamp_side) * 4U + 3U] == 128,
        "a match's layer multiplied a colour by its opacity"
    );

    std::vector<uint8_t> copied = layer_pixels;
    OaLayer::stamp(copied, stamp_side, stamp_side, source, whole);
    for (std::size_t at = 0; at < opacity.size(); ++at)
        require(
            copied[at * 4U] == colour[0] && copied[at * 4U + 1U] == colour[1] &&
                copied[at * 4U + 2U] == colour[2] && copied[at * 4U + 3U] == 255U,
            "a match's layer does not copy an opaque screen whole"
        );
}

} // namespace

void OaLayer::check_clear_screens() {
    require(
        runtime_.sdl_.renderer != nullptr && runtime_.sdl_.window != nullptr,
        "needs the SDL presenter"
    );
    require(runtime_.gamma_identity_, "compares frames at a display gamma that changes no colour");
    require(screens_.empty() && queue_.empty(), "screens were on the layer before the check");
    std::cout << "OA layer check: screens that are not opaque, and the information key\n";

    // F1 is the information key outside a match, and the game's in one.
    require(
        layer_key(SDLK_F1, SDL_KMOD_NONE) == kit::Key::info &&
            layer_key(SDLK_F1, SDL_KMOD_SHIFT) == kit::Key::info,
        "F1 is not the information key outside a match"
    );
    require(
        !layer_key(SDLK_F1, SDL_KMOD_NONE, true).has_value() &&
            layer_key(SDLK_ESCAPE, SDL_KMOD_NONE, true) == kit::Key::escape,
        "F1 does not go on to the game in a match"
    );
    check_clear_stamp();

    const auto previous_tick = runtime_.fake_frontend_tick_;
    runtime_.fake_frontend_tick_ = 1000U;
    int kept_width = 0;
    int kept_height = 0;
    require(
        SDL_GetWindowSize(runtime_.sdl_.window, &kept_width, &kept_height) && kept_width > 0 &&
            kept_height > 0,
        "the window has no size"
    );

    // Every frame read back holds the whole window.
    struct WholeWindow {
        OaLayer& layer;

        ~WholeWindow() { layer.read_whole_window(false); }
    } whole_window{*this};

    read_whole_window(true);

    for (const CheckWindow& checked : kCheckWindows) {
        const std::string size_name =
            std::to_string(checked.width) + 'x' + std::to_string(checked.height);
        const std::string on = " on the " + size_name + " window";
        require(
            SDL_SetWindowSize(runtime_.sdl_.window, checked.width, checked.height) &&
                SDL_SyncWindow(runtime_.sdl_.window),
            "the window did not take " + size_name
        );
        runtime_.load(Screen::main_menu);
        require(runtime_.screen_ == Screen::main_menu, "the main menu did not open" + on);
        runtime_.send_check_pointer(SDL_EVENT_MOUSE_MOTION, kCheckPointer, 0);
        const LayerView seen = view();
        require(
            seen.scale == checked.scale && seen.room.x == 0 && seen.room.y == 0 &&
                seen.room.width == checked.width && seen.room.height == checked.height,
            "the layer does not place its screens over the whole window at " +
                std::to_string(checked.scale) + "x" + on
        );
        // The window's frame and the frame without the cursor, drawn from
        // the same sparks.
        const auto sparks = runtime_.menu_sparks_;
        const auto read_frames =
            [this, &sparks](artless::Surface& window_read, artless::Surface& picture_read) {
                runtime_.menu_sparks_ = sparks;
                picture_read = runtime_.frame_without_cursor();
                runtime_.menu_sparks_ = sparks;
                runtime_.capture_frame_ = &window_read;
                runtime_.render();
                runtime_.capture_frame_ = nullptr;
            };
        artless::Surface closed_window;
        artless::Surface closed_picture;
        read_frames(closed_window, closed_picture);
        require(
            closed_window.width == static_cast<uint32_t>(checked.width) &&
                closed_window.height == static_cast<uint32_t>(checked.height),
            "the read-back does not hold the whole window" + on
        );
        const LayerPointer resting = window_pointer(seen, kCheckPointer);
        const LayerPointer no_cursor{};

        // A screen that draws nothing still presents the cursor above the
        // layer. Its square matches the cursor drawn in the picture when no
        // screen shows, at the picture's own scaling.
        {
            MarkLook bare{};
            bare.name = "check-cursor-scale";
            push(std::make_unique<MarkScreen>(bare, std::make_shared<MarkSeen>()));
            require(shows(false), "a screen that draws nothing does not show" + on);
            artless::Surface window_now;
            artless::Surface picture_now;
            read_frames(window_now, picture_now);
            const std::size_t cursor_differing =
                cursor_square_differences(window_now, closed_window, resting);
            require(
                cursor_differing == 0,
                "the cursor above a screen does not keep the picture's scaling" + on + ": " +
                    std::to_string(cursor_differing) + " pixels of its square differ"
            );
            close(find(bare.name));
            require(!shows(false), "the screen under the cursor stayed open" + on);
        }

        // The window and the frame without the cursor hold the closed ones
        // with every screen on the layer drawn straight over them.
        const auto shows_straight = [&](std::string_view what) {
            std::vector<ExpectedScreen> expected_screens;
            for (const auto& held : screens_) {
                const auto* marked = dynamic_cast<const MarkScreen*>(held.get());
                require(marked != nullptr, "a screen other than the check's is on the layer");
                expected_screens.push_back({marked, marked->placement(seen), seen.scale});
            }
            artless::Surface window_now;
            artless::Surface picture_now;
            read_frames(window_now, picture_now);
            const std::size_t window_differing =
                differences(window_now, expected_window(closed_window, expected_screens), resting);
            require(
                window_differing == 0,
                "the window does not show " + std::string(what) + on + ": " +
                    std::to_string(window_differing) + " pixels differ"
            );
            const std::size_t picture_differing = differences(
                picture_now, expected_picture(closed_picture, seen, expected_screens), no_cursor
            );
            require(
                picture_differing == 0,
                "the frame without the cursor does not show " + std::string(what) + on + ": " +
                    std::to_string(picture_differing) + " pixels differ"
            );
        };
        // A point of the view's room over a part of the picture, a share of
        // its width and height in.
        const auto room_point = [&seen](int32_t eighths_across, int32_t eighths_down) {
            return kit::Point{
                (seen.picture.x + seen.picture.width * eighths_across / 8 - seen.room.x) /
                    seen.scale,
                (seen.picture.y + seen.picture.height * eighths_down / 8 - seen.room.y) / seen.scale
            };
        };
        const kit::Point upper = room_point(3, 2);
        const kit::Point middle = room_point(4, 4);

        // A screen that is not opaque over the whole window, modal and
        // asking for a backdrop: only its mark and its veil change the
        // window, and nothing darkens. Automation lists its mark where the
        // window shows it.
        MarkLook clear{};
        clear.name = "check-clear";
        clear.modal = true;
        clear.backdrop = true;
        clear.mark = {upper.x, upper.y, 12, 8};
        clear.veil = {upper.x + 20, upper.y, 12, 8};
        clear.colour = kClearColour;
        push(std::make_unique<MarkScreen>(clear, std::make_shared<MarkSeen>()));
        const LayerPlacement covering = placement_of(clear.name);
        require(
            covering.shown.x == 0 && covering.shown.y == 0 &&
                covering.shown.width == checked.width && covering.shown.height == checked.height,
            "the screen that is not opaque does not cover the window" + on
        );
        shows_straight("a screen that is not opaque");
        {
            std::vector<AutomationControl> controls;
            automation_controls(controls);
            const layout::Rect marked = layer_window_rect(covering, clear.mark);
            const auto listed = std::find_if(
                controls.begin(), controls.end(), [](const AutomationControl& control) {
                    return control.name == "oa.mark" && control.dialog == "oa.check-clear";
                }
            );
            require(
                listed != controls.end() && listed->window_pixels && listed->visible &&
                    listed->window_x == marked.x && listed->window_y == marked.y &&
                    listed->window_width == marked.width && listed->window_height == marked.height,
                "automation does not list the mark of a screen that is not opaque where the "
                "window shows it" +
                    on
            );
        }
        close(find(clear.name));

        // The software cursor shows above a screen that is not opaque: a mark
        // across the cursor's square changes no pixel outside the square,
        // and the cursor still shows over the mark inside it.
        {
            MarkLook under_cursor{};
            under_cursor.name = "check-cursor";
            under_cursor.colour = kClearColour;
            const int32_t point_pixels = seen.scale;
            const int32_t left = std::max(resting.at.x - resting.reach, 0) / point_pixels;
            const int32_t first_row = std::max(resting.at.y - resting.reach, 0) / point_pixels;
            under_cursor.mark = {
                left,
                first_row,
                (resting.at.x + resting.reach + point_pixels - 1) / point_pixels - left,
                (resting.at.y + resting.reach + point_pixels - 1) / point_pixels - first_row
            };
            push(std::make_unique<MarkScreen>(under_cursor, std::make_shared<MarkSeen>()));
            shows_straight("a screen that is not opaque under the cursor");
            const auto* cursor_screen = dynamic_cast<const MarkScreen*>(find(under_cursor.name));
            require(
                cursor_screen != nullptr, "the screen under the cursor is not on the layer" + on
            );
            artless::Surface window_now;
            artless::Surface picture_now;
            read_frames(window_now, picture_now);
            const std::size_t cursor_pixels = differences(
                window_now,
                expected_window(
                    closed_window, {{cursor_screen, cursor_screen->placement(seen), seen.scale}}
                ),
                no_cursor
            );
            require(
                cursor_pixels > 0, "the cursor does not show above a screen that is not opaque" + on
            );
            close(find(under_cursor.name));
        }

        // A notice over the darkened main menu has rounded corners: outside
        // them the window shows the darkened menu, as expected_layer_frame
        // and round_drawn_screen expect.
        {
            kit::Notice told;
            told.title = "ROUNDED";
            told.paragraphs.push_back({"A notice of the layer's check.", false});
            push(
                std::make_unique<NoticeScreen>(
                    *this, Screen::main_menu, std::move(told), NoticeScreen::Host{}
                )
            );
            lay_out_screens();
            const LayerScreen* notice = find(NoticeScreen::screen_name);
            require(notice != nullptr, "the check's notice is not on the layer" + on);
            const LayerPlacement noticed = placement_of(NoticeScreen::screen_name);
            require(
                noticed.corner_radius == kit::metrics_of(seen.frame.size_class).dialog_radius,
                "the notice does not take its class's dialog radius" + on
            );
            const artless::Surface drawing = layer_drawing(
                noticed.points_width,
                noticed.points_height,
                noticed.scale_percent,
                runtime_.engine_settings_fonts(),
                nullptr,
                runtime_.engine_settings_icon(),
                [notice](const kit::Canvas& canvas) { notice->draw(canvas); }
            );
            artless::Surface window_now;
            artless::Surface picture_now;
            read_frames(window_now, picture_now);
            const artless::Surface expected =
                expected_layer_frame(closed_window, drawing, noticed, 1);
            // Over the notice's place: the window's own buttons elsewhere are
            // drawn in its pixels and darkened apart, which this step is not
            // about.
            std::size_t differing = 0;
            for (int32_t row = std::max(noticed.shown.y, 0);
                 row <
                 std::min(
                     noticed.shown.y + noticed.shown.height, static_cast<int32_t>(window_now.height)
                 );
                 ++row)
                for (int32_t column = std::max(noticed.shown.x, 0);
                     column < std::min(
                                  noticed.shown.x + noticed.shown.width,
                                  static_cast<int32_t>(window_now.width)
                              );
                     ++column) {
                    if (column >= resting.at.x - resting.reach &&
                        column < resting.at.x + resting.reach &&
                        row >= resting.at.y - resting.reach && row < resting.at.y + resting.reach)
                        continue;
                    const std::size_t at = (static_cast<std::size_t>(row) * window_now.width +
                                            static_cast<std::size_t>(column)) *
                                           3U;
                    if (window_now.rgb.size() != expected.rgb.size() ||
                        !std::equal(
                            window_now.rgb.begin() + static_cast<std::ptrdiff_t>(at),
                            window_now.rgb.begin() + static_cast<std::ptrdiff_t>(at + 3U),
                            expected.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                        ))
                        ++differing;
                }
            if (differing != 0) {
                // Beside the check, to compare.
                write_ppm("oa-layer-rounded-notice-" + size_name + "-window.ppm", window_now);
                write_ppm("oa-layer-rounded-notice-" + size_name + "-expected.ppm", expected);
            }
            require(
                differing == 0,
                "the window does not show the notice with rounded corners" + on + ": " +
                    std::to_string(differing) + " pixels differ"
            );
            // A check that draws the notice straight into its frame and
            // rounds it after expects the same.
            const artless::Surface darkened =
                expected_layer_frame(closed_window, {}, LayerPlacement{}, 1);
            artless::Surface straight = darkened;
            for (int32_t row = 0; row < noticed.shown.height; ++row)
                std::copy_n(
                    drawing.rgb.data() + static_cast<std::size_t>(row) * drawing.width * 3U,
                    static_cast<std::size_t>(noticed.shown.width) * 3U,
                    straight.rgb.data() +
                        (static_cast<std::size_t>(noticed.shown.y + row) * straight.width +
                         static_cast<std::size_t>(noticed.shown.x)) *
                            3U
                );
            round_drawn_screen(straight, darkened, noticed);
            require(
                straight.rgb == expected.rgb,
                "a notice drawn straight and rounded is not the layer's own" + on
            );
            close(find(NoticeScreen::screen_name));
        }

        // An opaque screen draws as before: its whole place, black where it
        // draws nothing.
        MarkLook box{};
        box.name = "check-box";
        box.opaque = true;
        box.area = {middle.x, middle.y, kBoxSize.x, kBoxSize.y};
        box.mark = {4, 4, 12, 8};
        box.colour = kBoxColour;
        push(std::make_unique<MarkScreen>(box, std::make_shared<MarkSeen>()));
        shows_straight("an opaque screen");
        {
            // The same picture as the layer's own expectation of an opaque
            // screen (expected_layer_frame) gives.
            const LayerPlacement boxed = placement_of(box.name);
            artless::Surface drawing;
            drawing.width = static_cast<uint32_t>(boxed.shown.width);
            drawing.height = static_cast<uint32_t>(boxed.shown.height);
            drawing.rgb.assign(static_cast<std::size_t>(drawing.width) * drawing.height * 3U, 0);
            const auto* box_screen = dynamic_cast<const MarkScreen*>(find(box.name));
            require(box_screen != nullptr, "the opaque screen is not on the layer" + on);
            draw_straight(drawing, {box_screen, boxed, seen.scale}, 0, 0);
            artless::Surface window_now;
            artless::Surface picture_now;
            read_frames(window_now, picture_now);
            const std::size_t differing = layer_differences(
                window_now, expected_layer_frame(closed_window, drawing, boxed, 0), resting
            );
            require(
                differing == 0,
                "the window does not show an opaque screen as before" + on + ": " +
                    std::to_string(differing) + " pixels differ"
            );
        }

        // A screen that is not opaque over the opaque one: its mark half over
        // it, half over the picture, and its veil over the opaque screen.
        MarkLook over = clear;
        over.name = "check-over";
        over.modal = false;
        over.backdrop = false;
        over.mark = {middle.x - 6, middle.y + 12, 12, 8};
        over.veil = {middle.x + 24, middle.y + 12, 12, 8};
        push(std::make_unique<MarkScreen>(over, std::make_shared<MarkSeen>()));
        shows_straight("a screen that is not opaque over an opaque one");
        close(find(over.name));
        close(find(box.name));

        // And under it: the opaque screen covers it where they meet.
        push(std::make_unique<MarkScreen>(over, std::make_shared<MarkSeen>()));
        push(std::make_unique<MarkScreen>(box, std::make_shared<MarkSeen>()));
        shows_straight("an opaque screen over one that is not opaque");
        close(find(box.name));
        close(find(over.name));
        require(screens_.empty(), "the check's screens did not close" + on);
        std::cout << "OA layer check: screens that are not opaque leave every other pixel at "
                  << checked.scale << "x" << on << '\n';
    }

    // The information key, on the main menu.
    {
        const auto tap = [this](SDL_Keycode code) {
            bool running = true;
            for (const bool down : {true, false}) {
                SDL_Event event{};
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.windowID = SDL_GetWindowID(runtime_.sdl_.window);
                event.key.key = code;
                event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
                event.key.mod = SDL_KMOD_NONE;
                event.key.down = down;
                runtime_.dispatch_event(event, running);
            }
            require(running, "a key ended the run");
        };
        ScreenInput f1{};
        f1.kind = ScreenInputKind::key_down;
        f1.key = SDLK_F1;
        require(!take_input(f1), "F1 did not go on with no screen on the layer");

        // A screen that takes the information key is given F1 as it, and
        // the layer takes F1.
        MarkLook taking{};
        taking.name = "check-info";
        taking.mark = {0, 0, 8, 8};
        taking.colour = kClearColour;
        taking.takes_info = true;
        auto taken = std::make_shared<MarkSeen>();
        push(std::make_unique<MarkScreen>(taking, taken));
        tap(SDLK_F1);
        require(
            taken->infos == 1 && taken->info_key == SDLK_F1 && taken->other_keys == 0,
            "F1 did not reach a screen that takes the information key as that key"
        );
        require(
            take_input(f1) && taken->infos == 2,
            "the layer did not take F1 that a screen took as the information key"
        );
        close(find(taking.name));

        // One that lets it go on sees it, and F1 goes on to the game.
        MarkLook passing = taking;
        passing.name = "check-info-pass";
        passing.takes_info = false;
        auto passed = std::make_shared<MarkSeen>();
        push(std::make_unique<MarkScreen>(passing, passed));
        require(
            !take_input(f1) && passed->infos == 1 && passed->info_key == SDLK_F1,
            "F1 did not go on past a screen that lets the information key go on"
        );
        close(find(passing.name));

        // Settings has nothing more to tell: F1 changes nothing in it, and
        // nothing under it sees F1.
        runtime_.open_engine_settings_from_menu();
        const auto* dialog = runtime_.engine_settings_dialog();
        require(
            dialog != nullptr && find("settings") != nullptr,
            "Settings did not open on the main menu"
        );
        const auto focused = dialog->focused;
        const auto page = dialog->page;
        tap(SDLK_F1);
        require(
            runtime_.engine_settings_dialog() == dialog && dialog->focused == focused &&
                dialog->page == page,
            "F1 changed Settings"
        );
        require(take_input(f1), "F1 went on past Settings");
        tap(SDLK_ESCAPE);
        require(
            runtime_.engine_settings_dialog() == nullptr && find("settings") == nullptr,
            "Escape did not close Settings"
        );
        std::cout << "OA layer check: F1 reaches a screen that takes the information key, goes on "
                     "past one that does not and past an empty layer, and changes nothing in "
                     "Settings\n";
    }

    // In a match, over the in-game menu: F1 is the game's, which no screen
    // is given, and a screen that is not opaque, modal and asking for a
    // backdrop, is stamped into the match's picture by its opacity, changing
    // no other pixel and darkening nothing.
    {
        runtime_.start_benchmark_skirmish();
        require(
            runtime_.screen_ == Screen::match && view().match, "the check's match did not start"
        );
        runtime_.show_match_pause_menu();
        require(runtime_.ingame_menu_column_shown(), "the in-game menu's column does not show");
        runtime_.render();
        const auto composed = [this] {
            artless::Surface frame;
            runtime_.compose_match_frame(frame);
            return frame;
        };
        const artless::Surface closed_match = composed();
        require(
            differences(
                composed(),
                {closed_match,
                 std::vector<uint8_t>(
                     static_cast<std::size_t>(closed_match.width) * closed_match.height, 0
                 )},
                {}
            ) == 0,
            "the match's frame changes from one composition to the next"
        );
        const LayerView seen = view();
        // Right of the side column, clear of the in-game menu.
        const kit::Point beside{seen.match_layout->left - seen.room.x + 40, 40 - seen.room.y};

        MarkLook taking{};
        taking.name = "check-match-info";
        taking.in_match = true;
        taking.area = {beside.x, beside.y + 40, 8, 8};
        taking.mark = {0, 0, 8, 8};
        taking.colour = kClearColour;
        taking.takes_info = true;
        auto taken = std::make_shared<MarkSeen>();
        push(std::make_unique<MarkScreen>(taking, taken));
        require(
            placement_of(taking.name).shown.width > 0, "the check's screen does not show in a match"
        );
        ScreenInput f1{};
        f1.kind = ScreenInputKind::key_down;
        f1.key = SDLK_F1;
        require(
            !take_input(f1) && taken->infos == 0,
            "F1 did not go on to the game in a match past a screen that takes the information key"
        );
        close(find(taking.name));

        MarkLook clear{};
        clear.name = "check-match-clear";
        clear.in_match = true;
        clear.modal = true;
        clear.backdrop = true;
        clear.mark = {beside.x, beside.y, 12, 8};
        clear.veil = {beside.x + 20, beside.y, 12, 8};
        clear.colour = kClearColour;
        push(std::make_unique<MarkScreen>(clear, std::make_shared<MarkSeen>()));
        const auto* clear_screen = dynamic_cast<const MarkScreen*>(find(clear.name));
        require(clear_screen != nullptr, "the check's screen is not on the layer in a match");
        const LayerPlacement covering = clear_screen->placement(seen);
        require(
            covering.shown.width == seen.room.width && covering.shown.height == seen.room.height,
            "the screen that is not opaque does not cover the match"
        );
        const std::size_t differing = differences(
            composed(), expected_window(closed_match, {{clear_screen, covering, 1}}), {}
        );
        require(
            differing == 0,
            "the match's frame does not show a screen that is not opaque: " +
                std::to_string(differing) + " pixels differ"
        );
        close(find(clear.name));
        runtime_.leave_match();
        std::cout << "OA layer check: in a match F1 goes on to the game, and a screen that is not "
                     "opaque changes only the pixels it draws\n";
    }

    // Settings moves by its title on the main menu of a 640 by 480 window,
    // at 1x, so that its points are the window's pixels: a drag keeps its
    // place for Compact in the preferences, a double-click on the title
    // centres it again, Alt and the arrows move it 8 points (32 with Shift),
    // and Reset window positions puts it back.
    {
        require(
            SDL_SetWindowSize(runtime_.sdl_.window, kCanvasWidth, kCanvasHeight) &&
                SDL_SyncWindow(runtime_.sdl_.window),
            "the window did not take 640x480 to move Settings"
        );
        runtime_.load(Screen::main_menu);
        static_cast<void>(reset_window_places());
        runtime_.open_engine_settings_from_menu();
        const LayerScreen* settings_screen = find("settings");
        require(
            settings_screen != nullptr && runtime_.engine_settings_dialog() != nullptr,
            "Settings did not open to be moved"
        );
        const LayerPlacement centred = placement_of("settings");
        require(
            view().scale_percent == 100 && window_offset("settings").x == 0 &&
                window_offset("settings").y == 0,
            "Settings is not centred at 1x before it is moved"
        );
        // A point of the title that no control holds.
        const kit::DisplayList list = settings_screen->display_list();
        const auto header =
            std::find_if(list.items.begin(), list.items.end(), [](const auto& item) {
                return item.role == kit::Role::header;
            });
        require(header != list.items.end(), "Settings has no title to be moved by");
        kit::Point grip{-1, -1};
        for (int32_t x = header->rect.x + 2;
             x < header->rect.x + header->rect.width - 2 && grip.x < 0;
             ++x) {
            const kit::Point at{x, header->rect.y + header->rect.height / 2};
            if (kit::hit(list, at) == kit::no_control)
                grip = at;
        }
        require(grip.x >= 0, "every point of Settings' title is a control");
        const std::string key = window_placement::place_key("settings", kit::SizeClass::compact);
        const auto stored = [this, &key]() -> std::string {
            const auto found = runtime_.preference_values_.find(key);
            return found == runtime_.preference_values_.end() ? std::string{} : found->second;
        };
        const auto shows_at = [this, &centred](int32_t across, int32_t down) {
            const LayerPlacement now = placement_of("settings");
            return now.shown.x == centred.shown.x + across &&
                   now.shown.y == centred.shown.y + down &&
                   now.shown.width == centred.shown.width &&
                   now.shown.height == centred.shown.height;
        };
        const layout::Point pressed{centred.shown.x + grip.x, centred.shown.y + grip.y};
        const layout::Point dropped{pressed.x + 60, pressed.y + 40};
        runtime_.send_check_pointer(SDL_EVENT_MOUSE_MOTION, pressed, 0);
        runtime_.send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, pressed, SDL_BUTTON_LEFT);
        runtime_.send_check_pointer(SDL_EVENT_MOUSE_MOTION, dropped, 0);
        runtime_.send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, dropped, SDL_BUTTON_LEFT);
        require(
            runtime_.engine_settings_dialog() != nullptr && shows_at(60, 40) && stored() == "60,40",
            "a drag by its title did not move Settings 60 by 40 points and keep its place"
        );
        // A double-click on the title centres it again and forgets the place.
        for (const uint8_t clicks : {uint8_t{1}, uint8_t{2}}) {
            runtime_.send_check_pointer(
                SDL_EVENT_MOUSE_BUTTON_DOWN, dropped, SDL_BUTTON_LEFT, clicks
            );
            runtime_.send_check_pointer(
                SDL_EVENT_MOUSE_BUTTON_UP, dropped, SDL_BUTTON_LEFT, clicks
            );
        }
        require(
            runtime_.engine_settings_dialog() != nullptr && shows_at(0, 0) && stored().empty(),
            "a double-click on its title did not centre Settings again"
        );
        // Alt and the arrows move the window that takes the keys.
        const auto press = [this](SDL_Keycode code, SDL_Keymod modifiers) {
            bool running = true;
            for (const bool down : {true, false}) {
                SDL_Event event{};
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.windowID = SDL_GetWindowID(runtime_.sdl_.window);
                event.key.key = code;
                event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
                event.key.mod = modifiers;
                event.key.down = down;
                runtime_.dispatch_event(event, running);
            }
            require(running, "a key ended the run");
        };
        press(SDLK_RIGHT, SDL_KMOD_LALT);
        press(SDLK_DOWN, static_cast<SDL_Keymod>(SDL_KMOD_LALT | SDL_KMOD_LSHIFT));
        require(
            runtime_.engine_settings_dialog() != nullptr && shows_at(8, 32) && stored() == "8,32",
            "Alt and Right, then Alt, Shift and Down did not move Settings 8 and 32 points"
        );
        require(
            reset_window_places() && shows_at(0, 0) && stored().empty(),
            "Reset window positions did not centre Settings again"
        );
        require(
            placement_of("settings").corner_radius == kit::compact_metrics.window_radius,
            "Settings at Compact does not take Compact's window radius"
        );
        // Accessibility's practice pad takes S, pressed and let go: it shows
        // there, and the main menu, whose S opens the single player screen,
        // never sees it.
        {
            oa::ui::engine_settings::Dialog* dialog = runtime_.engine_settings_dialog();
            dialog->page = oa::ui::engine_settings::Page::accessibility;
            redraw();
            press(SDLK_S, SDL_KMOD_NONE);
            require(
                runtime_.engine_settings_dialog() == dialog &&
                    runtime_.screen_ == Screen::main_menu,
                "S on the practice pad left Settings or reached the main menu"
            );
            require(
                dialog->practice.outcome_key == "S" &&
                    dialog->practice.outcome == oa::ui::engine_settings::PracticeOutcome::counted &&
                    dialog->practice.key_down.empty(),
                "the practice pad does not show S pressed and let go"
            );
            dialog->page = oa::ui::engine_settings::Page::controls;
            redraw();
            std::cout << "OA layer check: S on Accessibility's practice pad shows there and "
                         "does not reach the main menu\n";
        }
        press(SDLK_ESCAPE, SDL_KMOD_NONE);
        require(
            runtime_.engine_settings_dialog() == nullptr, "Escape did not close the moved Settings"
        );
        std::cout << "OA layer check: Settings moves by its title and keeps its place, a "
                     "double-click centres it, Alt and the arrows move it, and a reset puts it "
                     "back\n";
    }

    {
        // Rounded corners: a window's drawing of one colour, its corners
        // rounded at 1x, 1.5x and 2x. Outside each quarter circle the pixels
        // are clear, along it the strong edge at the top left and the dark
        // edge at the bottom right, and everything else as drawn.
        const kit::Colour face = kit::colour::panel;
        constexpr int32_t radius_points = 10;
        for (const int32_t percent : {100, 150, 200}) {
            const kit::Rect size = kit::to_canvas({0, 0, 60, 40}, percent);
            const int32_t radius = kit::to_canvas({0, 0, radius_points, 0}, percent).width;
            LayerDrawing drawn;
            drawn.picture.width = static_cast<uint32_t>(size.width);
            drawn.picture.height = static_cast<uint32_t>(size.height);
            for (int32_t pixel = 0; pixel < size.width * size.height; ++pixel)
                drawn.picture.rgb.insert(drawn.picture.rgb.end(), {face.r, face.g, face.b});
            round_layer_corners(drawn, radius_points, percent);
            const std::string at = " at " + std::to_string(percent) + "%";
            require(
                drawn.opacity.size() == static_cast<std::size_t>(size.width * size.height),
                "a rounded drawing has no opacity for each pixel" + at
            );
            const auto pixel_at = [&](int32_t column, int32_t row) {
                return static_cast<std::size_t>(row) * static_cast<std::size_t>(size.width) +
                       static_cast<std::size_t>(column);
            };
            const auto colour_is = [&](std::size_t pixel, const kit::Colour& ink) {
                const uint8_t* rgb = drawn.picture.rgb.data() + pixel * 3U;
                return rgb[0] == ink.r && rgb[1] == ink.g && rgb[2] == ink.b;
            };
            int32_t light = 0;
            int32_t dark = 0;
            for (int32_t row = 0; row < size.height; ++row)
                for (int32_t column = 0; column < size.width; ++column) {
                    const std::size_t pixel = pixel_at(column, row);
                    const int32_t across = std::min(column, size.width - 1 - column);
                    const int32_t down = std::min(row, size.height - 1 - row);
                    const bool in_corner = across < radius && down < radius;
                    if (!in_corner) {
                        require(
                            drawn.opacity[pixel] == 255 && colour_is(pixel, face),
                            "a pixel outside the corners changed" + at
                        );
                        continue;
                    }
                    if (drawn.opacity[pixel] == 0)
                        continue;
                    require(
                        drawn.opacity[pixel] == 255,
                        "a rounded corner has a pixel neither clear nor opaque" + at
                    );
                    const bool top_left = column < radius && row < radius;
                    const bool bottom_right =
                        column >= size.width - radius && row >= size.height - radius;
                    if (colour_is(pixel, kit::colour::edge_light)) {
                        require(!bottom_right, "the bottom right corner has the strong edge" + at);
                        ++light;
                    } else if (colour_is(pixel, kit::colour::edge_dark)) {
                        require(!top_left, "the top left corner has the dark edge" + at);
                        ++dark;
                    } else {
                        require(
                            colour_is(pixel, face), "a corner's pixel is neither edge nor face" + at
                        );
                    }
                }
            for (const auto& [column, row] : std::array<std::pair<int32_t, int32_t>, 4>{
                     {{0, 0},
                      {size.width - 1, 0},
                      {0, size.height - 1},
                      {size.width - 1, size.height - 1}}
                 })
                require(
                    drawn.opacity[pixel_at(column, row)] == 0, "a corner's pixel is not clear" + at
                );
            require(light > 0 && dark > 0, "the corners have no edge drawn round them" + at);
        }
        std::cout << "OA layer check: a window's and a dialog's corners are clear outside the "
                     "quarter circle and edged along it, at 1x, 1.5x and 2x\n";
    }

    require(
        SDL_SetWindowSize(runtime_.sdl_.window, kept_width, kept_height) &&
            SDL_SyncWindow(runtime_.sdl_.window),
        "the window did not take its size back"
    );
    runtime_.load(Screen::main_menu);
    forget_latched_key();
    runtime_.fake_frontend_tick_ = previous_tick;
}

} // namespace oa::app
