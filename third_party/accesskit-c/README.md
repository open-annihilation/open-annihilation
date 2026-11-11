# AccessKit C bindings

The C bindings of AccessKit, which the screen-reader bridge
([src/platform/screen-reader](../../src/platform/screen-reader/README.md))
calls. The header here is the one the release publishes, so a build compiles
without the archive. The libraries are not in the tree: `tools/bootstrap_accesskit.py`
fetches them.

- Release: <https://github.com/AccessKit/accesskit-c/releases/download/0.23.2/accesskit-c-0.23.2.zip>,
  version 0.23.2 (tag `0.23.2`, 2026-11-01), SHA-256
  `c7fa84f8f34fca909f662375737a54f98b17cf9eae51fffd94ab3d9f662e3681`.
- Source, for a target that release does not build:
  <https://github.com/AccessKit/accesskit-c/archive/refs/tags/0.23.2.tar.gz>,
  SHA-256 `dca8250dad6b2d21b6bf9bf6bc9e7fa6ede0ff21c0858e952b912d9ddf50ffc8`.
- `accesskit.h` is that release's `include/accesskit.h`, unchanged.
- Licence: MIT, chosen from MIT OR Apache-2.0. The texts are `LICENSE-MIT`
  and `LICENSE-APACHE` here, and
  [`licenses/AccessKit-MIT.txt`](../../licenses/AccessKit-MIT.txt),
  [`licenses/AccessKit-Apache-2.0.txt`](../../licenses/AccessKit-Apache-2.0.txt).
  Parts derived from Chromium keep the BSD-style terms in
  [`licenses/AccessKit-Chromium.txt`](../../licenses/AccessKit-Chromium.txt).
  Every crate in each target's library is listed in
  [`licenses/AccessKit-crates.txt`](../../licenses/AccessKit-crates.txt).

The release's Meson files are not part of this copy, and the build does not
use them.
