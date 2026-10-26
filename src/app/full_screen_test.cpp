// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The flags the game's window opens with, at native density among them, and
// the edge scroll's depth there; Alt+Enter: which keys switch full screen,
// the Enter key it holds until that key comes up, the mode a press asks for
// while the window is still switching (as a macOS full-screen space, an X11
// window manager or a Wayland compositor leaves it for a while), when the
// pointer is kept on the game's screen, the point of a full-screen window a
// pointer past its edge touches on a display with a camera housing, where a
// window that leaves full screen goes on its display, and windows of SDL's
// dummy video driver switched to full screen and back, losing and regaining
// the focus and coming back onto the display, one opened in full screen
// whose first events were dropped, and one opened at native density; the
// window's title bar and borders asked for as the game goes from its menus
// to a game, its game menu and full screen; and the line --trace-input
// writes of the window, its display and the pointer.
#include "oa/app/full_screen.hpp"
#include "oa/ui/hud/camera_scroll.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::app::FullScreenKey;
using oa::app::FullScreenSwitch;

// A press of Return, keypad Enter or A as SDL reports it.
SDL_Event key_down(SDL_Keycode key, SDL_Keymod mod, bool repeat) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.scancode = key == SDLK_KP_ENTER ? SDL_SCANCODE_KP_ENTER
                         : key == SDLK_RETURN ? SDL_SCANCODE_RETURN
                                              : SDL_SCANCODE_A;
    event.key.mod = mod;
    event.key.repeat = repeat;
    event.key.down = true;
    return event;
}

void test_keys() {
    using oa::app::full_screen_key;
    CHECK(full_screen_key(key_down(SDLK_RETURN, SDL_KMOD_LALT, false)) == FullScreenKey::toggle);
    CHECK(full_screen_key(key_down(SDLK_RETURN, SDL_KMOD_RALT, false)) == FullScreenKey::toggle);
    CHECK(full_screen_key(key_down(SDLK_KP_ENTER, SDL_KMOD_LALT, false)) == FullScreenKey::toggle);
    // Alt with Shift or Num Lock held still switches.
    CHECK(
        full_screen_key(key_down(SDLK_RETURN, SDL_KMOD_LALT | SDL_KMOD_LSHIFT, false)) ==
        FullScreenKey::toggle
    );
    CHECK(
        full_screen_key(key_down(SDLK_KP_ENTER, SDL_KMOD_RALT | SDL_KMOD_NUM, false)) ==
        FullScreenKey::toggle
    );
    // The held key's repeats are taken, but switch nothing.
    CHECK(full_screen_key(key_down(SDLK_RETURN, SDL_KMOD_LALT, true)) == FullScreenKey::repeat);
    CHECK(full_screen_key(key_down(SDLK_KP_ENTER, SDL_KMOD_RALT, true)) == FullScreenKey::repeat);
    // Enter alone, Alt with another key, and the release are the screens'.
    CHECK(full_screen_key(key_down(SDLK_RETURN, SDL_KMOD_NONE, false)) == FullScreenKey::none);
    CHECK(full_screen_key(key_down(SDLK_RETURN, SDL_KMOD_LCTRL, false)) == FullScreenKey::none);
    CHECK(full_screen_key(key_down(SDLK_A, SDL_KMOD_LALT, false)) == FullScreenKey::none);
    SDL_Event release = key_down(SDLK_RETURN, SDL_KMOD_LALT, false);
    release.type = SDL_EVENT_KEY_UP;
    release.key.down = false;
    CHECK(full_screen_key(release) == FullScreenKey::none);
}

// A key release as SDL reports it.
SDL_Event key_up(SDL_Keycode key, SDL_Keymod mod) {
    SDL_Event event = key_down(key, mod, false);
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    return event;
}

// Alt+Enter is the chord for the checks that have not read a layout.
FullScreenKey held_key(FullScreenSwitch& full_screen, const SDL_Event& event) {
    return full_screen.take_key(event, oa::app::full_screen_key(event) == FullScreenKey::toggle);
}

// The Enter key of an Alt+Enter stays the switch's until it comes up, with
// Alt held or not.
void test_held_enter() {
    FullScreenSwitch full_screen{};
    CHECK(
        held_key(full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)) == FullScreenKey::toggle
    );
    CHECK(full_screen.held_enter == SDL_SCANCODE_RETURN);
    // Alt let go of first: Enter's repeats and its release are still taken.
    CHECK(
        held_key(full_screen, key_down(SDLK_RETURN, SDL_KMOD_NONE, true)) == FullScreenKey::repeat
    );
    CHECK(
        held_key(full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, true)) == FullScreenKey::repeat
    );
    // Other keys meanwhile are the screens'.
    CHECK(held_key(full_screen, key_down(SDLK_A, SDL_KMOD_NONE, false)) == FullScreenKey::none);
    CHECK(
        held_key(full_screen, key_down(SDLK_KP_ENTER, SDL_KMOD_NONE, true)) == FullScreenKey::none
    );
    CHECK(held_key(full_screen, key_up(SDLK_RETURN, SDL_KMOD_NONE)) == FullScreenKey::release);
    CHECK(full_screen.held_enter == SDL_SCANCODE_UNKNOWN);
    // Once Enter is up, Enter alone, its repeats and its release are the screens'.
    CHECK(
        held_key(full_screen, key_down(SDLK_RETURN, SDL_KMOD_NONE, false)) == FullScreenKey::none
    );
    CHECK(held_key(full_screen, key_down(SDLK_RETURN, SDL_KMOD_NONE, true)) == FullScreenKey::none);
    CHECK(held_key(full_screen, key_up(SDLK_RETURN, SDL_KMOD_NONE)) == FullScreenKey::none);
    // A fresh press without Alt after a lost release lets the key go.
    CHECK(
        held_key(full_screen, key_down(SDLK_KP_ENTER, SDL_KMOD_RALT, false)) ==
        FullScreenKey::toggle
    );
    CHECK(full_screen.held_enter == SDL_SCANCODE_KP_ENTER);
    CHECK(
        held_key(full_screen, key_down(SDLK_KP_ENTER, SDL_KMOD_NONE, false)) == FullScreenKey::none
    );
    CHECK(full_screen.held_enter == SDL_SCANCODE_UNKNOWN);
    CHECK(
        held_key(full_screen, key_down(SDLK_KP_ENTER, SDL_KMOD_NONE, true)) == FullScreenKey::none
    );
}

// A layout's full-screen chord holds its own key. Its repeats and its release
// reach no screen, and Alt+Enter beside it is left for the screens.
void test_rebound_chord() {
    FullScreenSwitch full_screen{};
    SDL_Event down = key_down(SDLK_F6, SDL_KMOD_NONE, false);
    down.key.scancode = SDL_SCANCODE_F6;
    CHECK(full_screen.take_key(down, true) == FullScreenKey::toggle);
    CHECK(full_screen.held_enter == SDL_SCANCODE_F6);
    SDL_Event repeat = down;
    repeat.key.repeat = true;
    CHECK(full_screen.take_key(repeat, true) == FullScreenKey::repeat);
    CHECK(
        full_screen.take_key(key_down(SDLK_RETURN, SDL_KMOD_LALT, false), false) ==
        FullScreenKey::none
    );
    CHECK(full_screen.held_enter == SDL_SCANCODE_F6);
    SDL_Event up = down;
    up.type = SDL_EVENT_KEY_UP;
    up.key.down = false;
    CHECK(full_screen.take_key(up, false) == FullScreenKey::release);
    CHECK(full_screen.held_enter == SDL_SCANCODE_UNKNOWN);
}

