#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a unit created by its name on the chat line stands where 3.1c puts it.

A headless skirmish on Coast To Coast (its sea at 85 over a bed at 0
between the two coasts) places patrol boats on the deep water and a kbot on
a hill of the south-west coast, then points at each and enters a unit's
name: a shipyard, a floating torpedo launcher and a floating metal maker
on the water, a patrol boat, and a solar collector on the hill.

A building must stand as a builder would put it there: on its footprint's
grid, within a footprint of the pointer, at the height its yard takes. The
floating buildings' yards have no level cell, so each floats at the sea
level less its waterline, a shipyard 22 below the surface. The solar
collector's yard is level throughout, so it stands at the lowest corner
under its footprint, read from the map. The patrol boat floats at sea
level, as the placed one does. Each keeps its height to the end of the run.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

MAP = "Coast To Coast"
MAP_ARCHIVE = "totala2.hpi"
TICKS = 40
FIRST_TICK = 10
# Each name is entered two ticks after the one before, so the tick a unit
# first appears in the trace says which name made it.
TICK_STEP = 2
# The markers (type, map pixel x and z, off the build grid) and the name
# entered while the pointer is on each.
SPAWNS = [("CORPT", 1003, 453, "CORSY"), ("CORPT", 1301, 451, "CORTL"),
          ("CORPT", 1597, 455, "CORFMKR"), ("CORPT", 1899, 449, "CORPT"),
          ("CORAK", 350, 1620, "CORSOLAR")]
# Footprint and waterline of each building, from its unit file. The floating
# ones' yards hold only 'w' and 'C' cells, which are not level; the solar
# collector's only 'o' cells, which are.
FLOATING = {"CORSY": (8, 22), "CORTL": (3, 10), "CORFMKR": (3, 22)}
LEVEL = {"CORSOLAR": 5}

PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player 0 at (-?\d+),(-?\d+)$", re.M)
FIELD = re.compile(r"(\w+)=(\S+)")


def pixels(text):
    """A traced 16.16 hex value as map pixels."""
    value = int(text, 16) & 0xFFFFFFFF
    return (value - (1 << 32) if value >= 1 << 31 else value) / 65536


def read_map(oa_tool, game_dir, workdir):
    """Returns the map's sea level and its attribute heights, row by row."""
    tnt = workdir / "map.tnt"
    result = subprocess.run(
        [*RUNNER, str(oa_tool), "asset-extract", str(game_dir), f"maps/{MAP}.tnt", str(tnt),
         str(game_dir / MAP_ARCHIVE)],
        cwd=workdir, timeout=120, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"{MAP}'s map could not be read")
    data = tnt.read_bytes()
    # The TNT header: width and height in cells, the attribute table's
    # offset, and the sea level; each attribute starts with its height.
    _, width, height, _, attributes, _, _, _, _, sea_level = struct.unpack_from("<10i", data)
    rows = [[data[attributes + 4 * (z * width + x)] for x in range(width)] for z in range(height)]
    return sea_level, rows


def grid_cell(centre, footprint):
    """The footprint's first cell when its centre is on the build grid, else None."""
    steps = centre / 8 - footprint
    if steps != int(steps) or int(steps) % 2 != 0:
        return None
    return int(steps) // 2


def lowest_corner(rows, cell_x, cell_z, footprint):
    """The lowest plot under a level footprint; each plot spans its cell's corners."""
    return min(rows[z][x] for z in range(cell_z, cell_z + footprint + 1)
               for x in range(cell_x, cell_x + footprint + 1))


def run(native, game_dir, workdir):
    """Plays the stage; returns its output, the markers' slots and each unit's first and last places."""
    lines = [f"# Markers on {MAP}'s water and its south-western hill."]
    for index, (marker, x, z, _) in enumerate(SPAWNS):
        lines += [f"group m{index}", f"place 0 {marker} {x} {z}"]
    lines += ["group"]
    for index, (_, _, _, name) in enumerate(SPAWNS):
        tick = FIRST_TICK + TICK_STEP * index
        lines += [f"at {tick} hover m{index}", f"at {tick} console +{name}"]
    (workdir / "spawn.stage").write_text("\n".join(lines) + "\n")
    (workdir / "spawn.conf").write_text(
        "open-annihilation-preferences 1\n"
        f"\"Total Annihilation|SkirmishMap\" \"{MAP}\"\n")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(workdir / "spawn.conf"),
         "--match-ticks", str(TICKS), "--seed", "1", "--stage", str(workdir / "spawn.stage"),
         "--trace-digest", str(workdir / "spawn.trace"),
         "--trace-units", str(workdir / "spawn.units")],
        cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=600, check=False, capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(f"the staged run exited with {result.returncode}")
    placed = PLACED.findall(result.stdout)
    if [name for _, name, _, _ in placed] != [marker for marker, *_ in SPAWNS]:
        print(output, end="")
        raise SystemExit(f"the stage placed {placed}")
    first, last = {}, {}
    for line in (workdir / "spawn.units").read_text().splitlines():
        fields = dict(FIELD.findall(line))
        if fields.get("owner") != "0" or fields.get("type") in (None, "0"):
            continue
        place = (int(fields["tick"]), pixels(fields["x"]), pixels(fields["y"]),
                 pixels(fields["z"]))
        first.setdefault(int(fields["slot"]), place)
        last[int(fields["slot"])] = place
    return output, [int(slot) for slot, *_ in placed], first, last


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--oa-tool", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    # The runs start in the scratch folder.
    native = arguments.native.absolute()
    game_dir = arguments.game_dir.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        sea_level, rows = read_map(arguments.oa_tool.absolute(), game_dir, workdir)
        output, markers, first, last = run(native, game_dir, workdir)
        problems = []
        for index, (marker, x, z, name) in enumerate(SPAWNS):
            tick = FIRST_TICK + TICK_STEP * index
            made = [slot for slot, place in first.items() if place[0] == tick]
            if len(made) != 1:
                problems.append(f"+{name} at tick {tick} made the units {made}, not one")
                continue
            _, unit_x, unit_y, unit_z = first[made[0]]
            where = f"+{name} pointed at {x},{z} stands at {unit_x:g},{unit_y:g},{unit_z:g}"
            if last[made[0]][2] != unit_y:
                problems.append(where + f", and at tick {last[made[0]][0]} at height "
                                f"{last[made[0]][2]:g}")
            if name == marker:
                # A mobile unit settles as a placed one does.
                marked = first[markers[index]][2]
                if unit_y != marked:
                    problems.append(where + f", not at the placed {marker}'s height {marked:g}")
                continue
            footprint = FLOATING[name][0] if name in FLOATING else LEVEL[name]
            cell_x, cell_z = grid_cell(unit_x, footprint), grid_cell(unit_z, footprint)
            if cell_x is None or cell_z is None or abs(unit_x - x) > footprint * 16 or \
                    abs(unit_z - z) > footprint * 16:
                problems.append(where + f": not on its {footprint}-cell grid beside the pointer")
                continue
            if name in FLOATING:
                expected = sea_level - FLOATING[name][1]
            else:
                expected = lowest_corner(rows, cell_x, cell_z, footprint)
            if unit_y != expected:
                problems.append(where + f", not at its yard's height {expected}")
        if problems:
            print(output, end="")
            raise SystemExit("\n".join(problems))
    print(f"spawn height check: on {MAP}, a shipyard, a torpedo launcher and a metal maker "
          f"made by name float at sea level {sea_level} less their waterlines, a solar "
          "collector stands on its hill's lowest corner and a patrol boat floats as a placed "
          "one does")


if __name__ == "__main__":
    main()
