#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a builder guarding a factory helps build each of its products.

A headless skirmish is staged with the local player's advanced kbot lab
(CORALAB) building three Mortys (CORMORT) and an advanced construction kbot
(CORACK) guarding it, with +atm entered every two seconds so that no build
waits on metal or energy. The kbot helps the lab with each Morty in turn.
When one Morty is finished it ends its help and closes its nanolathe, and the
lab starts the next a second later, while that closing still runs; with
the kbot's walk stopping on the same tick, its StartBuilding finds all
eight script threads busy and drops the start-script that raises its build
stance. 3.1c leaves the kbot waiting for that stance for ever; the engine
runs StartBuilding again after ten seconds without it.

Each Morty must be finished, building faster over its last second than over
the ticks just after the lab started it, before the kbot can reach it: the
kbot's own work rate is four fifths of the lab's. As the run ends the kbot
must be guarding the lab again.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

SEED = 4242
TICKS = 3000
MORTYS = 3
# Unit order kinds the trace reports (Unit order table indices).
GUARD = 18
HELP_BUILD = 23
# Ticks a Morty's rate is measured over: just after the lab starts it, and
# just before it is finished.
EARLY = range(2, 12)
LATE = 30
# How much faster the last second must be with the kbot's help: the lab's
# and the kbot's work rates together are 1.8 times the lab's alone.
HELPED = 1.5
UNIT_LINE = re.compile(r"^stage: unit (\d+) (\S+) of player \d+ at (-?\d+),(-?\d+)$", re.M)
TRACE = re.compile(r"^tick=(\d+) slot=(\d+) .*? build=0x([0-9a-f]+) .*? parent=(\d+) .*? "
                   r"order_kind=(\d+) ", re.M)


def run(arguments, workdir, stage):
    (workdir / "assist.stage").write_text(stage)
    preferences = workdir / "assist.conf"
    preferences.write_text(native_offline.preferences_text())
    command = [*RUNNER, str(arguments.native), "--game-dir", str(arguments.game_dir),
               "--skip-intro", "--mute", "--headless-check", "--seed", str(SEED),
               "--match-ticks", str(TICKS), "--stage", str(workdir / "assist.stage"),
               "--preferences-file", str(preferences), "--trace-digest",
               str(workdir / "assist.trace"), "--trace-units", str(workdir / "assist.units")]
    result = subprocess.run(
        command, cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=900, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        fail(result, f"the run exited with {result.returncode}")
    return result, workdir / "assist.units"


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise SystemExit(message)


def remaining(bits):
    """A unit's share of its build still to do, from the trace's float bits."""
    return struct.unpack("<f", struct.pack("<I", int(bits, 16)))[0]


def history(units_file, lab, kbot):
    """Each product of the lab's build left by tick, and the kbot's order kind by tick."""
    builds, kinds = {}, {}
    with open(units_file, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            match = TRACE.match(line)
            if not match:
                continue
            tick, slot, bits, parent, kind = match.groups()
            tick, slot = int(tick), int(slot)
            if slot == kbot:
                kinds[tick] = int(kind)
            elif int(parent) == lab or slot in builds:
                builds.setdefault(slot, {})[tick] = remaining(bits)
    return builds, kinds


def rate(left, ticks):
    """The share of a build done each tick, on average over the given ticks."""
    pairs = [(left[tick - 1], left[tick]) for tick in ticks if tick - 1 in left and tick in left]
    return sum(before - after for before, after in pairs) / len(pairs) if pairs else 0.0


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
    stage = [
        "# An advanced kbot lab building Mortys, guarded by an advanced construction kbot.",
        "group lab", "unit 0 CORALAB 250 0", f"queue CORMORT {MORTYS}",
        "group helper", "unit 0 CORACK 150 150",
        "group", "guard helper lab", "console +atm",
        *(f"at {tick} console +atm" for tick in range(60, TICKS, 60)), ""]
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        result, units = run(arguments, Path(scratch), "\n".join(stage))
        slots = {name: int(slot) for slot, name, _, _ in UNIT_LINE.findall(result.stdout)}
        if set(slots) != {"CORALAB", "CORACK"}:
            fail(result, f"the stage placed {sorted(slots)}, not the lab and the kbot")
        builds, kinds = history(units, slots["CORALAB"], slots["CORACK"])
    finished = []
    for slot, left in sorted(builds.items()):
        done = min((tick for tick, share in left.items() if share == 0.0), default=None)
        if done is None:
            fail(result, f"the Morty in slot {slot} was not finished")
        start = min(left)
        early = rate(left, [start + tick for tick in EARLY])
        late = rate(left, range(done - LATE + 1, done))
        if early <= 0.0 or late < early * HELPED:
            fail(result, f"the Morty in slot {slot} (ticks {start} to {done}) was built at "
                         f"{late:.6f} a tick at its end and {early:.6f} at its start: the kbot "
                         "did not help")
        finished.append(f"{start}-{done}")
    if len(finished) != MORTYS:
        fail(result, f"the lab built {len(finished)} Mortys, not {MORTYS}")
    last = kinds.get(max(kinds), 0) if kinds else 0
    if last != GUARD:
        fail(result, f"the kbot ends the run in order kind {last}, not guarding the lab"
                     + (" (stuck helping)" if last == HELP_BUILD else ""))
    print(f"guard assist check: the kbot helped the lab with {MORTYS} Mortys (ticks "
          f"{', '.join(finished)}) and went back to guarding it")


if __name__ == "__main__":
    main()
