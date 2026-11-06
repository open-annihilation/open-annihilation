#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish lists its units, and a factory's queue, over MCP.

The game runs on the dummy SDL drivers, started with --mcp, --fark and
--give-orders. This client starts a skirmish the way the benchmark does,
then reads units.list, the factory's queue, units.get and units.types.
Standard input and output are the MCP stream. The automation endpoint is
the socket --fark-file names, so the two do not share a stream. Its first
frame is hello, carrying the token from the endpoint file. The log, the
preferences and the user folder go to a scratch folder that is new for
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
    answer = next_message(lines, timeout, stderr_lines, log_dir)
    require(answer.get("id") == call_id, f"{name} answered {answer}")
    result = answer.get("result") or {}
    content = (result.get("content") or [{}])[0]
    text = content.get("text") or ""
    if result.get("isError"):
        return {"_error": text}
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise Failure(f"{name} did not return JSON: {text!r}") from error


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


def ids_of(units):
    """The ids of a units array."""
    return [unit.get("id") for unit in units]


def hidden_ids(units):
    """The ids of units the player is not shown."""
    return [unit.get("id") for unit in units if unit.get("shown") is False]


def connect(endpoint):
    """Opens the endpoint. hello, with the file's token, is the first frame."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        client = automation.Client(endpoint["address"])
        hello = client.request(
            "hello", versions=[1], client="native-mcp-units", token=endpoint["token"]
        )
        if hello.get("ok") is True:
            return client
        client.close()
        if not automation.refused(hello, "busy") or time.monotonic() > deadline:
            raise Failure(f"hello was answered {hello}")
        time.sleep(0.1)


def find_build(queues, name):
    """An order whose product is name, from either queue, or None."""
    for queue in queues:
        for order in queue:
            if order.get("build") == name:
                return order
    return None


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M07-mcp-", dir=root) as work:
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
                "--give-orders",
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
                        "clientInfo": {"name": "native-mcp-units", "version": "0"},
                    },
                },
            )
            started = next_message(stdout_lines, START_TIMEOUT, stderr_lines, log_dir)
            require(started.get("id") == 1, f"initialize answered {started}")
            send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
            send(process, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            listed = next_message(stdout_lines, STEP_TIMEOUT, stderr_lines, log_dir)
            names = {tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])}
            for name in (
                "units.list",
                "units.get",
                "units.orders",
                "units.types",
                "game.status",
                "game.speed",
                "game.pause",
                "game.quit",
            ):
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

            lab = None
            deadline = time.monotonic() + MATCH_TIMEOUT
            call_id = 10
            last = None
            while time.monotonic() < deadline:
                call_id += 1
                last = call_tool(
                    process, stdout_lines, stderr_lines, log_dir, call_id, "units.list",
                    {"owner": "me", "type": "ARMLAB"},
                )
                if "_error" not in last:
                    found = last.get("units") or []
                    if found:
                        lab = found[0]
                        break
                time.sleep(0.2)
            require(lab is not None, f"no ARMLAB of mine appeared: {last}\n{log_tail(log_dir, stderr_lines)}")
            lab_id = lab.get("id")
            require(isinstance(lab_id, int), f"the lab is {lab}")

            call_id += 1
            orders = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.orders",
                {"id": lab_id},
            )
            require("_error" not in orders, f"units.orders failed: {orders}")
            build = find_build((orders.get("primary") or [], orders.get("secondary") or []), "ARMPW")
            require(
                build is not None and build.get("parameter_2") == 3,
                f"the lab's queue is {orders}",
            )

            call_id += 1
            mine = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.list",
                {"owner": "me"},
            )
            require("_error" not in mine, f"units.list failed: {mine}")
            commander = next(
                (unit for unit in mine.get("units") or [] if str(unit.get("type", "")).upper() == "ARMCOM"),
                None,
            )
            require(commander is not None, f"no commander of mine: {mine}")
            call_id += 1
            got = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.get",
                {"id": commander.get("id")},
            )
            require("_error" not in got, f"units.get failed: {got}")
            require(
                str((got.get("unit") or {}).get("type", "")).upper() == "ARMCOM",
                f"units.get returned {got}",
            )

            call_id += 1
            catalog = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.types",
                {"type": "ARMLAB"},
            )
            require("_error" not in catalog, f"units.types failed: {catalog}")
            types = catalog.get("types") or []
            require(len(types) == 1, f"units.types returned {catalog}")
            builds = [str(name).upper() for name in types[0].get("builds") or []]
            require("ARMPW" in builds, f"ARMLAB builds {builds}")

            call_id += 1
            shown = call_tool(process, stdout_lines, stderr_lines, log_dir, call_id, "units.list")
            require("_error" not in shown, f"units.list failed: {shown}")
            call_id += 1
            full = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.list",
                {"awareness": "full"},
            )
            require("_error" not in full, f"full awareness failed: {full}")
            shown_ids = set(ids_of(shown.get("units") or []))
            full_ids = set(ids_of(full.get("units") or []))
            require(shown_ids <= full_ids, f"the player's list is not within the full list: {shown_ids - full_ids}")
            leaked = [unit_id for unit_id in hidden_ids(full.get("units") or []) if unit_id in shown_ids]
            require(not leaked, f"units the player is not shown are in the list: {leaked}")

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
    print("native-mcp-units: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError, automation.AutomationFailure) as error:
        print(f"native-mcp-units: {error}", file=sys.stderr)
        sys.exit(1)
