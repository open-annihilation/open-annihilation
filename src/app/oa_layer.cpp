// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The OA layer (oa_layer.hpp): its stack of screens and the queue of those
// waiting for it, their input and ticks, its drawing over the front end's
// picture and over a match, the overlay that hands it the game's input and
// frames, the settings dialog as one of its screens (Runtime::SettingsScreen),
// on the main menu and in a match, the screens of a notice and a question
// (NoticeScreen, QuestionScreen), and the UI kit gallery over the whole
// window (GalleryScreen). On the front end the screens are laid out from the
// window's size in points and drawn at its pixel density, in the window's own
// pixels: crisp at a whole scale, smooth at 150%. A 2× window shows the same
// arrangement as that window at one pixel per point, with sharper edges.

#include "oa_layer.hpp"

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "render_state.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "oa/app/render_policy.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/window_surface.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/gallery.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace layout = oa::ui::display_layout;
namespace kit = oa::ui::kit;
namespace artless = oa::ui::frontend_renderer;
namespace paint = oa::ui::paint;
namespace text_font = oa::platform::text_font;
namespace mp = oa::ui::frontend_multiplayer;

namespace {

/// The layer's overlay's z: over the extensions' overlays, under the
/// frontend's message boxes.
constexpr int16_t kLayerOverlayZ = 100;

/// The settings' screen's name.
constexpr std::string_view kSettingsName = "settings";
/// Arrange overlays' button's words in the in-game menu, where its title
/// does not fit.
constexpr std::string_view kArrangeWords = "Overlays…";
/// Arrange overlays' button's automation name, after the oa. prefix.
constexpr std::string_view kArrangeButtonName = "arrange-button";

/// The match's layer's opacity where it darkens the screen, in 255ths.
constexpr uint32_t kBackdropAlpha = (kit::ingame_backdrop_opacity * 255U + 128U) / 256U;

/// The sound opening the settings and OK play.
constexpr std::string_view kOpenSound = "Options";
/// The sound Cancel plays.
constexpr std::string_view kCancelSound = "Previous";

/// Scales a source length by the side column's scale.
///
/// @param value source pixels
/// @param scale the side column's scale
/// @return window pixels, at least 1
int32_t scaled_length(int32_t value, double scale) noexcept {
    return std::max(1, static_cast<int32_t>(std::lround(static_cast<double>(value) * scale)));
}

/// Returns the scale the side column is drawn at: the chrome's, or the
/// smaller one a side-column page taller than the window gives it
/// (display_layout::fit_side_column).
///
/// @param laid_out the match's layout
/// @return canvas pixels per source pixel of the side column
double column_scale(const layout::MatchLayout& laid_out) noexcept {
    return laid_out.column_narrowed() ? laid_out.column_scale : laid_out.scale;
}

/// Tells whether a point lies in a rectangle.
///
/// @param rect the rectangle
/// @param x the point's column
/// @param y the point's row
/// @return true inside
bool rect_contains(const layout::Rect& rect, float x, float y) noexcept {
    return x >= static_cast<float>(rect.x) && y >= static_cast<float>(rect.y) &&
           x < static_cast<float>(rect.x + rect.width) &&
           y < static_cast<float>(rect.y + rect.height);
}

/// Tells whether a source rectangle lies wholly in another.
///
/// @param inner the rectangle
/// @param outer the other
/// @return true when no part of it lies outside
bool source_holds(const layout::Rect& outer, const layout::Rect& inner) noexcept {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Tells whether a rectangle holds no pixel.
///
/// @param rect the rectangle
/// @return true when it is not wide or not high
bool empty_rect(const layout::Rect& rect) noexcept {
    return rect.width <= 0 || rect.height <= 0;
}

/// Tells whether two rectangles are the same.
///
/// @param first a rectangle
/// @param second another
/// @return true when every edge is the same
bool same_rect(const layout::Rect& first, const layout::Rect& second) noexcept {
    return first.x == second.x && first.y == second.y && first.width == second.width &&
           first.height == second.height;
}

/// Returns the smallest rectangle holding two.
///
/// @param first a rectangle; empty for none
/// @param second another; empty for none
/// @return the rectangle holding both
layout::Rect union_rect(const layout::Rect& first, const layout::Rect& second) noexcept {
    if (empty_rect(first))
        return second;
    if (empty_rect(second))
        return first;
    const int32_t left = std::min(first.x, second.x);
    const int32_t top = std::min(first.y, second.y);
    const int32_t right = std::max(first.x + first.width, second.x + second.width);
    const int32_t bottom = std::max(first.y + first.height, second.y + second.height);
    return {left, top, right - left, bottom - top};
}

/// Returns the rectangle two rectangles share.
///
/// @param first a rectangle
/// @param second another
/// @return the shared rectangle; empty when they do not meet
layout::Rect intersection(const layout::Rect& first, const layout::Rect& second) noexcept {
    const int32_t left = std::max(first.x, second.x);
    const int32_t top = std::max(first.y, second.y);
    const int32_t right = std::min(first.x + first.width, second.x + second.width);
    const int32_t bottom = std::min(first.y + first.height, second.y + second.height);
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

/// Tells whether a screen darkens what lies under it while it shows: an
/// opaque modal screen with a backdrop. One that is not opaque never does.
///
/// @param screen the screen
/// @return true when it darkens what lies under it
bool darkens(const LayerScreen& screen) {
    return screen.modal() && screen.backdrop() && screen.opaque();
}

/// Lays one pixel of straight colour over a pixel of a picture with no
/// opacity of its own (src over): (colour × opacity + under × (255 −
/// opacity) + 127) / 255, rounded down, each channel.
///
/// @param[in,out] under the picture's pixel, red, green and blue
/// @param colour the pixel laid over it, red, green and blue
/// @param opacity the laid pixel's opacity, 0 clear to 255 opaque
void lay_over_picture(uint8_t* under, const uint8_t* colour, uint32_t opacity) noexcept {
    if (opacity == 0)
        return;
    for (std::size_t channel = 0; channel < 3U; ++channel)
        under[channel] = static_cast<uint8_t>(
            (colour[channel] * opacity + under[channel] * (255U - opacity) + 127U) / 255U
        );
}

/// Lays one pixel of straight colour over a pixel of a layer of straight
/// colours and opacity (src over): the opacity becomes opacity + under's
/// opacity × (255 − opacity) / 255, and each colour the two colours
/// weighted by their shares of it. A clear pixel changes nothing, and an
/// opaque one replaces the layer's.
///
/// @param[in,out] under the layer's pixel, red, green, blue and opacity
/// @param colour the pixel laid over it, red, green and blue
/// @param opacity the laid pixel's opacity, 0 clear to 255 opaque
void lay_over_layer(uint8_t* under, const uint8_t* colour, uint32_t opacity) noexcept {
    if (opacity == 0)
        return;
    // The share of the layer's own pixel that still shows, in 255ths of 255ths.
    const uint32_t through = under[3] * (255U - opacity);
    // The opacity of the two together, in 255ths of 255ths.
    const uint32_t together = opacity * 255U + through;
    for (std::size_t channel = 0; channel < 3U; ++channel)
        under[channel] = static_cast<uint8_t>(
            (colour[channel] * opacity * 255U + under[channel] * through + together / 2U) / together
        );
    under[3] = static_cast<uint8_t>((together + 127U) / 255U);
}

/// Returns the part of a drawing that holds what was drawn: the whole of an
/// opaque one, and the smallest rectangle holding every pixel of one that is
/// not opaque that is not clear.
///
/// @param drawing the drawing
/// @return the rectangle, in the drawing's pixels; empty when nothing shows
layout::Rect drawn_bounds(const LayerDrawing& drawing) {
    const auto width = static_cast<int32_t>(drawing.picture.width);
    const auto height = static_cast<int32_t>(drawing.picture.height);
    if (drawing.opacity.empty())
        return {0, 0, width, height};
    int32_t left = width;
    int32_t first_row = height;
    int32_t right = 0;
    int32_t bottom = 0;
    for (int32_t row = 0; row < height; ++row) {
        const uint8_t* line = drawing.opacity.data() +
                              static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        for (int32_t column = 0; column < width; ++column) {
            if (line[column] == 0)
                continue;
            left = std::min(left, column);
            right = std::max(right, column + 1);
            first_row = std::min(first_row, row);
            bottom = row + 1;
        }
    }
    if (right <= left || bottom <= first_row)
        return {};
    return {left, first_row, right - left, bottom - first_row};
}

/// Copies a screen drawn in window pixels onto the front end's picture: each
/// picture pixel the screen covers takes the screen's pixel at the picture
/// pixel's centre in the window, or, for a screen that is not opaque, has it
/// laid over it by its opacity. Where the picture fills the window, pixel
/// for pixel, that is the drawing copied, or laid, at its place.
///
/// @param[in,out] frame the picture
/// @param drawing the screen's drawing, as large as its place
/// @param shown where the screen shows, in window pixels
/// @param view where the picture lies in the window
void stamp_onto_picture(
    renderer::Surface& frame,
    const LayerDrawing& drawing,
    const layout::Rect& shown,
    const LayerView& view
) {
    const renderer::Surface& drawn = drawing.picture;
    if (drawn.width == 0 || drawn.height == 0 || empty_rect(shown))
        return;
    const bool laid = !drawing.opacity.empty();
    const auto width = static_cast<int32_t>(frame.width);
    const auto height = static_cast<int32_t>(frame.height);
    // The window pixel under a picture pixel's centre, along one axis.
    const auto window_pixel = [](int32_t pixel, int32_t start, int32_t length, int32_t size) {
        return start + static_cast<int32_t>(
                           (static_cast<int64_t>(2 * pixel + 1) * length) / (2 * int64_t{size})
                       );
    };
    for (int32_t row = 0; row < height; ++row) {
        const int32_t source_row =
            window_pixel(row, view.picture.y, view.picture.height, view.picture_size.y) - shown.y;
        if (source_row < 0 || source_row >= static_cast<int32_t>(drawn.height))
            continue;
        for (int32_t column = 0; column < width; ++column) {
            const int32_t source_column =
                window_pixel(column, view.picture.x, view.picture.width, view.picture_size.x) -
                shown.x;
            if (source_column < 0 || source_column >= static_cast<int32_t>(drawn.width))
                continue;
            const std::size_t source = static_cast<std::size_t>(source_row) * drawn.width +
                                       static_cast<std::size_t>(source_column);
            const auto* from = drawn.rgb.data() + source * 3U;
            auto* to = frame.rgb.data() + (static_cast<std::size_t>(row) * frame.width +
                                           static_cast<std::size_t>(column)) *
                                              3U;
            if (laid) {
                lay_over_picture(to, from, drawing.opacity[source]);
                continue;
            }
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
        }
    }
}

/// Copies a screen drawn as large as its place into a layer of straight
/// colours and opacity, pixel for pixel: each pixel of an opaque drawing
/// replaces the layer's, fully opaque, and each of one that is not opaque
/// is laid over the layer's by its opacity (src over, LayerDrawing).
///
/// @param[in,out] rgba the layer, 4 bytes a pixel: red, green, blue and opacity
/// @param width the layer's width
/// @param height the layer's height
/// @param drawing the screen's drawing
/// @param left the layer column its first column lands on
/// @param top the layer row its first row lands on
void copy_onto_layer(
    std::vector<uint8_t>& rgba,
    int32_t width,
    int32_t height,
    const LayerDrawing& drawing,
    int32_t left,
    int32_t top
) {
    const renderer::Surface& drawn = drawing.picture;
    const auto drawn_width = static_cast<int32_t>(drawn.width);
    const auto drawn_height = static_cast<int32_t>(drawn.height);
    const bool laid = !drawing.opacity.empty();
    if (laid && drawing.opacity.size() < static_cast<std::size_t>(drawn.width) * drawn.height)
        return;
    const int32_t first_row = std::max(top, 0);
    const int32_t bottom = std::min(top + drawn_height, height);
    const int32_t first_column = std::max(left, 0);
    const int32_t right = std::min(left + drawn_width, width);
    for (int32_t row = first_row; row < bottom; ++row) {
        const std::size_t source = static_cast<std::size_t>(row - top) * drawn.width +
                                   static_cast<std::size_t>(first_column - left);
        const uint8_t* from = drawn.rgb.data() + source * 3U;
        uint8_t* to =
            rgba.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(first_column)) *
                              4U;
        if (laid) {
            const uint8_t* opacity = drawing.opacity.data() + source;
            for (int32_t column = first_column; column < right; ++column) {
                lay_over_layer(to, from, *opacity);
                ++opacity;
                from += 3;
                to += 4;
            }
            continue;
        }
        for (int32_t column = first_column; column < right; ++column) {
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = 255U;
            from += 3;
            to += 4;
        }
    }
}

/// Tells whether two pictures hold the same pixels.
///
/// @param first a picture
/// @param second another
/// @return true when their sizes and pixels are the same
bool same_picture(const renderer::Surface& first, const renderer::Surface& second) {
    return first.width == second.width && first.height == second.height && first.rgb == second.rgb;
}

/// Tells whether two drawings hold the same pixels and opacity.
///
/// @param first a drawing
/// @param second another
/// @return true when their pictures and opacity are the same
bool same_drawing(const LayerDrawing& first, const LayerDrawing& second) {
    return same_picture(first.picture, second.picture) && first.opacity == second.opacity;
}

/// Darkens a picture once for each modal screen with a backdrop over it:
/// each pass blends the whole picture with the backdrop's colour.
///
/// @param[in,out] picture the picture
/// @param passes how many times it is darkened
/// @param opacity the backdrop's share of each pixel, in 256ths
void darken(renderer::Surface& picture, uint32_t passes, uint32_t opacity) {
    for (uint32_t pass = 0; pass < passes; ++pass)
        artless::blend_source_rect(
            picture,
            {0, 0, 1},
            {0, 0, static_cast<int32_t>(picture.width), static_cast<int32_t>(picture.height)},
            kit::rgb(kit::colour::backdrop),
            opacity
        );
}

/// How the window shows the canvas, for the rectangles automation lists the
/// layer's controls at.
struct WindowPlace {
    SDL_Renderer* renderer{}; ///< the renderer; null shows the canvas one to one
    float density{1.0F};      ///< the window's pixels to a window coordinate
    /// The front end's picture is drawn in the window, the screens over it
    /// in the window's own pixels (LayerPlacement::shown); otherwise the
    /// screens lie on the match's canvas, or the frame holds them.
    bool front_end{};
    SDL_FRect picture{};     ///< where the picture lies in the window, in its pixels
    double scale_x{};        ///< window pixels per picture pixel, across
    double scale_y{};        ///< window pixels per picture pixel, down
    int32_t output_width{};  ///< the window's width, in its pixels
    int32_t output_height{}; ///< the window's height, in its pixels
};

/// Returns how the window shows the canvas now, as present places the layer.
///
/// @param renderer the renderer; may be null
/// @param match a match shows
/// @param surface_width the front end's picture's width, without a logical presentation
/// @param surface_height its height
/// @param scaling how Menu scaling holds the picture, used when no logical
///     presentation is set
/// @return the placement
WindowPlace window_place(
    SDL_Renderer* renderer,
    bool match,
    int32_t surface_width,
    int32_t surface_height,
    render_policy::MenuScaling scaling
) {
    WindowPlace place{};
    place.renderer = renderer;
    if (renderer == nullptr)
        return place;
    SDL_Window* window = SDL_GetRenderWindow(renderer);
    if (window != nullptr) {
        const float density = SDL_GetWindowPixelDensity(window);
        if (density > 0.0F)
            place.density = density;
    }
    if (match)
        return place;
    int frame_w = surface_width;
    int frame_h = surface_height;
    int logical_w = 0;
    int logical_h = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    if (SDL_GetRenderLogicalPresentation(renderer, &logical_w, &logical_h, &mode) &&
        mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_w > 0 && logical_h > 0) {
        frame_w = logical_w;
        frame_h = logical_h;
    }
    if (frame_w <= 0 || frame_h <= 0)
        return place;
    const WindowSurface surface = window_surface(window, renderer);
    const int pixel_w = surface.pixel_width > 0 ? surface.pixel_width : 0;
    const int pixel_h = surface.pixel_height > 0 ? surface.pixel_height : 0;
    if (pixel_w <= 0 || pixel_h <= 0)
        return place;
    // The mode SDL is presenting with, so a whole-steps picture is not read
    // back as a letterbox. With none set, Menu scaling decides.
    const bool whole_steps =
        mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE ||
        (mode == SDL_LOGICAL_PRESENTATION_DISABLED &&
         render_policy::frame_fit(scaling, pixel_w, pixel_h, frame_w, frame_h) ==
             render_policy::FrameFit::whole_steps);
    const PlacedFrame placed = place_frame(window, renderer, frame_w, frame_h, whole_steps);
    if (!(placed.drawn.w > 0.0F) || !(placed.drawn.h > 0.0F))
        return place;
    place.front_end = true;
    place.picture = placed.drawn;
    place.scale_x = static_cast<double>(placed.drawn.w) / static_cast<double>(frame_w);
    place.scale_y = static_cast<double>(placed.drawn.h) / static_cast<double>(frame_h);
    place.output_width = pixel_w;
    place.output_height = pixel_h;
    return place;
}

/// A rectangle's edges on the canvas, not yet whole pixels.
struct Edges {
    double left{};   ///< its left edge
    double top{};    ///< its top edge
    double right{};  ///< the edge right of it
    double bottom{}; ///< the edge under it
};

/// Returns where a rectangle of a screen's points lies where the screen is
/// placed: the whole pixels the nearest pixel's rule stamps it on
/// (OaLayer::stamp). For a screen drawn at its scale as large as its place,
/// on the front end and the in-match Settings, those are the pixels it is
/// drawn on.
///
/// @param placed where the screen shows
/// @param points the rectangle, in the screen's points
/// @param scale_percent the scale the screen is drawn at
/// @return the edges; all 0 when the screen has no size
Edges screen_edges(const LayerPlacement& placed, const kit::Rect& points, int32_t scale_percent) {
    const kit::Rect drawn = kit::to_canvas(points, scale_percent);
    const kit::Rect whole =
        kit::to_canvas({0, 0, placed.points_width, placed.points_height}, scale_percent);
    if (whole.width <= 0 || whole.height <= 0)
        return {};
    const auto along = [](int32_t start, int32_t shown_length, int32_t length, int32_t pixel) {
        // The first pixel whose nearest drawn pixel is at or past this one.
        const int64_t scaled = static_cast<int64_t>(pixel) * shown_length;
        const int64_t first = scaled >= 0 ? (scaled + length - 1) / length : scaled / length;
        return static_cast<double>(start + first);
    };
    const auto& shown = placed.shown;
    return {
        along(shown.x, shown.width, whole.width, drawn.x),
        along(shown.y, shown.height, whole.height, drawn.y),
        along(shown.x, shown.width, whole.width, drawn.x + drawn.width),
        along(shown.y, shown.height, whole.height, drawn.y + drawn.height),
    };
}

/// Returns the window's pixels a canvas rectangle is drawn on: over the
/// front end's picture those whose centres it holds, clipped to the window,
/// as the picture's own pixels are scaled there; in a match its corners
/// through the renderer; without one, the canvas's own.
///
/// @param place how the window shows the canvas
/// @param edges the rectangle's edges on the canvas
/// @return the rectangle, in the window's pixels
layout::Rect window_rect_of(const WindowPlace& place, const Edges& edges) {
    if (place.renderer == nullptr) {
        const auto whole = [](double edge) { return static_cast<int32_t>(std::lround(edge)); };
        return {
            whole(edges.left),
            whole(edges.top),
            whole(edges.right) - whole(edges.left),
            whole(edges.bottom) - whole(edges.top)
        };
    }
    if (place.front_end) {
        const auto pixel = [](double area_start, double scale, double edge) {
            return static_cast<int32_t>(std::ceil(area_start + edge * scale - 0.5));
        };
        const int32_t left =
            std::clamp(pixel(place.picture.x, place.scale_x, edges.left), 0, place.output_width);
        const int32_t first_row =
            std::clamp(pixel(place.picture.y, place.scale_y, edges.top), 0, place.output_height);
        const int32_t right = std::clamp(
            pixel(place.picture.x, place.scale_x, edges.right), left, place.output_width
        );
        const int32_t bottom = std::clamp(
            pixel(place.picture.y, place.scale_y, edges.bottom), first_row, place.output_height
        );
        return {left, first_row, right - left, bottom - first_row};
    }
    float left = 0.0F;
    float first_row = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    if (!frame_to_window(
            place.renderer,
            static_cast<float>(edges.left),
            static_cast<float>(edges.top),
            &left,
            &first_row
        ) ||
        !frame_to_window(
            place.renderer,
            static_cast<float>(edges.right),
            static_cast<float>(edges.bottom),
            &right,
            &bottom
        ))
        return {};
    const auto pixel = [&place](float coordinate) {
        return static_cast<int32_t>(std::lround(coordinate * place.density));
    };
    return {
        pixel(left), pixel(first_row), pixel(right) - pixel(left), pixel(bottom) - pixel(first_row)
    };
}

/// Returns the window's pixels a rectangle of a front-end screen is drawn on:
/// its edges, which are window pixels already, clipped to the window.
///
/// @param place how the window shows the canvas
/// @param edges the rectangle's edges, in the window's pixels
/// @return the rectangle, in the window's pixels
layout::Rect window_rect_at(const WindowPlace& place, const Edges& edges) {
    const auto whole = [](double edge) { return static_cast<int32_t>(std::lround(edge)); };
    const int32_t left = std::clamp(whole(edges.left), 0, place.output_width);
    const int32_t first_row = std::clamp(whole(edges.top), 0, place.output_height);
    const int32_t right = std::clamp(whole(edges.right), left, place.output_width);
    const int32_t bottom = std::clamp(whole(edges.bottom), first_row, place.output_height);
    return {left, first_row, right - left, bottom - first_row};
}

/// Returns the canvas edges a window rectangle covers: its corners mapped
/// back through the canvas's placement, as the game maps the pointer.
///
/// @param place how the window shows the canvas
/// @param window the rectangle, in the window's pixels
/// @return the edges, in canvas pixels; the window's own without a renderer
Edges canvas_edges_of(const WindowPlace& place, const layout::Rect& window) {
    const Edges own{
        static_cast<double>(window.x),
        static_cast<double>(window.y),
        static_cast<double>(window.x) + static_cast<double>(window.width),
        static_cast<double>(window.y) + static_cast<double>(window.height)
    };
    if (place.renderer == nullptr)
        return own;
    const FrameShift shift = renderer_frame_shift(place.renderer);
    const auto to_canvas = [&place,
                            &shift](int32_t x, int32_t y, float& canvas_x, float& canvas_y) {
        if (!SDL_RenderCoordinatesFromWindow(
                place.renderer,
                static_cast<float>(x) / place.density,
                static_cast<float>(y) / place.density,
                &canvas_x,
                &canvas_y
            ))
            return false;
        canvas_x += shift.x;
        canvas_y += shift.y;
        return true;
    };
    float left = 0.0F;
    float first_row = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    if (!to_canvas(window.x, window.y, left, first_row) ||
        !to_canvas(window.x + window.width, window.y + window.height, right, bottom))
        return own;
    return {left, first_row, right, bottom};
}

/// Returns the canvas rectangle a window rectangle covers: its edges
/// (canvas_edges_of), the top left rounded down and the bottom right up.
///
/// @param place how the window shows the canvas
/// @param window the rectangle, in the window's pixels
/// @return the rectangle, in canvas pixels; the window's own without a renderer
layout::Rect canvas_rect_of(const WindowPlace& place, const layout::Rect& window) {
    const Edges edges = canvas_edges_of(place, window);
    const auto down = [](double edge) { return static_cast<int32_t>(std::floor(edge)); };
    const auto up = [](double edge) { return static_cast<int32_t>(std::ceil(edge)); };
    return {
        down(edges.left),
        down(edges.top),
        up(edges.right) - down(edges.left),
        up(edges.bottom) - down(edges.top)
    };
}

/// Returns how far a canvas point lies from a rectangle's edges, in canvas
/// pixels.
///
/// A point inside the edges is no distance. Outside, the distance is to the
/// nearest edge, as the touch dispatcher measures a gadget's, unrounded so
/// that a control drawn between canvas pixels is no nearer than it shows.
/// Edges with no area are as far as a distance can be.
///
/// @param x the point's column, in canvas pixels
/// @param y the point's row
/// @param edges the rectangle's edges, in canvas pixels
/// @return the distance
float distance_to_edges(float x, float y, const Edges& edges) noexcept {
    if (!(edges.right > edges.left) || !(edges.bottom > edges.top))
        return std::numeric_limits<float>::infinity();
    const double across = std::max({edges.left - x, 0.0, x - edges.right});
    const double down = std::max({edges.top - y, 0.0, y - edges.bottom});
    return static_cast<float>(std::hypot(across, down));
}

/// Sets a control's rectangles: in the window's pixels, and on the canvas.
///
/// @param[out] control the control
/// @param place how the window shows the canvas
/// @param window where it lies, in the window's pixels (window_rect_of, window_rect_at)
void place_control(
    AutomationControl& control, const WindowPlace& place, const layout::Rect& window
) {
    const layout::Rect canvas = canvas_rect_of(place, window);
    control.x = canvas.x;
    control.y = canvas.y;
    control.width = canvas.width;
    control.height = canvas.height;
    control.window_pixels = true;
    control.window_x = window.x;
    control.window_y = window.y;
    control.window_width = window.width;
    control.window_height = window.height;
}

/// The prefix of every name and dialog the layer lists to automation.
constexpr std::string_view kAutomationPrefix = "oa.";

/// The automation name of the main menu's Settings button.
constexpr std::string_view kSettingsButtonName = "oa.settings-button";

/// The automation text of the main menu's Settings button.
constexpr std::string_view kSettingsButtonText = "Settings";

/// GET MAP's screen over the battle room (runtime_get_map.cpp). Its button,
/// its line and its bar ("get-map", "get-map-text" and "get-map-progress")
/// stand on the battle room as the OA button stands on the main menu, so
/// automation lists them as it lists that button, with no dialog.
constexpr std::string_view kGetMapScreen = "get-map";

/// Returns a quotient rounded down, for a numerator below 0 too.
///
/// @param numerator the numerator
/// @param denominator the denominator, above 0
/// @return floor(numerator / denominator)
int32_t floor_div(int32_t numerator, int32_t denominator) noexcept {
    const int32_t quotient = numerator / denominator;
    return numerator % denominator < 0 ? quotient - 1 : quotient;
}

/// Returns the size class a screen of the front end is laid out at.
///
/// @param view what the screen is placed on
/// @return the view's class; Compact in a match
kit::SizeClass front_class(const LayerView& view) noexcept {
    return view.match ? kit::SizeClass::compact : view.frame.size_class;
}

/// Returns where a notice or a question of a height shows: centred in the
/// view's room at its scale, its class's notice width wide, its corners
/// rounded with the class's dialog radius.
///
/// @param view what it is placed on
/// @param box_height its height, in points
/// @return its place
LayerPlacement centred_box(const LayerView& view, int32_t box_height) noexcept {
    const kit::Metrics& sized = kit::metrics_of(front_class(view));
    LayerPlacement placed = centred_placement(view, sized.notice_width, box_height);
    placed.corner_radius = sized.dialog_radius;
    return placed;
}

/// Returns one of the Library's words in the language shown.
///
/// @param english the word, in English as the Library's text.hpp writes it
/// @return its translation, or the English
std::string_view library_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// Returns the main menu's OA button's text for automation, in the language
/// shown. While the package count is final it is "Library · 1 package" or
/// "Library · N packages", and "Library · N packages · 1 update" or
/// "Library · N packages · M updates" while updates wait. A package is a
/// mod, a map pack, a language pack, a mutator or a downloaded expansion. While the
/// count is not final the package count is left out: "Library", or
/// "Library · 1 update" and "Library · N updates".
///
/// @param updates the updates that wait (Runtime::library_update_count)
/// @param packages_ready the package count is final
/// @param packages how many packages the player has added
/// @return the text
std::string library_button_text(std::size_t updates, bool packages_ready, std::size_t packages) {
    namespace words = oa::ui::library::words;
    const oa::ui::library::TextHooks lookup{nullptr, library_word};
    std::string text(oa::ui::library::shown(lookup, words::title_library));
    const auto join = [&](std::string_view pattern, std::string number) {
        text += " \u00B7 ";
        text += oa::ui::library::filled(lookup, pattern, {{"count", number}});
    };
    if (packages_ready)
        join(
            packages == 1 ? words::package_count_one : words::package_count_many,
            std::to_string(packages)
        );
    if (updates != 0)
        join(
            updates == 1 ? words::update_count_one : words::update_count_many,
            std::to_string(updates)
        );
    return text;
}

/// Returns the parts of a notice or a question that its pointer and its keys
/// move: the hover, the press, the marked button and a finger's shift.
///
/// @param box the notice or the question
/// @return those parts
template <typename Box>
auto pointed_look(const Box& box) noexcept {
    return std::make_tuple(
        box.hovered, box.pressed, box.marked, box.finger_shift_x, box.finger_shift_y
    );
}

/// Returns the parts of a screen's interaction its drawing shows.
///
/// @param shown the interaction
/// @return the hover, the press, the focus, whether it shows and a
///     finger's shift
auto pointed_look(const kit::Interaction& shown) noexcept {
    return std::make_tuple(
        shown.hovered,
        shown.pressed,
        shown.focused,
        shown.focus_shown,
        shown.finger_shift.x,
        shown.finger_shift.y
    );
}

/// Counts a look an input moved in a screen's revision, so that a press
/// that hit no button but took the hover with it is drawn too.
///
/// @param answer what the screen answered the input
/// @param moved the input moved the screen's look
/// @param[in,out] revision the screen's revision
/// @return the answer; redraw in place of none when the look moved
LayerAnswer with_moved_look(LayerAnswer answer, bool moved, uint64_t& revision) noexcept {
    if (!moved)
        return answer;
    ++revision;
    return answer == LayerAnswer::none ? LayerAnswer::redraw : answer;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The settings dialog as a screen of the layer

namespace {

/// Returns the practice pad's key for a key the player pressed or let go:
/// Shift, Ctrl and Alt, either side, and the letters and digits by the
/// legend their keycap shows.
///
/// @param sdl_key the SDL keycode
/// @return the pad's key; nothing for a key the dialog answers itself (Tab,
///     the arrows, Space, Enter, Escape, the page keys) and every other key
std::optional<settings::PracticeKey> practice_key(uint32_t sdl_key) {
    using settings::PracticeModifier;
    switch (sdl_key) {
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        return settings::PracticeKey{PracticeModifier::shift, "Shift"};
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        return settings::PracticeKey{PracticeModifier::ctrl, "Ctrl"};
    case SDLK_LALT:
    case SDLK_RALT:
        return settings::PracticeKey{PracticeModifier::alt, "Alt"};
    default:
        break;
    }
    if ((sdl_key >= SDLK_A && sdl_key <= SDLK_Z) || (sdl_key >= SDLK_0 && sdl_key <= SDLK_9))
        return settings::PracticeKey{PracticeModifier::none, std::string(SDL_GetKeyName(sdl_key))};
    return std::nullopt;
}

} // namespace

/// The settings dialog as a screen of the OA layer: modal, over a backdrop,
/// centred on the main menu's picture or on the match's window, and moved by
/// its title.
struct Runtime::SettingsScreen final : LayerScreen {
    /// Makes the screen of the dialog open now.
    ///
    /// @param runtime the runtime
    /// @param in_match the dialog was opened in a match; otherwise on the main menu
    SettingsScreen(Runtime& runtime, bool in_match) noexcept
        : runtime_(runtime), in_match_(in_match) {}

    /// Returns "settings".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kSettingsName; }

    /// Returns where the dialog shows unmoved: on the main menu at the
    /// view's size class, centred in the window at its scale; in a match at
    /// the class, scale and place OaLayer::match_dialog gives, or on a phone
    /// at Compact fitted to the safe area. Its corners take its class's
    /// window radius. The layer moves it where the player put it
    /// (OaLayer::shown_placement).
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty once the dialog closed, or off its screen
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override {
        if (runtime_.engine_settings_dialog() == nullptr)
            return {};
        if (!in_match_) {
            if (view.match || runtime_.screen_ != Screen::main_menu)
                return {};
            // At the view's class (lay_out), centred at its scale.
            const kit::Metrics& sized = kit::metrics_of(front_class(view));
            LayerPlacement placed =
                centred_placement(view, sized.dialog_width, sized.dialog_height);
            placed.corner_radius = sized.window_radius;
            return placed;
        }
        if (!view.match || view.match_layout == nullptr)
            return {};
        // At its own class (lay_out) and scale, which input maps back by.
        const MatchDialogPlace place = OaLayer::match_dialog(
            *view.match_layout, view.interface_percent, view.fit_safe_area, view.window_size
        );
        const kit::Metrics& sized = kit::metrics_of(place.size_class);
        return {
            place.shown,
            sized.dialog_width,
            sized.dialog_height,
            place.scale_percent,
            sized.window_radius
        };
    }

    /// Tells that the dialog takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the dialog darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the dialog out at the view's size class on the main menu, and in
    /// a match at the class OaLayer::match_dialog gives (Dialog::size_class).
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override {
        auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return;
        // Off its match the dialog keeps its class until its tick closes it.
        if (in_match_ && (!view.match || view.match_layout == nullptr))
            return;
        const kit::SizeClass size_class = in_match_ ? OaLayer::match_dialog(
                                                          *view.match_layout,
                                                          view.interface_percent,
                                                          view.fit_safe_area,
                                                          view.window_size
                                                      )
                                                          .size_class
                                                    : front_class(view);
        if (dialog->size_class == size_class)
            return;
        dialog->size_class = size_class;
        ++revision_;
    }

    /// Draws the dialog with the dialog's fonts and the settings icon, at
    /// the canvas's scale. The layer's canvas carries the app icon; this
    /// screen replaces it, on the main menu and in a match.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        const auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr || !kit::can_draw(canvas) || canvas.fonts == nullptr)
            return;
        kit::Canvas drawn = canvas;
        drawn.icon = Runtime::EngineSettingsMenuHost::settings_icon(runtime_);
        settings::draw_dialog(drawn, *dialog);
    }

    /// Takes a pointer's move, press or release: a finger's press takes the
    /// nearest control within reach.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the dialog's pixels
    /// @param finger_reach a finger's reach in the dialog's pixels; 0 for a mouse
    /// @return what the dialog did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return LayerAnswer::close;
        auto action = settings::DialogAction::none;
        switch (kind) {
        case ScreenInputKind::pointer_move:
            action = settings::dialog_pointer_move(*dialog, at.x, at.y);
            break;
        case ScreenInputKind::pointer_down:
            if (button == SDL_BUTTON_LEFT)
                action = finger_reach > 0
                             ? settings::dialog_finger_down(*dialog, at.x, at.y, finger_reach)
                             : settings::dialog_pointer_down(*dialog, at.x, at.y);
            break;
        case ScreenInputKind::pointer_up:
            if (button == SDL_BUTTON_LEFT)
                action = settings::dialog_pointer_up(*dialog, at.x, at.y);
            break;
        default:
            break;
        }
        return take_action(action);
    }

    /// Takes a key's press. The dialog has nothing more to tell of its
    /// controls, so the information key goes on, which under the modal
    /// dialog does nothing.
    ///
    /// @param pressed the key
    /// @return what the dialog did; pass for the information key
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return LayerAnswer::close;
        if (pressed == kit::Key::info)
            return LayerAnswer::pass;
        return take_action(settings::dialog_key(*dialog, pressed));
    }

