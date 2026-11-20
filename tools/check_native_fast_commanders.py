#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a commander far faster than any stock unit arrives where it is sent.

ridge-racing-commander gives ARMCOM the movement seven stacked commander
mutators leave it (Krogoth Commander, Hypermobile Commanders 3x, Super
Commander and UberComm among them): 42 pixels a tick, a turn of 28800 a tick,
and acceleration and braking that change its speed by all of that in one
tick, on a 3x3 footprint. Such a commander used to step over the one cell a
move must end in, brake and turn, and step back over it, for ever; a build
queued behind the move never started.

Three headless skirmishes on the flat oil platform of Over Crude Water
(height 139 throughout, far from both start positions) place commanders of
the local player and move them at tick 30: fourteen on rows clear of the
platform's features, each sent east a different distance, so each reaches
the goal at another phase of its step; fourteen on a grid, each sent its
own way (east, south-east, south, south-west, north-west or north-east);
one whose way is shut by a wall of dragon's teeth, so its route turns round
the wall's end; and one with a solar collector queued behind its move.

Each commander must reach full speed, end its move standing within a cell of
the point, still, without reversing its direction more than three times on
the way (an oscillating one reverses every other tick), and the queued solar
collector must be finished.

Only a match that applied mutators holds a fast unit so; any other steers it
as 3.1c does, so that a mod's fast units move as 3.1c moves them. A fourth
skirmish plays the rows with no mutator, over a scratch mod folder whose
ARMCOM unit file, taken from the installation, carries the same values: there
a commander must step past its point by more than a cell, as 3.1c's steering
makes it.
"""
import argparse
import math
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

PACKAGE = "ridge-racing-commander"
MAP = "Over Crude Water"
TICKS = 600
ORDER_TICK = 30
# The commander's top speed, in map pixels a tick, and the least speed a run
# must see to show the package's values are in play.
TOP_SPEED = 42
FAST_ENOUGH = 40
# A move ends with the commander's footprint on the point's cell: within a
# cell of the point.
ARRIVED_REACH = 16
# Reversals of direction a commander may make on its way (a turn round a
# wall's end may take one); an oscillating one makes one every other tick.
MOST_REVERSALS = 3
# The order kind of a unit moving.
MOVE_GROUND = 26
# Rows of the platform clear of its features, each sent east 150 + 31 * row.
ROWS = [(5520, 5480), (5660, 5580), (5520, 5680), (5920, 5780), (5520, 5880), (6120, 5980),
        (6120, 6080), (6040, 6180), (5520, 6280), (5520, 6380), (5520, 6480), (6260, 6580),
        (6260, 6680), (5600, 6860)]
# A grid over the platform, each commander sent its own way, every straight
# path clear of the features and of the other paths.
WAYS = [((6100, 5600), (6215, 5715)), ((6550, 5600), (6550, 5776)),
        ((7000, 5600), (6866, 5734)), ((5650, 6050), (5507, 5907)),
        ((6550, 6050), (6711, 5889)), ((7000, 6050), (7170, 5880)),
        ((5650, 6500), (5904, 6500)), ((6100, 6500), (6100, 6767)),
        ((6550, 6500), (6550, 6780)), ((7000, 6500), (6793, 6707)),
        ((5650, 6950), (5956, 6950)), ((6100, 6950), (6326, 6724)),
        ((6550, 6950), (6882, 6950)), ((7000, 6950), (7244, 6706))]
# Dragon's teeth from (5900, 6100) down to (5900, 6420), across the way of a
# commander sent from (5700, 6250) to (6100, 6250).
WALL = [(5900, z) for z in range(6100, 6421, 32)]
DETOUR = ((5700, 6250), (6100, 6250))
# A commander sent from (5900, 5700) to (6300, 5700), with a solar collector
# queued 400 east and 150 south of where it stood when ordered.
QUEUED = ((5900, 5700), (6300, 5700))
SOLAR_SITE = (6300, 5850)
# A building's site snaps to its footprint, a few pixels from the point.
SITE_REACH = 16

# The movement the package gives ARMCOM, as unit-file keys, for the mod
# folder's copy of ARMCOM's unit file.
RACING_FIELDS = {"MaxVelocity": "42", "TurnRate": "28800", "Acceleration": "158.76",
                 "BrakeRate": "335.16", "FootprintX": "3", "FootprintZ": "3",
                 "MovementClass": "TANKDH3", "Builddistance": "720", "WorkerTime": "10800"}
# The archives ARMCOM's unit file is read from, the 3.1c patch's first.
UNIT_FILE_ARCHIVES = ("rev31.gp3", "totala1.hpi")

PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player 0 at (-?\d+),(-?\d+)$", re.M)
FIELD = re.compile(r"(\w+)=(\S+)")


def signed(value):
    value &= 0xFFFFFFFF
    return value - (1 << 32) if value >= 1 << 31 else value


def pixels(text):
    """A traced 16.16 hex value as map pixels."""
    return signed(int(text, 16)) / 65536


def stage_text(moves, first_lines=()):
    """Places a commander for each move, after first_lines, and moves them."""
    lines = [f"# Racing commanders on {MAP}'s platform, moved at tick {ORDER_TICK}."]
    lines += list(first_lines)
    for index, (start, _) in enumerate(moves):
        lines += [f"group c{index}", f"place 0 ARMCOM {start[0]} {start[1]}"]
    lines += ["group"]
    lines += [f"at {ORDER_TICK} move c{index} {end[0]} {end[1]}"
              for index, (_, end) in enumerate(moves)]
    return "\n".join(lines) + "\n"


def make_racing_mod(oa_tool, game_dir, workdir):
    """Writes a mod folder holding ARMCOM's unit file with RACING_FIELDS."""
    mod = workdir / "racing-mod"
    (mod / "units").mkdir(parents=True)
    archives = [str(game_dir / name) for name in UNIT_FILE_ARCHIVES if (game_dir / name).is_file()]
    stock = workdir / "armcom-stock.fbi"
    result = subprocess.run(
        [*RUNNER, str(oa_tool), "asset-extract", str(game_dir), "units/ARMCOM.FBI", str(stock),
         *archives],
        cwd=workdir, timeout=120, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit("the installation's ARMCOM unit file could not be read")
    text = stock.read_bytes().decode("latin-1")
    for key, value in RACING_FIELDS.items():
        text, count = re.subn(rf"(?im)^(\s*{key}\s*=)[^;]*;", rf"\g<1>{value};", text)
        if count != 1:
            raise SystemExit(f"ARMCOM's unit file has {count} {key} lines, not one")
    (mod / "units" / "ARMCOM.FBI").write_bytes(text.encode("latin-1"))
    return mod


def run(native, game_dir, package, workdir, name, stage, mod=None):
    (workdir / f"{name}.stage").write_text(stage)
    (workdir / f"{name}.conf").write_text(
        "open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n"
        f"\"Total Annihilation|SkirmishMap\" \"{MAP}\"\n")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(workdir / f"{name}.conf"),
         "--match-ticks", str(TICKS), "--seed", "1",
         *(["--mutator", str(package)] if mod is None else ["--mod-dir", str(mod)]),
         "--stage", str(workdir / f"{name}.stage"),
         "--trace-digest", str(workdir / f"{name}.trace"),
         "--trace-units", str(workdir / f"{name}.units")],
        cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=600, check=False, capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(f"{name}: the staged run exited with {result.returncode}")
    if (mod is None) != (f"mutators in play: {PACKAGE} 1.0" in output) or (
            mod is not None and "mutators in play" in output):
        print(output, end="")
        raise SystemExit(f"{name}: the mutators in play are not "
                         + (PACKAGE if mod is None else "none"))
    placed = [(int(slot), kind) for slot, kind, _, _ in PLACED.findall(result.stdout)]
    tracks = {}
    for line in (workdir / f"{name}.units").read_text().splitlines():
        fields = dict(FIELD.findall(line))
        if fields.get("owner") != "0":
            continue
        tracks.setdefault(int(fields["slot"]), []).append(
            (int(fields["tick"]), pixels(fields["x"]), pixels(fields["z"]),
             pixels(fields["speed"]), int(fields["order_kind"]), int(fields["state"], 16)))
    return output, placed, tracks


