#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a group moved to one point keeps its shape, as the game's group order does.

A headless skirmish is staged with a block of nine fighters and a row of
eight infantry kbots of the local player, each group then moved to one
point as a selection of it is. Every unit's own destination follows from
where the group stood: its centre is the average of the units' whole map
pixels, truncated toward zero, and a unit within sqrt(3000 * N) map pixels
of it is sent to the point moved by its displacement from the centre,
while one further away is sent to the point itself.

As the run ends, the fighters, all within that reach of their block's
centre, must have landed each on its own destination, the block's shape
moved to the point, on nine sites no two of whose footprints share a cell.
The kbots of the row within reach of its centre must stand at their own
destinations, a kbot whose move has finished on its destination's cell,
and the two at the ends of the row, out of reach, must have gone to the
point.
"""
import argparse
import itertools
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

TICKS = 900
ORDER_TICK = 30
# Map pixels of the headless skirmish's map (Canal Crossing), open ground
# on the local player's side.
BLOCK = [(1400 + 48 * column, 2700 + 48 * row) for row in range(3) for column in range(3)]
BLOCK_POINT = (2200, 1300)
ROW = [(300 + 48 * index, 2400) for index in range(8)]
ROW_POINT = (700, 2000)
STAGE = "".join(
    ["# Nine fighters in a block and eight infantry kbots in a row, each moved\n",
     "# to one point as a group.\n", "group block\n"]
    + [f"place 0 ARMFIG {x} {z}\n" for x, z in BLOCK]
    + ["group row\n"] + [f"place 0 ARMPW {x} {z}\n" for x, z in ROW]
    + ["group\n", f"at {ORDER_TICK} move block {BLOCK_POINT[0]} {BLOCK_POINT[1]}\n",
       f"at {ORDER_TICK} move row {ROW_POINT[0]} {ROW_POINT[1]}\n"])
# Footprints, in 16-pixel cells, of ARMFIG and ARMPW.
FOOTPRINT = 2
# Unit.flags' occupancy: 1 on the ground, 2 in the air.
OCCUPANCY_MASK = 3
ON_GROUND = 1
# The order kind of a unit with no order left but standing by.
STANDBY_KINDS = (41, 64)
# How far a landed aircraft may stand from its own destination: its move
# snaps the point to the footprint grid, up to half a cell on each axis.
LANDED_REACH = 12
# How far a kbot may stand from its own destination: within a cell, or a
# cell short of it when the kbot ahead holds the cell.
ARRIVED_REACH = 16
# How near the point a kbot sent to the point itself must come.
POINT_REACH = 48
PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player 0 at (-?\d+),(-?\d+)$", re.M)
UNIT = re.compile(r"^tick=(\d+) slot=(\d+) type=\d+ owner=\d+ flags=0x([0-9a-f]+) "
                  r".* x=0x([0-9a-f]+) y=0x[0-9a-f]+ z=0x([0-9a-f]+) .* order_kind=(\d+) ", re.M)
MASK = 0xFFFFFFFF


def signed(value):
    value &= MASK
    return value - (1 << 32) if value >= 1 << 31 else value


def whole_pixels(fixed):
    """The signed high half of a 16.16 value."""
    return (((fixed & MASK) >> 16) ^ 0x8000) - 0x8000


def toward_zero(total, count):
    quotient = abs(total) // count
    return quotient if total >= 0 else -quotient


def destinations(positions, point):
    """Each unit's 16.16 destination and whether it keeps its place."""
    count = len(positions)
    centre_x = (toward_zero(sum(whole_pixels(x) for x, _ in positions), count) << 16) & MASK
    centre_z = (toward_zero(sum(whole_pixels(z) for _, z in positions), count) << 16) & MASK
    result = []
    for x, z in positions:
        dx, dz = signed(x - centre_x), signed(z - centre_z)
        reach = signed(((dz * dz) >> 32) + ((dx * dx) >> 32))
        if reach <= count * 3000:
            result.append((signed(x + point[0] - centre_x), signed(point[1] + z - centre_z), True))
        else:
            result.append((point[0], point[1], False))
    return result


