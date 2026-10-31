# src/app

The game application: the native runtime that hosts the frontend and the
match, and, in `netgame/`, network play. The `oa-game` target builds it as `open-annihilation`
(`open-annihilation.exe` on Windows).

## The built game

On macOS the game is the application bundle `open-annihilation.app`, made
from the template `Info.plist.in`: the Dock, the application switcher and
the menu bar show its name, Open Annihilation, and it carries the project's
version and the oldest macOS release its code runs on. `run.sh` starts the
executable inside it, `Contents/MacOS/open-annihilation`, in place. CMake
names a bundle after its executable, so the bundle keeps the executable's
name (`cmake/OaGameBundle.cmake`).

The files that travel with the game (`LICENSE`, `ATTRIBUTIONS.md`,
`licenses/` and what the extensions add through their `GAME_FILES`)
go in the folder `SDL_GetBasePath()` names at run time: the bundle's
`Contents/Resources` on macOS, the executable's folder elsewhere. The
target's `OA_GAME_FILES_DIR` property names it for build commands. The
out-of-memory report goes to `ErrorLog.txt` beside the application: beside
the executable, or beside the bundle, never inside it.

Classic travels with those files as `classic.oabrain` in `brains/`. The build
packs `brains/classic` with `oa-tool brain pack` after the game links
(`cmake/OaBrains.cmake`). A build that runs its own `oa-tool` uses that
tool. A cross build cannot run the `oa-tool` it compiles: it packs Classic
with the host build's `oa-tool` when `OA_HOST_OA_TOOL` names that
executable, and leaves Classic out when the option is empty, saying so
once at configure time. `tools/build_windows.sh` passes `build/oa-tool`
when the host build has one.

The game carries three icons. `tools/make_icons.py` makes them on macOS
from the 1024-pixel pictures in `branding/` and the results are committed
beside those pictures. The app icon, the plates alone
(`open-annihilation-icon.png`), becomes the bundle's
`open-annihilation.icns`, which its Info.plist names;
`open-annihilation.ico`, which `open-annihilation.rc.in` compiles into
`open-annihilation.exe` with the version information that names it Open
Annihilation; and `open-annihilation-256.png`, which the build embeds
(`window_icon_png`) and start-up gives the window with `SDL_SetWindowIcon`.
A failure there is reported, and the game starts without it. That icon is
also the badge of every OA dialog and notice but Settings and the Library,
and the small maker's mark. The settings icon, the plates over two gears
(`open-annihilation-settings-icon.png`), is embedded at 256 pixels
(`open-annihilation-settings-256.png`; `settings_icon_png`) for the buttons
that open Settings and for the Settings dialog's header. The mods icon, the
plates over drive bays (`open-annihilation-mods-icon.png`), is embedded the
same way (`open-annihilation-mods-256.png`; `mods_icon_png`) for the Library
button and the Library's header. The branding is not under the project's
licence (`COPYRIGHT`). `branding-icons` checks the icons' sizes and that
their corners are clear, and `app-window-icon` that the embedded ones
decode.

Start-up makes the window's renderer by walking SDL's render drivers one
at a time, in SDL's own order (`render_host.hpp`): the first that starts is
kept, which with every driver working is the one SDL's own choice makes,
and each driver that refuses is logged with SDL's reason, as on the dummy
video driver: `open-annihilation: graphics: renderer vulkan refused: No
dynamic Vulkan support in current SDL video driver (dummy)`. Before SDL's
software renderer, the last of the walk, the framebuffer hint is set when
a driver before it refused: `0` where the window has a framebuffer of its
own (the windows, x11, dummy and offscreen video drivers), so that
software presents through it with no graphics driver, and otherwise the
hardware drivers of SDL's order for software to present through, or the
first of them alone before SDL 3.4. A hint that is not taken, as when the
player's own `SDL_FRAMEBUFFER_ACCELERATION` takes priority, is logged and
the walk goes on. Under `SDL_RENDER_DRIVER` the start is SDL's own call,
which tries only the drivers the variable names, with no walk and no hint,
and a failure ends the run as it always has. When nothing starts, the run
ends with `SDL_CreateRenderer:` and the last refusal's reason.
`app-render-host` checks the walk, and `native-renderer-walk` its log on
the dummy video driver.

While the game runs, a failed SDL call of the standard tier's presenting
throws `PresentError` (`scaled_world.hpp`), which `Runtime::render` catches
by its type alone, so that an error from a hook it reaches keeps its own
path; the display sink and `apply_output_mode`'s SDL calls catch it too,
while the layout and the pointer's known place change at once. The
renderer is then made again (`Runtime::rebuild_renderer`,
`RendererHost::rebuild`): every texture the game made is forgotten, the
drivers after the one that failed in SDL's order are tried, under
`SDL_RENDER_DRIVER` only those its list names after it, then SDL's
software renderer, with the framebuffer hint set before it; the screen is
laid out again, the pointer kept on the window as before, and a match's
message log says so. A rebuild waits for the next `render()`, never a hook
or a drain of events, and the failed frame is not shown. The run ends only
when no driver starts, or when SDL's software renderer that a rebuild made
fails before it presents a frame. The render events reach the event
dispatch, the loading pump, the movies' hook and `drain_input`
(`take_render_event`; before the runtime exists, `RendererHost::take_event`
and `service`): a reset device forgets every texture, each made again from
its buffer at the next frame, and the third reset within a minute, or a
lost device, makes the renderer again. A device that says it is lost, as
one does on some renderers while another program holds the screen
(`render_probe::device_state`), is waited for: until its render targets
are reset nothing it fails makes a rebuild and nothing is read back. A
present SDL refuses is the game's own fault: the render target goes back
to the window and it is logged once. Presents over 2 s three times within
10 s of steady frames are logged once and the game carries on. The
renderer records keep what fails (below): a present error, a lost device
and three resets are struck against the driver, a lost device or the
resets also recording at once that the graphics card is not to be used
with it, and the same in the next run on it recording it failed. Nothing
is struck or recorded on a driver whose device is lost in ordinary use,
on SDL's software renderer unless `--force-capable` runs the accelerated
tier on it, or under `SDL_RENDER_DRIVER`. Window-size textures beyond the renderer's texture limit
(the world, the match dialog layer, the OA settings layer and the front
end's) are made as tiles with gutters (`TiledTexture`), and a match makes
no front-end texture beyond it, since it never draws one; within the limit
each stays one texture, as on SDL's software renderer, which has none. On
a hardware driver the walk chose the opaque layers (the match's, the
loading screen's and the front end's) are ARGB8888, drawn with no
blending; SDL's software renderer, and every driver under
`SDL_RENDER_DRIVER`, keep XRGB8888, RGB565 on a 16-bit window, and RGB24
for the front end. SDL's software renderer asks the window for its pixels
at each frame, since a display mode of another depth or another display
can change them while the game runs. `--check-renderer-ladder`
(`native-renderer-ladder`) forces each of these failures on the dummy
video driver and checks that the game presents on through it, then
starts the game again on renderer records in scratch folders to check
what each crash and failure counts for at the next start;
`--render-fault POINT[@FRAME]` narrows it to one
(`native-renderer-ladder-create` makes every driver but software refuse at
start; `--render-fault card` fails a call of the Full tier's own on a
Full match frame, and `--render-fault effects` its effects pass). Two cases switch the accelerated tier on and run
only when named: `--render-fault memory` (`native-renderer-ladder-memory`),
the memory guard refusing buffers and then dropping the tier, and
`--render-fault full-memory` (`native-renderer-ladder-full-memory`), the
same for the Full tier, whose pages the guard drops first; each skips
under 2 GiB.

Once the renderer is made, start-up describes it with the
[render probe](../platform/render-probe/README.md) and logs one line
(`graphics_report.hpp`): the render driver on the video driver, the
adapter in brackets where it was read, the largest texture side as the
render policy corrects it (`corrected_texture_limit`, which asks the
policy's `texture_limit`) and the tier every frame is drawn in, as in `open-annihilation: graphics: metal on
cocoa (Apple M2), textures up to 16384; standard tier: the processor draws
everything`. Under `SDL_RENDER_DRIVER` the adapter is not read and the
brackets are left out, and so they are for SDL's software renderer, which
has no adapter; no limit reads "textures of any size". The +stats overlay
names the same renderer. `app-graphics-report` checks the limit and the
line, and `native-renderer-report` that a start on the dummy video driver
logs it.

## The start's movies and the menu hum

The game starts with two movies, the logo movie and the intro (the mod
profile's `media.movies`, `1.zrb` and `2.zrb` for 3.1c), unless
`--skip-intro` or the game's `-c`, `-n` or `-y` skips them, or a key ends
them. The main menu's hum, the BGM loop on the alternate route
(`Runtime::play_menu_voice`, `runtime_menu_audio.cpp`), starts with the
main menu once the movies are over, never while one plays:

- A start that does not show the setup plays the movies in `run_once`
  (`main.cpp`) before the archives are mounted. The runtime then opens the
  main menu, which starts the hum.
- A start that shows the setup, the first start and a `--first-run` start,
  keeps the movies until the setup will not show again
  (`Options::intro_after_setup`, `Runtime::play_intro_after_setup`), and
  the hum waits with them (`Runtime::menu_hum_waits`): it does not play
  through the setup window, the copy, the mounting of the archives or the
  setup's steps over the main menu. When the walk ends, the movies play,
  and the frame after them the hum starts
  (`Runtime::start_waiting_menu_hum`); with the movies skipped it starts
  as the walk ends. A `--first-run` start shows the setup again each time
  the menu settles, so its movies never come: its hum starts once the walk
  is first finished.

The hum stops as a match starts (`Runtime::stop_menu_music`) and starts
again as the game returns to the menus; a movie the menus play holds it
while it plays (`Runtime::play_movie_resource`). `--mute` and headless runs
play none of it, but the log says what it does in every run: `menu hum:
waits for the setup and the movies after it` when it waits, and `menu hum:
starts`, with `(muted)` or `(headless)`, each time it starts, after the
movies' `intro 1.ZRB: decoded …` lines. `native-menu-hum-start` and
`native-menu-hum-start-skip-intro` check that order on a start without the
setup, with the movies and with `--skip-intro`; `native-menu-hum-setup`
that the hum still waits once the setup window has copied the game and the
archives are mounted; and `native-menu-hum-first-run` and
`native-menu-hum-first-run-skip-intro` that, when the setup's walk over the
menu ends, the movies play first and the hum starts after them, or at once
when they are skipped.

## Adding a screen or overlay

1. In your package, write `void oa::app::register_<pkg>_screens(ScreenRegistry*)`
   (include `screen_registry.hpp`; link `oa-ui-screen-registry`).
2. Fill a `ScreenDesc`: pick an id `>= kFirstPackageScreen` (duplicates are
   rejected at startup), a name, GUI `assets` (null `layout` = you draw
   everything), and any of `enter/leave/event/tick/draw`. Put package state in
   `state`; it is passed back to every hook. Call `screen_register`.
3. For overlays (e.g. a status widget over the main menu) fill an
   `OverlayDesc`: `screen` filter (`kScreenAny` for all), `z` (drawn
   ascending, input descending), `create/event/tick/draw`; `overlay_register`.
4. Dispatcher steps: `step_register(registry, frontend::Step::..., fn, state)`.
   Dispatcher queries: `query_register(registry, frontend::Query::..., fn, state)`;
   the handler's result is the query's, and a query no handler takes is
   answered with 0.
5. Append one line `OA_REGISTER(register_<pkg>_screens)` to `screens.inc`,
   or, for an extension's screens, call the function from its
   `register_screens` hook.
6. Hooks receive a `ScreenContext`: assets, current `surface`, `world`
   (null outside a match), `input` (events only) and services. Navigate with
   `screen_request(ctx, id)` (applied after the current event/tick), play
   sounds with `screen_play_sound`, set the status line with `screen_status`,
   and use `ctx->services->read_number/...` for preferences. The services
   also stop every sound (`stop_sounds`), play a sound on the alternate
   route the menu music takes (`play_sound_alternate`), run one pass of the
   frontend dispatcher once the current event or frame is handled
   (`run_frontend`, never while a match is on screen) and end the run with
   a reason and an exit status once the current event or frame is handled
   (`quit`, which leaves a running match first). The context's `host` and `services` stay the same while the
   runtime lives, so a package may keep them and use the services outside
   its callbacks.
7. Event hooks return nonzero to consume input; otherwise the built-in
   handler still runs.

## The OA layer

`oa_layer.hpp` and `oa_layer.cpp` are one host for Open Annihilation's own
screens over every screen of the game: `OaLayer`, which every screen
reaches through `Runtime::oa_layer()`. It holds the settings dialog on the
main menu and in a match (`Runtime::SettingsScreen`), the Library on the
main menu (`Runtime::LibraryScreen`, [The Library](#the-library)), the
in-game menu's OA Settings button, Arrange overlays, and the
notices and questions of the engine's own
(`NoticeScreen`, `QuestionScreen`). It replaces the main menu's and the match's own hosts of
the dialog, and the notices' and the installs' prompts' own overlays, which
each mapped the pointer, told a finger from a mouse, mapped the keys,
latched the closing key and drew a backdrop.

- **A screen** is a `LayerScreen`: a name, a placement, whether it is modal
  and darkens what lies under it, whether it is opaque, a layout at the
  view's size class (`lay_out`), a drawing from its own top left corner at
  the canvas's scale, its answers to the pointer, the keys, the wheel
  and typed text, a text field, its display list and interaction (what
  automation lists), a tick and a revision. No call of it names an SDL
  type. A screen joins with
  `oa_layer().push(std::make_unique<...>())` and leaves when it answers
  `close`, by `oa_layer().close(screen)`, or by `close_top` and
  `close_above(name)`; `close` tells it it leaves, and a screen whose own
  model is still open closes it there as its Cancel would. A host may close
  a screen while that screen takes an input or its tick: it leaves once the
  call is done, as if it had answered `close`. `return_to(name)` brings a
  screen on the stack back to the top by closing every screen above it in
  that way, so that a screen asking for one already under it goes back to
  it rather than stacking a second.
- **Pushed or queued.** A screen somebody asked for at that moment joins at
  once with `push`: Settings, or a question a screen asks of its own over
  that screen (the Library's Switch Mod question over the Library). One nobody
  asked for at that moment, such as a notice or an install's question,
  joins with `show_when_free`: the screens wait first in, first out, and the
  first is pushed while no modal screen shows, no frontend message box is
  open and no match is being played, at once, after an input or a tick, or
  when a screen is closed, so that two never show at once, none opens over
  Settings and none is drawn over a match. A registered notice or prompt is
  never held back because nobody is watching. A question a screen asks about
  its own content is pushed, not queued. The engine's own unsolicited
  notices and prompts are still suppressed in an unwatched run by the code
  that raises them, unless that run's check asks for them. A screen waiting
  is ticked too, and leaves the queue
  when its tick closes it. `find(name)` finds a screen on the stack and
  `waiting(name)` one in the queue.
- **Notices and questions.** `NoticeScreen` holds a `kit::Notice`, and
  `QuestionScreen` a `kit::Question`, each named by its word (`notice` and
  `prompt` unless the code that raises it gives its own),
  with the callbacks of their host (`NoticeScreen::Host`,
  `QuestionScreen::Host`). Each shows over one screen of the game, laid out
  at the view's size class (its class's notice width wide, as tall as its
  text makes it, from `notice_height` or `prompt_height`) and centred in the
  window at the view's scale, modal over a backdrop, and is drawn with
  `draw_notice` or `draw_prompt`; a question keeps its class when its host
  puts another question in its place (`changed`). Its events go through
  the kit's notice and question functions with the layer's point, reach and
  key, and its display list and interaction are the kit's
  (`notice_list`, `question_list`) with its hover, press and mark. A
  notice's host says what its open button does (the failure it returns is
  shown in amber), what OK does besides closing it, and, each tick, whether
  it must close. A question's host takes each answer (the key that answered
  is latched, `latch_key`, whether the question closes or stays) and says
  whether the answer was its last; the host may change the question
  meanwhile (`question`, `changed`) or close it, and is told when it leaves
  for any reason (`closed`). To show one: make the screen with the layer,
  the screen of the game it shows over, the notice or question and the
  host's callbacks, then `show_when_free` it, or `push` it when the screen
  under it asked. The saves notice, the mod warning and the found folder's
  notice are made by `UserFolderState::show_notice`, and the installs'
  prompts by `ModInstallState::show`.
- **The work dialog.** `WorkScreen`, named `work`, holds a
  `kit::WorkDialog`: a title and one line as given, no buttons, its
  spinner turning with the time it has shown. It shows over one screen of
  the game, laid out at the view's size class and centred at its scale,
  modal over a backdrop, takes every input and does nothing with it, and
  closes when its host's tick says so (`WorkScreen::Host`). Automation
  lists it as `oa.work`, its one part the label `oa.progress.body` with
  its line. The one the engine shows is a
  Library switch's, over the main menu after the soft restart while the
  Library waits to open again
  ([the soft restart](#mod-profile-and-mod-folders)), for at most five
  seconds. It stands for the Library the player asked for, so it is
  pushed rather than queued, but only where that Library could open
  (`Runtime::kept_library_may_show`), and it closes as soon as it could
  not: a message box, a notice or a frame a package owns comes first.
- **Registered pages, screens, notices and prompts.** Code outside the
  engine's own screens registers them for this runtime: a settings page in
  the layer's registry (`registered_settings`), a kit screen in the
  runtime's `ScreenRegistry` (`kit_screen_register`), as
  `oa/ui/screen_registry/kit.hpp` describes them. An extension registers
  them through its table ([Extensions](#extensions), version 15). A settings page is listed by the engine's Settings dialog, under a divider
  naming its owner, after the engine's own sections, and laid out from its
  preference rows. OK writes the keys that changed and, once the preferences
  are saved, calls the page's `applied`; Cancel writes nothing, and a mod's
  options dialog lists no registered page. `open_registered_screen(name)`
  opens a kit screen by name: modal, over a backdrop, filling the front end's
  room, laid out and drawn with the kit. Its automation dialog is `oa.<name>`
  and its parts are named by its own layout. An unknown name opens nothing
  and returns false. `act` returning 2 pops the screen, and `closed` runs
  once as it leaves. `queue_notice` and `queue_prompt` build a notice (its
  word `registered`) or a prompt (its word `prompt`) and put it on this same
  queue, so a registered one and an engine one wait for each other and two
  are never shown at once. A prompt's `answered` is called once, with the
  button pressed, or with −1 from `drop_pending_prompts` when the runtime
  ends with the prompt still queued or shown. Calling that drop twice is safe.
- **The UI kit gallery.** Settings › Developer's UI kit gallery row asks
  for it (`DialogAction::show_gallery`), and `take_engine_settings_action`
  opens it with `OaLayer::open_gallery`: a `GalleryScreen` named
  `gallery`, modal over a backdrop and opaque, over the whole window, the
  settings dialog staying open under it. On the front end it takes the
  view's scale and size class (the Interface size, or the Auto scale) and
  centres the room's points at that scale in the window; in a match it
  takes `OaLayer::match_gallery`: the match canvas's points at the layer's
  scale on it, one step down where that gives Large, capped by the side
  column's scale, so that 640x480 is Compact at 1×, and 1280x720 and
  1920x1080 are Large at 1× with HUD scaling On (1920x1080 Large at 1.5×
  while 150% is offered), the room right of the column playing no part. Its model is
  its own (`kit::GalleryModel`); it reads and changes no setting. Its
  events go to `kit::gallery_act`, and it is laid out again
  (`kit::gallery_layout`) whenever the model or the frame changes; Escape
  and CLOSE take it off, back to Developer, and it leaves when the settings
  dialog does. Its automation dialog is `oa.gallery`, its controls
  `oa.gallery.<section>.<n>` (`oa.gallery.toggle.1.on`,
  `oa.gallery.drop-down.1`) and `oa.gallery.close`
  ([src/ui/kit](../ui/kit/README.md#gallery)).
- **The download check.** `Runtime::tell_challenges`, once a frame beside
  `tell_mod_installs`, reads the download queue when its generation moves.
  While the main menu shows, including under the Library or Settings, one
  window named `challenge` shows the registry's name, the page and the code
  (`draw_challenge`). It joins at once, or with `show_when_free` while a
  notice or a question is the top screen. OPEN THE CHECK calls
  `open_web_link` with the page the queue gave and leaves the window
  waiting; that button is absent when `web_address_available` is false. TRY
  AGAIN asks the queue to start that download again. CANCEL cancels that
  download and the same registry's downloads that are still waiting, then
  closes the window. A passed check, or a download cancelled somewhere
  else, closes it. A check that arrives during a game waits until the main
  menu, and leaving the main menu hides the window without cancelling the
  download. A new code refreshes the same window.
- **Registries.** `runtime_registries.cpp` hosts the Add registry window,
  a screen named `add-registry` (`Runtime::AddRegistryScreen`, modal, over
  a backdrop, centred over the front end at the view's size class, nowhere
  in a match) and drawn by `draw_add_registry`. ADD REGISTRY… on the
  Library's Registries tab opens it empty (`open_add_registry`); a caller
  with an address passes it, and its check starts at once. ADD MIRROR…
  opens it for a mirror of a registry the player added
  (`open_add_mirror`), with the keys the player trusts for that registry.
  `take_registry_files`, once a frame beside `tell_mod_installs`, takes the
  next `.oareg` file of the add-registry queue (`take_registry_file`) on
  the settled main menu with no OA screen taking every input and no game
  running, one at a time, reads it whole (at most
  `oa::data::registry::max_descriptor_bytes`; a larger or unreadable file
  is refused) through the platform's `take_opened_file` where it has one,
  and opens the window filled in from it. A run nobody watches leaves the
  files waiting, unless `--check-registries` asks for them. CHECK reads
  the typed address with `parse_http_url` first: an address the reader
  refuses is refused there, and the content service is never asked. An
  https address is asked. The client uses the system's TLS where this
  system has it, and reads https as http where it does not. A certificate
  the system rejects fails the check. The window asks the content
  service (`check_registry_url`, `check_mirror`, or `check_registry_file`
  for a file) and polls `registry_check` each frame in its tick; CANCEL
  closes it and drops the check, whose answer is never used. ADD calls
  `add_checked_registry` or `add_checked_mirror`, closes the window and
  refreshes an open Library so its Registries tab lists what was added; a
  refusal then, the registries having moved since the check, shows as any
  other. The refusal sentences are one table in that file, `{name}` the
  checked registry's name (for a mirror, the registry's own) or the
  address without one. The Library's Registries tab lists every registry
  in effect from the content service's snapshot (`library_inputs`), leaving
  out an added registry whose id a built-in one has taken. Its switch and
  REMOVE apply at once with `set_registry_enabled` and `remove_registry`
  (`change_library_registry`, `remove_library_registry`); a refusal is
  logged with `refusal_text` and said on the tab at once. The
  window's field keeps the system's text input started while it has the
  focus (`text_field`); Space or a pad's A on it, and a press on it, start
  that input again (`OaLayer::restart_text_input`), so that an on-screen
  keyboard the player put away shows again. Ctrl+V, or Cmd+V on macOS,
  gives a focused field of any of the layer's screens the clipboard's text
  up to its first line break (`layer_paste`), which the window takes up to
  its 2,048-byte limit. `native-registries` (`--check-registries`) writes
  its own registry list under `--user-folder` before the content service
  starts, switches its added registry off and on at once, removes it at
  once, asks about an https address, and opens an `.oareg`
  file whose catalogue at 127.0.0.1:1 cannot be reached.
- **Scale and size class.** On the front end the layer draws its screens at
  a whole scale on the kit canvas's crisp form, or at 150% on its smooth
  form in the modern fonts (`layer_drawing`), and lays them out at the size
  class of the points left over (`LayerView`, from `view`, worked out afresh whenever the window's
  size, the touch state or the Interface size in effect may have changed:
  each input, tick and present lays the screens out again,
  `lay_out_screens`). It fills a `kit::Viewport`
  as the Game files screen fills its own: the canvas in pixels (the
  renderer's output size, a desktop window's size in pixels, never its
  points), the canvas pixels a window point holds (`density`) and, while
  the game has touch controls, the window's safe insets. The scale
  (`kit::layer_viewport`) is the Interface size setting's whole step, taken
  when its OK is pressed, or for Auto `kit::auto_scale`: the canvas height
  over 720, the nearest step, never below the density, so a point is never
  smaller than a window point. While the modern fonts are on and the
  layer's stack of them opens (`smooth_fonts`), 150% is among the steps and
  a tie takes the larger: 1x below 900 lines, 150% from 900, 2x from 1260
  and 3x from 1800 (`kit::auto_with_fraction_percent`). Otherwise the steps
  are whole: 1x below 1080 lines, 2x from 1080. For Auto alone, where that
  scale leaves the room short of the class wanted (Large, or the class the
  Window size chooses) and one step down gives it that class, the scale
  takes the step: from 2× to 150% while 150% is offered, else to 1×, never
  below the density nor 1×. A chosen Interface size is never stepped down.
  The scale is then held to the largest at which Compact's 480 by 324
  dialog fits the canvas less its insets, and to at least 1. 150%, chosen
  or what Auto picked, is 1.5 where Compact fits at one and a half, else 1.
  The class comes from `kit::frame_of`: the largest whose settings dialog
  fits the room with 24 points round it (`kit::size_class_of`), Compact
  below 768 by 534 points, Regular from 768 by 534 and Large from 1008 by
  648. A Window size other than Auto (`open-annihilation.ui.window-size`,
  `OaLayer::window_size`, `window_placement::class_under`) takes the class
  it names wherever that class's dialog fits whole, and the largest that
  does elsewhere (`kit::size_class_within`). A 640 by 480 window is Compact
  at 1×, pixel for pixel as 0.7.3; 800 by 600 is Regular at 1×; 1024 by
  768, 1280 by 720, 1366 by 768 and 1280 by 800 are Large at 1×; 1920 by
  1080 and 2560 by 1080 are Large at 1× with the modern fonts off, where
  2× would leave them Regular, and Large at 1.5× with them on, as is 1914
  by 1077; 2560 by 1440 is Large at 2× and 3840 by 2160 Large at 3×; a
  phone of 852 by 393 points at 3 pixels a point is Compact at 3×, its
  density the floor; a chosen 150% on 1920 by 1080 is Large at 1.5×, and
  a chosen 200% there Regular at 2×.
  `screen_scale`, `screen_scale_percent`, `screen_class` and
  `placement_of(name)` report them, for the checks. Without a renderer the canvas is the picture
  itself, Compact at 1×. In a match the view is Compact at 100%, and
  Settings, and every window placed as it is, takes a scale and class of
  its own (`OaLayer::match_dialog`, `window_placement::match_window`). Its
  scale is the layer's on the match's canvas (`window_placement::match_viewport`:
  `kit::layer_viewport` of the canvas's pixels at the Interface size in
  effect, `LayerView::interface_percent`, with Auto's step down for the
  class the Window size wants), capped by the side column's scale
  (`MatchLayout::scale`, which HUD scaling caps, as before): above it, the
  largest whole step at or below it, and at least 1. The class comes from
  the points of the window's whole safe area at that scale, or the Window
  size held to what fits it, as on the front end: the dialog measures the
  whole window, not the area beside the side column. So with HUD scaling
  On and the modern fonts off, 640 by 480 is Compact at 1×, 800 by 600
  Regular at 1×, and 1024 by 768, 1280 by 720, 1920 by 1080 and 2560 by
  1080 Large at 1×; with the modern fonts on, 1920 by 1080 is Large at
  1.5×; 2560 by 1440 is Large at 2×, and 3840 by 2160 Large at 2× too, the
  column's scale of 2 holding 3× down. With HUD scaling Off, 1920 by 1080
  is Large at 1×. On a phone with touch controls (`match_fits_safe_area`)
  Settings is Compact, drawn at 1× and fitted to the safe area, as before.
- **Placement.** A screen says where it shows (`LayerPlacement::shown`) in
  window pixels, and how many points it has. Settings, a notice and a
  question centre themselves in the view's room at its scale
  (`centred_placement`): left = (window width − width at the scale) / 2,
  rounded down, and the same down, in the canvas less its safe insets on a
  touch canvas, the width at 150% each edge rounded to the nearest pixel
  (`kit::to_canvas`); input maps back by the same scale (`layer_point`). A screen anchored to a part of a 3.1c screen places itself by the
  picture's rectangle (`LayerView::picture`, where the 640 by 480 frame is
  drawn) at the same scale. The main menu's OA button stays in the frame.
  In a match Settings shows where `match_dialog` puts it: its class's
  dialog points at its scale, centred in the window's safe area; where
  that place would cover part of the side column and the safe area right
  of the column holds the dialog, it is centred across in that area
  instead. Input maps back by that scale. A 640 by 480 window shows it at
  (144, 78), pixel for pixel as before; 800 by 600 at (40, 57) and 1024 by
  768 at (32, 84), over the column, which the area right of it cannot
  hold the dialog beside; 1280 by 720 at (256, 60), right of the column;
  1920 by 1080 at (480, 240), centred, or at (368, 90) at 1.5×. A window
  that moves shows where the player put it (Moving windows, below). An
  empty place means the screen does not show: it neither draws nor takes
  input.
- **Moving windows.** Settings, the key editor, the Library, the map
  browser and the mutator and brain pickers are windows that move
  (`window_placement::moves`); notices, questions, the gallery and every
  other screen stay where they place themselves, and so does a window that
  fills its whole room or is fitted to its place, as the phone's Settings
  is. The layer moves a window as it shows it (`OaLayer::shown_placement`):
  its own placement, moved by its offset and kept on screen
  (`window_placement::shown_at`), so drawing, input, automation and
  `placement_of` all see the moved place, and an unmoved window's pixels
  are exactly as before. The offset is from the window's unmoved place, in
  points. Input that moves a window comes first (`take_window_move`): a
  press on the window's title, its first `kit::Role::header` item at a
  point no control holds, drags the window, a mouse's at once and a
  finger's once it has held 400 ms within 8 points, a finger that strays
  sooner dragging nothing (`TitleDrag`, `drag_move`); a double-click on the
  title puts the window back in its centre; Alt and an arrow move the top
  modal window 8 points, and with Shift 32. A dragged window snaps, across
  and down each on their own, within 12 points to its unmoved column or
  row, else to the room's centre, else to the room's near or far safe edge
  (`snapped`). It is kept on screen: at least 64 points of it across and
  down in the room, and its top, which holds the title, in the room; an
  edge less than a point from a safe edge lies on it. Each window keeps its
  place for each size class in the preferences,
  `open-annihilation.window.<window>.<class>` (`settings`, `key-editor`,
  `library`, `map-browser`, `mutator-picker`, `brain-picker`; `compact`,
  `regular`, `large`), as "x,y", the offset in points; an offset of 0, 0
  erases the key. The class is the in-match Settings' in a match and the
  view's on the front end. `reset_window_places`, which Settings › User
  Interface's Reset window positions calls, forgets every key that starts
  `open-annihilation.window.` and no other, writes the preferences and ends
  a drag under way; `set_window_size`, which its Window size applies at OK,
  writes `open-annihilation.ui.window-size` (Auto erases it) and lays every
  screen out again. A moved window keeps its kept front-end drawing.
- **Input.** One overlay at z 100 on every screen (`register_oa_layer`, over
  the extensions' overlays, under the frontend's message boxes) hands the
  layer every input. The key that closed a screen comes first: its presses
  do nothing until it is released. Then the top modal screen that shows
  takes every input, and nothing under it sees any. With none, each screen
  that shows sees the input from the top, and the first that answers other
  than `pass` takes it; what none takes goes on to the overlays and the
  screen under the layer, and in a match first to the in-game OA button.
  So a screen that is not modal sees every pointer move, mapped to its
  points even outside its rectangle, and keeps its own hover: a move that
  only changes its hover is answered `pass`. A screen that needs input where
  it draws nothing can take it through an overlay of its own above z 100
  that takes nothing while a modal screen shows. On the front end, where
  input arrives in the picture's pixels, a point is first mapped into the
  window through the picture's rectangle (`window_position`), then to the
  screen's points by its placement (`layer_point`): floor((pixel − left) /
  scale). A finger's press gets the reach of the touch controls in the
  screen's points, the pick distance over the window pixels a point takes;
  a mouse's none; keys go first to the screen's own shortcuts (`shortcut`:
  the Library's and the key editor's, and on Settings › Accessibility the
  practice pad's, which takes Shift, Ctrl, Alt, the letters and the digits
  so that they reach nothing else), then through `layer_key`. A key's
  release goes to the screen taking input (`key_up`); the practice pad is
  the one screen that follows it, and every other answers `pass`. Settings'
  screen also hands the pad its time each tick
  (`settings::dialog_practice_tick`). Outside a match F1 is the
  kit's information key, `Key::info`, which a screen with more to tell
  about what the pointer or the focus is on takes; a screen that answers
  `pass` lets F1 go on to the game as before, and Settings does. In a
  match F1 is the game's (unit information) and no screen is given it.
- **Opaque or not.** A screen is opaque unless it says otherwise
  (`opaque`): it covers every pixel of its place, drawn over black, as
  every screen did before. One that is not, such as a card beside a badge
  with its arrow outside the card, may take the whole window as its place
  and draw only what it shows: the layer draws it on a surface cleared to
  clear each time, once over black and once over white, and takes each
  pixel's opacity from what it leaves of the white and its colour from the
  one over black (`draw_screen`, `LayerDrawing`), so that what it fills is
  opaque in its own colour, what it leaves alone is clear, and what it
  blends takes the blend's opacity. The colours stay straight, never
  multiplied by their opacity, and a pixel is laid over what lies under it,
  the picture or the screens below, src over: (colour × opacity + under ×
  (255 − opacity)) / 255. That happens wherever the layer shows it: in the
  window (`present`, whose texture SDL blends the same way), in the frame
  (`compose_front_end`) and in a match's layer (`refresh_match`). Such a screen
  never darkens what lies under it, whatever `backdrop` says, and only the
  part of it that holds what it drew goes into the window's texture. The
  cursor stays above it.
- **Rounded corners.** The layer rounds what it shows, not each module's
  drawing: a placement carries `LayerPlacement::corner_radius`, in the
  screen's points. A window that moves takes its class's
  `kit::Metrics::window_radius` (10, 11 and 12 points at Compact, Regular
  and Large) from `shown_placement`, and Settings sets its own; a notice, a
  question and a work dialog take their class's `dialog_radius` (12, 13
  and 14) through `centred_box`; a screen that fills its room, and every
  other screen, stays square. `draw_screen` then rounds the opaque drawing
  (`round_layer_corners`): outside each quarter circle the pixels are
  clear, so the picture, the darkened menu or the battlefield shows there,
  and a ring a point thick along the arc takes the edge's light colour
  where the corner meets the top or the left and its dark colour where it
  meets the bottom or the right, continuing the window's bevelled edges.
  The shape is the kit's (`kit::corner_coverage`), stepped at every scale:
  a pixel is the window's or clear, so no blend rounds a corner pixel
  otherwise than the checks expect. A check that compares a screen with its own drawing does
  it through `expected_layer_frame`, which rounds the same way, or rounds
  a screen it draws straight with `round_drawn_screen`.
- **Typed text** goes to the screen taking input (`text`), and while that
  screen reports a `text_field` the system's text input stays started over
  the field, mapped back to the coordinates input arrives in
  (`Runtime::start_text_input`); it stops once no screen has one.
- **On the front end** each opaque modal screen with a backdrop darkens the
  frame itself, a blend in 256ths (`darken_front_end`), and the window round
  the picture, which SDL clears to black, takes black darkened as often, so
  the backdrop covers the whole window; every screen under it darkens too,
  as its drawing is darkened before it is copied: a question over Settings
  darkens the menu twice and Settings once. The screens are drawn through a
  `kit::Canvas` at the view's scale, crisp at a whole scale and smooth at
  150%, onto a picture of their size in points at the scale, never drawn at
  another scale and enlarged, and copied pixel for
  pixel to their places in the window's own pixels
  (`RenderState::use_window_pixels`); then the software cursor is
  presented above them, and the frame holds no cursor while a screen shows
  (`tick_and_draw_cursor`). The layer is uploaded again only when a
  screen's drawing or place changes. A screen whose revision covers
  everything it draws (`revision_covers_drawing`: the Library, the
  notices, the questions, the kit gallery and the extensions' kit
  screens) is drawn again only when its revision, its place, its scale,
  the screens darkening it, the fonts, the icon, the interface's language
  or its words, which a pack read again replaces in place
  (`interface_generation`), or the text settings change (`KeptDrawing`);
  every other screen, Settings among them, is drawn each frame. A check
  that reads what a frame draws drops the kept drawings before it
  (`forget_kept_drawings`), as the text-fit check does, since the kit
  records a text only while it is drawn; `kept_drawing` tells a check
  whether a present drew a screen again. Without a renderer, and while
  `frame_without_cursor` draws the picture, the screens are drawn into the
  frame instead (`compose_front_end`), each picture pixel taking the
  screen's pixel at its centre in the window, which is what the player sees
  where the picture fills the window. A check that compares the whole
  window with what it expects asks for the whole window in the frames it
  reads back (`read_whole_window`); otherwise the read-back holds the
  picture's area, as it always has. `oa_layer_check.hpp` gives the checks
  the frame expected with a screen open (`expected_layer_frame`), the pixels
  a frame differs in outside the cursor's square (`layer_differences`),
  pointer and finger events at a window pixel (`window_pointer_event`,
  `window_finger_event`, `layer_window_pixel`), and a screen of their own,
  opaque or not, that fills one small rectangle and notes the information
  key (`MarkScreen`), with which the layer's own check
  (`OaLayer::check_clear_screens`) holds screens that are not opaque and
  F1.
- **In a match** the layer is one picture of the window's size: while the
  in-game menu's column shows, the OA Settings button under Resume; the
  backdrop under a modal screen; and the screens at
  their places (one that is not opaque laid over by its opacity), each
  under a modal screen with a backdrop darkened as the button is. A screen
  that is not modal shows over the running match with no column, as the
  keys overlay does (`runtime_keys_overlay.cpp`); with no screen and no
  column the layer shows nothing. Each screen is drawn at its
  placement's scale as large as its place, Settings at its own class and
  scale on the kit canvas's crisp form at a whole scale and its smooth form
  at 150%, never drawn at 1× and enlarged, and copied into the layer pixel
  for pixel; the OA Settings button's face, at its source size, and on a
  phone the fitted Settings, drawn at a whole scale, are fitted to their
  places by the nearest pixel (`stamp`). The layer is drawn again only when what it shows changes
  (`refresh_match`), and the composed frame and the window take the same
  layer: it goes over the composed frame (`compose_match`) and is presented
  after the match's other layers and before the frontend dialogs' and the
  cursor (`present`).
- **The in-game OA Settings button.** The in-game menu has one OA button,
  under Resume, as wide as it from its left edge (`match_button_rect`, 96
  by 24 source pixels at (14, 455), placed in the column at its own scale,
  the phone's layout included). It is drawn in the menu's own art
  (`match_button_face`, through `oa/ui/frontend_renderer/room_parts.hpp`):
  a plate from the menu's BUTTONS0, its ends kept and its middle repeated,
  so a mod's skin carries through, the small OA stamp at its left end and
  "OA Settings" in the menu's caption font. It sinks while pressed, darkens
  under a modal screen as the screen does, and its release opens Settings.
- **Arrange overlays.** Settings › Accessibility's **Move overlays**
  (`EngineSettings::move_overlays`, off by default) turns the places on:
  `overlay_shown` gives an overlay its place only while it is on
  (`overlays_move`); off, every overlay shows where it lays itself out and
  the places stay kept. **Overlay places**' ARRANGE…, pressable while Move
  overlays is on, asks the host for `DialogAction::arrange_overlays`, which
  opens Arrange overlays over Settings (`open_arrange_overlays`,
  `oa_layer_arrange.cpp`), on the main menu and in a match: a modal,
  opaque screen named `arrange-overlays`, as large as the window, at the
  in-match windows' scale. It pictures the match in the kit's look at the
  window's proportions (`arranging_room`: in a match `overlay_room`; on the
  main menu the battlefield of a match laid out at the window's canvas,
  `Runtime::match_canvas_point`): the side panel, the top bar, the bottom
  bar, each named, and the battlefield with a grid. On it the keys overlay
  shows a short list (`keys_overlay_sample`: the first keys of a match being
  played, laid out as the match lays the panel out), the chat line a line
  being typed and the latch chips a latched QUEUE's chip, each where that
  overlay shows (`arranged_overlays_in`), with a dashed handle, a grip and
  its words, and a card with Reset, Cancel and Done where it covers the
  fewest handles, placed again as a handle is dragged under it. A drag
  moves a handle on an 8-point grid and keeps the place on release; Tab
  moves between the handles, Reset, Cancel and Done; an arrow moves the
  focused handle 8 points, 32 with Shift; a double-click on a handle
  forgets that overlay's place; Reset forgets every overlay's place at
  every class; Done keeps the places, and Cancel and Escape put them back
  as they were when the screen opened (`kept_overlay_places`,
  `restore_overlay_places`); either goes back to Settings.
  `oa_layer_overlays.hpp` (`overlay_placement`) holds the places: one for
  each overlay and size class, `open-annihilation.overlay.<keys-overlay|chat-line|latch-chips>.<compact|regular|large>`,
  each axis measured from the near edge, the far edge or the centre of
  the overlays' area, whichever third the overlay's centre lies in, on the
  grid, in the in-match windows' points, written as `l16,b8` or `c-24,t0`.
  The class is the in-match windows', so a 1280 by 720 and a 1920 by 1080
  match share their places. `overlay_shown` gives each overlay its place:
  where it lays itself out, pixel for pixel, while it has none or Move
  overlays is off, else moved to the place, as large as it is and kept
  wholly in the overlays' area (`Runtime::overlay_area`: the battlefield,
  or what the touch controls leave clear), never over the side column. The
  keys overlay's panel, the latch chips (`Runtime::draw_key_latch_chips`)
  and the chat line that stands over the battlefield
  (`Runtime::draw_risen_chat_line`) ask it where to draw. Reset window
  positions does not touch these keys. No OA notice shows over a running
  match, so there is no notices handle; one that does later adds its name
  to `overlay_placement::overlays` and asks `overlay_shown`.
- **Automation** lists what the layer shows (`automation_controls`), as
  [docs/automation.md](../../docs/automation.md#oas-own-screens) describes:
  `oa.button` (on the main menu its text says how many updates wait in the
  Library; in the in-game menu it is the OA Settings button, its text `OA
  Settings`) and, on the main menu, `oa.settings-button`, then the
  screens from the top down to the first modal one,
  each control and part named `oa.` and its kit name, with its exact
  rectangle in the window's pixels and that rectangle mapped back onto the
  canvas.

## The Library

`library_host.hpp`, `runtime_library.cpp` and `runtime_library_actions.cpp`
host the Library ([src/ui/library](../ui/library/README.md)) as a screen of
the OA layer on the main menu, named `library`: modal over a backdrop, laid
out at the view's size class (`library::window_size`, `library_layout`) and
centred in the window at its scale, or where the player moved it by its
title ([The OA layer](#the-oa-layer)), its corners rounded. Its model, its screen's state and its feed are
the host's (`Runtime::LibraryHost`, made on first use, joined as the
runtime goes).

- **How it opens.** The main menu's OA button opens it
  (`Runtime::open_library`) when a press is released over it, on Active,
  with the button's sound;
  Settings › Language's MORE IN THE LIBRARY… opens it on Languages over
  Settings (`DialogAction::open_library`). It opens only where the
  settings do: on the main menu, never over a message box, a frame a
  package owns or the saves notice. The layer never holds two Libraries or
  two Settings: opening one already on the stack goes back to it
  (`OaLayer::return_to`), so Library › SETTINGS… › MORE IN THE LIBRARY…
  returns to the first Library. The main menu's Settings button beside
  the OA button, Cmd+, (Ctrl+, elsewhere), the macOS menu's Settings… and
  the in-game OA button open Settings.
  `close_library` closes it and what stands over it. Each opening asks the
  content service for a refresh (`RefreshReason::library_opened`), which
  the player's setting and the once-a-minute limit govern; an unattended
  run asks nothing.
- **The feed** (`LibraryFeed`) gives the Library its inputs and takes its
  downloads. The runtime's own builds `Runtime::library_inputs` from the
  content service's snapshot (each registry, built in when its origin is
  built-in, its downloads off when it needs an install ID the player turned
  off, and each catalogue entry), the installed packages and updates the
  worker last found, the download queue (each item's state as the Library
  names it; a cancelled one left out) and the run (this build's version,
  the mod played, a match), and for the Active tab what the run already
  holds: the mod played as list_offered_mods finds it, the language shown and
  the first loaded pack that holds it, and the installed map packs' maps
  (`MapPacks`). GET goes to `queue_download`, UPDATE to
  `request_update` (below), CANCEL and RETRY to the queue's latest item of
  that package, and `library_registry_built_in` answers from the same
  snapshot as the inputs' `Registry::built_in`. An update queued from the
  Library keeps what it changes (`KeptUpdate`) for its queue line while the
  queue lists it. What the installer noted of a mod chosen here
  (`ModInstallState::library_notes`) goes on its queue item as the model is
  built, whatever the feed: why the installer refused it, in its own words,
  and, while its change waits for the player to leave the mod being played
  (`package_install::pending_change_target`), that it is held ("Waiting ·
  finishes when you leave Ridge").
  `tick_library` rebuilds the model, at most once a frame on the main
  menu, whenever the feed's generation moves (the service's, the queue's,
  the worker's searches and its pictures, added). It takes a finished
  check on every screen but a match's, because the picker asks for checks
  over SKIRMISH, NEWGAME and the battle room; the Library's model is
  rebuilt the next time the main menu shows.
  The runtime's own feed answers `remembered_verdict`, `start_check` and
  `keep_verdict`. A Checked fact is the verdict the file holds for that
  mutator with its prerequisites above it, every one at its default
  options, against the mod being played. Check with this mod prepares that
  list on the main thread and the Library's worker runs the dry run; the
  worker does not touch the live asset store. The verdict is kept when the
  worker finishes, back on the main thread, and a later rebuild reads it
  from a cache keyed the same way, not by running the dry run again. The
  rows' keys are made in one pass: the worker's listing from its last
  search (a feed of its own lists the folder), the verdict file and each
  shown list resolved from that listing. A rebuild keeps the pass until a
  search finishes, the installed mutators change, or the mod key or the
  clash mode moves, so a download, which rebuilds the Library on each
  frame, lists nothing and reads no package. The worker's search makes the
  digest of each mutator folder that has no package SHA-256, off the main
  thread, and the pass and the picker take it from that cache
  (`FolderDigests`) while the folder is unchanged. A compatible verdict with no
  counts reads that it changes other game data, and one whose last
  mutator declares maps reads that it changes the map when a match
  starts. With
  no mod played, the fact uses the base game's own sentence. A cycle, or
  a prerequisite that is not installed, starts no check and leaves the
  verdict unchecked.
- **The worker** (`LibraryWork`) is one thread of the Library's own, made
  from `oa-base-threads` and started at its first job. After a catalogue
  refresh (the service's generation moved), a package the queue installed,
  and a roll back in Settings, it lists the player's packages
  (`list_installed`), matches each to the catalogues, adopts each file
  install a catalogue lists (`adopt_match`), finds the updates
  (`find_updates`) and reads each installed mod's own picture; the main
  thread reads what it last found under its lock and never does that work
  itself. It also reads and fetches the badge pictures
  (`content::PictureCache`, with its own HTTP client and no install ID)
  for the rows the Library shows, never while a match runs. The host's
  destructor stops it and joins it within a second, a fetch included.
- **The dot and the count.** The main menu's OA button wears the kit's
  attention dot (`kit::draw_menu_icon_button`) while `library_update_count` is
  above 0: the updates the Library's last model counts as installable, so
  the dot shows from the first frame the cached catalogue offers one. Its
  bottom-right badge is the number of mods that model lists (the Mods tab's
  All), or a spinner while a fetch has not yet produced a list. With no
  updates the button is that face and that badge.
- **The actions.** Open folder shows the package's folder
  (`open_player_folder`) and HOMEPAGE opens its page (`open_web_link`),
  both recorded in a run nobody watches. SETTINGS… opens Settings over the
  Library and CLOSE closes it. UPDATE, Update all, Play now, Switch off,
  Roll back and Check now go through `library_entry_action`
  (`runtime_library_actions.cpp`): UPDATE calls `Runtime::request_update`,
  the one place an update reaches the download queue
  (`DownloadReason::update`). An added registry's mod whose rules hash changes or cannot be known
  (`library::asks_before_update`) asks the update question first
  (`update_question`), pushed at once over the screen that asked, Not now
  marked; its UPDATE queues the update and Not now leaves it. Every other
  update, every built-in registry's among them, is queued at once. UPDATE
  ALL calls it at once for every update that can act, is not queued and
  asks nothing, in list order, and asks one question about the others
  (`update_all_question`), whose UPDATE queues them in list order; Play now
  switches the game to the installed mod's folder (`switch_to_mod_folder`,
  the soft restart), and a folder it cannot play is told in a note over the
  Library (`library::switch_refused_note`); Switch off, on the mod played,
  asks the Switch Mod question of No Mod (`library::switch_off_question`),
  pushed over the Library at once, CANCEL marked, whose SWITCH plays no mod
  through the same switch. Either switch notes what the Library shows
  first (`keep_library_for_restart`, a `LibraryReturn`) and the line its
  progress dialog says (`restart_line`), and the next run opens the
  Library again as it showed
  ([the soft restart](#mod-profile-and-mod-folders)); Roll back asks the
  Roll Back Mod question
  (`library::roll_back_question`, with the reload line for the mod being
  played), pushed and marked the same way, and its Roll back rolls the
  folder back (`roll_back_mod_folder`), the feed looking again afterwards,
  a refusal told in MOD NOT ROLLED BACK over the Library. Every refusal
  shows in the status line too. Play now and Switch off do nothing during
  a game or while `--mod-dir` or `--base-game` chose the mod, where the
  Library greys them. Check now asks the registries again
  (`content::RefreshReason::check_now`, due whatever Check for updates
  says). UNINSTALL goes there too, to
  `library_uninstall`: for the mod being played or the language pack in use
  (`Installed::playing`, `Installed::in_use`, which `language_pack_in_use`
  fills, or `package_target_in_use` and `language_pack_in_use` now) it shows
  the notice of what comes first and removes nothing; otherwise it asks the
  Uninstall question (`library::uninstall_question`), whose UNINSTALL looks
  again and moves the folder in the player's Mods, Maps or Languages folder
  aside whole (`package_install::uninstall_package`) for the installer's
  `Discarder`, a folder anywhere else being refused. The Library's own
  keys are Cmd+F (Ctrl+F elsewhere) and `/` outside the search for Find, and
  Ctrl+Tab and Ctrl+Shift+Tab for the tabs; Cmd+, opens Settings over it.
- **Key layouts and USE.** The host gives the model `Library::key_hooks`.
  When USE's question opens, its `moves` reads the player's Keys folder and
  resolves the layout in use with its own changes, and the new layout with
  the own changes it would hold under Keep them on top and under Start
  clean, as `apply_library_key_use` would leave them, on the player's
  keyboard (`Runtime::key_map`) with the played mod's letters
  (`Runtime::key_mod_data`) and the pointer interface in use
  (`key_moves_between`); What moves lists each command whose first key
  differs, and each command on one of the player's own keys. The answer
  goes to `Runtime::apply_library_key_use`, which writes the layout
  preference, shared or for the played mod, switches on the helpers the
  question left on, and copies the old layout's own changes to the new
  one's own folder only under Keep them on top and only when the new
  layout has none; it never deletes or writes over a player's own
  changes. It returns what it changed (`Runtime::LibraryKeyUseUndo`: the
  layout preferences before, the engine settings before, this run's latch
  and hold lists, and the own folder only when USE made it). The Library
  then pushes a question over itself that says where the layout is used
  ("Using Lefty for every game.", or "… with {mod}."), with OK and Undo;
  Undo calls `Runtime::undo_library_key_use`, which puts all of that back,
  removes only an own folder USE made, and loads the layout again. EDIT
  KEYS… opens the key editor.
- **The check.** `native-library` (`--check-library`,
  `runtime_library_check.cpp`) gives the Library a feed of made-up
  packages and no network, with its data and the player's folder in the
  build tree, at 640x480 and 1920x1080: the button's count of the fixture's
  two mods and one map pack (`oa.button` "Library · 3 packages"), and of
  that map pack alone ("Library · 1 package"), the Library opened on
  Active, its lines read and taking no press, and closed, the dot and
  `oa.button`'s "Library · 3 packages · 2 updates" with two, the Library on
  Active and its Updates tab on its two updates,
  the Keys tab reading "Keys (3)" with OA default first over layouts the
  check writes into its Keys folder (Grid Test in use with the player's
  own change, Patrol on J, and Lefty Test), USE asking where first, then
  about the own changes, then listing what moves, writing the preference,
  keeping the own changes on top at 640x480 and starting clean at
  1920x1080, Grid Test's own changes never lost, and Undo on the notice
  after USE putting Grid Test back and removing only the own folder USE
  made, GET and CANCEL reaching the feed, Open folder and HOMEPAGE through the
  recorded openers, Roll back's question over the Library, Play now's soft
  restart, Settings over the Library and the Library over Settings, never
  two of either, the shortcut opening Settings, and in a skirmish the
  in-game OA button opening Settings and the Library opening nowhere.
- **Installing from the Library.** A mod the player chose in the Library
  reaches the installer as F10's hand-off posts it: an oamod package whose
  inbox origin (`Incoming::origin`) is a catalogue, which is the only way
  it is told from a file. Its registry decides what it asks, by
  `library_registry_built_in` of that origin's registry, asked once:
  - a built-in registry's asks nothing: an update, a reinstall or another
    version replaces whatever copy the folder holds (the same registry's,
    another's, a file's or one with no record), the copy going to
    `.backup` for Roll back; a folder of the player's own that does not
    hold the mod is never replaced nor joined, and the install is refused;
  - an added registry's replaces its own copy, and a copy with no record,
    with no question; asks REPLACE MOD (CANCEL, REPLACE) over another
    registry's copy of the same mod; asks FOLDER IN USE beside a folder of
    the player's own; and over a file's copy asks what the file would.

  It starts on the settled main menu, and also while the Library is the top
  screen over the main menu (`catalogue_mod_may_start`), never in a match,
  on a loading screen or under Settings, a notice or a question; a file
  waits for the settled main menu as before. While the Library shows, its
  questions are pushed over the Library at once, and its progress, a
  refusal ("Mods/cinder is a folder of your own, not Cinder. Move or rename
  it, then RETRY.") and its outcome show in the Library's queue rather than
  in a prompt, the entry then offering Play now; with the Library closed,
  its questions wait in the OA layer's queue and its prompts show as a
  file's do. Catalogue map packs and language packs keep their own rules.

  In a run nobody watches (`UserFolderState::unwatched`: unattended, on CI,
  or on a video driver that shows no window, such as SDL's dummy driver),
  such a mod asks and installs by the same rules, but starts only while the
  Library is the top screen, where whoever drives the run asked for it: a
  built-in registry's asks nothing, and an added registry's question is
  pushed over the Library and waits for its answer, which the automation
  endpoint can press (`oa.prompt.replace`); the folders its changes drop
  are deleted a step a frame while the Library stays on top. With the
  Library closed it waits in the inbox. A file opened outside the game and
  any other package without a catalogue origin still wait for a run
  someone watches, a catalogue map pack keeps the rule below, and a
  catalogue language pack still installs. A run nobody watches and nobody
  drives never shows the Library, the map browser or GET MAP: a package
  the download queue left unfinished in an earlier run, which it restores
  at start while the catalogue still offers that release and hands over
  again once its file is whole, waits in the inbox, its queue item
  installing, and stays in the saved queue as the game quits, for the next
  run someone watches or drives.

  A catalogue map pack installs in a run nobody watches as in one someone
  watches, with no prompt, but only while the player is getting one: while
  the map browser (`map-browser`) is the OA layer's top screen, as it stays
  after Download and select, or while GET MAP shows over the battle room
  (`ModInstallState::get_map_shows`: the battle room shows with no dialog
  over it, and GET MAP has its line there). A catalogue mutator installs
  the same way while GET MUTATORS shows over a guest's battle room
  (`ModInstallState::get_mutators_shows`). A pack that does not fit TA
  3.1c is refused as in a watched run, the download queue told so, and a
  pack whose plan asks waits, as it does off the main menu, for the
  settled main menu of a run someone watches. Off those screens,
  on the main menu and over SELMAP with the browser closed among them, it
  waits in the inbox, so a pack an earlier run's queue hands over never
  installs into a run nobody drives. The folders an install drops are left
  for a main menu someone watches, or the Library on top of one.
- **Held changes and the quit path.** A chosen mod's update to the mod
  being played is unpacked and checked as a file's, then held in
  `set_pending_change` with no soft restart; the download queue is told
  it is installed, so that it is not fetched again. Switching mod makes the
  change as a soft restart does, and quitting makes it too: `main()` calls
  `finish_pending_change` after every run, on the way out as after a soft
  restart, with the runtime and its archives gone. After a crash the next
  start's `recover_package_installs` settles the staged files away, the
  folder whole. CANCEL on that queue line drops the held update
  (`discard_pending_change`): the staged files are settled away the same
  way, the mod being played stays as it is, the line goes and the Library
  offers the update again. It waits for UPDATE or Update all; nothing
  fetches it on its own. A roll back waiting in its place is left.
- **The install check.** `native-library-install`
  (`--check-library-install`, `runtime_library_actions_check.cpp`) posts
  made-up mod packages with catalogue origins as the hand-off does, the
  Library showing over a feed of three registries (`check-builtin` built
  in, `check-added` and `check-added-2` added), and checks each rule above
  over four runs: the questions each registry asks or does not, the
  `.backup` and origin records after each, the refusals in the queue and
  told with the Library closed, a file still asking UPDATE MOD, Roll back
  and Play now from the Library, and an update to the mod played held,
  named by `pending_change_target`. CANCEL on that line drops it, the
  update is taken again and held, and it is made as the mod is switched.
  The second and third runs, which Play now began, check the Library
  opened again: the work dialog waiting for a notice raised at the start,
  then saying "Switching on Ridge…" until the fixture lists the installed
  packages, the Library on Mods with the mod played selected, before the
  fixture lists that mod and once it does, its scroll kept or its row
  brought into view, and a Library the run before kept never kept for the
  next run, dropped once the wait is over and not opened when the notice
  holding it back closes.
  The third run also checks UNINSTALL: off for a map pack during a game, a
  notice for the language pack in use and for the mod being played, the
  question for a built-in registry's mod and for one of the player's own,
  and each folder gone, with the copy Roll back keeps, once answered.
  The fourth run is one nobody watches, on the dummy video driver without
  the prompts the other runs show: a built-in registry's update waits in
  the inbox with the Library closed and installs with no prompt once the
  Library shows, an added registry's copy asks REPLACE MOD over the
  Library, which waits for its answer until REPLACE installs it, and a file
  opened outside the game stays in the inbox. After those fixture steps
  the check swaps in the runtime's verdict entries and dry-runs two
  made-up packages on the Library's worker against the installed game:
  one that changes every builder, whose Checked fact and verdict file say
  compatible, and one that matches no unit and reads the no-effect words.
  Rebuilding the Library shows the same fact and does not dry-run again.
  A FIXTURES_SETUP test
  (`library-install-clean`) clears its folders before each run.
- **The update question check.** `native-library-update-question`
  (`--check-library-update-question`, `runtime_library_actions_check.cpp`)
  installs made-up Ridge from an added registry and offers an update whose
  rules change. At 640x480 the row's details page has no tabs. Not now
  leaves that page. UPDATE, fifty times, leaves the Library on top with its
  tabs on the next frames, and it is still there once the update is no
  longer offered. A FIXTURES_SETUP test (`library-update-question-clean`)
  clears its folders before each run.
- **Limitations.** A held update is replaced by a Roll back of the mod
  being played, which waits for the run to end in its place; the download
  queue has counted the update installed by then. CANCEL on the held line
  does not drop that roll back. In a run nobody watches, a file opened
  outside the game that waits first in the inbox holds back every package
  behind it, a mod chosen in the Library among them, until a run someone
  watches takes it.

## The map browser

OA's map browser shows every map a player could play as one grid of cards:
the base maps the start's scan found (`BaseMapSummary`), the installed pack
maps (`map_packs()`), and the pack maps the catalogues offer (the content
service's snapshot, entries of kind `oamap`). Its model and screen are
[src/ui/map-browser](../ui/map-browser/README.md); `runtime_map_browser.cpp`
hosts them on the OA layer.

- **MORE MAPS….** A screen of the layer of its own (`more-maps`, not modal,
  no backdrop), kept on the layer while the skirmish setup's map picker
  (SELMAP) shows, or the host's map dialog in the battle room
  (`multiplayer_map_list_rect`), by `keep_more_maps_screen`, which
  `OaLayer::tick` calls once a frame, and taken off when they close. It is
  one kit button, the lime accent button at a footer button's 17 points,
  its caption in the small font with a button's padding, taller (up to the
  compact control height) only where the rows down to the description
  box's drawn border are clear, `oa.more-maps` to automation, directly
  under the picker's
  MAPNAMES list with its right edge on the list's, or above the list when
  that place meets another gadget of the picker, at the layer's scale. It
  hides while a message box shows. A mouse presses the drawn rectangle. A
  finger presses the button when it is at least as near as any gadget of
  the picker, the map list included; a point inside a gadget stays that
  gadget's. It also takes the controller's Y and Cmd+F (Ctrl+F elsewhere),
  which the pickers leave
  unused, and answers `pass` to everything else, a pointer move that only
  changes its hover included, so the picker sees every other click, key and
  move as before.
- **The browser.** Pressing MORE MAPS… pushes the browser (`map-browser`),
  modal over a backdrop, centred at its size class's dialog size; it closes
  when its picker closes or a match starts. The controller's A and B are
  Enter and Escape there, the D-pad the arrows, Y gives the search the
  focus and the right stick scrolls the grid. Its cards: base maps, which fit;
  installed pack maps, whose fit is P08's `pack_map_fit`, checked as each
  card comes into view or is selected (a failure is the mod's fit while a
  mod plays, the game's otherwise); and online maps, whose fit is the
  catalogue's `compatible` table, `ta-3.1c` for the game and
  `<id>@<release>` for the mod being played, its release from the mod
  folder's origin record when it came from a catalogue, and unknown
  otherwise. An online map whose name an installed pack already gives shows
  once, as installed.
- **Pictures.** The browser keeps its own preview cache
  (`oa::ui::map_previews::PreviewCache`): the layout asks for the cards in
  view and the row after them, and each frame the browser forgets the rest
  (`new_frame`), decodes up to four (`pump`) and lays itself out again
  before anything draws. A base map's picture is its terrain file's header
  and minimap; an installed pack's is its preview file; an online map's is
  the catalogue picture the Library's worker reads or fetches through its
  picture cache (`PictureCache::request`, `take_bytes`), checked against its
  SHA-256 name, never during a match.
- **Select.** Select moves the picker's selection to the map and closes the
  browser; the player still chooses the map with the picker's LOAD. In
  SELMAP a pack installed since the list was made joins it in the name
  order first, then `preview_map_index` selects and previews it; in the
  battle room `multiplayer_offer_map` builds the screens' map list again
  when it lacks the map, fills the dialog's list, selects the row and
  previews it, sending nothing.
- **Download and select.** An online map's pack is queued as the Library's
  GET queues it (`queue_download`, reason `install`), never during a match;
  the browser stays open with the download's progress in its footer, and
  once the installer has put the pack in place and `map_packs()` lists the
  map, it acts as Select. A failure, the installer's refusal among them,
  shows its reason in the footer, and the button comes back. Cancel closes
  the browser and changes nothing; a download under way goes on in the
  queue, which the Library shows. In a run nobody watches the pack
  installs while the browser stays the top screen, and waits in the inbox
  once it closes ([The Library](#the-library)).
- **The check.** `native-map-browser` (`--check-map-browser`,
  `runtime_map_browser_check.cpp`) writes the map pack check's two packs
  (`write_map_pack_check_packs`) into its own `--user-folder`, turns every
  built-in registry off there before the content service starts
  (`prepare_map_browser_check`), and at 640x480, 1280x720, 1920x1080 and
  2560x1440 opens SELMAP and presses MORE MAPS…. It requires the layer's
  size class and scale (Compact 1x, Large 1x, Large 1x, Large 2x) and the
  grid's columns (3, 6, 6, 6); MORE MAPS… over none of SELMAP's gadgets,
  in the Classic and the Modern look the accent button at a footer
  button's size under the list's frame, clear of the title and, where it
  grows, of the description box's border;
  every card and control inside the browser's window; Online empty;
  Installed packs showing the two check maps alone; the clashing map's card
  greyed, "New names" in the footer and Select disabled; the search "isle"
  leaving one map; and Select on `check_isle@oa-check-isles` closing the
  browser with SELMAP's selection on it. It writes each size's frame as
  `map-browser-<w>x<h>.png` in its folder for review. The run is one
  nobody watches, on the dummy video driver with no check showing the
  installer's prompts, and last it posts the two packs again under ids of
  their own, as the download queue hands a catalogue's over: the fitting
  one waits in the inbox, nothing asked or installed, on the main menu and
  over SELMAP with the browser closed, then installs with no prompt once
  MORE MAPS… opens the browser, its folder holding the catalogue's origin
  record and the installed packs listing its map; the clashing one is
  refused there for New names, with no prompt. Then it hosts a battle room
  alone (`--dplay-port 0`), presses Select Map, and at 640x480, 1280x720
  and 1920x1080 requires MORE MAPS… over none of the map dialog's gadgets
  with the same look and place, writing
  `more-maps-<look>-selmap-<w>x<h>.png` and
  `more-maps-<look>-room-<w>x<h>.png` for each look. A FIXTURES_SETUP test
  (`map-browser-clean`) clears its folders before each run, and the check
  removes the packs an earlier run installed as a catalogue's.
- **Limitations.** The fit of an installed pack map is checked on the main
  thread as its card comes into view, once per pack. A map pack installed
  while SELMAP is open joins its list, when the map fits, as Select or
  Download and select chooses it.

## Mutators

SKIRMISH, NEWGAME (and Any Mission, which uses the same panel) and the host's
battle room carry Mutators as one of the room's own on and off buttons,
drawn from the room's art (`brain_seats_room.hpp`,
[src/ui/frontend-renderer](../ui/frontend-renderer/README.md)): COMMONGUI's
on and off button, its plate and its lamps, with the engraved OA stamp at
its left and the word "Mutators" after it. The count is not on the button.
It lives in the hover card and in the automation text
(`mutators_automation_text`): "Mutators · off" while none is on, or
"Mutators · {count} on", its first word the button's word as drawn, ended
with "…" only where the button cuts it. Its lamp is unlit while none is
on, green while mutators are on, and red in the battle room while a seated
player lacks one. The word is drawn in the room's caption font, else its
label font, and cut with … only when even that does not fit. The button
does not grow for its words. No part of it is the
kit's, so a mod's art and palette carry through. It is a screen of the
layer of its own (`mutators-button`, not modal, no backdrop, not opaque,
so the picture shows round it), which draws only the pixels the button
covers over the room, at the room's own scale. `keep_mutator_button_screen`, which
`OaLayer::tick` calls once a frame beside `keep_more_maps_screen`, pushes it
while one of those screens shows, and the screen closes itself otherwise.
On the skirmish setup and the campaign launch that is while no match is
running and no front-end dialog is up. MORE MAPS… and Mutators never show
at once: the map picker is its own screen.

- **Placement.** Every place and size is worked out from the 3.1c
  gadgets' rectangles on each window (`oa_button_place`). The button does
  not grow for its words. On SKIRMISH `place_in_column` makes it as large
  as the gadget under Difficulty in its column (Previous Menu), with the
  same left edge, centred down the space between Difficulty and that
  gadget. On NEWGAME and Any Mission `place_at_column_head` makes it as
  large as Previous Menu, its left edge, at the head of Difficulty's
  column, over the Set Difficulty label and under the side bar. Where that
  meets a gadget, `place_under_middle` makes it as large as the SELECT SIDE
  middle of the side bar: the space between the gadgets Arm and Core, set
  in from each by the bolt between them, nine twentieths of the bar's
  height (`bar_middle`), and stands it under that middle, the left edges
  together, a bolt's width under the bar. When neither is clear
  `place_in_column` is used, and when that is not clear either
  `place_block`, MORE MAPS…'s rule, at the button's own size. The screen's
  place also covers the room's focus marker round the button and the open
  card.
- **Focus.** The button is pushed onto the screen's own Tab order. Tab
  from the record just before it, and Shift+Tab from the one just after,
  hand the focus over: the GUI's focus goes to none and the room's own
  focus marker shows on the button. On SKIRMISH, Down from Difficulty and
  Up from the next record in
  its column do too, and the arrows hand the focus to the nearest record
  that way. On NEWGAME and Any Mission, Up and Down are not taken: they
  step the campaign lists. While the button holds the focus, Left and
  Right are consumed there so they do not move the GUI's focus. Return,
  Space and the pad's A press the button, and Y opens the picker. A mouse presses the drawn
  rectangle. A finger presses the button when it is at least as near as
  any gadget, Difficulty included; a point inside a gadget stays that
  gadget's, so a tap on Difficulty steps Difficulty. A press anywhere else
  clears the focus. A locked button, grayed as a grayed button of the room
  is, still takes a press on its rectangle
  and opens nothing. The pad's R1 and L1 are Tab and Shift+Tab, as on every menu, and
  `take_pad_action` offers an action to each screen that shows, from the
  top, until one takes it. A modal screen is the only one offered it, and
  an action it passes on becomes a key or a click as before. The pad's
  button badges leave the button as it is.
- **The card.** Resting the pointer on the button, or the information key
  while it holds the focus, shows a kit hover card beside it
  (`mutators_card`): "No mutators on", "1 mutator on" or "{count} mutators
  on", then one row a mutator in the order they apply (a guest's missing
  one reads "{name}: not installed" in the lock colour), "and {count} more"
  past six, then "Applied in this order." and "Click to change." on the
  host's button or "Click to read." on a guest's. It stays while the
  pointer is on the button or the card, and Escape closes it.
- **The set.** Each setup keeps its own chosen set. Skirmish's is emptied
  when SKIRMISH is entered from anywhere but the map picker and itself, so
  it holds across SELMAP and a refused start. The campaign's is emptied
  when NEWGAME or Any Mission is entered from Single Player or the main
  menu, and holds through the briefing and every mission until the player
  leaves to the main menu, which empties both sets, so a campaign save
  loaded from there never starts with an earlier run's set. The button
  opens the picker with the chosen set, or, when that is empty, with the
  setup's remembered last set. Use these replaces the chosen set. CANCEL
  changes nothing. START, and each campaign mission, hands the chosen set
  on: an empty set starts the game as before, and a set that cannot be
  read refuses the start as any failed start does. The button reads
  "Mutators". Its text for automation counts the chosen set: "Mutators ·
  off" with none, "Mutators · 4 on" with four.
- **The check.** `native-mutator-button` (`--check-mutator-button`,
  `runtime_mutator_button_check.cpp`) uses the mutator picker's feed and a
  scratch preferences file holding Gatehouse with Gatehouse Keep for
  skirmish and Fast Builders for the campaign. At 640x480 and 1280x720 it
  requires the button's size and place, no part of the kit, the word
  "Mutators" and the automation count, the card with four chosen, the pad's badges leaving it as it is,
  Tab, Down, a finger's reach, Y, SELMAP keeping the
  set, START handing it on, and a return to the main menu clearing it;
  then NEWGAME's size and place, Tab, the remembered campaign set and the
  first mission.
  With the picker open, the D-pad and A must go on as keys.
  A start refused for a chosen mutator that is no longer installed must
  name it and keep the set.
  The main menu must empty both sets.
  The button must not read the feed's list on later frames.
  It writes `mutator-button-skirmish-<w>x<h>.png` and
  `mutator-button-newgame-<w>x<h>.png`, and with four chosen
  `mutator-button-skirmish-four-<w>x<h>.png` and
  `mutator-button-newgame-four-<w>x<h>.png`, and with the card open
  `mutator-button-<screen>-card-<w>x<h>.png`. `--check-text-fit` paints
  SKIRMISH with none and five chosen, and NEWGAME, in every language and
  both sizes, and requires the words whole.
  A FIXTURES_SETUP test (`mutator-button-clean`) clears its folders before
  each run.
- **The battle room.** The host's battle room shows the same button as
  large as Select Map, directly over it with Select Map's edges
  (`place_over`), centred down the space between the player rows and
  Select Map. It reads "Mutators"; its text for automation counts the
  room's list. Where that place meets a control, MORE MAPS…'s rule at the
  button's own size (`place_over_anchor`). The button does not grow for
  its words. Its lamp is green while
  mutators are on and red while a seated player lacks one. Y, Return,
  Space and a
  press open the picker for multiplayer, on the room's set, or on the
  remembered multiplayer set when the room has none. Use these stores that
  set as the room's. CANCEL changes nothing. A guest's battle room shows
  the same button for the host's set, read-only, described under
  [A guest's Mutators in the battle room](#a-guests-mutators-in-the-battle-room).
  While a launch locks the host's options the button stays, grayed, and a
  press, Return, Space and Y open nothing. The waiting
  text is
  [src/ui/frontend-multiplayer](../ui/frontend-multiplayer/README.md).
- **The chat.** Every machine posts a local line when the room's list
  changes: the host from its own list, a guest from the host's list as its
  records carry it, a mutator it lacks named by its id, and nothing while
  that list is still arriving. "*** {host} turned on mutators: {list}" names the host and the
  mutators, continued as "*** {list}" when the names do not fit one line,
  and "*** This game is Open Annihilation only, and unranked." when the
  room goes from none to some. "*** {host} turned mutators off." when it
  goes to none. The same list posts nothing. The host also posts
  "*** {player} does not have {name}." once each time a seated player is
  newly missing a mutator. A guest posts "*** You do not have {name}. GET
  MUTATORS downloads it from {registry}." once each time the ids of the
  mutators it lacks change, with the name and registry it knows then; a
  name or registry its catalogue gives later posts nothing more. It posts
  "*** You do not have {names}. GET MUTATORS downloads them from
  {registry}." with the first lack's registry when several are
  missing. When no registry offers the first lack, "*** You do not have
  {name}." or "*** You do not have {names}." posts alone. A sentence that
  does not fit one line splits after its first full stop, ". " or the
  Chinese "。", and a further name continues as "*** {list}". A line
  wider than the chat list, measured in hattfont12, the list's font, breaks
  at a space and goes on in a further line that starts "*** ". With Unicode
  chat off each line, player names included, is written in the 3.1c font's
  code page (`oa::netgame::chat_code_page`), so an accented letter is one
  glyph; with it on the line stays UTF-8. The two lines about a 3.1c player
  ("*** {player} plays Total Annihilation 3.1c, which cannot play with
  mutators." and "… and cannot join a game with mutators.") come from the
  interface catalogue in the language shown and are fitted the same way:
  the battle room is bound to the list's measure
  (`mp::multiplayer_bind_chat_measure`, `Runtime::chat_list_width`).
  Nothing is sent to the other machines.
- **Beside START.** While mutators hold the host's START and a seated
  player lacks one, the host reads "Waiting for {player}: {name}" just
  before START, ending before the doors drawn round it. A guest whose READY
  is waiting on a mutator reads "Ready once {name} is here" in that place.
  Both are drawn in the room's label font; automation lists the host's as
  the label `oa.mutators.waiting`. The line does not enable or disable
  START or READY.
- **The room check.** `native-mutator-room` (`--check-mutator-room`,
  `runtime_mutator_room_check.cpp`) hosts a battle room alone and uses the
  mutator picker's feed and a scratch preferences file holding Gatehouse
  with Gatehouse Keep. At 640x480 and 1280x720 it requires the button as
  large as Select Map, over it with Select Map's edges and no widening,
  with no part of the kit. The button reads "Mutators" and meets no control
  of the room. Its text for automation reads "Mutators · off". Y opens the
  remembered set, Use these stores it and the chat reads the turned-on
  lines, and at both sizes the text reads "Mutators · 2 on" with its lamp
  lit, and "Mutators · 4 on" with four on. Turning them off posts the off
  line. Then,
  in French at 640x480 with Unicode chat off and the host named "Jörg" in
  the code page, as a 3.1c player's name arrives, Use these posts lines
  that hold é and ö as one code-page byte each, that each fit the chat
  list in hattfont12, and whose unranked line wraps with every word kept.
  Then the room plays Ridge Tough alone, and at 640x480 in English, German,
  French, Spanish and Italian, and in Simplified Chinese when its pack is in
  the check's user folder, Visitor is seated in slot 1: on 3.1c, both 3.1c
  lines post in that language with every word, each stored line within the
  chat list; on Open Annihilation without Ridge Tough, the line beside
  START reads "Waiting for Visitor: Ridge Tough" in that language, uncut.
  Each language writes `mutator-room-3.1c-<tag>-640x480.png` and
  `mutator-room-waiting-<tag>-640x480.png`.
  With the room's options locked the button stays disabled and neither a
  press nor Y opens the picker.
  Hosting alone shows none of the guest's controls, and the guest screen
  has no place while it is on the layer. It
  writes `mutator-room-host-<w>x<h>.png`, with two on
  `mutator-room-host-on-<w>x<h>.png` and with four
  `mutator-room-host-four-<w>x<h>.png`. A FIXTURES_SETUP test
  (`mutator-room-clean`) clears its folders before each run.

`native-brain-room` (`--check-brain-room`, `runtime_brain_room_check.cpp`)
hosts a battle room alone and checks each computer row's brain at 640x480
and 1280x720 ([Brain seats](#brain-seats)): with no brain installed no
seat shows and the frame matches one taken with the screen closed; with
Steady Commander remembered at firm a new computer's Resolution cell
reads "Steady Commander 1.2 · Firm" and its Mem cell shows Firm over the
room's greyed Hard, the cell meeting no control of the room but its row's
empty screen-size column; the notch on the computer's name opens the
brain menu, drawn as the room's list box, and Escape closes it; the
menu's built-in player gives "Built-in · Hard", its level before gives
Medium, and the Mem cell steps the built-in player to Hard, Easy and
Medium; Use for every computer seat and Lantern Scout set both rows. It
writes `brain-room-host-<w>x<h>.png`, and with the menu open
`brain-room-menu-<w>x<h>.png`. A FIXTURES_SETUP test (`brain-room-clean`)
clears its folders before each run. `native-net-loopback-brain`
(`tools/check_native_net_brain.py`) seats Steady Commander at firm on the
host's computer. The brain places four frames late in the build window and
then stops, so the engine's computer player can strike, and the check
requires that seat's line once. `native-net-loopback-brain-starts`
(`tools/check_native_net_brain_starts.py`) plays fixed starts with a
computer player on each machine, whose seats print every player's start by
name; the two must agree. The host sends each machine only its own
players' starts and deals fixed starts in its own slot order, which the
other machines' slots need not follow. Network play therefore notes
another machine's player at the start worked out from the host's slot
table under fixed starts (`net_match_fixed_start_positions`), and at none
under shuffled ones, when a brain reads only its own.

## The mutator picker

The mutator picker is one modal screen on the OA layer (`mutator-picker`),
centred over the screen that asked, at the size class's dialog size, a
window that moves by its title ([The OA layer](#the-oa-layer)). Its
model and screen are
[src/ui/mutator-picker](../ui/mutator-picker/README.md);
`runtime_mutator_picker.cpp` hosts them. Mutators on the skirmish setup, the
campaign launch and the host's battle room opens it, and a guest's
Mutators button opens it read-only. It closes as Cancel
when that screen goes or a match starts.

- **The request.** `open_mutator_picker` takes a `MutatorPickerRequest`: the
  setup, the set it opens on (each mutator with its options), whether a
  guest is only looking, and two calls. Use these calls the first with the
  set, in order with its options, once every mutator in it is installed.
  CANCEL, the header's close mark, and the screen underneath going away call
  the second and hand nothing back. It refuses while a match is running,
  while a front-end dialog or a modal OA screen is up, without the layer's
  fonts, or while the picker is already open.
- **The feed.** `MutatorPickerFeed` is where the list comes from. The
  runtime's own feed, installed the first time the picker opens, lists the
  installed mutators (`list_installed`) and the catalogues' mutator entries,
  with each one's download, its badge, and why it cannot be switched on
  here. That list is rebuilt when the catalogues change, when the picture
  cache changes, or when a download ends, fails or is cancelled. Byte
  progress does not rebuild it: each frame reads the queue and updates the
  rows of the downloads the picker queued. An icon that failed to decode is
  remembered for the rest of the opening and is not read again. Each
  installed mutator's verdict is the one the file holds for the list it is
  shown with: its prerequisites, then itself. An On mutator and its On
  prerequisites take the options they are shown with; an Off mutator is
  read with defaults. `check` prepares that list on the main thread and
  the Library's worker runs the dry run, one at a time, a newer check of
  the same mutator replacing one that has not started. The chip stays on
  the checking words until the verdict is kept and the list is built
  again. A list that cannot be resolved stays unchecked and starts
  nothing, and the chip does not read the checking words unless that
  check started. A check
  replaces the feed with `set_mutator_picker_feed` and puts the runtime's
  back with `restore_mutator_picker_feed`. Switching on a mutator that is
  not installed queues it and the prerequisites it lacks (`get_plan` on the
  runtime's own catalogue; the listing itself when a check's feed has none).
  Use these does the same for every On mutator that is not installed yet.
  While one is still not installed it asks "Get mutators", pushed over the
  picker, and hands the set back once the last install finishes. CANCEL
  closes that question and leaves the picker open, with the downloads still
  queued. `waiting` only replaces the summary's words; it does not disable
  Use these. A refusal switches it off and the
  row reads why. A package whose download is already under way, such as one
  the Library queued, is not refused: its row shows that download. Becoming
  On, changing an option, or a download finishing asks `check` again, and
  the row reads the checking words until a later list brings the verdict.
- **Turn off.** Switching off a prerequisite asks "Turn off X?" over the
  picker, pushed at once. Turn off turns it and what needs it off. Switching
  a mutator off, by its switch or by that question, cancels a download the
  picker queued that is still waiting (queued, asking for its key, or
  waiting for the player's confirmation). CANCEL on the question leaves them
  on.
- **The check.** `native-mutator-picker` (`--check-mutator-picker`,
  `runtime_mutator_picker_check.cpp`) turns every built-in registry off and
  at 640x480, 1280x720, 1920x1080 and 2560x1440 opens the picker over the
  main menu on a feed of made-up mutators. It requires the size class, the
  mended On order, every row's grip and switch, one download with the
  waiting words, that download's cancel, one more check when it installs and
  when an installed mutator is switched on, the turn-off question, a drag,
  the set Use these hands back, Use these fetching an On mutator that is not
  installed (the question stays up, and the install hands the set), CANCEL
  handing nothing, and the verdict chip. At 2560x1440 it
  switches on a mutator whose download is already under way, and restores
  the runtime's own feed on a mutator written into the check's folder, in
  English and in German. It then opens that feed as skirmish on Ridge
  Check Pace, a made-up package that changes every builder: the chip reads
  the checking words, then the checked words, and again when its option
  changes, and the verdict file holds one line for each option. Over
  SKIRMISH it opens the picker with three made-up packages on and no
  verdict: each is checked in turn and the chip reads the checked words.
  It also opens that feed with a mutator whose check already stopped and
  an On mutator whose package cannot be resolved: the chip keeps the
  stopped verdict. It
  writes each size's opening frame as
  `mutator-picker-<w>x<h>.png`. `--check-text-fit` paints the same screen,
  its details, the waiting line, the question and the read-only footer in
  every language. A FIXTURES_SETUP test (`mutator-picker-clean`) clears its
  folders before each run.

## The brain picker

The brain picker is one modal screen on the OA layer (`brain-picker`),
centred over the screen that asked, at the size class's dialog size, a
window that moves by its title ([The OA layer](#the-oa-layer)). Its
model and screen are
[src/ui/brain-picker](../ui/brain-picker/README.md);
`runtime_brain_picker.cpp` hosts them. A seat's brain menu opens it with
Options... while the seat's brain has options
([Computer seats in the rooms](#computer-seats-in-the-rooms)), on that
seat, its brain, its level and its options, over the skirmish setup, the
campaign screens or the battle room. It closes as Cancel when the
screen underneath goes or a match starts.

- **The request.** `open_brain_picker` takes a `BrainPickerRequest`: the
  setup, the seat's name, the choice it opens on, the setup's difficulty,
  whether that difficulty fixes the built-in player's level, whether the
  picker offers the choice for every computer seat, and two calls. USE THIS
  calls the first with the choice and whether it is for every computer
  seat. CANCEL, the header's close mark, and the screen underneath going
  away call the second and hand nothing back. It refuses while a match is
  running, while a front-end dialog or a modal OA screen is up, without
  the layer's fonts, or while the picker is already open.
- **The feed.** `BrainPickerFeed` is where the list comes from. The
  runtime's own feed, installed the first time the picker opens, lists the
  built-in computer player and then each brain installed in the player's
  Brains folder. A brain's name, description, level names and option labels
  are resolved for the language in use when that list is built. Its icon
  is decoded then and kept until the picker closes. `draw` and `tick` do
  not list the folder, read a manifest or decode an icon. The generation
  moves on only when the Library installs or removes a brain. A check
  replaces the feed with `set_brain_picker_feed` and puts the runtime's
  back with `restore_brain_picker_feed`.
- **The check.** `native-brain-picker` (`--check-brain-picker`,
  `runtime_brain_picker_check.cpp`) turns every built-in registry off and
  at 640x480, 1280x720, 1920x1080 and 2560x1440 opens the picker over the
  main menu on a feed of made-up brains. It requires the size class and
  the centred window, the row order, Ridge Raider locked with "Made for
  Ridge", Firm, Calm then USE THIS handing Steady Commander at calm with
  aggression high and every seat off, the built-in player's Difficulty
  note and every computer seat, CANCEL handing nothing, and the keyboard
  alone and the controller alone each reaching a level and USE THIS. At
  1280x720 it also takes the header's close mark. It then restores the
  runtime's own feed and, after two made-up packages are written into the
  check's Brains folder, requires those rows under the built-in player.
  It writes each size's opening frame as `brain-picker-<w>x<h>.png`.
  `--check-text-fit` paints `brain-picker`, the inline details at Regular
  and Large, and Compact's details page, in every language. A
  FIXTURES_SETUP test (`brain-picker-clean`) clears its folders before
  each run.

## Computer seats in the rooms

While at least one brain is installed, a computer seat's brain is shown
and chosen in the game's rooms with parts drawn from the room's own art
(`runtime_brain_seats_screen.cpp`, the screen `brain-seats`, and
`brain_seats_room.hpp`, which the battle room and the Mutators buttons
share; the parts are
[src/ui/frontend-renderer](../ui/frontend-renderer/README.md)'s). No part
of them is the kit's, so a mod's art and palette carry through. With no
brain installed the screens are unchanged: no seat, no card and no extra
stop for the keyboard.

- **The skirmish setup.** A computer row's own Player button, redrawn in
  its own art, names the seat's brain, or the room's "Computer" for the
  built-in player, a long name cut with "..." in the room's caption font
  or its label font, and has a notch at its right end. The notch
  (`oa.brain-seat.<slot>`, its text the seat's whole label: a named brain
  with its level, or the built-in player with the word the Difficulty
  button shows), or the right button anywhere on the Player button, opens
  the brain menu; the left button elsewhere on it keeps 3.1c's own press,
  which cycles the row. Tab stops on the notch just after its Player
  button, the arrows and a controller's L1, R1 and A reach it, and while
  it has the focus the setup's own focus is none and the room's focus
  marker shows on it. Y still opens Mutators.
- **The brain menu** is the room's list box under the button, or above it
  where there is no room below, moved in from the room's edges: the title
  "Computer player for this seat", the built-in player, whose level the
  Difficulty button sets, then each installed brain with its level, the one
  the seat plays reading "< Firm >", whose arrows step its level; a brain
  that does not fit the game is greyed. Then "Options..." while the seat's
  brain has options, which closes the menu and opens the brain picker on
  that seat, its brain, level and options ([The brain picker](#the-brain-picker));
  "Use for every computer seat", a check box in the room's CHECKBOX art,
  which writes this seat's choice onto every computer seat, and later
  choices onto all of them while it is ticked; and "Get more brains...",
  which leaves for the main menu and opens the Library at Brains there,
  since the Library opens over the main menu alone. Up and Down move the
  selection, Left and Right step the current row's level, Return and Space
  press the selected row, Escape closes it, and a press outside it closes
  it. While it is open the kit's hover card shows the selected row's brain
  beside it. Automation lists `oa.brain-menu.row.<id>`
  (`oa.brain-menu.row.built-in` for the built-in player), each row's text
  its words and its level after " · ", `oa.brain-menu.level.previous` and
  `.next` on the current row, `oa.brain-menu.options`,
  `oa.brain-menu.every-seat`, checked while ticked, and
  `oa.brain-menu.get-brains`.
- **The card.** Resting the pointer on the Player button or the notch, or
  the information key while the notch has the focus, shows the seat's
  card: the brain, its version, its level, and Full awareness when the
  brain sees every unit. Escape closes the card.
- **The choice.** A row of the menu writes that seat; with Use for every
  computer seat ticked, it writes every computer row and leaves a human or
  an open slot as it is. The choice stays in memory until START, which
  remembers it.
- **The check.** `native-brain-seat-buttons`
  (`--check-brain-seat-buttons`, `runtime_brain_seats_check.cpp`) turns
  every built-in registry off and, at 640x480 and 1280x720, keeps a frame
  of the setup before any brain is written. It then writes three made-up
  brains, Steady Commander with two levels, and remembers Steady Commander
  at Firm on place 1. It requires the notch, `oa.brain-seat.1`, reading
  that label inside the right end of the row's own Player button; a second
  computer row reading the built-in player and the Difficulty button's
  word, which cycling Difficulty changes; Tab from Player1 onto the notch
  and on, and Shift+Tab back; Return opening the brain menu on Steady
  Commander at Firm, its next level stepping the seat to Bold and back,
  its Options... opening the picker, whose USE THIS keeps the brain with
  the option the check wrote, and the built-in player, which has no
  Options..., putting the Difficulty word back; the right button on the
  Player button opening the menu and Escape closing it; Use for every
  computer seat with Lantern Scout setting both rows; the card and Escape;
  and the two `plays lantern-scout` lines a start logs. Back on SKIRMISH
  the seats read as remembered, and the left button on Player2's words
  keeps 3.1c's own press. With the brains removed the frame matches the
  one it kept, and no seat remains. It writes
  `brain-seats-skirmish-<w>x<h>.png`, and with the menu open
  `brain-seats-menu-<w>x<h>.png`. `--check-text-fit` paints the seats with
  three computer rows, one of them a long name, in every language and both
  sizes. A FIXTURES_SETUP test (`brain-seats-clean`) clears its folders
  before each run.

## The campaign's computer player

While at least one brain is installed, NEWGAME and Any Mission show one
button in the room's button art (BUTTONS0), under the Difficulty button in
its column, with the OA stamp at its left and the notch at its right. It
reads the campaign seat's brain and level: "Steady Commander · Firm", or
for the built-in player the word the Difficulty button is showing. The
automation name is `oa.brain-seat.campaign`. With no brain installed those
screens are unchanged.

- **Placement.** The button takes the size of the control under Difficulty
  and sits in that column, with a gap of 2 above and below. When the column
  has no room for that, the button is the label's own size, placed against
  Difficulty by the same rule a block uses when it does not fit.
- **The menu.** A press anywhere on the button opens the brain menu, titled
  "Computer player in the missions". The built-in player's level stays the
  run's difficulty. A mission has one computer player, so the menu has no
  Use for every computer seat. A row sets the campaign's seat at place 1,
  which every mission of the run starts with; Options... opens the brain
  picker on it.
- **Focus.** Tab and Shift+Tab, and a controller's L1, R1 and A, reach the
  button. Up and Down stay with the campaign and mission lists. The card
  shows when the pointer rests on the button, and on the information key.
- **Saves.** Once a campaign save has loaded, the campaign's seat becomes
  the brain, level and options that save seated on player 1. A save that
  seated no brain there leaves the built-in player. A skirmish save leaves
  the campaign's seat as it is.
- **The check.** `native-brain-campaign` (`--check-brain-campaign`,
  `runtime_brain_campaign_check.cpp`) at 640x480 and 1280x720 keeps a frame
  of NEWGAME and of Any Mission before any brain is written. It then
  requires one button under Difficulty, inside the frame and meeting no
  gadget, reading the built-in player and the Difficulty button's word,
  Tab reaching it while Up does not, Return opening the brain menu with no
  every computer seat row, Steady Commander there making the label "Steady
  Commander · Firm", and the same button on Any Mission. The first
  mission logs `player 1 plays steady-commander 1.2 at firm`. A save made
  in that mission, loaded after the campaign seat was set back to the
  built-in player, leaves the seat at Steady Commander, and a skirmish save
  leaves it as it is. With the brain removed the frames match the ones it
  kept. It writes `brain-campaign-<screen>-<w>x<h>.png`, and with the menu
  open `brain-campaign-menu-<w>x<h>.png`.
  `--check-text-fit` paints the button with a long brain name, in every
  language and both sizes. A FIXTURES_SETUP test (`brain-campaign-clean`)
  clears its folders before each run.

## The mutator questions

Four questions live in the picker module (`prompts.hpp`) and the app raises
them. "Start without X?" is pushed over the skirmish setup or the campaign
briefing when a mutator stops that start and no match was built. Back leaves
the set. Start without it, or Start without them, drops the mutators that
stopped and the ones that need them, and starts again. "Forget the last set?"
is queued once the main menu shows, when the crash record named a set. Keep
it leaves the remembered set. Forget it clears that setup's set and
uninstalls nothing. "Get mutators" is pushed over the picker, as the feed
section says. "The game did not start" is pushed where a network start was
refused because the machines' results differ, on the machine that pressed
START: the host. A guest's record says only whether it has the host's
release, so the release line shows when that player has it, and otherwise
the line says the player does not have it. The GET MUTATORS paragraph names
the host's release while that player lacks it and this machine's registries
offer it (`Runtime::mutator_offer`, by the rule a guest's button follows).

`native-mutator-prompts` (`--check-mutator-prompts`,
`runtime_mutator_prompts_check.cpp`) writes Lantern and Slip into a scratch
user folder. Slip's plan raises an error. A skirmish with both shows the
start question naming Slip. Start without it starts with Lantern. Back, on
a second start, leaves SKIRMISH with both chosen. A crash record written
beside the preferences file shows the forget question on the main menu, and
Forget it empties the skirmish set. On the picker's check feed, Use these
with Grounded not installed shows Get mutators. Stopping that download shows
Retry, Retry asks the feed again, and installing Grounded hands the set.
Hosting a room with Lantern, START held on a guest whose record has another
release, or none, names no release, and one who has it and built another
layer shows the release line, with no GET MUTATORS paragraph. A
FIXTURES_SETUP test (`mutator-prompts-clean`) clears its folders before each
run. `--check-text-fit` paints the four questions, with long names, in every
language and both sizes.

## Updates

`runtime_update.cpp`, `runtime_update_offer.cpp` and
`runtime_update_install.cpp` are the game's side of a new release; the
model, the files and the words are [src/app/update](update/README.md),
and [docs/updates.md](../../docs/updates.md) says what the player sees.
Every screen reads one state, so the menu pill, the update centre, the
prompts, Settings › Updates and the headers name one state with one size.

- **The state.** `Runtime::update_centre()` gathers `update::CentreInputs`
  (the check's snapshot and its failure, the offer, the skipped, held and
  scheduled releases, the download's snapshot with its bytes and rate, the
  route, `--updated-from` and `--rolled-back-from`, the state file's
  confirmation and kept version, and the helper's receipt) and returns
  `update::centre_state`: one stage, its versions, its bytes, its percent
  and the actions it allows. The size is the file the route downloads.
- **The actions.** `Runtime::take_update_action` carries out one: Check
  now starts a check; Download update starts the download, or opens the
  release's page where this copy cannot install it; Pause and Resume stop
  and carry on the download; Cancel download discards it, keeping nothing,
  and takes back its schedule; Install when I quit writes the schedule;
  the install-now action, named for the route, writes it and quits;
  Don't install when I quit clears it; Skip this version and Try this
  version write and clear the skip; Try again clears a held release and
  starts the download again, or checks again after a failed check; Show
  details shows the recovery screen; Show the log opens the updates
  folder; What's new and Open the release page open the release's page;
  Roll back… asks the roll back question; Keep it ready and Later change
  nothing. Each preference written is flushed at once.
- **The update screens' header** (`update_header`): every update question
  reads "OPEN ANNIHILATION" then its second word, as Settings' header does:
  "VERSION {version} AVAILABLE" on the offer, the system-wide copy's notice
  and the packager's, "VERSION {version} READY" on the ready prompt, and
  "UPDATES" on the update centre, the recovery screen, the roll back's
  question, Back on, the Updated notice, the move to Applications and the
  page a Steam Deck names, whose state's words are their lead.
- **The offer** (`runtime_update_offer.cpp`, the question `prompt`, its
  buttons `skip`, `later` and `update`) asks at the settled main menu, and
  waits through a match, Settings and a message box: the header OPEN
  ANNIHILATION VERSION {version} AVAILABLE, the size, the release's notes in a What's new box and
  the reassurance; Skip this version (quiet, at the left), Later (Escape)
  and Download update. A release held after it failed to install, and one
  already downloaded, do not ask.
- **The menu pill** (`update-pill`, not modal) shows the update centre's
  short form at the main menu's top right, clear of the game's own
  buttons, or under the OA buttons while an extension's overlay has put
  them at the top; it hides off the main menu, in a match and under a
  modal screen. It shows while a download moves ("0.8.8 · 41%", its bar
  and Pause), is paused (its bar amber and Resume), is being checked, is
  ready (Install…, which asks the ready prompt again), is scheduled
  (Change) or stopped (Try again). A press on the pill opens the update
  centre, a question (`update-centre`) with the state's heading as its
  lead, its detail, the bar, its note and its actions as buttons, the first in the
  accent, then Close; it follows the state while it shows, Pause turning
  into Resume.
- **The ready prompt** (`ready-update`) asks once a download is ready: a
  callout "Downloaded and checked · {size}", a box listing the three
  answers with their notes, and Keep it ready (Escape, `keep-ready`), the
  install-now action (`restart-now`: Install and restart, Open the
  installer on the macOS installer route, Close the game on a Steam Deck)
  and Install when I quit (`when-i-quit`, marked). A quit installs only a
  release scheduled for it, or `--update-and-quit`
  (`Runtime::hand_off_requested`); quitting otherwise never installs.
- **The recovery screen** (`update-receipt`) shows a receipt the helper
  left that did not end well, once, at the first settled menu: the
  outcome's words as its lead, a callout, What happened with each step
  marked done or failed, To put the game right where recovery did not
  finish, and Show the log, Try again where the release can be tried again,
  and Continue (`ok`). It marks the receipt shown, holds the release and
  takes back its schedule. The Updated notice (`updated-notice`, a
  question with OK alone, `ok`) says once that this start replaced an
  older version.
- **The roll back's question** (`update-previous`,
  `settings::updates::roll_back_question`) shows what comes back and what
  stays as it is, a caution, and Cancel (marked) and Roll back and restart
  (a danger button), which quits for the helper to put the kept version
  back.
- **Settings › Updates.** Each frame `apply_update_download` fills the
  page's card from the state: "Installed {version}", the heading, the
  detail, the bar and its paused look, the note and the actions as buttons
  (Roll back… stays Previous version's; Later and Keep it ready, which ask
  nothing of a page that stays open, are left out); a card's button
  returns `DialogAction::update_action` with `Dialog::update_act`, which
  the host hands to `take_update_action`. It also fills Updates' badge in
  the section list ("41%" while a download moves or is paused, "ready"
  while a release is ready or scheduled, `Dialog::updates_badge`), and
  `update_header_note` gives the pill's words to Settings' header and the
  Library's.
- **The checks.** `native-engine-feed` (`--check-engine-feed`,
  `runtime_update_check.cpp`), `native-update-offer`
  (`--check-update-offer`, `runtime_update_offer_check.cpp`),
  `native-update-download` (`--check-update-download`,
  `runtime_update_install_check.cpp`, its starts run by
  `tools/check_native_update_download.py`, each in its own folders) and
  `native-update-roll-back` (`--check-update-roll-back`,
  `runtime_update_roll_back_check.cpp`) serve their feeds and packages on
  127.0.0.1 with a key made from a fixed seed, request no published feed
  and start no helper. The download check presses the pill's and the
  update centre's Pause and Resume, checks that Keep it ready schedules
  nothing, and records the hand-off. Its `cancel` case
  (`--update-download-case cancel`) opens the update centre from the pill
  during a download and presses Cancel download, which keeps nothing; its
  `pictures` case draws every update screen in every state, from made-up
  states, at 640x480, 1280x720 and 1920x1080 with Window size set to
  Compact, Regular and Large, and writes
  `update-<screen>-<state>-<w>x<h>.png` to its folder. The roll back check
  also shows a failed install's receipt once and holds its release.

## Files

- `app.hpp`, `runtime.hpp`, `runtime.cpp`, `runtime_frontend_host.cpp`,
  `runtime_builtin_screens.cpp`, `screen_registry.*`: the screen registry,
  screen loading and the host of the frontend dispatcher. Lines are only
  ever added to `screens.inc`.
- `runtime_update.cpp`: looks for a new release at every start and every
  hour while the game runs, keeps the signed feed it accepted, and reads it
  again at the first menu of each start; `runtime_update_offer.cpp` and
  `runtime_update_install.cpp` are its screens ([Updates](#updates)).
- `runtime_menu_audio.cpp`: the main menu's hum, and when it starts and
  stops (The start's movies and the menu hum, above).
- `runtime_world_draw.cpp`, `runtime_camera.cpp`: world rendering and the
  camera. The map the game shows (`shown_map_size`) is the tile mosaic less
  its last 32 columns and 128 rows of map pixels, the hidden edges the
  original never scrolls to and maps fill with filler tiles, as
  `Game.map_pixel_width/height` hold it; the terrain fills, the box filter,
  the far view's filter and the Full tier's atlas (`shown_tile_grid`) end
  there, and draw black past it and before its left and top edges, so no
  tier draws the filler at any zoom. The features, trees, rocks and wrecks,
  are cut off at the same edges in every tier, nothing of them drawn on
  the black (`shown_map_span`, `region_on_map`, `shown_map_scissor`);
  units, shots and effects past the edges are drawn whole. A wreck's
  picture follows its record (`follow_feature_draw`): while the wreck
  sinks it is drawn lower each frame, the underwater tint with it, and
  once the wreck rests the picture stays at that height. The view may go
  past the map's edges as far as View past the map's edge lets it
  (`past_map_edge_share`): at 50% its centre stays on the map, so that a
  map's edge or corner can be brought to the battlefield's middle, or,
  where the view shows more of an axis than the map holds, the map's
  centre stays in the view; 25% lets a quarter of the view lie past an
  edge, and Off none, the map centred in a view wider than it
  (`view_centre_span` in `far_view.hpp`). The view's exact place, the map
  point at the battlefield's corner (`exact_view_`, `match_view_place`),
  is what the zoom, the scroll and a finger's pan move, and the camera is
  taken from it (`place_match_view`): the nearest whole map pixel, or,
  while frames draw the view between map pixels, the one at or before the
  screen pixel nearest the place, on the screen pixels laid from the map's
  corner at the zoom (`scene_origin`). At 50% the
  limits hold a view from the one they held last (`view_hold_`,
  `view_hold_from`, `held_view`), so that a view a zoom left past them
  goes no further from the map but moves back toward it at once; at 25%
  and Off they hold every view by the span alone, so that a view held
  for a smaller battlefield, such as the first view of a match, placed
  for the game's own screen before the match is laid out for the window,
  comes back within them. Every frame holds the camera so
  (`view_camera`, `held_camera`), which catches the moves that set the
  camera themselves: the minimap, the megamap, a follow, a jump to a unit
  or a marker, a load. The camera may lie left of and above the map, and
  every drawing and pointer path takes it signed. Saves, the match's
  digest, a meteor strike and the camera network play shares take the
  camera held on the map as the game holds it (`on_map_camera`), so a
  view past the map's edges reaches none of them.
  The wheel and a trackpad set the zoom's target (`handle_match_zoom`,
  `wheel_zoom_target`), which `step_match_zoom` eases toward by the frame's
  time, the zoom's logarithm a share of the way each frame, whatever the
  game's speed and while the match is paused; a pinch and the pad's zoom
  take their zoom at once (`zoom_match_about`); the settings dialog and a
  change of the zoom's limits ease about the battlefield's centre. Every
  frame of a zoom keeps the exact map point under its focus there
  (`zoom_view_about`): the pointer where it is that frame while the wheel
  zooms about it, or the point the zoom was given. At View past the map's
  edge 50%, along an axis on which that point lies on the map, the view
  goes wherever that takes it, past the limits too, since the point keeps
  the map in view; past the map's edge it is held within the limits. At
  25% and Off it is held within the limits along both axes, so that the
  point slides where the view meets them. Every zoom keeps a camera's
  follow of a unit: the wheel's, a pinch's and the pad's are about the
  battlefield's centre while it follows, as the dialog's ease is, and the
  unit stays at the centre, or as near it as the limits let the view go.
  A unit followed toward the map's edge takes the view to the limits,
  where the view stops and the follow goes on; the view follows it again
  from there as it comes back. A scroll ends the follow, though the view
  is held at the limits, as do the minimap, a finger's drag, mouse-look
  and the unit's end. The wheel counts its steps
  from the target they began at (`zoom_wheel_`), so that as many steps
  back return the zoom exactly, and with it the view where the pointer
  rested. The step that reaches the nearest or farthest zoom counts whole,
  though the zoom stops there, and a step past it counts nothing, so that
  the first step back leaves the end and as many as reached it return the
  zoom. Every zoom of the player's view, the wheel's, a pinch's, the touch
  buttons', a pad's and the settings dialog's, stays between
  `least_match_zoom` and `most_match_zoom`, which the Maximum zoom out and
  Maximum zoom in settings set (`least_battlefield_zoom` in
  `far_view.hpp`): Automatic keeps the drawing's floor
  (`detail_zoom_floor`, half the game's scale, a sixth while Full draws),
  Whole map the zoom at which the whole shown map fits the battlefield
  (`whole_map_zoom`), a share that share or the whole map, never past
  `furthest_battlefield_zoom`, a sixty-fourth; a view past the limits eases
  within them about the battlefield's centre (`step_match_zoom`). The
  director, the checks' `--zoom` and the recorded games' replay keep their
  own range.
- `far_view.cpp`, `far_view.hpp`: the far view, the battlefield zoomed out
  past `detail_zoom_floor`, which the processor draws at the zoom in every
  tier (`far_view_frame`, `world_scaling`): the terrain averaged from a
  pyramid of each tile's means at 2 to 32 map pixels a texel
  (`build_terrain_pyramid`, made at the map's first far frame and kept in
  `far_terrain_` while the map and palette stay; `filter_far_terrain`
  reads the level whose texels are one or two to a screen pixel, in bands
  on the drawing threads), and the fog as at any zoom. How its units are
  drawn is the Zoomed out units setting's: Rendered draws them, and
  features, projectiles, fragments and debris, as at any zoom, with no
  enhanced anti-aliasing, on a model bridge of every map pixel in view;
  Dots draws a frame of dots farther out than After zoom
  (`dots_frame`, which the Full tier's card frames between After zoom and
  its floor draw too, on the overlay canvas): each unit the player sees
  as a dot of its owner's colour over the fog, framed in the selection
  boxes' colour while selected (`draw_far_view_dots`), and no model of a
  unit, feature, projectile, fragment or debris, no health bar or squad
  digit and a model bridge of one pixel. A frame at any zoom whose
  bridge would take more than `rendered_units_budget` is a frame of dots
  too: an eighth of the machine's physical memory, and on a 32-bit build
  also at most an eighth of the free address space (or 256 MiB when that
  is not known). `OA_RENDERED_UNITS_BUDGET` replaces that figure, in bytes.
  The picture grows with headroom and is released after the view has zoomed
  back in; a picture that cannot be allocated draws dots and does not end
  the game. A frame of dots of
  the whole map takes the memory of a frame at the processor's floor
  whatever the map's size, and about its time but for the fog, whose grid
  holds every cell of the map in view (`build_fog_grid`): on the largest
  maps in the Off tier it makes a frame of the whole map up to about twice
  as long as one at the floor; a rendered frame of the whole map takes two
  bytes a map pixel for its bridge and time with the units in view.
  The pointer picks a unit wherever its dot is drawn, or would be in a
  rendered far view (`far_view_dot_covers`, in `selection_hooks`), and a
  press on the black
  past the map's edges, at any zoom, takes the nearest point of the shown
  map (`ground_point_under`). The drag box's corner there is the point
  itself at height 0 while View past the map's edge lets the view past the
  edges (`match_pointer_ground`, `drag_box_corner`), so that a box reaches
  as far as the view shows and selects the units drawn there, as a click on
  one does; at Off it is held on the map's ground, as 3.1c holds it
  (`native-off-map-selection`). In the Full tier the far frame's world layer
  is the processor's picture, which Basic's presentation draws without
  leaving Full. `app-far-view` checks the floors, the pyramid, the filter
  before and past the map, the dots and the pixels they cover, and the
  view's limits (`view_centre_span`, `held_view`, `held_camera`) by table;
  `native-navigation-zoom` (`runtime_tracking_zoom_check.cpp`) every frame of
  the wheel's, a trackpad's, a pinch's and the pad's zooms at the map's
  corners, edges and middle and past them, with the pointer resting and
  moving, keeping the map point under the pointer to a millionth of a map
  pixel (`check_zoom_about_pointer`); every choice's limits on the game's
  screen, and on one larger window in each of
  `native-navigation-zoom-1366x768`, `native-navigation-zoom-1920x1080` and
  `native-navigation-zoom-2560x1440`, the whole map's fit, the point under
  the pointer on the way out to it and back, a zoom in from it landing where
  it is aimed, and a choice changed in play (`check_zoom_limit_choices`),
  and at View past the map's edge Off the first view of a match, placed
  for the game's own screen near the map's right and bottom edges, drawn
  on the map on that window (`check_first_view_on_window`); at
  Whole map on the
  game's screen a click on every pixel of a unit's dot, on an enemy's dot
  and on the black either side of the map (`check_far_view_presses`); a
  scroll stopping at the view's limits, the minimap bringing the map's
  corner to the middle, a view a zoom left past the limits, an aircraft
  past the map's edge drawn, hovered and selected as a model near and far
  and as a dot, and the digest of a view past the map
  (`check_view_past_map`); and a
  zoom keeping a follow, a unit followed to the map's edge and back at
  each View past the map's edge, and what ends a follow
  (`check_tracking_zoom`); `native-render-tiers` the
  whole map's far view in each tier, rendered and as dots, the Full tier's
  card frame of dots past After zoom, and the fill before the map's start
  and past its end.
- `runtime_skirmish_start.cpp` builds a match: the feature table's GAF files
  are kept as read and parsed without their pixels (`gaf::PixelData::checked`);
  a feature sequence's pixels are decoded from its file when a feature first
  draws it, and the map's sprite features' sequences once each, as the first
  of them is placed. A 3DO model the map's features share is loaded once.
  The collision plots go once the match holds its own copy. The weapons'
  explosion files are read and checked the same way before the match
  starts (`load_explosion_gaf`); a sequence is decoded whole the first time
  an explosion asks for it (`explosion_sequence`), and one that cannot be
  decoded is reported once and not read again. A file whose pixels would
  decode past 16 MiB is never decoded whole: the simulation gets its checked
  sequences, of which it reads only frame counts, durations and repeat
  flags, and each frame is rendered from the file's bytes as it is drawn,
  through a cache of 32 MiB that lets the frame drawn longest ago go first
  (`effect_frame`, `GafFrameCache` in `world_draws.hpp`).
- `world_draws.hpp`, `world_draws.cpp`, `runtime_match_render.cpp`: the
  battlefield drawn in horizontal bands. `render_match_surface` first works
  out the frame's draws in their order (`WorldDrawList` in `MatchModels`):
  the effect layers' particles, the features and units far to near, the
  projectiles, debris, explosions and smoke. Each explosion's flash, before
  its sprite and under the units off the ground, the smoke and the fog,
  lights the palette entry under each of its pixels through the light
  table's row the pixel names (`blit_world_lit_hotspot`), culled on its
  centre alone and with no sight test, and placed with the record's sprite
  on that centre, half the record's height rounded down above its ground
  point (`project_world_point`), as strongly as the Explosion flash
  setting asks, held to a mod's lower level
  (`view_rules::explosion_flash_drawn`, ui.explosion-flash); at Off none is
  listed, and the frame is drawn as without flashes. A projectile of render
  type 2 draws a lens among the other projectiles (`draw_world_lens`): the
  five by five map pixels at its centre spread out from it, read from the
  frame as it stood, as the game draws them. Its centre is raised by half
  the shot's height rounded down (`project_world_point`), where units and
  most particles round to the nearest pixel. A lens whose centre lies off
  the battlefield at the tick is not drawn
  (`projectile_lens_on_battlefield`), and every projectile after it is
  drawn as without it. The Full tier draws no lens. Everything drawing builds or
  changes on the way is done then, once, on the drawing thread: the piece
  transforms and the presented copies, the units' and features' cached
  images and silhouettes and those of the units they carry
  (`plan_unit_supersampled`), the texture animations, the projectiles'
  models, the GAF frames decoded for the particles (each once, kept from
  frame to frame), the selection boxes' lines and the frame's statistics.
  Before them the frame's shadows are set from the zoom (`set_frame_shadows`, `shadow_fade.hpp` of
  the model module): at zoom 1 and closer the game's own; zoomed out
  lighter, easing down to none at a quarter, the models' silhouettes and
  the projectiles' shadow sprite blended through a faded alpha table
  (`ShadowTable`) and a feature's shadow frame mixed from the frame toward
  the game's shadow by the level (`fade_shadow_channel`); from a quarter
  out none at all, the renderer's shadow option cleared for the frame and
  no shadow listed, so none is drawn or built. The model bridge is
  then split into one band of whole tile rows for each drawing thread
  (`bridge_split`), and each band draws the whole list with its own rows
  alone (`draw_world_band`): its own tiles of the bridge are captured, drawn
  and written back, and sprites, squares and lines change only its rows,
  each line and polygon working out its pixels as over the whole frame. A
  band reads the list and the models and writes only its rows of the frame
  and of the bridge, so the bands draw on the job pool at once and give the
  frame drawn whole byte for byte; a frame with a lens, which reads rows of
  other bands, is drawn as one band. The first band draws with the models'
  renderer and buffers; each other band keeps a renderer of its own (its
  composite buffer), its supersampling buffers and its own memory of
  colours outside the palette, about 1 MB a band; with one drawing thread
  there is one band and no more. The debug grid before the list, and the
  fog, the order overlays, the build ghost, the selection band, the health
  bars and the HUD after it, are drawn on the drawing thread as before.
  The terrain, the list and the fog are drawn at the draw scale into the
  scene `world_scaling` gives: the scene pixels per map pixel, which the
  terrain fill and its box filter, the fog, the model bridge and the
  sprites, particles and lines of the list take, while units and features
  are culled, and the order overlays, build ghost, selection band and every
  painter after them placed, in screen pixels at the zoom. The terrain
  fill, the model bridge's write-back and the list's sprites and particle
  squares lay the map's pixels over the scene one way, the scene grid
  ([`scene_grid.hpp`](../present/include/oa/present/scene_grid.hpp)): a
  sprite is placed by the map pixel its origin stands on as the game's
  unscaled view places it (`place_world_point`), and each of its pixels
  covers the scene pixels the terrain of its map pixel fills
  (`blit_world_sprite`), so that features, explosions and particles stay
  over their ground, and with the units, through a zoom and under a
  camera that moves; at zoom 1 they draw as the game draws them. The Full
  tier's sprite stage places them at their map pixels times the zoom, as
  its terrain and models. The standard
  tier draws at the zoom, its scene the world layer itself; a check may
  draw the scene at another scale apart from the world layer, which a
  nearest resample (`resample_nearest_rgb24`) then fills at the zoom; and
  the accelerated presentation (below) draws a zoomed-out scene at a draw
  scale of its own and reduces it by the area pass. What the match reads
  back from drawing (the view in Game, the on-screen list, the piece
  transforms, the radar) follows the zoom and the camera alone;
  `native-match-layers` draws the scene at 1 apart at zoom 1, 0.5 and 2 and
  checks that, and that at zoom 1 the world layer is the same byte for
  byte; at zoom 2 a nano particle drawn on that scene fills its 2 by 2
  square there and, resampled, a 4 by 4 square at twice its place.
- `world_scaling.hpp`, `world_scaling.cpp`: how a frame draws the
  battlefield (`world_scaling`), from the zoom, the battlefield's size and
  the draw scale asked for, as a pure function. Without a draw scale the
  scene is the battlefield at the zoom, the world layer itself; drawn apart
  at the zoom it is the battlefield's size, and at another scale it covers
  the battlefield's map pixels at that scale with two more columns and
  rows, rounded up to even sizes. The accelerated presentation's own
  (`accelerated_world_scaling`): below zoom 1 the scene is drawn at the
  highest draw scale its scene budget allows (`accelerated_draw_scale`: the
  zoom times the square root of the budget's scene pixels per battlefield
  pixel and of its most scene pixels over the battlefield's, from the zoom
  to 1; both numbers provisional, chosen without measurement) and reduced by the
  area pass, unless the budget is none or the zoom over the draw scale is
  above the cut-off of 0.9, where the frame draws at the zoom; zoom 1 draws
  as always; above it the scene is drawn at 1 and magnified, unless magnify
  is off. `area_scale` gives the area pass's 16.16 scale, and
  `largest_magnified_scene` the scene a magnified frame's texture is made
  at. For a view drawn between map pixels (below), `area_phase` gives the
  area pass's start in the scene, `magnified_span` the corner a magnified
  frame draws and where it lands, one more column and row at the same
  scale. `app-world-scaling` checks these by table, and that over the zoom
  range and every window's battlefield the scene holds every pixel the
  nearest resample and the area pass read.
- The accelerated presentation (`runtime_accelerated.cpp`,
  `scaled_world.hpp`, `scaled_world.cpp`): a component switched on at a
  rung of the step-down ladder (`switch_accelerated_presentation`) when
  the tier of the frame is accelerated (`runtime_render_tier.cpp`, below).
  Headless runs, the director, a named `--preferences-file` at its
  defaults and every check but `--check-render-tiers` draw and present as
  the standard tier always has. Switched on, a zoomed-out match frame
  draws its scene at the draw scale and the exact area pass reduces it into
  the world layer on the drawing threads, which is then painted over and
  uploaded 1:1 as always; lasers, lightning and selection lines are drawn
  thicker in that scene, so they stay about one screen pixel thick
  (`scene_line_thickness`). A zoomed-in frame draws its scene at 1, which is
  uploaded to a streaming ARGB8888 texture made once, at the first zoomed-in
  frame, at the largest size a magnified frame needs, in tiles beyond the
  renderer's texture limit
  (`TiledTexture`), and the card magnifies it into the battlefield
  (`draw_scaled_world`): NEAREST at a whole-number zoom, the renderer's
  PIXELART where it has it (`probe_pixelart`), and otherwise sharp-bilinear,
  NEAREST into a prescale target made once, at the first frame so drawn, at
  its largest within the prescale budget, then LINEAR; the prescale target is split into tiles with
  gutters beyond the renderer's texture limit, as the scene is, and each of
  the scene's tiles is drawn into each of its tiles (`PrescaleTarget`).
  The nearest picture of the scene is kept as
  the base the painters after the fog paint over; what they changed goes up
  as an overlay, transparent elsewhere, in the 32-row bands that hold it now
  or held it last (`convert_rgb24_overlay_argb`), laid over the magnified
  scene 1:1. A change of the window's size remakes the card's textures and
  the overlay but keeps the base the frame being presented drew, so that
  frame is magnified as every other.
  Smooth panning: while a frame is magnified or reduced by the area pass,
  drawn by the card in the Full tier, or drawn by the processor at a zoom
  below 1, the view lies between map pixels (`view_between_pixels_`,
  `view_offset`): at the screen pixel nearest its exact place
  (`match_view_place`), counted from the map's corner, past the camera
  taken at or before it, which, and Game's, steps whole map pixels as the
  place moves, so nothing reaches the simulation, saves, digests or the
  wire; a camera moved any other way starts the view on its own map pixel.
  Every map pixel then falls on the same parts of screen pixels from every
  camera, so the ground moves by whole screen pixels and is never sampled
  afresh: zoomed out it does not shimmer as the view moves, and zoomed in
  a magnified or Full frame moves a screen pixel at a time. The card draws
  the scene that far before the battlefield's edge (`magnified_span`), the
  area pass starts its picture that far into the scene, the Full tier
  draws its terrain, fog and stages that far on (below zoom 1 into its
  moved target, a pixel of room on every side, at twice the display's
  density in texture pixels, the terrain by the level rule at those
  texels, with the painters' overlay, then moved by the rest of a screen
  pixel, at the view's exact place (`view_shift`), at the target's own
  texels into the shifted target, which is reduced onto the battlefield,
  each display pixel the mean of its two by two texels: so the ground,
  everything on it and the marks move together every frame while their
  picture is never sampled afresh, and the ground keeps one sharpness
  wherever between pixels it lands, where one LINEAR draw by the fraction
  of a display pixel softened it between pixels and sharpened it on them;
  where the memory or the renderer refuses the two targets at that
  density, the moved target is made at the display's density and moved
  onto the battlefield by one LINEAR draw), a
  scene the processor
  draws at the zoom lays its terrain (the nearest fill, the box filter and
  the far view's), models and sprites from the offset's phase on the scene
  grid (`scene_phase`), and the painters after the fog move by it to the
  nearest screen pixel. A followed unit (`centre_view_on`) is kept at the
  battlefield's middle so, by screen pixels, and exactly where the Full
  tier's card moves its picture by the rest; in a frame the processor
  draws at zoom 1 and above, whose units are drawn on whole map pixels,
  the camera steps whole map pixels with the unit, as the game's does. Hover, picking, the drag box, the build site and orders'
  map pixels take the same offset and the card's shift
  (`game_screen_point`, `match_world_point`, `map_pixel_drawn_at`), so the
  pointer is over what is drawn under it, and orders stay whole map
  pixels; the offset never carries the pointer past Game's view
  (`Game.battlefield_rect`) at the battlefield's edges. Every other frame (`settle_view_offset`) draws the view on the
  camera's map pixel and forgets the offset; the picture kept for a
  reader is the standard tier's, on the camera's map pixel. The HUD strips, the front end and the
  loading screen are drawn by `sharp_draw`: NEAREST at a whole-number
  scale, else PIXELART or sharp-bilinear, the front end and the loading
  screen NEAREST at every scale under Menu scaling's Unfiltered
  (`frame_filter`), their prescale targets drawn
  again when the layer's revision moved. Whatever paints a layer moves its
  revision, and every frame the loop presents paints the HUD, and the front
  end too unless its panel keeps the frame shown, so the target is drawn
  again once on each such frame, and never for a present without a paint;
  the front end's target is freed during a match. A call
  only this tier makes that fails throws `AccelerationError`, after which
  the tier is dropped for the run and the frame presented as the standard
  tier presents it (`drop_acceleration`); the failing call is struck
  against the driver, but an error of the game's own, which no driver call
  made (`AccelerationFault::engine`), is not (`take_acceleration_error`). Screenshots, film frames
  and the end screen keep the standard tier's picture of the same moment
  (`ensure_screen_world`), drawn again with the frame's counts of units
  drawn kept and the HUD's resource readout, which saves keep in
  `Game.resource_readout`, not eased again. The load and save dialogs and
  the in-game briefing, drawn over a picture of the paused match, keep the
  battlefield as the frame presented it (`compose_shown_match`), so that
  the units, the water and the fog behind them look as they did without
  the dialog: a battlefield the card drew in the Full tier, or magnified in
  Basic, is drawn again as the last frame drew it, into the window's frame
  without presenting it, and read back at the layout's size and back
  through the display gamma (`read_presented_world`); the area pass's
  picture below zoom 1 is the world layer as presented; and a picture the
  card cannot give is the standard tier's draw. MakePoster draws every view of its poster as the standard tier
  draws it too (`render_poster`), and the frame again in its own tier
  after.
  `app-scaled-world-software` checks the drawing on SDL's software
  renderer against nearest replication and that renderer's own LINEAR,
  modelled on the processor (`software_linear_rgb24`), within 2 levels,
  scenes and prescale targets in tiles and a view between map pixels
  among it; `app-world-draws` checks
  the thick lines band by band, and a sprite over its ground through a
  zoom's ease from 0.25 to 3 and under a camera following it far out; and
  `--check-render-tiers` (`runtime_render_tiers_check.cpp`,
  `native-render-tiers`, with `--hardware-acceleration` and
  `--force-capable` on that renderer, which the start-up function test
  passes, and `native-demo-render-tiers` over the demo's first Arm
  mission) checks the presented frames of the main menu and a fight at
  zooms from 0.5 to 4, against the references of `scene_filter.hpp` on a
  card and against that model on SDL's software renderer, switching the
  tier off and on as the flags would; that a frame depends on none before
  it, the first after the tier is switched on or the window resized among
  them; that the picture kept for a reader is the standard tier's and eases
  nothing; that a slow scroll at zoom 2.5 and 0.5 moves the battlefield
  read back by at most a pixel a frame, the view drawn at the scroll's
  exact place either way while the camera steps whole map pixels, where at
  2.5 the standard tier jumps two or three pixels, and a second axis
  joining the scroll is at its exact place from its first frame; that at
  zoom 4 the pointer finds a unit three pixels further left with the view
  three quarters of a map pixel on, as it is drawn, and the game view up
  to the battlefield's edges; and that a zoom to the zoom it is at, by the
  wheel or about the centre, keeps the point drawn under its focus
  (`check_smooth_panning`, `runtime_smooth_pan_check.cpp`); that at zoom 4
  a view half a map pixel on is, as Full draws it, the frame before moved
  two pixels left; that a zoom
  ease makes no texture; that prescale targets are drawn once a painted
  frame; and that the Full tier's model stage (`runtime_full.hpp`), given
  the zoom-1 frame's list, draws the fight's units, projectiles, debris and
  fragments and their shadows on the game's renderer within the model
  raster bounds of the processor's raster of the same list
  (`check_full_models`); it writes pictures of one moment at zoom 0.5, 1
  and 2.5 in both tiers. Its flag names Full, so its Basic cases set the
  level to `basic` and its Full cases (`check_full_render_tier`) run after
  them at `full`: at zoom 1, 2 and 4 the frame equals the standard tier's
  draw of the same moment beside the card's own draws (`card_draw_mask`:
  the sprites the alpha table blends, the explosions' flashes, held within
  their own bounds of the standard tier's light where they alone are the
  card's, the sprites that reach a fog tile
  not wholly clear, the lines, the units, projectiles, debris and
  fragments with their shadows, and the fog tiles whose edges ramp), with
  the draws under the first differing pixel listed and a picture of them
  when it does not; a Full frame leaves what the match reads back
  (`match_draw_read_back`) as the standard tier's frame of the same moment
  does; with damage bars on, every health bar the frame lays out
  (`drawn_health_bars_`) has its zoom's size in the Off, Basic and Full
  tiers alike (`health_bar_size`: 35 by 5 pixels at zoom 1 and in, 29 by
  5 at 0.75, 23 by 3 at 0.5, 15 by 3 at a quarter and 13 by 3 at a sixth)
  and each no other bar covers shows its trough and fill on the frame,
  with pictures of each; below zoom 1 the terrain goes into the
  zoomed-out target at twice the display's density by the level rule at
  its texels, held within the renderer's tolerance of that renderer's own
  LINEAR of each tile's quad at those texels, pass over pass (2 and a mean
  of 1 on SDL's software renderer, 4 and a mean of 0.5 on a card), and the
  battlefield presented equals the target reduced on the processor as the
  card reduces it; at zoom 0.5 with the camera on an even map pixel the
  terrain under a transparent overlay, where the standard tier shows
  terrain too, equals that tier's box filter within the level the
  renderer's rounding of the mean of four may take, and at 0.75 level 0 enlarged between the texels and reduced, the
  card's own filter, is printed against it with its mean bounded, the
  references built from the check's own atlas of the map; following a
  walking unit at the zoom floor, every frame lays the ground at the
  view's exact place, the moved target's picture moves by whole pixels,
  the frame presented is the shifted target reduced and, on a card, the
  shifted target is the moved target moved by the shift, and the pointer
  finds the map point and the unit drawn under it; at 1.37 the terrain through the target keeps within
  the renderer's tolerance of the level-0 view enlarged twice and drawn
  LINEAR, the sharp-bilinear reference; at the window whose chrome scales
  by 1.6 the HUD strips keep within the chrome's filter, as the Basic case
  there holds them; the box filter never runs for a Full frame; the match
  loads in Full, its loading screen makes the terrain pages and their
  greyed pages and lets the atlas's texels go, and the first match frame
  draws from them; a Full frame draws its sprites and models in the same
  card frame as its terrain and fog; and the terrain's processor cost, the
  frame's build, the card's call and the overlay, is printed for each zoom
  beside the box filter's, with pictures of each zoom. Its anti-aliasing
  cases set the row to 2x and 4x and, at zooms 1, 1.37 and 2, require the
  factor the budget allows, no unit drawn finer on the processor, and the
  battlefield under the transparent overlay equal to the world target read
  back and reduced on the processor by halving as the card reduces it,
  within the renderer's tolerance, the target's memory printed within the
  budget; at zoom 0.5 they require the frame drawn through the zoomed-out
  target and nothing into the world target; with the row off again the
  target is freed. The fog, the canvas,
  the kill board and the +stats panel (`check_full_overlays`), at zoom 1
  and 2: with line of sight alone, under a fog tile wholly out of sight a
  pixel the tiers agree on with the fog off is the processor's exactly,
  under a tile at an edge it lies between its colour and its grey within
  2, and under no tile it is unchanged, passing over what the design
  accepts, the sprites under tiles of more than one state, the blended
  sprites over the fog and the models; with mapping too the tiles never
  mapped are the processor's black and nothing else changes; the dithered
  option is the even tone within 2; with Fog of War at Classic, under every
  tile with a corner out of sight or never mapped, its edges included, the
  greyed, the black, and the dithered with the black are the processor's
  exactly; the overlay canvas is painted exactly
  where the overlay is opaque; the kill board pinned by F4 darkens the
  world under it to the shade level's share within 2 with its foreground
  on the overlay, the local player's lit row left out and nothing outside
  it changed, and leaves no pixel behind; and the +stats panel darkens its
  padding and its graph well by its opacities and draws its outline in the
  dark and light edge colours, with nothing outside it changed. At native
  density the Full frame at zoom 1 is the standard tier's draw enlarged,
  beside the card's own draws.
- The Full tier (`runtime_full.cpp`, `runtime_full.hpp`,
  `full_presentation.hpp`, `full_terrain.hpp`, `full_terrain.cpp`,
  `runtime_full_sprites.cpp`, `runtime_full_models.cpp`, `full_fog.hpp`,
  `full_fog.cpp`, `runtime_full_overlays.cpp`): a branch of the
  accelerated presentation, taken when the tier decided for the frame is
  Full (`set_full_presentation`), in which the graphics card draws the
  whole battlefield and the processor paints the HUD and what the painters
  after the fog paint. The planner runs as in Basic and writes what it
  writes there; `world_scaling` returns the zoom with no split, so the
  frame is planned in screen pixels at the zoom, but the terrain is not
  filled, the bands do not draw and the fog is not rasterised
  (`note_full_canvas`): the world layer is cleared to a key colour, the
  lowest colour not in the palette (`full_overlay_key`,
  `overlay_key_colour`), and is the overlay canvas the painters paint on;
  the box filter never runs (`refresh_filtered_terrain`). As the match
  loads, with the terrain step of its loading screen and before the world
  is built or a shared game's load barrier runs (`make_full_match_pages`,
  from `bootstrap_match`), the card's executor (`src/app/card`) is opened
  on the renderer at its texture limit and the Full function test run on
  it, four batches into one target read back once: an opaque page of two
  levels drawn 1:1 NEAREST reads back as its texels, its level 1 drawn
  twice its size LINEAR within 2 of the enlargement (or exactly NEAREST on
  SDL's software renderer), a white quad at alpha one half over a known
  colour as the blend within 2, and a triangle with red, green and blue
  corners shows their mean at its centroid pixel within 4; the map's
  terrain atlas (`src/present/gpu-world`) is built within `fit_page_edge`
  of that limit, through the display gamma, twice, with the palette and
  with a palette of the gray table's entries, the greyed variant the fog
  reads (`greyed_palette`), levels 0 and 1 of each uploaded as pages and
  each page's texels let go once uploaded (`ensure_full_terrain_pages`),
  so that the match's first frame finds the pages and makes none of them.
  A frame builds them again only when the renderer was made again, the
  pages were freed, or the palette, the gamma or the page edge changed
  since (`ensure_full_match_textures`), which also gives the sprite pages
  the match's palette at the display gamma and the fog's gray table, and
  the model stage its palette; the atlas is keyed by the map's own
  storage, since one map record holds every map of the run in turn. A
  card failure at the loading screen drops Full for the run and the match
  plays in Basic. Each frame (`present_full_match_layers`) the builder
  appends to one `CardFrame`, in this order. The terrain: one quad per
  visible tile, a batch for each run of tiles on one page, by the level
  rule (`plan_terrain_draw`): between zoom 0.5 and 1 level 1 LINEAR, then
  level 0 LINEAR over it at alpha 1 - log2(1/zoom), which at 0.5 is level
  1 alone, today's box filter where the camera lies on an even map pixel;
  at 1 and every whole number above it level 0 NEAREST, 3.1c's pixels; at
  another zoom above 1 level 0 by the pixel-art sampling mode where the
  start-up probe found it, which the rung's card filter carries, else
  NEAREST into a target made once at twice the battlefield, a whole number
  of map pixels at zoom 2, 3 and 4, and the target drawn LINEAR to the
  window by zoom over the next whole number, the Basic tier's
  sharp-bilinear, or LINEAR straight where the target cannot be made. The
  fog's greyed pass (`full_fog::append_unseen_terrain`), from the fog grid
  the frame's fog pass built and kept in place of drawing
  (`apply_match_fog`, `note_full_fog_grid`): over each fog tile with a
  corner out of sight, the terrain again from the greyed pages at the
  terrain's levels and sampling, into whatever the terrain was drawn
  into, four quarter-tile quads whose corner alphas are 1 at the corners
  out of sight, so a tile wholly out of sight shows the gray table's
  colours exactly and a tile at an edge ramps from colour to grey where
  the processor's FOG.GAF masks cut, a terrain tile under four such tiles
  as one quad, and where two terrain levels blend each level's greyed pass
  at its share (`pass_alpha`). With Fog of War at Classic
  (`Runtime::fog_of_war`) a tile at an edge is drawn as the processor
  draws it instead (`full_fog::append_unseen_terrain_classic`,
  `append_unmapped_classic`, `append_unseen_dither_classic`): the greyed
  terrain at alpha 1 under the rectangles of its gray FOG.GAF tile
  (`ClassicShapes`, built once from the tile art the fog pass loads), the
  black tile's rectangles in their palette colours, and under the dithered
  option the gray tile's rectangles through a page of the checkerboard,
  drawn NEAREST over the objects with its texels on the target's pixels,
  so that the dither stays one pixel fine at every zoom and the card's fog
  is the processor's pixel for pixel; the fog mask is not used. The model stage's shadows, then the list's
  draws in painter's order, each to the stage of its kind (`sprite_kind`,
  `model_kind`), a stage's batches never joining another's: the sprites,
  blended sprites, explosions' flashes, particle squares, lines and
  selection lines (`SpriteFrame`, `runtime_full_sprites.cpp`), each flash a
  quad from its lit cell (`DrawMode::lit`) drawn by the lighten blend, which
  makes what is under it one more than row/30 as bright, at half that
  Reduced, each sprite a quad from its
  cell on the sprite pages (`src/present/gpu-world`, keyed by the frame
  the planner drew from) drawn by premultiplied alpha, at a vertex alpha
  of one half where the planner blends it through the alpha table, so the
  card blends to the true mean where the table snaps to the palette,
  sampled nearest at a whole-number zoom, linear below 1 and pixel-art
  above; a feature's opaque frame takes the fog's greyed pass pixel by
  pixel, as the ground under it does and as the processor greys the
  features with the ground (`full_fog::greyed_cover`): drawn in colour,
  then from its greyed cell over the pieces of the pass that cover it, at
  the alphas the pass draws the terrain at there, the Smooth fog's
  quarter-tile triangles or the Classic fog's FOG.GAF rectangles, or from
  its greyed cell alone where the pass covers it whole, so that a feature
  at the fog's edge greys where the ground beside it greys; any other
  sprite from its greyed cell where the map pixel under its drawn point
  lies in a cell out of sight (`cell_fog`), since the fog lays its tiles
  by map column and row alone; every sprite in colour under the dithered
  option; squares
  as the planner's rectangles; lines as quads `max(1, zoom)` pixels wide
  through the centres of their end pixels, selection lines the same from
  the bridge's map pixels. The sprite pages, and the model stage's two
  sets of texture pages, hold every frame a frame uses until it has run
  (`SpritePages::begin_frame`), so that no cell a frame draws from is
  evicted under it; where those alone fill the pages, the pages grow,
  doubling, from 64 MiB for the sprites and 32 MiB for each texture set up
  to 512 MiB each, as far as the memory guard allows
  (`FullPresentation::allow_page_growth`, `accelerated_buffer_fits`), each
  growth logged and the limits put back as the match ends
  (`note_page_memory`). Past what they may hold, what a frame needs beyond
  it is left out of that frame, logged once a match and named in the
  `+stats` renderer row's note, "sprite memory full", in place of the
  adapter (`FrameStatsRenderer::limit`). A frame whose sprites a cell was
  still evicted under, which the held frames leave no way to, draws none
  of them, since that cell would show another sprite; and the units, 3D
  features, projectiles,
  debris pieces and shatter fragments (`ModelStage`,
  `runtime_full_models.cpp`) as triangles from the models' meshes, a
  textured quad cut into strips across its rows where its two triangles
  would move its texels more than half a pixel from where the processor's
  walk puts them, a quarter of how far it is from a parallelogram at the
  zoom (`walk_strips`, `two_triangle_shift_allowed`), so that a frame
  zoomed out, where quads are small, draws almost none, each
  corner placed from the piece transforms the planner rebuilt with the
  arithmetic of the path the processor draws the piece by (a cached piece
  as its image, a moving piece flat, a carried unit as its carrier
  composes it), the polygons of a unit with a depth plane sorted lowest
  first and, where no order shows the plane's picture, as where two
  polygons cross or the overlaps go round in a circle, each given holes
  where the plane shows one drawn before it (`find_plane_cuts`), which its
  triangles, a textured quad's strips among them, are cut round
  (`cut_under_nearer`, `leave_out_holes`), the palette's tables
  approximated as the design says: the shade
  rows as a per-vertex multiplier with a bright page of doubled texels for
  the rows above unlit, the alpha table as alpha 0.5 for cloaked units and
  shadows, the blue table as a halved colour with an additive lift, the
  nanoframe's bands per polygon and its outline as the processor finds
  it, the first and the last pixel of each row a primitive spans where
  nothing nearer of the image lies, found on a depth plane of the model's
  own and drawn as quads of those pixels, each at least a screen pixel
  across and down in a view zoomed out, diggers' and other players'
  underwater polygons left out, and a polygon filled with the image key
  drawn as the image leaves it, clear: it draws nothing and cuts what it
  covers out of the polygons of the unit's picture drawn before it, as a
  model's walls of the key hide its pieces sunk below the ground (drawn
  flat, it is a colour); texture frames go on
  sprite pages on first sight in two variants, the image key transparent
  as in a cached image or a colour as in a flat draw; shadows go into a
  transparent shadow target the battlefield's size, every silhouette
  replacing what is there so overlaps darken once, an unfinished
  building's first and cut by its image's place as the processor cuts it,
  so that the ground shows unshaded through the image the bands clear,
  composed over the terrain at half darkness by one resolve before the
  list's draws, or
  straight into the battlefield where the target cannot be made
  (`emit_shadows`, the extension point a later better-shadows option
  replaces); with New unit shadows off (`ModelFrameInputs::new_unit_shadows`)
  the target is left empty: each shot casts its shadow sprite just before
  its draw, and each unit and 3D feature the game's own shadow, the silhouette
  the processor blends (`ground_shadow`) as a frame on the texture pages,
  numbered by a digest of its shape so that units of one pose share it,
  one quad at the shadows' darkness sampled as a feature's shadow frame is
  (`game_shadow_of_model`). Zoomed out, the shadows' alpha, and a feature's shadow frame's
  colour and alpha on the sprite stage, take the list's shadow level's
  share; at level 0, which the fade reaches before its strength does
  (`shadow_least_level`), the stage emits no shadow and neither clears nor
  composes the target. As a match ends, the log names its busiest frame's
  vertices, by shadows, models and sprites, and its zoom
  (`FullFrameVertices`). The fog's dither over everything under the dithered option
  (`append_unseen_dither`), palette index 0 at alpha one half, the even
  tone the processor's every-other pixel averages to, over the objects as
  the processor dithers them, since the pages hold no dithered sprite; and
  its black pass over everything (`append_unmapped`), UI colour 0 at the
  corner alphas of the cells never mapped, which blacks out the units as
  the processor does. Last the painters' quads (`paint_world_level`,
  `paint_world_blend`, `paint_world_minimum`): the kill board's shade of
  the world under it and the light of the local player's row, the +stats
  panel's fills, and the shadow, outline and letter edges of game text in
  the modern fonts, which
  in Full ask the card for a quad in place of reading and shading the
  canvas, which holds no world: a shade row as black by alpha over the
  world, leaving row x 0.06875 of it, a light row as white added by
  1 - 1 / (1 + row / 30) (`full_fog::level_quad`), a blend as its colour
  at its opacity, a minimum as each channel the lesser of the world's and
  its colour's; their foregrounds go on the canvas as in every tier. The
  executor runs the frame within the battlefield's scissor; the canvas
  goes up as the overlay by its key (`convert_rgb24_keyed_overlay_argb`),
  in the bands that hold paint, laid over the card's picture 1:1, so
  Basic's overlay by difference, which cannot tell a paint of the base's
  own colour from the base, is not needed; the sprite pages' texels and
  the model stage's pages go up as they change; the HUD strips
  (`draw_accelerated_hud_strips`), from the HUD layer's prescale target
  made as Basic makes it (`ensure_accelerated_match_textures`), the
  dialogs, the cursor and the present follow as in Basic, and the readers
  that keep a picture, the capture and the checks among them, get the
  standard tier's draw (`ensure_screen_world`), since the world layer
  holds no picture, but for the dialogs drawn over a picture of the paused
  match, which keep the card's (`compose_shown_match`). Between the frame and the overlay, the effects pass
  (`full_effects::compose`) works within the lights' bounds. Light alone,
  the weapons', the nanolathes' and the wrecks', is painted over opaque
  black on the processor and added by the card (`full_effects::Pass::light`),
  which reads nothing back, so the processor never waits for the card to
  finish the world: a readback waits as long as the card takes to draw
  it, which anti-aliasing lengthens. Heat distortion moves the world's
  own pixels, so on a frame where it reaches the view
  (`match_effects_distort`) they are read back, processed and drawn back
  in place (`full_effects::Pass::world`). Light raises a channel by its
  whole steps whatever the channel held (`renderer_effects::compose`), so
  the two passes give the same pixels for light alone (`app-full-effects`).
  Full falls back to Basic, which falls back to the standard tier, each
  for the rest of the run (`drop_full`, `TierInputs::full_drop`, a kind
  for each cause): a call of the card's own that fails (`FullCardError`,
  the stages' `full::CardError`), or the failure `--render-fault card`
  forces, drops Full with the failing call struck against the driver as a
  `card` strike (`take_full_failure`), which the same failure in the next
  run on the driver records `full-unusable`; a failed effects pass (its
  readback and conversion or its light, or its upload, which the memory
  guard counts before they are made, or the failure
  `--render-fault effects` forces) drops
  nothing: that frame is drawn without the effects, the failure logged
  once (`full_effects::compose`, `FullPresentation::effects_skipped`); a frame the executor refuses
  before drawing anything of it, one the stages built wrong
  (`FullFrameRefusedError`, `card::Executor::frame_refused`), drops Full
  with nothing struck, since no driver caused it; a failed Full function
  test drops it as the card lacking a feature Full needs, with nothing
  struck;
  the memory guard counts Full's pages and targets
  (`AcceleratedBuffer::card_pages`, `card_targets`) and refuses or drops
  Full before Basic, judging Basic afresh on the memory Full freed
  (`retry_memory_guard`); nothing drops Full for slow frames (design D83);
  and Full's first card calls of a run, the pages made as the match loads
  (`make_full_match_pages`) and its first frame, stand under its own trial
  and sentinel, `path full` (`begin_full_path`), a trial that cannot be
  written keeping Full off with nothing struck. Basic presents the frame
  that failed, with the standard tier's draw of the world, and every frame
  after, with the status saying why Full stopped, until Off and back, a
  raise of the row to Full, or Restore defaults lifts the drop, which
  nothing does for the memory guard's. In a shared game or a replay a
  lower tier applies at once and Full waits for the match to end
  (`SharedMatchGate::full`); its terrain pages, overlay and zoom-in target
  are made as the loading screen begins (`preallocate_full_match_textures`),
  and a page a frame needs later, a sprite page among them, is made then,
  as at any time (design D80). The start-up line
  and `+stats` name the full tier. Off and Basic
  are untouched: nothing here runs unless the tier is Full, which the
  setting's Full or `--hardware-acceleration=full` gives (design D76).
  While Full draws, Maximum zoom out's Automatic reaches
  `kMinFullBattlefieldZoom`, one sixth, three times as far as the
  processor's floor, and the terrain's level rule reaches the atlas's third
  level (design D79); farther out the frame is the far view, which the
  processor draws (`far_view.hpp`). Below the
  processor's floor a tile spans a fraction of a pixel or a few, so the
  terrain's tiles and the fog's quads there have their edges on whole
  pixels (`TerrainView::whole_pixels`, `FogPlacement::whole_pixels`), each
  where its map pixel lands rounded to the nearest, and meet with no row
  or column between them on a renderer that rounds each quad by itself, as
  SDL's software renderer does.
  Anti-aliasing in Full is the graphics card's (`full_supersampling.hpp`,
  `ensure_full_world_target`): the Enhanced anti-aliasing row's level is
  the supersample factor, its samples across, off 1, 2x 2, 4x 4, 8x 8 and
  16x 16 (`render_policy::supersample_factor`), halved to what the
  renderer's texture limit allows at the battlefield's size
  (`render_policy::fit_supersample_factor`) and then to what the memory
  guard allows, each refusal costing a halving and never the tier (design
  D81); above 1 the card draws the terrain, the fog's greyed pass and the
  stages into a world target at that factor, made once with its halves
  and remade when the factor or the battlefield changes, by the plan of
  `full_supersampling::plan_world_target`: from zoom 1 up at the zoom, the
  texture holding the factor's pixels a window pixel, reduced into the
  battlefield by exact halvings (`resolve`); the fog's black pass, its
  dither and the painters' quads go over the reduced picture, as the
  overlay does. Below zoom 1 no world target is drawn: the zoomed-out
  target already holds two texels a display pixel, as many as a world
  target would there, and moves the battlefield between pixels at the
  view's exact place. The
  processor's anti-aliasing never runs in a Full frame
  (`unit_supersampling_` is read as off there), the factor in use is
  logged with the target's size and memory when it changes and shows in
  the `+stats` renderer row and in the row's hint
  (`AccelerationStatus::full_supersample`, beside the rung's
  `supersample`, which the status line names), and a target the renderer
  or the memory guard refuses, or that a shared game's frame would make
  after its loading screen, leaves the tier drawing straight.
  Not yet: the `scale-level` key for Full's rungs, which the game writes
  for no tier, the sprite pages made ahead at a shared game's loading
  screen, smooth panning and native density for the fog passes and the
  painters' quads (they draw on the camera's map pixel at density 1), the
  sprite pages' level 1 (the stage samples level 0 linear when zoomed
  out), and the golden images.
  `app-full-terrain` checks the level rule and the quads over a small map
  on three pages, one batch per page whatever the grid's order;
  `app-full-supersampling` the world target's plan at every kind of zoom
  and battlefield; `app-full-sprites` and `app-full-sprites-data` the
  sprite stage against the bands' picture on SDL's software renderer
  (`full_sprites_test.cpp`); `app-full-models` and `app-full-models-data`
  the model stage against the processor's raster, the latter with every
  installed model with a depth plane drawn in a colour a face, at rest and
  with its pieces turned as moving pieces, its faces held to the
  processor's (`runtime_full_models_test.cpp`); `app-full-fog` the fog passes' quads,
  alphas and placement and the painters' level quads
  (`full_fog_test.cpp`).
- Native pixel density: a window's density is fixed when it opens.
  `decide_window_density` (`render_host.cpp`) decides it before the window
  opens by the render policy's rule (`decide_native_density`): from 2 GiB,
  with neither a flag that names Off, `SDL_RENDER_DRIVER`, a dummy
  or offscreen video driver, an unattended run nor a capture, with Basic or
  Full asked for by the setting or a flag, in a class of machine measured
  at native density, from a start above budget none and a remembered rung
  above magnify off, and with the `native-density` record an earlier run
  left (`note_density_run_end`, `forget_native_density`, which nothing in
  the game calls). The start reads the records before the window opens
  (`HostDisplay`, `RendererHost::open_records`), so the key's driver
  reaches the rule, though no remembered rung does, since the game writes
  no `scale-level` key. No class has been measured
  (`native_density_measured`), so only `--native-density`, the Native
  pixel density setting (`DensityReason::chosen`, after the flag), and a
  build whose platform opens its windows at native density (the
  `OA_NATIVE_DENSITY_WINDOWS` build option, `DensityReason::platform`,
  decided right after the memory and a flag that names Off) open a window
  at native density; every other opens at the window system's density, as
  before. On a window at
  native density (`at_native_density`) `apply_output_mode` lays the match
  out in window points and stretches it over the display's pixels by
  logical presentation, so the processor draws what it draws on any other
  window of that size, and the scene and its budget do not grow; the front
  end's letterbox is unchanged. The standard tier's layers are enlarged
  NEAREST; the accelerated tier draws its scaled layers at the display's
  scale, the layout's scale times the density, with NEAREST kept where that
  product is a whole number (`world_display_scale`, `chrome_filter`), and
  its layers laid out 1:1 NEAREST at a whole-number density and by the
  chrome's filter at any other, plain LINEAR where that filter would need a
  prescale target (`one_to_one_scale_mode`). Below zoom 1 the area pass,
  where it runs, still runs at the layout's size at every density, and the
  card enlarges its result by the density: the scene is not magnified
  there by the zoom times the density over the draw scale. Pointer events
  reach the layout through SDL's view, so picking is unchanged; the edge
  scroll is one layout pixel deep there (`edge_scroll_depth`); screenshots,
  film frames and snapshots keep the layout's size. With
  `--native-density` or the setting, `--check-render-tiers` runs its
  density case alone after the main menu and the loading screen
  (`native-render-tiers-density` and `native-render-tiers-density-setting`
  on the dummy video driver, whose density is 1, and by hand on a display
  above density 1).
- The accelerated tier's watch (`runtime_tier_watch.cpp`), made when the
  tier first switches on in a run, so that a run on the standard tier
  holds none of it (`AcceleratedWatch` in `render_run.hpp`). While the
  tier draws, the memory guard samples the system's memory about once a
  second (`watch_accelerated_memory`) and drops the tier for the rest of
  the run when it trips, which neither Off and back nor Restore defaults
  lifts; before the tier makes its scene and overlay or a prescale target
  it asks the guard with a fresh sample (`accelerated_buffer_allowed`), and
  where the guard refuses, the tier stays on the rung below, magnify off or
  the card's magnification one rung lower (`rung_without`). Nothing lowers
  a rung for slow frames (design D83): the player's settings hold, however
  slowly the machine draws them, and +stats shows the cost. Each step the
  guard takes lowers the rung for the rest of the run
  (`lower_accelerated_rung`), freeing what the lower rung no longer draws
  with: the scene and its terrain buffer when magnify goes off or the
  budget falls, and the prescale targets the chrome or the card's
  magnification no longer use, the magnified scene's only when the card's
  magnification changes, since the NEAREST-chrome rung leaves the scene's
  filter as it was (`world_filter`); each step is logged once, until Off
  and back or Restore defaults starts the ladder again from the top. A
  later switch-on, after a lost device or a shared game, keeps the rung
  reached. The rung is not kept for the next start: the game reads and
  writes the renderer records, but not their `scale-level` key, which is
  reserved for it. `--check-renderer-ladder --render-fault memory`
  (`native-renderer-ladder-memory`) forces the guard's sample, which
  refuses each buffer and then drops the tier, freeing the scene's
  buffers.
- The tier each frame is drawn in (`runtime_render_tier.cpp`): at start,
  once the renderer is made, `RendererHost::decide_start_tier` fills the
  render policy's facts (the flags, the Hardware acceleration setting read
  before the window opens, `SDL_RENDER_DRIVER`, a video driver with no
  window, a named preferences file, the machine's physical memory against
  the 2 GiB threshold, and what probe items 1 to 3 found of the renderer:
  on Windows before Vista only `direct3d` is capable, and under
  `SDL_RENDER_DRIVER` the adapter is read only when
  a flag that asks for the card or `--force-capable` asks for more than SDL's
  own start) and, where the tier could be accelerated but for it, runs the
  start-up function test (`run_function_test`): a render target cleared
  and read back, a LINEAR reduction by half within 2 of the texels'
  average, PIXELART (`probe_pixelart`), and a seeded pattern drawn NEAREST
  through a source rectangle into a prescale target, reduced LINEAR and
  overlaid, read back against `sharp_bilinear_rgb24` and `overlay_rgb24`
  within 3 and 0.5 on the mean. The trial `probe <driver>` is written and
  flushed before the test (`RendererHost::test_function`), so a start on
  the player's own profile runs it by itself where the policy allows; a
  trial that cannot be written skips the test and keeps the standard tier
  (`FunctionTest::trial_unwritten`) until the player tries again. With a
  named preferences file the records live in memory and it runs where the
  file sets the setting to Basic or Full. The start-up line names the
  tier, standard, basic or full, with what it does, "(Full is not in this
  build)" after basic where Full was asked for, or the reason the
  processor draws everything (`tier_description`); the `+stats` renderer
  row names the tier the same way. Full, the battlefield drawn on the
  graphics card (the Full tier, above): the render policy's request
  (`TierInputs::setting`, `AccelerationFlag`) carries Off, Basic or Full,
  and `decide_render_tier` gives `RenderTier::full` where Full was asked
  for, the driver has no `full-unusable` record or
  `--hardware-acceleration=full` was given, Full was not dropped for the
  run, and a shared game or a replay began in Full; otherwise Basic,
  with the Full reason (`FullReason`) in the decision, which the status
  and the start-up line's note say. Before each frame, `Runtime::update_render_tier`
  brings the facts up to date (the flags, the setting in effect, the
  director, a lost device) and takes the frame's step from the render
  policy (`step_tier`): the tier, the function test run where only it is
  missing, the frame noted in a shared game or a replay, and the switch
  that makes the accelerated presentation match, on at the machine's
  starting rung or off, with the Full branch marked where the tier is Full
  (`set_full_presentation`). Setting Hardware acceleration to Off applies at
  once; Basic and Full apply at once too, except in a shared game or a replay, known
  from its bootstrap (`MatchBootstrap::multiplayer`, `replay`), which keeps
  the tier it began with until it ends (`begin_render_tier_match`,
  `end_render_tier_match`). Setting it to Off and back, raising it from
  Basic to Full, or Restore defaults,
  clears the renderer records' strikes and failure records in memory, lets
  a failed function test or an unwritten trial try again, lifts a drop of
  either tier other than the memory guard's and starts the ladder again
  from the top, at once where the tier stays on (`take_renderer_retry`,
  `forget_render_failures`); OK writes the cleared records
  (`keep_renderer_records`) and Cancel puts them back
  (`restore_renderer_records`). A failed call of the accelerated tier
  drops it for the run and is struck against the driver
  (`take_acceleration_error`), and so is a renderer made again after a
  present error, a lost device or three resets within a minute; the memory
  guard drops it for the run with nothing struck. The dialog's status and
  locks follow these facts
  (`tier_acceleration_facts`, with what the records hold,
  `RendererHost::fill_record_facts`); a driver that failed in the run, a
  record and a driver a record passed over lock nothing, so that the row
  can retry them. `+stats` names the tier. `native-engine-settings` sets the row to Basic, Full
  and Off through the dialog under `--force-capable` and retries it after a
  drop and after a function test forced to draw wrongly.
- `runtime_match_menus.cpp`: the in-match menus. A dialog opened over the
  match HUD (the exit menu, the surrender confirmation, RESTART.GUI, the
  Game Settings sheet, the removal question) is placed as 3.1c's panel
  loader places it and keeps the panel it opened over drawn under it,
  darkened where 3.1c darkens the panel below (`open_match_dialog`). A
  dialog centred on the whole screen is drawn at the canvas's pixels over
  the side column too (`match_dialog_side_`); while
  the in-game menu or the tab menu is open the panels show their keyboard
  focus. Beside the in-game menu and the pages it opens the right button is
  still the battlefield's and the radar's, as in 3.1c
  (`right_press_beside_menu`): a press acts as in play, and one that drops
  the selection also closes the menu, which resumes a match the menu held,
  as F2 does; the Pause key's pause is left as it is. The load and save dialogs darken the panel below with the frontend
  renderer's `shade_panel_below`, as the frontend dialogs do. Over a match
  both open centred on the screen, at every window size and interface
  scale (3.1c leaves the save dialog where its GUI file puts it; the engine
  centres it on purpose);
  at the end of a mission the save dialog keeps its place from the GUI
  file, as in 3.1c (`enter_load_game`, `runtime_load_game.cpp`). The
  dialogs' buttons are clicked as the frontend's own are: a press holds the
  button, drawn pressed while the pointer stays over it, and only a release
  over it acts; a release away from it does nothing
  (`activate_load_game_gadget`). The save dialog saves only through OK and
  Return at the end of the name; a click on the name field gives it the
  keys (`savegame_on_save_press`). In the load dialog Return is OK and
  Escape is CANCEL, as in the save dialog: Escape leaves either dialog as
  its CANCEL does (`leave_load_dialog`), for the screen it was opened over,
  as in 3.1c: Single Player, the in-game menu or the end-of-mission panel,
  with saves listed or none. Every panel the match HUD loads, a unit's
  page, the general or idle page, the in-game menu or a dialog, is laid out
  over the side column's art (`load_match_hud_layout`, `match_hud_art`):
  the side tile, the GUI palettes and tables, `commongui.gaf`'s icons and
  the side's interface GAF in the shown language's captions, and the side's
  font, read from the game's files as the match's first panel loads and
  again only for another side's files, the next match, or a change of the
  language shown or its packs. A panel reads only its own GUI file and GAF,
  so that the right press of the left-click interface that drops the
  selection, which loads the idle page again, takes no frame's time
  reading the archives again. The art is held once: each panel takes it
  from the panel shown before it (`take_match_hud_art`), which gives back
  `commongui.gaf`'s icons without the sequences it added, and draws its
  chrome over the side tile decoded again from the file the art read, so
  that a match holds no more of it than one panel did when every panel
  read its own. The pointer interface check (`native-pointer-interfaces`)
  counts what the pages a selection and two right presses open read, and
  checks that they took the art without copying it. A unit's page or the order page that opens is
  drawn into the HUD layer alone (`compose_match_hud`, the HUD part of
  `draw_match_surface`): the bars widened to the window, the panel and what
  goes over it, so that the pointer meets the page where the next frame
  shows it (`match_hud_strips`), and that frame draws the whole match once,
  as every frame does.
- `runtime_scroll_bars.cpp`: the scroll bars of the frontend screen's panel
  and of the match HUD's panel (`renderer::LayoutScrolls`), bound as each
  panel's first draw binds them, drawn over it, driven by the pointer and
  each frame's tick, and kept in step with the lists they scroll; a slider's
  move runs its options callback or sets the share panel's amounts. The
  preferences' sub-panel keeps the side column's scale down to its bottom
  over the battlefield wherever the window puts the bottom bar
  (`place_preferences_rows`). `runtime_scroll_bar_check.cpp` holds
  `--check-scroll-bars` and the pointer helpers other checks drive scroll
  bars with.
- `runtime_team_panels.cpp`: the team panels of a multiplayer game over the
  running match: the tab menu (Tab), SHARE.GUI ('h'), ALLIES.GUI,
  CONTROL.GUI and its removal question, laid out and answered by
  `ui/hud/team_panels.hpp` and `share_panel.hpp`; what they tell the other
  players' machines goes through the extension's `TeamPanelHost`.
  `runtime_team_panel_check.cpp` checks the Pause key and the panels in
  `--check-navigation`.
- `match_clock.hpp`, `match_clock.cpp`: when the match clock steps. A menu
  and the outcome hold a match played on this machine alone; a match shared
  with other players' machines runs on under its menus, the preferences they
  open (`Runtime::match_running`) and while it waits on its outcome. The
  pause bit of `Game.sim_run_flags`, which the Pause key
  flips (and another player's machine may set), holds any match inside the
  clock, whose time moves on so that nothing is caught up on resuming; a
  save stores the bit as the match holds it. A frame whose clock reading
  lies behind the clock's last step (a load, a screenshot or a film frame
  set the clock past the frame's time) holds its step; the reading turns
  over to 0 about every 39.8 hours, and a reading more than half a turn
  behind has turned over: the frame steps and runs no tick, and the frames
  after it step as before, as in 3.1c. `away_action` says what a match does
  while the window is away: a game played alone pauses or plays on, as When
  the window loses focus says, and nothing else changes. `app-match-clock`
  tests all of these, with frames across the turn from a clock just before
  2^32 milliseconds.
- `memory_guard.hpp`, `memory_guard.cpp` (part of `oa-app-render-policy`):
  the memory guard of the accelerated tier, as a pure state machine with no
  clock, which the tier's watch acts on (above). Handed a sample of the system's
  memory about once a second (`oa::platform::sample_system_memory`), it
  asks the tier to drop acceleration for the rest of the run when the
  process's private committed memory rises above half of physical memory,
  or when free physical memory stays under a sixteenth of it for 3 s;
  where the system reports no free memory, its memory pressure at the
  critical level stands in for it, or else more than 128 hard page faults
  a second. These figures are conservative, chosen without a measurement
  on period hardware. Before the tier makes a buffer of its own,
  `memory_guard_allows` tells whether free memory would stay at or above
  its threshold and committed memory at or under its own.
  `app-memory-guard` tests it by table, and `platform-system-memory`
  samples the system it runs on and prints what it reports. The guard is
  built into the render policy's library, under the
  `oa::app::render_policy` namespace it uses; its header, source and test
  stay files of their own.
- `frame_pacing.hpp`, `frame_pacing.cpp`, `frame_stats_panel.hpp`,
  `frame_stats_panel.cpp`, `runtime_frame_stats.cpp`: the
  application loop's frames, apart from the simulation's 30 ticks a second.
  The loop draws up to `--max-fps` frames a second (120 unless it says
  otherwise; 0 for no limit, and never fewer than 30, so that a frame on
  time never runs two ticks at normal speed), each frame standing for its
  time on an evenly spaced run of frames (`FramePacer`), with a precise wait
  between them; a frame that ends late starts the next at once and the run
  goes on from it, never with frames bunched to catch up. At 30 a second,
  the tick rate, each frame is due at the middle of the match clock unit
  after the last one's (`next_clock_unit_middle`), so that each frame on
  time steps the clock by one unit, runs one tick at normal speed and shows
  it whole; frames evenly spaced at that rate from any start would, from
  some starts, run none and then two, as the clock's whole milliseconds turn
  its units over. While nothing
  moves on its own (no stepping match, no camera motion, no input for half
  a second), a paused multiplayer match among them, it draws 30 a second,
  and an event ends the wait at once. While Vertical sync is in effect the
  rate is also held to the largest whole rate below the display's, read
  each frame from the window's display (`vsync_frame_cap`), and never under
  30. The match clock steps to each
  frame's time, and the frame is drawn the fraction of the way between the
  state before the last batch of ticks and the state after it that its time
  stands for (`presentation_alpha()`, `next_presentation_alpha`): never
  past the current tick, whole ticks while the match is paused, waits on
  another machine or catches up, on a check's fixed clock, for a film
  frame and while Smooth motion is Off (`smooth_motion()`), when the
  frames between two ticks repeat the tick's picture at the same rate, and
  1 again once the frame is drawn, so that every other drawing shows whole
  ticks. At 30 a second a frame counts its clock unit whole: at
  normal speed and above it shows the state its ticks reached, and below
  normal speed the progress the clock makes by the unit's end, so that
  frames still move evenly between the ticks. The unit drawing adds what it
  drew to `frame_draws_` (`FrameDrawCounts`). The camera scrolls and the
  zoom eases for each frame's real time (`scroll_distance`), so they move a
  steady amount every
  frame, and a camera tracking a unit is centred, after the clock step,
  where the frame shows the unit (`place_tracking_camera`), so that the
  unit holds still on the screen, and the pointer, clicks and the build box
  map through the camera the frame is drawn from. The resource readout
  eases toward the stores 120 times a second of frame time, as often as it
  eased at the default rate. Drawing a unit refreshes the piece positions
  some of its script's queries read, so a tick with no frame drawn after it
  (a frame slower than a tick, or a batch of ticks above normal speed) can
  still change the match, as it could before frames were drawn between
  ticks. "+stats", an option command the runtime adds to the console,
  shows a panel at the battlefield's bottom right: the battlefield
  darkened under it, a black outline and a raised edge in the GUI
  palette's light and dark edge colours, and on it a table
  (`frame_stats_table`) titled "Frame stats (ms)" of the frames a second
  of the last second, with the rate the loop keeps, the frame, work, tick,
  draw and present times' least, mean and most in columns, the units
  the last frame drew, the units' picture ("models": the megabytes the
  model bridge holds, "MB of" its budget, and "dots" when the last frame
  drew the units as dots), the renderer: the tier frames are drawn in and
  the render driver, as in "standard: metal", with the adapter's name, or
  "sprite memory full" while the full tier leaves out of a frame what its
  pages may not hold, each cut to 23 bytes, never inside a character; and
  the display
  (`set_display_row`, `Runtime::frame_stats_display`): "window", "full
  screen" (on the desktop's mode) or "exclusive" (at a mode of the game's
  own) with the size the match is laid out and drawn at, as in "full screen
  1280x720", then the display's mode, its refresh rate and its scale
  (`SDL_GetWindowDisplayScale`), as in "1728x1117@120 2x". The panel keeps
  its width whatever the names: it cuts the renderer and display rows, in
  its font's own widths, where they would pass the panel's padding
  (`fit_run_on_row`). Each
  time is graded as it is taken, against the
  allowance of the frame it belongs to (`frame_allowance_ns`: 1 / the rate
  kept, and half a millisecond after a precise wait or two after an idle
  one, whose wait is rounded up to whole milliseconds), and shows in a
  green within it, the health bar's yellow over it but within a tick, or
  its red, on a red cell, over a tick (`time_severity`), so that times
  keep their colours when the rate changes. Under the table, a graph of
  the last two seconds of frames laid end to end (`FrameHistory`, a
  column for each 1/120 s holding the longest frame that covers it): a
  frame a column at 120 frames a second, a 33 ms idle frame four columns
  wide, and a hitch as wide as it lasted, each bar in its frame's grade's
  colour, capped in white past the graph's 40 ms, over a gray line at a
  tick and a fainter dotted one at the frame's allowance.
  `frame_stats_panel` lays the panel out in the match label font for the
  widest texts the table shows (`frame_stats_widest_table`, which keeps no
  room for the renderer and display rows), so nothing in it moves from
  frame to frame,
  and places it at the HUD's text scale, or
  at the largest whole scale below it at which it fits the battlefield's
  bottom right quarter; it reads the statistics and writes nothing of the
  match. The console check types "+stats" and checks the panel's outline,
  edge, fill and graph, that it fits the quarter at window sizes from
  640x480 to 3840x2160, that every column of the graph has its frame's
  height and colour over two seconds of late and slow frames, the lines,
  the colours, alignment and red cells of the table, that the last row
  names the window and the frame drawn, that nothing moves,
  and that nothing outside the battlefield's bottom right quarter changes. 3.1c
  has no command that shows these times; its frame rate shows as "FRATE:"
  on the debug keys' line (F11 after the developer passphrase), with
  "[Release]" and "MODE DEBUG INFO ON" or "OFF", which
  `draw_debug_status_line` draws as 3.1c does and the console check
  checks. `app-frame-pacing` tests the pacing, the fraction, the figures,
  the history and the grades over a fake clock, among them a frame for each
  tick at 30 a second from any start, each shown whole, and how faster and
  slower game speeds show at that rate, and
  `app-frame-stats-panel` the panel's rows, columns, notes and graph, its
  place and scale, the bars and lines and each grade's colour. `--frame-rate FPS` with `--match-ticks` plays
  the headless skirmish frame by frame on a clock of its own, moving the
  camera and stepping the clock as the loop does and drawing each frame as
  the loop does; `--frame-log FILE` writes each frame's time, tick,
  fraction, camera and a unit it follows, where the simulation holds it and
  where the frame drew it, `--scroll-camera` sweeps the camera's scroll
  right and back over the army, `--march` sends the local army south,
  `--follow` tracks the unit the log follows and `--frame-clock MS` starts
  the run's clock MS milliseconds in instead of at 0. `native-frame-rate`
  checks that 30, 60, 120 and 144 frames a second write one trace stream
  and reach one world digest; that at 30 each tick has one frame, which
  shows it whole; that at 120 the camera moves evenly, each frame shows a
  quarter of a tick more, and the unit drawn moves on nearly every frame,
  none carrying more than half the most it moves in a tick; that a
  tracked unit is drawn at one place of the screen on every frame; that
  with Smooth motion Off a run at 120 writes the same trace and digest
  with no frame between ticks, the camera moving as evenly and the unit
  drawn where each tick holds it; and that runs at 30 and 120 from 2
  seconds before 2^32 milliseconds, where
  the match clock's reading turns over to 0, step on through the turn to
  the same trace and digest, at 30 each frame running a tick but the first
  past the turn. The run
  ends with a line giving the world digest, a frames digest of every
  frame's drawn battlefield, the drawing threads it drew on and the most
  bands a frame's battlefield was drawn in. `--busy-combat` adds missile
  trucks to both armies, and for the local player a kbot lab building
  peewees and an air transport loading one, and selects the local army
  with selection boxes shown, so that the frames reach every kind of
  battlefield draw. `--stage FILE` sets up a headless skirmish of
  `--match-ticks` ticks from a file of actions, one a line, after any
  `--combat` armies: `unit PLAYER TYPE DX DZ [FROM]` places a finished unit
  of a player DX, DZ map pixels from where the first unit of player FROM
  (the owner by default) stood, `build TYPE DX DZ` queues the last unit
  placed to build a type DX, DZ map pixels from it, `stockpile ROUNDS` has
  the last unit placed build rounds for its first weapon, `console LINE` enters a chat line as
  the local player, `type LINE` opens the chat line and types a line into
  it, leaving it open, `pointer X Y` moves the pointer to a point of
  the window, `click X Y` presses and releases the pointer's left button
  there, `key NAME` presses and releases a key SDL names so (`Space`,
  `Down`), both reaching an open dialog or menu first, as the player's do,
  and `settings SECTION` opens the settings dialog beside the
  in-game menu at the section its list names so, such as
  `settings Language & Text` or `settings User Interface`, or at the
  section its word names in every language, such as `settings language`
  or `settings user-interface`. `settings Language` still opens Language &
  Text. The line prints `stage: settings SECTION at X,Y WxH,
  version at X,Y WxH`: where the dialog shows and the room its header keeps
  for the version text, in the match canvas's pixels, so that a golden
  frame can leave the version out. That room is the header's version part
  as the dialog's class lays it out, from 71 points left of its right edge
  to 5 points right of it, and the header's rows 6 to 22, at the dialog's
  scale: at 640x480 `at 144,78 480x324, version at 540,84 76x16`, and at
  1920x1080 `at 480,240 960x600`, Large at 1×, with a version room of
  76x16;
  `native-stage` checks it. `gallery` opens the settings dialog beside the
  in-game menu at Developer and takes its UI kit gallery row's SHOW as the
  dialog takes it, so that the UI kit gallery covers the window; it stops
  with `<where>: the gallery did not open` unless the gallery is then the
  top screen, and prints `stage: gallery at X,Y WxH CLASS Nx`, where it
  shows in the match canvas's pixels, its size class and its scale: at
  640x480 `at 0,0 640x480 Compact 1x`, at 1280x720 `at 0,0 1280x720 Large
  1x` and at 1920x1080 `at 0,0 1920x1080 Large 1x`. Its text holds no
  version, so a golden frame of it takes the whole window. A save/load run ends by
  reporting the live units by type, the veterans by veterancy level, the
  stockpiled rounds, the buildings by facing and each player's health.
- `runtime_stage.cpp` and `stage_state.hpp`: the parts of a stage that play
  out over its match. `place PLAYER TYPE X Z [FACING]` places a finished
  unit at map pixel X, Z, turned a quarter (`east`), half (`north`) or
  three quarters (`west`) from the way its type is built facing (`south`,
  the default); `group NAME` gathers the units the stage places after it,
  by `unit` or `place`, into a group, and `group` alone stops gathering;
  `move GROUP X Z` and `patrol GROUP X Z` give a group's live units that
  order to map pixel X, Z as a player's order gives a selection of them
  (each unit near the group's centre keeps its place around the point, and
  one far from it goes to the point itself); `attack GROUP TARGETS` sets
  each of them on the nearest live unit of another group, `attack-ground
  GROUP X Z` commands them to attack the ground at map pixel X, Z, `guard
  GROUP GUARDED` sets them guarding the first live unit of another group,
  and `activate GROUP` and `deactivate GROUP` switch them on and off with
  the order panel's ON/OFF button's orders (`give_state_order`, which the
  button gives each selected unit), as a player's orders do; a type that
  cannot be switched on and off stays as it is. `move-order GROUP STANCE`
  and `fire-order GROUP STANCE` set their standing move or fire order to
  stance 0, 1 or 2 (Hold, Maneuver, Roam; Hold Fire, Return Fire, Fire at
  Will) with the panel's standing-order buttons' orders; a type without
  that button stays as it is. `health GROUP PERCENT` leaves each live unit
  of a group with that share of its type's health, rounded down and at
  least 1. The pointer's lines act as a player's pointer does:
  `select GROUP` makes the group's live units of the local player the
  selection, `interface left` or `interface right` sets the interface type,
  `arm COMMAND` arms an order button's command (`none`, `move`, `attack`,
  `patrol`, `guard`, `repair`, `reclaim`, `capture`, `load` or `unload`),
  `hover GROUP` centres the view on the group's first live unit, as far as
  the view's limits let it go, past the map's edges too, and moves the
  pointer onto it, printing `stage: hover GROUP: unit N under the
  pointer, cursor NAME` with the unit the pointer picked and the cursor's
  CURSORS.GAF sequence, and `press left` or `press right` presses and lets
  go of that button there, printing `stage: press BUTTON: ORDER` with the
  order the press gave (`-` for none); `native-air-pads` uses them.
  `drag X0 Z0 X1 Z1` holds the select button from the point of the
  battlefield that shows map pixel X0, Z0 at height 0 to the one that shows
  X1, Z1, past the map's edges too, and lets go there, as the player drags a
  box; a point out of view is held a pixel inside the battlefield's edges,
  and the same point twice is a click there. It prints
  `stage: drag X0,Z0 to X1,Z1: RESULT` with what the box or the click did
  (`2 selected`, `No units`, the unit a click selected, `-` for nothing);
  with `--snapshot` the frame with the box still held is written beside it
  as `-drag`. `native-off-map-selection` uses it.
  `at TICK` before any action keeps its line for that match tick, and the line
  runs before the tick does (`run_due_stage_lines`, from every step of the
  match), the lines of a tick in the order the file gives them; each tick
  that runs lines prints `stage: tick TICK` before their own lines. A
  director script that names a stage in place of a recording renders the
  headless skirmish the stage sets up and plays out
  ([docs/director.md](../../docs/director.md)). `native-stage-render` checks
  the timed lines, the orders, the lines a stage refuses and two renders of
  a stage; `native-group-orders` checks that a block of fighters and a row
  of kbots moved as groups keep their shape where they land and stop.
- `xrgb_conversion.hpp`, `xrgb_conversion.cpp`: each frame's RGB layers
  converted into the window's 32-bit pixels (0xffRRGGBB) as they are
  uploaded, through the display gamma's table when the gamma is not 1, in
  bands of 32 rows; when SDL's software renderer draws into a 16-bit RGB565
  window (`frame_texture_format`), the match's layers are converted into
  RGB565 pixels instead, the ones SDL would make of the 32-bit pixels, so
  that presenting copies them as they are. In the Full tier the world
  layer is the overlay canvas, cleared to a key colour, the lowest
  0xRRGGBB not in the palette (`overlay_key_colour`), and
  `convert_rgb24_keyed_overlay_argb` makes the overlay of it by that key,
  opaque exactly where the painters painted, in the same bands. The
  Runtime keeps a [job pool](../platform/job-pool/README.md)
  (`draw_pool_`) that this conversion, the terrain fill, the fog, the
  battlefield's draws (`world_draws.hpp`), the effects' light and heat
  (`renderer_effects::compose`) and the 3D view's depth under them
  (`live_scene_visibility`) run their bands on:
  `--draw-threads N` (1 to 32), else the `OA_DRAW_THREADS`
  environment variable, else the pool's default, one thread on a machine of
  one or two logical processors and otherwise one fewer than the
  processors, at most four. With one thread there is no pool and every
  band runs on the drawing thread. The conversion's, the terrain's, the
  fog's and the effects' bands are fixed by the rows, and the 3D view's
  depth is worked out in bands of 16 rows, or in one band with no pool,
  each band taking the triangles that reach it in their order; the
  battlefield's are one a thread, and any number of them draws the same
  frames, so every count draws the same frames.
  `app-xrgb-conversion` checks each pixel's packing and gamma for rows of
  any width, that pools of 2, 3, 4 and 8 threads convert the same bytes,
  the key colour as the lowest outside a palette and the keyed overlay
  holding what was painted;
  `native-draw-threads` draws the seeded skirmish's fight frame by frame at
  zoom 1, 1.37 and 0.6, and a longer `--busy-combat` fight into its
  explosions and debris with enhanced anti-aliasing off and at 4x, on 1, 2,
  3, 4 and 7 drawing threads and checks that every count gives the same
  frames digest and world digest, and draws in one band a thread at zoom 1
  and in more than one elsewhere.
- `full_screen.hpp`, `full_screen.cpp`: Alt+Enter (Return or keypad Enter,
  either Alt key; Option on macOS), which switches the window between full
  screen and a window on every screen, during the movies and while a match
  loads; its Enter key's repeats and release reach no screen, even once Alt
  is let go. While macOS, X11 or Wayland is
  still switching the window, a second press switches from the mode last
  asked for. The window opens with `game_window_flags`: full screen on
  Windows unless `-d` is given, and at the display's own pixel density only
  where `decide_window_density` allows it (`render_host.hpp`). In full
  screen, whether on the desktop's display mode or on the one the Screen
  size setting picks, and while the
  window has the input focus, the pointer is kept on the game's screen, as
  in 3.1c (`keeps_pointer_on_screen`, `keep_pointer_on_screen`): it stops at
  the screen's edges, can rest on their last row or column of pixels, and
  never strays onto another monitor. Windows clips the cursor to the window,
  macOS confines it to the window's content, X11 grabs it inside the window
  and Wayland confines it there, each following the window's size and
  display. A screen that answers none of the window's events, the progress
  dialog while the game's archives load at start among them, still lets
  them settle the hold (`drop_window_events`), and the menu settles it as
  it takes the window over, so a window that opened in full screen holds
  the pointer from the start, as one Alt+Enter switched does. macOS places
  a full-screen window below the camera housing of a MacBook's built-in
  display, with the strip beside the housing above it; there a pointer in
  that strip, or past any other edge of a full-screen window on its
  display, touches that edge for the edge scroll (`full_screen_pointer`).
  Switching to another program (Alt+Tab, Command+Tab), a dialog of the
  system's taking the focus, or Alt+Enter to a window lets the pointer
  go, and coming back to full screen with the focus holds it again. A window
  keeps the pointer only while a match's battlefield shows and the window
  has the focus, as Window edge says (`apply_window_edge`). Capture, the
  default, takes SDL's window grab, which confines the pointer to the window
  on every system the way full screen's does, so no push or flick takes it
  out and every edge scrolls (`apply_capture`). Escape and a lost focus let
  it go until a press on the battlefield; after a lost focus that press and
  its release reach nothing, so the click that brings the window back is no
  order (`take_window_edge_event`). Sticky holds the pointer at the edges
  while the pointer is in the window (`sticky_edge.hpp`), as firmly as Edge
  stickiness says (`sticky_limits`): the pointer rests on the last pixel and
  the view edge-scrolls. On macOS, Windows and X11 it stays free and is put
  back on that pixel until a firm push lets it go; on Wayland it is
  confined, and a quick approach lets it go. Menus, the pause screen,
  Settings, a message box, a window without the focus and Window edge Free
  leave it free. The window events that may change this
  (`changes_pointer_bounds`) settle it in every event loop that takes
  Alt+Enter (the movies, a match loading and the game), and the window's own
  state settles it after the game drops pending input. The game lets the
  pointer go (`release_pointer`) before it shows an error or information
  box, before breaking into a debugger and on exit. The system's pointer is
  hidden wherever the game draws its own cursor, in play and in the game's
  menus, dialogs and message boxes; it shows only without the game's
  cursors, while the game is inactive and over the Game files screen
  (`system_pointer_wanted`). Every event, every frame of the loop and every
  change of screen apply that rule again (`apply_system_pointer`), so a
  pointer shown anywhere else is hidden again by the next frame. A window
  that leaves full screen, by Alt+Enter, by the window system's own control
  or before a debugger break, comes back onto the display it was full
  screen on
  (`window_on_display`, `bring_window_on_display`): each side of its frame
  (the title bar and borders included where the window system reports them,
  as Windows and X11 do) that lies outside the display's usable area
  (without the menu bar, the dock or the taskbar, so that the title bar can
  be reached) moves 5% of that area's width or height inside it, and a
  window with no side outside stays where it is. A window too wide or too
  tall to fit between two margins keeps the left or top margin, so that its
  title bar and controls are on the display, and shrinks to fit between the
  margins; the screen is laid out again at its new size as after any resize.
  Displays left of or above the primary one, at negative coordinates, are
  handled alike. The window is checked once the window system has given it
  its place as a window, on the window's own events, for at most two
  seconds after it left: Windows places it as it leaves, macOS as it leaves
  its full-screen space, and an X11 window manager once it has put the
  window's decorations back (a window manager that reports no decorations
  leaves the window where it puts it). Wayland places windows itself and
  refuses to move them, so there the window stays where the compositor puts
  it. A maximised window is left as the window system fits it, and a window
  the player moves off the display later stays there. A Screen size chosen
  in full screen is the window's once it leaves (`FullScreenSwitch::window_width`,
  `window_at_size_on_display`): the window takes the size and keeps it,
  moved no more than it must to lie on the display, from its left or top
  edge where it is larger; entering full screen again forgets the size.
  `app-full-screen` tests
  the keys, the modes, when the pointer is held and where a window goes on
  its display (each side out, corners, windows too large, displays at
  negative coordinates, edges exactly on the display's), over windows of
  SDL's dummy video driver that switch modes, lose and regain the focus and
  come back onto the display from off it, also with a usable area smaller
  than the display, and `--check-frontend-controls` presses Alt+Enter on a
  menu, over a message box and in a match, and checks that full screen
  holds the pointer, that Window edge Capture keeps it in a match's window
  through pushes and flicks, lets it go in the game menu, Settings, on the
  pause screen, after Escape and without the focus, and takes it back on a
  click on the battlefield, the focusing click reaching nothing, that Sticky
  holds it at an edge until a push as firm as Edge stickiness lets it go,
  0.0 never holding, and that Free leaves it free, and that the window comes back
  at its own size, or onto its display when it started off it (the dummy
  driver's display is smaller than the game's first window).
  The Window frame setting hides a window's title bar and borders while a
  game is played and shows them on every other screen and while the game
  menu or a panel it opens is up (`window_frame_request`, applied each
  frame by `Runtime::apply_window_frame` in `runtime_screen_size.cpp`);
  Always shown keeps them. Full screen, a window still switching to or
  from it, and a run without a window are left as they are. Windows and
  X11 keep the window's contents at their size as the frame comes and
  goes; macOS keeps the frame's, so the contents are put back at the size
  and place they had (`set_window_frame`), and the screen size the
  settings show never changes with the frame. A maximised window is left
  at the size the window system gives it. The full screen or window the
  player left is remembered (`open-annihilation.window-place`,
  `runtime_window.cpp`) when it changes and when the game closes, and the
  next start opens that way directly: the same display when it is still
  connected, otherwise the main one, a window kept wholly on the screen.
  `-d` and `--resolution` win for that run and are not remembered. A first
  start is unchanged, and a build whose touch controls are on from the
  start does not keep a desktop place. `app-full-screen` asks for the
  frame in each case and through a game, its game menu and full screen,
  and checks that a saved place round-trips, including a window whose
  display has gone. With `--trace-input`, the window writes one line as a
  match begins and after each event of the window or a display
  (`traced_window_event`, `Runtime::trace_window` in `runtime_present.cpp`,
  called from `dispatch_event` once the event and Window edge are settled):
  its place against its display's bounds and usable area, its state, how
  the pointer is held and where the pointer is (`window_trace_line`,
  `window_edges`, `pointer_place`), so that a log tells why an edge stopped
  scrolling ([`--trace-input`](../../docs/command-line.md#--trace-input));
  `app-full-screen` checks the lines.
- `runtime_hud.cpp`, `runtime_match_hud.cpp`: the HUD.
- `runtime_messages.cpp`: the in-game message log (`Game.chat_lines`) drawn
  over the battlefield, and the speed and message part of `--check-navigation`.
- `runtime_seat_chat.cpp`: chat a computer seat on this machine reads and
  sends. The listener, the pace and the queue live in `oa-app-seat-chat`.
- `runtime_console.cpp`, `runtime_console_debug.cpp`: the in-game console's
  host hooks, the debug grid, "Profile" bars and DebugBreak, and the console
  part of `--check-navigation`. `PrintWeights` writes a seated brain's
  `report()` text, or one line of the brain's name, its version and
  "gives no report." when that brain has no report, and today's report
  when the built-in player has the seat. `ReloadAIProfiles` reloads the
  built-in players' profile and calls each seated brain's `start` again
  with that profile. `native-brain-console` checks both on a headless
  skirmish.
- `runtime_console_cheats.cpp`, `runtime_console_sound.cpp`: the cheat flag
  each mission start sets, from `sim::scenario::session_cheats_allowed`: a
  campaign and a skirmish run cheats, a multiplayer game while its host's
  CHEATING option is on (3.1c refuses them in a campaign; the engine
  differs there on purpose). "+Sing" flips the novelty voice,
  which lasts from match to match until the program ends and no save
  holds; its two sounds are the mod profile's `strings.cheat.sing-sounds`,
  else 3.1c's honk and sing, bound to the announcement gates as each match
  starts. `check_console_cheat_effects` types every cheat 3.1c registers
  through the chat line and checks what it does to the match, in the
  skirmish of `--check-navigation` and in the campaign mission of
  `native-campaign-restart`, which also checks "+Sing";
  `native-campaign-sing-sounds` runs it under a profile that names its own
  sing sounds. The match view (`match_view_player`) follows
  `Game.viewpoint_player`, which "+View" moves: the fog, the units drawn,
  the economy and the units that speak follow the viewed player.
- A sound that does not play, through any of the game's routes, is
  reported on stderr once a run for each sound, as "sound unavailable:"
  and why, and never for one 3.1c's own data names but never shipped
  (`report_unplayed_sound`, `audio::game_audio::known_missing_sound`); a
  sound whose file was not found is not looked for again in the run
  (`sound_found_missing`).
- `runtime_unit_speech_check.cpp`: `--check-unit-speech`
  (`native-unit-speech`): the commander clicked says its select line, and
  clicked onto open ground its order line, in a skirmish and in a second
  one started after it. Each match starts the unit speech queue empty, with
  no category cooling down (`NativeOfflineServices::bind_announcements`),
  as each mission start does in 3.1c, so what one match said never
  silences the next.
- `runtime_mutator_layer_check.cpp`: `--check-mutator-layer`
  (`native-mutator-layer`): a skirmish with no mutator, one that changes
  the commanders, the same skirmish again, and one mutator that does not
  apply. The layer is mounted only while the match that applied it runs.
- `runtime_mutators_check.cpp`: prepares a dry run from the running game's
  loose roots and mounts, without a map and without mounting the live
  store, and remembers the verdict in `Cache/mutator-verdicts.tsv`. The
  mod key holds the simulation hash, the mod's id, version and packaging
  revision, the base game and its expansions, and each loose folder and
  archive the dry run reads, an archive with its size and time: a mod
  folder without a profile, or a game folder with other archives, is
  asked again.
- `runtime_mutator_dry_run.cpp`: `--check-mutator-dry-run`
  (`native-mutator-dry-run`): two made-up mutators each multiply commander
  turn rate and worker time. The verdict stays compatible and each field
  of ARMCOM and CORCOM is held at 65535, with exactly one note each. No
  match starts.
- `runtime_mutator_verdicts_check.cpp`: `--check-mutator-verdicts`
  (`native-mutator-verdicts`): four made-up packages against the installed
  game, then the same answers from the verdict file. A changed option
  runs again. A file of garbage is ignored and rewritten. No match starts.
- `runtime_recorded_mutators_check.cpp`: `--check-replay-loadout`
  (`native-replay-loadout`): a recorded loadout is refused when it cannot
  be read or its mutator is not the installed one, played for one skirmish,
  and the player's own list is put back, including after a start that does
  not happen.
- `runtime_side_data_check.cpp`: `--check-side-data-per-match`
  (`native-side-data-per-match`): each match reads `gamedata/sidedata.tdf`
  again. When the file is unchanged the match keeps the sides the game
  started with; when one side names the other's commander the match plays
  that commander, and the menus return to the started sides afterwards. A
  file with no sides, or a commander the game has no unit for, refuses the
  start and leaves those sides in place.
- `runtime_gadget_art_check.cpp`: `--check-gadget-art-per-match`
  (`native-gadget-art-per-match`): three skirmishes of one run. A download
  button is linked with no picture, then draws the picture after the mod
  folder gains its GAF, then has no picture once that file is gone. A GAF
  the previous match could not read is read again.
- `runtime_match_menus.cpp` also registers the load-game overlay
  (`register_load_game_screens`).
- The Open Annihilation settings ([oa/ui/engine_settings.hpp](../ui/engine-settings/README.md)):
  `engine_settings_state.hpp` and `runtime_engine_settings.cpp` read them at
  start, put them in effect, save them, and run the dialog. The dialog is a
  screen of the OA layer ([The OA layer](#the-oa-layer)),
  `Runtime::SettingsScreen` in `oa_layer.cpp`, on the main menu and in a
  match. `engine_settings_menu_host.hpp` and
  `runtime_engine_settings_menu.cpp` hold the main menu's OA button and
  Settings button, an overlay at the picture's bottom-right corner (its
  top-right corner while an extension's overlay stands over the main menu)
  under the extensions' overlays, drawn into the main menu's picture: the
  Settings button in the corner, showing the settings icon
  (`EngineSettingsMenuHost::settings_icon`), and the OA button a quarter of
  a button's side left of it, showing the mods icon
  (`EngineSettingsMenuHost::mods_icon`), which opens the Library
  ([The Library](#the-library)) and wears a dot while updates wait there.
  The dialog is centred over the darkened menu, or where the player moved
  it by its title, and takes every input while it shows. The Settings button, Cmd+, on macOS or Ctrl+, elsewhere, the
  macOS application menu's Settings… item and the Library's SETTINGS… open
  it, the Settings button with no Library under it; nothing opens
  over a message box or a frame a package owns. Enter is OK and Escape is Cancel;
  the key that closed the dialog does nothing more until it is released, so
  that a held key never reaches the main menu, where Escape itself does
  nothing. Another screen replacing the main menu closes the dialog as
  Cancel does. `runtime_engine_settings_match.cpp` opens it over the
  match, centred in the window or right of the in-game menu's column
  (`match_dialog`). The column's OA button, which shows the settings
  icon, with the shortcut and Ctrl+F2 for the mod options, the layer draws
  and drives; the column going closes the dialog as OK does. `runtime_engine_settings_app_menu.cpp` is the
  application menu's item.
  `acceleration_status.hpp` and `acceleration_status.cpp`
  (`app-acceleration-status`) say what the dialog shows of the renderer:
  Hardware acceleration's status, first reason first, and whether nothing
  could help the run, which locks the row "Not available here". The
  machine's memory comes first, whatever the setting or the flags: it needs
  the render policy's 2 GiB threshold, `smallest_accelerated_memory`,
  1.75 GiB as the system reports it, so that a machine sold with 2 GB
  counts. Then come the setting and the flags, `SDL_RENDER_DRIVER` or a
  video driver with no window, a shared game or a replay, and whether the
  renderer is able. With the game's renderer the status follows the facts
  the tier is decided from (`tier_acceleration_facts`): probe items 1 to 3
  and the start-up function test decide whether the renderer is able, a
  failure in the run says the graphics driver failed and leaves the row
  within reach, whatever renderer it left, and in use the second line says
  what the graphics card does at its rung (`acceleration_reach`). A runtime
  without it does not look at the renderer: only SDL's software renderer
  is known unable. Vertical sync is locked
  on SDL's software renderer; on SDL's `direct3d` renderer
  (`render_probe::vertical_sync_resets_device`), where each change resets
  the graphics device and the game does not recover one the reset leaves
  lost; and once the renderer refused it.
  `Runtime::acceleration_facts` gathers those facts, the settings screen
  refreshes the status each frame while the dialog is open, and
  `Runtime::apply_vertical_sync` asks the renderer to wait for the display
  only when the setting in effect changes it, never while it stays Off.
  `--check-engine-settings` (`native-engine-settings`) drives them through
  the SDL presenter over a preferences file it empties first, the main
  menu's part and the dialog's in a 640x480 window, where the dialog is
  Compact at 1×:
  `runtime_engine_settings_check.cpp` holds the main menu's part, with each
  look of the OA button and the Settings button (which opens Settings with
  no Library under it) and the darkened menu under the dialog compared pixel
  for pixel with what they should draw, the 640x480 window compared with
  that picture, the OA layer passing on what its screens do not take
  (screens of the check's own that are not modal, a text field's text and
  text input, `layer_key`, `close_above`), Escape doing nothing on the menu
  itself and EXIT ending the run; `oa_layer_check.cpp` the layer's own
  check, run after the window sizes: in a 640x480 and a 2560x1440 window
  a screen that is not opaque, alone, over an opaque screen and under one,
  and under the cursor, changes no pixel of the window or of the frame
  without the cursor but those it draws (a half blend within 2 of the
  colour blended straight over what lies under it), darkens nothing, keeps
  the cursor above it and lists its control to automation, while an opaque
  one draws as before; F1 reaches a screen that takes the information key,
  goes on past one that does not, and changes nothing in Settings; a
  notice over the main menu shows with its corners rounded, as
  `round_drawn_screen` rounds the notice drawn straight; S on
  Accessibility's practice pad shows there and does not reach the main
  menu; Settings moves 60 by 40 points by a drag on its title and keeps
  that place in the preferences, a double-click on the title centres it,
  Alt and the arrows move it, and `reset_window_places` puts it back; a
  window's and a dialog's corners are clear outside the quarter circle and
  edged along it at 1×, 1.5× and 2×; and in a
  match F1 goes on to the game and such a screen changes only the pixels it
  draws in the match's picture;
  `runtime_engine_settings_dialog_check.cpp` the dialog driven by the
  pointer, the wheel and the keys (every section, scrolling, each setting in
  effect at once, Vertical sync read back from the renderer, Font shadow
  read back from the text style, OK, Cancel, Restore defaults and the keys
  they save) and the main menu with the
  buttons as 640x480, 800x600, 1280x720, 1920x1080 and 2560x1080 windows
  show it, each button opening its screen, and in each the dialog at the
  size class and scale the window
  gives it (Compact 1×, Regular 1×, Large 1×, Large 1×, Large 1×), its
  corners rounded as the layer shows them,
  clicked where the window shows it and compared, every window pixel but
  the cursor's square, with the menu's frame darkened and the dialog drawn
  at its class and scale (`oa_layer_check.hpp`): each section, Developer
  Mode's list open, off and on, and Graphics at its top and at its class's
  end;
  `runtime_engine_settings_match_check.cpp` the in-game menu's button and
  dialog at 640x480, 1280x720, 1920x1080 and 2560x1080, and 1920x1080
  with HUD scaling Off, each at the class and scale `match_dialog` gives
  (Compact 1× at 640x480, Large 1× on the others), its class's points at
  the scale clear of the column, centred right of it at 640x480 and
  1280x720 and centred in the window on the wider ones, drawn there pixel
  for pixel as the dialog drawn offscreen at them, its corners rounded
  (`round_layer_corners`), a click on
  a section's entry showing it, with the locks of a game played alone and of a
  shared game, Hardware acceleration set to Full and to Basic in a shared
  game, where each waits for the game's end, and the wheel scrolling the
  dialog, not the battlefield, where its section is taller than its view; and `runtime_engine_settings_wiring_check.cpp` each setting
  taking effect in a match, Escape's order, Vertical sync, the lock
  either acceleration flag puts on Hardware acceleration and the row set
  to Basic, Full and Off among them. The check runs with `--force-capable`,
  which lifts the software renderer's lock on both rows; every other step
  leaves Hardware acceleration Off, so every frame it compares is drawn on
  the processor. Both dialog steps also scroll a
  section of nine rows, `engine_settings_tall_section.hpp`, shown in place
  of the open section's rows, Page Up and Page Down by the dialog's own
  page step (`settings::dialog_page_step`: the view at the dialog's class
  less three hint lines, 184 rows at Compact); the wheel events are the
  check host's (`check_host_input.hpp`). At Compact Developer scrolls as
  one, its rows, its list, Show Active Only and Restore profile values in
  the section's own view, so the dialog step pages down to Show Active
  Only and Restore profile values, then scrolls back up before it turns
  Developer Mode on. With `--snapshot`, the check writes each of
  those frames beside the named file. While an extension's overlay stands
  over the main menu, the check first clicks the OA button in the menu's
  top-right corner through that overlay and closes the dialog it opens;
  then it sets the extensions' overlays aside
  (`Runtime::set_extension_overlays_aside`) until it ends, so that every
  frame it compares is the engine's own drawing over TA's own layout.
  `--check-kit-gallery` (`native-kit-gallery`, `runtime_kit_gallery_check.cpp`)
  needs `--preferences-file`, empties it first and sets the extensions'
  overlays aside as `--check-engine-settings` does, its ctest's
  `kit-gallery-clean` removing a previous run's file before the game reads
  it. On 640x480, 1280x720 and 1920x1080 windows it loads the main menu,
  opens Settings with its shortcut, clicks Developer's entry and then SHOW
  where the window shows them, and requires the UI kit gallery on top at
  Compact 1×, Large 1× and Large 1×, covering the window, its controls
  listed to the automation endpoint in dialog `oa.gallery`
  (`oa.gallery.close`, `oa.gallery.toggle.1.on`, `oa.gallery.drop-down.1`);
  the window,
  every pixel but the cursor's square, as the gallery drawn offscreen at
  that class and scale (`layer_drawing`) over the main menu darkened by
  Settings' backdrop and the gallery's (`expected_layer_frame`); Tab taking
  the focus to every control the gallery declares, once each, in its order;
  and Escape bringing Settings › Developer back. With `--snapshot` it writes
  `<stem>-kit-gallery-<W>x<H>.ppm`, the window as it shows the gallery, and
  prints `kit gallery check: <Compact|Regular|Large>, <n>x, on the <W>x<H>
  window` for each and `kit gallery check: passed`.
  `native-engine-settings-determinism`
  (`tools/check_native_engine_settings.py`) checks that every setting at its
  default plays the game as it plays without any, and that the settings that
  change only the look or the input, enhanced anti-aliasing among them,
  leave the world alone.
- `runtime_notices.cpp`: the notices for the entries the game data cannot
  support, such as skirmish, multiplayer and the missions after the last in
  the Total Annihilation demo (1997): DEMOMSG.GUI when the data can draw it,
  else a message box. `web_link.hpp` and `web_link.cpp` hold the seam the
  notice's website button opens its address through: the player's browser,
  or in a run nobody watches only a record of the request; the runtime keeps
  the hooks it chose and that record in `web_link_state.hpp`.
  `runtime_notice_check.cpp` holds the navigation and load-save checks over
  such data.
- `map_picture_state.hpp`: the map selection's picture, the selected map's
  minimap and where it is fitted in MAPPIC, which `runtime_skirmish_host.cpp`
  loads and fits and the frontend frame draws.
- A screen package registers through `screens.inc` rather than adding its
  cases to `runtime.cpp`.
- `check_host.hpp`, `runtime_check_host.cpp`, `check_host_input.*`: the
  check host, through which a check an extension runs drives the running
  game (see [Extensions](#extensions)); `app-check-host` tests its table and
  the parts that need no running game.
- `renderer_records.hpp`, `renderer_records.cpp` (`oa-app-renderer-records`)
  and `renderer_state.hpp`, `renderer_state.cpp` (`oa-app-renderer-state`):
  the renderer records, which the renderer host keeps (below). What the engine has
  seen of each render driver on this machine is kept in `renderer-state.conf`
  beside the preferences file: a strike against a driver for a stage the game
  died in, or a failure seen while running, which becomes a record only when
  the same is seen at the next start or in the next run (`failed-driver`,
  which the walk of SDL's drivers skips, never for `software`;
  `accelerated-unusable`, which keeps the driver on the standard tier;
  `full-unusable`, from a left-over trial of Full's path, `path full`, or
  a repeated `card` strike, which keeps the driver on the Basic tier where
  Full is asked for, unless `--hardware-acceleration=full` was given), or at
  the first left-over trial on Windows before Vista and on Linux
  (`crash_evidence`); the adapter they were written under, the remembered
  step-down rung, the `native-density` key, the trial of a stage under way and
  the told mark of the main menu's notice. On a machine under 2 GiB
  (`RecordRules`) no trial is written and nothing of the accelerated tier is
  struck or recorded, and what a run with more memory left of it stays for a
  start from 2 GiB to judge. Strikes, records and remembered rungs are
  written under the engine's build, its version and the commit it was built
  from (`OA_ENGINE_BUILD`, written by `cmake/OaEngineBuild.cmake` on every
  build), and those of another adapter or build are dropped, so that a new
  build starts free of an older one's failures; the `native-density` key
  stays with the engine's version. `clear_failures` gives every driver a fresh try, as
  Off and back and Restore defaults do. The sentinel of the stage a start has
  reached is kept apart in `renderer-sentinel.conf`; `sentinel_step` moves it
  and the trial through a run, from `create` to `running` and each path's
  first frames, with none under `SDL_RENDER_DRIVER`. Their text and rules are
  pure (`renderer_records.hpp`, tested by table in `app-renderer-records`);
  `RendererState` reads and writes the files: every write best effort, logged
  once on failure with the records kept in memory, except the trial's, whose
  failure the caller is told of; the records written only when one changes,
  flushed with the folder synced, and not while a match runs; while Off then
  On's clearing waits for OK, what it cleared kept in the file with what was
  struck since, and put back with it by Cancel (`confirm_clear`,
  `restore_failures`); a trial written with the strike the last run left; the sentinel rewritten in place
  unflushed; a clean exit erasing the run's trial, writing the records left
  and deleting the sentinel; a missing or garbled file read as empty. With a
  named `--preferences-file` they live in memory, and under
  `SDL_RENDER_DRIVER` nothing is read or written. `app-renderer-state` tests
  the files in scratch folders, read-only and garbled ones among them. The
  main menu's notice of a new record (`next_notice`, `notice_action`) is
  told once (`Runtime::tell_renderer_records`, `runtime_notices.cpp`): once
  the main menu, its own, has shown for a frame and stays, with no
  multiplayer signal waiting to leave it, so that a start with `-n`, whose
  signal waits for the frontend's next pass, or with `--play-demo`, which
  passes the main menu, waits for it to show again; a run nobody watches
  notes the request and leaves the record untold, and
  `--check-renderer-ladder` notes it and marks it told.
- `mutators_memory.hpp`, `mutators_memory.cpp` (`oa-app-mutators-memory`):
  the last mutator set of each setup, and the crash record kept beside the
  preferences file while a match with mutators runs. A set is the canonical
  loadout text and nothing else. `preference_key` names the three keys
  `open-annihilation.mutators.skirmish`, `.multiplayer` and `.campaign`.
  `Sentinel` reads `mutators-sentinel.conf`, replaces it and
  deletes it; a file that cannot be read is a leftover with no mutators,
  logged once, and is never fatal. `runtime_mutators_memory.cpp` is the
  game's side of it: `remembered_mutators` (the picker), `remember_mutators`
  and `forget_mutators` (Forget it), `start_with_mutators` (a setup's
  START), `take_mutator_crash` (the crash question) and
  `mutators_refused_for_run` (`--no-mutators`). The record is opened when
  the preferences are loaded, including under `--preferences-file`, and a
  leftover file is deleted once it has been read.
- `video_capture.hpp`, `video_capture.cpp`: `--capture-video`, the
  developer's capture of the window's frames and the game's sound as an MP4
  video through the `ffmpeg` program; `runtime_showcase.cpp`: the scripted
  runs `--showcase` plays, among them `skirmish-battle`, which plays a
  skirmish's fight for a minute on the game's own loop and reports the ticks
  and frames a second it kept. [docs/capture.md](../../docs/capture.md)
  describes both. Benchmarks, `--frame-rate` runs, headless saved-game runs
  and the battle end with the memory report, its peaks the largest the
  system saw (`oa/platform/memory_status.hpp`).
- `screen_size.hpp`, `screen_size.cpp`: the settings read before the window
  opens (`start_settings`), with the defaults of a light machine
  (`oa/platform/machine.hpp`): the Screen size, the window opened at that
  size and full screen given the display's mode of that size where full
  screen switches modes (`take_screen_size`, `run_full_screen_method`),
  else drawn at the size and scaled, and a stored size the display does
  not offer shown as Desktop for the run (`shown_screen_size`), checked
  against the primary display, which the window opens on; the sizes a
  display offers (`offered_screen_sizes`,
  [display modes](../platform/display-modes/README.md)), which the options'
  Screen Size, the settings' Screen size after Desktop and the battle room's
  RES column list (`multiplayer_bind_display_modes`), from what SDL reports
  of the display the window is on: in full screen, the display's modes,
  including where the desktop scales the frame and cannot switch the
  output's mode; in a window, the sizes that fit the desktop; up to
  the longest side the setting keeps, or the made-up monitor
  `--display-modes` names for a check on SDL's dummy video driver; the
  table of SDL's calls that apply a size (`sdl_screen_hooks`); and Hardware
  acceleration, which either flag decides over
  (`hardware_acceleration_asked`). With no size named the window opens at
  the default, held to the desktop in Steam's Game Mode, where gamescope's
  pointer reaches no further (`default_window_size`); the log says the size
  it opened at, the desktop's and whether the run is in Game Mode
  (`report_window_size`).
- `screen_mode.hpp`, `screen_mode.cpp`, `runtime_screen_size.cpp`: the
  Screen size applied at once, when the options or the settings close with
  OK, and when the battle room's RES column steps to one, on the menus and
  in a match, which carries on as it was
  (`Runtime::apply_screen_size`, `EngineSettingsState::take_screen_size`).
  How full screen shows a size depends on the window system
  (`full_screen_method`): Windows, Windows XP's build among them, and X11
  outside a Wayland session and Steam's Game Mode switch the display to the
  mode of the size (`switch_mode`), which Windows puts back when the game
  leaves full screen, is switched away from or ends; macOS, Wayland, X11
  within a Wayland session, Steam's Game Mode and every other driver keep
  the desktop's mode (`scale_frame`). `apply_screen_size` takes its steps
  through a table of hooks (`ScreenHooks`): in a window, a maximised window
  brought back to a size of its own, the size asked for, the window kept on
  its display at that size (`keep_window_on_display`), and the mode full
  screen will take; in full screen, the display's mode of the size where
  the method switches modes and the display has one, else the desktop's,
  the window taking the size once it leaves full screen
  (`FullScreenSwitch::window_width`); Desktop gives full screen the
  desktop's mode back and leaves a window as it is. The size applied
  (`FullScreenSwitch::screen_width`) lasts the run, soft restarts included.
  The next start opens in the full screen or window the player left
  (`open-annihilation.window-place`), and full screen uses this Screen size.
  In full screen on the desktop's mode at a size of its own
  (`scaled_frame`, `Runtime::scaled_frame_size`), a match is laid out and
  drawn at that size, as in a window of it, and presented letterboxed, or
  in whole steps as Menu scaling says (`set_frame_presentation`): the
  interface scale, the side column and the HUD follow the layout; the
  standard tier draws its layers with the frame's filter
  (`standard_frame_scale_mode`) and the accelerated tiers at the density
  the frame is presented at (`match_display_density`); a pointer in the
  black bars rests on the frame's edge, and the edge-scroll band is as
  deep as one window point. The menus keep their 640x480 frame.
  `app-screen-mode` follows the steps on made-up windows over made-up
  monitors (a 4K monitor on Windows, a Retina Mac's display, a monitor of
  Windows XP's time and a Wayland desktop): each window system's method,
  the scaled frame, a window, a maximised window, full screen at a mode or
  on the desktop's, a mode missing or refused, and Desktop.
  `--check-frontend-controls` drags the window to a size the display does
  not offer, which the options' Screen Size shows as Custom, and OK on
  1280x720 snaps it to it; in a match the Options' Visuals Screen Size
  chooses a listed size and OK stores it and applies it to the window and
  the match, then the check applies 1280x720, switches to full screen,
  where the dummy driver keeps the desktop's mode and the match is
  drawn at 1280x720 and letterboxed, the pointer in the bars resting on the
  frame's edge, applies 800x600 there, which the window takes as it leaves,
  and Desktop. Alt+Enter does not change the size the game draws at.
  `--check-engine-settings` chooses 1280x720 in the settings,
  which the window takes on OK and not before, in the menus and in a game,
  whose tick and world stay as they were.
- `render_host.hpp`, `render_host.cpp` (`oa-app-render-host`, with
  `graphics_report.cpp`): the game's renderer. `walk_render_drivers` acts
  on the render policy's walk through hooks (`CreationHooks`): it sets the
  framebuffer hint, tries each driver, logs each refusal and, when the
  records would leave nothing able to present, that the walk starts again
  from the top. `RendererHost` gives it SDL's calls, keeps the renderer,
  what the probe found of it and the walk's attempts, and puts back the
  floating-point settings the game started with once the renderer is made
  and described. `HostDisplay` (`main.cpp`) owns one, and the runtime
  borrows it with the renderer: `render_run.hpp`'s `Runtime::RenderRun` is
  the runtime's own renderer state, made only when it is handed a window,
  a renderer and its host, so a headless run, a window the runtime made
  itself and a loopback check's second runtime have none. The host keeps
  the run's renderer records: `HostDisplay` has it read them before the
  walk (`RendererHost::open_records`, `RecordsPlace`), beside the player's
  own preferences file or in memory with a named one, applying what the
  last run left behind; the walk skips the drivers they hold failed and
  walks again with them ignored, for the run, where that leaves nothing
  able to present; the sentinel stands at `create <driver>` before each
  attempt and `standard <driver>` while the probe reads the renderer, and,
  where the walk passed over no driver by record, the adapter the probe
  describes is noted, since each driver names it in words of its own. The
  runtime moves it on: the first accelerated frame, the start-up stage
  passing after 60 presented frames and 2 s on the host's `StageClock`
  (`note_presented_frame`), and each accelerated path's first use
  (`begin_path`, `begin_accelerated_path` at the first frame drawn through
  a scene, overlay or prescale target, which is made then), a path whose
  trial cannot be written dropping the tier, and switching the tier off
  closing a path's stage under way. Strikes and records a match
  makes are written when it ends, and `HostDisplay`'s destructor ends the
  records cleanly at every exit through `main` (`finish_records`).
  `walk_rebuild_drivers` and `RendererHost::rebuild` make the renderer
  again after a failure, striking it against the driver that failed;
  `RenderFaultHooks` are what `--check-renderer-ladder` forces, among them
  the machine's memory, how a left-over trial counts and the name the
  records keep the renderer under.
  `RendererHost::decide_start_tier` decides the first frame's tier and
  logs the start-up line, running the start-up function test
  (`run_function_test`) where the tier could be accelerated; the facts the
  tier is decided from stay with the host (`tier_inputs`), and a rebuild
  drops the accelerated tier for the run. `app-render-host` runs the
  function test on SDL's software renderer, and sees it fail where its
  faults draw a reduction NEAREST (`FunctionTestFaults`).
  `runtime_renderer.cpp` holds the runtime's side: the render events,
  present errors and rebuilds, a lost device's wait and the stall rule;
  `runtime_tier_watch.cpp` the accelerated tier's memory guard;
  `runtime_renderer_ladder_check.cpp` the ladder check.
- `scaled_world.hpp`, `scaled_world.cpp`: `TiledTexture`, a streaming
  texture made as one texture within the renderer's limit and as tiles
  with one-texel gutters beyond it, for the standard tier's window-size
  layers and the accelerated tier's scene and overlay; `PresentError` and
  `AccelerationError`; and the accelerated tier's drawing on the card
  (`PrescaleTarget`, `draw_scaled_world`, `sharp_draw`, `probe_pixelart`);
  and a screen of one frame, letterboxed or in whole steps as Menu scaling
  asks (`set_frame_presentation`), drawn by the standard tier with the
  pixel-art mode where it works (`draw_frame`). `frame_coordinates.hpp`
  maps the pointer onto such a frame where it is drawn, on whole pixels
  where SDL reckons half of one. `app-scaled-world-software` checks on
  SDL's software renderer that tiles read back as one texture does, the
  card's drawing against that renderer's own filters, and the frame's
  rectangle and the pointer's mapping under each way of Menu scaling.
- `render_policy.hpp`, `render_policy.cpp` (`oa-app-render-policy`): the
  decisions of hardware-accelerated presentation as pure functions, with
  no SDL, no files and no clock, of which the game uses so far the walk of
  the render drivers and its rebuilds (`render_host.hpp`), the texture
  limit, in the line it logs at start and for the tiles, the capability,
  the tier each frame is drawn in and the step that acts on it
  (`step_tier`, `tier_action`, `forget_failures`),
  the shared-game gate, the starting rung, the stall rule, the count of
  device resets (`note_device_reset`), the layers' texture formats
  (`layer_formats`), the rung below a buffer the memory guard refuses, and
  the tiles of a texture beyond the renderer's
  limit. The walk
  of SDL's render drivers in SDL's own order, skipping drivers recorded as
  failed (`failed_driver_list` of the renderer records) but never
  `software`, with the framebuffer hint set before
  `software` and never empty, a second walk with the records ignored
  whenever they would leave nothing able to present, and SDL's own call
  under `SDL_RENDER_DRIVER` (`start_creation`, `start_rebuild`,
  `next_attempt`); whether probe items 1 to 3 found the renderer capable
  (`assess_renderer`, `texture_limit`); the tier each frame is drawn in,
  standard (today's renderer) or accelerated, with the reason
  (`decide_render_tier`): the standard tier whatever the flags on a
  machine with under 2 GiB of physical memory, or whose memory the system
  does not report, where a machine that reports 1.75 GiB
  (`smallest_accelerated_memory`) counts as having 2 GiB; when the
  start-up function test may run, never under 2 GiB; and the gate that
  keeps a shared game or a replay from starting anything until it ends;
  present stalls;
  the rung a machine starts at on the ladder from 2 GiB, its budget sized
  from its processors and kind and never from its memory (`start_budget`),
  magnify off before Vista and at budget none, and the blend only above
  4 GiB and never on a driver that excludes it (`start_rung`); the
  remembered rung (`resume_rung`), each step described for the log
  (`describe_step`); the rung the tier stays on
  where the memory guard refuses a buffer (`rung_without`); the chrome's
  filter (`chrome_filter`), the magnified scene's (`world_filter`), how a
  screen of one frame fills the window and the filter it is drawn with
  under Menu scaling (`frame_fit`, `frame_filter`) and the
  prescale budget; and the tiles of a texture beyond the renderer's limit
  (`plan_tiles`). The names of the drivers' graphics interfaces stay with
  the platform: the policy takes each driver's traits (`DriverTraits`).
  `app-render-policy` tests them all by table. What a left-over sentinel
  or trial, or a failure while running, counts for, and the sentinel and
  the trial through a run, are the renderer records' (above). The memory
  guard is built into the policy's library (above); the world's scaling
  (`world_scaling`), the native-density rule, which needs 2 GiB as the
  accelerated tier does, and the probe's report join the policy with the
  code that uses them.
- Director scripts ([docs/director.md](../../docs/director.md)):
  `runtime_director.cpp` runs `--generate-script` (the recording replayed
  undrawn through the extension that replays it, its timeline recorded and
  the shots planned) and `--render-script` (the recording replayed tick by
  tick, the shots drawn, the sound mixed offline, the chunks written, the
  stills `--stills` asks for written), a script that names a stage played
  out over the headless skirmish in place of a recording, and
  `--check-director-render`, which renders a small script over the headless
  skirmish. `runtime_director_view.cpp` and `director_state.hpp` are director
  mode (`director_presentation.hpp`): the frame drawn from the director's
  camera at the output size, the battlefield alone, the match's sounds and
  every player's unit announcements sent to the director's sound hooks;
  as everywhere, nothing drawn changes the match, and
  `--check-director-view` checks that a drawn and an undrawn replay reach
  one world. `runtime_director_full.cpp` draws director mode's frames on
  the graphics card when a render asks for `--hardware-acceleration=full`:
  the Full tier's presenter on SDL's GPU renderer with no window,
  presenting into a texture (`FullPresentation::final_target`) that is read
  back into the frame. `director_output.hpp` and
  `director_output.cpp` (`oa-app-director-output`) write a render's files
  (each chunk's frame manifest and sound, the run manifest, the stills as
  PNG pictures), run `ffmpeg` on
  the chunks and join them, and read and write the `.oamovie` bundle;
  `app-director-output` tests them without an encoder.
- Frames between ticks: `presentation_interpolation.hpp` and `.cpp` keep
  each unit's pose (place, heading and the pieces its script moved and
  turned), the projectile pool and the debris table at the last two ticks
  the presentation saw, and blend them; a batch of ticks one frame ran
  (above normal speed) blends from the tick before the batch.
  `match_models.hpp` holds the match renderer's state with them
  (`MatchModels`). `render_match_surface` draws at `presentation_alpha()`,
  which the application loop's pacing chooses for each frame: at 1 the
  tick as it is, below 1 each moved unit from copies of its record and
  model instance placed part of the way from the tick before, with a draw
  state of their own, while the match's own pieces are rebuilt as a whole
  tick's draw rebuilds them; projectiles, debris, fragments, particles,
  health bars, order lines and a tracking camera follow. With Smooth
  motion Off the loop draws every frame at 1, so each shows its tick as
  it is, a mirrored unit where its playout has it at the tick. The
  director draws its frames between ticks so, whatever the setting. The
  debug grid draws its random numbers from its own generator, never from
  the match's streams, and draws the numbers a tick's first draw took
  again on the tick's later draws (`DebugGridRandom`), so that showing it
  never changes the game and a tick drawn more than once shows one grid.
  `app-presentation-interpolation` tests the blends and the grid's
  numbers, and `--check-interpolation` (`runtime_interpolation_check.cpp`)
  the frames, the application loop's among them with Smooth motion On and
  Off.
- Units of other machines' players: `advance_match_clock` has the unit
  playout (`src/present/unit-playout`, `Runtime::unit_playout_`) read the
  match after each step, whether the engine or an extension ran it, and the
  director after each tick of a recording it plays; `MatchPresentation::playout`
  gives the draw paths its places. With each observation it reads what each
  unit's movement holds (`unit_motion` in `runtime.cpp`: the movement
  record's speed and velocity, the route head its owner shared with the
  mirrored navigator, the air driver's point and seek goal). A unit of a
  player in use with `OA_PLAYER_STATUS_MIRRORED` is drawn from copies of its
  record on every frame, whole ticks and a paused game included, placed,
  turned and tilted where its owner's playout clock has it at the frame's
  moment (`mirrored_pose`, `playout_moment`): near its newest record, moved on
  ahead of it between records, with what new records change faded in; its
  pieces are placed between their two ticks' poses on every frame, even when
  its records moved it by a jump (`UnitMotion::pieces_moved`,
  `blend_unit_pieces`). Its shadow, selection box, health bar and digits, the
  order lines that start at it, the culling and the far-to-near order of the
  frame, a camera tracking it and the pointer's pick (against the frame last
  drawn, `MatchPresentation::drawn_moment`) all take that place; a unit it
  carries is moved as far as it is drawn from its simulated place. Whether a
  unit is seen, the on-screen list, the radar, projectiles, nanolathe
  streams, explosions and wrecks, and every simulation read keep the
  simulated place. With no such player the frame is drawn as before.
  `--check-unit-playout` (`runtime_unit_playout_check.cpp`) takes the
  skirmish's other player as another machine's, whose records, with the
  runner's speed and route head, arrive within the steps in 3.1c's bursts,
  and checks at 120 frames a second that its runner moves on every frame by
  about its pace, within two ticks of its simulated place on average, on a
  whole tick's frame too, with the tracking camera and the pick where it is
  drawn; that the local runner and the world are as without the playout;
  that with Smooth motion Off every frame draws the runner where its
  playout has it at the latest tick, the world as with it On; and that,
  with no such player, every frame is.

## Brain seats

A computer seat of a skirmish or a campaign mission can be played by an
installed brain. `set_brain_seat` names the brain, the level and the
options for one player. Just before a new game finishes loading, the
runtime reads that brain from `Brains` in the player's folder, checks that
it fits, and seats it. A seat with no brain, or a brain that does not fit
or cannot be read, plays as the built-in computer player. A network game
is not saved. In the battle room, drawn from the room's own art
([Computer seats in the rooms](#computer-seats-in-the-rooms)), a computer
row keeps 3.1c's "AI:<owner>" name. Its empty Resolution cell
(`oa.brain-seat.<slot>`) shows the OA stamp and the seat's brain with its
version and level, or "Built-in", and its Mem cell
(`oa.brain-seat-level.<slot>`) shows the brain's level over the room's
greyed word. On a computer this machine runs the name has a notch
(`oa.brain-seat-menu.<slot>`); the notch, the Resolution cell and the
right button on the name open the brain menu, without Get more brains...,
since leaving would leave the game; a row writes that computer, or every
computer this machine runs with Use for every computer seat ticked. The
Mem cell steps the brain's level, or the built-in player's difficulty as
3.1c steps a computer's (Easy, Medium, Hard). Another machine's brain is
shown in the same place and read alone, and its card names a brain that
sees every unit. Tab, the arrows and the controller reach the Resolution
cells, and resting the pointer on one, or the information key, shows the
seat's card. The battle
room remembers the seats, and START seats them before the first tick. With
no brain installed and none chosen, the row is unchanged. The screen is
`runtime_brain_room.cpp` (`brain-room`), kept while the battle room is in
front. A recording does not seat a brain. A saved game names each
computer seat's brain and seats that brain before the first tick. The
package is read by `oa-app-brain-seats` (`brain_seats.hpp`); the seat
itself lives in the simulation.

A save made while brains play stores a Brains account beside the mutators'
loadout. The account holds the seats, each brain's version, package, level,
options and the name players see, and each brain's own saved state, at most
1 MiB. A brain without `save()` starts again from the units. Loading seats
each saved brain that is installed, and hands `start` the saved state when
the installed brain is the same version and package. A different version or
package leaves the state out, and the log names the version that saved it.
A save that names a brain which is not installed is refused with a sentence
naming it. From the game's Load screen the question "Load with the built-in
player?" is pushed over that screen. LOAD WITH THE BUILT-IN PLAYER loads
with the built-in player on the missing seats, and BACK loads nothing. A
save with no brain seated holds no Brains account. The end screen's save
between missions is made after the match was released, so it holds no
account and the next mission starts from the campaign seat.
`--check-saved-brains` (`runtime_brain_saves_check.cpp`, ctest
`native-saved-brains`) saves a skirmish Steady Commander plays, loads that
state, and walks the missing-brain question.

A recording names each computer player that ran a brain: the player, the
brain, its version and its level, and, for a brain this machine ran, the
package. A brain another machine ran is the one that machine's presence
record named when the match started. Choosing the brain sends that record
then, so a start that does not pass the battle room again still leaves
every machine the name. It stores that as its own sector, after the
mutators' sector, and leaves the sector out when no brain played, so a
recording without brains
is unchanged. Text that does not fit one sector is left out and the
recording is still written. Playback reads the sector, logs each seat's
brain, and seats none: every player stays mirrored. The note beside a save,
a recording or a film lists each of those seats after the mutators, in
player order: the player and the brain's id, then the version, the level's
name and the brain's name. A note without those lines reads as it does
today. An older build ignores the lines it does not know.

When a seated brain's turn fails, its sandbox closes and its hooks come
off before that turn returns. From the next tick the built-in computer
player plays the seat from the units as they stand. The machine that
seated the brain posts one notice in the match message log, in the
player's language: the brain stopped, it could not start, or it does not
fit this game. The same machine writes `logs/brains/seat-<n>.log` under
the data directory (`--data-dir`'s folder when it is given), where n is the player index plus one. The log starts
with the brain's name, version, id, level, seat and match seed, then each
printed line and the error, and it stops at 1 MiB with the line `the log
is full`. A log that cannot be written is reported once on standard error,
and the notice is still shown. A brain that cannot be read stays a line on
standard error only.

A skirmish and a campaign also remember which brain sits on each computer
seat. Opening the skirmish setup, other than from the map picker or from a
start that returned to it, reads `open-annihilation.brains.skirmish`.
Opening a new campaign or Any Mission from Single Player or the main menu
reads `open-annihilation.brains.campaign`. START seats those brains before
the first tick. A computer seat with nothing remembered takes the default
brain, at the level that brain offers for the setup's difficulty. A seat
left on the built-in player stays on the built-in player. A brain that is
not installed, or that does not fit the game being played, leaves the
built-in player in the seat, and the log says why. Each brain that plays
is one line: `brains: player N plays <id> <version> at <level>`. With no
brain seated and no default, START changes nothing about brains and writes
no seats key. Restore defaults does not erase a key that is there.

`tools/check_native_brain_seats.py` (`native-brain-seats`) plays a headless
skirmish from a scratch folder that holds Steady Commander and Ridge
Raider. It checks a start with no brain, Steady Commander remembered at
Firm, the default brain, Ridge Raider with no mod played, and a brain that
is not installed.

## Chat a computer seat reads and sends

A computer seat on this machine can read chat and say a line of its own.
`oa-app-seat-chat` (`src/app/seat_chat.cpp`) keeps one listener, a pace and
a queue. The runtime (`runtime_seat_chat.cpp`) feeds the listener from three
places and flushes the queue once a frame.

The three sources are: a line the local human types
(`Runtime::submit_chat_line`, handed on as `hear_local` with that line's
mode and targets); a chat record from another machine whose addressee is a
player of this machine (`NetMatchHooks::seat_chat`, handed on through
`hear_remote_seat_chat`); and a line another computer seat on this machine
says, handed to the listener when the queue is flushed, for each other
computer seat here that hears it.

The listener is one function. It is called with the seat, the sender, who
the line was said to (everyone, allies, enemies or the seat alone) and the
words without the "<Name>" head. With no listener the lines are still
posted and sent, and nobody is called.

A seat may say at most one line per 30 ticks of `Game.tick`, counted per
seat, so a replay of the same start repeats the same lines. The first line
is always allowed. `Runtime::advance_match_clock` flushes the queue after
the frame's steps and before the oldest message expires, so the message log
is written and a network record is sent outside `Match::tick`. A line shows
in the local message log when the local player hears it, and goes out to
other machines from the seat's own player id.

These are not handed to a seat: a command the local human types (a line
whose first character is '+'), `NetworkPlay::net_say`, the automatic lines
of the loopback check, a private-channel record, a command ('+' or '.'
first, after spaces), an empty line, the seat's own line, a line the seat
does not hear, and a further line inside 30 ticks.

## Touch controls

The touch controls ([docs/touch-controls.md](../../docs/touch-controls.md))
are a way to play added beside the mouse and the keyboard. They switch on
when the build sets the capability `OA_TOUCH_FIRST` (a touch-first platform's build does),
when `--touch-controls` is passed, when a direct-touch finger arrives or
when `--check-touch-controls` forces them, and then stay on for the run
(`touch_controls_active`). Until then none of their code draws, lays out or
reads anything, so a desktop with a mouse plays as before. Every order a
finger gives goes through the Runtime functions a mouse or a key reaches, so
saves, recordings and network games are unaffected.

- `runtime_touch.cpp` is the dispatcher. `dispatch_event` hands it every
  event after the Cmd alternates (`remap_command_key`) and the lifecycle
  events; `take_touch_event` takes fingers from direct-touch devices,
  claims each by what it lands on (an open sheet or the order wheel, a touch
  control, a dialog, a gadget of the 3.1c panel or of a placed region, the
  minimap, the battlefield, a frontend screen). Off a match, a finger that
  is not already on a 3.1c gadget snaps to the nearest one within about 22
  points, unless an OA screen that shows has a pressable control under the
  point or no farther than that gadget: the finger then keeps its own point.
  Mutators, MORE MAPS… and GET MAP take that finger by their reach only
  when no gadget of the screen under them is nearer, its anchor included; a
  point inside a gadget stays that gadget's. A mouse still presses the
  drawn rectangle. The dispatcher drops the mouse events
  SDL itself made from a finger. Battlefield fingers feed one gesture
  recogniser (`src/ui/touch-gestures`); every other claimed finger has its
  own for tap and hold. Taps, boxes and frontend presses become synthetic
  mouse events (`which` `SDL_TOUCH_MOUSEID`, `windowID` 0) sent back through
  `dispatch_event`, so they reach the engine as clicks; a battlefield tap is
  sent once the recogniser knows it is a tap, a gadget's press at landing
  and its release at lift. `tick_touch`, from `idle_tick`, runs the hold
  timers, inertia, auto-scroll and the ghost's anchor, writes the HUD state
  (selection text, the tap's action, the rail, the lit and paused looks) and
  lays out the controls' frame with `oa::ui::touch_hud::lay_out`.
- `runtime_touch_actions.cpp` holds what each control, sheet item and
  wheel item does. On the open megamap a battlefield finger's tap, wheel
  order and drag go to the megamap as the mouse's clicks (a right press
  where the right-click interface gives one), never to the battlefield
  under it. `runtime_touch_camera.cpp` holds the camera a finger moves:
  `pan_match_camera_by` (the map follows the finger, past the map's edges
  as far as a scroll goes) and `zoom_match_about` (a pinch's zoom applied
  at once about the fingers, so the map stays under them, or about the
  battlefield's centre while the camera follows a unit), inertia and
  auto-scroll.
- `runtime_input_modifiers.cpp`: every place the engine reads the modifier
  keys asks `input_modifiers(use)` for its own use, so a latch gives Shift
  only to its kind of action: ADD to selecting, QUEUE to orders, placement
  and the queued-order overlays, x5 to build buttons. Without the touch
  state it is exactly the modifier keys held. Every read of the keys,
  pointer buttons and modifier keys held goes through `device_state.hpp`,
  defined here, which adds what the automation endpoint holds down to
  what SDL's devices hold. `press_match_action` presses an action id with
  no modifier pulse, answered as a key's action would be (SELECT ▾'s items),
  `refresh_pointer_modifiers`
  brings the pointer's key word up to a changed latch, and `play_haptic`
  reaches the platform's haptics. `remap_keypad_enter` turns the keypad's
  Enter into Return as `dispatch_event` takes each key, so it opens and
  sends the chat line, answers dialogs and ends typed names wherever Return
  does, as in 3.1c; the event keeps the keypad's scancode.
- `runtime_key_bindings.cpp`: after composition, `feed_input_actions` gives
  the dispatcher each key, button and wheel with the contexts then.
  `advance_input_actions` runs once a frame. Losing the window's focus
  releases every held action and role, and a keymap change rebuilds the
  keyboard map. The action events are kept and nothing acts on them yet.
- `runtime_touch_hud.cpp` draws the controls into a layer of their own,
  composed over the CPU frame (`compose_touch_layer`) and presented in every
  tier (`present_touch_layer`), with the primitives of `oa-ui-paint`
  (`src/ui/paint`, tested by `ui-paint`).
- `runtime_phone_hud.cpp` lays out the match for the window
  (`make_window_match_layout`): on a phone the battlefield fills the canvas
  and the 3.1c HUD's minimap, resources, drawer cells, MORE sheet gadgets
  and panels are drawn into placed regions (`display_layout`'s placed
  mode), whose hit tests map back to the HUD's own gadgets; on a tablet the
  3.1c layout with the window's density and safe area; without the touch
  controls the 3.1c layout at the window's scale, no larger than the HUD
  scaling setting lets it be (`match_chrome_most_scale`: twice the original
  size, or the original size with HUD scaling Off). `overlay_area` is
  where the message log, the chat line, the kill board, the megamap, the
  whiteboard and the commander placement prompt go: the battlefield less
  the touch controls, or the battlefield itself without them.
- `runtime_touch_check.cpp` is `--check-touch-controls`
  ([testing.md](../../docs/development/testing.md#touch-controls)).

`touch_state.hpp` defines `Runtime::TouchState`, made on first use and
null on a desktop that never sees touch (`touch_`): the HUD state the
dispatcher writes and the drawing reads (`oa::ui::touch_hud::HudState`),
the window's viewport and the frame laid out for it, the safe-area insets
a check sets in place of the window's, and each part's own state:
`TouchDispatch` (`touch_dispatch.hpp`: claimed fingers, the recognisers,
the check's clock and forcing, the pulse a synthetic key holds, inertia),
`TouchLayer` (`touch_layer.hpp`: the layer, its texture and fonts) and
`PhoneHud` (`phone_hud.hpp`). `runtime.hpp` declares every touch member
once, and each part keeps its private helpers in a friend struct of its
own whose static functions take `Runtime&`: `TouchDispatchAccess`,
`TouchDrawAccess`, `PhoneHudAccess`, `OverlayAccess` (the overlays'
anchors), `TouchCheckAccess` (the check) and `LifecycleAccess`.

`platform_hooks.hpp` (`oa::app::PlatformHooks`, library
`oa-app-platform-hooks`) is what the platform the game runs on provides
beyond SDL, filled once by the platform's extension init: haptics, a
default game folder, the advice shown when there is none and the label of
its look-again button, and a call when the window opens; every member may be null, and the desktop leaves
them all null. `input_hints.hpp` (`set_input_hints`) sets the touch and pen
hints the touch controls read input with: no mouse made from fingers, no
fingers made from the mouse, the pen as a mouse that hovers. It runs before
video starts when the touch controls are on from the start and again when
the first finger switches them on, so a desktop that never sees a finger
keeps SDL's own hints; a hint an environment variable holds is reported and
left as the variable says.

`runtime_lifecycle.cpp` watches the app lifecycle events with an SDL event
watch, so they are acted on as they happen rather than when the loop next
polls: going to the background pauses a running game played on this machine
alone while When the window loses focus is Pause single player (the away
pause, below), or with Keep playing opens the in-game menu over it, which
holds it (the Pause key's bit is left alone), and saves the preferences;
the system's low-memory warning lets the cached model images go.
`take_lifecycle_event` keeps every lifecycle event from reaching a screen.

The away pause (`runtime_platform.cpp`): while the window is away, without
the focus, minimised or with the application in the background, a
campaign mission or a skirmish against computer players on screen pauses
when Settings › Controls › When the window loses focus is Pause single
player (`away_action`, the rule in `match_clock.hpp`). It sets the pause bit
of `Game.sim_run_flags`, the bit the Pause key flips, so the simulation
stops, the paused title shows and a save stores the match paused; no other
machine hears of it, and the extension's `pause_changed` is not called. A
match the player already paused, by the Pause key or an in-game menu or
dialog that holds it, is left alone. `settle_away_pause`, once a frame
before the match clock steps, lets the bit go `away_resume_ms` (500 ms, as
long as `focus_click_ms`) after the first frame the window is back; Pause
pressed in that moment keeps the match paused as the player's own pause,
and Resume on the game menu lets it go at once. The battlefield counts as
in play under the away pause, so the click that brings the focus back is
still only that. A multiplayer game (`keeps_running_inactive`, or a match
shared with other machines, hosting with only computer players included),
a replay, a finished match, the menus and a frame-limited,
remote-controlled or director's run are never paused. With Keep playing a
game played alone keeps the main loop running every frame while the
window is away (`keeps_playing_away`), as a multiplayer game does, except
while the system has the application in the background.
`--check-away-pause` (`runtime_away_pause_check.cpp`) checks it.

## Gamepads

The gamepad controls (described in docs/controllers.md) are
a way to play added beside the mouse, the keyboard and touch, laid out for
the Steam Deck and working with every gamepad SDL knows. They switch on when
a gamepad sends input, or when `--check-pad-controls` forces them, and then
stay on for the run (`pad_used`). Until then nothing of theirs draws, lays
out or takes a key, so a desktop without a gamepad plays as before. Every
order a pad gives goes through the Runtime functions a mouse, a key or a
finger reaches, so saves, recordings and network games are unaffected.
`start_gamepad_subsystem` (`pad_state.hpp`) starts SDL's gamepads right
after video starts, in `main.cpp` and in `runtime_present.cpp`; a failure
leaves the game without gamepads.

- `runtime_pad.cpp` is the dispatcher, and logs each gamepad it opens,
  with its ids and whether Steam Input gives it. `dispatch_event` hands it
  every event the touch controls did not take (`take_pad_event`): gamepads
  added and removed, buttons, axes, touchpads and sensors, and, while a
  gamepad is open, the F13–F16 keys Open Annihilation's Steam Input layout
  sends for the back grips. `tick_pad`, from `idle_tick` before
  `tick_touch`, runs the sticks, glide, gyro, hold timers, menu repeat and
  ring aim, and writes the pad's looks into the touch HUD state.
  `pad_screen_changed` lets go of held buttons when the screen changes;
  `pad_force_held`, `pad_steam_input` and `pad_settings` answer the rest of
  the engine.
- `runtime_pad_actions.cpp` holds what each button does in each layer
  (the grips' latches and FORCE, the order and build rings, the groups,
  game and standing orders layers, SELECT ▾ and the menus, where A and Menu
  send Return, which presses a 640×480 menu's focused button or, with none,
  focuses its first, `press_frontend_focus_key`);
  `runtime_pad_pointer.cpp` the pad's pointer (the right trackpad, the
  stick cursor and the gyro, sent as synthetic mouse events of the pad's own
  mouse, `pad_mouse_id`) and the camera the sticks and the left trackpad
  move; `runtime_pad_haptics.cpp` `play_pad_feel`, a trackpad pulse where
  the driver takes it, else a rumble.
- `runtime_pad_check.cpp` is `--check-pad-controls`
  ([testing.md](../../docs/development/testing.md#gamepad-controls)); while
  it runs, the dispatcher opens only SDL's virtual pads, its stand-ins
  (`PadState::virtual_pads_only`).
- `oa-ui-paint` (`src/ui/paint`) paints the button glyphs of the
  Steam Deck, Xbox, PlayStation and Nintendo styles with the touch layer's
  painter: the project's own shapes and letters.

`pad_state.hpp` defines `Runtime::PadState`, made on first use and null on
a desktop that never sees a gamepad (`pad_`): the open pads and what each
has, the pad that last sent input, whether the layer is on, the grips' keys
seen, FORCE, and the check's clock. `runtime.hpp` declares every gamepad
member once; the dispatcher's private helpers live in `PadAccess` and the
check's in `PadCheckAccess`, friend structs whose static functions take
`Runtime&`. The pure model the dispatcher drives (the maps from buttons to
actions, the timing pieces, the pointer, the stick cursor, haptics and
glyphs) is the module in src/ui/pad-controls.

`touch_control_scale` (`runtime_touch.cpp`) is the touch layer's points
scale from the Control size setting. `runtime_text_input.cpp`
(`start_text_input`, `stop_text_input`) starts text input with the field's
place given to the system, so that an on-screen keyboard, Steam's in Game
Mode among them, opens clear of it.

## Game folder

`game_directory.cpp` finds the installation: `--game-dir`, else the
platform's default folder while it is usable (`PlatformHooks`'
`default_game_folder`; a platform's folders may move, so it ranks above
the remembered one and is never remembered itself), else the folder
remembered in the preferences, else the folder dialog
(`game_directory_dialog.cpp`) until the player picks a usable folder, which
is then remembered. A build without the folder dialog
(`OA_NATIVE_FOLDER_DIALOG` off) never asks: it tells the player which
folders it looked at, why each cannot be played, and the platform's advice
(`missing_game_folder_advice`), else the `--game-dir` advice. A folder that holds no game archives but holds the
installer of the Total Annihilation demo (1997) is usable too:
`demo_installer.cpp` recognises the installer by its size and SHA-256, never
by its name, and unpacks the game data archive it carries, checked by its
SHA-256, into `Games/totala-demo` in the player's Open Annihilation folder
(`--data-dir` names another folder only when the player's folder is not
known), with its `.oa-content` record, where later starts check it and use
it again. An earlier unpack in `demo-1997` of the per-user data folder is
copied from once, sharing disk blocks where the volume can, and left there.
While the archive passes that check the installer is recognised by its size
alone, since only unpacking
reads it. That folder is the installation, with the checked archive as its
only archive whatever else the folder holds, and the folder the player chose
is the one remembered. Temporary files an unpacking that stopped part way
left there are removed on a later start. The installer is only read, never
run; any other program in the folder is refused with a message saying it is
not the release the engine recognises, and a failed unpacking or a full disk
is reported the same way.
`DemoRelease` holds everything that identifies the release, in one place, so
that the tests (`demo_installer_test.cpp`) substitute a synthetic one.

Where the in-engine locator is offered, it asks instead of opening the
system's folder dialog (`folder_chooser_screen.*`, over the model in
src/ui/folder-chooser). The player browses places, found installs, volumes
and launcher prefixes, and the column says what each folder or file is.
Touch, a pointer, the keys or a gamepad drive it. On the desktop the system's
dialog stays a button on that screen. Steam's Game Mode does not offer the
button and never opens the dialog. Like the Game files screen, the chooser is laid out, hit
tested and focused by the UI kit, so every OA screen is one UI stack, and it
reads the window, shows its frame and reads the pointer through the same
window-pixel code (`window_pixels.*`, below); its gamepad handling is its
own. `folder_chooser_offered` says whether it may show, and
`run_folder_chooser_until_resolved` runs it from `main.cpp` before the Game
files screen. Resolution looks for folders where Steam, Heroic and Lutris
put Total Annihilation (the module in src/platform/game-installs)
after the remembered folder; one usable folder is used and remembered
(`GameDirectorySource::found`), and the main menu says once where it was
found (`found_install_notice`, `runtime_found_install.cpp`,
`tell_found_install`). A folder the chooser picks is inspected by
`take_chosen_folder`.

## Game files screen

Where the platform brings game files into the game's own storage (on a
phone or a tablet, whose apps cannot read the player's installation where
it is), it installs `GameFilesHooks` (`game_files_hooks.hpp`, in
`oa-app-platform-hooks`): its file picker, listing and copying the chosen
files, free space, time to finish a copy away from the screen, device
backups and its own words. The desktop installs none, so
`game_files_import_offered` is false there and nothing below runs. Where
they are installed, resolution reports a missing folder in
`GameFilesNeeded` instead of a notice, and the Game files screen opens on
the game's window ([docs/game-files.md](../../docs/game-files.md)).

- `game_files_import.hpp` (`oa-app-game-files`, no SDL) is the import:
  `game_files_import.cpp` (the files left out, the name check, the plan,
  the space a copy needs, what is installed), `game_files_scan.cpp`
  (listing a source and the `SourceScan` worker), `game_files_run.cpp`
  (the chunked copy and the `ImportRun` worker) and `game_files_state.cpp`
  (the state file, the commit, recovery at the next start, adopting copied
  files, backups). `game_files_import_test.cpp` tests it.
- `game_files_screen.cpp` runs the screen's loop and controller over the
  model of [src/ui/game-files](../ui/game-files/README.md);
  `game_files_paint.cpp` paints its layout with the touch controls' painter
  and the bundled fonts; `game_files_dialog.cpp` hosts the Language
  settings over it, placed by the OA layer's rule over the screen's
  viewport (`kit::layer_viewport`): the scale the Interface size in effect
  chooses, or the Auto scale, held to what fits Compact's dialog, the size
  class of the points left over, centred.
- `game_files_check.cpp` is `--check-game-files`: scripted hooks, the route
  driven by taps and keys, pictures of each step and the verdict line.
- `runtime_game_files.cpp` fills Settings › Game files and opens the
  screen's management state from its Manage… button over the main menu.
- `render_state.hpp` (`RenderState`) keeps the renderer's target, logical
  presentation, scale, viewport, clip, colour and blend mode and puts them
  back, and `use_window_pixels` draws in the window's own pixels: the Game
  files screen, the folder chooser and the OA layer each keep one while
  they draw.
- `window_pixels.hpp` holds the rest of the window-pixel path the Game files
  screen and the folder chooser share: `read_window_viewport` reads the
  window's canvas (its pixels, the pixels a window point holds and the safe
  area), `WindowFrame` shows a painted frame on the window in its own pixels
  through a texture it keeps, and `read_pointer` reads fingers, the pointer
  and the wheel as one press in canvas pixels (`WindowPress`), with
  `touch_screen`, `press_scrolls` (a press over the rows that becomes a
  scroll), `canvas_point` and the drag and wheel distances. What a press
  does, and which touch devices a screen takes, stay each screen's.

## The player's own folder

`user_folder.cpp` (`oa-app-user-folder`, `user_folder.hpp`) keeps the
player's saved games, screenshots, films, recordings and mods in one folder
they can find: `--user-folder`, else the preferences' `open-annihilation.user-folder`,
else `Open Annihilation` in their Documents folder
(`oa::platform::preferences::default_user_folder`), or beside a named
`--preferences-file`, so that a check never reaches the Documents folder
(`choose_user_folder`, `user_folder_beside`). It holds `Saves`,
`Screenshots`, `Films` and `Recordings`, each with a folder for each mod,
named after its id, and `default` for games without a mod's profile, 3.1c's and a mod
folder's without an `oamod.yaml` (`mod_subfolder_name`; a mod whose id is
`default` takes `default (mod)`, which no kebab-case id can be), and
`Mods`, each made when first needed. `runtime_user_folder.cpp` puts it in
effect:

- **Paths:** `Runtime::game_file_path` places each path the game names: a
  `SAVEGAME` path in `Runtime::saves_folder`, or for reading in a folder
  that held saved games before while it holds the name and the folders
  before it do not: without a mod, `Saves` itself while it holds a file,
  then the `SAVEGAME` folder beside the preferences file
  (`savegame_host_path`, `SaveRoots`); any other relative path under
  `save_game_root`, as before; and within the player's own folder, which is
  the Image Output Directory by default (`own_image_output_directory`), the
  `screenshots` folder is the mod's folder in `Screenshots` and a
  `MOVIEnnn` folder lies in its folder in `Films` (`place_capture_path`).
  A relative path whose `..` parts would leave its folder, such as a saved
  game's name taken from a downloaded save's description, has no place,
  and nothing is written for it. A network game's recording goes in the
  mod's folder in `Recordings`, unless `--net-record` names the file,
  which is written as it is given (`recording_file`). A stored Image Output Directory that is
  the game's default, the game folder's folder named after the user, gives
  way to it; any other one the player chose wins, used as it is.
- **The moves:** the first start with the player's own preferences file
  that finds no `open-annihilation.saves-moved` record moves the saved
  games earlier versions kept beside that file, `SAVEGAME` and each
  `mods/<id>/SAVEGAME`, into `Saves/default` and `Saves/<id>`
  (`move_saves_once`, `move_earlier_saves`), and one that finds no
  `open-annihilation.loose-saves-moved` record moves the files loose in
  `Saves` itself into `Saves/default` (`move_loose_saves`); loose
  screenshots and films stay where they are. With `--preferences-file` or
  `--user-folder` nothing moves, so that a folder named for one start
  never takes them from the next. Nothing is overwritten: a name the folder
  moved to holds, matched without case, is kept, and the file moved takes a
  free one,
  `NAME (2).SAV` (`free_file_name`). A file is renamed, else
  copied and the original removed once the copy is whole, else left where
  it is, where the dialogs and the console still find it. Each step is said
  on standard error, which the log keeps. A move that moved or left a file
  is recorded, and one that moved or left a saved game makes its notice
  due (`record_saves_move`, `record_loose_saves_move`). In the same way,
  one that finds no `open-annihilation.recordings-moved` record moves the
  recordings earlier versions kept beside that file, `demos` and each
  `mods/<id>/demos`, every file a recording whatever its extension, into
  `Recordings/default` and `Recordings/<id>`
  (`move_recordings_once`, `move_earlier_recordings`,
  `record_recordings_move`); no notice follows it.
- **The notice:** `tell_saves_moved` shows it over the darkened main menu
  once, as the renderer records' notice is shown: a notice screen of the
  OA layer ([The OA layer](#the-oa-layer)) in the settings dialog's look
  (`oa/ui/engine_settings/notice.hpp`), with the count of the moves
  whose notice is due (`moves_to_tell`), the `Saves/default` path wrapped
  at its separators, and that screenshots, films and mods now go in the
  same folder, or that each mod's go in a folder of its own; **Open
  folder** shows `Saves/default` and **OK**, Enter and
  Escape close it (`UserFolderState::show_notice`). It waits for the layer
  behind an install's prompt, and none of these notices is made while
  another shows or waits. Showing it records it `told`. A run nobody
  watches leaves it due.
- **Opening folders:** the settings' Your files buttons and the notice show
  a folder through `system_folder_opener` (`user_folder_open.cpp`): the
  platform's own `PlatformHooks::show_folder` where it has one (the Files
  app on iOS, through its `shareddocuments` link); else `open` on macOS,
  the shell's open verb on Windows, `xdg-open` on Linux, or the desktop
  portal through `gdbus` where `xdg-open` is missing. A build that starts
  no other programs (`OA_PROCESS_SPAWNING` off) and has no such hook shows
  none and says no file manager is there. A run nobody watches records the
  requests instead (`recorded_folder_opener`).

`--check-user-folder` (`native-user-folder`, `runtime_user_folder_check.cpp`)
checks all of it beside its preferences file: the saves notice in a
640x480 window, Compact at 1× pixel for pixel, then in 800x600 and
2560x1440 windows at Regular 1× and Large 2×, the whole window compared
with the menu darkened and the notice drawn at that class and scale, its
corners rounded as the layer shows them (`round_drawn_screen` for the
notice, Settings and the question the check draws straight).

## Mod profile and mod folders

`mod_profile_loader.cpp` and `mod_folder.cpp` (`oa-app-mod-profile`) do the
file work for mod profiles, which
[src/data/mod-profile](../data/mod-profile/README.md) resolves:
`find_mod_profile` finds a folder's `oamod.yaml`, its name matched without
case, and refuses a folder holding two names that differ only in case;
`load_mod_profile` reads a file and resolves it. `--print-profile` prints the
resolved profile of `--mod FILE`, or of the `--mod-dir` or `--game-dir`
folder, with its sim and full hashes, and exits; a folder without a profile
prints that it plays base 3.1c, and a profile with errors prints each, one a
line, and exits with status 1.

A mod plays in one of two ways, which give the same game:

- **Copied install:** the mod's files copied into the game folder, with its
  `oamod.yaml` in the folder's root.
- **Mod folder:** a folder holding the mod's own files and its `oamod.yaml`,
  layered over a plain game folder: `--mod-dir PATH`, or the folder the
  preferences remember (`open-annihilation.mod-directory`); `--base-game`
  plays without the remembered one, and one that is gone is dropped with a
  notice (the game then plays no mod, and the next save of the settings
  stores that over it: `EngineSettingsState::dropped_mod_folder`). Either
  flag greys the Library's Play now and Switch off for the run
  (`library::Inputs::mod_from_command_line`), as a match does while it
  runs. A game folder holding its own
  `oamod.yaml` cannot carry a mod folder. A mod folder without an
  `oamod.yaml` layers over the game folder with no profile, by 3.1c's own
  rules, which the start says on standard output.

The **Library** chooses the mod (`runtime_library.cpp`).
`list_offered_mods` (`runtime_engine_settings.cpp`) finds the
mod folders of the game folder's `mods` folder (`list_mod_folders`), of the
player's own `Documents/Open Annihilation/Mods` (`list_mods_in`), the folder
an earlier version's Pick Folder... stored
(`open-annihilation.picked-mod-directory`) while it is the mod stored
(`open-annihilation.mod-directory`) and still a folder, and the one played;
a folder found twice counts once. `library_inputs` lists each that the
installs' own records do not as an installed mod of its own, its key the
profile's id or the folder's name. Once another mod or No Mod is stored,
the picked folder is forgotten: the save erases its key, a start that
finds the key naming another folder erases it with the next save, and the
folder is listed only while the game folder's `mods` folder or the
player's own Mods holds it. `read_mod_summary` (`mod_summary.hpp`) reads
each from the folder alone, without resolving its profile: its
`oamod.yaml`'s name, version and description, and its `oamod.png` badge,
decoded by the engine's PNG reader (`oa/formats/png.hpp`), which the
Library's row shows. The details show the description where no listing
gives a summary (`library::entry_summary`), as they show the description
an installed package's manifest gives (`package_install::InstalledPackage`,
`content::InstalledPackage`); a folder without an `oamod.yaml` shows "N/A"
for its version and "No oamod.yaml present" in its status line
(`library::Installed::without_manifest`), and one that cannot be read says
why in the details. Settings › Your files' Mods opens
`Documents/Open Annihilation/Mods`.

Play now on a mod not played, and Switch off's SWITCH on the mod played,
call `Runtime::switch_to_mod_folder` (`runtime_mod_install.cpp`), with no
folder for no mod. A folder is first checked as the next start will play
it (`check_picked_mod_folder`): one it could not play is refused, a note
over the Library says why, and nothing changes. Otherwise the new Mod is
put in effect and saved with the other settings, then a soft restart is
asked for (`Runtime::soft_restart_requested`); a save that fails leaves
the game as it is and says so. The runtime's loop ends after that
frame. `main()` (`run_once`) frees the textures the runtime made on the
renderer (`Runtime::release_renderer_textures`), destroys the Runtime and the
mounted archives, runs the game-folder search again, which reads the newly
stored mod and resolves its profile, sets the data layout again, mounts the
archives again and builds a new Runtime on the same window and renderer, with
the full-screen state Alt+Enter left and the intro movies skipped
(`Options::restarts`), which opens on the main menu playing the new mod. The
player never exits the game.

A switch made in the Library, Play now or the SWITCH of the question SWITCH
OFF and Unload mod ask, also opens the Library again in the next run as it
showed. Before switching, `Runtime::keep_library_for_restart` notes what it
shows in a `Runtime::LibraryReturn`: the tab, each tab's filter, the tag,
the search, the selected entry and its place in the list, the list's scroll
and the details. `restart_line` is what the progress dialog says while the
switch applies, "Switching on Ridge…" or "Switching off Ridge…" in the
language shown (`switching_line`); a refused switch forgets both. `run_once`
hands them to the next run: the line as `Options::restart_line`, which the
reload's dialogs say in place of "Applying your changes" (the frame
`show_work_frame` leaves on the window as the run ends, then the mount's and
the new Runtime's `run_long_work`, which shows a line its catalogue does not
hold as given), and the Library through `take_library_return`.
`library_return` gives only what the run that ends noted, never what it was
handed: a run whose reopen did not happen hands nothing on. The new run's
`reopen_kept_library`, from `tick_library`, opens the Library once the main
menu lets it open (`library_can_open`, as `open_library` is guarded) with no
other screen of the OA layer showing (`kept_library_may_show`), and once its
feed lists the installed packages (`LibraryFeed::installed_listed`, the
worker's first search for the runtime's own feed), so that no entry moves
as they arrive. Until then the switch's work dialog shows over the main menu
where the Library could. The wait is `kept_library_wait_ms`, five seconds
from the run's first frame on the main menu: then the Library opens with
what is listed if it can, and is dropped if it cannot, whichever screen
shows, so that it never opens later on a main menu that became free. It
opens over the work dialog, which then closes under it, so a screen waiting
in the layer's queue waits for the Library. The chosen entry is selected
once its feed lists it, unless the player has chosen another
(`LibraryHost::select_kept`), and the list keeps its scroll while that
entry is where it was; otherwise its row is brought into view
(`ScreenState::reveal_row`). `--check-mod-switch` (`native-mod-switch`,
`runtime_mod_switch_check.cpp`) switches ten times between No Mod and two
test profiles through that same path, on the dummy video driver's window,
and requires what the host heap holds in live allocations as the last
round's runs reach the main menu (`oa::platform::sample_host_heap_use`) to
stay within 64 KiB of the first round's after the first start that played
the same mods. Runs that play the same mod hold the same blocks; the working
set, which it prints beside them, also counts the free memory the allocator
keeps, which grows over the switches by an amount that changes from one
start of the check to the next.

A mod folder whose profile plays but whose game files are missing is never
refused (`runtime_mod_warning.cpp`). `Runtime::mod_start_gaps` finds what
keeps its games from starting: no unit in its folders that the match's
unit catalog keeps, under the Version and Copyright rules a match applies,
or a side SIDEDATA names whose commander is not among them (a side that
names none is passed over), looked for in the file of the commander's own
name and, when not found so, by the UnitName each definition gives
(`find_mod_start_gaps`, `user_folder.cpp`). It also finds, for any mod
(`Runtime::plays_mod`), with a profile or without one, each file a side's
section names, its intgaf or font, that the mod's folders lack
(`Runtime::missing_side_files`): the warning names them, and its games
start and play without them, a side without its intgaf's GAF drawn without
panels and one without its font in COMIX, unmeasured, as sides that name
none (`Runtime::match_side_file`). The game played without a mod ends its
start instead when such a file is missing, naming the first: every side's
intgaf before any side's font (`Runtime::require_side_files`). The
Library's Play now refuses a folder without a profile whose start would
end on a side's missing section or no side at all, before anything is
stored
(`Runtime::side_data_problem_over`). SIDEDATA and the files its sides name
are looked for in the language folders of the language the game starts in
first (`gamedata-German`, `anims-German`, `fonts-German`), once for the
run; a language chosen later reads them at the next start. The main menu
shows the warning (`mod_files_missing_notice`) once from each start of a
mod that lacks any of these (`Runtime::tell_incomplete_mod`), as the notice of the saved games' move, in
the settings dialog's look, its Open mod folder button showing the mod's
folder through the same opener. Every start of a game asks
`Runtime::refuse_incomplete_mod_start` first, which refuses it only for
missing units or a missing commander: Skirmish's Start, the main
menu's MULTI, a campaign mission and a saved game, and a shared game's
launch through `Runtime::bootstrap_match`, which network play's launch
calls, so network play names no new private name of `Runtime`.
A refused start shows the warning over the screen it was asked from, which
stays, or, from a match, the loading screen or a screen package's frame,
over the main menu when it next shows. A skirmish start that fails all the
same (`Runtime::start_skirmish_from_setup`) leaves no match and sets the
skirmish setup up again, with the warning or the failure in a message box,
in place of ending the program. `--check-mod-warning`
(`native-mod-warning`, `runtime_mod_warning_check.cpp`) checks the warning
and the refused start through the Library's Play now and its note, the
main menu's warning in 800x600 and 2560x1440 windows at Regular 1× and
Large 2× too.

`inspect_game_install` resolves the profile (`resolve_folder_profile`) before
any archive is mounted: `--mod`'s file, else the mod folder's, else the game
folder's. It resolves it twice, first to learn its settings file and
registry root, then with the settings it binds: the INI of that name from
the first folder that holds it, read only (`read_ini_settings`; a `;` ends a
value, as the mod's own readers take only its leading number or word), and
the preferences' registry section. A profile that cannot be used makes the
folder unusable with every error, and the game never falls back to 3.1c.
The profile's layout then names the archives discovery mounts
(`discovery_plan_of`) and the directories of the resources the folder must
provide; with a mod folder the asset store layers it over the game folder
(`GameInstall::folders`, `Options::game_folders`), so that it reads exactly
as a copied install of the same files. `layout.installation-archives` names
archives from the installation that mount after the mod's own ccx and ufo
archives and before the hpi group, in the order written; one that is missing
or cannot be opened makes the folder unusable.

`main` puts the profile in `Options::mod_profile`, and `Runtime::mod_profile()`
returns it (null for base 3.1c) to everything that reads it: the runtime
copies its limits into `limits_` before anything sizes a table from it, and
every match is built with its rules, so unit scripts read the extensions it
mounts, the unit types' and weapons' own records (`unit_type_rules_`,
`weapon_rules_`) and its sim hash. The unit and weapon files are read with
the data keys the profile binds (`oa/data/defs/rule_keys.hpp`) into those
records and, for the build-cursor preview, `unit_preview_keys_`; a bound key
whose value cannot be used is reported and the profile's default applies to
that type. `main` also puts its layout in the data layout every loader reads
(`oa/data/defs/layout.hpp`, `data_layout_of`): the renamed directories, the
unit file extension, the map units section, the build version unit files are
checked against (the profile's network version) and the two side names. The
runtime then:

- shows the profile's display version on the main menu, when it names one
  other than 3.1c's, even where its game data holds no `version.tdf`.
  Without one the label reads "Open Annihilation" and this build's version
  where the game data names its revision, and stays hidden where it does
  not;
- keeps the game's settings under the profile's registry root, as
  `registry:<root>\<section>|<name>` keys, unless the root is the base
  game's; a first run seeds the profile's registry seeds where no value of
  that name, matched without case, exists (`seed_registry`);
- keeps saved games in `Saves/<id>` in the player's own folder
  (`Runtime::saves_folder`), screenshots and films in
  `Screenshots/<id>` and `Films/<id>` (`Runtime::game_file_path`), and
  the recordings of network games in `Recordings/<id>`
  (`recording_file`);
- reads the movies, the music folder and the disc archives through the
  folders, the mod folder first; a profile whose `cd-check` is false always
  finds its disc.
- hands its setup and team rules to the battle room
  (`multiplayer_bind_rules`) and to each network match
  (`net_match_bind_rules`), and with `setup.map-scripted-units` places a
  skirmish or multiplayer map's schema units in place of the commanders and
  as their countdowns come due (`runtime_map_units.cpp`);
- sizes the weapon table and the side table from the data, within the
  profile's `weapon-types` and `sides` (`load_weapon_defs`,
  `load_side_data`, `side_table_past_limit_line`); copies the limits onto
  the room's wire rules with `wire_rules_of`, which the room reads through
  `oa_only`; and writes and reads a recording's limits sector as
  `RecordedLimits`, judged by `demo_limits_verdict`.

A save made under a profile holds a ModProfile account (its id, version,
catalogue, hashes and the match's rule-state tables); such a save loads only
under a profile of the same sim hash, which the status line otherwise names
with the game's, and a save without one, written by 3.1c or by a mod's own
client, loads under any.

### Mod packages (.oamod)

A mod package is a zip archive of a mod's folder, which the game installs
into the player's own Mods folder (`src/app/package-install`,
`oa-app-package-install`, says how a package is read, planned, unpacked and put
in place, and how what a stop leaves is settled). The runtime's part:

- **Opening a package.** `--open FILE` and `--install-mod FILE`, repeated,
  and any bare argument naming a `.oamod`, `.oalang`, `.oamap`, `.oamut` or
  `.oareg` file, any case, are files to open once the main menu shows. `post_opened_file`
  sends a `.oareg` file to the add-registry queue, which opens the Add
  registry window (see the OA layer's Registries), and every other file to
  the package inbox (`oa/app/package_install/inbox.hpp`); macOS's `-psn_`
  argument is skipped. A file the system opens in the game, or one dropped
  on the window, arrives as SDL's drop event: a watch on SDL's events
  (`mod_install_watch.cpp`), started right after each start of SDL's video
  (the window, the folder dialog's and the runtime's own), copies each file
  the game opens into the inbox as it is queued, whatever polls the queue
  then, reads a launch file (a `.oalaunch` file, or any file whose text
  begins `oalaunch:`, as `open -f` hands one over) into network play's
  waiting launches (`oa/app/netgame/opened_launch.hpp`, see
  [Network play](#network-play)), and logs any other. A platform that
  brings opened files into its own storage first does so through
  `PlatformHooks::take_opened_file`, and gets its copy back through
  `release_opened_file` once the install is done with it. On Windows and
  Linux a second start that carries only such files, while another copy holds
  the instance lock, hands them over through the hand-off folder
  (`oa/app/package_install/handoff.hpp`) and ends; the running copy looks there
  once a second, in the background too, and brings its window forward.
- **At the main menu** (`runtime_mod_install.cpp`,
  `Runtime::tell_mod_installs`): once the menu has settled for two frames,
  with no dialog, notice or settings dialog over it, the next package is
  read, by the kind its extension names; a plan that asks nothing unpacks at
  once, and the others show their
  question in a `Prompt` (`oa/ui/engine_settings/prompt.hpp`) over the
  darkened menu, a question screen of the OA layer
  (`ModInstallState::show`, `QuestionScreen`) that waits for the layer to be
  free; the same screen then shows the progress and what came of it, and
  another screen replacing the main menu sets a question aside and ends a
  telling. The unpacking runs steps of a 256 KiB budget,
  each folder or file made costing 16 KiB of it and each step ending after
  about 4 ms, for up to 15 ms a frame under the progress prompt; then the
  prompt says
  the files are being put in place, and the next frame makes the change,
  pumping events while it waits for files another program holds. What came
  of it is told: MOD INSTALLED, MOD UPDATED or MOD NOT INSTALLED, with OPEN
  FOLDER and, where the Mod setting is the player's to change, Play now
  (`Runtime::switch_to_mod_folder`, the Library's own switch). A run
  nobody watches leaves packages waiting, but for a catalogue language
  pack, a mod chosen in the Library while the Library is the top screen,
  and a catalogue map pack while the map browser is the top screen or GET
  MAP shows over the battle room ([The Library](#the-library)).
- **The mod played.** A replace or reinstall of the folder the game plays,
  once unpacked and checked as Play now checks a folder, waits in
  `set_pending_change`, holding the Mods folder's lock, and the run ends
  (`request_soft_restart`); `main()` makes the change between runs, once
  the runtime and its archives are gone, and the next run tells it. A mod
  chosen in the Library is held instead until the player leaves the mod
  (switching mod or quitting; [The Library](#the-library)). Every
  run starts with `recover_package_installs`, which runs `recover_changes`
  for each kind's root folder (Mods for a mod, Maps for a map pack), before the mod
  folder is resolved, so that a stop mid-change never drops the Mod
  setting. A file of no known kind is told NOT INSTALLED and is not read.
- **Roll back** in the Library (`Runtime::roll_back_mod_folder`, which
  takes the folder): a folder of the player's own Mods folder whose
  `.backup` keeps an earlier version of its mod, one Play now would accept,
  offers it (`OfferedMod::roll_back`, filled by `list_offered_mods`; the
  Library hides the kept version otherwise). The kept version is checked
  again first; then the folder is swapped at once, or, for the mod played,
  as the run ends. A roll back refused, the kept version changed since it
  was listed or the swap undone, is told in a prompt over the Library (MOD
  NOT ROLLED BACK, `ModInstallState::roll_back_refusal`), pushed over it at
  once, whose OK returns to it; the status line shows the same words.
- **Registering the file type.** A start someone plays makes the game the
  opener of `.oamod`, `.oalang`, `.oamap`, `.oamut` and `.oareg` files, and of
  `.oalaunch` launch files (opened with `-c FILE`), for the player
  where the system registers at run
  time (`oa/platform/file_types.hpp`); none does with `--preferences-file`,
  `--user-folder`, `--data-dir`, unattended, headless or on SDL's dummy or
  offscreen video driver.

`native-mod-install` (`--check-mod-install`, `runtime_mod_install_check.cpp`)
drives every prompt over five runs, pressing each where the OA layer shows
it, and holds the first update question to its picture in 800x600 and
2560x1440 windows at Regular 1× and Large 2×.

### Map packs (.oamap)

A map pack is a zip archive whose manifest is `oamap.yaml`. It installs into
`Maps/<id>` in the player's own folder, with the same staging, lock,
`.backup` and recovery a mod has (`src/app/package-install` says the checks
and the questions). A file the player opens is asked on the settled main
menu. A pack from a catalogue installs on any screen except while a match
loads or runs, and shows no prompt: a question waits for that menu, and a
refusal is reported to the screen that asked. In a run nobody watches it
installs only while the map browser is the top screen or GET MAP shows
over the battle room, and waits in the inbox otherwise
([The Library](#the-library)). A file the player opens, and a package of
another kind, still wait for the menu. Nothing starts while a match loads
or runs.

Before the pack is put in place it is checked: the manifest, that the pack
holds the files it lists and nothing else, that those files sit in the
folders a map pack may hold, that each preview is a PNG of at most 2 MiB
and 1024 pixels on a side, that this build meets `requires.engine`, and that
every map fits the content the game finds its features in, with no mod:
the folders the game plays without the mod's (the expansions that are on,
then the base game), with each expansion the pack's `requiredExpansions`
names, and those it needs, that the player has copied
(`map_pack_fit_folders` and `check_content_fit` in `map_packs.hpp`). Their
archives are mounted as the game's discovery mounts them from those folders
together, so a feature an expansion defines, such as Core Contingency's
`ArchiAquaOre1` to `ArchiAquaOre3` in its own archive, is found, and a
feature none of them defines is refused, named. A clash with the mod being
played does not refuse the install.

A folder of the pack's id that holds nothing but empty folders, at most
256 of them, is free: the kind reads it as missing, the install removes
those folders (each with the call that removes only an empty folder)
before the staged files move in, and the deletion of a discard folder
removes such a folder at its package's name once it is done, as a file sync
can bring the empty folders of an uninstalled pack back. A folder that
holds anything else is refused as before, and the plan's refusal is logged
(`open-annihilation: package install: <file>: The folder <id> does not
hold this map pack. It is left as it is.`).

The runtime keeps one index of the installed packs (`MapPacks`,
`src/app/map_packs.cpp`) over that Maps folder, made when the packs are
first needed and refreshed after each install. It lists each pack's maps
from the manifest. A manifest is read once, and again only when the pack's
key changes. The key is the SHA-256 in the pack's origin record, or the
SHA-256 of its `oamap.yaml` when it has no record. A terrain's size is the
size of the map's TNT file. The index does not open a map.

### Map packs in the game

The skirmish map list lists the base maps as the scan of `Maps/*.ota` finds
them, then each installed pack map that fits the mod being played, by its
name `<stem>@<id>`, in the index's order. The battle room's map list holds
the same pack maps among the base maps, sorted by name. A row is that game
name. The title under a chosen pack map is the title from the pack's index
when the screen has a name there. The stock skirmish screen has none, so
its rows stay the game's names. The battle room's map name is the title.

`refresh_listed_pack_maps` (`runtime_pack_maps.cpp`) builds that pack-map
tail. It runs from `discover_first_map`, after a map pack is installed or
removed, and when the catalogue snapshot moves on. No pack map's OTA or TNT
is opened to list it. The base maps stay in the order and under the names
the scan gave them, and are not scanned again. The first base map stays the
default selection while it is still listed. The map picker (SELMAP) sorts
the base maps as before and the pack maps after them.

A pack from a registry follows the catalogue when the snapshot has a row
for the mod's key: true lists the map, and a recorded false leaves it out
without asking the files. The base game's key is `ta-3.1c`. A catalogue
mod's key is `<catalogue-id>@<release>` from the mod folder's origin
record. An empty key, or a key that table lacks, is decided by the
map-fit check, and a miss is final. A release the snapshot does not list
yet is listed by the map-fit check until the catalogue has it, and a miss
then is not logged. A pack with no catalogue origin is sideloaded: the
map-fit check decides, and a miss is final. One log line names each pack
and mod the first time a map of that pair is left out:

```
open-annihilation: map pack <id>: <n> map does not fit <key>
```

The plural is `maps do not fit`. `<key>` is `the mod being played` when the
mod has no catalogue key.

Only one map's files are ever mounted (`runtime_pack_maps.cpp`). Choosing a
pack map (`select_map`, from the picker, the skirmish setup, a saved game,
a recording or the network load) mounts that map's files alone as the
asset store's pack layer, under every archive and loose file, labelled
with the pack's id and the first 8 hex digits of its SHA-256. Before that
the map is checked against the game and the mod played
(`oa::data::map_fit::check_map`, over the files in the pack's folder); the
game's names are collected once for the run, whose archives never change,
and the result is kept for each pack's SHA-256 and map stem. Once mounted,
the files are checked again as the store shows them, whatever the kept
check found, since the pack's folder may have changed on disk; a failure
there unmounts them. A map that fails either check is refused. One the
lists left out is still refused when something chooses it by name. One
the lists kept, because a catalogue recorded it as fitting, stays
listed: highlighting it shows its title, its size and players and
"Doesn't fit {game}: {reason}", where the game is the mod played or
Total Annihilation 3.1c and the reason is the first failure, with no
picture, and choosing it says why in a message box and keeps the earlier
map. A start on a refused map fails as other loads do. The layer stays
while that map is selected and through its match. It is unmounted when
another map is chosen, at the end of the match (`leave_match`) and
whenever the main menu shows (`Runtime::load`). Mounting and unmounting
happen on the main thread, never while a match loads or runs: the
match's start mounts its map, if a refused highlight released it, before
it reads anything.

The log says what happened, one line each:

```
open-annihilation: map pack <id>: mounted <name> (<n> files)
open-annihilation: map pack <id>: unmounted <name>
open-annihilation: map <name> does not fit: <rule>: <why>
```

A map that does not fit logs one line per failure, each time it is refused.

`native-map-packs` (`--check-map-packs`, `runtime_map_packs_check.cpp`)
writes two packs into the Maps folder of the run's own `--user-folder`: one
whose map, made in code with one feature of its own drawn with a model of
the game, fits, and one whose feature file also defines a feature the game
has. It requires the fitting map listed after every base map, in the list
and in the picker, and the clashing map in neither, with one log line for
that pack and the base game; nothing mounted before a map is chosen; the
fitting map mounted alone when chosen and unmounted for a base map; a
skirmish on it playing 30 ticks with its terrain readable, and its end
unmounting it and leaving the archives as they were; the clashing map
refused for New names when it is chosen by name; and nothing mounted at
the main menu.

### Developer Mode

Developer Mode, in the settings' Developer section
(`EngineSettings::developer_mode`,
[src/ui/engine-settings](../ui/engine-settings/README.md#developer-mode)),
lays the player's overrides of the standard hacks
(`EngineSettings::hack_overrides`) over the profile the game plays.
`load_engine_settings` first sets up its layers (`load_profile_layers`):
it reads again what the profile is resolved from, the mod's file and the
settings it binds (`folder_profile_source`), or without a mod the plain
3.1c baseline (`base_game_profile_text`); the id the overrides are kept
under, the profile's, `ta-3.1c`, or for a mod folder without a profile
`folder:` and its path (`folder_overrides_id`); and the profile's hacks as the dialog
shows them. Each time Developer Mode or the overrides change,
`apply_hack_overrides` resolves the profile again with them as its last
layer, reporting on standard error, once, each override the resolver left
out, and keeps the result as the latest profile. Off, or with no override,
the latest is the profile as it ships, and the game plays as without the
setting.

The display rules (`ui_rules`) read the latest profile at once, a running
match's voices and explosions (`Match::set_display_rules`) and the
player's view settings included. The rules (`mod_profile`) read the
played profile: the latest one while no match runs, held from a match's
start until `teardown_match` puts the latest in play, so that a running
match and its saves keep the rules it started with; a saved game is checked
against the profile the match it loads into will play
(`next_match_profile`). A game without a mod plays by no profile while its
overrides change none of 3.1c's rules (its sim hash is the baseline's), so
its saves and network games are as without them; once an override changes
a rule, it plays the baseline with the overrides, whose account its saves
then hold.

The run names the profile and its sim hash. `--print-profile` applies the
INI settings of the folder it prints, as a game does (`mod_settings_of`).
`--accept-unimplemented-hacks` accepts, with a warning each, hacks this build
does not implement yet, for development. `--trace-lookups FILE` writes every
game file and listing the run looks up, one a line, and, for a path a
mutator serves or hides, which mutator, which shows that a
renamed directory is never read by its base name. `app-mod-profile` and
`game-directory` cover these; `native-mod-layout-data` checks a mod's real
data (docs/development/testing.md).

### Display rules

A profile's display rules (`ModProfile::ui`, the `ui.*` hacks) change what
the player sees and hears and how this machine's input works, never what
another machine is told. `view_rules.hpp` (`oa-app-view-rules`) turns them
into the records and decisions of the modules that carry them out, and
`app-view-rules` checks each against 3.1c's:

- **Voices and explosions** (ui.unit-voice-fixes, ui.effects-tweaks):
  `match_display_rules` gives the match its `DisplayRules`
  ([src/sim/match-runtime](../sim/match-runtime/README.md#rules)), and
  gives a running match new ones as Developer Mode changes them.
- **Music and the victory announcement** (ui.audio): `music_source` picks
  the disc's layout, numbered MP3 files from `1.mp3` or every MP3 file of
  the folder in name order ([src/audio](../audio/README.md)). A match starts
  without waiting for the track to open; the music begins when it is ready,
  and a track that cannot be read is skipped. On macOS the start counts
  files under Mods, Maps, Languages, Mutators, Games and Expansions that are
  not on this Mac yet, from startup, and the music folder once it is known,
  and asks for them in the background
  ([files kept in iCloud](../../docs/troubleshooting.md#files-kept-in-icloud)).
  The CD music
  pauses as a finished match leaves for its end screen
  (`music_end_game`); drawing the victory banner plays the "Victory
  Condition" sound, at most once in 300 ticks by
  `victory_announcement_due`, whose tick is kept across matches. Sound
  keeps playing while the window is in the background, and sounds are
  placed in 3D by the Sound Mode setting, which the profile's registry
  seeds set.
- **Display modes and screenshots** (ui.display-modes): with
  `min-height-768` the options screen and the settings' Screen size offer
  only sizes of 768 rows or more (`minimum_mode_height`), and the DisplaymodeWidth and
  DisplaymodeHeight settings start at 1024 by 768 and raise a smaller
  stored width or height to it as they are read (`display_mode_setting`).
  Ctrl+F9 and Print Screen, on release and on every screen before the
  screen sees them (`handle_view_rule_key`), save the frame as an 8-bit
  PCX in the Image Output Directory's `screenshots` folder, the mod's
  folder in the player's own folder's `Screenshots` by default
  (`Runtime::game_file_path`), named by
  the date, the map and the players, or SHOT outside a match, with the
  first unused number from 0 (`screenshot_file_name`,
  `capture_named_screenshot`). The graphics-driver warning `dx-warning`
  keeps or drops is one the engine never shows, so both values play alike;
  a forced resolution comes from the profile's registry seeds like any
  other setting.
- **Click snap** (ui.click-snap): while the snap override key (Alt) is up,
  a build click with a metal extractor snaps, within the mex radius, to the
  place whose footprint holds the most cells richer than the map's
  SurfaceMetal, the nearest of the best to the cursor (`snap_cell`), and
  keeps the snap only when snapping again from there, with the larger
  footprint side as radius, agrees; a building whose yard map holds a
  geothermal cell (read as text, so an open cell before it hides it) snaps
  to the nearest place it may be built. A reclaim click on ground with no
  feature snaps, within the wreck radius, to the nearest reclaimable
  feature with metal or energy, and a queued order then cancels only
  within -8 to 7 whole pixels (`Match::cancel_queued_order`). The ghost
  shows a snapped site as one that may be built on. The radii start at the
  profile's defaults and are held to its maxima (`click_snap_radius`); a
  radius the profile gives no room is never stored (`write_view_settings`).
  With RECLAIM armed and no unit under the pointer, open ground where a
  click would snap shows the Reclaim cursor when the cursor at the snapped
  point would be Reclaim (`shown_match_cursor`). `native-click-snap` shows
  the cursor and clicks beside a wreck and a deposit, and
  `native-click-snap-off` does without the hack.
- **Line and ring build tools** (ui.build-tools): in build mode, with the
  autoclick key (X) held, a click starts a line at the cursor (at a snapped
  site's middle cell when the click snaps); the line follows the cursor
  (`line_build_slots`: whole footprints plus the spacing along the axis
  with more cells, a cell at a time along the other) and the next click
  gives it, one queued build order a building, and starts the next line
  there. Over a unit the tool lays a ring around its footprint widened by
  the spacing (`ring_build_slots`, corners closed with full rings) and a
  click gives it. The mouse wheel, Page Up and Page Down change the
  spacing (0 to 10) while the key is held; letting the key go drops the
  line unbuilt. A line of 2 by 2 buildings is given as a staggered double
  row with Optimize DT rows (`optimize_dt_rows`). The ghost outlines every
  building. A site the build cursor accepts over the local player's own
  units (orders.build-site-kickout place-over-own-units) is outlined in
  interface colour 14 in place of 10, as a building there has them moved
  off. With the snap override key (Alt) held, a left press on one of the
  local player's own units with a movement object picks it up and the
  release sends it to the terrain under the pointer ahead of its orders
  (`order_drag_pointer`, `Match::send_ahead_of_orders`): its first order
  waits behind the move, or starts again from its beginning behind it, and
  its later orders are kept, as the kickout sends a unit off a site.
- **Build preview** (ui.build-preview): over a site the game accepts, the
  building being placed is drawn at its site through a stand-in unit
  record (`ready_build_preview`), facing the way the building is built:
  half a turn from heading 0, then its facing's quarter turns. It is drawn
  with build bands of its own (`ModelState::build_bands`, which the
  processor's `apply_build_effect` and the Full tier's nanoframe take in
  place of the unit's), set for each frame from its pulse
  (`build_preview_look`, `build_preview_bands`): every 30 ticks of the
  match, restarting as the player turns it, its outline walks the nano
  greens from bright to black and back, with `fill` the model is filled
  half a walk away, and for the first 15 ticks a bright green scanline
  climbs the model from its foot to its top. The pulse follows the match's
  ticks alone, at every zoom and frame rate. A type's preview keys pick
  the pieces drawn (`preview_lists_piece`, per facing first) or another
  model (`objects3d/<name>.3DO`); without them every piece is drawn but
  the muzzle flashes and wakes (`preview_skips_piece`).
  `--check-build-preview` (`runtime_build_preview_check.cpp`,
  `native-build-preview`) places a tower in the standard, Basic and Full
  tiers at three zooms and checks the pulse's period, that a frame drawn
  again matches, that nothing is drawn over a refused site, and that the
  tower lies over a finished one at its site. The rotate key (/, without Ctrl) or the
  snap override key with the wheel turns a type that may face more than
  one way to its next facing (`next_build_facing`), playing MORE; the
  footprint's sides swap for east and west, the facing's letter shows at
  the site, and "Press /, or Alt+wheel, to rotate" until the player has
  turned one (`rotate_key_discovered`). The site is tested turned to the
  facing, and the build order carries it (`issue_mobile_build`), so the
  building rotation rule places the building that way. A type whose preview
  keys face its opponent is drawn, though not built, turned toward the
  nearest enemy (`opponent_facing`) while the site lies within build
  distance of one of the player's selected, finished units: the first unit
  of each player who is in use, no watcher and not allied, the nearest
  faced east or west when it lies farther off across than down, else
  south or north.
- **Share dialog and battle room buttons** (ui.share-dialog-and-lobby-buttons):
  a share panel whose GUI has them gets EN_SHAREMETAL, EN_SHAREENERGY,
  EN_SHOOTALL and EN_NOSHAKE switches set from the player's share switches
  and the console's flags, each click flipping the switch and giving its
  command (+sharemetal, +shareenergy, +shootall, +noshake) as a line shown
  here and run on the console (`give_view_command`); EN_READY says
  ".ready" to every player; SRL_SETSHRMETAL and SRL_SETSHAREGRY run over
  the stores with their knobs at the share thresholds
  (`share_threshold_knob`), SM# and SE# showing the threshold a moved knob
  stands for (`share_threshold_value`) when it differs from the player's,
  and OK gives +setsharemetal and +setshareenergy for each that differs;
  SRL_GAMESPEED, when a GUI has it, asks for its speed as the speed keys
  do. The battle room offers the host the AUTOTEAM, AUTOPAUSE, RANDOMTEAM
  and CRCREPORT buttons the profile lists (`lobby_button_bits`,
  `multiplayer_bind_lobby_buttons`) when its GUI has them, grayed for the
  other players; a press says "+autoteam", ".autopause", "+randomteam" or
  ".crcreport" as if typed.
- **Text** (ui.text-rendering): with `unicode`, game text holds UTF-8: the
  chat line and the whiteboard's text send what is typed as UTF-8, and
  well-formed UTF-8 in game text is read as its characters
  (`game_text_utf8`); without it, typed text goes out in the game's 8-bit
  code page, a character it lacks as `?`, and game text is read in the code
  page (`typed_game_text`, `oa::present::encode_game_text` and
  `decode_game_text`). With the accessible chat on, each line of the
  message log gets a black backdrop from 4 pixels left of the log to 4 past
  its text, a logo counted a line height wide, from the row above to the
  row below (`chat_backdrop_rect`), whatever the Game text background
  setting says; the line is drawn as all game text is. The chat line shows
  the input method's composition after its text, underlined
  (`oa::present::modern_text_underline`, `underline_typed_composition`),
  until it is committed, and
  Backspace in the chat line and on the whiteboard takes the whole last
  character.
- **Game text** (the Language settings, `text_style`): the match's
  text, what players type and send and their names are drawn as
  `oa/present/game_text.hpp` lays them out (`runtime_game_text.cpp`): with
  Use modern fonts for game text, in the bundled fonts through FreeType
  ([text font](../platform/text-font/README.md)) at the Text size of the game
  fonts' sizes, times the text's scale, in their colours, with the outline,
  shadow and background the settings choose, reduced to the palette
  (`install_game_text_hooks`, `paint_modern_text`), the shadow over the
  Full tier's battlefield drawn by the card; without it, in the
  game's fonts, with Latin-1 drawn with the glyphs the fonts hold and the
  rest in the modern fonts at the fonts' own size. `paint_text` (the
  labels, the clock, the frame statistics), `overlay_gui_text` (the message
  log, the status readouts) and `draw_board_text` (the kill board) draw
  this way; menu and dialog labels, and the loading screen whatever the
  settings say, keep the game's fonts for every character they hold.
  Over the battlefield the text grows with the size: the message log steps
  by its font's height at the size (`message_log_step`), breaks a line
  wider than the battlefield into rows (`message_log_rows`) and lets the
  oldest lines go while its rows do not fit above the clock or the
  battlefield's bottom (`message_log_most_rows`); the clock rises with its
  height (`console_clock_pen_row`); the chat line is drawn at the size in
  the TALK field, showing its end as it grows wider, and rises over the
  battlefield in a black box across it when it is taller than the field and
  two rows above and below it (`chat_line_layers`, `draw_risen_chat_line`). The fixed panels paint
  their text within a `PanelText`, which holds it to the game fonts' size
  on their baseline (`painted_text_size`, `painted_baseline`): the top and
  bottom bars, the build captions, the digits under and over a unit's bar,
  the kill board, the resource panel and its clock line, the frame
  statistics, the debug keys' line, the commander placement's prompt and
  an extension's overlay. The unit panel's `PanelText` also names the
  bottom bar's top row, so that its status line, whose ideographs would
  rise above the bar, is lowered to keep within it (`panel_text_drop`);
  the unit's metal and energy figures under it then move down as far and
  one outline further, clear of its outline and shadow. While a line's box
  or the log's backdrop shows (`text_backings_shown`), the bars' text and
  each overlay's over the battlefield are painted within a `BackedText`
  group (`runtime_match_render.cpp`): each box is laid where its line is
  drawn and the lines, in either font, once the group ends
  (`hold_painted_text`, `oa::ui::hud::TextHold`), so that no box covers
  another line of its group; a fading clock's lines are painted at once
  (`paint_faded`).
- **Language** (`runtime_language.cpp`, `language_state.hpp`): the
  language the game shows its text in ([docs/languages.md](../../docs/languages.md)).
  `start_language` asks the operating system for its preferred locales
  (`oa::platform::locale::preferred_locales`), reads the interface
  catalogue's files from the `languages` folder beside the game's other
  files and the setting, and `apply_language` puts the language in effect:
  3.1c's command-line word, else the setting, else the system's. It loads
  `gamedata\translate.tdf` and the fonts for the game data's word
  (`game_language`, "German"), and installs the lookups every interface
  reads: the game's own texts through `game_translation`
  (`translation_hook`, which the gadgets, dialogs, menus, the kill board,
  the F1 panel, the message log's phrases and the loading screen take, and
  `oa::data::languages::set_translation_hooks` for the GUI files and the
  campaign's files), units' names and descriptions from the table the unit
  loaders fill through `unit_text_sink`, and the engine's own words in the
  catalogue. `set_language_choice` takes the setting as it changes, and
  `show_language_change` shows it at once where the settings show over:
  the main menu loads again in it (`reload_main_menu_language`), and in a
  match the in-game menu opens again, its texts translated and the
  captions over its pictures drawn afresh; the titles over the battlefield
  and the chat line's panel load again as they next show. The match's
  definitions, saves and what a shared game sends never see it.
  `--check-unit-language TAG` (`runtime_language_check.cpp`) checks the
  build menu's bottom bar and the unit panel in a language against the
  unit files themselves, and English after it; the
  `native-unit-language-*` checks run it in each language and in the
  system's. `--check-language-switch` (`runtime_language_switch_check.cpp`,
  `native-language-switch`) changes the language in the settings from
  Simplified Chinese to English, German and back, over the main menu and
  over a skirmish's in-game menu, and checks each screen shows what it
  shows opened again in the language, that the stack holds each face the
  pack brings, and that in Chinese every caption over the match's
  pictures shows each of its characters, none as the missing-glyph box
  (`picture_captions_missing_glyphs`). `--check-language TAG` starts it in
  another installed pack's language (French takes the place of English or
  German when that is the one named), whose pack may bring no face and no
  captions. `--check-text-fit` (`runtime_text_fit_check.cpp`,
  `native-text-fit`) takes the same option, once for each language, to
  check those in place of its own six (`native-text-fit-language`).
- **Picture captions** (`runtime_picture_captions.cpp`): while a language
  other than English is shown, the words its packs' `pictures.tdf` gives
  (`language_pictures`, drawn by `oa/present/picture_captions.hpp`) are
  drawn over the pictures they name as each is loaded: the GAF files the
  runtime reads (`append_gaf_file`), the match bar's `commongui.gaf` and
  side panel, a screen's sprites and the named backgrounds. They are drawn
  in the faces game text is drawn in: the bundled fonts and the language
  packs' faces, which both stacks take through `follow_language_fonts`,
  again whenever the language's faces change, forgetting the lines drawn
  before. A picture the
  game data holds in the language's own folder (`bitmaps-<word>`,
  `anims-<word>`) already shows its words and is left as it is, as is
  every picture no pack names.
- **Options dialog** (ui.options-dialog): Ctrl+F2 in a match opens the
  settings dialog as its mod options kind beside the in-game menu
  (`open_engine_settings_in_match`), over the player's view settings
  (`dialog_options`), with the snap radii the profile gives no room locked
  (`dialog_option_locks`). Changes take effect at once
  (`apply_dialog_options`), OK keeps them in the preferences, Cancel puts
  back what it opened with; the builders' options apply from the next
  game. F11 in a match gives each line of the chat macro
  (`chat_macro_lines`) as a line shown here only, a line starting with '+'
  also run on the console. Not in the dialog: the whiteboard and full
  screen map keys, the build menu's facing overlay, VSync and editing the
  macro, which keep their stored values.
- **External exports** (ui.external-exports): live unit state and lobby
  state are not published for outside viewers and lobbies; publishing them
  has no effect on the game, so the profile's parameters are accepted and
  change nothing.
- **Player settings** (`ViewSettings`, `read_view_settings`): the keys,
  snap radii, builders' patrol and guard options, chat macro and display
  switches the options dialog changes, kept under the preferences' Eye
  section; the builders' options go into each match's rules while the
  profile turns them on (`apply_builder_options`).

The profile's visual rules (`ModProfile::ui`, read through
`Runtime::ui_rules`, with the player's Developer Mode overrides as they are
now; a default record without a profile) change only what this machine
draws and how its pointer and keys select. Their logic lives in the
HUD and selection modules (`oa/ui/hud/resource_panel.hpp`,
`shared_views.hpp`, `whiteboard.hpp`, `megamap.hpp`, `unit_labels.hpp`,
`oa/sim/selection/shortcuts.hpp`) and the runtime wires it in:
`runtime_view_panels.cpp` draws the resource panel, the clock, wind and
tidal line and the shared camera rectangles, and switches a watcher's view;
`runtime_whiteboard.cpp` takes the whiteboard's pointer, keys and text and
makes and applies its batches (`take_whiteboard_batch`,
`receive_whiteboard_batch`); `runtime_megamap.cpp` opens, draws and clicks
the megamap and redraws the enhanced minimap, while the Mouse wheel zoom
setting is off (`megamap_on`, `megamap_wheel_zoom_changed`), its clicks
acting as the battlefield's do in the chosen interface type, a drop of the
selection closing the unit info panel, and a panel over it, such as the exit
menu, taking its own; a press takes the icon last drawn under it
(`MegamapState::unit_at`), so a finger's tap needs no frame between. The
touch controls' taps, order wheel and drags on it are its clicks too. That
is what `native-megamap-clicks` checks (`check_megamap_clicks`) after what
it draws (`check_megamap_picture`);
`runtime_selection_shortcuts.cpp` takes the double-click, Ctrl+S/B/F and
the drag filters. What other machines report about their players' views
(`shared_views_`) is left to network play to carry; a game played alone
reports and receives nothing.
Network play carries the whiteboard's batches: each frame `net_frame` sends
the batches drawn here (`net_match_send_whiteboard`), and the match hands a
batch an allied player's recorder sent to `receive_whiteboard_batch`
through its `whiteboard_marks` hook.

The overlays are painted after the fog, on the world layer or the HUD's,
so the accelerated tier presents them as it presents the game's own
painters: 1:1 over the area pass's picture, or as the overlay laid over the
magnified scene; in the Full tier they are painted on the overlay canvas
and laid 1:1 over the battlefield the card draws, at every zoom down to its
floor of a sixth. Those placed at map points take the frame's viewport for
those painters, moved by the offset of a view drawn between map pixels:
the build tools' sites, the build preview's facing letter and the
whiteboard's marks (`draw_whiteboard`), whose pointer, like the commander
placement's, finds the map pixel drawn under it (`battlefield_map_point`).
The build preview's model is drawn into the scene at the draw scale, as
every unit is, and in the Full tier by the card's model stage, which draws
an unfinished mobile unit's nanoframe over every piece where the profile's
interface fixes hold its moving pieces until it is built
(`ModelFrameInputs::moving_pieces_once_built`). The canvas holds no
battlefield, so game text in the modern fonts paints there only the
pixels its letters cover whole and leaves the rest to the card
(`paint_modern_text`, `paints_full_canvas`): its shadow as a darkening and
its letters' partial coverage as their colour at that coverage
(`paint_world_blend`), and its outline as a hold of each channel to the
outline grey at the most (`paint_world_minimum`, `card::Blend::minimum`),
which is black over black ground, as the processor's outline is, and the
grey over ground lighter than it. A renderer without the minimum blend,
SDL's software renderer among them, draws the outline in the grey itself.
The megamap covers the
battlefield it opens over, so a frame under it draws at the zoom in the
accelerated tier too (`megamap_shown`, `world_scaling`) and the megamap is
presented as the processor composes it, never through the card's
magnification of the scene it hides; in the Full tier it covers the
canvas. `native-render-tiers-visual-rules` runs the render tiers check
under a profile that turns the visual rules on and checks these overlays
in the standard and accelerated tiers, and in the Full tier after its own
cases (`check_visual_rule_overlays`), with the megamap opening only while
the Mouse wheel zoom setting is off and closing when it is turned on.

## Mutators at the start of a match

Every match start applies the mutators the run chose, after a pack map's
files are mounted and before the match reads its game data. The command
line's `--mutator` values are that choice, in the order given. A run that
names none applies nothing, mounts nothing and logs nothing. The pipeline
leaves its layer mounted for the match. The log names each mutator that
applied, how many files were added, hidden or rewritten, each conflict,
and each mutator that did not apply. A list the manifests refuse logs one
`mutators: refused:` line and nothing about each mutator. The applied line
is written only when a mutator applied.

A mutator that does not apply stops the start, except one that failed
alone: its hacks were refused, or it was left out only because a mutator
it needs failed that way. A list the pipeline refuses stops the start
too. The message names the mutator that stopped it, by the name in its
manifest and by its id, and why. The layer is released before the
failure leaves the start, and again when the match ends. A start that
fails by an exception after the mutators were applied releases the layer
too, so a campaign, a save, a network game and a replay leave none
either. The next match applies the list chosen then, including none.

The scripts' seed is the seed of the mutators that applied. When the
start leaves a mutator out and still goes on, it runs that list again
with only the mutators that applied, and the match plays the second
result. It logs one line, `mutators: seeded again from the mutators that
applied, because <ids> did not apply`. A second run that does not apply
every mutator it was given stops the start, and the layer is released.
A list that applied in full is run once.

The last set each setup started a game with is kept in the preferences:
one key for skirmish, one for the multiplayer host and one for a campaign.
A setup's START writes its key through `start_with_mutators`, and only
when that game has mutators. An empty loadout remembers nothing; the
next matches play the run's own list (`--mutator`'s or a launch file's),
normally none. `--mutator`, a launch file, a replay, a
saved game, a guest's game and a game without mutators do not write it,
and CANCEL does not erase it. While a match with mutators runs,
`mutators-sentinel.conf` beside the preferences file names them and the
setup that started the match, or none when no setup did. The match start
also writes `open-annihilation: mutators in play:`, naming each mutator.
A clean match end (`teardown_match`) or a clean exit deletes the file. A
fatal error that unwinds leaves it. The next start keeps what it names
(`take_mutator_crash`) and deletes the file. `--no-mutators` refuses every
mutator for the run: a list `--mutator` or a launch file names is logged
once and not applied. `remembered_mutators` returns none and
`start_with_mutators` remembers nothing. The crash record is still read.

## Extensions

`extension.hpp` is the table of hooks through which libraries linked into
`oa-game` extend it: long options and game switches, start-up and
shutdown, screens, Settings pages, the frontend's entry (its game name and nickname), run
modes, per-frame work, match events, the Pause key, the speed keys and
the GAME slider, the frontend's application modes, the loading's
progress, the team panels' host (a tournament game withholds CONTROL),
requests to close the window, the label a match's return names in its
menus, whether the preferences keep the stored password, recordings to
replay, console commands and checks. Each such library is an extension.
The extension API's chapter for an extension's author is
[docs/extensions/README.md](../../docs/extensions/README.md), and the OA UI
kit's for extensions [docs/extensions/ui-kit.md](../../docs/extensions/ui-kit.md).
The engine registers its own, network play ([below](#network-play)) and
the automation endpoint ([below](#the-automation-endpoint)); a project
that builds the game registers further ones after adding the engine, each
with the function that fills its table:

```cmake
oa_add_extension(<target> INIT <function> [SWITCHES <letters>] [GAME_FILES <COMMAND ...>])
```

`cmake/OaExtensions.cmake` describes the arguments. When the configure
ends, the engine lists the registered extensions, each after every
registered extension its library links and otherwise in registration
order, compiles the list into `oa-game` and links the libraries.
`main()` has every extension fill a table of its own before the command
line is parsed, and `ExtensionList` (`extension_list.hpp`) combines the
tables into the one table the runtime calls, by the rules `extension.hpp`
states: most hooks are called for every extension in list order;
`shutdown` and `release_runtime` in reverse; the hooks that take something (an option, a
switch, a run, a close request, a recording) ask the last extension in the
list first, since it builds on those before it; answers are combined; and
`frontend_game`, `frontend_states` and each entry of the hosts the
extensions fill belong to one extension at most, so that a second one
stops the start or the call with a message naming both. Every hook no
extension fills keeps the engine's behaviour. A reserved game switch no
extension takes is refused as "not handled by this build". An extension that includes `runtime.hpp` builds against
`oa::extension-sdk`, the include directories and libraries an extension
may use; one that needs only the table links `oa::app::headers`. The
engine calls every hook through one guarded call, `call_hook`
(`hook_call.hpp`), which catches what the hook throws before it reaches
engine code; `hook_call.hpp` lists each hook's handling, which
`extension.hpp` documents: the error is raised as the engine's own at the
call, which ends the game except where a match start the frontend falls
back from catches it; or it is reported and the engine carries on as for
a null hook; or the hook must not throw, and is reported when it does.
`hook-calls` (`tools/check_hook_calls.py`) fails when engine code calls a
hook pointer itself, and `app-hook-call` tests each handling with hooks
that throw. `OA_EXTENSION_API_VERSION` in `extension.hpp` numbers
the table's contract; an extension checks its typed copy,
`oa::app::extension_api_version`, with `static_assert`, and any change to
the contract raises it (its comment says what counts). The recorder test
extension checks it too, beside its count of the table's hooks.

Version 9 adds `open_recording`, through which `--generate-script` and
`--render-script` replay the recording a director script names. The
engine hands the extensions the recording's name and bytes
(`RecordingInput`), asking the last in the list first; the one that
replays recordings of that kind starts the recording's match through the
engine's own match start and returns what the recording holds
(`RecordingInfo`: the tick after its last, when known, its length, the
player it is watched from, how many players it holds and whether the
installation's unit definitions differ from its own) and the replay's
hooks (`ReplayHooks`). The engine then runs none of that match's ticks
itself: it calls `step` once for each tick, `status` for where the replay
stands (`RecordingStatus`: the tick, whether everything recorded has been
replayed, whether the replay is clean and its periodic records paced, its
errors and the last one's text) and `close` once, before it tears the
match down. An extension that does not recognise the recording declines
and the next is asked; one that recognises it but cannot replay it
throws. Each extension asked starts from a zeroed replay and information,
and only the one that takes the recording fills the caller's.

Version 11 adds `release_runtime`, through which an extension frees what
it keeps for one runtime. The engine calls it as each runtime is
destroyed, the game's own and the second one a check creates beside it,
also when the runtime's constructor throws, at the point in the runtime's
teardown where its match is still whole; the runtime is passed only to say
which one goes. Network play keeps its state for each runtime this way.

Version 12 adds `option_effect::remote_controlled`, the effect of an
option through which a program on this machine controls the run, as the
automation endpoint's `--fark` does. The engine sets
`Options::remote_controlled` for it, and the main loop then runs every
frame while the window is inactive, so that the extension serves that
program from its frame hook; without it the loop waits for events there.

Version 13 adds `FrameStage::presented`: the frame hook is called a third
time each frame, after the frame is drawn and shown, so that an extension
sees the frame the player saw. Network play does nothing there; the
automation endpoint answers its frame request.

Version 14 adds the functions below. An extension calls each of them; none
is a hook, the engine does not call them, and no extension fills them.

An extension calls `player_folder` with the runtime a hook was given, once
that runtime exists, and reads the player's own folder for the run: where
saved games, screenshots, films, recordings and mods are kept. The runtime
makes the choice after the preferences load, which is after `startup`,
`register_screens` and `ready`, so a call from those hooks reads an empty
folder. It is not a hook, and no extension fills it.

An extension calls `set_unit_limit` with the runtime a hook was given, once
that runtime's preferences are loaded, and sets the unit limit, the
configured limit a game uses, as a `SettingScope` says.
`SettingScope::immediate`, as a settings page the player uses does, sets
the player's setting and the next game's limit and writes the preference
(`open-annihilation.unit-limit`) at once. `SettingScope::next_game`, as for
a game mode that starts a game at a fixed limit, sets the limit for the
next new game (a skirmish this machine starts, or a multiplayer game it
hosts or joins, a joined one playing at its host's limit) and leaves the
setting and the preferences file as they are; a game that brings its own
limit (a saved game, a campaign mission, a recording, a restart) neither
uses nor ends it, and when the match it was set for ends the player's
setting is the limit again. `SettingScope::next_restart` sets the player's
setting, which Settings shows at once, and writes the preference; the games
of the running session keep their limit until the game next starts. A
value outside the range the setting keeps is clamped into it, as a stored
preference is. The preferences load after `startup`, `register_screens` and
`ready`, and that load replaces a limit set from those hooks. It is not a
hook, and no extension fills it.

An extension calls `keep_running_while_inactive` with the runtime a hook
was given to hold or release a request that the main loop keep running
every frame while the window is inactive. Held, the frame hook keeps being
called there, as it does for a live multiplayer game and for a
remote-controlled run; released, the loop waits for an event, as it does
otherwise. The request starts released. Holding it again while it is held
leaves it held, and releasing it while it is released leaves it released.
It is not a hook, and no extension fills it.
`native-running-while-inactive` checks both sides
([testing.md](../../docs/development/testing.md#the-main-loop-while-inactive)).

An extension calls `web_address_available` to ask whether this machine can
open a web address in the system's browser. The answer is no on a Steam
Deck in Game Mode, which has no browser to hand an address to; a Steam Deck
in its desktop session, and every other machine, answers yes.
`open_web_address` opens an http or https address there. Any other scheme,
a missing scheme, and an address whose body holds a space or an ASCII
control, is refused and is not handed to the browser. `app-web-address`
checks that through a stub opener
([testing.md](../../docs/development/testing.md#opening-a-web-address)).
The download check asks `web_address_available` before it offers OPEN THE
CHECK, and opens the page with `open_web_link` only when that button is
pressed. `native-challenge` checks the window, the recorded page, the
retry, the cancel and a check that waits out a skirmish.
Neither is a hook, and no extension fills them.

An extension calls `modern_text_chain`, `modern_text_layout` and
`modern_text_pixels` to read the modern text faces and their fallback chain
at a pixel size, to lay a line out and to draw it as coverage.
`message_log_text_size` is the message log's face at the player's Text size
and the screen's scale. `game_text_preferences_of` holds a text size to the
sizes the setting offers, and `game_text_preferences` reads the settings in
effect: "Use modern fonts for game text", including when the language shown
turns those fonts on, and "Text size". `set_focused_text_field` tells the
engine where the focused text field is, in canvas pixels, so the input
method's candidate window and the on-screen keyboard can stand clear of it.
The caret's distance from the field's left places the candidates beside the
caret; the game's own fields pass 0. `app-extension-text` checks the face's
rows at a size, that the settings written and read come back, and that the
field is handed to a stub
([testing.md](../../docs/development/testing.md#modern-text-for-an-extension)).
None of these is a hook, and no extension fills them.

An extension calls `read_game_file` with the runtime a hook was given and a
path, and reads that whole file from the game's files: the loose files of
the folders the runtime layers, then the archives it mounted. A loose file
wins over a file of the same path in any archive, a mod's archive among
them. Where several loose folders are layered, the first that holds the
path wins, and a mod's folder is layered ahead of the game folder. Where
only archives hold the path, the earliest mounted archive wins. Mount order
after the loose files is the revision archive, the ccx group, the ufo
group, the installation archives in the order a mod names them, at most ten
hpi archives, then the disc's archives. A missing file returns false, and
the call does not throw. `app-read-game-file` checks a loose file, a file
that only an archive holds, and a missing path
([testing.md](../../docs/development/testing.md#reading-a-game-file)). It
is not a hook, and no extension fills it.

An extension calls `set_extension_window_source` with the runtime a hook
was given, a context of its own and a source, and the endpoint then lists
that source's windows and controls ahead of the screen's own when a program
driving the game asks for them (`controls`, with each control's `window`).
A null source removes it. The same context replaces the source. The source
is called on the main thread when the controls are asked for, and must not
change the runtime. `extension_clock` is the clock the extension's idle
work follows. While the fixed clock is on it is that clock, the match tick
times one thirtieth of a second. In a run a program on this machine
controls it advances by that same step once a frame and does not read the
wall clock. Otherwise it is the steady clock.
`native-automation-extension-windows`, in a build configured with
`-DOA_RECORD_EXTENSION_HOOKS=ON`, lists a fixture window's controls, clicks
its button and reads that clock
([testing.md](../../docs/development/testing.md#the-automation-endpoint)).
Neither call is a hook, and no extension fills it.

An extension calls `simulation_hash` with the runtime a hook was given to
read the simulation hash the run plays under: the profile in play, with
every Developer Mode override that changes the simulation, or the plain
baseline when the game plays 3.1c. A running match keeps the hash it
started with. It is not a hook, and no extension fills it.

An extension calls `run_mutators` with that same runtime to read the
mutators whose digest `simulation_hash` folds in: how many, their ids in
order, and the simulation hash the run would have without them. That hash
is 64 lower-case hexadecimal digits and equals `simulation_hash` when none
play. Both calls read the profile and the mutator digest of that moment. A
running match keeps them until it ends. It is not a hook, and no extension
fills it.

Version 15 adds the OA UI kit (`src/ui/kit`) to the contract, so that an
extension's pages, screens, notices and prompts look, scale, focus and
answer as OA's own do. `register_settings_pages`, called right after
`register_screens` while each runtime is built, hands the extension the
runtime's `SettingsRegistry`: each page it takes (`settings_page_register`,
a `SettingsPageDesc` of preference rows bound to the extension's own keys)
is listed in Settings under a divider with its owner's name, after the
engine's sections, and saved, restored and reported to its `applied` as the
engine's pages are; a page the registry refuses stops the start with
"settings page registration rejected: <reason>". `register_screens` may
also register kit screens (`kit_screen_register`, a `KitScreenDesc` with
`lay_out`, `act` and `closed`) in the `ScreenRegistry`, beside its screens
and overlays. Three entries appended to `ScreenServices` reach the OA layer
from a screen's or an overlay's context: `open_kit_screen` opens a kit
screen by name, modal over the front end; `show_notice` and `ask_prompt`
copy a `NoticeRequest` or `PromptRequest` at once and queue it with the
engine's own, so two never show at once, a prompt's `answered` called once
on the main thread with the button pressed, or −1 when the runtime ends
with it still waiting. `read_string` with a null section reads a whole
preference key, as a page's row binds one. `MatchOverlay` gains
`kit_text_width`, `draw_kit_text`, `draw_chip`, `draw_progress` (−1
thousandths draws it busy) and `draw_hover_card`, which draw the kit's
components crisp at the HUD's scale in the game's fonts, clipped to the
battlefield. From this version the kit's public headers,
`oa/ui/kit/*.hpp`, are part of the contract: a change to one before 0.8.0,
the first release that carries version 15, keeps 15 and updates the
engine's followers and the test extensions in the same change; from that
release on, any change to one raises the version. The kit is provisional
all the same: it changes as the engine's screens need.

`app-extension-list` checks each rule of the combined table over two test
extensions, and `tests/extension/` tests the boundary itself.
`extension-layout-mismatch` links a unit that sees `Runtime` with members
`oa-game` does not have and expects the link to fail. A build configured
with `-DOA_RECORD_EXTENSION_HOOKS=ON` registers two test extensions after
network play: the recorder, which fills every hook with a recorder but
`frontend_game` and `frontend_states`, which network play fills, and adds
one `Runtime` member, and the follower, whose library links the
recorder's and which fills the same hooks; the follower is registered
before the recorder and listed after it. `extension-hooks-options` and,
over the installed game, `extension-hooks-game` check that the game calls
every hook of both but `disconnect_text`, which only a shared match
reaches (of `match_event`'s events they see `finished`, `torn_down` and
`results_released`), in the order the rules set, and that a follower that
fills `frontend_game` beside network play stops the start; the navigation check's Pause
key reaches `pause_changed`, its speed keys `speed_changed` and its menus
`app_mode_set`, a skirmish's loading `load_progress`, `team_panel_host` and
`return_label`, its preferences write `keep_stored_password`,
`--check-match-dialogs`'s close requests `close_requested` and its GAME
slider `speed_changed`, a `--generate-script` run over a file no
extension replays `open_recording`, asking the follower first, and the
end of a headless skirmish `release_runtime`, the follower first. The
recorder registers a Settings page of its own from
`register_settings_pages`; with `--record-refused-page` it gives that page
an id the registry refuses, and the start stops with the registry's
reason.
With `--record-quit STATUS` the recorder keeps the screen services an
overlay is given, stops the sounds, plays BGM on the alternate route,
asks for a frontend pass and ends the run through `quit`, which must exit
with STATUS. The recorder drives the check host too, through its entries
alone: after network play's `--check-multiplayer-menu` check it clicks
MULTI twice, taking it over each time (`select_multiplayer` answers
`taken`) so that the main menu stays up, with the cursor, the clock, a
composed frame, a sound's file and the preferences checked on the way; and with `--record-check-host` it takes the headless run for the
entries that work without a window, down to a close request, which ends
the run, and a frame, which needs the window. The navigation check itself puts probes in place of the
hooks to check what the engine does with their answers: a close request
answered or declined, quit's status, one frontend pass for two requests,
a query binding, the launch's nickname, the return label kept as a match
starts and quit leaving a match, with the preferences open over it,
first. The same configuration registers a third test extension, the Kit
fixture (`kit_fixture.cpp`), which uses the OA UI kit through the table
alone: a Settings page, a kit screen, a prompt and the overlay's kit calls.
`extension-kit` (`tests/extension/check_kit_fixture.py`) drives it through
the automation endpoint in 640x480, 1280x720 and 1920x1080 windows, and
finds its chip in a headless match's frame
([testing.md](../../docs/development/testing.md#the-kit-for-an-extension)).
CI builds that configuration as a job of its own, but has no game
installation: there `extension-hooks-game` and `extension-kit` skip, and
only the option hooks and the hook list are checked. The rest of the hook coverage runs
only where `OA_GAME_DIR` is set; run it there before changing the table.

A check an extension runs, from `run_mode` or `check_multiplayer_menu`,
drives the running game through the check host (`check_host.hpp`), as the
engine's own checks drive it: `check_host(runtime)` returns a table whose
entries hand the game SDL events and left-button pointer events at canvas
points (placed in the window as the game shows the canvas, or taken as they
are without one), run a frame of the main loop (with the window up) or a
pass of the screen packages, compose the frame and return it, hold the
frontend clock at a tick and release it, and read the cursor, the window's
SDL id, the screen shown, the frontend's state, whether a package owns the
main menu's frame, a gadget of the shown layout by name, the game's files,
the file a sound name plays, and write the preferences.
`runtime_options(runtime)` returns the command line's options. The header
includes no other engine header, so such a check needs only
`oa::app::headers` and never a private name of `Runtime`. The check host is
not part of the extension table, which `OA_EXTENSION_API_VERSION` numbers
alone.

The automation endpoint reads what the check host does not through the
automation host (`automation_host.hpp`, `runtime_automation_host.cpp`):
`automation_host(runtime)` returns a table whose entries read the
preferences as the game holds them now and the file they live in, the
running match's Game block, its world and the digest of its state a saved
game carries, and the controls of the built-in screen shown or the match's
panel, with the dialog over them (names, kinds, places on the canvas,
state, text and a list's rows); through which the endpoint holds keys and
pointer buttons down for the game's reads of what is held
(`device_state.hpp`), since the input it hands the game comes through
SDL's event queue alone; and which have the game copy each frame it
presents, just before it shows it, into a surface the endpoint keeps,
until the endpoint stops it. Like the check host it is not part of the
extension table.

An extension reaches `oa-game` only through these hooks and declared
headers. When it needs something the table does not offer, add a hook or
declare a header for it here; never give it a new `Runtime` member or
friend, or another of `Runtime`'s private names.

Network play adds no members to `Runtime`. Its state for one runtime, the
network session, the replay of a recording and the match report, is its
own: `NetworkPlay` (`netgame/network_play.hpp`), which network play's
extension creates the first time a hook is called for a runtime, finds
again for each later hook and frees through `release_runtime`.
`NetworkPlay` is still a friend of `Runtime` and reaches some of its
private names until hooks and declared headers cover everything:
`tools/check_runtime_surface.py` (`netgame-runtime-surface`) holds the
private `Runtime` names network play uses, each with its number of uses,
to `tools/runtime-surface-baseline.json`, which may only shrink. The
engine's `runtime-surface-names` test fails when a change to `runtime.hpp`
leaves the baseline naming something that is no longer a private name of
`Runtime`. Renaming one of those names replaces the old name with the new
one in the baseline, keeping its uses, in the same change: the one addition
the baseline takes. Removing one, or making it public, drops it.

One extension that is not part of the engine may still add members to
`Runtime`: the build names its header in the `OA_RUNTIME_EXTENSION_MEMBERS`
CMake variable, and the engine compiles `oa-game` and, through the SDK, the
extension with that definition; `runtime.hpp` includes the header inside
the class. That header declares the extension's own friend of `Runtime`,
under a name other than `NetworkPlay`, which is network play's: two
definitions of one friend in one game do not link as two. The check holds
such a header's declarations, which must be plain declarations with no
preprocessor directive, to the same kind of baseline. A unit compiled
without the definition, or with it where `oa-game` has none, fails to link
(`extension_members.cpp`). In the engine's own tree only the recorder test
extension (`tests/extension`) uses it, beside network play, with its
friend `RecorderExtension`.

## Network play

`netgame/` is network play's extension, `oa-app-netgame`, which the engine
always builds and registers. It takes the 3.1c network switches
`-b -c -e -h -n -p -t -y` and the launch: `-c FILE` or `--launch FILE`
(an `.oalaunch` file or TA Forever's, told apart by what it holds), a
launch piped in on standard input, and an option for each launch setting,
made from one table (`oa/app/netgame/launch_file.hpp`) that also makes
those options' part of `--help`.
[Command Line and Game Launching](../../docs/command-line.md) is every
option and way to launch; `netgame-launch-help` checks that page against
that table and against `--help`, and reads every launch file it shows. It
binds the network session (`src/netgame`) to the
multiplayer screens (`src/ui/frontend-multiplayer`), launches the
match from the battle room and runs it over the network, and replays
recorded games (`.tad`, `src/formats/tad` and `src/session/demo`) through
`open_recording` and `--play-demo FILE.tad`. Its long options include
`--net-loopback-check N`, which hosts and joins a match in one process
over 127.0.0.1 and compares both worlds after N ticks (when the run
chooses mutators, the unit sync is read over the shared mutator layer,
which is taken off before the launch mounts the same bytes again; the
check creates no battle-room state, so the launch plays the run's own
list),
`--check-recording-hook` and `--check-host-not-found`, and `--host` and
`--join ADDRESS` (with `--player-name`, `--game-name`,
`--game-password` and `--dplay-guid`), which have the multiplayer screens
host or join a
TCP/IP game at once, each screen doing what a player would
(`multiplayer_bind_direct_game`), and print the battle room's progress;
`--check-multiplayer-menu` runs its check of the multiplayer screens.
A transport named with no game to host or join (`-n`, or the `-c` file's
`connect`) takes the main menu to the multiplayer screens at once, as in
3.1c: each frame asks for a frontend pass until a multiplayer screen shows
(`transport_frame`), and the screens take the transport, so `-n1:ADDRESS`
alone lists the games at that address for the player to choose.
A launch's player and game names keep 16 characters and its password 10,
the battle room's and the session's own limits (`kPlayerNameLimit`,
`kGameNameLimit`, `kPasswordLimit` in `launch_file.hpp`, which
`launch_file.cpp` checks against those fields): a longer one is refused,
and TA Forever's file keeps the first ones.
A launch file the system opens in the game (`opened_launch.hpp`) is read
as it arrives and waits for the game to take it. One that arrives before
the runtime starts is the start's launch, exactly as `-c FILE` makes it:
`main.cpp` starts SDL's video without the window and pumps its events, so
that the file macOS hands a game it starts for arrives then, and before
the game folder is resolved puts the launch in place
(`put_launch_in_place`, the public entry point that puts a plan in place
after the command line was checked), adopts its content and skips the
intro; the map check then runs before the runtime as for `-c`, and a
refusal ends the start with status 2. A launch, from the command line, a
pipe or an opened file, keeps the window closed until its content and map
are found installed, and `--list-content --json` opens none at all and
writes its listing to the standard output the process was started with,
whatever takes the log; a refusal after the status stream's hello writes
its exit line, reason `refused`. One that
arrives while the game runs is network play's frame's
(`follow_opened_launches`): from the main menu, settled for two frames
with no dialog and no multiplayer signal waiting, the launch is put in
place, made active, the direct game bound and the main menu sent to
multiplayer, as `-n` sends it, so the battle room opens on the file's map,
rules and seats; the single-player menu goes back to the main menu first.
During a match the match's message log says the file was not opened; while
a game is being set up, on any other screen, while a launch is already
under way, for a file the battle room cannot use, and for a launch whose
content (mod, base game or expansions) differs from what the game plays,
a notice says so, and nothing is put in place. In the intro, a loading
screen or a change of screen the file waits. A late launch's status group
has no effect. `--check-opened-launch start|menu FILE` opens FILE as the
system would, a drop of the file, as the game starts, before its window
opens, or once the main menu has settled (`native-opened-launch`).
MULTI on the main menu asks the extensions (`select_multiplayer`), and
network play answers by moving the frontend to its multiplayer states;
when no extension answers, MULTI does nothing. Over game data with no
multiplayer map, such as the 1997 demo's, MULTI asks no extension and
shows the missing-content notice, and `--check-multiplayer-menu` checks
that notice instead.
Extensions built on network play use its own table,
`oa/app/netgame/extension_api.hpp`, numbered by
`OA_NET_EXTENSION_API_VERSION`.
Network play binds the profile's network, setup and team rules and the
battle room buttons its display rules add to the multiplayer screens and
the session (`bind_profile_rules`), and binds them again in the first frame
they differ from those bound (`follow_profile_rules`), so that the overrides
Developer Mode lays over the profile reach them; the profile the rules are
played by is one object for the whole run, which each change is copied
into, so that what is bound to it stays valid. It binds the engine's line
to the battle room (`multiplayer_bind_engine_banner`): `[Engine: OpenAnnihilation v<version>]`,
with ` DEV MODE` before the bracket while `Runtime::developer_mode` says
Developer Mode is on, and then ` REMOTED` while a program on this machine
may control the game (`Options::remote_controlled`, read through
`runtime_options`), which the battle room says as the local player's chat
line as it is entered and again each time the line changes there, a
deliberate difference from 3.1c.
The setup blocks OA sends carry no presence: the five bytes after the
engine signature stay zero, as 0.7 sent them, in the battle room's blocks, a
computer player's and the match's. OA machines know each other by the
signature, 'O' and 'A', which 0.7 already sends. What a machine plays
travels only in the presence record (`oa/netgame/presence.hpp`, type
0xf0), which the battle room sends to the OA players of other machines:
this build's version (`0.8.0`, with `-` and the version label after it in a
development build), the platform and architecture, the mod (its id, name,
version and packaging revision) and where it was published, the sim hash,
the game and view hacks that are on with their counts, Developer Mode with
its overrides, whether the machine can fetch content, and on the host the
map pack of its selected map.
`follow_presence`, run every frame, reads the facts from `presence_facts`
(`oa/app/presence_facts.hpp`, which `--check-simulation-hash` checks) while
the multiplayer screens show, writes the record within 1024 bytes less the
491 a map pack may take, so that the host's pack always fits beside it, and
binds it to the battle room when its bytes change
(`multiplayer_bind_presence_record`). A fact that breaks the record's rules,
such as a mod name over 64 bytes, is left out with one line on the error
stream the first time. Where the mod was published comes from the
installer's origin record in the mod's folder (`.oa-origin.yaml`, read with
`package_install::read_origin`): only a catalogue origin gives one, with
its registry, release and SHA-256, and the registry's descriptor URL from
the registries in effect (empty for a built-in registry or one added from a
file); a mod installed from a file, or a folder without a record, sends
none. The record is read again only when the mod or its folder changes. The
host's map pack comes from `LobbyMaps::pack`, which the map packs bind
through `multiplayer_bind_map_pack`; the battle room asks it once each time
the selected map changes, and until it is bound no record names a pack.
Network play binds it from the map packs' source (`pack_map_source`, whose
`pack` answers for the pack map whose files are mounted, on the host the
selected map's): the pack's id as the key, the release and SHA-256 of its
folder's origin record, the registry, the address the registry's descriptor
was read from (empty for a built-in registry, one added from a file and one
no longer in effect) and the pack's name and version. A pack installed from
a file has no catalogue origin and is named by no record. Whether the
machine can fetch content is the record's content field (tag 0x30, one byte
of flags, bit 0): `PresenceFacts::fetches_content`, true when the content
service has a downloads folder and at least one registry that is on,
allowed by Developer Mode and whose downloads are on (its install ID not
off where it needs one). A setup block carries none of it.
The battle room sends the record to every human player whose own setup
block carries the signature (`presence_peer`), on joining, when the record
changes and once to each such player who arrives later, from the local
player to that player alone and alone in its frame; never to a computer
player, to everyone at once or to a machine without the signature, so no
3.1c machine receives one. OA 0.7 machines carry the signature too: they
receive the record and lose nothing, since a frame that holds only a 0xf0
gives a reader without presence records nothing, and the frames after it
arrive as before. Each machine keeps the record each OA player sent it
(`lobby_presence_record`) until that player leaves. `--net-loopback-check`
checks that both machines hold each other's record, naming this build's
version, and prints how many of the setup blocks each machine holds of the
other's players carry no presence bytes.
[docs/development/testing.md](../../docs/development/testing.md#network-play)
lists its tests.

The battle room builds the host's mutator list on each machine
(`follow_room_mutators` in `runtime_room_mutators.cpp`). The host's list
starts as the run's `--mutator` choice. A guest uses only the list the
host's records are carrying now, and only once every mutator of it has
arrived; it never builds from a list of its own or from a list it hosted
before. Joining a room and leaving the screens forget the previous host's
list, even when the next host's records carry none. The build runs on the
calling thread when the wanted digest or this machine's sim hash changes,
on the live mod data, with no map. It
judges the list as the match start does: the same hack check and the same
blocking judgement, against this machine's own Developer Mode. It does not
play the hacks into the profile, so the room's sim hash stays the
profile's. A mutator whose hacks were refused fails alone, and the field's
index, while built, names the first mutator this machine dropped for that
reason. The field's layer digest is the mutator digest of the applied
loadout and its layer. When the build leaves a mutator out, it runs the
list again with only the mutators that applied and with their seed, logs
the same `seeded again` line as the match, and the digest is that second
run, so the room and the match agree. The index still names the first
mutator the first run dropped. A second run that does not apply every
mutator it was given fails the room. A list that applied in full is
built once. A mutator dropped on every machine alike does not
hold START; two machines that drop different ones do, and the hold names
the earlier of the two positions. The same Developer Mode settings are
what make the machines agree. The room's layer is not the match's: leaving
the multiplayer screens releases it. A network start applies the list this
room built; a guest's start applies it only while the host's current
record names it. The start then puts the run's own choice back, so a
later skirmish plays that choice. A network start that never showed the
battle room, such as `--net-loopback-check`, has no room list and plays
the run's own choice, as a single-player start does. The loopback check
still builds that choice's layer before its unit sync, without a room
(`mount_chosen_mutator_layer`, which builds as the room does), and takes
the layer off before the launch mounts the same bytes
(`release_chosen_mutator_layer`). The presence record's mutators field
says what this machine built. The host's names every mutator and says it has each one. Whenever
that field goes out, the host's record also carries each mutator's clash
mode, one bit a position: set means Add and clear means Replace. The mode
is the player's choice for that mutator, or the manifest's `clashes` when
the player left it unset, or the host's `mutators.clash-default`. The
loadout digest includes those modes. A guest builds the layer, and the
match start plays it, with the modes the host sent, never with its own
default. A record that leaves the modes out is built all Replace; when the
host's mode was Add the digests differ and START stays unavailable. A guest's
mutators field carries the host's digest, how much of the list it holds and whether each
package is installed here, and it names no mutator. While a guest is
behind, the host's record still leaves room for the map pack, and one
continued part of the list follows the pack. A guest that has not built
the host's list keeps READY off. START stays unavailable while mutators
are on and an OA player of another machine lacks one, failed one, is still
receiving the list, built another layer or plays another sim hash. It also
stays unavailable when the host has dropped the list and an OA peer's
record still carries one. When the layer is built or cleared, the battle
room reads its unit headers again and restarts unit sync: the host keeps a
3.1c peer that had finished and greets the OA peers again. The first time
a peer's record says it built the host's current list, and the first time
that record drops the field after the host's list is empty, the host
greets that peer once more.

### GET MAP in the battle room

A joiner whose host is an OA machine playing a pack map gets the host's
release of the pack in one press (`runtime_get_map.cpp`). The screen
`get-map` of the OA layer, which is not modal, has no backdrop and is not
opaque, is kept on the layer while the multiplayer screens show
(`keep_get_map_screen`, from `OaLayer::tick`). Each frame it reads the
battle room (`multiplayer_battle_room_map`): the host's map name, read
within its field, the map pack of the presence record the host's player
last sent, and the MAPNAME label's rectangle. It shows only on a joiner,
only when the map's name is a pack map's, the host's record names that
pack, and this machine does not have the record's SHA-256: never for a base
map, a host that sends no record (a 3.1c host or OA 0.7), or a pack
installed at the host's release. A 3.1c player sees the missing-map line as
before. What it shows (`get_map_state` and `get_map_text`,
`oa/ui/frontend_multiplayer/get_map.hpp`) comes from the packs installed
(`local_map_pack`, from each pack folder's origin record), the registries in
effect and their merged catalogue, and the download queue:

| State | Line | Button |
|---|---|---|
| get | "{pack} {version} · {size} from {registry}" | GET MAP |
| add_registry_first | "Add {registry} to get {pack}" | GET MAP |
| other_release_get | "You have release {mine} of {pack}; the host plays release {theirs}." | GET MAP |
| other_release_gone | the same line | none |
| gone | "The host's release of {pack} is no longer offered by {registry}." | none |
| unplayable | the map list's sentence for a greyed map, such as "Needs Core Contingency, which is off. Turn it on in the Library." | none |
| downloads_off | "Downloads from {registry} are off (Settings › Downloads)." | none |
| downloading | "Downloading {pack} · {done} of {size}" | the download's bar |
| failed | "Couldn't get {pack}: {reason}" | GET MAP, to try again |

Before it offers the button, GET MAP asks the release's catalogue entry
whether this game can play the host's map (`catalogue_map_verdict`, read
as the map list reads an installed pack: the base games it supports, the
expansions it requires and the map's compatible rows, for the base game,
the expansions that are on and the mod played). When the entry says no,
the line is the sentence choosing that greyed map in the list would show
(`Runtime::catalogue_map_refusal`), unplayable, and nothing is fetched: a
joiner whose copy of Core Contingency is off, or who has none, is told it
is needed rather than downloading a pack the installer would refuse. When
the rows do not answer for this game, GET MAP fetches the pack as before
and the installer's check decides.

The words come from the interface's catalogue. The line stands under the
map name's box and the button, or the bar in its place, above it at its
right, drawn with the kit at the layer's scale; the 3.1c gadgets are left as
they are. A mouse presses the drawn rectangle, and so does a gamepad's
pointer. A finger presses the button when it is at least as near as any
battle-room gadget; a point inside the map name stays the map name's.
Automation lists the
button as `oa.get-map`, the line as `oa.get-map-text` and the bar as
`oa.get-map-progress`, with no dialog, as it lists the OA button, and only
while they show; on the multiplayer screens the endpoint lists the OA
layer's controls before the panel's.

Pressing GET MAP for a registry the player has not added opens the Add
registry window with the address the record names, which checks it at once
and adds the registry only on the player's ADD; GET MAP goes on by itself
once the registry is added and its catalogue lists the pack at the record's
SHA-256, asking for that catalogue once if it has not been read. Otherwise
it requires the catalogue's SHA-256 for the pack to be the record's, queues
that release (`queue_download`, reason `offered-in-lobby`), and, when the
mounted pack layer is another release of this pack, unmounts it. The queue
checks the file against that SHA-256 and the installer puts the pack in
place, replacing another release (kept as `.backup`); in a run nobody
watches it does so while GET MAP shows ([The Library](#the-library)).
Until the queue item ends the pack's maps are held (`hold_pack_maps`):
`prepare_pack_map` refuses them, so no folder is replaced while its layer
is mounted. Once the installed packs list the map, the battle room's own
selection of the host's map finds it (`maps_select` lists a pack map the
source has gained since the list was read, and a pack map mounted again is
read again, its hashes among it) and the map name stops flashing. Nothing
readies the player. A download that fails says why on the line
(`failure_reason`): a package the installer refused names the installer's
own reason, such as a map that does not fit the game, not only that it was
refused. One cancelled leaves GET MAP as it was. The log says what happened:

```
open-annihilation: GET MAP: offering to add the registry <registry> for <pack>
open-annihilation: GET MAP: downloading <pack> release <release> from <registry>
open-annihilation: GET MAP: installed <pack>
open-annihilation: GET MAP: couldn't get <pack>: <why>
open-annihilation: map pack <pack>: its maps may be mounted again
```

The host's presence card says whether this machine has the host's pack
(`PackState`): installed when a pack of its key has the record's SHA-256,
another release when one has another, missing when none has its key.
In the battle room the OA badge is the app icon, drawn by the OA layer at the
window's own pixels in the CD rectangle, with the rules dot crisp on it. A
run without a renderer keeps the badge the 640 by 480 frame draws. The card
lists the platform and architecture on a Platform line under Engine, leaving
out a field the record leaves empty and the line when both are, and the mark
by its title is the same icon.

### A guest's Mutators in the battle room

A guest sees the host's set as the room's own Mutators button
(`runtime_get_mutators.cpp`). The screen `get-mutators` of the OA layer,
which is not modal, has no backdrop and is not opaque, is drawn from the
room's own art (`brain_seats_room.hpp`) and kept on the layer while the
multiplayer screens show (`keep_get_mutators_screen`, from the battle
room's chat follow, once a frame). It shows for every guest in the battle
room. The host's own room leaves it with nothing to draw.

The button is the room's on and off button with the OA stamp, where the
host's stands: as large as Select Map and over it, its edges Select Map's,
with no widening, missing every control of the room and GET MAP's
rectangle while that shows. It reads "Mutators". Its text for automation
is "Mutators · off" while the host plays none, else "Mutators · N on".
Its lamp is unlit with no set, green while
this machine has built the host's list, and red while READY waits on a
mutator it lacks. A press, Return or Y opens the picker read-only on the
host's set: CLOSE leaves the set as it is, and nothing readies the player.
Resting the pointer on it, or the information key, shows the Mutators card
([Mutators](#mutators)), a missing one reading "{name}: not installed".
To its left, cut short to miss every control of the room, stands one
part, by the state of the first mutator of the host's list that this
machine lacks:

| State | To the left | The line (`oa.get-mutators-text`) |
|---|---|---|
| none | nothing | "Mutators:" and the host's list |
| get | GET MUTATORS, a button in the room's button art | "Mutators:" and the host's list |
| downloading | the room's bar (`draw_room_bar`), filling in its green lamp's light | "Getting {name}: {percent}%" |
| failed | GET MUTATORS | "{name}: {problem}", the problem a short phrase such as "unreachable" |
| gone | the line, in the room's label font | "{name}: not offered." |
| downloads_off | the line | "{registry}: downloads off.", or "Downloads are off." with no registry |

`waiting_for_catalogue` shows nothing to the left until that catalogue is
read, and a later item is not consulted. A guest that has built the host's
list shows nothing beside the button, whatever a catalogue says of the
releases. Pressing GET MUTATORS calls `fetch_host_mutators` and nothing
else. GET MUTATORS, when it shows, comes first in the row's Tab order, as
it stands left; the row joins the room's Tab order as the host's button
does, the room's focus marker showing on the part that has the focus.
While READY is held because a mutator is missing, "Ready once {name} is
here" sits where the host's START reason sits, in the room's label font.
It does not enable or disable READY or START.

Automation lists the button as `oa.mutators-view` (its text the words as
drawn, checked while its lamp is green), GET MUTATORS as
`oa.get-mutators`, the bar as `oa.get-mutators-progress` (its text the
percent), the line as `oa.get-mutators-text`, which holds the whole line
uncut, and the ready line as `oa.get-mutators-ready`, with no dialog, and
only while they show.

Every player's card, after Rules, has one row a mutator of the host's set.
The first is labelled "Mutators". "{item} ✓" when that player has the
host's release, or, in the lock colour, "{item}: not installed" or
"{item}: another release here". The host's own card marks every one ✓. A
player who lacks one then reads, after the content difference lines,
"Different from the host: no {name}." or "Different from the host: another
release of {name}.", and "The game cannot start until {player} gets it, or
the host turns it off." The rules dot on the OA badge already follows that
same per-player comparison.

### Fetching the host's mutators

A guest whose host plays a mutator this machine lacks can fetch that
release (`runtime_host_mutators.cpp`). What can be fetched is decided
without drawing anything (`mutator_fetch_state` in
`oa/ui/frontend_multiplayer/host_mutators.hpp`), from the host's list
(each mutator's id, version and the first eight bytes of its package
SHA-256), the release installed in the Mutators folder, and the
catalogues in effect. The list names no registry, so every registry this
machine uses is considered, in the snapshot's order. The battle room's
screen reads `host_mutators_fetch` and queues with `fetch_host_mutators`
when the player asks. The guest's screen draws the button, the line and
the bar from this decision. This path itself draws none of them.

| State | When |
|---|---|
| have | an installed package's SHA-256 starts with the host's prefix |
| get | a registry offers that release, and none is installed |
| other_release_get | another release is installed, and a registry offers the host's |
| other_release_gone | another release is installed, and no registry offers the host's |
| gone | no registry this machine uses offers the host's release |
| downloads_off | the registry is off, or downloads from it are |
| waiting_for_catalogue | a usable registry's catalogue has not been read yet |
| downloading | that release is on its way |
| failed | the last try failed, and says why |

`host_mutators_fetch` is empty on the host and while the list is still
arriving. It reads the list, the installed mutators, the catalogues and
the download queue, and it never starts a download. The battle room asks
every frame, so the Mutators folder is read again only when its last
write moves; the installer reads it again after each mutator it installs.
The first ask of a tick reads, and the rest of that tick reuse its rows;
the layer's tick forgets them with `drop_host_mutator_fetch_cache`. While
the rows are read, a line a second counts the reads, the ticks and the
most reads of one tick without a press. `fetch_host_mutators`
queues every row in `get` or `other_release_get`, and a press of it tries
a failed row again. A catalogue entry is refused unless its SHA-256 starts
with the host's prefix (`not_offered`); the queue then checks the file
against that catalogue SHA-256 (`queue_download`, reason
`offered-in-lobby`). The package installs as the Library installs a
mutator. A prerequisite is fetched only as its own row of the host's
list, at the release the host named, never as whatever release is newest.
A refusal, a failure or a cancellation is not tried again by itself. The
failure is kept in English, for the screen to translate. The log says what
happened:

```
open-annihilation: host mutators: read <reads> times in <ticks> ticks, at most <most> in a tick
open-annihilation: host mutators: downloading <id> release <release> from <registry>
open-annihilation: host mutators: installed <id>
open-annihilation: host mutators: couldn't get <id>: <why>
```

## The automation endpoint

`automation/` is the automation endpoint's extension, `oa-app-automation`,
which the engine always builds and registers after network play's. Without
`--fark` it does nothing: no socket is opened and no file written. With
it, a program on this machine drives the game through a loopback TCP
address and the automation protocol: it reads the screen shown, its
controls, the preferences, the frames the game presents, the running
match and the battle room, is sent events as they change, hands the game
keys, text, pointer and finger events through SDL's event queue as a
device's, and quits the game as a player closing its window does. The
endpoint is served from the frame hook on the main thread, never waiting,
and changes nothing of the simulation. While `--fark` is on, the battle
room's engine line ends in ` REMOTED`, so that every player there knows a
program may control this machine's game.
[automation/README.md](automation/README.md) describes the options, the
protocol and the tests, and [docs/automation.md](../../docs/automation.md)
the endpoint for those who write a program that drives the game.