    /// Takes a key's press before the layer maps it to the kit's keys: on
    /// Accessibility, the practice pad takes Shift, Ctrl, Alt, the letters
    /// and the digits (practice_key), which reach nothing else; every other
    /// key, and every key on the other sections, goes on to key.
    ///
    /// @param sdl_key the SDL keycode
    /// @return redraw when the pad shows something else, none when it took
    ///     the key and shows the same; pass for a key it does not take
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t /*modifiers*/) override {
        return practice(sdl_key, true);
    }

    /// Takes a key's release: the practice pad's keys on Accessibility, as
    /// shortcut takes their presses.
    ///
    /// @param sdl_key the SDL keycode
    /// @return what the pad did; pass for a key it does not take
    LayerAnswer key_up(uint32_t sdl_key, uint16_t /*modifiers*/) override {
        return practice(sdl_key, false);
    }

    /// Takes a turn of the wheel, which scrolls the open section; nothing
    /// under the dialog sees it.
    ///
    /// @param at the pointer, in the dialog's pixels
    /// @param notches how far the wheel turned
    /// @return what the dialog did
    LayerAnswer wheel(kit::Point at, float notches) override {
        auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return LayerAnswer::close;
        return take_action(settings::dialog_wheel(*dialog, at.x, at.y, notches));
    }

    /// Returns the dialog's display list.
    ///
    /// @return the list; empty once the dialog closed
    [[nodiscard]] kit::DisplayList display_list() const override {
        const auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return {};
        return settings::geometry::dialog_list(*dialog, runtime_.engine_settings_fonts());
    }

    /// Returns the dialog's hover, press and focus.
    ///
    /// @return the interaction; none once the dialog closed
    [[nodiscard]] kit::Interaction interaction() const override {
        const auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return {};
        kit::Interaction shown{};
        shown.hovered = dialog->hovered;
        shown.pressed = dialog->pressed;
        shown.focused = dialog->focused;
        shown.focus_shown = dialog->focused != settings::no_control;
        shown.finger_shift = {dialog->finger_shift_x, dialog->finger_shift_y};
        return shown;
    }

    /// Brings the dialog up to date: Hardware acceleration's status follows
    /// the renderer, Touch shows once a finger turns the touch controls on,
    /// and Controller once a gamepad sends input, with its Steam Input
    /// notice while that applies. Language's rows follow the catalogues and
    /// the download queue, so a match draws the field again as a pack
    /// arrives, and Mods' updates and its line on the check for updates
    /// follow the Library's model and the registries. Off the main menu the
    /// main menu's dialog closes as Cancel; once the in-game menu's column no
    /// longer shows under it, the match's closes as OK. Accessibility's
    /// practice pad is told the time (dialog_practice_tick).
    ///
    /// @return close when the dialog closed
    LayerAnswer tick() override {
        auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return LayerAnswer::close;
        if (in_match_) {
            if (runtime_.screen_ != Screen::match || !runtime_.ingame_menu_column_shown()) {
                // OK always closes the dialog; a failed save is reported
                // where it happens.
                ++revision_;
                std::ignore =
                    runtime_.take_engine_settings_action(settings::DialogAction::accepted);
                return LayerAnswer::close;
            }
        } else if (runtime_.screen_ != Screen::main_menu) {
            // Cancel always closes an open dialog.
            std::ignore = runtime_.take_engine_settings_action(settings::DialogAction::cancelled);
            return LayerAnswer::close;
        }
        bool changed =
            settings::set_acceleration_status(*dialog, runtime_.acceleration_report().status) ==
            settings::DialogAction::redraw;
        changed = settings::set_touch_controls(*dialog, runtime_.touch_controls_active()) ==
                      settings::DialogAction::redraw ||
                  changed;
        changed = settings::set_controller_section(
                      *dialog, runtime_.pad_used(), runtime_.pad_steam_input()
                  ) == settings::DialogAction::redraw ||
                  changed;
        // The front end draws every frame. A match draws this dialog again
        // only when the revision moves, so a download's progress has to.
        changed = runtime_.refresh_settings_languages(*dialog) || changed;
        changed = runtime_.refresh_game_content(*dialog) || changed;
        // The practice pad's time: Slow keys' wait and Hold to confirm's bar
        // move while a key is held.
        changed = settings::dialog_practice_tick(*dialog, SDL_GetTicks()) ==
                      settings::DialogAction::redraw ||
                  changed;
        if (!changed)
            return LayerAnswer::none;
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Returns the dialog's revision, which counts the actions it took.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Closes a dialog still open as Cancel closes it.
    void close(bool /*by_key*/) override {
        if (runtime_.engine_settings_dialog() != nullptr)
            std::ignore = runtime_.take_engine_settings_action(settings::DialogAction::cancelled);
    }

    /// Puts an action of the dialog in effect and plays its sound: OK plays
    /// the opening sound, Cancel its own.
    ///
    /// @param action what the dialog's event asked
    /// @return close when the dialog closed; none when it asked nothing
    LayerAnswer take_action(settings::DialogAction action) {
        if (action == settings::DialogAction::accepted)
            runtime_.play_ui_sound(kOpenSound, 0);
        else if (action == settings::DialogAction::cancelled)
            runtime_.play_ui_sound(kCancelSound, 0);
        if (action == settings::DialogAction::none)
            return LayerAnswer::none;
        ++revision_;
        return runtime_.take_engine_settings_action(action) ? LayerAnswer::close
                                                            : LayerAnswer::redraw;
    }

  private:

    /// Hands a key's press or release to the practice pad while
    /// Accessibility shows in the engine's settings.
    ///
    /// @param sdl_key the SDL keycode
    /// @param down true for a press, false for a release
    /// @return redraw or none when the pad took the key; pass otherwise
    LayerAnswer practice(uint32_t sdl_key, bool down) {
        auto* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr || dialog->page != settings::Page::accessibility ||
            dialog->kind != settings::DialogKind::engine)
            return LayerAnswer::pass;
        const std::optional<settings::PracticeKey> pressed = practice_key(sdl_key);
        if (!pressed)
            return LayerAnswer::pass;
        if (settings::dialog_practice_key(*dialog, *pressed, down, SDL_GetTicks()) !=
            settings::DialogAction::redraw)
            return LayerAnswer::none;
        ++revision_;
        return LayerAnswer::redraw;
    }

    Runtime& runtime_;    ///< the runtime whose dialog it shows
    bool in_match_{};     ///< opened in a match; otherwise on the main menu
    uint64_t revision_{}; ///< counts the actions the dialog took and the changes its tick made
};

void Runtime::push_settings_screen(bool in_match) {
    oa_layer().push(std::make_unique<SettingsScreen>(*this, in_match));
}

