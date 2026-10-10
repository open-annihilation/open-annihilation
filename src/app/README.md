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

The game carries the Open Annihilation icon in three forms, which
`tools/make_icons.py` makes on macOS from `branding/open-annihilation-icon.png`
and which are committed beside it: the bundle's `open-annihilation.icns`,
which its Info.plist names; `open-annihilation.ico`, which
`open-annihilation.rc.in` compiles into `open-annihilation.exe` with the
version information that names it Open Annihilation; and
`open-annihilation-256.png`, which the build embeds (`window_icon.hpp`) and
start-up gives the window with `SDL_SetWindowIcon`; a failure there is
reported, and the game starts without it. The branding is not under the
project's licence (`COPYRIGHT`). `branding-icons` checks the icons' sizes,
and `app-window-icon` that the embedded one decodes.

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
Full match frame). Two cases switch the accelerated tier on and run
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

## Files

- `app.hpp`, `runtime.hpp`, `runtime.cpp`, `runtime_frontend_host.cpp`,
  `runtime_builtin_screens.cpp`, `screen_registry.*`: the screen registry,
  screen loading and the host of the frontend dispatcher. Lines are only
  ever added to `screens.inc`.
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
  units, shots and effects past the edges are drawn whole. The view may go
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
  corner at the zoom (`scene_origin`). The
  limits hold a view from the one they held last (`view_hold_`,
  `held_view`), so that a view past them goes no further from the map but
  moves back toward it at once; every frame holds the camera so
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
  digit and a model bridge of one pixel. A frame past the floor whose
  bridge would take more than `rendered_units_budget`, an eighth of the
  machine's physical memory, is a frame of dots too. A frame of dots of
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
  map (`ground_point_under`). In the Full tier the far frame's world layer
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
  it is aimed, and a choice changed in play (`check_zoom_limit_choices`); at
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
  models, the GAF frames decoded for the particles (each once a frame), the
  selection boxes' lines and the frame's statistics. Before them the frame's
  shadows are set from the zoom (`set_frame_shadows`, `shadow_fade.hpp` of
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
  made (`AccelerationFault::engine`), is not (`take_acceleration_error`). Screenshots, film frames, the
  load and save backdrop, the briefing's backdrop and the end screen keep
  the standard tier's picture of the same moment (`ensure_screen_world`),
  drawn again with the frame's counts of units drawn kept and the HUD's
  resource readout, which saves keep in `Game.resource_readout`, not eased
  again.
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
  option is the even tone within 2; the overlay canvas is painted exactly
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
  at its share (`pass_alpha`). The model stage's shadows, then the list's
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
  above, from its greyed cell where the map pixel under its drawn point
  lies in a cell out of sight (`cell_fog`), since the fog lays its tiles
  by map column and row alone, so a sprite at the fog's edge is wholly
  grey or wholly colour, and in colour under the dithered option; squares
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
  first, the palette's tables approximated as the design says: the shade
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
  replaces). Zoomed out, the shadows' alpha, and a feature's shadow frame's
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
  holds no picture.
  Full falls back to Basic, which falls back to the standard tier, each
  for the rest of the run (`drop_full`, `TierInputs::full_drop`, a kind
  for each cause): a call of the card's own that fails (`FullCardError`,
  the stages' `full::CardError`), or the failure `--render-fault card`
  forces, drops Full with the failing call struck against the driver as a
  `card` strike (`take_full_failure`), which the same failure in the next
  run on the driver records `full-unusable`; a frame the executor refuses
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
  the model stage against the processor's raster
  (`runtime_full_models_test.cpp`); `app-full-fog` the fog passes' quads,
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
  with saves listed or none.
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
  after it step as before, as in 3.1c. `app-match-clock` tests all of
  these, with frames across the turn from a clock just before 2^32
  milliseconds.
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
  another machine or catches up, on a check's fixed clock and for a film
  frame, and 1 again once the frame is drawn, so that every other drawing
  shows whole ticks. At 30 a second a frame counts its clock unit whole: at
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
  the last frame drew, the renderer: the tier frames are drawn in and
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
  tracked unit is drawn at one place of the screen on every frame; and
  that runs at 30 and 120 from 2 seconds before 2^32 milliseconds, where
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
  `settings Language`;
  `native-stage` checks it. A save/load run ends by
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
  cannot be switched on and off stays as it is. `at TICK` before any
  action keeps its line for that match tick, and the line
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
  (`draw_pool_`) that this conversion, the terrain fill, the fog and the
  battlefield's draws (`world_draws.hpp`) run their bands on:
  `--draw-threads N` (1 to 32), else the `OA_DRAW_THREADS`
  environment variable, else the pool's default, one thread on a machine of
  one or two logical processors and otherwise one fewer than the
  processors, at most four. With one thread there is no pool and every
  band runs on the drawing thread. The conversion's, the terrain's and the
  fog's bands are fixed by the rows; the battlefield's are one a thread, and
  any number of them draws the same frames, so every count draws the same
  frames.
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
  display. Switching to another program (Alt+Tab, Command+Tab), a dialog of
  the system's taking the focus, or Alt+Enter to a window lets the pointer
  go, and coming back to full screen with the focus holds it again; a window
  never holds it. The window events that may change this
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
  holds the pointer and a window lets it go, and that the window comes back
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
  at the size the window system gives it. `app-full-screen` asks for the
  frame in each case and through a game, its game menu and full screen.
