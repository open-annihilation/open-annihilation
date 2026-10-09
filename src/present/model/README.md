# Model drawing

Draws 3DO models (units, features, debris, shatter fragments and projectiles)
the way the game does, into 8-bit palette-indexed surfaces, and carries those
draws onto the engine's RGB match frame. It is presentation only: it reads the
simulation's units and piece transforms and never changes what the simulation
holds.

## Entry points

Namespace `oa::present::model`, headers in `include/oa/present/model/`:

- `model_library.hpp`: texture archives, each model's primitives in draw order
  with their textures, and the display tables (alpha, shade, blue) the drawing
  reads.
- `mesh_raster.hpp`: scan conversion of textured 3DO quadrilaterals, with and
  without a depth plane.
- `model_draw.hpp`: units from their cached images and the pieces that move
  every frame, shadows, the build effect, carried units, features, debris,
  fragments and projectiles.
- `unit_supersampling.hpp`: enhanced anti-aliasing of units, drawn at several
  samples a pixel and reduced into the frame.
- `rgb_bridge.hpp`: the bridge between the 8-bit surfaces and the RGB frame.
- `shadow_fade.hpp`: how dark shadows are drawn at a zoom (`shadow_strength`,
  `shadow_level`): the game's own at zoom 1 and closer, easing on a
  smoothstep of the halvings of the zoom toward none at a quarter, none once
  the level would fall below a sixteenth of the game's darkness
  (`shadow_least_level`), a little before, and none farther out; and the
  faded alpha table a renderer's shadows blend through
  in between (`ShadowTable`, `ModelRenderer::shadow_table`), the display's
  own with the rows of the shadows' colours mixed toward the colour under
  them.

## The RGB bridge

The game draws models in 8 bits and blends through palette tables, so a draw
needs the palette indices of what is already on screen. The match frame is
RGB, and sprites, particles and the terrain are drawn into it in full colour.
A bridge (`RgbBridge`) lays an 8-bit surface over a rectangle of the frame, at
a scale of frame pixels to 8-bit pixels:

- `bridge_begin` starts a frame's drawing over a rectangle of the frame.
- `bridge_open` captures the 32-pixel tiles a region overlaps: each 8-bit pixel
  starts as the palette index of the frame pixel it captures from (that colour's
  entry, or the nearest entry by squared distance for a colour outside the
  palette), and draws are clipped to the region.
- `bridge_end` writes every 8-bit pixel that a draw changed back to the frame
  through the palette, for the tiles captured since the last write-back.
  Pixels no draw changed keep their colour, so colours outside the palette
  survive. At a scale other than 1 an 8-bit pixel covers the frame pixels
  the scene grid lays its place over
  ([`scene_grid.hpp`](../include/oa/present/scene_grid.hpp)), the pixels the
  terrain fill gives the map pixel of the same place, so a model stays over
  its ground at every zoom.
- `bridge_open_sampled` and `bridge_end_sampled` do the same for a region
  drawn at several samples a pixel.

The bridge keeps a capture copy (`CaptureCopy`) from one capture to the next:
for each frame pixel it captures from, the colour it last mapped and that
colour's index. A capture compares the frame with the copy and maps again only
the pixels whose colour changed, whoever changed them: the bridge's own
write-backs, or sprites and particles drawn into the frame between two model
draws. A write-back at a scale of 1 over a rectangle inside the frame also
brings the copy up to date with the pixels it writes. Every capture therefore
gives the same indices as mapping every pixel afresh, and the frame comes out
byte for byte as it would without the copy.

The copy is kept across `bridge_begin` calls while the frame's size and row
length, the rectangle, the scale and the palette stay the same, so a frame
whose picture did not change under a tile maps nothing there. A change of any
of them forgets it.

Memory: besides the 8-bit surface and its captured indices (one byte each per
8-bit pixel), the copy holds four bytes (colour and index) for each distinct
frame pixel captured from, so never more than four bytes per frame pixel of the
rectangle.

## Drawing a frame in bands

