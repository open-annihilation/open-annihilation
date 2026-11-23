#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a route burst in a real network game, played by two programs.

A host and one guest each run as a program of their own through the main
loop on the dummy SDL drivers, over 127.0.0.1, at a DirectPlay port of the
check's own (--dplay-port), with Enhanced Mode on. The host's room plays
Painted Desert with fixed start positions. Both start with --net-route-burst,
so once the match starts each machine gives its own player the movers, and at
tick 300 each sends its own movers south in one group move. The check readies
both players and presses START through the automation endpoint.

Each machine writes its units every tick (--trace-units, read as they come
through a pipe, so nothing large is kept). In the 3.1c protocol a machine
simulates only its own player's units and follows the other's from its
records, with a lag: it numbers its own player 0, its copies hold no orders,
and a copy's health comes with its owner's round-robin full record, one unit a
tick. So the machines are compared on what the network shares, never on world
digests:
- every 300th tick, each player's live units, the same on both machines;
- once every mover has stood still on its owner for 300 ticks and a whole
  round of full records (the room's unit limit in ticks) has passed since the
  last unit appeared on either machine, every unit's owning player (by who
  plays it, not by slot), type, map cell and health, the same on both
  machines; a mover still moving then is named and fails the check;
- each machine's "route burst: player P movers M tick T" line, the two
  players at different places, every mover sent at tick 300;
- each machine's "route batches:" line as it leaves the match, with the other
  player's whole burst applied and no movement step held longer than the
  bound: a second without loss, 150 ticks with it.

With --guest-loss PERCENT the guest starts with -e PERCENT and loses that
share of every frame it sends; its lost creates reach the host late, and the
end comparison waits a whole round of full records after the last.

The scratch folder is removed whatever happens, so a second run starts as the
first did.
"""
import argparse
import collections
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
# Seconds the match may take to settle on both machines.
MATCH_TIMEOUT = 900
# The map, a large one of the base game, and the room's name.
MAP = "Painted Desert"
GAME_NAME = "Route Burst"
# The tick of the group move.
BURST_TICK = 300
# Each player's live units are compared every this many ticks.
COUNT_STEP = 300
# The units are read every this many ticks; a multiple divides COUNT_STEP.
SAMPLE_STEP = 30
# Ticks every mover stands still on its owner before the end comparison.
SETTLE_TICKS = 300
# The match plays at least this long, and the check stops it at the latest here.
LEAST_TICKS = 900
MOST_TICKS = 2400
# The longest a machine may hold another's movement step, in ticks: a
# second without loss, and the bound with it (docs/enhanced-mode.md).
HOLD_BOUND = 30
LOSSY_HOLD_BOUND = 150
# The live bit of a unit's flags.
UNIT_LIVE = 0x10000000
BURST = re.compile(r"route burst: player (\d+) movers (\d+) tick (\d+)$")
ROOM_LIMIT = re.compile(r"route burst: the room holds (\d+) units a player")
BATCHES = re.compile(
    r"route batches: ticks (-?\d+), changes (\d+), parts (\d+), resent (\d+), "
    r"longest hold (\d+) ticks")
ROOM = f'multiplayer: in the battle room of "{GAME_NAME}" as '

OFFLINE_PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"\n'
)


class RouteBurstFailure(Exception):
    """A behaviour that differs from the expected one."""


def free_port():
    """Returns a free UDP port using the native host's wildcard bind."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((HOST_BIND_ADDRESS, 0))
        return probe.getsockname()[1]


