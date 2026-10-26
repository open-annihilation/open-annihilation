// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Alt+Enter: switching the game's window between full screen and a window,
// the pointer kept on the game's screen in full screen, also while a screen
// drops the window's events, and the edge a pointer past a full-screen
// window's edge touches; the window brought back onto its display when it
// leaves full screen, its title bar and borders shown or hidden, and the
// line --trace-input writes of it.
#include "oa/app/full_screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace oa::app {

namespace {

/// One axis of a window's frame or of a display's usable area: where it
/// starts and how long it is, in the desktop's coordinates.
struct Span {
    int64_t start{};
    int64_t length{};
};

/// Places one axis of a window's frame on the same axis of a display's
/// usable area, as window_on_display says.
///
/// @param window the frame's start and length on the axis
/// @param usable the usable area's start and length on the axis
/// @param resizable whether the window may be resized
/// @return the frame's start and length on the display
Span span_on_display(Span window, Span usable, bool resizable) {
    const int64_t usable_end = usable.start + usable.length;
    if (window.start >= usable.start && window.start + window.length <= usable_end)
        return window;
    const int64_t margin = usable.length * window_margin_percent / 100;
    const int64_t room = usable.length - 2 * margin;
    if (window.length <= room) {
        // A window that fits between the margins has only one side outside.
        const int64_t start = window.start < usable.start ? usable.start + margin
                                                          : usable_end - margin - window.length;
        return {start, window.length};
    }
    return {usable.start + margin, resizable ? std::max<int64_t>(room, 1) : window.length};
}

/// Tells whether an event is one of a window's own events.
///
/// @param event any SDL event
/// @return true for the SDL_EVENT_WINDOW_ events
bool is_window_event(const SDL_Event& event) {
    return event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST;
}

/// The function told when the window's place may have changed; null until one is set.
WindowPlaceWatcher place_watcher = nullptr;

/// Tells the watcher the window's place may have changed.
///
/// @param window the game's window; null or no watcher does nothing
void note_window_place(SDL_Window* window) {
    if (place_watcher != nullptr && window != nullptr)
        place_watcher(window);
}

/// Tells whether an event can move, resize or change the mode of the window.
///
/// @param event any SDL event
/// @return true for a move, a resize, full screen, a display change, or a maximise or restore
bool changes_saved_place(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        return true;
    default:
        return false;
    }
}

/// Notes the display a full-screen window is on, and brings a window that has
/// left full screen back onto that display once the window system has placed
/// it (bring_window_on_display).
///
/// @param window the game's window
/// @param[in,out] full_screen the display noted and the window's place still to
///        be checked
void follow_window_place(SDL_Window* window, FullScreenSwitch& full_screen) {
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
        full_screen.full_screen_display = SDL_GetDisplayForWindow(window);
        return;
    }
    if (full_screen.places_window(SDL_GetTicks()) && bring_window_on_display(
                                                         window,
                                                         full_screen.full_screen_display,
                                                         full_screen.window_width,
                                                         full_screen.window_height
                                                     ))
        full_screen.placing_window = false;
}

/// Places one axis of a window's frame that keeps its size on the same
/// axis of a display's usable area, as window_at_size_on_display says.
///
/// @param window the frame's start and length on the axis
/// @param usable the usable area's start and length on the axis
/// @return the frame's start and length on the display
Span span_at_size_on_display(Span window, Span usable) {
    if (window.length > usable.length)
        return {usable.start, window.length};
    return {
        std::clamp(window.start, usable.start, usable.start + usable.length - window.length),
        window.length
    };
}

