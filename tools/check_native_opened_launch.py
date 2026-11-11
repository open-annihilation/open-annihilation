#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a launch file the system opens in the game, end to end.

The game is opened with each file as the system opens one, a drop of the file
on the game (--check-opened-launch), so that no file association of this
machine is used or changed. Each game runs as a program of its own on the
dummy SDL drivers, over 127.0.0.1, at a DirectPlay port of the check's own
(--dplay-port), so the check runs beside anything else on this machine:

- an .oalaunch host file, in a folder whose name has a space, opened as the
  game starts, before its window opens, is the start's launch, as -c makes it: the battle room opens on the file's map and rules
  and seats the player and two computer players as the file says, and the main
  menu is never left for it;
- the same launch in a temporary .txt file, as macOS's `open -f` hands one
  over, opened once the main menu has settled, goes from the main menu to the
  same battle room;
- a file whose metal is above the slider, opened at the main menu, is not
  opened: the log names the field, and no battle room opens.

The lines are read from the game's log (--log-dir), which is where those
lines are kept.
"""
import argparse
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
# Seconds a game may take to log a line the check waits for.
LINE_TIMEOUT = 120
# Seconds a refused file is given to open a battle room it must not open.
REFUSED_GRACE = 5
ROOM_LINE = 'multiplayer: in the battle room of "Example game" as Commander'
MAP_LINE = "multiplayer: battle room map: Acid Foursome"
RULES_LINE = ("multiplayer: battle room rules: commander ends, metal 1000, energy 1000, "
              "units 250, los true, mapping mapped, starts random, cheats off, watchers on, "
              "player limit 4")
SEATS_LINE = ("multiplayer: battle room seats: Commander core team 1 colour 2, "
              "AI:Commander arm team 2 colour 5 hard, AI:Commander core team 1 colour 1 easy")
AT_START_LINE = "a launch file opened with the game is this start's launch: "
AT_MENU_LINE = "multiplayer: a launch file opened at the main menu goes to its battle room: "
MENU_CHECK_LINE = "opened-launch check: the main menu shows; the file is opened in the game"
NOT_OPENED_LINE = "multiplayer: launch file not opened: "
# The preferences every game of the check starts with: no catalogue is
# fetched, so nothing leaves 127.0.0.1.
PREFERENCES = 'open-annihilation-preferences 1\n"open-annihilation.update-check" "never"\n"open-annihilation.content-updates" "never"\n'

YAML = """oalaunch: 1
game:
  role: host
  name: Example game
  password: secret
  player-limit: 4
  launcher-name: Ladder
player:
  name: Commander
  side: core
  team: 1
  colour: 2
map: "Acid Foursome"
rules:
  commander-death: ends
  metal: 1000
  energy: 1000
  unit-limit: 250
  line-of-sight: true
  mapping: mapped
  start-positions: random
  cheats: off
  watchers: on
  time-limit: 0
  lock-options: false
computer-players:
  - side: arm
    team: 2
    colour: 5
    difficulty: hard
  - side: core
    team: 1
    difficulty: easy
"""

BAD_YAML = """oalaunch: 1
game:
  role: host
  name: Example game
map: "Acid Foursome"
rules:
  metal: 250000
