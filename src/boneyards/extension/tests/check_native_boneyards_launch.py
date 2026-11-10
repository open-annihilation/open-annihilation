#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Plays a battle the Boneyards launches with two copies of the game.

The host runs --check-boneyards-launch host under its replay and the joiner
--check-boneyards-launch join under its own, both started at once; the
joiner waits on the game list until the host has hosted the battle room. Both play the battle through to the return to the Boneyards
over TCP/IP on this machine. Each prints its lines with its side in front.
The check passes when both exit with 0 after printing that they passed; a
side that runs past --timeout seconds is stopped and fails it. Each side also
records its battle (--record-game), and the check passes only when that
replay's meta.json names the battle as the Boneyards' own: the game kind, the
battle's game id, its planet and its sector. Both copies
use a DirectPlay port the check picks (--dplay-port), so other checks can run
beside it.

--overlay rewrites copies of the two replays so launch_game names a relay on
127.0.0.1 and an overlay address. The relay is --relay. The originals are
not changed. The check then also requires each side to report no direct
packet, and the relay to have forwarded a data frame from each overlay.
"""
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path
_OA_TOOLS = Path(__file__).resolve().parents[4] / "tools"
if str(_OA_TOOLS) not in sys.path:
    sys.path.insert(0, str(_OA_TOOLS))
import native_offline  # noqa: E402

HOSTED = "boneyards launch check: the battle room is hosted"
PASSED = "boneyards launch check: passed"
NOTICE = "boneyards launch check: after-battle notice shown"
DIRECT = "boneyards launch check: direct packets "
# The replay each side records of its battle, in the folder the side works in.
RECORDED = "battle.oademo"
# Frames each run may take at most; the check ends the run itself long before.
FRAME_LIMIT = "400000"
# Host and joiner identities. The address both dial is the host's overlay.
# 10.66.1.10 is the host; 10.66.1.11 is the joiner. The relay field is the
# percent-escaped form of 127.0.0.1:PORT (%6 is the colon).
HOST_OVERLAY = "10.66.1.10"
JOIN_OVERLAY = "10.66.1.11"
HOST_KEY = "aabbccdd"
JOIN_KEY = "11223344"
# The launch record, without the "launch " word, must fit Rover's record buffer.
RECORD_LIMIT = 1024



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding='utf-8') if target.is_file() else 'open-annihilation-preferences 1\n'
    if 'open-annihilation.update-check' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.update-check" "never"\n'
    if 'open-annihilation.content-updates' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding='utf-8')


def free_udp_port():
    """Returns a UDP port on this machine that nothing is bound to."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def side_command(args, role):
    command = [
        args.game,
        "--game-dir", args.game_dir,
    ]
    if args.mod_dir:
        command += ["--mod-dir", args.mod_dir]
    command += [
        "--mute",
        "--dplay-port", str(args.dplay_port),
        "--frames", FRAME_LIMIT,
        "--boneyards-replay", os.path.join(args.replays, "bypacific_launch_%s.replay" % role),
        "--check-boneyards-launch", role,
        "--preferences-file", os.path.join(args.work, role, "%s.conf" % role),
        "--record-game", os.path.join(args.work, role, RECORDED),
        "--snapshot", os.path.join(args.work, role, "replays.ppm"),
    ]
    if args.for_fun:
        command.append("--boneyards-launch-for-fun")
    return command


