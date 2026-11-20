#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Build the pinned SDL3, FreeType and HarfBuzz as static libraries for iPhone and iPad.

The iOS and iPadOS build (platforms/ios) links them into the game. Each
system image (SDK) gets its own prefix root, <deps>/ios-<sdk>, with one
install prefix per library (sdl/, freetype/, harfbuzz/): iphonesimulator for the
simulator on this Mac, iphoneos for devices. Both are built for arm64 and
run from iOS 15.0. The sources are the engine's pins: the SDL release
tools/bootstrap_sdl.py names and the FreeType and HarfBuzz releases and
options tools/bootstrap_text_fonts.py names, from the same verified archives in
<deps>. zlib comes with the system image, and the fonts are the host's
(local/deps/text-fonts), which are the same on every platform.

Build trees live beside each root (<root>-build). Each prefix records in
build-settings.json what its library was built from and how: the version,
the archive's SHA-256, the system image and its version, the deployment
target, the architecture and the options, and for SDL the patches applied
to it (tools/sdl-patches), each by its SHA-256 and name. A library whose
prefix is complete and records the settings of this run is not built again;
any other is built afresh, so an SDL built before its release had patches,
or with a patch that has changed since, is built again from the patched
source. Nothing outside the archive cache, the roots and their build trees
is modified.

--self-test checks, in a scratch folder, which SDL prefixes a run reuses
and which it builds again, and that SDL's settings name the patches of
tools/sdl-patches. It builds and downloads nothing, needs no Xcode tools
and runs on any system. Exit status: 0 when the libraries are ready (or
the self-test passed); 1 when the self-test fails.
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

ENGINE_ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ENGINE_ROOT / "tools"))
import bootstrap_sdl  # noqa: E402
import bootstrap_text_fonts  # noqa: E402

# The system images the libraries are built for: the simulator's and the
# devices'.
SDKS = ("iphonesimulator", "iphoneos")
ARCHITECTURE = "arm64"
DEPLOYMENT_TARGET = "15.0"
DEFAULT_JOBS = 3
SETTINGS_FILE = "build-settings.json"
# SDL's own options: a static library without its tests and examples, read
# with the options file tools/bootstrap_sdl.py builds the host's SDL with.
SDL_OPTIONS = [
    "-DSDL_SHARED=OFF",
    "-DSDL_STATIC=ON",
    "-DSDL_TEST_LIBRARY=OFF",
    "-DSDL_TESTS=OFF",
    "-DSDL_EXAMPLES=OFF",
    f"-DCMAKE_PROJECT_INCLUDE={(ENGINE_ROOT / 'tools' / 'sdl_build_options.cmake').as_posix()}",
]
SDL_INSTALLED = "lib/cmake/SDL3/SDL3Config.cmake"
# The files a complete SDL prefix holds.
SDL_FILES = (SDL_INSTALLED, "lib/libSDL3.a")
# The system image version the self-test records its made-up prefixes for,
# and its made-up patch, at the bytes an older prefix was built with and at
# the bytes it has now, written as bytes so that no platform changes their
# line endings.
SELF_TEST_SDK_VERSION = "26.0"
SELF_TEST_PATCH = f"0001-change{bootstrap_sdl.PATCH_SUFFIX}"
SELF_TEST_OLDER_PATCH = "the change as it was\n"
SELF_TEST_PATCH_TEXT = "the change as it is now\n"


def sdk_version(sdk):
    """The version of the system image the Xcode tools hold for sdk, e.g. "26.2"."""
    result = subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-version"], check=True,
                            capture_output=True, text=True)
    return result.stdout.strip()


def settings(version, sha256, sdk, image_version, options):
    """Returns what a library's prefix records it was built from and how.

    @param version the library's release
    @param sha256 the SHA-256 of its archive
    @param sdk the system image it is built for
    @param image_version that system image's version, as sdk_version gives it
    @param options its CMake options
    @return the settings
    """
    return {"version": version, "sha256": sha256, "sdk": sdk, "sdk_version": image_version,
            "deployment_target": DEPLOYMENT_TARGET, "architecture": ARCHITECTURE,
            "options": options}


