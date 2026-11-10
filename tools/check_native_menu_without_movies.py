#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the installation offers its movies without its Data folder.

The rules for game data with no movies, which gray out INTRO, hide Credits
and end a won campaign with a notice instead of the ending movies, belong to
data such as the Total Annihilation demo (1997). An installation of the game
keeps its movies' menu entries and ending whether or not its Data folder
holds the movies, since its game disc's archives are there.

The game starts headless twice, with --skip-intro --frames 60 on the dummy
SDL drivers: once on the installation --game-dir names, and once on a scratch
folder that links to everything in it but its Data folder. Both must end
with status 0, say that the game offers movies, and draw the same main menu,
byte for byte.

Where the scratch folder cannot link to the installation's files, the check
prints one line and exits with 77, which ctest reports as skipped.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# ctest's SKIP_RETURN_CODE.
SKIP = 77
# The folder of the installation that holds the movies, matched in any case.
MOVIE_FOLDER = "data"
# What a headless run prints when the game offers movies.
OFFERS_MOVIES = "native check: the game offers movies"
FRAMES = "60"
RUN_TIMEOUT = 900


class MenuFailure(Exception):
    """A behaviour that differs from the expected one."""



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding="utf-8") if target.is_file() else "open-annihilation-preferences 1\n"
    if "open-annihilation.update-check" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.update-check" "never"\n'
    if "open-annihilation.content-updates" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding="utf-8")


def main_menu(native, game_dir, workdir, name):
    """Starts open-annihilation headless on `game_dir`; returns the main menu it drew, as PPM bytes."""
    snapshot = workdir / f"{name}.ppm"
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    _offline_preferences(workdir / f"{name}.conf")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute", "--headless-check",
         "--frames", FRAMES, "--snapshot", str(snapshot),
         "--preferences-file", str(workdir / f"{name}.conf")],
        cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False, capture_output=True, text=True,
        errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise MenuFailure(f"open-annihilation exited with {result.returncode} on the {name} folder")
    if OFFERS_MOVIES not in output.splitlines():
        print(output, end="")
        raise MenuFailure(f"the {name} folder does not offer the game's movies")
    if not snapshot.is_file():
        raise MenuFailure(f"open-annihilation drew no main menu for the {name} folder")
    return snapshot.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-menu-without-movies-",
                                     dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        without_movies = workdir / "without-movies"
        without_movies.mkdir()
        try:
            for entry in game_dir.iterdir():
                if entry.name.lower() != MOVIE_FOLDER:
                    (without_movies / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())
        except OSError as error:
            print(f"skipped the menu check without movies: cannot link to the installation's files ({error})")
            return SKIP
        try:
            installed = main_menu(native, game_dir, workdir, "installation")
            linked = main_menu(native, without_movies, workdir, "without-movies")
            if installed != linked:
                raise MenuFailure("the main menu differs without the Data folder")
        except MenuFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print("the menu check without movies passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
