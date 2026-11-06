#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the launcher's argument forwarding, installation choice and failed-build
protection without game assets.

run.sh passes --game-dir from its option or OA_GAME_DIR and nothing else, so
without either the game uses the remembered folder or asks; it starts the file
the oa-game target builds, open-annihilation, never an older build's oa-game,
and on macOS the executable inside the application bundle open-annihilation.app;
--movie plays from the chosen installation's Data folder; a failed configure
or build stops it before anything launches. It takes SDL as built when its
CMake package is in either place SDL installs it, and bootstraps a missing
SDL with a Python 3 that runs, passing over a python3 that fails.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

# A path outside the checkout in the launcher's text: a '..' path part, the
# directory that holds the checkout (the dirname of $repo_dir, or $repo_dir
# with its end trimmed), a path in the user's home, or one under a root where
# installations and users' files are kept. Homebrew's prefix under /opt holds
# build tools, which the launcher may name to find them.
OUTSIDE_PATH_RE = re.compile(
    r"(?<![.\w])\.\.(?=[/\"'\s);&|]|$)"
    r"|\bdirname\b[^\n]*\$\{?repo_dir\b|\$\{repo_dir%"
    r"|\$\{?HOME\b|(?<![\w$}])~/"
    r"|(?<![\w.$}])/(?:Users|home|Volumes|mnt|media|opt(?!/homebrew/)|srv|Applications)/",
    re.MULTILINE)
# Paths outside the checkout that OUTSIDE_PATH_RE must find, each as a shell
# line might spell it.
OUTSIDE_PATH_SAMPLES = ('cd ..; pwd', 'x="${repo_dir%/*}/installs"', 'x="$(dirname "$repo_dir")/installs"',
                        'x="$HOME/installs"', 'x=~/installs', 'x="/Volumes/installs"', 'x="/opt/installs"')
# Paths of build tools that it must let pass.
BUILD_TOOL_PATH_SAMPLES = ('PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig"',)
# The file the oa-game target builds.
GAME_FILE = "open-annihilation"


class LauncherFailure(Exception):
    """A launcher behaviour that differs from the expected one."""


def expect(condition, what):
    """Fails the check with `what` unless `condition` holds."""
    if not condition:
        raise LauncherFailure(what)


