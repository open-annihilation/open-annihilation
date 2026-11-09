#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The in-game menu's and the order buttons' keys.

The game runs on the dummy SDL drivers, started with --mcp and a stepped
clock, in a 640x480 window. Two launches each write their own preferences.

The first uses the default keys. On the battlefield a held Right pans the
camera. In the in-game menu Tab and Shift+Tab walk, Down moves the ring,
Enter with the ring hidden resumes, and Escape resumes. On Options, Right
steps the sound slider and the stepped value is still there when the page
opens again.

The second installs a layout that binds next to F7, previous to Shift+F7
and the panel focus to Shift+F12, and keeps pan-down on Down and on S. A
mod offers the team menu in this solo game. Tab opens and closes that menu
on the battlefield and over the in-game menu, and does not walk the menu.
Closing it resumes the match. F7 walks the menu and the order buttons. With
the ring showing, a held Down
moves the ring and leaves the camera where it is, and a held S still pans.
Enter presses and hides the ring. Escape leaves only once the ring shows.
Shift+F12 enters and leaves.

Update checks stay off, so the game does not contact a catalogue.
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

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
START_TIMEOUT = 120
STEP_TIMEOUT = 45
END_TIMEOUT = 60
WAIT_MS = 20000

LAYOUT_TEXT = """oakeys: 1
id: a18-rebound
name: "A18 rebound"
version: "1.0"
author: {name: "Open Annihilation"}
tags: [grid]
base: oa-default
bindings:
  ui.hud-focus: ["Shift+F12"]
  ui.menu-focus-next: ["F7"]
  ui.menu-focus-previous: ["Shift+F7"]
  camera.pan-down: ["Down", "S"]
"""

PROFILE_TEXT = """oamod: 1
id: a18-team-menu
name: A18 team menu
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: "Open Annihilation"}
packaging: {revision: 1, date: 2026-11-08, packager: Open Annihilation}
hacks:
  teams.alliance-menu-all-game-types: true
"""


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


def sample_names(tree):
    """A short list of control names, for a failure."""
    names = [leaf.get("name") for leaf in leaves(tree) if leaf.get("name")]
    return ", ".join(names[:24])


def tree_of(client):
    """The control tree, checked to be an object."""
    tree = body_of(client.call("ui.tree"))
    require(isinstance(tree, dict), f"the tree is {type(tree).__name__}")
    return tree


def visible(tree, name):
    """A shown control of that name, a focused one first."""
    folded = name.casefold()
    found = None
    for leaf in leaves(tree):
        if str(leaf.get("name") or "").casefold() != folded or leaf.get("hidden"):
            continue
        if leaf.get("focused"):
            return leaf
        if found is None:
            found = leaf
    return found


def focus_of(client):
    """The control ui.screen names, and whether its ring is drawn."""
    body = body_of(client.call("ui.screen"))
    focused = body.get("focused")
    if isinstance(focused, dict):
        focused = focused.get("name")
    return focused, bool(body.get("focus_shown"))


def camera_of(client):
    """The battlefield camera's map pixel, or a failure when the match has none."""
    body = body_of(client.call("ui.screen"))
    camera = body.get("camera")
    require(isinstance(camera, dict), f"the screen has no camera: {body.get('screen')}")
    return int(camera.get("x") or 0), int(camera.get("y") or 0)


def wait_screen(client, name):
    """Waits until that screen is shown."""
    body = body_of(client.call("ui.wait", {"screen": name, "timeout_ms": WAIT_MS}, START_TIMEOUT))
    require(body.get("ok") is True, f"waiting for {name} answered {body}")


def click_named(client, wanted):
    """Clicks the control by the name the menus use.

    A menu's tree can list that name as hidden while the click still reaches
    it, which is how the main menu's SINGLE starts a skirmish.
    """
    client.call("ui.click", {"name": wanted})


def tap(client, chord):
    """Presses and releases one chord."""
    client.call("ui.key", {"chord": chord, "phase": "tap"})


def settle_focus(client, predicate, label):
    """Polls the focus until predicate accepts it. Frames keep drawing."""
    last = None
    for _ in range(30):
        last = focus_of(client)
        if predicate(last):
            return last
        time.sleep(0.05)
    raise Failure(f"{label}: focus is {last[0]!r}, ring {last[1]}")


def hold(client, chord):
    """Holds a chord across ten stepped frames, then releases it."""
    client.call("ui.key", {"chord": chord, "phase": "press"})
    for _ in range(10):
        client.call("clock.step", {"ticks": 3})
        time.sleep(0.05)
    client.call("ui.key", {"chord": chord, "phase": "release"})
    client.call("clock.step", {"ticks": 1})