class Side:
    def __init__(self, args, role):
        self.role = role
        self.hosted = threading.Event()
        self.passed = False
        self.notice = False
        self.direct = None
        directory = os.path.join(args.work, role)
        os.makedirs(directory, exist_ok=True)
        _offline_preferences(os.path.join(directory, "%s.conf" % role))
        self.process = subprocess.Popen(
            side_command(args, role), cwd=directory, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace")
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            line = line.rstrip("\n")
            print("[%s] %s" % (self.role, line), flush=True)
            if line == HOSTED:
                self.hosted.set()
            if line == PASSED:
                self.passed = True
            if line == NOTICE:
                self.notice = True
            if line.startswith(DIRECT):
                self.direct = line[len(DIRECT):]

    def wait(self, deadline):
        remaining = max(0.0, deadline - time.monotonic())
        try:
            code = self.process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            print("[%s] stopped after the time limit" % self.role, flush=True)
            code = None
        self.reader.join(timeout=5)
        return code


def battle_facts(path):
    """The game kind and the Boneyards' object of a replay's meta.json, or None without them."""
    try:
        with zipfile.ZipFile(path) as archive:
            game = json.loads(archive.read("meta.json")).get("game", {})
    except (OSError, KeyError, ValueError, zipfile.BadZipFile):
        return None
    return game.get("kind"), game.get("boneyards")


def rewrite_replay(source, destination, overlay_ip, address, key, key_id, port):
    """Writes one replay whose launch_game names the overlay. Other lines stay."""
    with open(source, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    needle = "<S:address=127.0.0.1>"
    if text.count(needle) != 1:
        raise SystemExit("%s does not name 127.0.0.1 once" % source)
    text = text.replace(needle, "<S:address=%s>" % address, 1)
    lines = text.splitlines(keepends=True)
    found = False
    for index, line in enumerate(lines):
        if "command=launch_game" not in line:
            continue
        if line.count("]}") != 1:
            raise SystemExit("%s launch line does not close once" % source)
        fields = (
            "<S:relay=127.0.0.1%%6%s><S:overlay_ip=%s><S:overlay_key=%s><I:key_id=%s>"
            % (port, overlay_ip, key, key_id)
        )
        lines[index] = line.replace("]}", fields + "]}", 1)
        record = lines[index].split(" ", 1)[1].strip()
        if len(record) >= RECORD_LIMIT:
            raise SystemExit("%s launch record is %s bytes" % (source, len(record)))
        found = True
        break
    if not found:
        raise SystemExit("%s has no launch_game line" % source)
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "w", encoding="utf-8", newline="") as handle:
        handle.write("".join(lines))


class Relay:
    """The check's relay. It listens on 127.0.0.1 and prints forward counts."""

    def __init__(self, executable):
        self.forwards = None
        self.port = None
        self.process = subprocess.Popen(
            [
                executable,
                HOST_OVERLAY, "1", HOST_KEY,
                JOIN_OVERLAY, "2", JOIN_KEY,
            ],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
        )
        self.ready = threading.Event()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()
        deadline = time.monotonic() + 10
        while not self.ready.is_set():
            if self.process.poll() is not None:
                self.reader.join(timeout=2)
                raise SystemExit("the relay exited before printing a port")
            if time.monotonic() > deadline:
                self.stop()
                raise SystemExit("the relay printed no port")
            self.ready.wait(timeout=0.2)

    def read(self):
        for line in self.process.stdout:
            line = line.rstrip("\n")
            print("[relay] %s" % line, flush=True)
            if line.startswith("relay port "):
                self.port = int(line.split()[2])
                self.ready.set()
            if line.startswith("relay forwards "):
                self.forwards = line

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join(timeout=5)


def forward_counts(line):
    """The last relay line as overlay to count. None when the line is short."""
    parts = line.split()
    if len(parts) < 4 or (len(parts) % 2) != 0:
        return None
    counts = {}
    for index in range(2, len(parts), 2):
        try:
            counts[parts[index]] = int(parts[index + 1])
        except ValueError:
            return None
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--game", required=True, help="the open-annihilation executable")
    parser.add_argument("--game-dir", required=True, help="the BY1117a installation")
    parser.add_argument("--mod-dir", default="", help="mod folder mounted over the installation")
    parser.add_argument("--replays", required=True, help="directory of the two replays")
    parser.add_argument("--work", required=True, help="directory the two runs work in")
    parser.add_argument("--timeout", type=float, default=600.0, help="seconds each side may run")
    parser.add_argument("--overlay", action="store_true", help="carry the battle through a relay")
    parser.add_argument(
        "--for-fun", action="store_true",
        help="the host picks For Fun, so the return offers a share")
    parser.add_argument("--relay", default="", help="relay executable; required with --overlay")
    args = parser.parse_args()
    if args.overlay and not args.relay:
        print("--overlay requires --relay", flush=True)
        return 1
    # Both copies host and search on a port of their own, so other checks
    # that host or search on this machine can run beside them.
    args.dplay_port = free_udp_port()

    relay = None
    try:
        if args.overlay:
            relay = Relay(args.relay)
            if relay.port is None or relay.port <= 0:
                print("the relay has no port", flush=True)
                return 1
            copies = os.path.join(args.work, "replays")
            rewrite_replay(
                os.path.join(args.replays, "bypacific_launch_host.replay"),
                os.path.join(copies, "bypacific_launch_host.replay"),
                HOST_OVERLAY, HOST_OVERLAY, HOST_KEY, "1", relay.port,
            )
            rewrite_replay(
                os.path.join(args.replays, "bypacific_launch_join.replay"),
                os.path.join(copies, "bypacific_launch_join.replay"),
                JOIN_OVERLAY, HOST_OVERLAY, JOIN_KEY, "2", relay.port,
            )
            args.replays = copies

        deadline = time.monotonic() + args.timeout
        # Both start at once: a joiner the Boneyards launches waits on the game
        # list for its host's game, as it would for a slower host machine.
        host = Side(args, "host")
        joiner = Side(args, "join")
        results = [(host, host.wait(deadline)), (joiner, joiner.wait(deadline))]
    finally:
        if relay is not None:
            relay.stop()

    ok = host.hosted.is_set()
    if not ok:
        print("the host never hosted the battle room", flush=True)
    game_ids = set()
    for side, code in results:
        print("%s: exit %s%s" % (side.role, code, ", passed" if side.passed else ""), flush=True)
        # The host picks For Fun, so its return is the share prompt. The joiner
        # reads the listed battle's type, which this replay can leave as a
        # ladder game, so that side's picture keeps the ladder name.
        if args.for_fun and side.role == "host":
            notice_name = "replays-notice-share.ppm"
        elif args.for_fun:
            share = os.path.join(args.work, side.role, "replays-notice-share.ppm")
            notice_name = (
                "replays-notice-share.ppm" if os.path.isfile(share) else "replays-notice-ranked.ppm"
            )
        else:
            notice_name = "replays-notice-ranked.ppm"
        notice_picture = os.path.join(args.work, side.role, notice_name)
        print(
            "%s: after-battle notice %s, picture %s"
            % (side.role, "shown" if side.notice else "missing", "yes" if os.path.isfile(notice_picture) else "no"),
            flush=True,
        )
        ok = ok and code == 0 and side.passed and side.notice and os.path.isfile(notice_picture)
        facts = battle_facts(os.path.join(args.work, side.role, RECORDED))
        kind, battle = facts if facts is not None else (None, None)
        named = (
            kind == "boneyards"
            and isinstance(battle, dict)
            and all(
                isinstance(battle.get(name), str) and battle.get(name)
                for name in ("game_id", "planet", "sector")
            )
        )
        print("%s: recorded %s" % (side.role, json.dumps({"kind": kind, "boneyards": battle})), flush=True)
        ok = ok and named
        if named:
            game_ids.add(battle["game_id"])
            if side.role == "host":
                host_recording = os.path.join(args.work, side.role, RECORDED)
                listed = os.path.join(os.path.dirname(args.work), "listed-battle.oademo")
                # Written whole under another name, then moved into place: the
                # replays check that reads it may run beside this one, and the
                # overlay check writes the same file.
                partial = "%s.%d.part" % (listed, os.getpid())
                with open(host_recording, "rb") as source_file, open(partial, "wb") as listed_file:
                    listed_file.write(source_file.read())
                os.replace(partial, listed)
        if args.overlay:
            print("%s: direct packets %s" % (side.role, side.direct), flush=True)
            ok = ok and side.direct == "0"
    # Both sides played the same battle.
    ok = ok and len(game_ids) == 1
    if args.overlay:
        counts = forward_counts(relay.forwards) if relay.forwards else None
        print("relay forwards: %s" % (relay.forwards or "none"), flush=True)
        ok = ok and counts is not None and counts.get(HOST_OVERLAY, 0) > 0 and counts.get(
            JOIN_OVERLAY, 0
        ) > 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
