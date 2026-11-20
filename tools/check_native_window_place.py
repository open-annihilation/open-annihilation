#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a window opens again at the size it was left.

The game starts twice on the dummy video driver with --check-window-place.
The first start saves a window that fits the display. The second start must
open at that size and say so. A run with --resolution, and a run with -d,
must leave the saved place byte for byte and say the command line was kept.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
RUN_TIMEOUT = 180
PLACE_KEY = "open-annihilation.window-place"


class PlaceFailure(Exception):
    """A start that did not keep the window's place."""


def run_game(native, game_dir, workdir, preferences, extra):
    """Starts one check and returns its combined output and status."""
    environment = dict(os.environ)
    environment["SDL_VIDEO_DRIVER"] = "dummy"
    environment["SDL_AUDIO_DRIVER"] = "dummy"
    result = subprocess.run(
        [
            *RUNNER,
            str(native),
            "--game-dir",
            str(game_dir),
            "--skip-intro",
            "--mute",
            "--preferences-file",
            str(preferences),
            "--check-window-place",
            *extra,
        ],
        cwd=workdir,
        env=environment,
        timeout=RUN_TIMEOUT,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise PlaceFailure(f"open-annihilation exited with {result.returncode}")
    return output


def place_line(output, label):
    """Returns the size text of the window-place line `label` names."""
    prefix = f"open-annihilation: window place: {label} "
    for line in output.splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :]
    print(output, end="")
    raise PlaceFailure(f"no window place line saying {label}")


def write_place(preferences, value):
    """Writes a preferences file whose only entry is the window place."""
    preferences.write_text(
        "open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n"
        f'"{PLACE_KEY}" "{value}"\n',
        encoding="utf-8",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    workdir = args.scratch_root.resolve() / "G94-native-window-place"
    workdir.mkdir(parents=True, exist_ok=True)
    preferences = workdir / "window-place.conf"
    if preferences.exists():
        preferences.unlink()
    saved = place_line(run_game(native, game_dir, workdir, preferences, []), "saved")
    stored = preferences.read_bytes()
    opened = place_line(run_game(native, game_dir, workdir, preferences, []), "opened")
    if opened != saved:
        raise PlaceFailure(f"opened {opened} after saving {saved}")
    if preferences.read_bytes() != stored:
        raise PlaceFailure("the second start rewrote the saved place")

    command = workdir / "command-line.conf"
    write_place(command, "window 800x600 at 10,20 on 0,0,1920,1080")
    before = command.read_bytes()
    kept = run_game(native, game_dir, workdir, command, ["--resolution", "640x480"])
    if "open-annihilation: window place: command line kept" not in kept.splitlines():
        print(kept, end="")
        raise PlaceFailure("--resolution did not keep the command line")
    if command.read_bytes() != before:
        raise PlaceFailure("--resolution rewrote the saved place")

    write_place(command, "full-screen on 0,0,1920,1080 window 800x600 at 10,20")
    before = command.read_bytes()
    kept = run_game(native, game_dir, workdir, command, ["-d"])
    if "open-annihilation: window place: command line kept" not in kept.splitlines():
        print(kept, end="")
        raise PlaceFailure("-d did not keep the command line")
    if command.read_bytes() != before:
        raise PlaceFailure("-d rewrote the saved place")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PlaceFailure as failure:
        print(f"native-window-place: {failure}", file=sys.stderr)
        sys.exit(1)
