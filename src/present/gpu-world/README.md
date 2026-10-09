# Card-drawn world

Groundwork for the tier in which the graphics card draws the battlefield
itself: the card-ready forms of the game's assets, built on the processor
from the same decoded data the processor draws from today, in pure C++20
with no window, renderer or platform interface. The module is one library,
`oa-present-gpu-world` (`oa::present::gpu_world`), whose main header
`include/oa/present/gpu_world.hpp` includes every part; each part has its
own header under `include/oa/present/gpu_world/`, its own source and its
own test, so that the parts land separately, and `texel.hpp` holds what
they share: `texel_bytes`, the four bytes of an RGBA8 texel. Nothing in
this module touches SDL or a graphics API. The Full tier
(`src/app/runtime_full.cpp`) draws from the terrain atlas, the sprite pages
and the model meshes; the standard tier is unchanged, and no part of this
module runs unless that tier does.

## The terrain atlas

`include/oa/present/gpu_world/terrain_atlas.hpp` turns a map's 32x32 tile
mosaic into pages of RGBA8 texels (red, green, blue and alpha in memory
order, alpha always 255) with a mip chain each, and the grid of atlas slots
the map's cells show. The whole map is resident: every distinct tile the
grid names has its slot before the first frame, so nothing is placed or
evicted while a match runs.

- **Slots.** `build_terrain_atlas` walks the grid asked of it, the map's
  first columns of its first rows (the whole mosaic, or the map the view
  shows: the game never shows a map's last tile column and last four rows,
  which maps fill with filler tiles), row by row and gives each distinct
  tile, compared by its pixels, the next slot in order of first use, so
  cells that share a tile share a slot and tiles no cell names take none.
  `TerrainAtlas::grid` holds the slot of every cell and
  `slot_tiles` the map's tile each slot was filled from. A slot spans
  `slot_pitch` (40) texels a side at level 0: the tile and a gutter ring
  of `level_0_gutter` (4) texels copied from the tile's edge, so that
  linear filtering never reads a neighbour.
- **Colour.** A texel of a tile is the tile's pixel through the palette and
  then the display gamma table, the bytes today's terrain fill and
  conversion give it at zoom 1. Every other texel of a page is opaque
  black.
- **Pages.** The build takes the largest edge a page may have:
  `fit_page_edge` of the card's texture limit, a power of two from
  `page_edge_minimum` (64) to `page_edge_limit` (4096), 2048 on a card
  whose textures stop there. Slots fill pages of `full_page_slots` of that
  edge (10,404 at 4096, 2,601 at 2048) in slot order, and `plan_page` sizes
  each page for the slots it holds: each edge a power of two up to the page
  edge, the smallest area whose rows of whole slots hold the count, then
  the squarer page, then the wider of two the same shape. A cell names its
  slot in 16 bits, so an atlas holds at most `slot_limit` (65,536) slots on
  `page_limit` of the edge pages (7 at 4096, 26 at 2048). The atlas records
  its `page_edge` and `slots_per_page`, and the same map gives the same
  tiles and views within every edge, on more pages as the edge falls.
- **Levels.** Level L of a page is the exact box reduction of level 0:
  each texel the average of the 2^L by 2^L level-0 texels under it,
  rounded once, halves up, in palette colour before the gamma table, the
  order today's zoomed-out terrain filter and conversion keep. The chain
  runs down to one texel (`level_count`, `level_size`). At the tile levels,
  `tile_level_count` (3) of them, every tile is a whole number of texels
  (32, 16 and 8 a side) with a whole gutter ring (4, 2 and 1 texels), and
  the gutters are rebuilt from the level's own reduced tile edges, so each
  level filters without bleeding on its own. Deeper levels reduce the whole
  page, tiles and gutters together, and complete the chain; a renderer
  stops at the last tile level. `tile_rect` gives a slot's tile at a tile
  level.
