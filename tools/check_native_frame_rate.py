#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the headless skirmish drawn frame by frame (--frame-rate).

The same seeded skirmish, its local army marching south, played through the
match clock and drawn every frame at 30, 60, 120 and 144 frames a second
must write the same trace stream, tick for tick, and reach the same world
digest: how many frames are drawn, and how far between two ticks each is
drawn, never changes the match. At 30 frames a second, the tick rate, there
must be a frame for each tick, each showing its tick whole; at the higher
rates at least half the frames are drawn between two ticks.

A run at 120 frames a second that sweeps the camera's scroll over the
marching army writes a frame log, in which the camera must move the same
distance every frame, and each frame must show a quarter of a tick more than
the one before. The check prints the motion from frame to frame of the
ground (the camera), against the whole clock units the camera stepped by
before, and of the unit the log follows over the ground, as the simulation
holds it, which moves once a tick (as every frame drew it before), and as
drawn, and on the screen. The unit as drawn must move on nearly every frame,
and no frame may carry more than half the most it moves in a tick: no step
at the tick rate.

A run at 120 frames a second whose camera tracks the marching unit (--follow)
writes a second frame log, in which the unit must be drawn at the same whole
pixel of the screen on every frame: the camera is placed where the frame
shows the unit, after the frame's clock step, so the ground moves under it
and the unit never jumps by a tick's step. The check prints the spread the
unit had on the screen when the camera was centred before the clock step on
the tick's place and the view moved by the unit's step since.

Runs at 120 frames a second with Smooth motion Off, as a preferences file
sets it, must write the same trace stream and reach the same world digest
as the runs with it On, with no frame drawn between two ticks: every frame
shows its tick whole. Their frame log, the camera's scroll swept over the
marching army, must show every frame at a whole tick, the camera moving the
same distance every frame as with Smooth motion On, and the unit the log
follows drawn where the simulation holds it, moving only on the frames
that run a tick.