def center_pointer(client):
    """Parks the pointer away from the edges, so only a held key can pan."""
    client.call("ui.pointer", {"x": 320, "y": 240, "space": "window"})


def start_skirmish(client):
    """Starts the solo skirmish from the main menu and lets the clock step."""
    wait_screen(client, "main_menu")
    click_named(client, "SINGLE")
    wait_screen(client, "single_player")
    click_named(client, "Skirmish")
    wait_screen(client, "skirmish")
    click_named(client, "Start")
    body = body_of(client.call("ui.wait", {"screen": "match", "timeout_ms": 90000}, START_TIMEOUT))
    require(body.get("ok") is True, f"the skirmish did not start: {body}")
    client.call("clock.step", {"ticks": 30})
    center_pointer(client)


def open_menu(client):
    """Opens the in-game menu. The ring is hidden and a stop is focused."""
    tap(client, "F2")
    body = body_of(
        client.call("ui.wait", {"screen": "match", "control": "OK", "timeout_ms": WAIT_MS})
    )
    require(body.get("ok") is True, f"the in-game menu did not open: {body}")
    return settle_focus(client, lambda item: item[0] and not item[1], "the menu at open")


def menu_gone(client):
    """True when the in-game menu and the options page are both gone."""
    tree = tree_of(client)
    return visible(tree, "LOADGAME") is None and visible(tree, "PREV") is None


def leave_menus(client):
    """Escape leaves the options page and the in-game menu."""
    for _ in range(4):
        if menu_gone(client):
            return
        tap(client, "Escape")
        wait_screen(client, "match")
    require(menu_gone(client), "the in-game menu stayed open")


def tab_until(client, name, limit=40):
    """Tabs until that control is the shown focus, and returns it."""
    folded = name.casefold()
    for _ in range(limit):
        tree = tree_of(client)
        leaf = visible(tree, name)
        focused, _shown = focus_of(client)
        if leaf is not None and leaf.get("focused") and str(focused or "").casefold() == folded:
            return leaf
        tap(client, "Tab")
        wait_screen(client, "match")
    tree = tree_of(client)
    raise Failure(f"Tab did not reach {name}; controls: {sample_names(tree)}")


def slider_value(client, name):
    """The integer value of a shown slider."""
    leaf = visible(tree_of(client), name)
    require(leaf is not None and "value" in leaf, f"{name} has no value: {leaf}")
    return int(leaf["value"])


def check_default_keys(client):
    """Tab, the arrows, Enter, Escape, a saved slider step, and a pan."""
    start_skirmish(client)
    before = camera_of(client)
    hold(client, "Right")
    after = camera_of(client)
    require(
        abs(after[0] - before[0]) > 100,
        f"a held Right did not pan (camera {before} then {after})",
    )

    opened, _shown = open_menu(client)
    tap(client, "Tab")
    focused, shown = settle_focus(
        client, lambda item: item[0] == "LOADGAME" and item[1], "Tab in the in-game menu"
    )
    require(focused == "LOADGAME" and shown, f"Tab landed on {focused!r}, ring {shown}")
    tap(client, "Shift+Tab")
    focused, shown = settle_focus(
        client,
        lambda item: item[0] == opened and item[1],
        "Shift+Tab in the in-game menu",
    )
    require(focused == opened and shown, f"Shift+Tab landed on {focused!r}, ring {shown}")
    tap(client, "Down")
    moved, shown = settle_focus(
        client, lambda item: item[0] not in (None, opened) and item[1], "Down in the in-game menu"
    )
    require(moved != opened and shown, f"Down stayed on {moved!r}, ring {shown}")
    menu_camera = camera_of(client)
    require(
        menu_camera == after,
        f"Down in the menu moved the camera from {after} to {menu_camera}",
    )
    leave_menus(client)

    open_menu(client)
    tap(client, "Enter")
    for _ in range(20):
        if menu_gone(client):
            break
        time.sleep(0.05)
    require(menu_gone(client), "Enter with the ring hidden did not resume")

    open_menu(client)
    tap(client, "Escape")
    for _ in range(20):
        if menu_gone(client):
            break
        time.sleep(0.05)
    require(menu_gone(client), "Escape did not resume")

    open_menu(client)
    tap(client, "O")
    body = body_of(
        client.call("ui.wait", {"screen": "match", "control": "PREV", "timeout_ms": WAIT_MS})
    )
    require(body.get("ok") is True, f"the in-game Options page did not open: {body}")
    if visible(tree_of(client), "FXVOL") is None:
        click_named(client, "SOUND")
        body = body_of(
            client.call("ui.wait", {"screen": "match", "control": "FXVOL", "timeout_ms": WAIT_MS})
        )
        require(body.get("ok") is True, f"the sound page did not open: {body}")
    tab_until(client, "FXVOL")
    _focused, shown = focus_of(client)
    require(shown, "the sound slider's ring is hidden")
    started = slider_value(client, "FXVOL")
    tap(client, "Right")
    time.sleep(0.1)
    stepped = slider_value(client, "FXVOL")
    if stepped == started:
        tap(client, "Left")
        time.sleep(0.1)
        stepped = slider_value(client, "FXVOL")
    require(stepped != started, f"the sound slider stayed at {started}")
    leave_menus(client)
    open_menu(client)
    tap(client, "O")
    body = body_of(
        client.call("ui.wait", {"screen": "match", "control": "PREV", "timeout_ms": WAIT_MS})
    )
    require(body.get("ok") is True, f"Options did not open again: {body}")
    if visible(tree_of(client), "FXVOL") is None:
        click_named(client, "SOUND")
        body = body_of(
            client.call("ui.wait", {"screen": "match", "control": "FXVOL", "timeout_ms": WAIT_MS})
        )
        require(body.get("ok") is True, f"the sound page did not open again: {body}")
    kept = slider_value(client, "FXVOL")
    require(kept == stepped, f"the sound slider returned to {kept}, not {stepped}")
    leave_menus(client)
    client.call("game.quit")


