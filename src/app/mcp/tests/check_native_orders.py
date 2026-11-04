#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish orders its own units over MCP, and refuses an enemy's.

The game runs on the dummy SDL drivers, started with --mcp and --fark, and
without --give-orders, so the commander's queue starts empty. This client
starts a skirmish the way the benchmark does, then selects the commander,
sets its fire order, builds a solar collector, moves, attacks, and reads
the queues back. An order for the enemy commander is refused. Standard
input and output are the MCP stream. The automation endpoint is the socket
--fark-file names, so the two do not share a stream. Its first frame is
hello, carrying the token from the endpoint file. The log, the preferences
and the user folder go to a scratch folder that is new for every run.
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


def log_text(log_dir):
    """The text of every log file, for a check of the landed lines."""
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
                parts.append(text)
    return "\n".join(parts)


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
            "hello", versions=[1], client="native-mcp-orders", token=endpoint["token"]
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


def queued(result):
    """The primary and secondary orders of an orders.issue or orders.state result."""
    found = []
    for unit in result.get("units") or []:
        found.extend(unit.get("primary") or [])
        found.extend(unit.get("secondary") or [])
    return found


def view_queued(result):
    """The primary and secondary orders of a units.orders result."""
    return (result.get("primary") or []) + (result.get("secondary") or [])


def solar_name(catalog):
    """A build whose unit name ends in SOLAR (ARMSOLAR, CORSOLAR), ARMSOLAR first."""
    types = catalog.get("types") or []
    if len(types) != 1:
        return None
    builds = [str(name).upper() for name in types[0].get("builds") or []]
    for name in builds:
        if name == "ARMSOLAR":
            return name
    for name in builds:
        if name.endswith("SOLAR"):
            return name
    return None


def has_build(orders, name):
    """A mobile build of name."""
    for item in orders:
        kind = str(item.get("kind") or "")
        if str(item.get("build") or "").upper() == name and kind in ("MobileBuild", "VTOL_MobileBuild"):
            return True
    return False


def has_move(orders, x_pixel, z_pixel):
    """A move whose point is the asked x and z."""
    for item in orders:
        kind = str(item.get("kind") or "")
        if kind not in ("Move_Ground", "VTOL_Move", "QMove"):
            continue
        point = item.get("point") or []
        if len(point) >= 3 and point[0] == x_pixel and point[2] == z_pixel:
            return True
    return False


