#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the battle room's Enhanced Mode on three programs.

Each game runs as a program of its own through its main loop on the dummy SDL
drivers, over 127.0.0.1, at a DirectPlay port of the check's own
(--dplay-port), driven through the automation endpoint.

1. A host with Enhanced Mode on in its Settings, so its switch starts on, and
   two guests, one with Enhanced Mode on in its Settings and one with it off.
   Every machine plays Revision 1 and posts "*** Enhanced Mode: Revision 1."
   once, written whole to its log; the host's switch reads "Enhanced Mode ·
   Revision 1" and each guest's is read-only. START waits until every hash
   matches, the match plays past the integrity report with one game-data
   digest on all three, and both guests' recordings (--net-record) carry the
   Enhanced Mode sector of Revision 1, the same as a skirmish played with
   Enhanced Mode on.
2. A second room: a host with the switch on and one guest play Revision 1.
   A guest started with --presence-without-enhanced-mode, which sends no
   revision as Open Annihilation 0.9.2 does, joins: the room goes Off and
   both machines post "*** Enhanced Mode is off: Older's game doesn't have
   it." once. It leaves, and the room returns to Revision 1 with one line.
   It joins again, and START plays Off: the match passes the integrity
   report on all three and the guest's recording has no Enhanced Mode
   sector.

The scratch folder is removed whatever happens, so a second run starts as the
first did.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time

import check_native_automation as automation
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
HOST_BIND_ADDRESS = "0.0.0.0"
# Seconds a game may take to print a line the check waits for, or to end.
LINE_TIMEOUT = 120
# Seconds a driven match may take to pass the integrity report.
MATCH_TIMEOUT = 300
# The tick at which a disagreement would be reported. The match plays past it.
INTEGRITY_TICK = 600
INTEGRITY = re.compile(r"multiplayer: integrity game data ([0-9a-f]{16})")
PLAYS = "multiplayer: Enhanced Mode: the room plays "
REVISION_LINE = "*** Enhanced Mode: Revision 1."
OFF_LINE = "*** Enhanced Mode is off: Older's game doesn't have it."
ROOM = 'multiplayer: in the battle room of "Open Room" as '
# The recording sector that holds Enhanced Mode: its type word.
ENHANCED_SECTOR = 0x4F414531

OFFLINE_PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"\n'
)


class RoomEnhancedFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a free UDP port using the native host's wildcard bind."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((HOST_BIND_ADDRESS, 0))
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
        self.preferences.write_text(OFFLINE_PREFERENCES, encoding="utf-8")
        user_folder = workdir / f"{name}-user"
        user_folder.mkdir(parents=True, exist_ok=True)
        data_dir = workdir / f"{name}.data"
        data_dir.mkdir(exist_ok=True)
        self.fark_file = workdir / f"{name}-fark" / "automation.json"
        self.fark_file.parent.mkdir(parents=True, exist_ok=True)
        command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
                   "--preferences-file", str(self.preferences), "--user-folder", str(user_folder),
                   "--data-dir", str(data_dir), "--fark", "--fark-file", str(self.fark_file),
                   *arguments]
        self.process = subprocess.Popen(
            command, cwd=workdir, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def _take(self, timeout):
        """Moves the next line read into self.lines; False once the output has ended."""
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
        """Takes every line already queued."""
        while not self._ended:
            try:
                line = self._queue.get_nowait()
            except queue.Empty:
                return
            if line is None:
                self._ended = True
                return
            self.lines.append(line)

    def wait_until(self, description, predicate):
        """Waits until predicate(self.lines) is true; raises when it never is."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            if predicate(self.lines):
                return
            if not self._take(0.5):
                break
        raise RoomEnhancedFailure(f"{self.name} never reached: {description}")

    def wait_for(self, expected):
        """Waits for a line of the game's output."""
        self.wait_until(expected, lambda lines: expected in lines)

    def count(self, line):
        """How many times the game wrote this line whole."""
        self.drain()
        return sum(1 for seen in self.lines if seen == line)

    def plays(self):
        """The last revision the room said this machine plays, or None."""
        self.drain()
        found = [line[len(PLAYS):] for line in self.lines if line.startswith(PLAYS)]
        return found[-1] if found else None

    def wait_end(self):
        """Waits for the game to end by itself; raises unless it ends with status 0."""
        try:
            status = self.process.wait(timeout=LINE_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise RoomEnhancedFailure(f"{self.name} did not end within {LINE_TIMEOUT} s") from None
        self.drain()
        if status != 0:
            raise RoomEnhancedFailure(f"{self.name} exited with {status}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        """Returns what the game printed, each line under its name."""
        self.drain()
        return "".join(f"{self.name}: {line}\n" for line in self.lines)


def host_game(native, game_dir, workdir, name, port):
    """Starts a host whose Settings turn Enhanced Mode on."""
    return Game(native, game_dir, workdir, name,
                ["--dplay-port", str(port), "--host", "--player-name", "Hoster",
                 "--game-name", "Open Room", "--enhanced-mode"])


def joiner_game(native, game_dir, workdir, name, port, player, extra):
    """Starts a joiner that stays until the check quits it."""
    return Game(native, game_dir, workdir, name,
                ["--dplay-port", str(port), "--join", ADDRESS, "--player-name", player, *extra])


def open_client(game):
    """Connects to the game's automation endpoint and says hello."""
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline:
        game.drain()
        if game.fark_file.is_file():
            break
        if game.process.poll() is not None:
            raise RoomEnhancedFailure(f"{game.name} ended before it served automation")
        time.sleep(0.1)
    else:
        raise RoomEnhancedFailure(f"{game.name} wrote no automation file")
    endpoint = json.loads(game.fark_file.read_text(encoding="utf-8"))
    client = automation.Client(endpoint["address"])
    hello = client.request("hello", versions=[1], client="room-enhanced", token=endpoint["token"])
    if not hello.get("ok"):
        raise RoomEnhancedFailure(f"{game.name} answered hello with {hello}")
    return client


def control_named(client, name):
    """Returns the shown control of that name, or nothing."""
    for control in client.request("controls").get("controls", []):
        if control.get("name", "").lower() == name.lower():
            return control
    return None


def room_picture(client):
    """The seated players and START, for a failure that never leaves the room."""
    room = client.request("room")
    players = [
        {"slot": player.get("slot"), "ready": player.get("ready")}
        for player in room.get("players") or []
    ]
    start = control_named(client, "START")
    shown = {"enabled": start.get("enabled")} if start is not None else None
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
    raise RoomEnhancedFailure(f"{screen} never enabled {name}: {room_picture(client)}")


def wait_switch(game, text, enabled):
    """Waits until the game's Enhanced Mode switch reads `text`, enabled or read-only."""
    client = open_client(game)
    seen = None
    try:
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            control = control_named(client, "oa.enhanced-mode")
            if control is not None:
                seen = (control.get("text"), bool(control.get("enabled")))
                if seen == (text, enabled):
                    return
            time.sleep(0.1)
    finally:
        client.close()
    raise RoomEnhancedFailure(
        f"{game.name}'s switch reads {seen}, not {(text, enabled)}")


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
    raise RoomEnhancedFailure(f"{name} did not ready the local player: {room_picture(client)}")


def play_match(host, guests):
    """Readies every machine, presses the host's START, and plays past the report.

    START stays gray until every seated player is ready and every machine's
    rules are the host's.
    """
    games = [host, *guests]
    clients = [open_client(game) for game in games]
    try:
        for client in clients:
            automation.wait_screen(client, "mp_battleroom")
        for client in clients[1:]:
            click_ready(client)
        click_ready(clients[0])
        wait_enabled(clients[0], "START", "mp_battleroom")
        automation.click_control(clients[0], "START", "mp_battleroom")
        deadline = time.monotonic() + MATCH_TIMEOUT
        room_deadline = time.monotonic() + LINE_TIMEOUT
        ticks = [0] * len(games)
        names = [None] * len(games)
        while time.monotonic() < deadline and min(ticks) <= INTEGRITY_TICK:
            for at, (game, client) in enumerate(zip(games, clients)):
                game.drain()
                shown = client.request("screen")
                ticks[at] = shown.get("tick") or 0
                names[at] = shown.get("screen")
            if names[0] == "mp_battleroom" and time.monotonic() > room_deadline:
                raise RoomEnhancedFailure(
                    f"START left the host in the battle room: {room_picture(clients[0])}")
            if "main_menu" in names:
                raise RoomEnhancedFailure(f"the match returned to the menu on {names}")
            time.sleep(0.2)
        if min(ticks) <= INTEGRITY_TICK:
            raise RoomEnhancedFailure(
                f"the match reached ticks {ticks} on {names}, not past {INTEGRITY_TICK}")
        for game in games:
            game.drain()
            if any("reports VerCheck issues" in line for line in game.lines):
                raise RoomEnhancedFailure(f"{game.name} reported VerCheck issues")
        for client in clients:
            client.request("quit")
    finally:
        for client in clients:
            client.close()
    for game in games:
        game.wait_end()


def same_integrity(games):
    """Returns the shared game-data digest of every machine."""
    digests = []
    for game in games:
        found = [INTEGRITY.search(line) for line in game.lines]
        found = [match.group(1) for match in found if match is not None]
        if not found:
            raise RoomEnhancedFailure(f"{game.name} logged no integrity digest")
        digests.append(found[-1])
    if len(set(digests)) != 1:
        raise RoomEnhancedFailure(f"the machines logged different integrity digests {digests}")
    return digests[0]


def enhanced_sectors(path):
    """Reads every Enhanced Mode sector of a recording: revision, feature bits, needed version."""
    with path.open("rb") as stream:
        def chunk():
            length_bytes = stream.read(2)
            length = int.from_bytes(length_bytes, "little")
            if len(length_bytes) != 2 or length < 2:
                raise RoomEnhancedFailure(f"{path.name}: invalid recording chunk length")
            body = stream.read(length - 2)
            if len(body) != length - 2:
                raise RoomEnhancedFailure(f"{path.name}: truncated recording chunk")
            return body

        if not chunk().startswith(b"TA Demo\0"):
            raise RoomEnhancedFailure(f"{path.name}: missing recording header")
        count_bytes = chunk()
        if len(count_bytes) != 4:
            raise RoomEnhancedFailure(f"{path.name}: invalid sector count")
        count = int.from_bytes(count_bytes, "little")
        if count > 256:
            raise RoomEnhancedFailure(f"{path.name}: too many recording sectors")
        found = []
        for _ in range(count):
            sector = chunk()
            if len(sector) < 4:
                raise RoomEnhancedFailure(f"{path.name}: missing sector type")
            if int.from_bytes(sector[:4], "little") == ENHANCED_SECTOR:
                if len(sector) != 20:
                    raise RoomEnhancedFailure(f"{path.name}: invalid Enhanced Mode sector")
                body = sector[4:]
                found.append((
                    int.from_bytes(body[0:4], "little"),
                    int.from_bytes(body[4:12], "little"),
                    int.from_bytes(body[12:16], "little"),
                ))
        return found


def wait_file(game, path):
    """Waits for a recording the game wrote."""
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline and not path.is_file():
        time.sleep(0.2)
    if not path.is_file():
        raise RoomEnhancedFailure(f"{game.name} wrote no recording at {path.name}")


def revision_one_sector(native, game_dir, workdir):
    """The Enhanced Mode sector a skirmish played with Enhanced Mode on records."""
    recording = workdir / "skirmish-revision.tad"
    preferences = workdir / "skirmish.conf"
    preferences.write_text(OFFLINE_PREFERENCES, encoding="utf-8")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--enhanced-mode", "--give-orders", "--match-ticks", "60",
         "--record-game", str(recording), "--preferences-file", str(preferences),
         "--user-folder", str(workdir / "skirmish-user")],
        cwd=workdir, env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                              SDL_RENDER_DRIVER="software"),
        capture_output=True, text=True, errors="replace", timeout=600, check=False)
    if result.returncode != 0 or not recording.is_file():
        raise RoomEnhancedFailure(
            "the Enhanced Mode skirmish was not recorded\n" + result.stdout + result.stderr)
    sectors = enhanced_sectors(recording)
    if len(sectors) != 1 or sectors[0][0] != 1:
        raise RoomEnhancedFailure(f"the Enhanced Mode skirmish recorded {sectors}")
    return sectors[0]


