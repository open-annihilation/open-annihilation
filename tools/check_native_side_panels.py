#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that each side draws the panels its intgaf names and the font it names.

A side's HUD panels (PANELTOP, PANELSIDE and PANELBOT) come from the GAF in
anims/ that its SIDEDATA intgaf names, and its resource numbers and unit panel
are drawn in the font in fonts/ that its font key names; a side without a font
key draws them in COMIX, and places its right-aligned and centred labels as
though they had no width. A scratch mod folder over the installation holds:

- SIDEBINT.GAF, the installation's CORINT.GAF with every pixel of PANELTOP
  drawn in one palette colour, which SIDE1 (CORE) names as its intgaf;
- SIDECFNT.FNT, a copy of the installation's HATT14.FNT;
- SIDE2, SIDE3 and SIDE4, SIDE0's section (ARM) under other names: SIDE2 with
  font=SIDECFNT, SIDE3 without a font key and SIDE4 with font=COMIX.

Over it, the local player plays a headless skirmish on each of the five
sides, against side 1 (side 0 when it plays side 1 itself), and the
snapshots of the HUD at 640x480 show:

- on SIDE0 the top and bottom bars match those of the same skirmish without
  the mod folder;
- on SIDE1 the top bar is drawn in SIDEBINT's colour, not in CORINT's;
- on SIDE2 the bars keep the ARM art, but the numbers drawn on them change;
- on SIDE3 the numbers are those of SIDE4, in COMIX, but the storage numbers
  start at their place instead of ending there.

A second mod folder, whose SIDE1 names an intgaf and a font the game data
lacks, starts and plays a skirmish on SIDE1, whose bars are drawn as those of
a fourth mod folder, whose SIDE1 names neither.

A third holds SIDEDATA, naming SIDEBINT for SIDE1, and SIDEBINT.GAF only in
gamedata-German and anims-German: a skirmish on SIDE1 in German draws
SIDEBINT's top bar, and one in English the installation's CORINT.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
PANEL_GAF = "SIDEBINT"
FONT = "SIDECFNT"
FONT_SOURCE = "fonts/HATT14.FNT"
MISSING_GAF = "NOSUCHINT"
MISSING_FONT = "NOSUCHFNT"
# The language whose folders alone hold the third mod folder's files, and
# the settings' tags for it and for English.
LANGUAGE = "German"
LANGUAGE_TAG = "de"
ENGLISH_TAG = "en"
# The palette entry SIDEBINT's PANELTOP is drawn in: pure magenta, which
# neither side's own panels use.
PANEL_COLOUR = 253
# The heads of the sections SIDE0's keys and sections follow.
COPIED_SIDES = (f"[SIDE2]\n{{\nname=THIRD;\nfont={FONT};\n",
                "[SIDE3]\n{\nname=FOURTH;\n",
                "[SIDE4]\n{\nname=FIFTH;\nfont=COMIX;\n")
# The HUD's top and bottom bars right of the side column, at 640x480: x1, y1,
# x2, y2.
TOP_BAR = (128, 0, 639, 31)
BOTTOM_BAR = (128, 448, 639, 479)
# SIDE0's METALMAX and ENERGYMAX points, where its storage numbers end, at
# 640x480, each with the band of the top bar around it that holds that
# number, wherever it is placed, and no other label drawn right-aligned or
# centred: x1, y1, x2, y2.
STORAGE_NUMBERS = ((341, (300, 0, 375, 15)), (595, (550, 0, 639, 15)))
# The share of the top bar SIDEBINT's colour covers at least: the resource
# bars and the numbers are drawn over it.
RECOLOURED = 0.6
# The share it covers at most in a top bar drawn from CORINT.
NOT_RECOLOURED = 0.01
# The share of the bars' pixels a snapshot shares with the installation's ARM
# HUD at least when it draws the same art.
SAME_ART = 0.9
SEED = 4242
TICKS = 30
RUN_TIMEOUT = 600
# GAF layout: the file header, an entry's header, a frame's header.
GAF_HEADER = struct.Struct("<III")
GAF_ENTRY = struct.Struct("<HHI32s")
GAF_FRAME = struct.Struct("<HHhhBBHIII")
GAF_TRANSPARENT = 0x01
GAF_REPEAT = 0x02


class PanelFailure(Exception):
    """A behaviour that differs from the expected one."""