def has_attack(orders, target):
    """An attack whose target is the enemy, or a Suppress order on it."""
    for item in orders:
        kind = str(item.get("kind") or "")
        if item.get("target") != target:
            continue
        if "Attack" in kind or kind == "Suppress":
            return True
    return False


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M08-mcp-", dir=root) as work:
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
                        "clientInfo": {"name": "native-mcp-orders", "version": "0"},
                    },
                },
            )
            started = next_message(stdout_lines, START_TIMEOUT, stderr_lines, log_dir)
            require(started.get("id") == 1, f"initialize answered {started}")
            send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
            send(process, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            listed = next_message(stdout_lines, STEP_TIMEOUT, stderr_lines, log_dir)
            names = {tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])}
            for name in ("orders.issue", "orders.state", "units.select"):
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

            # Placing a commander can pause the match for a moment. Wait until
            # both commanders are on the field and the match is running.
            commander = None
            enemy = None
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
                call_id += 1
                full = call_tool(
                    process, stdout_lines, stderr_lines, log_dir, call_id, "units.list",
                    {"awareness": "full"},
                )
                last = {"status": status, "mine": mine, "full": full}
                if "_error" in status or "_error" in mine or "_error" in full:
                    time.sleep(0.2)
                    continue
                commander = next((unit for unit in mine.get("units") or [] if is_commander(unit)), None)
                if commander is None:
                    time.sleep(0.2)
                    continue
                owner = commander.get("owner")
                enemy = next(
                    (
                        unit
                        for unit in full.get("units") or []
                        if is_commander(unit) and unit.get("owner") != owner
                    ),
                    None,
                )
                if enemy is not None and status.get("in_match") is True and status.get("paused") is False:
                    break
                time.sleep(0.2)
            require(
                commander is not None and enemy is not None,
                f"the commanders did not appear: {last}\n{log_tail(log_dir, stderr_lines)}",
            )
            commander_id = commander.get("id")
            enemy_id = enemy.get("id")
            position = commander.get("position") or []
            require(
                isinstance(commander_id, int) and isinstance(enemy_id, int) and len(position) >= 3,
                f"the commanders are {commander} and {enemy}",
            )
            origin_x, origin_y, origin_z = position[0], position[1], position[2]

            call_id += 1
            paused = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "game.pause",
                {"paused": True},
            )
            require("_error" not in paused and paused.get("paused") is True, f"pause answered {paused}")

            call_id += 1
            selected = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.select",
                {"units": [commander_id]},
            )
            require("_error" not in selected, f"units.select failed: {selected}")
            require(commander_id in (selected.get("selected") or []), f"selected is {selected}")
            call_id += 1
            got = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.get",
                {"id": commander_id},
            )
            require("_error" not in got, f"units.get failed: {got}")
            require(
                ((got.get("unit") or {}).get("flags") or {}).get("selected") is True,
                f"the commander is not selected: {got}",
            )

            call_id += 1
            refused_select = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.select",
                {"units": [enemy_id]},
            )
            require(
                "not this machine's" in refused_select.get("_error", ""),
                f"selecting the enemy answered {refused_select}",
            )
            call_id += 1
            still = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.get",
                {"id": commander_id},
            )
            require(
                ((still.get("unit") or {}).get("flags") or {}).get("selected") is True,
                f"the selection changed: {still}",
            )

            call_id += 1
            standing = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "orders.state",
                {"units": [commander_id], "state": "fire", "value": 1},
            )
            require("_error" not in standing, f"orders.state failed: {standing}")
            require(
                any(str(item.get("kind") or "") == "Standing_FireOrder" for item in queued(standing)),
                f"the fire order is {standing}",
            )

            call_id += 1
            catalog = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.types",
                {"type": commander.get("type")},
            )
            require("_error" not in catalog, f"units.types failed: {catalog}")
            solar = solar_name(catalog)
            require(solar is not None, f"the commander builds no solar: {catalog}")

            call_id += 1
            built = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "orders.issue",
                {
                    "units": [commander_id],
                    "kind": "build",
                    "type": solar,
                    "point": [origin_x + 64, origin_y, origin_z + 32],
                    "append": False,
                },
            )
            require("_error" not in built, f"the build failed: {built}")
            require(isinstance(built.get("applied_tick"), int), f"the build named no tick: {built}")
            require(has_build(queued(built), solar), f"the build queue is {built}")
            call_id += 1
            confirmed = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.orders",
                {"id": commander_id},
            )
            require("_error" not in confirmed, f"units.orders failed: {confirmed}")
            require(has_build(view_queued(confirmed), solar), f"units.orders is {confirmed}")

            move_x = origin_x + 48
            move_z = origin_z
            call_id += 1
            moved = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "orders.issue",
                {
                    "units": [commander_id],
                    "kind": "move",
                    "point": [move_x, origin_y, move_z],
                    "append": True,
                },
            )
            require("_error" not in moved, f"the move failed: {moved}")
            require(has_build(queued(moved), solar), f"the build left the queue: {moved}")
            require(has_move(queued(moved), move_x, move_z), f"the move queue is {moved}")

            call_id += 1
            enemy_before = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.get",
                {"id": enemy_id, "awareness": "full"},
            )
            require("_error" not in enemy_before, f"the enemy could not be read: {enemy_before}")
            enemy_order = (enemy_before.get("unit") or {}).get("order")

            call_id += 1
            attacked = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "orders.issue",
                {
                    "units": [commander_id],
                    "kind": "attack",
                    "target": enemy_id,
                    "append": False,
                },
            )
            require("_error" not in attacked, f"the attack failed: {attacked}")
            require(has_attack(queued(attacked), enemy_id), f"the attack queue is {attacked}")

            call_id += 1
            refused = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "orders.issue",
                {"units": [enemy_id], "kind": "move", "point": [move_x, move_z]},
            )
            require(
                "not this machine's" in refused.get("_error", ""),
                f"ordering the enemy answered {refused}",
            )
            call_id += 1
            enemy_after = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "units.get",
                {"id": enemy_id, "awareness": "full"},
            )
            require("_error" not in enemy_after, f"the enemy could not be read: {enemy_after}")
            require(
                (enemy_after.get("unit") or {}).get("order") == enemy_order,
                f"the enemy's order changed from {enemy_order!r} to {enemy_after}",
            )

            logged = log_text(log_dir)
            for detail in (
                "orders.state fire",
                "orders.issue build",
                "orders.issue move",
                "orders.issue attack",
                "units.select",
            ):
                require(detail in logged, f"the log has no {detail!r} line\n{log_tail(log_dir, stderr_lines)}")

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
    print("native-mcp-orders: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError, automation.AutomationFailure) as error:
        print(f"native-mcp-orders: {error}", file=sys.stderr)
        sys.exit(1)
