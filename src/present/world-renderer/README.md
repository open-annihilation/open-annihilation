# Terrain viewport renderer

This SDL-neutral component renders a camera crop from the TNT 32x32 tile
mosaic into RGB. It uses the game palette supplied by the caller and does not
define camera input, scrolling, zoom, fog, or presentation.

The game's terrain view divides the camera pixel origin by 32, carries the
remainders into clipped edge tiles, and then copies complete interior tiles.
Direct per-pixel selection in `render_viewport` has the same visible result
while keeping those coordinate units. The radar picture uses the same
`tile_index * 0x400 + y * 32 + x` mosaic addressing.

`draw_terrain_view` is the 8-bit form of the terrain view itself: it draws the
tiles cut by the view's edges through `draw_sprite_opaque`, clipped to the
target, and the whole tiles between them through `blit_tile`, reading the
camera, view size and battlefield rectangle from the Game block. Its test
compares the installation's maps with `render_viewport` through a palette
whose red channel is the index.

Out-of-map crops are rejected: `render_viewport` and
`render_battlefield_viewport` take a camera on the map. Output is capped at
64 million pixels.

`BattlefieldViewport` carries the camera crop and destination clip together.
`render_battlefield_viewport` copies the TNT mosaic directly into that clip;
the TNT already stores the battlefield artwork in screen-map pixel space, the
space the radar sampler reads as well. The default destination is
`(128,32)`, set when the game screen is laid out. That layout reserves the
128-pixel command panel, 32-pixel top bar, and 32-pixel bottom strip,
producing a `512x416` battlefield on the `640x480` surface.
`unit_projection_for_viewport`, `map_pixel_to_screen`, and
`screen_to_map_pixel` use the same origin and camera values, all signed: a
camera, and a map pixel, may lie left of or above the map, where the view
shows past its edges. A zero-height unit projects to
`(map_x-camera_x+128, map_y-camera_y+32)`; unit height alone supplies the
additional `-height/2` vertical displacement. A view drawn
between map pixels, as the accelerated tier draws it while it scrolls, lies
a `ViewOffset` past the camera's map pixel, from 0 to 1 along each axis;
`screen_to_map_pixel` given that offset maps a screen point to the whole map
pixel drawn there, and with no offset to the one it always has.

The viewport-derived unit projection also carries the battlefield raster clip.
Solid and textured primitives are clipped to that rectangle, matching the
game's surface clip and preventing units at the camera edge from drawing over
the command panel, top bar, or bottom strip. When the game draws units it
leaves the surface clip's inclusive right column and bottom row undrawn, so
viewport-derived unit clips keep that one-pixel-short right and bottom edge
while terrain and hit testing keep the full inclusive extent.

## Drawing in bands

The match's terrain fill (`fill_scaled_viewport`, the destination-sized
sample of the mosaic at any zoom, within the map the view shows: the fill
takes the shown width and height, the mosaic's or less where the game never
shows a map's last columns and rows, and is black past them as past the
mosaic, and before the map's left and top edges) and its fog
(`draw_fog_grid`, whose grid counts its cells toward negative infinity for
a camera before the map, as it counts them on the map) take an
optional [job pool](../../platform/job-pool/README.md) and split their rows
into bands by the data: the fill by `terrain_band_rows` (32) destination
rows, each band finding its first map row and fraction from its first row's
index exactly as the row-by-row step reaches them; the fog by one row of its
grid, which draws only the surface rows its map rows land on. Bands write
nothing another reads, so a pool of any size draws the bytes the calling
thread draws alone, and with no pool the bands run in order on the calling
thread. A missing tile stops the band that finds it; the fill then reports
it, and which rows were written is not specified. `shown_map_span` names
the destination pixels along an axis that the fill takes from the shown
map, where the match cuts its features off.

