#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""camera.get, camera.set, camera.center and camera.zoom in a skirmish.

The game runs on the dummy SDL drivers with --mcp and --fark. Those
drivers draw the standard tier, which draws the world layer at the zoom,
so a zoom shows in the picture. The check reaches a skirmish through the
automation endpoint, pauses it, and centres a unit before it zooms: the
start camera is placed for the classic 512 by 416 battlefield, and the
middle of a larger window is unexplored ground, which is one colour at
every zoom. camera.get reports the place the view's limits held, which is
the place the move answered. A zoom of 2 halves the view's span about the
focus. A screenshot of the ground under the unit follows the move: it
changes by more than two frames of the same view do. game.quit then
leaves the game.
"""
import argparse
import json
import math
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
START_TIMEOUT = 120
STEP_TIMEOUT = 30
SETTLE_TIMEOUT = 30
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


def grab(client, region):
    """Returns one presented frame's message and its RGB bytes."""
    client.next_id += 1
    fields = {"id": client.next_id, "op": "frame", "format": "rgb"}
    if region is not None:
        fields["region"] = region
    client.send_raw(automation.encode(fields))
    answer = client.read_frame()
    if answer is None:
        raise Failure("the endpoint closed the connection instead of a frame")
    message, payload = answer
    if message.get("id") != client.next_id:
        raise Failure(f"the frame answered another request: {message}")
    if not message.get("ok"):
        raise Failure(f"the frame was refused: {message}")
    if not payload:
        raise Failure(f"the frame carried no pixels: {message}")
    return message, payload


def picture(client, region):
    """Returns the RGB bytes of a region, or of the whole frame when it does not fit."""
    try:
        _message, payload = grab(client, region)
        return payload
    except Failure:
        if region is None:
            raise
        _message, payload = grab(client, None)
        return payload


def frame_size(client):
    """Returns the presented frame's width and height, in pixels."""
    message, _payload = grab(client, None)
    image = message.get("image") or {}
    width = image.get("width")
    height = image.get("height")
    require(
        isinstance(width, int) and isinstance(height, int) and width > 0 and height > 0,
        f"the frame named no size: {message}",
    )
    return width, height


def half_up(value):
    """Rounds half away from zero, as lround does for the chrome."""
    if value >= 0:
        return int(math.floor(value + 0.5))
    return int(math.ceil(value - 0.5))