class UnitStream:
    """One machine's --trace-units lines, read from a pipe as the game writes them.

    Every SAMPLE_STEP-th tick it keeps each live unit's owner slot, type,
    position and health. It counts each owner's live units every
    COUNT_STEP-th tick, notes the last sampled tick at which each of this
    machine's own units (owner 0) stood somewhere new, and the last sampled
    tick at which a unit appeared.
    """

    def __init__(self, path):
        self.lock = threading.Lock()
        self.counts = {}
        self.recent = collections.OrderedDict()
        self.complete = -1
        self.moved = {}
        self.appeared = 0
        self.error = None
        self.ended = threading.Event()
        self._previous = None
        threading.Thread(target=self._read, args=(path,), daemon=True).start()

    def _read(self, path):
        try:
            # Opening blocks until the game opens the pipe as its match starts.
            with open(path, encoding="ascii", errors="replace") as stream:
                current = None
                rows = None
                for line in stream:
                    space = line.find(" ", 5)
                    tick = int(line[5:space])
                    if tick != current:
                        if rows is not None:
                            self._finish(current, rows)
                        current = tick
                        rows = {} if tick % SAMPLE_STEP == 0 else None
                    if rows is None:
                        continue
                    # tick slot type owner flags state health build x y z ...
                    fields = line.split(" ", 11)
                    if int(fields[4][6:], 16) & UNIT_LIVE == 0:
                        continue
                    rows[int(fields[1][5:])] = (
                        int(fields[3][6:]), int(fields[2][5:]), int(fields[8][2:], 16),
                        int(fields[10][2:], 16), int(fields[6][7:]))
                if rows is not None:
                    self._finish(current, rows)
        except (OSError, ValueError, IndexError) as error:
            self.error = error
        finally:
            self.ended.set()

    def _finish(self, tick, rows):
        """Keeps a sampled tick whose lines have all been read."""
        with self.lock:
            previous = self._previous
            for slot, row in rows.items():
                before = previous.get(slot) if previous is not None else None
                if before is None:
                    self.appeared = tick
                elif row[0] == 0 and row[2:4] != before[2:4]:
                    self.moved[slot] = tick
            self._previous = rows
            if tick % COUNT_STEP == 0:
                self.counts[tick] = collections.Counter(row[0] for row in rows.values())
            self.recent[tick] = rows
            while len(self.recent) > 40:
                self.recent.popitem(last=False)
            self.complete = tick

    def progress(self):
        """The last complete sampled tick, the last own move and the last appearance."""
        with self.lock:
            return self.complete, max(self.moved.values(), default=0), self.appeared