void Runtime::take_settings_screen_action(settings::DialogAction action) {
    auto& layer = oa_layer();
    auto* screen = dynamic_cast<SettingsScreen*>(layer.find(kSettingsName));
    if (screen == nullptr) {
        std::ignore = take_engine_settings_action(action);
        return;
    }
    layer.take_answer(*screen, screen->take_action(action), 0);
}

// ---------------------------------------------------------------------------------------------
// A work dialog as a screen of the layer

WorkScreen::WorkScreen(
    OaLayer& layer, Screen shown_over, std::string title, std::string line, Host answers
)
    : layer_(layer), over_(shown_over), host_(std::move(answers)),
      began_ms_(oa::platform::tick_ms()) {
    dialog_.title = std::move(title);
    dialog_.line = std::move(line);
}

LayerPlacement WorkScreen::placement(const LayerView& view) const {
    if (view.match || layer_.screen_fonts() == nullptr || view.game_screen != over_)
        return {};
    kit::WorkDialog sized = dialog_;
    sized.size_class = front_class(view);
    return centred_box(view, kit::place_work_dialog(sized, layer_.screen_fonts()).height);
}

void WorkScreen::lay_out(const LayerView& view) {
    const kit::SizeClass size_class = front_class(view);
    if (dialog_.size_class == size_class)
        return;
    dialog_.size_class = size_class;
    ++revision_;
}

void WorkScreen::draw(const kit::Canvas& canvas) const {
    if (!kit::can_draw(canvas) || canvas.fonts == nullptr)
        return;
    kit::draw_work_dialog(canvas, dialog_);
}

kit::DisplayList WorkScreen::display_list() const {
    return kit::work_dialog_list(dialog_, layer_.screen_fonts());
}

LayerAnswer WorkScreen::tick() {
    if (host_.tick && host_.tick())
        return LayerAnswer::close;
    const uint32_t turn = kit::refresh_eighth(oa::platform::tick_ms() - began_ms_);
    if (turn == dialog_.turn)
        return LayerAnswer::none;
    dialog_.turn = turn;
    ++revision_;
    return LayerAnswer::redraw;
}

// ---------------------------------------------------------------------------------------------
// A notice as a screen of the layer

NoticeScreen::NoticeScreen(OaLayer& layer, Screen shown_over, kit::Notice told, Host answers)
    : layer_(layer), over_(shown_over), notice_(std::move(told)), host_(std::move(answers)) {
}

int32_t NoticeScreen::shown_height() const {
    return settings::notice_height(notice_, layer_.screen_fonts());
}

LayerPlacement NoticeScreen::placement(const LayerView& view) const {
    if (view.match || layer_.screen_fonts() == nullptr)
        return {};
    if (!any_menu_ && view.game_screen != over_)
        return {};
    // As tall as its text makes it at the view's class.
    kit::Notice sized = notice_;
    sized.size_class = front_class(view);
    return centred_box(view, settings::notice_height(sized, layer_.screen_fonts()));
}

void NoticeScreen::lay_out(const LayerView& view) {
    const kit::SizeClass size_class = front_class(view);
    if (notice_.size_class == size_class)
        return;
    notice_.size_class = size_class;
    ++revision_;
}

void NoticeScreen::draw(const kit::Canvas& canvas) const {
    if (!kit::can_draw(canvas) || canvas.fonts == nullptr)
        return;
    settings::draw_notice(canvas, notice_);
}

LayerAnswer
NoticeScreen::pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) {
    const auto before = pointed_look(notice_);
    const int32_t tall = shown_height();
    auto action = kit::NoticeAction::none;
    switch (kind) {
    case ScreenInputKind::pointer_move:
        action = kit::notice_pointer_move(notice_, at, tall);
        break;
    case ScreenInputKind::pointer_down:
        // A finger's press takes the nearer button within reach.
        if (button == SDL_BUTTON_LEFT)
            action = finger_reach > 0 ? kit::notice_finger_down(notice_, at, tall, finger_reach)
                                      : kit::notice_pointer_down(notice_, at, tall);
        break;
    case ScreenInputKind::pointer_up:
        if (button == SDL_BUTTON_LEFT)
            action = kit::notice_pointer_up(notice_, at, tall);
        break;
    default:
        break;
    }
    const LayerAnswer answer = take(action);
    return with_moved_look(answer, pointed_look(notice_) != before, revision_);
}

LayerAnswer NoticeScreen::key(kit::Key pressed, uint32_t /*sdl_key*/) {
    const auto before = pointed_look(notice_);
    const LayerAnswer answer = take(kit::notice_key(notice_, pressed));
    return with_moved_look(answer, pointed_look(notice_) != before, revision_);
}

kit::DisplayList NoticeScreen::display_list() const {
    return kit::notice_list(notice_, layer_.screen_fonts(), oa::data::languages::interface_text);
}

kit::Interaction NoticeScreen::interaction() const {
    kit::Interaction shown{};
    shown.hovered = notice_.hovered;
    shown.pressed = notice_.pressed;
    shown.focused = notice_.marked;
    shown.focus_shown = notice_.marked != kit::no_control;
    shown.finger_shift = {notice_.finger_shift_x, notice_.finger_shift_y};
    return shown;
}

LayerAnswer NoticeScreen::tick() {
    return host_.tick && host_.tick() ? LayerAnswer::close : LayerAnswer::none;
}

LayerAnswer NoticeScreen::take(kit::NoticeAction action) {
    switch (action) {
    case kit::NoticeAction::open_folder:
        // The notice stays, saying why the folder could not be shown.
        notice_.failure = host_.open ? host_.open() : std::string();
        ++revision_;
        return LayerAnswer::redraw;
    case kit::NoticeAction::closed:
        if (host_.ok)
            host_.ok();
        ++revision_;
        return LayerAnswer::close;
    case kit::NoticeAction::redraw:
        ++revision_;
        return LayerAnswer::redraw;
    case kit::NoticeAction::none:
        break;
    }
    return LayerAnswer::none;
}

// ---------------------------------------------------------------------------------------------
// A question as a screen of the layer

QuestionScreen::QuestionScreen(OaLayer& layer, Screen shown_over, kit::Question asked, Host answers)
    : layer_(layer), over_(shown_over), question_(std::move(asked)), host_(std::move(answers)) {
}

int32_t QuestionScreen::shown_height() const {
    return settings::prompt_height(question_, layer_.screen_fonts());
}

LayerPlacement QuestionScreen::placement(const LayerView& view) const {
    if (view.match || layer_.screen_fonts() == nullptr)
        return {};
    if (!any_menu_ && view.game_screen != over_)
        return {};
    // As tall as its text makes it at the view's class.
    kit::Question sized = question_;
    sized.size_class = front_class(view);
    return centred_box(view, settings::prompt_height(sized, layer_.screen_fonts()));
}

void QuestionScreen::lay_out(const LayerView& view) {
    size_class_ = front_class(view);
    if (question_.size_class == size_class_)
        return;
    question_.size_class = size_class_;
    ++revision_;
}

void QuestionScreen::draw(const kit::Canvas& canvas) const {
    if (!kit::can_draw(canvas) || canvas.fonts == nullptr)
        return;
    settings::draw_prompt(canvas, question_);
}

LayerAnswer
QuestionScreen::pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) {
    const auto before = pointed_look(question_);
    const int32_t tall = shown_height();
    // The buttons are tested where the fonts the question is drawn in place them.
    const kit::Fonts* fonts = layer_.screen_fonts();
    kit::QuestionAnswer outcome{};
    switch (kind) {
    case ScreenInputKind::pointer_move:
        outcome = kit::question_pointer_move(question_, at, tall, fonts);
        break;
    case ScreenInputKind::pointer_down:
        // A finger's press takes the nearest button within reach.
        if (button == SDL_BUTTON_LEFT)
            outcome = finger_reach > 0
                          ? kit::question_finger_down(question_, at, tall, finger_reach, fonts)
                          : kit::question_pointer_down(question_, at, tall, fonts);
        break;
    case ScreenInputKind::pointer_up:
        if (button == SDL_BUTTON_LEFT)
            outcome = kit::question_pointer_up(question_, at, tall, fonts);
        break;
    default:
        break;
    }
    const LayerAnswer answer = take(outcome, 0);
    return with_moved_look(answer, pointed_look(question_) != before, revision_);
}

LayerAnswer QuestionScreen::key(kit::Key pressed, uint32_t sdl_key) {
    const auto before = pointed_look(question_);
    const LayerAnswer answer = take(kit::question_key(question_, pressed), sdl_key);
    return with_moved_look(answer, pointed_look(question_) != before, revision_);
}

kit::DisplayList QuestionScreen::display_list() const {
    return kit::question_list(question_, layer_.screen_fonts());
}

kit::Interaction QuestionScreen::interaction() const {
    kit::Interaction shown{};
    shown.hovered = question_.hovered;
    shown.pressed = question_.pressed;
    shown.focused = question_.marked;
    shown.focus_shown = question_.marked != kit::no_control;
    shown.finger_shift = {question_.finger_shift_x, question_.finger_shift_y};
    return shown;
}

LayerAnswer QuestionScreen::tick() {
    return host_.tick && host_.tick() ? LayerAnswer::close : LayerAnswer::none;
}

void QuestionScreen::close(bool /*by_key*/) {
    if (host_.closed)
        host_.closed(*this);
}

LayerAnswer QuestionScreen::take(kit::QuestionAnswer outcome, uint32_t sdl_key) {
    switch (outcome.action) {
    case kit::QuestionAction::answered: {
        // The key that answered does nothing more until it is released,
        // whether the question closes or stays with what its host shows next.
        if (sdl_key != 0)
            layer_.latch_key(sdl_key);
        ++revision_;
        const bool last = !host_.answer || host_.answer(outcome.button);
        return last ? LayerAnswer::close : LayerAnswer::redraw;
    }
    case kit::QuestionAction::redraw:
        ++revision_;
        return LayerAnswer::redraw;
    case kit::QuestionAction::none:
        break;
    }
    return LayerAnswer::none;
}

// ---------------------------------------------------------------------------------------------
// The UI kit gallery as a screen of the layer

GalleryScreen::GalleryScreen(OaLayer& layer, bool in_match) noexcept
    : layer_(layer), in_match_(in_match) {
}

kit::Frame GalleryScreen::frame_for(const LayerView& view) const {
    kit::Frame given{};
    if (view.match != in_match_ || layer_.screen_fonts() == nullptr)
        return given;
    if (view.match) {
        if (view.match_layout == nullptr)
            return given;
        given = OaLayer::match_gallery(*view.match_layout, view.interface_percent);
    } else {
        // The view's room at its scale, and the class of its points.
        given = view.frame;
        given.scale_percent = view.scale_percent;
    }
    given.area.x = 0;
    given.area.y = 0;
    return given;
}

LayerPlacement GalleryScreen::placement(const LayerView& view) const {
    const kit::Frame given = frame_for(view);
    if (given.area.width <= 0 || given.area.height <= 0)
        return {};
    if (!view.match)
        return centred_placement(view, given.area.width, given.area.height);
    // The match's canvas at the gallery's own scale, centred in its safe area.
    const auto safe = OaLayer::match_safe_area(*view.match_layout);
    const kit::Rect drawn = kit::to_canvas(given.area, given.scale_percent);
    return {
        {safe.x + (safe.width - drawn.width) / 2,
         safe.y + (safe.height - drawn.height) / 2,
         drawn.width,
         drawn.height},
        given.area.width,
        given.area.height,
        given.scale_percent
    };
}

void GalleryScreen::lay_out(const LayerView& view) {
    const kit::Frame given = frame_for(view);
    const kit::Fonts* fonts = layer_.screen_fonts();
    if (fonts == nullptr || given.area.width <= 0 || given.area.height <= 0)
        return;
    if (given.area.width == frame_.area.width && given.area.height == frame_.area.height &&
        given.size_class == frame_.size_class && given.scale_percent == frame_.scale_percent)
        return;
    frame_ = given;
    layout_ = kit::gallery_layout(model_, frame_, *fonts);
    ++revision_;
}

void GalleryScreen::draw(const kit::Canvas& canvas) const {
    if (!kit::can_draw(canvas) || canvas.fonts == nullptr)
        return;
    kit::draw_gallery(canvas, layout_);
}

LayerAnswer
GalleryScreen::pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) {
    kit::GalleryEvent event;
    event.at = at;
    event.reach = finger_reach;
    switch (kind) {
    case ScreenInputKind::pointer_move:
        event.kind = kit::GalleryEventKind::pointer_move;
        break;
    case ScreenInputKind::pointer_down:
        if (button != SDL_BUTTON_LEFT)
            return LayerAnswer::none;
        event.kind = kit::GalleryEventKind::pointer_down;
        break;
    case ScreenInputKind::pointer_up:
        if (button != SDL_BUTTON_LEFT)
            return LayerAnswer::none;
        event.kind = kit::GalleryEventKind::pointer_up;
        break;
    default:
        return LayerAnswer::none;
    }
    return take(event);
}

LayerAnswer GalleryScreen::key(kit::Key pressed, uint32_t /*sdl_key*/) {
    if (pressed == kit::Key::info)
        return LayerAnswer::none;
    kit::GalleryEvent event;
    event.kind = kit::GalleryEventKind::key;
    event.key = pressed;
    return take(event);
}

LayerAnswer GalleryScreen::wheel(kit::Point at, float notches) {
    kit::GalleryEvent event;
    event.kind = kit::GalleryEventKind::wheel;
    event.at = at;
    event.notches = notches;
    return take(event);
}

LayerAnswer GalleryScreen::text(std::string_view utf8) {
    kit::GalleryEvent event;
    event.kind = kit::GalleryEventKind::text;
    event.text = utf8;
    return take(event);
}

std::optional<kit::Rect> GalleryScreen::text_field() const {
    return kit::gallery_text_field(model_, layout_);
}

LayerAnswer GalleryScreen::tick() {
    // The gallery shows over the settings dialog, and leaves with it.
    return layer_.find("settings") == nullptr ? LayerAnswer::close : LayerAnswer::none;
}

LayerAnswer GalleryScreen::take(const kit::GalleryEvent& event) {
    const kit::Fonts* fonts = layer_.screen_fonts();
    if (fonts == nullptr || frame_.area.width <= 0 || frame_.area.height <= 0)
        return LayerAnswer::none;
    switch (kit::gallery_act(model_, frame_, *fonts, event)) {
    case kit::GalleryResult::close:
        return LayerAnswer::close;
    case kit::GalleryResult::redraw:
        layout_ = kit::gallery_layout(model_, frame_, *fonts);
        ++revision_;
        return LayerAnswer::redraw;
    case kit::GalleryResult::none:
        break;
    }
    return LayerAnswer::none;
}

// ---------------------------------------------------------------------------------------------
// The layer

uint64_t LayerScreen::next_identity() noexcept {
    static std::atomic<uint64_t> made{0};
    return made.fetch_add(1, std::memory_order_relaxed) + 1;
}

kit::Point layer_point(const LayerPlacement& placement, float x, float y) noexcept {
    const auto& shown = placement.shown;
    // Drawn at a scale as large as its place: the window pixels over the
    // scale, as the drawing's own edges are lround(point * scale).
    if (placement.scale_percent > 0) {
        const auto map = [&placement](float offset) {
            return static_cast<int32_t>(std::floor(
                static_cast<double>(offset) * 100.0 / static_cast<double>(placement.scale_percent)
            ));
        };
        return {map(x - static_cast<float>(shown.x)), map(y - static_cast<float>(shown.y))};
    }
    const auto map = [](float offset, int32_t length, int32_t points) {
        if (length <= 0)
            return 0;
        return static_cast<int32_t>(
            std::floor(static_cast<double>(offset) * static_cast<double>(points) / length)
        );
    };
    return {
        map(x - static_cast<float>(shown.x), shown.width, placement.points_width),
        map(y - static_cast<float>(shown.y), shown.height, placement.points_height)
    };
}

LayerPlacement
centred_placement(const LayerView& view, int32_t width_points, int32_t height_points) noexcept {
    const int32_t percent = std::max(view.scale_percent, int32_t{100});
    const kit::Rect drawn = kit::to_canvas({0, 0, width_points, height_points}, percent);
    return {
        {view.room.x + floor_div(view.room.width - drawn.width, 2),
         view.room.y + floor_div(view.room.height - drawn.height, 2),
         drawn.width,
         drawn.height},
        width_points,
        height_points,
        percent
    };
}

renderer::Surface layer_drawing(
    int32_t points_width,
    int32_t points_height,
    int32_t scale_percent,
    const kit::Fonts* fonts,
    text_font::FontStack* modern_fonts,
    const artless::RgbaPicture& icon,
    const std::function<void(const kit::Canvas&)>& draw,
    uint8_t ground
) {
    const int32_t percent = std::max(scale_percent, int32_t{100});
    const kit::Rect size =
        kit::to_canvas({0, 0, std::max(points_width, 0), std::max(points_height, 0)}, percent);
    renderer::Surface drawn;
    drawn.width = static_cast<uint32_t>(size.width);
    drawn.height = static_cast<uint32_t>(size.height);
    drawn.rgb.assign(static_cast<std::size_t>(drawn.width) * drawn.height * 3U, ground);
    if (drawn.rgb.empty())
        return drawn;
    kit::Canvas canvas{};
    canvas.fonts = fonts;
    canvas.icon = icon;
    if (percent % 100 == 0) {
        // At the scale itself on the crisp form: never drawn at 1x and enlarged.
        canvas.surface = &drawn;
        canvas.placement = {0, 0, percent / 100};
        draw(canvas);
        return drawn;
    }
    // A fractional scale is laid out and drawn at itself on the smooth form,
    // its every pixel starting opaque at the ground as the crisp form's do.
    paint::Canvas pixels = paint::make_canvas(size.width, size.height);
    for (std::size_t at = 0; at < pixels.rgba.size(); at += 4) {
        pixels.rgba[at] = ground;
        pixels.rgba[at + 1U] = ground;
        pixels.rgba[at + 2U] = ground;
        pixels.rgba[at + 3U] = 255U;
    }
    paint::Painter painter(pixels);
    canvas.smooth.painter = &painter;
    canvas.smooth.fonts = modern_fonts;
    canvas.smooth.scale_percent = percent;
    draw(canvas);
    for (std::size_t pixel = 0; pixel < drawn.rgb.size() / 3U; ++pixel) {
        drawn.rgb[pixel * 3U] = pixels.rgba[pixel * 4U];
        drawn.rgb[pixel * 3U + 1U] = pixels.rgba[pixel * 4U + 1U];
        drawn.rgb[pixel * 3U + 2U] = pixels.rgba[pixel * 4U + 2U];
    }
    return drawn;
}

void round_layer_corners(LayerDrawing& drawing, int32_t radius_points, int32_t scale_percent) {
    renderer::Surface& picture = drawing.picture;
    const auto width = static_cast<int32_t>(picture.width);
    const auto height = static_cast<int32_t>(picture.height);
    if (radius_points <= 0 || width <= 0 || height <= 0)
        return;
    const int32_t percent = scale_percent > 0 ? scale_percent : 100;
    // The radius and the edge's thickness at the canvas's own pixels, as the
    // kit's rounded shapes take them.
    const int32_t radius = std::min(
        kit::to_canvas({0, 0, radius_points, 0}, percent).width, std::min(width, height) / 2
    );
    const int32_t edge = std::max(
        int32_t{1}, static_cast<int32_t>(std::lround(static_cast<double>(percent) / 100.0))
    );
    if (radius <= 0)
        return;
    // Stepped at every scale: a pixel is the window's or clear, so what lies
    // behind shows through whole pixels and no blend of the window's own
    // rounds them differently.
    const std::vector<uint8_t> shape = kit::corner_coverage(radius, 0, true);
    const std::vector<uint8_t> ring = kit::corner_coverage(radius, edge, true);
    if (shape.empty() || ring.size() != shape.size())
        return;
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (drawing.opacity.size() != pixels)
        drawing.opacity.assign(pixels, 255U);
    const kit::Colour light = kit::colour::edge_light;
    const kit::Colour dark = kit::colour::edge_dark;

    // Each corner: whether its column and row count from the right and the
    // bottom, and the colour where it meets the top or bottom edge and where
    // it meets the left or right one.
    struct Corner {
        bool right{};       ///< the corner's columns count from the right
        bool bottom{};      ///< its rows count from the bottom
        kit::Colour across; ///< the edge along the top or bottom
        kit::Colour down;   ///< the edge along the left or right
    };

    const std::array<Corner, 4> corners{{
        {false, false, light, light},
        {true, false, light, dark},
        {false, true, dark, light},
        {true, true, dark, dark},
    }};
    for (const Corner& corner : corners) {
        for (int32_t from_edge_down = 0; from_edge_down < radius; ++from_edge_down) {
            for (int32_t from_edge_across = 0; from_edge_across < radius; ++from_edge_across) {
                const std::size_t sample =
                    static_cast<std::size_t>(from_edge_down) * static_cast<std::size_t>(radius) +
                    static_cast<std::size_t>(from_edge_across);
                const int32_t column =
                    corner.right ? width - 1 - from_edge_across : from_edge_across;
                const int32_t row = corner.bottom ? height - 1 - from_edge_down : from_edge_down;
                const std::size_t at =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(column);
                uint8_t* colour = picture.rgb.data() + at * 3U;
                if (shape[sample] == 0) {
                    drawing.opacity[at] = 0;
                    colour[0] = 0;
                    colour[1] = 0;
                    colour[2] = 0;
                    continue;
                }
                if (ring[sample] == 0)
                    continue;
                // Nearer the top or bottom edge than the side, that edge's
                // colour, and nearer the side the side's; on the diagonal the
                // dark edge but at the top left, as the dark edges run the
                // whole width and height.
                const kit::Colour& ink =
                    from_edge_down < from_edge_across ? corner.across : corner.down;
                const kit::Colour& shown =
                    from_edge_down == from_edge_across && (corner.right || corner.bottom) ? dark
                                                                                          : ink;
                colour[0] = shown.r;
                colour[1] = shown.g;
                colour[2] = shown.b;
            }
        }
    }
}

