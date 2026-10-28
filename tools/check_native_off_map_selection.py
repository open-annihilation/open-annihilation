#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that units drawn past the map's edge are selected while the view shows them.

A headless skirmish is staged near the map's left edge with the local
player's aircraft: two gunships sent past the edge, two aircraft standing on
the map near it and one farther in. A first run finds the tick at which both
gunships are well past the edge. Then, at View past the map's edge 50% and
Off, with the view centred on the first gunship as far as the setting lets
it go:

- after a box round open ground has dropped the selection the match starts
  with, a click where the first gunship is drawn selects it at 50%; at Off
  the view stays on the map, the click lands at the battlefield's edge and
  selects nothing;
- a box dragged from past the edge round the gunships and the two aircraft
  near it selects all four at 50%, and not the one farther in; at Off the
  box starts at the battlefield's edge and selects the two on the map alone,
  as 3.1c's does.

The selection is the player's own: every run's units, but for the selected
bit and the select-next marks of their flags, are the same tick for tick as
the first run's, in which nothing is selected. Each run's last frame, and
each box as it shows while the button is held, stay in the check's folder
under --scratch-root.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

SEED = 4242
# Ticks the first run plays while the gunships fly past the edge.
SURVEY_TICKS = 240
# Unit.flags' selected bit, and the selected bit with the select-next marks a
# selection sets on the units in view.
SELECTED = 0x10
SELECTION_MARKS = 0xd0
# How far past the map's left edge, in map pixels, the gunships are sent, and
# how far past it they must be before the check selects them: farther than
# half a gunship, so that no part of one is drawn over the map.
SENT_PAST = -140
WELL_PAST = -96
# Map pixels the box reaches beyond the units it is dragged round.
BOX_MARGIN = 40
# A box round open ground in view at both settings, which drops the
# commander the match starts with selected, so that the click below gives no
# order if it selects nothing.
EMPTY_BOX = (160, 430, 200, 470)
GUNSHIP = "ARMBRAWL"
SCOUT = "ARMPEEP"
STAGE = [
    "# Two gunships sent past the map's left edge, two aircraft standing near",
    "# it on the map, and a scout farther in.",
    "group past1", f"place 0 {GUNSHIP} 60 300",
    "group past2", f"place 0 {GUNSHIP} 60 380",
    "group near1", f"place 0 {SCOUT} 90 320",
    "group near2", f"place 0 {GUNSHIP} 120 390",
    "group farther", f"place 0 {SCOUT} 230 350",
    "group",
    f"at 2 move past1 {SENT_PAST} 300",
    f"at 2 move past2 {SENT_PAST} 380",
]
UNIT_LINE = re.compile(r"^stage: unit (\d+) (\S+) of player \d+ at (-?\d+),(-?\d+)$", re.M)
HOVER = re.compile(r"^stage: hover (\S+): unit (\d+) under the pointer, cursor (\S+)$", re.M)
DRAG = re.compile(r"^stage: drag (\S+) to (\S+): (.*)$", re.M)
TRACE = re.compile(r"^tick=(\d+) slot=(\d+) type=\S+ owner=\S+ flags=0x([0-9a-f]+) (.*)$")
FIELD = re.compile(r"\b(x|y|z)=0x([0-9a-f]+)")


def fail(result, message):
    if result is not None:
        print(result.stdout + result.stderr, end="")
    raise SystemExit(message)


