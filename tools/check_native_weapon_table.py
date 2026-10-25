#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a raised weapon-types limit keeps a mod's weapons and logs the rest.

A headless skirmish over the ridge-wide fixture (limits.weapon-types 1024,
with one weapon section past that limit) must exit 0 and print, once, the
line that names the limit and the highest left-out ID. The same skirmish
over the installation alone must exit 0 and print no such line.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 30
SEED = 4242
# The fixture's ridge-wide profile leaves one section out: ID 2000, past 1024.
LEFT_OUT = (
    "open-annihilation: weapons: 1 weapon section has an ID past the limit of "
    "1024 weapon types and is left out; the highest ID is 2000"
)


def past_limit_lines(text):
    return [line for line in text.splitlines() if "weapons:" in line and "past the limit" in line]


def skirmish(native, game_dir, workdir, mod_dir):
    preferences = workdir / "preferences.conf"
    preferences.write_text("open-annihilation-preferences 1\n")
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--preferences-file",
        str(preferences),
        "--match-ticks",
        str(TICKS),
        "--seed",
        str(SEED),
    ]
    if mod_dir is not None:
        command += ["--mod-dir", str(mod_dir)]
    return subprocess.run(
        command,
        cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=600,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )


def require_ok(result, name):
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"{name} exited with {result.returncode}")
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--fixture-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    fixture = args.fixture_dir.resolve() / "ridge-wide"
    if not fixture.is_dir():
        raise SystemExit(f"{fixture} is not a directory")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="native-weapon-table-", dir=args.scratch_root.resolve()
    ) as temporary:
        workdir = Path(temporary)
        wide = require_ok(skirmish(native, game_dir, workdir, fixture), "ridge-wide")
        found = past_limit_lines(wide)
        if found != [LEFT_OUT]:
            raise SystemExit(f"ridge-wide logged {found}, not the one left-out line")
        stock = require_ok(skirmish(native, game_dir, workdir, None), "the installation")
        stock_lines = past_limit_lines(stock)
        if stock_lines:
            raise SystemExit(f"the installation logged {stock_lines}")
    print("native weapon table: ridge-wide logs ID 2000 past 1024 once; the installation does not")


if __name__ == "__main__":
    main()
