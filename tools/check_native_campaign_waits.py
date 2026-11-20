#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a campaign's scripted factories wait out their 'w' seconds.

Core Contingency mission 6 (EXP1CC06) at medium opens with both computer
factories under script: the hover plant "w 100,b ARMMH 2,..." and the
aircraft plant "w 200,b ARMBRAWL 2,...". The game multiplies a 'w' by 30.0,
so the Wait orders last 3000 and 6000 ticks. A script clears unit flag 0x20,
which keeps the unit out of the computer player's squad sort until its
MakeSelectable, and the computer player gives no factory a pick while it
holds an order. So the computer
side makes nothing until the hover plant's wait ends, its first unit then
starts on the plant's pad, and the aircraft plant is still waiting when the
run stops.
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

CAMPAIGN = "Core Campaign - Core Contingency "
MISSION = 5
TICKS = 3300
TICKS_PER_SECOND = 30
HOVER_PLANT_WAIT = 100 * TICKS_PER_SECOND
AIR_PLANT_WAIT = 200 * TICKS_PER_SECOND
# ARMSCORP "o 1 2,w 90,3211 1608" has the schema's shortest wait.
SHORTEST_WAIT = 90 * TICKS_PER_SECOND
# Ticks between a wait running out and the unit trace showing the next order.
LATENCY_LIMIT = 5
# A factory's first nanoframe after its BuildingBuild starts.
FIRST_BUILD_LIMIT = 150
WAIT_KIND = 66
BUILDING_BUILD_KIND = 12
COMPUTER = "1"
FLAG_SELECTABLE = 0x20
START = re.compile(
    r"campaign start: Core Campaign - Core Contingency +mission 5 \(EXP1CC06\.ota\) "
    r"side (\d+) difficulty (\d+)"
)
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
    units = workdir / "exp1cc06.units"
    _offline_preferences(workdir / "preferences.conf")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute", "--headless-check",
         "--preferences-file", str(workdir / "preferences.conf"),
         "--campaign", CAMPAIGN, "--mission", str(MISSION), "--match-ticks", str(TICKS),
         "--trace-digest", str(workdir / "exp1cc06.trace"), "--trace-units", str(units)],
        cwd=workdir, timeout=1200, check=False, capture_output=True, text=True, errors="replace",
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise SystemExit(f"EXP1CC06 exited with {result.returncode}")
    return output, units


class Waiter:
    def __init__(self, slot, mobile):
        self.slot = slot
        self.mobile = mobile
        self.left = None
        self.next_kind = None
        self.selectable_tick = None


def read_units(path):
    """Scripted waiters from tick 1, and the computer units made later."""
    waiters = {}
    seen = set()
    made = []
    last_tick = 0
    with path.open() as stream:
        for line in stream:
            fields = dict(item.split("=", 1) for item in line.split())
            tick, slot = int(fields["tick"]), int(fields["slot"])
            last_tick = tick
            kind = int(fields["order_kind"])
            if slot not in seen:
                seen.add(slot)
                if tick == 1 and fields["owner"] == COMPUTER and kind == WAIT_KIND:
                    waiters[slot] = Waiter(slot, fields["movement"] == "1")
                elif tick > 1 and fields["owner"] == COMPUTER:
                    made.append((tick, slot, int(fields["parent"])))
            waiter = waiters.get(slot)
            if waiter is None or waiter.left is not None:
                continue
            if kind != WAIT_KIND:
                waiter.left, waiter.next_kind = tick, kind
            elif int(fields["flags"], 16) & FLAG_SELECTABLE and waiter.selectable_tick is None:
                waiter.selectable_tick = tick
    return waiters, made, last_tick


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-campaign-waits-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        output, units_path = run_mission(args.native.resolve(), args.game_dir.resolve(), workdir)
        start = START.search(output)
        if start is None or start.group(2) != "1":
            print(output, end="")
            raise SystemExit("EXP1CC06 did not start at medium from fresh preferences")
        result = RESULT.search(output)
        if result is None:
            print(output, end="")
            raise SystemExit("EXP1CC06 wrote no campaign result")
        if int(result.group(2)) or int(result.group(3)):
            print(output, end="")
            raise SystemExit(f"EXP1CC06: {result.group(2)} failed ticks, "
                             f"{result.group(3)} distinct errors")
        waiters, made, last_tick = read_units(units_path)

    if last_tick != TICKS:
        raise SystemExit(f"EXP1CC06: the unit trace ends at tick {last_tick}, not {TICKS}")
    held = [w.slot for w in waiters.values() if w.selectable_tick is not None]
    if held:
        raise SystemExit(f"EXP1CC06: waiting units {held} became selectable during their wait")
    plants = sorted((w for w in waiters.values() if not w.mobile), key=lambda w: w.left or TICKS + 1)
    if len(plants) != 2:
        raise SystemExit(f"EXP1CC06: {[w.slot for w in plants]} are the structures waiting at "
                         f"tick 1, not the hover and aircraft plants")
    hover, air = plants
    if hover.left is None:
        raise SystemExit(f"EXP1CC06: no plant left its wait by tick {TICKS}")
    latency = hover.left - HOVER_PLANT_WAIT
    if not 0 <= latency <= LATENCY_LIMIT or hover.next_kind != BUILDING_BUILD_KIND:
        raise SystemExit(f"EXP1CC06: the hover plant (slot {hover.slot}) left its wait at tick "
                         f"{hover.left} for order kind {hover.next_kind}, not BuildingBuild at "
                         f"{HOVER_PLANT_WAIT}")
    if air.left is not None:
        raise SystemExit(f"EXP1CC06: the aircraft plant (slot {air.slot}) left its "
                         f"{AIR_PLANT_WAIT}-tick wait at tick {air.left}")
    for waiter in waiters.values():
        if waiter.left is None:
            continue
        waited = waiter.left - latency
        if waited % TICKS_PER_SECOND or waited < SHORTEST_WAIT:
            raise SystemExit(f"EXP1CC06: slot {waiter.slot} left its wait at tick {waiter.left}, "
                             f"not a whole number of script seconds after the plant's latency")
    if not made:
        raise SystemExit(f"EXP1CC06: the computer made nothing by tick {TICKS}")
    early = [(tick, slot) for tick, slot, _ in made if tick <= hover.left]
    if early:
        raise SystemExit(f"EXP1CC06: computer units {early} appeared before the hover plant's "
                         f"wait ended at tick {hover.left}")
    strays = [(tick, slot, parent) for tick, slot, parent in made if parent != hover.slot]
    if strays:
        raise SystemExit(f"EXP1CC06: computer units {strays} did not come off the hover plant "
                         f"(slot {hover.slot})")
    first = made[0][0]
    if first - hover.left > FIRST_BUILD_LIMIT:
        raise SystemExit(f"EXP1CC06: the hover plant's first build appeared at tick {first}, "
                         f"{first - hover.left} ticks after its wait")
    print(f"native campaign waits: EXP1CC06 medium, hover plant slot {hover.slot} built from "
          f"tick {hover.left} (first unit at {first}), aircraft plant slot {air.slot} still "
          f"waiting at {TICKS}; {len(waiters)} scripted waits, {len(made)} units made")


if __name__ == "__main__":
    main()
