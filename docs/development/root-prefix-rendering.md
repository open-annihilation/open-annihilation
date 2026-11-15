# Experimental root-prefix rendering

`--prototype-root-prefix` is off by default. It permits a different body
rendering and picking policy in Full, undirected, locally simulated Enhanced
10 and 11 matches. It does not choose a camera, change the default preset, or
change simulation, unit scripts, effects geometry, saves, or random numbers.

The renderer retains an owned exact model prefix with the final root XZ turn
removed. It supplies a signed Q30 sine/cosine pair and root placement for each
draw. The GPU finishes that turn using 32-bit integer pairs; it does not
require shader 64-bit arithmetic. CPU queries use the same nearest-even Q30
recipe. Within the admitted raw coordinate range of 512 world pixels, its
final root coordinates differ from the native double calculation by at most
one 16.16 coordinate unit. The prefix never contains approximated coordinates
from an earlier rendered frame.

Admission is deliberately narrow: completed ordinary ground robots with no
attachments, cloak, digger, hovering, floating, aircraft or building policy.
Packets have at most 64 pieces and 2,048 points, with no root siblings. Root
placement must stay within the existing no-wrap local and world limits;
projection, water, fog, materials and lighting must remain supported. A miss
or unsupported draw takes the existing native geometry and image path before
Scene workers read it. Simulation effects still refresh their canonical
native geometry when they need it.

The new body policy submits the selected visible pieces' triangles and uses
GPU clockwise front faces. It can submit more indices than CPU polygon
culling. Hover picking uses the same owned Q30 points, front-face rule,
texture alpha and depth ordering, with a bounded triangle test that rejects
ill-conditioned slivers. Selection bounds are conservative integer proxies
centred on the displayed unit anchor. Existing box-selection centres
therefore follow that anchor. This policy does not promise pixel-identical
legacy body images or the old sliver picks.

Tests cover all native heading words against the bounded Q30 recipe, actual
windowless GPU surfaces and shadows, canonical geometry debt and fallback,
coarse point mapping, bounded picking, disjoint worker ranges and retries.
The ordinary default-off 51-row renderer signature corpus remains unchanged.

One matched private 20,000-unit 3D case improved from 13.473 to 14.990 FPS.
That is not 60 FPS. This implementation still recomputes piece bounds and
per-frame Scene data; it includes no same-frame bounds memo. Offline or
synthetic CPU measurements are not foreground gameplay FPS. Director mode
does not admit this prototype, and ray tracing is deferred.
