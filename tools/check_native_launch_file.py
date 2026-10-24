#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a network game started from a launch, end to end.

Each game runs as a program of its own on the dummy SDL drivers, over
127.0.0.1, at a DirectPlay port of the check's own (--dplay-port), so the
check runs beside anything else on this machine:

- --launch with an .oalaunch host file opens the battle room on the file's
  map and rules, seats the player and two computer players as the file says;
- -c with the same file, under another name, does the same;
- the same launch piped in on standard input, with no option, does the same;
- the same launch as options alone does the same;
- -c with TA Forever's file opens the battle room on its map and rules, and
  the log names the two keys 3.1c does not read;
- -c with either file in a folder whose name has a space, the .oalaunch file
  apart and TA Forever's file attached to -C, does the same: each argument
  is one word;
- -c with a bare name in the current folder, a file that names connect,
  creator and address as 3.1c reads them, and -b after it, hosts at once with
  the values 3.1c's battle room takes;
- a file whose metal is above the slider ends with status 2 and names the
  field, and an option above it names the option;
- -r ends with status 0 and says why, without opening a window;
- --list-content --json prints one JSON object and ends with status 0,
  without opening a window, and does so at once with an idle pipe as its
  standard input; with --log-dir the object is still printed on standard
  output, as its only line, and the log does not hold it;
- a launch whose map or mod is not installed, with the status lines on
  standard output, ends with status 2 before a window opens, and its status
  lines are hello and an exit line with reason refused and code 2;
- -n1:127.0.0.1 alone, with no launch, goes from the main menu through the
  provider list and the TCP/IP address to the game list of that address,
  as 3.1c does, and stays there for the player to choose a game.

The battle room's lines are read from the game's log (--log-dir), which is
where those lines are kept. The password in the file is never printed.
Every game keeps its preferences, its data and its log in the check's
scratch folder, and fetches no catalogue.
"""
import argparse
import json
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

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
# Seconds a game may take to print a line the check waits for, or to end.
LINE_TIMEOUT = 120
# Seconds an idle pipe on standard input may hold a start up: the game waits
# a quarter of a second for its first byte; the rest is the start itself.
IDLE_PIPE_LIMIT = 60
RULES = ("multiplayer: battle room rules: commander ends, metal 1000, energy 1000, "
         "los true, mapping mapped, starts random, cheats off, watchers on, player limit 4")
LAUNCH_RULES = ("multiplayer: battle room rules: commander ends, metal 1000, energy 1000, "
                "units 250, los true, mapping mapped, starts random, cheats off, watchers on, "
                "player limit 4")
MAP_LINE = "multiplayer: battle room map: Acid Foursome"
ROOM_LINE = 'multiplayer: in the battle room of "Example game" as Commander'
SEATS_LINE = ("multiplayer: battle room seats: Commander core team 1 colour 2, "
              "AI:Commander arm team 2 colour 5 hard, AI:Commander core team 1 colour 1 easy")

YAML = """oalaunch: 1
game:
  role: host
  name: Example game
  password: secret
  player-limit: 4
  launcher-name: Ladder
  directplay:
    application-guid: "{99797420-F5F5-11CF-9827-00A0241496C8}"
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

# The same launch as options alone.
OPTIONS = [
    "--host", "--game-name", "Example game", "--game-password", "secret",
    "--player-limit", "4", "--launcher-name", "Ladder", "--side", "core", "--team", "1",
    "--colour", "2", "--map", "Acid Foursome", "--commander-death", "ends", "--metal", "1000",
    "--energy", "1000", "--unit-limit", "250", "--line-of-sight", "true", "--mapping", "mapped",
    "--start-positions", "random", "--cheats", "off", "--watchers", "on", "--time-limit", "0",
    "--computer-player", "arm:2:5:hard", "--computer-player", "core:1::easy",
]

INI = """[totala]
watching=2
mapmode=2
location=2
cheats=1
los=1
deathopt=2
metal=1000
energy=1000
lockOptions=0
playerlimit=4
pfs=0
session=Example game
provider=TAForever
mission=Acid Foursome
timelimit=0
registeredonly=0
tournment=1
tournament=1
"""

