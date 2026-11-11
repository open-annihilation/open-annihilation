#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that damaged aircraft use repair pads as the game's orders and missions do.

Headless skirmishes are staged with the local player's air repair pads
(ARMASP) and gunships (ARMBRAWL) left with 40% of their health, beside the
local commander:

- With a gunship selected, the pointer over a pad shows the select cursor
  under the default order of the left-click interface, and the pad cursor
  (cursorunload, or cursorload with --pad-cursor load) once Move is armed;
  a left click then orders the landing. A second gunship, selected in the
  right-click interface, shows the select cursor over another pad and a
  right press orders the landing there. Each gunship lands on its pad and
  the pad repairs it.
- A gunship on patrol near a pad breaks off to land on it, below three
  quarters of its health, once its owner's pad list (rebuilt every 30
  ticks) holds the pad, and is repaired there; an idle gunship beside it
  never makes for the pad.
- With --kept-off TYPE,... (the aircraft a mod's air.no-repair-retreat-flag
  keeps off pads: every flying type of the mod with CantBeTransported=1),
  those aircraft, damaged and selected together, show no pad cursor and
  take no landing from Move or a right press, and on patrol never break off
  for the pad.

--mod-dir plays a mod's folder; the stage names only types every side of
Total Annihilation has.
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
TICKS = 1200
# The pad and the aircraft that use it.
PAD = "ARMASP"
GUNSHIP = "ARMBRAWL"
# Unit order kinds the trace reports (Unit order table indices).
VTOL_LANDING = 53
UNIT_LINE = re.compile(r"^stage: unit (\d+) (\S+) of player \d+ at (-?\d+),(-?\d+)$", re.M)
HOVER = re.compile(r"^stage: hover (\S+): unit (\d+) under the pointer, cursor (\S+)$", re.M)
PRESS = re.compile(r"^stage: press (left|right): (.*)$", re.M)
TRACE = re.compile(r"^tick=(\d+) slot=(\d+) .*? health=(-?\d+) .*? parent=(\d+) .*? "
                   r"order_kind=(\d+) ", re.M)


def run(arguments, workdir, name, stage, ticks):
    (workdir / f"{name}.stage").write_text(stage)
    preferences = workdir / f"{name}.conf"
    preferences.write_text("open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n")
    command = [*RUNNER, str(arguments.native), "--game-dir", str(arguments.game_dir),
               "--skip-intro", "--mute", "--headless-check", "--seed", str(SEED),
               "--match-ticks", str(ticks), "--stage", str(workdir / f"{name}.stage"),
               "--preferences-file", str(preferences), "--trace-digest",
               str(workdir / f"{name}.trace"), "--trace-units", str(workdir / f"{name}.units")]
    if arguments.mod_dir:
        command[3:3] = ["--mod-dir", str(arguments.mod_dir)]
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


def placed(result):
    """The unit slot of each placement, by the order the stage made them."""
    return [int(slot) for slot, _, _, _ in UNIT_LINE.findall(result.stdout)]


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


def lands_and_is_repaired(result, rows, pad, what):
    """The aircraft makes for the pad, comes to stand on it and is repaired there."""
    if not any(kind == VTOL_LANDING for _, _, _, kind in rows):
        fail(result, f"{what} never took VTOL_Landing")
    on_pad = [(tick, health) for tick, health, parent, _ in rows if parent == pad]
    if not on_pad:
        fail(result, f"{what} never came to stand on the pad (unit {pad})")
    best = max(health for _, health in on_pad)
    if best <= on_pad[0][1]:
        fail(result, f"{what} landed with {on_pad[0][1]} health and was not repaired")
    return on_pad[0][0]


def stays_off_pads(result, rows, what):
    if any(kind == VTOL_LANDING for _, _, _, kind in rows) or any(row[2] for row in rows):
        fail(result, f"{what} made for a pad")


def origin(arguments, workdir):
    """The local commander's map pixel, from where the stage's first unit stands."""
    result, _ = run(arguments, workdir, "origin", f"unit 0 {PAD} 0 0\n", 1)
    units = UNIT_LINE.findall(result.stdout)
    if not units:
        fail(result, "the origin run placed nothing")
    return int(units[0][2]), int(units[0][3])


