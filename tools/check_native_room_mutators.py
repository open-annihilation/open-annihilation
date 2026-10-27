#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the battle room's mutator layer on two programs.

Each game runs as a program of its own through its main loop on the dummy SDL
drivers, over 127.0.0.1, at a DirectPlay port of the check's own
(--dplay-port). The host is started with --mutator on ridge-tough. A joiner
whose user folder lacks that package makes the host keep START waiting,
lacking at position 0, does not reread the Mutators folder on every frame, and
reads the host-mutator fetch state once a frame. A joiner who has the package
builds the same layer. Both programs then play a match, driven through the
automation endpoint, past the integrity report, and log the same game-data
digest. A match with no mutator logs a different digest, the same on both
machines. A mutator that changes nothing is built once. A hacks-only mutator
the plain game refuses stays built, names the dropped position, and allows
START. An installed copy named by id, with an origin record, builds the same
layer on both; the host's Mutators button counts it, and the room's chat
names it by its name, not its id, without the picker opening. Two machines whose mutators.clash-default differ,
with ridge-commander-copy (its manifest names no clash mode, and it clones
onto ARMCOM), build the same layer and play the host's mode: Add on the host
reaches the guest as Add, and a guest whose own default is Add still plays the
host's Replace. After the host starts with ridge-tough, its preferences hold
that multiplayer set and the guest's hold none. A room that never starts, and
a match with no mutator, store nothing. The host's next battle room opens the
picker on that set, with the remembered hint.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

import check_native_automation as automation

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
# Frames a joiner runs before it ends by itself: enough to join and exchange
# presence many times over.
JOIN_FRAMES = "900"
# Seconds a game may take to print a line the check waits for, or to end.
LINE_TIMEOUT = 120
# Seconds a driven match may take to pass the integrity report.
MATCH_TIMEOUT = 300
# The tick at which a disagreement would be reported. The match plays past it.
INTEGRITY_TICK = 600
BUILT = re.compile(
    r"open-annihilation: mutators: room built \d+ in \d+ ms "
    r"loadout ([0-9a-f]{16}) layer ([0-9a-f]{16}) "
    r"mutators ([0-9a-f]{16}) dropped (\d+|none)"
)
INTEGRITY = re.compile(r"multiplayer: integrity game data ([0-9a-f]{16})")
MULTIPLAYER_SET = "open-annihilation.mutators.multiplayer"
REMEMBERED_HINT = "last multiplayer set"
RESOLVE = "open-annihilation: mutators: room resolve "
FETCH_READS = re.compile(
    r"open-annihilation: host mutators: read (\d+) times in (\d+) ticks, at most (\d+) in a tick"
)
PACKAGE_FILES = ("oamut.yaml", "mutator.lua")
NOOP_LUA = (
    "-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT\n"
    "-- SPDX-License-Identifier: GPL-3.0-only\n"
    "\n"
    "function mutate(ctx)\n"
    "  return {\n"
    "    units = function(units, ctx)\n"
    "    end\n"
    "  }\n"
    "end\n"
)
# min 15 and max 5 break the profile's min <= max rule, so the hack is refused
# and the mutator fails alone. A game without a mod still resolves hacks
# against the base profile, so a value the profile accepts would apply.
# ridge-commander-copy names no clash mode and clones onto an existing unit.
RENAMED_NOTE = "ridge-commander-copy renamed ARMCOM to ARMCOM_2"
REPLACED_NOTE = "ridge-commander-copy replaced ARMCOM from the game"
HACKS_LUA = (
    "-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT\n"
    "-- SPDX-License-Identifier: GPL-3.0-only\n"
    "\n"
    "function mutate(ctx)\n"
    "  return { hacks = { [\"console.game-speed-range\"] = { min = 15, max = 5 } } }\n"
    "end\n"
)


class RoomMutatorsFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


