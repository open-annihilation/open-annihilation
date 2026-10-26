// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Full screen and a window: the flags the game's window opens with, among
// them the display's own pixel density, and what a window at that density
// changes; the Alt+Enter key that switches between full screen and a window,
// the mode the player last asked for while the window system is still
// switching to it, the pointer kept on the game's screen in full screen, the
// window brought back onto its display when it leaves full screen, the
// window's title bar and borders hidden while a game is played, and the line
// --trace-input writes of the window, its display and the pointer.
#pragma once

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// How long a requested switch may take before the window's own report of its
/// mode is trusted again, in milliseconds.
///
/// A macOS full-screen space animates in and out for under a second; a window
/// manager that ignores the request never answers at all.
inline constexpr uint64_t full_screen_settle_ms = 2000;

/// How far inside its display a window that left full screen is put back, in
/// percent of the display's usable width (the left and right sides) or height
/// (the top and bottom); fractions of a pixel are dropped.
inline constexpr int window_margin_percent = 5;

/// Returns where a window goes so that it lies on its display.
///
/// Each side of the window outside the display's usable area moves to
/// window_margin_percent of the area's width or height inside it, and a
/// window with no side outside stays where it is; a side exactly on the
/// area's edge is inside. A window wider or taller than the room between two
/// margins keeps the left or top margin, so that its title bar and its
/// controls are on the display, and shrinks to fit between the margins when
/// it may be resized; one that may not keeps its size and the left or top
/// margin only.
///
/// @param window the window's frame (its contents with its title bar and
///        borders), in the desktop's coordinates, which are negative left of
///        and above the primary display
/// @param usable the display's usable area (SDL_GetDisplayUsableBounds), without
///        the menu bar, the dock or the taskbar
/// @param resizable whether the window may be resized
/// @return the window's frame on the display
[[nodiscard]] SDL_Rect
window_on_display(const SDL_Rect& window, const SDL_Rect& usable, bool resizable);

/// Returns where a window that keeps its size goes so that it lies on its
/// display.
///
/// On each axis a window that fits the display's usable area moves no more
/// than it must to lie within it, and one that lies within it stays where
/// it is; one longer than the area starts at the area's start, so that its
/// title bar and its controls are on the display.
///
/// @param window the window's frame (its contents with its title bar and
///        borders), in the desktop's coordinates, which are negative left of
///        and above the primary display
/// @param usable the display's usable area (SDL_GetDisplayUsableBounds), without
///        the menu bar, the dock or the taskbar
/// @return the window's frame on the display, of the window's size
[[nodiscard]] SDL_Rect window_at_size_on_display(const SDL_Rect& window, const SDL_Rect& usable);

/// What a keyboard event means to the full-screen switch.
enum class FullScreenKey {
    none,    ///< not Alt+Enter; the screens receive it
    toggle,  ///< a fresh Alt+Enter or Alt+keypad Enter: switch the mode
    repeat,  ///< the held key's repeats: nothing happens, and no screen sees them
    release, ///< the release of the Enter key that switched: no screen sees it
};

/// Returns the window flags the game's window is created with.
///
/// A window's pixel density is fixed when it opens: one opened at native
/// density keeps the display's own pixels for the run, and any other the
/// window system's density, which on a high-density display enlarges the
/// game's frame.
///
/// @param start_full_screen whether the run opens full screen
///        (Options::start_full_screen)
/// @param native_density whether the window opens at the display's own
///        pixel density (render_policy::decide_native_density)
/// @return a resizable window, full screen on the desktop's mode when asked,
///         at native density when asked
[[nodiscard]] inline SDL_WindowFlags
game_window_flags(bool start_full_screen, bool native_density) {
    return SDL_WINDOW_RESIZABLE | (start_full_screen ? SDL_WINDOW_FULLSCREEN : 0) |
           (native_density ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0);
}

