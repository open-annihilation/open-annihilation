# Card command lists

The core of the Full tier's drawing, in which the graphics card draws the
battlefield itself: a command list that says what one frame asks the card
to draw, and the executor that runs it on SDL's renderer. The Full tier
(`src/app/runtime_full.cpp`) builds a frame of the whole battlefield from
the planner's draw list through its stages (`src/app/runtime_full.hpp`,
`src/app/full_fog.hpp`) and runs it here. The standard tier and the Basic
tier, which present frames the processor drew, never reach it, and no
existing behaviour changes.

## Entry points

`oa/app/card.hpp` (`oa-app-card`, no SDL) is the command list:

- `CardFrame`: one array of `Vertex` (a position in pixels of the target it
  is drawn into, a `Colour` of four floats, a texture coordinate from 0 to
  1), one array of `Index` into it, and an ordered list of `Batch`.
- `Batch`: a `draw` of a range of the indices as triangles, from a level of
  a texture page or untextured, with a `Blend` (`none`, `alpha`,
  `alpha_premultiplied`: the source's colour multiplied by its alpha
  already, as the sprite pages hold theirs; `additive`, `modulate`,
  `darken`: what is under the draw times one less the source alpha, the
  shade a shadow casts; `minimum`: each channel the lesser of the
  source's and what is under the draw, the outline of game text in the
  modern fonts over the battlefield), a `Sampling` (`nearest`, `linear`, `pixel_art`:
  how the draw reads the page where it enlarges or reduces it, the draw's
  own, so that one page is drawn differently at different zooms) and an
  optional scissor; a `clear` of a target; a `resolve`, which draws a
  render target reduced to its own size into a rectangle of another target
  by `none`, `alpha` or `alpha_premultiplied`, landed less than a pixel past
  the rectangle by its `shift_x` and `shift_y`, drawn LINEAR between the
  pixels; or a `blend_reduce`, which
  draws a part of a render target's texture, on even pixels, into a
  rectangle of another target by the two-level blend: the texture's half,
  each pixel the mean of four, LINEAR at twice the scale, then the part
  LINEAR over it at alpha `1 - log2(1 / scale)`, the scale across from one
  half to 1, so that at one half the result is the box of four pixels, at
  1 the part itself, and between the weights change evenly with the scale.
  Every batch names the target it draws into: a `TargetHandle`, or none
  for the frame's final target.
- `append_quad` adds a quad as two triangles that share the diagonal from
  the top-left to the bottom-right corner. `check_frame` says what is
  malformed about a frame; the limits it holds a frame to are the named
  constants beside it.
- Pages hold `Texel`s: four bytes, red, green, blue and alpha, in that
  order in memory, which is the order the terrain and sprite pages hold
  their texels, so a page is uploaded as those pages hold it. A page drawn
  by `alpha_premultiplied` holds colours multiplied by their alpha already.

`oa/app/card/executor.hpp` (`oa-app-card-executor`, SDL) is the executor:

- `Executor::open(renderer, texture_limit)` binds it to a renderer. The
  texture limit is the caller's: the render policy's corrected limit, so
  that a page or target beyond it is refused with an error naming the size
  and the limit before SDL is asked. `open` composes the darken blend mode
  (`SDL_ComposeCustomBlendMode`, source factor zero and destination factor
  one less the source alpha) and finds whether the renderer takes it
  (`Capabilities::darken_composed`); where it does not, darken runs as
  SDL's multiply mode over a copy of the batch's vertices with their
  colours black, which gives the same pixels. It composes the minimum
  blend mode too (source and destination factors one, the minimum
  operation) and finds whether the renderer takes it
  (`Capabilities::minimum_composed`); where it does not, as on SDL's
  software renderer, a minimum draw blends as alpha. The lighten blend is
  SDL's multiply mode on every renderer: what is under the draw times one
  more than the source colour less the source alpha, which a source of no
  alpha makes what is under it times one more than its colour. On SDL 3.4 and later it also
  tells the renderer to clamp texture coordinates, so that no draw makes
  the renderer read every vertex to decide, and finds whether the renderer
  takes the pixel-art sampling mode (`Capabilities::pixel_art_sampling`);
  where it does not, or on SDL before 3.4, a `pixel_art` draw reads the
  nearest texel.