class Game:
    """One running game, whose standard output the check reads line by line."""

    def __init__(self, native, game_dir, workdir, name, user_folder, arguments, fark_file=None):
        self.name = name
        self.lines = []
        self.fark_file = fark_file
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.preferences = workdir / f"{name}.conf"
        command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
                   "--preferences-file", str(self.preferences), "--user-folder", str(user_folder)]
        if fark_file is not None:
            fark_file.parent.mkdir(parents=True, exist_ok=True)
            command.extend(["--fark", "--fark-file", str(fark_file)])
        command.extend(arguments)
        self.process = subprocess.Popen(
            command, cwd=workdir, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace")
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

    def drain(self):
        """Takes every line already queued. An empty queue ends the drain."""
        while True:
            if self._ended:
                return
            try:
                line = self._queue.get_nowait()
            except queue.Empty:
                return
            if line is None:
                self._ended = True
                return
            self.lines.append(line)

    def wait_for(self, expected):
        """Waits for a line of the game's output; raises when it never comes."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while expected not in self.lines and time.monotonic() < deadline:
            if not self._take(0.5):
                break
        if expected not in self.lines:
            raise RoomMutatorsFailure(f"{self.name} never printed: {expected}")

    def wait_until(self, description, predicate):
        """Waits until predicate(self.lines) is true; raises when it never is."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            if predicate(self.lines):
                return
            if not self._take(0.5):
                break
        raise RoomMutatorsFailure(f"{self.name} never reached: {description}")

    def wait_end(self):
        """Waits for the game to end by itself; raises unless it ends with status 0."""
        try:
            status = self.process.wait(timeout=LINE_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise RoomMutatorsFailure(f"{self.name} did not end within {LINE_TIMEOUT} s") from None
        self.drain()
        if status != 0:
            raise RoomMutatorsFailure(f"{self.name} exited with {status}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        """Returns what the game printed, each line under its name."""
        self.drain()
        return "".join(f"{self.name}: {line}\n" for line in self.lines)


def last_match(lines, pattern):
    """Returns the last match of pattern in lines, or None."""
    found = None
    for line in lines:
        match = pattern.search(line)
        if match:
            found = match
    return found


def built_lines(lines):
    """Returns every room-built line."""
    return [line for line in lines if "open-annihilation: mutators: room built " in line]


def install_package(source, user_folder, origin_sha=None):
    """Copies ridge-tough into the user's Mutators folder.

    With origin_sha, writes the origin record an install writes, naming that
    SHA-256. Without it, the folder is the same kind of package a path on
    --mutator resolves.
    """
    package = source / "tests" / "data" / "mutators" / "packages" / "ridge-tough"
    destination = user_folder / "Mutators" / "ridge-tough"
    destination.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_FILES:
        shutil.copyfile(package / name, destination / name)
    if origin_sha is not None:
        (destination / ".oa-origin.yaml").write_text(
            "oa-origin: 1\n"
            "origin: file\n"
            f'sha256: "{origin_sha}"\n'
            'installed: "2026-01-01"\n',
            encoding="utf-8")


def package_sha256(source):
    """A SHA-256 both installs can name. It covers the package's two files."""
    package = source / "tests" / "data" / "mutators" / "packages" / "ridge-tough"
    digest = hashlib.sha256()
    for name in PACKAGE_FILES:
        digest.update((package / name).read_bytes())
    return digest.hexdigest()


def write_package(folder, package_id, layer, lua):
    """Writes a one-layer package the check's games can share."""
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "oamut.yaml").write_text(
        "oamut: 1\n"
        f"id: {package_id}\n"
        f'name: "{package_id}"\n'
        'version: "1.0"\n'
        "author:\n"
        '  name: "Ridge Maker"\n'
        'tags: ["gameplay"]\n'
        'works-with: ["*"]\n'
        f"layers: [\"{layer}\"]\n",
        encoding="utf-8")
    (folder / "mutator.lua").write_text(lua, encoding="utf-8")


def copy_package(folder, user_folder, package_id):
    """Installs a hand-written package as a folder, with no origin record."""
    destination = user_folder / "Mutators" / package_id
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(folder, destination)


def write_clash_default(path, mode):
    """Writes mutators.clash-default before the game reads its preferences."""
    path.write_text(
        "open-annihilation-preferences 1\n"
        f'"mutators.clash-default" "{mode}"\n',
        encoding="utf-8",
    )


def host_game(native, game_dir, workdir, name, user_folder, port, mutator, fark_file=None,
              clash=None):
    """Starts a host. mutator is a --mutator value, or None for a plain room."""
    user_folder.mkdir(parents=True, exist_ok=True)
    if clash is not None:
        write_clash_default(workdir / f"{name}.conf", clash)
    arguments = ["--dplay-port", str(port), "--host", "--player-name", "Hoster",
                 "--game-name", "Open Room"]
    if mutator is not None:
        arguments.extend(["--mutator", mutator])
    return Game(native, game_dir, workdir, name, user_folder, arguments, fark_file)


def joiner_game(native, game_dir, workdir, name, user_folder, port, frames=JOIN_FRAMES,
                fark_file=None, clash=None):
    """Starts a joiner. frames None keeps it until the check quits it."""
    user_folder.mkdir(parents=True, exist_ok=True)
    if clash is not None:
        write_clash_default(workdir / f"{name}.conf", clash)
    arguments = ["--dplay-port", str(port), "--join", ADDRESS, "--player-name", "Visitor"]
    if frames is not None:
        arguments.extend(["--frames", frames])
    return Game(native, game_dir, workdir, name, user_folder, arguments, fark_file)


def open_client(game):
    """Connects to the game's automation endpoint and says hello."""
    if game.fark_file is None:
        raise RoomMutatorsFailure(f"{game.name} has no automation endpoint")
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline:
        game.drain()
        if game.fark_file.is_file():
            break
        if game.process.poll() is not None:
            raise RoomMutatorsFailure(f"{game.name} ended before it served automation")
        time.sleep(0.1)
    else:
        raise RoomMutatorsFailure(f"{game.name} wrote no automation file")
    endpoint = json.loads(game.fark_file.read_text(encoding="utf-8"))
    client = automation.Client(endpoint["address"])
    hello = client.request("hello", versions=[1], client="room-mutators", token=endpoint["token"])
    if not hello.get("ok"):
        raise RoomMutatorsFailure(f"{game.name} answered hello with {hello}")
    return client


def control_named(client, name):
    """Returns the shown control of that name, or nothing."""
    for control in client.request("controls").get("controls", []):
        if control.get("name", "").lower() == name.lower():
            return control
    return None


def wait_mutators(game, count, name, mutator_id):
    """Waits until the game's Mutators button counts the room's set and the
    room's chat names it.

    The button reads "Mutators"; its automation text counts the set, "Mutators
    · N on" for N mutators on. The line the room posts when its list is turned
    on names each by its name, never by its id. Returns the button's text and
    that line.
    """
    client = open_client(game)
    text = None
    turned_on = []
    try:
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            control = control_named(client, "oa.mutators")
            text = control.get("text") if control is not None else None
            chat = client.request("room").get("chat") or []
            turned_on = [line for line in chat if "turned on mutators" in line]
            named = [line for line in turned_on if name in line and mutator_id not in line]
            if text == f"Mutators · {count} on" and named:
                return text, named[-1]
            time.sleep(0.1)
    finally:
        client.close()
    raise RoomMutatorsFailure(
        f"{game.name}'s Mutators button reads {text!r}, not 'Mutators · {count} on', or its "
        f"turned-on lines {turned_on!r} do not name {name} by its name")


def room_picture(client):
    """The seated players and START, for a failure that never leaves the room."""
    room = client.request("room")
    players = [
        {"slot": player.get("slot"), "ready": player.get("ready"), "team": player.get("team")}
        for player in room.get("players") or []
    ]
    start = control_named(client, "START")
    shown = {
        "enabled": start.get("enabled"), "visible": start.get("visible")
    } if start is not None else None
    return {"players": players, "start": shown, "chat": (room.get("chat") or [])[-4:]}


def wait_enabled(client, name, screen):
    """Waits until a control is shown and enabled."""
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline:
        shown = client.request("screen")
        if shown.get("screen") == screen:
            control = control_named(client, name)
            if control is not None and control.get("enabled"):
                return
        time.sleep(0.1)
    raise RoomMutatorsFailure(f"{screen} never enabled {name}: {room_picture(client)}")


def click_ready(client):
    """Clicks this machine's own READY and waits until its player is ready."""
    room = client.request("room")
    name = f"READY{room.get('local_slot')}"
    wait_enabled(client, name, "mp_battleroom")
    automation.click_control(client, name, "mp_battleroom")
    deadline = time.monotonic() + LINE_TIMEOUT
    slot = room.get("local_slot")
    while time.monotonic() < deadline:
        pictured = client.request("room")
        for player in pictured.get("players") or []:
            if player.get("slot") == slot and player.get("ready"):
                return
        time.sleep(0.1)
    raise RoomMutatorsFailure(f"{name} did not ready the local player: {room_picture(client)}")


def play_match(host, visitor):
    """Presses each machine's READY and the host's START, and plays past the report.

    START stays gray until every seated player, the host included, is ready.
    """
    host_client = open_client(host)
    guest_client = open_client(visitor)
    try:
        automation.wait_screen(guest_client, "mp_battleroom")
        automation.wait_screen(host_client, "mp_battleroom")
        click_ready(guest_client)
        click_ready(host_client)
        wait_enabled(host_client, "START", "mp_battleroom")
        automation.click_control(host_client, "START", "mp_battleroom")
        deadline = time.monotonic() + MATCH_TIMEOUT
        room_deadline = time.monotonic() + LINE_TIMEOUT
        host_tick = 0
        guest_tick = 0
        host_screen = {}
        guest_screen = {}
        while time.monotonic() < deadline and (host_tick <= INTEGRITY_TICK or
                                               guest_tick <= INTEGRITY_TICK):
            host.drain()
            visitor.drain()
            host_screen = host_client.request("screen")
            guest_screen = guest_client.request("screen")
            host_tick = host_screen.get("tick") or 0
            guest_tick = guest_screen.get("tick") or 0
            host_name = host_screen.get("screen")
            guest_name = guest_screen.get("screen")
            if host_name == "mp_battleroom" and time.monotonic() > room_deadline:
                raise RoomMutatorsFailure(
                    f"START left the host in the battle room: {room_picture(host_client)}"
                )
            if host_name == "main_menu" or guest_name == "main_menu":
                host.drain()
                visitor.drain()
                raise RoomMutatorsFailure(
                    f"the match returned to the menu on {host_name} and {guest_name}"
                )
            time.sleep(0.2)
        if host_tick <= INTEGRITY_TICK or guest_tick <= INTEGRITY_TICK:
            raise RoomMutatorsFailure(
                f"the match reached tick {host_tick} and {guest_tick} on "
                f"{host_screen.get('screen')} and {guest_screen.get('screen')}, "
                f"not past {INTEGRITY_TICK}"
            )
        host.drain()
        visitor.drain()
        for game in (host, visitor):
            if any("reports VerCheck issues" in line for line in game.lines):
                raise RoomMutatorsFailure(f"{game.name} reported VerCheck issues")
        host_client.request("quit")
        guest_client.request("quit")
    finally:
        host_client.close()
        guest_client.close()
    host.wait_end()
    visitor.wait_end()


def require_same_build(host, visitor):
    """Returns the two room-built matches when they name the same digests."""
    host_built = last_match(host.lines, BUILT)
    guest_built = last_match(visitor.lines, BUILT)
    if host_built is None or guest_built is None or host_built.groups() != guest_built.groups():
        raise RoomMutatorsFailure(
            "the host and the joiner did not log the same loadout, layer and mutator digests"
        )
    return host_built, guest_built


def confirm_clash_mode(host, visitor, note, other):
    """Starts the match and waits until both machines log the host's clash mode.

    `note` is the line the host's mode writes. `other` is the line the other
    mode would write. Both machines must log `note` and neither may log `other`.
    """
    host_client = open_client(host)
    guest_client = open_client(visitor)
    try:
        automation.wait_screen(guest_client, "mp_battleroom")
        automation.wait_screen(host_client, "mp_battleroom")
        click_ready(guest_client)
        click_ready(host_client)
        wait_enabled(host_client, "START", "mp_battleroom")
        automation.click_control(host_client, "START", "mp_battleroom")
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            host.drain()
            visitor.drain()
            if any(other in line for line in host.lines + visitor.lines):
                raise RoomMutatorsFailure(f"a machine played the other clash mode: {other}")
            if (any(note in line for line in host.lines) and
                    any(note in line for line in visitor.lines)):
                host_client.request("quit")
                guest_client.request("quit")
                break
            time.sleep(0.2)
        else:
            raise RoomMutatorsFailure(f"the match did not log the host's clash mode: {note}")
    finally:
        host_client.close()
        guest_client.close()
    host.wait_end()
    visitor.wait_end()


def preferences_text(path):
    """The preferences file's text, or empty when the game wrote none."""
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8")


def require_remembered(path):
    """The host's preferences name ridge-tough as the multiplayer set."""
    text = preferences_text(path)
    if MULTIPLAYER_SET not in text or "ridge-tough" not in text:
        raise RoomMutatorsFailure(f"{path.name} does not hold the multiplayer set ridge-tough")


def require_not_remembered(path, why):
    """The preferences file does not hold a multiplayer set."""
    if MULTIPLAYER_SET in preferences_text(path):
        raise RoomMutatorsFailure(why)


def open_remembered_picker(native, game_dir, workdir, source, preferences, user_folder):
    """Starts another battle room on the host's preferences and opens the picker.

    The room has no mutator on the command line, so the picker offers the set
    the last start stored, with the remembered hint, and ridge-tough is on.
    """
    install_package(source, user_folder)
    port = free_port()
    shutil.copyfile(preferences, workdir / "remembered-host.conf")
    host = host_game(
        native, game_dir, workdir, "remembered-host", user_folder, port, None,
        workdir / "remembered-host-fark" / "automation.json")
    try:
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        client = open_client(host)
        try:
            automation.wait_screen(client, "mp_battleroom")
            wait_enabled(client, "oa.mutators", "mp_battleroom")
            automation.click_control(client, "oa.mutators", "mp_battleroom")
            deadline = time.monotonic() + LINE_TIMEOUT
            hint = None
            switch = None
            while time.monotonic() < deadline:
                hint = control_named(client, "oa.mutators.hint")
                switch = control_named(client, "oa.mutators.row.ridge-tough.switch.on")
                if (hint is not None and REMEMBERED_HINT in (hint.get("text") or "") and
                        switch is not None and switch.get("checked")):
                    break
                time.sleep(0.1)
            else:
                shown = hint.get("text") if hint is not None else None
                raise RoomMutatorsFailure(
                    "the next battle room did not open the picker on the remembered "
                    f"multiplayer set ({shown})"
                )
            client.request("quit")
        finally:
            client.close()
        host.wait_end()
    except Exception:
        host.stop()
        print(host.output(), end="")
        raise
    finally:
        host.stop()


def require_same_integrity(host, visitor):
    """Returns the shared game-data digest."""
    host_integrity = last_match(host.lines, INTEGRITY)
    guest_integrity = last_match(visitor.lines, INTEGRITY)
    if (host_integrity is None or guest_integrity is None or
            host_integrity.group(1) != guest_integrity.group(1)):
        raise RoomMutatorsFailure("the host and the joiner did not log the same integrity digest")
    return host_integrity.group(1)


def check(native, game_dir, workdir, source):
    """Runs the rooms; raises RoomMutatorsFailure when a game differs.

    Returns the lines the success report prints.
    """
    games = []
    evidence = []
    try:
        port = free_port()
        host = host_game(native, game_dir, workdir, "lacking-host", workdir / "lacking-host-user",
                         port, str(source / "tests" / "data" / "mutators" / "packages" /
                                   "ridge-tough"))
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        host.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        joined_at = time.monotonic()
        lacking = joiner_game(native, game_dir, workdir, "lacking-joiner",
                              workdir / "lacking-joiner-user", port)
        games.append(lacking)
        lacking.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        host.wait_until(
            "START held because the joiner lacks position 0",
            lambda lines: any(
                "mutators hold START: lacking" in line and "position 0" in line for line in lines
            ),
        )
        lacking.wait_end()
        host.stop()
        lacking.drain()
        # The guest keeps a failed resolve for a second, so it resolves about
        # once a second while it lacks the mutator, however slowly the build
        # runs; once a frame would be many times more.
        seconds = time.monotonic() - joined_at
        resolves = sum(1 for line in lacking.lines if RESOLVE in line)
        if resolves == 0 or resolves > int(seconds) + 5:
            raise RoomMutatorsFailure(
                f"the joiner resolved the missing mutator {resolves} times in {seconds:.0f} s"
            )
        # One read a frame. The joiner counts its reads in a line a second;
        # a tick that read twice without a press means the fetch ran again
        # inside a frame.
        counts = [
            tuple(int(value) for value in found.groups())
            for found in (FETCH_READS.search(line) for line in lacking.lines)
            if found is not None
        ]
        reads = sum(count[0] for count in counts)
        ticks = sum(count[1] for count in counts)
        most = max((count[2] for count in counts), default=0)
        if reads == 0 or most > 1 or reads > ticks:
            raise RoomMutatorsFailure(
                f"the joiner read the host's mutators {reads} times in {ticks} ticks, "
                f"at most {most} in a tick"
            )
        hold = next(line for line in host.lines if "mutators hold START: lacking" in line)
        evidence.append(hold)
        evidence.append(
            f"open-annihilation: host mutators: read {reads} times in {ticks} ticks, "
            f"at most {most} in a tick"
        )
        require_not_remembered(
            host.preferences, "a battle room that never started stored the multiplayer set"
        )
        require_not_remembered(
            lacking.preferences, "a guest that never started stored the multiplayer set"
        )

        port = free_port()
        built_host_user = workdir / "built-host-user"
        built_joiner_user = workdir / "built-joiner-user"
        install_package(source, built_joiner_user)
        host = host_game(
            native, game_dir, workdir, "built-host", built_host_user, port,
            str(source / "tests" / "data" / "mutators" / "packages" / "ridge-tough"),
            workdir / "built-host-fark" / "automation.json")
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        host.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        visitor = joiner_game(
            native, game_dir, workdir, "built-joiner", built_joiner_user, port, frames=None,
            fark_file=workdir / "built-joiner-fark" / "automation.json")
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        visitor.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        host.wait_until(
            "START allowed after the joiner built",
            lambda lines: (
                (mutator_lines := [line for line in lines if line.startswith("multiplayer: mutators ")])
                and mutator_lines[-1] == "multiplayer: mutators allow START"
            ),
        )
        host.wait_until(
            "unit sync complete",
            lambda lines: any(line == "multiplayer: unit sync OK" for line in lines),
        )
        host_built, guest_built = require_same_build(host, visitor)
        evidence.append(host_built.group(0))
        evidence.append(guest_built.group(0))
        play_match(host, visitor)
        mutated = require_same_integrity(host, visitor)
        evidence.append(f"multiplayer: integrity game data {mutated}")
        evidence.append(f"multiplayer: integrity game data {mutated}")
        require_remembered(host.preferences)
        require_not_remembered(
            visitor.preferences, "the guest's preferences hold the multiplayer set"
        )
        remembered = host.preferences
        remembered_user = built_host_user
        open_remembered_picker(native, game_dir, workdir, source, remembered, remembered_user)
        evidence.append("mutator room check: the next battle room offers the remembered set")

        port = free_port()
        host = host_game(
            native, game_dir, workdir, "plain-host", workdir / "plain-host-user", port, None,
            workdir / "plain-host-fark" / "automation.json")
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        visitor = joiner_game(
            native, game_dir, workdir, "plain-joiner", workdir / "plain-joiner-user", port,
            frames=None, fark_file=workdir / "plain-joiner-fark" / "automation.json")
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        play_match(host, visitor)
        plain = require_same_integrity(host, visitor)
        if plain == mutated:
            raise RoomMutatorsFailure(
                "a match with no mutator logged the same game-data digest as ridge-tough"
            )
        require_not_remembered(
            host.preferences, "a match with no mutator stored the multiplayer set"
        )
        require_not_remembered(
            visitor.preferences, "a guest of a match with no mutator stored the multiplayer set"
        )
        evidence.append(f"multiplayer: integrity game data {plain}")
        evidence.append(f"multiplayer: integrity game data {plain}")

        noop = workdir / "packages" / "noop-units"
        write_package(noop, "noop-units", "units", NOOP_LUA)
        port = free_port()
        noop_joiner = workdir / "noop-joiner-user"
        copy_package(noop, noop_joiner, "noop-units")
        host = host_game(native, game_dir, workdir, "noop-host", workdir / "noop-host-user", port,
                         str(noop))
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        visitor = joiner_game(native, game_dir, workdir, "noop-joiner", noop_joiner, port)
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        visitor.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        host.wait_until(
            "START allowed for a mutator that changes nothing",
            lambda lines: (
                (mutator_lines := [line for line in lines if line.startswith("multiplayer: mutators ")])
                and mutator_lines[-1] == "multiplayer: mutators allow START"
            ),
        )
        visitor.wait_end()
        host.stop()
        host.drain()
        visitor.drain()
        if len(built_lines(host.lines)) != 1 or len(built_lines(visitor.lines)) != 1:
            raise RoomMutatorsFailure(
                "a mutator that changes nothing was built "
                f"{len(built_lines(host.lines))} and {len(built_lines(visitor.lines))} times"
            )
        evidence.extend(built_lines(host.lines))
        evidence.extend(built_lines(visitor.lines))

        hacks = workdir / "packages" / "hacks-only"
        write_package(hacks, "hacks-only", "hacks", HACKS_LUA)
        port = free_port()
        hacks_joiner = workdir / "hacks-joiner-user"
        copy_package(hacks, hacks_joiner, "hacks-only")
        host = host_game(native, game_dir, workdir, "hacks-host", workdir / "hacks-host-user",
                         port, str(hacks))
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        visitor = joiner_game(native, game_dir, workdir, "hacks-joiner", hacks_joiner, port)
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        visitor.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        host.wait_until(
            "START allowed after a refused hack",
            lambda lines: (
                (mutator_lines := [line for line in lines if line.startswith("multiplayer: mutators ")])
                and mutator_lines[-1] == "multiplayer: mutators allow START"
            ),
        )
        host_built, guest_built = require_same_build(host, visitor)
        if host_built.group(4) != "0":
            raise RoomMutatorsFailure(
                f"the refused hack was dropped at {host_built.group(4)}, not position 0"
            )
        visitor.wait_end()
        host.stop()
        evidence.append(host_built.group(0))
        evidence.append(guest_built.group(0))

        origin = package_sha256(source)
        port = free_port()
        id_host_user = workdir / "id-host-user"
        id_joiner_user = workdir / "id-joiner-user"
        install_package(source, id_host_user, origin)
        install_package(source, id_joiner_user, origin)
        host = host_game(native, game_dir, workdir, "id-host", id_host_user, port, "ridge-tough",
                         workdir / "id-host-fark" / "automation.json")
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
        host.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        visitor = joiner_game(native, game_dir, workdir, "id-joiner", id_joiner_user, port)
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
        visitor.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
        host.wait_until(
            "START allowed for an installed id",
            lambda lines: (
                (mutator_lines := [line for line in lines if line.startswith("multiplayer: mutators ")])
                and mutator_lines[-1] == "multiplayer: mutators allow START"
            ),
        )
        host_built, guest_built = require_same_build(host, visitor)
        # The host never opened the picker, yet its button counts the
        # installed copy and its room's chat names it by its name, not by
        # the id it was started with.
        button_text, turned_on = wait_mutators(host, 1, "Ridge Tough", "ridge-tough")
        visitor.wait_end()
        host.stop()
        evidence.append(host_built.group(0))
        evidence.append(guest_built.group(0))
        evidence.append(
            f"mutator room check: the host's Mutators button after --mutator counts "
            f"{button_text!r}, and its chat {turned_on!r}")

        commander = source / "tests" / "data" / "mutators" / "packages" / "ridge-commander-copy"
        for name, host_mode, guest_mode, note, other in (
            ("clash-add", "add", "replace", RENAMED_NOTE, REPLACED_NOTE),
            ("clash-replace", "replace", "add", REPLACED_NOTE, RENAMED_NOTE),
        ):
            port = free_port()
            host_user = workdir / f"{name}-host-user"
            guest_user = workdir / f"{name}-joiner-user"
            copy_package(commander, host_user, "ridge-commander-copy")
            copy_package(commander, guest_user, "ridge-commander-copy")
            host = host_game(
                native, game_dir, workdir, f"{name}-host", host_user, port,
                "ridge-commander-copy", workdir / f"{name}-host-fark" / "automation.json",
                clash=host_mode)
            games.append(host)
            host.wait_for('multiplayer: in the battle room of "Open Room" as Hoster')
            host.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
            visitor = joiner_game(
                native, game_dir, workdir, f"{name}-joiner", guest_user, port, frames=None,
                fark_file=workdir / f"{name}-joiner-fark" / "automation.json", clash=guest_mode)
            games.append(visitor)
            visitor.wait_for('multiplayer: in the battle room of "Open Room" as Visitor')
            visitor.wait_until("room built", lambda lines: last_match(lines, BUILT) is not None)
            host.wait_until(
                f"START allowed when the host's clash mode is {host_mode}",
                lambda lines: (
                    (mutator_lines := [
                        line for line in lines if line.startswith("multiplayer: mutators ")
                    ]) and mutator_lines[-1] == "multiplayer: mutators allow START"
                ),
            )
            host_built, guest_built = require_same_build(host, visitor)
            evidence.append(host_built.group(0))
            evidence.append(guest_built.group(0))
            confirm_clash_mode(host, visitor, note, other)
            evidence.append(note)
            evidence.append(note)
    except RoomMutatorsFailure:
        for game in games:
            game.stop()
            print(game.output(), end="")
        raise
    finally:
        for game in games:
            game.stop()
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True, help="the engine source tree")
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    # The automation helper lives beside this script.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    with tempfile.TemporaryDirectory(prefix="native-net-room-mutators-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            evidence = check(args.native.resolve(), args.game_dir.resolve(), Path(scratch),
                             args.source.resolve())
        except RoomMutatorsFailure as failure:
            print(f"room mutators check: {failure}", file=sys.stderr)
            return 1
    for line in evidence:
        print(line)
    print("room mutators check: a joiner without ridge-tough held START at position 0, and a "
          "joiner with it built the same loadout and layer; the host then allowed START, both "
          "integrity lines matched, and a match with no mutator logged a different digest; "
          "two machines whose clash defaults differ played the host's mode, and a host set "
          "to Add reached the guest as Add; the host's preferences held ridge-tough "
          "and the next battle room offered that set, and the guest's held none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
