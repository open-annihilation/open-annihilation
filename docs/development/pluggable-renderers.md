# Pluggable renderers and a classic-first 3D view

Status: the live camera is enabled by default through Graphics settings.
This branch also includes an offscreen renderer comparison.
The classic orientation uses the existing renderer. Optional lighting and
heat effects work in the classic and live 3D views.

## Intended experience

Every match opens in the familiar classic projection. Turning off the optional
effects preserves the existing engine picture. Full acceleration permits mouse panning and constrained
rotation. An explicit **Unlock camera** setting permits angles that expose
the limitations of painted terrain and sprite artwork. **Reset view** always
returns to the exact classic projection. All additional effects are optional
and enabled by default, with separate Graphics switches in either view.

## Fit with the engine

The [Full tier](../../src/app/runtime_full.cpp) already uses
[GPU asset preparation](../../src/present/gpu-world/README.md) for terrain,
sprites and model meshes. Its [card commands](../../src/app/card/README.md)
carry projected 2D vertices. Keep that working path while introducing a
renderer seam before projection and classic draw ordering.

Use a `RendererHooks` table with a context pointer and operations for
capabilities, resource creation, resizing, drawing and teardown. The app
owns renderer selection, input and fallback. Renderer implementations read
canonical state and shared decoded assets; they own graphics resources and
presentation-only caches. They must not change simulation, saved-game
layouts, gameplay visibility or random streams.

