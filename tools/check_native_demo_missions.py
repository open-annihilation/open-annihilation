#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Play the Total Annihilation demo's (1997) missions and a save inside each.

open-annihilation starts on the folder holding the installer that the
OA_DEMO_INSTALLER environment variable names, with --data-dir on a scratch
folder, as native-demo-installer starts it, headless on SDL's dummy drivers
and from fresh preferences. --check picks what it runs:

  mission   the Arm Campaign mission --mission names (counted from 0) starts
            from its briefing with units on both sides and plays
            MISSION_TICKS ticks, or up to its outcome, without a failed
            tick;
  saveload  each demo mission saved at SAVE_TICK loads back into the same
            tick, units, world digest and orders, and the same counts of
            normal, 3D and animating features and of the features playing
            a burn, die or reclamate sequence, and the same digest of the
            saved features, with no state dropped either way, and, loaded
            again with none dropped, plays RESUME_TICKS ticks on.

Without OA_DEMO_INSTALLER the check prints one line and exits with 77, which
ctest reports as skipped.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# ctest's SKIP_RETURN_CODE.
SKIP = 77
INSTALLER_VARIABLE = "OA_DEMO_INSTALLER"

# Wall-time limit per game run; generous because a loaded machine stretches
# the headless checks without extra CPU work.
RUN_TIMEOUT_SECONDS = 900

# The demo's campaign and the mission files its campaign file lists, in order.
CAMPAIGN = "Arm Campaign"
MISSION_FILES = ("AC01.ota", "AC02.ota", "AC03.ota")
# The side the player takes, the one whose campaign list holds the campaign:
# ARM, the first side.
CAMPAIGN_SIDE = 0
# 100 seconds of game time at 30 ticks a second, the length a headless
# campaign run plays when not told otherwise.
MISSION_TICKS = 3000
# The save is taken 10 seconds into the mission, once its scripts have run,
# and the loaded game plays 5 seconds on.
SAVE_TICK = 300
RESUME_TICKS = 150

# open-annihilation's line once the demo's archive is mounted.
MOUNTED = "open-annihilation: mounted the Total Annihilation demo (1997) from "
CAMPAIGN_START = re.compile(
    r"^campaign start: (.+) mission (\d+) \((\S+)\) side (\d+) difficulty (\d+)$", re.M)
CAMPAIGN_FIRST_CENSUS = re.compile(r"^campaign tick +0: units (\d+) \(player (\d+), computer (\d+)\)", re.M)
CAMPAIGN_RESULT = re.compile(
    r"^campaign result: (.+) mission (\d+) \((\S+)\): (\d+) ticks, (\d+) failed ticks, "
    r"(\d+) distinct errors, outcome (\w+) at tick (\d+)$", re.M)
SAVED = re.compile(r"^saveload: saved .* at tick (\d+); save failures (\d+)$", re.M)
LOADED = re.compile(r"^saveload: Loaded .* at tick (\d+); restore failures (\d+)$", re.M)
DIGEST = re.compile(r"^saveload: tick (\d+) units (\d+) digest ([0-9a-f]{16})$", re.M)
ORDERS = re.compile(r"^saveload: orders (\d+)((?: \S+=\d+)*)$", re.M)
# The Features section the match would save: its counts and its digest.
FEATURES = re.compile(
    r"^saveload: features normal (\d+) 3d (\d+) animating (\d+) burn (\d+) die (\d+) "
    r"reclaim (\d+) digest ([0-9a-f]{16})$", re.M)
# The line open-annihilation writes to stderr for each tick that threw.
SIMULATION_ERROR = "simulation error:"


class CheckFailed(Exception):
    """A behaviour that differs from the expected one."""



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding="utf-8") if target.is_file() else "open-annihilation-preferences 1\n"
    if "open-annihilation.update-check" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.update-check" "never"\n'
    if "open-annihilation.content-updates" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding="utf-8")


def run_game(native, installer, data, workdir, *arguments):
    """Runs open-annihilation over the demo from preferences in `workdir`; returns its exit code, stdout and stderr."""
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
    _offline_preferences(workdir / "preferences.conf")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(installer.parent), "--data-dir", str(data),
         "--skip-intro", "--mute", "--preferences-file", str(workdir / "preferences.conf"), *arguments],
        cwd=workdir, env=environment, timeout=RUN_TIMEOUT_SECONDS, check=False,
        capture_output=True, text=True, errors="replace",
    )
    print(result.stdout, end="")
    print(result.stderr, end="")
    if MOUNTED not in result.stdout + result.stderr:
        raise CheckFailed("open-annihilation did not mount the demo's archive")
    return result.returncode, result.stdout, result.stderr