def check(root, source):
    """Runs the launcher copy in `root` through every case; raises LauncherFailure."""
    repo = root / "repo"
    repo.mkdir()
    shutil.copy2(source / "run.sh", repo / "run.sh")
    # The files run.sh looks for before it bootstraps the pinned SDL, and
    # FreeType with the text fonts, so that it bootstraps neither. It takes
    # FreeType of any release, freetype-install-*.
    (repo / "local/deps/sdl-install/lib/cmake/SDL3").mkdir(parents=True)
    (repo / "local/deps/sdl-install/lib/cmake/SDL3/SDL3Config.cmake").touch()
    (repo / "local/deps/text-fonts").mkdir(parents=True)
    (repo / "local/deps/text-fonts/build-settings.json").touch()
    freetype_config = repo / "local/deps/freetype-install-any/lib/cmake/freetype"
    freetype_config.mkdir(parents=True)
    (freetype_config / "freetype-config.cmake").touch()
    (root / "bin").mkdir()
    (root / "build").mkdir()
    # The installation lies beside the checkout, where a workspace might keep
    # one: the launcher must never pick it up by itself. A default of another
    # name or place would pass that case, so the launcher's text must name no
    # path outside the checkout either.
    game = root / "game assets"
    (game / "Data").mkdir(parents=True)
    (game / "Data/3.ZRB").touch()
    (game / "totala1.hpi").touch()
    launcher_text = (source / "run.sh").read_text()
    for sample in OUTSIDE_PATH_SAMPLES:
        expect(OUTSIDE_PATH_RE.search(sample) is not None, f"the text check misses {sample!r}")
    for sample in BUILD_TOOL_PATH_SAMPLES:
        expect(OUTSIDE_PATH_RE.search(sample) is None, f"the text check refuses {sample!r}")
    outside = OUTSIDE_PATH_RE.search(launcher_text)
    expect(outside is None, f"run.sh names a path outside the checkout: {outside and outside.group(0)!r}")
    # cmake records the OA_GAME_DIR it was started with, which its
    # configure reads too.
    configured_install = root / "configured-install.txt"
    cmake = root / "bin/cmake"
    cmake.write_text('#!/bin/sh\nif [ "$1" = --build ]; then exit "${BUILD_RESULT:-0}"; fi\n'
                     f'printf "%s" "${{OA_GAME_DIR-}}" >"{configured_install}"\n'
                     'exit "${CONFIGURE_RESULT:-0}"\n')
    cmake.chmod(0o755)
    record = root / "arguments.json"
    # The files the oa-game and oa-intro targets build, and the file an older
    # build left under the game target's name, which must never start.
    for name in (GAME_FILE, "oa-intro", "oa-game"):
        executable = root / "build" / name
        executable.write_text(
            f'#!{sys.executable}\nimport json, os, sys\n'
            f'open(os.environ["LAUNCH_RECORD"], "w").write(json.dumps(["{name}", *sys.argv[1:]]))\n'
        )
        executable.chmod(0o755)
    clean_env = {key: value for key, value in os.environ.items() if not key.startswith("OA_")}
    env = dict(clean_env, PATH=str(root / "bin") + os.pathsep + os.environ["PATH"],
               LAUNCH_RECORD=str(record), OA_BUILD_DIR=str(root / "build"))
    options = ["--campaign", "Arm Campaign", "--mission", "0", "--match-ticks", "60",
               "--resolution", "1280x720", "--load", "a save.sav", "--play-demo", "a game.tad",
               "--net-loopback-check", "300", "--debug-order-lines"]
    # The stock macOS bash is 3.2, where an empty array under set -u is unbound.
    shell = "/bin/bash" if Path("/bin/bash").exists() else "bash"
    run_sh = [shell, str(repo / "run.sh")]
    command = [*run_sh, "--game-dir", str(game), *options]

    def launched(arguments, environment=env, target=GAME_FILE, cwd=None):
        result = subprocess.run(arguments, env=environment, capture_output=True, cwd=cwd)
        expect(result.returncode == 0, f"{arguments} failed with {result.returncode}: {result.stderr!r}")
        expect(record.exists(), f"{arguments} launched nothing")
        forwarded = json.loads(record.read_text())
        record.unlink()
        expect(forwarded[0] == target, f"{arguments} launched {forwarded[0]}, not {target}")
        return forwarded[1:]

    def refused(arguments, environment=env):
        result = subprocess.run(arguments, env=environment, capture_output=True)
        expect(not record.exists(), f"{arguments} launched")
        return result

    forwarded = launched(command)
    expect(forwarded == ["--game-dir", str(game), *options], f"--game-dir forwarded as {forwarded}")
    for failure in ("CONFIGURE_RESULT", "BUILD_RESULT"):
        result = refused(command, dict(env, **{failure: "17"}))
        expect(result.returncode == 17, f"a failed {failure} ends the launcher with {result.returncode}")

    # Without --game-dir or OA_GAME_DIR nothing names a folder, whatever lies
    # beside the checkout: the game uses the remembered one or asks.
    forwarded = launched([*run_sh, *options])
    expect(forwarded == options, f"no installation forwarded as {forwarded}")
    forwarded = launched(run_sh)
    expect(forwarded == [], f"a bare start forwarded {forwarded}")
    forwarded = launched([*run_sh, "--choose-game-dir", *options])
    expect(forwarded == ["--choose-game-dir", *options], f"--choose-game-dir forwarded as {forwarded}")

    with_install = dict(env, OA_GAME_DIR=str(game))
    forwarded = launched([*run_sh, *options], with_install)
    expect(forwarded == ["--game-dir", str(game), *options], f"OA_GAME_DIR forwarded as {forwarded}")
    forwarded = launched([*run_sh, "--choose-game-dir"], with_install)
    expect(forwarded == ["--choose-game-dir"], f"--choose-game-dir over OA_GAME_DIR forwarded {forwarded}")
    result = refused(run_sh, dict(env, OA_GAME_DIR=str(root / "missing")))
    expect(result.returncode == 1 and b"does not exist" in result.stderr,
           f"a missing OA_GAME_DIR ends with {result.returncode}: {result.stderr!r}")
    # A relative OA_GAME_DIR names a folder below the caller's directory, and
    # CMake, which refuses a relative one, is handed the absolute path.
    forwarded = launched(run_sh, dict(env, OA_GAME_DIR=game.name), cwd=root)
    expect(len(forwarded) == 2 and forwarded[0] == "--game-dir" and os.path.samefile(forwarded[1], game)
           and os.path.isabs(forwarded[1]), f"a relative OA_GAME_DIR forwarded as {forwarded}")
    configured_with = configured_install.read_text()
    expect(os.path.isabs(configured_with) and os.path.samefile(configured_with, game),
           f"CMake was started with OA_GAME_DIR={configured_with!r}")

    # --movie plays from the chosen installation's Data folder and needs one.
    # A file system that folds case may spell the movie either way.
    def plays_movie(forwarded):
        return len(forwarded) == 1 and os.path.samefile(forwarded[0], game / "Data/3.ZRB")

    forwarded = launched([*run_sh, "--movie", "3"], with_install, target="oa-intro")
    expect(plays_movie(forwarded), f"--movie 3 played {forwarded}")
    forwarded = launched([*run_sh, "--game-dir", str(game), "--movie", "3"], target="oa-intro")
    expect(plays_movie(forwarded), f"--movie 3 with --game-dir played {forwarded}")
    result = refused([*run_sh, "--movie", "3"])
    expect(result.returncode == 2 and b"--game-dir" in result.stderr,
           f"--movie without an installation ends with {result.returncode}: {result.stderr!r}")
    result = refused([*run_sh, "--movie", "2"], with_install)
    expect(result.returncode == 1 and b"missing" in result.stderr,
           f"a missing movie ends with {result.returncode}: {result.stderr!r}")

    # SDL built on Windows keeps its CMake package in cmake, not lib/cmake/SDL3;
    # either one is the built SDL, and the copy holds no bootstrap to run.
    sdl_unix_config = repo / "local/deps/sdl-install/lib/cmake/SDL3/SDL3Config.cmake"
    sdl_windows_config = repo / "local/deps/sdl-install/cmake/SDL3Config.cmake"
    sdl_unix_config.unlink()
    sdl_windows_config.parent.mkdir(parents=True)
    sdl_windows_config.touch()
    forwarded = launched(run_sh)
    expect(forwarded == [], f"SDL's Windows layout forwarded {forwarded}")
    # Without SDL the launcher bootstraps it with a Python 3 that runs: a
    # python3 that fails when run, as the Microsoft Store's placeholder does,
    # is passed over for python.
    sdl_windows_config.unlink()
    store_bin = root / "store-bin"
    store_bin.mkdir()
    bootstraps = root / "bootstraps.txt"
    (store_bin / "python3").write_text('#!/bin/sh\necho "Python was not found" >&2\nexit 49\n')
    (store_bin / "python").write_text('#!/bin/sh\nif [ "$1" = -c ]; then exit 0; fi\n'
                                      f'printf "%s\\n" "$1" >>"{bootstraps}"\n')
    for stub in store_bin.iterdir():
        stub.chmod(0o755)
    forwarded = launched(run_sh, dict(env, PATH=str(store_bin) + os.pathsep + env["PATH"]))
    ran = bootstraps.read_text().splitlines() if bootstraps.exists() else []
    expect(forwarded == [] and len(ran) == 1 and Path(ran[0]).name == "bootstrap_sdl.py"
           and os.path.samefile(Path(ran[0]).parent.parent, repo),
           f"a missing SDL ran {ran} and forwarded {forwarded}")
    sdl_unix_config.touch()

    # A macOS build makes the game an application bundle, whose executable
    # is started in place, before a bare file of the same name.
    bundled = root / "build" / f"{GAME_FILE}.app" / "Contents" / "MacOS" / GAME_FILE
    bundled.parent.mkdir(parents=True)
    bundled.write_text(
        f'#!{sys.executable}\nimport json, os, sys\n'
        f'open(os.environ["LAUNCH_RECORD"], "w").write(json.dumps(["{GAME_FILE}.app", *sys.argv[1:]]))\n'
    )
    bundled.chmod(0o755)
    forwarded = launched([*run_sh, *options], target=f"{GAME_FILE}.app")
    expect(forwarded == options, f"the bundle's executable was given {forwarded}")


def main():
    source = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="oa-launcher-") as temporary:
        try:
            check(Path(temporary), source)
        except LauncherFailure as failure:
            print(f"launcher check failed: {failure}", file=sys.stderr)
            return 1
    print("The launcher forwards application arguments, passes only --game-dir or OA_GAME_DIR, "
          "plays movies from that installation and stops on build failure")
    return 0


if __name__ == "__main__":
    sys.exit(main())
