#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Build the pinned zlib, SDL3, FreeType and HarfBuzz as universal static libraries for macOS.

The macOS release (tools/release_macos.sh) links them into the application,
which then needs no library beyond the system's. Each library runs on arm64
and x86_64 Macs from the oldest macOS release --deployment-target names, and
gets its own install prefix (zlib/, sdl/, freetype/, harfbuzz/) under the prefix root,
<deps>/macos-<deployment target> unless --prefix-root names another. Each
library is built for both architectures at once. Build trees live beside
the root (<root>-build). Each
prefix records in build-settings.json what its library was built from and
how: the version, the archive's SHA-256, the deployment target, the
architectures and the options, and for SDL the patches applied to it
(tools/sdl-patches), each by its SHA-256 and name. A library whose prefix
is complete and records the settings of this run is not built again; any
other is built afresh, so a new pin, a new or changed patch or new options
never leave older libraries in place: an SDL built before its release had
patches records none, and is built again from the patched source.
Nothing outside the cache, the root and its build trees is modified.

--self-test checks, in a scratch folder, which SDL prefixes a run reuses
and which it builds again, and that SDL's settings name the patches of
tools/sdl-patches. It builds and downloads nothing, and runs on any
system. Exit status: 0 when the libraries are ready (or the self-test
passed); 1 when the self-test fails.
"""
import argparse
import contextlib
import hashlib
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bootstrap_sdl  # noqa: E402
import bootstrap_text_fonts  # noqa: E402
import bootstrap_windows_deps as pins  # noqa: E402

ARCHITECTURES = ("arm64", "x86_64")
DEFAULT_DEPLOYMENT_TARGET = "11.0"
SETTINGS_FILE = "build-settings.json"
ZLIB_OPTIONS = ["-DZLIB_BUILD_EXAMPLES=OFF", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"]
SDL_OPTIONS = ["-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
               "-DSDL_EXAMPLES=OFF"]
# The file a complete SDL prefix holds.
SDL_INSTALLED = "lib/cmake/SDL3/SDL3Config.cmake"
# The self-test's made-up patch, at the bytes an older prefix was built
# with and at the bytes it has now, written as bytes so that no platform
# changes their line endings.
SELF_TEST_PATCH = f"0001-change{bootstrap_sdl.PATCH_SUFFIX}"
SELF_TEST_OLDER_PATCH = "the change as it was\n"
SELF_TEST_PATCH_TEXT = "the change as it is now\n"


def settings(version, sha256, deployment_target, options):
    """Returns what a library's prefix records it was built from and how."""
    return {"version": version, "sha256": sha256, "deployment_target": deployment_target,
            "architectures": list(ARCHITECTURES), "options": options}


def sdl_patches(patches=bootstrap_sdl.PATCHES):
    """Lists the patches applied to the pinned SDL as its prefix records them.

    @param patches the folder that holds a folder of patches per release
    @return each patch's SHA-256 and name, in the order they apply; empty for a release without patches
    """
    return (bootstrap_sdl.patch_stamp(bootstrap_sdl.VERSION, patches) or "").splitlines()


def sdl_settings(deployment_target, patches=bootstrap_sdl.PATCHES):
    """Returns what SDL's prefix records it was built from and how, its patches among them.

    @param deployment_target the oldest macOS release the library runs on
    @param patches the folder that holds a folder of patches per release
    @return the settings
    """
    wanted = settings(bootstrap_sdl.VERSION, bootstrap_sdl.SHA256, deployment_target, SDL_OPTIONS)
    wanted["patches"] = sdl_patches(patches)
    return wanted


def is_current(install, files, wanted):
    """Whether install holds files and was built with the settings wanted."""
    if not all((install / name).exists() for name in files):
        return False
    try:
        recorded = json.loads((install / SETTINGS_FILE).read_text())
    except (OSError, ValueError):
        recorded = None
    if recorded != wanted:
        print(f"{install} was built with other settings; building it afresh")
        return False
    return True