void test_window_flags() {
    using oa::app::game_window_flags;
    CHECK(game_window_flags(false, false) == SDL_WINDOW_RESIZABLE);
    CHECK(game_window_flags(true, false) == (SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN));
    // At native density the window holds the display's own pixels.
    CHECK(game_window_flags(false, true) == (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    CHECK(
        game_window_flags(true, true) ==
        (SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY)
    );
    CHECK(oa::app::at_native_density(game_window_flags(false, true)));
    CHECK(oa::app::at_native_density(game_window_flags(true, true) | SDL_WINDOW_INPUT_FOCUS));
    CHECK(!oa::app::at_native_density(game_window_flags(false, false)));
    CHECK(!oa::app::at_native_density(game_window_flags(true, false)));
}

// The band along the screen's edges that scrolls is one window point deep:
// at native density one layout pixel, elsewhere the window's density rounded
// up, as before.
void test_edge_scroll_depth() {
    using oa::app::edge_scroll_depth;
    CHECK(edge_scroll_depth(true, 1.0F) == 1);
    CHECK(edge_scroll_depth(true, 1.5F) == 1);
    CHECK(edge_scroll_depth(true, 2.0F) == 1);
    CHECK(edge_scroll_depth(true, 3.0F) == 1);
    CHECK(edge_scroll_depth(false, 1.0F) == 1);
    CHECK(edge_scroll_depth(false, 1.25F) == 2);
    CHECK(edge_scroll_depth(false, 1.5F) == 2);
    CHECK(edge_scroll_depth(false, 2.0F) == 2);
    CHECK(edge_scroll_depth(false, 3.0F) == 3);
}

// A window system that switches at once: every press switches from the mode
// the window shows.
void test_switch_at_once() {
    FullScreenSwitch full_screen{};
    CHECK(full_screen.next_mode(false, 0));
    full_screen.note_request(true, 0);
    full_screen.note_shown(true);
    CHECK(!full_screen.awaiting_shown);
    CHECK(!full_screen.next_mode(true, 1));
    full_screen.note_request(false, 1);
    full_screen.note_shown(false);
    CHECK(full_screen.next_mode(false, 2));
}

// A window that is still switching reports its old mode: presses in that time
// switch from the mode last asked for.
void test_switch_while_changing() {
    FullScreenSwitch full_screen{};
    constexpr uint64_t pressed_ms = 10000;
    // The first press asks for full screen; the window still reports a window.
    CHECK(full_screen.next_mode(false, pressed_ms));
    full_screen.note_request(true, pressed_ms);
    // A second press during the change asks for a window, not full screen
    // again, though the window still reports a window.
    CHECK(!full_screen.next_mode(false, pressed_ms + 100));
    full_screen.note_request(false, pressed_ms + 100);
    // The window reaches full screen on the way back to a window: the wait
    // goes on, and a third press asks for full screen again.
    full_screen.note_shown(true);
    CHECK(full_screen.awaiting_shown);
    CHECK(full_screen.next_mode(true, pressed_ms + 300));
    // Leaving full screen then ends the wait for the window asked for.
    full_screen.note_shown(false);
    CHECK(!full_screen.awaiting_shown);
    CHECK(full_screen.next_mode(false, pressed_ms + 400));
}

// A request the window never answers (a window manager without full screen)
// is forgotten after full_screen_settle_ms, and the window's own report
// counts again.
void test_unanswered_request() {
    FullScreenSwitch full_screen{};
    full_screen.note_request(true, 0);
    CHECK(!full_screen.next_mode(false, oa::app::full_screen_settle_ms - 1));
    CHECK(full_screen.next_mode(false, oa::app::full_screen_settle_ms));
}

// The player's own switch outside the game (the green button or Control+
// Command+F on macOS, a window manager's key) is followed.
void test_switch_outside_the_game() {
    FullScreenSwitch full_screen{};
    full_screen.note_request(true, 0);
    full_screen.note_shown(true);
    full_screen.note_shown(false);
    CHECK(full_screen.next_mode(false, 1));
}

// Tells whether two rectangles have the same place and size.
bool same_rect(const SDL_Rect& a, const SDL_Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// A window's frame placed on a display's usable area: each side outside moves
// 5% of the area's width or height inside it, and a window too large for the
// room between the margins keeps the left and top margins.
void test_window_on_display() {
    using oa::app::window_on_display;
    // A 1920x1080 display whose usable area is all of it: margins of 96 and
    // 54 pixels.
    constexpr SDL_Rect display{0, 0, 1920, 1080};
    const auto placed = [&](SDL_Rect window) { return window_on_display(window, display, true); };

    // Inside, and exactly on every edge, the window stays where it is.
    CHECK(same_rect(placed({100, 100, 640, 480}), {100, 100, 640, 480}));
    CHECK(same_rect(placed({0, 0, 640, 480}), {0, 0, 640, 480}));
    CHECK(same_rect(placed({1280, 600, 640, 480}), {1280, 600, 640, 480}));
    CHECK(same_rect(placed({0, 0, 1920, 1080}), {0, 0, 1920, 1080}));

    // One pixel past one edge: that side moves 5% inside, the other axis stays.
    CHECK(same_rect(placed({-1, 100, 640, 480}), {96, 100, 640, 480}));
    CHECK(same_rect(placed({1281, 100, 640, 480}), {1184, 100, 640, 480}));
    CHECK(same_rect(placed({100, -1, 640, 480}), {100, 54, 640, 480}));
    CHECK(same_rect(placed({100, 601, 640, 480}), {100, 546, 640, 480}));

    // Out at a corner: both sides move in.
    CHECK(same_rect(placed({-300, -200, 640, 480}), {96, 54, 640, 480}));
    CHECK(same_rect(placed({1700, 900, 640, 480}), {1184, 546, 640, 480}));
    CHECK(same_rect(placed({-300, 900, 640, 480}), {96, 546, 640, 480}));
    CHECK(same_rect(placed({1700, -200, 640, 480}), {1184, 54, 640, 480}));
    // Entirely off the display.
    CHECK(same_rect(placed({5000, -4000, 640, 480}), {1184, 54, 640, 480}));

    // Too wide for the room between the margins (1728): the left margin is
    // kept and the width shrinks to the room; the height is placed as usual.
    CHECK(same_rect(placed({-10, 100, 2000, 480}), {96, 100, 1728, 480}));
    CHECK(same_rect(placed({30, 700, 1900, 480}), {96, 546, 1728, 480}));
    // Too tall (room 972), and too large both ways.
    CHECK(same_rect(placed({100, 50, 640, 1080}), {100, 54, 640, 972}));
    CHECK(same_rect(placed({-50, -50, 2500, 1500}), {96, 54, 1728, 972}));
    // A window that fits the area but not between the margins, with no side
    // outside, stays.
    CHECK(same_rect(placed({10, 10, 1900, 1060}), {10, 10, 1900, 1060}));
    // A window that may not be resized keeps its size and the left and top
    // margins only.
    CHECK(
        same_rect(window_on_display({-10, 100, 2000, 480}, display, false), {96, 100, 2000, 480})
    );
    CHECK(same_rect(window_on_display({30, 700, 1900, 480}, display, false), {96, 546, 1900, 480}));
    CHECK(
        same_rect(window_on_display({-50, -50, 2500, 1500}, display, false), {96, 54, 2500, 1500})
    );

    // A usable area without a menu bar and a dock: the margins are 5% of
    // the area (72 and 43 pixels), measured from its edges.
    constexpr SDL_Rect desk{0, 25, 1440, 875};
    CHECK(same_rect(window_on_display({100, 10, 800, 600}, desk, true), {100, 68, 800, 600}));
    CHECK(same_rect(window_on_display({100, 400, 800, 600}, desk, true), {100, 257, 800, 600}));
    CHECK(same_rect(window_on_display({100, 25, 800, 600}, desk, true), {100, 25, 800, 600}));

    // Displays left of and above the primary display have negative
    // coordinates.
    constexpr SDL_Rect left_display{-1920, 0, 1920, 1040};
    CHECK(same_rect(
        window_on_display({-2000, 100, 800, 600}, left_display, true), {-1824, 100, 800, 600}
    ));
    CHECK(same_rect(
        window_on_display({-1000, 100, 1200, 600}, left_display, true), {-1296, 100, 1200, 600}
    ));
    CHECK(same_rect(
        window_on_display({-1920, 0, 1920, 1040}, left_display, true), {-1920, 0, 1920, 1040}
    ));
    CHECK(same_rect(
        window_on_display({-2500, -100, 2400, 600}, left_display, true), {-1824, 52, 1728, 600}
    ));
    constexpr SDL_Rect upper_display{-500, -1080, 1920, 1040};
    CHECK(same_rect(
        window_on_display({100, -1200, 800, 600}, upper_display, true), {100, -1028, 800, 600}
    ));
    CHECK(same_rect(
        window_on_display({100, -500, 800, 600}, upper_display, true), {100, -692, 800, 600}
    ));
    CHECK(same_rect(
        window_on_display({-600, -1100, 800, 1200}, upper_display, true), {-404, -1028, 800, 936}
    ));

    // A display too small for a margin keeps the window on its edge.
    CHECK(same_rect(window_on_display({-5, -5, 4, 4}, {0, 0, 10, 10}, true), {0, 0, 4, 4}));
    CHECK(same_rect(window_on_display({-5, -5, 40, 40}, {0, 0, 10, 10}, true), {0, 0, 10, 10}));

    // A placed window is on the display: placing it again leaves it there.
    for (const SDL_Rect window : {
             SDL_Rect{-300, -200, 640, 480},
             SDL_Rect{1700, 900, 640, 480},
             SDL_Rect{-50, -50, 2500, 1500},
             SDL_Rect{30, 700, 1900, 480},
         }) {
        const SDL_Rect once = placed(window);
        CHECK(same_rect(placed(once), once));
        CHECK(once.x >= display.x && once.y >= display.y);
        CHECK(once.x + once.w <= display.x + display.w && once.y + once.h <= display.y + display.h);
    }
}

// When the window's place as a window is checked: from leaving full screen,
// or the request to, for full_screen_settle_ms; not on the way back to a full
// screen asked for, and no longer once it is full screen again.
void test_window_place_checked() {
    FullScreenSwitch full_screen{};
    CHECK(!full_screen.places_window(0));
    // Asking for full screen checks nothing.
    full_screen.note_request(true, 0);
    full_screen.note_shown(true);
    CHECK(!full_screen.places_window(1));
    // Asking for a window checks its place from then on.
    constexpr uint64_t asked_ms = 10000;
    full_screen.note_request(false, asked_ms);
    CHECK(full_screen.places_window(asked_ms));
    // Leaving full screen later starts the time again.
    full_screen.note_shown(false);
    full_screen.note_left(asked_ms + 500);
    CHECK(full_screen.places_window(asked_ms + 500 + oa::app::full_screen_settle_ms - 1));
    CHECK(!full_screen.places_window(asked_ms + 500 + oa::app::full_screen_settle_ms));
    // Settled, it is not checked again.
    full_screen.placing_window = false;
    CHECK(!full_screen.places_window(asked_ms + 600));

    // Leaving full screen from outside the game (the green button, a window
    // manager's key) checks the window's place too.
    FullScreenSwitch outside{};
    outside.note_shown(true);
    outside.note_shown(false);
    outside.note_left(5);
    CHECK(outside.places_window(6));
    // Entering full screen again ends the check.
    outside.note_shown(true);
    CHECK(!outside.places_window(7));

    // A window that leaves full screen on its way back to the full screen a
    // later press asked for is not placed.
    FullScreenSwitch back_and_forth{};
    back_and_forth.note_request(true, 0);
    back_and_forth.note_shown(true);
    back_and_forth.note_request(false, 100);
    back_and_forth.note_request(true, 200);
    CHECK(!back_and_forth.places_window(200));
    back_and_forth.note_shown(false);
    back_and_forth.note_left(300);
    CHECK(!back_and_forth.places_window(300));
}

// Full screen with the focus keeps the pointer on the window; losing the
// focus, or a window, leaves it free.
void test_pointer_bounds() {
    using oa::app::keeps_pointer_on_screen;
    constexpr SDL_WindowFlags focused = SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    CHECK(keeps_pointer_on_screen(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS));
    CHECK(keeps_pointer_on_screen(oa::app::game_window_flags(true, false) | focused));
    // Already held, it stays held.
    CHECK(keeps_pointer_on_screen(SDL_WINDOW_FULLSCREEN | focused | SDL_WINDOW_MOUSE_GRABBED));
    // Switched away from (Alt+Tab, Command+Tab, a dialog of the system's).
    CHECK(!keeps_pointer_on_screen(SDL_WINDOW_FULLSCREEN));
    CHECK(!keeps_pointer_on_screen(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MOUSE_FOCUS));
    CHECK(!keeps_pointer_on_screen(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED));
    // A window, focused or not, held before or not.
    CHECK(!keeps_pointer_on_screen(oa::app::game_window_flags(false, false) | focused));
    CHECK(!keeps_pointer_on_screen(focused | SDL_WINDOW_MOUSE_GRABBED));
    CHECK(!keeps_pointer_on_screen(0));
}

// The window events after which the pointer's bounds are settled again.
void test_pointer_bounds_events() {
    const auto window_event = [](SDL_EventType type) {
        SDL_Event event{};
        event.type = type;
        return event;
    };
    constexpr std::array<SDL_EventType, 11> changing{
        SDL_EVENT_WINDOW_ENTER_FULLSCREEN,
        SDL_EVENT_WINDOW_LEAVE_FULLSCREEN,
        SDL_EVENT_WINDOW_RESIZED,
        SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
        SDL_EVENT_WINDOW_DISPLAY_CHANGED,
        SDL_EVENT_WINDOW_SHOWN,
        SDL_EVENT_WINDOW_HIDDEN,
        SDL_EVENT_WINDOW_MINIMIZED,
        SDL_EVENT_WINDOW_RESTORED,
        SDL_EVENT_WINDOW_FOCUS_GAINED,
        SDL_EVENT_WINDOW_FOCUS_LOST,
    };
    constexpr std::array<SDL_EventType, 6> unchanging{
        SDL_EVENT_WINDOW_EXPOSED,
        SDL_EVENT_WINDOW_MOVED,
        SDL_EVENT_WINDOW_MOUSE_ENTER,
        SDL_EVENT_WINDOW_MOUSE_LEAVE,
        SDL_EVENT_MOUSE_MOTION,
        SDL_EVENT_KEY_DOWN,
    };
    for (const auto type : changing)
        CHECK(oa::app::changes_pointer_bounds(window_event(type)));
    for (const auto type : unchanging)
        CHECK(!oa::app::changes_pointer_bounds(window_event(type)));
}

// The line --trace-input writes of the window: a window lying exactly over
// its display, one whose top rows lie above it, and the pointer over the
// window, outside it on its display and on another display.
void test_window_trace() {
    using oa::app::PointerPlace;
    using oa::app::pointer_place;
    using oa::app::window_edges;
    constexpr SDL_Rect display{0, 0, 1920, 1080};
    CHECK(window_edges({0, 0, 1920, 1080}, display).covers());
    // Eight rows above the display's top: the pointer cannot reach the
    // window's top row, though the bottom eight rows are free of it.
    const auto above = window_edges({0, -8, 1920, 1080}, display);
    CHECK(!above.covers());
    CHECK(above.left == 0 && above.top == -8 && above.right == 0 && above.bottom == 8);
    // A window on a second display left of and above the first.
    const auto beside = window_edges({-1500, -200, 1280, 720}, {-1600, -300, 1600, 900});
    CHECK(beside.left == 100 && beside.top == 100 && beside.right == 220 && beside.bottom == 80);
    // The window holds its left and top edges' points, not its right and
    // bottom edges'.
    constexpr SDL_Rect windowed{100, 50, 800, 600};
    CHECK(pointer_place(100.0F, 50.0F, windowed, display) == PointerPlace::window);
    CHECK(pointer_place(899.5F, 649.5F, windowed, display) == PointerPlace::window);
    CHECK(pointer_place(900.0F, 300.0F, windowed, display) == PointerPlace::display);
    CHECK(pointer_place(500.0F, 0.0F, windowed, display) == PointerPlace::display);
    CHECK(pointer_place(500.0F, -1.0F, windowed, display) == PointerPlace::elsewhere);
    CHECK(pointer_place(1920.0F, 500.0F, windowed, display) == PointerPlace::elsewhere);

    oa::app::WindowTrace trace{};
    trace.reason = "leave";
    trace.window = display;
    trace.pixel_width = 1920;
    trace.pixel_height = 1080;
    trace.display = 1;
    trace.bounds = display;
    trace.usable = {0, 0, 1920, 1040};
    trace.scale = 1.25F;
    trace.flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_GRABBED;
    trace.pointer_x = 960.0F;
    trace.pointer_y = 0.0F;
    CHECK(
        oa::app::window_trace_line(trace) ==
        "input window leave window=0,0,1920x1080 pixels=1920x1080 display=1 "
        "bounds=0,0,1920x1080 usable=0,0,1920x1040 scale=1.25 edges=covers "
        "flags=full-screen,input-focus,grabbed keep=yes mouse-rect=none pointer=960,0 "
        "pointer-on=window"
    );
    // On a display mode of its own, eight rows above the display, the
    // pointer on a display above.
    trace.reason = "full-screen-entered";
    trace.window = {0, -8, 1920, 1080};
    trace.exclusive = true;
    trace.flags |= SDL_WINDOW_MOUSE_FOCUS;
    trace.pointer_y = -20.0F;
    CHECK(
        oa::app::window_trace_line(trace) ==
        "input window full-screen-entered window=0,-8,1920x1080 pixels=1920x1080 display=1 "
        "bounds=0,0,1920x1080 usable=0,0,1920x1040 scale=1.25 "
        "edges=left:0,top:-8,right:0,bottom:8 "
        "flags=full-screen,exclusive,input-focus,mouse-focus,grabbed keep=yes "
        "mouse-rect=none pointer=960,-20 pointer-on=elsewhere"
    );
    // A borderless window, without the focus, held in a part of itself; a
    // display mode asked for counts only in full screen.
    trace.reason = "focus-lost";
    trace.window = windowed;
    trace.flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE;
    trace.mouse_rect = SDL_Rect{0, 0, 800, 600};
    trace.scale = 1.0F;
    trace.pointer_x = 1000.0F;
    trace.pointer_y = 10.5F;
    CHECK(
        oa::app::window_trace_line(trace) ==
        "input window focus-lost window=100,50,800x600 pixels=1920x1080 display=1 "
        "bounds=0,0,1920x1080 usable=0,0,1920x1040 scale=1 "
        "edges=left:100,top:50,right:1020,bottom:430 flags=borderless keep=no "
        "mouse-rect=0,0,800x600 pointer=1000,10.5 pointer-on=display"
    );
    trace.flags = 0;
    CHECK(oa::app::window_trace_line(trace).find(" flags=none ") != std::string::npos);

    // The events traced, and others not.
    const auto event_of = [](SDL_EventType type) {
        SDL_Event event{};
        event.type = type;
        return event;
    };
    CHECK(oa::app::traced_window_event(event_of(SDL_EVENT_WINDOW_MOUSE_LEAVE)) == "leave");
    CHECK(oa::app::traced_window_event(event_of(SDL_EVENT_WINDOW_MOUSE_ENTER)) == "enter");
    CHECK(oa::app::traced_window_event(event_of(SDL_EVENT_WINDOW_FOCUS_GAINED)) == "focus-gained");
    CHECK(
        oa::app::traced_window_event(event_of(SDL_EVENT_WINDOW_ENTER_FULLSCREEN)) ==
        "full-screen-entered"
    );
    CHECK(
        oa::app::traced_window_event(event_of(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)) ==
        "scale-changed"
    );
    CHECK(
        oa::app::traced_window_event(event_of(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)) ==
        "display-mode-changed"
    );
    CHECK(oa::app::traced_window_event(event_of(SDL_EVENT_WINDOW_EXPOSED)).empty());
    CHECK(oa::app::traced_window_event(event_of(SDL_EVENT_MOUSE_MOTION)).empty());
    CHECK(oa::app::traced_window_event(event_of(SDL_EVENT_KEY_DOWN)).empty());
}

// Pumps the window's events through the switch, as the game's loop does.
void pump(FullScreenSwitch& full_screen, bool& entered, bool& left) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        entered = entered || event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        left = left || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN;
        CHECK(!oa::app::take_full_screen_event(nullptr, full_screen, event));
    }
}

bool shows_full_screen(SDL_Window* window) {
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

// Pumps the game window's events through the switch, which keeps the
// pointer on the window or lets it go, as the game's loop does.
void pump_window(SDL_Window* window, FullScreenSwitch& full_screen) {
    (void)SDL_SyncWindow(window);
    SDL_Event event{};
    while (SDL_PollEvent(&event))
        (void)oa::app::take_full_screen_event(window, full_screen, event);
}

bool has_focus(SDL_Window* window) {
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

// A window of SDL's video driver (the dummy driver under ctest) keeps the
// pointer in full screen while it has the focus: another window taking the
// focus lets it go, regaining the focus holds it again, a window lets it go,
// and full screen entered again holds it again.
void test_pointer_window() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++failures;
        return;
    }
    SDL_Window* window =
        SDL_CreateWindow("pointer test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window == nullptr) {
        SDL_Quit();
        return;
    }
    FullScreenSwitch full_screen{};
    pump_window(window, full_screen);
    CHECK(has_focus(window));
    CHECK(!SDL_GetWindowMouseGrab(window));

    const auto alt_enter = [&] {
        CHECK(
            oa::app::take_full_screen_event(
                window, full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)
            )
        );
        CHECK(
            oa::app::take_full_screen_event(window, full_screen, key_up(SDLK_RETURN, SDL_KMOD_LALT))
        );
        pump_window(window, full_screen);
    };
    alt_enter();
    CHECK(shows_full_screen(window));
    CHECK(SDL_GetWindowMouseGrab(window));

    // Another window takes the focus, as another program switched to does.
    SDL_Window* other = SDL_CreateWindow("other program", 320, 240, 0);
    CHECK(other != nullptr);
    pump_window(window, full_screen);
    CHECK(!has_focus(window));
    CHECK(shows_full_screen(window));
    CHECK(!SDL_GetWindowMouseGrab(window));
    CHECK((SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_GRABBED) == 0);
    if (other != nullptr)
        SDL_DestroyWindow(other);

    // The game's window takes the focus back: the dummy driver gives it to a
    // window as it is shown.
    CHECK(SDL_HideWindow(window));
    pump_window(window, full_screen);
    CHECK(SDL_ShowWindow(window));
    pump_window(window, full_screen);
    CHECK(has_focus(window));
    CHECK(shows_full_screen(window));
    CHECK(SDL_GetWindowMouseGrab(window));

    // A window lets the pointer go; full screen again holds it again.
    alt_enter();
    CHECK(!shows_full_screen(window));
    CHECK(!SDL_GetWindowMouseGrab(window));
    alt_enter();
    CHECK(shows_full_screen(window));
    CHECK(SDL_GetWindowMouseGrab(window));

    // Closing lets it go.
    oa::app::release_pointer(window);
    CHECK(!SDL_GetWindowMouseGrab(window));
    oa::app::keep_pointer_on_screen(window);
    CHECK(SDL_GetWindowMouseGrab(window));
    oa::app::release_pointer(window);
    SDL_DestroyWindow(window);

    // A window created full screen holds the pointer from its first events.
    window = SDL_CreateWindow("pointer test", 640, 480, oa::app::game_window_flags(true, false));
    CHECK(window != nullptr);
    if (window != nullptr) {
        FullScreenSwitch started{};
        pump_window(window, started);
        CHECK(shows_full_screen(window));
        CHECK(SDL_GetWindowMouseGrab(window));
        oa::app::release_pointer(window);
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

// The built-in display of a 16-inch MacBook Pro, 1728 by 1117 points, and
// the full-screen window macOS places below its camera housing: 1728 by
// 1080 points, 37 points down, the strip beside the housing above it.
constexpr SDL_Rect notched_display{0, 0, 1728, 1117};
constexpr SDL_Rect below_housing{0, 37, 1728, 1080};

// Where the pointer comes to rest when the player pushes it toward a point
// of the notched display: on the game's window while the window holds it
// (SDL's grab), and anywhere on the display otherwise.
SDL_FPoint pushed_on_notched_display(SDL_WindowFlags flags, float x, float y) {
    const SDL_Rect& held =
        (flags & SDL_WINDOW_MOUSE_GRABBED) != 0 ? below_housing : notched_display;
    return {
        std::clamp(x, static_cast<float>(held.x), static_cast<float>(held.x + held.w - 1)),
        std::clamp(y, static_cast<float>(held.y), static_cast<float>(held.y + held.h - 1))
    };
}

// The way the screen's edges scroll the view for a pointer pushed toward a
// point of the notched display, with the window's flags, at one window
// point to a layout pixel: the point of the window the pointer touches, on
// the window or past its edges (full_screen_pointer), as the match's edge
// test takes it.
oa::ui::hud::EdgeScroll notched_edge_way(SDL_WindowFlags flags, float x, float y) {
    const SDL_FPoint rest = pushed_on_notched_display(flags, x, y);
    const std::optional<SDL_FPoint> touched =
        oa::app::full_screen_pointer(flags, below_housing, notched_display, rest.x, rest.y);
    if (!touched)
        return {};
    return oa::ui::hud::edge_scroll(
        static_cast<int32_t>(std::floor(touched->x)),
        static_cast<int32_t>(std::floor(touched->y)),
        below_housing.w,
        below_housing.h,
        1
    );
}

bool scrolls(const oa::ui::hud::EdgeScroll& way, int32_t x, int32_t y) {
    return way.x == x && way.y == y;
}

// The point of a full-screen window the pointer touches: on the window
// where it is, past an edge on the window's display on that edge's last row
// or column, and nowhere off the display, in a window, or without the
// focus.
void test_full_screen_pointer() {
    using oa::app::full_screen_pointer;
    constexpr SDL_WindowFlags held = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS;
    const auto at = [](const std::optional<SDL_FPoint>& point, float x, float y) {
        return point && point->x == x && point->y == y;
    };
    // On the window, in the window's coordinates.
    CHECK(at(full_screen_pointer(held, below_housing, notched_display, 864.5F, 37.0F), 864.5F, 0));
    CHECK(
        at(full_screen_pointer(held, below_housing, notched_display, 1727.5F, 1116.5F),
           1727.5F,
           1079.5F)
    );
    // In the strip beside the camera housing: the window's top row.
    CHECK(at(full_screen_pointer(held, below_housing, notched_display, 864.0F, 10.0F), 864.0F, 0));
    CHECK(at(full_screen_pointer(held, below_housing, notched_display, 0.0F, 0.0F), 0, 0));
    CHECK(at(full_screen_pointer(held, below_housing, notched_display, 1727.9F, 36.9F), 1727, 0));
    // Past the right and bottom edges of a window short of its display's.
    constexpr SDL_Rect inset{100, 50, 800, 600};
    constexpr SDL_Rect display{0, 0, 1024, 768};
    CHECK(at(full_screen_pointer(held, inset, display, 950.0F, 700.0F), 799, 599));
    CHECK(at(full_screen_pointer(held, inset, display, 20.0F, 300.0F), 0, 250));
    // Off the window's display, and an empty window.
    CHECK(!full_screen_pointer(held, below_housing, notched_display, 864.0F, -1.0F));
    CHECK(!full_screen_pointer(held, below_housing, notched_display, 1728.0F, 500.0F));
    CHECK(!full_screen_pointer(held, {0, 37, 0, 0}, notched_display, 0.0F, 0.0F));
    // A window, and full screen without the focus, leave the pointer free.
    CHECK(!full_screen_pointer(SDL_WINDOW_INPUT_FOCUS, below_housing, notched_display, 864, 10));
    CHECK(!full_screen_pointer(SDL_WINDOW_FULLSCREEN, below_housing, notched_display, 864, 10));
    CHECK(!full_screen_pointer(SDL_WINDOW_INPUT_FOCUS, inset, display, 500.0F, 300.0F));
}

// The edges of the notched display scroll the view toward each of them for
// a full-screen window with these flags, the top edge from the strip beside
// the camera housing among them, and the middle of the screen does not.
void check_notched_edges(SDL_WindowFlags flags) {
    CHECK(scrolls(notched_edge_way(flags, 864.0F, 10.0F), 0, -1));
    CHECK(scrolls(notched_edge_way(flags, 864.0F, 0.0F), 0, -1));
    CHECK(scrolls(notched_edge_way(flags, 864.0F, 37.0F), 0, -1));
    CHECK(scrolls(notched_edge_way(flags, 864.0F, 1116.0F), 0, 1));
    CHECK(scrolls(notched_edge_way(flags, -40.0F, 500.0F), -1, 0));
    CHECK(scrolls(notched_edge_way(flags, 1800.0F, 500.0F), 1, 0));
    CHECK(scrolls(notched_edge_way(flags, -5.0F, 5.0F), -1, -1));
    CHECK(scrolls(notched_edge_way(flags, 864.0F, 500.0F), 0, 0));
    CHECK(scrolls(notched_edge_way(flags, 864.0F, 38.0F), 0, 0));
}

// The game's window opened in full screen, whose first events the progress
// dialog drops while the game's archives load at start (drop_window_events),
// holds the pointer as a window that Alt+Enter switched to full screen does,
// and on the notched display both scroll the view toward every edge the
// pointer is pushed against, the top edge from the strip beside the camera
// housing among them. A close among the dropped events is put back.
void test_launch_holds_pointer() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++failures;
        return;
    }
    SDL_Window* window =
        SDL_CreateWindow("launch test", 640, 480, oa::app::game_window_flags(true, false));
    CHECK(window != nullptr);
    if (window == nullptr) {
        SDL_Quit();
        return;
    }
    (void)SDL_SyncWindow(window);
    CHECK(!oa::app::drop_window_events(window));
    CHECK(shows_full_screen(window));
    CHECK(has_focus(window));
    CHECK(SDL_GetWindowMouseGrab(window));
    const SDL_WindowFlags launched = SDL_GetWindowFlags(window);
    // A close is kept for the screen that answers it.
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    CHECK(SDL_PushEvent(&quit));
    CHECK(oa::app::drop_window_events(window));
    CHECK(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) == 1);
    SDL_FlushEvent(SDL_EVENT_QUIT);
    oa::app::release_pointer(window);
    SDL_DestroyWindow(window);

    window = SDL_CreateWindow("toggle test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window == nullptr) {
        SDL_Quit();
        return;
    }
    FullScreenSwitch full_screen{};
    pump_window(window, full_screen);
    CHECK(
        oa::app::take_full_screen_event(
            window, full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)
        )
    );
    CHECK(oa::app::take_full_screen_event(window, full_screen, key_up(SDLK_RETURN, SDL_KMOD_LALT)));
    pump_window(window, full_screen);
    CHECK(shows_full_screen(window));
    CHECK(SDL_GetWindowMouseGrab(window));
    const SDL_WindowFlags toggled = SDL_GetWindowFlags(window);
    oa::app::release_pointer(window);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // The grab is what the two ways share; a pointer macOS lets into the
    // strip, held or not, still touches the top edge.
    CHECK((launched & SDL_WINDOW_MOUSE_GRABBED) == (toggled & SDL_WINDOW_MOUSE_GRABBED));
    check_notched_edges(launched);
    check_notched_edges(toggled);
    check_notched_edges(launched & ~SDL_WINDOW_MOUSE_GRABBED);
    // Without the focus the pointer is free and touches no edge from the strip.
    CHECK(scrolls(notched_edge_way(launched & ~SDL_WINDOW_INPUT_FOCUS, 864.0F, 10.0F), 0, 0));
}