LayerDrawing clear_drawing(
    int32_t points_width,
    int32_t points_height,
    int32_t scale_percent,
    const kit::Fonts* fonts,
    text_font::FontStack* modern_fonts,
    const artless::RgbaPicture& icon,
    const std::function<void(const kit::Canvas&)>& draw
) {
    LayerDrawing drawing;
    drawing.picture =
        layer_drawing(points_width, points_height, scale_percent, fonts, modern_fonts, icon, draw);
    renderer::Surface& drawn = drawing.picture;
    if (drawn.rgb.empty())
        return drawing;
    // A surface cleared to clear: what is drawn over white tells how much of
    // what lies under each pixel it leaves showing.
    const renderer::Surface over_white = layer_drawing(
        points_width, points_height, scale_percent, fonts, modern_fonts, icon, draw, 255U
    );
    const std::size_t pixels = static_cast<std::size_t>(drawn.width) * drawn.height;
    drawing.opacity.assign(pixels, 0);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        uint8_t* colour = drawn.rgb.data() + pixel * 3U;
        const uint8_t* white = over_white.rgb.data() + pixel * 3U;
        int32_t through = 0;
        for (std::size_t channel = 0; channel < 3U; ++channel)
            through = std::max(through, int32_t{white[channel]} - int32_t{colour[channel]});
        const auto opacity = static_cast<uint32_t>(255 - std::clamp(through, 0, 255));
        drawing.opacity[pixel] = static_cast<uint8_t>(opacity);
        // Straight colours: the colour over black is the colour times its
        // opacity; a clear pixel keeps none.
        for (std::size_t channel = 0; channel < 3U; ++channel)
            colour[channel] =
                opacity == 0 ? uint8_t{0}
                             : static_cast<uint8_t>(
                                   std::min(255U, (colour[channel] * 255U + opacity / 2U) / opacity)
                               );
    }
    return drawing;
}

WindowPosition window_position(const LayerView& view, float x, float y) noexcept {
    if (view.match || view.picture_size.x <= 0 || view.picture_size.y <= 0)
        return {static_cast<double>(x), static_cast<double>(y)};
    // The picture's pixels as the window draws them; a pixel's edge mapped
    // back from the picture lands on it to the nearest 64th.
    const auto along = [](float at, int32_t start, int32_t shown, int32_t size) {
        const double exact = static_cast<double>(start) + static_cast<double>(at) *
                                                              static_cast<double>(shown) /
                                                              static_cast<double>(size);
        return std::round(exact * 64.0) / 64.0;
    };
    return {
        along(x, view.picture.x, view.picture.width, view.picture_size.x),
        along(y, view.picture.y, view.picture.height, view.picture_size.y)
    };
}

bool layer_paste(uint32_t sdl_key, uint16_t modifiers) noexcept {
    if (sdl_key != SDLK_V)
        return false;
#ifdef SDL_PLATFORM_MACOS
    if ((modifiers & SDL_KMOD_GUI) != 0)
        return true;
#endif
    return (modifiers & SDL_KMOD_CTRL) != 0;
}

std::optional<kit::Key> layer_key(uint32_t sdl_key, uint16_t modifiers, bool in_match) noexcept {
    switch (sdl_key) {
    case SDLK_RETURN:
        return kit::Key::enter;
    case SDLK_ESCAPE:
        return kit::Key::escape;
    case SDLK_UP:
        return kit::Key::up;
    case SDLK_DOWN:
        return kit::Key::down;
    case SDLK_LEFT:
        return kit::Key::left;
    case SDLK_RIGHT:
        return kit::Key::right;
    case SDLK_SPACE:
        return kit::Key::space;
    case SDLK_TAB:
        return (modifiers & SDL_KMOD_SHIFT) != 0 ? kit::Key::back_tab : kit::Key::tab;
    case SDLK_PAGEUP:
        return kit::Key::page_up;
    case SDLK_PAGEDOWN:
        return kit::Key::page_down;
    case SDLK_HOME:
        return kit::Key::home;
    case SDLK_END:
        return kit::Key::end;
    case SDLK_Y:
        return kit::Key::yes;
    case SDLK_N:
        return kit::Key::no;
    case SDLK_BACKSPACE:
        return kit::Key::backspace;
    case SDLK_DELETE:
        return kit::Key::delete_forward;
    case SDLK_F1:
        // In a match F1 is the game's unit information.
        if (in_match)
            return std::nullopt;
        return kit::Key::info;
    default:
        return std::nullopt;
    }
}

void Runtime::destroy_oa_layer(OaLayer* layer) noexcept {
    delete layer;
}

OaLayer& Runtime::oa_layer() {
    if (!oa_layer_)
        oa_layer_.reset(new OaLayer(*this));
    return *oa_layer_;
}

void Runtime::register_oa_layer() {
    OverlayDesc layer{};
    layer.name = "oa_layer";
    // On every screen: a screen closes in its tick when the screen it was
    // opened on goes, and the key that closed one is latched wherever it is
    // released.
    layer.screen = kScreenAny;
    layer.z = kLayerOverlayZ;
    layer.event = OaLayer::overlay_event;
    layer.tick = OaLayer::overlay_tick;
    layer.draw = OaLayer::overlay_draw;
    // A refused overlay is recorded in the registry, and register_screens
    // reports it once every screen and overlay is in. The frame keeps its
    // own badge until the overlay is registered.
    if (overlay_register(&screens_, &layer)) {
        mp::multiplayer_bind_layer_presence_badges(&oa_layer(), [](void* context) -> bool {
            return context != nullptr && static_cast<OaLayer*>(context)->draws_presence_badges();
        });
    }
}

OaLayer::OaLayer(Runtime& runtime) noexcept : runtime_(runtime) {
}

struct OaLayer::RegisteredPrompt {
    uint32_t id{};
    void (*answered)(void* user, uint32_t id, int32_t button){};
    void* user{};
    bool done{};
    QuestionScreen* screen{};
};

OaLayer::~OaLayer() {
    mp::multiplayer_bind_layer_presence_badges(nullptr, nullptr);
    drop_pending_prompts();
}

int OaLayer::overlay_event(ScreenContext* context, void*) {
    if (context == nullptr || context->input == nullptr || context->host == nullptr)
        return 0;
    auto& runtime = *static_cast<Runtime*>(context->host);
    return runtime.oa_layer().take_input(*context->input) ? 1 : 0;
}

void OaLayer::overlay_tick(ScreenContext* context, void*) {
    if (context == nullptr || context->host == nullptr)
        return;
    static_cast<Runtime*>(context->host)->oa_layer().tick();
}

void OaLayer::overlay_draw(ScreenContext* context, void*) {
    if (context == nullptr || context->host == nullptr || context->surface == nullptr)
        return;
    auto& runtime = *static_cast<Runtime*>(context->host);
    // A match draws its layer over its composed frame and its other layers.
    if (runtime.screen_ == Screen::match)
        return;
    auto& layer = runtime.oa_layer();
    layer.lay_out_screens();
    layer.darken_front_end(*context->surface);
    // With a renderer the screens go into the window over the picture
    // (present); the frame holds them without one, and for the picture
    // frame_without_cursor returns.
    if (runtime.sdl_.renderer == nullptr || runtime.frame_without_cursor_)
        layer.compose_front_end(*context->surface);
}

void OaLayer::push(std::unique_ptr<LayerScreen> screen) {
    if (!screen)
        return;
    screen->lay_out(view());
    screens_.push_back(std::move(screen));
    button_hovered_ = false;
    button_pressed_ = false;
    ++revision_;
    sync_text_input();
}

void OaLayer::show_when_free(std::unique_ptr<LayerScreen> screen) {
    if (!screen)
        return;
    queue_.push_back(std::move(screen));
    show_waiting();
}

void OaLayer::close(const LayerScreen* screen) {
    if (screen == nullptr)
        return;
    // A screen that is taking an input or its tick leaves once that is done.
    if (screen == calling_) {
        close_asked_ = true;
        return;
    }
    if (holds(screen))
        remove(screen, false);
    else
        drop_waiting(screen);
    show_waiting();
}

void OaLayer::close_top() {
    if (!screens_.empty())
        remove(screens_.back().get(), false);
}

void OaLayer::close_above(std::string_view name) {
    for (;;) {
        const LayerScreen* named = find(name);
        if (named == nullptr || screens_.back().get() == named)
            return;
        remove(screens_.back().get(), false);
    }
}

void OaLayer::return_to(std::string_view name) {
    const LayerScreen* named = find(name);
    if (named == nullptr)
        return;
    std::vector<const LayerScreen*> above;
    for (auto it = screens_.rbegin(); it != screens_.rend() && it->get() != named; ++it)
        above.push_back(it->get());
    for (const LayerScreen* screen : above)
        close(screen);
}

LayerScreen* OaLayer::top() const noexcept {
    return screens_.empty() ? nullptr : screens_.back().get();
}

LayerScreen* OaLayer::find(std::string_view name) const noexcept {
    for (auto it = screens_.rbegin(); it != screens_.rend(); ++it)
        if ((*it)->name() == name)
            return it->get();
    return nullptr;
}

LayerScreen* OaLayer::waiting(std::string_view name) const noexcept {
    for (const auto& screen : queue_)
        if (screen->name() == name)
            return screen.get();
    return nullptr;
}

const kit::Fonts* OaLayer::screen_fonts() const {
    return runtime_.engine_settings_fonts();
}

text_font::FontStack* OaLayer::smooth_fonts() const {
    if (!smooth_fonts_tried_) {
        smooth_fonts_tried_ = true;
        try {
            const uint32_t fonts_began = oa::platform::tick_ms();
            smooth_fonts_ = text_font::FontStack::open(text_font::bundled_font_directory());
            oa::platform::log_files::note_timing_span("fonts", fonts_began);
        } catch (const std::exception&) {
            smooth_fonts_.reset();
        }
    }
    // The language shown may add faces of its pack, for its own letters.
    std::ignore = runtime_.follow_language_fonts(smooth_fonts_.get(), smooth_fonts_generation_);
    return smooth_fonts_.get();
}

LayerView OaLayer::view() const {
    LayerView seen{};
    seen.match = runtime_.screen_ == Screen::match;
    seen.game_screen = runtime_.screen_;
    // The Interface size in effect, chosen in Settings as it is chosen.
    // Cancel puts back the size the dialog opened with, and OK keeps it.
    // 150% is drawn in the modern fonts, and is 100% while they are off or
    // cannot be opened.
    const settings::EngineSettings& current = runtime_.engine_settings_state().current;
    settings::InterfaceSize chosen = current.interface_size;
    if (chosen == settings::InterfaceSize::size_150)
        chosen = settings::interface_size_with_fonts(
            chosen, current.modern_fonts && smooth_fonts() != nullptr
        );
    seen.interface_percent = settings::interface_size_percent(chosen);
    // Auto counts 150% among its steps only while the modern fonts are on
    // and the layer's stack of them opened.
    if (chosen == settings::InterfaceSize::automatic && current.modern_fonts &&
        smooth_fonts() != nullptr)
        seen.interface_percent = kit::auto_with_fraction_percent;
    seen.window_size = window_size();
    if (seen.match) {
        // The in-match Settings takes its own class and scale from the area
        // beside the side column (match_dialog).
        seen.match_layout = &runtime_.match_layout_;
        seen.fit_safe_area = match_fits_safe_area(runtime_);
        const auto safe = match_safe_area(runtime_.match_layout_);
        seen.frame.area = {safe.x, safe.y, safe.width, safe.height};
        seen.frame.size_class = kit::SizeClass::compact;
        seen.frame.scale_percent = 100;
        seen.room = safe;
        seen.picture = {0, 0, runtime_.match_layout_.width, runtime_.match_layout_.height};
        seen.picture_size = {runtime_.match_layout_.width, runtime_.match_layout_.height};
        return seen;
    }
    // The front end's picture, which without a renderer is the canvas too.
    const auto picture_width = static_cast<int32_t>(runtime_.surface_.width);
    const auto picture_height = static_cast<int32_t>(runtime_.surface_.height);
    seen.picture_size = {picture_width, picture_height};
    seen.picture = {0, 0, picture_width, picture_height};
    kit::Viewport viewport;
    viewport.width = picture_width;
    viewport.height = picture_height;
    SDL_Renderer* sdl_renderer = runtime_.sdl_.renderer;
    SDL_Window* window = runtime_.sdl_.window;
    if (sdl_renderer != nullptr && window != nullptr) {
        int frame_w = picture_width;
        int frame_h = picture_height;
        int logical_width = 0;
        int logical_height = 0;
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
        if (SDL_GetRenderLogicalPresentation(
                sdl_renderer, &logical_width, &logical_height, &mode
            ) &&
            mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_width > 0 && logical_height > 0) {
            frame_w = logical_width;
            frame_h = logical_height;
        }
        const WindowSurface surface = window_surface(window, sdl_renderer);
        if (surface.pixel_width > 0 && surface.pixel_height > 0 && surface.points_width > 0) {
            // The canvas in pixels, and the points a pixel's density lays out
            // at. A 2× surface of a 1000×628 point window is that window.
            viewport.width = surface.pixel_width;
            viewport.height = surface.pixel_height;
            viewport.density = surface.density;
            SDL_Rect safe{};
            if (runtime_.touch_controls_active() && SDL_GetWindowSafeArea(window, &safe) &&
                safe.w > 0 && safe.h > 0) {
                viewport.safe = {
                    surface_canvas_pixels(surface, safe.x, true),
                    surface_canvas_pixels(surface, safe.y, false),
                    surface_canvas_pixels(surface, surface.sdl_width - safe.x - safe.w, true),
                    surface_canvas_pixels(surface, surface.sdl_height - safe.y - safe.h, false)
                };
            }
            const bool whole_steps = mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE ||
                                     (mode == SDL_LOGICAL_PRESENTATION_DISABLED &&
                                      render_policy::frame_fit(
                                          runtime_.menu_scaling(),
                                          surface.pixel_width,
                                          surface.pixel_height,
                                          frame_w,
                                          frame_h
                                      ) == render_policy::FrameFit::whole_steps);
            const PlacedFrame placed =
                place_frame(window, sdl_renderer, frame_w, frame_h, whole_steps);
            if (placed.drawn.w > 0.0F && placed.drawn.h > 0.0F) {
                const SDL_Rect drawn = drawn_frame_rect(placed.drawn);
                seen.picture = {drawn.x, drawn.y, drawn.w, drawn.h};
                seen.picture_size = {frame_w, frame_h};
            }
        }
    }
    seen.room = {
        viewport.safe.left,
        viewport.safe.top,
        viewport.width - viewport.safe.left - viewport.safe.right,
        viewport.height - viewport.safe.top - viewport.safe.bottom
    };
    // The Interface size in effect, or the Auto scale serving the class the
    // Window size wants, no larger than lets Compact's dialog fit the room,
    // and at least 1; the class from the points left over, or the Window
    // size's held to them.
    viewport = kit::layer_viewport(
        viewport, seen.interface_percent, window_placement::wanted_class(seen.window_size)
    );
    seen.scale_percent = viewport.scale_percent;
    seen.scale = std::max(viewport.scale_percent / 100, int32_t{1});
    seen.frame = kit::frame_of(viewport);
    seen.frame.size_class = window_placement::class_under(
        seen.window_size, seen.frame.size_class, seen.frame.area.width, seen.frame.area.height
    );
    return seen;
}

LayerPlacement OaLayer::placement_of(std::string_view name) const {
    const LayerScreen* named = find(name);
    if (named == nullptr)
        return {};
    return shown_placement(*named, view());
}

LayerPlacement OaLayer::shown_placement(const LayerScreen& screen, const LayerView& seen) const {
    LayerPlacement own = screen.placement(seen);
    // A window that moves takes its class's window radius unless it gives
    // its own; one that fills its whole room keeps square corners.
    if (own.corner_radius <= 0 && window_placement::moves(screen.name()))
        own.corner_radius = kit::metrics_of(window_class(seen)).window_radius;
    if (empty_rect(own.shown) ||
        (own.shown.width >= seen.room.width && own.shown.height >= seen.room.height))
        own.corner_radius = 0;
    const auto room = window_room(screen, seen, own);
    if (!room)
        return own;
    const kit::Point offset =
        drag_ && dragged_ == &screen
            ? drag_->offset
            : window_placement::stored_offset(
                  runtime_.preference_values_, screen.name(), window_class(seen)
              );
    const kit::Rect shown = window_placement::shown_at(*room, offset);
    own.shown = {shown.x, shown.y, shown.width, shown.height};
    return own;
}

std::optional<window_placement::WindowRoom> OaLayer::window_room(
    const LayerScreen& screen, const LayerView& seen, const LayerPlacement& own
) const {
    // A window fitted to a place of its own size, as the phone's Settings
    // is, and one that fills the whole room stay where they are.
    if (!window_placement::moves(screen.name()) || own.scale_percent <= 0 ||
        empty_rect(own.shown) || empty_rect(seen.room))
        return std::nullopt;
    if (own.shown.width >= seen.room.width && own.shown.height >= seen.room.height)
        return std::nullopt;
    window_placement::WindowRoom room;
    room.room = {seen.room.x, seen.room.y, seen.room.width, seen.room.height};
    room.centred = {own.shown.x, own.shown.y, own.shown.width, own.shown.height};
    room.scale_percent = own.scale_percent;
    return room;
}

kit::SizeClass OaLayer::window_class(const LayerView& seen) const {
    if (seen.match && seen.match_layout != nullptr)
        return match_dialog(
                   *seen.match_layout, seen.interface_percent, seen.fit_safe_area, seen.window_size
        )
            .size_class;
    return seen.frame.size_class;
}

window_placement::WindowSize OaLayer::window_size() const {
    return window_placement::stored_window_size(runtime_.preference_values_);
}

void OaLayer::set_window_size(window_placement::WindowSize size) {
    const std::string key(window_placement::window_size_key);
    bool changed = false;
    if (size == window_placement::WindowSize::automatic) {
        changed = runtime_.preference_values_.erase(key) != 0;
    } else {
        const std::string word(window_placement::window_size_word(size));
        std::string& stored = runtime_.preference_values_[key];
        changed = stored != word;
        stored = word;
    }
    if (changed)
        write_window_preferences();
    ++revision_;
    lay_out_screens();
}

bool OaLayer::reset_window_places() {
    drag_.reset();
    dragged_ = nullptr;
    ++revision_;
    if (!window_placement::forget_places(runtime_.preference_values_))
        return false;
    write_window_preferences();
    return true;
}

kit::Point OaLayer::window_offset(std::string_view name) const {
    const LayerScreen* named = find(name);
    if (named == nullptr)
        return {};
    const LayerView seen = view();
    const auto room = window_room(*named, seen, named->placement(seen));
    if (!room)
        return {};
    const LayerPlacement shown = shown_placement(*named, seen);
    return window_placement::offset_at(
        *room, {shown.shown.x, shown.shown.y, shown.shown.width, shown.shown.height}
    );
}

void OaLayer::remember_window(
    std::string_view window, kit::SizeClass size_class, kit::Point offset
) {
    ++revision_;
    if (window_placement::store_offset(runtime_.preference_values_, window, size_class, offset))
        write_window_preferences();
}

void OaLayer::write_window_preferences() {
    runtime_.preferences_dirty_ = true;
    try {
        runtime_.flush_preferences();
    } catch (const std::exception& error) {
        if (window_write_failure_told_)
            return;
        window_write_failure_told_ = true;
        std::cerr << "open-annihilation: a window's place was not saved: " << error.what() << '\n';
    }
}

