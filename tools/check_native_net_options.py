#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check hosting and joining a network game from the command line, end to end.

Each game runs as a program of its own through its main loop on the dummy SDL
drivers, over 127.0.0.1, at a DirectPlay port of the check's own
(--dplay-port), so the check runs beside anything else on this machine:

- a host started with --host, --player-name and --game-name reaches its
  battle room;
- a joiner started with --join and --player-name joins it: both battle rooms
  list both players by the names given;
- with a game hosted with --game-password, a joiner that sends the wrong
  password is refused with the notice the game list shows ("You did not have
  the correct password"), and one that sends the right password and names
  the game with --game-name joins it.

The joiners' preferences hold a TCP/IP address and a player's name of their
own, which the run must leave as they were.
"""
import argparse
import os
from pathlib import Path
import queue
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
# Frames a joiner runs before it ends by itself: enough to join and hear the
# host many times over, or to be refused.
JOIN_FRAMES = "900"
# Seconds a game may take to print a line the check waits for, or to end.
LINE_TIMEOUT = 120
# The address and name a joiner's preferences hold before the run.
STORED_ADDRESS = "192.0.2.7"
STORED_NAME = "Stored"
PREFERENCES = ('open-annihilation-preferences 1\n"open-annihilation.update-check" "never"\n"open-annihilation.content-updates" "never"\n'
               f'"multiplayer|tcpaddr" "{STORED_ADDRESS}"\n'
               f'"Total Annihilation|Nickname" "{STORED_NAME}"\n')

# A launch whose preferences file nothing wrote runs on the defaults, with both
# online checks on. This is the file such a launch starts from.
OFFLINE_PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"\n'
)


class NetOptionsFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


class Game:
    """One running game, whose standard output the check reads line by line."""

    def __init__(self, native, game_dir, workdir, name, arguments):
        self.name = name
        self.lines = []
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.preferences = workdir / f"{name}.conf"
        if not self.preferences.exists():
            self.preferences.write_text(OFFLINE_PREFERENCES, encoding="utf-8")
        data_dir = workdir / f"{name}.data"
        data_dir.mkdir(exist_ok=True)
        self.process = subprocess.Popen(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute",
             "--preferences-file", str(self.preferences), "--data-dir", str(data_dir), *arguments],
            cwd=workdir, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def _take(self, timeout):
        """Moves the next line read into self.lines, waiting up to timeout seconds.

        Returns False once the output has ended.
        """
        if self._ended:
            return False
        try:
            line = self._queue.get(timeout=timeout)
        except queue.Empty:
            return True
        if line is None:
            self._ended = True
            return False
        self.lines.append(line)
        return True

    def wait_for(self, expected):
        """Waits for a line of the game's output; raises NetOptionsFailure when it never comes."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while expected not in self.lines and time.monotonic() < deadline:
            if not self._take(0.5):
                break
        if expected not in self.lines:
            raise NetOptionsFailure(f"{self.name} never printed: {expected}")

    def wait_end(self):
        """Waits for the game to end by itself; raises NetOptionsFailure unless it ends with status 0."""
        try:
            status = self.process.wait(timeout=LINE_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise NetOptionsFailure(f"{self.name} did not end within {LINE_TIMEOUT} s") from None
        while self._take(LINE_TIMEOUT):
            pass
        if status != 0:
            raise NetOptionsFailure(f"{self.name} exited with {status}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        """Returns what the game printed, each line under its name."""
        while not self._queue.empty() and self._take(0):
            pass
        return "".join(f"{self.name}: {line}\n" for line in self.lines)


def joiner(native, game_dir, workdir, name, port, arguments):
    """Starts a joiner whose preferences hold an address and a name of their own."""
    (workdir / f"{name}.conf").write_text(PREFERENCES, encoding="utf-8")
    return Game(native, game_dir, workdir, name,
                ["--dplay-port", str(port), "--join", ADDRESS, "--frames", JOIN_FRAMES, *arguments])


def check_preferences_kept(game):
    """Raises NetOptionsFailure when a joiner's run changed the address or name its preferences held."""
    text = game.preferences.read_text(encoding="utf-8", errors="replace")
    for key, value in (("multiplayer|tcpaddr", STORED_ADDRESS),
                       ("Total Annihilation|Nickname", STORED_NAME)):
        if f'"{key}" "{value}"' not in text:
            raise NetOptionsFailure(f"{game.name}'s preferences no longer hold {key} {value}")


def check(native, game_dir, workdir):
    """Hosts and joins both games; raises NetOptionsFailure when a game differs."""
    games = []
    try:
        port = free_port()
        host = Game(native, game_dir, workdir, "open-host",
                    ["--dplay-port", str(port), "--host", "--player-name", "Hoster",
                     "--game-name", "Open Room"])
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        visitor = joiner(native, game_dir, workdir, "open-joiner", port,
                         ["--player-name", "Visitor"])
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        visitor.wait_for("multiplayer: battle room players: Visitor, Hoster")
        host.wait_for("multiplayer: battle room players: Hoster, Visitor")
        visitor.wait_end()
        check_preferences_kept(visitor)
        host.stop()

        port = free_port()
        keeper = Game(native, game_dir, workdir, "locked-host",
                      ["--dplay-port", str(port), "--host", "--player-name", "Keeper",
                       "--game-name", "Locked Room", "--game-password", "secret"])
        games.append(keeper)
        keeper.wait_for('multiplayer: in the battle room of "Locked Room" as Keeper')
        guesser = joiner(native, game_dir, workdir, "wrong-password", port,
                         ["--player-name", "Guesser", "--game-password", "wrong"])
        games.append(guesser)
        guesser.wait_for(f"multiplayer: could not join {ADDRESS}: "
                         "You did not have the correct password")
        guesser.wait_end()
        friend = joiner(native, game_dir, workdir, "right-password", port,
                        ["--player-name", "Friend", "--game-name", "Locked Room",
                         "--game-password", "secret"])
        games.append(friend)
        friend.wait_for('multiplayer: in the battle room of "Locked Room" as Friend')
        friend.wait_for("multiplayer: battle room players: Friend, Keeper")
        keeper.wait_for("multiplayer: battle room players: Keeper, Friend")
        friend.wait_end()
        check_preferences_kept(friend)
        if any("Guesser" in line for line in keeper.lines):
            raise NetOptionsFailure("the locked game's host seated the joiner with the wrong password")
    except NetOptionsFailure:
        for game in games:
            game.stop()
            print(game.output(), end="")
        raise
    finally:
        for game in games:
            game.stop()
    print("net options check: --host reached the battle room, --join joined it by address with "
          "both names listed on both machines, a wrong --game-password was refused and the right "
          "one joined the game --game-name named; the joiners' preferences kept their address "
          "and name")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-net-options-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), Path(scratch))
        except NetOptionsFailure as failure:
            print(f"net options check: {failure}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
