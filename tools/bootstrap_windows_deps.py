#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Cross-build the pinned zlib, SDL3 and FreeType for Windows into local/deps/windows.

The toolchain file names the target: cmake/toolchains/x86_64-w64-mingw32.cmake
(the default) or another toolchain file there. --xp builds them for
executables that also run on Windows XP, as a build configured with
-DOA_WINDOWS_XP=ON links them. Every library gets its own install prefix
under the prefix root, which the toolchain files search. Build trees live
beside the root (<root>-build) so they are never mistaken for prefixes.
Nothing outside local/ is modified.
"""
import argparse
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bootstrap_sdl  # noqa: E402
import bootstrap_text_fonts  # noqa: E402

ROOT = bootstrap_sdl.ROOT
ZLIB_VERSION = "1.3.1"
ZLIB_SHA256 = "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
ZLIB_URL = f"https://github.com/madler/zlib/releases/download/v{ZLIB_VERSION}/zlib-{ZLIB_VERSION}.tar.gz"


def zlib_source(deps):
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / f"zlib-{ZLIB_VERSION}.tar.gz"
    bootstrap_sdl.fetch_archive(archive, ZLIB_URL, ZLIB_SHA256)
    source = deps / f"zlib-{ZLIB_VERSION}"
    bootstrap_sdl.extract_source(archive, source)
    return source


def cross_build(source, build, install, toolchain, jobs, options):
    subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                    f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
                    f"-DCMAKE_INSTALL_PREFIX={install}", *options], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(jobs)], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--deps", type=pathlib.Path, default=ROOT / "local" / "deps",
                        help="archive and source cache (default: local/deps)")
    parser.add_argument("--prefix-root", type=pathlib.Path,
                        help="install prefix root (default: <deps>/windows)")
    parser.add_argument("--toolchain", type=pathlib.Path,
                        default=ROOT / "cmake" / "toolchains" / "x86_64-w64-mingw32.cmake")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--xp", action="store_true",
                        help="build for executables that also run on Windows XP (OA_WINDOWS_XP)")
    parser.add_argument("--win95", action="store_true",
                        help="build for executables that also run on Windows 95 (OA_WINDOWS_95)")
    args = parser.parse_args()
    deps = args.deps.resolve()
    prefixes = (args.prefix_root or deps / "windows").resolve()
    builds = prefixes.parent / f"{prefixes.name}-build"
    toolchain = args.toolchain.resolve()
    # The toolchain files read OA_WINDOWS_XP and OA_WINDOWS_95 to choose the C
    # library.
    target_options = [f"-DOA_WINDOWS_XP={'ON' if args.xp else 'OFF'}",
                      f"-DOA_WINDOWS_95={'ON' if args.win95 else 'OFF'}"]

    zlib_install = prefixes / "zlib"
    if not (zlib_install / "lib" / "libzlibstatic.a").exists():
        # zlib's own CMake lists a pre-3.5 minimum version that CMake 4 rejects.
        cross_build(zlib_source(deps), builds / "zlib", zlib_install, toolchain, args.jobs,
                    [*target_options, "-DZLIB_BUILD_EXAMPLES=OFF", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"])
    sdl_install = prefixes / "sdl"
    if not (sdl_install / "lib" / "cmake" / "SDL3" / "SDL3Config.cmake").exists():
        cross_build(bootstrap_sdl.sdl_source(deps), builds / "sdl", sdl_install, toolchain, args.jobs,
                    [*target_options, "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST_LIBRARY=OFF",
                     "-DSDL_TESTS=OFF", "-DSDL_EXAMPLES=OFF"])
    freetype_install = prefixes / "freetype"
    if not (freetype_install / bootstrap_text_fonts.FREETYPE_INSTALLED).exists():
        cross_build(bootstrap_text_fonts.freetype_source(deps), builds / "freetype", freetype_install, toolchain,
                    args.jobs, [*target_options, *bootstrap_text_fonts.FREETYPE_OPTIONS])
    print(f"Windows dependencies ready under {prefixes}")


if __name__ == "__main__":
    main()
