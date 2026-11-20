#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the game plays the Total Annihilation demo (1997) from the folder holding its installer.

The installer the OA_DEMO_INSTALLER environment variable names stays where it
is. The game, open-annihilation, starts headless with --game-dir on its folder
and --data-dir on a scratch folder. The preferences file sits in that scratch
folder, so the player's folder is "Open Annihilation" beside it. The first
start must unpack the archive to Games/totala-demo/TADemo.hpi there, with the
.oa-content record of that base game, check it and mount it, and the verifier
(oa-demo-installer-test --verify) must then find the file's size and SHA-256
to be those of the release the engine recognises. A second start must check
the archive and mount it without unpacking it again, and one after the
archive was damaged must unpack it again. An earlier unpack left at
demo-1997/TADemo.hpi in the data folder is copied into Games/totala-demo once
and left where it was. The installer's folder must be left as it was.

Each start runs --skip-intro --frames 60 on the dummy SDL drivers and must
reach the main menu: it ends with status 0 and its "native check:" lines,
which count the gadgets of the open screen and name it, must show the main
menu open with no dialog over it, such as a disc prompt or a message box.

With --check NAME it runs one of the game's own checks over the demo instead,
started the same way on a fresh scratch folder: navigation
(--check-navigation, which over the demo greys Skirmish so a press and its
key do nothing, walks MULTI's notice, the other grayed-out entries and the
campaign's way in and out), saved-games
(--check-load-save, which over data with no save and load dialog checks that
every entry to it is grayed out), multiplayer-menu
(--check-multiplayer-menu, whose MULTI shows the notice for data with no
multiplayer map) or render-tiers (--check-render-tiers with
--hardware-acceleration and --force-capable on SDL's software renderer, which
switch the accelerated presentation on, once the start-up function test
passed, over the main menu and the demo's first Arm mission, since the demo
has no skirmish map). Each must end with status 0 and print the line its check
prints when it passes; a check that skips, as render-tiers does on a machine
under 2 GiB of memory, ends with 77 and the demo check skips with it.

Without OA_DEMO_INSTALLER the check prints one line and exits with 77, which
ctest reports as skipped.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# ctest's SKIP_RETURN_CODE.
SKIP = 77
INSTALLER_VARIABLE = "OA_DEMO_INSTALLER"
# The player's folder beside the scratch preferences file.
PLAYER = "Open Annihilation"
# Where the archive is unpacked, under the player's folder, and the record beside it.
DEMO_FOLDER = Path(PLAYER) / "Games" / "totala-demo"
ARCHIVE_NAME = "TADemo.hpi"
RECORD_NAME = ".oa-content"
# An earlier unpack, under the data folder, copied from once.
LEGACY = Path("demo-1997") / ARCHIVE_NAME
# The game's line once the archive is mounted, and how it says where the
# archive came from.
MOUNTED = "open-annihilation: mounted the Total Annihilation demo (1997) from "
UNPACKED_NOW = ", unpacked now from "
CHECKED = ", unpacked earlier and checked, from "
# A headless run's last lines: the gadget count of the screen it opened, and
# which screen that is with how many dialogs are open over it.
READY = re.compile(r"^native check: \d+x\d+, (\d+) GUI gadgets,", re.MULTILINE)
OPEN_SCREEN = re.compile(r"^native check: (.+) open, (\d+) dialogs over it$", re.MULTILINE)
FRAMES = "60"
START_TIMEOUT = 900
# The game's checks over the demo, by --check name: the switches that run one
# and the start of the line it prints when it passes.
GAME_CHECKS = {
    "navigation": (["--headless-check", "--check-navigation"], "navigation check without maps: "),
    "saved-games": (["--check-load-save"],
                  "load/save check: the data has no save and load dialog; Load Game, SAVEGAME and "
                  "LOADGAME are grayed out and take no press"),
    "multiplayer-menu": (["--check-multiplayer-menu"],
                         "multiplayer menu check: MULTI shows the notice for data with no multiplayer map"),
    "render-tiers": (["--check-render-tiers", "--hardware-acceleration", "--force-capable",
                      "--campaign", "Arm Campaign", "--mission", "0"],
                     "render tiers check: the accelerated presentation draws within its references at every zoom"),
}


class DemoFailure(Exception):
    """A behaviour that differs from the expected one."""



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


def expect(condition, what, output=""):
    """Fails the check with `what`, printing `output`, unless `condition` holds."""
    if not condition:
        if output:
            print(output, end="" if output.endswith("\n") else "\n")
        raise DemoFailure(what)


def listing(folder):
    """The names, sizes and modification times of a folder's files."""
    return sorted((entry.name, entry.stat().st_size, entry.stat().st_mtime_ns)
                  for entry in folder.iterdir())


def start(native, folder, data, workdir):
    """Starts open-annihilation on `folder` with `data` as its data folder; returns its output."""
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    _offline_preferences(workdir / "preferences.conf")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(folder), "--data-dir", str(data), "--skip-intro",
         "--mute", "--headless-check", "--frames", FRAMES,
         "--preferences-file", str(workdir / "preferences.conf")],
        cwd=workdir, env=environment, timeout=START_TIMEOUT, check=False, capture_output=True, text=True,
        errors="replace")
    output = result.stdout + result.stderr
    expect(MOUNTED in output, "open-annihilation did not mount the demo's archive", output)
    expect(result.returncode == 0, f"open-annihilation exited with {result.returncode} after mounting the archive",
           output)
    ready = READY.search(output)
    expect(ready is not None, "open-annihilation ended without its native check line", output)
    expect(int(ready.group(1)) > 0, "open-annihilation opened a screen without gadgets", output)
    screen = OPEN_SCREEN.search(output)
    expect(screen is not None, "open-annihilation did not say which screen it opened", output)
    expect(screen.group(1) == "main menu", f"open-annihilation opened the {screen.group(1)}, not the main menu", output)
    expect(screen.group(2) == "0", f"open-annihilation opened the main menu under {screen.group(2)} dialogs", output)
    return output


