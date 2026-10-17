#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Drive the Game files screen's native checks (--check-game-files).

Each variant recreates the work folder DIR (a second half, given
--keep-work, reuses the folder its first half left), prepares the variant's
source folder and Documents folder from the installed game by hard links,
falling back to copies where the volume refuses links, and runs the game
with --skip-intro --mute --check-game-files --data-dir DIR/Library
--preferences-file DIR/preferences.conf and the variant's options on the
dummy video and audio drivers and the software renderer, with DIR as the
working directory. The game prints one verdict line, "game-files check:
<variant>: passed" or "... failed: <why>", which this script prints with
the rest of the game's output, and its status is this script's. The
pictures of each state, game-files-<variant>-<nn>-<step>-*.png, are left
in DIR.

Without the installed game (or, for the demo variant, the demo's installer)
the script exits with --skip-code, which ctest reports as skipped.

The subset of the installed game the variants copy is totala1.hpi,
rev31.gp3 and tactics1.hpi, two music tracks, the smallest movie of Data/,
a mod with a usable profile, and files the copy leaves out (setup.exe,
manual.pdf, Thumbs.db, .DS_Store, unins000.dat). The fixtures that are not
game data are written by this script; no game data is kept anywhere else.

Variants: folder, phone, demo, copy-yourself, stop, resume, not-game,
short-space, manage, manage-next-start (docs/game-files.md).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

VARIANTS = (
    "folder",
    "phone",
    "demo",
    "copy-yourself",
    "stop",
    "resume",
    "not-game",
    "short-space",
    "manage",
    "manage-next-start",
)

# The second halves, which run in the folder their first halves left.
SECOND_HALVES = {"resume": "stop", "manage-next-start": "manage"}

# The game's archives the subset holds, matched without regard to case.
SUBSET_ARCHIVES = ("totala1.hpi", "rev31.gp3", "tactics1.hpi")

# How many music tracks the subset holds.
MUSIC_TRACKS = 2

# Files no game data uses, which the copy leaves out, with the bytes written in each.
LEFT_OUT_FIXTURES = {
    "setup.exe": b"MZ not a program\n",
    "manual.pdf": b"%PDF-1.4 not a manual\n",
    "Thumbs.db": b"not a thumbnail cache\n",
    ".DS_Store": b"not a folder's view settings\n",
    "unins000.dat": b"not an uninstaller's log\n",
}

# A mod profile that changes nothing (docs/mods/oamod-standard.md).
MOD_PROFILE = (
    "oamod: 1\n"
    "id: {id}\n"
    "name: {name}\n"
    'version: "1.0"\n'
    "requires: {{base: ta-3.1c, catalogue: 1}}\n"
    "author: {{name: unknown}}\n"
    "packaging: {{revision: 1, date: 2026-10-04, packager: Open Annihilation}}\n"
)

# The window the variants open when --resolution names none: a tablet's, in points.
TABLET_RESOLUTION = "1194x834"

# The copy's pace, and where the stop variant stops it, in bytes.
STOP_COPY_RATE = 4_000_000
STOP_AFTER = 8_000_000

# The free space the short-space variant reports, in bytes.
SHORT_FREE_BYTES = 50_000_000


def parse_arguments(argv):
    """Reads the command line; the options after -- go to the game unchanged."""
    extra = []
    if "--" in argv:
        split = argv.index("--")
        argv, extra = argv[:split], argv[split + 1 :]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", required=True, help="the open-annihilation executable")
    parser.add_argument("--work", required=True, help="the work folder the variant runs in")
    parser.add_argument("--variant", required=True, choices=VARIANTS, help="the route to check")
    parser.add_argument("--game-dir", default="", help="the installed game (OA_GAME_DIR)")
    parser.add_argument("--demo-installer", default="", help="the demo's installer (OA_DEMO_INSTALLER)")
    parser.add_argument("--resolution", default="", help="the window's size in points, WxH")
    parser.add_argument(
        "--keep-work",
        action="store_true",
        help="reuse the work folder a first half left instead of recreating it",
    )
    parser.add_argument(
        "--skip-code",
        type=int,
        default=77,
        help="the status that reports the check as skipped",
    )
    arguments = parser.parse_args(argv)
    arguments.extra = extra
    return arguments


def say(variant, text):
    """Prints a line of the driver's own, flushed so it keeps its place among the game's."""
    print(f"check_native_game_files: {variant}: {text}", flush=True)


def find_entry(folder, name):
    """Returns the entry of a folder whose name matches without regard to case, or None."""
    wanted = name.lower()
    try:
        for entry in sorted(folder.iterdir()):
            if entry.name.lower() == wanted:
                return entry
    except OSError:
        return None
    return None


def place(source, target):
    """Hard-links a file to a new place, or copies it where the volume refuses links."""
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def write_file(path, data):
    """Writes a fixture's bytes, making its folder."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def prepare_subset(game_dir, folder, with_fixtures):
    """Puts the subset of the installed game into a folder; returns False when it lacks one."""
    folder.mkdir(parents=True, exist_ok=True)
    for name in SUBSET_ARCHIVES:
        entry = find_entry(game_dir, name)
        if entry is None or not entry.is_file():
            return False
        place(entry, folder / entry.name)
    music = find_entry(game_dir, "music")
    if music is not None and music.is_dir():
        tracks = sorted(path for path in music.iterdir() if path.is_file() and path.suffix.lower() == ".mp3")
        for track in tracks[:MUSIC_TRACKS]:
            place(track, folder / music.name / track.name)
    data = find_entry(game_dir, "data")
    if data is not None and data.is_dir():
        movies = sorted(
            (path for path in data.iterdir() if path.is_file() and path.suffix.lower() == ".zrb"),
            key=lambda path: (path.stat().st_size, path.name),
        )
        if movies:
            place(movies[0], folder / data.name / movies[0].name)
    write_file(
        folder / "mods" / "check-units" / "oamod.yaml",
        MOD_PROFILE.format(id="check-units", name="Check units").encode("utf-8"),
    )
    if with_fixtures:
        for name, data_bytes in LEFT_OUT_FIXTURES.items():
            write_file(folder / name, data_bytes)
    return True


def prepare_not_game(folder):
    """Writes a folder with no game in it, nor two levels down."""
    write_file(folder / "readme.txt", b"Notes about something else.\n")
    write_file(folder / "letters" / "first.txt", b"A letter.\n")
    write_file(folder / "letters" / "older" / "second.txt", b"Another letter.\n")


def prepare_additions(folder):
    """Writes a folder that holds a mod at its top, as an addition."""
    write_file(
        folder / "oamod.yaml",
        MOD_PROFILE.format(id="check-addition", name="Check addition").encode("utf-8"),
    )
    write_file(folder / "readme.txt", b"A mod that changes nothing.\n")


def recreate(work):
    """Removes what an earlier run left in the work folder and makes it again."""
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)


def prepare(arguments, work, game_dir):
    """Prepares the variant's folders; returns the game options it adds, or None to skip."""
    variant = arguments.variant
    source = work / "source"
    options = []
    if variant in ("folder", "phone", "stop", "short-space", "copy-yourself"):
        if not prepare_subset(game_dir, source, variant != "copy-yourself"):
            return None
        options += ["--game-files-source", str(source)]
    if variant in ("folder", "phone"):
        options += ["--game-files-route", "folder"]
    elif variant == "demo":
        installer = Path(arguments.demo_installer)
        picked = work / "picked" / installer.name
        place(installer, picked)
        options += ["--game-files-route", "demo", "--game-files-source", str(picked)]
    elif variant == "copy-yourself":
        options += ["--game-files-route", "copy-yourself"]
    elif variant == "stop":
        options += [
            "--game-files-copy-rate",
            str(STOP_COPY_RATE),
            "--game-files-stop-after",
            str(STOP_AFTER),
            "--game-files-expect",
            "stopped-kept",
        ]
    elif variant == "resume":
        options += ["--game-files-source", str(source), "--game-files-expect", "resumed"]
    elif variant == "not-game":
        prepare_not_game(source)
        options += ["--game-files-source", str(source), "--game-files-expect", "not-a-game"]
    elif variant == "short-space":
        options += [
            "--game-files-free-bytes",
            str(SHORT_FREE_BYTES),
            "--game-files-expect",
            "short-space",
        ]
    elif variant == "manage":
        if not prepare_subset(game_dir, work / "Documents" / "Total Annihilation", False):
            return None
        additions = work / "additions"
        prepare_additions(additions)
        options += ["--game-files-route", "manage", "--game-files-source", str(additions)]
    elif variant == "manage-next-start":
        options += [
            "--game-files-route",
            "manage",
            "--game-files-expect",
            "next-start",
            "--game-files-source",
            str(work / "additions"),
        ]
    return options


def main(argv):
    """Runs one variant and returns the process status."""
    arguments = parse_arguments(argv)
    variant = arguments.variant
    game_dir = Path(arguments.game_dir) if arguments.game_dir else None
    if game_dir is None or not game_dir.is_dir():
        say(variant, "skipped: no installed game (OA_GAME_DIR)")
        return arguments.skip_code
    if variant == "demo" and not (arguments.demo_installer and Path(arguments.demo_installer).is_file()):
        say(variant, "skipped: no installer of the demo (OA_DEMO_INSTALLER)")
        return arguments.skip_code
    work = Path(arguments.work).resolve()
    if arguments.keep_work or variant in SECOND_HALVES:
        if not work.is_dir():
            first = SECOND_HALVES.get(variant, "its first half")
            print(f"game-files check: {variant}: failed: {work} is missing; run {first} first", flush=True)
            return 1
        for picture in work.glob(f"game-files-{variant}-*.png"):
            picture.unlink()
    else:
        recreate(work)
    options = prepare(arguments, work, game_dir)
    if options is None:
        say(variant, "skipped: the installed game lacks a file of the subset")
        return arguments.skip_code
    command = [
        str(Path(arguments.game).resolve()),
        "--skip-intro",
        "--mute",
        "--check-game-files",
        "--data-dir",
        str(work / "Library"),
        "--preferences-file",
        str(work / "preferences.conf"),
        "--resolution",
        arguments.resolution or TABLET_RESOLUTION,
    ]
    command += options + arguments.extra
    environment = dict(os.environ)
    environment.update(
        {"SDL_VIDEO_DRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy", "SDL_RENDER_DRIVER": "software"}
    )
    say(variant, "runs " + " ".join(command))
    # The output is taken and printed here: a game built as a windowed
    # program has no standard output of its own on Windows unless it is
    # handed one.
    completed = subprocess.run(
        command, cwd=work, env=environment, check=False, capture_output=True, text=True,
        errors="replace",
    )
    print(completed.stdout + completed.stderr, end="", flush=True)
    say(variant, f"pictures in {work}")
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