/// Tells whether a window opened at native density, where a pixel of the
/// match's layout is a window point and the renderer enlarges what is drawn
/// to the display's pixels.
///
/// @param window_flags the window's flags, as SDL_GetWindowFlags reports them
/// @return true for a window game_window_flags opened at native density
[[nodiscard]] inline bool at_native_density(SDL_WindowFlags window_flags) {
    return (window_flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0;
}

/// Returns how deep the band along the screen's edges is in which the
/// pointer scrolls the battlefield: the layout pixels one window point
/// covers, since the pointer rests on the outermost point, not the outermost
/// pixel.
///
/// @param native_density the window opened at native density
///        (at_native_density), where a layout pixel is a window point
/// @param pixel_density the layout pixels a window point covers: the
///        window's pixels per window point (SDL_GetWindowPixelDensity), or
///        for a frame scaled to the screen that times the frame's pixels per
///        pixel it is presented at
/// @return 1 at native density; otherwise the pixel density rounded up
[[nodiscard]] inline int32_t edge_scroll_depth(bool native_density, float pixel_density) {
    return native_density ? 1 : static_cast<int32_t>(std::ceil(pixel_density));
}

/// Tells whether an event is the Alt+Enter that switches full screen.
///
/// Either Alt key counts (Option on macOS), with Return or keypad Enter.
///
/// @param event any SDL event
/// @return what the event means to the switch
[[nodiscard]] inline FullScreenKey full_screen_key(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || (event.key.mod & SDL_KMOD_ALT) == 0 ||
        (event.key.key != SDLK_RETURN && event.key.key != SDLK_KP_ENTER))
        return FullScreenKey::none;
    return event.key.repeat ? FullScreenKey::repeat : FullScreenKey::toggle;
}

/// The window's full-screen mode as the player last asked for it, and the key that asked.
///
/// On macOS, X11 and Wayland the window changes mode some time after it is
/// asked to, and until then it still reports its old mode; a second Alt+Enter
/// in that time switches from the mode the player asked for, not from the one
/// still shown. Windows switches at once.
///
/// The trigger key of the switch's chord belongs to the switch until it comes
/// up: letting go of a modifier first does not hand its repeats to the screens.
///
/// A window that leaves full screen is brought back onto the display it was
/// full screen on (window_on_display) once the window system has given it its
/// place as a window, which an X11 window manager does some time after the
/// window has left (bring_window_on_display).
struct FullScreenSwitch {
    bool requested{};        ///< the mode last asked for: true for full screen
    bool awaiting_shown{};   ///< the window has not yet shown the requested mode
    uint64_t requested_ms{}; ///< when the mode was asked for, SDL_GetTicks milliseconds
    /// The trigger key of the last switch chord while it is held; SDL_SCANCODE_UNKNOWN when none is.
    SDL_Scancode held_enter{SDL_SCANCODE_UNKNOWN};
    /// The display the window was last seen full screen on; 0 before it was.
    SDL_DisplayID full_screen_display{};
    /// The window left full screen, or was asked to, and its place as a window
    /// is still to be checked.
    bool placing_window{};
    /// When the window left full screen or was asked to, SDL_GetTicks milliseconds.
    uint64_t left_ms{};
    /// The screen size applied this run (apply_screen_size in
    /// screen_mode.hpp), in the window system's units: what full screen
    /// shows, through a display mode of the size or a frame drawn at it and
    /// scaled to the screen; 0 by 0 for the desktop's own.
    int32_t screen_width{};
    int32_t screen_height{}; ///< the screen size's height, as screen_width
    /// The size the window takes once it leaves full screen, in the window
    /// system's units, until it enters full screen again; 0 by 0 keeps the
    /// size it comes back at.
    int32_t window_width{};
    int32_t window_height{}; ///< that size's height, as window_width

    /// Tells what a keyboard event means to the switch, following the key its chord holds.
    ///
    /// A fresh press of the switch's chord holds that key. Until the key comes
    /// up, its repeats are repeats and its release is taken, whether or not the
    /// chord's modifiers are still held. A fresh press of the same key that is
    /// not the chord (its release was lost) lets it go and reaches the screens.
    /// Any other key reaches the screens and leaves the hold as it is.
    ///
    /// @param event any SDL event
    /// @param switch_chord true when this event is a fresh press of the switch's chord
    /// @return what the event means to the switch
    [[nodiscard]] FullScreenKey take_key(const SDL_Event& event, bool switch_chord) {
        const bool key_event = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
        const bool held =
            key_event && held_enter != SDL_SCANCODE_UNKNOWN && event.key.scancode == held_enter;
        if (held && event.type == SDL_EVENT_KEY_UP) {
            held_enter = SDL_SCANCODE_UNKNOWN;
            return FullScreenKey::release;
        }
        if (held && event.type == SDL_EVENT_KEY_DOWN && event.key.repeat)
            return FullScreenKey::repeat;
        if (switch_chord && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            held_enter = event.key.scancode;
            return FullScreenKey::toggle;
        }
        if (held && event.type == SDL_EVENT_KEY_DOWN)
            held_enter = SDL_SCANCODE_UNKNOWN;
        return FullScreenKey::none;
    }

