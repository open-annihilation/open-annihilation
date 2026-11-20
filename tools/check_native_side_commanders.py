#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check sides that name commanders and HUD art of their own.

Each player's commander is the type its side names in SIDEDATA, whatever its
name and whatever its unit file says. A side's HUD panels come from the GAF
its SIDEDATA intgaf names, and its resource bars from its own SIDEn section.

A scratch mod folder over the installation gives SIDE0 (ARM) the commander
CAPTAIN and SIDE1 (CORE) the commander MARSHAL: copies of the installation's
commanders under those names, without the Commander and ShowPlayerName keys
or the commander category, each with its build pages and SIDEDATA build
list. Each side's bars take colours of their own. A third side, SIDE2, is
ARM's section under another name, with CORE's panels (intgaf=CORINT) and
bar colours of its own. Over it:

- A headless skirmish of the local player on each of the three sides draws
  its side's panels: the top and bottom bars match the installation's own
  ARM HUD on SIDE0 and its CORE HUD on SIDE1 and SIDE2, outside the resource
  bars, which are filled in the colours of the player's own section.
- With the commander rule "game ends", the stage gives the computer player
  three more units and then kills its commander through the console. The
  commander's death takes every unit of its player with it, and the local
  player wins. With the rule "game continues", the same stage leaves the
  three units standing, and nobody wins. Left alone, the computer player
  builds with its commander.
- The network loopback check with a computer player finds each player's
  commander on both machines.
- In the network loopback check without one, the host's player gives every
  unit it has, its commander among them, through SHARE.GUI. Only a type in
  the Commander category stays with the giver, so CAPTAIN goes with the
  rest; the joiner's player gives it back, and both machines agree.
- The navigation check, whose player records check reads each side's
  commander, passes.

Each skirmish must start with one commander for each player, of its side's
type, and no other, end with status 0 and report no simulation error.

--part runs one of the checks above, each over a mod folder of its own: art
(the HUDs), rule (the commander rule and the computer player's builds),
loopback, give and records (the navigation check). --navigation-group runs
the records part's navigation check as that group alone (--check-navigation
GROUP). Without --part every check runs, over one mod folder.
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
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# The installation's commanders and the names the mod folder gives them.
COMMANDERS = {"ARMCOM": "CAPTAIN", "CORCOM": "MARSHAL"}
# The build pages each installation commander has.
BUILD_PAGES = 4
# Keys the copies leave out, in lower case.
DROPPED_KEYS = ("commander", "showplayername")
# Each mod side's commander, its bar colours (energy, metal), and the
# installation's side whose panels it draws: 0 ARM, 1 CORE.
SIDES = (
    {"commander": "CAPTAIN", "energy": 208, "metal": 224, "panels": 0},
    {"commander": "MARSHAL", "energy": 201, "metal": 232, "panels": 1},
    {"commander": "CAPTAIN", "energy": 32, "metal": 144, "panels": 1},
)
# The head of SIDE2's section; SIDE0's keys and sections follow it.
THIRD_SIDE = "[SIDE2]\n{\nname=THIRD;\nintgaf=CORINT;\n"
# The resource bars, as the installation's SIDEDATA places them: x1, y1, x2, y2.
BARS = {"energy": (471, 12, 598, 14), "metal": (218, 12, 345, 14)}
# The HUD's top and bottom bars, right of the side column: x1, y1, x2, y2.
CHROME = ((128, 0, 639, 31), (128, 448, 639, 479))
# The share of chrome pixels a HUD shares with the side's own installation
# HUD at least, and with the other side's at most.
SAME_ART = 0.97
OTHER_ART = 0.6
ART_TICKS = 30
# The extra units the stage gives the computer player, by type.
EXTRA_TYPE = "CORAK"
EXTRA_COUNT = 3
KILL_TICK = 90
STAGE = (
    "".join(f"unit 1 {EXTRA_TYPE} {offset} 160\n" for offset in (-60, 0, 60))
    + f"at {KILL_TICK} console +Now Film Chris Include Reload Assert\n"
    + f"at {KILL_TICK} console +Reload {COMMANDERS['CORCOM']}\n"
)
STAGE_TICKS = 450
BUILD_TICKS = 1800
LOOPBACK_TICKS = 6000
GIVE_TICKS = 300
SEED = 4242
RULE_CONTINUES = 0
RULE_ENDS = 1
OUTCOME = re.compile(r"^saveload: outcome (victory|defeat) at tick (\d+)$", re.M)
TYPES = re.compile(r"^saveload: unit types \d+((?: \S+=\d+)*)$", re.M)
PLAYER_CHECK = re.compile(r"^skirmish player check: Game\.sides (\S+?),", re.M)
DIGESTS = re.compile(r"digests ([0-9a-f]{16}) / ([0-9a-f]{16})")
COMMANDER_GIVEN = "its commander went too, then came back from the joiner"
# Wall-time limit per game run. OA_TEST_TIMEOUT_SCALE, a positive number,
# multiplies it for a build whose run-time error checks make the game several
# times slower.
RUN_TIMEOUT = 900 * float(os.environ.get("OA_TEST_TIMEOUT_SCALE") or 1)
# The checks --part runs one of, in the order a run of all of them takes.
PARTS = ("art", "rule", "loopback", "give", "records")
# The navigation check's groups (--check-navigation GROUP), and the one whose
# skirmish runs the player records check.
NAVIGATION_GROUPS = ("screens", "orders", "outcomes", "zoom", "zoom-1366x768", "zoom-1920x1080",
                     "zoom-2560x1440", "campaign")
