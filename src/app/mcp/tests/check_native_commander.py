#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Selects the local commander at tick 0 of a skirmish.

The game runs on the dummy SDL drivers with --mcp --mcp-clock stepped and
--fark, and without --give-orders. The clock does not advance until
clock.step, and this check never steps it. As soon as the local commander
is listed, even while the match is still paused, units.select must accept
it and the tick must still be 0. Standard input and output are the MCP
stream. The automation endpoint is the socket --fark-file names. The log,
the preferences and the user folder go to a scratch folder that is new for
every run.
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
import native_offline  # noqa: E402
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


def log_text(log_dir):
    """Returns the text of every file the game wrote under the log folder."""
    text = ""
    for path in sorted(log_dir.rglob("*")):
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def next_message(lines, timeout, stderr_lines):
    """Returns the next JSON object, skipping blank lines."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            err = "".join(stderr_lines)[-2000:]
            raise Failure(f"no MCP message arrived\n{err}")
        try:
            line = lines.get(timeout=remaining)
        except queue.Empty:
            err = "".join(stderr_lines)[-2000:]
            raise Failure(f"no MCP message arrived\n{err}") from None
        if line is None:
            err = "".join(stderr_lines)[-2000:]
            raise Failure(f"standard output closed before a message\n{err}")
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


class Mcp:
    """One MCP client on the game's standard streams."""

    def __init__(self, process, lines, stderr_lines):
        self.process = process
        self.lines = lines
        self.stderr_lines = stderr_lines
        self.next_id = 1

    def request(self, method, params=None, timeout=STEP_TIMEOUT):
        """Sends one request and returns the answer with its id."""
        call_id = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": call_id, "method": method}
        if params is not None:
            message["params"] = params
        send(self.process, message)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                err = "".join(self.stderr_lines)[-2000:]
                raise Failure(f"no answer to {method}\n{err}")
            answer = next_message(self.lines, remaining, self.stderr_lines)
            if answer.get("id") == call_id:
                return answer

    def notify(self, method):
        """Sends one notification."""
        send(self.process, {"jsonrpc": "2.0", "method": method})

    def tool(self, name, arguments=None, timeout=STEP_TIMEOUT):
        """Calls a tool and returns its text parsed as JSON, or _error."""
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        answer = self.request("tools/call", params, timeout)
        if "error" in answer:
            raise Failure(f"{name} was refused: {answer}")
        result = answer.get("result") or {}
        content = (result.get("content") or [{}])[0]
        text = content.get("text") or ""
        if result.get("isError"):
            return {"_error": text}
        try:
            return json.loads(text)
        except json.JSONDecodeError as error:
            raise Failure(f"{name} answered {content!r}") from error


def wait_endpoint(path, process, stderr_lines):
    """Waits for the automation endpoint's file and returns what it holds."""
    deadline = time.monotonic() + automation.TIMEOUT
    while time.monotonic() < deadline:
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
        if process.poll() is not None:
            break
        time.sleep(0.1)
    err = "".join(stderr_lines)[-2000:]
    raise Failure(f"the game wrote no endpoint file\n{err}")


def connect(endpoint):
    """Connects to the endpoint, retrying while a previous client still holds it."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        client = automation.Client(endpoint["address"])
        hello = client.request(
            "hello", versions=[1], client="native-mcp-commander", token=endpoint["token"]
        )
        if hello.get("ok"):
            return client
        client.close()
        if not automation.refused(hello, "busy") or time.monotonic() > deadline:
            raise Failure(f"hello was answered {hello}")
        time.sleep(0.1)


def is_commander(unit):
    """A commander is a unit whose type name ends in COM."""
    return str(unit.get("type") or "").upper().endswith("COM")


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M16-mcp-commander-", dir=root) as work:
        workdir = Path(work)
        preferences = workdir / "mcp.conf"
        preferences.write_text(automation.PREFERENCES, encoding="utf-8")
        log_dir = workdir / "logs"
        user_folder = workdir / "user"
        endpoint_file = workdir / "endpoint" / "automation.json"
        log_dir.mkdir()
        user_folder.mkdir()
        endpoint_file.parent.mkdir()
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
                "--mcp-clock",
                "stepped",
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
            mcp = Mcp(process, stdout_lines, stderr_lines)
            started = mcp.request(
                "initialize",
                {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "native-mcp-commander", "version": "0"},
                },
                timeout=START_TIMEOUT,
            )
            result = started.get("result") or {}
            require(
                result.get("protocolVersion") == "2025-06-18",
                f"protocol is {result.get('protocolVersion')}",
            )
            mcp.notify("notifications/initialized")
            listed = mcp.request("tools/list")
            names = {tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])}
            require("units.select" in names and "units.list" in names, f"tools are {sorted(names)}")

            endpoint = wait_endpoint(endpoint_file, process, stderr_lines)
            client = connect(endpoint)
            automation.wait_screen(client, "main_menu")
            automation.click_control(client, "SINGLE", "main_menu")
            automation.wait_screen(client, "single_player")
            automation.click_control(client, "Skirmish", "single_player")
            automation.wait_screen(client, "skirmish")
            automation.click_control(client, "Start", "skirmish")
            automation.wait_screen(client, "match")

            # The commander is placed before the match screen. Select it as
            # soon as it is listed, without waiting for the pause to lift and
            # without stepping the clock.
            commander = None
            last = None
            deadline = time.monotonic() + MATCH_TIMEOUT
            while time.monotonic() < deadline:
                mine = mcp.tool("units.list", {"owner": "me"})
                last = mine
                if "_error" in mine:
                    time.sleep(0.2)
                    continue
                if mine.get("tick") != 0:
                    raise Failure(
                        "the match left tick 0 before the commander was selected: "
                        + str(mine)
                    )
                commander = next(
                    (unit for unit in mine.get("units") or [] if is_commander(unit)), None
                )
                if commander is not None:
                    break
                time.sleep(0.2)
            require(commander is not None, f"the commander did not appear at tick 0: {last}")
            commander_id = commander.get("id")
            require(isinstance(commander_id, int), f"the commander is {commander}")

            selected = mcp.tool("units.select", {"units": [commander_id]})
            require("_error" not in selected, f"units.select failed: {selected}")
            require(selected.get("applied_tick") == 0, f"select landed on {selected}")
            require(commander_id in (selected.get("selected") or []), f"selected is {selected}")

            got = mcp.tool("units.get", {"id": commander_id})
            require("_error" not in got, f"units.get failed: {got}")
            require(
                ((got.get("unit") or {}).get("flags") or {}).get("selected") is True,
                f"the commander is not selected: {got}",
            )
            status = mcp.tool("game.status")
            require(
                status.get("tick") == 0 and status.get("in_match") is True,
                f"the match is {status}",
            )
            require(
                f"mcp: 0 units.select {commander_id}" in log_text(log_dir),
                "the log has no units.select line for tick 0",
            )

            quitting = mcp.tool("game.quit")
            require(quitting == {"quitting": True}, f"quit answered {quitting}")
            code = process.wait(timeout=END_TIMEOUT)
            require(code == 0, f"the game exited {code}\n{''.join(stderr_lines)[-2000:]}")
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
    print("native-mcp-commander: ok tick 0")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, automation.AutomationFailure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-mcp-commander: {error}", file=sys.stderr)
        sys.exit(1)