/// Reads a window's frame: its contents with its title bar and borders,
/// where the window system reports them, at a size asked for.
///
/// @param window the window
/// @param width the contents' width asked for; 0 for the window's own
/// @param height the contents' height asked for; 0 for the window's own
/// @param[out] frame the frame, in the desktop's coordinates
/// @param[out] left the left border's width
/// @param[out] top the title bar's and top border's height
/// @return false when the window system reports no place or size, or the
///     window's decorations are still to come back
bool read_window_frame(
    SDL_Window* window, int width, int height, SDL_Rect& frame, int& left, int& top
) {
    // The title bar and borders, where the window system reports them. An X11
    // window manager reports none while it puts a window's decorations back
    // after full screen, and the window's place as a window comes after them.
    top = 0;
    left = 0;
    int bottom = 0;
    int right = 0;
    if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) &&
        (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) == 0 && top == 0 && left == 0 &&
        bottom == 0 && right == 0)
        return false;
    SDL_Rect contents{};
    if (!SDL_GetWindowPosition(window, &contents.x, &contents.y) ||
        !SDL_GetWindowSize(window, &contents.w, &contents.h))
        return false;
    if (width > 0 && height > 0) {
        contents.w = width;
        contents.h = height;
    }
    frame = {
        contents.x - left, contents.y - top, contents.w + left + right, contents.h + top + bottom
    };
    return true;
}

} // namespace

SDL_Rect window_on_display(const SDL_Rect& window, const SDL_Rect& usable, bool resizable) {
    const Span across = span_on_display({window.x, window.w}, {usable.x, usable.w}, resizable);
    const Span down = span_on_display({window.y, window.h}, {usable.y, usable.h}, resizable);
    // Every start and length lies between the window's own and the usable
    // area's, so it fits an int.
    return {
        static_cast<int>(across.start),
        static_cast<int>(down.start),
        static_cast<int>(across.length),
        static_cast<int>(down.length)
    };
}

SDL_Rect window_at_size_on_display(const SDL_Rect& window, const SDL_Rect& usable) {
    const Span across = span_at_size_on_display({window.x, window.w}, {usable.x, usable.w});
    const Span down = span_at_size_on_display({window.y, window.h}, {usable.y, usable.h});
    // Every start lies between the window's own and the usable area's, so
    // it fits an int.
    return {static_cast<int>(across.start), static_cast<int>(down.start), window.w, window.h};
}

void keep_window_on_display(SDL_Window* window, int width, int height) {
    if (window == nullptr || width <= 0 || height <= 0)
        return;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0)
        return;
    SDL_Rect usable{};
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect frame{};
    int left = 0;
    int top = 0;
    if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable) ||
        !read_window_frame(window, width, height, frame, left, top))
        return;
    const SDL_Rect placed = window_at_size_on_display(frame, usable);
    // A window system that places windows itself refuses the move.
    if (placed.x != frame.x || placed.y != frame.y)
        std::ignore = SDL_SetWindowPosition(window, placed.x + left, placed.y + top);
}

