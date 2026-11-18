#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a builder whose build stance never comes goes on with its builds.

A headless skirmish is staged with six of the local player's advanced
construction kbots (CORACK), each ordered to build a line of six radar towers
(CORRAD), the lines' sites spaced differently, and each guarded by another
advanced construction kbot, with +atm entered every second so that no build
waits on metal or energy. When a builder finishes a tower it closes its
nanolathe and walks to the next site; when its walk stops while that closing
still runs, its StartBuilding finds all eight script threads busy and drops
the start-script that raises its build stance. The guard goes on to finish
the tower, and 3.1c leaves the builder waiting for that stance for ever, with
the rest of its line unbuilt; the engine looks at the builder's frame after
ten seconds without the stance and ends the order once the frame is
finished.

Every builder must be idle (Standby) when the run ends, its line worked
through, and every guard guarding its builder again. Before the change four
of the six builders were still in MobileBuild at the end.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

SEED = 4242
TICKS = 3000
TOWERS = 6
# The spacing of each line's sites, in pixels.
SPACINGS = (34, 38, 42, 46, 50, 56)
# Unit order kinds the trace reports (Unit order table indices).
GUARD = 18
MOBILE_BUILD = 25
STANDBY = 41
UNIT_LINE = re.compile(r"^stage: unit (\d+) CORACK of player \d+ at -?\d+,-?\d+$")
GROUP_LINE = re.compile(r"^stage: group (\S+)$")
TRACE = re.compile(r"^tick=(\d+) slot=(\d+) .*? order_kind=(\d+) ", re.M)


def stage_text():
    lines = ["# Advanced construction kbots building lines of radar towers, each guarded.",
             "console +atm"]
    for row, spacing in enumerate(SPACINGS):
        z = -400 + row * 120
        lines += [f"group builder{row}", f"unit 0 CORACK 200 {z}"]
        lines += [f"build CORRAD {48 + tower * spacing} 0" for tower in range(TOWERS)]
        lines += [f"group guard{row}", f"unit 0 CORACK 160 {z + 40}", "group",
                  f"guard guard{row} builder{row}"]
    lines += [f"at {tick} console +atm" for tick in range(30, TICKS, 30)]
    return "\n".join(lines) + "\n"


def run(arguments, workdir):
    (workdir / "build.stage").write_text(stage_text())
    preferences = workdir / "build.conf"
    preferences.write_text(native_offline.preferences_text())
    command = [*RUNNER, str(arguments.native), "--game-dir", str(arguments.game_dir),
               "--skip-intro", "--mute", "--headless-check", "--seed", str(SEED),
               "--match-ticks", str(TICKS), "--stage", str(workdir / "build.stage"),
               "--preferences-file", str(preferences), "--trace-digest",
               str(workdir / "build.trace"), "--trace-units", str(workdir / "build.units")]
    result = subprocess.run(
        command, cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=900, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        fail(result, f"the run exited with {result.returncode}")
    return result, workdir / "build.units"


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise SystemExit(message)


def placed(stdout):
    """The slot of each staged kbot, by the group it was placed in."""
    slots, group = {}, None
    for line in stdout.splitlines():
        if match := GROUP_LINE.match(line):
            group = match.group(1)
        elif (match := UNIT_LINE.match(line)) and group:
            slots[group] = int(match.group(1))
    return slots


def last_kinds(units_file, slots):
    """Each unit's order kind at the last tick the trace reports it."""
    kinds = {}
    with open(units_file, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            match = TRACE.match(line)
            if match and int(match.group(2)) in slots:
                kinds[int(match.group(2))] = int(match.group(3))
    return kinds


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    # The run starts in the scratch folder.
    arguments.native = arguments.native.absolute()
    arguments.game_dir = arguments.game_dir.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        result, units = run(arguments, Path(scratch))
        slots = placed(result.stdout)
        wanted = {f"{role}{row}" for role in ("builder", "guard") for row in range(len(SPACINGS))}
        if set(slots) != wanted:
            fail(result, f"the stage placed kbots for {sorted(slots)}, not {sorted(wanted)}")
        kinds = last_kinds(units, set(slots.values()))
    for row in range(len(SPACINGS)):
        builder = kinds.get(slots[f"builder{row}"])
        if builder != STANDBY:
            fail(result, f"the builder of line {row} ends the run in order kind {builder}, not "
                         "idle" + (" (stuck building)" if builder == MOBILE_BUILD else ""))
        guard = kinds.get(slots[f"guard{row}"])
        if guard != GUARD:
            fail(result, f"the guard of line {row} ends the run in order kind {guard}, not "
                         "guarding its builder")
    print(f"build assist check: {len(SPACINGS)} guarded builders worked through their lines of "
          f"{TOWERS} radar towers")


if __name__ == "__main__":
    main()
