#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Configure a scratch tree with self-update off and check the targets.

The scratch configure uses the running build's prefix path, text-font folder,
generator, compilers and the dependency folders its cache records, read from
its CMakeCache.txt. It does not build. oa-game is listed and the oa-update
program is not. A second configure with self-update on and process spawning
off fails, and names both options.

A Windows host skips. The nested configure is not run there. When the running
build's FreeType cannot be found for a new configure, the check skips too.
"""
import subprocess
import sys
import tempfile
from pathlib import Path


class SelfUpdateFailure(Exception):
    """A configure that does not do what self-update off requires."""


class SelfUpdateSkip(Exception):
    """This host cannot run the nested configure."""


def expect(condition, what):
    """Fails the check with `what` unless `condition` holds."""
    if not condition:
        raise SelfUpdateFailure(what)


def cache_entries(cache_path):
    """Returns the cache's KEY=value entries. A missing key is absent."""
    entries = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        split = line.find("=")
        if split < 0:
            continue
        name = line[:split].split(":", 1)[0]
        entries[name] = line[split + 1 :]
    return entries


def require(entries, key):
    """Returns one cache value, or fails when it is missing or empty."""
    value = entries.get(key, "")
    expect(value != "", f"the running build's cache has no {key}")
    return value


# Passed through when the running build set them. OA_SELF_UPDATE is not one
# of them: the scratch configure sets that itself.
PASSTHROUGH = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_C_COMPILER",
    "CMAKE_CXX_COMPILER",
    "CMAKE_OBJCXX_COMPILER",
    "CMAKE_C_COMPILER_LAUNCHER",
    "CMAKE_CXX_COMPILER_LAUNCHER",
    "CMAKE_OBJCXX_COMPILER_LAUNCHER",
    "CMAKE_OSX_ARCHITECTURES",
    "CMAKE_OSX_DEPLOYMENT_TARGET",
    "CMAKE_OSX_SYSROOT",
    "CMAKE_TOOLCHAIN_FILE",
    "CMAKE_MAKE_PROGRAM",
    "CMAKE_GENERATOR_TOOLSET",
    "CMAKE_GENERATOR_PLATFORM",
)

# Where the running build found a dependency that CMAKE_PREFIX_PATH does not
# name. A FreeType in its own folder is freetype_DIR. A value CMake recorded
# as missing is not a place to look.
DEPENDENCY = (
    "freetype_DIR",
    "Freetype_DIR",
    "SDL3_DIR",
    "ZLIB_INCLUDE_DIR",
    "ZLIB_LIBRARY",
    "ZLIB_LIBRARY_RELEASE",
    "ZLIB_ROOT",
    "FREETYPE_INCLUDE_DIR_ft2build",
    "FREETYPE_INCLUDE_DIR_freetype2",
    "FREETYPE_LIBRARY",
    "FREETYPE_LIBRARY_RELEASE",
    "CMAKE_IGNORE_PREFIX_PATH",
    "CMAKE_FIND_ROOT_PATH",
)


def configured(value):
    """True when a cache value names something the build found."""
    return bool(value) and not value.endswith("-NOTFOUND")


def configure_flags(source, build, entries, extra):
    """The flags of one scratch configure."""
    flags = [
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        require(entries, "CMAKE_GENERATOR"),
        f"-DCMAKE_PREFIX_PATH={require(entries, 'CMAKE_PREFIX_PATH')}",
        f"-DOA_TEXT_FONTS_DIR={require(entries, 'OA_TEXT_FONTS_DIR')}",
    ]
    for key in PASSTHROUGH:
        value = entries.get(key, "")
        if value:
            flags.append(f"-D{key}={value}")
    for key in DEPENDENCY:
        value = entries.get(key, "")
        if configured(value):
            flags.append(f"-D{key}={value}")
    flags.extend(extra)
    return flags