// Where a window of SDL's video driver is, and its size, in the desktop's
// coordinates.
SDL_Rect window_rect(SDL_Window* window) {
    SDL_Rect rect{};
    CHECK(SDL_GetWindowPosition(window, &rect.x, &rect.y));
    CHECK(SDL_GetWindowSize(window, &rect.w, &rect.h));
    return rect;
}

// A window of SDL's dummy video driver, on its one 1024x768 display, that
// leaves full screen by Alt+Enter comes back onto the display, 5% inside it
// (51 and 38 pixels) on each side that was outside, and shrinks when it is
// too large. A window the player moves off the display stays there. Other
// video drivers place windows in their own time, so there the test only
// switches.
void test_window_back_on_display() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++failures;
        return;
    }
    SDL_Window* window =
        SDL_CreateWindow("window place test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window == nullptr) {
        SDL_Quit();
        return;
    }
    SDL_Rect display{};
    CHECK(SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &display));
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr || std::string_view(driver) != "dummy" ||
        !same_rect(display, {0, 0, 1024, 768})) {
        std::puts("full screen: the window's place is checked only on SDL's dummy video driver");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }
    FullScreenSwitch full_screen{};
    pump_window(window, full_screen);
    const auto alt_enter = [&] {
        CHECK(
            oa::app::take_full_screen_event(
                window, full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)
            )
        );
        CHECK(
            oa::app::take_full_screen_event(window, full_screen, key_up(SDLK_RETURN, SDL_KMOD_LALT))
        );
        pump_window(window, full_screen);
    };
    // Full screen and back to a window.
    const auto switch_twice = [&] {
        alt_enter();
        CHECK(shows_full_screen(window));
        CHECK(full_screen.full_screen_display == SDL_GetDisplayForWindow(window));
        alt_enter();
        CHECK(!shows_full_screen(window));
        CHECK(!full_screen.placing_window);
    };
    const auto place = [&](int x, int y) {
        CHECK(SDL_SetWindowPosition(window, x, y));
        pump_window(window, full_screen);
    };

    // A window on the display comes back where it was.
    place(100, 100);
    switch_twice();
    CHECK(same_rect(window_rect(window), {100, 100, 640, 480}));
    place(0, 0);
    switch_twice();
    CHECK(same_rect(window_rect(window), {0, 0, 640, 480}));

    // The player moves the window partly off the display, and it stays there.
    place(700, 500);
    CHECK(same_rect(window_rect(window), {700, 500, 640, 480}));
    // Out at the right and bottom, it comes back inside them.
    switch_twice();
    CHECK(same_rect(window_rect(window), {333, 250, 640, 480}));
    // Out at the left and top.
    place(-200, -100);
    CHECK(same_rect(window_rect(window), {-200, -100, 640, 480}));
    switch_twice();
    CHECK(same_rect(window_rect(window), {51, 38, 640, 480}));
    // Out at the left only.
    place(-1, 100);
    switch_twice();
    CHECK(same_rect(window_rect(window), {51, 100, 640, 480}));

    // A usable area without a menu bar of 25 pixels and a dock of 43: the
    // margins are 51 and 35 pixels, measured from its edges.
    CHECK(SDL_SetHint(SDL_HINT_DISPLAY_USABLE_BOUNDS, "0,25,1024,700"));
    place(-100, 10);
    switch_twice();
    CHECK(same_rect(window_rect(window), {51, 60, 640, 480}));
    place(300, 260);
    switch_twice();
    CHECK(same_rect(window_rect(window), {300, 210, 640, 480}));
    CHECK(SDL_ResetHint(SDL_HINT_DISPLAY_USABLE_BOUNDS));

    // Too large for the display: shrunk to the room between the margins,
    // 922x692, and laid out at that size in pixels.
    CHECK(SDL_SetWindowSize(window, 1200, 700));
    place(-50, 100);
    switch_twice();
    CHECK(same_rect(window_rect(window), {51, 38, 922, 692}));
    int width = 0;
    int height = 0;
    CHECK(SDL_GetWindowSizeInPixels(window, &width, &height));
    CHECK(width == 922 && height == 692);
    // Moved off the display again as a window, it stays off it.
    place(500, 400);
    CHECK(same_rect(window_rect(window), {500, 400, 922, 692}));

    SDL_DestroyWindow(window);

    // A window that may not be resized keeps its size and the left margin.
    window = SDL_CreateWindow("window place test", 1100, 500, 0);
    CHECK(window != nullptr);
    if (window != nullptr) {
        full_screen = FullScreenSwitch{};
        pump_window(window, full_screen);
        place(-30, 10);
        switch_twice();
        CHECK(same_rect(window_rect(window), {51, 10, 1100, 500}));
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

// A window that keeps its size moves no more than it must to lie on the
// display, and one larger than the usable area starts at its corner.
void test_window_at_size_on_display() {
    const SDL_Rect usable{0, 25, 1024, 700};
    CHECK(same_rect(
        oa::app::window_at_size_on_display({100, 100, 640, 480}, usable), {100, 100, 640, 480}
    ));
    CHECK(same_rect(
        oa::app::window_at_size_on_display({600, 400, 640, 480}, usable), {384, 245, 640, 480}
    ));
    CHECK(
        same_rect(oa::app::window_at_size_on_display({-30, 0, 640, 480}, usable), {0, 25, 640, 480})
    );
    // As large as the area on one axis, larger on the other.
    CHECK(same_rect(
        oa::app::window_at_size_on_display({200, 100, 1024, 800}, usable), {0, 25, 1024, 800}
    ));
    // A second display left of the first.
    CHECK(same_rect(
        oa::app::window_at_size_on_display({-1900, 50, 1280, 720}, {-1920, 0, 1920, 1080}),
        {-1900, 50, 1280, 720}
    ));
    CHECK(same_rect(
        oa::app::window_at_size_on_display({-1000, 50, 1280, 720}, {-1920, 0, 1920, 1080}),
        {-1280, 50, 1280, 720}
    ));
}

// A screen size chosen on SDL's dummy video driver: a window asked for a
// size keeps it on its display, and a size asked for in full screen is the
// window's once it leaves full screen, kept where it was placed.
void test_window_takes_a_size() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++failures;
        return;
    }
    SDL_Window* window =
        SDL_CreateWindow("window size test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window == nullptr) {
        SDL_Quit();
        return;
    }
    SDL_Rect display{};
    CHECK(SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &display));
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr || std::string_view(driver) != "dummy" ||
        !same_rect(display, {0, 0, 1024, 768})) {
        std::puts("full screen: a window's size is checked only on SDL's dummy video driver");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }
    FullScreenSwitch full_screen{};
    pump_window(window, full_screen);
    // Sized in a window where it lies: it stays where it is.
    CHECK(SDL_SetWindowPosition(window, 100, 100));
    CHECK(SDL_SetWindowSize(window, 800, 600));
    oa::app::keep_window_on_display(window, 800, 600);
    pump_window(window, full_screen);
    CHECK(same_rect(window_rect(window), {100, 100, 800, 600}));
    // Sized past the display's edge: moved back onto it, at the size.
    CHECK(SDL_SetWindowSize(window, 1024, 600));
    oa::app::keep_window_on_display(window, 1024, 600);
    pump_window(window, full_screen);
    CHECK(same_rect(window_rect(window), {0, 100, 1024, 600}));

    // A size asked for in full screen is the window's once it leaves.
    const auto alt_enter = [&] {
        CHECK(
            oa::app::take_full_screen_event(
                window, full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)
            )
        );
        CHECK(
            oa::app::take_full_screen_event(window, full_screen, key_up(SDLK_RETURN, SDL_KMOD_LALT))
        );
        pump_window(window, full_screen);
    };
    alt_enter();
    CHECK(shows_full_screen(window));
    full_screen.window_width = 1280;
    full_screen.window_height = 720;
    alt_enter();
    CHECK(!shows_full_screen(window));
    CHECK(!full_screen.placing_window);
    // Wider than the display: kept at its size from the display's left edge,
    // and moved up no more than it must to end on its last row.
    CHECK(same_rect(window_rect(window), {0, 48, 1280, 720}));
    int width = 0;
    int height = 0;
    CHECK(SDL_GetWindowSizeInPixels(window, &width, &height));
    CHECK(width == 1280 && height == 720);
    // Entering full screen again forgets the size: the window comes back at
    // the size it went in at.
    CHECK(SDL_SetWindowSize(window, 800, 600));
    CHECK(SDL_SetWindowPosition(window, 50, 60));
    pump_window(window, full_screen);
    alt_enter();
    CHECK(full_screen.window_width == 0 && full_screen.window_height == 0);
    alt_enter();
    CHECK(same_rect(window_rect(window), {50, 60, 800, 600}));
    SDL_DestroyWindow(window);
    SDL_Quit();
}

