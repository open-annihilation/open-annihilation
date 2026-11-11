#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a stage plays out over its match and renders through a director script.

A headless skirmish is staged with units placed by map pixel and gathered
into groups, and lines timed for later ticks that place another unit, move
one group, send one on patrol, set one on the other's nearest units, switch
a radar tower off and on again with the ON/OFF button's orders, command one
to attack the ground and set one guarding another. The run must report
each line as its tick comes, the moved group must stand nearer its point
than where it was placed, a timed placement must hold the unit the trace
shows, and the radar must be on, then off, then on. A line
facing nowhere, an order to a group no unit joined, an "at" without an
action and an "activate" without a group must stop the run with the file
and line.

The same stage is then rendered by a director script that names it in place
of a recording, with encoding off and two stills: the render must write
both stills as PNG pictures of the script's size, the frames of every tick
and the manifest, and a second render must draw the same frames.
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

TICKS = 120
# Map pixels of the headless skirmish's map (Canal Crossing) beside the
# local player's start.
STAGE = """# Two groups of the local player and one of the computer player.
group guards
place 0 ARMPW 420 1660 north
place 0 ARMPW 460 1660
group scouts
place 0 ARMFLASH 420 1840
group raiders
place 1 CORAK 700 1700
place 1 CORAK 740 1700
group radar
place 0 ARMRAD 260 1700
group
at 20 move guards 420 1500
at 30 patrol scouts 560 1840
at 40 group late
at 40 place 0 ARMPW 380 1780 east
at 50 deactivate radar
at 60 attack late raiders
at 80 activate radar
at 90 attack-ground late 600 1700
at 90 guard scouts guards
"""
SCRIPT = """oascript: 1
input:
  stage: fight.stage
output:
  resolution: { width: 320, height: 180 }
  framerate: 30
  chunking: { mode: ticks, length: 60 }
director:
  shots:
    - tick: 0
      cameraStart: { position: { x: 520, y: 400, z: 1700 } }
      cameraEnd: { position: { x: 540, y: 300, z: 1720 } }
  endTick: 90
"""
STILLS = (0, 75)
PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player (\d+) at (-?\d+),(-?\d+)$", re.M)
UNIT = re.compile(r"^tick=(\d+) slot=(\d+) type=\d+ owner=(\d+) .* x=0x([0-9a-f]+) .* z=0x([0-9a-f]+) ",
                  re.M)
# A unit's state flags, bit 0 of which is set while the unit is on.
STATE = re.compile(r"^tick=(\d+) slot=(\d+) .* state=0x([0-9a-f]+) ", re.M)
STILL = re.compile(r"^director: still of frame (\d+), tick (\d+): (.+)$", re.M)
FRAMES = re.compile(r"^director: rendered (\d+) frames", re.M)


def run(native, game_dir, workdir, name, arguments):
    preferences = workdir / f"{name}.conf"
    preferences.write_text("open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n")
    return subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--preferences-file",
         str(preferences), *arguments],
        cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                              OA_DIRECTOR_ENCODER="none"),
        timeout=600, check=False, capture_output=True, text=True, errors="replace")


def staged(native, game_dir, workdir, name, text):
    stage = workdir / f"{name}.stage"
    stage.write_text(text)
    return run(native, game_dir, workdir, name,
               ["--skip-intro", "--headless-check", "--match-ticks", str(TICKS), "--stage", str(stage),
                "--trace-digest", str(workdir / f"{name}.trace"), "--trace-units",
                str(workdir / f"{name}.units")])


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise SystemExit(message)


def signed(word):
    value = int(word, 16)
    return (value - (1 << 32) if value >= 1 << 31 else value) >> 16