def sdl_patches(patches=bootstrap_sdl.PATCHES):
    """Lists the patches applied to the pinned SDL as its prefix records them.

    @param patches the folder that holds a folder of patches per release
    @return each patch's SHA-256 and name, in the order they apply; empty for a release without patches
    """
    return (bootstrap_sdl.patch_stamp(bootstrap_sdl.VERSION, patches) or "").splitlines()


def sdl_settings(sdk, image_version, patches=bootstrap_sdl.PATCHES):
    """Returns what SDL's prefix records it was built from and how, its patches among them.

    @param sdk the system image it is built for
    @param image_version that system image's version
    @param patches the folder that holds a folder of patches per release
    @return the settings
    """
    wanted = settings(bootstrap_sdl.VERSION, bootstrap_sdl.SHA256, sdk, image_version, SDL_OPTIONS)
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


def launcher_options():
    """The compiler launcher options: ccache when this machine has it, else none."""
    if shutil.which("ccache") is None:
        return []
    return ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
            "-DCMAKE_OBJC_COMPILER_LAUNCHER=ccache"]


def generator_options():
    """Ninja when this machine has it, else CMake's default generator."""
    return ["-G", "Ninja"] if shutil.which("ninja") is not None else []


def build_for_ios(source, build, install, sdk, jobs, options):
    """Builds and installs a CMake project for sdk, arm64, afresh."""
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(install, ignore_errors=True)
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), *generator_options(),
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=iOS",
                    f"-DCMAKE_OSX_SYSROOT={sdk}", f"-DCMAKE_OSX_ARCHITECTURES={ARCHITECTURE}",
                    f"-DCMAKE_OSX_DEPLOYMENT_TARGET={DEPLOYMENT_TARGET}",
                    f"-DCMAKE_INSTALL_PREFIX={install}", *launcher_options(), *options], check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", str(jobs)],
                   check=True)
    subprocess.run(["cmake", "--install", str(build), "--config", "Release"], check=True)


def bootstrap(deps, sdk, jobs):
    """Builds what is not current of SDL, FreeType and HarfBuzz for sdk; returns the prefix root."""
    root = deps / f"ios-{sdk}"
    builds = deps / f"ios-{sdk}-build"
    image_version = sdk_version(sdk)
    sdl_install = root / "sdl"
    sdl_wanted = sdl_settings(sdk, image_version)
    if not is_current(sdl_install, SDL_FILES, sdl_wanted):
        build_for_ios(bootstrap_sdl.sdl_source(deps), builds / "sdl", sdl_install, sdk, jobs,
                      SDL_OPTIONS)
        record(sdl_install, sdl_wanted)
    freetype_install = root / "freetype"
    freetype_settings = settings(bootstrap_text_fonts.FREETYPE_VERSION,
                                 bootstrap_text_fonts.FREETYPE_SHA256, sdk, image_version,
                                 bootstrap_text_fonts.FREETYPE_OPTIONS)
    if not is_current(freetype_install, [bootstrap_text_fonts.FREETYPE_INSTALLED], freetype_settings):
        build_for_ios(bootstrap_text_fonts.freetype_source(deps), builds / "freetype",
                      freetype_install, sdk, jobs, bootstrap_text_fonts.FREETYPE_OPTIONS)
        record(freetype_install, freetype_settings)
    harfbuzz_install = root / "harfbuzz"
    harfbuzz_options = bootstrap_text_fonts.harfbuzz_cmake_options(freetype_install)
    harfbuzz_settings = settings(bootstrap_text_fonts.HARFBUZZ_VERSION,
                                 bootstrap_text_fonts.HARFBUZZ_SHA256, sdk, image_version,
                                 harfbuzz_options)
    if not is_current(harfbuzz_install, [bootstrap_text_fonts.HARFBUZZ_INSTALLED], harfbuzz_settings):
        build_for_ios(bootstrap_text_fonts.harfbuzz_source(deps), builds / "harfbuzz",
                      harfbuzz_install, sdk, jobs, harfbuzz_options)
        record(harfbuzz_install, harfbuzz_settings)
    print(f"SDL {bootstrap_sdl.VERSION}, FreeType {bootstrap_text_fonts.FREETYPE_VERSION} "
          f"and HarfBuzz {bootstrap_text_fonts.HARFBUZZ_VERSION} "
          f"for {sdk} {image_version}, {ARCHITECTURE}, iOS {DEPLOYMENT_TARGET} and later: "
          f"{root}")
    return root