def require_exit(code, what):
    if code != 0:
        raise CheckFailed(f"{what}: open-annihilation exited with {code}")


def require_no_simulation_errors(stderr, what):
    if SIMULATION_ERROR in stderr:
        raise CheckFailed(f"{what}: a tick failed")


def check_mission(native, installer, data, workdir, mission):
    what = f"{CAMPAIGN} mission {mission} ({MISSION_FILES[mission]})"
    code, stdout, stderr = run_game(native, installer, data, workdir, "--headless-check",
                                    "--campaign", CAMPAIGN, "--mission", str(mission),
                                    "--match-ticks", str(MISSION_TICKS))
    start = CAMPAIGN_START.search(stdout)
    if start is None:
        raise CheckFailed(f"{what} did not start (exit {code})")
    if (start.group(1), int(start.group(2))) != (CAMPAIGN, mission) or \
            start.group(3).lower() != MISSION_FILES[mission].lower():
        raise CheckFailed(f"{what} started {start.group(1)} mission {start.group(2)} ({start.group(3)})")
    if int(start.group(4)) != CAMPAIGN_SIDE:
        raise CheckFailed(f"{what} started on side {start.group(4)}")
    census = CAMPAIGN_FIRST_CENSUS.search(stdout)
    if census is None:
        raise CheckFailed(f"{what} reported no units at tick 0")
    units, player, computer = (int(value) for value in census.groups())
    if player == 0 or computer == 0:
        raise CheckFailed(f"{what} started with {player} player and {computer} computer units")
    result = CAMPAIGN_RESULT.search(stdout)
    if result is None:
        raise CheckFailed(f"{what} wrote no campaign result (exit {code})")
    ran, failed, distinct = int(result.group(4)), int(result.group(5)), int(result.group(6))
    outcome, outcome_tick = result.group(7), int(result.group(8))
    if failed or distinct:
        raise CheckFailed(f"{what}: {failed} failed ticks, {distinct} distinct errors")
    decided = outcome in ("victory", "defeat")
    if ran != (outcome_tick if decided else MISSION_TICKS):
        raise CheckFailed(f"{what} ran {ran} of {MISSION_TICKS} ticks, outcome {outcome} at tick "
                          f"{outcome_tick}")
    require_exit(code, what)
    require_no_simulation_errors(stderr, what)
    print(f"demo mission check: {what} started with {units} units (player {player}, computer {computer}) "
          f"and played {ran} ticks without a failed tick, outcome {outcome}")


def saveload_state(stdout, what):
    """The tick, unit count, digest, order count, orders by kind, and feature counts and digest a saveload run printed."""
    digest = DIGEST.search(stdout)
    orders = ORDERS.search(stdout)
    features = FEATURES.search(stdout)
    if digest is None or orders is None or features is None:
        raise CheckFailed(f"{what}: no saveload digest, orders or features line")
    by_kind = dict(item.split("=") for item in orders.group(2).split())
    feature_counts = tuple(int(count) for count in features.groups()[:6])
    return (int(digest.group(1)), int(digest.group(2)), digest.group(3), int(orders.group(1)), by_kind,
            feature_counts, features.group(7))


def require_full_load(stdout, what):
    """Requires the save to have loaded into a match at SAVE_TICK with no unit part's state dropped."""
    loaded_line = LOADED.search(stdout)
    if loaded_line is None:
        raise CheckFailed(f"{what}: the save did not load into a match")
    if int(loaded_line.group(1)) != SAVE_TICK:
        raise CheckFailed(f"{what}: the save loaded at tick {loaded_line.group(1)}, not {SAVE_TICK}")
    if int(loaded_line.group(2)) != 0:
        raise CheckFailed(f"{what}: the load dropped state of {loaded_line.group(2)} unit parts")


