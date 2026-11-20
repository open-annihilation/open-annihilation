#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A stepped skirmish run twice, with the same saved-state hash.

Each run sets the speed and steps the same number of ticks. The game runs on the dummy SDL drivers with
--mcp --mcp-clock stepped and --fark. Standard input and output are the
MCP stream. The check clicks through the menus on the automation endpoint,
waits while the tick is held, steps, and reads the match's state hash.
The two runs must name the same tick and the same hash. The hash is read
twice in a run, a moment apart, and those two must match as well: the
clock stays held after the step.
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
# Long enough for the view to finish moving while the tick is held. The
# state hash includes the camera.
HOLD_SECONDS = 1.0
# Under the ticks one frame of clock.step runs, so the call finishes in that frame.
STEP_TICKS = 40
SPEED = 4


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
        hello = client.request(
            "hello", versions=[1], client="check", token=endpoint["token"]
        )
        if hello.get("ok"):
            return client
        client.close()
        if not automation.refused(hello, "busy") or time.monotonic() > deadline:
            raise Failure(f"hello was answered {hello}")
        time.sleep(0.1)


def log_text(log_dir):
    """Returns the text of every file the game wrote under the log folder."""
    text = ""
    for path in sorted(log_dir.rglob("*")):
        if path.is_file():
            text += path.read_text(encoding="utf-8", errors="replace")
    return text


def state_hash(client):
    """Returns the match's saved-state hash."""
    answer = client.request("match", state_hash=True)
    if answer.get("ok") is False:
        raise Failure(f"match was refused: {answer}")
    digest = answer.get("state_hash")
    if not isinstance(digest, str) or len(digest) != 16:
        raise Failure(f"match named no state hash: {answer}")
    try:
        int(digest, 16)
    except ValueError as error:
        raise Failure(f"state hash was {digest!r}") from error
    return digest


def one_run(native, game_dir, workdir):
    """Plays one stepped skirmish and returns its tick and state hash."""
    workdir.mkdir(parents=True)
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
            str(native),
            "--game-dir",
            str(game_dir),
            "--skip-intro",
            "--mute",
            "--mcp",
            "--mcp-clock",
            "stepped",
            # The skirmish draws its random seed from the time unless given one.
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
                "clientInfo": {"name": "native-mcp-clock", "version": "0"},
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
        require(
            {"clock.mode", "clock.step", "clock.run", "game.speed", "game.status"} <= names,
            f"tools are {sorted(names)}",
        )
        endpoint = wait_endpoint(endpoint_file, process, stderr_lines)
        client = connect(endpoint)
        automation.wait_screen(client, "main_menu")
        automation.click_control(client, "SINGLE", "main_menu")
        automation.wait_screen(client, "single_player")
        automation.click_control(client, "Skirmish", "single_player")
        automation.wait_screen(client, "skirmish")
        automation.click_control(client, "Start", "skirmish")
        automation.wait_screen(client, "match")

        # Placing a commander can pause the match for a moment. Wait until it
        # is running, as the play check does, before judging the held clock.
        deadline = time.monotonic() + SETTLE_TIMEOUT
        status = {}
        while time.monotonic() < deadline:
            status = mcp.tool("game.status")
            if (
                status.get("in_match") is True
                and status.get("paused") is False
                and isinstance(status.get("speed"), int)
                and isinstance(status.get("tick"), int)
            ):
                break
            time.sleep(0.2)
        else:
            raise Failure(f"the match did not settle: {status}")
        held_tick = status.get("tick")
        time.sleep(HOLD_SECONDS)
        still = mcp.tool("game.status")
        require(
            still.get("tick") == held_tick and still.get("paused") is False,
            f"the clock moved while held: {held_tick} then {still}",
        )

        # A speed the match already has is refused, so pick the other one the
        # play check uses. Both runs start from the same preferences, so they
        # pick the same speed and the hashes still match.
        current = status.get("speed")
        target = 12 if current == SPEED else SPEED
        changed = mcp.tool("game.speed", {"speed": target})
        require(changed.get("speed") == target, f"speed answered {changed}")
        require(changed.get("applied_tick") == held_tick, f"speed moved the tick: {changed}")

        stepped = mcp.tool("clock.step", {"ticks": STEP_TICKS})
        require(stepped.get("ran") == STEP_TICKS, f"step ran {stepped}")
        require(stepped.get("events") == [], f"step reported events {stepped}")
        reached = stepped.get("tick")
        require(reached == held_tick + STEP_TICKS, f"step reached {stepped} from {held_tick}")
        seen = mcp.tool("game.status")
        require(seen.get("tick") == reached, f"status tick is {seen.get('tick')} after {reached}")

        digest = state_hash(client)
        time.sleep(0.5)
        again = state_hash(client)
        require(again == digest, f"the hash moved while held: {digest} then {again}")
        require(
            mcp.tool("game.status").get("tick") == reached,
            "the tick moved after the step",
        )
        logged = log_text(log_dir)
        require(
            f"mcp: {reached} clock.step {STEP_TICKS}" in logged,
            f"the log has no clock.step line for tick {reached}",
        )

        quitting = mcp.tool("game.quit")
        require(quitting == {"quitting": True}, f"quit answered {quitting}")
        code = process.wait(timeout=END_TIMEOUT)
        require(code == 0, f"the game exited {code}\n{''.join(stderr_lines)[-2000:]}")
        return {"tick": reached, "state_hash": digest}
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
    """Runs the check twice and compares the hashes."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M06-mcp-clock-", dir=root) as work:
        workdir = Path(work)
        first = one_run(args.native, args.game_dir, workdir / "run-a")
        second = one_run(args.native, args.game_dir, workdir / "run-b")
    require(
        first["tick"] == second["tick"] and first["state_hash"] == second["state_hash"],
        f"the two runs differ: {first} and {second}",
    )
    print(f"native-mcp-clock: ok tick {first['tick']} hash {first['state_hash']}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, automation.AutomationFailure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-mcp-clock: {error}", file=sys.stderr)
        sys.exit(1)
