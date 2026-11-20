#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the game plays from folders deeper than its own path fields.

The game's own records hold paths in fields of 256 bytes, and the Image
Output Directory it starts with is the game folder followed by the user's
name, so a deep install or a long user name once stopped the game at start.
The game now keeps such paths whole. It starts headless on the dummy SDL
drivers, from a scratch game folder that links to every entry of the
installation --game-dir names, with its preferences file (and so the saved
games and pictures kept beside it) and an empty mod folder (--mod-dir) in
folders as deep, and USER and USERNAME set to a name of USER_NAME_LENGTH
characters:

- at DEEP_LENGTH characters, more than the game's fields hold;
- where the system says how long a path it opens (1,023 bytes on macOS,
  4,095 on Linux), at that length less MARGIN, which leaves room for the
  names inside each folder.

At each depth the game must open the main menu, and a headless skirmish
must save its world at SAVE_TICK into the saved-games folder beside the
preferences file and load it back with the same tick, units and digest.

Where the scratch folder can link to the installation's files neither by
symbolic links nor, for files, by hard links, or the system cannot make a
folder DEEP_LENGTH characters deep, the check prints one line and exits
with 77, which ctest reports as skipped.
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
# ctest's SKIP_RETURN_CODE.
SKIP = 77
RUN_TIMEOUT = 900
# More than the 256 bytes of the game's own path fields.
DEEP_LENGTH = 300
# Room left below the system's longest path for the names inside a folder:
# the game's archives and folders, the preferences file's temporary copy and
# a saved game's name.
MARGIN = 64
# The longest name one folder of the path may have on every system.
FOLDER_NAME_LENGTH = 200
USER_NAME_LENGTH = 200
SAVE_TICK = 20
MAIN_MENU = "native check: main menu open, 0 dialogs over it"
SAVED = re.compile(r"^saveload: saved (.+) at tick (\d+); save failures 0$", re.M)
DIGEST = re.compile(r"^saveload: tick (\d+) units (\d+) digest ([0-9a-f]{16})$", re.M)


class LongPathFailure(Exception):
    """A behaviour that differs from the expected one."""


class CannotPrepare(Exception):
    """The system cannot hold the folders the check needs."""


def deep_folder(base, length):
    """Makes and returns a folder under `base` whose path is `length` characters long."""
    folder = str(base)
    while len(folder) < length:
        name_length = min(FOLDER_NAME_LENGTH, length - len(folder) - 1)
        if name_length < 1:
            break
        folder = os.path.join(folder, "d" * name_length)
    try:
        os.makedirs(folder, exist_ok=True)
    except OSError as error:
        raise CannotPrepare(f"cannot make a folder {length} characters deep ({error})") from error
    return Path(folder)


def link_installation(game_dir, folder):
    """Links every entry of the installation into `folder`.

    Each entry gets a symbolic link; a file that cannot have one, as where
    Windows grants no symbolic links, gets a hard link instead.
    """
    try:
        for entry in game_dir.iterdir():
            link = folder / entry.name
            try:
                link.symlink_to(entry, target_is_directory=entry.is_dir())
            except OSError:
                if entry.is_dir():
                    raise
                os.link(entry, link)
    except OSError as error:
        raise CannotPrepare(f"cannot link to the installation's files ({error})") from error


def run(native, arguments, environment, workdir, what):
    """Runs open-annihilation headless and returns its output."""
    result = subprocess.run(
        [*RUNNER, str(native), "--skip-intro", "--mute", "--headless-check", *arguments],
        cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise LongPathFailure(f"open-annihilation exited with {result.returncode} {what}")
    return output


def check_depth(native, game_dir, workdir, length, label):
    """Plays from a game folder, preferences file and mod folder `length` characters deep."""
    game = deep_folder(workdir / f"{label}-game", length)
    link_installation(game_dir, game)
    preferences_folder = deep_folder(workdir / f"{label}-preferences", length)
    mod = deep_folder(workdir / f"{label}-mod", length)
    preferences = preferences_folder / "p.conf"
    preferences.write_text("open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n")
    user = "u" * USER_NAME_LENGTH
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       USER=user, USERNAME=user)
    common = ["--game-dir", str(game), "--mod-dir", str(mod), "--preferences-file", str(preferences)]
    where = f"from folders {length} characters deep"
    menu = run(native, [*common, "--frames", "60"], environment, workdir, where)
    if MAIN_MENU not in menu.splitlines():
        print(menu, end="")
        raise LongPathFailure(f"the main menu did not open {where}")
    played = run(native, [*common, "--match-ticks", str(SAVE_TICK), "--seed", "7",
                          "--save-after", str(SAVE_TICK)], environment, workdir, where)
    saved = SAVED.search(played)
    digests = DIGEST.findall(played)
    if saved is None or not digests:
        print(played, end="")
        raise LongPathFailure(f"the skirmish did not save {where}")
    save = Path(saved.group(1))
    if not save.is_file() or preferences_folder not in save.parents:
        raise LongPathFailure(f"the save is not beside the preferences file {where}: {save}")
    loaded = run(native, [*common, "--load", str(save), "--match-ticks", "0"], environment, workdir,
                 where)
    if DIGEST.findall(loaded)[:1] != digests[-1:]:
        print(played + loaded, end="")
        raise LongPathFailure(f"the saved world did not load back {where}")
    print(f"{label}: the main menu opened and a save loaded back from folders {length} characters "
          f"deep, with a user name of {USER_NAME_LENGTH} characters")


def longest_path(folder):
    """The longest path the system opens, or None where it does not say."""
    try:
        return os.pathconf(folder, "PC_PATH_MAX") - 1
    except (AttributeError, OSError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-long-paths-",
                                     dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        try:
            check_depth(native, game_dir, workdir, DEEP_LENGTH, "deep")
            longest = longest_path(workdir)
            if longest is not None and longest - MARGIN > DEEP_LENGTH:
                check_depth(native, game_dir, workdir, longest - MARGIN, "limit")
            else:
                print("the system does not say how long a path it opens; checked "
                      f"{DEEP_LENGTH} characters only")
        except CannotPrepare as reason:
            print(f"skipped the long path check: {reason}")
            return SKIP
        except LongPathFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print("the long path check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