def extract(oa_tool, game_dir, archives, entry, output):
    """Writes an installation file, read in the game's mount order, to `output`."""
    result = subprocess.run([*RUNNER, str(oa_tool), "asset-extract", str(game_dir), entry,
                             str(output), *map(str, archives)],
                            check=False, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        raise PanelFailure(f"cannot read {entry}: {result.stdout}{result.stderr}")
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


def recolour_entry(gaf, name, colour):
    """The GAF bytes with every drawn pixel of entry `name`'s frames set to `colour`."""
    data = bytearray(gaf)
    _, entries, _ = GAF_HEADER.unpack_from(data, 0)
    for index in range(entries):
        entry_at = struct.unpack_from("<I", data, GAF_HEADER.size + 4 * index)[0]
        frames, _, _, raw_name = GAF_ENTRY.unpack_from(data, entry_at)
        if raw_name.split(b"\0")[0].decode("latin-1").upper() != name:
            continue
        for frame in range(frames):
            frame_at = struct.unpack_from("<I", data, entry_at + GAF_ENTRY.size + 8 * frame)[0]
            width, height, _, _, _, compressed, subframes, _, pixels, _ = \
                GAF_FRAME.unpack_from(data, frame_at)
            if subframes != 0:
                raise PanelFailure(f"{name} has frames of several parts")
            if not compressed:
                data[pixels:pixels + width * height] = bytes([colour]) * (width * height)
                continue
            at = pixels
            for _ in range(height):
                length = struct.unpack_from("<H", data, at)[0]
                at += 2
                end = at + length
                while at < end:
                    command = data[at]
                    at += 1
                    if command & GAF_TRANSPARENT:
                        continue
                    count = 1 if command & GAF_REPEAT else (command >> 2) + 1
                    data[at:at + count] = bytes([colour]) * count
                    at += count
        return bytes(data)
    raise PanelFailure(f"the GAF has no {name}")


def section(text, name):
    """The start and end of the top-level section `name` of a TDF text."""
    header = re.search(rf"\[\s*{name}\s*\]", text, flags=re.I)
    if header is None:
        raise PanelFailure(f"SIDEDATA has no [{name}]")
    depth = 0
    for index in range(text.index("{", header.end()), len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return header.start(), index + 1
    raise PanelFailure(f"SIDEDATA's [{name}] is not closed")


def name_side1(sidedata, key, value):
    """SIDEDATA with SIDE1's `key` set to `value`, or dropped when it is None."""
    start, end = section(sidedata, "SIDE1")
    replacement = "" if value is None else rf"\g<1>{value};"
    side, count = re.subn(rf"(\b{key}\s*=\s*)[^;]*;", replacement, sidedata[start:end],
                          count=1, flags=re.I)
    if count != 1:
        raise PanelFailure(f"SIDEDATA's SIDE1 names no {key}")
    return sidedata[:start] + side + sidedata[end:]


def name_panels(sidedata, gaf):
    """SIDEDATA with SIDE1's intgaf set to `gaf`."""
    return name_side1(sidedata, "intgaf", gaf)


def make_mods(oa_tool, game_dir, mod_dir, missing_dir, language_dir, unnamed_dir, scratch):
    """Writes the four mod folders; returns the installation's palette as RGB triples."""
    archives = mount_order(game_dir)
    for folder in (mod_dir / "gamedata", mod_dir / "anims", mod_dir / "fonts",
                   missing_dir / "gamedata", language_dir / f"gamedata-{LANGUAGE}",
                   language_dir / f"anims-{LANGUAGE}", unnamed_dir / "gamedata"):
        folder.mkdir(parents=True)
    corint = extract(oa_tool, game_dir, archives, "anims/CORINT.GAF", scratch / "CORINT.GAF")
    recoloured = recolour_entry(corint, "PANELTOP", PANEL_COLOUR)
    (mod_dir / "anims" / f"{PANEL_GAF}.GAF").write_bytes(recoloured)
    (language_dir / f"anims-{LANGUAGE}" / f"{PANEL_GAF}.GAF").write_bytes(recoloured)
    (mod_dir / "fonts" / f"{FONT}.FNT").write_bytes(
        extract(oa_tool, game_dir, archives, FONT_SOURCE, scratch / "font.fnt"))
    sidedata = extract(oa_tool, game_dir, archives, "gamedata/sidedata.tdf",
                       scratch / "sidedata.tdf").decode("latin-1")
    (missing_dir / "gamedata" / "SIDEDATA.TDF").write_text(
        name_side1(name_panels(sidedata, MISSING_GAF), "font", MISSING_FONT),
        encoding="latin-1", newline="")
    (unnamed_dir / "gamedata" / "SIDEDATA.TDF").write_text(
        name_side1(name_side1(sidedata, "intgaf", None), "font", None),
        encoding="latin-1", newline="")
    (language_dir / f"gamedata-{LANGUAGE}" / "SIDEDATA.TDF").write_text(
        name_panels(sidedata, PANEL_GAF), encoding="latin-1", newline="")
    # SIDE2 to SIDE4 are SIDE0's section under other names, each with its
    # own font key or none.
    start, end = section(sidedata, "SIDE0")
    body = sidedata[start:end]
    body = body[body.index("{") + 1:]
    body = re.sub(r"\b(name|font)\s*=\s*[^;]*;", "", body, count=2, flags=re.I)
    sidedata = name_panels(sidedata, PANEL_GAF).rstrip() + "\n" + "".join(
        head + body + "\n" for head in COPIED_SIDES)
    (mod_dir / "gamedata" / "SIDEDATA.TDF").write_text(sidedata, encoding="latin-1", newline="")
    palette = extract(oa_tool, game_dir, archives, "palettes/palette.pal", scratch / "palette.pal")
    return [tuple(palette[index * 4:index * 4 + 3]) for index in range(256)]


def run(native, game_dir, workdir, name, arguments, mod_dir=None):
    """Runs the game headless; returns its exit status and output."""
    command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
               "--headless-check", "--preferences-file", str(workdir / f"{name}.conf"),
               *arguments]
    if mod_dir is not None:
        command += ["--mod-dir", str(mod_dir)]
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    result = subprocess.run(command, cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False,
                            capture_output=True, text=True, errors="replace")
    return result.returncode, result.stdout + result.stderr


def skirmish(native, game_dir, workdir, name, local_side, mod_dir=None, language=None):
    """Plays a headless skirmish on `local_side`, in the language a settings tag
    chooses when one is given; returns its snapshot and output."""
    enemy_side = 0 if local_side == 1 else 1
    chosen = f"\"open-annihilation.language\" \"{language}\"\n" if language else ""
    (workdir / f"{name}.conf").write_text(
        "open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n"
        f"\"Total Annihilation\\\\Skirmish|Player0Side\" \"{local_side}\"\n"
        f"\"Total Annihilation\\\\Skirmish|Player1Side\" \"{enemy_side}\"\n" + chosen)
    snapshot = workdir / f"{name}.ppm"
    status, output = run(native, game_dir, workdir, name,
                         ["--seed", str(SEED), "--match-ticks", str(TICKS),
                          "--snapshot", str(snapshot)], mod_dir)
    if status != 0 or any(line.startswith("simulation error") for line in output.splitlines()):
        print(output, end="")
        raise PanelFailure(f"the {name} run exited with {status} or reported a simulation error")
    return read_ppm(snapshot), output


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
        raise PanelFailure(f"{path.name} is not a binary PPM")
    return int(fields[1]), int(fields[2]), data[at + 1:]


def pixels(image, area):
    """The RGB triples of an area of a PPM `image`, row by row."""
    width, _, rgb = image
    x1, y1, x2, y2 = area
    return [tuple(rgb[(y * width + x) * 3:(y * width + x) * 3 + 3])
            for y in range(y1, y2 + 1) for x in range(x1, x2 + 1)]


def changed_sides(image, reference, area, x):
    """Whether pixels of `area` that two snapshots draw differently lie left of
    column `x`, and whether any lie right of it."""
    x1, y1, x2, _ = area
    width = x2 - x1 + 1
    left = right = False
    for index, (drawn, expected) in enumerate(zip(pixels(image, area), pixels(reference, area))):
        if drawn != expected:
            column = x1 + index % width
            left |= column < x
            right |= column > x
    return left, right


def outside(image, reference, bands):
    """The number of pixels of the bars that two snapshots draw differently
    outside the `bands`."""
    count = 0
    for x1, y1, x2, y2 in (TOP_BAR, BOTTOM_BAR):
        width = x2 - x1 + 1
        for index, (drawn, expected) in enumerate(zip(pixels(image, (x1, y1, x2, y2)),
                                                      pixels(reference, (x1, y1, x2, y2)))):
            x = x1 + index % width
            y = y1 + index // width
            if drawn != expected and not any(bx1 <= x <= bx2 and by1 <= y <= by2
                                             for bx1, by1, bx2, by2 in bands):
                count += 1
    return count


def shared(image, reference):
    """The share of the bars' pixels two snapshots have alike."""
    same = total = 0
    for area in (TOP_BAR, BOTTOM_BAR):
        for drawn, expected in zip(pixels(image, area), pixels(reference, area)):
            total += 1
            same += drawn == expected
    return same / total


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--oa-tool", type=Path, required=True, help="oa-tool")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-side-panels-",
                                     dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        mod_dir = workdir / "mod"
        missing_dir = workdir / "missing"
        language_dir = workdir / "language"
        unnamed_dir = workdir / "unnamed"
        try:
            palette = make_mods(args.oa_tool.resolve(), game_dir, mod_dir, missing_dir,
                                language_dir, unnamed_dir, workdir)
            installation, _ = skirmish(native, game_dir, workdir, "installation", 0)
            if installation[:2] != (640, 480):
                raise PanelFailure(f"the snapshot is {installation[0]}x{installation[1]}, "
                                   "not 640x480")
            image, _ = skirmish(native, game_dir, workdir, "side-0", 0, mod_dir)
            arm = shared(image, installation)
            if arm < 1:
                raise PanelFailure(f"SIDE0's bars share only {arm:.3f} of their pixels with the "
                                   "installation's ARM HUD")
            image, _ = skirmish(native, game_dir, workdir, "side-1", 1, mod_dir)
            top = pixels(image, TOP_BAR)
            recoloured = top.count(palette[PANEL_COLOUR]) / len(top)
            if recoloured < RECOLOURED:
                raise PanelFailure(f"{PANEL_GAF}'s colour covers {recoloured:.3f} of SIDE1's top "
                                   f"bar, less than {RECOLOURED}")
            image, _ = skirmish(native, game_dir, workdir, "side-2", 2, mod_dir)
            third = shared(image, installation)
            if not SAME_ART <= third < 1:
                raise PanelFailure(f"SIDE2's bars share {third:.3f} of their pixels with the "
                                   f"installation's ARM HUD: not the ARM art with {FONT}'s "
                                   "numbers")
            unnamed, _ = skirmish(native, game_dir, workdir, "side-3", 3, mod_dir)
            comix, _ = skirmish(native, game_dir, workdir, "side-4", 4, mod_dir)
            unnamed_art = shared(unnamed, installation)
            if not SAME_ART <= unnamed_art < 1:
                raise PanelFailure(f"SIDE3's bars share {unnamed_art:.3f} of their pixels with "
                                   "the installation's ARM HUD: not the ARM art with other "
                                   "numbers")
            # Both are drawn in COMIX: each storage number ends at its place
            # on SIDE4 and starts there on SIDE3, which names no font, and
            # nothing else differs.
            for x, band in STORAGE_NUMBERS:
                if changed_sides(unnamed, comix, band, x) != (True, True):
                    raise PanelFailure("SIDE3's storage number is not moved from ending at "
                                       f"column {x}, where SIDE4 draws it in COMIX, to "
                                       "starting there")
            elsewhere = outside(unnamed, comix, [band for _, band in STORAGE_NUMBERS])
            if elsewhere:
                raise PanelFailure(f"SIDE3's bars differ from SIDE4's in COMIX in {elsewhere} "
                                   "pixels besides their storage numbers")
            # An intgaf and a font the game data lacks: SIDE1 plays, drawn as a
            # side that names neither.
            missing, _ = skirmish(native, game_dir, workdir, "missing", 1, missing_dir)
            unnamed_side, _ = skirmish(native, game_dir, workdir, "unnamed", 1, unnamed_dir)
            missing_share = shared(missing, unnamed_side)
            if missing_share < 1:
                raise PanelFailure(f"SIDE1's bars without its missing intgaf and font share "
                                   f"only {missing_share:.3f} of their pixels with those of "
                                   "a SIDE1 that names neither")
            # SIDEDATA and SIDEBINT in the language's folders alone.
            for tag, name, wanted in ((LANGUAGE_TAG, LANGUAGE, True),
                                      (ENGLISH_TAG, "English", False)):
                image, _ = skirmish(native, game_dir, workdir, f"language-{name}", 1,
                                    language_dir, tag)
                top = pixels(image, TOP_BAR)
                share = top.count(palette[PANEL_COLOUR]) / len(top)
                if share < RECOLOURED if wanted else share > NOT_RECOLOURED:
                    raise PanelFailure(f"{PANEL_GAF}'s colour covers {share:.3f} of SIDE1's top "
                                       f"bar in {name}, whose folders "
                                       f"{'hold' if wanted else 'do not hold'} the SIDEDATA "
                                       "naming it")
                if wanted:
                    language_share = share
        except PanelFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print(f"the side panels check passed: SIDE0's bars match the installation's, {PANEL_GAF}'s "
          f"colour covers {recoloured:.3f} of SIDE1's top bar, SIDE2's bars share {third:.3f} "
          f"with SIDE0's in {FONT}, SIDE3's {unnamed_art:.3f} in COMIX with its storage numbers "
          f"unmeasured, a missing intgaf and font are drawn as none named, and {LANGUAGE}'s "
          f"folders' SIDEDATA and {PANEL_GAF} cover {language_share:.3f} of SIDE1's top bar in "
          f"{LANGUAGE} alone")
    return 0


if __name__ == "__main__":
    sys.exit(main())