- **Memory.** `terrain_atlas_footprint` costs an atlas before it is built,
  within a page edge: its pages and their every level, the tables, and the
  builder's working storage, which is the sums of the largest page's last
  tile level and the level after it. The largest atlas among the installed
  game's maps is Crystal Maze's, 11,444 slots: a 4096x4096 page and a
  2048x1024 page within 4096, or four 2048x2048 pages and a 2048x1024 page
  within 2048, 96.0 MiB of texels with their chains either way against
  11.2 MiB of 8-bit tiles, built with 15.0 MiB of working storage within
  4096 and 3.8 MiB within 2048. A map of 3,466 distinct tiles takes one
  4096x2048 page, 42.7 MiB, or two pages within 2048, 32.0 MiB; the
  format's bound of 65,536 slots takes seven pages, 554.7 MiB, or 26 pages,
  538.7 MiB. The game's zoom never goes below one half, so the levels the
  card draws from are 0 and 1, and the chain beyond the tile levels is
  under two per cent of the whole.
- **Reading back.** `read_terrain_view` reads what the card shows at a tile
  level when the camera lies on a whole texel of that level, black past
  the map's edges and before them, for checks against the processor's
  terrain; it refuses a deeper level and a camera between texels.
- **Malformed maps** are refused with the atlas left empty: an empty grid,
  a grid that is not `tile_width` by `tile_height` cells or that exceeds
  `grid_cell_limit`, a grid asked for of no cells or of more columns or
  rows than the map holds, a tile set that is not `tile_count` tiles, and a
  cell naming a tile the set lacks.

### What the atlas holds exactly, and what the renderer decides

What the card does with the atlas, and how a zoom between levels is
filtered, is the renderer's to decide. At zoom 1, level 0 holds today's
bytes exactly at every camera. At zoom one half, level 1 drawn 1:1 holds
today's bytes exactly when the camera lies on an even map pixel. Today's
zoomed-out filter clamps the camera to any integer map pixel, so at an odd
camera, half of the positions, level 1 drawn 1:1 shows today's picture
shifted by one map pixel, half a level-1 texel; at zoom one quarter the
same holds for a camera off a multiple of 4, three positions in four. The
renderer either snaps the camera to the level's grid at those zooms, which
the camera's clamp allows, or accepts the shift; `read_terrain_view`
refuses a camera between the level's texels rather than choose. Between
zoom one half and 1 the card's own filter, or a blend of the two levels,
applies, and those bytes are the card's, not today's.

### Limitations and follow-ups

- **The greyed variant is the Full tier's.** The fog's mapped-but-unseen
  cells show the terrain through the gray table: the same build with a
  palette whose entries are the gray table's, a second atlas the Full
  tier builds beside this one (`greyed_palette`,
  `src/app/runtime_full.cpp`), sharing its slots and grid, and uploads as
  its greyed pages.
- **No eviction and no budget.** The atlas is the whole map, and
  `terrain_atlas_footprint` is what a tier's memory figure costs it with.
  There is no smaller form: dropping the chain beyond the tile levels saves
  under two per cent, so a machine that cannot afford a map's atlas within
  the card's page edge is a tier choice, not this module's.
- **Uploaded by the Full tier.** The pages are processor memory until the
  Full tier (`src/app/runtime_full.cpp`) uploads levels 0 and 1 of each
  page as the match loads, with a greyed variant built with a palette of
  the gray table's entries for the fog's greyed pass; it then lets each
  page's texels go and keeps the grid, the slots and the pages' sizes and
  levels, which its builder reads through `tile_rect`, choosing the level
  per draw by its zoom.

### Tests

