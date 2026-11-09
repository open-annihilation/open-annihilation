#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The strip's keyboard and the main menu's order, with the Boneyards
# installed. The game runs on the dummy SDL drivers, started with --mcp.
#
# Strip mode and button mode each walk the order. Tab reaches it before the
# strip's windows. Return on Enter opens the services question, and after
# that question closes the next Tab moves on, to Credits. While the ring
# shows, Left from Intro rings Single player and Return opens it. In button
# mode that Credits stop is the menu's own Credits, where it is drawn.
#
# Return and Space on the strip's Credits start the credits film, and so
# does Tab, Down, Down, Return. After the first Tab, B opens the services
# question, and a pointer move then Return, Return opens Single player.
# In button mode a click under the plate starts nothing. A player who never
# presses Tab never meets a Credits the menu does not draw: four fresh
# games walk those keys and the tree never names that button.
#
# A film is seen in the game's log, then the game is stopped. It is not
# played through.
#
# A second pair of launches plays a layout that moves Accept to F5 and
# Press to F6. Those keys open More maps… and the update pill. Return, no
# longer bound to either, opens neither. Each launch has its own folders.

import argparse
import http.server
import json
import os
from pathlib import Path
import queue
import shlex
import shutil
import subprocess
import sys
import threading
import time

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
START_TIMEOUT = 120
STEP_TIMEOUT = 45
END_TIMEOUT = 60
PILL_TIMEOUT = 120

PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.language" "en"\n'
    '"open-annihilation.first-run" "done"\n'
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"\n'
    '"open-annihilation.access.always-show-focus" "0"\n'
)

PILL_PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.language" "en"\n'
    '"open-annihilation.first-run" "done"\n'
    '"open-annihilation.update-check" "automatically"\n'
    '"open-annihilation.update-download" "1"\n'
    '"open-annihilation.content-updates" "never"\n'
    '"open-annihilation.access.always-show-focus" "0"\n'
)

# Accept and Press move. Tab, the arrows and the list keys move with them,
# so a default Return is not also Accept. The folder is a --keys path.
KEYS = """oakeys: 1
id: a20-menu-keys
name: "A20 menu keys"
version: "1.0"
author: {name: The Open Annihilation Authors}
description: "Menu accept and press moved off Return and Space."
tags: [window]
base: oa-default
bindings:
  ui.menu-focus-next: ["F7"]
  ui.menu-focus-previous: ["Shift+F7"]
  ui.menu-focus-left: ["F3"]
  ui.menu-focus-right: ["F4"]
  ui.menu-list-up: ["F8"]
  ui.menu-list-down: ["Shift+F8"]
  ui.menu-accept: ["F5"]
  ui.menu-press: ["F6"]
"""

# A foreign mod named with --mod-dir is never switched off, so Enter shows
# that notice and the services question never opens. Button mode therefore
# keeps this build's own profile and only names the mode.
BUTTON_MODE = "media:\n  boneyards-entry: {mode: button}\n"

SERVICES = "oa.expansion-services"
BROWSER = "oa.map-browser"
CENTRE = "oa.update-centre"
PILL = "oa.update-pill.open"
READY = "oa.ready-update"
FILM = "intro backends"
# A click under the plate, on the spot that used to reach a shifted Credits.
UNDER_PLATE = (564, 94)
# The menu's own Credits. A click here plays the film on either build.
MENU_CREDITS = (320, 466)
# With no Tab, the menu's own focus, or the plate's Enter. Credits is not
# one of them: the base names Single player where a shifted Credits was named.
NOTAB_FOCUS = {None, "SINGLE", "MULTI", "INTRO", "EXIT", "EnterBYBtn"}


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


