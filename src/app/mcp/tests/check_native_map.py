#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish's sight matches the minimap's fog, over MCP.

The game runs on the dummy SDL drivers, started with --mcp and --fark.
This client starts a skirmish the way the benchmark does, then reads
map.visibility for sight and explored and compares each grid with the
minimap field of the same answer. It also reads map.info and map.radar.
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
PNG_CELLS = 256 * 256
PNG_PREFIX = "iVBORw0KGgo"


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


def connect(endpoint):
    """Opens the endpoint. hello, with the file's token, is the first frame."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        client = automation.Client(endpoint["address"])
        hello = client.request(
            "hello", versions=[1], client="native-mcp-map", token=endpoint["token"]
        )
        if hello.get("ok") is True:
            return client
        client.close()
        if not automation.refused(hello, "busy") or time.monotonic() > deadline:
            raise Failure(f"hello was answered {hello}")
        time.sleep(0.1)


def expand_runs(runs, width, height, label):
    """Expands run-length pairs into one 0 or 1 per cell."""
    cells = []
    require(isinstance(runs, list), f"{label} runs are {runs!r}")
    for run in runs:
        require(isinstance(run, list) and len(run) == 2, f"{label} run {run!r}")
        count, value = run
        require(isinstance(count, int) and count > 0, f"{label} run {run!r}")
        require(value in (0, 1), f"{label} run {run!r}")
        cells.extend([value] * count)
    require(
        len(cells) == width * height,
        f"{label} covers {len(cells)} cells of a {width} by {height} grid",
    )
    return cells


def first_difference(left, right):
    """The first index where two grids differ, or None."""
    for index, (one, other) in enumerate(zip(left, right)):
        if one != other:
            return index
    if len(left) != len(right):
        return min(len(left), len(right))
    return None


def wait_fog(process, lines, stderr_lines, log_dir, call_id, layer):
    """Reads one layer until its grid matches the minimap and is partly on.

    A missing grid, or one that is all on or all off, is waited out: the
    match may not have a sight grid on the first frames. A grid that is
    present and differs from the minimap fails at once.
    """
    deadline = time.monotonic() + MATCH_TIMEOUT
    last = None
    while time.monotonic() < deadline:
        call_id += 1
        last = call_tool(
            process,
            lines,
            stderr_lines,
            log_dir,
            call_id,
            "map.visibility",
            {"player": "me", "layer": layer, "cell": 32, "format": "rle"},
        )
        if "_error" in last or "rle" not in last or "minimap" not in last:
            time.sleep(0.2)
            continue
        width = last.get("w")
        height = last.get("h")
        require(
            isinstance(width, int) and isinstance(height, int) and width > 0 and height > 0,
            f"{layer} is {last}",
        )
        cells = expand_runs(last["rle"], width, height, layer)
        fog = expand_runs(last["minimap"], width, height, f"{layer} minimap")
        differ = first_difference(cells, fog)
        if differ is not None:
            raise Failure(f"{layer} differs from the minimap at cell {differ}")
        total = sum(cells)
        if 0 < total < len(cells):
            return last, call_id
        time.sleep(0.2)
    raise Failure(
        f"{layer} never matched a partial minimap fog: {last}\n{log_tail(log_dir, stderr_lines)}"
    )


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M09-mcp-", dir=root) as work:
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
                        "clientInfo": {"name": "native-mcp-map", "version": "0"},
                    },
                },
            )
            started = next_message(stdout_lines, START_TIMEOUT, stderr_lines, log_dir)
            require(started.get("id") == 1, f"initialize answered {started}")
            send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
            send(process, {"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
            listed = next_message(stdout_lines, STEP_TIMEOUT, stderr_lines, log_dir)
            names = {tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])}
            for name in ("map.info", "map.visibility", "map.radar", "game.quit"):
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

            call_id = 10
            sight, call_id = wait_fog(
                process, stdout_lines, stderr_lines, log_dir, call_id, "sight"
            )
            _explored, call_id = wait_fog(
                process, stdout_lines, stderr_lines, log_dir, call_id, "explored"
            )

            call_id += 1
            refused = call_tool(
                process,
                stdout_lines,
                stderr_lines,
                log_dir,
                call_id,
                "map.visibility",
                {"player": "1", "layer": "sight"},
            )
            require(
                refused.get("_error") == "another player's view needs full awareness",
                f"another player's sight was {refused}",
            )
            call_id += 1
            other = call_tool(
                process,
                stdout_lines,
                stderr_lines,
                log_dir,
                call_id,
                "map.visibility",
                {"player": "1", "layer": "sight", "awareness": "full", "cell": 32},
            )
            require("_error" not in other, f"full awareness failed: {other}")
            require(other.get("player") == 1, f"full awareness returned {other}")

            png_cell = 32
            if sight["w"] * sight["h"] > PNG_CELLS:
                png_cell = 128
            call_id += 1
            png = call_tool(
                process,
                stdout_lines,
                stderr_lines,
                log_dir,
                call_id,
                "map.visibility",
                {"layer": "sight", "format": "png", "cell": png_cell},
            )
            if png.get("_error") == "png needs a coarser cell":
                png_cell = 512
                call_id += 1
                png = call_tool(
                    process,
                    stdout_lines,
                    stderr_lines,
                    log_dir,
                    call_id,
                    "map.visibility",
                    {"layer": "sight", "format": "png", "cell": png_cell},
                )
            require("_error" not in png, f"map.visibility png failed: {png}")
            require(
                isinstance(png.get("png"), str) and png["png"].startswith(PNG_PREFIX),
                f"the png is {str(png.get('png'))[:16]!r}",
            )

            call_id += 1
            info = call_tool(
                process,
                stdout_lines,
                stderr_lines,
                log_dir,
                call_id,
                "map.info",
                {"cell": 128},
            )
            require("_error" not in info, f"map.info failed: {info}")
            heights = info.get("heights")
            require(isinstance(heights, list), f"map.info is {info}")
            require(
                len(heights) == info.get("cells_x") * info.get("cells_z"),
                f"heights cover {len(heights)} of {info.get('cells_x')} by {info.get('cells_z')}",
            )
            require(isinstance(info.get("features"), list), f"features are {info.get('features')}")
            require(isinstance(info.get("metal"), list), f"metal is {info.get('metal')}")
            require(isinstance(info.get("name"), str), f"the map name is {info.get('name')!r}")

            call_id += 1
            radar = call_tool(
                process, stdout_lines, stderr_lines, log_dir, call_id, "map.radar"
            )
            require("_error" not in radar, f"map.radar failed: {radar}")
            require(isinstance(radar.get("kept"), bool), f"map.radar is {radar}")
            contacts = radar.get("contacts")
            require(isinstance(contacts, list), f"contacts are {contacts}")
            identity = {"id", "type", "owner"}
            position = {"x", "y", "z", "radar", "sonar"}
            for contact in contacts:
                require(isinstance(contact, dict), f"a contact is {contact}")
                require(set(contact) == position, f"a contact is {contact}")
                require(identity.isdisjoint(contact), f"a contact names itself: {contact}")
            call_id += 1
            hidden = call_tool(
                process,
                stdout_lines,
                stderr_lines,
                log_dir,
                call_id,
                "map.radar",
                {"player": "1"},
            )
            require(
                hidden.get("_error") == "another player's view needs full awareness",
                f"another player's radar was {hidden}",
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
    print("native-mcp-map: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError, automation.AutomationFailure) as error:
        print(f"native-mcp-map: {error}", file=sys.stderr)
        sys.exit(1)
