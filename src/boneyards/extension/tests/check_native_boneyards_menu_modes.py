#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The way into the Boneyards on a mod's main menu, with made-up mods this
# check writes itself, played with the Boneyards installed as the expansion:
# its files in the user folder's Expansions, and a game folder without them.
# Ridge on the base game's art brings only its own MAINMENU.GUI, whose EXIT
# lies under the strip's art away from the strip's buttons, and a plate
# place in its profile: the strip shows, a click on its art there leaves the
# menu as it is, and the log says the place is for button mode only. Ridge
# with art of its own brings a FrontendX of its own and no MAINMENU.GUI: the
# strip's rows are not shown, the menu keeps the base game's own layout
# (SINGLE at 139,393, not the layout the expansion brings for the strip), the
# credits give back the menu as it was, the plate and all, and
# ENTER on the strip's plate starts the Boneyards entry, which tells this
# command-line mod to start without it (the mod question), the plate at the default
# place at the top right (497,8) without a place in the profile, and at the
# profile's place (40,200) with one. Each run starts from a fresh work
# folder, so a second run finds what the first did.

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path
_OA_TOOLS = Path(__file__).resolve().parents[4] / "tools"
if str(_OA_TOOLS) not in sys.path:
    sys.path.insert(0, str(_OA_TOOLS))
import native_offline  # noqa: E402

# The expansion check's staging, imported without writing bytecode beside it.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_boneyards_expansion as expansion_files  # noqa: E402

PROFILE = """oamod: 1
id: ridge
name: Ridge
version: "1.0"
author: {name: The Open Annihilation Authors}
packaging: {revision: 1, date: 2026-11-05, packager: The Open Annihilation Authors}
"""

PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.language" "en"\n'
    '"open-annihilation.first-run" "done"\n'
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"\n'
)

# The base game's three buttons where its layout has them, Credits, and an
# EXIT moved under the strip's art at the bottom left, clear of the strip's
# own buttons, as a mod's own column of buttons can lie.
GADGETS = (
    ("SINGLE", 139, 393, 96, 20, "SINGLE", ord("s")),
    ("MULTI", 139, 430, 96, 20, "MULTI", ord("m")),
    ("INTRO", 409, 393, 96, 20, "INTRO", ord("i")),
    ("EXIT", 8, 452, 64, 20, "EXIT", ord("e")),
)


def main_menu_layout():
    """A MAINMENU.GUI of the base game's buttons, its EXIT under the strip."""
    parts = [
        "[GADGET0]\n{\n  [COMMON]\n  {\n    id=0;\n    name=MAINMENU;\n    xpos=0;\n"
        "    ypos=0;\n    width=640;\n    height=480;\n    active=1;\n  }\n"
        "  panel=MAINMENU;\n}\n"
    ]
    for number, (name, x, y, width, height, text, key) in enumerate(GADGETS, start=1):
        parts.append(
            f"[GADGET{number}]\n{{\n  [COMMON]\n  {{\n    id=1;\n    name={name};\n"
            f"    xpos={x};\n    ypos={y};\n    width={width};\n    height={height};\n"
            f"    attribs=2;\n    active=1;\n  }}\n  text={text};\n  quickkey={key};\n}}\n"
        )
    credits = len(GADGETS) + 1
    parts.append(
        f"[GADGET{credits}]\n{{\n  [COMMON]\n  {{\n    id=1;\n    name=Credits;\n"
        "    xpos=280;\n    ypos=440;\n    width=80;\n    height=40;\n    active=1;\n  }\n}\n"
    )
    return "".join(parts)