def battlefield_rect(width, height):
    """Returns the battlefield's left, top, width and height, as the match layout does.

    Chrome is at most twice the 640 by 480 art. A side column of the classic
    480 rows fits a window of this height at that scale, so the column is not
    narrowed.
    """
    scale = min(width / 640.0, height / 480.0, 2.0)
    left = max(1, half_up(128 * scale))
    top = max(1, half_up(32 * scale))
    bottom = max(1, half_up(32 * scale))
    if left >= width:
        left = max(1, width // 4)
    if top + bottom >= height:
        top = max(1, height // 16)
        bottom = max(1, height // 16)
    return left, top, width - left, height - top - bottom


def centre_sample(width, height):
    """Returns a sample about the battlefield's middle, and the battlefield's size.

    The sample stays inside the battlefield, clear of the minimap. It is where
    camera.center puts a unit, so the picture is that unit and its ground.
    """
    left, top, breadth, depth = battlefield_rect(width, height)
    sample_w = min(320, breadth)
    sample_h = min(240, depth)
    return (
        [left + (breadth - sample_w) // 2, top + (depth - sample_h) // 2, sample_w, sample_h],
        (breadth, depth),
    )


def centre_first_unit(mcp):
    """Centres on the first unit the match has among ids 1 to 40."""
    for unit in range(1, 41):
        answer = mcp.request(
            "tools/call", {"name": "camera.center", "arguments": {"unit": unit}}
        )
        if "error" in answer:
            continue
        result = answer.get("result") or {}
        if result.get("isError"):
            continue
        content = (result.get("content") or [{}])[0]
        return json.loads(content.get("text") or "")
    return None


def span_halved(before, after, battlefield, factor):
    """A zoom about the battlefield's middle moves the corner by the halved span.

    The span is the battlefield's pixels divided by the zoom. The focus is the
    middle of that span, and it stays put, so a factor of 2 leaves the corner
    halfway from where it was to where the focus is.
    """
    def expected(old, zoom, breadth):
        visible = breadth / zoom
        focus = old + visible / 2.0
        return focus - (breadth / (zoom * factor)) / 2.0

    for axis, breadth in (("x", battlefield[0]), ("z", battlefield[1])):
        want = expected(before[axis], before["zoom"], breadth)
        got = after[axis]
        require(
            abs(got - want) <= 3,
            f"a zoom of {factor} did not halve the span about the focus: {axis} went from "
            f"{before[axis]} to {got}, and half the span is {want:.1f}",
        )


def differing(left, right):
    """How many bytes of two pictures differ. The pictures must be the same size."""
    require(left and len(left) == len(right), f"pictures are {len(left)} and {len(right)} bytes")
    return sum(byte != other for byte, other in zip(left, right))


def picture_followed(before, after, noise, what):
    """The picture after a move differs by more than two frames of one view do."""
    change = differing(before, after)
    least = noise + max(1000, noise // 10)
    require(
        change > least,
        f"the picture did not follow {what}: {change} bytes changed, and two frames of one "
        f"view changed {noise}",
    )


def same_place(reported, seen, what):
    """camera.get reports the place a move answered."""
    require(
        reported.get("x") == seen.get("x") and reported.get("z") == seen.get("z"),
        f"{what} answered {reported} and camera.get saw {seen}",
    )
    require(isinstance(reported.get("zoom"), (int, float)), f"{what} named no zoom: {reported}")


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M10-mcp-camera-", dir=root) as work:
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
                    "clientInfo": {"name": "native-mcp-camera", "version": "0"},
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
            names = {
                tool.get("name") for tool in (listed.get("result") or {}).get("tools", [])
            }
            require(
                {"camera.get", "camera.set", "camera.center", "camera.zoom"} <= names,
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

            deadline = time.monotonic() + SETTLE_TIMEOUT
            status = {}
            while time.monotonic() < deadline:
                status = mcp.tool("game.status")
                if status.get("in_match") is True and status.get("paused") is False:
                    break
                time.sleep(0.2)
            else:
                raise Failure(f"the match did not settle: {status}")
            paused = mcp.tool("game.pause", {"paused": True})
            require(paused.get("paused") is True, f"pause answered {paused}")

            # The sample is the battlefield's middle, from the frame's own
            # size. A fixed window rectangle sits off the commander: the start
            # camera is placed for the classic 512 by 416 battlefield, and
            # unexplored ground is one colour at every zoom.
            frame_w, frame_h = frame_size(client)
            sample, span = centre_sample(frame_w, frame_h)
            centred_unit = centre_first_unit(mcp)
            require(centred_unit is not None, "no unit in the first 40 could be centred on")
            seen = mcp.tool("camera.get")
            same_place(centred_unit, seen, "camera.center unit")

            # Two frames of that ground are the noise a later picture has to
            # beat. The pause stays on.
            still = picture(client, sample)
            again = picture(client, sample)
            noise = differing(still, again)

            # Zoom while that ground is on screen, so the picture has to
            # change. A shrouded corner looks the same at another zoom.
            home = mcp.tool("camera.get")
            zoom_before = home["zoom"]
            magnified = mcp.tool("camera.zoom", {"factor": 2})
            seen = mcp.tool("camera.get")
            factor = 2
            if abs(seen["zoom"] - zoom_before) < 0.001:
                magnified = mcp.tool("camera.zoom", {"factor": 0.5})
                seen = mcp.tool("camera.get")
                factor = 0.5
            same_place(magnified, seen, "camera.zoom")
            require(
                abs(seen["zoom"] - zoom_before) > 0.001,
                f"the zoom stayed {zoom_before}",
            )
            span_halved(home, seen, span, factor)
            zoomed = picture(client, sample)
            picture_followed(again, zoomed, noise, "camera.zoom")

            corner = mcp.tool("camera.set", {"x": 0, "z": 0})
            seen = mcp.tool("camera.get")
            same_place(corner, seen, "camera.set")
            require(corner["x"] < 100000 and corner["z"] < 100000, f"corner is {corner}")
            require(
                abs(corner["zoom"] - magnified["zoom"]) < 0.001,
                f"a move changed the zoom from {magnified['zoom']} to {corner['zoom']}",
            )
            at_corner = picture(client, sample)
            picture_followed(zoomed, at_corner, noise, "camera.set")

            far = mcp.tool("camera.set", {"x": 100000, "z": 100000})
            seen = mcp.tool("camera.get")
            same_place(far, seen, "camera.set")
            require(
                far["x"] != corner["x"] or far["z"] != corner["z"],
                f"the view stayed at {corner} after a move to the far corner",
            )
            require(far["x"] < 100000 and far["z"] < 100000, f"the limits did not hold {far}")
            require(
                abs(far["zoom"] - corner["zoom"]) < 0.001,
                f"a move changed the zoom from {corner['zoom']} to {far['zoom']}",
            )
            moved = picture(client, sample)
            # The far corner and the map's corner are both shroud, so they can
            # look alike. Either one has to leave the commander's ground.
            picture_followed(zoomed, moved, noise, "camera.set")
            require(
                f"mcp: {far['applied_tick']} camera.set {far['x']} {far['z']}" in log_text(log_dir),
                f"the log has no camera.set line for tick {far.get('applied_tick')}",
            )

            centred = mcp.tool("camera.center", {"x": 0, "z": 0})
            seen = mcp.tool("camera.get")
            same_place(centred, seen, "camera.center")
            require(
                centred["x"] != far["x"] or centred["z"] != far["z"],
                f"centring on the map's corner left the view at {far}",
            )

            centred_unit = centre_first_unit(mcp)
            require(centred_unit is not None, "no unit in the first 40 could be centred on")
            seen = mcp.tool("camera.get")
            same_place(centred_unit, seen, "camera.center unit")
            # The far corner is shroud. The unit is not, so the picture has to
            # leave that corner.
            picture_followed(moved, picture(client, sample), noise, "camera.center")

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
    print("native-mcp-camera: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, automation.AutomationFailure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-mcp-camera: {error}", file=sys.stderr)
        sys.exit(1)
