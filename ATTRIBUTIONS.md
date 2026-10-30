# Attributions

Open Annihilation is licensed under the GNU General Public License version 3
only (GPL-3.0-only; see [`LICENSE`](LICENSE)). The release packages also
contain the third-party software listed below. Each component keeps its own
licence, and the notices those licences require are reproduced here. Full
licence texts are in the [`licenses/`](licenses/) folder of this repository.
A build copies this file, `LICENSE` and the `licenses/` folder beside
`open-annihilation`, `oa-intro` and `oa-tool`, and on macOS into the game's
bundle, `open-annihilation.app/Contents/Resources`, so the links below also
resolve there.

No Total Annihilation or Total Annihilation: Kingdoms game data is included
in this repository or in the release packages.

## What each package contains

| Component | Version | Licence | macOS | Windows | Linux |
|---|---|---|---|---|---|
| [SDL](#sdl) | 3.4.16, changed | zlib | static | static | static |
| [zlib](#zlib) | 1.3.1 | zlib | static | static | static |
| [FreeType](#freetype) | 2.14.3 | FreeType License | static | static | static |
| [DejaVu fonts](#dejavu-fonts) | 2.37, DejaVu Sans and DejaVu Sans Bold | Bitstream Vera and Arev licences; DejaVu changes public domain | font files | font files | font files |
| [Noto Sans CJK](#noto-sans-cjk) | 2.004, SC Bold, cut to the languages' own names and the language notice | SIL Open Font License 1.1 | font file | font file | font file |
| [Noto Emoji](#noto-emoji) | 3.002, monochrome | SIL Open Font License 1.1 | font file | font file | font file |
| [Barlow](#barlow) | 1.422, Barlow Regular, Medium, SemiBold and Bold, Barlow Condensed SemiBold and Bold | SIL Open Font License 1.1 | font files | font files | font files |
| [stb_vorbis](#stb_vorbis) | 1.22, changed | public domain or MIT | static | static | static |
| [dr_mp3 and dr_flac](#dr_mp3-and-dr_flac) | 0.7.3 and 0.13.3, with later fixes | public domain or MIT No Attribution | static | static | static |
| [Monocypher](#monocypher) | 4.0.2 | BSD-2-Clause | static | static | static |
| [LZMA SDK](#lzma-sdk) | 25.01, decoder only | public domain | static | static | static |
| [Lua](#lua) | 5.4.9 | MIT | static | static | static |
| [mingw-w64 runtime and winpthreads](#mingw-w64-runtime-and-winpthreads) | 15.0.0 | ZPL 2.1, MIT, BSD | | static | |
| [LLVM runtime libraries](#llvm-runtime-libraries) (libc++, libc++abi, libunwind, compiler-rt) | 23.1.2 | Apache 2.0 with LLVM Exceptions | | static | |

The font files are in the `fonts` folder beside `open-annihilation`, and on
macOS in the game's bundle, `open-annihilation.app/Contents/Resources/fonts`.
The operating system's own libraries, such as the C and C++ runtimes and
the graphics, audio and windowing libraries, are not included.

## Builds from this source

The table above describes the release packages. A build made from this
repository with CMake links these components as follows:

- stb_vorbis, dr_mp3 and dr_flac, which decode the music, are kept in
  [`third_party/`](third_party/) and compiled into every build. The movies
  and the rest of the sound are decoded by the engine's own code.
- Monocypher 4.0.2, which checks a catalogue's signature, is kept in
  [`third_party/monocypher/`](third_party/monocypher/) and compiled into
  every build.
- The LZMA SDK decoder, which reads LZMA and LZMA2 streams, is kept in
  [`src/formats/lzma/vendor/`](src/formats/lzma/vendor/) and compiled into
  every build.
- Lua 5.4.9, which runs the scripts of a mutator, is kept in
  [`third_party/lua/`](third_party/lua/) and compiled into every build.
  The coroutine, debug, input and output, maths, operating-system and
  package libraries are left out.
- When CMake is pointed at the SDL that `tools/bootstrap_sdl.py` installs,
  as `run.sh` and the README do, SDL 3.4.16 with the changes described
  under [SDL](#sdl) is linked statically. Otherwise the build takes
  whichever SDL 3.2 or later CMake finds, which may be a shared library and
  lacks those changes; the game then plays the Steam Deck's trackpad
  ticks as rumble.
- On macOS and Linux, zlib is the system's, except in the macOS release
  build below: the executables link the zlib CMake finds, and the build
  copies none beside the executables.
- The native Windows build that the README describes, with vcpkg's
  `zlib:x64-windows`, links that zlib as a DLL and copies it beside
  `oa-tool.exe`.
- The Windows cross-build, `tools/build_windows.sh`, builds zlib 1.3.1 and
  SDL for the target and links them statically.
- The macOS release build, `tools/release_macos.sh`, builds zlib 1.3.1 and
  SDL 3.4.16 for arm64 and x86_64 as static libraries with
  `tools/bootstrap_macos_deps.py` and links them into the application, as
  the macOS package in the table above has them.
- FreeType and the fonts come from `tools/bootstrap_text_fonts.py`, which
  builds FreeType 2.14.3 as a static library into `local/deps` and puts the
  fonts in `local/deps/text-fonts`; the build links that FreeType and copies
  the fonts beside the game. A build CMake points at another FreeType 2.10
  or later links that one instead. The macOS release build and the Windows
  cross-build build the same FreeType for their targets.
- The movies and music need no other library; the `ffmpeg` program that
  video capture and the director's renders start (docs/capture.md) is a
  separate program that no package contains.

## SDL

SDL 3.4.16, from <https://www.libsdl.org/release/SDL3-3.4.16.tar.gz>
(SHA-256 `7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68`),
linked statically. **This is an altered version of SDL**: the packages'
SDL is that release with the patches in
[`tools/sdl-patches/`](tools/sdl-patches/) applied, which `tools/bootstrap_sdl.py` applies when it
unpacks the release, and which are offered upstream to SDL under SDL's own
licence. Each patch says what it changes; today there is one, which lets
SDL's Steam Deck driver send the controller a trackpad haptic report given
to `SDL_SendGamepadEffect`. The rest of SDL is as the release published
it. SDL 3.2.0, which the build also accepts, is built unchanged.

> Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
>
> This software is provided 'as-is', without any express or implied
> warranty. In no event will the authors be held liable for any damages
> arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose,
> including commercial applications, and to alter it and redistribute it
> freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not
>    claim that you wrote the original software. If you use this software
>    in a product, an acknowledgment in the product documentation would be
>    appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be
>    misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

SDL contains code from other projects:

- **yuv2rgb** (BSD 3-Clause), Copyright (c) 2016, Adrien Descamps. All
  rights reserved. Redistributions in binary form must reproduce the
  copyright notice, the list of conditions and the disclaimer; the full text
  is in [`licenses/SDL-yuv2rgb.txt`](licenses/SDL-yuv2rgb.txt).
- **HIDAPI**, used under its original licence: "HIDAPI - Multi-Platform
  library for communication with HID devices. Copyright 2009, Alan Ott,
  Signal 11 Software. All Rights Reserved. This software may be used by
  anyone for any reason so long as the copyright notice in the source files
  remains intact."
- **fdlibm**: "Copyright (C) 1993 by Sun Microsystems, Inc. All rights
  reserved. Developed at SunPro, a Sun Microsystems, Inc. business.
  Permission to use, copy, modify, and distribute this software is freely
  granted, provided that this notice is preserved."
- **stb_image**, **miniz**, Doug Lea's **malloc** and the IBM VGA font are
  in the public domain.
- **Linux only:** SDL's X11 and Wayland support contains code under
  MIT-style licences (keysym conversion, EDID parsing, XSETTINGS and the
  Wayland protocol files). Their notices are in
  [`licenses/SDL-linux.txt`](licenses/SDL-linux.txt).

## stb_vorbis

stb_vorbis 1.22, the Ogg Vorbis decoder of the stb libraries, from
<https://github.com/nothings/stb>, decodes `.ogg` music files. It is
compiled into the game from [`third_party/stb_vorbis/`](third_party/stb_vorbis/),
with one change that its [README](third_party/stb_vorbis/README.md)
describes. It is in the public domain, or, at your choice, licensed under
the MIT licence; both texts are in
[`licenses/stb_vorbis-LICENSE.txt`](licenses/stb_vorbis-LICENSE.txt).

> Copyright (c) 2017 Sean Barrett
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of this software and associated documentation files (the
> "Software"), to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit
> persons to whom the Software is furnished to do so, subject to the
> following conditions: The above copyright notice and this permission
> notice shall be included in all copies or substantial portions of the
> Software.

## dr_mp3 and dr_flac

dr_mp3 and dr_flac, David Reid's MP3 and FLAC decoders, from
<https://github.com/mackron/dr_libs>, decode `.mp3` and `.flac` music
files. They are compiled into the game unchanged from
[`third_party/dr_libs/`](third_party/dr_libs/). They are in the public
domain, or, at your choice, licensed under the MIT No Attribution licence
(Copyright 2020 David Reid); both texts are in
[`licenses/dr_libs-LICENSE.txt`](licenses/dr_libs-LICENSE.txt). dr_mp3 is
based on minimp3, which is in the public domain (CC0).

## Monocypher

Monocypher 4.0.2, from
<https://monocypher.org/download/monocypher-4.0.2.tar.gz> (SHA-256
`38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`),
checks a catalogue's signature. It is compiled into the game unchanged
from [`third_party/monocypher/`](third_party/monocypher/). The release
offers BSD-2-Clause or CC0-1.0; this project uses BSD-2-Clause. The
licence text is in
[`licenses/monocypher-LICENSE.txt`](licenses/monocypher-LICENSE.txt).

> Copyright (c) 2017-2023, Loup Vaillant
> Copyright (c) 2017-2019, Michael Savage
> Copyright (c) 2017-2023, Fabio Scotoni
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are
> met:
>
> 1. Redistributions of source code must retain the above copyright
>    notice, this list of conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright
>    notice, this list of conditions and the following disclaimer in the
>    documentation and/or other materials provided with the
>    distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
> "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
> LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
> A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
> HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
> SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
> LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
> DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
> THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
> (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Lua

Lua 5.4.9, from <https://www.lua.org/ftp/lua-5.4.9.tar.gz> (SHA-256
`2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`),
runs the scripts of a mutator. It is compiled into the game unchanged
from [`third_party/lua/`](third_party/lua/). The script host also keeps a
changed copy of the pattern matcher from that release, in
[`src/data/script-host/src/patterns.cpp`](src/data/script-host/src/patterns.cpp),
which counts each step of `string.find`, `string.match`, `string.gmatch` and
`string.gsub` against the script's instruction allowance. It keeps the same
kind of copy in
[`src/data/script-host/src/libraries.cpp`](src/data/script-host/src/libraries.cpp)
for `next`, `rawequal`, `tonumber`, `warn`, `string.byte`, `table.move`,
`table.insert`, `table.remove`, `table.concat`, `table.pack`,
`table.unpack`, `table.sort`, `utf8.char`, `utf8.len`, `utf8.codepoint`,
`utf8.offset` and `utf8.codes`, and in
[`src/data/script-host/src/string_format.cpp`](src/data/script-host/src/string_format.cpp)
for `string.format`, counting the bytes, elements or comparisons those
calls perform. The licence is the MIT licence. The licence text is in
[`licenses/lua-LICENSE.txt`](licenses/lua-LICENSE.txt).

> Copyright (C) 1994-2026 Lua.org, PUC-Rio.
>
> Permission is hereby granted, free of charge, to any person obtaining
> a copy of this software and associated documentation files (the
> "Software"), to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to
> permit persons to whom the Software is furnished to do so, subject to
> the following conditions:
>
> The above copyright notice and this permission notice shall be
> included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
> EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
> IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
> CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
> TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
> SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## LZMA SDK

The LZMA and LZMA2 decoder from the 7-Zip / LZMA SDK 25.01 by Igor Pavlov,
from <https://github.com/ip7z/7zip> tag `25.01`, directory `C/`. The files
`LzmaDec`, `Lzma2Dec`, `7zTypes.h`, `Compiler.h` and `Precomp.h` are compiled
into the game unchanged from
[`src/formats/lzma/vendor/`](src/formats/lzma/vendor/). The SDK is in the
public domain. The notice is in
[`licenses/lzma-sdk.txt`](licenses/lzma-sdk.txt). Each file carries its own
line, "Igor Pavlov : Public domain".

## zlib

All packages link zlib 1.3.1 statically, from
<https://github.com/madler/zlib/releases/> (SHA-256
`9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`).

> Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
>
> This software is provided 'as-is', without any express or implied
> warranty. In no event will the authors be held liable for any damages
> arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose,
> including commercial applications, and to alter it and redistribute it
> freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not
>    claim that you wrote the original software. If you use this software
>    in a product, an acknowledgment in the product documentation would be
>    appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be
>    misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## FreeType

FreeType 2.14.3, from <https://download.savannah.gnu.org/releases/freetype/>
(SHA-256 `e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1`),
draws the fonts. It is linked statically, unmodified, built without zlib,
bzip2, PNG, HarfBuzz or Brotli and with only its TrueType and CFF drivers,
their helpers, the auto-hinter and the two rasterizers. It is used under the
FreeType License; the full text is in
[`licenses/FreeType-FTL.txt`](licenses/FreeType-FTL.txt).

> Portions of this software are copyright © 2026 The FreeType
> Project (https://freetype.org).  All rights reserved.

## DejaVu fonts

DejaVu Sans and DejaVu Sans Bold 2.37, unmodified, from
<https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37>
(`dejavu-fonts-ttf-2.37.zip`, SHA-256
`7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a`), draw
Latin, Greek, Cyrillic and symbols. Fonts are (c) Bitstream; DejaVu changes
are in the public domain; glyphs imported from the Arev fonts are (c)
Tavmjong Bah. Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.
Bitstream Vera is a trademark of Bitstream, Inc. The licence, with its
conditions and disclaimer, is in
[`licenses/DejaVu-LICENSE.txt`](licenses/DejaVu-LICENSE.txt).

## Noto Sans CJK

Noto Sans CJK SC Bold 2.004, from the `NotoSansCJK-Bold.ttc` collection of
<https://github.com/notofonts/noto-cjk> at the release tag `Sans2.004`
(SHA-256 `faa5f3656a78b2e2d450d27fe8382c778bc2b6bb5ea29c986664a6a435056ceb`),
draws Chinese, Japanese and Korean. The packages hold one cut of the
Simplified Chinese face of the collection, made by
`tools/bootstrap_text_fonts.py`: `NotoSansCJKsc-Bold-Endonyms.otf` holds
the languages' own names and the language notice, the lines of
`tools/text-fonts/endonyms.txt` that the sans faces do not draw. The
Simplified Chinese language pack carries its own copy of the face, under
the same licence, for the language's text. © 2014-2021 Adobe (http://www.adobe.com/). It is
licensed under the SIL Open Font License, Version 1.1; the text is in
[`licenses/NotoSansCJK-OFL.txt`](licenses/NotoSansCJK-OFL.txt).

## Noto Emoji

Noto Emoji 3.002, the monochrome emoji font, unmodified, from
<https://github.com/google/fonts/tree/main/ofl/notoemoji> at commit
`b979dba422e445492b0eb9951ac52ee0b4d648c3` (`NotoEmoji[wght].ttf`, SHA-256
`de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551`), shipped
as `NotoEmoji.ttf`, draws emoji in the colour of the text. Copyright 2013
Google LLC. It is licensed under the SIL Open Font License, Version 1.1; the
text is in [`licenses/NotoEmoji-OFL.txt`](licenses/NotoEmoji-OFL.txt).

## Barlow

Barlow Regular, Medium, SemiBold and Bold and Barlow Condensed SemiBold and
Bold 1.422, unmodified, from <https://github.com/jpt/barlow> at the release
tag `1.422`: `fonts/ttf/Barlow-Regular.ttf` (SHA-256
`a72240db047b468c957834cdbd607f013da32edee62d096f5fe0a4c2dc8433ce`),
`Barlow-Medium.ttf`
(`5a26763b11d11a437ab9f1c46281d3615ab1f6b93c90b8fa181e03978971b9e9`),
`Barlow-SemiBold.ttf`
(`cafab6a24fc131fd2dbbe56b9cf8daa616db3d5929fa89c35acba690e620585e`),
`Barlow-Bold.ttf`
(`b5af7fa4dd961a259b075cd23094cd4e961afb6a0b5ac5cf0e3395fc54b7a4b5`),
`BarlowCondensed-SemiBold.ttf`
(`0dae49e31a5dd598184b8ed18ed877f9280e07c4cc3fa7c2cad25a5e7d8a3ad4`) and
`BarlowCondensed-Bold.ttf`
(`e4fd0ebbc8b4346229f5c409f34e052b5e0d7fbcb7a7c4c30f7c5d334b4953b2`), draw
Open Annihilation's own screens in the modern look. Copyright 2017 The
Barlow Project Authors (https://github.com/jpt/barlow). It is licensed
under the SIL Open Font License, Version 1.1; the text is in
[`licenses/Barlow-OFL.txt`](licenses/Barlow-OFL.txt).

## mingw-w64 runtime and winpthreads

The Windows packages are linked statically against the mingw-w64 15.0.0
runtime (Zope Public License 2.1, with parts in the public domain or under
BSD licences; Copyright (c) 2009-2013 by the mingw-w64 project) and its
winpthreads library (MIT; Copyright (c) 2011-2016 mingw-w64 project; parts
(C) 2010 Lockless Inc., BSD 3-Clause). Their full notices and disclaimers
are in [`licenses/mingw-w64.txt`](licenses/mingw-w64.txt).

## LLVM runtime libraries

The Windows packages (x64, x86 and ARM64) are
built with the LLVM toolchain llvm-mingw 20260922, with LLVM 23.1.2, and
link LLVM's C++ standard library and its support libraries (libc++,
libc++abi, libunwind and compiler-rt) statically. These are licensed under
the Apache License 2.0 with LLVM Exceptions, which place no requirements on
programs that embed them in compiled form.
