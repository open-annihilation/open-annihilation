#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that --stage sets up a headless skirmish as its file says.

A skirmish of the local player against one computer player is staged with
solar collectors and a missile defence beside the local commander, the
defence asked for stockpiled rounds, light infantry of the computer player
placed beside the local commander and a construction kbot, set to hold its
ground, asked to build a solar collector; a kbot lab is placed and given
two peewees to build, and every unit of the computer player that moves but
the light infantry is sent on patrol. The skirmish is then played for TICKS
ticks and saved. The run must report each placement, the lab's queue and
the patrol of the computer's commander alone, keep the defence's stockpile
order and the lab's build among the saved orders, count the staged types
and the lab's first peewee among the live units, and end on the same world
digest when staged twice. A stage line naming a type the game lacks must
stop the run with the file and line.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 90
SEED = 4242
STAGE = """# The local player's base and the computer player's raid.
unit 0 ARMSOLAR -150 -100
unit 0 ARMSOLAR -150 50
unit 0 ARMAMD 150 0
stockpile 2
group raid
unit 1 CORAK 0 250 0
unit 1 CORAK 40 250 0
group builder
unit 0 ARMCK 0 -200
move-order builder 0
build ARMSOLAR 0 -100
group
unit 0 ARMLAB 200 -200
queue ARMPW 2
patrol-all 1 1000 1000 raid
"""
# One line a placement prints.
PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player (\d+) at (-?\d+),(-?\d+)$", re.M)
TYPES = re.compile(r"^saveload: unit types \d+((?: \S+=\d+)*)$", re.M)
ORDERS = re.compile(r"^saveload: orders \d+((?: \S+=\d+)*)$", re.M)
DIGEST = re.compile(r"^saveload: tick (\d+) units (\d+) digest ([0-9a-f]{16})$", re.M)


def run(native, game_dir, workdir, name, stage_text):
    stage = workdir / f"{name}.stage"
    stage.write_text(stage_text)
    preferences = workdir / f"{name}.conf"
    preferences.write_text("open-annihilation-preferences 1\n")
    return subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--seed", str(SEED), "--match-ticks", str(TICKS), "--save-after",
         str(TICKS), "--save-file", str(workdir / f"{name}.sav"), "--stage", str(stage),
         "--preferences-file", str(preferences)],
        cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=600, check=False, capture_output=True, text=True, errors="replace")


def counts(line):
    return {name: int(count) for name, count in re.findall(r"(\S+)=(\d+)", line)}


def staged(result):
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"the staged run exited with {result.returncode}")
    out = result.stdout
    placed = PLACED.findall(out)
    if [(name, int(player)) for _, name, player, _, _ in placed] != [
            ("ARMSOLAR", 0), ("ARMSOLAR", 0), ("ARMAMD", 0), ("CORAK", 1), ("CORAK", 1),
            ("ARMCK", 0), ("ARMLAB", 0)]:
        raise SystemExit(f"the stage placed {placed}")
    if "stage: unit " + placed[2][0] + " stockpiles 2" not in out:
        raise SystemExit("the stage did not ask the defence for its rounds")
    if "stage: move-order builder: 1 units to 0" not in out:
        raise SystemExit("the stage did not set the kbot's standing move order")
    if f"stage: unit {placed[6][0]} queues 2 ARMPW" not in out:
        raise SystemExit("the stage did not queue the lab's peewees")
    # The computer's commander, without the light infantry the stage kept.
    if "stage: patrol-all 1: 1 units to 1000,1000" not in out:
        raise SystemExit("the stage did not send the computer's commander alone on patrol")
    types, orders, digest = TYPES.search(out), ORDERS.search(out), DIGEST.search(out)
    if not types or not orders or not digest:
        raise SystemExit("the run reported no units, orders or digest")
    live = counts(types.group(1))
    # The two staged solar collectors and the one the kbot has begun.
    if live.get("ARMSOLAR") != 3 or live.get("ARMAMD") != 1 or live.get("CORAK") != 2 or \
            live.get("ARMLAB") != 1 or live.get("ARMPW") != 1:
        raise SystemExit(f"the live units are {live}")
    if counts(orders.group(1)).get("BuildWeapon") != 1:
        raise SystemExit(f"the saved orders are {orders.group(1)}, without the defence's rounds")
    if counts(orders.group(1)).get("BuildingBuild", 0) < 1:
        raise SystemExit(f"the saved orders are {orders.group(1)}, without the lab's build")
    if not re.search(r"^stage: unit \d+ builds ARMSOLAR at -?\d+,-?\d+$", out, re.M) or \
            counts(orders.group(1)).get("MobileBuild", 0) + counts(orders.group(1)).get(
                "HelpBuild", 0) < 1:
        raise SystemExit(f"the saved orders are {orders.group(1)}, without the builder's")
    return digest.group(3)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    # The runs start in the scratch folder.
    arguments.native = arguments.native.absolute()
    arguments.game_dir = arguments.game_dir.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        first = staged(run(arguments.native, arguments.game_dir, workdir, "first", STAGE))
        second = staged(run(arguments.native, arguments.game_dir, workdir, "second", STAGE))
        if first != second:
            raise SystemExit(f"the same stage ended on digests {first} and {second}")
        missing = run(arguments.native, arguments.game_dir, workdir, "missing",
                      "unit 0 ARMSOLAR 0 -150\nunit 0 NOSUCHUNIT 0 0\n")
        if missing.returncode == 0 or "missing.stage:2: the game has no type NOSUCHUNIT" not in (
                missing.stdout + missing.stderr):
            print(missing.stdout + missing.stderr, end="")
            raise SystemExit("a stage naming a missing type did not stop at its line")
    print(f"stage check: the staged skirmish places, stockpiles, queues, patrols and plays to digest "
          f"{first} twice")


if __name__ == "__main__":
    main()