class Session:
    """One game, spoken to through MCP, stopped in close."""

    def __init__(self, game, game_dir, mod_dir, work, preferences, keys=None, extra=()):
        work.mkdir(parents=True)
        (work / "user").mkdir()
        (work / "data").mkdir()
        self.logs = work / "logs"
        self.logs.mkdir()
        prefs = work / "preferences.conf"
        prefs.write_text(preferences, encoding="utf-8")
        command = [
            *RUNNER,
            str(game),
            "--mcp",
            "--skip-intro",
            "--mute",
            "--resolution",
            "640x480",
            "--game-dir",
            str(game_dir),
            "--mod-dir",
            str(mod_dir),
            "--boneyards-server",
            "127.0.0.1",
            "--preferences-file",
            str(prefs),
            "--user-folder",
            str(work / "user"),
            "--data-dir",
            str(work / "data"),
            "--log-dir",
            str(work / "logs"),
            *extra,
        ]
        if keys is not None:
            command.extend(["--keys", str(keys)])
        environment = dict(
            os.environ,
            SDL_VIDEO_DRIVER="dummy",
            SDL_AUDIO_DRIVER="dummy",
            SDL_RENDER_DRIVER="software",
        )
        self.process = subprocess.Popen(
            command,
            cwd=work,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
        )
        self.stderr_lines = []
        stdout_lines = queue.Queue()
        threading.Thread(
            target=read_pipe, args=(self.process.stdout, stdout_lines), daemon=True
        ).start()
        threading.Thread(
            target=read_pipe, args=(self.process.stderr, _Stderr(self.stderr_lines)), daemon=True
        ).start()
        self.client = Client(self.process, stdout_lines, self.stderr_lines)
        self.client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "native-boneyards-menu-keys", "version": "0"},
            },
            START_TIMEOUT,
        )
        send(self.process, {"jsonrpc": "2.0", "method": "notifications/initialized"})

    def close(self):
        """Quits the game. A game that does not answer is killed."""
        code = self.process.poll()
        if code is None:
            try:
                self.client.call("game.quit", timeout=END_TIMEOUT)
                code = self.process.wait(timeout=END_TIMEOUT)
            except (Failure, subprocess.TimeoutExpired, OSError):
                self.process.kill()
                try:
                    code = self.process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    code = self.process.poll()
        if self.process.stdin is not None:
            self.process.stdin.close()
        return code

    def tail(self):
        """The last lines of the game's standard error."""
        return "".join(self.stderr_lines)[-2000:]


class _Stderr:
    """A queue-like sink that keeps stderr lines on a list."""

    def __init__(self, lines):
        self._lines = lines

    def put(self, line):
        """Keeps one line. None is the end of the pipe."""
        if line is not None:
            self._lines.append(line)


def shown(session):
    """The ui.screen answer."""
    return body_of(session.client.call("ui.screen"))


def where(body):
    """A short description of a screen answer, for a failure."""
    return (
        f"screen {body.get('screen')!r} focus {body.get('focused')!r} "
        f"ring {body.get('focus_shown')!r} dialogs {body.get('dialogs')!r}"
    )


def wait_screen(session, name):
    """Waits until that screen is shown and returns its ui.screen answer."""
    try:
        body = body_of(
            session.client.call(
                "ui.wait", {"screen": name, "timeout_ms": 20000}, START_TIMEOUT
            )
        )
    except Failure as error:
        try:
            landed = shown(session)
        except Failure:
            landed = {}
        raise Failure(f"waiting for {name}: {error}; {where(landed)}") from error
    require(body.get("ok") is True, f"waiting for {name} answered {body}\n{session.tail()}")
    landed = shown(session)
    require(
        landed.get("screen") == name,
        f"the screen is {landed.get('screen')!r}, not {name}\n{session.tail()}",
    )
    return landed


def tap(session, action=None, chord=None):
    """Presses one action or one chord and lets it go."""
    if action is not None:
        session.client.call("ui.key", {"action": action, "phase": "tap"})
    else:
        session.client.call("ui.key", {"chord": chord, "phase": "tap"})


def expect_focus(session, name, ring, screen="main_menu"):
    """The named control has the keyboard, and the ring is or is not drawn."""
    body = shown(session)
    require(
        body.get("screen") == screen
        and body.get("focused") == name
        and bool(body.get("focus_shown")) is ring,
        f"wanted {name!r} on {screen} ring {ring}; {where(body)}",
    )
    return body


def tab_until(session, name, screen, limit=40, chord=None):
    """Tabs until that control has the ring on that screen.

    chord is the moved next key. With none, the default next action is used,
    which is Tab. An action id ignores a layout and presses the default key.
    """
    folded = name.casefold()
    for _ in range(limit):
        body = shown(session)
        require(
            body.get("screen") == screen,
            f"left {screen} while looking for {name}; {where(body)}",
        )
        focused = body.get("focused")
        if (
            isinstance(focused, str)
            and focused.casefold() == folded
            and body.get("focus_shown")
        ):
            return body
        if chord is None:
            tap(session, action="ui.menu-focus-next")
        else:
            tap(session, chord=chord)
    body = shown(session)
    raise Failure(f"Tab did not reach {name} on {screen}; {where(body)}")


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