# A file that names connect, creator and address, with odd values 3.1c's
# battle room takes as it can: energy above the slider, a unit limit below it,
# a key it does not read; and -b after -c replaces the line of sight.
KEYS_INI = """[TotalA]
connect=TCPIP
creator=1
address=127.0.0.1
session="Example game"
mission=Acid Foursome
deathopt=3
los=1
metal=2500
energy=40000
maxunits=5
cheats=1
watching=2
mapmode=2
location=2
tournament=1
"""
KEYS_RULES = ("multiplayer: battle room rules: commander deathmatch, metal 2500, energy 10000, "
              "units 20, los permanent, mapping mapped, starts random, cheats off, watchers on, "
              "player limit 6")
KEYS_NOTE = "open-annihilation: keys.ini: tournament: not read, as 3.1c does not read them"
INI_NOTE = "totala.ini: tournment, tournament: not read, as 3.1c does not read them"
SPACED_INI_NOTE = "TA Forever.ini: tournment, tournament: not read, as 3.1c does not read them"

# The preferences every game of the check starts with: no catalogue is
# fetched, so nothing leaves 127.0.0.1.
PREFERENCES = 'open-annihilation-preferences 1\n"open-annihilation.content-updates" "never"\n'
# The line the game logs as its window opens.
WINDOW_LINE = "open-annihilation: window: "

# A host whose map is not installed, and one whose mod is not, each with the
# status lines on standard output.
MISSING_MAP_YAML = """oalaunch: 1
game:
  role: host
  name: Example game
  password: secret
map: "Acid Foursum"
status:
  standard-output: on
"""
MISSING_MOD_YAML = """oalaunch: 1
game:
  role: host
  name: Example game
map: "Acid Foursome"
content:
  mod: ridge
status:
  standard-output: on
"""

BAD_YAML = """oalaunch: 1
game:
  role: host
  name: Example game
  password: secret
map: "Acid Foursome"
rules:
  metal: 250000
"""


class LaunchFileFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


class Game:
    """One running game. Standard output and, when asked, the log are both read."""

    def __init__(self, native, game_dir, workdir, name, arguments, log_dir=None, piped=None):
        """Starts the game. piped is the text written to its standard input,
        which is then closed; "" leaves the pipe open and idle; None gives
        the game no pipe."""
        self.name = name
        self.lines = []
        self.log_dir = log_dir
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.preferences = workdir / f"{name}.conf"
        self.preferences.write_text(PREFERENCES, encoding="utf-8")
        data = workdir / f"{name}-data"
        data.mkdir()
        self.process = subprocess.Popen(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute",
             "--preferences-file", str(self.preferences), "--data-dir", str(data), *arguments],
            cwd=workdir, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL if piped is None else subprocess.PIPE,
            text=True, errors="replace")
        if piped:
            self.process.stdin.write(piped)
            self.process.stdin.close()
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

    def log_text(self):
        """Returns the text of every log file written so far."""
        if self.log_dir is None or not self.log_dir.is_dir():
            return ""
        parts = []
        for path in sorted(self.log_dir.glob("*.log")):
            parts.append(path.read_text(encoding="utf-8", errors="replace"))
        return "\n".join(parts)

    def wait_for(self, expected):
        """Waits for a line of the game's output; raises LaunchFileFailure when it never comes."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while expected not in self.lines and time.monotonic() < deadline:
            if not self._take(0.5):
                break
        if expected not in self.lines:
            raise LaunchFileFailure(f"{self.name} never printed: {expected}")

    def wait_in_log(self, expected):
        """Waits for a line of the game's log; raises LaunchFileFailure when it never comes.

        Standard output may have been sent to the log. The process staying up
        is not the end of the log, so a closed standard output is not the end
        of the wait.
        """
        deadline = time.monotonic() + LINE_TIMEOUT
        while expected not in self.log_text() and time.monotonic() < deadline:
            if self.process.poll() is not None:
                time.sleep(0.2)
                break
            time.sleep(0.2)
        while self._take(0):
            pass
        if expected not in self.log_text():
            raise LaunchFileFailure(f"{self.name} never logged: {expected}\n{self.log_text()}")

    def wait_status(self, expected):
        """Waits for the game to end; raises LaunchFileFailure unless the status is expected."""
        try:
            status = self.process.wait(timeout=LINE_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise LaunchFileFailure(f"{self.name} did not end within {LINE_TIMEOUT} s") from None
        while self._take(LINE_TIMEOUT):
            pass
        if status != expected:
            raise LaunchFileFailure(f"{self.name} exited with {status}, expected {expected}")

    def stop(self):
        """Ends the game if it still runs, and closes a pipe left open."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()

    def output(self):
        """Returns what the game has printed."""
        return "\n".join(self.lines)