def allies_open(client):
    """True when the team menu's Allies control is shown."""
    return visible(tree_of(client), "ALLIES") is not None


def wait_allies(client, want, label):
    """Waits until the team menu's Allies button is shown, or gone."""
    for _ in range(30):
        if allies_open(client) == want:
            return
        time.sleep(0.05)
    tree = tree_of(client)
    state = "open" if want else "closed"
    raise Failure(f"{label}: the team menu stayed {state}; controls: {sample_names(tree)}")


def open_and_close_team_menu(client, where):
    """Tab opens the team menu and the next Tab closes it."""
    tap(client, "Tab")
    wait_allies(client, True, f"Tab {where}")
    tap(client, "Tab")
    wait_allies(client, False, f"the second Tab {where}")


def check_rebound_keys(client):
    """A rebound layout, the team menu's Tab, and a pan that is not an arrow."""
    start_skirmish(client)
    open_and_close_team_menu(client, "on the battlefield")
    client.call("clock.step", {"ticks": 5})

    opened, shown = open_menu(client)
    require(not shown, f"the menu opened with the ring on {opened!r}")
    tap(client, "Tab")
    wait_allies(client, True, "Tab over the in-game menu")
    focused, _shown = focus_of(client)
    require(focused != "LOADGAME", f"Tab walked the in-game menu to {focused!r}")
    tap(client, "Tab")
    wait_allies(client, False, "the second Tab over the in-game menu")
    # Closing the team menu resumes the match, as it does from the battlefield.
    for _ in range(30):
        if menu_gone(client):
            break
        time.sleep(0.05)
    require(menu_gone(client), "closing the team menu left the in-game menu open")
    client.call("clock.step", {"ticks": 5})

    opened, shown = open_menu(client)
    require(not shown, f"the menu opened again with the ring on {opened!r}")
    tap(client, "F7")
    focused, shown = settle_focus(
        client, lambda item: item[0] not in (None, opened) and item[1], "F7 in the in-game menu"
    )
    require(focused != opened and shown, f"F7 landed on {focused!r}, ring {shown}")
    walked = focused
    tap(client, "Shift+F7")
    focused, shown = settle_focus(
        client, lambda item: item[0] == opened and item[1], "Shift+F7 in the in-game menu"
    )
    require(focused == opened and shown, f"Shift+F7 landed on {focused!r}, ring {shown}")
    tap(client, "Tab")
    wait_allies(client, True, "Tab after the rebound walk")
    focused, _shown = focus_of(client)
    require(focused != walked, f"Tab walked the rebound menu to {focused!r}")
    tap(client, "Tab")
    wait_allies(client, False, "the second Tab after the rebound walk")
    for _ in range(30):
        if menu_gone(client):
            break
        time.sleep(0.05)
    require(menu_gone(client), "the in-game menu stayed open after the team menu closed")
    client.call("clock.step", {"ticks": 5})
    center_pointer(client)

    tap(client, "Shift+F12")
    entered, shown = settle_focus(
        client, lambda item: bool(item[0]) and not item[1], "Shift+F12"
    )
    require(entered and not shown, f"the panel focus is {entered!r}, ring {shown}")
    tap(client, "Tab")
    time.sleep(0.15)
    if allies_open(client):
        tap(client, "Tab")
        for _ in range(30):
            if not allies_open(client):
                break
            time.sleep(0.05)
        client.call("clock.step", {"ticks": 5})
        tap(client, "Shift+F12")
        entered, shown = settle_focus(
            client, lambda item: bool(item[0]) and not item[1], "Shift+F12 after the team menu"
        )
    else:
        stayed, shown = focus_of(client)
        require(stayed == entered and not shown, f"Tab walked the panel to {stayed!r}, ring {shown}")
    tap(client, "F7")
    walked, shown = settle_focus(
        client, lambda item: item[0] not in (None, entered) and item[1], "F7 on the panel"
    )
    require(shown, f"F7 left the ring hidden on {walked!r}")
    before = camera_of(client)
    hold(client, "Down")
    landed, shown = focus_of(client)
    after = camera_of(client)
    require(landed != walked and shown, f"Down stayed on {landed!r}, ring {shown}")
    require(
        abs(after[0] - before[0]) < 2 and abs(after[1] - before[1]) < 2,
        f"a held Down panned while the ring showed ({before} to {after})",
    )
    hold(client, "S")
    panned = camera_of(client)
    require(
        abs(panned[1] - after[1]) > 100,
        f"a held S did not pan while the ring showed ({after} to {panned})",
    )
    tap(client, "Enter")
    pressed, shown = settle_focus(
        client, lambda item: bool(item[0]) and not item[1], "Enter on the panel"
    )
    require(pressed and not shown, f"Enter left {pressed!r}, ring {shown}")
    tap(client, "F7")
    _again, shown = settle_focus(client, lambda item: item[1], "F7 shows the ring again")
    require(shown, "F7 did not show the ring again")
    tap(client, "Escape")
    left, shown = settle_focus(client, lambda item: not item[0], "Escape leaves the panel")
    require(left is None and not shown, f"Escape left the focus on {left!r}, ring {shown}")
    tap(client, "Shift+F12")
    entered, shown = settle_focus(
        client, lambda item: bool(item[0]) and not item[1], "Shift+F12 again"
    )
    require(entered and not shown, f"Shift+F12 is {entered!r}, ring {shown}")
    tap(client, "Shift+F12")
    left, shown = settle_focus(client, lambda item: not item[0], "Shift+F12 leaves")
    require(left is None and not shown, f"Shift+F12 left the focus on {left!r}")
    client.call("game.quit")