def check_play(native, game_dir, workdir):
    result = staged(native, game_dir, workdir, "play", STAGE)
    if result.returncode != 0:
        fail(result, f"the staged run exited with {result.returncode}")
    out = result.stdout
    placed = PLACED.findall(out)
    if [(name, int(player), int(x), int(z)) for _, name, player, x, z in placed] != [
            ("ARMPW", 0, 420, 1660), ("ARMPW", 0, 460, 1660), ("ARMFLASH", 0, 420, 1840),
            ("CORAK", 1, 700, 1700), ("CORAK", 1, 740, 1700), ("ARMRAD", 0, 260, 1700),
            ("ARMPW", 0, 380, 1780)]:
        fail(result, f"the stage placed {placed}")
    expected = ["stage: tick 20", "stage: move guards: 2 units to 420,1500", "stage: tick 30",
                "stage: patrol scouts: 1 units to 560,1840", "stage: tick 40", "stage: group late",
                "stage: tick 50", "stage: deactivate radar: 1 units", "stage: tick 60",
                "stage: attack late: 1 units on raiders", "stage: tick 80",
                "stage: activate radar: 1 units", "stage: tick 90",
                "stage: attack-ground late: 1 units at 600,1700",
                "stage: guard scouts: 1 units on guards"]
    at = 0
    for line in expected:
        found = out.find(line + "\n", at)
        if found < 0:
            fail(result, f"the run did not report \"{line}\" in its order")
        at = found
    # Where the units stand as the run ends: the guards moved north, and the
    # unit placed at tick 40 is in the slot the run reported.
    units = {}
    for tick, slot, owner, x, z in UNIT.findall((workdir / "play.units").read_text()):
        if int(tick) == TICKS:
            units[int(slot)] = (int(owner), signed(x), signed(z))
    guards = [int(slot) for slot, name, _, _, _ in placed[:2]]
    if any(slot not in units or units[slot][2] >= 1660 - 40 for slot in guards):
        fail(result, f"the guards did not move toward their point: {[units.get(s) for s in guards]}")
    late = int(placed[6][0])
    if late not in units or units[late][0] != 0:
        fail(result, f"the unit placed at tick 40 is not in slot {late} at the end")
    # The radar tower, on as it is built, is off from the deactivate line's
    # tick to the activate line's.
    radar = int(placed[5][0])
    on = {int(tick): int(state, 16) & 1 for tick, slot, state in
          STATE.findall((workdir / "play.units").read_text()) if int(slot) == radar}
    seen = [on.get(tick) for tick in (45, 55, 75, 85, 100)]
    if seen != [1, 0, 0, 1, 1]:
        fail(result, f"the radar was not on, off and on again at ticks 45, 55, 75, 85 and 100: {seen}")
    return units


def check_refusals(native, game_dir, workdir):
    for name, text, message in (
            ("facing", "place 0 ARMPW 420 1660 up\n",
             "facing.stage:1: a place line faces south, east, north or west, not up"),
            ("nogroup", "place 0 ARMPW 420 1660\nmove nobody 400 400\n",
             "nogroup.stage:2: no unit joined the group nobody"),
            ("noaction", "at 0\n", "noaction.stage:1: at takes TICK ACTION"),
            ("noswitch", "activate\n", "noswitch.stage:1: activate takes GROUP")):
        result = staged(native, game_dir, workdir, name, text)
        if result.returncode == 0 or message not in result.stdout + result.stderr:
            fail(result, f"the stage did not stop with \"{message}\"")


def png_size(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return None
    width, height, depth, colour = struct.unpack(">IIBB", data[16:26])
    return width, height, depth, colour


def render(native, game_dir, workdir, name):
    folder = workdir / name
    folder.mkdir()
    (folder / "fight.stage").write_text(STAGE)
    (folder / "fight.oascript").write_text(SCRIPT)
    result = run(native, game_dir, folder, name,
                 ["--render-script", str(folder / "fight.oascript"), "--stills",
                  ",".join(str(frame) for frame in STILLS)])
    if result.returncode != 0:
        fail(result, f"the render of the stage exited with {result.returncode}")
    stills = STILL.findall(result.stdout)
    if [(int(frame), int(tick)) for frame, tick, _ in stills] != [(0, 0), (75, 75)]:
        fail(result, f"the render wrote the stills {stills}")
    for _, _, path in stills:
        if png_size(folder / path) != (320, 180, 8, 2):
            fail(result, f"the still {path} is not an 8-bit RGB picture of 320 by 180")
    frames = FRAMES.search(result.stdout)
    if not frames or int(frames.group(1)) != 90:
        fail(result, "the render did not draw a frame for each of the 90 ticks")
    if "stage: attack late: 1 units on raiders" not in result.stdout:
        fail(result, "the render did not play the stage's timed lines")
    manifest = folder / "fight" / "fight.manifest"
    if not manifest.exists():
        fail(result, "the render wrote no manifest")
    return "".join((folder / "fight" / f"fight-{chunk:03}.frames").read_text() for chunk in (0, 1))


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
        check_play(arguments.native, arguments.game_dir, workdir)
        check_refusals(arguments.native, arguments.game_dir, workdir)
        first = render(arguments.native, arguments.game_dir, workdir, "first")
        second = render(arguments.native, arguments.game_dir, workdir, "second")
        if first != second:
            raise SystemExit("two renders of the same stage drew different frames")
    print("stage render check: the stage's timed lines place, move, patrol, attack and switch "
          "units off and on, and its render writes the same frames twice with its stills")


if __name__ == "__main__":
    main()
