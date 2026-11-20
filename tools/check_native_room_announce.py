#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A battle-room join over loopback is spoken.

The host runs with MCP and the screen-reader preference on. Once its
battle room is up, a joiner named Visitor arrives, also with MCP and
the same preference. The host posts an announce note for that join.
Neither side hears a line that is not a change: no "An option changed"
for the count of seated players, no colour change while the host picks
Visitor's colour, and no "Hoster joined" on the joiner, who found the
host there. The host then leaves, and the joiner posts "The host has left".
Nothing on the wire is this check's to change: it only reads the notes.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import time

import check_native_net_options as net
import native_offline

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds the host may take to reach the battle room, or to speak the join.
ROOM_TIMEOUT = 120
ANNOUNCE_TIMEOUT = 90
END_TIMEOUT = 30
# Seconds both sides keep listening after the join, for a line that is not a change.
SETTLE_SECONDS = 3
# Words neither side may hear for the join alone.
NOT_CHANGES = ("An option changed", "changed colour", "Hoster joined")
PREFERENCES = (
    "open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"never\"\n"
    '"multiplayer|tcpaddr" "192.0.2.7"\n'
    '"open-annihilation.access.screen-reader" "on"\n'
    '"open-annihilation.access.speak-chat" "all"\n'
    '"open-annihilation.access.speak-lobby" "1"\n'
    '"open-annihilation.access.speak-ticker" "0"\n'
)


class RoomFailure(Exception):
    """The join was not announced."""


def next_message(lines, timeout):
    """Returns the next JSON object, skipping lines that are not JSON."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RoomFailure("no MCP message arrived")
        try:
            line = lines.get(timeout=remaining)
        except queue.Empty:
            raise RoomFailure("no MCP message arrived") from None
        if line is None:
            raise RoomFailure("standard output closed before a message")
        text = line.strip()
        if not text:
            continue
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            continue


def send(process, message):
    """Writes one JSON-RPC message and a newline."""
    process.stdin.write(json.dumps(message) + "\n")
    process.stdin.flush()


def request(process, lines, ident, method, params=None, timeout=ROOM_TIMEOUT):
    """Sends one request and returns the result, skipping notices."""
    message = {"jsonrpc": "2.0", "id": ident, "method": method}
    if params is not None:
        message["params"] = params
    send(process, message)
    while True:
        answer = next_message(lines, timeout)
        if answer.get("method"):
            continue
        if answer.get("id") != ident:
            continue
        if "error" in answer:
            raise RoomFailure(f"{method} failed: {answer['error']}")
        return answer.get("result") or {}


def tool_body(process, lines, ident, name, arguments=None, timeout=ROOM_TIMEOUT):
    """Calls one tool and returns its JSON body."""
    params = {"name": name}
    if arguments is not None:
        params["arguments"] = arguments
    result = request(process, lines, ident, "tools/call", params, timeout)
    if result.get("isError"):
        content = (result.get("content") or [{}])[0]
        raise RoomFailure(f"{name} failed: {content.get('text')}")
    content = (result.get("content") or [{}])[0]
    text = content.get("text") or ""
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise RoomFailure(f"{name} did not return JSON: {text!r}") from error


def screen_name(body):
    """The screen a ui.screen or ui.wait answer names."""
    if not isinstance(body, dict):
        return None
    screen = body.get("screen")
    if isinstance(screen, dict):
        return screen.get("screen")
    return screen


def read_pipe(pipe, into):
    """Reads lines from a pipe into a queue until the pipe closes."""
    for line in pipe:
        into.put(line)
    into.put(None)


def open_session(process, lines):
    """Initialises MCP and subscribes to announce."""
    request(
        process,
        lines,
        1,
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "native-room-announce", "version": "0"},
        },
    )
    send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
    listed = request(process, lines, 2, "tools/list")
    names = {tool.get("name") for tool in listed.get("tools", [])}
    if "ui.wait" not in names or "events.read" not in names:
        raise RoomFailure(f"tools are {sorted(names)}")
    subscribed = tool_body(process, lines, 3, "events.subscribe", {"events": ["announce"]})
    accepted = subscribed.get("events") or []
    if "announce" not in accepted:
        raise RoomFailure(f"subscribe refused announce; accepted {accepted}")


def wait_for_room(process, lines):
    """Waits until the host's battle room is the screen on show."""
    body = tool_body(
        process,
        lines,
        4,
        "ui.wait",
        {"screen": "mp_battleroom", "timeout_ms": ROOM_TIMEOUT * 1000},
        timeout=ROOM_TIMEOUT + 30,
    )
    if body.get("ok") is not True or screen_name(body) != "mp_battleroom":
        raise RoomFailure(f"the host did not reach the battle room: {body}")


def announcements(process, lines, ident):
    """The announce notes still kept."""
    body = tool_body(process, lines, ident, "events.read", timeout=30)
    found = []
    for note in body.get("events") or []:
        if note.get("event") != "announce":
            continue
        found.append(str(note.get("text") or ""))
    return found


def stop(process):
    """Ends a game if it still runs."""
    if process is None or process.poll() is not None:
        return
    process.kill()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