def own_art():
    """A 640x480 FrontendX of the mod's own: bands of colour unlike the base game's art.

    The palette runs through hues; the picture is wide diagonal bands, so
    every row of the strip differs from the base game's menu.
    """
    width, height = 640, 480
    palette = bytearray()
    for index in range(256):
        palette += bytes(((index * 7) % 256, (index * 3 + 64) % 256, (255 - index) % 256))
    header = bytearray(128)
    struct.pack_into(
        "<BBBBHHHHHH", header, 0, 10, 5, 1, 8, 0, 0, width - 1, height - 1, 72, 72
    )
    header[65] = 1  # planes
    struct.pack_into("<HH", header, 66, width, 1)  # bytes a line, colour palette
    pixels = bytearray()
    for row in range(height):
        line = bytes(((column + row) // 16 * 37) % 256 for column in range(width))
        at = 0
        while at < width:
            value = line[at]
            run = 1
            while at + run < width and run < 63 and line[at + run] == value:
                run += 1
            if run > 1 or value >= 0xC0:
                pixels += bytes((0xC0 | run, value))
            else:
                pixels.append(value)
            at += run
    return bytes(header) + bytes(pixels) + b"\x0c" + bytes(palette)


def write_mod(folder, coreprime, files, media=""):
    """Writes one made-up mod: its profile, Core Prime's archives and its own files."""
    folder.mkdir(parents=True)
    (folder / "oamod.yaml").write_text(PROFILE + media, encoding="utf-8")
    for archive in sorted(coreprime.glob("*.ccx")):
        shutil.copyfile(archive, folder / archive.name)
    for name, content in files.items():
        target = folder / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, str):
            target.write_text(content, encoding="utf-8")
        else:
            target.write_bytes(content)


def install_expansion(arguments, names, user, game):
    """Installs the Boneyards as the expansion for one run.

    The bundle's files are linked from the installed game into the user
    folder's Expansions/totala/boneyards with a content record; the game
    folder is the same install without the bundle's root file and folder.
    """
    expansion = user / "Expansions" / "totala" / "boneyards"
    expansion.mkdir(parents=True)
    missing = []
    for name in names:
        found = expansion_files.find_source(arguments.game_dir, name)
        if found is None:
            missing.append(name)
            continue
        expansion_files.place_file(found, str(expansion.joinpath(*name.split("/"))))
    if missing:
        raise SystemExit("the installed game is missing: " + ", ".join(missing))
    expansion_files.place_alpha_map(arguments.game_dir, str(expansion))
    expansion_files.write_record(str(expansion), names, "scratch")
    expansion_files.link_tree(arguments.game_dir, str(game), {"by.ccx", "boneyards"})
    if expansion_files.root_has_bundle(str(game)):
        raise SystemExit("the scratch game folder still holds the bundle")


def play(arguments, names, mod, work, mode, name):
    """Runs the game once on a mod and returns its output, or None when it failed."""
    run = work / ("run-" + name)
    for sub in ("data", "user"):
        (run / sub).mkdir(parents=True)
    install_expansion(arguments, names, run / "user", run / "game")
    preferences = run / "preferences.conf"
    preferences.write_text(PREFERENCES, encoding="utf-8")
    command = [
        arguments.game,
        "--game-dir", str(run / "game"),
        "--content", "totala:boneyards",
        "--mod-dir", str(mod),
        "--mute", "--skip-intro",
        "--check-boneyards-menu-modes", mode,
        "--preferences-file", str(preferences),
        "--data-dir", str(run / "data"),
        "--user-folder", str(run / "user"),
    ]
    completed = subprocess.run(
        command, cwd=run, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        print(f"the {mode} run ended with {completed.returncode}", file=sys.stderr)
        return None
    return completed.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--coreprime", required=True, help="the mod folder this build writes")
    parser.add_argument("--bundle-table", required=True, help="the content bundles' table")
    parser.add_argument("--work", required=True)
    arguments = parser.parse_args()
    names = expansion_files.copy_names(arguments.bundle_table)
    work = Path(arguments.work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    coreprime = Path(arguments.coreprime)
    base_art = work / "ridge-base-art"
    write_mod(
        base_art,
        coreprime,
        {"guis/mainmenu.gui": main_menu_layout()},
        "media:\n  boneyards-entry: {x: 100, y: 100}\n",
    )
    own = work / "ridge-own-art"
    write_mod(own, coreprime, {"bitmaps/frontendx.pcx": own_art()})
    placed = work / "ridge-own-art-placed"
    write_mod(
        placed,
        coreprime,
        {"bitmaps/frontendx.pcx": own_art()},
        "media:\n  boneyards-entry: {mode: button, x: 40, y: 200}\n",
    )
    runs = (
        ("strip", "strip", base_art, (
            "boneyards menu modes check: strip: ",
            "boneyards menu: media.boneyards-entry x 100 and y 100 place the ENTER plate in "
            "button mode only; this menu is in strip mode",
        )),
        ("button", "button", own, (
            "boneyards menu: the ENTER plate stands at 497,8, the default place at the top right",
            "boneyards menu modes check: Credits played its movie and gave back the main menu "
            "as it was",
            "boneyards menu modes check: button: ",
            "ENTER on the plate at 497,8 was pressed",
            "Start Open Annihilation without the mod on its command line to enter the Boneyards.",
        )),
        ("button", "button-placed", placed, (
            "boneyards menu: the ENTER plate stands at 40,200, where the mod's profile puts it",
            "ENTER on the plate at 40,200 was pressed",
            "Start Open Annihilation without the mod on its command line to enter the Boneyards.",
        )),
    )
    for mode, name, mod, lines in runs:
        output = play(arguments, names, mod, work, mode, name)
        missing = [line for line in lines if output is None or line not in output]
        if missing:
            print(f"boneyards menu modes: the {name} run did not pass; missing: {missing}",
                  file=sys.stderr)
            return 1
    print("boneyards menu modes: the strip on the base game's art, the ENTER plate on art of "
          "the mod's own, with the expansion installed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