- `runtime_hud.cpp`, `runtime_match_hud.cpp`: the HUD.
- `runtime_messages.cpp`: the in-game message log (`Game.chat_lines`) drawn
  over the battlefield, and the speed and message part of `--check-navigation`.
- `runtime_console.cpp`, `runtime_console_debug.cpp`: the in-game console's
  host hooks, the debug grid, "Profile" bars and DebugBreak, and the console
  part of `--check-navigation`.
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
- `runtime_match_menus.cpp` also registers the load-game overlay
  (`register_load_game_screens`).
- The Open Annihilation settings ([oa/ui/engine_settings.hpp](../ui/engine-settings/README.md)):
  `engine_settings_state.hpp` and `runtime_engine_settings.cpp` read them at
  start, put them in effect, save them, and run the dialog for both of its
  hosts. `engine_settings_menu_host.hpp` and
  `runtime_engine_settings_menu.cpp` are the main menu's host: two overlays
  on the main menu, the OA button at the picture's bottom-right corner (its
  top-right corner while an extension's overlay stands over the main menu)
  under the extensions' overlays, and the dialog centred over the darkened
  menu above them, which takes every input while it shows. A press released
  over the button, Cmd+, on macOS or Ctrl+, elsewhere, and the macOS
  application menu's Settings… item open it; nothing opens over a message
  box or a frame a package owns. Enter is OK and Escape is Cancel; the key
  that closed the dialog does nothing more until it is released, so that a
  held key never reaches the main menu, where Escape itself does nothing.
  Another screen replacing the main menu closes the dialog as Cancel does.
  `engine_settings_match_host.hpp` and
  `runtime_engine_settings_match.cpp` are the in-game menu's host, and
  `runtime_engine_settings_app_menu.cpp` the application menu's item.
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
  `Runtime::acceleration_facts` gathers those facts, both hosts refresh
  the status each frame while the dialog is open, and
  `Runtime::apply_vertical_sync` asks the renderer to wait for the display
  only when the setting in effect changes it, never while it stays Off.
  `--check-engine-settings` (`native-engine-settings`) drives them through
  the SDL presenter over a preferences file it empties first:
  `runtime_engine_settings_check.cpp` holds the main menu's part, with each
  look of the button and the darkened menu under the dialog compared pixel
  for pixel with what they should draw, Escape doing nothing on the menu
  itself and EXIT ending the run;
  `runtime_engine_settings_dialog_check.cpp` the dialog driven by the
  pointer, the wheel and the keys (every section, scrolling, each setting in
  effect at once, Vertical sync read back from the renderer, Font shadow
  read back from the text style, OK, Cancel, Restore defaults and the keys
  they save) and the main menu with the
  button and the dialog as 640x480, 1280x720, 1920x1080 and 2560x1080
  windows show them, Graphics at its top and its end;
  `runtime_engine_settings_match_check.cpp` the in-game menu's button and
  dialog at those sizes, with the locks of a game played alone and of a
  shared game, Hardware acceleration set to Full and to Basic in a shared
  game, where each waits for the game's end, and the wheel scrolling the dialog, not the
  battlefield; and `runtime_engine_settings_wiring_check.cpp` each setting
  taking effect in a match, Escape's order, Vertical sync, the lock
  either acceleration flag puts on Hardware acceleration and the row set
  to Basic, Full and Off among them. The check runs with `--force-capable`,
  which lifts the software renderer's lock on both rows; every other step
  leaves Hardware acceleration Off, so every frame it compares is drawn on
  the processor. Both dialog steps also scroll a
  section of nine rows, `engine_settings_tall_section.hpp`, shown in place
  of the open section's rows; the wheel events are the check host's
  (`check_host_input.hpp`). With `--snapshot`, the check writes each of
  those frames beside the named file. While an extension's overlay stands
  over the main menu, the check first clicks the OA button in the menu's
  top-right corner through that overlay and closes the dialog it opens;
  then it sets the extensions' overlays aside
  (`Runtime::set_extension_overlays_aside`) until it ends, so that every
  frame it compares is the engine's own drawing over TA's own layout.
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
  of the display the window is on, its modes in full screen where full
  screen switches modes and the sizes that fit its desktop otherwise, up to
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
  OK, on the menus and in a match, which carries on as it was
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
  1280x720 snaps it to it; in a match it applies 1280x720, switches to full
  screen, where the dummy driver keeps the desktop's mode and the match is
  drawn at 1280x720 and letterboxed, the pointer in the bars resting on the
  frame's edge, applies 800x600 there, which the window takes as it leaves,
  and Desktop. `--check-engine-settings` chooses 1280x720 in the settings,
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
  one world. `director_output.hpp` and
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
  health bars, order lines and a tracking camera follow. The director
  draws its frames between ticks so. The debug grid draws its random
  numbers from its own generator, never from the match's streams, and
  draws the numbers a tick's first draw took again on the tick's later
  draws (`DebugGridRandom`), so that showing it never changes the game and
  a tick drawn more than once shows one grid. `app-presentation-interpolation`
  tests the blends and the grid's numbers, and `--check-interpolation`
  (`runtime_interpolation_check.cpp`) the frames.
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
  and that, with no such player, every frame is.

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
  minimap, the battlefield, a frontend screen) and drops the mouse events
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
  wheel item does; `runtime_touch_camera.cpp` the camera a finger moves:
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
  what SDL's devices hold. `press_match_key` runs a key with
  modifiers held for the call (SELECT ▾'s items), `refresh_pointer_modifiers`
  brings the pointer's key word up to a changed latch, and `play_haptic`
  reaches the platform's haptics. `remap_keypad_enter` turns the keypad's
  Enter into Return as `dispatch_event` takes each key, so it opens and
  sends the chat line, answers dialogs and ends typed names wherever Return
  does, as in 3.1c; the event keeps the keypad's scancode.
- `runtime_touch_hud.cpp` draws the controls into a layer of their own,
  composed over the CPU frame (`compose_touch_layer`) and presented in every
  tier (`present_touch_layer`), with the primitives of `touch_paint.*`
  (`oa-app-touch-paint`, tested by `app-touch-paint`).
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
polls: going to the background opens the in-game menu over a running game
played on this machine alone, which holds it (the Pause key's bit is left
alone), and saves the preferences; the system's low-memory warning lets the
cached model images go. `take_lifecycle_event` keeps every lifecycle event
from reaching a screen.

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
- `pad_glyphs.*` (in `oa-app-touch-paint`) paint the button glyphs of the
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
SHA-256, into `demo-1997` in the per-user data folder (`--data-dir` names
another), where later starts check it and use it again; while it passes that
check the installer is recognised by its size alone, since only unpacking
reads it. That folder is the installation, with the checked archive as its
only archive whatever else the folder holds, and the folder the player chose
is the one remembered. Temporary files an unpacking that stopped part way
left there are removed on a later start. The installer is only read, never
run; any other program in the folder is refused with a message saying it is
not the release the engine recognises, and a failed unpacking or a full disk
is reported the same way.
`DemoRelease` holds everything that identifies the release, in one place, so
that the tests (`demo_installer_test.cpp`) substitute a synthetic one.

Where the system's folder dialog cannot show, as in Steam's Game Mode, or
when several folders are found or the remembered one has gone, the
in-engine folder chooser picks the folder (`folder_chooser_screen.*`, over
the model of the module in src/ui/folder-chooser): the folders
found on this machine, a folder browser driven by touch, a gamepad, the keys
or the pointer, the 1997 demo's folder and, outside Game Mode, the
desktop's dialog. `folder_chooser_offered` says whether it may show, and
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
  settings over it.
- `game_files_check.cpp` is `--check-game-files`: scripted hooks, the route
  driven by taps and keys, pictures of each step and the verdict line.
- `runtime_game_files.cpp` fills Settings › Game files and opens the
  screen's management state from its MANAGE… button over the main menu.

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
  once, as the renderer records' notice is shown: in the settings dialog's
  look (`oa/ui/engine_settings/notice.hpp`), with the count of the moves
  whose notice is due (`moves_to_tell`), the `Saves/default` path wrapped
  at its separators, and that screenshots, films and mods now go in the
  same folder, or that each mod's go in a folder of its own; **Open
  folder** shows `Saves/default` and **OK**, Enter and
  Escape close it (`UserFolderState`). Showing it records it `told`. A run
  nobody watches leaves it due.
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
checks all of it beside its preferences file.

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
  notice (the settings then show No Mod, and their next save stores the
  choice over it: `EngineSettingsState::dropped_mod_folder`). Either flag
  locks the Mod setting for the run (`GameState::mod_from_command_line`),
  and a match locks it while it runs. A game folder holding its own
  `oamod.yaml` cannot carry a mod folder. A mod folder without an
  `oamod.yaml` layers over the game folder with no profile, by 3.1c's own
  rules, which the start says on standard output.

The settings' **Mods** page (`runtime_engine_settings.cpp`) lists the mod
folders of the game folder's `mods` folder (`list_mod_folders`), of the
player's own `Documents/Open Annihilation/Mods` (`list_mods_in`), the folder
an earlier version's Pick Folder... stored
(`open-annihilation.picked-mod-directory`) while it is the mod stored
(`open-annihilation.mod-directory`) and still a folder, and the one played;
a folder listed twice shows once. Once another mod or No Mod is stored, the
picked folder is forgotten: the save erases its key, a start that finds the
key naming another folder erases it with the next save, and the folder is
listed only while the game folder's `mods` folder or the player's own Mods
holds it. `read_mod_summary`
(`mod_summary.hpp`) reads each row from the folder alone, without resolving
its profile: its `oamod.yaml`'s name, version and description, and its
`oamod.png` badge, decoded by the engine's PNG reader
(`oa/formats/png.hpp`); a folder without an `oamod.yaml` shows its folder
name, "N/A" and "No oamod.yaml present". OPEN MODS FOLDER opens
`Documents/Open Annihilation/Mods` with the Your files buttons' opener.

Choosing another row asks Switch Mod. SWITCH (`DialogAction::switch_mod`)
first checks the folder as the next start will play it
(`check_picked_mod_folder`): one it could not play is refused, the line under
the list says why, and the dialog stays open. Otherwise it puts the dialog's
settings in effect and saves them as OK does, the new Mod among them, then
asks for a soft restart (`Runtime::soft_restart_requested`); a save that
fails leaves the game as it is and says so. The runtime's loop ends after
that frame. `main()` (`run_once`) frees the textures the runtime made on the
renderer (`Runtime::release_renderer_textures`), destroys the Runtime and the
mounted archives, runs the game-folder search again, which reads the newly
stored mod and resolves its profile, sets the data layout again, mounts the
archives again and builds a new Runtime on the same window and renderer, with
the full-screen state Alt+Enter left and the intro movies skipped
(`Options::restarts`), which opens on the main menu playing the new mod. The
player never exits the game. `--check-mod-switch` (`native-mod-switch`,
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
intgaf before any side's font (`Runtime::require_side_files`). The Mods
page refuses a folder without a profile whose start would end on a side's
missing section or no side at all, before anything is stored
(`Runtime::side_data_problem_over`). SIDEDATA and the files its sides name
are looked for in the language folders of the language the game starts in
first (`gamedata-German`, `anims-German`, `fonts-German`), once for the
run; a language chosen later reads them at the next start. The main menu
shows the warning (`mod_files_missing_notice`) once from each start of a
mod that lacks any of these (`Runtime::tell_incomplete_mod`), as the notice of the saved games' move, in
the settings dialog's look, its OPEN MOD FOLDER button showing the mod's
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
and the refused start through the Mods page's Switch Mod question.

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

- shows the profile's display version on the main menu, when it names one,
  even where its game data holds no `version.tdf` (without one the base
  game's label is hidden);
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
  as their countdowns come due (`runtime_map_units.cpp`).

A save made under a profile holds a ModProfile account (its id, version,
catalogue, hashes and the match's rule-state tables); such a save loads only
under a profile of the same sim hash, which the status line otherwise names
with the game's, and a save without one, written by 3.1c or by a mod's own
client, loads under any.

### Mod packages (.oamod)

A mod package is a zip archive of a mod's folder, which the game installs
into the player's own Mods folder (`src/app/mod-install`,
`oa-app-mod-install`, says how a package is read, planned, unpacked and put
in place, and how what a stop leaves is settled). The runtime's part:

- **Opening a package.** `--install-mod FILE`, repeated, and any bare
  argument ending in `.oamod`, any case, queue packages in the
  process-wide inbox (`oa/app/mod_install/inbox.hpp`); macOS's `-psn_`
  argument is skipped. A file the system opens in the game, or one dropped
  on the window, arrives as SDL's drop event: a watch on SDL's events
  (`mod_install_watch.cpp`), started right after each start of SDL's video
  (the window, the folder dialog's and the runtime's own), copies each into
  the inbox as it is queued, whatever polls the queue then. A platform that
  brings opened files into its own storage first does so through
  `PlatformHooks::take_opened_file`, and gets its copy back through
  `release_opened_file` once the install is done with it. On Windows and
  Linux a second start that carries only packages, while another copy holds
  the instance lock, hands them over through the hand-off folder
  (`oa/app/mod_install/handoff.hpp`) and ends; the running copy looks there
  once a second, in the background too, and brings its window forward.
- **At the main menu** (`runtime_mod_install.cpp`,
  `Runtime::tell_mod_installs`): once the menu has settled for two frames,
  with no dialog, notice or settings dialog over it, the next package is
  read; a plan that asks nothing unpacks at once, and the others show their
  question in a `Prompt` (`oa/ui/engine_settings/prompt.hpp`) over the
  darkened menu, at z 102. The unpacking runs steps of a 256 KiB budget,
  each folder or file made costing 16 KiB of it and each step ending after
  about 4 ms, for up to 15 ms a frame under the progress prompt; then the
  prompt says
  the files are being put in place, and the next frame makes the change,
  pumping events while it waits for files another program holds. What came
  of it is told: MOD INSTALLED, MOD UPDATED or MOD NOT INSTALLED, with OPEN
  FOLDER and, where the Mod setting is the player's to change, PLAY NOW
  (`Runtime::switch_to_mod_folder`, the Mods page's own switch). A run
  nobody watches leaves packages waiting.
- **The mod played.** A replace or reinstall of the folder the game plays,
  once unpacked and checked as the Mods page checks a folder, waits in
  `set_pending_change`, holding the Mods folder's lock, and the run ends
  (`request_soft_restart`); `main()` makes the change between runs, once
  the runtime and its archives are gone, and the next run tells it. Every
  run starts with `recover_changes` on the player's Mods folder, before the
  mod folder is resolved, so that a stop mid-change never drops the Mod
  setting.
- **ROLL BACK** on the Mods page (`Runtime::roll_back_mod_folder`): a folder
  of the player's own Mods folder whose `.backup` keeps an earlier version
  of its mod, one a pick would accept, offers it
  (`ModDetails::roll_back_from`, filled by `list_offered_mods`). The kept
  version is checked again first; then the folder is swapped at once, or,
  for the mod played, as the run ends. A roll back refused, the kept
  version changed since the page listed it or the swap undone, is told in
  a prompt over the dialog (MOD NOT ROLLED BACK) whose OK returns to it.
- **Registering the file type.** A start someone plays makes the game the
  opener of `.oamod` files for the player where the system registers at run
  time (`oa/platform/file_types.hpp`); none does with `--preferences-file`,
  `--user-folder`, `--data-dir`, unattended, headless or on SDL's dummy or
  offscreen video driver.

`native-mod-install` (`--check-mod-install`, `runtime_mod_install_check.cpp`)
drives every prompt over five runs.

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
game file and listing the run looks up, one a line, which shows that a
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
  the folder in name order ([src/audio](../audio/README.md)); the CD music
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
  profile's defaults and are held to its maxima (`click_snap_radius`).
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
  one outline further, clear of its outline and shadow.
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
  shows opened again in the language.
- **Picture captions** (`runtime_picture_captions.cpp`): while a language
  other than English is shown, the words its packs' `pictures.tdf` gives
  (`language_pictures`, drawn by `oa/present/picture_captions.hpp`) are
  drawn in the bundled fonts over the pictures they name as each is
  loaded: the GAF files the runtime reads (`append_gaf_file`), the match
  bar's `commongui.gaf` and side panel, a screen's sprites and the named
  backgrounds. A picture the
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
acting as the battlefield's do in the chosen interface type, which
`native-megamap-clicks` checks (`check_megamap_clicks`) after what it draws
(`check_megamap_picture`);
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

## Extensions

`extension.hpp` is the table of hooks through which libraries linked into
`oa-game` extend it: long options and game switches, start-up and
shutdown, screens, the frontend's entry (its game name and nickname), run
modes, per-frame work, match events, the Pause key, the speed keys and
the GAME slider, the frontend's application modes, the loading's
progress, the team panels' host (a tournament game withholds CONTROL),
requests to close the window, the label a match's return names in its
menus, whether the preferences keep the stored password, recordings to
replay, console commands and checks. Each such library is an extension.
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
end of a headless skirmish `release_runtime`, the follower first.
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
first. CI builds that configuration as a job of its own, but has no game
installation: there `extension-hooks-game` skips, and only the option
hooks and the hook list are checked. The rest of the hook coverage runs
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
`-e -h -n -p -t`, binds the network session (`src/netgame`) to the
multiplayer screens (`src/ui/frontend-multiplayer`), launches the
match from the battle room and runs it over the network, and replays
recorded games (`.tad`, `src/formats/tad` and `src/session/demo`) through
`open_recording` and `--play-demo FILE.tad`. Its long options include
`--net-loopback-check N`, which hosts and joins a match in one process
over 127.0.0.1 and compares both worlds after N ticks,
`--check-recording-hook` and `--check-host-not-found`, and `--host` and
`--join ADDRESS` (with `--player-name`, `--game-name` and
`--game-password`), which have the multiplayer screens host or join a
TCP/IP game at once, each screen doing what a player would
(`multiplayer_bind_direct_game`), and print the battle room's progress;
`--check-multiplayer-menu` runs its check of the multiplayer screens.
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
[docs/development/testing.md](../../docs/development/testing.md#network-play)
lists its tests.

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