def self_test():
    """Checks which SDL prefixes a run reuses, in a scratch folder; returns the exit status.

    @return 0 when every check passes, else 1
    """
    failures = []
    sdk = SDKS[0]
    with tempfile.TemporaryDirectory() as scratch:
        root = pathlib.Path(scratch)
        patches = root / "sdl-patches"
        (patches / bootstrap_sdl.VERSION).mkdir(parents=True)
        patch = patches / bootstrap_sdl.VERSION / SELF_TEST_PATCH
        patch.write_bytes(SELF_TEST_OLDER_PATCH.encode())
        older = sdl_settings(sdk, SELF_TEST_SDK_VERSION, patches)
        patch.write_bytes(SELF_TEST_PATCH_TEXT.encode())
        wanted = sdl_settings(sdk, SELF_TEST_SDK_VERSION, patches)
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
            ("this run's settings for another system image version", True,
             {**wanted, "sdk_version": "1.0"}, False),
            ("no settings", True, None, False),
            ("settings that cannot be read", True, "{", False),
            ("this run's settings but no library", False, wanted, False),
        )
        for what, complete, recorded, reused in cases:
            install = root / "sdl"
            shutil.rmtree(install, ignore_errors=True)
            install.mkdir()
            for name in SDL_FILES if complete else SDL_FILES[:1]:
                (install / name).parent.mkdir(parents=True, exist_ok=True)
                (install / name).write_text("", encoding="utf-8")
            if isinstance(recorded, str):
                (install / SETTINGS_FILE).write_text(recorded, encoding="utf-8")
            elif recorded is not None:
                record(install, recorded)
            with contextlib.redirect_stdout(io.StringIO()):
                got = is_current(install, SDL_FILES, wanted)
            if got != reused:
                failures.append(f"a prefix with {what} is {'reused' if got else 'built again'}, "
                                f"not {'reused' if reused else 'built again'}")
    # The engine's own patches of the pinned release, as tools/sdl-patches holds them.
    folder = bootstrap_sdl.PATCHES / bootstrap_sdl.VERSION
    own = sorted(folder.glob(f"*{bootstrap_sdl.PATCH_SUFFIX}")) if folder.is_dir() else []
    expected = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}" for path in own]
    recorded = sdl_settings(sdk, SELF_TEST_SDK_VERSION).get("patches")
    if recorded != expected:
        failures.append(f"SDL {bootstrap_sdl.VERSION}'s settings name the patches {recorded}, not {expected}")
    for failure in failures:
        print(f"bootstrap_ios_deps self-test: {failure}")
    if failures:
        return 1
    print(f"bootstrap_ios_deps self-test: {len(cases)} SDL prefixes reused or built again as expected; "
          f"SDL {bootstrap_sdl.VERSION}'s settings name the {len(expected)} "
          f"patch{'' if len(expected) == 1 else 'es'} of tools/sdl-patches/{bootstrap_sdl.VERSION}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sdk", choices=(*SDKS, "all"), action="append",
                        help="system image to build for; repeat it, or name all "
                             "(default: iphonesimulator)")
    parser.add_argument("--deps", type=pathlib.Path, default=ENGINE_ROOT / "local" / "deps",
                        help="archive and source cache, and the place of the prefixes "
                             "(default: local/deps)")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                        help=f"parallel build jobs (default: {DEFAULT_JOBS})")
    parser.add_argument("--self-test", action="store_true",
                        help="check which SDL prefixes a run builds again, in a scratch folder, and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if sys.platform != "darwin":
        parser.error("the iOS libraries are built on macOS with the Xcode tools")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    wanted = args.sdk or ["iphonesimulator"]
    sdks = list(SDKS) if "all" in wanted else list(dict.fromkeys(wanted))
    deps = args.deps.resolve()
    for sdk in sdks:
        bootstrap(deps, sdk, args.jobs)
    text_fonts = deps / "text-fonts"
    if not text_fonts.is_dir():
        print(f"The fonts are missing from {text_fonts}: run python3 tools/bootstrap_text_fonts.py "
              "--fonts-only from the engine's folder")
    return 0


if __name__ == "__main__":
    sys.exit(main())
