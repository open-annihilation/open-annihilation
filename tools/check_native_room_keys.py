#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Keys in a hosted room, from a host and a joiner.

Both games run on the dummy SDL drivers, started with --mcp, in a 640x480
window. The host opens a TCP/IP room and the joiner enters it. By ui.key
alone the joiner picks a side and a colour, then gets ready, and the host
gets ready and starts. Ready is last because it locks the side and the
colour. Another launch, the host alone, sets the player's own repeat delay
and rate, and a held Right steps the Metal slider at that rate. Each launch
writes its preferences, log folder and user folder at the start, so a
second run of this script finds nothing the first one left.
Update checks stay off, so the game does not contact a catalogue.
"""

import argparse
import json
import os
from pathlib import Path
import queue
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time
import native_offline

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
ADDRESS = "127.0.0.1"
START_TIMEOUT = 180
STEP_TIMEOUT = 45
END_TIMEOUT = 60
ROOM_WAIT_MS = 150000
START_WAIT = 150
# The held-key launch: the player's own repeat, 300 ms before the first
# repeat and then ten a second, and how long Right is held, in seconds.
REPEAT_DELAY_MS = 300
REPEAT_RATE = 10
HOLD = 1.0


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
    """The controls of a tree, groups left out, in the order the tree lists them."""
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


def screen_name(body):
    """The screen a ui.screen, ui.tree or ui.wait answer names."""
    if isinstance(body, dict) and isinstance(body.get("screen"), dict):
        return body["screen"].get("screen")
    if isinstance(body, dict):
        return body.get("screen")
    return None


def quoted(text):
    """One preferences value, with quotes and backslashes escaped."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_preferences(path, address="192.0.2.7", extra=()):
    """Writes a fresh preferences file. Always show focus stays off."""
    rows = [
        ("multiplayer|tcpaddr", address),
        ("open-annihilation.update-check", "never"),
        ("open-annihilation.content-updates", "never"),
        ("open-annihilation.access.always-show-focus", "0"),
        *extra,
    ]
    lines = ["open-annihilation-preferences 1"]
    lines.extend(quoted(key) + " " + quoted(value) for key, value in rows)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


def tree_of(client):
    """The control tree."""
    tree = body_of(client.call("ui.tree"))
    require(isinstance(tree, dict), f"the tree is {type(tree).__name__}")
    return tree


def focused_name(client):
    """The control ui.screen names as focused."""
    body = body_of(client.call("ui.screen"))
    return body.get("focused")


def leaf_named(tree, name):
    """The leaf of that name, or None."""
    for leaf in leaves(tree):
        if leaf.get("name") == name:
            return leaf
    return None


def sample_names(tree):
    """A short list of control names, for a failure."""
    names = [str(leaf.get("name")) for leaf in leaves(tree) if leaf.get("name")]
    return ", ".join(names[:24])


def shown_value(leaf):
    """The value a control shows, or its text when it has no value phrase."""
    text = leaf.get("value_text")
    if isinstance(text, str) and text:
        return text
    return str(leaf.get("text") or "")


def press(client, chord):
    """Taps one chord."""
    client.call("ui.key", {"chord": chord, "phase": "tap"})


def wait_value(client, name, before, timeout):
    """Polls until a control shows a new value and still has the focus.

    A joiner's side and colour are applied when the host answers, which is
    a later frame than the key.
    """
    deadline = time.monotonic() + timeout
    last = before
    while time.monotonic() < deadline:
        focus = focused_name(client)
        require(focus == name, f"{name} lost the focus to {focus!r}")
        leaf = leaf_named(tree_of(client), name) or {}
        last = shown_value(leaf)
        if last != before:
            return last
        time.sleep(0.05)
    raise Failure(f"{name} stayed {before!r}")


def wait_checked(client, name, timeout):
    """Polls until a control is checked."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = leaf_named(tree_of(client), name) or {}
        if last.get("checked") is True:
            return
        time.sleep(0.05)
    raise Failure(f"{name} is not checked: {last}")


def tab_to(client, wanted):
    """Tabs until that control is focused, stepping past an overlay's control."""
    for _ in range(64):
        name = focused_name(client)
        if name == wanted:
            return
        press(client, "Tab")
    raise Failure(f"Tab never reached {wanted}; focus is {focused_name(client)!r}")