    /// Chooses the mode an Alt+Enter asks for.
    ///
    /// @param shown whether the window reports full screen now
    /// @param now_ms the time of the key, SDL_GetTicks milliseconds
    /// @return true to ask for full screen, false for a window
    [[nodiscard]] bool next_mode(bool shown, uint64_t now_ms) const {
        const bool settling = awaiting_shown && now_ms - requested_ms < full_screen_settle_ms;
        return !(settling ? requested : shown);
    }

    /// Records a mode the window was asked for.
    ///
    /// Asking for a window starts the check of the window's place as a
    /// window; asking for full screen ends it.
    ///
    /// @param full_screen true for full screen, false for a window
    /// @param now_ms the time of the request, SDL_GetTicks milliseconds
    void note_request(bool full_screen, uint64_t now_ms) {
        requested = full_screen;
        awaiting_shown = true;
        requested_ms = now_ms;
        placing_window = !full_screen;
        left_ms = now_ms;
    }

    /// Notes that the window entered or left full screen.
    ///
    /// A change to the requested mode ends the wait; a change on the way to it
    /// (entering a space that a later request leaves again) does not. Entering
    /// full screen ends the check of the window's place as a window, and
    /// forgets the size asked for as the window leaves it (window_width): the
    /// window comes back at the size it went in at.
    ///
    /// @param shown true when the window entered full screen
    void note_shown(bool shown) {
        if (awaiting_shown && shown == requested)
            awaiting_shown = false;
        if (shown) {
            placing_window = false;
            window_width = 0;
            window_height = 0;
        }
    }

    /// Notes that the window left full screen, after note_shown: its place as
    /// a window is checked from now on, unless it is leaving on its way back
    /// to the full screen a later request asked for.
    ///
    /// @param now_ms the time the window left, SDL_GetTicks milliseconds
    void note_left(uint64_t now_ms) {
        placing_window = !(awaiting_shown && requested);
        left_ms = now_ms;
    }

    /// Tells whether the window's place as a window is still to be checked.
    ///
    /// The check lasts full_screen_settle_ms from the time the window left
    /// full screen or was asked to; a window the window system has not placed
    /// by then is left where it is.
    ///
    /// @param now_ms the time now, SDL_GetTicks milliseconds
    /// @return true while the window's place is still to be checked
    [[nodiscard]] bool places_window(uint64_t now_ms) const {
        return placing_window && now_ms - left_ms < full_screen_settle_ms;
    }
};

/// Asks a window for the mode Alt+Enter switches to.
///
/// Switches from the mode last asked for while the window is still changing
/// to it, and from the mode the window shows otherwise.
///
/// @param window the game's window
/// @param[in,out] full_screen the mode last asked for; left as it was when
///        SDL refuses
/// @return true when SDL accepted the request; false leaves the window as it
///         was, with SDL_GetError saying why
[[nodiscard]] bool switch_full_screen(SDL_Window* window, FullScreenSwitch& full_screen);

/// Tells whether the pointer is kept on the game's window.
///
/// In full screen with the input focus the pointer stops at the edges of the
/// game's screen, as in 3.1c: it can rest against an edge, on its last row or
/// column of pixels, and never strays onto another monitor. A window, and full
/// screen without the focus (another program switched to, or a dialog of the
/// system's in front), leave it free. A window's sticky edge is a separate
/// rule (sticky_edge.hpp) and is not this one.
///
/// @param window_flags the window's flags, as SDL_GetWindowFlags reports them
/// @return true to keep the pointer on the window
[[nodiscard]] inline bool keeps_pointer_on_screen(SDL_WindowFlags window_flags) {
    return (window_flags & SDL_WINDOW_FULLSCREEN) != 0 &&
           (window_flags & SDL_WINDOW_INPUT_FOCUS) != 0;
}

/// Tells whether an event may change what keeps_pointer_on_screen says.
///
/// These are a window's entering or leaving full screen, a change of its size
/// or of the display it is on, its being shown, hidden, minimised or restored,
/// and its gaining or losing the input focus.
///
/// @param event any SDL event
/// @return true for those window events
[[nodiscard]] inline bool changes_pointer_bounds(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        return true;
    default:
        return false;
    }
}

/// Keeps the pointer on the game's window, or lets it go, as
/// keeps_pointer_on_screen says for the window's flags now.
///
/// The window system holds the pointer inside the whole window and follows
/// the window's size and place. While the window is without the focus the
/// pointer is free, whatever was asked last. A window system that cannot hold
/// the pointer leaves it free, as in a window.
///
/// @param window the game's window; null does nothing
void keep_pointer_on_screen(SDL_Window* window);