def tree_of(session):
    """The control tree."""
    tree = body_of(session.client.call("ui.tree"))
    require(isinstance(tree, dict), f"the tree is {type(tree).__name__}")
    return tree


def click_named(session, names):
    """Clicks the first visible control of those names."""
    listed = leaves(tree_of(session))
    wanted = [name.casefold() for name in names]
    for control in listed:
        found = str(control.get("name") or "")
        if found.casefold() not in wanted or not control.get("visible", True):
            continue
        session.client.call("ui.click", {"name": found})
        return found
    sample = [str(control.get("name")) for control in listed if control.get("visible", True)][:24]
    raise Failure(f"no visible control among {names}; controls: {', '.join(sample)}")


def dialogs_of(body):
    """The dialog names a screen answer lists."""
    names = []
    for dialog in body.get("dialogs") or []:
        if isinstance(dialog, dict):
            names.append(dialog.get("name"))
        else:
            names.append(dialog)
    return names


def until_dialog(session, name, present, timeout=20):
    """Waits until a dialog is present or gone, and returns the screen."""
    deadline = time.monotonic() + timeout
    body = {}
    while time.monotonic() < deadline:
        body = shown(session)
        there = name in dialogs_of(body)
        if there is present:
            return body
        time.sleep(0.2)
    raise Failure(
        f"{name} {'did not open' if present else 'stayed open'}; {where(body)}\n{session.tail()}"
    )


def control_focused(session, name):
    """Whether any listed control of that name is focused."""
    folded = name.casefold()
    for control in leaves(tree_of(session)):
        if str(control.get("name") or "").casefold() == folded and control.get("focused"):
            return True
    return False


def film_count(session):
    """How many times the credits film has announced itself."""
    count = "".join(session.stderr_lines).count(FILM)
    for path in session.logs.glob("*.log"):
        try:
            count += path.read_text(encoding="utf-8", errors="replace").count(FILM)
        except OSError:
            continue
    return count


def send_tool(session, name, arguments):
    """Sends one tool call and returns its id. The answer is not waited for."""
    client = session.client
    client.next_id += 1
    ident = client.next_id
    send(
        session.process,
        {
            "jsonrpc": "2.0",
            "id": ident,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
    )
    return ident


def poll_tool(session, ident, seconds, want_film):
    """Waits for that tool's answer, or for the credits film to start.

    A film blocks the call, so a launch that wants the film does not wait
    for the answer. A launch that wants none fails as soon as the log has it.
    """
    before = film_count(session)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if film_count(session) > before:
            if want_film:
                return "film"
            raise Failure(f"a film started\n{session.tail()}")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            answer = next_message(
                session.client.lines, min(0.4, remaining), session.client.stderr_lines
            )
        except Failure:
            continue
        if answer.get("method") or answer.get("id") != ident:
            continue
        if "error" in answer:
            raise Failure(f"the tool failed: {answer['error']}\n{session.tail()}")
        result = answer.get("result") or {}
        if result.get("isError"):
            raise Failure(f"the tool failed: {text_of(result)}\n{session.tail()}")
        if not want_film:
            return result
    if want_film:
        raise Failure(f"no film started\n{session.tail()}")
    raise Failure(f"the tool did not answer\n{session.tail()}")


def stop_hard(session):
    """Stops the game at once. A film would not answer game.quit."""
    if session.process.poll() is None:
        session.process.kill()
        try:
            session.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            session.process.kill()
    if session.process.stdin is not None:
        try:
            session.process.stdin.close()
        except OSError:
            pass


def expect_film(session, chord):
    """Presses one chord, sees the credits film start, and stops the game."""
    ident = send_tool(session, "ui.key", {"chord": chord, "phase": "tap"})
    try:
        poll_tool(session, ident, 20, True)
    finally:
        stop_hard(session)


def click_at(session, x, y):
    """Clicks one canvas point and returns. A film that starts fails the check."""
    ident = send_tool(
        session,
        "ui.drag",
        {"x0": x, "y0": y, "x1": x, "y1": y, "button": "left"},
    )
    return poll_tool(session, ident, 8, False)


def pointer_at(session, x, y):
    """Moves the pointer to one canvas point."""
    session.client.call("ui.pointer", {"x": x, "y": y})


def tree_names(session, name):
    """Whether the tree lists a control of that name."""
    folded = name.casefold()
    return any(
        str(control.get("name") or "").casefold() == folded for control in leaves(tree_of(session))
    )


def focused_control(session, name):
    """The focused control of that name, or None."""
    folded = name.casefold()
    for control in leaves(tree_of(session)):
        if str(control.get("name") or "").casefold() == folded and control.get("focused"):
            return control
    return None


def holds(rect, x, y):
    """Whether a [x, y, width, height] rectangle contains a point."""
    if not isinstance(rect, list) or len(rect) != 4:
        return False
    left, top, width, height = rect
    return left <= x < left + width and top <= y < top + height


def open_services(session, enter_name, after):
    """Return on Enter opens the services question. The next Tab moves on."""
    tab_until(session, enter_name, "main_menu")
    tap(session, chord="Return")
    until_dialog(session, SERVICES, True)
    tap(session, chord="Escape")
    body = until_dialog(session, SERVICES, False)
    require(
        body.get("screen") == "main_menu",
        f"the services question left {where(body)}",
    )
    expect_focus(session, enter_name, False)
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, after, True)