def check_orders(arguments, workdir, x, z):
    stage = "\n".join([
        "# Two damaged gunships, each sent onto its own pad: one by Move in the",
        "# left-click interface, one by a right press in the right-click one.",
        "group pad1", f"place 0 {PAD} {x + 200} {z}",
        "group pad2", f"place 0 {PAD} {x + 200} {z + 200}",
        "group movers", f"place 0 {GUNSHIP} {x + 60} {z + 120}",
        "group pressers", f"place 0 {GUNSHIP} {x + 60} {z + 320}",
        "group", "health movers 40", "health pressers 40",
        "at 40 select movers", "at 40 interface left", "at 40 arm none", "at 40 hover pad1",
        "at 40 arm move", "at 40 hover pad1", "at 40 press left",
        "at 41 select pressers", "at 41 interface right", "at 41 hover pad2",
        "at 41 press right", "at 41 interface left", ""])
    result, units = run(arguments, workdir, "orders", stage, TICKS)
    pad1, pad2, mover, presser = placed(result)
    hovers = HOVER.findall(result.stdout)
    pad_cursor = "cursorload" if arguments.pad_cursor == "load" else "cursorunload"
    expected = [("pad1", str(pad1), "cursorselect"), ("pad1", str(pad1), pad_cursor),
                ("pad2", str(pad2), "cursorselect")]
    if hovers != expected:
        fail(result, f"the pointer over the pads showed {hovers}, not {expected}")
    if PRESS.findall(result.stdout) != [("left", "Land"), ("right", "Land")]:
        fail(result, f"the presses gave {PRESS.findall(result.stdout)}, not two landings")
    rows = history(units, [mover, presser])
    lands_and_is_repaired(result, rows[mover], pad1, "the gunship sent by Move")
    lands_and_is_repaired(result, rows[presser], pad2, "the gunship sent by a right press")


def check_break_off(arguments, workdir, x, z):
    stage = "\n".join([
        "# A damaged gunship patrolling beside a pad, and an idle one.",
        "group pad", f"place 0 {PAD} {x + 200} {z}",
        "group patrollers", f"place 0 {GUNSHIP} {x + 60} {z + 200}",
        "group idlers", f"place 0 {GUNSHIP} {x - 100} {z + 200}",
        "group", "health patrollers 40", "health idlers 40",
        f"patrol patrollers {x + 460} {z + 200}", ""])
    result, units = run(arguments, workdir, "break-off", stage, TICKS)
    pad, patroller, idler = placed(result)
    rows = history(units, [patroller, idler])
    landed = lands_and_is_repaired(result, rows[patroller], pad, "the patrolling gunship")
    # The pad joins its owner's pad list at the first rebuild, on tick 30.
    first = next(tick for tick, _, _, kind in rows[patroller] if kind == VTOL_LANDING)
    if first < 30:
        fail(result, f"the patrolling gunship made for the pad on tick {first}, before its "
                     "owner's pad list held it")
    stays_off_pads(result, rows[idler], "the idle gunship")
    if len({health for _, health, _, _ in rows[idler]}) != 1:
        fail(result, "the idle gunship's health changed")
    return landed


def check_kept_off(arguments, workdir, x, z):
    kept = arguments.kept_off.split(",")
    places = []
    for index, name in enumerate(kept):
        column, row = index % 6, index // 6
        places.append(f"place 0 {name} {x + 60 + 160 * column} {z + 260 + 160 * row}")
    stage = "\n".join([
        "# Damaged aircraft of the types the mod keeps off repair pads.",
        "group pad", f"place 0 {PAD} {x + 200} {z}",
        "group kept", *places,
        "group", "health kept 40",
        "at 40 select kept", "at 40 interface left", "at 40 arm move", "at 40 hover pad",
        "at 40 press left", "at 41 interface right", "at 41 arm none", "at 41 hover pad",
        "at 41 press right", "at 41 interface left",
        f"at 42 patrol kept {x + 260} {z + 100}", ""])
    result, units = run(arguments, workdir, "kept-off", stage, TICKS)
    slots = placed(result)
    if len(slots) != len(kept) + 1:
        fail(result, f"the stage placed {len(slots)} units, not {len(kept) + 1}")
    hovers = HOVER.findall(result.stdout)
    if len(hovers) != 2 or any(cursor in ("cursorload", "cursorunload") for *_, cursor in hovers):
        fail(result, f"the pointer over the pad showed {hovers} for {', '.join(kept)}")
    if any(issued == "Land" for _, issued in PRESS.findall(result.stdout)):
        fail(result, "a press landed an aircraft the mod keeps off pads")
    rows = history(units, slots[1:])
    for name, slot in zip(kept, slots[1:]):
        stays_off_pads(result, rows[slot], name)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--mod-dir", type=Path)
    parser.add_argument("--pad-cursor", choices=("load", "unload"), default="unload",
                        help="the cursor Move shows over a pad: unload in 3.1c, load under "
                             "ui.interface-fixes pad-cursor")
    parser.add_argument("--kept-off", metavar="TYPE[,TYPE...]",
                        help="aircraft types the mod keeps off repair pads")
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    # The runs start in the scratch folder.
    arguments.native = arguments.native.absolute()
    arguments.game_dir = arguments.game_dir.absolute()
    if arguments.mod_dir:
        arguments.mod_dir = arguments.mod_dir.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        x, z = origin(arguments, workdir)
        check_orders(arguments, workdir, x, z)
        landed = check_break_off(arguments, workdir, x, z)
        if arguments.kept_off:
            check_kept_off(arguments, workdir, x, z)
    print(f"air pads check: the pad cursor, Move and a right press landed damaged gunships, "
          f"which were repaired, and a patrolling one broke off to land on tick {landed}"
          + (f"; {arguments.kept_off} kept off the pad" if arguments.kept_off else ""))


if __name__ == "__main__":
    main()