// The frame asked for in each case, one of each: hidden while a game is
// played at Hidden in play, shown elsewhere and at Always shown, and left
// as it is when the window shows what it should, in full screen, while
// switching and without a window.
void test_window_frame_cases() {
    using oa::app::window_frame_request;
    using oa::app::WindowFramePlace;
    using oa::app::WindowFrameRequest;
    CHECK(
        window_frame_request(true, WindowFramePlace{true, true, true}) == WindowFrameRequest::hide
    );
    CHECK(
        window_frame_request(true, WindowFramePlace{true, true, false}) == WindowFrameRequest::none
    );
    CHECK(
        window_frame_request(true, WindowFramePlace{true, false, false}) == WindowFrameRequest::show
    );
    CHECK(
        window_frame_request(true, WindowFramePlace{true, false, true}) == WindowFrameRequest::none
    );
    CHECK(
        window_frame_request(false, WindowFramePlace{true, true, false}) == WindowFrameRequest::show
    );
    CHECK(
        window_frame_request(false, WindowFramePlace{true, true, true}) == WindowFrameRequest::none
    );
    CHECK(
        window_frame_request(true, WindowFramePlace{false, true, true}) == WindowFrameRequest::none
    );
    CHECK(
        window_frame_request(true, WindowFramePlace{false, false, false}) ==
        WindowFrameRequest::none
    );
}