bool OaLayer::take_window_move(const LayerView& seen, const ScreenInput& input) {
    namespace places = window_placement;
    const WindowPosition at = window_position(seen, input.x, input.y);
    const kit::Point pointer{
        static_cast<int32_t>(std::floor(at.x)), static_cast<int32_t>(std::floor(at.y))
    };
    if (drag_) {
        // A drag ends once the window it moves leaves or no longer moves.
        std::optional<places::WindowRoom> room;
        if (dragged_ != nullptr && holds(dragged_))
            room = window_room(*dragged_, seen, dragged_->placement(seen));
        if (!room) {
            drag_.reset();
            dragged_ = nullptr;
        } else {
            switch (input.kind) {
            case ScreenInputKind::pointer_move: {
                const kit::Point before = drag_->offset;
                if (places::drag_move(*drag_, *room, pointer, SDL_GetTicks()) ==
                    places::DragStep::cancelled) {
                    drag_.reset();
                    dragged_ = nullptr;
                } else if (drag_->offset.x != before.x || drag_->offset.y != before.y) {
                    ++revision_;
                }
                return true;
            }
            case ScreenInputKind::pointer_up: {
                if (input.button != SDL_BUTTON_LEFT)
                    return true;
                const places::TitleDrag ended = *drag_;
                drag_.reset();
                dragged_ = nullptr;
                if (ended.moving)
                    remember_window(ended.window, ended.size_class, ended.offset);
                return true;
            }
            case ScreenInputKind::pointer_down:
                return true;
            default:
                // Keys, text and the wheel go on to the screens.
                return false;
            }
        }
    }
    if (input.kind == ScreenInputKind::key_down && (input.modifiers & kInputModifierAlt) != 0) {
        int32_t across = 0;
        int32_t down = 0;
        switch (input.key) {
        case SDLK_LEFT:
            across = -1;
            break;
        case SDLK_RIGHT:
            across = 1;
            break;
        case SDLK_UP:
            down = -1;
            break;
        case SDLK_DOWN:
            down = 1;
            break;
        default:
            return false;
        }
        LayerScreen* focused = top_modal(seen);
        if (focused == nullptr)
            return false;
        const auto room = window_room(*focused, seen, focused->placement(seen));
        if (!room)
            return false;
        const kit::SizeClass size_class = window_class(seen);
        const kit::Point offset =
            places::stored_offset(runtime_.preference_values_, focused->name(), size_class);
        const bool large = (input.modifiers & SDL_KMOD_SHIFT) != 0;
        remember_window(
            focused->name(), size_class, places::moved_by_key(*room, offset, across, down, large)
        );
        return true;
    }
    if (input.kind != ScreenInputKind::pointer_down || input.button != SDL_BUTTON_LEFT)
        return false;
    // The screen the press reaches: the top modal one, else the topmost
    // that shows under it.
    LayerScreen* pressed = top_modal(seen);
    if (pressed == nullptr)
        for (auto it = screens_.rbegin(); it != screens_.rend() && pressed == nullptr; ++it) {
            const LayerPlacement shown = shown_placement(**it, seen);
            if (!empty_rect(shown.shown) &&
                rect_contains(shown.shown, static_cast<float>(at.x), static_cast<float>(at.y)))
                pressed = it->get();
        }
    if (pressed == nullptr)
        return false;
    const auto room = window_room(*pressed, seen, pressed->placement(seen));
    if (!room)
        return false;
    // The title: the window's header, where no control lies.
    const LayerPlacement shown = shown_placement(*pressed, seen);
    const kit::Point point = layer_point(shown, static_cast<float>(at.x), static_cast<float>(at.y));
    const kit::DisplayList list = pressed->display_list();
    const auto header = std::find_if(list.items.begin(), list.items.end(), [](const auto& item) {
        return item.role == kit::Role::header;
    });
    if (header == list.items.end() || !kit::contains(header->rect, point) ||
        kit::hit(list, point) != kit::no_control)
        return false;
    const kit::SizeClass size_class = window_class(seen);
    if (input.clicks >= 2) {
        // A double-click puts the window back in its centre.
        drag_.reset();
        dragged_ = nullptr;
        remember_window(pressed->name(), size_class, {});
        return true;
    }
    places::TitleDrag drag;
    drag.window = std::string(pressed->name());
    drag.size_class = size_class;
    drag.grab = pointer;
    drag.start_offset = places::offset_at(
        *room, {shown.shown.x, shown.shown.y, shown.shown.width, shown.shown.height}
    );
    drag.offset = drag.start_offset;
    drag.finger = runtime_.engine_settings_state().finger_pointer;
    drag.moving = !drag.finger;
    drag.pressed_ms = SDL_GetTicks();
    drag_ = std::move(drag);
    dragged_ = pressed;
    return true;
}

void OaLayer::lay_out_screens() {
    const LayerView seen = view();
    for (const auto& screen : screens_)
        screen->lay_out(seen);
    for (const auto& screen : queue_)
        screen->lay_out(seen);
}

void OaLayer::lay_out_for_size() {
    lay_out_screens();
}

bool OaLayer::shows(bool match) const {
    const LayerView seen = view();
    if (seen.match != match)
        return false;
    return std::any_of(screens_.begin(), screens_.end(), [&seen](const auto& screen) {
        return !empty_rect(screen->placement(seen).shown);
    });
}

bool OaLayer::modal_shown(bool match) const {
    const LayerView seen = view();
    return seen.match == match && top_modal(seen) != nullptr;
}

LayerScreen* OaLayer::top_modal(const LayerView& seen) const {
    for (auto it = screens_.rbegin(); it != screens_.rend(); ++it)
        if ((*it)->modal() && !empty_rect((*it)->placement(seen).shown))
            return it->get();
    return nullptr;
}

bool OaLayer::holds(const LayerScreen* screen) const noexcept {
    return std::any_of(screens_.begin(), screens_.end(), [screen](const auto& held) {
        return held.get() == screen;
    });
}

bool OaLayer::queued(const LayerScreen* screen) const noexcept {
    return std::any_of(queue_.begin(), queue_.end(), [screen](const auto& waiting_screen) {
        return waiting_screen.get() == screen;
    });
}

std::vector<uint32_t> OaLayer::backdrops_above(const LayerView& seen) const {
    std::vector<uint32_t> above(screens_.size(), 0U);
    uint32_t darkening = 0;
    for (std::size_t index = screens_.size(); index > 0; --index) {
        above[index - 1] = darkening;
        const auto& screen = *screens_[index - 1];
        if (darkens(screen) && !empty_rect(screen.placement(seen).shown))
            ++darkening;
    }
    return above;
}

bool OaLayer::take_input(const ScreenInput& input) {
    // The key that closed a screen does nothing more until it is released,
    // so that a held key never reaches what lies under the layer.
    if (latched_key_ != 0 && input.key == latched_key_ &&
        (input.kind == ScreenInputKind::key_down || input.kind == ScreenInputKind::key_up)) {
        if (input.kind == ScreenInputKind::key_up)
            latched_key_ = 0;
        return true;
    }
    lay_out_screens();
    const LayerView seen = view();
    // A window's title, held or pressed, and Alt with an arrow move it.
    if (take_window_move(seen, input)) {
        show_waiting();
        sync_text_input();
        return true;
    }
    const uint32_t key_down = input.kind == ScreenInputKind::key_down ? input.key : 0U;
    bool taken = false;
    if (LayerScreen* modal = top_modal(seen)) {
        // A modal screen takes every input: nothing under it sees any.
        take_answer(*modal, deliver(*modal, seen, input), key_down);
        taken = true;
    } else {
        // Each screen that shows sees it from the top; the first that answers
        // other than pass takes it. A move that only changes a screen's own
        // hover is answered pass, and goes on.
        std::vector<LayerScreen*> shown_screens;
        for (auto it = screens_.rbegin(); it != screens_.rend(); ++it)
            if (!empty_rect((*it)->placement(seen).shown))
                shown_screens.push_back(it->get());
        for (LayerScreen* screen : shown_screens) {
            if (!holds(screen))
                continue;
            const LayerAnswer answer = deliver(*screen, seen, input);
            if (answer == LayerAnswer::pass)
                continue;
            take_answer(*screen, answer, key_down);
            taken = true;
            break;
        }
        if (!taken && seen.match)
            taken = take_match_button(input);
    }
    // A screen that closed may free the layer for the next one waiting.
    show_waiting();
    sync_text_input();
    return taken;
}

LayerAnswer OaLayer::deliver(LayerScreen& screen, const LayerView& seen, const ScreenInput& input) {
    calling_ = &screen;
    close_asked_ = false;
    const LayerAnswer answer = hand_input(screen, seen, input);
    calling_ = nullptr;
    // A screen whose host closed it while it took the input leaves now.
    return std::exchange(close_asked_, false) ? LayerAnswer::close : answer;
}

namespace {

/// The kit key a screen action delivers. Cancel, paste and a screen's own
/// shortcut are answered apart from these.
std::optional<kit::Key> screen_kit_key(std::string_view action) {
    if (action == "ui.screen-accept")
        return kit::Key::enter;
    if (action == "ui.screen-up")
        return kit::Key::up;
    if (action == "ui.screen-down")
        return kit::Key::down;
    if (action == "ui.screen-left")
        return kit::Key::left;
    if (action == "ui.screen-right")
        return kit::Key::right;
    if (action == "ui.screen-press")
        return kit::Key::space;
    if (action == "ui.screen-focus-next")
        return kit::Key::tab;
    if (action == "ui.screen-focus-previous")
        return kit::Key::back_tab;
    if (action == "ui.screen-page-up")
        return kit::Key::page_up;
    if (action == "ui.screen-page-down")
        return kit::Key::page_down;
    if (action == "ui.screen-home")
        return kit::Key::home;
    if (action == "ui.screen-end")
        return kit::Key::end;
    if (action == "ui.screen-yes")
        return kit::Key::yes;
    if (action == "ui.screen-no")
        return kit::Key::no;
    if (action == "ui.screen-info")
        return kit::Key::info;
    return std::nullopt;
}

} // namespace

LayerAnswer
OaLayer::hand_input(LayerScreen& screen, const LayerView& seen, const ScreenInput& input) {
    const LayerPlacement placed = shown_placement(screen, seen);
    // The pointer in the window's pixels, mapped to the screen's points by
    // its placement.
    const WindowPosition at = window_position(seen, input.x, input.y);
    const kit::Point point =
        layer_point(placed, static_cast<float>(at.x), static_cast<float>(at.y));
    switch (input.kind) {
    case ScreenInputKind::pointer_move:
    case ScreenInputKind::pointer_down:
    case ScreenInputKind::pointer_up: {
        // A finger's reach, in the screen's points as it shows: the pick
        // distance in the canvas the touch controls measure it in (the
        // picture's pixels on the front end), over the canvas pixels a
        // point takes.
        int32_t reach = 0;
        if (runtime_.engine_settings_state().finger_pointer && placed.points_width > 0 &&
            seen.picture.width > 0) {
            const double window_per_point =
                static_cast<double>(placed.shown.width) / static_cast<double>(placed.points_width);
            const double canvas_per_window =
                static_cast<double>(seen.picture_size.x) / static_cast<double>(seen.picture.width);
            reach = Runtime::EngineSettingsState::finger_reach(
                runtime_, window_per_point * canvas_per_window
            );
        }
        return screen.pointer_with_clicks(input.kind, input.button, point, reach, input.clicks);
    }
    case ScreenInputKind::wheel:
        return screen.wheel(point, input.wheel_y);
    case ScreenInputKind::key_down: {
        const LayerAnswer own = screen.shortcut(input.key, input.modifiers);
        if (own != LayerAnswer::pass)
            return own;
        // Backspace, Delete, and Home, End and the left and right arrows in a
        // focused field stay text editing. They are the physical keys, so a
        // layout cannot move them off the field.
        if (const auto editing = layer_key(input.key, input.modifiers, seen.match)) {
            if (*editing == kit::Key::backspace || *editing == kit::Key::delete_forward)
                return screen.key(*editing, input.key);
            if (screen.text_field() && (*editing == kit::Key::home || *editing == kit::Key::end ||
                                        *editing == kit::Key::left || *editing == kit::Key::right))
                return screen.key(*editing, input.key);
        }
        const std::string action = runtime_.screen_action_for_key(input.key, input.modifiers);
        // Escape always cancels the screen on top, whichever action a layout
        // gives the key in another context.
        if (action == "ui.screen-cancel" || input.key == SDLK_ESCAPE)
            return screen.key(kit::Key::escape, input.key);
        if (const std::optional<kit::Key> pressed = screen_kit_key(action)) {
            // In a match the information key is the game's unit information.
            if (*pressed == kit::Key::info && seen.match)
                return LayerAnswer::pass;
            return screen.key(*pressed, input.key);
        }
        // Paste gives a focused field the clipboard's text up to its first
        // line break, as typed text arrives. With no field, the key goes on.
        if (action == "ui.screen-paste" && screen.text_field()) {
            char* clipboard = SDL_GetClipboardText();
            std::string pasted = clipboard != nullptr ? clipboard : "";
            SDL_free(clipboard);
            pasted = pasted.substr(0, pasted.find_first_of("\r\n"));
            if (pasted.empty())
                return LayerAnswer::none;
            return screen.text(pasted);
        }
        return LayerAnswer::pass;
    }
    case ScreenInputKind::text:
        return screen.text(
            input.text != nullptr ? std::string_view(input.text) : std::string_view()
        );
    case ScreenInputKind::key_up:
        return screen.key_up(input.key, input.modifiers);
    }
    return LayerAnswer::pass;
}

bool OaLayer::finish_pad(LayerScreen& screen, LayerAnswer answer) {
    calling_ = nullptr;
    if (std::exchange(close_asked_, false))
        answer = LayerAnswer::close;
    if (answer == LayerAnswer::pass)
        return false;
    take_answer(screen, answer, 0);
    show_waiting();
    sync_text_input();
    return true;
}

bool OaLayer::take_pad_action(oa::ui::pad_controls::Action action) {
    lay_out_screens();
    const LayerView seen = view();
    // A modal screen is the only one offered the action, so Y under a
    // question never reaches the button. One it passes on becomes a key or a
    // click, which the modal screen then takes as input.
    if (LayerScreen* modal = top_modal(seen)) {
        if (!holds(modal))
            return false;
        calling_ = modal;
        close_asked_ = false;
        return finish_pad(*modal, modal->pad_action(action));
    }
    // The same walk as take_input: top down, skipping a screen that is not
    // showing, until one answers other than pass.
    std::vector<LayerScreen*> shown_screens;
    for (auto it = screens_.rbegin(); it != screens_.rend(); ++it)
        if (!empty_rect((*it)->placement(seen).shown))
            shown_screens.push_back(it->get());
    for (LayerScreen* screen : shown_screens) {
        if (!holds(screen))
            continue;
        calling_ = screen;
        close_asked_ = false;
        if (finish_pad(*screen, screen->pad_action(action)))
            return true;
    }
    return false;
}

bool OaLayer::take_pad_stick(float notches) {
    lay_out_screens();
    const LayerView seen = view();
    if (LayerScreen* modal = top_modal(seen)) {
        if (!holds(modal))
            return false;
        calling_ = modal;
        close_asked_ = false;
        return finish_pad(*modal, modal->pad_stick(notches));
    }
    std::vector<LayerScreen*> shown_screens;
    for (auto it = screens_.rbegin(); it != screens_.rend(); ++it)
        if (!empty_rect((*it)->placement(seen).shown))
            shown_screens.push_back(it->get());
    for (LayerScreen* screen : shown_screens) {
        if (!holds(screen))
            continue;
        calling_ = screen;
        close_asked_ = false;
        if (finish_pad(*screen, screen->pad_stick(notches)))
            return true;
    }
    return false;
}

void OaLayer::take_answer(LayerScreen& screen, LayerAnswer answer, uint32_t key_down) {
    if (answer != LayerAnswer::close || !holds(&screen))
        return;
    // The key that closed the screen does nothing more until it is released.
    if (key_down != 0)
        latched_key_ = key_down;
    remove(&screen, key_down != 0);
    show_waiting();
}

void OaLayer::remove(const LayerScreen* screen, bool by_key) {
    const auto it = std::find_if(screens_.begin(), screens_.end(), [screen](const auto& held) {
        return held.get() == screen;
    });
    if (it == screens_.end())
        return;
    std::unique_ptr<LayerScreen> leaving = std::move(*it);
    screens_.erase(it);
    if (dragged_ == screen) {
        drag_.reset();
        dragged_ = nullptr;
    }
    button_hovered_ = false;
    button_pressed_ = false;
    ++revision_;
    leaving->close(by_key);
    sync_text_input();
}

void OaLayer::drop_waiting(const LayerScreen* screen) {
    const auto it = std::find_if(queue_.begin(), queue_.end(), [screen](const auto& held) {
        return held.get() == screen;
    });
    if (it == queue_.end())
        return;
    std::unique_ptr<LayerScreen> leaving = std::move(*it);
    queue_.erase(it);
    leaving->close(false);
}

void OaLayer::show_waiting() {
    // One at a time. Nothing is promoted over a modal screen, a frontend
    // message box, or a match being played. The next waits while one shows.
    while (!queue_.empty() && runtime_.screen_ != Screen::match &&
           oa::ui::frontend_dialogs::dialog_count() == 0 && top_modal(view()) == nullptr) {
        std::unique_ptr<LayerScreen> next = std::move(queue_.front());
        queue_.erase(queue_.begin());
        push(std::move(next));
    }
}

namespace {

/// Tells whether a notice's or a prompt's paragraphs can be copied.
///
/// @param paragraphs the paragraphs; null only while none are given
/// @param count how many paragraphs are given
/// @return false when a paragraph is missing
bool paragraphs_ok(const char* const* paragraphs, std::size_t count) noexcept {
    if (count == 0)
        return true;
    if (paragraphs == nullptr)
        return false;
    for (std::size_t index = 0; index < count; ++index)
        if (paragraphs[index] == nullptr)
            return false;
    return true;
}

/// Gives one drawn part the pointer's hover and press, on its look, so the
/// painter shows them. The extension's list is not this copy.
///
/// @param[in,out] item one part of the copy
/// @param interaction the pointer and the focus
void show_pointer(kit::Item& item, const kit::Interaction& interaction) {
    if (item.control == kit::no_control)
        return;
    const bool over = item.control == interaction.hovered;
    const bool held = item.control == interaction.pressed;
    std::visit(
        [&](auto& look) {
            using Look = std::decay_t<decltype(look)>;
            if constexpr (requires(Look& value) { value.hovered = true; })
                look.hovered = over || held;
            if constexpr (requires(Look& value) { value.held = true; })
                look.held = over && held;
            if constexpr (std::is_same_v<Look, kit::OaButtonLook>) {
                if (over && held)
                    look.state = kit::OaButtonState::pressed;
                else if (over || held)
                    look.state = kit::OaButtonState::hovered;
                else
                    look.state = kit::OaButtonState::idle;
            }
        },
        item.look
    );
}

/// A registered kit screen: modal, over a backdrop, filling the front end's
/// room. Its layout is the descriptor's, drawn with the kit. act returning 2
/// closes it; closed runs once, from close.
class RegisteredKitScreen final : public LayerScreen {
  public:

    /// Makes the screen from a descriptor copied at the open.
    ///
    /// @param name the screen's name, copied
    /// @param desc the descriptor; its function pointers and model are copied
    RegisteredKitScreen(std::string name, KitScreenDesc desc)
        : name_(std::move(name)), desc_(desc) {}

    /// Returns the screen's name.
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return name_; }