Start with built-in registered renderers: the existing classic paths and an
experimental 3D path. Defer dynamic library loading and a stable external
binary interface. Future extension registration should use the existing
[extension boundary](../../src/app/include/oa/app/extension.hpp), with the
required contract version change. Graphics access must follow the engine's
[runtime loading rule](conventions.md#link-no-graphics-api).

Separate renderer capabilities from the Full setting: existing Full support
does not imply support for depth buffers, programmable materials or heat
distortion. Refuse unsupported options clearly and retain the classic
fallback. A fallback resets camera orientation while retaining its map
focus; the match continues unchanged.

## The main visual risk

The [classic camera](../../src/present/world-renderer/src/camera.cpp)
projects approximately as `screen_x = x`, `screen_y = z - height / 2`,
before camera offset, zoom and integer rounding. A conventional overhead
camera will not reproduce that picture.

Give the 3D renderer an explicit classic projection. Investigate terrain
texture coordinates derived from the reference projection, so heightfield
geometry does not shift the painted map in the initial view. Keep the baked
terrain shading as the base colour; do not add a second directional-light
pass by default. Additional lighting is a separate contribution.

This needs a proof: cliffs, occluded ground and sprite features may not have
enough artwork to look correct from arbitrary directions. Constrained motion
limits that exposure; unlocking accepts it. Existing Full shading and
sampling approximations also mean that Full output alone is not a sufficient
reference for base-game visual parity.

## Camera proposal

Keep existing selection and command bindings. Trial middle-drag for pan,
a modifier plus middle-drag for orbit, and wheel zoom anchored to the ground
beneath the pointer, after checking current binding conflicts.

Trial yaw limits of plus or minus 45 degrees from the classic orientation,
no roll, and elevation changes of plus or minus 20 degrees around the
reference view. Continue evaluating these limits on real map art.
Unlocking permits full yaw and a wider elevation range, while retaining
terrain clearance and valid near/far planes. Re-locking eases into the valid
range. Camera state belongs in presentation preferences, outside packed game
records; a new match still starts in the classic orientation.

Picking, drag selection, build placement, command markers and the minimap
view footprint must use the active projection. Share project/unproject
operations with the input layer. Screen-space UI remains unaffected.

## Three small proofs of concept

| Proof | Small demonstration | Evidence to keep | Decision it enables |
|---|---|---|---|
| Classic view and renderer seam | Select either renderer at launch for one frozen scene: terrain with a cliff, a unit, a tree and a wreck. The experimental path uses depth and world geometry but starts at the classic projection. | Matching captures and a difference image at native zoom; label comparison against the existing engine separately from comparison against the base game. Record positional, colour and occlusion differences. | Whether the seam and terrain projection are viable before expanding scope. |
| Constrained camera | Pan and orbit that same scene within limits, unlock, then reset. Add a ground cursor, one build footprint and a minimap footprint. | Short capture showing the worst permitted angles, cursor alignment and exact return to the starting view. Repeat on a second map with strong baked shading. | How much motion the original artwork tolerates, and whether picking stays usable. |
| Optional effects | One tank firing near another, one destroyed unit and one builder constructing a unit. Toggle each effect independently. | Side-by-side effects-off/on captures, a cooling sequence, frame time and light counts; include a hidden source behind fog. | Whether the effects improve readability within a bounded cost and without information leaks. |

The first experiment has an offscreen implementation described below; the
camera and effects experiments remain proposals. Reuse locally
installed game assets and keep captures out of Git. Use the real application
through `run.sh` for interactive review; automated captures remain headless.
Do the first proof before committing to a graphics backend or broad refactor.

## Optional lighting and heat effects

Graphics offers four independent switches, enabled by default: **Coloured
weapon lighting**, **Nanolathe glow**, **Wreck glow** and **Heat distortion**.
They apply to both views and persist with the graphics preferences. Disable
all four when comparing the original appearance or measuring their cost.

- **Wreck heat:** emissive hot areas and optional local light, fading with
  simulation time after destruction. Track identity across wreck replacement
  and reclamation; do not attach heat to a reused object slot.
- **Weapon lighting:** brief muzzle lights, moving projectile lights and
  impact splash lights that illuminate nearby units and terrain.
- **Nanolathe lighting:** a bounded set of lights along the stream and near
  its endpoints, illuminating the builder or factory and the work site.
- **Heat sizzle:** local depth-aware distortion around hot weapons and
  smouldering wrecks, excluded from the HUD and independently disabled.

Use capped light counts and finite radii; shadowed dynamic lights and bloom
are deferred. Visibility-filter sources before collecting lights or distortion,
and prevent effects from brightening unknown terrain. Cooling uses simulation
time so pause and replay speed behave sensibly. For the proof, loading or
seeking clears transient effects rather than inventing destruction history;
a later implementation must say how a loaded match brings them back without
changing save bytes.

## Sequence and acceptance

1. Prove the renderer seam and classic projection with effects disabled.
   Resolve visible alignment or occlusion errors before broadening coverage.
2. Prove constrained motion and all cursor-to-world operations; tune limits
   against representative terrain and sprite art.
3. Prove optional effects independently, including fog, pause, reset and
   renderer fallback. Compare frame cost with the same scene and effects off.
4. Only then plan production coverage: remaining assets, water, shadows,
   transparency, resource recovery, platform backends and settings integration.

The first production gate is classic-view parity, not a dramatic free-camera
shot. Future implementation checks should include reference captures, camera
round trips, picking on slopes, unchanged simulation digests, visibility
isolation and graphics-device failure. No production readiness or performance
claim follows from the small proofs alone.

## Running the first proof

Build the native application in Check mode, using an installed game:

```sh
OA_BUILD_TYPE=Check ./run.sh --game-dir "$OA_GAME_DIR" \
  --check-renderer-proof --snapshot local/reports/renderer-proof \
  --preferences-file local/reports/renderer-proof.conf
```

This check opens no window. On macOS it creates an offscreen Metal device
through SDL. Do not set `SDL_VIDEO_DRIVER=dummy` for the real GPU comparison:
that driver has no Metal device. A missing supported GPU returns exit 77;
other platforms currently have no proof shaders. Normal play still uses the
existing renderer. This check is available only with self-checks built in.

The native check selects a deterministic relief crop from an installed map,
prepares terrain geometry and model textures, and compares three renderers:
the existing classic rasteriser, a CPU depth reference, and an experimental
GPU path with world-space vertices, a depth buffer and alpha cutouts. The
provisional hook table selects the proof implementations; it is not yet a
public world-renderer plugin interface or a gameplay renderer setting.

The output directory contains `classic.png`, `cpu-depth.png`, `gpu.png`,
`difference.png`, `comparison.png`, terrain-only images, and `report.txt`.
The comparison panels are classic, GPU, then absolute RGB difference amplified
four times. The report separates terrain parity from scene parity. A successful
exit requires exact terrain RGB agreement; it does not imply full-scene parity.
Device creation and readback timing is reported separately from steady-state
frame cost, which this experiment does not measure.

The classic reference is the engine's existing rasteriser, not a capture of
the base game. The terrain mesh deliberately keeps painted
coordinates; alignment to the simulation's actual heightfield and correctness
from other angles remain to be established. Model shading, classic polygon
edge rules, shadows, animation, water and fog are not covered by this small
scene. These gaps must be resolved before calling the renderer visually
indistinguishable or enabling it in gameplay.

### First measured result

On the local Metal device, the Greenhaven crop at (4608, 2944), with heights
80 through 222, an ARM Flash, its wreck and the `blaktree` sprite produced:

| Comparison at 640 by 480 | Changed RGB pixels |
|---|---:|
| Classic terrain versus GPU terrain | 0 |
| Classic scene versus GPU scene | 837 |
| CPU depth scene versus GPU scene | 43 |

These counts describe this installed-asset fixture, not general parity. The
visible scene differences concentrate on the models; the next investigation
should isolate model edge coverage, texture interpolation and depth ordering
before expanding the renderer. The 43 CPU/GPU differences also need resolution
before the CPU raster can serve as an exact reference for the GPU. Base-game comparison remains outstanding. The live camera below extends
the proof, with separate input and visibility checks.


## Interactive Skirmish preview

With Allow Full 3D Mode enabled, the renderer draws the current match's terrain, visible units,
wrecks and sprite effects. Start the normal application, then choose Skirmish:

```sh
OA_BUILD_TYPE=Check ./run.sh --game-dir "$OA_GAME_DIR" \
  --hardware-acceleration=full
```

The initial view uses the existing renderer. Option+primary-click-drag
orbits within 45 degrees of yaw and 20 degrees of elevation change;
Option+Shift+primary-click-drag pans. Option+U toggles the wider camera
range, and Option+R restores classic orientation and normal play. These
controls work with Mac trackpads; other platforms use Alt instead of Option.
Alt/Option+middle-drag also orbits; middle-drag pans when middle-click pan is enabled.
F2 (Fn+F2 on Macs with media keys) opens the menu. With “Escape opens the game
menu” enabled in Settings, Escape opens it after cancelling any active command
or selection. Ordinary selection, box selection and unit commands
use the rotated camera's visible unit geometry and terrain intersection.
Selection bounds and building footprints are projected into that view.
Mouse-wheel and fractional trackpad scrolling zoom around the terrain point
under the pointer. Unlocked Camera allows wider angles without changing the
starting orientation. Reset returns to the classic renderer while preserving zoom.
Hold Shift to show projected order paths, range rings and queued build sites;
queued build-site dragging follows the rotated view. Backtick toggles health
bars at the projected unit positions; squad digits and self-destruct counts
also follow the units. A complete building ghost remains incomplete. Returning
to the menu discards the camera orientation. The saved Unlocked Camera setting
applies when the next camera gesture begins.

Low surface features, such as metal spots, follow the drawn terrain
triangles as decals; standing sprites such as trees remain upright. Fog
brightness blends across the mapped side of visibility boundaries, while
unmapped cells remain black and hidden units stay excluded. Unit shadows
use the existing cached silhouettes, projected onto the terrain.
Ground structures level the rendered terrain beneath their gameplay footprint,
and beneath their model's base piece where it reaches past the footprint, to
their placement height, with a small separation beneath floor plates and
a one-cell transition to surrounding terrain. A base piece whose floor plates
lie below the placement height, as the Arm aircraft plants' do, takes the
ground down below its lowest corner, by up to a map pixel, so that the
terrain hides no part of the base at any angle; other pieces sunk into the
ground stay covered. Ground decals and shadows follow
that foundation. Placement previews show the same leveling; simulation terrain
and model positions remain unchanged.
The terrain crop follows the viewport across the full terrain height range.
Foundation leveling queries nearby footprints through spatial bins rather
than comparing every terrain vertex with every building.

The live model geometry uses the same interpolated presentation poses as
the classic view, including piece movement. Construction particles retain
their spatial positions and glow green; units under construction emit green
light, and nearby model surfaces receive a bounded contribution from visible
construction particles. Shared local lights also illuminate the rendered
terrain and units in either view; a full bloom pipeline remains deferred.
Construction emission and glow gently pulse by 15 percent over two simulation
seconds, freezing when paused. The glow fades as the unit finishes and obeys
the Nanolathe glow intensity in both views.

Visible shots produce coloured muzzle, projectile and impact lights. Shot
events are captured before retirement, including shots that last less than
one tick; explosions without a weapon event use warm splash lighting.
Ground-level explosion sprites keep their camera-facing shape and clear the
rendered terrain, including slopes and leveled foundations. Transparent frame
padding does not raise them; airborne explosions and projectiles keep their
altitude. Ground flash masks remain draped over the terrain.
Newly observed wrecks cool over 900
simulation ticks, fading from orange toward red. Existing wrecks loaded from
a save start cold. Wrecks and hot weapons can distort nearby battlefield
pixels; selection markers and HUD elements are drawn afterward. The 3D pass
uses scene depth to limit light reach and protect foreground silhouettes.
Classic rendering has no depth buffer, so its effects use screen-space
falloff and visibility masks rather than physical occlusion. Neither path
casts dynamic light shadows. Source counts and radii are bounded.
The four effect intensity sliders run from 0% (off) to 200% in 10% steps.
100% preserves the standard appearance; intensity does not alter source
lifetime, light radius or construction progress. Heat distortion increases
its maximum sampling displacement from two pixels at 100% to four at 200%.

Classic Full acceleration retains its GPU renderer and applies effects to
its battlefield before overlays. This currently requires a synchronous
readback and upload of the area reached by visible sources, including the
heat sampling border. Its frame cost still needs evaluation in large battles. With all four effect intensities at zero this
extra pass is skipped.

Terrain debug commands, including `+contour`, use projected terrain cells.
Their overlays are restricted to visible terrain pixels so they do not paint
over units. Order markers remain overlays, as in the classic view.

| Setting | Live 3D behaviour |
| --- | --- |
| Enhanced anti-aliasing | Supersamples the whole 3D battlefield, then box-filters it on the GPU before readback. The requested factor is lowered when needed to fit a 4096-pixel target edge. This differs from classic per-unit supersampling. |
| Unit, vehicle and feature shadows | Uses the existing graphics flags and shadow level. |
| Mapping and line of sight | Uses the match's visibility data, with a smooth visual boundary. |
| Dithered fog | Uses the classic grey palette for mapped-but-unseen terrain; live boundaries remain smoothly interpolated. |
| Smooth motion | Uses the existing presentation fraction for model movement and animated pieces. |
| Gamma | Uses the existing presentation conversion. |
| Classic model shading | Buildings drawn from their picture take the classic shade rows, the bright page above the unlit row and the shade table's colours, as the classic Full view shades them; mobile units and moving pieces stay unshaded, as there. A pictured unit's polygons of the image key, the walls that hide its pieces below the ground, draw nothing: the terrain hides those pieces. |
| Cloak blending | A cloaked unit, or every unit under the debug overlay, is drawn at half opacity, as the classic view blends its picture, after the opaque scene so that its depth hides nothing behind it, and stays pickable. Its own faces blend one by one, where the classic view blends its whole picture once. |

The preview draws through SDL's GPU interface on Metal or Vulkan: SDL
chooses Metal on Apple systems and Vulkan elsewhere, and `SDL_GPU_DRIVER`
names one. Each stage is written once, in Vulkan's GLSL, in
`src/app/shaders/`; `tools/make_live_shaders.py` compiles it to SPIR-V and,
through SPIR-V, to Metal shading language, in
`src/app/renderer_live_shaders.inc`, which is committed, so the build needs
no shader tools, and the `live-shaders` check makes it afresh where the
tools are installed. Windows's own graphics interface has no form yet: on
Windows the preview needs a Vulkan driver. On a Mac, Vulkan through a translation layer such as
MoltenVK runs the SPIR-V for testing:
`SDL_GPU_DRIVER=vulkan SDL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib`.
It reuses its device,
pipeline, atlas and frame buffers, but each frame waits for GPU readback and
uploads the result through the existing presentation path. Performance is
provisional. The live path omits the unused classic battlefield raster, and
its effect visibility raster only visits rows and columns reached by effects.
Zooming out reduces fog subdivision, merges planar terrain cells, and fades
subpixel decals and particles. Small light footprints fade out; heat distortion
fades sooner as its pixel displacement becomes imperceptible.
Terrain minification blends averaged texture levels, with each tile clamped to
its own texels. Level selection uses output pixels independently of the
anti-aliasing factor; sprites retain nearest sampling.
Terrain vertices use the gameplay world height grid. Their artwork is sampled
at the original projected map coordinates, splitting triangles at texture tile
edges. Ground features retain their canonical world anchors. Effect sprites
face the camera at their actual altitude. Light-table flashes
are translucent terrain decals, approximating classic relighting. Construction
starts with polygon-edge nanoframes, then fades in textured faces as work
finishes. Some projectile and effect types remain incomplete, and textured
faces are drawn as triangles rather than as the classic view's strips.
Oblique views expose stretched artwork on surfaces missing
from the original painted view. The rotated view does not meet the classic
parity gate.

The windowless live check starts an actual Skirmish and exercises orbit,
selection, menu keyboard navigation, release of camera drags across menus,
terrain picking, orders, pointer-anchored zoom, queue overlays,
contours, projected health bars, interpolated movement, factory build subjects,
construction particles, coloured light on terrain, advancing simulation,
unlock, exact classic reset, both zoom limits, recovery after GPU rejection
and target resize:

```sh
OA_BUILD_TYPE=Check ./run.sh --game-dir "$OA_GAME_DIR" --check-live-renderer
```

Use the native video driver for this check; the dummy driver cannot create
a GPU device. An unavailable backend returns 77, while rendering failures
fail the check. The module checks separately cover camera projection, live
scene visibility and atlas invalidation, and persistent GPU resources.


## Inspecting weapon effects

For a short automatic battle, use the existing [battle showcase](../capture.md):

```sh
OA_BUILD_TYPE=Check ./run.sh --game-dir "$OA_GAME_DIR" --skip-intro \
  --hardware-acceleration=full \
  --showcase skirmish-battle --combat 8
```

It starts armies automatically and runs for about one minute. Orbit using
Option+drag while they fight. The showcase adds its busy-combat units too,
so `--combat 8` is the main army size per side, not the total unit count.

For a controllable test, start a normal two-player Skirmish. Point at open
ground, open chat with Enter, type `+armstump 0`, and submit. Point nearby
and enter `+armstump 1` to create an opponent. Select your tank and order
it to attack the other tank. `+nowisee` removes fog for inspection; enter
it again to restore the visibility switches. These are existing
[cheat commands](../cheats.md), available in single-player Skirmish.

This exercises the weapon animation, supported projectile/explosion art
and wrecks that the preview currently draws. Some projectile types are
still missing. Use the four Graphics switches to compare the shot and
impact lighting, cooling wreck glow and heat distortion in either view.


## Vegetation beyond a single sprite

For trees and animated plants, directional billboards are a promising next
proof, not implemented by this branch yet. Preserve the supplied sprite at
the exact classic view. For the constrained camera, first author front-left
and front-right views around that sprite; a fully unlocked camera can use
eight azimuth views and additional elevation rows. Animated plants need
the same directional coverage for each animation frame.

Where the installed asset has only one view, extra views cannot recover
hidden surfaces automatically. A small authored mesh or estimated-depth
volume can provide them, with the original front image kept as the visual
reference. Simply extruding the alpha silhouette into voxels gives thickness
but invents the unseen shape and side colours. This may suit rounded plants
better than thin branches.

The general method is an image-based impostor: substitute suitable images
as the viewpoint changes. See NVIDIA's [True Impostors](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-21-true-impostors)
for a depth-aware extension. A useful bounded proof is one tree and one
animated gasbag, comparing the original view, constrained orbit and an
unlocked orbit before building an asset conversion pipeline.
