#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a headless run with --give-orders wins Arm mission 1 and goes on.

AC01 is won when any of the player's units comes within 64 of the Galactic
Gate (MoveUnitToRadius). --give-orders sends the player's armed units there,
so a headless run from fresh preferences must reach victory, then go through
the end screen and the next briefing into AC02 as a player pressing Start
would, and keep playing it, without a failed tick. AC02 counts its own
ticks from its start: its unit counts begin at tick 0 and its orders come
every 300 of them. --snapshot must write the two briefings, and between
them AC01's last frame, with its outcome's title, and its end screen.

Both briefings are drawn over ARM's background (bitmaps/mbriefarm.pcx, read
through --tool), every pixel in a colour of its palette, the planet and the
panorama as much as the gadgets. Their text is in ARM's colours: the rows in
palette entry 53 and the MORE... caption in 51, and AC01's "*PRIORITY
CRITICAL*" in the yellow of the highlights, 64, as it shows before its first
flash (the headless clock stands still).
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
MISSION = 0
TICKS = 2400
OUTCOME = re.compile(r"^campaign outcome: (\w+) at tick (\d+)$", re.M)
ADVANCE = re.compile(r"^campaign advance: run tick (\d+): mission (\d+) \((\S+)\), (.+)$", re.M)
ORDERS = re.compile(r"^mission orders: tick (\d+): (\d+) to the goal", re.M)
CENSUS = re.compile(r"^campaign tick +(\d+): units", re.M)
# Ticks of a mission between the orders --give-orders gives.
ORDER_PERIOD = 300
RESULT = re.compile(
    r"^campaign result: .* mission (\d+) \((\S+)\): (\d+) ticks, (\d+) failed ticks, "
    r"(\d+) distinct errors, outcome (\w+) at tick (\d+)$", re.M
)
STAGES = ("briefing-0", "outcome-0", "end-0", "briefing-1")
BRIEFINGS = ("briefing-0", "briefing-1")
# The briefing background of the side fresh preferences choose, ARM.
BACKGROUND = "bitmaps/mbriefarm.pcx"
# Archive kinds the game folder holds.
ARCHIVES = (".hpi", ".ccx", ".ufo", ".gp3")
# MSNBRIEF.GUI's TextRegion and MOREBAR as (left, top, right, bottom), right
# and bottom excluded.
TEXT_REGION = (48, 285, 408, 433)
MORE_BAR = (47, 437, 417, 452)
# ARM's palette entries for the briefing's rows, its MOREBAR caption and a
# yellow highlight.
ROW_COLOUR = 53
MORE_COLOUR = 51
YELLOW_COLOUR = 64
# PCX files end in a marker byte and a 256-entry RGB palette.
PCX_PALETTE_BYTES = 769
PCX_PALETTE_MARKER = 0x0C



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


def read_ppm(path):
    """The width, height and RGB bytes of a binary PPM."""
    data = path.read_bytes()
    magic, size, depth, pixels = data.split(b"\n", 3)
    width, height = map(int, size.split())
    if magic != b"P6" or depth != b"255" or len(pixels) != width * height * 3:
        raise SystemExit(f"{path.name} is not a 24-bit PPM")
    return width, height, pixels


def background_palette(tool, game_dir, workdir):
    """The 256 RGB entries of the briefing background's palette."""
    archives = sorted(str(entry) for entry in game_dir.iterdir()
                      if entry.suffix.lower() in ARCHIVES)
    target = workdir / "background.pcx"
    result = subprocess.run(
        [*RUNNER, str(tool.resolve()), "asset-extract", str(game_dir), BACKGROUND, str(target),
         *archives],
        cwd=workdir, timeout=120, check=False, capture_output=True, text=True, errors="replace",
    )
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"cannot read {BACKGROUND}")
    tail = target.read_bytes()[-PCX_PALETTE_BYTES:]
    if len(tail) != PCX_PALETTE_BYTES or tail[0] != PCX_PALETTE_MARKER:
        raise SystemExit(f"{BACKGROUND} holds no 256-colour palette")
    return [tuple(tail[1 + entry * 3:4 + entry * 3]) for entry in range(256)]