// A run through the game, the window doing what it is asked each frame:
// the frame goes as a game starts and comes back with its game menu, stays
// as it is in full screen, comes back on leaving full screen with the menu
// up, stays at Always shown and comes back at the main menu.
void test_window_frame_through_a_game() {
    using oa::app::window_frame_request;
    using oa::app::WindowFramePlace;
    using oa::app::WindowFrameRequest;

    struct Step {
        bool hidden_in_play;
        bool windowed;
        bool playing;
        WindowFrameRequest expected;
    };

    const std::array<Step, 12> steps{{
        {true, true, false, WindowFrameRequest::none},  // the main menu
        {true, true, true, WindowFrameRequest::hide},   // a game starts
        {true, true, true, WindowFrameRequest::none},   // and goes on
        {true, true, false, WindowFrameRequest::show},  // the game menu opens
        {true, true, true, WindowFrameRequest::hide},   // and closes
        {true, false, true, WindowFrameRequest::none},  // Alt+Enter: full screen
        {true, false, false, WindowFrameRequest::none}, // the game menu, in full screen
        {true, true, false, WindowFrameRequest::show},  // Alt+Enter: a window, the menu up
        {false, true, false, WindowFrameRequest::none}, // Always shown chosen
        {false, true, true, WindowFrameRequest::none},  // the menu closes
        {true, true, true, WindowFrameRequest::hide},   // Hidden in play again
        {true, true, false, WindowFrameRequest::show},  // the game ends: the main menu
    }};
    bool bordered = true;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const Step& step = steps[index];
        const auto request =
            window_frame_request(step.hidden_in_play, {step.windowed, step.playing, bordered});
        if (request != step.expected)
            std::fprintf(stderr, "window frame step %zu\n", index);
        CHECK(request == step.expected);
        if (request != WindowFrameRequest::none)
            bordered = request == WindowFrameRequest::show;
    }
}

