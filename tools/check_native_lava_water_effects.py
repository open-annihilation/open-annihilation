#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a lava world's sea shows none of OA's water effects and a water map's shows them.

Three maps' shores are rendered by director scripts on the graphics card's
Full tier: a lava world (Lava & Two Hills, lavaworld=1), a lava world whose
sea swallows shots (Higher Ground, lavaworld=1 and nosealeveltrigger=1)
and a water map (Coast To Coast). On each, a stage puts a kbot at the
shore's edge and walks it along the shore, while two destroyers off the
shore, one of each player, shell each other. The script shows the shore in
the classic view for four seconds, then turned, as Full 3D Mode draws it,
for four more.

Each map is rendered twice: with Water ripples, Coastal waves, Underwater
shimmer and Naval reflections at 100%, and with all four at 0%. On the two
lava worlds every frame of the two renders must be the same picture: no
coastal waves or ripples, no rings round the destroyers as they fire or
are hit, and no rings or splashes where their shells land. On the water
map the frames of each view must differ, as the waves, the ripples and the
shells' rings and splashes draw. The shores are found in each map's
terrain: flat land at the edge of the sea.

The card draws these frames, so the check needs the system's video driver;
where no renderer on a graphics card can be made it skips. With --pictures
it copies a still of each view of each render there.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

# Each map: the name the game lists it by, its archive, and whether its sea
# is lava.
MAPS = (("lava", "Lava & Two Hills", "totala2.hpi", True),
        ("slate", "Higher Ground", "ccmaps.ccx", True),
        ("water", "Coast To Coast", "totala2.hpi", False))
# The classic view's shot runs from tick 0 to TURN_TICK, the turned view's
# from there to END_TICK; a frame a tick.
TURN_TICK = 120
END_TICK = 240
# The still of each view, once the destroyers' shells land in the sea.
STILLS = (100, 220)
WATER_EFFECTS = ("water-ripples", "coastal-waves", "underwater-shimmer", "naval-reflections")
# The lighting effects stay off, so that only the water's differ.
OTHER_EFFECTS = ("weapon-lighting", "nanolathe-lighting", "wreck-glow", "heat-distortion")
# Cells of map terrain, map pixels a side.
CELL = 16
# A shore: this many cells of land, level within LEVEL_SPAN, then within
# MOST_SLOPE cells this many cells of sea, on each of the rows SHORE_ROWS
# either side, at least MARGIN cells from the map's edges.
LAND_CELLS = 4
LEVEL_SPAN = 14
SEA_CELLS = 6
MOST_SLOPE = 8
SHORE_ROWS = 8
MARGIN = 24
SCRIPT = """oascript: 1
input:
  stage: shore.stage
output:
  resolution: {{ width: 640, height: 360 }}
  framerate: 30
  chunking: {{ mode: ticks, length: 60 }}
director:
  shots:
    - tick: 0
      cameraStart: {{ position: {{ x: {x}, y: 320, z: {z} }} }}
      cameraEnd: {{ position: {{ x: {x}, y: 320, z: {z} }} }}
    - tick: {turn}
      cameraStart: {{ position: {{ x: {x}, y: 320, z: {z} }}, orientation: {{ yaw: 30, pitch: -20 }} }}
      cameraEnd: {{ position: {{ x: {x}, y: 320, z: {z} }}, orientation: {{ yaw: 30, pitch: -20 }} }}
  endTick: {end}
"""
STILL = re.compile(r"^director: still of frame (\d+), tick (\d+): (.+)$", re.M)
SKIPPED = "no renderer on a graphics card"


def fail(output, message):
    print(output, end="")
    raise SystemExit(message)