bool bring_window_on_display(SDL_Window* window, SDL_DisplayID display, int width, int height) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0)
        return false;
    const bool sized = width > 0 && height > 0;
    if ((flags & SDL_WINDOW_MAXIMIZED) != 0) {
        if (!sized)
            return true;
        // A size asked for is taken once the window is back to a size of
        // its own; a window system that refuses leaves it maximised.
        return !SDL_RestoreWindow(window);
    }
    // The title bar and borders, where the window system reports them. An X11
    // window manager reports none while it puts a window's decorations back
    // after full screen, and the window's place as a window comes after them.
    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
    if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) &&
        (flags & SDL_WINDOW_BORDERLESS) == 0 && top == 0 && left == 0 && bottom == 0 && right == 0)
        return false;
    // The size asked for reaches the game as any resize does, and the screen
    // is laid out again at it; a window system that refuses it leaves the
    // window as it was, which the game keeps drawing at.
    if (sized)
        std::ignore = SDL_SetWindowSize(window, width, height);
    SDL_Rect usable{};
    if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable)) {
        display = SDL_GetDisplayForWindow(window);
        if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable))
            return true;
    }
    SDL_Rect contents{};
    if (!SDL_GetWindowPosition(window, &contents.x, &contents.y) ||
        !SDL_GetWindowSize(window, &contents.w, &contents.h))
        return true;
    if (sized) {
        contents.w = width;
        contents.h = height;
    }
    const SDL_Rect frame{
        contents.x - left, contents.y - top, contents.w + left + right, contents.h + top + bottom
    };
    const SDL_Rect placed =
        sized ? window_at_size_on_display(frame, usable)
              : window_on_display(frame, usable, (flags & SDL_WINDOW_RESIZABLE) != 0);
    const int x = placed.x + left;
    const int y = placed.y + top;
    const int placed_width = std::max(1, placed.w - left - right);
    const int placed_height = std::max(1, placed.h - top - bottom);
    // A window system that places windows itself refuses the move, and the
    // window keeps its size as well.
    if ((x != contents.x || y != contents.y) && !SDL_SetWindowPosition(window, x, y))
        return true;
    // The window's new size reaches the game as any resize does, and the
    // screen is laid out again at it.
    // A window system that refuses the size leaves the window as it was,
    // which the game keeps drawing at.
    if (placed_width != contents.w || placed_height != contents.h)
        std::ignore = SDL_SetWindowSize(window, placed_width, placed_height);
    return true;
}