// SDL's dummy video driver has no frame to hide: the window keeps its size
// and SDL accepts the request; with no window nothing is asked.
void test_window_frame_kept_size() {
    CHECK(oa::app::set_window_frame(nullptr, false));
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++failures;
        return;
    }
    SDL_Window* window =
        SDL_CreateWindow("window frame test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window != nullptr) {
        CHECK(oa::app::set_window_frame(window, false));
        CHECK(oa::app::set_window_frame(window, true));
        int width = 0;
        int height = 0;
        CHECK(SDL_GetWindowSize(window, &width, &height) && width == 640 && height == 480);
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

// A window of SDL's video driver (the dummy driver under ctest), switched by
// Alt+Enter, its repeats and keypad Enter.
void test_window() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        ++failures;
        return;
    }
    SDL_Window* window =
        SDL_CreateWindow("full screen test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window == nullptr) {
        SDL_Quit();
        return;
    }
    FullScreenSwitch full_screen{};
    bool entered = false;
    bool left = false;
    pump(full_screen, entered, left);
    CHECK(!shows_full_screen(window));

    CHECK(
        oa::app::take_full_screen_event(
            window, full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)
        )
    );
    (void)SDL_SyncWindow(window);
    pump(full_screen, entered, left);
    CHECK(shows_full_screen(window));
    CHECK(entered);
    CHECK(!full_screen.awaiting_shown);
    int width = 0;
    int height = 0;
    SDL_Rect display{};
    CHECK(SDL_GetWindowSizeInPixels(window, &width, &height));
    CHECK(SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &display));
    CHECK(width == display.w && height == display.h);

    // A held Alt+Enter's repeats are taken and change nothing.
    for (int repeat = 0; repeat < 3; ++repeat)
        CHECK(
            oa::app::take_full_screen_event(
                window, full_screen, key_down(SDLK_RETURN, SDL_KMOD_LALT, true)
            )
        );
    (void)SDL_SyncWindow(window);
    pump(full_screen, entered, left);
    CHECK(shows_full_screen(window));
    CHECK(!left);

    CHECK(
        oa::app::take_full_screen_event(
            window, full_screen, key_down(SDLK_KP_ENTER, SDL_KMOD_RALT, false)
        )
    );
    (void)SDL_SyncWindow(window);
    pump(full_screen, entered, left);
    CHECK(!shows_full_screen(window));
    CHECK(left);
    CHECK(SDL_GetWindowSizeInPixels(window, &width, &height));
    CHECK(width == 640 && height == 480);
    SDL_DestroyWindow(window);

    // A window created full screen starts in it, and Alt+Enter leaves it.
    window =
        SDL_CreateWindow("full screen test", 640, 480, oa::app::game_window_flags(true, false));
    CHECK(window != nullptr);
    if (window != nullptr) {
        FullScreenSwitch started{};
        CHECK(shows_full_screen(window));
        CHECK(
            oa::app::take_full_screen_event(
                window, started, key_down(SDLK_RETURN, SDL_KMOD_LALT, false)
            )
        );
        (void)SDL_SyncWindow(window);
        CHECK(!shows_full_screen(window));
        SDL_DestroyWindow(window);
    }

    // A window opened at native density keeps the flag, which says so for
    // the run; the dummy display's density is 1, so its pixels are its
    // points, as every windowed check's are.
    window =
        SDL_CreateWindow("full screen test", 640, 480, oa::app::game_window_flags(false, true));
    CHECK(window != nullptr);
    if (window != nullptr) {
        CHECK(oa::app::at_native_density(SDL_GetWindowFlags(window)));
        CHECK(SDL_GetWindowSizeInPixels(window, &width, &height));
        CHECK(width == 640 && height == 480);
        CHECK(SDL_GetWindowPixelDensity(window) == 1.0F);
        SDL_DestroyWindow(window);
    }
    window =
        SDL_CreateWindow("full screen test", 640, 480, oa::app::game_window_flags(false, false));
    CHECK(window != nullptr);
    if (window != nullptr) {
        CHECK(!oa::app::at_native_density(SDL_GetWindowFlags(window)));
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

oa::app::DisplayArea display_area(int32_t x, int32_t y, int32_t width, int32_t height, bool main) {
    oa::app::DisplayArea area;
    area.x = x;
    area.y = y;
    area.width = width;
    area.height = height;
    area.usable_x = x;
    area.usable_y = y;
    area.usable_width = width;
    area.usable_height = height;
    area.main = main;
    return area;
}

bool wholly_inside(const oa::app::RestoredPlace& place, const oa::app::DisplayArea& area) {
    const int64_t right = static_cast<int64_t>(place.x) + place.width;
    const int64_t bottom = static_cast<int64_t>(place.y) + place.height;
    const int64_t area_right = static_cast<int64_t>(area.usable_x) + area.usable_width;
    const int64_t area_bottom = static_cast<int64_t>(area.usable_y) + area.usable_height;
    return place.sized && place.positioned && place.x >= area.usable_x &&
           place.y >= area.usable_y && right <= area_right && bottom <= area_bottom;
}

void test_saved_window_place() {
    using oa::app::PlaceOverride;
    using oa::app::SavedWindow;
    oa::app::SavedWindow windowed;
    windowed.full_screen = false;
    windowed.has_window = true;
    windowed.window = {10, 20, 1280, 720};
    windowed.display_x = 0;
    windowed.display_y = 0;
    windowed.display_width = 1920;
    windowed.display_height = 1080;
    CHECK(oa::app::decode_window_place(oa::app::encode_window_place(windowed)) == windowed);

    oa::app::SavedWindow full;
    full.full_screen = true;
    full.display_x = 1920;
    full.display_y = 0;
    full.display_width = 1920;
    full.display_height = 1080;
    CHECK(oa::app::decode_window_place(oa::app::encode_window_place(full)) == full);
    full.has_window = true;
    full.window = {-100, 40, 800, 600};
    CHECK(oa::app::decode_window_place(oa::app::encode_window_place(full)) == full);

    CHECK(!oa::app::decode_window_place(""));
    CHECK(!oa::app::decode_window_place("window 0x720 at 0,0 on 0,0,1280,800"));
    CHECK(!oa::app::decode_window_place("window 1280x720 at 0,0 on 0,0,1280,800 extra"));
    CHECK(!oa::app::decode_window_place("full-screen on 0,0,0,800"));
    CHECK(!oa::app::decode_window_place("window 20000x720 at 0,0 on 0,0,1920,1080"));

    const oa::app::DisplayArea main = display_area(0, 0, 1280, 800, true);
    const oa::app::DisplayArea gone = display_area(1920, 0, 1920, 1080, false);
    const std::vector<oa::app::DisplayArea> now{main};
    oa::app::SavedWindow off = windowed;
    off.display_x = gone.x;
    off.display_y = gone.y;
    off.display_width = gone.width;
    off.display_height = gone.height;
    off.window = {2000, 100, 900, 700};
    const oa::app::RestoredPlace fitted =
        oa::app::restore_window_place(&off, PlaceOverride::none, true, now);
    CHECK(!fitted.full_screen);
    CHECK(fitted.width == 900 && fitted.height == 700);
    CHECK(wholly_inside(fitted, main));
    CHECK(fitted.x == 380 && fitted.y == 100);

    off.window = {2000, 100, 2000, 1500};
    const oa::app::RestoredPlace shrunk =
        oa::app::restore_window_place(&off, PlaceOverride::none, false, now);
    CHECK(shrunk.width == 1280 && shrunk.height == 800);
    CHECK(shrunk.x == 0 && shrunk.y == 0);
    CHECK(wholly_inside(shrunk, main));

    const oa::app::DisplayArea left = display_area(-1920, 0, 1920, 1080, false);
    const std::vector<oa::app::DisplayArea> two{main, left};
    oa::app::SavedWindow negative = windowed;
    negative.display_x = left.x;
    negative.display_y = left.y;
    negative.display_width = left.width;
    negative.display_height = left.height;
    negative.window = {-1500, 100, 800, 600};
    const oa::app::RestoredPlace stayed =
        oa::app::restore_window_place(&negative, PlaceOverride::none, false, two);
    CHECK(stayed.x == -1500 && stayed.y == 100);
    CHECK(stayed.width == 800 && stayed.height == 600);
    negative.window.x = -3000;
    const oa::app::RestoredPlace moved =
        oa::app::restore_window_place(&negative, PlaceOverride::none, false, two);
    // Entirely left of the display. The least move that lands it wholly on
    // the display puts its left edge on the display's left edge. The right
    // edge of that display would be -800, which moves the window further.
    CHECK(moved.x == -1920 && moved.y == 100);
    CHECK(wholly_inside(moved, left));

    std::map<std::string, std::string> values;
    oa::app::put_window_place(values, full);
    const auto remembered = values;
    oa::app::SavedWindow observed = windowed;
    CHECK(!oa::app::store_window_place_unless(values, observed, false));
    CHECK(values == remembered);
    const oa::app::RestoredPlace commanded =
        oa::app::restore_window_place(&full, PlaceOverride::window, true, two);
    CHECK(!commanded.full_screen);
    CHECK(commanded.sized);
    CHECK(commanded.width == 800 && commanded.height == 600);
    const oa::app::DisplayArea wide = display_area(0, 0, 1920, 1080, false);
    const std::vector<oa::app::DisplayArea> wide_and_main{wide, main};
    const oa::app::RestoredPlace forced_full =
        oa::app::restore_window_place(&windowed, PlaceOverride::full_screen, false, wide_and_main);
    CHECK(forced_full.full_screen);
    CHECK(!forced_full.sized);
    CHECK(forced_full.positioned);
    CHECK(forced_full.x == windowed.display_x && forced_full.y == windowed.display_y);

    const oa::app::RestoredPlace first_full =
        oa::app::restore_window_place(nullptr, PlaceOverride::none, true, two);
    CHECK(first_full.full_screen && !first_full.sized && !first_full.positioned);
    const oa::app::RestoredPlace first_window =
        oa::app::restore_window_place(nullptr, PlaceOverride::none, false, two);
    CHECK(!first_window.full_screen && !first_window.sized && !first_window.positioned);

    const oa::app::DisplayArea side = display_area(1920, 0, 1920, 1080, false);
    const std::vector<oa::app::DisplayArea> with_side{main, side};
    const oa::app::RestoredPlace full_again =
        oa::app::restore_window_place(&full, PlaceOverride::none, false, with_side);
    CHECK(full_again.full_screen && !full_again.sized && full_again.positioned);
    CHECK(full_again.x == full.display_x && full_again.y == full.display_y);
    const std::vector<oa::app::DisplayArea> main_only{main};
    const oa::app::RestoredPlace full_moved =
        oa::app::restore_window_place(&full, PlaceOverride::none, false, main_only);
    CHECK(full_moved.full_screen && full_moved.x == main.x && full_moved.y == main.y);
}

} // namespace

int main() {
    test_keys();
    test_held_enter();
    test_rebound_chord();
    test_window_flags();
    test_edge_scroll_depth();
    test_switch_at_once();
    test_switch_while_changing();
    test_unanswered_request();
    test_switch_outside_the_game();
    test_window_on_display();
    test_window_place_checked();
    test_pointer_bounds();
    test_pointer_bounds_events();
    test_window_trace();
    test_window();
    test_pointer_window();
    test_full_screen_pointer();
    test_launch_holds_pointer();
    test_window_back_on_display();
    test_window_at_size_on_display();
    test_window_takes_a_size();
    test_window_frame_cases();
    test_window_frame_through_a_game();
    test_window_frame_kept_size();
    test_saved_window_place();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts(
        "full screen: the window opens at native density only when asked, Alt+Enter switches, "
        "the mode asked for is kept, the pointer stays on the screen while the window has the "
        "focus, from the start in a window opened in full screen, a pointer past a full-screen "
        "window's edge touches that edge, a window leaving full screen comes back onto its "
        "display, at a size asked for, a saved place round-trips, the window's frame follows "
        "the game, and the trace line tells the window's place on its display and the pointer's"
    );
    return 0;
}