`gpu-world-terrain-atlas` (`tests/terrain_atlas_test.cpp`) plans pages by
table within every page edge, with the edge fitted from a texture limit;
packs a seeded map with duplicated and unused tiles within 4096 and within
2048 and checks the slots, the grid, every tile's texels against the
palette and gamma, the gutters at every tile level, that no slot overlaps
another and that the rest of the page is black; builds the same map within
64, 128 and 2048, and a map of one slot more than a 2048 page holds within
2048, 4096 and a limit of 3000, and checks the pages, the same tiles and
views at every tile level, and the footprint against each build; reads
views at level 0 against today's terrain fill (`fill_scaled_viewport` at
zoom 1, through the gamma table as the conversion applies it), at the
camera positions and sizes that cross the map's edges, with no gamma and
with gamma 0.75 and 1.25; reads views at level 1 against the present
layer's area pass at a scale of exactly one half (`plan_area_filter` and
`area_filter_rgb24`, which `world-scene-filter` pins to today's box filter
byte for byte) and against that filter written out, and at level 2 against
the filter written out at one quarter, with the camera on whole texels,
with and without gamma; checks every texel of every level against the
exact average of the level-0 texels under it; holds the seeded map's page
and grid to pinned FNV-1a digests, with and without a gamma table and
within either edge; refuses each malformed map and each bad read; and
prints the memory figures within 4096 and within 2048.

`gpu-world-terrain-atlas-part1-data` to `gpu-world-terrain-atlas-part4-data`
do the same over every map of the installed game, each over a quarter of the
maps in name order (`--data --part K/4`): level 0 against the fill over the
whole map, level 1 against the area pass and level 2 against the filter
written out on every fourth band of 256 rows, and every eighth map again
through a gamma table; then each builds the largest atlas of its quarter
within 2048 as well, checks that it shows the same views from pages within
the edge, and prints both figures.

## Sprite pages

`oa::present::gpu_world::SpritePages`
(`include/oa/present/gpu_world/sprite_pages.hpp`) holds GAF sprite frames
as texels the card can draw: trees, wrecks and other features, explosions,
smoke, flames, plasma and the sprite shadows. A frame comes in as the GAF
reader renders it for the match today (`oa::formats::gaf::RenderedFrame`:
one palette index and one coverage byte a pixel, with the hotspot), is
decoded once into RGBA8 texels and placed on a page; the next request for
the same frame is answered from the page.

**What a request gives back.** `frame(frame_id, mode, source)` returns a
`FrameRecord`: the page, the frame's rectangle of texels on it, the hotspot
as the GAF frame carries it, and the draw mode. `frame_id` is the caller's
own number for the frame, the same every time for the same frame; the pages
never read the source again once a frame is held, so a frame whose pixels
change needs `forget()` first. `find()` answers without a source, and
`holds()` asks without counting a use.

**Texels.** Each texel is four bytes, red, green, blue and alpha, rows top
to bottom with no padding. A covered pixel takes its palette entry's colour
through the display gamma, each channel the truncated channel times gamma,
clamped at 255, as the device palette has it; every other texel, the
two-texel gutter around the frame included, is zero in every byte. Colours
are premultiplied by the alpha, so the card draws every page with the
one-minus-source-alpha blend and may filter the texels without dark fringes.
What is drawn follows the frame's coverage exactly as today's drawers do: a
raw frame's pixels equal to the transparent index are not drawn, while a
row-RLE literal or run equal to it is drawn in that colour. A changed palette
or gamma means new texels: `set_palette` with a value that differs empties
the pages and starts a new `palette_generation()`.

**Draw modes.** The mode is part of the frame's identity on the pages, since
it decides the texels:

| Mode | Today | On the pages |
| --- | --- | --- |
| `opaque` | the keyed copy: a covered pixel replaces the pixel beneath | alpha 255 |
| `greyed` | the fog's gray: the colour's index through the gray table | the gray table's entry for each covered pixel, alpha 255; needs `set_gray_table` |
| `lit` | an explosion's flash: the pixel under it through the light table's row the pixel names | for each covered pixel of the light ramp, row r's share of white (r / 30, held to white) in each colour and alpha 0, which the card's lighten blend multiplies what is under it by one more than; nothing elsewhere; the same in every palette |

Translucency is not a mode: a translucent feature or shadow (`ANIM_TRANS`,
`SHAD_TRANS`, the feature shadow sprites) is the opaque cell drawn with a
vertex alpha of one half under the premultiplied alpha blend, so it costs
no second copy of the frame, and a greyed frame can be drawn translucent
the same way.