/// Lets the pointer leave the game's window, before the window closes or
/// before a dialog of the system's opens over it.
///
/// Also clears a mouse rect a sticky edge set, so the dialog is not
/// confined to the window. keep_pointer_on_screen holds a full screen
/// window again.
///
/// @param window the game's window; null does nothing
void release_pointer(SDL_Window* window);

/// Drops every event waiting but a close, which is put back for whatever
/// answers it next, as a screen that answers none of them does: the
/// progress dialog while the game's archives load at start and while other
/// work runs.
///
/// A dropped event that may change whether the pointer is kept on the
/// game's window (changes_pointer_bounds) still has its say: the window's
/// state then settles the hold (keep_pointer_on_screen). A window that
/// opened in full screen, or took the focus, while its events were dropped
/// holds the pointer as one Alt+Enter switched to full screen does.
///
/// @param window the game's window; null leaves the pointer as it is
/// @return true when the player asked to close the window
bool drop_window_events(SDL_Window* window);

/// How far each edge of a window lies inside a display, in the desktop's
/// coordinates: above 0 inside the display, 0 on the display's own edge, and
/// below 0 past it, where the pointer cannot reach that edge of the window.
struct WindowEdges {
    int64_t left{};   ///< from the display's left edge to the window's
    int64_t top{};    ///< from the display's top edge to the window's
    int64_t right{};  ///< from the window's right edge to the display's
    int64_t bottom{}; ///< from the window's bottom edge to the display's

    /// Tells whether the window lies exactly over the display.
    ///
    /// @return true when every edge lies on the display's
    [[nodiscard]] constexpr bool covers() const noexcept {
        return left == 0 && top == 0 && right == 0 && bottom == 0;
    }
};

/// Returns how far each edge of a window lies inside a display.
///
/// @param window the window's contents, in the desktop's coordinates
/// @param display the display's bounds, in the desktop's coordinates
/// @return each edge's distance inside the display
[[nodiscard]] constexpr WindowEdges
window_edges(const SDL_Rect& window, const SDL_Rect& display) noexcept {
    const auto wide = [](int value) { return static_cast<int64_t>(value); };
    return {
        wide(window.x) - wide(display.x),
        wide(window.y) - wide(display.y),
        wide(display.x) + wide(display.w) - (wide(window.x) + wide(window.w)),
        wide(display.y) + wide(display.h) - (wide(window.y) + wide(window.h)),
    };
}

/// Where the pointer is against the game's window and the display it is on.
enum class PointerPlace : uint8_t {
    window,    ///< over the window's contents
    display,   ///< outside the window, on its display
    elsewhere, ///< off the window's display
};

/// Returns where a point of the desktop lies against a window and its display.
///
/// A rectangle holds the points of its left and top edges, not those of its
/// right and bottom edges.
///
/// @param x the point's column, in the desktop's coordinates
/// @param y the point's row, in the desktop's coordinates
/// @param window the window's contents, in the desktop's coordinates
/// @param display the display's bounds, in the desktop's coordinates
/// @return where the point lies
[[nodiscard]] PointerPlace
pointer_place(float x, float y, const SDL_Rect& window, const SDL_Rect& display) noexcept;

/// Returns the point of a full-screen window the pointer touches, for the
/// screen's edges.
///
/// In full screen with the input focus (keeps_pointer_on_screen) the window
/// is the game's screen, and the pointer against one of its edges scrolls
/// toward it, as against the screen's edge in 3.1c. A pointer on the
/// window's display but past one of the window's edges touches that edge:
/// it is taken on the window's last row or column on that side, and in a
/// corner on both. macOS places a full-screen window below the camera
/// housing of a MacBook's built-in display and leaves the strip beside the
/// housing, above the window, to the menu bar. A pointer on the window is
/// where it is.
///
/// @param window_flags the window's flags, as SDL_GetWindowFlags reports them
/// @param window the window's contents, in the desktop's coordinates
/// @param display the bounds of the window's display, in the desktop's coordinates
/// @param x the pointer's column, in the desktop's coordinates
/// @param y the pointer's row, in the desktop's coordinates
/// @return the point, in the window's coordinates; none outside full screen,
///         without the input focus, for an empty window, or for a pointer
///         off the window's display
[[nodiscard]] std::optional<SDL_FPoint> full_screen_pointer(
    SDL_WindowFlags window_flags, const SDL_Rect& window, const SDL_Rect& display, float x, float y
) noexcept;

