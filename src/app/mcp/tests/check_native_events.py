#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A subscribed client hears a unit go idle and an order finish.

The game runs on the dummy SDL drivers, started with --mcp and --fark, and
without --give-orders, so the commander's queue starts empty. This client
starts a skirmish the way the orders check does, subscribes to unit_idle and
order_done, and moves the commander a short way. events.read then shows both
for that unit, stamped with the match's tick, and oa://events holds the same
notes. Standard input and output are the MCP stream. The automation endpoint
is the socket --fark-file names. The log, the preferences and the user folder
go to a scratch folder that is new for every run.
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

_TOOLS = Path(__file__).resolve().parents[4] / "tools"
sys.path.insert(0, str(_TOOLS))
import check_native_automation as automation  # noqa: E402

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
START_TIMEOUT = 180
STEP_TIMEOUT = 30
MATCH_TIMEOUT = 180
END_TIMEOUT = 60


class Failure(Exception):
    """The native check failed."""


def read_pipe(pipe, into):
    """Reads lines from a pipe into a queue until the pipe closes."""
    for line in pipe:
        into.put(line)
    into.put(None)


class _Stderr:
    """A queue-like sink that keeps stderr lines on a list."""

    def __init__(self, lines):
        self._lines = lines

    def put(self, line):
        """Keeps one line. None is the end of the pipe."""
        if line is not None:
            self._lines.append(line)


def require(condition, message):
    """Raises Failure when a condition does not hold."""
    if not condition:
        raise Failure(message)


def log_tail(log_dir, stderr_lines):
    """The end of the log and of standard error, for a failure."""
    parts = []
    if log_dir.is_dir():
        for path in sorted(log_dir.iterdir()):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if text:
                parts.append(f"{path.name}:\n{text[-2000:]}")
    err = "".join(stderr_lines)[-2000:]
    if err:
        parts.append(err)
    return "\n".join(parts)


def next_message(lines, timeout, stderr_lines, log_dir):
    """Returns the next JSON object, skipping blank lines."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure(f"no MCP message arrived\n{log_tail(log_dir, stderr_lines)}")
        try:
            line = lines.get(timeout=remaining)
        except queue.Empty:
            raise Failure(f"no MCP message arrived\n{log_tail(log_dir, stderr_lines)}") from None
        if line is None:
            raise Failure(
                f"standard output closed before a message\n{log_tail(log_dir, stderr_lines)}"
            )
        text = line.strip()
        if not text:
            continue
        try:
            return json.loads(text)
        except json.JSONDecodeError as error:
            raise Failure(f"standard output was not JSON: {text!r}") from error


def send(process, message):
    """Writes one JSON-RPC message and a newline."""
    process.stdin.write(json.dumps(message) + "\n")
    process.stdin.flush()


def call_tool(process, lines, stderr_lines, log_dir, call_id, name, arguments=None, timeout=STEP_TIMEOUT):
    """Calls one tool and returns the parsed text of its result."""
    params = {"name": name}
    if arguments is not None:
        params["arguments"] = arguments
    send(process, {"jsonrpc": "2.0", "id": call_id, "method": "tools/call", "params": params})
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure(f"no answer to {name}\n{log_tail(log_dir, stderr_lines)}")
        answer = next_message(lines, remaining, stderr_lines, log_dir)
        if answer.get("id") != call_id:
            continue
        result = answer.get("result") or {}
        content = (result.get("content") or [{}])[0]
        text = content.get("text") or ""
        if result.get("isError"):
            return {"_error": text}
        try:
            return json.loads(text)
        except json.JSONDecodeError as error:
            raise Failure(f"{name} did not return JSON: {text!r}") from error


def call_rpc(process, lines, stderr_lines, log_dir, call_id, method, params=None, timeout=STEP_TIMEOUT):
    """Sends one request and returns its result object."""
    message = {"jsonrpc": "2.0", "id": call_id, "method": method}
    if params is not None:
        message["params"] = params
    send(process, message)
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise Failure(f"no answer to {method}\n{log_tail(log_dir, stderr_lines)}")
        answer = next_message(lines, remaining, stderr_lines, log_dir)
        if answer.get("id") != call_id:
            continue
        if "error" in answer:
            return {"_error": answer["error"]}
        return answer.get("result") or {}


def wait_endpoint(path, process, log_dir, stderr_lines):
    """Waits for the automation endpoint's file and returns what it holds."""
    deadline = time.monotonic() + START_TIMEOUT
    while time.monotonic() < deadline:
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
        if process.poll() is not None:
            break
        time.sleep(0.1)
    raise Failure(f"the game wrote no endpoint file\n{log_tail(log_dir, stderr_lines)}")