RECORDS_GROUP = "outcomes"


class CommanderFailure(Exception):
    """A behaviour that differs from the expected one."""


def extract(oa_tool, game_dir, archives, entry, output):
    """Writes an installation file, read in the game's mount order, to `output`."""
    result = subprocess.run([*RUNNER, str(oa_tool), "asset-extract", str(game_dir), entry,
                             str(output), *map(str, archives)],
                            check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        raise CommanderFailure(f"cannot read {entry}: {result.stdout}{result.stderr}")
    return output.read_bytes()


def mount_order(game_dir):
    """The installation's archives, the first to mount first."""
    files = sorted((path for path in game_dir.iterdir() if path.is_file()),
                   key=lambda path: path.name.lower())
    groups = ([path for path in files if path.name.lower() == "rev31.gp3"],
              [path for path in files if path.suffix.lower() == ".ccx"],
              [path for path in files if path.suffix.lower() == ".ufo"],
              [path for path in files if path.suffix.lower() == ".hpi"])
    return [path for group in groups for path in group]


def rename_unit(text, old, new):
    """The unit file `text` under the name `new`, without the commander keys."""
    lines = []
    for line in text.splitlines(keepends=True):
        key = line.split("=", 1)[0].strip().lower()
        if key in DROPPED_KEYS:
            continue
        if key == "unitname":
            line = re.sub(re.escape(old), new, line, flags=re.I)
        elif key == "category":
            line = re.sub(r"\bcommander\b ?", "", line, flags=re.I)
        lines.append(line)
    return "".join(lines)


def section(text, name):
    """The start and end of the top-level section `name` of a TDF text."""
    header = re.search(rf"\[\s*{name}\s*\]", text, flags=re.I)
    if header is None:
        raise CommanderFailure(f"SIDEDATA has no [{name}]")
    depth = 0
    for index in range(text.index("{", header.end()), len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return header.start(), index + 1
    raise CommanderFailure(f"SIDEDATA's [{name}] is not closed")


def set_key(text, key, value):
    """The section `text` with its first `key` set to `value`."""
    text, count = re.subn(rf"(\b{key}\s*=\s*)[^;]*;", rf"\g<1>{value};", text, count=1,
                          flags=re.I)
    if count != 1:
        raise CommanderFailure(f"a SIDEDATA side has no {key}")
    return text


def make_mod(oa_tool, game_dir, mod_dir, scratch):
    """Writes the mod folder: SIDEDATA, unit files and scripts for the new commanders.

    Returns the installation's palette as a list of RGB triples.
    """
    archives = mount_order(game_dir)
    for directory in ("gamedata", "units", "scripts", "guis", "anims"):
        (mod_dir / directory).mkdir(parents=True)
    sidedata = extract(oa_tool, game_dir, archives, "gamedata/sidedata.tdf",
                       scratch / "sidedata.tdf").decode("latin-1")
    for old, new in COMMANDERS.items():
        # The side's commander and its build list's section.
        sidedata, named = re.subn(rf"(commander\s*=\s*){old}\b", rf"\g<1>{new}", sidedata,
                                  flags=re.I)
        sidedata, listed = re.subn(rf"\[\s*{old}\s*\]", f"[{new}]", sidedata, flags=re.I)
        if named != 1 or listed != 1:
            raise CommanderFailure(f"SIDEDATA names {old} {named} times and lists it {listed} times")
        unit = extract(oa_tool, game_dir, archives, f"units/{old}.fbi",
                       scratch / f"{old}.fbi").decode("latin-1")
        (mod_dir / "units" / f"{new}.FBI").write_text(rename_unit(unit, old, new),
                                                      encoding="latin-1", newline="")
        script = extract(oa_tool, game_dir, archives, f"scripts/{old}.cob", scratch / f"{old}.cob")
        (mod_dir / "scripts" / f"{new}.COB").write_bytes(script)
        # The commander's build pages and their art.
        for page in range(1, BUILD_PAGES + 1):
            for directory, extension in (("guis", "gui"), ("anims", "gaf")):
                page_file = extract(oa_tool, game_dir, archives,
                                    f"{directory}/{old}{page}.{extension}",
                                    scratch / f"{old}{page}.{extension}")
                (mod_dir / directory / f"{new}{page}.{extension.upper()}").write_bytes(page_file)
    # SIDE2 is SIDE0's section under another name, with CORE's panels.
    start, end = section(sidedata, "SIDE0")
    body = sidedata[start:end]
    body = body[body.index("{") + 1:]
    body = re.sub(r"\b(name|intgaf)\s*=\s*[^;]*;", "", body, count=2, flags=re.I)
    sidedata = sidedata.rstrip() + "\n" + THIRD_SIDE + body + "\n"
    for index, side in enumerate(SIDES):
        start, end = section(sidedata, f"SIDE{index}")
        text = set_key(sidedata[start:end], "energycolor", side["energy"])
        text = set_key(text, "metalcolor", side["metal"])
        sidedata = sidedata[:start] + text + sidedata[end:]
    (mod_dir / "gamedata" / "SIDEDATA.TDF").write_text(sidedata, encoding="latin-1", newline="")
    palette = extract(oa_tool, game_dir, archives, "palettes/palette.pal", scratch / "palette.pal")
    return [tuple(palette[index * 4:index * 4 + 3]) for index in range(256)]


def preferences(workdir, name, rule, local_side, enemy_side):
    """A preferences file for a skirmish under `rule` between the two sides."""
    path = workdir / f"{name}.conf"
    path.write_text("open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n"
                    f"\"Total Annihilation|SkirmishCommanderDeath\" \"{rule}\"\n"
                    f"\"Total Annihilation\\\\Skirmish|Player0Side\" \"{local_side}\"\n"
                    f"\"Total Annihilation\\\\Skirmish|Player1Side\" \"{enemy_side}\"\n")
    return path


def run(native, game_dir, workdir, name, arguments, mod_dir=None):
    """Runs the game headless; returns its output, which must report no failure."""
    command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
               "--headless-check", *arguments]
    if mod_dir is not None:
        command += ["--mod-dir", str(mod_dir)]
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    result = subprocess.run(command, cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False,
                            capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise CommanderFailure(f"open-annihilation exited with {result.returncode} on the {name} run")
    if any(line.startswith("simulation error") for line in output.splitlines()):
        print(output, end="")
        raise CommanderFailure(f"the {name} run reported a simulation error")
    return output


def skirmish(native, game_dir, mod_dir, workdir, name, rule, ticks, stage=None, local_side=0,
             enemy_side=1, snapshot=None):
    """Plays the headless skirmish; returns its output and the saved unit counts by type."""
    arguments = ["--seed", str(SEED), "--match-ticks", str(ticks), "--save-after", str(ticks),
                 "--save-file", str(workdir / f"{name}.sav"),
                 "--preferences-file",
                 str(preferences(workdir, name, rule, local_side, enemy_side))]
    if stage is not None:
        stage_file = workdir / f"{name}.stage"
        stage_file.write_text(stage)
        arguments += ["--stage", str(stage_file)]
    if snapshot is not None:
        arguments += ["--snapshot", str(snapshot)]
    output = run(native, game_dir, workdir, name, arguments, mod_dir)
    types = TYPES.findall(output)
    if len(types) != 1:
        print(output, end="")
        raise CommanderFailure(f"the {name} run saved {len(types)} unit lists instead of one")
    counts = {key: int(value) for key, value in
              (pair.split("=") for pair in types[0].split())}
    return output, counts


def require_commanders(counts, name, expected):
    """The expected counts of the new commanders, and no installation commander."""
    for old in COMMANDERS:
        if counts.get(old, 0) != 0:
            raise CommanderFailure(f"the {name} run has {counts[old]} {old}")
    for new in COMMANDERS.values():
        if counts.get(new, 0) != expected.get(new, 0):
            raise CommanderFailure(f"the {name} run ends with {counts.get(new, 0)} {new}, "
                                   f"not {expected.get(new, 0)}")


def read_ppm(path):
    """The width, height and RGB bytes of a binary PPM."""
    data = path.read_bytes()
    fields = []
    at = 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    if fields[0] != b"P6" or fields[3] != b"255":
        raise CommanderFailure(f"{path.name} is not a binary PPM")
    return int(fields[1]), int(fields[2]), data[at + 1:]


def pixel(image, x, y):
    """The RGB triple at a point of a PPM `image`."""
    width, _, rgb = image
    at = (y * width + x) * 3
    return tuple(rgb[at:at + 3])


def chrome_share(image, reference):
    """The share of the chrome's pixels outside the bars that two snapshots share."""
    same = total = 0
    for x1, y1, x2, y2 in CHROME:
        for y in range(y1, y2 + 1):
            for x in range(x1, x2 + 1):
                if any(bx1 <= x <= bx2 and by1 <= y <= by2
                       for bx1, by1, bx2, by2 in BARS.values()):
                    continue
                total += 1
                same += pixel(image, x, y) == pixel(reference, x, y)
    return same / total


def check_art(native, game_dir, mod_dir, workdir, palette):
    """Each side's HUD draws its own panels and bars; returns the shares measured."""
    references = []
    for side in (0, 1):
        snapshot = workdir / f"installation-{side}.ppm"
        skirmish(native, game_dir, None, workdir, f"installation-{side}", RULE_ENDS, ART_TICKS,
                 local_side=side, enemy_side=1 - side, snapshot=snapshot)
        references.append(read_ppm(snapshot))
    shares = []
    for index, side in enumerate(SIDES):
        name = f"side-{index}"
        snapshot = workdir / f"{name}.ppm"
        enemy = 0 if index == 1 else 1
        _, counts = skirmish(native, game_dir, mod_dir, workdir, name, RULE_ENDS, ART_TICKS,
                             local_side=index, enemy_side=enemy, snapshot=snapshot)
        expected = {}
        for commander in (side["commander"], SIDES[enemy]["commander"]):
            expected[commander] = expected.get(commander, 0) + 1
        require_commanders(counts, name, expected)
        image = read_ppm(snapshot)
        own = chrome_share(image, references[side["panels"]])
        other = chrome_share(image, references[1 - side["panels"]])
        shares.append(f"SIDE{index} {own:.3f}/{other:.3f}")
        if own < SAME_ART or other > OTHER_ART:
            raise CommanderFailure(f"SIDE{index}'s HUD shares {own:.3f} of its chrome with the "
                                   f"installation's {('ARM', 'CORE')[side['panels']]} HUD and "
                                   f"{other:.3f} with the other")
        for bar, (x1, y1, _, _) in BARS.items():
            drawn = pixel(image, x1, y1)
            if drawn != palette[side[bar]]:
                raise CommanderFailure(f"SIDE{index}'s {bar} bar is drawn in {drawn}, not in its "
                                       f"own colour {side[bar]} {palette[side[bar]]}")
    return shares


def check_commander_rule(native, game_dir, mod_dir, workdir):
    """The death of the computer player's commander under each rule; returns what it built."""
    output, counts = skirmish(native, game_dir, mod_dir, workdir, "ends", RULE_ENDS,
                              STAGE_TICKS, STAGE)
    require_commanders(counts, "ends", {COMMANDERS["ARMCOM"]: 1})
    outcome = OUTCOME.findall(output)
    if not outcome or outcome[0][0] != "victory" or int(outcome[0][1]) <= KILL_TICK:
        print(output, end="")
        raise CommanderFailure(f"the commander's death ended the game in {outcome}, "
                               "not in a victory")
    if counts.get(EXTRA_TYPE, 0) != 0:
        raise CommanderFailure(f"{counts[EXTRA_TYPE]} {EXTRA_TYPE} outlived their commander")
    output, counts = skirmish(native, game_dir, mod_dir, workdir, "continues", RULE_CONTINUES,
                              STAGE_TICKS, STAGE)
    require_commanders(counts, "continues", {COMMANDERS["ARMCOM"]: 1})
    if OUTCOME.search(output):
        print(output, end="")
        raise CommanderFailure("a game that continues after its commander ended")
    if counts.get(EXTRA_TYPE, 0) != EXTRA_COUNT:
        raise CommanderFailure(f"{counts.get(EXTRA_TYPE, 0)} {EXTRA_TYPE} stand, "
                               f"not {EXTRA_COUNT}")
    _, counts = skirmish(native, game_dir, mod_dir, workdir, "builds", RULE_ENDS, BUILD_TICKS)
    require_commanders(counts, "builds", {new: 1 for new in COMMANDERS.values()})
    built = {key: value for key, value in counts.items() if key not in COMMANDERS.values()}
    if not built:
        raise CommanderFailure("the computer player built nothing with its commander")
    return built


def check_give(native, game_dir, mod_dir, workdir):
    """A network game in which the host's player gives every unit, its commander too."""
    output = run(native, game_dir, workdir, "network give",
                 ["--net-loopback-check", str(GIVE_TICKS),
                  "--preferences-file", str(workdir / "give.conf")], mod_dir)
    digests = DIGESTS.search(output)
    if digests is None or digests.group(1) != digests.group(2):
        print(output, end="")
        raise CommanderFailure("the machines of the network give ended with digests "
                               f"{digests.groups() if digests else None}")
    if COMMANDER_GIVEN not in output:
        print(output, end="")
        raise CommanderFailure("the host's commander did not go with every unit it gave")


def check_records(native, game_dir, mod_dir, workdir, group):
    """The navigation check, or its group `group`, passes and reads each side's commander."""
    navigation = ["--check-navigation", *([group] if group else [])]
    output = run(native, game_dir, workdir, "navigation",
                 [*navigation, "--preferences-file", str(workdir / "navigation.conf")], mod_dir)
    records = PLAYER_CHECK.findall(output)
    wanted = ["/".join(COMMANDERS.values())] if group in (None, RECORDS_GROUP) else []
    if records != wanted:
        print(output, end="")
        raise CommanderFailure(f"the player records check read Game.sides {records}, "
                               f"not {wanted[0] if wanted else 'nothing'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--oa-tool", type=Path, required=True, help="oa-tool")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--part", choices=PARTS, help="the check to run; every check without")
    parser.add_argument("--navigation-group", choices=NAVIGATION_GROUPS,
                        help="the records part's navigation group; the whole check without")
    args = parser.parse_args()
    if args.navigation_group and args.part != "records":
        parser.error("--navigation-group needs --part records")
    parts = [args.part] if args.part else list(PARTS)
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    passed = []
    with tempfile.TemporaryDirectory(prefix="native-side-commanders-",
                                     dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        mod_dir = workdir / "mod"
        try:
            palette = make_mod(args.oa_tool.resolve(), game_dir, mod_dir, workdir)
            if "art" in parts:
                shares = check_art(native, game_dir, mod_dir, workdir, palette)
                passed.append(f"the HUD chrome shares (own/other) {', '.join(shares)}")
            if "rule" in parts:
                built = check_commander_rule(native, game_dir, mod_dir, workdir)
                passed.append("the death of the computer player's commander ends the game under "
                              f"the rule; the computer player built {built}")
            if "loopback" in parts:
                run(native, game_dir, workdir, "network loopback",
                    ["--net-loopback-check", str(LOOPBACK_TICKS), "--net-loopback-computer",
                     "--preferences-file", str(workdir / "loopback.conf")], mod_dir)
                passed.append("the network loopback check found the commanders")
            if "give" in parts:
                check_give(native, game_dir, mod_dir, workdir)
                passed.append("a network game gives CAPTAIN away with the other units")
            if "records" in parts:
                check_records(native, game_dir, mod_dir, workdir, args.navigation_group)
                group = f" (navigation group {args.navigation_group})" if args.navigation_group else ""
                passed.append(f"the player records check found the commanders{group}")
        except CommanderFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print(f"the side commanders check passed: {'; '.join(passed)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