def walk_strip(session):
    """Strip mode: Enter, then Credits, then Left from Intro presses Single."""
    wait_screen(session, "main_menu")
    body = shown(session)
    require(
        body.get("focused") is None and not body.get("focus_shown"),
        f"the main menu at open names a ring; {where(body)}",
    )
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, "MultiBtn", True)
    open_services(session, "EnterBYBtn", "CreditsBtn")
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, "IntroBtn", True)
    tap(session, action="ui.menu-focus-left")
    expect_focus(session, "SingleBtn", True)
    tap(session, chord="Return")
    wait_screen(session, "single_player")


def walk_button(session):
    """Button mode: a click under the plate starts nothing, then the order."""
    wait_screen(session, "main_menu")
    before = shown(session)
    require(not tree_names(session, "CreditsBtn"), "button mode lists CreditsBtn before a click")
    click_at(session, *UNDER_PLATE)
    time.sleep(1.0)
    body = shown(session)
    require(
        film_count(session) == 0
        and body.get("screen") == "main_menu"
        and body.get("focused") == before.get("focused")
        and not tree_names(session, "CreditsBtn"),
        f"a click under the plate changed the menu; {where(body)}",
    )
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, "MULTI", True)
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, "INTRO", True)
    tap(session, action="ui.menu-focus-left")
    expect_focus(session, "SINGLE", True)
    tap(session, chord="Return")
    wait_screen(session, "single_player")
    tap(session, chord="Escape")
    wait_screen(session, "main_menu")
    open_services(session, "EnterBYBtn", "Credits")


def walk_strip_credits(session, chord):
    """Tab to the strip's Credits and press it. The film starts."""
    wait_screen(session, "main_menu")
    tab_until(session, "CreditsBtn", "main_menu")
    expect_film(session, chord)


def walk_strip_arrows_credits(session):
    """Tab, Down, Down lands on the strip's Credits. Return starts the film."""
    wait_screen(session, "main_menu")
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, "MultiBtn", True)
    # Down has no default chord, so the key itself is sent. The order owns it
    # while the ring shows.
    tap(session, chord="Down")
    tap(session, chord="Down")
    expect_focus(session, "CreditsBtn", True)
    expect_film(session, "Return")


def walk_tab_then_b(session):
    """After the first Tab, B opens the services question, as it does at open."""
    wait_screen(session, "main_menu")
    tap(session, action="ui.menu-focus-next")
    body = shown(session)
    require(body.get("focus_shown"), f"the first Tab drew no ring; {where(body)}")
    tap(session, chord="B")
    until_dialog(session, SERVICES, True)


