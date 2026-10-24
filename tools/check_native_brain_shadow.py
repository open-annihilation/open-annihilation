#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a brain running in shadow beside the built-in player.

The headless skirmish of tools/check_native_trace.py (seed 1234567), for 600
ticks, with and without steady-shadow on task9: the trace streams and the unit
dumps match byte for byte, and the shadowed run prints no difference. With
sort compared, the line names tick 29 and the log directory holds that tick's
two logs.

The game's working directory is the scratch folder, so the brain is an
absolute path beside this file. The load's player pass runs once before the
timed ticks, so those 600 ticks compare 601 passes and the first sort is at
tick 29.

Legs are positional. `inert` is this check. `classic` plays three skirmishes
with `--brain` and `--sections` and requires every `brain shadow: seat` line
to end with `no difference`.
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
# The load's player pass, then one pass for each timed tick.
COMPARED = TICKS + 1
SORT_TICK = 29
SEED = 1234567
BRAIN = Path(__file__).resolve().parents[1] / "tests" / "data" / "brains" / "steady-shadow"


def run(native, game_dir, workdir, *extra, timeout=600):
    profile = workdir / "preferences.conf"
    user = workdir / "user"
    user.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(profile), "--user-folder", str(user),
         *map(str, extra)],
        cwd=workdir, timeout=timeout, check=False, capture_output=True, text=True,
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
    if f"start not compared, {COMPARED} ticks, no difference" not in shadow_out:
        print(shadow_out, end="")
        raise SystemExit(
            f"the task9 shadow did not name {COMPARED} ticks with no difference"
        )
    logs = workdir / "sort-logs"
    sort_stream, sort_units, sort_out = skirmish(
        native, game_dir, workdir, "sort",
        "--brain-shadow", BRAIN, "--brain-shadow-sections", "sort",
        "--brain-shadow-log", logs,
    )
    if f"first difference at tick {SORT_TICK} in sort" not in sort_out:
        print(sort_out, end="")
        raise SystemExit(f"the sort shadow did not name tick {SORT_TICK} in sort")
    builtin = list(logs.glob(f"seat-*-tick-{SORT_TICK}-builtin.log"))
    brain = list(logs.glob(f"seat-*-tick-{SORT_TICK}-brain.log"))
    if len(builtin) != 1 or len(brain) != 1:
        raise SystemExit(f"tick {SORT_TICK} logs: {len(builtin)} builtin, {len(brain)} brain")
    if builtin[0].stat().st_size == 0 or brain[0].stat().st_size == 0:
        raise SystemExit(f"tick {SORT_TICK} logs are empty")
    # The sort run is a different comparison. Its stream still has to be a
    # complete trace of the same skirmish: the shadow does not play the seat.
    if plain_stream.read_bytes() != sort_stream.read_bytes():
        raise SystemExit("a sort shadow changed the trace stream")
    if plain_units.read_bytes() != sort_units.read_bytes():
        raise SystemExit("a sort shadow changed the unit dump")
    print(f"native brain shadow: {TICKS} ticks from seed {SEED} repeat, "
          f"task9 has no difference, sort names tick {SORT_TICK}")


RIDGE = """\
oamod: 1
id: ridge
name: Ridge
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-24, packager: Open Annihilation}
hacks:
  ai.attack-wave-size: 10
  ai.patrol-group-size: 15
  ai.builder-withhold-threshold: 10
  ai.difficulty-names: [hard, medium, easy]
  ai.squad-assignment: role-squads
  ai.nearest-enemy-filter: skip-submerged
  ai.squad5-factory-tick: true
  ai.patrol-null-enemy-skip: true
  ai.commander-keeps-orders-when-damaged: true
  ai.factory-tick-filter:
    skip-busy-background-queue: true
    power-toggle: energy-use-32
"""

CLASSIC_TICKS = 3000
CLASSIC_TIMEOUT = 500


def four_player_preferences(path):
    """Four slots: local Arm, Arm computer, Core computer, Arm computer."""
    path.write_text(
        "open-annihilation-preferences 1\n"
        "\"Total Annihilation|NumSkirmishPlayers\" \"4\"\n"
        "\"Total Annihilation\\\\Skirmish|Player0Controller\" \"1\"\n"
        "\"Total Annihilation\\\\Skirmish|Player0Side\" \"0\"\n"
        "\"Total Annihilation\\\\Skirmish|Player1Controller\" \"2\"\n"
        "\"Total Annihilation\\\\Skirmish|Player1Side\" \"0\"\n"
        "\"Total Annihilation\\\\Skirmish|Player2Controller\" \"2\"\n"
        "\"Total Annihilation\\\\Skirmish|Player2Side\" \"1\"\n"
        "\"Total Annihilation\\\\Skirmish|Player3Controller\" \"2\"\n"
        "\"Total Annihilation\\\\Skirmish|Player3Side\" \"0\"\n",
        encoding="utf-8",
    )


def require_no_difference(output, name):
    """Every brain-shadow seat line must end with no difference."""
    found = False
    for line in output.splitlines():
        if not line.startswith("brain shadow: seat "):
            continue
        found = True
        if not line.endswith("no difference"):
            print(output, end="")
            raise SystemExit(f"{name}: a seat line did not end with no difference")
    if not found:
        print(output, end="")
        raise SystemExit(f"{name}: no brain shadow seat line")


def classic_skirmish(native, game_dir, workdir, name, *extra):
    workdir.mkdir(parents=True, exist_ok=True)
    result = run(native, game_dir, workdir, *extra, timeout=CLASSIC_TIMEOUT)
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(f"skirmish {name} exited with {result.returncode}")
    require_no_difference(output, name)
    return output


def check_classic(native, game_dir, workdir, brain, sections):
    ridge = workdir / "ridge.yaml"
    ridge.parent.mkdir(parents=True, exist_ok=True)
    ridge.write_text(RIDGE, encoding="utf-8")
    shadow = ["--brain-shadow", str(brain), "--brain-shadow-sections", sections]
    classic_skirmish(
        native, game_dir, workdir / "seed", "classic-seed",
        "--match-ticks", CLASSIC_TICKS, "--seed", SEED, *shadow,
    )
    sides = workdir / "sides"
    sides.mkdir(parents=True, exist_ok=True)
    four_player_preferences(sides / "preferences.conf")
    classic_skirmish(
        native, game_dir, sides, "classic-sides",
        "--match-ticks", CLASSIC_TICKS, "--seed", "4242", *shadow,
    )
    ridged = workdir / "ridge"
    ridged.mkdir(parents=True, exist_ok=True)
    four_player_preferences(ridged / "preferences.conf")
    classic_skirmish(
        native, game_dir, ridged, "classic-ridge",
        "--match-ticks", CLASSIC_TICKS, "--seed", "4242", "--mod", ridge, *shadow,
    )
    print(f"native brain shadow classic: schedule,sort on three skirmishes, no difference")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--brain", type=Path)
    parser.add_argument("--sections", default="")
    parser.add_argument("legs", nargs="*")
    args = parser.parse_args()
    legs = args.legs or ["inert"]
    for leg in legs:
        if leg not in ("inert", "classic"):
            raise SystemExit(f"unknown leg: {leg}")
        if leg == "classic" and (args.brain is None or args.sections == ""):
            raise SystemExit("classic needs --brain and --sections")
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-brain-shadow-", dir=args.scratch_root.resolve()) as temporary:
        root = Path(temporary)
        for leg in legs:
            if leg == "inert":
                check_inert(native, game_dir, root / "inert")
            else:
                check_classic(native, game_dir, root / "classic", args.brain.resolve(), args.sections)


if __name__ == "__main__":
    main()