bool set_window_frame(SDL_Window* window, bool shown) {
    if (window == nullptr)
        return true;
    SDL_Rect before{};
    const bool read = SDL_GetWindowPosition(window, &before.x, &before.y) &&
                      SDL_GetWindowSize(window, &before.w, &before.h);
    if (!SDL_SetWindowBordered(window, shown))
        return false;
    SDL_Rect after{};
    if (!read || (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0 ||
        !SDL_GetWindowSize(window, &after.w, &after.h) ||
        (after.w == before.w && after.h == before.h))
        return true;
    // The contents go back where they were, then to their size, which a
    // window system that places windows itself refuses; the window is then
    // drawn at the size it is given, as after any resize.
    std::ignore = SDL_SetWindowPosition(window, before.x, before.y);
    std::ignore = SDL_SetWindowSize(window, before.w, before.h);
    return true;
}

bool switch_full_screen(SDL_Window* window, FullScreenSwitch& full_screen) {
    const bool shown = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const bool wanted = full_screen.next_mode(shown, SDL_GetTicks());
    if (!SDL_SetWindowFullscreen(window, wanted))
        return false;
    full_screen.note_request(wanted, SDL_GetTicks());
    return true;
}

void keep_pointer_on_screen(SDL_Window* window) {
    if (window == nullptr)
        return;
    // A window system that refuses leaves the pointer free, and the next
    // change of the window's state asks again.
    std::ignore =
        SDL_SetWindowMouseGrab(window, keeps_pointer_on_screen(SDL_GetWindowFlags(window)));
}

void release_pointer(SDL_Window* window) {
    // A grab the window system will not release ends when the window
    // closes. The rect is the one a sticky edge set.
    if (window == nullptr)
        return;
    std::ignore = SDL_SetWindowMouseGrab(window, false);
    std::ignore = SDL_SetWindowMouseRect(window, nullptr);
}

bool drop_window_events(SDL_Window* window) {
    bool closed = false;
    bool bounds_changed = false;
    SDL_Event polled{};
    while (SDL_PollEvent(&polled)) {
        if (polled.type == SDL_EVENT_QUIT || polled.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            std::ignore = SDL_PushEvent(&polled);
            closed = true;
            break;
        }
        bounds_changed = bounds_changed || changes_pointer_bounds(polled);
    }
    // SDL has updated the window's flags by the time its event is read, so
    // the flags settle the hold whichever of the events came last.
    if (bounds_changed)
        keep_pointer_on_screen(window);
    return closed;
}

namespace {

/// Tells whether a rectangle holds a point of the desktop: the points of its
/// left and top edges, not those of its right and bottom edges.
///
/// @param rect the rectangle
/// @param x the point's column
/// @param y the point's row
/// @return true when the rectangle holds the point
bool holds_point(const SDL_Rect& rect, float x, float y) noexcept {
    const auto left = static_cast<double>(rect.x);
    const auto top = static_cast<double>(rect.y);
    const auto column = static_cast<double>(x);
    const auto row = static_cast<double>(y);
    return column >= left && column < left + static_cast<double>(rect.w) && row >= top &&
           row < top + static_cast<double>(rect.h);
}

/// Writes a rectangle as `--trace-input` does: x,y,WxH.
///
/// @param[in,out] line the line being written
/// @param rect the rectangle
void write_rect(std::ostringstream& line, const SDL_Rect& rect) {
    line << rect.x << ',' << rect.y << ',' << rect.w << 'x' << rect.h;
}

} // namespace

PointerPlace
pointer_place(float x, float y, const SDL_Rect& window, const SDL_Rect& display) noexcept {
    if (holds_point(window, x, y))
        return PointerPlace::window;
    return holds_point(display, x, y) ? PointerPlace::display : PointerPlace::elsewhere;
}

std::optional<SDL_FPoint> full_screen_pointer(
    SDL_WindowFlags window_flags, const SDL_Rect& window, const SDL_Rect& display, float x, float y
) noexcept {
    if (!keeps_pointer_on_screen(window_flags) || window.w <= 0 || window.h <= 0)
        return std::nullopt;
    const PointerPlace place = pointer_place(x, y, window, display);
    if (place == PointerPlace::elsewhere)
        return std::nullopt;
    const double column = static_cast<double>(x) - static_cast<double>(window.x);
    const double row = static_cast<double>(y) - static_cast<double>(window.y);
    if (place == PointerPlace::window)
        return SDL_FPoint{static_cast<float>(column), static_cast<float>(row)};
    return SDL_FPoint{
        static_cast<float>(std::clamp(column, 0.0, static_cast<double>(window.w - 1))),
        static_cast<float>(std::clamp(row, 0.0, static_cast<double>(window.h - 1)))
    };
}

std::optional<SDL_FPoint> read_full_screen_pointer(SDL_Window* window) {
    if (window == nullptr)
        return std::nullopt;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    SDL_Rect bounds{};
    SDL_Rect contents{};
    if (display == 0 || !SDL_GetDisplayBounds(display, &bounds) ||
        !SDL_GetWindowPosition(window, &contents.x, &contents.y) ||
        !SDL_GetWindowSize(window, &contents.w, &contents.h))
        return std::nullopt;
    float x = 0.0F;
    float y = 0.0F;
    std::ignore = SDL_GetGlobalMouseState(&x, &y);
    return full_screen_pointer(SDL_GetWindowFlags(window), contents, bounds, x, y);
}

std::string window_trace_line(const WindowTrace& trace) {
    std::ostringstream line;
    line << "input window " << trace.reason << " window=";
    write_rect(line, trace.window);
    line << " pixels=" << trace.pixel_width << 'x' << trace.pixel_height
         << " display=" << trace.display << " bounds=";
    write_rect(line, trace.bounds);
    line << " usable=";
    write_rect(line, trace.usable);
    line << " scale=" << trace.scale << " edges=";
    const WindowEdges edges = window_edges(trace.window, trace.bounds);
    if (edges.covers())
        line << "covers";
    else
        line << "left:" << edges.left << ",top:" << edges.top << ",right:" << edges.right
             << ",bottom:" << edges.bottom;
    // The window's state, in this order; a display mode of its own counts
    // only in full screen.
    const bool full_screen = (trace.flags & SDL_WINDOW_FULLSCREEN) != 0;
    const std::array<std::pair<bool, std::string_view>, 10> states{{
        {full_screen, "full-screen"},
        {full_screen && trace.exclusive, "exclusive"},
        {(trace.flags & SDL_WINDOW_MAXIMIZED) != 0, "maximised"},
        {(trace.flags & SDL_WINDOW_MINIMIZED) != 0, "minimised"},
        {(trace.flags & SDL_WINDOW_HIDDEN) != 0, "hidden"},
        {(trace.flags & SDL_WINDOW_BORDERLESS) != 0, "borderless"},
        {(trace.flags & SDL_WINDOW_INPUT_FOCUS) != 0, "input-focus"},
        {(trace.flags & SDL_WINDOW_MOUSE_FOCUS) != 0, "mouse-focus"},
        {(trace.flags & SDL_WINDOW_MOUSE_GRABBED) != 0, "grabbed"},
        {(trace.flags & SDL_WINDOW_MOUSE_CAPTURE) != 0, "mouse-capture"},
    }};
    line << " flags=";
    bool any = false;
    for (const auto& [set, word] : states) {
        if (!set)
            continue;
        line << (any ? "," : "") << word;
        any = true;
    }
    if (!any)
        line << "none";
    line << " keep=" << (keeps_pointer_on_screen(trace.flags) ? "yes" : "no") << " mouse-rect=";
    if (trace.mouse_rect)
        write_rect(line, *trace.mouse_rect);
    else
        line << "none";
    line << " pointer=" << trace.pointer_x << ',' << trace.pointer_y << " pointer-on=";
    switch (pointer_place(trace.pointer_x, trace.pointer_y, trace.window, trace.bounds)) {
    case PointerPlace::window:
        line << "window";
        break;
    case PointerPlace::display:
        line << "display";
        break;
    case PointerPlace::elsewhere:
        line << "elsewhere";
        break;
    }
    return line.str();
}

std::string_view traced_window_event(const SDL_Event& event) noexcept {
    switch (event.type) {
    case SDL_EVENT_WINDOW_SHOWN:
        return "shown";
    case SDL_EVENT_WINDOW_HIDDEN:
        return "hidden";
    case SDL_EVENT_WINDOW_MOVED:
        return "moved";
    case SDL_EVENT_WINDOW_RESIZED:
        return "resized";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        return "pixel-size-changed";
    case SDL_EVENT_WINDOW_MINIMIZED:
        return "minimised";
    case SDL_EVENT_WINDOW_MAXIMIZED:
        return "maximised";
    case SDL_EVENT_WINDOW_RESTORED:
        return "restored";
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        return "enter";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        return "leave";
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        return "focus-gained";
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return "focus-lost";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        return "display-changed";
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        return "scale-changed";
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
        return "safe-area-changed";
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        return "full-screen-entered";
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        return "full-screen-left";
    case SDL_EVENT_DISPLAY_ORIENTATION:
        return "display-turned";
    case SDL_EVENT_DISPLAY_ADDED:
        return "display-added";
    case SDL_EVENT_DISPLAY_REMOVED:
        return "display-removed";
    case SDL_EVENT_DISPLAY_MOVED:
        return "display-moved";
    case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        return "display-desktop-mode-changed";
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        return "display-mode-changed";
    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
        return "display-scale-changed";
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED:
        return "display-usable-area-changed";
#endif
    default:
        return {};
    }
}

bool take_full_screen_event(
    SDL_Window* window, FullScreenSwitch& full_screen, const SDL_Event& event, bool switch_chord
) {
    // SDL has updated the window's flags by the time its event is read.
    if (window != nullptr && changes_pointer_bounds(event))
        keep_pointer_on_screen(window);
    if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
        const bool entered = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        full_screen.note_shown(entered);
        if (!entered)
            full_screen.note_left(SDL_GetTicks());
    }
    if (is_window_event(event)) {
        if (window != nullptr)
            follow_window_place(window, full_screen);
        if (changes_saved_place(event))
            note_window_place(window);
        return false;
    }
    switch (full_screen.take_key(event, switch_chord)) {
    case FullScreenKey::none:
        return false;
    case FullScreenKey::toggle:
        if (window == nullptr)
            return true;
        // The display is noted before the window leaves full screen; a
        // window system that switches at once has placed the window as a
        // window when the switch returns.
        follow_window_place(window, full_screen);
        if (switch_full_screen(window, full_screen)) {
            follow_window_place(window, full_screen);
            note_window_place(window);
        } else {
            std::cerr << "open-annihilation: switching full screen failed: " << SDL_GetError()
                      << '\n';
        }
        return true;
    case FullScreenKey::repeat:
    case FullScreenKey::release:
        return true;
    }
    return false;
}