    /// Returns the front end's room, in window pixels, as wide and tall as
    /// the frame's area in points. Empty in a match, or while the room or
    /// the area is empty.
    ///
    /// @param seen what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& seen) const override {
        if (seen.match || seen.room.width <= 0 || seen.room.height <= 0 ||
            seen.frame.area.width <= 0 || seen.frame.area.height <= 0)
            return {};
        return {seen.room, seen.frame.area.width, seen.frame.area.height};
    }

    /// Tells that the screen takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the screen darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the screen out from its descriptor when the frame changes.
    ///
    /// @param seen what the screen is placed on
    void lay_out(const LayerView& seen) override {
        if (desc_.lay_out == nullptr)
            return;
        if (has_frame_ && seen.frame.area.width == frame_.area.width &&
            seen.frame.area.height == frame_.area.height &&
            seen.frame.size_class == frame_.size_class &&
            seen.frame.scale_percent == frame_.scale_percent)
            return;
        frame_ = seen.frame;
        has_frame_ = true;
        list_ = desc_.lay_out(desc_.model, frame_);
        ++revision_;
    }

    /// Draws a copy of the screen's display list, with the pointer's hover
    /// and press and the focus ring on it.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, shown_list()); }

    /// Takes a pointer's move, press or release. A release over the control
    /// it held is a press of that control (act, with enter). A hover or a
    /// press only redraws.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach in the screen's points; 0 for a mouse
    /// @return what the screen did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const auto before = pointed_look(interaction_);
        const LayerAnswer answer = take_pointer(kind, button, at, finger_reach);
        return with_moved_look(answer, pointed_look(interaction_) != before, revision_);
    }

    /// Takes a key. A move of the focus redraws and does not call act. A key
    /// the focused control takes calls act with that control. Any other key
    /// calls act with no control.
    ///
    /// @param pressed the key
    /// @return what the screen did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        const auto before = pointed_look(interaction_);
        const LayerAnswer answer = take_key(pressed);
        return with_moved_look(answer, pointed_look(interaction_) != before, revision_);
    }

    /// Takes a turn of the wheel, which the screen swallows.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the screen's display list, with the hover, the press and the
    /// focus ring it draws.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override { return shown_list(); }

    /// Returns the screen's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Has nothing to do once a frame.
    ///
    /// @return none
    LayerAnswer tick() override { return LayerAnswer::none; }

    /// Returns the screen's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the screen draws its layout and its pointer and focus
    /// alone, each change of which moves its revision.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Tells the descriptor the screen closed, once.
    void close(bool /*by_key*/) override {
        if (closed_ || desc_.closed == nullptr)
            return;
        closed_ = true;
        desc_.closed(desc_.model);
    }

  private:

    /// Takes a pointer's move, press or release, as pointer does, before
    /// the look it moved is counted.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach in the screen's points; 0 for a mouse
    /// @return what the screen did
    LayerAnswer
    take_pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) {
        kit::PointerOutcome outcome{};
        switch (kind) {
        case ScreenInputKind::pointer_move:
            outcome = kit::pointer_move(interaction_, list_, at);
            break;
        case ScreenInputKind::pointer_down:
            if (button != SDL_BUTTON_LEFT)
                return LayerAnswer::none;
            outcome = finger_reach > 0 ? kit::finger_down(interaction_, list_, at, finger_reach)
                                       : kit::pointer_down(interaction_, list_, at);
            break;
        case ScreenInputKind::pointer_up:
            if (button != SDL_BUTTON_LEFT)
                return LayerAnswer::none;
            outcome = kit::pointer_up(interaction_, list_, at);
            break;
        default:
            return LayerAnswer::none;
        }
        if (outcome.result == kit::PointerResult::activated)
            return take_act({outcome.control, kit::Key::enter});
        if (outcome.result == kit::PointerResult::redraw) {
            ++revision_;
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Takes a key, as key does, before the look it moved is counted.
    ///
    /// @param pressed the key
    /// @return what the screen did
    LayerAnswer take_key(kit::Key pressed) {
        const kit::KeyOutcome outcome = kit::key(interaction_, list_, pressed);
        if (outcome.result == kit::KeyResult::redraw) {
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (outcome.result == kit::KeyResult::to_control)
            return take_act({outcome.control, outcome.key});
        return take_act({kit::no_control, outcome.key});
    }

    /// Gives one action to the descriptor. 0 changes nothing, 1 lays the
    /// screen out again, and 2 closes it. The layer calls close, which tells
    /// the descriptor; this does not.
    ///
    /// @param action the press or the key
    /// @return what the screen answers
    LayerAnswer take_act(const KitScreenAction& action) {
        if (desc_.act == nullptr)
            return LayerAnswer::none;
        const int result = desc_.act(desc_.model, action);
        if (result == 2)
            return LayerAnswer::close;
        if (result == 1) {
            if (desc_.lay_out != nullptr && has_frame_)
                list_ = desc_.lay_out(desc_.model, frame_);
            ++revision_;
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Returns a copy of the layout with the pointer's hover and press and
    /// the focus ring marked on it. The extension's list is left as lay_out
    /// returned it.
    ///
    /// @return the list the screen draws
    [[nodiscard]] kit::DisplayList shown_list() const {
        kit::DisplayList shown = list_;
        std::vector<kit::Item> items;
        items.reserve(shown.items.size());
        for (kit::Item& item : shown.items) {
            if (item.role == kit::Role::focus_ring)
                continue;
            show_pointer(item, interaction_);
            items.push_back(std::move(item));
        }
        shown.items = std::move(items);
        kit::mark_states(shown, interaction_);
        if (interaction_.focus_shown)
            if (const kit::Control* control = kit::control_of(shown, interaction_.focused))
                kit::add_focus_ring(shown, control->rect, true);
        return shown;
    }

    std::string name_;               ///< the screen's name
    KitScreenDesc desc_{};           ///< the descriptor, copied at the open
    kit::DisplayList list_{};        ///< what it draws, from lay_out
    kit::Interaction interaction_{}; ///< the pointer and the focus
    kit::Frame frame_{};             ///< the frame it was laid out for
    bool has_frame_{};               ///< frame_ was set by lay_out
    uint64_t revision_{};            ///< counts the changes to its look
    bool closed_{};                  ///< closed has been called
};

/// The scale a front-end screen is drawn at: its placement's, which is the
/// view's for every screen but the setup, or the view's when it has none.
///
/// @param placed where the screen shows
/// @param seen the view
/// @return the scale, in percent
int32_t front_scale_of(const LayerPlacement& placed, const LayerView& seen) noexcept {
    return placed.scale_percent > 0 ? placed.scale_percent : seen.scale_percent;
}

} // namespace

uint32_t OaLayer::take_request_id() noexcept {
    const uint32_t id = next_request_ == 0 ? 1U : next_request_;
    next_request_ = id == std::numeric_limits<uint32_t>::max() ? 1U : id + 1U;
    return id;
}

GalleryScreen& OaLayer::open_gallery() {
    auto screen = std::make_unique<GalleryScreen>(*this, view().match);
    GalleryScreen& opened = *screen;
    push(std::move(screen));
    return opened;
}

bool OaLayer::open_registered_screen(std::string_view name) {
    const ScreenRegistry& registry = runtime_.screens_;
    const KitScreenDesc* found = nullptr;
    for (uint32_t index = 0; index < registry.kit_screen_count; ++index) {
        const char* registered = registry.kit_screens[index].name;
        if (registered != nullptr && name == registered) {
            found = &registry.kit_screens[index];
            break;
        }
    }
    if (found == nullptr || found->lay_out == nullptr)
        return false;
    push(std::make_unique<RegisteredKitScreen>(std::string(name), *found));
    return true;
}

uint32_t OaLayer::queue_notice(const NoticeRequest& request) {
    if (request.title == nullptr || !paragraphs_ok(request.paragraphs, request.paragraph_count))
        return 0;
    kit::Notice notice;
    notice.title = request.title;
    notice.word = "registered";
    notice.paragraphs.reserve(request.paragraph_count);
    for (std::size_t index = 0; index < request.paragraph_count; ++index)
        notice.paragraphs.push_back(kit::Paragraph{request.paragraphs[index], false});
    const uint32_t id = take_request_id();
    auto screen = std::make_unique<NoticeScreen>(
        *this, Screen::main_menu, std::move(notice), NoticeScreen::Host{}
    );
    screen->show_on_any_menu();
    show_when_free(std::move(screen));
    return id;
}

uint32_t OaLayer::queue_prompt(const PromptRequest& request) {
    if (request.title == nullptr || !paragraphs_ok(request.paragraphs, request.paragraph_count))
        return 0;
    if (request.button_count < 1 || request.button_count > most_prompt_buttons ||
        request.buttons == nullptr)
        return 0;
    for (std::size_t index = 0; index < request.button_count; ++index)
        if (request.buttons[index] == nullptr)
            return 0;
    if (request.button_ids != nullptr) {
        for (std::size_t index = 0; index < request.button_count; ++index) {
            const char* word = request.button_ids[index];
            if (word == nullptr || !kit::row_word(word))
                return 0;
        }
    }
    const auto buttons = static_cast<int32_t>(request.button_count);
    if (request.cancel_button < -1 || request.cancel_button >= buttons ||
        request.primary_button < -1 || request.primary_button >= buttons)
        return 0;

    const uint32_t id = take_request_id();
    auto ticket = std::make_shared<RegisteredPrompt>();
    ticket->id = id;
    ticket->answered = request.answered;
    ticket->user = request.user;
    prompts_.push_back(ticket);

    kit::Question question;
    question.title = request.title;
    question.paragraphs.reserve(request.paragraph_count);
    for (std::size_t index = 0; index < request.paragraph_count; ++index)
        question.paragraphs.push_back(kit::Paragraph{request.paragraphs[index], false});
    question.buttons.reserve(request.button_count);
    for (std::size_t index = 0; index < request.button_count; ++index) {
        kit::QuestionButton button;
        button.caption = request.buttons[index];
        if (request.button_ids != nullptr)
            button.id = request.button_ids[index];
        if (static_cast<int32_t>(index) == request.primary_button)
            button.accent = true;
        question.buttons.push_back(std::move(button));
    }
    question.cancel_button = request.cancel_button;
    question.primary_button = request.primary_button;

    QuestionScreen::Host host;
    host.answer = [ticket](int32_t button) {
        if (ticket->done)
            return true;
        ticket->done = true;
        if (ticket->answered != nullptr)
            ticket->answered(ticket->user, ticket->id, button);
        return true;
    };
    host.closed = [ticket](const QuestionScreen&) { ticket->screen = nullptr; };
    auto screen = std::make_unique<QuestionScreen>(
        *this, Screen::main_menu, std::move(question), std::move(host)
    );
    ticket->screen = screen.get();
    screen->show_on_any_menu();
    show_when_free(std::move(screen));
    return id;
}

void OaLayer::drop_pending_prompts() {
    const std::vector<std::shared_ptr<RegisteredPrompt>> pending = prompts_;
    for (const auto& ticket : pending) {
        if (!ticket || ticket->done)
            continue;
        ticket->done = true;
        if (ticket->answered != nullptr)
            ticket->answered(ticket->user, ticket->id, -1);
    }
    for (const auto& ticket : pending) {
        if (!ticket)
            continue;
        QuestionScreen* screen = ticket->screen;
        ticket->screen = nullptr;
        if (screen != nullptr && (holds(screen) || queued(screen)))
            close(screen);
    }
    std::vector<std::shared_ptr<RegisteredPrompt>> kept;
    for (auto& ticket : prompts_) {
        bool dropped = false;
        for (const auto& old : pending) {
            if (old == ticket) {
                dropped = true;
                break;
            }
        }
        if (!dropped)
            kept.push_back(std::move(ticket));
    }
    prompts_ = std::move(kept);
}

void OaLayer::tick() {
    // One read of the host's mutators serves placement, this tick, the draw
    // and the room chat. The draw of this frame still sees the rows.
    runtime_.drop_host_mutator_fetch_cache();
    // The window's size, and with it the screens' class, may have changed.
    lay_out_screens();
    runtime_.keep_presence_card_screen();
    runtime_.keep_more_maps_screen();
    runtime_.keep_mutator_button_screen();
    runtime_.keep_brain_seats_screen();
    runtime_.keep_brain_room_screen();
    runtime_.offer_differing_results_question();
    runtime_.follow_mutator_room_chat();
    runtime_.keep_get_map_screen();
    // A tick may close a screen, or open another. The screens waiting are
    // ticked too, so that one whose screen of the game goes leaves the queue.
    std::vector<LayerScreen*> ticking;
    ticking.reserve(screens_.size() + queue_.size());
    for (const auto& screen : screens_)
        ticking.push_back(screen.get());
    for (const auto& screen : queue_)
        ticking.push_back(screen.get());
    for (LayerScreen* screen : ticking) {
        const bool on_stack = holds(screen);
        if (!on_stack && !queued(screen))
            continue;
        calling_ = screen;
        close_asked_ = false;
        const LayerAnswer answer = screen->tick();
        calling_ = nullptr;
        if (answer != LayerAnswer::close && !std::exchange(close_asked_, false))
            continue;
        if (holds(screen))
            remove(screen, false);
        else
            drop_waiting(screen);
    }
    show_waiting();
    sync_text_input();
}

bool OaLayer::take_match_button(const ScreenInput& input) {
    if (input.kind == ScreenInputKind::key_down) {
        const auto* settings_action = runtime_.fired_action("ui.settings");
        if (settings_action != nullptr &&
            settings_action->phase == oa::ui::key_bindings::Phase::pressed) {
            runtime_.open_engine_settings_in_match();
            return true;
        }
    }
    // The options action opens the mod options while that hack is on. The
    // press was already given to the dispatcher, so a moved chord still opens
    // it and Ctrl+F2 does not when the player has moved the key.
    if (input.kind == ScreenInputKind::key_down && runtime_.ui_rules().options_dialog.enabled) {
        for (const oa::ui::key_bindings::ActionEvent& fed : runtime_.match_action_events()) {
            if (fed.action == "game.options-dialog" &&
                fed.phase == oa::ui::key_bindings::Phase::pressed) {
                runtime_.open_engine_settings_in_match(settings::DialogKind::mod_options);
                return true;
            }
        }
    }
    const bool pointer = input.kind == ScreenInputKind::pointer_move ||
                         input.kind == ScreenInputKind::pointer_down ||
                         input.kind == ScreenInputKind::pointer_up;
    if (!pointer)
        return false;
    if (!match_button_shown(runtime_)) {
        if (button_hovered_ || button_pressed_ || arrange_hovered_ || arrange_pressed_)
            ++revision_;
        button_hovered_ = false;
        button_pressed_ = false;
        arrange_hovered_ = false;
        arrange_pressed_ = false;
        return false;
    }
    const bool fit = match_fits_safe_area(runtime_);
    // The OA button and Arrange overlays' button beside it: each is hovered
    // while the pointer is over it, held by a left press on it, and acts on
    // the release over it.
    const auto take_button = [&](const layout::Rect& place, bool& hovered, bool& pressed) {
        const bool over = rect_contains(place, input.x, input.y);
        if (over != hovered)
            ++revision_;
        hovered = over;
        if (input.kind == ScreenInputKind::pointer_down && input.button == SDL_BUTTON_LEFT &&
            over) {
            pressed = true;
            ++revision_;
            return std::optional<bool>{false};
        }
        if (input.kind == ScreenInputKind::pointer_up && input.button == SDL_BUTTON_LEFT &&
            pressed) {
            pressed = false;
            ++revision_;
            return std::optional<bool>{over};
        }
        return std::optional<bool>{};
    };
    const auto settings_taken = take_button(
        match_button_rect(runtime_.match_layout_, fit), button_hovered_, button_pressed_
    );
    const auto arrange_taken = take_button(
        arrange_button_rect(runtime_.match_layout_, fit), arrange_hovered_, arrange_pressed_
    );
    if (settings_taken) {
        if (*settings_taken)
            runtime_.open_engine_settings_in_match();
        return true;
    }
    if (arrange_taken) {
        if (*arrange_taken)
            open_arrange_overlays();
        return true;
    }
    return false;
}

void OaLayer::restart_text_input() {
    if (!text_field_)
        return;
    runtime_.stop_text_input();
    runtime_.start_text_input(*text_field_);
}

void OaLayer::sync_text_input() {
    const LayerView seen = view();
    // The field of a screen, in the coordinates input arrives in: through
    // its placement into the window, and on the front end through the
    // picture's rectangle into the picture.
    const auto field_of = [this, &seen](const LayerScreen& screen) -> std::optional<layout::Rect> {
        const auto field = screen.text_field();
        if (!field)
            return std::nullopt;
        const LayerPlacement placed = shown_placement(screen, seen);
        if (empty_rect(placed.shown) || placed.points_width <= 0 || placed.points_height <= 0 ||
            empty_rect(seen.picture))
            return std::nullopt;
        const auto start =
            [](int32_t shown_start, int32_t shown_length, int32_t points, int32_t at) {
                return shown_start +
                       static_cast<int32_t>(static_cast<int64_t>(at) * shown_length / points);
            };
        const auto in_x = [&seen](int32_t window) {
            return static_cast<int32_t>(
                static_cast<int64_t>(window - seen.picture.x) * seen.picture_size.x /
                seen.picture.width
            );
        };
        const auto in_y = [&seen](int32_t window) {
            return static_cast<int32_t>(
                static_cast<int64_t>(window - seen.picture.y) * seen.picture_size.y /
                seen.picture.height
            );
        };
        const int32_t left =
            in_x(start(placed.shown.x, placed.shown.width, placed.points_width, field->x));
        const int32_t first_row =
            in_y(start(placed.shown.y, placed.shown.height, placed.points_height, field->y));
        const int32_t right = in_x(
            start(placed.shown.x, placed.shown.width, placed.points_width, field->x + field->width)
        );
        const int32_t bottom = in_y(start(
            placed.shown.y, placed.shown.height, placed.points_height, field->y + field->height
        ));
        return layout::Rect{left, first_row, right - left, bottom - first_row};
    };
    std::optional<layout::Rect> wanted;
    if (const LayerScreen* modal = top_modal(seen))
        wanted = field_of(*modal);
    else
        for (auto it = screens_.rbegin(); it != screens_.rend() && !wanted; ++it)
            if (!empty_rect((*it)->placement(seen).shown))
                wanted = field_of(**it);
    if (wanted) {
        if (!text_field_ || !same_rect(*text_field_, *wanted)) {
            runtime_.start_text_input(*wanted);
            text_field_ = wanted;
        }
        return;
    }
    // Only the text input the layer started stops with its field.
    if (text_field_) {
        runtime_.stop_text_input();
        text_field_.reset();
    }
}

LayerDrawing OaLayer::draw_screen(
    const LayerScreen& screen, const LayerPlacement& placed, int32_t scale_percent
) const {
    // At the scale itself: never drawn at another and enlarged.
    const bool whole = scale_percent % 100 == 0;
    text_font::FontStack* modern = whole ? nullptr : smooth_fonts();
    const auto draw = [&screen](const kit::Canvas& canvas) { screen.draw(canvas); };
    if (!screen.opaque())
        return clear_drawing(
            placed.points_width,
            placed.points_height,
            scale_percent,
            runtime_.engine_settings_fonts(),
            modern,
            runtime_.engine_settings_icon(),
            draw
        );
    LayerDrawing drawing;
    drawing.picture = layer_drawing(
        placed.points_width,
        placed.points_height,
        scale_percent,
        runtime_.engine_settings_fonts(),
        modern,
        runtime_.engine_settings_icon(),
        draw
    );
    // A window's and a dialog's rounded corners leave what lies behind them
    // showing.
    if (!drawing.picture.rgb.empty())
        round_layer_corners(drawing, placed.corner_radius, scale_percent);
    return drawing;
}

uint32_t OaLayer::front_darkenings() const {
    const LayerView seen = view();
    if (seen.match)
        return 0;
    return static_cast<uint32_t>(
        std::count_if(screens_.begin(), screens_.end(), [&seen](const auto& screen) {
            return darkens(*screen) && !empty_rect(screen->placement(seen).shown);
        })
    );
}

void OaLayer::forget_kept_drawings() noexcept {
    front_drawings_.clear();
}

uint64_t OaLayer::kept_drawing(const LayerScreen& screen) const noexcept {
    for (const KeptDrawing& drawing : front_drawings_)
        if (drawing.screen == screen.identity())
            return drawing.drawing;
    return 0;
}

void OaLayer::darken_front_end(renderer::Surface& frame) const {
    // A blend of the frame itself, in 256ths, for each: the same picture the
    // window shows at every size. A match is left as it is.
    darken(frame, front_darkenings(), kit::menu_backdrop_opacity);
}

void OaLayer::compose_front_end(renderer::Surface& frame) const {
    const LayerView seen = view();
    if (seen.match || empty_rect(seen.picture) || seen.picture_size.x <= 0 ||
        seen.picture_size.y <= 0)
        return;
    const std::vector<uint32_t> above = backdrops_above(seen);
    for (std::size_t index = 0; index < screens_.size(); ++index) {
        const auto& screen = *screens_[index];
        const LayerPlacement placed = shown_placement(screen, seen);
        if (empty_rect(placed.shown))
            continue;
        // A screen under a modal screen with a backdrop darkens with what
        // lies under it.
        // At the screen's own scale, which every screen but the setup takes
        // from the view.
        LayerDrawing drawn = draw_screen(screen, placed, front_scale_of(placed, seen));
        darken(drawn.picture, above[index], kit::menu_backdrop_opacity);
        stamp_onto_picture(frame, drawn, placed.shown, seen);
    }
}

void OaLayer::present(const SDL_FRect* picture_area) {
    if (runtime_.sdl_.renderer == nullptr)
        return;
    // A match presented as one frame shows no layer, as its composed frame
    // is the picture of its layers.
    if (runtime_.screen_ == Screen::match) {
        if (picture_area == nullptr)
            present_match();
        return;
    }
    if (picture_area != nullptr)
        present_front_end(*picture_area);
}

bool OaLayer::draws_presence_badges() noexcept {
    return runtime_.sdl_.renderer != nullptr && runtime_.screen_ != Screen::match &&
           artless::picture_drawable(runtime_.engine_settings_icon());
}

void OaLayer::present_presence_badges(const LayerView& seen, int output_width, int output_height) {
    if (!draws_presence_badges() || output_width <= 0 || output_height <= 0 ||
        seen.picture_size.x <= 0 || seen.picture_size.y <= 0 || seen.picture.width <= 0 ||
        seen.picture.height <= 0)
        return;
    // The same list the frame uses, so a dialog that hides the frame's badge
    // hides this one too.
    std::array<mp::PresenceBadgeSpot, 10> spots{};
    const int32_t count =
        mp::multiplayer_presence_badges(spots.data(), static_cast<int32_t>(spots.size()));
    if (count <= 0)
        return;
    const artless::RgbaPicture icon = runtime_.engine_settings_icon();
    std::vector<PresenceBadgeDrawn> drawn;
    drawn.reserve(static_cast<std::size_t>(count));
    for (int32_t spot_index = 0; spot_index < count; ++spot_index) {
        const mp::PresenceBadgeSpot& spot = spots[static_cast<std::size_t>(spot_index)];
        drawn.push_back({spot.slot, spot.x, spot.y, spot.width, spot.height, spot.rules_differ});
    }
    const bool unchanged =
        badges_uploaded_ && drawn == badges_drawn_ && same_rect(badges_picture_, seen.picture) &&
        badges_picture_size_.x == seen.picture_size.x &&
        badges_picture_size_.y == seen.picture_size.y && badges_icon_width_ == icon.width &&
        badges_icon_height_ == icon.height && badges_gamma_ == runtime_.gamma_table_;
    if (!unchanged) {
        const auto window_x = [&seen](int32_t column) -> int32_t {
            const int64_t mapped =
                static_cast<int64_t>(seen.picture.x) +
                static_cast<int64_t>(column) * seen.picture.width / seen.picture_size.x;
            return static_cast<int32_t>(mapped);
        };
        const auto window_y = [&seen](int32_t row) -> int32_t {
            const int64_t mapped =
                static_cast<int64_t>(seen.picture.y) +
                static_cast<int64_t>(row) * seen.picture.height / seen.picture_size.y;
            return static_cast<int32_t>(mapped);
        };
        const int32_t window_width = static_cast<int32_t>(output_width);
        const int32_t window_height = static_cast<int32_t>(output_height);
        layout::Rect bounds{};
        const auto cover = [&bounds, window_width, window_height](
                               int32_t left, int32_t top, int32_t right, int32_t bottom
                           ) {
            left = std::max(left, int32_t{0});
            top = std::max(top, int32_t{0});
            right = std::min(right, window_width);
            bottom = std::min(bottom, window_height);
            if (right <= left || bottom <= top)
                return;
            bounds = union_rect(bounds, {left, top, right - left, bottom - top});
        };
        for (const PresenceBadgeDrawn& badge : drawn) {
            if (badge.width > 0 && badge.height > 0)
                cover(
                    window_x(badge.x),
                    window_y(badge.y),
                    window_x(badge.x + badge.width),
                    window_y(badge.y + badge.height)
                );
            if (!badge.rules_differ)
                continue;
            const int32_t ring_left = badge.x + badge.width + mp::presence_badge_dot_overhang -
                                      mp::presence_badge_dot_ring;
            const int32_t ring_top = badge.y - mp::presence_badge_dot_overhang;
            cover(
                window_x(ring_left),
                window_y(ring_top),
                window_x(ring_left + mp::presence_badge_dot_ring),
                window_y(ring_top + mp::presence_badge_dot_ring)
            );
        }
        if (empty_rect(bounds)) {
            badges_uploaded_ = false;
            return;
        }
        std::vector<uint8_t> rgba(
            static_cast<std::size_t>(bounds.width) * static_cast<std::size_t>(bounds.height) * 4U, 0
        );
        const auto pixel_at = [&rgba, &bounds](int32_t x, int32_t y) -> uint8_t* {
            if (x < bounds.x || y < bounds.y || x >= bounds.x + bounds.width ||
                y >= bounds.y + bounds.height)
                return nullptr;
            return rgba.data() + (static_cast<std::size_t>(y - bounds.y) *
                                      static_cast<std::size_t>(bounds.width) +
                                  static_cast<std::size_t>(x - bounds.x)) *
                                     4U;
        };
        for (const PresenceBadgeDrawn& badge : drawn) {
            const int32_t left = window_x(badge.x);
            const int32_t top = window_y(badge.y);
            const int32_t right = window_x(badge.x + badge.width);
            const int32_t bottom = window_y(badge.y + badge.height);
            const int32_t badge_width = right - left;
            const int32_t badge_height = bottom - top;
            if (badge_width <= 0 || badge_height <= 0)
                continue;
            paint::Canvas logo = paint::make_canvas(badge_width, badge_height);
            paint::Painter painter(logo);
            painter.draw_rgba(
                icon.pixels,
                static_cast<int>(icon.width),
                static_cast<int>(icon.height),
                paint::Box{0, 0, badge_width, badge_height}
            );
            for (int32_t logo_row = 0; logo_row < badge_height; ++logo_row) {
                for (int32_t logo_column = 0; logo_column < badge_width; ++logo_column) {
                    uint8_t* dest = pixel_at(left + logo_column, top + logo_row);
                    if (dest == nullptr)
                        continue;
                    const uint8_t* from =
                        logo.rgba.data() + (static_cast<std::size_t>(logo_row) *
                                                static_cast<std::size_t>(badge_width) +
                                            static_cast<std::size_t>(logo_column)) *
                                               4U;
                    dest[0] = from[0];
                    dest[1] = from[1];
                    dest[2] = from[2];
                    dest[3] = from[3];
                }
            }
        }
        // Each canvas pixel of the dot fills the window pixels it maps to,
        // nearest, so the ring stays a hard edge at any window size. The
        // four corners of the ring, and of the face inside it, are left out.
        constexpr uint8_t ring_red = 0x0d;
        constexpr uint8_t ring_green = 0x10;
        constexpr uint8_t ring_blue = 0x0e;
        constexpr uint8_t face_red = 0xe2;
        constexpr uint8_t face_green = 0xb2;
        constexpr uint8_t face_blue = 0x3d;
        const auto corner = [](int32_t at, int32_t size) { return at == 0 || at == size - 1; };
        const auto stamp =
            [&](int32_t column, int32_t row, uint8_t red, uint8_t green, uint8_t blue) {
                const int32_t left = window_x(column);
                const int32_t right = window_x(column + 1);
                const int32_t top = window_y(row);
                const int32_t bottom = window_y(row + 1);
                for (int32_t pixel_row = top; pixel_row < bottom; ++pixel_row) {
                    for (int32_t pixel_column = left; pixel_column < right; ++pixel_column) {
                        uint8_t* dest = pixel_at(pixel_column, pixel_row);
                        if (dest == nullptr)
                            continue;
                        dest[0] = red;
                        dest[1] = green;
                        dest[2] = blue;
                        dest[3] = 255U;
                    }
                }
            };
        for (const PresenceBadgeDrawn& badge : drawn) {
            if (!badge.rules_differ)
                continue;
            const int32_t ring_left = badge.x + badge.width + mp::presence_badge_dot_overhang -
                                      mp::presence_badge_dot_ring;
            const int32_t ring_top = badge.y - mp::presence_badge_dot_overhang;
            for (int32_t dot_row = 0; dot_row < mp::presence_badge_dot_ring; ++dot_row) {
                for (int32_t dot_column = 0; dot_column < mp::presence_badge_dot_ring;
                     ++dot_column) {
                    if (corner(dot_column, mp::presence_badge_dot_ring) &&
                        corner(dot_row, mp::presence_badge_dot_ring))
                        continue;
                    stamp(
                        ring_left + dot_column, ring_top + dot_row, ring_red, ring_green, ring_blue
                    );
                }
            }
            constexpr int32_t face = mp::presence_badge_dot_ring - 2;
            for (int32_t dot_row = 0; dot_row < face; ++dot_row) {
                for (int32_t dot_column = 0; dot_column < face; ++dot_column) {
                    if (corner(dot_column, face) && corner(dot_row, face))
                        continue;
                    stamp(
                        ring_left + 1 + dot_column,
                        ring_top + 1 + dot_row,
                        face_red,
                        face_green,
                        face_blue
                    );
                }
            }
        }
        if (!runtime_.gamma_identity_)
            runtime_.apply_gamma_rgb(rgba.data(), rgba.size() / 4U, 4);
        const auto limit = runtime_.render_texture_limit();
        badge_texture_.ensure(
            runtime_.sdl_.renderer,
            SDL_PIXELFORMAT_RGBA32,
            bounds.width,
            bounds.height,
            limit,
            SDL_BLENDMODE_BLEND
        );
        badge_texture_.update(rgba.data(), bounds.width * 4, 4);
        badges_drawn_ = std::move(drawn);
        badges_picture_ = seen.picture;
        badges_picture_size_ = seen.picture_size;
        badges_icon_width_ = icon.width;
        badges_icon_height_ = icon.height;
        badges_gamma_ = runtime_.gamma_table_;
        badge_bounds_ = bounds;
        badges_uploaded_ = true;
    }
    const SDL_FRect at{
        static_cast<float>(badge_bounds_.x),
        static_cast<float>(badge_bounds_.y),
        static_cast<float>(badge_bounds_.width),
        static_cast<float>(badge_bounds_.height)
    };
    badge_texture_.draw(runtime_.sdl_.renderer, nullptr, &at);
}

void OaLayer::present_front_end(const SDL_FRect& area) {
    SDL_Renderer* sdl_renderer = runtime_.sdl_.renderer;
    lay_out_screens();
    const LayerView seen = view();
    if (seen.match)
        front_drawings_.clear();
    if (seen.match || !(area.w > 0.0F) || !(area.h > 0.0F))
        return;
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderOutputSize(sdl_renderer, &output_width, &output_height) ||
        output_width <= 0 || output_height <= 0)
        return;
    const layout::Rect output{0, 0, output_width, output_height};
    FrontLook look{};
    look.picture_width = seen.picture_size.x;
    look.picture_height = seen.picture_size.y;
    look.area = {area.x, area.y, area.w, area.h};
    layout::Rect bounds{};
    const std::vector<uint32_t> above = backdrops_above(seen);
    uint32_t darkenings = 0;
    // What every screen is drawn in, besides its place and scale.
    DrawingInputs common{};
    common.fonts = runtime_.engine_settings_fonts();
    const artless::RgbaPicture icon = runtime_.engine_settings_icon();
    common.icon = icon.pixels.data();
    common.icon_bytes = icon.pixels.size();
    common.icon_width = icon.width;
    common.icon_height = icon.height;
    common.language = &oa::data::languages::interface_language();
    common.languages = oa::data::languages::registry_generation();
    common.interface_words = oa::data::languages::interface_generation();
    const oa::present::TextSettings text = oa::present::game_text_settings();
    common.modern_text = text.style.modern_fonts;
    common.outline = text.style.outline;
    common.shadow = text.style.shadow;
    common.background = text.style.background;
    common.text_size = text.style.size;
    common.utf8 = text.utf8;
    std::vector<KeptDrawing> kept_drawings;
    for (std::size_t index = 0; index < screens_.size(); ++index) {
        const auto& screen = *screens_[index];
        const LayerPlacement placed = shown_placement(screen, seen);
        if (empty_rect(placed.shown) || placed.points_width <= 0 || placed.points_height <= 0)
            continue;
        if (darkens(screen))
            ++darkenings;
        // The part of its place the window shows.
        const layout::Rect window = intersection(placed.shown, output);
        if (empty_rect(window))
            continue;
        // Drawn at its own scale, as large as its place: the view's, or the
        // setup's.
        const int32_t scale_percent = front_scale_of(placed, seen);
        DrawingInputs inputs = common;
        // Where it shows draws nothing different: a window moved keeps its drawing.
        inputs.shown = {0, 0, placed.shown.width, placed.shown.height};
        inputs.points_width = placed.points_width;
        inputs.points_height = placed.points_height;
        inputs.placed_percent = placed.scale_percent;
        inputs.scale_percent = scale_percent;
        inputs.darkenings = above[index];
        inputs.corner_radius = placed.corner_radius;
        inputs.opaque = screen.opaque();
        inputs.modern_fonts = scale_percent % 100 == 0 ? nullptr : smooth_fonts();
        // A screen whose revision covers what it draws keeps its drawing
        // while the revision and everything else it is drawn from hold;
        // every other screen is drawn again.
        const bool keeps = screen.revision_covers_drawing();
        const uint64_t revision = keeps ? screen.revision() : 0;
        const auto held =
            std::find_if(front_drawings_.begin(), front_drawings_.end(), [&](const auto& drawing) {
                return keeps && drawing.screen == screen.identity() &&
                       drawing.revision == revision && drawing.inputs == inputs;
            });
        KeptDrawing drawing{};
        if (held != front_drawings_.end()) {
            drawing = *held;
        } else {
            LayerDrawing drawn = draw_screen(screen, placed, scale_percent);
            // Of a screen that is not opaque, only the part that holds what
            // it drew: the rest is clear.
            drawing.drawn_part = drawn_bounds(drawn);
            darken(drawn.picture, above[index], kit::menu_backdrop_opacity);
            drawing.screen = screen.identity();
            drawing.revision = revision;
            drawing.inputs = inputs;
            drawing.drawn = std::make_shared<const LayerDrawing>(std::move(drawn));
            drawing.drawing = ++front_drawing_count_;
        }
        if (keeps)
            kept_drawings.push_back(drawing);
        FrontPiece piece{};
        piece.shown = placed.shown;
        piece.window = intersection(
            window,
            {placed.shown.x + drawing.drawn_part.x,
             placed.shown.y + drawing.drawn_part.y,
             drawing.drawn_part.width,
             drawing.drawn_part.height}
        );
        if (empty_rect(piece.window))
            continue;
        piece.drawn = std::move(drawing.drawn);
        piece.drawing = drawing.drawing;
        bounds = union_rect(bounds, piece.window);
        look.pieces.push_back(std::move(piece));
    }
    front_drawings_ = std::move(kept_drawings);
    // In the window's own pixels, not through the picture's logical
    // presentation; everything is put back for the cursor and the next frame.
    const RenderState kept(sdl_renderer);
    kept.use_window_pixels();
    // Under a modal screen with a backdrop the whole window darkens: the
    // picture is darkened in the frame (darken_front_end), and the window
    // outside it, where SDL clears to black, takes black darkened as often.
    if (darkenings > 0) {
        renderer::Surface black;
        black.width = 1;
        black.height = 1;
        black.rgb.assign(3U, 0);
        darken(black, darkenings, kit::menu_backdrop_opacity);
        if (!runtime_.gamma_identity_)
            runtime_.apply_gamma_rgb(black.rgb.data(), 1U, 3);
        std::vector<SDL_FRect> bars;
        const auto bar = [&bars](int32_t x, int32_t y, int32_t width, int32_t height) {
            if (width > 0 && height > 0)
                bars.push_back(
                    {static_cast<float>(x),
                     static_cast<float>(y),
                     static_cast<float>(width),
                     static_cast<float>(height)}
                );
        };
        const layout::Rect& picture = seen.picture;
        bar(0, 0, output_width, picture.y);
        bar(
            0, picture.y + picture.height, output_width, output_height - picture.y - picture.height
        );
        bar(0, picture.y, picture.x, picture.height);
        bar(picture.x + picture.width,
            picture.y,
            output_width - picture.x - picture.width,
            picture.height);
        if (!bars.empty()) {
            SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(
                sdl_renderer, black.rgb[0], black.rgb[1], black.rgb[2], SDL_ALPHA_OPAQUE
            );
            SDL_RenderFillRects(sdl_renderer, bars.data(), static_cast<int>(bars.size()));
        }
    }
    // Under the screens, so an open card covers the badge it describes.
    present_presence_badges(seen, output_width, output_height);
    if (look.pieces.empty())
        return;
    const auto limit = runtime_.render_texture_limit();
    const bool made = texture_.ensure(
        sdl_renderer,
        SDL_PIXELFORMAT_RGBA32,
        bounds.width,
        bounds.height,
        limit,
        SDL_BLENDMODE_BLEND
    );
    const auto same_look = [&look](const FrontLook& held) {
        if (held.picture_width != look.picture_width ||
            held.picture_height != look.picture_height || held.area != look.area ||
            held.pieces.size() != look.pieces.size())
            return false;
        for (std::size_t index = 0; index < held.pieces.size(); ++index) {
            const auto& was = held.pieces[index];
            const auto& now = look.pieces[index];
            if (!same_rect(was.window, now.window) || !same_rect(was.shown, now.shown))
                return false;
            // A drawing kept from before is the one uploaded; one drawn
            // again is compared pixel by pixel.
            if (was.drawing != now.drawing && !same_drawing(*was.drawn, *now.drawn))
                return false;
        }
        return true;
    };
    if (made || !front_uploaded_ || !same_look(*front_uploaded_) ||
        !same_rect(front_bounds_, bounds) || uploaded_gamma_ != runtime_.gamma_table_) {
        std::vector<uint8_t> rgba(
            static_cast<std::size_t>(bounds.width) * static_cast<std::size_t>(bounds.height) * 4U, 0
        );
        // Each piece's pixels as drawn, one window pixel each, bottom first:
        // an opaque screen's copied, fully opaque; one that is not opaque
        // laid over what is already there (src over, straight colours).
        for (const auto& piece : look.pieces) {
            const renderer::Surface& drawn = piece.drawn->picture;
            const bool laid = !piece.drawn->opacity.empty();
            const int32_t first_column = piece.window.x - piece.shown.x;
            for (int32_t row = 0; row < piece.window.height; ++row) {
                const auto source_row =
                    static_cast<std::size_t>(piece.window.y + row - piece.shown.y);
                const std::size_t first =
                    source_row * drawn.width + static_cast<std::size_t>(first_column);
                const auto* from = drawn.rgb.data() + first * 3U;
                auto* to =
                    rgba.data() + (static_cast<std::size_t>(piece.window.y + row - bounds.y) *
                                       static_cast<std::size_t>(bounds.width) +
                                   static_cast<std::size_t>(piece.window.x - bounds.x)) *
                                      4U;
                if (laid) {
                    const uint8_t* opacity = piece.drawn->opacity.data() + first;
                    for (int32_t column = 0; column < piece.window.width; ++column) {
                        lay_over_layer(to, from, opacity[column]);
                        from += 3;
                        to += 4;
                    }
                    continue;
                }
                for (int32_t column = 0; column < piece.window.width; ++column) {
                    to[0] = from[0];
                    to[1] = from[1];
                    to[2] = from[2];
                    to[3] = 255U;
                    from += 3;
                    to += 4;
                }
            }
        }
        // At the display gamma, as the picture under it is uploaded.
        if (!runtime_.gamma_identity_)
            runtime_.apply_gamma_rgb(rgba.data(), rgba.size() / 4U, 4);
        texture_.update(rgba.data(), bounds.width * 4, 4);
        front_uploaded_ = std::move(look);
        front_bounds_ = bounds;
        uploaded_.reset();
        uploaded_gamma_ = runtime_.gamma_table_;
    }
    const SDL_FRect at{
        static_cast<float>(bounds.x),
        static_cast<float>(bounds.y),
        static_cast<float>(bounds.width),
        static_cast<float>(bounds.height)
    };
    texture_.draw(sdl_renderer, nullptr, &at);
}

bool OaLayer::match_button_shown(Runtime& runtime) {
    return runtime.ingame_menu_column_shown() && runtime.engine_settings_fonts() != nullptr;
}

bool OaLayer::match_fits_safe_area(const Runtime& runtime) {
    return runtime.touch_controls_active() && runtime.touch_phone_class();
}

layout::Rect OaLayer::match_safe_area(const layout::MatchLayout& match_layout) noexcept {
    return window_placement::match_safe_area(match_layout);
}

namespace {

/// Returns where a rectangle of the in-game menu's column shows on the
/// match's canvas, as OaLayer::match_button_rect places the OA button.
///
/// @param match_layout the match's layout
/// @param source the rectangle, in source pixels
/// @param fit it fits the safe area (OaLayer::match_fits_safe_area)
/// @return the rectangle, in window pixels
layout::Rect column_rect(
    const layout::MatchLayout& match_layout, const layout::Rect& source, bool fit
) noexcept {
    const auto safe = OaLayer::match_safe_area(match_layout);
    if (fit && safe.width > 0 && safe.height > 0) {
        // Where a placed region shows that part of the side column, the
        // button stands on it.
        const std::size_t count =
            std::min<std::size_t>(match_layout.placed_count, layout::kMaxPlacedRegions);
        for (std::size_t index = count; index > 0; --index)
            if (source_holds(match_layout.placed[index - 1].source, source))
                return layout::source_rect_to_canvas(
                    match_layout, source.x, source.y, source.width, source.height
                );
        // Else the column as the 640x480 frame fitted to the safe area
        // places it, from the safe area's top left corner.
        const double scale = std::min(
            static_cast<double>(safe.width) / layout::kSourceWidth,
            static_cast<double>(safe.height) / layout::kSourceHeight
        );
        return {
            safe.x + static_cast<int>(std::lround(static_cast<double>(source.x) * scale)),
            safe.y + static_cast<int>(std::lround(static_cast<double>(source.y) * scale)),
            scaled_length(source.width, scale),
            scaled_length(source.height, scale)
        };
    }
    // The side column hangs from the window's top left corner at its own
    // scale, and the in-game menu in it with the button.
    const double scale = column_scale(match_layout);
    return {
        static_cast<int>(std::lround(static_cast<double>(source.x) * scale)),
        static_cast<int>(std::lround(static_cast<double>(source.y) * scale)),
        scaled_length(source.width, scale),
        scaled_length(source.height, scale)
    };
}

} // namespace

layout::Rect
OaLayer::match_button_rect(const layout::MatchLayout& match_layout, bool fit) noexcept {
    return column_rect(
        match_layout,
        {match_button_source_x,
         match_button_source_y,
         settings::ingame_button_side,
         settings::ingame_button_side},
        fit
    );
}

layout::Rect
OaLayer::arrange_button_rect(const layout::MatchLayout& match_layout, bool fit) noexcept {
    return column_rect(
        match_layout,
        {arrange_button_source_x,
         arrange_button_source_y,
         arrange_button_source_width,
         arrange_button_source_height},
        fit
    );
}

renderer::Surface OaLayer::match_button_face(
    const layout::MatchLayout& match_layout,
    settings::ButtonLook look,
    bool darkened,
    const settings::DialogFonts& fonts,
    const artless::RgbaPicture& icon,
    bool fit
) {
    // Fitted, the face takes the button's own scale; else the side column's.
    const double shown_scale =
        fit ? static_cast<double>(match_button_rect(match_layout, true).width) /
                  settings::ingame_button_side
            : column_scale(match_layout);
    const int32_t scale = std::max(1, static_cast<int32_t>(std::ceil(shown_scale)));
    const artless::Placement placement{0, 0, scale};
    renderer::Surface face;
    face.width = static_cast<uint32_t>(settings::ingame_button_side * scale);
    face.height = face.width;
    face.rgb.assign(static_cast<std::size_t>(face.width) * face.height * 3U, 0);
    settings::draw_oa_button(face, placement, settings::ingame_button_side, look, fonts, icon);
    if (darkened)
        artless::blend_source_rect(
            face,
            placement,
            {0, 0, settings::ingame_button_side, settings::ingame_button_side},
            settings::backdrop_color,
            settings::ingame_backdrop_opacity
        );
    return face;
}

LayerDrawing OaLayer::arrange_button_face(
    const layout::MatchLayout& match_layout,
    settings::ButtonLook look,
    bool darkened,
    const kit::Fonts& fonts,
    bool fit
) {
    // Fitted, the face takes the button's own scale; else the side column's.
    const double shown_scale =
        fit ? static_cast<double>(arrange_button_rect(match_layout, true).height) /
                  arrange_button_source_height
            : column_scale(match_layout);
    const int32_t scale = std::max(1, static_cast<int32_t>(std::ceil(shown_scale)));
    const kit::Rect whole{0, 0, arrange_button_source_width, arrange_button_source_height};
    kit::ButtonLook button;
    button.style = kit::ButtonStyle::plain;
    button.hovered = look != settings::ButtonLook::idle;
    button.held = look == settings::ButtonLook::pressed;
    const int32_t room = whole.width - 2 * kit::compact_metrics.chip_padding;
    button.caption = kit::cut_to_width(
        fonts, kit::FontRole::small, oa::data::languages::interface_text(kArrangeWords), room
    );
    LayerDrawing face = clear_drawing(
        whole.width,
        whole.height,
        scale * 100,
        &fonts,
        nullptr,
        {},
        [&button, &whole](const kit::Canvas& canvas) { kit::draw_button(canvas, whole, button); }
    );
    if (darkened)
        artless::blend_source_rect(
            face.picture,
            {0, 0, scale},
            whole,
            settings::backdrop_color,
            settings::ingame_backdrop_opacity
        );
    return face;
}

kit::Frame OaLayer::match_gallery(
    const layout::MatchLayout& match_layout, int32_t interface_percent
) noexcept {
    return kit::frame_of(window_placement::match_viewport(match_layout, interface_percent));
}

MatchDialogPlace OaLayer::match_dialog(
    const layout::MatchLayout& match_layout,
    int32_t interface_percent,
    bool fit,
    window_placement::WindowSize size
) noexcept {
    return window_placement::match_window(match_layout, interface_percent, size, fit);
}

void OaLayer::stamp(
    std::vector<uint8_t>& rgba,
    int32_t width,
    int32_t height,
    const renderer::Surface& source,
    const layout::Rect& rect,
    std::span<const uint8_t> opacity
) {
    if (source.width == 0 || source.height == 0 || rect.width <= 0 || rect.height <= 0)
        return;
    const std::size_t source_pixels = static_cast<std::size_t>(source.width) * source.height;
    const bool laid = !opacity.empty();
    if (laid && opacity.size() < source_pixels)
        return;
    const int32_t first_row = std::max(rect.y, 0);
    const int32_t bottom = std::min(rect.y + rect.height, height);
    const int32_t left = std::max(rect.x, 0);
    const int32_t right = std::min(rect.x + rect.width, width);
    for (int32_t row = first_row; row < bottom; ++row) {
        const auto source_row = static_cast<std::size_t>(
            static_cast<int64_t>(row - rect.y) * source.height / rect.height
        );
        for (int32_t column = left; column < right; ++column) {
            const auto source_column = static_cast<std::size_t>(
                static_cast<int64_t>(column - rect.x) * source.width / rect.width
            );
            const std::size_t at = source_row * source.width + source_column;
            const auto* from = source.rgb.data() + at * 3U;
            auto* to =
                rgba.data() + (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(column)) *
                                  4U;
            if (laid) {
                lay_over_layer(to, from, opacity[at]);
                continue;
            }
            to[0] = from[0];
            to[1] = from[1];
            to[2] = from[2];
            to[3] = 255U;
        }
    }
}

bool OaLayer::refresh_match() {
    const auto* fonts = runtime_.engine_settings_fonts();
    const auto& laid_out = runtime_.match_layout_;
    const bool fit = match_fits_safe_area(runtime_);
    const auto button_at = match_button_rect(laid_out, fit);
    // Settings takes its class from the match's canvas, which a window's
    // new size changes.
    lay_out_screens();
    const LayerView seen = view();
    MatchLook look{};
    look.width = laid_out.width;
    look.height = laid_out.height;
    look.scale = laid_out.scale;
    look.button = {button_at.x, button_at.y, button_at.width, button_at.height};
    // The layer shows while the in-game menu's column shows, the button in it.
    look.button_shown = fonts != nullptr && runtime_.screen_ == Screen::match &&
                        runtime_.ingame_menu_column_shown();
    look.button_look = static_cast<uint8_t>(
        button_pressed_ && button_hovered_ ? settings::ButtonLook::pressed
        : button_hovered_                  ? settings::ButtonLook::hovered
                                           : settings::ButtonLook::idle
    );
    look.arrange_look = static_cast<uint8_t>(
        arrange_pressed_ && arrange_hovered_ ? settings::ButtonLook::pressed
        : arrange_hovered_                   ? settings::ButtonLook::hovered
                                             : settings::ButtonLook::idle
    );
    look.revision = revision_;
    if (fonts == nullptr || look.width <= 0 || look.height <= 0 || !seen.match)
        return false;

    // Each screen that shows, where, and how many modal screens with a
    // backdrop show above it.
    struct ShownScreen {
        const LayerScreen* screen{};
        LayerPlacement placed{};
        uint32_t darkenings{};
    };

    std::vector<ShownScreen> shown_screens;
    const std::vector<uint32_t> above = backdrops_above(seen);
    for (std::size_t index = 0; index < screens_.size(); ++index) {
        const auto& screen = *screens_[index];
        const LayerPlacement placed = shown_placement(screen, seen);
        if (empty_rect(placed.shown))
            continue;
        shown_screens.push_back({&screen, placed, above[index]});
        look.placed.insert(
            look.placed.end(),
            {placed.shown.x,
             placed.shown.y,
             placed.shown.width,
             placed.shown.height,
             placed.corner_radius}
        );
        look.revisions.push_back(screen.revision());
        look.darkened = look.darkened || darkens(screen);
    }
    // Without the in-game menu's column the layer shows only the screens
    // that are not modal and show over the match as it runs, such as the
    // keys overlay; with neither, it shows nothing.
    if (!look.button_shown && shown_screens.empty())
        return false;
    if (drawn_ == look)
        return true;
    const auto pixels =
        static_cast<std::size_t>(look.width) * static_cast<std::size_t>(look.height);
    rgba_.assign(pixels * 4U, 0);
    // The in-game OA button opens Settings, so it shows the settings icon,
    // as the main menu's Settings button does.
    const renderer::Surface button = match_button_face(
        laid_out,
        static_cast<settings::ButtonLook>(look.button_look),
        look.darkened,
        *fonts,
        Runtime::EngineSettingsMenuHost::settings_icon(runtime_),
        fit
    );
    // Arrange overlays' button beside it, under Resume.
    const layout::Rect arrange_at = arrange_button_rect(laid_out, fit);
    const LayerDrawing arrange = arrange_button_face(
        laid_out, static_cast<settings::ButtonLook>(look.arrange_look), look.darkened, *fonts, fit
    );
    bounds_ = look.button_shown ? union_rect(button_at, arrange_at) : layout::Rect{};
    if (look.darkened) {
        // The whole screen darkens, the button with it; the screens go over.
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
            auto* to = rgba_.data() + pixel * 4U;
            to[0] = settings::backdrop_color[0];
            to[1] = settings::backdrop_color[1];
            to[2] = settings::backdrop_color[2];
            to[3] = static_cast<uint8_t>(kBackdropAlpha);
        }
        bounds_ = {0, 0, look.width, look.height};
    }
    // The button's face, drawn at the whole step at or above its place's
    // scale, fitted to its place.
    if (look.button_shown) {
        stamp(rgba_, look.width, look.height, button, button_at);
        stamp(rgba_, look.width, look.height, arrange.picture, arrange_at, arrange.opacity);
    }
    for (const auto& shown : shown_screens) {
        // At its placement's scale, as large as its place, never drawn at 1x
        // and enlarged: Settings at its own (match_dialog). A screen under a
        // modal screen with a backdrop darkens as the button does; one that
        // is not opaque is laid over what is already there by its opacity.
        const int32_t drawn_percent =
            shown.placed.scale_percent > 0 ? shown.placed.scale_percent : 100;
        LayerDrawing drawn = draw_screen(*shown.screen, shown.placed, drawn_percent);
        darken(drawn.picture, shown.darkenings, settings::ingame_backdrop_opacity);
        const auto& place = shown.placed.shown;
        if (static_cast<int32_t>(drawn.picture.width) == place.width &&
            static_cast<int32_t>(drawn.picture.height) == place.height) {
            copy_onto_layer(rgba_, look.width, look.height, drawn, place.x, place.y);
        } else {
            // A screen with no scale of its own for a place of another size,
            // as the phone's fitted Settings: drawn at 1x, fitted to it.
            stamp(rgba_, look.width, look.height, drawn.picture, place, drawn.opacity);
        }
        bounds_ = union_rect(bounds_, place);
    }
    drawn_ = look;
    uploaded_.reset();
    return true;
}

void OaLayer::compose_match(renderer::Surface& frame) {
    if (!refresh_match())
        return;
    const auto width = static_cast<int32_t>(frame.width);
    const auto height = static_cast<int32_t>(frame.height);
    if (!drawn_ || drawn_->width != width || drawn_->height != height)
        return;
    const int32_t first_row = std::max(bounds_.y, 0);
    const int32_t bottom = std::min(bounds_.y + bounds_.height, height);
    const int32_t left = std::max(bounds_.x, 0);
    const int32_t right = std::min(bounds_.x + bounds_.width, width);
    const auto& gamma = runtime_.gamma_table_;
    const bool identity = runtime_.gamma_identity_;
    // As the frontend dialogs' layer goes over the composed frame.
    for (int32_t row = first_row; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column) {
            const auto pixel = static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(column);
            const auto* source = rgba_.data() + pixel * 4U;
            const unsigned alpha = source[3];
            if (alpha == 0)
                continue;
            auto* target = frame.rgb.data() + pixel * 3U;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const unsigned shown = identity ? source[channel] : gamma[source[channel]];
                target[channel] =
                    static_cast<uint8_t>((shown * alpha + target[channel] * (255U - alpha)) / 255U);
            }
        }
}