def wait_screen(client, name, timeout_ms=ROOM_WAIT_MS):
    """Waits until that screen is shown."""
    body = body_of(
        client.call(
            "ui.wait",
            {"screen": name, "timeout_ms": timeout_ms},
            timeout=timeout_ms / 1000 + 30,
        )
    )
    require(body.get("ok") is True, f"waiting for {name} answered {body}")
    require(screen_name(body) == name, f"the screen is {screen_name(body)}, not {name}")


def wait_enabled(client, name, timeout):
    """Polls until a control is enabled. Returns its leaf."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        tree = tree_of(client)
        leaf = leaf_named(tree, name)
        last = leaf
        if leaf is not None and leaf.get("enabled"):
            return leaf
        time.sleep(0.05)
    shown = None
    if last is not None:
        shown = {key: last.get(key) for key in ("name", "enabled", "visible", "text")}
    visible = sample_names(tree_of(client))
    raise Failure(f"{name} did not enable; last {shown}; controls: {visible}")


def own_ready(tree):
    """The player's own Ready, the one enabled control of that name."""
    found = None
    for leaf in leaves(tree):
        name = str(leaf.get("name") or "")
        ready = (
            len(name) == 6
            and name.startswith("READY")
            and name[-1].isdigit()
            and leaf.get("enabled")
        )
        if ready:
            if found is not None:
                raise Failure(
                    f"two Ready controls are enabled: {found.get('name')} and {name}"
                )
            found = leaf
    return found


def wait_own_ready(client, timeout):
    """Polls until the player's own Ready is enabled."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = own_ready(tree_of(client))
        if found is not None:
            return found
        time.sleep(0.05)
    tree = tree_of(client)
    raise Failure(f"no Ready was enabled; controls: {sample_names(tree)}")


def launch(native, game_dir, workdir, label, arguments, address="192.0.2.7", extra=()):
    """Starts one game from a fresh preferences file and returns its client.

    `extra` holds more preference rows, as (key, value) pairs.
    """
    work = Path(workdir).resolve()
    preferences = (work / f"{label}.PREFERENCES").resolve()
    log_dir = (work / f"{label}-logs").resolve()
    user_folder = (work / f"{label}-user").resolve()
    write_preferences(preferences, address, extra)
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
            str(native),
            "--game-dir",
            str(Path(game_dir).resolve()),
            "--skip-intro",
            "--mute",
            "--mcp",
            "--resolution",
            "640x480",
            "--preferences-file",
            str(preferences),
            "--log-dir",
            str(log_dir),
            "--user-folder",
            str(user_folder),
            *arguments,
        ],
        cwd=work,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
    )
    stdout_lines = queue.Queue()
    stderr_lines = []
    threading.Thread(target=read_pipe, args=(process.stdout, stdout_lines), daemon=True).start()
    threading.Thread(
        target=read_pipe, args=(process.stderr, _Stderr(stderr_lines)), daemon=True
    ).start()
    client = Client(process, stdout_lines, stderr_lines)
    try:
        client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "native-room-keys", "version": "0"},
            },
            START_TIMEOUT,
        )
        send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
    except Exception:
        if process.poll() is None:
            process.kill()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        if process.stdin is not None:
            process.stdin.close()
        raise
    return process, client


def stop(process):
    """Ends a game that is still running."""
    if process.poll() is not None:
        return
    process.kill()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    if process.stdin is not None:
        process.stdin.close()


def quit_game(process, client):
    """Asks the game to leave, then ends it if it does not."""
    try:
        client.call("game.quit", timeout=END_TIMEOUT)
    except (Failure, OSError):
        stop(process)
        return
    try:
        code = process.wait(timeout=END_TIMEOUT)
    except subprocess.TimeoutExpired:
        stop(process)
        raise Failure("the game did not leave") from None
    require(code == 0, f"the game exited {code}")


# Next button moves to F7. Tab is left unbound to that command.
NEXT_ON_F7 = """oakeys: 1
id: a19-next-f7
name: "A19 next on F7"
version: "1.0"
author: {name: The Open Annihilation Authors}
description: "Next button on F7."
tags: [window]
base: oa-default
bindings:
  ui.menu-focus-next: ["F7"]