/// Reads the point of the game's window the pointer touches now
/// (full_screen_pointer), from the pointer's place on the desktop, the
/// window's place and size and the bounds of its display.
///
/// @param window the game's window; null gives none
/// @return the point, in the window's coordinates; none as
///         full_screen_pointer says, or when SDL reports no display, place
///         or size for the window
[[nodiscard]] std::optional<SDL_FPoint> read_full_screen_pointer(SDL_Window* window);

/// What `--trace-input` writes of the game's window: where it lies on its
/// display, its state, how the pointer is held, and where the pointer is.
struct WindowTrace {
    std::string_view reason; ///< what the line is for: traced_window_event's word, or "match"
    SDL_Rect window{};       ///< the window's contents, in the desktop's coordinates
    int pixel_width{};       ///< the contents' width in pixels
    int pixel_height{};      ///< the contents' height in pixels
    SDL_DisplayID display{}; ///< the display the window is on; 0 when SDL names none
    SDL_Rect bounds{};       ///< the display's bounds, in the desktop's coordinates
    /// The display's usable area, without the taskbar, the menu bar or the
    /// dock, in the desktop's coordinates.
    SDL_Rect usable{};
    float scale{};           ///< the window's display scale, as SDL_GetWindowDisplayScale gives it
    SDL_WindowFlags flags{}; ///< the window's flags, as SDL_GetWindowFlags reports them
    bool exclusive{}; ///< full screen takes a display mode of its own (SDL_GetWindowFullscreenMode)
    /// The part of the window the pointer is held in (SDL_GetWindowMouseRect);
    /// none for the whole window.
    std::optional<SDL_Rect> mouse_rect;
    float pointer_x{}; ///< the pointer's column, in the desktop's coordinates
    float pointer_y{}; ///< the pointer's row, in the desktop's coordinates
};

/// Returns the line `--trace-input` writes of the game's window.
///
/// The line begins "input window" and the reason, then gives, as words of
/// the form name=value: the window's contents (window=x,y,WxH) and its size
/// in pixels, the display's number, bounds and usable area, the display
/// scale, how far each edge of the window lies inside the display
/// (window_edges; "covers" when the window lies exactly over it), the
/// window's state (full screen, on its own display mode, maximised,
/// minimised, hidden, borderless, the input focus, the pointer's focus, the
/// window's grab and the capture while a button is held; "none" without
/// them), whether full screen keeps the pointer on the window now
/// (keeps_pointer_on_screen), the part of the window the pointer is held in,
/// and the pointer's place on the desktop and against the window and its
/// display (pointer_place).
///
/// @param trace what the line tells
/// @return the line, without a line break
[[nodiscard]] std::string window_trace_line(const WindowTrace& trace);

/// Returns the word `--trace-input` names an event of the game's window or
/// of a display by: its showing, hiding, moving and resizing, its being
/// minimised, maximised and restored, the pointer entering and leaving it,
/// its gaining and losing the input focus, its entering and leaving full
/// screen, its moving to another display, a change of its display's scale
/// or safe area, and a display's being added, removed, moved or turned or
/// changing its mode, its scale or its usable area.
///
/// @param event any SDL event
/// @return the word, such as "leave" or "full-screen-entered"; empty for
///         every other event
[[nodiscard]] std::string_view traced_window_event(const SDL_Event& event) noexcept;

/// Brings a window that has left full screen back onto a display, once the
/// window system has given it its place as a window.
///
/// The window's frame, its title bar and borders included where the window
/// system reports them, is placed on the display's usable area as
/// window_on_display says; a window that may be resized and is too large is
/// resized, and the screen is laid out again at its new size as after any
/// resize. A size asked for is taken first, and kept where the window is
/// placed (window_at_size_on_display); a maximised window is brought back
/// to a size of its own for it. A window in full screen, minimised or
/// hidden, or one whose decorations the window system has yet to put back
/// (an X11 window manager still restoring it), is not yet placed. A
/// maximised window, which the window system fits to the display itself,
/// is left as it is when no size is asked for, and so is a window on a
/// window system that places windows itself (Wayland), which refuses to
/// move it.
///
/// @param window the game's window
/// @param display the display the window was full screen on; 0, or a display
///        no longer connected, takes the display the window is on now
/// @param width the width the window takes, in the window system's units;
///        0 keeps its own
/// @param height the height the window takes; 0 keeps its own
/// @return true when the window's place is settled: it was on the display,
///         or it was moved or resized onto it, or it is left as it is; false
///         while the window system is still placing it
bool bring_window_on_display(
    SDL_Window* window, SDL_DisplayID display, int width = 0, int height = 0
);