**Pages and cells.** A page is square, a power of two from `min_page_size`
(64) to `max_page_size` (2048) texels a side; the limits name the ordinary
side (`default_page_size`, 1024) and the largest side a frame too big for
an ordinary page may have of its own. Frames are packed on shelves in
cells: a cell is the frame with `frame_gutter` (2) transparent texels on
each side, its width and height rounded up to a multiple of
`cell_alignment` (2), at a corner that is a multiple of it, and the frame's
rectangle begins two texels inside the cell. That alignment is the packing
contract: a level 1 of the page computed with the exact 2x2 box holds each
frame's texels within its own cell with a one-texel gutter around them. A
frame whose cell exceeds the largest page is refused as `too_large`. A
shelf is a row of cells of one height, and a freed cell is taken again by a
later frame that fits it. `pages()` lists
every page by the index the records carry; a released page keeps its index
with a size of 0 until a new page takes it. Each page counts a `revision`
that grows whenever it is made, written or released, and keeps a `dirty`
rectangle of the texels written since `clear_dirty`, so that an upload
sends what changed.

**Memory and eviction.** `Limits::memory_limit` (`default_memory_limit`,
32 MiB) bounds the texel bytes of the pages alive. A frame that finds no
room on a page gets a new page while the limit allows one; otherwise the
least recently used frames are evicted one by one, each request and each
`find` counting as a use, until a freed slot or a released page makes room.
A page whose last frame goes is released at once. A frame whose own page
could never fit the limit is refused as `no_room` before anything is
evicted for it. `memory()` reports the page bytes, the frame bytes as whole
cells, the limit and the counts; `statistics()` counts hits, decodes,
evictions, refusals, growths and the refusals of held frames.

**Frames held through a frame, and growth.** `begin_frame` starts a frame
drawn on the battlefield: every frame placed or found from then until the
next `begin_frame` is held and never evicted to make room, so a cell a
frame's draws read keeps its frame until the frame has run. Room comes from
the frames of earlier frames; where the held frames alone fill the limit,
the limit grows, doubling or by what the page needs where that is more, up
to `Limits::largest_memory_limit`, each time the caller's `GrowthHooks`
allow the bytes it adds. Past that a new frame is refused as `no_room` and
counted as a held refusal, and the caller draws the frame without it. A
page larger than the limit itself waits for the limit to grow and evicts
nothing first. `reset_memory_limit` brings the limit back to the one the
pages were made with. Before the first `begin_frame` nothing is held, and
without growth hooks the limit never grows.

**Malformed frames.** A frame with a width or height of 0 is `empty`; one
whose pixel or coverage count differs from its size is `malformed` and
nothing is read from it. Today's blitter draws such a frame as far as its
bytes reach; the pages refuse it, since the GAF reader never produces one.
Frames whose composite children need the special blending that the reader
does not render fail in the reader, as they do for the match today, and
never reach the pages.

**Threads.** One set of pages belongs to one thread at a time; nothing
inside is locked.

### Tests

`present-gpu-world-sprite-pages` (`tests/sprite_pages_test.cpp`): the
limits, a frame's texels through the palette against the display's own
device palette at a gamma other than 1, the two modes, the row-RLE edge
cases through the GAF reader (skips, literals equal to the transparent
index, an empty row, a skip and a run past the width, composite children at
their hotspots), malformed frames and streams refused, packing without
overlap in aligned cells on power-of-two pages, eviction in
least-recently-used order under the memory limit, the frames of the frame
under way held and the limit grown as the hooks allow up to the largest,
cell reuse and shelf
release, the palette and gray-table changes with the same tables again
changing nothing, the dirty rectangles and revisions.
`present-gpu-world-sprite-pages-data` places every frame of every GAF file
of the installed game and compares the texels with the GAF reader's pixels
through the palette and with `draw_sprite`, today's sprite drawer, holding
the page bytes under the limit throughout.

### Limitations