bool take_full_screen_event(
    SDL_Window* window, FullScreenSwitch& full_screen, const SDL_Event& event
) {
    const bool chord = full_screen_key(event) == FullScreenKey::toggle;
    if (take_full_screen_event(window, full_screen, event, chord))
        return true;
    // A repeat of Alt+Enter that is not the held key is still the intro's.
    return full_screen_key(event) == FullScreenKey::repeat;
}

namespace {

/// The largest width or height a remembered place accepts.
constexpr int32_t kLargestPlaceSize = 16384;

/// The furthest coordinate, either way, a remembered place accepts.
constexpr int32_t kFurthestPlaceCoordinate = 100000;

/// One preference value being read.
class PlaceText {
  public:

    /// Reads `text` from its start.
    ///
    /// @param text the value
    explicit PlaceText(std::string_view text) : rest_(text) {}

    /// Takes a literal prefix.
    ///
    /// @param word the characters to take
    /// @return true when they were next
    bool take(std::string_view word) {
        if (!rest_.starts_with(word))
            return false;
        rest_.remove_prefix(word.size());
        return true;
    }

    /// Reads one integer.
    ///
    /// @return the integer, or nothing when the next characters are not one
    std::optional<int32_t> integer() {
        if (rest_.empty())
            return std::nullopt;
        const char* const begin = rest_.data();
        const char* const end = begin + rest_.size();
        int32_t number = 0;
        const auto parsed = std::from_chars(begin, end, number);
        if (parsed.ec != std::errc{} || parsed.ptr == begin)
            return std::nullopt;
        rest_.remove_prefix(static_cast<std::size_t>(parsed.ptr - begin));
        return number;
    }