def connect(endpoint):
    """Opens the endpoint. hello, with the file's token, is the first frame."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        client = automation.Client(endpoint["address"])
        hello = client.request(
            "hello", versions=[1], client="native-mcp-events", token=endpoint["token"]
        )
        if hello.get("ok") is True:
            return client
        client.close()
        if not automation.refused(hello, "busy") or time.monotonic() > deadline:
            raise Failure(f"hello was answered {hello}")
        time.sleep(0.1)


def is_commander(unit):
    """A commander is a unit whose type name ends in COM."""
    return str(unit.get("type") or "").upper().endswith("COM")


def seen(body, commander_id, applied, kind):
    """Events of one kind for the commander at or after the move's tick."""
    found = []
    for event in body.get("events") or []:
        if event.get("event") != kind or event.get("unit") != commander_id:
            continue
        tick = event.get("tick")
        if isinstance(tick, int) and tick >= applied and isinstance(event.get("seq"), int):
            found.append(event)
    return found


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M11-mcp-", dir=root) as work:
        workdir = Path(work)
        preferences = workdir / "mcp.conf"
        preferences.write_text(automation.PREFERENCES, encoding="utf-8")
        log_dir = workdir / "logs"
        user_folder = workdir / "user"
        endpoint_dir = workdir / "endpoint"
        log_dir.mkdir()
        user_folder.mkdir()
        endpoint_dir.mkdir()
        endpoint_file = endpoint_dir / "automation.json"
        environment = dict(
            os.environ,
            SDL_VIDEO_DRIVER="dummy",
            SDL_AUDIO_DRIVER="dummy",
            SDL_RENDER_DRIVER="software",
        )
        process = subprocess.Popen(
            [
                *RUNNER,
                str(args.native),
                "--game-dir",
                str(args.game_dir),
                "--skip-intro",
                "--mute",
                "--mcp",
                "--fark",
                "--fark-file",
                str(endpoint_file),
                "--preferences-file",
                str(preferences),
                "--log-dir",
                str(log_dir),
                "--user-folder",
                str(user_folder),
            ],
            cwd=workdir,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
        )
        stdout_lines = queue.Queue()
        stderr_lines = []
        out_thread = threading.Thread(
            target=read_pipe, args=(process.stdout, stdout_lines), daemon=True
        )
        err_thread = threading.Thread(
            target=read_pipe, args=(process.stderr, _Stderr(stderr_lines)), daemon=True
        )
        out_thread.start()
        err_thread.start()
        client = None
        try:
            endpoint = wait_endpoint(endpoint_file, process, log_dir, stderr_lines)
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "protocolVersion": "2025-06-18",
                        "capabilities": {},
                        "clientInfo": {"name": "native-mcp-events", "version": "0"},
                    },
                },
            )
            started = next_message(stdout_lines, START_TIMEOUT, stderr_lines, log_dir)
            require(started.get("id") == 1, f"initialize answered {started}")
            send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
            send(process, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            listed = next_message(stdout_lines, STEP_TIMEOUT, stderr_lines, log_dir)
            names = {tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])}
            for name in ("events.subscribe", "events.read", "orders.issue"):
                require(name in names, f"tools are {sorted(names)}")

            address = endpoint.get("address", "")
            require(address.startswith("127.0.0.1:"), f"the endpoint file holds {endpoint}")
            require(endpoint.get("token"), "the endpoint file holds no token")
            client = connect(endpoint)
            automation.wait_screen(client, "main_menu")
            automation.click_control(client, "SINGLE", "main_menu")
            automation.wait_screen(client, "single_player")
            automation.click_control(client, "Skirmish", "single_player")
            automation.wait_screen(client, "skirmish")
            automation.click_control(client, "Start", "skirmish")
            automation.wait_screen(client, "match")

            commander = None
            deadline = time.monotonic() + MATCH_TIMEOUT
            call_id = 10
            last = None
            while time.monotonic() < deadline:
                call_id += 1
                status = call_tool(
                    process, stdout_lines, stderr_lines, log_dir, call_id, "game.status"
                )
                call_id += 1
                mine = call_tool(
                    process, stdout_lines, stderr_lines, log_dir, call_id, "units.list",
                    {"owner": "me"},
                )
                last = {"status": status, "mine": mine}
                if "_error" in status or "_error" in mine:
                    time.sleep(0.2)
                    continue
                commander = next(
                    (unit for unit in mine.get("units") or [] if is_commander(unit)), None
                )
                if (
                    commander is not None
                    and status.get("in_match") is True
                    and status.get("paused") is False
                ):
                    break
                time.sleep(0.2)
            require(
                commander is not None,
                f"the commander did not appear: {last}\n{log_tail(log_dir, stderr_lines)}",
            )
            commander_id = commander.get("id")
            position = commander.get("position") or []
            require(
                isinstance(commander_id, int) and len(position) >= 3,
                f"the commander is {commander}",
            )

            call_id += 1
            subscribed = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "events.subscribe",
                {"events": ["unit_idle", "order_done"]},
            )
            require("_error" not in subscribed, f"events.subscribe failed: {subscribed}")
            kinds = subscribed.get("events") or []
            require(
                "unit_idle" in kinds and "order_done" in kinds,
                f"the subscription is {subscribed}",
            )
            cursor = subscribed.get("next")
            require(isinstance(cursor, int), f"the subscription named no seq: {subscribed}")

            origin_x, origin_y, origin_z = position[0], position[1], position[2]
            call_id += 1
            moved = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "orders.issue",
                {
                    "units": [commander_id],
                    "kind": "move",
                    "point": [origin_x + 48, origin_y, origin_z],
                    "append": False,
                },
            )
            require("_error" not in moved, f"the move failed: {moved}")
            applied = moved.get("applied_tick")
            require(isinstance(applied, int), f"the move named no tick: {moved}")

            idle = []
            done = []
            body = None
            deadline = time.monotonic() + MATCH_TIMEOUT
            while time.monotonic() < deadline:
                call_id += 1
                body = call_tool(
                    process, stdout_lines, stderr_lines, log_dir, call_id, "events.read",
                    {"after": cursor},
                )
                if "_error" in body:
                    time.sleep(0.2)
                    continue
                idle = seen(body, commander_id, applied, "unit_idle")
                done = seen(body, commander_id, applied, "order_done")
                if idle and done:
                    break
                time.sleep(0.2)
            require(
                idle and done,
                f"no idle and order-done after tick {applied}: {body}\n"
                f"{log_tail(log_dir, stderr_lines)}",
            )

            call_id += 1
            resources = call_rpc(
                process, stdout_lines, stderr_lines, log_dir, call_id, "resources/list"
            )
            require("_error" not in resources, f"resources/list failed: {resources}")
            uris = {item.get("uri") for item in resources.get("resources") or []}
            require("oa://events" in uris, f"resources are {sorted(uris)}")

            call_id += 1
            reading = call_rpc(
                process, stdout_lines, stderr_lines, log_dir, call_id, "resources/read",
                {"uri": "oa://events"},
            )
            require("_error" not in reading, f"resources/read failed: {reading}")
            text = ((reading.get("contents") or [{}])[0]).get("text") or ""
            try:
                held = json.loads(text)
            except json.JSONDecodeError as error:
                raise Failure(f"oa://events was not JSON: {text[:200]!r}") from error
            require(
                seen(held, commander_id, applied, "unit_idle")
                and seen(held, commander_id, applied, "order_done"),
                f"oa://events lacks the notes: {held}",
            )

            if client is not None:
                client.close()
                client = None
            send(
                process,
                {
                    "jsonrpc": "2.0",
                    "id": 90,
                    "method": "tools/call",
                    "params": {"name": "game.quit"},
                },
            )
            quitting = next_message(stdout_lines, STEP_TIMEOUT, stderr_lines, log_dir)
            quit_text = ((quitting.get("result") or {}).get("content") or [{}])[0].get("text")
            require(json.loads(quit_text or "") == {"quitting": True}, f"quit answered {quitting}")
            code = process.wait(timeout=END_TIMEOUT)
            require(code == 0, f"the game exited {code}\n{log_tail(log_dir, stderr_lines)}")
        finally:
            if client is not None:
                client.close()
            if process.poll() is None:
                process.kill()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
            if process.stdin is not None:
                process.stdin.close()
    print("native-mcp-events: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError, automation.AutomationFailure) as error:
        print(f"native-mcp-events: {error}", file=sys.stderr)
        sys.exit(1)
