#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish read through game.info and game.status.

The game runs on the dummy SDL drivers with --mcp --mcp-clock stepped and
--fark. Standard input and output are the MCP stream. The check reads the
engine version and the mod before the match, starts a skirmish, and checks
the unit limit, the local metal and energy, a clock that advances, and a
computer player's economy left unread.
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
SEED = 20261103
START_TIMEOUT = 120
STEP_TIMEOUT = 30
SETTLE_TIMEOUT = 30
END_TIMEOUT = 60
UNIT_LIMIT = 400
# Long enough for a commander's storage to be copied into the player.
FILL_TICKS = 60
# A few seconds of the simulation, under the ticks one frame of clock.step runs.
STEP_TICKS = 90
PREFERENCES = (
    automation.PREFERENCES + f'"open-annihilation.unit-limit" "{UNIT_LIMIT}"\n'
)


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


def require(condition, message):
    """Raises Failure when a condition does not hold."""
    if not condition:
        raise Failure(message)


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
        """Calls a tool and returns its text parsed as JSON."""
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        answer = self.request("tools/call", params, timeout)
        if "error" in answer:
            raise Failure(f"{name} was refused: {answer}")
        result = answer.get("result") or {}
        if result.get("isError"):
            raise Failure(f"{name} failed: {answer}")
        content = (result.get("content") or [{}])[0]
        try:
            return json.loads(content.get("text") or "")
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
        hello = client.request("hello", versions=[1], client="check", token=endpoint["token"])
        if hello.get("ok"):
            return client
        client.close()
        if not automation.refused(hello, "busy") or time.monotonic() > deadline:
            raise Failure(f"hello was answered {hello}")
        time.sleep(0.1)


def positive(value):
    """True when a JSON number is above zero."""
    return type(value) in (int, float) and value > 0


def local_economy(status):
    """Returns the local player's economy, or None."""
    for player in status.get("players") or []:
        if player.get("local") is True:
            return player.get("economy")
    return None


def stores_ready(economy):
    """True when metal and energy each have an amount and a storage above zero."""
    if not isinstance(economy, dict):
        return False
    for name in ("metal", "energy"):
        store = economy.get(name)
        if not isinstance(store, dict):
            return False
        if not positive(store.get("amount")) or not positive(store.get("storage")):
            return False
    return True


def one_run(native, game_dir, workdir):
    """Starts one skirmish and reads game.info and game.status."""
    workdir.mkdir(parents=True)
    preferences = workdir / "mcp.conf"
    preferences.write_text(PREFERENCES, encoding="utf-8")
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
            str(native),
            "--game-dir",
            str(game_dir),
            "--skip-intro",
            "--mute",
            "--mcp",
            "--mcp-clock",
            "stepped",
            "--seed",
            str(SEED),
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
                "clientInfo": {"name": "native-mcp-status", "version": "0"},
            },
            timeout=START_TIMEOUT,
        )
        result = started.get("result") or {}
        version = (result.get("serverInfo") or {}).get("version")
        require(
            isinstance(version, str) and version != "",
            f"initialize named no version: {result}",
        )
        mcp.notify("notifications/initialized")
        listed = mcp.request("tools/list")
        names = {tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])}
        require({"game.info", "game.status"} <= names, f"tools are {sorted(names)}")

        endpoint = wait_endpoint(endpoint_file, process, stderr_lines)
        client = connect(endpoint)
        automation.wait_screen(client, "main_menu")
        before = mcp.tool("game.info")
        require(before.get("in_match") is False, f"a match was running at the menu: {before}")
        require(
            before.get("engine") == version,
            f"engine was {before.get('engine')}, initialize {version}",
        )
        require(before.get("mod") is None, f"the base game named a mod: {before.get('mod')}")

        automation.click_control(client, "SINGLE", "main_menu")
        automation.wait_screen(client, "single_player")
        automation.click_control(client, "Skirmish", "single_player")
        automation.wait_screen(client, "skirmish")
        automation.click_control(client, "Start", "skirmish")
        automation.wait_screen(client, "match")

        deadline = time.monotonic() + SETTLE_TIMEOUT
        status = {}
        while time.monotonic() < deadline:
            status = mcp.tool("game.status")
            if (
                status.get("in_match") is True
                and status.get("paused") is False
                and isinstance(status.get("tick"), int)
            ):
                break
            time.sleep(0.2)
        else:
            raise Failure(f"the match did not settle: {status}")

        info = mcp.tool("game.info")
        require(info.get("in_match") is True, f"game.info left the match: {info}")
        require(info.get("engine") == version, f"engine changed to {info.get('engine')}")
        require(info.get("mod") is None, f"the skirmish named a mod: {info.get('mod')}")
        rules = info.get("rules") or {}
        require(
            rules.get("unit_limit") == UNIT_LIMIT,
            f"game.info unit limit is {rules.get('unit_limit')}",
        )
        require(
            status.get("unit_limit") == UNIT_LIMIT,
            f"game.status unit limit is {status.get('unit_limit')}",
        )

        economy = local_economy(status)
        if not stores_ready(economy):
            mcp.tool("clock.step", {"ticks": FILL_TICKS})
            status = mcp.tool("game.status")
            economy = local_economy(status)
        require(stores_ready(economy), f"the local economy is {economy}")

        held = status.get("clock") or {}
        held_tick = held.get("tick")
        require(isinstance(held_tick, int), f"the clock named no tick: {status}")
        stepped = mcp.tool("clock.step", {"ticks": STEP_TICKS})
        require(stepped.get("ran") == STEP_TICKS, f"step ran {stepped}")
        seen = mcp.tool("game.status")
        clock = seen.get("clock") or {}
        require(
            isinstance(clock.get("tick"), int) and clock["tick"] > held_tick,
            f"the clock stayed at {held_tick}: {clock}",
        )
        require(
            isinstance(clock.get("seconds"), int) and clock["seconds"] > held.get("seconds", -1),
            f"the seconds stayed: {held} then {clock}",
        )
        require(
            isinstance(clock.get("display"), str) and clock["display"] != held.get("display"),
            f"the display stayed {held.get('display')}: {clock}",
        )
        require(
            isinstance(seen.get("tick"), int) and seen["tick"] > status.get("tick"),
            f"the match tick stayed: {seen.get('tick')}",
        )

        computer = None
        for player in seen.get("players") or []:
            if player.get("computer") is True:
                computer = player
                break
        require(computer is not None, f"the match seated no computer: {seen}")
        require(computer.get("economy") is None, f"the computer's economy was read: {computer}")

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


def main():
    """Runs the check once in a fresh folder."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M14-mcp-status-", dir=root) as work:
        one_run(args.native, args.game_dir, Path(work) / "run")
    print("native-mcp-status: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, automation.AutomationFailure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-mcp-status: {error}", file=sys.stderr)
        sys.exit(1)