"""


def click_named(client, wanted):
    """Clicks the control of that name."""
    tree = tree_of(client)
    leaf = leaf_named(tree, wanted)
    require(leaf is not None, f"no control {wanted}; controls: {sample_names(tree)}")
    client.call("ui.click", {"name": leaf["name"]})


def expect_f7_walks(client, where, focused):
    """Tab stays where it is, and F7, bound to Next button, moves the focus."""
    require(focused_name(client) == focused, f"{where} focuses {focused_name(client)!r}")
    press(client, "Tab")
    require(
        focused_name(client) == focused,
        f"Tab walked {where} from {focused} to {focused_name(client)!r}",
    )
    press(client, "F7")
    require(focused_name(client) != focused, f"F7 stayed on {focused} in {where}")


def expect_f7_reaches_mutators(client):
    """F7 walks on from Select Map to the host's Mutators, and Tab does not.

    Mutators is the layer's button, which takes its step in the room's order
    from Next button as the room does. Previous button is still Shift+Tab.
    """
    for _ in range(30):
        if focused_name(client) == "MAP":
            break
        press(client, "F7")
    require(
        focused_name(client) == "MAP",
        f"F7 never reached Select Map; at {focused_name(client)!r}",
    )
    press(client, "Tab")
    require(
        focused_name(client) == "MAP",
        f"Tab walked the battle room from Select Map to {focused_name(client)!r}",
    )
    press(client, "F7")
    require(
        focused_name(client) == "oa.mutators",
        f"F7 went from Select Map to {focused_name(client)!r}, not Mutators",
    )
    press(client, "Tab")
    require(
        focused_name(client) == "oa.mutators",
        f"Tab walked the battle room from Mutators to {focused_name(client)!r}",
    )
    press(client, "F7")
    require(
        focused_name(client) == "METAL",
        f"F7 went from Mutators to {focused_name(client)!r}, not Metal",
    )
    press(client, "Shift+Tab")
    require(
        focused_name(client) == "oa.mutators",
        f"Previous button went from Metal to {focused_name(client)!r}, not Mutators",
    )


def rebound(native, game_dir, workdir):
    """With Next button on F7, F7 walks the battle room, Mutators included, and the game list."""
    keys = Path(workdir) / "keys"
    keys.mkdir()
    (keys / "oakeys.yaml").write_text(NEXT_ON_F7, encoding="utf-8")
    host_args = [
        "--dplay-port",
        str(free_port()),
        "--host",
        "--player-name",
        "Hoster",
        "--game-name",
        "Open Room",
        "--keys",
        str(keys),
    ]
    host_process, host = launch(native, game_dir, workdir, "rebound-host", host_args)
    try:
        wait_screen(host, "mp_battleroom")
        expect_f7_walks(host, "the battle room", "MESSAGE")
        expect_f7_reaches_mutators(host)
        quit_game(host_process, host)
        host_process = None
    finally:
        if host_process is not None:
            stop(host_process)

    browse_args = [
        "--dplay-port",
        str(free_port()),
        "--keys",
        str(keys),
    ]
    browse_process, browse = launch(
        native, game_dir, workdir, "rebound-list", browse_args, address="127.0.0.1"
    )
    try:
        wait_screen(browse, "main_menu")
        click_named(browse, "MULTI")
        wait_screen(browse, "mp_providers")
        click_named(browse, "SELECT")
        wait_screen(browse, "mp_tcp")
        click_named(browse, "OK")
        wait_screen(browse, "mp_game_list")
        expect_f7_walks(browse, "the game list", "GAMENAME")
        quit_game(browse_process, browse)
        browse_process = None
    finally:
        if browse_process is not None:
            stop(browse_process)


def steps_between(least_ms, most_ms):
    """The fewest and the most steps a press held that long makes.

    The press steps once, and each repeat that falls due while the key is
    down steps once more. A frame may pass between a repeat falling due and
    the release, so a frame's worth is allowed at each end.
    """
    interval = 1000 // REPEAT_RATE

    def steps(held_ms):
        if held_ms < REPEAT_DELAY_MS:
            return 1
        return 2 + int((held_ms - REPEAT_DELAY_MS) // interval)

    return steps(max(0, least_ms - interval)), steps(most_ms) + 1


def metal_value(client):
    """The Metal slider's value, as a number."""
    leaf = leaf_named(tree_of(client), "METAL") or {}
    text = shown_value(leaf)
    require(text.isdigit(), f"Metal shows {text!r}")
    return int(text)