def walk_tab_pointer_return(session):
    """After Tab, a pointer move, Return gives the panel focus and opens nothing.

    The next Return opens Single player.
    """
    wait_screen(session, "main_menu")
    tap(session, action="ui.menu-focus-next")
    expect_focus(session, "MultiBtn", True)
    # The first move only records the pointer. The next one hides the ring.
    pointer_at(session, 100, 100)
    pointer_at(session, 320, 300)
    body = shown(session)
    require(
        body.get("screen") == "main_menu" and not body.get("focus_shown"),
        f"the pointer move left the ring; {where(body)}",
    )
    tap(session, chord="Return")
    body = shown(session)
    require(
        body.get("screen") == "main_menu" and SERVICES not in dialogs_of(body),
        f"the first Return left the menu; {where(body)}",
    )
    tap(session, chord="Return")
    wait_screen(session, "single_player")


def walk_button_credits_film(session):
    """Tab to the menu's own Credits. Its ring is there, and Return starts the film."""
    wait_screen(session, "main_menu")
    require(not tree_names(session, "CreditsBtn"), "button mode lists CreditsBtn")
    tab_until(session, "Credits", "main_menu")
    control = focused_control(session, "Credits")
    require(control is not None, "the tree does not focus the menu's Credits")
    rect = control.get("rect")
    require(
        isinstance(rect, list) and len(rect) == 4 and rect[1] >= 380 and holds(rect, *MENU_CREDITS),
        f"Credits is not the menu's own control: {rect!r}",
    )
    require(not tree_names(session, "CreditsBtn"), "the tree names a Credits drawn nowhere")
    expect_film(session, "Return")


def assert_notab(session, body):
    """One step of a walk that never presses Tab, in button mode."""
    require(film_count(session) == 0, f"a film started; {where(body)}")
    require(not tree_names(session, "CreditsBtn"), f"the tree names CreditsBtn; {where(body)}")
    require(body.get("focused") != "CreditsBtn", f"the screen names CreditsBtn; {where(body)}")
    if body.get("screen") != "main_menu" or SERVICES in dialogs_of(body):
        return
    require(
        body.get("focused") in NOTAB_FOCUS and not body.get("focus_shown"),
        f"a no-Tab step left the base's menu; {where(body)}",
    )


def walk_notab(session, chords):
    """Presses chords from a fresh main menu and checks each step."""
    wait_screen(session, "main_menu")
    assert_notab(session, shown(session))
    for chord in chords:
        tap(session, chord=chord)
        assert_notab(session, shown(session))


def walk_notab_arrows(session):
    """Arrows, Home, End and the page keys never name a Credits drawn nowhere."""
    walk_notab(
        session,
        ["Escape", "Down", "Right", "Up", "Left", "Home", "End", "PageDown", "PageUp", "Up",
         "Down", "Left"],
    )
    require(shown(session).get("screen") == "main_menu", "the arrows left the main menu")


def walk_notab_return(session):
    """Return, the arrows, Return, Escape. The strip keeps the keys it always took.

    Nothing here names a Credits the menu does not draw, and no film starts.
    """
    walk_notab(session, ["Return", "Down", "Right", "Left", "Return", "Escape"])


def walk_notab_space(session):
    """Space, Space, Return, Escape. None of them starts a film or names Credits."""
    walk_notab(session, ["Space", "Space", "Return", "Escape"])


def walk_notab_letters(session):
    """S, M and B, each closed. B opens the services question."""
    wait_screen(session, "main_menu")
    assert_notab(session, shown(session))
    tap(session, chord="S")
    wait_screen(session, "single_player")
    assert_notab(session, shown(session))
    tap(session, chord="Escape")
    wait_screen(session, "main_menu")
    assert_notab(session, shown(session))
    tap(session, chord="M")
    deadline = time.monotonic() + 20
    body = shown(session)
    while time.monotonic() < deadline:
        body = shown(session)
        if body.get("screen") != "main_menu" or dialogs_of(body):
            break
        time.sleep(0.2)
    else:
        raise Failure(f"M left the main menu unchanged; {where(body)}")
    assert_notab(session, body)
    tap(session, chord="Escape")
    wait_screen(session, "main_menu")
    assert_notab(session, shown(session))
    tap(session, chord="B")
    until_dialog(session, SERVICES, True)
    assert_notab(session, shown(session))
    tap(session, chord="Escape")
    body = until_dialog(session, SERVICES, False)
    require(body.get("screen") == "main_menu", f"B's question left {where(body)}")
    assert_notab(session, body)


