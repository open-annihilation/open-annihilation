# Platform fonts

The faces the pages select by name, standing in for the Windows fonts.

## Entry points

`oa/platform/fonts.hpp`. The library is `oa-boneyards-platform-fonts`.

## State

Reads the font files under the game's fonts folder. Writes nothing the
player saves. A face that cannot be opened leaves the pages on the bitmap
fonts.

## Invariants

The header stays `oa/platform/fonts.hpp`, the path the sources already
include. The library does not link a graphics API.

## Tests

`platform-fonts`. It reads `OA_FONTS_DIR`.

## Limitations

The metrics recorded for Arial and Haettenschweiler are numbers only. No
glyph program of those fonts is included.