/// Moves a window that has just been asked for a size so that, at that
/// size, it lies on the display it is on (window_at_size_on_display), its
/// title bar and borders included where the window system reports them. A
/// window in full screen, minimised or hidden is left where it is, and so
/// is a window on a window system that places windows itself (Wayland).
///
/// @param window the game's window; null does nothing
/// @param width the window's width asked for, in the window system's units
/// @param height the window's height asked for
void keep_window_on_display(SDL_Window* window, int width, int height);

/// What the game's window is asked to do with its frame: its title bar and
/// borders.
enum class WindowFrameRequest : uint8_t {
    none, ///< nothing: the window already shows what it should, or has no frame to change
    show, ///< show the title bar and borders
    hide, ///< hide them
};

/// Where the game is, as the window's frame follows it.
struct WindowFramePlace {
    /// The game has a window that is neither in full screen nor still
    /// switching to or from it (FullScreenSwitch::awaiting_shown within
    /// full_screen_settle_ms); a run without a window has none.
    bool windowed{};
    /// A game is played: its battlefield shows, unfinished, with neither the
    /// game menu nor a panel it opens, a team panel or the exit confirmation
    /// over it.
    bool playing{};
    /// The window shows its title bar and borders now (no
    /// SDL_WINDOW_BORDERLESS among its flags).
    bool bordered{};
};

/// Returns what to ask of the game's window's frame (the Window frame
/// setting).
///
/// With Window frame at Hidden in play, the window hides its title bar and
/// borders while a game is played, and shows them on every other screen
/// and while the game menu or a panel it opens is over the game, so that
/// the window can be moved and closed there; at Always shown it shows them
/// everywhere. Full screen, a window still switching to or from it, and a
/// run without a window, are left as they are.
///
/// @param hidden_in_play Window frame is Hidden in play
/// @param place where the game is, and what the window shows now
/// @return show or hide where the window shows otherwise; none where it
///         already shows what it should, or is not windowed
[[nodiscard]] constexpr WindowFrameRequest
window_frame_request(bool hidden_in_play, const WindowFramePlace& place) noexcept {
    if (!place.windowed)
        return WindowFrameRequest::none;
    const bool shown = !(hidden_in_play && place.playing);
    if (shown == place.bordered)
        return WindowFrameRequest::none;
    return shown ? WindowFrameRequest::show : WindowFrameRequest::hide;
}

/// Shows or hides a window's title bar and borders, keeping the window's
/// contents at their size and place.
///
/// A window system that changes the contents' size with the frame (macOS
/// keeps the frame's own) has the contents put back at the size and place
/// they had, so that the change reaches the game as no resize; a
/// maximised window, which the window system fits to the display itself,
/// is left at the size it is given.
///
/// @param window the game's window; null does nothing
/// @param shown true to show the title bar and borders, false to hide them
/// @return false when SDL refused, with SDL_GetError saying why
bool set_window_frame(SDL_Window* window, bool shown);

/// Handles an event of the full-screen switch before anything else sees it.
///
/// Alt+Enter switches the mode, and a switch SDL refuses is reported on
/// standard error; its repeats and its Enter key's release do nothing
/// (FullScreenSwitch::take_key). The window's entering or leaving full screen
/// is noted, and the caller still handles it. After an event that may change
/// it (changes_pointer_bounds), the pointer is kept on the window or let go
/// (keep_pointer_on_screen). The display a full-screen window is on is noted,
/// and after the window leaves full screen, its own events (its leaving,
/// moving and resizing among them) bring it back onto that display
/// (bring_window_on_display) once the window system has placed it, at the
/// size FullScreenSwitch::window_width and window_height ask for, if any.
///
/// @param window the game's window; null ignores the chord and leaves the
///        pointer and the window's place as they are
/// @param[in,out] full_screen the mode last asked for, the key held and the
///        window's place still to be checked
/// @param event any SDL event
/// @param switch_chord true when this event is a fresh press of the switch's chord
/// @return true when the event was that chord, one of its repeats or its
///         key's release, which nothing else may see
bool take_full_screen_event(
    SDL_Window* window, FullScreenSwitch& full_screen, const SDL_Event& event, bool switch_chord
);