A frame's models can be drawn in horizontal bands, one after another or on
several threads at once, and come out byte for byte as the frame drawn
whole. Each band draws every draw of the frame in order, with its own rows
alone:

- `plan_model_draw` builds what `draw_model` builds of a model on the way, in
  the same order (a building's silhouette, the images of the units it
  carries with their build effect), and notes what the draw decides (the
  ground height, the carried units' models and images), without drawing;
  `draw_planned_model` then draws the model from the plan, building nothing
  and changing no draw state and calling none of the renderer's hooks.
  `plan_unit_supersampled` and `draw_planned_unit` do the same for a unit's
  draw at a level of enhanced anti-aliasing, the unit readied
  (`prepare_linked_draw`) and its finer image built at planning.
  `draw_model`, `draw_linked_model` and `draw_unit_supersampled` plan and
  draw in one call.
- `bridge_split` splits a bridge into bands of whole tile rows, about even
  in frame rows. A band's edge lies only between two tile rows where the
  frame rows split too: the rows above capture from and are written back to
  frame rows above the band's first frame row, and the rows below from and
  to that row or further down. At a scale of 1 every tile row qualifies; at
  other scales some do not, and the bands are fewer when too few do.
  `bridge_open`, `bridge_end`, `bridge_open_sampled`, `bridge_end_sampled`
  and `bridge_index` taking a `BridgeBand` capture, write back and look up
  only the band's rows: a band's tiles, rows of the capture copy and frame
  rows are its own. A band keeps its own open tiles and, given
  `bridge_band_colours`, its own memory of colours outside the palette
  (64 Ki slots), so that bands can draw at once; the copy's count of mapped
  pixels is added back with `bridge_join_band`.
- A band's draws change only its rows through the surface's band
  (`OA_SURFACE_FLAG_BANDED`). `surface.h` lists the draws that honour it:
  a filled polygon, a textured quad or a line works out its pixels from the
  clip alone, as over the whole surface, and writes only those in the band;
  a sprite's rows draw alone, so its rows outside the band go
  (`trim_to_surface`). The model draws use only those; the other draws of
  `oa-present` must not be given a banded surface, which the ones
  `surface.h` names check in debug builds. A sampled region of a band is laid
  out for the whole region and captures, draws and writes back only the
  band's rows.

The display a draw reads its tables from (`bind_display`) is bound per
thread, so that each thread drawing a band binds the frame's display.

## Tests

`tests/` holds one ctest per file (`model-render-*`). `rgb_bridge_test.cpp`
checks captures, write-backs, scaling and sampled regions, that at 0.37 and
1.37 every pixel of a frame 2400 pixels wide is written back from the map
pixel the scene grid shows there, and holds the copy
to a bridge that maps every pixel afresh: over many captures, write-backs and
writes into the frame between them, at scales of 1, 2, 0.5, 0.75 and 1.5, with
the rectangle inside the frame or past its edges and with colours that more than
one entry holds, the indices captured and the frames written back are the same.
It also checks that unchanged pixels are not mapped again, and that frames
drawn through the bridge band by band (2, 3, 4 and 7 bands) come out as
those drawn whole, frame after frame with the copy kept, captures,
polygons, lines, blended sprites, write-backs, writes into the frame and
sampled regions among the draws, at scales of 1, 2, 0.5, 0.75, 1.1, 1.5 and
0.3 and with the rectangle inset in the frame. `shadow_fade_test.cpp` holds
the strength curve to its ends, its middle and its easing, the levels, the
faded table to the mix of each colour and the display's own elsewhere, and a
silhouette drawn through it. `mesh_raster_test.cpp` draws
textured quads, twisted ones among them, whole and band by band. The native
checks and the director render pin the frames the match draws through the
bridge; `native-draw-threads` draws a fight on 1 to 7 bands.

## Limitations

At scales other than 1 some tile rows cannot start a band, so a frame
zoomed by an odd amount splits into fewer bands than asked for. The bands
are even in rows, not in work: a fight crowded into a few rows leaves the
other bands little to do.

A capture still compares every frame pixel of each tile it opens with the copy,
since sprites and particles draw into the frame without telling the bridge
where.