def check(native, game_dir, workdir):
    """Runs both rooms; raises RoomEnhancedFailure when a game differs.

    Returns the lines the success report prints.
    """
    games = []
    evidence = []
    try:
        expected = revision_one_sector(native, game_dir, workdir)
        evidence.append(f"Enhanced Mode skirmish sector: revision {expected[0]}, "
                        f"features 0x{expected[1]:x}, needs 0x{expected[2]:06x}")

        # 1. Settings On and Off on the guests: every machine plays Revision 1.
        port = free_port()
        host = host_game(native, game_dir, workdir, "host", port)
        games.append(host)
        host.wait_for(ROOM + "Hoster")
        host.wait_for(PLAYS + "Revision 1")
        recordings = {}
        guests = []
        for name, player, extra in (
            ("guest-on", "Visitor", ["--enhanced-mode"]),
            ("guest-off", "Ridge", ["--no-enhanced-mode"]),
        ):
            recordings[name] = workdir / f"{name}.tad"
            guest = joiner_game(native, game_dir, workdir, name, port, player,
                                [*extra, "--net-record", str(recordings[name])])
            games.append(guest)
            guests.append(guest)
            guest.wait_for(ROOM + player)
            guest.wait_until("Revision 1", lambda lines: any(
                line == PLAYS + "Revision 1" for line in lines))
        wait_switch(host, "Enhanced Mode · Revision 1", True)
        for guest in guests:
            wait_switch(guest, "Enhanced Mode · Revision 1", False)
        for game in (host, *guests):
            if game.count(REVISION_LINE) != 1:
                raise RoomEnhancedFailure(
                    f"{game.name} wrote {REVISION_LINE!r} {game.count(REVISION_LINE)} times")
            evidence.append(f"{game.name}: {REVISION_LINE} (once); {PLAYS}{game.plays()}")
        play_match(host, guests)
        digest = same_integrity([host, *guests])
        evidence.append(f"multiplayer: integrity game data {digest} on all three")
        for game in (host, *guests):
            if game.plays() != "Revision 1":
                raise RoomEnhancedFailure(f"{game.name} played {game.plays()}, not Revision 1")
        for guest in guests:
            wait_file(guest, recordings[guest.name])
            sectors = enhanced_sectors(recordings[guest.name])
            if sectors != [expected]:
                raise RoomEnhancedFailure(
                    f"{guest.name}'s recording holds {sectors}, not Revision 1's {expected}")
            evidence.append(f"{guest.name}'s recording: revision {sectors[0][0]}, "
                            f"features 0x{sectors[0][1]:x}, needs 0x{sectors[0][2]:06x}")

        # 2. An older build turns the room Off, and its leaving brings it back.
        port = free_port()
        host = host_game(native, game_dir, workdir, "second-host", port)
        games.append(host)
        host.wait_for(ROOM + "Hoster")
        host.wait_for(PLAYS + "Revision 1")
        recording = workdir / "second-guest.tad"
        guest = joiner_game(native, game_dir, workdir, "second-guest", port, "Visitor",
                            ["--net-record", str(recording)])
        games.append(guest)
        guest.wait_for(ROOM + "Visitor")
        guest.wait_until("Revision 1", lambda lines: any(
            line == PLAYS + "Revision 1" for line in lines))
        older = joiner_game(native, game_dir, workdir, "older", port, "Older",
                            ["--presence-without-enhanced-mode"])
        games.append(older)
        older.wait_for(ROOM + "Older")
        for game in (host, guest):
            game.wait_until("the room Off for Older", lambda lines: OFF_LINE in lines)
            game.wait_until("Off", lambda lines: [
                line for line in lines if line.startswith(PLAYS)][-1] == PLAYS + "Off")
            if game.count(OFF_LINE) != 1:
                raise RoomEnhancedFailure(
                    f"{game.name} wrote {OFF_LINE!r} {game.count(OFF_LINE)} times")
            evidence.append(f"{game.name}: {OFF_LINE} (once); {PLAYS}{game.plays()}")
        wait_switch(host, "Enhanced Mode · off", True)
        before = {game.name: game.count(REVISION_LINE) for game in (host, guest)}
        client = open_client(older)
        try:
            client.request("quit")
        finally:
            client.close()
        older.wait_end()
        for game in (host, guest):
            game.wait_until("Revision 1 again", lambda lines, seen=before[game.name]: sum(
                1 for line in lines if line == REVISION_LINE) > seen)
        time.sleep(2)
        for game in (host, guest):
            added = game.count(REVISION_LINE) - before[game.name]
            if added != 1 or game.plays() != "Revision 1":
                raise RoomEnhancedFailure(
                    f"{game.name} wrote {added} revision lines as Older left and plays "
                    f"{game.plays()}")
            evidence.append(f"{game.name}: {REVISION_LINE} (once, as Older left); "
                            f"{PLAYS}{game.plays()}")
        wait_switch(host, "Enhanced Mode · Revision 1", True)
        older = joiner_game(native, game_dir, workdir, "older-again", port, "Older",
                            ["--presence-without-enhanced-mode"])
        games.append(older)
        older.wait_for(ROOM + "Older")
        for game in (host, guest):
            game.wait_until("the room Off for Older again",
                            lambda lines: sum(1 for line in lines if line == OFF_LINE) == 2)
        play_match(host, [guest, older])
        digest = same_integrity([host, guest, older])
        evidence.append(f"multiplayer: integrity game data {digest} on all three, Off")
        for game in (host, guest, older):
            if game.plays() != "Off":
                raise RoomEnhancedFailure(f"{game.name} played {game.plays()}, not Off")
        wait_file(guest, recording)
        sectors = enhanced_sectors(recording)
        if sectors:
            raise RoomEnhancedFailure(f"the Off match recorded Enhanced Mode sectors {sectors}")
        evidence.append("second-guest's recording: no Enhanced Mode sector")
    except (RoomEnhancedFailure, automation.AutomationFailure):
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
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    # The automation helper lives beside this script.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    with tempfile.TemporaryDirectory(prefix="native-net-room-enhanced-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            evidence = check(args.native.resolve(), args.game_dir.resolve(), Path(scratch))
        except (RoomEnhancedFailure, automation.AutomationFailure) as failure:
            print(f"room enhanced check: {failure}", file=sys.stderr)
            return 1
    for line in evidence:
        print(line)
    print("room enhanced check: a host with the switch on and guests whose own settings were "
          "on and off all played Revision 1, posted its line once, started once every hash "
          "matched and recorded Revision 1's sector; a guest that sends no revision turned "
          "the room Off with a line naming it, its leaving brought Revision 1 back with one "
          "line, and the room it sat in played Off")
    return 0


if __name__ == "__main__":
    sys.exit(main())