`world-draw-bands` fills a random map at zoom 1, 1.37, 0.6, 0.75 and 2, from
the corner, the middle and past the map's edges, and draws a random fog grid
with random tile art at four zooms, plain and dithered, without a pool and
on pools of 2, 3, 4 and 8 threads: every pool gives the same bytes, each
filled pixel is the map pixel its zoom names, no band writes past its row,
a missing tile is reported from a pool, and `shown_map_span` names exactly
the pixels a fill draws from the shown map.

## The area pass

`area_filter_rgb24` (`scene_filter.hpp`) reduces a scene drawn finer than
the screen to the screen's picture: each picture pixel is the average of the
scene under its footprint, each scene pixel weighted by the area of it the
footprint covers. The scale `m`, screen pixels per scene pixel, is 16.16
fixed point from one half to one; the scene starts at the picture's corner,
and picture column `x` covers scene columns `[x / m, (x + 1) / m)`, at most
three of them, and rows alike. Only the accelerated presentation calls it
([src/app](../../app/README.md)); in the standard tier the game's zoomed-out
battlefield is drawn as before, terrain averaged over whole map pixels and
units point-sampled.

- **Exact weights.** Measured in 16.16 screen units, a picture pixel spans
  65536 and a scene pixel `m`'s 16.16 value, so the part of a footprint a
  scene pixel covers is a whole number of units: the weights of every
  footprint sum to exactly 65536, and each scene pixel counts, over all the
  footprints it falls in, for exactly its length. `plan_area_filter` builds
  them once for a scale and a picture size (an `AreaPlan`), and every frame
  at that scale and size uses them. Only `plan_area_filter` fills a plan, so
  its taps always lie inside the scene it names; rebuilding one at a size no
  larger keeps its storage.
- **Rounding.** The pass is separable: each scene row under a footprint is
  averaged across its columns and kept at 16 bits, eight below the channel's
  level, rounded to the nearest with halves up; those are averaged across
  the rows and rounded once to the 8-bit level, halves up. A level is
  within half a level, and half of 1/256 of one, of the exact average.
- **At one half** every weight is one half, so the picture is the 2x2 box
  `(a + b + c + d + 2) / 4`, the rounding the terrain's box filter gives its
  2x2 footprints at zoom 0.5; `native-render-tiers` compares the two byte
  for byte on the map's terrain. **At one** the picture is a copy of the
  scene.
- **Cost.** Each band averages every scene row it needs across the columns
  once, then sums those row averages down each picture row's footprint. It
  works in strips of 256 columns, whose row averages, four scene rows of
  them at most, stay on the stack (6 KB), so a frame allocates nothing. A
  1664x952 picture takes 28.5 million multiply-adds at one half and about
  43 million at most, just above one half, where a footprint covers three
  scene pixels a side.
- **Integers only**, in plain C++ with no float and no vector instructions
  of its own, so the bytes are the same on every platform and build type.
- **Bands.** A picture pixel depends only on the plan and the scene, so any
  split of the rows gives the same bytes. `area_filter_rgb24` filters bands
  of `area_band_rows` (32) rows on an optional job pool;
  `area_filter_rgb24_rows` filters any range of rows.
- **A view between map pixels.** A plan may start the picture part of a
  scene pixel into the scene, a phase along each axis in 16.16 screen
  pixels from 0 to the scale: picture column `x` then covers scene columns
  `[(x + p) / m, (x + 1 + p) / m)`. The weights stay exact, the scene it
  reads grows by the phase, and a phase of one whole scene pixel gives the
  picture of the scene without its first column or row. The accelerated
  presentation plans again whenever a scrolling view moves the phase.
- **Bounds.** A plan takes pictures of 1 to `area_picture_edge_limit` (8192)
  pixels a side, and a phase of at most one scene pixel; a frame is refused,
  with nothing written, when it has no plan, when the picture's size is not the plan's, when the scene is smaller
  than the plan reads, when a stride is below its width or above
  `area_stride_limit`, or when storage is missing. The pass reads only the
  scene's covered corner and writes only the picture's rows, never the
  padding past them.

`area_sample_reference` is the exact average in double precision, from the
footprint's geometry rather than a plan; tests hold the pass to it.