def read_map(oa_tool, game_dir, workdir, name, archive):
    """Returns the map's sea level and its terrain heights, row by row."""
    tnt = workdir / f"{name}.tnt"
    result = subprocess.run(
        [*RUNNER, str(oa_tool), "asset-extract", str(game_dir), f"maps/{name}.tnt", str(tnt),
         str(game_dir / archive)],
        cwd=workdir, timeout=120, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        fail(result.stdout + result.stderr, f"{name}'s map could not be read")
    data = tnt.read_bytes()
    # The TNT header: width and height in cells, the attribute table's
    # offset, and the sea level; each attribute starts with its height.
    _, width, height, _, attributes, _, _, _, _, sea_level = struct.unpack_from("<10i", data)
    rows = [[data[attributes + 4 * (z * width + x)] for x in range(width)] for z in range(height)]
    return sea_level, rows


def find_shore(name, sea, rows):
    """The centre of a shore's last land cell, the way to the sea along x, and the slope's cells.

    The shore nearest the map's middle with the gentlest slope wins.
    """
    height, width = len(rows), len(rows[0])
    zs = sorted(range(MARGIN, height - MARGIN), key=lambda z: abs(z - height // 2))
    xs = sorted(range(MARGIN, width - MARGIN), key=lambda x: abs(x - width // 2))

    def holds(x, z, way, slope):
        for row in rows[z - SHORE_ROWS:z + SHORE_ROWS + 1]:
            land = [row[x - way * k] for k in range(LAND_CELLS)]
            water = [row[x + way * k] for k in range(1 + slope, 1 + slope + SEA_CELLS)]
            if min(land) <= sea or max(land) - min(land) > LEVEL_SPAN or max(water) >= sea:
                return False
        return True

    for slope in range(MOST_SLOPE + 1):
        for z in zs:
            for x in xs:
                for way in (1, -1):
                    if holds(x, z, way, slope):
                        return x * CELL + CELL // 2, z * CELL + CELL // 2, way, slope
    raise SystemExit(f"{name} has no shore of flat land beside the sea")


def stage_text(x, z, way, slope):
    """A kbot walking along the shore's edge, and two destroyers off it shelling each other."""
    facing = "east" if way > 0 else "west"
    edge = x - way * 24
    sea = x + way * CELL * (slope + 4)
    return "\n".join([
        "# A kbot at the shore's edge walks along it; two destroyers off the",
        "# shore shell each other.",
        "group walker",
        f"place 0 ARMPW {edge} {z - 96} {facing}",
        "group ship",
        f"place 0 ARMROY {sea} {z - 96} south",
        "group target",
        f"place 1 CORROY {sea} {z + 112} north",
        "group",
        "at 5 attack ship target",
        f"at 10 move walker {edge} {z + 96}",
    ]) + "\n"


def render(native, game_dir, workdir, label, map_name, stage, camera, percent):
    """Renders the shore; returns the frames' hashes and the stills' paths."""
    folder = workdir / f"{label}-{percent}"
    folder.mkdir()
    (folder / "shore.stage").write_text(stage)
    (folder / "shore.oascript").write_text(
        SCRIPT.format(x=camera[0], z=camera[1], turn=TURN_TICK, end=END_TICK))
    preferences = folder / "shore.conf"
    preferences.write_text("open-annihilation-preferences 1\n"
                           f"\"Total Annihilation|SkirmishMap\" \"{map_name}\"\n")
    effects = []
    for effect in WATER_EFFECTS:
        effects += ["--effect", f"{effect}={percent}"]
    for effect in OTHER_EFFECTS:
        effects += ["--effect", f"{effect}=0"]
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--preferences-file",
         str(preferences), "--render-script", str(folder / "shore.oascript"),
         "--hardware-acceleration=full", *effects,
         "--stills", ",".join(str(frame) for frame in STILLS)],
        cwd=folder, env=dict(os.environ, SDL_AUDIO_DRIVER="dummy", OA_DIRECTOR_ENCODER="none"),
        timeout=600, check=False, capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    if SKIPPED in output:
        print(f"lava water effects check: skipped: {SKIPPED}")
        raise SystemExit(77)
    if result.returncode != 0:
        fail(output, f"the render of {map_name} at {percent}% exited with {result.returncode}")
    stills = {int(frame): folder / path for frame, _, path in STILL.findall(result.stdout)}
    if sorted(stills) != list(STILLS) or not all(path.exists() for path in stills.values()):
        fail(output, f"the render of {map_name} at {percent}% wrote the stills {stills}")
    frames = {}
    for chunk in sorted((folder / "shore").glob("shore-*.frames")):
        for line in chunk.read_text().splitlines():
            frame, _, digest = line.split()
            frames[int(frame)] = digest
    if sorted(frames) != list(range(END_TICK)):
        fail(output, f"the render of {map_name} at {percent}% drew the frames {sorted(frames)}")
    return frames, stills


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--oa-tool", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--pictures", type=Path,
                        help="a folder to copy each render's stills into")
    arguments = parser.parse_args()
    # The runs start in the scratch folder.
    native = arguments.native.absolute()
    game_dir = arguments.game_dir.absolute()
    oa_tool = arguments.oa_tool.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    if arguments.pictures:
        arguments.pictures.mkdir(parents=True, exist_ok=True)
    problems = []
    with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        for label, map_name, archive, lava in MAPS:
            sea, rows = read_map(oa_tool, game_dir, workdir, map_name, archive)
            x, z, way, slope = find_shore(map_name, sea, rows)
            stage = stage_text(x, z, way, slope)
            # The view's centre a little out to sea, on the sea's row of the
            # map's picture.
            camera = (x + way * 48, z - sea // 2)
            print(f"{label}: {map_name}, sea level {sea}, shore at {x},{z} facing "
                  f"{'east' if way > 0 else 'west'} over {slope} cells of slope")
            renders = {}
            for percent in (100, 0):
                renders[percent] = render(native, game_dir, workdir, label, map_name, stage,
                                          camera, percent)
                if arguments.pictures:
                    for frame, path in renders[percent][1].items():
                        view = "classic" if frame < TURN_TICK else "turned"
                        shutil.copyfile(path, arguments.pictures / f"{label}-{view}-{percent}.png")
            on, off = renders[100][0], renders[0][0]
            views = {"classic": range(TURN_TICK), "turned": range(TURN_TICK, END_TICK)}
            for view, frames in views.items():
                differing = sum(on[frame] != off[frame] for frame in frames)
                print(f"{label} {view}: {differing} of {len(frames)} frames differ with the "
                      "water effects at 100% and at 0%")
                if lava and differing:
                    problems.append(f"{map_name}'s lava shows water effects in {differing} "
                                    f"frames of the {view} view")
                if not lava and differing < len(frames):
                    problems.append(f"{map_name}'s water shows no water effects in "
                                    f"{len(frames) - differing} frames of the {view} view")
    if problems:
        raise SystemExit("\n".join(problems))
    print("lava water effects check: on Lava & Two Hills and Higher Ground the water effects "
          "change no frame of a shore with a kbot walking along it and destroyers shelling "
          "each other in the lava, in the classic or the turned view; on Coast To Coast they "
          "change every frame")


if __name__ == "__main__":
    main()