def require_quiet_password(game):
    """Raises LaunchFileFailure when the game printed the file's password."""
    if "secret" in game.output() or "secret" in game.log_text():
        raise LaunchFileFailure(f"{game.name} printed the password")


def battle_room(native, game_dir, workdir, name, launch_arguments, rules=RULES, seats=None,
                piped=None, notes=()):
    """Starts a host from a launch and waits for its map, rules, seats and notes in the log."""
    port = free_port()
    user = workdir / f"{name}-user"
    user.mkdir()
    logs = workdir / f"{name}-log"
    logs.mkdir()
    game = Game(native, game_dir, workdir, name, [
        "--skip-intro", "--dplay-port", str(port), "--player-name", "Commander",
        "--user-folder", str(user), "--log-dir", str(logs), *launch_arguments,
    ], log_dir=logs, piped=piped)
    expected = [ROOM_LINE, MAP_LINE, rules] + ([seats] if seats else []) + list(notes)
    try:
        for line in expected:
            game.wait_in_log(line)
        require_quiet_password(game)
    finally:
        game.stop()
    require_quiet_password(game)
    for line in expected:
        print(f"{name}: {line}")


def check_bad_file(native, game_dir, workdir):
    """A metal value above the slider ends with status 2 and names the field."""
    path = workdir / "bad.yaml"
    path.write_text(BAD_YAML, encoding="utf-8")
    user = workdir / "bad-user"
    user.mkdir()
    game = Game(native, game_dir, workdir, "bad-file", [
        "--skip-intro", "--launch", str(path), "--user-folder", str(user),
    ])
    try:
        game.wait_status(2)
    finally:
        game.stop()
    text = game.output()
    if "rules.metal" not in text:
        raise LaunchFileFailure("bad-file did not name rules.metal")
    if "secret" in text or "secret" in game.log_text():
        raise LaunchFileFailure("bad-file printed the password")
    named = next(line for line in game.lines if "rules.metal" in line)
    print(f"bad-file: {named}")
    option_user = workdir / "bad-option-user"
    option_user.mkdir()
    option = Game(native, game_dir, workdir, "bad-option", [
        "--skip-intro", "--host", "--metal", "20000", "--user-folder", str(option_user),
    ])
    try:
        option.wait_status(2)
    finally:
        option.stop()
    if "--metal: 20000 is above 10000" not in option.output():
        raise LaunchFileFailure(f"bad-option did not name --metal:\n{option.output()}")
    print("bad-option: --metal: 20000 is above 10000")


def check_register(native, game_dir, workdir):
    """-r says this game needs no registration and ends with status 0, with no window."""
    user = workdir / "register-user"
    user.mkdir()
    game = Game(native, game_dir, workdir, "register", [
        "--user-folder", str(user), "-s", "-r", "C:\\Games\\launcher", "-c", "game.ini",
    ])
    try:
        game.wait_status(0)
    finally:
        game.stop()
    line = next((line for line in game.lines if line.startswith("-r registers")), None)
    if line is None:
        raise LaunchFileFailure(f"register did not say why it ended:\n{game.output()}")
    print(f"register: {line}")


def check_list(native, game_dir, workdir):
    """--list-content --json prints the installed maps, mods and expansions.

    Its standard input is a pipe nothing is written to, which the game
    leaves after a short wait rather than holding the start up.
    """
    user = workdir / "list-user"
    user.mkdir()
    started = time.monotonic()
    game = Game(native, game_dir, workdir, "list-content", [
        "--skip-intro", "--list-content", "--json", "--user-folder", str(user),
    ], piped="")
    try:
        game.wait_status(0)
    finally:
        game.stop()
    took = time.monotonic() - started
    if took > IDLE_PIPE_LIMIT:
        raise LaunchFileFailure(f"an idle pipe held the start up for {took:.1f} s")
    if any(line.startswith(WINDOW_LINE) for line in game.lines):
        raise LaunchFileFailure(f"list-content opened a window:\n{game.output()}")
    listed = None
    for line in game.lines:
        if line.startswith("{") and '"maps"' in line:
            listed = json.loads(line)
            break
    if not isinstance(listed, dict):
        raise LaunchFileFailure("list-content printed no JSON object")
    for key in ("maps", "mods", "expansions"):
        if not isinstance(listed.get(key), list):
            raise LaunchFileFailure(f"list-content JSON has no {key} list")
    foursome = [
        item for item in listed["maps"]
        if item.get("shown-name") == "Acid Foursome" or item.get("key") == "Acid Foursome"
    ]
    if listed["maps"] and not foursome:
        raise LaunchFileFailure("list-content omitted Acid Foursome")
    for item in foursome:
        if item.get("players") != 4 or item.get("size") != "13 x 13":
            raise LaunchFileFailure(
                f"Acid Foursome is {item.get('players')} players, size {item.get('size')!r}"
            )
    shown = foursome[0] if foursome else {"players": None, "size": None}
    print(
        f"list-content: {len(listed['maps'])} maps, {len(listed['mods'])} mods, "
        f"{len(listed['expansions'])} expansions; "
        f"Acid Foursome players {shown.get('players')} size {shown.get('size')!r}"
    )