    /// Tells whether the value has been read to its end.
    ///
    /// @return true when nothing remains
    [[nodiscard]] bool empty() const { return rest_.empty(); }

  private:

    std::string_view rest_;
};

/// Tells whether a size is one a remembered place accepts.
///
/// @param size a width or a height
/// @return true for 1 through kLargestPlaceSize
bool allowed_size(int32_t size) {
    return size >= 1 && size <= kLargestPlaceSize;
}

/// Tells whether a coordinate is one a remembered place accepts.
///
/// @param coordinate a position
/// @return true for -kFurthestPlaceCoordinate through kFurthestPlaceCoordinate
bool allowed_coordinate(int32_t coordinate) {
    return coordinate >= -kFurthestPlaceCoordinate && coordinate <= kFurthestPlaceCoordinate;
}

/// Reads `WIDTHxHEIGHT at X,Y`.
///
/// @param[in,out] cursor the value, at the width
/// @param[out] geometry the contents
/// @return false when the text is not that shape or a number is out of range
bool read_geometry(PlaceText& cursor, WindowGeometry& geometry) {
    const auto width = cursor.integer();
    if (!width || !cursor.take("x"))
        return false;
    const auto height = cursor.integer();
    if (!height || !cursor.take(" at "))
        return false;
    const auto x = cursor.integer();
    if (!x || !cursor.take(","))
        return false;
    const auto y = cursor.integer();
    if (!y || !allowed_size(*width) || !allowed_size(*height) || !allowed_coordinate(*x) ||
        !allowed_coordinate(*y))
        return false;
    geometry.width = *width;
    geometry.height = *height;
    geometry.x = *x;
    geometry.y = *y;
    return true;
}

/// Reads ` on DX,DY,DW,DH`.
///
/// @param[in,out] cursor the value, at the space before on
/// @param[out] place the display
/// @return false when the text is not that shape or a number is out of range
bool read_display(PlaceText& cursor, SavedWindow& place) {
    if (!cursor.take(" on "))
        return false;
    const auto x = cursor.integer();
    if (!x || !cursor.take(","))
        return false;
    const auto y = cursor.integer();
    if (!y || !cursor.take(","))
        return false;
    const auto width = cursor.integer();
    if (!width || !cursor.take(","))
        return false;
    const auto height = cursor.integer();
    if (!height || !allowed_coordinate(*x) || !allowed_coordinate(*y) || !allowed_size(*width) ||
        !allowed_size(*height))
        return false;
    place.display_x = *x;
    place.display_y = *y;
    place.display_width = *width;
    place.display_height = *height;
    return true;
}

/// Writes one integer into a place's text.
///
/// @param[in,out] text the value being written
/// @param number the integer
void append_number(std::string& text, int32_t number) {
    text += std::to_string(number);
}

/// Writes a window's contents into a place's text.
///
/// @param[in,out] text the value being written
/// @param geometry the contents
void append_geometry(std::string& text, const WindowGeometry& geometry) {
    append_number(text, geometry.width);
    text += 'x';
    append_number(text, geometry.height);
    text += " at ";
    append_number(text, geometry.x);
    text += ',';
    append_number(text, geometry.y);
}

/// Writes a display's bounds into a place's text.
///
/// @param[in,out] text the value being written
/// @param place the place whose display is written
void append_display(std::string& text, const SavedWindow& place) {
    text += " on ";
    append_number(text, place.display_x);
    text += ',';
    append_number(text, place.display_y);
    text += ',';
    append_number(text, place.display_width);
    text += ',';
    append_number(text, place.display_height);
}

/// Moves one axis of a window so the window lies wholly in a usable area.
///
/// A length longer than the area shrinks to the area. A length that fits
/// keeps its size and moves the least that puts it inside.
///
/// @param[in,out] start the window's start on the axis
/// @param[in,out] length the window's length on the axis
/// @param area_start the usable area's start
/// @param area_length the usable area's length
void fit_axis(int32_t& start, int32_t& length, int32_t area_start, int32_t area_length) {
    if (area_length < 1)
        area_length = 1;
    if (length > area_length)
        length = area_length;
    if (length < 1)
        length = 1;
    const int64_t latest =
        static_cast<int64_t>(area_start) + static_cast<int64_t>(area_length) - length;
    int64_t placed = start;
    if (placed < area_start)
        placed = area_start;
    if (placed > latest)
        placed = latest;
    if (placed > std::numeric_limits<int32_t>::max())
        placed = std::numeric_limits<int32_t>::max();
    if (placed < std::numeric_limits<int32_t>::min())
        placed = std::numeric_limits<int32_t>::min();
    start = static_cast<int32_t>(placed);
}

/// Finds the display a remembered place opens on.
///
/// The display whose bounds equal the remembered ones wins, the main one
/// when more than one does. Otherwise the main display, or the first.
///
/// @param saved the remembered place; null when there is none
/// @param displays the displays connected now
/// @return the display, or null when none is connected
const DisplayArea* chosen_display(const SavedWindow* saved, std::span<const DisplayArea> displays) {
    if (displays.empty())
        return nullptr;
    const DisplayArea* main = nullptr;
    const DisplayArea* matched = nullptr;
    for (const DisplayArea& area : displays) {
        if (area.main)
            main = &area;
        if (saved == nullptr || area.x != saved->display_x || area.y != saved->display_y ||
            area.width != saved->display_width || area.height != saved->display_height)
            continue;
        if (matched == nullptr || area.main)
            matched = &area;
    }
    if (matched != nullptr)
        return matched;
    if (main != nullptr)
        return main;
    return &displays.front();
}

} // namespace