void OaLayer::present_match() {
    SDL_Renderer* sdl_renderer = runtime_.sdl_.renderer;
    if (sdl_renderer == nullptr || !refresh_match())
        return;
    const int width = drawn_->width;
    const int height = drawn_->height;
    if (texture_.ensure(
            sdl_renderer,
            SDL_PIXELFORMAT_RGBA32,
            width,
            height,
            runtime_.render_texture_limit(),
            SDL_BLENDMODE_BLEND
        ))
        uploaded_.reset();
    // A texture that held the front end's screens holds nothing of the match's.
    if (front_uploaded_) {
        front_uploaded_.reset();
        uploaded_.reset();
    }
    if (uploaded_ != drawn_ || uploaded_gamma_ != runtime_.gamma_table_) {
        const uint8_t* pixels = rgba_.data();
        std::vector<uint8_t> corrected;
        if (!runtime_.gamma_identity_) {
            corrected = rgba_;
            runtime_.apply_gamma_rgb(corrected.data(), corrected.size() / 4U, 4);
            pixels = corrected.data();
        }
        texture_.update(pixels, width * 4, 4);
        uploaded_ = drawn_;
        uploaded_gamma_ = runtime_.gamma_table_;
    }
    const SDL_FRect bounds{
        static_cast<float>(bounds_.x),
        static_cast<float>(bounds_.y),
        static_cast<float>(bounds_.width),
        static_cast<float>(bounds_.height)
    };
    // At the display's pixels with the match's other layers laid out 1:1.
    draw_one_to_one(sdl_renderer, texture_, &bounds, &bounds, runtime_.one_to_one_scale_mode());
}