def check(native, game_dir, workdir):
    """Hosts a room and requires Visitor's join to be announced."""
    port = net.free_port()
    host_preferences = workdir / "room-host.conf"
    host_preferences.write_text(PREFERENCES, encoding="utf-8")
    log_dir = workdir / "host-logs"
    joiner_logs = workdir / "joiner-logs"
    user_folder = workdir / "host-user"
    joiner_user = workdir / "joiner-user"
    log_dir.mkdir()
    joiner_logs.mkdir()
    user_folder.mkdir()
    joiner_user.mkdir()
    environment = dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        SDL_RENDER_DRIVER="software",
    )
    host = subprocess.Popen(
        [
            *RUNNER,
            str(native),
            "--game-dir",
            str(game_dir),
            "--mute",
            "--skip-intro",
            "--mcp",
            "--dplay-port",
            str(port),
            "--host",
            "--player-name",
            "Hoster",
            "--game-name",
            "Open Room",
            "--preferences-file",
            str(host_preferences),
            "--log-dir",
            str(log_dir),
            "--user-folder",
            str(user_folder),
        ],
        cwd=workdir,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        errors="replace",
    )
    lines = queue.Queue()
    threading.Thread(target=read_pipe, args=(host.stdout, lines), daemon=True).start()
    visitor = None
    try:
        open_session(host, lines)
        wait_for_room(host, lines)
        joiner_preferences = workdir / "room-joiner.conf"
        joiner_preferences.write_text(PREFERENCES, encoding="utf-8")
        visitor = subprocess.Popen(
            [
                *RUNNER,
                str(native),
                "--game-dir",
                str(game_dir),
                "--mute",
                "--skip-intro",
                "--mcp",
                "--dplay-port",
                str(port),
                "--join",
                net.ADDRESS,
                "--player-name",
                "Visitor",
                "--preferences-file",
                str(joiner_preferences),
                "--log-dir",
                str(joiner_logs),
                "--user-folder",
                str(joiner_user),
            ],
            cwd=workdir,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            errors="replace",
        )
        visitor_lines = queue.Queue()
        threading.Thread(target=read_pipe, args=(visitor.stdout, visitor_lines), daemon=True).start()
        open_session(visitor, visitor_lines)
        deadline = time.monotonic() + ANNOUNCE_TIMEOUT
        ident = 10
        texts = []
        while time.monotonic() < deadline:
            if host.poll() is not None:
                raise RoomFailure(f"the host exited {host.returncode} before the join was spoken")
            ident += 1
            texts = announcements(host, lines, ident)
            if any("Visitor" in text and "joined" in text for text in texts):
                break
            time.sleep(0.25)
        else:
            raise RoomFailure(f"no announce for Visitor joining; announcements were {texts[-12:]!r}")
        time.sleep(SETTLE_SECONDS)
        ident += 1
        texts = announcements(host, lines, ident)
        joiner_ident = 10
        joiner_texts = announcements(visitor, visitor_lines, joiner_ident)
        for side, heard in (("host", texts), ("joiner", joiner_texts)):
            wrong = [text for text in heard if any(words in text for words in NOT_CHANGES)]
            if wrong:
                raise RoomFailure(f"the {side} heard a line that is not a change: {wrong!r}")
        if sum(1 for text in texts if text == "Visitor joined") != 1:
            raise RoomFailure(f"the host did not hear the join once; announcements were {texts!r}")
        send(
            host,
            {
                "jsonrpc": "2.0",
                "id": ident + 1,
                "method": "tools/call",
                "params": {"name": "game.quit"},
            },
        )
        joiner_deadline = time.monotonic() + ANNOUNCE_TIMEOUT
        joiner_texts = []
        while time.monotonic() < joiner_deadline:
            if visitor.poll() is not None:
                raise RoomFailure(
                    f"the joiner exited {visitor.returncode} before the host's leave was spoken"
                )
            joiner_ident += 1
            joiner_texts = announcements(visitor, visitor_lines, joiner_ident)
            if any("The host has left" in text for text in joiner_texts):
                break
            time.sleep(0.25)
        else:
            raise RoomFailure(
                "no announce for the host leaving; announcements were "
                + repr(joiner_texts[-12:])
            )
    except (RoomFailure, net.NetOptionsFailure):
        if visitor is not None and visitor.stdout is not None:
            print("joiner stdout closed", file=sys.stderr)
        for folder in (log_dir, joiner_logs):
            for log in sorted(folder.glob("*")):
                if not log.is_file():
                    continue
                tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
                print(f"--- {log.name} ---", file=sys.stderr)
                print("\n".join(tail), file=sys.stderr)
        raise
    finally:
        stop(visitor)
        if host.poll() is None and host.stdin is not None:
            try:
                send(host, {"jsonrpc": "2.0", "id": 99, "method": "tools/call",
                            "params": {"name": "game.quit"}})
                host.wait(timeout=END_TIMEOUT)
            except (OSError, subprocess.TimeoutExpired):
                pass
        stop(host)
        if host.stdin is not None:
            host.stdin.close()


def main():
    """Runs the battle-room announcement check."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="A10-room-announce-", dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), Path(scratch))
        except (RoomFailure, net.NetOptionsFailure) as failure:
            print(f"native-room-announce: {failure}", file=sys.stderr)
            return 1
    print(
        "native-room-announce: Visitor joining was an announce note with no false line beside it, "
        "and the host leaving was one too"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
