#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that what is drawn and heard never changes the game.

One seeded skirmish, two armies of sixty fighting from the first ticks, is
played headless many ways that change only what is drawn, when and how:

- drawn at 30 frames a second with the camera where the run puts it, at the
  map's top left corner (0,0) and far from the fight (3000,3000), and with
  its scroll swept over the fight;
- drawn at 60, 120 and 144 frames a second, and at 10, where most ticks are
  never drawn;
- drawn once after every tick without the match clock (no --frame-rate);
- drawn at zoom 0.5 and 4;
- drawn with enhanced anti-aliasing at 4x.

Each frame drawn presents at most one of the unit announcements the fight
queues, as every frame of the game does; drawn after every tick, none is
presented, and once the queue is full each new one pushes out the oldest.
Which announcements are queued depends on the camera, since a unit on screen
does not say it is under attack.

Every run must write the same trace stream, tick for tick, and every run that
ends on the same tick must reach the same world digest. The units aim at
where their targets' pieces are, which the game works out from the units
themselves, the debris of explosions starts its particles in the game's
tick, and the announcements draw their random numbers from a stream of the
presentation's own, so neither the camera nor the frames drawn can move the
fight. The director's drawn and undrawn replays are compared by
native-director-view.
"""
import argparse
import concurrent.futures
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

TICKS = 300
# Units each side: two armies of sixty face each other from the first tick.
COMBAT_UNITS = 60
SEED = 7
# Runs played at once.
WORKERS = 4

# A preferences file's first line, and the enhanced anti-aliasing setting.
PREFERENCES_HEADER = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"'
)
ANTI_ALIASING_KEY = "open-annihilation.anti-aliasing"

# The trace stream: a header, then for each tick one tick record and one
# record for each section.
TRACE_HEADER_BYTES = 16
TRACE_RECORD_BYTES = 24
TRACE_SECTIONS = 7
TRACE_TICK_BYTES = TRACE_RECORD_BYTES * (1 + TRACE_SECTIONS)

RUN_LINE = re.compile(r"frame run: \d+ frames a second, \d+ frames, (\d+) ticks, .* "
                      r"world digest ([0-9a-f]{16})")

# Each run's name, extra options and preference values. The first is the
# reference the others are held to.
RUNS = (
    ("drawn-30", ("--frame-rate", "30"), {}),
    ("camera-corner", ("--frame-rate", "30", "--camera", "0,0"), {}),
    ("camera-far", ("--frame-rate", "30", "--camera", "3000,3000"), {}),
    ("camera-swept", ("--frame-rate", "30", "--scroll-camera"), {}),
    ("drawn-60", ("--frame-rate", "60"), {}),
    ("drawn-120", ("--frame-rate", "120"), {}),
    ("drawn-144", ("--frame-rate", "144"), {}),
    ("slow-frames-10", ("--frame-rate", "10"), {}),
    ("every-tick", (), {}),
    ("zoom-half", ("--frame-rate", "30", "--zoom", "0.5"), {}),
    ("zoom-4", ("--frame-rate", "30", "--zoom", "4"), {}),
    ("anti-aliasing-4", ("--frame-rate", "30"), {ANTI_ALIASING_KEY: "4"}),
)


def play(native, game_dir, workdir, name, options, preferences):
    """Plays the skirmish one way; returns the ticks run, the world digest and the trace."""
    profile = workdir / f"{name}.conf"
    if preferences:
        lines = [PREFERENCES_HEADER] + [f'"{key}" "{value}"' for key, value in preferences.items()]
        profile.write_text("\n".join(lines) + "\n")
    trace = workdir / f"{name}.trace"
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(profile),
         "--match-ticks", str(TICKS), "--combat", str(COMBAT_UNITS), "--seed", str(SEED),
         "--trace-digest", str(trace), *options],
        cwd=workdir, timeout=900, check=False, capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"{name}: exited with {result.returncode}")
    match = RUN_LINE.search(result.stdout)
    ticks, digest = (int(match[1]), match[2]) if match else (TICKS, None)
    return ticks, digest, trace.read_bytes()


def ticks_in(trace):
    """Returns how many ticks a trace stream holds."""
    return (len(trace) - TRACE_HEADER_BYTES) // TRACE_TICK_BYTES


def first_difference(trace, reference, ticks):
    """Returns the first of the ticks whose records differ, or None."""
    for tick in range(ticks):
        at = TRACE_HEADER_BYTES + tick * TRACE_TICK_BYTES
        if trace[at:at + TRACE_TICK_BYTES] != reference[at:at + TRACE_TICK_BYTES]:
            return tick
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-sim-independent-of-drawing-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
            futures = {name: pool.submit(play, native, game_dir, workdir, name, options,
                                         preferences)
                       for name, options, preferences in RUNS}
            results = {name: future.result() for name, future in futures.items()}
        reference_name = RUNS[0][0]
        reference_ticks, reference_digest, reference_trace = results[reference_name]
        if reference_ticks != TICKS or ticks_in(reference_trace) != TICKS:
            raise SystemExit(f"{reference_name}: {reference_ticks} ticks, not {TICKS}")
        for name, (ticks, digest, trace) in results.items():
            if ticks_in(trace) < TICKS:
                raise SystemExit(f"{name}: the trace holds {ticks_in(trace)} ticks")
            tick = first_difference(trace, reference_trace, TICKS)
            if tick is not None:
                raise SystemExit(f"{name}: the trace leaves {reference_name}'s at tick {tick}")
            if digest is not None and ticks == TICKS and digest != reference_digest:
                raise SystemExit(f"{name}: world digest {digest}, not {reference_digest}")
        print(f"simulation independent of drawing: {TICKS} ticks of {2 * COMBAT_UNITS} fighting "
              f"units write one trace and reach digest {reference_digest} drawn "
              f"{len(RUNS)} ways ({', '.join(name for name, _, _ in RUNS)})")


if __name__ == "__main__":
    main()