void OaLayer::destroy_textures() noexcept {
    const bool screens = texture_.tile_count() != 0;
    const bool badges = badge_texture_.tile_count() != 0;
    if (!screens && !badges)
        return;
    if (screens)
        texture_.reset();
    if (badges)
        badge_texture_.reset();
    badges_uploaded_ = false;
    uploaded_.reset();
    front_uploaded_.reset();
}

// ---------------------------------------------------------------------------------------------
// What automation lists of the layer

AutomationControlKind automation_kind(const kit::AutomationEntry& entry) noexcept {
    if (entry.part)
        return entry.kind == kit::ControlKind::buttons ? AutomationControlKind::button
                                                       : AutomationControlKind::check_box;
    switch (entry.kind) {
    case kit::ControlKind::button:
    case kit::ControlKind::link:
    case kit::ControlKind::choice:
        return AutomationControlKind::button;
    case kit::ControlKind::toggle:
    case kit::ControlKind::levels:
    case kit::ControlKind::buttons:
        return AutomationControlKind::label;
    case kit::ControlKind::slider:
    case kit::ControlKind::scroll_bar:
        return AutomationControlKind::slider;
    case kit::ControlKind::tab:
    case kit::ControlKind::list_item:
        return AutomationControlKind::check_box;
    case kit::ControlKind::text_field:
        return AutomationControlKind::text_field;
    case kit::ControlKind::list:
        return AutomationControlKind::list;
    case kit::ControlKind::area:
        // An area that takes nothing is a line a journey reads.
        return entry.enabled || entry.focusable ? AutomationControlKind::area
                                                : AutomationControlKind::label;
    }
    return AutomationControlKind::area;
}

OaLayer::MenuButtonWindows OaLayer::menu_button_windows() const {
    const LayerView seen = view();
    const WindowPlace place = window_place(
        runtime_.sdl_.renderer,
        seen.match,
        static_cast<int32_t>(runtime_.surface_.width),
        static_cast<int32_t>(runtime_.surface_.height),
        runtime_.menu_scaling()
    );
    const auto window_of = [&](const artless::SourceRect& at) {
        return window_rect_of(
            place,
            {static_cast<double>(at.x),
             static_cast<double>(at.y),
             static_cast<double>(at.x) + static_cast<double>(at.width),
             static_cast<double>(at.y) + static_cast<double>(at.height)}
        );
    };
    return {
        window_of(Runtime::EngineSettingsMenuHost::button_rect(runtime_)),
        window_of(Runtime::EngineSettingsMenuHost::settings_button_rect(runtime_))
    };
}

void OaLayer::automation_controls(std::vector<AutomationControl>& controls) const {
    const LayerView seen = view();
    const LayerScreen* modal = top_modal(seen);
    const WindowPlace place = window_place(
        runtime_.sdl_.renderer,
        seen.match,
        static_cast<int32_t>(runtime_.surface_.width),
        static_cast<int32_t>(runtime_.surface_.height),
        runtime_.menu_scaling()
    );
    // A button of the main menu's picture or the in-game menu's column, with
    // no dialog. Under a modal screen it takes no press.
    const auto add_button = [&](std::string name, std::string text, const layout::Rect& at) {
        AutomationControl button;
        button.name = std::move(name);
        button.kind = AutomationControlKind::button;
        button.text = std::move(text);
        button.visible = true;
        button.enabled = modal == nullptr;
        place_control(
            button,
            place,
            window_rect_of(
                place,
                {static_cast<double>(at.x),
                 static_cast<double>(at.y),
                 static_cast<double>(at.x + at.width),
                 static_cast<double>(at.y + at.height)}
            )
        );
        controls.push_back(std::move(button));
    };
    // The OA button, on the main menu's picture or in the in-game menu's
    // column. On the main menu it opens the Library, and says how many
    // packages the player has added (mods, map packs, language packs and
    // downloaded expansions) and how many updates wait; the Settings button
    // beside it opens Settings. In the in-game menu the OA button opens
    // Settings and its text stays empty.
    const std::string button_name = std::string(kAutomationPrefix) + "button";
    if (seen.match) {
        if (match_button_shown(runtime_)) {
            const bool fit = match_fits_safe_area(runtime_);
            add_button(button_name, {}, match_button_rect(runtime_.match_layout_, fit));
            // Arrange overlays' button beside it, with its words.
            add_button(
                std::string(kAutomationPrefix) + std::string(kArrangeButtonName),
                std::string(oa::data::languages::interface_text(kArrangeWords)),
                arrange_button_rect(runtime_.match_layout_, fit)
            );
        }
    } else if (Runtime::EngineSettingsMenuHost::button_shown(runtime_)) {
        const auto square = Runtime::EngineSettingsMenuHost::button_rect(runtime_);
        const kit::OaButtonMark library = Runtime::EngineSettingsMenuHost::library_mark(runtime_);
        add_button(
            button_name,
            library_button_text(
                runtime_.library_update_count(),
                library.corner == kit::OaButtonCorner::count,
                library.count
            ),
            {square.x, square.y, square.width, square.height}
        );
        const auto beside = Runtime::EngineSettingsMenuHost::settings_button_rect(runtime_);
        add_button(
            std::string(kSettingsButtonName),
            std::string(kSettingsButtonText),
            {beside.x, beside.y, beside.width, beside.height}
        );
    }
    // The screens that show, from the top, down to the first that takes
    // every input: nothing under it takes any.
    for (auto it = screens_.rbegin(); it != screens_.rend(); ++it) {
        const LayerScreen& screen = **it;
        const LayerPlacement placed = shown_placement(screen, seen);
        if (empty_rect(placed.shown) || placed.points_width <= 0 || placed.points_height <= 0)
            continue;
        const std::string dialog = screen.name() == kGetMapScreen ? std::string()
                                                                  : std::string(kAutomationPrefix) +
                                                                        std::string(screen.name());
        const kit::DisplayList list = screen.display_list();
        for (const kit::AutomationEntry& entry :
             kit::automation_parts(list, screen.interaction())) {
            AutomationControl control;
            control.name = std::string(kAutomationPrefix) + entry.name;
            control.kind = automation_kind(entry);
            control.dialog = dialog;
            control.visible = entry.shown;
            control.enabled = entry.enabled;
            control.focused = entry.focused;
            control.checked = entry.checked;
            control.text = entry.text;
            // A front-end screen is drawn in the window's own pixels at its
            // place; a match's on its canvas, the in-match Settings at its
            // own scale.
            const Edges edges = screen_edges(
                placed,
                entry.rect,
                placed.scale_percent > 0 ? placed.scale_percent : seen.frame.scale_percent
            );
            place_control(
                control,
                place,
                place.front_end ? window_rect_at(place, edges) : window_rect_of(place, edges)
            );
            controls.push_back(std::move(control));
        }
        if (screen.modal())
            break;
    }
}

std::optional<float> OaLayer::nearest_control(float x, float y, float radius) const {
    if (!(radius >= 0.0F))
        return std::nullopt;
    const LayerView seen = view();
    const LayerScreen* modal = top_modal(seen);
    const WindowPlace place = window_place(
        runtime_.sdl_.renderer,
        seen.match,
        static_cast<int32_t>(runtime_.surface_.width),
        static_cast<int32_t>(runtime_.surface_.height),
        runtime_.menu_scaling()
    );
    float best = radius;
    bool found = false;
    const auto consider = [&](const LayerScreen& screen) {
        const LayerPlacement placed = shown_placement(screen, seen);
        if (empty_rect(placed.shown) || placed.points_width <= 0 || placed.points_height <= 0)
            return;
        const kit::DisplayList list = screen.display_list();
        const int32_t scale =
            placed.scale_percent > 0 ? placed.scale_percent : seen.frame.scale_percent;
        for (const kit::AutomationEntry& entry :
             kit::automation_parts(list, screen.interaction())) {
            if (!entry.enabled || !entry.shown || entry.rect.width <= 0 || entry.rect.height <= 0)
                continue;
            const Edges edges = screen_edges(placed, entry.rect, scale);
            const layout::Rect window =
                place.front_end ? window_rect_at(place, edges) : window_rect_of(place, edges);
            const float distance = distance_to_edges(x, y, canvas_edges_of(place, window));
            if (distance <= best) {
                best = distance;
                found = true;
            }
        }
    };
    // A modal screen answers for itself alone. Otherwise every screen that
    // shows does, and one that does not show adds nothing.
    if (modal != nullptr)
        consider(*modal);
    else
        for (auto it = screens_.rbegin(); it != screens_.rend(); ++it)
            consider(**it);
    return found ? std::optional<float>{best} : std::nullopt;
}

} // namespace oa::app