def record(install, wanted):
    """Records in install the settings its library was built with."""
    (install / SETTINGS_FILE).write_text(json.dumps(wanted, indent=2, sort_keys=True) + "\n")


def build_universal(source, build, install, deployment_target, jobs, options):
    """Builds and installs a CMake project for both architectures, afresh."""
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(install, ignore_errors=True)
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                    f"-DCMAKE_INSTALL_PREFIX={install}",
                    f"-DCMAKE_OSX_ARCHITECTURES={';'.join(ARCHITECTURES)}",
                    f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}", *options], check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel", str(jobs)], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)


def self_test():
    """Checks which SDL prefixes a run reuses, in a scratch folder; returns the exit status.

    @return 0 when every check passes, else 1
    """
    failures = []
    with tempfile.TemporaryDirectory() as scratch:
        root = pathlib.Path(scratch)
        patches = root / "sdl-patches"
        (patches / bootstrap_sdl.VERSION).mkdir(parents=True)
        patch = patches / bootstrap_sdl.VERSION / SELF_TEST_PATCH
        patch.write_bytes(SELF_TEST_OLDER_PATCH.encode())
        older = sdl_settings(DEFAULT_DEPLOYMENT_TARGET, patches)
        patch.write_bytes(SELF_TEST_PATCH_TEXT.encode())
        wanted = sdl_settings(DEFAULT_DEPLOYMENT_TARGET, patches)
        named = [f"{hashlib.sha256(SELF_TEST_PATCH_TEXT.encode()).hexdigest()}  {SELF_TEST_PATCH}"]
        if wanted.get("patches") != named:
            failures.append(f"SDL's settings name the patches {wanted.get('patches')}, not {named}")
        if sdl_patches(root / "no-patches") != []:
            failures.append("a release without patches is recorded with some")
        unpatched = {key: value for key, value in wanted.items() if key != "patches"}
        # What an SDL prefix may hold, and whether this run reuses it.
        cases = (
            ("this run's settings", True, wanted, True),
            ("the settings of a build from before its release had patches", True, unpatched, False),
            ("the settings of a build with no patches", True, {**wanted, "patches": []}, False),
            ("the settings of a build with a patch that has changed since", True, older, False),
            ("this run's settings for another deployment target", True,
             {**wanted, "deployment_target": "99.0"}, False),
            ("no settings", True, None, False),
            ("settings that cannot be read", True, "{", False),
            ("this run's settings but no library", False, wanted, False),
        )
        for what, complete, recorded, reused in cases:
            install = root / "sdl"
            shutil.rmtree(install, ignore_errors=True)
            install.mkdir()
            if complete:
                (install / SDL_INSTALLED).parent.mkdir(parents=True)
                (install / SDL_INSTALLED).write_text("", encoding="utf-8")
            if isinstance(recorded, str):
                (install / SETTINGS_FILE).write_text(recorded, encoding="utf-8")
            elif recorded is not None:
                record(install, recorded)
            with contextlib.redirect_stdout(io.StringIO()):
                got = is_current(install, [SDL_INSTALLED], wanted)
            if got != reused:
                failures.append(f"a prefix with {what} is {'reused' if got else 'built again'}, "
                                f"not {'reused' if reused else 'built again'}")
    # The engine's own patches of the pinned release, as tools/sdl-patches holds them.
    folder = bootstrap_sdl.PATCHES / bootstrap_sdl.VERSION
    own = sorted(folder.glob(f"*{bootstrap_sdl.PATCH_SUFFIX}")) if folder.is_dir() else []
    expected = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in own]
    recorded = sdl_settings(DEFAULT_DEPLOYMENT_TARGET).get("patches")
    if recorded != expected:
        failures.append(f"SDL {bootstrap_sdl.VERSION}'s settings name the patches {recorded}, not {expected}")
    for failure in failures:
        print(f"bootstrap_macos_deps self-test: {failure}")
    if failures:
        return 1
    print(f"bootstrap_macos_deps self-test: {len(cases)} SDL prefixes reused or built again as expected; "
          f"SDL {bootstrap_sdl.VERSION}'s settings name the {len(expected)} "
          f"patch{'' if len(expected) == 1 else 'es'} of tools/sdl-patches/{bootstrap_sdl.VERSION}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--deps", type=pathlib.Path, default=bootstrap_sdl.ROOT / "local" / "deps",
                        help="archive and source cache (default: local/deps)")
    parser.add_argument("--prefix-root", type=pathlib.Path,
                        help="install prefix root (default: <deps>/macos-<deployment target>)")
    parser.add_argument("--deployment-target", default=DEFAULT_DEPLOYMENT_TARGET,
                        help=f"oldest macOS release the libraries run on (default: {DEFAULT_DEPLOYMENT_TARGET})")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--self-test", action="store_true",
                        help="check which SDL prefixes a run builds again, in a scratch folder, and exit")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if sys.platform != "darwin":
        parser.error("the macOS libraries are built on macOS")
    deps = args.deps.resolve()
    prefixes = (args.prefix_root or deps / f"macos-{args.deployment_target}").resolve()
    builds = prefixes.parent / f"{prefixes.name}-build"

    # zlib's own CMake lists a pre-3.5 minimum version that CMake 4 rejects,
    # hence CMAKE_POLICY_VERSION_MINIMUM in ZLIB_OPTIONS.
    zlib_install = prefixes / "zlib"
    zlib_settings = settings(pins.ZLIB_VERSION, pins.ZLIB_SHA256, args.deployment_target, ZLIB_OPTIONS)
    if not is_current(zlib_install, ["lib/libz.a"], zlib_settings):
        build_universal(pins.zlib_source(deps), builds / "zlib", zlib_install, args.deployment_target,
                        args.jobs, ZLIB_OPTIONS)
        record(zlib_install, zlib_settings)
    sdl_install = prefixes / "sdl"
    sdl_wanted = sdl_settings(args.deployment_target)
    if not is_current(sdl_install, [SDL_INSTALLED], sdl_wanted):
        build_universal(bootstrap_sdl.sdl_source(deps), builds / "sdl", sdl_install, args.deployment_target,
                        args.jobs, SDL_OPTIONS)
        record(sdl_install, sdl_wanted)
    # FreeType for the text fonts, with the options and modules
    # tools/bootstrap_text_fonts.py builds it with.
    freetype_install = prefixes / "freetype"
    freetype_settings = settings(bootstrap_text_fonts.FREETYPE_VERSION, bootstrap_text_fonts.FREETYPE_SHA256,
                                 args.deployment_target, bootstrap_text_fonts.FREETYPE_OPTIONS)
    if not is_current(freetype_install, [bootstrap_text_fonts.FREETYPE_INSTALLED], freetype_settings):
        build_universal(bootstrap_text_fonts.freetype_source(deps), builds / "freetype", freetype_install,
                        args.deployment_target, args.jobs, bootstrap_text_fonts.FREETYPE_OPTIONS)
        record(freetype_install, freetype_settings)
    harfbuzz_install = prefixes / "harfbuzz"
    harfbuzz_options = bootstrap_text_fonts.harfbuzz_cmake_options(freetype_install)
    harfbuzz_settings = settings(bootstrap_text_fonts.HARFBUZZ_VERSION, bootstrap_text_fonts.HARFBUZZ_SHA256,
                                 args.deployment_target, harfbuzz_options)
    if not is_current(harfbuzz_install, [bootstrap_text_fonts.HARFBUZZ_INSTALLED], harfbuzz_settings):
        build_universal(bootstrap_text_fonts.harfbuzz_source(deps), builds / "harfbuzz", harfbuzz_install,
                        args.deployment_target, args.jobs, harfbuzz_options)
        record(harfbuzz_install, harfbuzz_settings)
    for install, wanted in ((zlib_install, zlib_settings), (sdl_install, sdl_wanted),
                            (freetype_install, freetype_settings), (harfbuzz_install, harfbuzz_settings)):
        print(f"{install.name} {wanted['version']} for macOS {args.deployment_target}, {' '.join(ARCHITECTURES)}")
    print(f"macOS dependencies ready under {prefixes}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