- `create_page`, `update_page` (the whole level or a part, from bytes with
  a pitch in bytes) and `destroy_page`. A page has 1 to `most_page_levels`
  levels, each a texture of level 0 halved once more (`level_edge`), which
  the engine fills itself: the caller supplies each reduced level, filtered
  by its own exact filter, and a draw names the level it reads, so a
  reduced draw shows the same texels on every renderer and the zoom-out
  filter stays the processor's. The textures are SDL's `RGBA32` format,
  the texel order above, which SDL's `metal`, `direct3d11`, `direct3d12`,
  `vulkan`, `opengl`, `opengles2` and `gpu` renderers hold as it is; its
  `direct3d` and `software` renderers hold such a page as an `ARGB8888`
  texture and reorder the bytes of each upload, which a page uploaded once
  can bear.
- `create_target(width, height, factor, keep_half)` and `destroy_target`. A render
  target is a texture of the size times the supersampling factor (1, 2 or
  4), drawn into with vertices in pixels of the size, and reduced by a
  resolve: by one LINEAR draw at factor 2, which averages each square of
  four texels exactly, and through a half-size texture at factor 4, so that
  every sample counts. Into another render target a resolve halves its
  source only until its factor is at most twice that target's, so that a
  source at the target's own factor lands texel for texel, moved by its
  shift between them; the Full tier moves its zoomed-out battlefield that
  way before reducing it to the window. A new target, and the half, is cleared to a check
  colour and one pixel is read back before it is trusted, since a texture a
  driver failed to make draws black and reports no error; a pixel of
  another colour refuses the target with an error naming the read-back. It
  is then cleared transparent. Drawn into by `alpha_premultiplied` and
  resolved by it, a transparent target composites each partly covered
  pixel by its coverage, which is what anti-aliased models need later.
  With `keep_half` the target keeps the half at any factor, which a
  `blend_reduce` reads; the half's pixels count with the target's. The
  Full tier's world target (`src/app/runtime_full.cpp`) is one: the
  battlefield drawn at the Enhanced anti-aliasing row's factor and reduced
  by `resolve` from zoom 1 up, or drawn at zoom 1 and reduced by
  `blend_reduce` below it.
- `execute(frame, final_target)` checks the frame whole, `check_frame` and
  then every page, level and target it names, refuses it with nothing drawn
  when anything is wrong, and otherwise runs the batches in order: one
  `SDL_RenderGeometryRaw` for each run of draw batches that share their
  target, page, level, blend, sampling and scissor and follow one another
  in the indices, the vertex and index arrays passed by pointer and stride
  with no copy; the render target, scissor, blend mode and sampling mode
  set only when they change. A factor-4 target is halved once for its
  resolves in a run, and again after any draw, clear or resolve into it.
  When it returns, the render target is `final_target` again, with the
  scissor, draw colour and draw blend mode it found there. Every failure is
  in `error()`, and `frame_refused()` tells a frame refused before anything
  of it was drawn, which the frame's builder caused, from a call to SDL
  that failed; `counts()` holds the calls, merges, state changes and
  texture bytes for checks and the frame statistics.
- `retire_page(page)` destroys a page once the next frame has run, refused
  or not, or at `close`: a stage that replaces a page while its frame is
  built retires the old one, which batches of that frame may still name
  and draw from as it was.

## State

The executor owns its pages, targets and the scratch copy the darken
fallback uses, and nothing else: no process-wide state, so two executors on
two renderers never meet. It reads no game state; the frames it is given
are built elsewhere.

## Invariants and limits

- A frame that is malformed or names what does not exist is refused before
  any call to SDL, and a frame that fails part way, which only a failed SDL
  call can cause, leaves the final target bound with its state put back.
- Handles carry the slot's generation, so the handle of a destroyed page
  or target misses the slot however it is reused.