def walk_more_maps(session):
    """A moved Accept and Press open the browser. An unbound Return does not."""
    wait_screen(session, "main_menu")
    click_named(session, ["SingleBtn", "SINGLE"])
    wait_screen(session, "single_player")
    click_named(session, ["Skirmish"])
    wait_screen(session, "skirmish")
    click_named(session, ["SelectMap"])
    wait_screen(session, "map_selection")
    # The layout moved next, Accept and Press. An action id would press the
    # default key, which this layout no longer binds to those actions.
    tab_until(session, "oa.more-maps", "map_selection", chord="F7")
    require(
        not control_focused(session, "MAPNAMES"),
        "the map list stays focused beside More maps…",
    )
    tap(session, chord="F5")
    until_dialog(session, BROWSER, True)
    tap(session, chord="Escape")
    until_dialog(session, BROWSER, False)
    require(shown(session).get("screen") == "map_selection", "Escape left map selection")
    tap(session, chord="Enter")
    body = shown(session)
    require(
        BROWSER not in dialogs_of(body) and body.get("screen") == "map_selection",
        f"an unbound Return opened the browser; {where(body)}",
    )
    tab_until(session, "oa.more-maps", "map_selection", chord="F7")
    tap(session, chord="F6")
    until_dialog(session, BROWSER, True)
    tap(session, chord="Escape")
    until_dialog(session, BROWSER, False)


def publish_feed(tool, root):
    """Signs a one-byte 0.9.9 feed and returns its address, key and server."""
    serve_root = root / "serve"
    files = serve_root / "files"
    files.mkdir(parents=True)
    key = root / "feed.key"
    passphrase = root / "passphrase"
    passphrase.write_text("a20 menu keys loopback feed\n", encoding="utf-8")
    run_tool(
        tool,
        ["catalogue", "keygen", "--id", "a20-menu-keys", "--out", str(key),
         "--passphrase-file", str(passphrase)],
    )
    package = files / "open-annihilation-0.9.9-macos-universal.zip"
    package.write_bytes(b"x")

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            return

    handler = lambda *args, **kwargs: Quiet(*args, directory=str(serve_root), **kwargs)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    feed = serve_root / "feed.json"
    run_tool(
        tool,
        ["engine-feed", "write", "--version=0.9.9", "--date", "2026-11-06", "--from", "0.0.1",
         "--page", base + "/page", "--mirror", base + "/files/", "--out", str(feed), str(package)],
    )
    run_tool(
        tool,
        ["engine-feed", "sign", "--key", str(key), "--passphrase-file", str(passphrase), str(feed)],
    )
    made = run_tool(tool, ["catalogue", "public", str(key)])
    public = next(
        line.split(":", 1)[1].strip() for line in made.splitlines() if line.startswith("public:")
    )
    return base + "/feed.json", "a20-menu-keys=" + public, server


def run_tool(tool, args):
    """Runs oa-tool and returns its output."""
    done = subprocess.run([str(tool), *args], capture_output=True, text=True)
    if done.returncode != 0:
        raise Failure(f"oa-tool {' '.join(args[:2])} failed: {done.stdout} {done.stderr}")
    return done.stdout


def settle_pill(session):
    """Waits for the pill, keeping a finished download ready instead of installing."""
    deadline = time.monotonic() + PILL_TIMEOUT
    body = {}
    while time.monotonic() < deadline:
        body = shown(session)
        names = dialogs_of(body)
        if READY in names:
            click_named(session, ["oa.ready-update.keep-ready"])
            time.sleep(0.4)
            continue
        listed = leaves(tree_of(session))
        if any(control.get("name") == PILL for control in listed):
            return
        time.sleep(0.4)
    raise Failure(f"the update pill did not show; {where(body)}\n{session.tail()}")


