#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that damaged aircraft waiting for an occupied repair pad go to free ones.

A headless skirmish is staged with ten of the local player's stealth
fighters (CORVAMP), left with 30% of their health and sent on patrol beside
one Core air repair pad (CORASP), which seats one aircraft at a time. Each
breaks off for the only pad its owner's pad list holds. Two more pads are
placed beside it while they wait. An aircraft that finds its pad occupied as
it circles it, or is turned away from it, makes for the nearest free pad in
reach instead; 3.1c keeps the pad it drew and waits for it.

Every fighter must make for a pad, each of the two later pads must hold
fighters and repair them, and by the end of the run every fighter must have
been set down on a pad and repaired.
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
PAD = "CORASP"
FIGHTER = "CORVAMP"
FIGHTERS = 10
# The match tick the two later pads are placed on, after every fighter has
# made for the first.
LATER_PADS = 400
# Unit order kinds the trace reports (Unit order table indices).
VTOL_LANDING = 53
UNIT_LINE = re.compile(r"^stage: unit (\d+) (\S+) of player \d+ at (-?\d+),(-?\d+)$", re.M)
TRACE = re.compile(r"^tick=(\d+) slot=(\d+) .*? health=(-?\d+) .*? parent=(\d+) .*? "
                   r"order_kind=(\d+) ", re.M)


def run(arguments, workdir, name, stage, ticks):
    (workdir / f"{name}.stage").write_text(stage)
    preferences = workdir / f"{name}.conf"
    preferences.write_text(native_offline.preferences_text())
    command = [*RUNNER, str(arguments.native), "--game-dir", str(arguments.game_dir),
               "--skip-intro", "--mute", "--headless-check", "--seed", str(SEED),
               "--match-ticks", str(ticks), "--stage", str(workdir / f"{name}.stage"),
               "--preferences-file", str(preferences), "--trace-digest",
               str(workdir / f"{name}.trace"), "--trace-units", str(workdir / f"{name}.units")]
    result = subprocess.run(
        command, cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=900, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        fail(result, f"the {name} run exited with {result.returncode}")
    return result, workdir / f"{name}.units"


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise SystemExit(message)


def origin(arguments, workdir):
    """The local commander's map pixel, from where the stage's first unit stands."""
    result, _ = run(arguments, workdir, "origin", f"unit 0 {PAD} 0 0\n", 1)
    units = UNIT_LINE.findall(result.stdout)
    if not units:
        fail(result, "the origin run placed nothing")
    return int(units[0][2]), int(units[0][3])


def history(units_file, slots):
    """Each watched slot's (tick, health, parent, order kind), tick by tick."""
    watched = {slot: [] for slot in slots}
    with open(units_file, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            match = TRACE.match(line)
            if match and int(match.group(2)) in watched:
                tick, slot, health, parent, kind = (int(value) for value in match.groups())
                watched[slot].append((tick, health, parent, kind))
    return watched


def repaired_on(rows):
    """The pads a fighter was set down on and repaired by, in the order it used them."""
    pads = []
    for (_, health, parent, _), (_, later, still, _) in zip(rows, rows[1:]):
        if parent and still == parent and later > health and (not pads or pads[-1] != parent):
            pads.append(parent)
    return pads


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
        x, z = origin(arguments, workdir)
        fighters = [f"place 0 {FIGHTER} {x + 100 + 40 * (i % 5)} {z - 150 + 40 * (i // 5)}"
                    for i in range(FIGHTERS)]
        stage = "\n".join([
            "# Damaged fighters on patrol beside one pad; two more pads come later.",
            "group pad", f"place 0 {PAD} {x + 300} {z}",
            "group fighters", *fighters,
            "group", "health fighters 30", f"patrol fighters {x + 400} {z - 250}",
            f"at {LATER_PADS} group later",
            f"at {LATER_PADS} place 0 {PAD} {x + 420} {z}",
            f"at {LATER_PADS} place 0 {PAD} {x + 540} {z}",
            f"at {LATER_PADS} group", ""])
        result, units = run(arguments, workdir, "spread", stage, TICKS)
        placed = [(int(slot), name) for slot, name, _, _ in UNIT_LINE.findall(result.stdout)]
        pads = [slot for slot, name in placed if name == PAD]
        planes = [slot for slot, name in placed if name == FIGHTER]
        if len(pads) != 3 or len(planes) != FIGHTERS:
            fail(result, f"the stage placed {placed}, not 3 pads and {FIGHTERS} fighters")
        rows = history(units, planes)
    used = {pad: 0 for pad in pads}
    for plane in planes:
        if not any(kind == VTOL_LANDING for _, _, _, kind in rows[plane]):
            fail(result, f"fighter {plane} never made for a pad")
        landed = repaired_on(rows[plane])
        if not landed:
            fail(result, f"fighter {plane} was never set down on a pad and repaired")
        for pad in landed:
            used[pad] += 1
    for pad in pads[1:]:
        if not used[pad]:
            fail(result, f"no fighter was repaired on the later pad {pad}; the pads repaired "
                         f"{used}")
    print(f"air pad choice check: {FIGHTERS} damaged fighters waiting for one pad spread to "
          f"the two placed later; the pads repaired {', '.join(map(str, used.values()))}")


if __name__ == "__main__":
    main()