def origin_cell(fixed):
    return ((fixed - FOOTPRINT * 0x80000 + 0x80000) & MASK) >> 20


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise SystemExit(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        (workdir / "groups.stage").write_text(STAGE)
        (workdir / "groups.conf").write_text("open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n")
        result = subprocess.run(
            [*RUNNER, str(arguments.native.absolute()), "--game-dir",
             str(arguments.game_dir.absolute()), "--skip-intro", "--mute", "--headless-check",
             "--preferences-file", str(workdir / "groups.conf"), "--match-ticks", str(TICKS),
             "--stage", str(workdir / "groups.stage"), "--trace-digest",
             str(workdir / "groups.trace"), "--trace-units", str(workdir / "groups.units")],
            cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
            timeout=600, check=False, capture_output=True, text=True, errors="replace")
        if result.returncode != 0:
            fail(result, f"the staged run exited with {result.returncode}")
        for line in (f"stage: move block: 9 units to {BLOCK_POINT[0]},{BLOCK_POINT[1]}",
                     f"stage: move row: 8 units to {ROW_POINT[0]},{ROW_POINT[1]}"):
            if line not in result.stdout:
                fail(result, f"the run did not report \"{line}\"")
        placed = [int(slot) for slot, _, _, _ in PLACED.findall(result.stdout)]
        if len(placed) != len(BLOCK) + len(ROW):
            fail(result, f"the stage placed {placed}")
        block, row = placed[:len(BLOCK)], placed[len(BLOCK):]
        units = {}
        for tick, slot, flags, x, z, kind in UNIT.findall((workdir / "groups.units").read_text()):
            if int(tick) in (ORDER_TICK, TICKS):
                units[(int(tick), int(slot))] = (int(flags, 16), int(x, 16), int(z, 16), int(kind))

    def start_and_end(slots):
        if any((tick, slot) not in units for slot in slots for tick in (ORDER_TICK, TICKS)):
            fail(result, "the trace lacks a unit of the groups")
        return ([units[(ORDER_TICK, slot)][1:3] for slot in slots],
                [units[(TICKS, slot)] for slot in slots])

    def apart(unit, destination):
        across = abs(signed(unit[1] - destination[0]))
        down = abs(signed(unit[2] - destination[1]))
        return max(across, down) / 65536

    starts, ends = start_and_end(block)
    wanted = destinations(starts, (BLOCK_POINT[0] << 16, BLOCK_POINT[1] << 16))
    if not all(keeps for _, _, keeps in wanted):
        fail(result, "a fighter of the block is out of the block's reach")
    for slot, end, destination in zip(block, ends, wanted):
        if end[0] & OCCUPANCY_MASK != ON_GROUND or apart(end, destination) > LANDED_REACH:
            fail(result, f"fighter {slot} did not land at its own destination "
                         f"{signed(destination[0]) / 65536},{signed(destination[1]) / 65536}: "
                         f"it stands at {signed(end[1]) / 65536},{signed(end[2]) / 65536}, "
                         f"occupancy {end[0] & OCCUPANCY_MASK}")
    for (a, first), (b, second) in itertools.combinations(zip(block, ends), 2):
        dx = abs(origin_cell(first[1]) - origin_cell(second[1]))
        dz = abs(origin_cell(first[2]) - origin_cell(second[2]))
        if dx < FOOTPRINT and dz < FOOTPRINT:
            fail(result, f"fighters {a} and {b} landed on footprints that share a cell")

    starts, ends = start_and_end(row)
    wanted = destinations(starts, (ROW_POINT[0] << 16, ROW_POINT[1] << 16))
    if [keeps for _, _, keeps in wanted] != [False] + [True] * 6 + [False]:
        fail(result, f"the row's reach is not its six middle kbots: {wanted}")
    for slot, end, destination in zip(row, ends, wanted):
        if destination[2]:
            if apart(end, destination) > ARRIVED_REACH:
                fail(result, f"kbot {slot} stands {apart(end, destination)} pixels from its own "
                             "destination")
            if end[3] in STANDBY_KINDS and (origin_cell(end[1]), origin_cell(end[2])) != (
                    origin_cell(destination[0]), origin_cell(destination[1])):
                fail(result, f"kbot {slot} finished its move off its destination's cell")
        elif apart(end, (ROW_POINT[0] << 16, ROW_POINT[1] << 16)) > POINT_REACH:
            fail(result, f"kbot {slot}, out of the row's reach, did not go to the point")
    print("group order check: nine fighters landed on their own sites in the block's shape, "
          "the row's six middle kbots reached their own points and its two ends went to the point")


if __name__ == "__main__":
    main()