std::string encode_window_place(const SavedWindow& place) {
    std::string text;
    if (place.full_screen) {
        text = "full-screen";
        append_display(text, place);
        if (place.has_window) {
            text += " window ";
            append_geometry(text, place.window);
        }
        return text;
    }
    text = "window ";
    append_geometry(text, place.window);
    append_display(text, place);
    return text;
}

std::optional<SavedWindow> decode_window_place(std::string_view text) {
    PlaceText cursor(text);
    SavedWindow place;
    if (cursor.take("full-screen")) {
        place.full_screen = true;
        if (!read_display(cursor, place))
            return std::nullopt;
        if (cursor.empty())
            return place;
        if (!cursor.take(" window ") || !read_geometry(cursor, place.window) || !cursor.empty())
            return std::nullopt;
        place.has_window = true;
        return place;
    }
    if (!cursor.take("window ") || !read_geometry(cursor, place.window) ||
        !read_display(cursor, place) || !cursor.empty())
        return std::nullopt;
    place.full_screen = false;
    place.has_window = true;
    return place;
}

std::optional<SavedWindow> window_place_in(const std::map<std::string, std::string>& values) {
    const auto found = values.find(std::string(window_place_key));
    if (found == values.end())
        return std::nullopt;
    return decode_window_place(found->second);
}

