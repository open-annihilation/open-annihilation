#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the drawing threads never change a frame.

The seeded headless skirmish is drawn frame by frame (--frame-rate) on one
drawing thread and on several (--draw-threads):

- its armies starting to fight with the camera's scroll held, at zoom 1,
  zoomed in and zoomed out: the terrain fill and the fog split their rows
  into bands by the data alone;
- a longer fight with --busy-combat, into its explosions, debris, shatter
  fragments and smoke, with missile trucks firing, a kbot lab building
  peewees in a spray of nano particles, an air transport loading a peewee
  and the local army selected with its selection boxes shown, at zoom 1
  with enhanced anti-aliasing off, at 45 frames a second so that half the
  frames fall between ticks, and at 4x: the battlefield's units,
  features and effects are drawn in one band of rows for each drawing
  thread, each band drawing every draw of the frame with its own rows
  alone, and the units are planned on the drawing threads before the
  frame's draw list takes them in their order.

Every run of a view must print the same frames digest, a digest of every
frame's drawn battlefield, the same draw list digest, a digest of every
frame's draw list as the units' plans left it, and the same world digest;
and each run must say it drew on the threads it was given, and in as many
bands at zoom 1 (where every tile row can start one) and in at least two
elsewhere. (The frame's conversion for the window, which a headless run
does not make, is checked by app-xrgb-conversion.)
"""
import argparse
import concurrent.futures
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

SEED = 20261001
# A preferences file's first line, and the enhanced anti-aliasing setting.
PREFERENCES_HEADER = "open-annihilation-preferences 1"
ANTI_ALIASING_KEY = "open-annihilation.anti-aliasing"
# The views: window size, zoom, ticks, units each side, frames a second,
# whether the scroll is held, the anti-aliasing level (1 for off) and
# whether the fight is --busy-combat's. Zoom 1 copies whole tile rows; the
# others sample the map through the zoom's step, in and out. The busy
# fights of 24 a side reach their explosions, debris, wrecks, missiles,
# nano particles and loaded transport within 180 ticks; at 45 frames a
# second every other frame falls between two ticks, where the units, the
# transport and its cargo among them, draw from copies placed between them.
VIEWS = (
    ("1280x720", "1", 45, 12, 60, True, 1, False),
    ("1024x768", "1.37", 45, 12, 60, True, 1, False),
    ("1024x768", "0.6", 45, 12, 60, True, 1, False),
    ("1280x720", "1", 180, 24, 45, False, 1, True),
    ("1920x1080", "1", 180, 24, 30, False, 4, True),
)
# One drawing thread, then counts that split the rows unevenly and evenly.
THREAD_COUNTS = (1, 2, 3, 4, 7)
# Runs played at once.
WORKERS = 3

RUN_LINE = re.compile(
    r"frame run: .* world digest ([0-9a-f]{16}), frames digest ([0-9a-f]{16}), "
    r"drawing threads (\d+), drawing bands (\d+), draw list digest ([0-9a-f]{16})"
)


def run(native, game_dir, workdir, view, threads):
    """Draws the skirmish's frames and returns its world, frames and draw list digests."""
    resolution, zoom, ticks, units, frame_rate, scroll, anti_aliasing, busy = view
    name = (f"{resolution}-zoom-{zoom}-ticks-{ticks}-aa-{anti_aliasing}"
            f"{'-busy' if busy else ''}-threads-{threads}")
    preferences = workdir / f"{name}.conf"
    if anti_aliasing != 1:
        preferences.write_text(
            f'{PREFERENCES_HEADER}\n"{ANTI_ALIASING_KEY}" "{anti_aliasing}"\n'
        )
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(preferences),
         "--match-ticks", str(ticks), "--combat", str(units), "--seed", str(SEED),
         *(["--busy-combat"] if busy else []),
         "--frame-rate", str(frame_rate), *(["--scroll-camera"] if scroll else []),
         "--resolution", resolution, "--zoom", zoom, "--draw-threads", str(threads)],
        cwd=workdir, timeout=900, check=False, capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"{name}: the run exited with {result.returncode}")
    match = RUN_LINE.search(result.stdout)
    if match is None:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"{name}: the run printed no frame run line")
    if int(match[3]) != threads:
        raise SystemExit(f"{name}: drew on {match[3]} threads, not {threads}")
    bands = int(match[4])
    # One band a thread wherever every tile row can start a band, and more
    # than one elsewhere: a count that drew in one band would test nothing.
    least = threads if zoom == "1" else min(threads, 2)
    if bands < least or bands > threads:
        raise SystemExit(f"{name}: drew in at most {bands} bands on {threads} threads")
    return match[1], match[2], match[5]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-draw-threads-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
            futures = {(view, threads): pool.submit(run, native, game_dir, workdir, view, threads)
                       for view in VIEWS for threads in THREAD_COUNTS}
            results = {key: future.result() for key, future in futures.items()}
        for view in VIEWS:
            resolution, zoom, ticks, units, frame_rate, _, anti_aliasing, busy = view
            what = (f"{resolution} at zoom {zoom}, {ticks} ticks of {2 * units} units"
                    f"{' and the busy combat' if busy else ''} at {frame_rate} frames a "
                    f"second, anti-aliasing {anti_aliasing}x")
            alone = results[(view, THREAD_COUNTS[0])]
            for threads in THREAD_COUNTS[1:]:
                digests = results[(view, threads)]
                if digests != alone:
                    raise SystemExit(
                        f"{what}: {threads} threads drew frames digest {digests[1]} "
                        f"(world {digests[0]}, draw list {digests[2]}), one thread "
                        f"{alone[1]} (world {alone[0]}, draw list {alone[2]})"
                    )
            print(f"  {what}: frames digest {alone[1]}, draw list digest {alone[2]} on "
                  f"{', '.join(str(count) for count in THREAD_COUNTS)} threads")
        print("native draw threads: every view plans the same draw lists and draws the same "
              "frames on every count of drawing threads")


if __name__ == "__main__":
    main()
