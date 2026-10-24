#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a brain running in shadow beside the built-in player.

The headless skirmish of tools/check_native_trace.py (seed 1234567), for 600
ticks, with and without steady-shadow on task9: the trace streams and the unit
dumps match byte for byte, and the shadowed run prints no difference. With
sort compared, the line names tick 30 and the log directory holds that tick's
two logs.

Legs are positional. `inert` is this check. A later ticket adds its own leg.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 600
SEED = 1234567
BRAIN = "tests/data/brains/steady-shadow"


def run(native, game_dir, workdir, *extra):
    profile = workdir / "preferences.conf"
    user = workdir / "user"
    user.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(profile), "--user-folder", str(user),
         *map(str, extra)],
        cwd=workdir, timeout=600, check=False, capture_output=True, text=True,
    )
    return result


def skirmish(native, game_dir, workdir, name, *extra):
    stream = workdir / f"{name}.trace"
    units = workdir / f"{name}.units"
    result = run(
        native, game_dir, workdir, "--match-ticks", TICKS, "--seed", SEED,
        "--trace-digest", stream, "--trace-units", units, *extra,
    )
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"skirmish {name} exited with {result.returncode}")
    return stream, units, result.stdout


def check_inert(native, game_dir, workdir):
    plain_stream, plain_units, _ = skirmish(native, game_dir, workdir, "plain")
    shadow_stream, shadow_units, shadow_out = skirmish(
        native, game_dir, workdir, "shadow",
        "--brain-shadow", BRAIN, "--brain-shadow-sections", "task9",
    )
    if plain_stream.read_bytes() != shadow_stream.read_bytes():
        raise SystemExit("a task9 shadow changed the trace stream")
    if plain_units.read_bytes() != shadow_units.read_bytes():
        raise SystemExit("a task9 shadow changed the unit dump")
    if "start not compared, 600 ticks, no difference" not in shadow_out:
        print(shadow_out, end="")
        raise SystemExit("the task9 shadow did not report 600 ticks with no difference")
    logs = workdir / "sort-logs"
    sort_stream, sort_units, sort_out = skirmish(
        native, game_dir, workdir, "sort",
        "--brain-shadow", BRAIN, "--brain-shadow-sections", "sort",
        "--brain-shadow-log", logs,
    )
    if "first difference at tick 30 in sort" not in sort_out:
        print(sort_out, end="")
        raise SystemExit("the sort shadow did not name tick 30 in sort")
    builtin = list(logs.glob("seat-*-tick-30-builtin.log"))
    brain = list(logs.glob("seat-*-tick-30-brain.log"))
    if len(builtin) != 1 or len(brain) != 1:
        raise SystemExit(f"tick 30 logs: {len(builtin)} builtin, {len(brain)} brain")
    if builtin[0].stat().st_size == 0 or brain[0].stat().st_size == 0:
        raise SystemExit("tick 30 logs are empty")
    # The sort run is a different comparison. Its stream still has to be a
    # complete trace of the same skirmish: the shadow does not play the seat.
    if plain_stream.read_bytes() != sort_stream.read_bytes():
        raise SystemExit("a sort shadow changed the trace stream")
    if plain_units.read_bytes() != sort_units.read_bytes():
        raise SystemExit("a sort shadow changed the unit dump")
    print(f"native brain shadow: {TICKS} ticks from seed {SEED} repeat, "
          f"task9 has no difference, sort names tick 30")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("legs", nargs="*")
    args = parser.parse_args()
    legs = args.legs or ["inert"]
    for leg in legs:
        if leg != "inert":
            raise SystemExit(f"unknown leg: {leg}")
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-brain-shadow-", dir=args.scratch_root.resolve()) as temporary:
        check_inert(native, game_dir, Path(temporary))


if __name__ == "__main__":
    main()
