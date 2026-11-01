# platform/display-modes

The screen sizes a display offers, which the options' Screen Size and the
settings' Screen size list, and the full-screen mode a window takes for one
of them.

`oa/platform/display_modes.hpp` (namespace `oa::platform::display_modes`,
target `oa-platform-display-modes`) works on a report of the display
(`DisplayReport`): its full-screen modes as the system lists them, each with
its size, pixel density and refresh rate, and the desktop's mode. The rules
read nothing else, so that a test or a check can hand them any monitor's
modes.

- `offered_sizes(report, use, minimum_height)` lists the sizes the display
  offers, each once whatever its refresh rates, colour depths and pixel
  densities, the narrower first and of two as wide the shorter, as the
  game's resolution lists are sorted. Every mode at least 640 wide and
  `minimum_height` tall is offered (480, or the 768 of a mod's
  `ui.display-modes`); for a window (`Use::window`), only those that fit a
  known desktop, with the `fallback_sizes` that fit it, since a window
  takes any size and a display may list no mode of the smaller ones (a
  Mac's scaled modes). A display that reports more than `most_sizes` (100)
  sizes, the number the game's own list holds, offers its largest. A display that
  reports nothing useful (no mode of 640x480 or more, as SDL's dummy video
  driver, or a phone's) offers `fallback_sizes`, the game's earlier fixed
  list from 640x480 to 1600x1200, without those larger than a known desktop;
  640x480, the game's own screen, always stays (with a mod's floor, the
  shortest size tall enough, or 1600x1200 when none is).
- `can_show(report, size, use)` says whether a stored size can be shown as
  the game starts: a size the display has a mode of, or for a window any
  size that fits the desktop. A display that reports nothing useful cannot
  be judged and shows every size.
- `mode_for(report, size)` picks the mode a size switches to in full
  screen: one of exactly that size, at the desktop's pixel density first,
  then at the desktop's refresh rate, then the highest rate.
- `nearest_offered(sizes, size)` finds a size in a list, or the last one
  listed before it.
- `listed_size_text(size)` is a size as the screen-size controls show it,
  "1920 x 1080". `size_text` is the form a setting stores, "1920x1080".
- `report_from_text(text)` reads a made-up monitor, as the game's
  `--display-modes` names one for its checks: modes separated by commas,
  each `WIDTHxHEIGHT`, optionally followed by `@RATE` and `/DENSITY`, the
  first also the desktop's, or `none` for a display that reports nothing.

Sizes are in the window system's own units, the units a window's size is
given in: pixels on Windows and X11, points on macOS and Wayland. A Retina
display's list is in points, as macOS lists its modes, so that a listed
size is the size the game lays its frame out in and fits the screen; the
display draws each point with several pixels.

`oa/platform/display_modes/sdl.hpp` (target `oa-platform-display-modes-sdl`)
fills the report through SDL: `read_display(display)` reads a display's
full-screen modes (`SDL_GetFullscreenDisplayModes`) and desktop mode, and
`take_full_screen_size(window, size)` sets the mode the window takes in
full screen to `mode_for`'s. SDL lists each mode of every platform the game
runs on, Windows XP's included, from the system's own list: Windows' display
settings, macOS's display modes (with the low-density duplicates of each
scaled size), XRandR's on X11, and on Wayland the desktop's logical size
and, where the compositor can scale a frame, the panel's own pixel size as
a full-screen mode. SDL cannot change the compositor's output mode. A
window lists the sizes that fit the logical desktop; full screen lists
every reported mode, so a 1920x1080 panel whose desktop is the logical
1536x864 offers 1920x1080 in full screen and not in a window. Alt+Enter
stays on the desktop's mode; choosing another size draws at that size and
the picture is scaled.

The module reads and writes no game state. `platform-display-modes` tests
the rules over made-up monitors: a 4K monitor on Windows, a 14-inch Retina
MacBook Pro's display, a 1280x1024 monitor of Windows XP's time, a 3440x1440
ultrawide, a portrait monitor, a Wayland laptop whose desktop is a scaled
1920x1080, one that reports more sizes than a list holds, and one that
reports nothing, for full screen and for a window, the floor a mod raises,
the mode each size switches to and the made-up monitors' text.
`platform-display-modes-sdl` runs them over SDL's dummy video driver, which
reports no full-screen mode and a 1024x768 desktop.