def check_saveload(native, installer, data, workdir):
    for mission, mission_file in enumerate(MISSION_FILES):
        what = f"{CAMPAIGN} mission {mission} ({mission_file})"
        # Each mission starts from fresh preferences of its own.
        mission_dir = workdir / Path(mission_file).stem.lower()
        mission_dir.mkdir()
        save = mission_dir / "savegame" / "mission.sav"
        code, stdout, stderr = run_game(
            native, installer, data, mission_dir, "--headless-check", "--campaign", CAMPAIGN,
            "--mission", str(mission), "--match-ticks", str(SAVE_TICK), "--save-after", str(SAVE_TICK),
            "--save-file", str(save))
        require_exit(code, f"{what} saving")
        require_no_simulation_errors(stderr, f"{what} before the save")
        saved_line = SAVED.search(stdout)
        if saved_line is None or not save.is_file():
            raise CheckFailed(f"{what} wrote no save")
        if int(saved_line.group(1)) != SAVE_TICK or int(saved_line.group(2)) != 0:
            raise CheckFailed(f"{what} saved at tick {saved_line.group(1)} with {saved_line.group(2)} "
                              f"save failures")
        saved = saveload_state(stdout, f"{what} saving")
        if saved[1] == 0:
            raise CheckFailed(f"{what} saved a world without units")

        code, stdout, stderr = run_game(native, installer, data, mission_dir, "--headless-check",
                                        "--load", str(save), "--match-ticks", "0")
        require_exit(code, f"{what} loading")
        require_full_load(stdout, f"{what} loading")
        loaded = saveload_state(stdout, f"{what} loading")
        if loaded != saved:
            raise CheckFailed(
                f"{what}: loaded world differs: saved tick {saved[0]} units {saved[1]} digest {saved[2]} "
                f"orders {saved[3]} {saved[4]} features {saved[5]} {saved[6]}, loaded tick "
                f"{loaded[0]} units {loaded[1]} digest {loaded[2]} orders {loaded[3]} {loaded[4]} "
                f"features {loaded[5]} {loaded[6]}")

        code, stdout, stderr = run_game(native, installer, data, mission_dir, "--headless-check",
                                        "--load", str(save), "--match-ticks", str(RESUME_TICKS))
        require_exit(code, f"{what} resuming")
        require_full_load(stdout, f"{what} resuming")
        require_no_simulation_errors(stderr, f"{what} after the load")
        resumed = saveload_state(stdout, f"{what} resuming")
        if resumed[0] != SAVE_TICK + RESUME_TICKS or resumed[1] == 0:
            raise CheckFailed(f"{what}: the loaded mission reached tick {resumed[0]} with {resumed[1]} "
                              f"units, not tick {SAVE_TICK + RESUME_TICKS}")
        print(f"demo saveload check: {what} at tick {SAVE_TICK}, digest {saved[2]} with {saved[1]} units, "
              f"{saved[3]} orders and {saved[5][0]} normal, {saved[5][1]} 3D and {saved[5][2]} animating "
              f"features (digest {saved[6]}), loads back the same and plays on to tick {resumed[0]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", required=True, choices=("mission", "saveload"))
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--mission", type=int, choices=range(len(MISSION_FILES)),
                        help="the mission counted from 0, for --check mission")
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    if args.check == "mission" and args.mission is None:
        parser.error("--check mission needs --mission")
    named = os.environ.get(INSTALLER_VARIABLE, "")
    if not named:
        print(f"skipped the demo {args.check} native check: {INSTALLER_VARIABLE} is not set; "
              "set it to the Total Annihilation demo (1997) installer")
        return SKIP
    installer = Path(named)
    if not installer.is_file():
        print(f"FAIL {INSTALLER_VARIABLE} names no file: {named}")
        return 1
    native = args.native.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=f"native-demo-{args.check}-",
                                         dir=args.scratch_root.resolve()) as temporary:
            workdir = Path(temporary)
            # The runs of one check share the data folder the first one unpacks.
            data = workdir / "data"
            if args.check == "mission":
                check_mission(native, installer.resolve(), data, workdir, args.mission)
            else:
                check_saveload(native, installer.resolve(), data, workdir)
    except CheckFailed as failure:
        print(f"FAIL demo {args.check} check: {failure}")
        return 1
    except subprocess.TimeoutExpired as timeout:
        print(f"FAIL demo {args.check} check: a run took longer than {timeout.timeout} seconds")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
