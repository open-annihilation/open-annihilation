# SDL3 platform probe

`tools/platform-probe/platform_probe.cpp` is a small presentation and audio smoke target for the
portable engine. It accepts an extracted binary PPM (`P6`) image,
uploads that image to an SDL3 texture, and presents it through a resizable
window. An optional WAV is loaded with SDL3 and queued on an
`SDL_AudioStream`. This is platform scaffolding; it is not an implementation
of the game's renderer, and the image/audio output makes no claim of fidelity
to the game.

The executable is named `oa-platform` by the project CMake configuration. A
normal run is:

```text
oa-platform --image extracted-frame.ppm --wav intro.wav
```

Use `--frames N` for a finite smoke run. The value is the number of frames to
present and may be zero; each presented frame advances by roughly 16 ms so a
queued short WAV has time to play. Without it, the window runs until it
receives a quit event, its close request, or Escape. Resizing uses SDL3 logical
presentation in letterbox mode, with the source image's dimensions as the
logical resolution. A successful window run prints the selected SDL video,
renderer, and (when requested) audio backends, followed by the number of
frames completed.

`--headless-check` parses and validates the PPM and, if provided, loads the WAV
through SDL3 without creating a window, renderer, or audio device:

```text
oa-platform --headless-check --image extracted-frame.ppm --wav intro.wav
```

The PPM parser accepts comments and `maxval` values from 1 through 255, scales
lower max values to 8-bit RGB, and bounds dimensions at 16384 by 16384 with a
128 MiB decoded-pixel limit. Input files are bounded at 256 MiB. PPM raster
bytes are treated as binary data after the mandatory header separator, so a
leading whitespace-valued pixel is preserved.

The code uses SDL3 APIs available since SDL 3.2.0: `SDL_LoadWAV`,
`SDL_OpenAudioDeviceStream`, `SDL_PutAudioStreamData`,
`SDL_ResumeAudioStreamDevice`, and `SDL_SetRenderLogicalPresentation`, so
the project's CMake asks for SDL 3.2 or newer. The `oa-platform` target links
`SDL3::SDL3`, which `tools/bootstrap_sdl.py` builds at the pinned version
into `local/deps`; SDL3 is not vendored in the tree.

Text outside the game's own fonts is drawn through FreeType from fonts that
travel with the game, the same on every platform
([text font](../../src/platform/text-font/README.md)).
`tools/bootstrap_text_fonts.py` fetches FreeType and the fonts, pinned by
SHA-256, builds FreeType as a static library into
`local/deps/freetype-install-<release>` and puts the fonts in
`local/deps/text-fonts`: DejaVu Sans Bold, DejaVu Sans,
`NotoSansCJKsc-Bold-Endonyms.otf` and Noto Emoji.
A configuration without them stops and says so
(`cmake/OaTextFonts.cmake`). Neither is vendored in the tree. The macOS and
Windows dependency bootstraps build the same FreeType for their targets.

## What a platform can do

`cmake/OaPlatformCapabilities.cmake` holds the options a platform sets
before it adds the engine. Each is on or off, and the sources that read it
are given 0 or 1.

| Option | Default | What it does |
|---|---|---|
| `OA_PROCESS_SPAWNING` | ON | the game may start other programs |
| `OA_NATIVE_FOLDER_DIALOG` | ON | the game may ask for its folder with the system's folder dialog |
| `OA_NATIVE_DENSITY_WINDOWS` | OFF | the window opens at the display's own pixel density |
| `OA_TOUCH_FIRST` | OFF | the touch controls are on from the start |
| `OA_SELF_UPDATE` | ON | the game may replace itself with a newer release through `oa-update` |

`OA_PACKAGER_NAME` names who updates a copy that cannot replace itself:
Flathub, Nix, Homebrew or a distribution. Empty names nobody.
`OA_PACKAGER_UPDATE_COMMAND` is a command a player can type, or empty. A
name or a command longer than 64 bytes, or one that holds a line break,
stops the configuration. Self-update on while process spawning is off also
stops it, because that build cannot start the helper.

With self-update off, `oa-update` is not built. The game still checks for a
release when the player lets it. The prompt and Settings say who updates
the copy ([Updates](../updates.md#copies-a-package-manager-updates)). The
repository's Nix package sets self-update off and the name Nix. iOS sets
self-update off and leaves the name empty. The official Windows package
leaves self-update on.
