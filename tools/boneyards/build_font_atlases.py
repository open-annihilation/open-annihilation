#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Draw the analogue fonts into one atlas, or check the committed file.

The bitmap faces keep their own pictures. This builds the pictures for every
other character: Fira Sans Extra Condensed SemiBold, then Noto Sans CJK SC
Medium, then the engine's Noto Emoji. tools/boneyards/bootstrap_fonts.py
fetches the first two into local/deps/font-sources; Noto Emoji is the
engine's, from tools/bootstrap_text_fonts.py. The committed atlas is
data/boneyards/fonts/boneyards-analogues.bin. A build copies that file. It
does not draw it. Run this when a font or this script changes, and commit
the file it writes. The bytes are the same from one run to the next.

--check draws the atlas again and compares it with the committed file. It
skips, with status 77, when a font or a host tool is missing, and changes
nothing in the repository. The boneyards-font-atlas test runs it.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
ATLAS_NAME = "boneyards-analogues.bin"
STAMP_NAME = "boneyards-analogues.stamp"
COMMITTED = ROOT / "data" / "boneyards" / "fonts" / ATLAS_NAME
SKIPPED = 77


class Missing(Exception):
    """A font or a host tool this draw needs is not on the machine."""


def sha256_of(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


# Static library names the pinned FreeType install uses. Unix archives come
# first; Visual Studio's install names the same library freetype.lib.
FREETYPE_LIBRARIES = (
    ("lib", "libfreetype.a"),
    ("lib", "freetype.lib"),
    ("lib", "Release", "freetype.lib"),
    ("lib", "RelWithDebInfo", "freetype.lib"),
    ("lib", "MinSizeRel", "freetype.lib"),
    ("lib", "Debug", "freetype.lib"),
)


def freetype_library(prefix):
    for parts in FREETYPE_LIBRARIES:
        candidate = prefix.joinpath(*parts)
        if candidate.is_file():
            return candidate
    return None


def freetype_prefix(engine):
    """The newest pinned FreeType install under the engine's local/deps."""
    deps = engine / "local" / "deps"
    found = []
    for prefix in deps.glob("freetype-install-*"):
        suffix = prefix.name[len("freetype-install-"):]
        try:
            version = tuple(int(part) for part in suffix.split("."))
        except ValueError:
            continue
        include = prefix / "include" / "freetype2"
        library = freetype_library(prefix)
        if include.is_dir() and library is not None:
            found.append((version, include, library))
    if not found:
        raise Missing(
            "build_font_atlases: the engine's FreeType install was not found. "
            "Run the engine's tools/bootstrap_text_fonts.py."
        )
    found.sort()
    return found[-1][1], found[-1][2]


def host_compiler():
    """The compiler for this machine: CXX when it is set, otherwise c++, otherwise cl."""
    chosen = os.environ.get("CXX")
    if chosen:
        return chosen
    if shutil.which("c++"):
        return "c++"
    if shutil.which("cl"):
        return "cl"
    return "c++"


def cl_compiler(compiler):
    return pathlib.Path(compiler).stem.lower() == "cl"


def compile_rasterizer(engine, cache):
    include, library = freetype_prefix(engine)
    source = ROOT / "tools" / "boneyards" / "rasterize_font_atlas.cpp"
    compiler = host_compiler()
    if shutil.which(compiler) is None:
        raise Missing(f"build_font_atlases: compiler {compiler} was not found")
    binary = cache / ("rasterize_font_atlas.exe" if cl_compiler(compiler) or os.name == "nt"
                      else "rasterize_font_atlas")
    cache.mkdir(parents=True, exist_ok=True)
    newest_input = max(source.stat().st_mtime, library.stat().st_mtime)
    if binary.is_file() and binary.stat().st_mtime >= newest_input:
        return binary
    if cl_compiler(compiler):
        # Warning 4996 is fopen's deprecation. The rasterizer is a host tool.
        command = [compiler, "/nologo", "/std:c++17", "/O2", "/EHsc", "/MD",
                   "/wd4996", f"/Fe:{binary}", str(source),
                   f"/I{include}", str(library)]
    else:
        command = [compiler, "-std=c++17", "-O2", "-o", str(binary), str(source),
                   f"-I{include}", str(library)]
    subprocess.run(command, check=True)
    return binary


def stamp_text(paths):
    lines = [f"{name} {sha256_of(path)}" for name, path in paths]
    return "\n".join(lines) + "\n"


def font_paths(deps, engine, text_fonts):
    fira = deps / "font-sources" / "fira-sans-extra-condensed" / "FiraSansExtraCondensed-SemiBold.ttf"
    cjk = deps / "font-sources" / "noto-sans-cjk" / "NotoSansCJK-Medium.ttc"
    emoji = (text_fonts or engine / "local" / "deps" / "text-fonts") / "NotoEmoji.ttf"
    missing = [path for path in (fira, cjk, emoji) if not path.is_file()]
    if missing:
        listed = ", ".join(str(path) for path in missing)
        raise Missing(f"build_font_atlases: missing {listed}")
    return fira, cjk, emoji


def draw(deps, engine, force, rasterizer_path, text_fonts):
    """Draws the atlas under deps/font-atlases and returns that file.

    An up-to-date file is left untouched. Raises Missing when a font or a
    host tool is absent.
    """
    fira, cjk, emoji = font_paths(deps, engine, text_fonts)
    library = freetype_prefix(engine)[1]
    atlases = deps / "font-atlases"
    atlases.mkdir(parents=True, exist_ok=True)
    atlas = atlases / ATLAS_NAME
    stamp = atlases / STAMP_NAME
    inputs = (
        ("fira", fira),
        ("cjk", cjk),
        ("emoji", emoji),
        ("freetype", library),
        ("rasterizer", ROOT / "tools" / "boneyards" / "rasterize_font_atlas.cpp"),
        ("builder", ROOT / "tools" / "boneyards" / "build_font_atlases.py"),
    )
    current = stamp_text(inputs)
    if atlas.is_file() and stamp.is_file() and not force and stamp.read_text() == current:
        print(f"analogue atlas: up to date ({atlas})")
        return atlas

    if rasterizer_path is not None:
        rasterizer = rasterizer_path
        if not rasterizer.is_file():
            sys.exit(f"build_font_atlases: the rasterizer was not built: {rasterizer}")
    else:
        rasterizer = compile_rasterizer(engine, atlases / "cache")
    temporary = atlases / (ATLAS_NAME + ".part")
    subprocess.run(
        [str(rasterizer), "--fira", str(fira), "--cjk", str(cjk), "--emoji", str(emoji),
         "--out", str(temporary)],
        check=True,
    )
    temporary.replace(atlas)
    stamp.write_text(current)
    return atlas


def write_committed(atlas):
    """Copies atlas over the committed file when the bytes differ."""
    drawn = atlas.read_bytes()
    if COMMITTED.is_file() and COMMITTED.read_bytes() == drawn:
        print(f"build_font_atlases: {COMMITTED.relative_to(ROOT)} already matches")
        return
    COMMITTED.parent.mkdir(parents=True, exist_ok=True)
    temporary = COMMITTED.with_name(ATLAS_NAME + ".part")
    temporary.write_bytes(drawn)
    temporary.replace(COMMITTED)
    print(f"build_font_atlases: wrote {COMMITTED.relative_to(ROOT)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--deps", type=pathlib.Path, default=None,
                        help="deps directory (default: OA_DEPS_DIR, else local/deps)")
    parser.add_argument("--engine", type=pathlib.Path, default=None,
                        help="engine checkout (default: OA_ENGINE_DIR, else this tree)")
    parser.add_argument("--force", action="store_true",
                        help="draw the atlas again even when the inputs are unchanged")
    parser.add_argument("--rasterizer", type=pathlib.Path, default=None,
                        help="rasterizer already built with this machine's compiler "
                             "(Visual Studio's on Windows); otherwise one is compiled "
                             "with CXX or c++")
    parser.add_argument("--text-fonts", type=pathlib.Path, default=None,
                        help="the engine's text fonts folder, which holds Noto Emoji "
                             "(default: the engine's local/deps/text-fonts)")
    parser.add_argument("--check", action="store_true",
                        help="redraw the atlas and compare it with the committed file; "
                             "skip with status 77 when a font or a host tool is missing")
    args = parser.parse_args()
    deps = (args.deps or pathlib.Path(os.environ.get("OA_DEPS_DIR") or ROOT / "local" / "deps")).resolve()
    if args.engine is not None:
        engine = args.engine.resolve()
    elif os.environ.get("OA_ENGINE_DIR"):
        engine = pathlib.Path(os.environ["OA_ENGINE_DIR"]).resolve()
    elif (ROOT / "CMakeLists.txt").is_file() and (ROOT / "tools" / "bootstrap_sdl.py").is_file():
        engine = ROOT
    else:
        engine = (ROOT / "engine").resolve()
    text_fonts = args.text_fonts.resolve() if args.text_fonts is not None else None
    if not args.check:
        os.environ["OA_ENGINE_DIR"] = str(engine)
        sys.path.insert(0, str(ROOT / "tools" / "boneyards"))
        import bootstrap_fonts
        bootstrap_fonts.fetch_fonts(deps)
    try:
        atlas = draw(deps, engine, args.force, args.rasterizer, text_fonts)
    except Missing as error:
        print(error, file=sys.stderr)
        if args.check:
            print("build_font_atlases: skipped", file=sys.stderr)
            return SKIPPED
        return 1
    except subprocess.CalledProcessError as error:
        print(f"build_font_atlases: {error}", file=sys.stderr)
        return 1
    if args.check:
        if not COMMITTED.is_file() or COMMITTED.read_bytes() != atlas.read_bytes():
            print(
                f"build_font_atlases: {COMMITTED.relative_to(ROOT)} is not what the fonts draw; "
                "run tools/boneyards/build_font_atlases.py",
                file=sys.stderr,
            )
            return 1
        print(f"build_font_atlases: {COMMITTED.relative_to(ROOT)} is what the fonts draw")
        return 0
    write_committed(atlas)
    print(f"Atlases ready under {atlas}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
