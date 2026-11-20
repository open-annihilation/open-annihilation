#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that Arm Campaign mission 9 (AC09) at medium is lost with its commander.

The medium schema starts the player with the commander and a Galactic Gate and
sends two Pyros at the commander from the first seconds ("w 2,a ARMCOM"
and "a ARMCOM"). Left idle, the commander burns down and the gate goes in its
death blast, so a headless run from fresh preferences must end in defeat
without a failed tick. The game tests the local outcome every 30 ticks and
lets a defeat stand only when a countdown of 4 runs out below zero, five tests
later: the defeat lands 150 to 180 ticks after the commander's death.
"""
import argparse
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

CAMPAIGN = "Arm Campaign"
MISSION = 8
TICKS = 700
OUTCOME_PERIOD = 30
OUTCOME_COUNTDOWN_TICKS = 5 * OUTCOME_PERIOD
DEFEAT_WINDOW = (450, 620)
START = re.compile(r"campaign start: Arm Campaign mission 8 \(AC09\.ota\) side 0 difficulty (\d+)")
RESULT = re.compile(
    r"campaign result: .*: (\d+) ticks, (\d+) failed ticks, (\d+) distinct errors, "
    r"outcome (\w+) at tick (\d+)"
)



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


def run_mission(native, game_dir, workdir):
    units = workdir / "ac09.units"
    _offline_preferences(workdir / "preferences.conf")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute", "--headless-check",
         "--preferences-file", str(workdir / "preferences.conf"),
         "--campaign", CAMPAIGN, "--mission", str(MISSION), "--match-ticks", str(TICKS),
         "--trace-digest", str(workdir / "ac09.trace"), "--trace-units", str(units)],
        cwd=workdir, timeout=600, check=False, capture_output=True, text=True, errors="replace",
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(f"AC09 exited with {result.returncode}")
    return output, units


def read_units(path):
    """Per slot, the (tick, fields) samples of the unit dump, in tick order."""
    samples = {}
    with path.open() as stream:
        for line in stream:
            fields = dict(item.split("=", 1) for item in line.split())
            samples.setdefault(int(fields["slot"]), []).append((int(fields["tick"]), fields))
    return samples


def death_tick(samples):
    for tick, fields in samples:
        if int(fields["health"]) <= 0:
            return tick
    return samples[-1][0] + 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-campaign-defeat-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        output, units_path = run_mission(args.native.resolve(), args.game_dir.resolve(), workdir)
        start = START.search(output)
        if start is None or start.group(1) != "1":
            print(output, end="")
            raise SystemExit("AC09 did not start at medium from fresh preferences")
        result = RESULT.search(output)
        if result is None:
            print(output, end="")
            raise SystemExit("AC09 wrote no campaign result")
        failed, distinct = int(result.group(2)), int(result.group(3))
        outcome, outcome_tick = result.group(4), int(result.group(5))
        if failed or distinct:
            print(output, end="")
            raise SystemExit(f"AC09: {failed} failed ticks, {distinct} distinct errors")
        if outcome != "defeat":
            raise SystemExit(f"AC09 ended {outcome} at tick {outcome_tick}, not in defeat")

        units = read_units(units_path)
        player = {slot: samples for slot, samples in units.items()
                  if samples[0][0] == 1 and samples[0][1]["owner"] == "0"}
        mobile = [slot for slot, samples in player.items() if samples[0][1]["movement"] == "1"]
        if len(player) != 2 or len(mobile) != 1:
            raise SystemExit(f"AC09 starts the player with slots {sorted(player)}, "
                             f"mobile {mobile}: not the commander and the gate")
        commander = mobile[0]
        died = death_tick(player[commander])
        attackers = sorted(
            slot for slot, samples in units.items()
            if samples[0][1]["owner"] == "1"
            and any(fields["w0_target_a"] == str(commander) for tick, fields in samples if tick < died)
        )
        if len(attackers) < 2:
            raise SystemExit(f"AC09: only {attackers} aimed at the commander before tick {died}")
        survivors = [slot for slot, samples in player.items() if samples[-1][0] >= outcome_tick]
        if survivors:
            raise SystemExit(f"AC09: player slots {survivors} still stand at the defeat")
        delay = outcome_tick - died
        if not OUTCOME_COUNTDOWN_TICKS <= delay <= OUTCOME_COUNTDOWN_TICKS + OUTCOME_PERIOD:
            raise SystemExit(f"AC09: defeat at tick {outcome_tick}, {delay} ticks after the "
                             f"commander died at {died}")
        if not DEFEAT_WINDOW[0] <= outcome_tick <= DEFEAT_WINDOW[1]:
            raise SystemExit(f"AC09: defeat at tick {outcome_tick}, outside {DEFEAT_WINDOW}")
        print(f"native campaign defeat: AC09 medium, commander slot {commander} attacked by "
              f"{attackers}, died at tick {died}, defeat at tick {outcome_tick}")


if __name__ == "__main__":
    main()