Runs at 30 and 120 frames a second whose clock starts 2 seconds before
2^32 milliseconds (--frame-clock), where the match clock's reading turns over
to 0, must write the same trace stream and reach the same world digest as
the runs from 0, the match stepping on through the turn: at 30 frames a
second every frame runs a tick but the first past the turn, which runs none,
and at 120 no more than one tick's frames are added.
"""
import argparse
import csv
import math
import os
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 150
LOG_TICKS = 120
COMBAT_UNITS = 16
SEED = 1234567
RATES = (30, 60, 120, 144)
LOG_RATE = 120
TICKS_PER_SECOND = 30
# The scroll speed fresh preferences give: map pixels each clock unit.
SCROLL_SPEED = 32
# The shown time may step by up to a millisecond of ticks more or less than
# a frame's share: the clock steps on whole milliseconds.
SHOWN_STEP_TOLERANCE = 0.03
# Map pixels a unit moves between frames below which it counts as still.
STILL_PIXELS = 1e-6
# The most frames of the drawn unit's moving stretch it may stand still.
MOST_STILL_DRAWN = 0.1
# The largest step the drawn unit may take in a frame, as a share of the
# largest it takes in a tick: at 120 frames a second each frame carries about
# a quarter of a tick's step; a frame of a whole tick carries all of it.
MOST_DRAWN_STEP_SHARE = 0.5

# The whole pixels of the screen a tracked unit may be drawn at: one.
FOLLOW_SCREEN_PIXELS = 1

# A preferences file's first line, and the line that turns Smooth motion Off.
PREFERENCES_HEADER = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"'
)
SMOOTH_MOTION_OFF = '"open-annihilation.smooth-motion" "0"'

# The millisecond at which the steady clock's low 32 bits, and with them the
# match clock's reading, turn over to 0; and how long before it the runs
# across the turn start.
CLOCK_TURN_MS = 1 << 32
TURN_LEAD_MS = 2000
TURN_RATES = (30, 120)

RUN_LINE = re.compile(
    r"frame run: (\d+) frames a second, (\d+) frames, (\d+) ticks, (\d+) frames between ticks, "
    r"world digest ([0-9a-f]{16})"
)


def run(native, game_dir, workdir, name, rate, *extra, smooth=True):
    """Plays the skirmish at a frame rate and returns its run line's figures and its trace.

    With smooth False the run's preferences file holds Smooth motion Off.
    """
    if not smooth:
        (workdir / f"{name}.conf").write_text(f"{PREFERENCES_HEADER}\n{SMOOTH_MOTION_OFF}\n")
    trace = workdir / f"{name}.trace"
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(workdir / f"{name}.conf"),
         "--match-ticks", str(extra[0] if extra else TICKS), "--combat", str(COMBAT_UNITS),
         "--march", "--seed", str(SEED), "--trace-digest", str(trace), "--frame-rate", str(rate),
         *map(str, extra[1:])],
        cwd=workdir, timeout=900, check=False, capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"the {rate} frames a second run exited with {result.returncode}")
    match = RUN_LINE.search(result.stdout)
    if match is None:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"the {rate} frames a second run printed no frame run line")
    frames, ticks, between, digest = (int(match[2]), int(match[3]), int(match[4]), match[5])
    return frames, ticks, between, digest, trace.read_bytes()


def steps(values):
    """Returns the differences between neighbouring values."""
    return [later - earlier for earlier, later in zip(values, values[1:])]


def describe(name, motion):
    """Prints the spread of a motion from frame to frame and its share of still frames."""
    moving = [abs(step) for step in motion]
    still = sum(1 for step in moving if step <= STILL_PIXELS) / len(moving)
    print(f"  {name}: mean {statistics.mean(moving):.3f}, least {min(moving):.3f}, "
          f"most {max(moving):.3f} map pixels a frame; still on {still:.0%} of frames")
    return still


def whole_unit_steps(times_ms):
    """Returns the camera's steps under the whole clock units it scrolled by before."""
    units = [int(time) * TICKS_PER_SECOND // 1000 for time in times_ms]
    return [SCROLL_SPEED * step for step in steps(units)]


def check_log(path):
    rows = list(csv.DictReader(path.open()))
    if len(rows) < LOG_RATE:
        raise SystemExit(f"{path}: {len(rows)} frames")
    times = [float(row["time_ms"]) for row in rows]
    shown = [int(row["tick"]) - 1 + float(row["alpha"]) for row in rows]
    alphas = [float(row["alpha"]) for row in rows]
    if any(alpha < 0.0 or alpha > 1.0 for alpha in alphas):
        raise SystemExit(f"{path}: a fraction of a tick outside 0 to 1")
    # From the first tick on, each frame shows a quarter of a tick more.
    first = next(index for index, row in enumerate(rows) if int(row["tick"]) > 0)
    share = TICKS_PER_SECOND / LOG_RATE
    worst = max(abs(step - share) for step in steps(shown[first:]))
    if worst > SHOWN_STEP_TOLERANCE:
        raise SystemExit(f"{path}: the shown time steps {worst:.3f} ticks off {share} a frame")
    # The camera moves the same whole map pixels every frame; it turns back
    # at the map's edge.
    camera = [int(row["camera_x"]) for row in rows]
    camera_steps = [step for step in steps(camera)]
    runs = [abs(step) for step in camera_steps if step != 0]
    if not runs or max(runs) - min(runs) > 1 or len(runs) + 2 < len(camera_steps):
        raise SystemExit(f"{path}: the camera moved unevenly: {sorted(set(camera_steps))}")
    print(f"frame log at {LOG_RATE} frames a second, {len(rows)} frames:")
    describe("ground (camera), scrolled for each frame's time", camera_steps)
    describe("ground (camera), scrolled by whole clock units as before", whole_unit_steps(times))
    unit_x = [float(row["unit_x"]) for row in rows if row["unit_x"]]
    unit_z = [float(row["unit_z"]) for row in rows if row["unit_z"]]
    if len(unit_z) != len(rows):
        raise SystemExit(f"{path}: the unit the log follows is gone")
    sim_motion = [abs(dx) + abs(dz) for dx, dz in zip(steps(unit_x), steps(unit_z))]
    describe("unit as the simulation holds it (drawn so before)", sim_motion[first:])
    drawn = [row for row in rows if row["drawn_x"]]
    if not drawn:
        print("  unit as drawn: the drawing reports no position for it")
        return
    if len(drawn) != len(rows):
        raise SystemExit(f"{path}: the unit was drawn on {len(drawn)} of {len(rows)} frames")
    drawn_x = [float(row["drawn_x"]) for row in rows]
    drawn_z = [float(row["drawn_z"]) for row in rows]
    drawn_motion = [abs(dx) + abs(dz) for dx, dz in zip(steps(drawn_x), steps(drawn_z))]
    still = describe("unit as drawn between ticks", drawn_motion[first:])
    # On the screen: the unit's place less the camera's; before, the unit
    # where the simulation holds it under the camera scrolled by whole clock
    # units the same way.
    whole_camera = [camera[0]]
    for step, swept in zip(whole_unit_steps(times), camera_steps):
        whole_camera.append(whole_camera[-1] + (step if swept >= 0 else -step))
    screen_z = [z - int(row["camera_z"]) for z, row in zip(drawn_z, rows)]
    before_x = [x - at for x, at in zip(unit_x, whole_camera)]
    before_z = [z - int(row["camera_z"]) for z, row in zip(unit_z, rows)]
    describe("unit on the screen as before: whole ticks under whole clock units",
             [abs(dx) + abs(dz) for dx, dz in zip(steps(before_x), steps(before_z))][first:])
    screen_x = [x - at for x, at in zip(drawn_x, camera)]
    describe("unit on the screen, drawn between ticks under the sweeping camera",
             [abs(dx) + abs(dz) for dx, dz in zip(steps(screen_x), steps(screen_z))][first:])
    if still > MOST_STILL_DRAWN:
        raise SystemExit(f"{path}: the drawn unit stood still on {still:.0%} of frames")
    largest_tick = max(sim_motion[first:])
    largest_frame = max(drawn_motion[first:])
    print(f"  largest step: {largest_tick:.3f} map pixels in a tick, "
          f"{largest_frame:.3f} in a frame as drawn ({largest_frame / largest_tick:.0%})")
    if largest_frame > MOST_DRAWN_STEP_SHARE * largest_tick:
        raise SystemExit(f"{path}: a frame carried {largest_frame:.3f} of the unit's "
                         f"{largest_tick:.3f} map pixels a tick")


def check_whole_log(path):
    """Checks a frame log drawn with Smooth motion Off: each frame shows its tick whole."""
    rows = list(csv.DictReader(path.open()))
    if len(rows) < LOG_RATE:
        raise SystemExit(f"{path}: {len(rows)} frames")
    if any(float(row["alpha"]) != 1.0 for row in rows):
        raise SystemExit(f"{path}: a frame was drawn between two ticks with Smooth motion Off")
    # The camera's scroll is not the ticks': it moves the same whole map
    # pixels every frame, turning back at the map's edge.
    camera_steps = steps([int(row["camera_x"]) for row in rows])
    runs = [abs(step) for step in camera_steps if step != 0]
    if not runs or max(runs) - min(runs) > 1 or len(runs) + 2 < len(camera_steps):
        raise SystemExit(f"{path}: the camera moved unevenly with Smooth motion Off: "
                         f"{sorted(set(camera_steps))}")
    first = next(index for index, row in enumerate(rows) if int(row["tick"]) > 0)
    rows = rows[first:]
    if any(not row["drawn_x"] for row in rows):
        raise SystemExit(f"{path}: the unit the log follows was not drawn on every frame")
    for row in rows:
        if (abs(float(row["drawn_x"]) - float(row["unit_x"])) > STILL_PIXELS or
                abs(float(row["drawn_z"]) - float(row["unit_z"])) > STILL_PIXELS):
            raise SystemExit(f"{path}: frame {row['frame']} drew the unit away from its tick's "
                             "place with Smooth motion Off")
    drawn_motion = []
    for earlier, later in zip(rows, rows[1:]):
        moved = (abs(float(later["drawn_x"]) - float(earlier["drawn_x"])) +
                 abs(float(later["drawn_z"]) - float(earlier["drawn_z"])))
        if moved > STILL_PIXELS and later["tick"] == earlier["tick"]:
            raise SystemExit(f"{path}: frame {later['frame']} moved the unit within tick "
                             f"{later['tick']} with Smooth motion Off")
        drawn_motion.append(moved)
    print(f"frame log at {LOG_RATE} frames a second with Smooth motion Off, {len(rows)} frames:")
    describe("ground (camera), scrolled for each frame's time", camera_steps)
    describe("unit as drawn, at each tick", drawn_motion)


def spread(places, axis):
    """Returns how far apart the places lie along one axis."""
    return max(place[axis] for place in places) - min(place[axis] for place in places)


def check_follow_log(path):
    rows = list(csv.DictReader(path.open()))
    first = next(index for index, row in enumerate(rows) if int(row["tick"]) > 0)
    rows = rows[first:]
    if len(rows) < LOG_RATE or any(not row["drawn_x"] for row in rows):
        raise SystemExit(f"{path}: the tracked unit was not drawn on every frame")
    drawn_x = [float(row["drawn_x"]) for row in rows]
    drawn_z = [float(row["drawn_z"]) for row in rows]
    camera_x = [int(row["camera_x"]) for row in rows]
    camera_z = [int(row["camera_z"]) for row in rows]
    unit_x = [float(row["unit_x"]) for row in rows]
    unit_z = [float(row["unit_z"]) for row in rows]
    screen = {(math.floor(x) - cx, math.floor(z) - cz)
              for x, z, cx, cz in zip(drawn_x, drawn_z, camera_x, camera_z)}
    # Before, the camera was centred before the clock step on the place the
    # tick then held the unit at (the frame before's tick), and the view moved
    # by the unit's step from its tick's place to where the frame showed it.
    before = {(math.floor(x) - (math.floor(px) + math.floor(x) - math.floor(ux)),
               math.floor(z) - (math.floor(pz) + math.floor(z) - math.floor(uz)))
              for x, z, px, pz, ux, uz in zip(drawn_x[1:], drawn_z[1:], unit_x, unit_z,
                                              unit_x[1:], unit_z[1:])}
    print(f"tracking camera at {LOG_RATE} frames a second, {len(rows)} frames:")
    describe("ground (camera) under the tracked unit",
             [abs(dx) + abs(dz) for dx, dz in zip(steps(camera_x), steps(camera_z))])
    print(f"  unit on the screen: {len(screen)} whole pixel place(s), spread "
          f"{spread(screen, 0)} x {spread(screen, 1)} pixels; centred before the clock step "
          f"as before: spread {spread(before, 0)} x {spread(before, 1)} pixels")
    if len(screen) != FOLLOW_SCREEN_PIXELS:
        raise SystemExit(f"{path}: the tracked unit was drawn at {len(screen)} places of the "
                         f"screen: {sorted(screen)}")


def check_turn_log(path):
    """Checks that each frame across the turn runs a tick but the first past it."""
    rows = list(csv.DictReader(path.open()))
    first = next(index for index, row in enumerate(rows) if int(row["tick"]) > 0)
    ticks = [int(row["tick"]) for row in rows[first:]]
    times = [float(row["time_ms"]) for row in rows[first:]]
    if times[0] >= CLOCK_TURN_MS or times[-1] < CLOCK_TURN_MS:
        raise SystemExit(f"{path}: the frames do not cross the turn at {CLOCK_TURN_MS} ms")
    ran = steps(ticks)
    if any(step not in (0, 1) for step in ran):
        raise SystemExit(f"{path}: frames ran {sorted(set(ran))} ticks, not 1")
    idle = [time for time, step in zip(times[1:], ran) if step == 0]
    turn = next(time for time in times if time >= CLOCK_TURN_MS)
    if idle != [turn]:
        raise SystemExit(f"{path}: the frames at {idle} ms ran no tick, not the first past "
                         f"the turn alone, at {turn} ms")
    print(f"frames across the turn at {TICKS_PER_SECOND} frames a second: each ran a tick but "
          f"the first past it, at {turn:.3f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-frame-rate-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        results = {rate: run(native, game_dir, workdir, f"rate-{rate}", rate) for rate in RATES}
        reference = results[RATES[0]]
        for rate, (frames, ticks, between, digest, trace) in results.items():
            expected_frames = TICKS * rate // TICKS_PER_SECOND
            if ticks != TICKS or abs(frames - expected_frames) > 1:
                raise SystemExit(f"{rate} frames a second: {frames} frames for {ticks} ticks")
            if rate == TICKS_PER_SECOND:
                if between != 0:
                    raise SystemExit(f"{rate} frames a second: {between} of {frames} frames "
                                     "between ticks, not a whole tick each")
            elif between * 2 < frames:
                raise SystemExit(f"{rate} frames a second: {between} of {frames} frames between ticks")
            if digest != reference[3]:
                raise SystemExit(f"{rate} frames a second reached digest {digest}, "
                                 f"not {reference[3]}")
            if trace != reference[4]:
                raise SystemExit(f"{rate} frames a second wrote another trace stream")
        log = workdir / "frames.csv"
        run(native, game_dir, workdir, "log", LOG_RATE, LOG_TICKS, "--scroll-camera",
            "--frame-log", log)
        check_log(log)
        frames, ticks, between, digest, trace = run(native, game_dir, workdir, "whole",
                                                    LOG_RATE, smooth=False)
        if ticks != TICKS or frames != results[LOG_RATE][0]:
            raise SystemExit(f"Smooth motion Off: {frames} frames for {ticks} ticks, against "
                             f"{results[LOG_RATE][0]} with it On")
        if between != 0:
            raise SystemExit(f"Smooth motion Off: {between} of {frames} frames between ticks, "
                             "not a whole tick each")
        if digest != reference[3]:
            raise SystemExit(f"Smooth motion Off reached digest {digest}, not {reference[3]}")
        if trace != reference[4]:
            raise SystemExit("Smooth motion Off wrote another trace stream")
        whole_log = workdir / "whole.csv"
        run(native, game_dir, workdir, "whole-log", LOG_RATE, LOG_TICKS, "--scroll-camera",
            "--frame-log", whole_log, smooth=False)
        check_whole_log(whole_log)
        follow_log = workdir / "follow.csv"
        run(native, game_dir, workdir, "follow", LOG_RATE, LOG_TICKS, "--follow",
            "--frame-log", follow_log)
        check_follow_log(follow_log)
        for rate in TURN_RATES:
            turn_log = workdir / f"turn-{rate}.csv"
            frames, ticks, _, digest, trace = run(
                native, game_dir, workdir, f"turn-{rate}", rate, TICKS, "--frame-clock",
                CLOCK_TURN_MS - TURN_LEAD_MS, "--frame-log", turn_log)
            added = frames - results[rate][0]
            if ticks != TICKS or added < 0 or added > rate // TICKS_PER_SECOND:
                raise SystemExit(f"{rate} frames a second across the turn: {frames} frames for "
                                 f"{ticks} ticks, against {results[rate][0]} from 0")
            if digest != reference[3]:
                raise SystemExit(f"{rate} frames a second across the turn reached digest "
                                 f"{digest}, not {reference[3]}")
            if trace != reference[4]:
                raise SystemExit(f"{rate} frames a second across the turn wrote another "
                                 "trace stream")
            if rate == TICKS_PER_SECOND:
                check_turn_log(turn_log)
        print(f"native frame rate: {TICKS} ticks drawn at "
              f"{', '.join(str(rate) for rate in RATES)} frames a second, at {LOG_RATE} with "
              f"Smooth motion Off, and at "
              f"{' and '.join(str(rate) for rate in TURN_RATES)} across the clock's turn, "
              f"write one trace and reach digest {reference[3]}")


if __name__ == "__main__":
    main()