## What the graphics card is asked to do

`scene_filter.hpp` also holds, in double precision on the processor, the
references the accelerated presentation's drawing on the card is held to:
`nearest_rgb24` and `bilinear_rgb24`, the card's NEAREST and LINEAR scale
modes, with pixel centres at half-pixel places and the scene's edges
clamped; `sharp_bilinear_rgb24`, NEAREST into a prescale target a whole
number of times the scene's size and then LINEAR; `pixelart_rgb24`, the
renderer's pixel-art filter; `overlay_rgb24`, the overlay rule: an opaque
overlay pixel's colour replaces the picture's and a transparent one leaves
it; `two_level_rgb24`, the card's two-level reduction, which reduces the
Full tier's world target when the view is zoomed out: the scene's half,
each pixel the mean of four, drawn LINEAR at twice the scale under the
scene drawn LINEAR at alpha 1 - log2(1 / scale), from one half, where it
is the box of four, to 1, where it is the scene; and
`footprint_sample_reference` and `exact_channel`, the exact average
under a picture pixel's footprint wherever the scene lands. A
`ScenePlacement` says where the scene lands: its scale across and down and
the picture point its corner lands on. `scene_line_thickness` is the rule
the game draws lasers, lightning and selection lines by in a scene the area
pass reduces: the draw scale over the zoom, rounded, at least 1, so a line
stays about one screen pixel thick. `line_energy` measures a thin line's
light, and `shimmer` how far a run of frames' changes depart from those of
the exact pictures of the same moments.

`world-scene-filter` checks that the sharp-bilinear and pixel-art
references copy the scene at 1 and replicate it into whole blocks at 2, 3
and 4 where LINEAR blends, that between whole scales each scene pixel is a
block with one blended edge pixel, the overlay rule, and the footprint
average against the area pass's reference. `world-zoom-stability` follows a
seeded line one map pixel wide, a map pixel a frame for 64 frames and in
eighths of a pixel, at zooms 0.5 to 0.95: the area pass keeps each row's
light within one level per pixel the line lit, today's point sampling
loses the line on some frames, a line drawn by the thin-line rule
keeps from two thirds to four thirds of a screen pixel's light, and the
two-level reduction keeps the line's light within 0.85 and 1.16 of the
ideal at every zoom, exactly at 0.5, and swings no wider than plain
bilinear reduction; and on a
seeded map of fine detail, the area pass's frames shimmer less than point
sampling's at 0.5, 0.6 and 0.75, and the sharp-bilinear and pixel-art
references less than NEAREST at 1.37 and 2, the scene moving in eighths of
a map pixel. `world-screen-span` keeps today's one-pixel minimum.

`world-scene-filter` checks the weights; that the pass gives the bytes of a
straightforward implementation intersecting every footprint with every scene
pixel, at scales across the range; that it stays within half a level and the
row averages' rounding of `area_sample_reference`; the box at one half and
the copy at one; that a line one scene pixel wide keeps its intensity, within
the rounding of the two pixels it touches, at 40 places and six scales from
0.5 to 0.9; that bands of 1, 7, 33 and 350 rows and pools of 1 to 8 threads
give the same bytes and write nothing outside their rows; that malformed
plans and frames are refused, a plan moved from among them; that rebuilding
a plan no larger allocates nothing; a pinned FNV-1a digest of seeded
scenes; and, for a picture started part of a scene pixel in, exact weights,
the bytes of the straightforward implementation started there, the picture
of the scene moved on a pixel for a whole pixel's phase, and a thin line's
intensity kept as the phase moves through a pixel in sixteenths. It logs the time of a 1664x952 picture, the battlefield of a
1920x1080 window, from a scene twice its size at one half, just above one
half and at two thirds, the last two with footprints of three scene pixels
a side, for information.

## The nearest resample

