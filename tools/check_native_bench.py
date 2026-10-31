#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that --bench plays the bench's battle for a few seconds and prints well-formed lines.

The game starts on the dummy video and sound drivers with the bench's own
preferences (tools/bench/preferences.conf) and a short view file that plays
the bench's battle stage (tools/bench/battle.stage) on Lava Highground: a
warm-up of WARMUP_TICKS ticks and a measured span of MEASURE_SECONDS
seconds, the camera turned. The run must print, in order, WARMUP,
MEASURING, a SEC line for each second, SHOT, RESULT, the five MEASURE
spreads with the tick's mean, a STAGE line for every stage timer under its
name, COUNTS, GROUND, ATLAS and DONE; every spread must be in order (least,
median, 95th and 99th percentiles, slowest 1%, most), the frame rates must
agree with the frames and seconds, and the stage must have placed its
armies and queued its factories. tools/bench/summarise.py must read the
run. A second run with the stage timers off prints no STAGE line, and a
view file with a key the bench does not know stops the bench with a FAILED
line naming the file's line, and the game with status 1.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

BENCH = Path(__file__).resolve().parent / "bench"
RUN_TIMEOUT = 300
WARMUP_TICKS = 30
MEASURE_SECONDS = 3
VIEW = """label check
stage {stage}
menu_seconds 0
warmup_ticks {warmup}
measure_seconds {seconds}
shot_at 1
centre_x 1980
centre_z 2080
yaw 20
pitch -53
zoom 1
stage_timers {timers}
"""
# The stage timers' names, in the order the STAGE lines give them: the names
# stay the same from one release to the next, so that runs compare.
STAGES = (
    "frame(render_match_surface)", "live_total", "scene_build", "  survey+plan", "  terrain",
    "  ground_sprites", "  unit_shadows", "  foundations", "  models", "  projectiles",
    "  sprites", "  particles", "  water+rest", "gpu_total", "  validate", "  upload_copy",
    "  submit+wait", "  download_copy", "rgba_to_rgb", "picking", "effects",
    "overlays(draw_live_overlays)", "ds_setup", "ds_hud(compose_match_hud)",
    "ds_plan(draw list)", "ds_post(fog,bars)", "ds_live(render_live_match_surface)",
    "ds_after(megamap,hud text)", "ov_selection", "ov_unit_overlays", "ov_on_screen_units",
    "cursor_pick", "  effect_visibility",
)
MEASURES = ("frame", "work", "sim(per frame)", "draw", "present")
NUMBER = r"(\d+(?:\.\d+)?)"
RESULT = re.compile(
    rf"^bench: RESULT label=check seconds {NUMBER} frames (\d+) fps {NUMBER} low1_fps {NUMBER} "
    rf"ticks (\d+) \({NUMBER}/s\) game speed (\d+) of (\d+)$")
MEASURE = re.compile(
    rf"^bench: MEASURE (\S+(?: \S+)?)\s+min\s+{NUMBER} avg\s+{NUMBER} p50\s+{NUMBER} "
    rf"p95\s+{NUMBER} p99\s+{NUMBER} low1\s+{NUMBER} max\s+{NUMBER}$")
TICK = re.compile(rf"^bench: MEASURE tick\(per tick\)\s+avg\s+{NUMBER}$")
STAGE = re.compile(rf"^bench: STAGE (.+?)\s+{NUMBER}$")
SEC = re.compile(
    rf"^bench: SEC (\d+) frames (\d+) frame {NUMBER} sim {NUMBER} draw {NUMBER} "
    rf"units (\d+)\+(\d+) vertices (\d+) tick (\d+)$")
COUNTS = re.compile(r"^bench: COUNTS units (\d+) \(player0 (\d+), others (\d+)\) drawn (\d+) ")
AWAY = re.compile(r"^bench: AWAY label=check frames (\d+) of (\d+): the window was away, .+$")
GROUND = re.compile(r"^bench: GROUND revisions (\d+) in (\d+) frames$")
ATLAS = re.compile(r"^bench: ATLAS terrain_packs [\d.]+ a frame, atlas_resets (\d+) in (\d+) frames$")
QUEUED = re.compile(r"^stage: unit \d+ queues (\d+) (\S+)$", re.M)
# The battle's armies, factories, builders and economies, placed at its
# start: at least this many live units once it has begun.
LEAST_UNITS = 400


class BenchFailure(Exception):
    """A bench run that did not print what it should."""


def run_bench(native, game_dir, workdir, name, view_text):
    """Runs the game with --bench on a view file and returns its result."""
    folder = workdir / name
    (folder / "data").mkdir(parents=True)
    preferences = folder / "preferences.conf"
    shutil.copyfile(BENCH / "preferences.conf", preferences)
    view = folder / f"{name}.bench"
    view.write_text(view_text, encoding="utf-8")
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    return subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--bench", str(view), "--preferences-file", str(preferences), "--data-dir",
         str(folder / "data"), "--resolution", "640x480"],
        cwd=folder, env=environment, timeout=RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise BenchFailure(message)


def ordered(values):
    return all(left <= right for left, right in zip(values, values[1:]))


