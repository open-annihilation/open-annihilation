#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that winning the demo's last Arm mission leaves its end screen for the ending.

open-annihilation starts on the folder holding the installer of the Total
Annihilation demo (1997) that the OA_DEMO_INSTALLER environment variable
names, with --data-dir on a scratch folder, as native-demo-installer starts
it. It runs the Arm Campaign's last mission (--mission, counted from 0; AC03
in the demo, won in about 2300 ticks) headless with --give-orders at easy,
from preferences that set nothing else. The mission must be won without a
failed tick, and the end screen after it must leave for the ending instead
of offering a next mission, though the demo holds no outcome0 bitmap for its
panel; with movies off the ending goes to the main menu.

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

CAMPAIGN = "Arm Campaign"
TICKS = 4000
# Easy: the difficulty index the preferences file stores.
EASY = 0
# The frontend's main menu state (state_id::main_menu), where the ending goes
# with movies off, as headless runs have them.
MAIN_MENU_STATE = 2
START = re.compile(r"^campaign start: .* mission (\d+) \((\S+)\) side \d+ difficulty (\d+)$", re.M)
OUTCOME = re.compile(r"^campaign outcome: (\w+) at tick (\d+)$", re.M)
ENDING = re.compile(r"^campaign end: the end screen of (.+) left for frontend state (\d+)$", re.M)
RESULT = re.compile(
    r"^campaign result: .* mission (\d+) \((\S+)\): (\d+) ticks, (\d+) failed ticks, "
    r"(\d+) distinct errors, outcome (\w+) at tick (\d+)$", re.M
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--mission", type=int, required=True,
                        help="the campaign's last mission, counted from 0")
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    named = os.environ.get(INSTALLER_VARIABLE, "")
    if not named:
        print(f"skipped the demo campaign ending native check: {INSTALLER_VARIABLE} is not set; "
              "set it to the Total Annihilation demo (1997) installer")
        return SKIP
    installer = Path(named)
    if not installer.is_file():
        print(f"FAIL {INSTALLER_VARIABLE} names no file: {named}")
        return 1
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-campaign-ending-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        preferences = workdir / "preferences.conf"
        preferences.write_text("open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n"
                               f'"Total Annihilation|Difficulty" "{EASY}"\n')
        result = subprocess.run(
            [*RUNNER, str(args.native.resolve()), "--game-dir", str(installer.resolve().parent),
             "--data-dir", str(workdir / "data"), "--skip-intro", "--mute", "--headless-check",
             "--preferences-file", str(preferences), "--campaign", CAMPAIGN,
             "--mission", str(args.mission), "--match-ticks", str(TICKS), "--give-orders"],
            cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
            timeout=900, check=False, capture_output=True, text=True, errors="replace",
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            print(output, end="")
            raise SystemExit(f"the last mission's run exited with {result.returncode}")
        start = START.search(output)
        if start is None or int(start.group(1)) != args.mission or int(start.group(3)) != EASY:
            print(output, end="")
            raise SystemExit(f"the run did not start mission {args.mission} at easy")
        outcome = OUTCOME.search(output)
        if outcome is None or outcome.group(1) != "victory":
            print(output, end="")
            raise SystemExit(f"{start.group(2)} was not won in {TICKS} ticks")
        ending = ENDING.search(output)
        if ending is None or int(ending.group(2)) != MAIN_MENU_STATE:
            print(output, end="")
            raise SystemExit(f"the end screen after {start.group(2)} did not leave through the "
                             "ending for the main menu")
        final = RESULT.search(output)
        if final is None:
            print(output, end="")
            raise SystemExit("the run wrote no campaign result")
        if int(final.group(4)) or int(final.group(5)):
            print(output, end="")
            raise SystemExit(f"{final.group(4)} failed ticks, {final.group(5)} distinct errors")
        print(f"native campaign ending: {start.group(2)} won at tick {outcome.group(2)}; the end "
              f"screen of {ending.group(1)} left through the ending for the main menu")
    return 0


if __name__ == "__main__":
    sys.exit(main())