def reversals(track):
    count = 0
    previous = None
    for before, after in zip(track, track[1:]):
        step = (after[1] - before[1], after[2] - before[2])
        if math.hypot(*step) <= 0.01:
            continue
        if previous is not None and previous[0] * step[0] + previous[1] * step[1] < 0:
            count += 1
        previous = step
    return count


def check_moves(name, commanders, moves, tracks, queued=()):
    """Lists how the commanders failed to reach full speed and end their moves on the points.

    A commander with nothing queued must then rest there; for one in
    `queued`, which goes on to build, only its way to the point counts.
    """
    problems = []
    for slot, (start, end) in zip(commanders, moves):
        track = [entry for entry in tracks.get(slot, []) if entry[0] >= ORDER_TICK]
        if not track:
            problems.append(f"commander {slot} is not in the trace")
            continue
        # The move ends the tick the commander is found on the point's cell:
        # where it stood at the end of its last tick of moving.
        ended = next((index for index, entry in enumerate(track) if index > 0 and
                      entry[4] != MOVE_GROUND), None)
        way = track if ended is None else track[:ended]
        rest = track if slot not in queued else way
        last = rest[-1]
        top = max(entry[3] for entry in way)
        turned = reversals(rest)
        where = (f"commander {slot} sent from {start[0]},{start[1]} to {end[0]},{end[1]}, "
                 f"top speed {top:.2f}, {turned} reversals")
        if top < FAST_ENOUGH:
            problems.append(where + f": it never reached {FAST_ENOUGH} pixels a tick")
        if ended is None:
            problems.append(where + f": its move had not ended at tick {track[-1][0]}, "
                            f"where it stands at {track[-1][1]:.1f},{track[-1][2]:.1f}")
            continue
        arrived = way[-1]
        apart = math.hypot(arrived[1] - end[0], arrived[2] - end[1])
        if apart > ARRIVED_REACH:
            problems.append(where + f": its move ended at tick {arrived[0] + 1} "
                            f"{apart:.1f} pixels from the point")
        if slot not in queued:
            settled = math.hypot(last[1] - end[0], last[2] - end[1])
            if settled > ARRIVED_REACH or last[3] != 0:
                problems.append(where + f": at tick {last[0]} it is {settled:.1f} pixels from "
                                f"the point at speed {last[3]:.2f}, not at rest on it")
        if turned > MOST_REVERSALS:
            problems.append(where + ": it oscillated")
    return [f"{name}: {problem}" for problem in problems]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--packages", type=Path, required=True,
                        help="tests/data/mutators/packages")
    parser.add_argument("--oa-tool", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    native = arguments.native.absolute()
    game_dir = arguments.game_dir.absolute()
    package = (arguments.packages / PACKAGE).absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)

        problems = []
        rows = [((x, z), (x + 150 + 31 * index, z)) for index, (x, z) in enumerate(ROWS)]
        outputs = {}
        for name, moves in (("rows", rows), ("ways", WAYS)):
            output, placed, tracks = run(native, game_dir, package, workdir, name,
                                         stage_text(moves))
            outputs[name] = output
            commanders = [slot for slot, kind in placed if kind == "ARMCOM"]
            if len(commanders) != len(moves):
                raise SystemExit(f"{name}: the stage placed {placed}")
            problems += check_moves(name, commanders, moves, tracks)

        # The wall goes up first and the queued commander is placed last, so
        # the build line, which orders the stage's last unit, is its.
        moves = [DETOUR, QUEUED]
        wall = ["group wall"] + [f"place 0 ARMDRAG {x} {z}" for x, z in WALL] + ["group"]
        text = stage_text(moves, wall) + (
            f"at {ORDER_TICK} build ARMSOLAR {SOLAR_SITE[0] - QUEUED[0][0]} "
            f"{SOLAR_SITE[1] - QUEUED[0][1]}\n")
        output, placed, tracks = run(native, game_dir, package, workdir, "detour", text)
        outputs["detour"] = output
        commanders = [slot for slot, kind in placed if kind == "ARMCOM"]
        if len(commanders) != len(moves):
            raise SystemExit(f"detour: the stage placed {placed}")
        problems += check_moves("detour", commanders, moves, tracks, queued=(commanders[-1],))
        # The dragon's teeth turn into features, and the solar collector may
        # take a slot one of them held: it is the unit at the site at the end.
        built = [slot for slot, track in tracks.items()
                 if slot not in commanders and track[-1][0] == TICKS and
                 math.hypot(track[-1][1] - SOLAR_SITE[0], track[-1][2] - SOLAR_SITE[1]) <=
                 SITE_REACH]
        if not built:
            problems.append("detour: the solar collector queued behind the move was never "
                            "started")
        elif not tracks[built[0]][-1][5] & 1:
            problems.append("detour: the solar collector queued behind the move was not "
                            "finished")
        # The same values with no mutator: the rows steer as 3.1c steers
        # them, so a commander steps past its point.
        mod = make_racing_mod(arguments.oa_tool.absolute(), game_dir, workdir)
        output, placed, tracks = run(native, game_dir, package, workdir, "unheld",
                                     stage_text(rows), mod)
        outputs["unheld"] = output
        commanders = [slot for slot, kind in placed if kind == "ARMCOM"]
        if len(commanders) != len(rows):
            raise SystemExit(f"unheld: the stage placed {placed}")
        furthest = 0.0
        top = 0.0
        for slot, ((x, z), (end_x, end_z)) in zip(commanders, rows):
            track = [entry for entry in tracks.get(slot, []) if entry[0] >= ORDER_TICK]
            top = max([top] + [entry[3] for entry in track])
            furthest = max([furthest] + [entry[1] - end_x for entry in track])
        if top < FAST_ENOUGH:
            problems.append(f"unheld: the mod's ARMCOM never reached {FAST_ENOUGH} pixels a "
                            f"tick (top speed {top:.2f})")
        if furthest <= ARRIVED_REACH:
            problems.append(f"unheld: with no mutator no commander stepped more than a cell "
                            f"past its point (at most {furthest:.1f} pixels): the steering held "
                            "them, where 3.1c's does not")
        if problems:
            for name, output in outputs.items():
                print(f"--- {name} run ---")
                print(output, end="")
            raise SystemExit("\n".join(problems))
    print(f"fast commander check: {len(ROWS)} commanders sent east, {len(WAYS)} sent their own "
          "ways and one round a wall arrived at "
          f"{TOP_SPEED} pixels a tick without oscillating, the build queued behind a "
          "move was finished, and with no mutator they stepped past their points as 3.1c's "
          "steering makes them")


if __name__ == "__main__":
    main()