def held(native, game_dir, workdir):
    """With the player's own repeat, a held Right steps Metal at that rate."""
    host_args = [
        "--dplay-port",
        str(free_port()),
        "--host",
        "--player-name",
        "Hoster",
        "--game-name",
        "Open Room",
    ]
    repeat = (
        ("open-annihilation.access.repeat-delay-ms", str(REPEAT_DELAY_MS)),
        ("open-annihilation.access.repeat-rate", str(REPEAT_RATE)),
    )
    host_process, host = launch(native, game_dir, workdir, "held-host", host_args, extra=repeat)
    try:
        wait_screen(host, "mp_battleroom")
        tab_to(host, "METAL")
        press(host, "Home")
        deadline = time.monotonic() + STEP_TIMEOUT
        while metal_value(host) != 0:
            require(time.monotonic() < deadline, "Home did not put Metal at 0")
            time.sleep(0.05)
        began = time.monotonic()
        host.call("ui.key", {"chord": "Right", "phase": "press"})
        time.sleep(HOLD)
        host.call("ui.key", {"chord": "Right", "phase": "release"})
        most_ms = (time.monotonic() - began) * 1000
        time.sleep(0.5)
        require(focused_name(host) == "METAL", f"Right left Metal for {focused_name(host)!r}")
        stepped = metal_value(host) // 100
        least, most = steps_between(HOLD * 1000, most_ms)
        require(
            least <= stepped <= most,
            f"Right held {HOLD} s, released after at most {most_ms:.0f} ms, stepped Metal "
            f"{stepped} times, not {least} to {most}, at a {REPEAT_DELAY_MS} ms delay and "
            f"{REPEAT_RATE} a second",
        )
        quit_game(host_process, host)
        host_process = None
    finally:
        if host_process is not None:
            stop(host_process)


def play(native, game_dir, workdir):
    """Hosts a room, joins it, and plays the keys that ready the room and start it."""
    port = free_port()
    host_args = [
        "--dplay-port",
        str(port),
        "--host",
        "--player-name",
        "Hoster",
        "--game-name",
        "Open Room",
    ]
    join_args = [
        "--dplay-port",
        str(port),
        "--join",
        ADDRESS,
        "--player-name",
        "Visitor",
    ]
    host_process, host = launch(native, game_dir, workdir, "host", host_args)
    joiner_process = None
    joiner = None
    try:
        wait_screen(host, "mp_battleroom")
        joiner_process, joiner = launch(native, game_dir, workdir, "joiner", join_args)
        wait_screen(joiner, "mp_battleroom")
        ready = wait_own_ready(joiner, 60)
        slot = str(ready.get("name"))[-1]
        side_name = "SIDE" + slot
        logo_name = "LOGO" + slot
        side = wait_enabled(joiner, side_name, 60)
        side_before = shown_value(side)
        tab_to(joiner, side_name)
        press(joiner, "Return")
        wait_value(joiner, side_name, side_before, 15)
        logo = wait_enabled(joiner, logo_name, 60)
        logo_before = shown_value(logo)
        tab_to(joiner, logo_name)
        press(joiner, "Right")
        wait_value(joiner, logo_name, logo_before, 15)
        ready_name = str(ready.get("name"))
        tab_to(joiner, ready_name)
        press(joiner, "Return")
        wait_checked(joiner, ready_name, 15)

        host_ready = wait_own_ready(host, 60)
        host_ready_name = str(host_ready.get("name"))
        tab_to(host, host_ready_name)
        press(host, "Return")
        wait_checked(host, host_ready_name, 15)
        wait_enabled(host, "START", START_WAIT)
        tab_to(host, "START")
        press(host, "Return")
        deadline = time.monotonic() + START_WAIT
        screen = None
        while time.monotonic() < deadline:
            screen = screen_name(body_of(host.call("ui.screen")))
            if screen in ("loading", "match"):
                break
            time.sleep(0.05)
        require(screen in ("loading", "match"), f"the host's screen is {screen}")
        quit_game(joiner_process, joiner)
        joiner_process = None
        quit_game(host_process, host)
        host_process = None
    finally:
        if joiner_process is not None:
            stop(joiner_process)
        if host_process is not None:
            stop(host_process)


def main():
    """Runs the one room. A second ctest run starts from its own folder."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="A19-room-keys-", dir=root) as work:
        play(Path(args.native), Path(args.game_dir), Path(work))
        rebound(Path(args.native), Path(args.game_dir), Path(work))
        held(Path(args.native), Path(args.game_dir), Path(work))
    print("native-room-keys: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-room-keys: {error}", file=sys.stderr)
        sys.exit(1)