The pages hold level 0 only: level 1, the exact half-size box the design
draws sprites from when zoomed out, follows, and the cells are aligned for
it; until then the Full tier's sprite stage samples level 0 linear when
zoomed out. The gray table is the caller's, as the palette is; the pages
build neither.

## Model meshes

`build_model_mesh` (`include/oa/present/gpu_world/model_meshes.hpp`) turns
a loaded 3DO model and the primitives prepared for drawing it
(`prepare_model` in `src/present/model`) into a `ModelMesh`:

- **Triangles.** Every primitive today's raster draws becomes a fan of
  triangles, in the prepared order: the selection primitive left out, the
  rest sorted as the model library sorts them. A textured primitive of other
  than four corners, which draws nothing today, adds nothing; nor does a
  primitive with neither colour nor texture, or a colour primitive of fewer
  than three corners. The mesh counts each. Each run (`MeshPrimitive`) names
  its corners, consecutive vertices in the 3DO's corner order, so that a
  consumer can still walk a whole primitive as today's fills do, and names
  its 3DO and prepared primitives.
- **The quad split.** Today's whole walk interpolates a quad's texels along
  its edges and then along each row, which two triangles follow exactly only
  where the quad projects to a parallelogram. The build places each
  four-cornered primitive at rest (its piece's offsets, no turn), evaluates
  that walk at the midpoint of each diagonal, and splits along the diagonal
  whose midpoint the walk interpolates nearer the mean of its two corners;
  the first diagonal on a tie, and where the quad cannot be walked at rest
  (edge-on, or not convex). Other primitives fan from their first corner.
- **Corners** (`MeshVertex`, 40 bytes, with its 3DO vertex index beside it
  in `ModelMesh::source_vertex`): the position in piece space, in world units
  of the loaded orientation (x and z negated), converted from the 16.16
  coordinates exactly while they lie within 256 units (the mesh counts the
  rest); the colour, the palette entry with the display gamma applied for a
  colour primitive and white for a textured one, with the palette index
  beside it so that a card may remap it through the shade rows exactly;
  where on the texture's frame the corner lies, 0 to 1 along each axis, the
  corners mapped in order to (0, 0), (1, 0), (1, 1) and (0, 1), which are
  the texels (0, 0), (w - 1, 0), (w - 1, h - 1) and (0, h - 1) of the frame
  shown, today's raster fetching the texel at the floor of u * (w - 1) and
  v * (h - 1); the vertex normal at rest; the piece; and the primitive's
  flags.
- **Placement.** The processor keeps transforming each piece's vertices
  (`PieceState::transformed_vertices`, from `rebuild_transforms` with the
  unit's bank, heading and pitch). A builder that draws a unit places each
  corner from the transformed vertex its `source_vertex` names, through
  `pixel_of_model_point`, which floors the 16.16 value as today's images do,
  so that every corner lands on today's pixel in every pose. The mesh keeps
  no transforms of its own: the pieces (`MeshPiece`, one per 3DO object,
  indexed like the objects) carry their parent and their offset from it, for
  what wants the rest shape.
- **Textures** (`MeshTexture`): each texture a mesh addresses, by its
  sequence name as the texture library keys it and the library's own
  `TextureSequence`, with its frame count, the size of its first frame and
  whether the others differ from it, how its frame is chosen (fixed,
  animated by the processor's cursor, or the owner's team colour) and
  whether today's samplers can read it. The frame a primitive shows is the
  sequence's frame at an index the kind gives: 0, the running frame of the
  prepared primitive's cursor (`MeshPrimitive::prepared_index`;
  `primitive_texture` gives the frame), or the owner's team colour. The
  pair of the sequence and the index names a frame of the library for as
  long as the library lives; a consumer that numbers frames for the sprite
  pages numbers these pairs.
- **Shade rows.** `vertex_normals` averages the unit normals of an object's
  drawn primitives into its vertices as the building builder does, over any
  points: the loaded vertices give the mesh's normals, which are today's for
  a piece the script has not turned, and a piece's transformed vertices give
  the normals today's builder shades a turned piece with. `shade_row` turns
  a normal, the light and its scale into the row the builder takes, with its
  truncation and its wrap of negative products. An unlit piece, and every
  mobile unit, uses row 15.
- **The projection** is `model_projection`, a 3x4 matrix: screen x = x,
  screen y = -z - y / 2, depth = y, plus the draw's depth base. Today's
  raster floors -z and y / 2 apart (`pixel_of_model_point`, from a float
  point or from the 16.16 one); a card that floors the matrix's screen y
  whole places a corner one row higher wherever the fraction of -z is below
  the fraction of y / 2.

Draw order and depth stay the processor's to state each frame: the pieces
of a unit last to first with their visibility and shaded flag, the
primitives of a piece in order, the triangles of a primitive in order; a
unit with a depth plane writes a pixel when the stored depth byte is not
above the new one, and one without writes every pixel in order.

A model is refused when it has no objects, when its hierarchy is out of
range, out of order or does not meet (the model runtime's rule), when the
prepared model was prepared for another model or its model has been freed,
when the prepared model does not match it, when a vertex index is out of
range, or when an object, a primitive or the mesh exceeds its named bound.
The build allocates nothing beyond those bounds.

### Tests

`present-gpu-world-model-meshes` checks the projection against today's
placement over 16.16 values of every sign, from floats and from the 16.16
values; meshes synthetic models (a square, textured quads over fixed,
animated and team textures, a turret with an offset child) and rasterises
them from their meshes with a small software rasteriser that walks edges and
spans as today's does, each corner placed from the instance's transformed
vertex, each primitive walked whole and triangle by triangle, holding the
pixels and depth bytes equal to today's model images in all four modes
(with and without a depth plane, lit and unlit), at rest and in turned
poses: the pieces turned and moved by their words under a heading, and
under bank and pitch with a tilted piece, where the lit rows come from the
normals of the transformed vertices. It checks the quad split on a wedge and
a kite, whose whole walk strays from the two diagonals by different amounts:
the split joins the nearer, and its triangles come nearer today's pixels
than the other split's. It checks the malformed models and the mismatched
prepared models refused, and the bytes a mesh holds.

`present-gpu-world-model-meshes-data` reads the installed game: every unit
model named by `units/*.fbi` (278 in 3.1c), meshed with the installation's
texture archives and palette and rasterised from its mesh in the frame of
today's image, in the four modes, at rest and turned (a heading with a
little bank and pitch), two ways:

- each primitive walked whole, as today's quad and polygon fills walk it,
  which holds the mesh's corners, order, textures, colours, normals and
  hierarchy to today's images exactly, in both poses: every pixel and depth
  byte of every model;
- triangle by triangle, as a card draws it, which measures what the split
  costs. At rest the silhouettes agree (the pixels one walk alone covers are
  0.01% of those drawn), 13% of the drawn pixels take a colour today's image
  has nowhere within a pixel, and the textures are sampled 1.2 texels from
  the whole walk on average, 70% of textured pixels within one texel and
  none farther than 63; turned, 15%, 1.4 texels, 67% and 98. The test holds
  each model and mode to the coverage, the share of pixels farther than a
  texel of phase and the largest texel distance, and each mode to the mean
  and the share within one texel, with bounds at rest that a fixed split
  from either corner breaks, so that the split is held as chosen.

It also prints the memory: 6.1 MB for every unit model's mesh together,
71 KB for the largest (held under 16 MiB and 512 KiB), how many quads split
from their second corner (3,215 of 23,986), the counts of primitives that
add nothing, the textures whose frames vary in size, and how many corners
the matrix's single floor would move up a row (half of them: corners at an
odd whole height).

### Limitations

The mesh holds the corners of the primitives drawn, so a frame measured
from it can be smaller than today's image, which measures every vertex of
every visible piece, the selection primitive's among them. The quad split is
chosen at rest; a turned pose may favour the other diagonal, and no split
follows today's quad walk texel for texel: the data test states how far the
triangles stray. The mesh's normals are today's for a piece the script has
not turned; a turned piece's rows need `vertex_normals` over its transformed
vertices.