def run(command, cwd):
    """Runs one command and returns its completed process."""
    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def target_names(help_text):
    """The build targets named by `cmake --build --target help`.

    Ninja prints `name: phony`. Make prints a bare name, after a header.
    A name is one word, so a header line is not a target. The match is the
    whole name: oa-update-core is not oa-update.
    """
    names = set()
    for line in help_text.splitlines():
        text = line.strip()
        if not text or text.startswith(".") or text.startswith("#"):
            continue
        name = text.split(":", 1)[0].strip()
        if not name or " " in name or name.startswith("-"):
            continue
        names.add(name)
    return names


def build_lists_target(build, name):
    """True when the generated build names `name` as its own target.

    Ninja's help lists only targets nothing else builds, so an executable
    that `all` builds is absent there even when it exists. The build file
    names it directly. Other generators leave CMakeFiles/<name>.dir.
    """
    ninja = build / "build.ninja"
    if ninja.is_file():
        prefix = f"build {name}:"
        for line in ninja.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(prefix):
                return True
        return False
    return (build / "CMakeFiles" / f"{name}.dir").is_dir()


def check(source, binary):
    """Configures the scratch trees and checks both results."""
    entries = cache_entries(binary / "CMakeCache.txt")
    cmake = require(entries, "CMAKE_COMMAND")
    with tempfile.TemporaryDirectory(prefix="oa-v05-") as root:
        off = Path(root) / "off"
        refused = Path(root) / "refused"
        off_run = run(
            [
                cmake,
                *configure_flags(
                    source,
                    off,
                    entries,
                    [
                        "-DOA_SELF_UPDATE=OFF",
                        "-DOA_PACKAGER_NAME=Ridge",
                        "-DBUILD_TESTING=OFF",
                        "-DOA_SELF_CHECKS=OFF",
                    ],
                ),
            ],
            source,
        )
        if off_run.returncode != 0:
            told = off_run.stderr + "\n" + off_run.stdout
            if "FreeType 2.10 or later was not found" in told:
                raise SelfUpdateSkip(
                    "the running build's FreeType could not be found for a new configure"
                )
            raise SelfUpdateFailure("self-update off did not configure:\n" + off_run.stderr)
        helped = run([cmake, "--build", str(off), "--target", "help"], source)
        expect(helped.returncode == 0, "target help failed:\n" + helped.stderr)
        names = target_names(helped.stdout + "\n" + helped.stderr)
        expect("oa-game" in names, "oa-game is not a target of a self-update off configure")
        expect("oa-update" not in names, "help still lists oa-update with self-update off")
        expect(build_lists_target(off, "oa-game"), "the build does not name oa-game")
        expect(
            not build_lists_target(off, "oa-update"),
            "the build still names oa-update with self-update off",
        )
        bad = run(
            [
                cmake,
                *configure_flags(
                    source,
                    refused,
                    entries,
                    [
                        "-DOA_SELF_UPDATE=ON",
                        "-DOA_PROCESS_SPAWNING=OFF",
                        "-DBUILD_TESTING=OFF",
                        "-DOA_SELF_CHECKS=OFF",
                    ],
                ),
            ],
            source,
        )
        expect(bad.returncode != 0, "self-update on with process spawning off configured")
        told = bad.stderr + "\n" + bad.stdout
        expect("OA_SELF_UPDATE" in told, "the refusal does not name OA_SELF_UPDATE")
        expect("OA_PROCESS_SPAWNING" in told, "the refusal does not name OA_PROCESS_SPAWNING")


def main():
    """Checks the source tree against the running build's cache."""
    if sys.platform == "win32":
        print("self-update off: skipped on Windows", file=sys.stderr)
        return 77
    if len(sys.argv) != 3:
        print("usage: check_self_update_off.py SOURCE BINARY", file=sys.stderr)
        return 2
    try:
        check(Path(sys.argv[1]), Path(sys.argv[2]))
    except SelfUpdateSkip as skipped:
        print(f"self-update off: skipped, {skipped}", file=sys.stderr)
        return 77
    except SelfUpdateFailure as failure:
        print(f"self-update off: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