"""


class OpenedLaunchFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


class Game:
    """One running game, whose log is read as it grows."""

    def __init__(self, native, game_dir, workdir, name, arguments):
        self.name = name
        self.log_dir = workdir / f"{name}-log"
        self.log_dir.mkdir()
        user = workdir / f"{name}-user"
        user.mkdir()
        data = workdir / f"{name}-data"
        data.mkdir()
        preferences = workdir / f"{name}.conf"
        preferences.write_text(PREFERENCES, encoding="utf-8")
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.output = workdir / f"{name}-output.txt"
        with open(self.output, "w", encoding="utf-8") as output:
            self.process = subprocess.Popen(
                [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
                 "--preferences-file", str(preferences), "--data-dir", str(data),
                 "--user-folder", str(user), "--log-dir", str(self.log_dir),
                 "--dplay-port", str(free_port()), *arguments],
                cwd=workdir, env=environment, stdin=subprocess.DEVNULL, stdout=output,
                stderr=subprocess.STDOUT)

    def log_text(self):
        """Returns the text of every log file written so far, and of the output."""
        parts = [path.read_text(encoding="utf-8", errors="replace")
                 for path in sorted(self.log_dir.glob("*.log"))]
        parts.append(self.output.read_text(encoding="utf-8", errors="replace"))
        return "\n".join(parts)

    def wait_in_log(self, expected, timeout=LINE_TIMEOUT):
        """Waits for a line in the game's log; raises OpenedLaunchFailure when it never comes."""
        deadline = time.monotonic() + timeout
        while expected not in self.log_text() and time.monotonic() < deadline:
            if self.process.poll() is not None:
                break
            time.sleep(0.2)
        if expected not in self.log_text():
            raise OpenedLaunchFailure(f"{self.name} never logged: {expected}\n{self.log_text()}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


def battle_room(native, game_dir, workdir, name, when, path, opened_line):
    """Opens a launch file in the game and waits for its battle room in the log."""
    game = Game(native, game_dir, workdir, name, ["--check-opened-launch", when, str(path)])
    try:
        game.wait_in_log(opened_line + str(path))
        for line in (ROOM_LINE, MAP_LINE, RULES_LINE, SEATS_LINE):
            game.wait_in_log(line)
        text = game.log_text()
    finally:
        game.stop()
    if "secret" in text:
        raise OpenedLaunchFailure(f"{name} printed the password")
    if when == "start" and (MENU_CHECK_LINE in text or AT_MENU_LINE in text):
        raise OpenedLaunchFailure(f"{name}: the launch opened at start waited for the main menu")
    if when == "menu" and text.find(MENU_CHECK_LINE) > text.find(AT_MENU_LINE):
        raise OpenedLaunchFailure(f"{name}: the battle room opened before the main menu showed")
    print(f"{name}: {opened_line}{path.name}")
    for line in (ROOM_LINE, MAP_LINE, RULES_LINE, SEATS_LINE):
        print(f"{name}: {line}")


def refused(native, game_dir, workdir, path):
    """A file the battle room cannot use, opened at the main menu, opens nothing."""
    game = Game(native, game_dir, workdir, "refused", ["--check-opened-launch", "menu", str(path)])
    try:
        expected = f"{NOT_OPENED_LINE}{path}: rules.metal: 250000 is above 10000"
        game.wait_in_log(expected)
        time.sleep(REFUSED_GRACE)
        text = game.log_text()
    finally:
        game.stop()
    if ROOM_LINE in text or "multiplayer: in the battle room" in text:
        raise OpenedLaunchFailure("refused: a battle room opened for a file that was not opened")
    print(f"refused: {NOT_OPENED_LINE}{path.name}: rules.metal: 250000 is above 10000")


def check(native, game_dir, workdir):
    """Runs each case; raises OpenedLaunchFailure when one differs."""
    # A file the system opens is handed over whole, a space in its path included.
    downloads = workdir / "Downloads folder"
    downloads.mkdir()
    host = downloads / "host now.oalaunch"
    host.write_text(YAML, encoding="utf-8")
    # `open -f` writes what it is piped into a temporary file with a .txt name.
    piped = workdir / "5E1F0A2C.txt"
    piped.write_text(YAML, encoding="utf-8")
    bad = workdir / "bad.oalaunch"
    bad.write_text(BAD_YAML, encoding="utf-8")
    battle_room(native, game_dir, workdir, "opened-at-start", "start", host, AT_START_LINE)
    battle_room(native, game_dir, workdir, "opened-at-menu", "menu", piped, AT_MENU_LINE)
    refused(native, game_dir, workdir, bad)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-opened-launch-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), Path(scratch))
        except OpenedLaunchFailure as failure:
            print(f"opened launch check: {failure}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