def run_apart(native, game_dir, workdir, name, arguments):
    """Runs a game to its end, its standard output and standard error read apart.

    The game logs to a folder of its own (--log-dir). Returns the exit
    status, the lines of standard output and the text of the log.
    """
    user = workdir / f"{name}-user"
    user.mkdir()
    data = workdir / f"{name}-data"
    data.mkdir()
    logs = workdir / f"{name}-log"
    logs.mkdir()
    preferences = workdir / f"{name}.conf"
    preferences.write_text(PREFERENCES, encoding="utf-8")
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
    try:
        result = subprocess.run(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
             "--preferences-file", str(preferences), "--user-folder", str(user),
             "--data-dir", str(data), "--log-dir", str(logs), *arguments],
            cwd=workdir, env=environment, stdin=subprocess.DEVNULL, capture_output=True,
            text=True, errors="replace", timeout=LINE_TIMEOUT, check=False)
    except subprocess.TimeoutExpired:
        raise LaunchFileFailure(f"{name} did not end within {LINE_TIMEOUT} s") from None
    log = "\n".join(path.read_text(encoding="utf-8", errors="replace")
                    for path in sorted(logs.glob("*.log")))
    return result.returncode, result.stdout.splitlines(), log


def status_objects(name, lines):
    """Reads each line of standard output as one JSON object; raises LaunchFileFailure otherwise."""
    objects = []
    for line in lines:
        try:
            objects.append(json.loads(line))
        except ValueError:
            raise LaunchFileFailure(f"{name} printed a line that is no JSON object: {line}") \
                from None
    return objects


def check_list_with_log(native, game_dir, workdir):
    """--list-content --json with --log-dir prints its object on standard output, its only line."""
    status, lines, log = run_apart(native, game_dir, workdir, "list-content-log",
                                   ["--list-content", "--json"])
    if status != 0:
        raise LaunchFileFailure(f"list-content-log exited with {status}, expected 0")
    if len(lines) != 1 or not lines[0].startswith("{"):
        raise LaunchFileFailure(f"list-content-log printed {len(lines)} lines, not its object alone")
    listed = json.loads(lines[0])
    if not isinstance(listed.get("maps"), list) or not listed["maps"]:
        raise LaunchFileFailure("list-content-log listed no maps")
    if '"maps"' in log:
        raise LaunchFileFailure("list-content-log wrote its object into the log")
    if WINDOW_LINE in log:
        raise LaunchFileFailure(f"list-content-log opened a window:\n{log}")
    print(f"list-content-log: one line of standard output, {len(listed['maps'])} maps; "
          "the log holds no listing and no window")


def check_refused_launch(native, game_dir, workdir, name, text, field):
    """A launch whose map or mod is not installed ends with status 2 before a window opens,
    and its status lines end with exit, reason refused."""
    path = workdir / f"{name}.oalaunch"
    path.write_text(text, encoding="utf-8")
    status, lines, log = run_apart(native, game_dir, workdir, name,
                                   ["--dplay-port", str(free_port()), "-c", str(path)])
    if status != 2:
        raise LaunchFileFailure(f"{name} exited with {status}, expected 2")
    objects = status_objects(name, lines)
    events = [item.get("event") for item in objects]
    if events != ["hello", "exit"]:
        raise LaunchFileFailure(f"{name} wrote the status lines {events}, not hello and exit")
    if objects[1].get("reason") != "refused" or objects[1].get("code") != 2:
        raise LaunchFileFailure(f"{name} ended its status lines with {objects[1]}")
    refusal = next((line for line in log.splitlines() if f": {field}: " in line), None)
    if refusal is None:
        raise LaunchFileFailure(f"{name} did not log the refusal of {field}:\n{log}")
    if WINDOW_LINE in log:
        raise LaunchFileFailure(f"{name} opened a window before its refusal:\n{log}")
    if "secret" in log or "secret" in "\n".join(lines):
        raise LaunchFileFailure(f"{name} printed the password")
    print(f"{name}: exit 2, no window, {lines[-1]}")
    print(f"{name}: {refusal[refusal.rfind('/') + 1:]}")