class Game:
    """One running game, whose standard output the check reads line by line."""

    def __init__(self, native, game_dir, workdir, name, arguments):
        self.name = name
        self.lines = []
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        preferences = workdir / f"{name}.conf"
        preferences.write_text(OFFLINE_PREFERENCES, encoding="utf-8")
        user_folder = workdir / f"{name}-user"
        user_folder.mkdir(parents=True, exist_ok=True)
        data_dir = workdir / f"{name}.data"
        data_dir.mkdir(exist_ok=True)
        units = workdir / f"{name}.units"
        os.mkfifo(units)
        self.units = UnitStream(units)
        self.fark_file = workdir / f"{name}-fark" / "automation.json"
        self.fark_file.parent.mkdir(parents=True, exist_ok=True)
        command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
                   "--preferences-file", str(preferences), "--user-folder", str(user_folder),
                   "--data-dir", str(data_dir), "--fark", "--fark-file", str(self.fark_file),
                   "--trace-digest", str(workdir / f"{name}.trace"), "--trace-units", str(units),
                   *arguments]
        self.process = subprocess.Popen(
            command, cwd=workdir, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def drain(self, timeout=0.0):
        """Takes every line queued, waiting up to timeout seconds for the first."""
        while not self._ended:
            try:
                line = self._queue.get(timeout=timeout) if timeout else self._queue.get_nowait()
            except queue.Empty:
                return
            timeout = 0.0
            if line is None:
                self._ended = True
                return
            self.lines.append(line)

    def wait_for(self, expected):
        """Waits for a line of the game's output; raises when it never comes."""
        deadline = time.monotonic() + LINE_TIMEOUT
        while time.monotonic() < deadline:
            if expected in self.lines:
                return
            if self._ended:
                break
            self.drain(0.5)
        raise RouteBurstFailure(f"{self.name} never wrote {expected!r}")

    def found(self, pattern):
        """The matches of a pattern in the game's lines, in order."""
        self.drain()
        return [match for match in (pattern.search(line) for line in self.lines) if match]

    def wait_end(self):
        """Waits for the game to end by itself; raises unless it ends with status 0."""
        try:
            status = self.process.wait(timeout=LINE_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise RouteBurstFailure(f"{self.name} did not end within {LINE_TIMEOUT} s") from None
        while not self._ended:
            self.drain(0.5)
        if status != 0:
            raise RouteBurstFailure(f"{self.name} exited with {status}")

    def stop(self):
        """Ends the game if it still runs."""
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        """Returns what the game printed, each line under its name."""
        self.drain()
        return "".join(f"{self.name}: {line}\n" for line in self.lines)


def open_client(game):
    """Connects to the game's automation endpoint and says hello."""
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline:
        game.drain()
        if game.fark_file.is_file():
            break
        if game.process.poll() is not None:
            raise RouteBurstFailure(f"{game.name} ended before it served automation")
        time.sleep(0.1)
    else:
        raise RouteBurstFailure(f"{game.name} wrote no automation file")
    endpoint = json.loads(game.fark_file.read_text(encoding="utf-8"))
    client = automation.Client(endpoint["address"])
    hello = client.request("hello", versions=[1], client="net-route-burst", token=endpoint["token"])
    if not hello.get("ok"):
        raise RouteBurstFailure(f"{game.name} answered hello with {hello}")
    return client


def control_named(client, name):
    """Returns the shown control of that name, or nothing."""
    for control in client.request("controls").get("controls", []):
        if control.get("name", "").lower() == name.lower():
            return control
    return None


def wait_enabled(client, name, screen):
    """Waits until a control is shown and enabled."""
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline:
        if client.request("screen").get("screen") == screen:
            control = control_named(client, name)
            if control is not None and control.get("enabled"):
                return
        time.sleep(0.1)
    raise RouteBurstFailure(f"{screen} never enabled {name}: {client.request('room')}")


def click_ready(client):
    """Clicks this machine's own READY and waits until its player is ready."""
    slot = client.request("room").get("local_slot")
    name = f"READY{slot}"
    wait_enabled(client, name, "mp_battleroom")
    automation.click_control(client, name, "mp_battleroom")
    deadline = time.monotonic() + LINE_TIMEOUT
    while time.monotonic() < deadline:
        for player in client.request("room").get("players") or []:
            if player.get("slot") == slot and player.get("ready"):
                return
        time.sleep(0.1)
    raise RouteBurstFailure(f"{name} did not ready the local player")


def settled_tick(games, round_ticks):
    """The last sampled tick both machines have read, once the match may end there; else None.

    It may end once both moves were given, every mover has stood still on its
    owner for SETTLE_TICKS, and round_ticks have passed since a unit last
    appeared on either machine.
    """
    if any(len(game.found(BURST)) != 1 for game in games):
        return None
    progress = [game.units.progress() for game in games]
    tick = min(complete for complete, _, _ in progress)
    moved = max(last_move for _, last_move, _ in progress)
    appeared = max(last_seen for _, _, last_seen in progress)
    if tick < LEAST_TICKS or tick < moved + SETTLE_TICKS or tick < appeared + round_ticks:
        return None
    return tick


def play_match(host, guest, round_ticks):
    """Readies both machines, presses the host's START, plays until the units settle, and quits both."""
    games = [host, guest]
    clients = [open_client(game) for game in games]
    try:
        for client in clients:
            automation.wait_screen(client, "mp_battleroom")
        click_ready(clients[1])
        click_ready(clients[0])
        wait_enabled(clients[0], "START", "mp_battleroom")
        automation.click_control(clients[0], "START", "mp_battleroom")
        deadline = time.monotonic() + MATCH_TIMEOUT
        while time.monotonic() < deadline:
            screens = []
            for game, client in zip(games, clients):
                game.drain()
                screens.append(client.request("screen").get("screen"))
                if game.units.error is not None:
                    raise RouteBurstFailure(f"{game.name}'s units: {game.units.error}")
            if "main_menu" in screens:
                raise RouteBurstFailure(f"the match returned to the menu on {screens}")
            if settled_tick(games, round_ticks) is not None:
                break
            if min(game.units.progress()[0] for game in games) >= MOST_TICKS:
                break
            time.sleep(0.5)
        else:
            raise RouteBurstFailure(
                f"the match reached ticks {[game.units.progress()[0] for game in games]} "
                f"in {MATCH_TIMEOUT} s")
        for client in clients:
            client.request("quit")
    finally:
        for client in clients:
            client.close()
    for game in games:
        game.wait_end()


def identities(game, other):
    """Names each owner slot of a machine by the player: its own player is slot 0."""
    return lambda slot: game.name if slot == 0 else other.name


def compare_counts(host, guest, last):
    """Requires each player's live units equal on both machines every COUNT_STEP ticks to last.

    Returns each tick compared, with each player's live units.
    """
    host_player = identities(host, guest)
    guest_player = identities(guest, host)
    compared = []
    for tick in range(COUNT_STEP, last + 1, COUNT_STEP):
        on_host = host.units.counts.get(tick)
        on_guest = guest.units.counts.get(tick)
        if on_host is None or on_guest is None:
            raise RouteBurstFailure(f"a machine read no units at tick {tick}")
        by_host = {host_player(slot): count for slot, count in on_host.items()}
        by_guest = {guest_player(slot): count for slot, count in on_guest.items()}
        if len(on_host) != 2 or len(on_guest) != 2 or by_host != by_guest:
            raise RouteBurstFailure(
                f"the players' live units differ at tick {tick}: host {by_host}, guest {by_guest}")
        compared.append((tick, by_host))
    return compared


def compare_units(host, guest, tick):
    """Requires every unit's owning player, type, cell and health equal on both machines at tick.

    A unit that moved on its owner within SETTLE_TICKS of tick is named.
    Returns the units compared.
    """
    host_player = identities(host, guest)
    guest_player = identities(guest, host)
    for game in (host, guest):
        moving = sorted(slot for slot, last_move in game.units.moved.items()
                        if last_move > tick - SETTLE_TICKS)
        if moving:
            # Where each stood at the last few samples, x and z in world units.
            samples = [rows for at, rows in game.units.recent.items() if at <= tick][-4:]
            named = "; ".join(
                f"unit {slot} at " + " ".join(
                    f"({rows[slot][2] >> 16}, {rows[slot][3] >> 16})"
                    for rows in samples if slot in rows)
                for slot in moving[:6])
            raise RouteBurstFailure(
                f"{len(moving)} of {game.name}'s units still moved within {SETTLE_TICKS} ticks "
                f"of tick {tick}: {named}")
    on_host = host.units.recent[tick]
    on_guest = guest.units.recent[tick]

    def shared(row, player):
        owner, unit_type, x, z, health = row
        return player(owner), unit_type, (x >> 16) // 16, (z >> 16) // 16, health

    differing = []
    for slot in sorted(set(on_host) | set(on_guest)):
        held = [shared(rows[slot], player) if slot in rows else None
                for rows, player in ((on_host, host_player), (on_guest, guest_player))]
        if held[0] != held[1]:
            differing.append(f"unit {slot}: host {held[0]}, guest {held[1]}")
    if differing:
        raise RouteBurstFailure(
            f"{len(differing)} units differ at tick {tick} (player, type, cell, health): "
            + "; ".join(differing[:8]))
    return len(on_host)


def check(native, game_dir, workdir, movers, guest_loss):
    """Plays the burst; raises RouteBurstFailure when a game differs.

    Returns the lines the success report prints.
    """
    games = []
    evidence = []
    try:
        port = free_port()
        burst = ["--enhanced-mode", "--net-route-burst", str(movers)]
        host = Game(native, game_dir, workdir, "host", [
            "--dplay-port", str(port), "--host", "--player-name", "Hoster",
            "--game-name", GAME_NAME, "--map", MAP, "--start-positions", "fixed", *burst])
        games.append(host)
        host.wait_for(ROOM + "Hoster")
        # The host's room is a launch's, which only a launch joins: the
        # guest's side makes its join one.
        guest_switches = [f"-e{guest_loss}"] if guest_loss else []
        guest = Game(native, game_dir, workdir, "guest", [
            "--dplay-port", str(port), "--join", ADDRESS, "--player-name", "Visitor",
            "--side", "arm", *burst, *guest_switches])
        games.append(guest)
        guest.wait_for(ROOM + "Visitor")
        limit = host.found(ROOM_LIMIT)
        if len(limit) != 1:
            raise RouteBurstFailure("the host did not give its room the movers' unit limit")
        evidence.append(f"host: {limit[0].group(0)}")
        round_ticks = int(limit[0].group(1))
        play_match(host, guest, round_ticks)
        places = set()
        for game in games:
            found = game.found(BURST)
            if len(found) != 1:
                raise RouteBurstFailure(f"{game.name} wrote {len(found)} route burst lines")
            place, moved, tick = (int(group) for group in found[0].groups())
            if moved != movers or tick != BURST_TICK:
                raise RouteBurstFailure(f"{game.name}: {found[0].group(0)}")
            places.add(place)
            evidence.append(f"{game.name}: {found[0].group(0)}")
        if places != {1, 2}:
            raise RouteBurstFailure(f"the players took places {sorted(places)}, not 1 and 2")
        for game in games:
            if not game.units.ended.wait(LINE_TIMEOUT):
                raise RouteBurstFailure(f"{game.name}'s units were not all read")
            if game.units.error is not None:
                raise RouteBurstFailure(f"{game.name}'s units: {game.units.error}")
        common = sorted(set(host.units.recent) & set(guest.units.recent))
        if not common:
            raise RouteBurstFailure("the machines read no units at a common tick")
        # Unsettled by MOST_TICKS, the comparison names the units still moving.
        settled = settled_tick(games, round_ticks)
        tick = max(sample for sample in common if settled is None or sample <= settled)
        if settled is None and tick < max(game.units.appeared for game in games) + round_ticks:
            progress = {game.name: game.units.progress() for game in games}
            raise RouteBurstFailure(
                f"no round of full records passed after the last unit appeared, by tick "
                f"{MOST_TICKS} (last read, last own move, last appearance: {progress})")
        compared = compare_counts(host, guest, tick)
        evidence.append("each player's live units agree at ticks "
                        f"{', '.join(str(at) for at, _ in compared)}: {compared[-1][1]}")
        units = compare_units(host, guest, tick)
        if settled is None:
            raise RouteBurstFailure(f"the units did not settle by tick {MOST_TICKS}")
        evidence.append(f"at tick {tick}, after every mover stood still {SETTLE_TICKS} ticks and "
                        f"a round of {round_ticks} full records, all {units} units agree on "
                        "player, type, cell and health")
        bound = LOSSY_HOLD_BOUND if guest_loss else HOLD_BOUND
        for game in games:
            found = game.found(BATCHES)
            if len(found) != 1:
                raise RouteBurstFailure(f"{game.name} wrote {len(found)} route batches lines")
            changes = int(found[0].group(2))
            hold = int(found[0].group(5))
            if changes != movers or hold > bound:
                raise RouteBurstFailure(
                    f"{game.name}: {found[0].group(0)} (the burst is {movers} changes, the "
                    f"hold bound {bound} ticks)")
            evidence.append(f"{game.name}: {found[0].group(0)}")
    except (RouteBurstFailure, automation.AutomationFailure):
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
    parser.add_argument("--movers", type=int, default=1000)
    parser.add_argument("--guest-loss", type=int, default=0)
    args = parser.parse_args()
    if not hasattr(os, "mkfifo"):
        print("net route burst check: skipped: the units are read through a pipe, which this "
              "system does not make")
        return 77
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    # The automation helper lives beside this script.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    with tempfile.TemporaryDirectory(prefix="native-net-route-burst-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            evidence = check(args.native.resolve(), args.game_dir.resolve(), Path(scratch),
                             args.movers, args.guest_loss)
        except (RouteBurstFailure, automation.AutomationFailure) as failure:
            print(f"net route burst check: {failure}", file=sys.stderr)
            return 1
    for line in evidence:
        print(line)
    loss = f", the guest losing {args.guest_loss}% of its frames" if args.guest_loss else ""
    print(f"net route burst check: a host and a guest, each a program of its own{loss}, gave "
          f"their players {args.movers} movers each and sent them south at tick {BURST_TICK}; "
          "both machines held each player's units alike, and both left with their route batches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
