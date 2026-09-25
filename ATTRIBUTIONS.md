# Attributions

Open Annihilation is licensed under the GNU General Public License version 3
(see `LICENSE`). The release packages also contain the third-party software
listed below. Each component keeps its own licence, and the notices those
licences require are reproduced here. Full licence texts are in the
[`licenses/`](licenses/) folder of this repository.

No Total Annihilation game data is included in this repository or in the
release packages.

## What each package contains

| Component | Version | Licence | macOS | Windows | Linux |
|---|---|---|---|---|---|
| [SDL](#sdl) | 3.4.16 | zlib | static | static | static |
| [FFmpeg](#ffmpeg) (libavcodec, libavformat, libavutil, libswresample, libswscale) | 9.0.2 | LGPL 2.1 or later | static, inside the app | DLLs beside `oa-game.exe` | shared libraries in `lib/` |
| [zlib](#zlib) | 1.3.1 | zlib | static | static | static |
| [mingw-w64 runtime and winpthreads](#mingw-w64-runtime-and-winpthreads) | 14.0.0 | ZPL 2.1, MIT, BSD | | static | |
| [GCC runtime](#gcc-runtime) | 16.2.0 | GPL 3 with the GCC Runtime Library Exception | | static | |

The operating system's own libraries, such as the C and C++ runtimes and
the graphics, audio and windowing libraries, are not included.

## SDL

SDL 3.4.16, from <https://www.libsdl.org/release/SDL3-3.4.16.tar.gz>
(SHA-256 `7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68`),
unmodified and linked statically.

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

## FFmpeg

This software uses libraries from the FFmpeg project under the LGPL v2.1.
FFmpeg is Copyright (c) 2000-2026 the FFmpeg developers. The licence text is
in [`licenses/FFmpeg-COPYING.LGPLv2.1`](licenses/FFmpeg-COPYING.LGPLv2.1), and the
source and build details are in [`licenses/FFmpeg-SOURCE.txt`](licenses/FFmpeg-SOURCE.txt).

The FFmpeg libraries decode the game's movies and music. They are built from
the unmodified release archive
<https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.gz>
(SHA-256 `84960df915059e8754fef2cd7c9afeb614062b1b5458ec471eecee619ee04e98`),
which is their complete corresponding source. They include only the
components below, and no GPL or non-free component:

```
--enable-shared --disable-static --disable-programs --disable-doc
--disable-debug --disable-autodetect --disable-network --disable-avdevice
--disable-avfilter --disable-everything --enable-protocol=file
--enable-demuxer=smacker,mp3,ogg,wav,flac
--enable-decoder=smacker,smackaud,mp3float,vorbis,flac,pcm_s16le,pcm_s24le,pcm_u8
--enable-parser=mpegaudio,vorbis,flac
```

The Windows build adds the cross-compilation, `--enable-w32threads` and
static runtime options for mingw-w64.

On Windows and Linux the libraries are separate files, so you can replace
them with another compatible build of FFmpeg 9.0. On macOS they are linked
into the application; you can rebuild it against a different FFmpeg from
the Open Annihilation source code. Open Annihilation's GNU GPL v3 terms give
you the rights that section 6 of the LGPL v2.1 requires for this.

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

## mingw-w64 runtime and winpthreads

The Windows package is linked statically against the mingw-w64 14.0.0
runtime (Zope Public License 2.1, with parts in the public domain or under
BSD licences; Copyright (c) 2009-2013 by the mingw-w64 project) and its
winpthreads library (MIT; Copyright (c) 2011-2016 mingw-w64 project; parts
(C) 2010 Lockless Inc., BSD 3-Clause). Their full notices and disclaimers
are in [`licenses/mingw-w64.txt`](licenses/mingw-w64.txt).

## Windows on ARM (experimental)

The experimental Windows ARM64 package contains the same components as the
Windows package (SDL, zlib and the mingw-w64 runtime linked statically, and
the FFmpeg DLLs), built with the LLVM toolchain instead of GCC. It also links
LLVM's C++ runtime (libc++, libc++abi, libunwind and compiler-rt) statically.
These are licensed under the Apache License 2.0 with LLVM Exceptions, which
place no requirements on programs that embed them in compiled form.

## GCC runtime

The Windows package links libgcc and libstdc++ from GCC 16.2.0 statically.
They are covered by the GCC Runtime Library Exception, which places no
requirements on programs compiled with an unmodified GCC.