def run(arguments, workdir, name, lines, ticks, past_edge):
    """Plays the stage at a View past the map's edge, writing the run's units and frames."""
    stage = workdir / f"{name}.stage"
    stage.write_text("\n".join(lines) + "\n")
    preferences = workdir / f"{name}.conf"
    preferences.write_text("open-annihilation-preferences 1\n"
                           f'"open-annihilation.view-past-map-edge" "{past_edge}"\n')
    command = [*RUNNER, str(arguments.native), "--game-dir", str(arguments.game_dir),
               "--skip-intro", "--mute", "--headless-check", "--seed", str(SEED),
               "--match-ticks", str(ticks), "--stage", str(stage),
               "--preferences-file", str(preferences),
               "--trace-digest", str(workdir / f"{name}.trace"),
               "--trace-units", str(workdir / f"{name}.units"),
               "--snapshot", str(workdir / f"{name}.ppm")]
    result = subprocess.run(
        command, cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=900, check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        fail(result, f"the {name} run exited with {result.returncode}")
    return result


def signed(text):
    value = int(text, 16)
    return value - (1 << 32) if value >= 1 << 31 else value


def units(path):
    """Each tick's units: slot to (flags, x, y, z in map pixels, the rest of the line)."""
    ticks = {}
    with open(path, encoding="utf-8", errors="replace") as lines:
        for line in lines:
            match = TRACE.match(line.rstrip("\n"))
            if not match:
                continue
            tick, slot, flags, rest = match.groups()
            place = {key: signed(value) / 65536 for key, value in FIELD.findall(rest)}
            ticks.setdefault(int(tick), {})[int(slot)] = (
                int(flags, 16), place.get("x"), place.get("y"), place.get("z"), rest)
    return ticks


def selected(ticks, tick, slots):
    """The slots of `slots` whose selected bit is set after `tick`."""
    return {slot for slot in slots if ticks[tick][slot][0] & SELECTED}


def drawn_at(unit):
    """The map pixel at height 0 drawn where a unit stands: x, and z less half its height."""
    _, x, y, z, _ = unit
    return round(x), round(z - y / 2)


def survey(arguments, workdir):
    """Plays the stage alone and finds the first tick at which both gunships are well past."""
    result = run(arguments, workdir, "survey", STAGE, SURVEY_TICKS, "50")
    placed = [int(slot) for slot, *_ in UNIT_LINE.findall(result.stdout)]
    if len(placed) != 5:
        fail(result, f"the stage placed {len(placed)} units, not 5")
    ticks = units(workdir / "survey.units")
    past = placed[:2]
    for tick in range(2, SURVEY_TICKS):
        # The tick before the actions run, and the tick they run before.
        if all(ticks[at][slot][1] <= WELL_PAST for at in (tick - 1, tick) for slot in past):
            break
    else:
        fail(result, f"the gunships were never both {-WELL_PAST} map pixels past the map's edge")
    if any(ticks[tick - 1][slot][1] < 0 for slot in placed[2:]):
        fail(result, "an aircraft meant to stand on the map is past its edge")
    if any(ticks[at][slot][0] & SELECTED for at in ticks for slot in placed):
        fail(result, "a unit was selected in the run that selects nothing")
    return placed, ticks, tick


def box_round(ticks, tick, slots):
    """A box round the places the units are drawn at, BOX_MARGIN beyond them."""
    places = [drawn_at(ticks[tick][slot]) for slot in slots]
    return (min(x for x, _ in places) - BOX_MARGIN, min(z for _, z in places) - BOX_MARGIN,
            max(x for x, _ in places) + BOX_MARGIN, max(z for _, z in places) + BOX_MARGIN)


def untouched(result, survey_ticks, ticks, name):
    """The run's units, but for their selection marks, are the first run's, tick for tick."""
    for tick, slots in ticks.items():
        for slot, (flags, *_, rest) in slots.items():
            first = survey_ticks.get(tick, {}).get(slot)
            if first is None or (first[0] & ~SELECTION_MARKS, first[4]) != (
                    flags & ~SELECTION_MARKS, rest):
                fail(result, f"the {name} run's unit {slot} differs on tick {tick} from the run "
                             "that selects nothing")


def check(arguments, workdir, placed, survey_ticks, tick):
    past1, past2, near1, near2, farther = placed
    click = drawn_at(survey_ticks[tick - 1][past1])
    box = box_round(survey_ticks, tick - 1, [past1, past2, near1, near2])
    if drawn_at(survey_ticks[tick - 1][farther])[0] <= box[2]:
        fail(None, "the scout meant to stand outside the box stands inside it")
    outcomes = []
    for past_edge, clicked, boxed in (("50", {past1}, {past1, past2, near1, near2}),
                                      ("off", set(), {near1, near2})):
        name = f"click-{past_edge}"
        lines = [*STAGE, f"at {tick} hover past1",
                 f"at {tick} drag {' '.join(map(str, EMPTY_BOX))}",
                 f"at {tick} drag {click[0]} {click[1]} {click[0]} {click[1]}"]
        result = run(arguments, workdir, name, lines, tick + 1, past_edge)
        ticks = units(workdir / f"{name}.units")
        untouched(result, survey_ticks, ticks, name)
        dragged = DRAG.findall(result.stdout)
        if not dragged or dragged[0][2] != "No units":
            fail(result, f"the box round open ground said {dragged[:1]}, not No units")
        hovered = HOVER.findall(result.stdout)
        if past_edge == "50" and hovered != [("past1", str(past1), "cursorselect")]:
            fail(result, f"the pointer on the gunship past the edge found {hovered}")
        if any(ticks[tick][slot][1] > WELL_PAST for slot in (past1, past2)):
            fail(result, "the gunships were not past the map's edge when they were clicked")
        if selected(ticks, tick, placed) != clicked:
            fail(result, f"at View past the map's edge {past_edge} a click on the gunship past "
                         f"the edge selected {sorted(selected(ticks, tick, placed))}, not "
                         f"{sorted(clicked)}")
        name = f"box-{past_edge}"
        lines = [*STAGE, f"at {tick} hover past1", f"at {tick} drag {' '.join(map(str, box))}"]
        result = run(arguments, workdir, name, lines, tick + 1, past_edge)
        ticks = units(workdir / f"{name}.units")
        untouched(result, survey_ticks, ticks, name)
        if selected(ticks, tick, placed) != boxed:
            fail(result, f"at View past the map's edge {past_edge} a box from past the edge "
                         f"selected {sorted(selected(ticks, tick, placed))}, not "
                         f"{sorted(boxed)}")
        dragged = DRAG.findall(result.stdout)
        if [said for *_, said in dragged] != [f"{len(boxed)} selected"]:
            fail(result, f"the box said {dragged}, not {len(boxed)} selected")
        if not (workdir / f"{name}-drag.ppm").is_file():
            fail(result, "the frame with the box held was not written")
        outcomes.append(f"{past_edge}: the click {len(clicked)}, the box {len(boxed)}")
    return outcomes


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
        # Each run starts from an empty folder of its own, and leaves its
        # frames there to look at.
        workdir = arguments.scratch_root.absolute() / "off-map-selection"
        shutil.rmtree(workdir, ignore_errors=True)
        workdir.mkdir(parents=True)
        placed, ticks, tick = survey(arguments, workdir)
        outcomes = check(arguments, workdir, placed, ticks, tick)
    else:
        with tempfile.TemporaryDirectory() as scratch:
            workdir = Path(scratch)
            placed, ticks, tick = survey(arguments, workdir)
            outcomes = check(arguments, workdir, placed, ticks, tick)
    print(f"off-map selection check: gunships past the map's edge on tick {tick}; selected at "
          f"View past the map's edge {'; '.join(outcomes)}; the simulation the same in every "
          "run")


if __name__ == "__main__":
    main()
