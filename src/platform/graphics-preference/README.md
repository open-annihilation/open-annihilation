# Graphics preference

The graphics card the game asks for on a Windows laptop that has two: the
discrete one. Target `oa-platform-graphics-preference`, built on Windows
only, with no header: nothing in the game calls what is here.

## What it does

On a laptop with NVIDIA Optimus or AMD switchable graphics, the drivers start
a program they have no profile for on the processor's own graphics, which
draws the game far more slowly, unless the program's executable exports
one of two values set to 1:

- `NvOptimusEnablement`, a 32-bit unsigned value, read by NVIDIA's drivers;
- `AmdPowerXpressRequestHighPerformance`, an `int`, read by AMD's.

[graphics_preference.cpp](src/graphics_preference.cpp) defines both, exported
as C names. They decide the adapter of the window's renderer, which SDL
makes on the adapter the system offers first; the 3D view's graphics device
already asks SDL for the high-performance one, which ranks a discrete card
first. The `+stats` overlay names the adapter of each
([src/app](../../app/README.md)).

The player's own choice for the program wins over the request: Windows'
Settings › System › Display › Graphics, and the program settings of NVIDIA's
or AMD's control panel. On a computer with one graphics card nothing reads
them.

## Invariants

- The drivers read the export table of the executable that started the
  process, never a library's, so the values are objects of
  `open-annihilation.exe` itself. The module is an object library that
  `oa-game` links directly, which puts its object in the program whole: an
  object of a static library that nothing calls would be left out of the
  link. Linking it into a library, or into another program, asks for
  nothing.
- Exported names are roots of the link, so `--gc-sections` and the Visual
  Studio linker's removal of unused data keep them, and stripping the
  program's symbols leaves its export table as it was.

## Tests

None run automatically: the drivers' choice cannot be seen from inside the
game, and a computer with one graphics card ignores the values. The export
table of a built `open-annihilation.exe` lists both, as
`x86_64-w64-mingw32-objdump -p open-annihilation.exe` shows; a program
without them exports nothing.

## Known limitations

- The values are a request. A profile the driver has for the program, or a
  laptop set to use only one of its graphics cards, overrides it, as the
  player's own choice does.