STEADY_MANIFEST = """oabrain: 1
id: steady-commander
name: "Steady Commander"
version: "1.2"
author:
  name: "Ridge Testers"
description: "Holds the line."
tags: ["teaching"]
works-with: ["*"]
levels:
  - id: firm
    name: "Firm"
    difficulty: hard
"""

BRAIN_SCRIPT = "local hooks = {}\nreturn hooks\n"
LAUNCH_PUTS = "brains: the launch puts steady-commander on player"


def install_steady(user):
    """Writes steady-commander under the scratch player's Brains folder."""
    folder = user / "Brains" / "steady-commander"
    folder.mkdir(parents=True)
    (folder / "oabrain.yaml").write_text(STEADY_MANIFEST, encoding="utf-8")
    (folder / "brain.lua").write_text(BRAIN_SCRIPT, encoding="utf-8")


def check_launch_brain(native, game_dir, workdir):
    """A host option names an installed brain, and the battle room logs that it was seated."""
    port = free_port()
    user = workdir / "brain-host-user"
    user.mkdir()
    install_steady(user)
    logs = workdir / "brain-host-log"
    logs.mkdir()
    game = Game(native, game_dir, workdir, "brain-host", [
        "--skip-intro", "--dplay-port", str(port), "--player-name", "Commander",
        "--user-folder", str(user), "--log-dir", str(logs), "--host",
        "--game-name", "Example game", "--map", "Acid Foursome",
        "--computer-player", "core:2::hard:steady-commander",
    ], log_dir=logs)
    try:
        game.wait_in_log(LAUNCH_PUTS)
        game.wait_in_log(ROOM_LINE)
    finally:
        game.stop()
    if LAUNCH_PUTS not in game.log_text():
        raise LaunchFileFailure(f"brain-host did not log the launch line:\n{game.log_text()}")
    print(f"brain-host: {LAUNCH_PUTS}")


def check_missing_brain(native, game_dir, workdir):
    """A brain that is not installed ends with status 2 before a window, and the log names it."""
    status, lines, log = run_apart(native, game_dir, workdir, "missing-brain", [
        "--dplay-port", str(free_port()), "--status-stdout", "--host", "--map", "Acid Foursome",
        "--computer-player", "core:2::hard:tide-keeper",
    ])
    if status != 2:
        raise LaunchFileFailure(f"missing-brain exited with {status}, expected 2")
    objects = status_objects("missing-brain", lines)
    events = [item.get("event") for item in objects]
    if events != ["hello", "exit"]:
        raise LaunchFileFailure(f"missing-brain wrote the status lines {events}, not hello and exit")
    if objects[1].get("reason") != "refused" or objects[1].get("code") != 2:
        raise LaunchFileFailure(f"missing-brain ended its status lines with {objects[1]}")
    combined = log + "\n" + "\n".join(lines)
    if "tide-keeper is not installed" not in combined:
        raise LaunchFileFailure(f"missing-brain did not name tide-keeper:\n{combined}")
    if "--computer-player brain" not in combined:
        raise LaunchFileFailure(f"missing-brain did not name --computer-player brain:\n{combined}")
    if WINDOW_LINE in combined:
        raise LaunchFileFailure(f"missing-brain opened a window:\n{combined}")
    print(f"missing-brain: exit 2, no window, {lines[-1]}")
    print("missing-brain: tide-keeper is not installed")