def check_lines(result, timers):
    """Checks a finished run's lines and returns them."""
    if result.returncode != 0:
        fail(result, f"the bench exited with {result.returncode}")
    lines = [line for line in result.stdout.splitlines() if line.startswith("bench: ")]
    # A window the dummy driver leaves away is noted after RESULT.
    away = [line for line in lines if line.startswith("bench: AWAY")]
    if away and not AWAY.match(away[0]):
        fail(result, f"the AWAY line does not read: {away[0]}")
    lines = [line for line in lines if not line.startswith("bench: AWAY")]
    kinds = [line.split()[1] for line in lines]
    # The lines come in their order, each kind once but SEC, MEASURE, STAGE
    # and ATLAS.
    order = ["WARMUP", "MEASURING", "SEC", "SHOT", "RESULT", "MEASURE", "STAGE", "COUNTS",
             "GROUND", "ATLAS", "DONE"]
    if not timers:
        order.remove("STAGE")
    runs = [kind for index, kind in enumerate(kinds) if index == 0 or kinds[index - 1] != kind]
    # SHOT falls within the seconds.
    runs = [kind for kind in runs if kind != "SEC"]
    expected = [kind for kind in order if kind != "SEC"]
    if runs != expected:
        fail(result, f"the bench's lines came as {runs}, not {expected}")
    if kinds.count("SEC") < MEASURE_SECONDS - 1:
        fail(result, f"the bench printed {kinds.count('SEC')} SEC lines in {MEASURE_SECONDS} s")
    for line in lines:
        if line.startswith("bench: SEC") and not SEC.match(line):
            fail(result, f"a SEC line does not read: {line}")
    result_match = next((RESULT.match(line) for line in lines if RESULT.match(line)), None)
    if not result_match:
        fail(result, "the RESULT line does not read")
    seconds, frames, fps, low1_fps = (float(result_match.group(index)) for index in (1, 2, 3, 4))
    if frames < 1 or not 0 < seconds <= MEASURE_SECONDS + 1:
        fail(result, f"the bench measured {frames:.0f} frames in {seconds} s")
    # The seconds are printed to two places, so the rate agrees within 1%.
    if abs(fps - frames / seconds) > 0.01 * fps or not 0 < low1_fps <= fps + 0.01:
        fail(result, f"the bench's frame rates {fps} and {low1_fps} do not agree")
    measured = {}
    for line in lines:
        if line.startswith("bench: MEASURE") and not TICK.match(line):
            match = MEASURE.match(line)
            if not match:
                fail(result, f"a MEASURE line does not read: {line}")
            least, mean, p50, p95, p99, low1, most = (float(value) for value in match.groups()[1:])
            if not ordered([least, p50, p95, p99, most]) or not least <= mean <= most or \
                    not p50 <= low1 <= most:
                fail(result, f"a spread is out of order: {line}")
            measured[match.group(1)] = (mean, low1)
    if tuple(measured) != MEASURES or not any(TICK.match(line) for line in lines):
        fail(result, f"the bench measured {tuple(measured)}, not {MEASURES} and the tick")
    if abs(measured["frame"][1] - 1000 / low1_fps) > 0.05 * measured["frame"][1]:
        fail(result, "the 1% low frame rate is not the slowest 1% of the frames")
    stages = [STAGE.match(line) for line in lines if line.startswith("bench: STAGE")]
    names = tuple(match.group(1).rstrip() if match else "" for match in stages)
    if timers and names != STAGES:
        fail(result, f"the stage timers are {names}, not {STAGES}")
    counts = next((COUNTS.match(line) for line in lines if COUNTS.match(line)), None)
    ground = next((GROUND.match(line) for line in lines if GROUND.match(line)), None)
    atlas = next((ATLAS.match(line) for line in lines if ATLAS.match(line)), None)
    if not counts or not ground or not atlas:
        fail(result, "the COUNTS, GROUND or ATLAS line does not read")
    if int(counts.group(1)) < LEAST_UNITS or int(ground.group(2)) != frames or \
            int(atlas.group(2)) != frames:
        fail(result, f"the bench counted {counts.group(1)} units and other frames than {frames:.0f}")
    return lines


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
    stage = BENCH / "battle.stage"
    try:
        with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
            workdir = Path(scratch)
            timed = run_bench(arguments.native, arguments.game_dir, workdir, "timed", VIEW.format(
                stage=stage, warmup=WARMUP_TICKS, seconds=MEASURE_SECONDS, timers=1))
            lines = check_lines(timed, True)
            queued = QUEUED.findall(timed.stdout)
            if len(queued) != 12 or ("60", "ARMWAR") not in queued or ("30", "CORVENG") not in queued:
                fail(timed, f"the stage queued {queued}")
            # The summariser reads the run.
            log = workdir / "timed" / "bench.txt"
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")
            table = subprocess.run(
                [sys.executable, str(BENCH / "summarise.py"), str(log)], timeout=60,
                check=False, capture_output=True, text=True, errors="replace")
            if table.returncode != 0 or "| check " not in table.stdout:
                print(table.stdout + table.stderr, end="")
                raise BenchFailure("tools/bench/summarise.py did not read the run")
            untimed = run_bench(arguments.native, arguments.game_dir, workdir, "untimed",
                                VIEW.format(stage=stage, warmup=WARMUP_TICKS, seconds=1, timers=0))
            if any(line.startswith("bench: STAGE") for line in untimed.stdout.splitlines()):
                fail(untimed, "the bench printed its stage timers with them off")
            if untimed.returncode != 0 or "bench: DONE label=check" not in untimed.stdout:
                fail(untimed, "the bench with its timers off did not finish")
            refused = run_bench(arguments.native, arguments.game_dir, workdir, "refused",
                                "label refused\nspeed 3\n")
            view = workdir / "refused" / "refused.bench"
            expected = f"bench: FAILED {view}: line 2: the bench has no key speed"
            if refused.returncode != 1 or expected not in refused.stdout.splitlines():
                fail(refused, "a view file with a key the bench does not know was not refused")
    except BenchFailure as failure:
        raise SystemExit(f"bench check: {failure}")
    print(f"bench check: the battle played, measured and reported {len(lines)} well-formed lines")


if __name__ == "__main__":
    main()