void put_window_place(std::map<std::string, std::string>& values, const SavedWindow& place) {
    values.insert_or_assign(std::string(window_place_key), encode_window_place(place));
}

bool store_window_place_unless(
    std::map<std::string, std::string>& values, const SavedWindow& place, bool write
) {
    if (!write)
        return false;
    put_window_place(values, place);
    return true;
}

void set_window_place_watcher(WindowPlaceWatcher watcher) {
    place_watcher = watcher;
}

RestoredPlace restore_window_place(
    const SavedWindow* saved,
    PlaceOverride command_line,
    bool platform_full_screen,
    std::span<const DisplayArea> displays
) {
    RestoredPlace restored;
    if (command_line == PlaceOverride::full_screen)
        restored.full_screen = true;
    else if (command_line == PlaceOverride::window)
        restored.full_screen = false;
    else if (saved != nullptr)
        restored.full_screen = saved->full_screen;
    else
        restored.full_screen = platform_full_screen;

    const DisplayArea* const display = chosen_display(saved, displays);
    if (restored.full_screen) {
        // A first start is left where the window system opens full screen.
        // A remembered full screen, or one asked for this run, opens on its display.
        const bool place_on_display =
            saved != nullptr || command_line == PlaceOverride::full_screen;
        if (place_on_display && display != nullptr) {
            restored.positioned = true;
            restored.x = display->x;
            restored.y = display->y;
        }
        return restored;
    }
    if (saved == nullptr || !saved->has_window) {
        return restored;
    }
    WindowGeometry fitted = saved->window;
    if (display == nullptr) {
        restored.sized = true;
        restored.width = fitted.width;
        restored.height = fitted.height;
        return restored;
    }
    const bool usable = display->usable_width > 0 && display->usable_height > 0;
    const int32_t area_x = usable ? display->usable_x : display->x;
    const int32_t area_y = usable ? display->usable_y : display->y;
    const int32_t area_width = usable ? display->usable_width : display->width;
    const int32_t area_height = usable ? display->usable_height : display->height;
    fit_axis(fitted.x, fitted.width, area_x, area_width);
    fit_axis(fitted.y, fitted.height, area_y, area_height);
    restored.sized = true;
    restored.positioned = true;
    restored.x = fitted.x;
    restored.y = fitted.y;
    restored.width = fitted.width;
    restored.height = fitted.height;
    return restored;
}

} // namespace oa::app