- Pages and targets are refused beyond `largest_page_edge`,
  `largest_target_edge` and the caller's texture limit, and at
  `most_pages` and `most_targets` alive; frames beyond
  `most_frame_vertices`, `most_frame_indices` and `most_frame_batches`,
  and vertices beyond `largest_coordinate` or not finite. The frame limits
  bound only the memory a frame built wrong could take: SDL takes any count,
  and a battle of thousands of units at the widest zoom stays far below
  them.
- On SDL's software renderer, which runs the Full tier only in checks, an
  axis-aligned quad of one colour becomes a texture copy, so a draw's
  sampling mode applies to it, with `pixel_art` read as nearest, and an
  untextured one is always drawn by alpha, whatever its blend; everything
  else is filled as triangles, the nearest texel read whatever the
  sampling mode, and a textured triangle that is not such a quad is drawn
  by `none` where its blend is `alpha_premultiplied`. The quirks affect no
  black shadow quad, whose alpha blend equals darken, no opaque quad, and
  no sprite quad, which is a quad of one colour. On that renderer pages
  are streaming textures, since it run-length encodes a static one and
  each copy of a part of it then scans the whole page; on every other
  renderer they are static.

## Tests

`app-card-frame` (`tests/card_frame_test.cpp`) runs frames on SDL's
software renderer over a surface, with no window, and reads them back
against a reference rasteriser on the processor, which samples pixel
centres with the top-left rule, interpolates and truncates colours and
texel coordinates exactly and blends in whole levels: opaque draws equal
it; every blend mode, premultiplied alpha from a premultiplied page and
from vertex colours among them, a scissor, a page's levels and a part of
one updated, and render targets at factors 2 and 4 resolved by none and by
alpha keep every channel within 2 levels and 0.5 on average, the room the
renderer's own rounding of each blend takes. A transparent target drawn
into and resolved by premultiplied alpha composites a half-covered pixel
at half the canvas, which it checks against the value as well as the
reference; a factor-4 target cleared and drawn again between resolves
shows each new content; a target resolved into another at its own
factor, 2 or 4, is halved never and lands texel for texel; a draw's
sampling mode is set on the page only when it changes and keeps batches
apart; a new target reads back transparent. A frame of edges between a target's pixels at factors 2 and
4, resolved, lands each edge pixel at the share of it the drawing covers,
one of two columns at 127 and one or three of four at 63 and 191, and
matches the reference reduced by halving exactly; the two-level reduction
of a target that keeps its half, by one half, where it is the box of four
pixels within the level the renderer's own halving may lose, by three
quarters under a scissor and nine tenths of a part within, and by 1, at
factors 1 and 2, keeps within the tolerance of the reference, which
stretches as that renderer's LINEAR does, in 16.16 positions with 7-bit
fractions, rows then columns, truncated once, and blends a texture at an
alpha as it does, with the half made once a run. It checks malformed
frames, a two-level reduction's among them, and frames naming destroyed
pages or a target made without its half refused with nothing drawn, a
retired page drawn in the next frame and gone after it, a frame of more
than a million vertices run whole, pages and targets beyond the limit
refused with an error naming both, batches merged into one call, and the
renderer's state put back; and it measures
and prints the processor cost of frames of 5,000 and 20,000 quads, sprite
quads of one colour and lit quads filled as triangles, and the texture
bytes of the supersampled targets, which it never checks.

## Known limitations

- Wired into the match for the Full tier (`src/app/README.md`): the Full
  tier (`src/app/runtime_full.cpp`) draws the whole battlefield through
  the executor, the terrain from the terrain atlas's pages, the fog's
  passes, the sprite and model stages' batches and the painters'
  darkening quads, with the painters' overlay canvas laid over it.
- The frames the Full tier runs here are held to the processor's
  picture on SDL's software renderer alone (`native-render-tiers`,
  `src/app/README.md`); the golden images of whole frames the design
  names are not made yet.
- The composed darken and minimum modes and the pixel-art sampling mode
  are exercised only on renderers that take them; the software renderer
  takes none of them, so the fallbacks are what the test covers.
- The one-pixel read-back of a new render target copies the whole target
  into memory on SDL's `direct3d` renderer, once at creation. The design's
  4x4 check target for that renderer, drawn into from the new target and
  read back instead, waits for the slice that wires the fallbacks.