def verify(verifier, archive):
    """Checks the archive's size and SHA-256 with the verifier."""
    result = subprocess.run([*RUNNER, str(verifier), "--verify", str(archive)], check=False,
                            capture_output=True, text=True, errors="replace")
    expect(result.returncode == 0, f"{archive} is not the demo's archive", result.stdout + result.stderr)


def demo_record(archive):
    """The base game's record beside `archive`."""
    return archive.parent / RECORD_NAME


def expect_demo_folder(archive):
    """The archive and its record, and nothing else, are in the demo's folder."""
    record = demo_record(archive)
    expect(archive.is_file(), f"no archive at {archive}")
    expect(record.is_file(), f"no content record at {record}")
    text = record.read_text(encoding="utf-8", errors="replace")
    expect("bundle totala-demo" in text and ARCHIVE_NAME in text,
           "the content record does not name the demo's archive")
    expect(sorted(path.name for path in archive.parent.iterdir()) == [RECORD_NAME, ARCHIVE_NAME],
           "the demo's folder holds more than its archive and record")


def check(native, verifier, installer, workdir):
    """Runs every start; raises DemoFailure."""
    folder = installer.parent
    data = workdir / "data"
    before = listing(folder)
    archive = workdir / DEMO_FOLDER / ARCHIVE_NAME

    output = start(native, folder, data, workdir)
    expect(UNPACKED_NOW in output, "the first start did not unpack the archive", output)
    expect_demo_folder(archive)
    verify(verifier, archive)

    output = start(native, folder, data, workdir)
    expect(CHECKED in output, "the second start did not reuse the checked archive", output)

    with archive.open("r+b") as damaged:
        damaged.seek(archive.stat().st_size // 2)
        byte = damaged.read(1)
        damaged.seek(-1, os.SEEK_CUR)
        damaged.write(bytes([byte[0] ^ 1]))
    output = start(native, folder, data, workdir)
    expect(UNPACKED_NOW in output, "a damaged archive was not unpacked again", output)
    expect_demo_folder(archive)
    verify(verifier, archive)

    # An earlier unpack is copied once into the player's folder and left in place.
    legacy = data / LEGACY
    legacy.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(archive, legacy)
    legacy_stamp = legacy.stat().st_mtime_ns
    shutil.rmtree(archive.parent)
    output = start(native, folder, data, workdir)
    expect(CHECKED in output,
           "an earlier demo-1997 archive was not copied into the player's folder", output)
    expect_demo_folder(archive)
    verify(verifier, archive)
    expect(legacy.is_file() and legacy.stat().st_mtime_ns == legacy_stamp,
           "the earlier demo-1997 archive was not left as it was")
    output = start(native, folder, data, workdir)
    expect(CHECKED in output, "a copied demo-1997 archive was not reused", output)
    expect(legacy.is_file() and legacy.stat().st_mtime_ns == legacy_stamp,
           "an earlier demo-1997 archive was copied again")

    expect(listing(folder) == before, "the installer's folder changed")


def game_check(native, name, installer, workdir):
    """Runs open-annihilation's check `name` over the demo from its installer's folder.

    Returns True when it passed and False when the check skipped; raises DemoFailure.
    """
    switches, passed = GAME_CHECKS[name]
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
    _offline_preferences(workdir / "preferences.conf")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(installer.parent), "--data-dir", str(workdir / "data"),
         "--skip-intro", "--mute", *switches, "--preferences-file", str(workdir / "preferences.conf")],
        cwd=workdir, env=environment, timeout=START_TIMEOUT, check=False, capture_output=True, text=True,
        errors="replace")
    output = result.stdout + result.stderr
    expect(MOUNTED in output, "open-annihilation did not mount the demo's archive", output)
    if result.returncode == SKIP:
        print(output, end="" if output.endswith("\n") else "\n")
        return False
    expect(result.returncode == 0, f"open-annihilation's {name} check exited with {result.returncode}", output)
    expect(passed in output, f"open-annihilation's {name} check did not say it passed", output)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--verifier", type=Path, help="oa-demo-installer-test, for the installer check")
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--check", choices=["installer", *GAME_CHECKS], default="installer",
                        help="the installer's own starts (the default), or one of open-annihilation's checks")
    args = parser.parse_args()
    if args.check == "installer" and args.verifier is None:
        parser.error("the installer check needs --verifier")
    named = os.environ.get(INSTALLER_VARIABLE, "")
    if not named:
        print(f"skipped the demo {args.check} native check: {INSTALLER_VARIABLE} is not set; "
              "set it to the Total Annihilation demo (1997) installer")
        return SKIP
    installer = Path(named)
    if not installer.is_file():
        print(f"FAIL {INSTALLER_VARIABLE} names no file: {named}")
        return 1
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"native-demo-{args.check}-", dir=args.scratch_root.resolve()) as scratch:
        try:
            if args.check == "installer":
                check(args.native.resolve(), args.verifier.resolve(), installer.resolve(), Path(scratch))
            elif not game_check(args.native.resolve(), args.check, installer.resolve(), Path(scratch)):
                print(f"skipped the demo {args.check} native check: open-annihilation's check skipped")
                return SKIP
        except DemoFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print(f"the demo {args.check} native check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