def check_briefing(path, palette):
    """Checks a briefing snapshot's colours against its background's palette."""
    width, _, pixels = read_ppm(path)
    colours = set(palette)
    stray = {tuple(pixels[at:at + 3]) for at in range(0, len(pixels), 3)} - colours
    if stray:
        raise SystemExit(f"{path.name} draws {len(stray)} colours outside its background's "
                         f"palette, such as {sorted(stray)[:4]}")

    def shown(area, entry):
        left, top, right, bottom = area
        wanted = bytes(palette[entry])
        return any(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] == wanted
                   for y in range(top, bottom) for x in range(left, right))

    for area, entry, what in ((TEXT_REGION, ROW_COLOUR, "rows"),
                              (MORE_BAR, MORE_COLOUR, "MORE... caption")):
        if not shown(area, entry):
            raise SystemExit(f"{path.name} shows no {what} in ARM's colour {entry}")
    return shown(TEXT_REGION, YELLOW_COLOUR)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True, help="oa-tool, to read the background")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-campaign-advance-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        snapshot = workdir / "play.ppm"
        _offline_preferences(workdir / "preferences.conf")
        result = subprocess.run(
            [*RUNNER, str(args.native.resolve()), "--game-dir", str(args.game_dir.resolve()),
             "--skip-intro", "--mute", "--headless-check",
             "--preferences-file", str(workdir / "preferences.conf"),
             "--campaign", CAMPAIGN, "--mission", str(MISSION), "--match-ticks", str(TICKS),
             "--give-orders", "--snapshot", str(snapshot)],
            cwd=workdir, timeout=900, check=False, capture_output=True, text=True, errors="replace",
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            print(output, end="")
            raise SystemExit(f"AC01 exited with {result.returncode}")
        orders = ORDERS.search(output)
        if orders is None or int(orders.group(2)) == 0:
            print(output, end="")
            raise SystemExit("AC01: --give-orders sent no unit to the Galactic Gate")
        outcome = OUTCOME.search(output)
        if outcome is None or outcome.group(1) != "victory":
            print(output, end="")
            raise SystemExit("AC01 was not won")
        advance = ADVANCE.search(output)
        if advance is None or advance.group(2) != "1" or advance.group(3).lower() != "ac02.ota":
            print(output, end="")
            raise SystemExit("the won AC01 did not go on into AC02")
        # AC01 started with the run, so it ended at the same tick of both.
        if advance.group(1) != outcome.group(2):
            print(output, end="")
            raise SystemExit(f"AC01 ended at tick {outcome.group(2)} but the advance names run "
                             f"tick {advance.group(1)}")
        after = output[advance.end():]
        census = CENSUS.search(after)
        if census is None or census.group(1) != "0":
            print(output, end="")
            raise SystemExit("AC02's unit counts do not start at its tick 0")
        late = [tick for tick in map(int, (order.group(1) for order in ORDERS.finditer(after)))
                if tick % ORDER_PERIOD]
        if late:
            print(output, end="")
            raise SystemExit(f"AC02's orders came at its ticks {late}, not every {ORDER_PERIOD}")
        final = RESULT.search(output)
        if final is None:
            print(output, end="")
            raise SystemExit("the run wrote no campaign result")
        if final.group(1) != "1" or int(final.group(3)) != TICKS:
            raise SystemExit(f"the run ended in mission {final.group(1)} after {final.group(3)} "
                             f"ticks, not in AC02 after {TICKS}")
        if int(final.group(4)) or int(final.group(5)):
            print(output, end="")
            raise SystemExit(f"{final.group(4)} failed ticks, {final.group(5)} distinct errors")
        missing = [stage for stage in STAGES if not (workdir / f"play-{stage}.ppm").is_file()]
        if missing:
            raise SystemExit(f"no snapshot of {', '.join(missing)}")
        palette = background_palette(args.tool, args.game_dir.resolve(), workdir)
        highlighted = {stage: check_briefing(workdir / f"play-{stage}.ppm", palette)
                       for stage in BRIEFINGS}
        if not highlighted["briefing-0"]:
            raise SystemExit("AC01's briefing shows no highlight in ARM's yellow "
                             f"{YELLOW_COLOUR}")
        print(f"native campaign advance: AC01 won at tick {outcome.group(2)}, then "
              f"{advance.group(4)} played to tick {TICKS}; both briefings in their "
              f"background's palette and ARM's colours")


if __name__ == "__main__":
    main()
