#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The game's screens, driven from MCP by each control's name.

The game runs on the dummy SDL drivers, started with --mcp. This client
speaks one request at a time. It lists the tools, then from the main menu
opens single player, the skirmish setup and comes back, opens the Library
and Settings and comes back, and opens the multiplayer providers screen
from the main menu strip and comes back. INTRO, EXIT and Start are not
clicked. Each step names a control from ui.tree. Two pictures are saved.
The preferences, the log and the pictures' scratch live in a fresh folder,
so a run writes nothing a later run would read.
"""
import argparse
import base64
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
START_TIMEOUT = 120
STEP_TIMEOUT = 45
END_TIMEOUT = 60
WAIT_MS = 20000
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
UI_TOOLS = (
    "ui.screen",
    "ui.tree",
    "ui.click",
    "ui.select",
    "ui.type",
    "ui.key",
    "ui.pointer",
    "ui.scroll",
    "ui.drag",
    "ui.wait",
    "capture.screenshot",
    "game.status",
    "game.quit",
)
STRIP = ("SINGLE", "MULTI", "INTRO", "EXIT")


class Failure(Exception):
    """The native check failed."""


def read_pipe(pipe, into):
    """Reads lines from a pipe into a queue until the pipe closes."""
    for line in pipe:
        into.put(line)
    into.put(None)


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


class _Stderr:
    """A queue-like sink that keeps stderr lines on a list."""

    def __init__(self, lines):
        self._lines = lines

    def put(self, line):
        """Keeps one line. None is the end of the pipe."""
        if line is not None:
            self._lines.append(line)


class Client:
    """One request, then its answer. A notice in between is skipped."""

    def __init__(self, process, lines, stderr_lines):
        self.process = process
        self.lines = lines
        self.stderr_lines = stderr_lines
        self.next_id = 0

    def request(self, method, params=None, timeout=STEP_TIMEOUT):
        """Sends one request and returns the result, skipping notices."""
        self.next_id += 1
        ident = self.next_id
        message = {"jsonrpc": "2.0", "id": ident, "method": method}
        if params is not None:
            message["params"] = params
        send(self.process, message)
        while True:
            answer = next_message(self.lines, timeout, self.stderr_lines)
            if answer.get("method"):
                continue
            require(answer.get("id") == ident, f"{method} answered {answer}")
            if "error" in answer:
                err = "".join(self.stderr_lines)[-2000:]
                raise Failure(f"{method} failed: {answer['error']}\n{err}")
            return answer.get("result") or {}

    def call(self, name, arguments=None, timeout=STEP_TIMEOUT):
        """Calls one tool and returns its result. A tool error fails the check."""
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        result = self.request("tools/call", params, timeout)
        if result.get("isError"):
            err = "".join(self.stderr_lines)[-2000:]
            raise Failure(f"{name} failed: {text_of(result)}\n{err}")
        return result


def text_of(result):
    """Returns the text of a tool result's first content block."""
    content = result.get("content") or [{}]
    return content[0].get("text") or ""


def body_of(result):
    """Parses a tool result's text as JSON."""
    raw = text_of(result)
    try:
        return json.loads(raw)
    except json.JSONDecodeError as error:
        raise Failure(f"a tool result was not JSON: {raw!r}") from error


def leaves(tree):
    """The controls of a tree, groups left out."""
    found = []

    def walk(node):
        if node.get("group"):
            for child in node.get("children") or []:
                walk(child)
        else:
            found.append(node)

    for root in tree.get("roots") or []:
        walk(root)
    return found


def sample_names(tree):
    """A short list of control names, for a failure."""
    names = [leaf.get("name") for leaf in leaves(tree) if leaf.get("name")]
    return ", ".join(names[:24])


def find_leaf(tree, wanted):
    """A control of that name, ignoring ASCII case. A visible one wins."""
    folded = wanted.casefold()
    hidden = None
    for leaf in leaves(tree):
        if str(leaf.get("name") or "").casefold() != folded:
            continue
        if leaf.get("visible"):
            return leaf
        if hidden is None:
            hidden = leaf
    return hidden


def screen_name(body):
    """The screen a ui.screen, ui.tree or ui.wait answer names."""
    if isinstance(body, dict) and isinstance(body.get("screen"), dict):
        return body["screen"].get("screen")
    if isinstance(body, dict):
        return body.get("screen")
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
    with tempfile.TemporaryDirectory(prefix="M04-mcp-", dir=root) as work:
        workdir = Path(work)
        preferences = workdir / "automation.PREFERENCES"
        preferences.write_text(automation.PREFERENCES, encoding="utf-8")
        log_dir = workdir / "logs"
        user_folder = workdir / "user"
        log_dir.mkdir()
        user_folder.mkdir()
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
        try:
            drive(process, stdout_lines, stderr_lines, workdir)
            code = process.wait(timeout=END_TIMEOUT)
            require(code == 0, f"the game exited {code}\n{''.join(stderr_lines)[-2000:]}")
        finally:
            if process.poll() is None:
                process.kill()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
            if process.stdin is not None:
                process.stdin.close()
    print("native-mcp-ui: ok")
    return 0