def check_transport(native, game_dir, workdir):
    """-n1:127.0.0.1 alone goes from the main menu to the game list of that address.

    The game asks for games at a port the check holds and never answers, and
    runs a fixed number of frames, which the screens take far fewer of.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent:
        silent.bind((ADDRESS, 0))
        port = silent.getsockname()[1]
        status, lines, _ = run_apart(native, game_dir, workdir, "transport", [
            "--status-stdout", "--frames", "300", "--dplay-port", str(port), f"-n1:{ADDRESS}",
        ])
    if status != 0:
        raise LaunchFileFailure(f"transport exited with {status}, expected 0")
    objects = status_objects("transport", lines)
    screens = [item.get("name") for item in objects if item.get("event") == "screen"]
    expected = ["main_menu", "mp_providers", "mp_tcp", "mp_game_list"]
    if screens != expected:
        raise LaunchFileFailure(f"-n1 alone showed the screens {screens}, not {expected}")
    if not objects or objects[-1].get("event") != "exit":
        raise LaunchFileFailure("transport did not end its status lines with exit")
    print("transport: -n1:127.0.0.1 alone showed " + ", ".join(screens))


def check(native, game_dir, workdir):
    """Runs each launch-file case; raises LaunchFileFailure when one differs."""
    launch_path = workdir / "host.oalaunch"
    launch_path.write_text(YAML, encoding="utf-8")
    renamed_path = workdir / "host-settings.ini"
    renamed_path.write_text(YAML, encoding="utf-8")
    ini_path = workdir / "totala.ini"
    ini_path.write_text(INI, encoding="utf-8")
    battle_room(native, game_dir, workdir, "launch-host", ["--launch", str(launch_path)],
                LAUNCH_RULES, SEATS_LINE)
    battle_room(native, game_dir, workdir, "c-launch-host", ["-c", str(renamed_path)],
                LAUNCH_RULES, SEATS_LINE)
    battle_room(native, game_dir, workdir, "piped-host", [], LAUNCH_RULES, SEATS_LINE,
                piped=YAML)
    battle_room(native, game_dir, workdir, "options-host", OPTIONS, LAUNCH_RULES, SEATS_LINE)
    battle_room(native, game_dir, workdir, "ini-host", ["-c", str(ini_path)], notes=[INI_NOTE])
    # A path with a space in it, as a launcher's folder or a player's often
    # has, is one argument, and -c reads it whole.
    spaced = workdir / "G113 Launch Files"
    spaced.mkdir()
    spaced_launch = spaced / "host game.oalaunch"
    spaced_launch.write_text(YAML, encoding="utf-8")
    spaced_ini = spaced / "TA Forever.ini"
    spaced_ini.write_text(INI, encoding="utf-8")
    battle_room(native, game_dir, workdir, "spaced-launch-host", ["-c", str(spaced_launch)],
                LAUNCH_RULES, SEATS_LINE)
    battle_room(native, game_dir, workdir, "spaced-ini-host", ["-C" + str(spaced_ini)],
                notes=[SPACED_INI_NOTE])
    # A bare name is read from the current folder, which is the scratch folder.
    (workdir / "keys.ini").write_text(KEYS_INI, encoding="utf-8")
    battle_room(native, game_dir, workdir, "keys-host", ["-c", "keys.ini", "-b", "permlos"],
                KEYS_RULES, notes=[KEYS_NOTE])
    check_bad_file(native, game_dir, workdir)
    check_register(native, game_dir, workdir)
    check_list(native, game_dir, workdir)
    check_list_with_log(native, game_dir, workdir)
    check_refused_launch(native, game_dir, workdir, "missing-map", MISSING_MAP_YAML, "map")
    check_refused_launch(native, game_dir, workdir, "missing-mod", MISSING_MOD_YAML, "content.mod")
    check_launch_brain(native, game_dir, workdir)
    check_missing_brain(native, game_dir, workdir)
    check_transport(native, game_dir, workdir)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="G104-native-launch-file-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), Path(scratch))
        except LaunchFileFailure as failure:
            print(f"launch file check: {failure}", file=sys.stderr)
            return 1
    print("launch file check: --launch, -c, a pipe and the options opened the battle room on "
          "Acid Foursome with the launch's rules and seats, -c read TA Forever's file and a "
          "file of 3.1c's keys as 3.1c reads them, -c and -C read both from a folder whose "
          "name has a space, a bad file and a bad option exited 2, -r "
          "exited 0, --list-content --json parsed beside an idle pipe and alone on standard "
          "output beside --log-dir with no window, a missing map and a missing mod exited 2 "
          "with no window and an exit line, a host named steady-commander and the log said "
          "the launch put it on a player, tide-keeper was refused before a window, and -n1 "
          "alone opened the game list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