def write_preferences(path):
    """A fresh preferences file with update checks off and the ring hidden at open."""
    rows = [
        ("multiplayer|tcpaddr", "192.0.2.7"),
        ("open-annihilation.update-check", "never"),
        ("open-annihilation.content-updates", "never"),
        ("open-annihilation.access.always-show-focus", "0"),
    ]
    lines = ["open-annihilation-preferences 1"]
    for key, value in rows:
        lines.append(f'"{key}" "{value}"')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def launch(native, game_dir, workdir, extra, drive):
    """Starts one game from this folder and runs drive."""
    write_preferences(workdir / "panel.PREFERENCES")
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
            str(native),
            "--game-dir",
            str(game_dir),
            "--skip-intro",
            "--mute",
            "--mcp",
            "--mcp-clock",
            "stepped",
            "--resolution",
            "640x480",
            "--preferences-file",
            str(workdir / "panel.PREFERENCES"),
            "--log-dir",
            str(log_dir),
            "--user-folder",
            str(user_folder),
            *extra,
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
        client = Client(process, stdout_lines, stderr_lines)
        client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "native-match-panel-keys", "version": "0"},
            },
            START_TIMEOUT,
        )
        send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        drive(client)
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


def main():
    """Runs the default launch and the rebound launch, each from its own folder."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    native = Path(args.native)
    game_dir = Path(args.game_dir)
    with tempfile.TemporaryDirectory(prefix="A18-panel-default-", dir=root) as work:
        launch(native, game_dir, Path(work), [], check_default_keys)
    with tempfile.TemporaryDirectory(prefix="A18-panel-rebound-", dir=root) as work:
        folder = Path(work)
        (folder / "layout").mkdir()
        (folder / "layout" / "oakeys.yaml").write_text(LAYOUT_TEXT, encoding="utf-8")
        (folder / "team.oamod.yaml").write_text(PROFILE_TEXT, encoding="utf-8")
        launch(
            native,
            game_dir,
            folder,
            ["--keys", str(folder / "layout"), "--mod", str(folder / "team.oamod.yaml")],
            check_rebound_keys,
        )
    print("native-match-panel-keys: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-match-panel-keys: {error}", file=sys.stderr)
        sys.exit(1)