/// Handles an event of the full-screen switch before anything else sees it.
///
/// Alt+Enter is the chord, for the intro and any caller that has not read a
/// layout. A repeat of Alt+Enter is taken even when it is not the key the
/// switch is holding.
///
/// @param window the game's window; null ignores Alt+Enter and leaves the
///        pointer and the window's place as they are
/// @param[in,out] full_screen the mode last asked for, the key held and the
///        window's place still to be checked
/// @param event any SDL event
/// @return true when the event was Alt+Enter, one of its repeats or its
///         Enter key's release, which nothing else may see
bool take_full_screen_event(
    SDL_Window* window, FullScreenSwitch& full_screen, const SDL_Event& event
);

/// The preferences key that remembers full screen or the window's place.
///
/// One value holds the whole place. It is not one of the Settings keys, so
/// restoring defaults leaves it, and a run that never writes it keeps the
/// first start's window.
inline constexpr std::string_view window_place_key = "open-annihilation.window-place";

/// A command line that chooses full screen or a window for one run.
enum class PlaceOverride : uint8_t {
    none,       ///< the remembered place, or the platform's first start
    window,     ///< a window for this run, which is not remembered
    full_screen ///< full screen for this run, which is not remembered
};

/// A window's contents, in the desktop's coordinates.
struct WindowGeometry {
    int32_t x{};      ///< the left of the contents
    int32_t y{};      ///< the top of the contents
    int32_t width{};  ///< the contents' width, at least 1
    int32_t height{}; ///< the contents' height, at least 1

    /// Tells whether two places name the same contents.
    ///
    /// @param other the place to compare
    /// @return true when the origin and the size agree
    [[nodiscard]] bool operator==(const WindowGeometry& other) const = default;
};

/// The full screen or window the player left, and the display it was on.
struct SavedWindow {
    bool full_screen{};       ///< true when the player left the game full screen
    bool has_window{};        ///< a window's size and place are recorded too
    WindowGeometry window{};  ///< that window; unused when has_window is false
    int32_t display_x{};      ///< the display's left, in desktop coordinates
    int32_t display_y{};      ///< the display's top
    int32_t display_width{};  ///< the display's width
    int32_t display_height{}; ///< the display's height

    /// Tells whether two records name the same place.
    ///
    /// @param other the record to compare
    /// @return true when the mode, the window and the display agree
    [[nodiscard]] bool operator==(const SavedWindow& other) const = default;
};

/// One connected display, in desktop coordinates.
struct DisplayArea {
    int32_t x{};             ///< the display's left
    int32_t y{};             ///< the display's top
    int32_t width{};         ///< the display's width
    int32_t height{};        ///< the display's height
    int32_t usable_x{};      ///< the usable area's left, inside the menu bar and the dock
    int32_t usable_y{};      ///< the usable area's top
    int32_t usable_width{};  ///< the usable area's width
    int32_t usable_height{}; ///< the usable area's height
    bool main{};             ///< the system's main display
};

/// Where the next start opens.
struct RestoredPlace {
    bool full_screen{}; ///< open full screen
    bool sized{};       ///< width and height name a window's size
    bool positioned{};  ///< x and y name where the window opens
    int32_t x{};        ///< the left, when positioned
    int32_t y{};        ///< the top, when positioned
    int32_t width{};    ///< the width, when sized
    int32_t height{};   ///< the height, when sized
};

/// Writes a place as one preference value.
///
/// A window reads `window WIDTHxHEIGHT at X,Y on DX,DY,DW,DH`. Full screen
/// reads `full-screen on DX,DY,DW,DH`, and when a window was left as well,
/// ` window WIDTHxHEIGHT at X,Y` after the display.
///
/// @param place the place
/// @return the value
[[nodiscard]] std::string encode_window_place(const SavedWindow& place);

/// Reads a place written by encode_window_place.
///
/// Sizes are from 1 to 16384 and coordinates from -100000 to 100000. Any
/// other text, a trailing character included, is not a place.
///
/// @param text the preference value
/// @return the place, or nothing when the text is not one
[[nodiscard]] std::optional<SavedWindow> decode_window_place(std::string_view text);

/// Reads the place stored under window_place_key.
///
/// @param values the preferences
/// @return the place, or nothing when the key is absent or its text is not a place
[[nodiscard]] std::optional<SavedWindow>
window_place_in(const std::map<std::string, std::string>& values);

/// Stores a place under window_place_key.
///
/// @param[in,out] values the preferences
/// @param place the place
void put_window_place(std::map<std::string, std::string>& values, const SavedWindow& place);