def walk_pill(session):
    """A moved Accept and Press open the update centre. An unbound Return does not."""
    wait_screen(session, "main_menu")
    settle_pill(session)
    tab_until(session, PILL, "main_menu", chord="F7")
    tap(session, chord="F5")
    until_dialog(session, CENTRE, True)
    tap(session, chord="Escape")
    until_dialog(session, CENTRE, False)
    tap(session, chord="Enter")
    body = shown(session)
    require(
        CENTRE not in dialogs_of(body) and body.get("screen") == "main_menu",
        f"an unbound Return opened the update centre; {where(body)}",
    )
    tab_until(session, PILL, "main_menu", chord="F7")
    tap(session, chord="F6")
    until_dialog(session, CENTRE, True)
    tap(session, chord="Escape")
    until_dialog(session, CENTRE, False)


def write_button_mod(folder, coreprime):
    """This build's profile, with the menu entry asked for as a button."""
    folder.mkdir(parents=True)
    profile = (coreprime / "oamod.yaml").read_text(encoding="utf-8").rstrip() + "\n"
    if "boneyards-entry:" not in profile:
        profile += BUTTON_MODE
    (folder / "oamod.yaml").write_text(profile, encoding="utf-8")
    for archive in sorted(coreprime.glob("*.ccx")):
        shutil.copyfile(archive, folder / archive.name)


def play(name, arguments, work, mod, preferences, walk, keys=None, extra=()):
    """Starts one game, runs one walk and stops it."""
    session = Session(
        arguments.game,
        arguments.game_dir,
        mod,
        work / name,
        preferences,
        keys,
        extra,
    )
    try:
        walk(session)
        code = session.close()
        require(code == 0, f"{name} exited {code}\n{session.tail()}")
    except Exception:
        session.close()
        raise


def play_film(name, arguments, work, mod, walk):
    """Starts one game, runs a walk that stops it when the film starts."""
    session = Session(arguments.game, arguments.game_dir, mod, work / name, PREFERENCES)
    try:
        walk(session)
    except Exception:
        stop_hard(session)
        raise
    stop_hard(session)


def main():
    """Runs each walk from its own folder."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--coreprime", required=True)
    parser.add_argument("--work", required=True)
    arguments = parser.parse_args()
    work = Path(arguments.work)
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    keys = work / "keys"
    keys.mkdir()
    (keys / "oakeys.yaml").write_text(KEYS, encoding="utf-8")
    coreprime = Path(arguments.coreprime)
    button_mod = work / "button-mod"
    write_button_mod(button_mod, coreprime)
    play("strip", arguments, work, coreprime, PREFERENCES, walk_strip)
    play("button", arguments, work, button_mod, PREFERENCES, walk_button)
    play_film(
        "strip-credits-return", arguments, work, coreprime,
        lambda session: walk_strip_credits(session, "Return"),
    )
    play_film(
        "strip-credits-space", arguments, work, coreprime,
        lambda session: walk_strip_credits(session, "Space"),
    )
    play_film("strip-arrows-credits", arguments, work, coreprime, walk_strip_arrows_credits)
    play("strip-tab-b", arguments, work, coreprime, PREFERENCES, walk_tab_then_b)
    play("button-tab-b", arguments, work, button_mod, PREFERENCES, walk_tab_then_b)
    play("strip-pointer-return", arguments, work, coreprime, PREFERENCES, walk_tab_pointer_return)
    play_film("button-credits", arguments, work, button_mod, walk_button_credits_film)
    play("notab-arrows", arguments, work, button_mod, PREFERENCES, walk_notab_arrows)
    play("notab-return", arguments, work, button_mod, PREFERENCES, walk_notab_return)
    play("notab-space", arguments, work, button_mod, PREFERENCES, walk_notab_space)
    play("notab-letters", arguments, work, button_mod, PREFERENCES, walk_notab_letters)
    play("maps", arguments, work, coreprime, PREFERENCES, walk_more_maps, keys)
    feed_root = work / "feed"
    feed, key, server = publish_feed(arguments.tool, feed_root)
    (work / "pill-install").mkdir()
    try:
        play(
            "pill",
            arguments,
            work,
            coreprime,
            PILL_PREFERENCES,
            walk_pill,
            keys,
            ("--update-feed", feed, "--update-key", key, "--update-install-folder",
             str(work / "pill-install")),
        )
    finally:
        server.shutdown()
    print(
        "boneyards menu keys: the strip and the plate agree with the order, "
        "and a moved Accept opens More maps and the update pill"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-boneyards-menu-keys: {error}", file=sys.stderr)
        sys.exit(1)