`resample_nearest_rgb24` (`scene_filter.hpp`) turns a scene, the battlefield
drawn at a draw scale of its own, into the picture at another scale,
nearest-pixel, in bands of `resample_band_rows` (32) picture rows on an
optional job pool. It reads and writes the area pass's `RgbSource` and
`RgbTarget`. Its step is the terrain fill's: picture column `x` shows scene
column `floor(x * 65536 / s)`, where `s` is the scale in 16.16 rounded to
the nearest, and rows alike, so a scene of the mosaic drawn at one pixel per
map pixel and resampled at a zoom is, byte for byte, the terrain the fill
draws at that zoom, and at scale 1 the picture is a copy of the scene's
corner. It refuses, writing nothing, missing storage, a stride below its
row's width and a scene smaller than the picture reads, which
`resample_scene_extent` gives for each axis; `area_error_text` says what
each refusal means. The game draws its battlefield at the zoom and
resamples nothing, except for a check that draws the scene apart from the
world layer.

`world-scene-filter`'s nearest-resample case checks every picture pixel
against that mapping at scales from 0.5 to 4, and 0, which counts as 1;
resamples a scene of a random map filled 1:1, two pixels larger than the
picture reads, at zooms from 0.6 to 4, from the corner, the middle and past
the map's edges, and checks the picture against the fill at that zoom
without a pool and on pools of 2, 3, 4 and 8 threads; checks the scene's
size against `resample_scene_extent`, that nothing past a row is written,
and that a scene too small, missing storage or a short row is refused with
nothing written.

## Draw order

`plan_battlefield_draws` (`world_draw_order.hpp`) orders a frame's features
and units the way the battlefield draws them, far to near in rows one plot
deep. Features lower than `standing_feature_min_height` lie under every unit
and draw first, row by row. Then each row draws its ground units, in the
order given, followed by the standing features whose origin plot is in that
row, left to right: a tree covers the units in its own row and the rows
behind it, and units in the rows in front of it cover the tree. A unit's row
is the camera's plot row plus the whole rows between the camera and the
unit, truncated toward zero, so it follows the camera's position within a
plot. A ground unit whose place lies on a building's footprint
(`stands_on_footprint`), as a unit leaving a factory stands on its
platform, draws straight after that building wherever its row and its place
in the list would draw it first, so that the building never covers it; units
following one building keep their order among themselves. 3.1c drew such a
unit by its own row alone, so a unit released from a platform north of the
factory's centre, as the Arm Vehicle Plant's is, was drawn under the
platform at most camera positions until it drove past the centre. Units not
on the ground (in the air, or carried) draw in a last pass, after the
projectiles and explosions. Its test pins each of these rules, and
`native-factory-departure` checks every factory with a platform in a match.

## Solid 3DO primitives

`render_colored_instance` draws the solid-color path. It consumes the
transformed vertices from `model-runtime`, visits visible pieces in reverse
order, projects signed 16.16 coordinates with the orthographic formula, and
fills convex scanline spans with the primitive color's low palette byte. The
battlefield destination origin is `(128,32)` after the command panel and top
bar; camera inputs remain map pixels and are shifted to 16.16 by the
projection.

Textured four-vertex primitives use the textured quad rasterizer. Static,
raw, single-frame GAF textures are resolved by name and sampled with the game
palette. The rasterizer covers clipping, rotation and both windings, and
samples textures of every width alike, the 8, 16, 32, 64 and 128-pixel
widths included. Bounds checks clamp malformed out-of-range UVs to the
texture's edges.

`UnitMaterialState` supplies animation cursors keyed by model object and
primitive, plus the draw call's force-first flag and owner's team-frame byte.
The per-primitive cursor matches the runtime record the model loader builds;
an absent cursor is its initial frame-zero snapshot, and the renderer does
not advance it or derive it from a clock. In `select_material_frame` team
materials use the owner byte even when force-first is set; animated materials
use frame zero only when force-first is set; fixed materials always use frame
zero. Out-of-range team bytes and unsupported indexed frames select null and
remain deferred.

Texture GAF filenames use AssetStore's loose-file, mount, and stored HPI
directory order. A first sequence-name match is kept even when unsupported,
so a later archive cannot incorrectly replace it.