/// Stores a place only when this run is allowed to remember it.
///
/// A command-line choice for one run passes write false, and the values
/// are then left as they were, the remembered place included.
///
/// @param[in,out] values the preferences
/// @param place the place just observed
/// @param write true to store it
/// @return true when it was stored
bool store_window_place_unless(
    std::map<std::string, std::string>& values, const SavedWindow& place, bool write
);

/// Decides how the next start opens.
///
/// A command-line choice wins for this start. Otherwise a remembered full
/// screen opens full screen on the display it names when that display is
/// still connected, and a remembered window opens at its size and place
/// there. A display that is gone yields the main display, or the first one
/// when none is the main. A window that fits is moved the least that puts
/// it wholly inside the usable area; one larger than that area shrinks to
/// it. With nothing remembered, the platform's first start is kept and the
/// window is not given a place. With no display connected, a remembered
/// window still names its size and is not given a place.
///
/// @param saved the remembered place; null when there is none
/// @param command_line a choice for this run alone
/// @param platform_full_screen the platform's first start (full screen on
///        Windows for a player, a window otherwise)
/// @param displays the displays connected now
/// @return how to open
[[nodiscard]] RestoredPlace restore_window_place(
    const SavedWindow* saved,
    PlaceOverride command_line,
    bool platform_full_screen,
    std::span<const DisplayArea> displays
);

/// A function told when the window's place may have changed.
using WindowPlaceWatcher = void (*)(SDL_Window* window);

/// Sets the function told when the window's place may have changed.
///
/// Null stops the telling. The tests that do not remember a place leave it null.
///
/// @param watcher the function, or null
void set_window_place_watcher(WindowPlaceWatcher watcher);

/// Opens the game's window, at a place when one is given.
///
/// The flags are the ones game_window_flags returns. A place is set only
/// when both x and y are given, so a window with no remembered place is
/// left for the window system to place.
///
/// @param title the window's title
/// @param width the contents' width
/// @param height the contents' height
/// @param flags the window's flags
/// @param x the left, when the window has a place
/// @param y the top, when the window has a place
/// @return the window, or null when it could not be opened, with SDL_GetError saying why
[[nodiscard]] SDL_Window* create_game_window(
    const char* title,
    int width,
    int height,
    SDL_WindowFlags flags,
    std::optional<int> x,
    std::optional<int> y
);

/// Returns the displays connected now.
///
/// @return each display's bounds and usable area; empty when none is connected
[[nodiscard]] std::vector<DisplayArea> connected_displays();

/// Reads the remembered place from a preferences file.
///
/// A missing file and a file without the key are nothing. A file that
/// cannot be read throws std::runtime_error.
///
/// @param file the preferences file
/// @return the place, or nothing when it is not there or its text is not a place
[[nodiscard]] std::optional<SavedWindow> read_window_place_file(const std::filesystem::path& file);

/// Remembers where the game's window will be found next time.
///
/// Does nothing when this run was asked not to remember, when the window
/// is minimised or hidden, or when the place is the one already stored.
/// Full screen keeps the window's last size and place rather than the
/// screen's pixel size. A failure is written to standard error.
///
/// @param window the game's window; null does nothing
void remember_open_window(SDL_Window* window);

/// Writes the window's current place even when this run was asked not to remember.
///
/// The check that starts twice uses this for the line it saves on the first start.
///
/// @param window the game's window
/// @return false when the place could not be read or saved
[[nodiscard]] bool save_window_place(SDL_Window* window);

/// Points later remembers at a preferences file.
///
/// The place already in the file is kept, so a later full screen does not
/// forget the window it was left from. A file that cannot be read is
/// reported on standard error and remembered as empty.
///
/// @param file the preferences file
/// @param enabled true when this run may remember a change
void bind_window_place(const std::filesystem::path& file, bool enabled);

/// Forgets the runtime's preferences, so a later change writes the file itself.
void detach_window_place_values();

/// Runs the window-place check and returns the process status.
///
/// A command-line choice prints that it was kept and writes nothing. With
/// nothing remembered, the window is put at a size that fits the main
/// display and that place is saved. With a place remembered, the window
/// must already be that size and wholly on a connected display.
///
/// @param window the game's window
/// @param preferences the preferences file
/// @param command_line_wins true when -d or --resolution named this run
/// @return 0 when the check passed
[[nodiscard]] int check_saved_window_place(
    SDL_Window* window, const std::filesystem::path& preferences, bool command_line_wins
);

} // namespace oa::app