def drive(process, stdout_lines, stderr_lines, workdir):
    """Drives the screens and saves the two pictures."""
    client = Client(process, stdout_lines, stderr_lines)
    client.request(
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "native-mcp-ui", "version": "0"},
        },
        START_TIMEOUT,
    )
    send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
    listed = client.request("tools/list")
    names = {tool.get("name") for tool in listed.get("tools", [])}
    missing = [name for name in UI_TOOLS if name not in names]
    require(not missing, f"tools lack {missing}; have {sorted(names)}")

    shown = body_of(client.call("ui.screen"))
    require(screen_name(shown) == "main_menu", f"the screen is {screen_name(shown)}")
    menu = body_of(client.call("ui.tree"))
    require(screen_name(menu) == "main_menu", f"the tree's screen is {screen_name(menu)}")
    for part in STRIP:
        require(
            find_leaf(menu, part) is not None,
            f"the main menu strip has no {part}; controls: {sample_names(menu)}",
        )
    shoot(client, workdir / "M04-main.png")

    click(client, "SINGLE")
    wait_screen(client, "single_player")
    click(client, "Skirmish")
    wait_screen(client, "skirmish")
    shoot(client, workdir / "M04-skirmish.png")
    click(client, "PrevMenu")
    wait_screen(client, "single_player")
    click(client, "PrevMenu")
    wait_screen(client, "main_menu")

    click(client, "oa.button")
    wait_control(client, "oa.library.close")
    library = body_of(client.call("ui.tree"))
    settings_row = find_leaf(library, "oa.library.settings")
    require(
        settings_row is None or not settings_row.get("visible"),
        "the Library shows oa.library.settings",
    )
    tab = first_tab(library)
    if tab is not None:
        client.call("ui.click", {"name": tab})
    click(client, "oa.library.close")
    wait_until_gone(client, "oa.library.close")

    click(client, "oa.settings-button")
    wait_control(client, "oa.settings.ok")
    click(client, "oa.settings.nav.controls")
    click(client, "oa.settings.ok")
    wait_until_gone(client, "oa.settings.ok")

    click(client, "MULTI")
    wait_screen(client, "mp_providers")
    click(client, "PrevMenu")
    wait_screen(client, "main_menu")
    client.call("game.quit")


def click(client, wanted):
    """Clicks the control ui.tree names, using the spelling the tree gives."""
    tree = body_of(client.call("ui.tree"))
    leaf = find_leaf(tree, wanted)
    require(
        leaf is not None and leaf.get("visible"),
        f"no visible control {wanted}; controls: {sample_names(tree)}",
    )
    client.call("ui.click", {"name": leaf["name"]})


def wait_screen(client, name):
    """Waits until that screen is shown."""
    body = body_of(client.call("ui.wait", {"screen": name, "timeout_ms": WAIT_MS}))
    require(body.get("ok") is True, f"waiting for {name} answered {body}")
    require(screen_name(body) == name, f"the screen is {screen_name(body)}, not {name}")


def wait_control(client, name):
    """Waits until that control is visible."""
    body = body_of(client.call("ui.wait", {"control": name, "timeout_ms": WAIT_MS}))
    require(body.get("ok") is True, f"waiting for {name} answered {body}")


def wait_until_gone(client, name):
    """Reads the tree until that control is no longer listed."""
    last = {}
    for _attempt in range(30):
        last = body_of(client.call("ui.tree"))
        if find_leaf(last, name) is None:
            return
    raise Failure(f"{name} is still listed; controls: {sample_names(last)}")


def first_tab(tree):
    """The first visible Library tab, or None."""
    for leaf in leaves(tree):
        name = str(leaf.get("name") or "")
        if leaf.get("visible") and name.startswith("oa.library.tab.") and "scroll" not in name:
            return name
    return None


def shoot(client, path):
    """Saves a presented picture, narrowed, and checks that it is a PNG."""
    result = client.call(
        "capture.screenshot", {"source": "presented", "max_width": 160}
    )
    content = result.get("content") or []
    require(len(content) >= 2, f"a picture answered {content}")
    info = json.loads(content[0].get("text") or "")
    image = content[1]
    require(image.get("type") == "image", f"the picture block is {image.get('type')}")
    require(image.get("mimeType") == "image/png", f"the picture type is {image.get('mimeType')}")
    raw = base64.b64decode(image.get("data") or "")
    require(raw.startswith(PNG_SIGNATURE), "the picture is not a PNG")
    require(info.get("source") == "presented", f"the picture source is {info.get('source')}")
    width = info.get("width") or 0
    require(1 <= width <= 160, f"the picture is {width} wide")
    path.write_bytes(raw)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-mcp-ui: {error}", file=sys.stderr)
        sys.exit(1)
