#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The lobby, driven through MCP against a replay.

Four launches. The chat replay covers an ordinary account's login,
registration, the map, chat without matchmaking, recon, the host pane,
profile, and the locator and quick play, which every account has. The
side-prompt replay covers a member's login and that prompt. The contents
replay covers news, ladders and help, and a message box. The beta replay
covers a Beta Tester's account: battles to join, matchmaking, a locator
search and quick play's options. Nothing is dialled, and nothing launches a
battle. The guest password stays empty.

Each step is logged as it starts. A failure names the launch and the step,
and the last screen: its name, its dialogs and the controls on it. The
game's log and a picture of that screen are kept in the scratch folder.
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
import check_native_automation as automation  # noqa: E402

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
START_TIMEOUT = 120
STEP_TIMEOUT = 45
END_TIMEOUT = 60
WAIT_MS = 60000
CALL_TIMEOUT = 90
NOTE_KINDS = ("said", "page", "ticker", "alert", "session", "console")
# Seconds the last screen may take to read after a failure.
LAST_SCREEN_TIMEOUT = 15
# How many visible controls a failure lists.
LAST_SCREEN_CONTROLS = 80
# How many lines of the game's own log a failure prints.
LOG_TAIL_LINES = 40
STARTED = time.monotonic()
# The launch and the step the check is on.
PROGRESS = {"launch": "", "step": "starting"}


class Failure(Exception):
    """The native check failed."""


def step(text):
    """Logs the step the check starts, so a failure can name it."""
    PROGRESS["step"] = text
    elapsed = time.monotonic() - STARTED
    print(f"native-mcp-lobby: {elapsed:6.1f}s {PROGRESS['launch']}: {text}", flush=True)


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
        """Calls one tool. A tool error fails the check."""
        result = self.call_result(name, arguments, timeout)
        if result.get("isError"):
            asked = json.dumps(arguments or {}, sort_keys=True)
            raise Failure(f"{name} {asked} failed: {text_of(result)}")
        return result

    def call_result(self, name, arguments=None, timeout=STEP_TIMEOUT):
        """Calls one tool and returns the result, errors included."""
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments
        return self.request("tools/call", params, timeout)


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
    names = []
    for leaf in leaves(tree):
        name = leaf.get("name")
        if not name:
            continue
        window = leaf.get("window")
        names.append(f"{name}@{window}" if window else str(name))
    return ", ".join(names[:30])


def same_name(leaf, wanted, window):
    """Whether a control has that name, and that window when one was asked."""
    if str(leaf.get("name") or "").casefold() != wanted.casefold():
        return False
    if window is None:
        return True
    return str(leaf.get("window") or "").casefold() == window.casefold()


def find_leaf(tree, wanted, window=None):
    """A control of that name. A visible one wins."""
    hidden = None
    for leaf in leaves(tree):
        if not same_name(leaf, wanted, window):
            continue
        if leaf.get("visible"):
            return leaf
        if hidden is None:
            hidden = leaf
    return hidden


def find_text_field(tree, wanted, window=None):
    """The text field of that name, when a label shares it."""
    for leaf in leaves(tree):
        if (
            leaf.get("kind") == "text_field"
            and same_name(leaf, wanted, window)
            and leaf.get("visible")
        ):
            return leaf
    return find_leaf(tree, wanted, window)


def area_of(leaf):
    """Width times height, from the control's rectangle."""
    rect = leaf.get("rect")
    if not isinstance(rect, list) or len(rect) < 4:
        return 0
    width = rect[2] or 0
    height = rect[3] or 0
    return width * height


def type_into(client, name, text, window=None, timeout=15):
    """Clicks the text field of that name once it takes a click, and types into it.

    The field must then hold what was typed at its end; a masked field's
    length must grow by as many characters.
    """
    deadline = time.monotonic() + timeout
    while True:
        leaf = find_text_field(tree_of(client), name, window)
        if leaf is not None and leaf.get("visible") and leaf.get("enabled"):
            break
        require(time.monotonic() < deadline, f"the field {name} never took a click")
        time.sleep(0.25)
    before = int(leaf.get("length") or 0)
    click(client, leaf["name"], leaf.get("window"))
    client.call("ui.type", {"text": text}, CALL_TIMEOUT)
    while True:
        typed = find_text_field(tree_of(client), name, window) or {}
        if typed.get("masked") or "length" in typed:
            if int(typed.get("length") or 0) == before + len(text):
                return
        elif str(typed.get("text") or "").endswith(text):
            return
        require(time.monotonic() < deadline, f"{text!r} did not land in {name}: {typed}")
        time.sleep(0.25)


def click(client, name, window=None):
    """Clicks a control by the name the tree spells, and its window."""
    arguments = {"name": name}
    if window:
        arguments["window"] = window
    client.call("ui.click", arguments, CALL_TIMEOUT)


def click_named(client, wanted, window=None, timeout=15):
    """Clicks that control once it is shown and takes a click.

    A console lights its buttons a frame or more after what enables them
    (a typed name, a chosen row, the end of a slide), and a press on a
    disabled button does nothing, so the click waits for the button to light,
    as a player does.
    """
    deadline = time.monotonic() + timeout
    while True:
        tree = tree_of(client)
        leaf = find_leaf(tree, wanted, window)
        if leaf is not None and leaf.get("visible") and leaf.get("enabled"):
            click(client, leaf["name"], leaf.get("window") if window else None)
            return leaf
        if time.monotonic() >= deadline:
            state = "is not shown" if leaf is None or not leaf.get("visible") else "stayed disabled"
            raise Failure(f"{wanted} {state}")
        time.sleep(0.25)


def wait_control(client, name, window=None, timeout_ms=WAIT_MS):
    """Waits until that control is visible."""
    arguments = {"control": name, "timeout_ms": timeout_ms}
    if window:
        arguments["window"] = window
    body = body_of(client.call("ui.wait", arguments, timeout=CALL_TIMEOUT))
    require(body.get("ok") is True, f"waiting for {name} answered {body}")
    return body


def tree_of(client):
    """The current control tree."""
    return body_of(client.call("ui.tree"))


def poll_leaf(client, wanted, window=None, timeout=45):
    """Polls the tree until that control is visible."""
    deadline = time.monotonic() + timeout
    last = {}
    while True:
        last = tree_of(client)
        leaf = find_leaf(last, wanted, window)
        if leaf is not None and leaf.get("visible"):
            return last, leaf
        if time.monotonic() >= deadline:
            raise Failure(f"no visible {wanted}; controls: {sample_names(last)}")
        time.sleep(0.25)


def open_session(process, lines, stderr_lines):
    """Initialises MCP and subscribes to the lobby notes."""
    client = Client(process, lines, stderr_lines)
    step("initializing MCP")
    client.request(
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "native-mcp-lobby", "version": "0"},
        },
        START_TIMEOUT,
    )
    send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
    listed = client.request("tools/list")
    names = {tool.get("name") for tool in listed.get("tools", [])}
    require("ui.click" in names and "events.read" in names, f"tools are {sorted(names)}")
    step("subscribing to the lobby notes")
    subscribed = body_of(client.call("events.subscribe", {"events": list(NOTE_KINDS)}))
    accepted = subscribed.get("events") or []
    missing = [kind for kind in NOTE_KINDS if kind not in accepted]
    require(not missing, f"subscribe refused {missing}; accepted {accepted}")
    return client


def page_open(leaf):
    """Whether that control is on screen."""
    return leaf is not None and leaf.get("visible")


def dismiss_registration(client):
    """Opens registration and leaves it.

    The page the 1998 client shows is the web page: Url, Host and Back.
    A message box stands in when that page is missing. The in-game form,
    when it is the page on screen, is closed with Cancel.
    """
    step("opening REGISTER")
    click_named(client, "REGISTER")
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        current = tree_of(client)
        box = find_leaf(current, "Text", "BYMessageBox")
        if page_open(box):
            require(box.get("text") is not None, "the message box has no text")
            click_named(client, "Ok", "BYMessageBox")
            return
        url = find_leaf(current, "Url")
        host = find_leaf(current, "Host")
        if page_open(url) or page_open(host):
            if page_open(url):
                require(bool(url.get("text")), "the registration page has no address")
            back = find_leaf(current, "Back")
            require(page_open(back), "the registration page has no Back")
            click(client, back["name"], back.get("window"))
            return
        user = find_text_field(current, "User")
        if page_open(user):
            require("text" in user, "User has no text")
            click_named(client, "Cancel")
            return
        time.sleep(0.25)
    raise Failure("registration did not open the web page, a form, or a message box")


def guest_login(client, user_name):
    """Enters as the replay's guest and waits until the war map is up."""
    step("waiting for the login console")
    wait_control(client, "Guest Login")
    dismiss_registration(client)
    step("waiting for the login console after registration")
    wait_control(client, "Guest Login")
    step("choosing Guest Login and typing the name")
    click_named(client, "Guest Login")
    type_into(client, "Username", user_name)
    step("pressing LOGIN")
    click_named(client, "LOGIN")
    step("waiting for the war map")
    wait_for_war_map(client)


def member_login(client, user_name):
    """Logs in as a registered player so the side prompt can open.

    The password only enables LOGIN. The replay does not check it. A guest
    login never reaches the prompt.
    """
    step("waiting for the login console")
    wait_control(client, "Username")
    step("choosing Registered User")
    click_named(client, "Registered User")
    wait_checked(client, "Registered User", True)
    step("typing the name and a password")
    type_into(client, "Username", user_name)
    type_into(client, "Password", "x")
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        login = find_leaf(tree_of(client), "LOGIN")
        if login is not None and login.get("enabled"):
            step("pressing LOGIN")
            click(client, login["name"], login.get("window"))
            return
        time.sleep(0.25)
    raise Failure("LOGIN stayed disabled")


def events_of(client):
    """The notes still kept."""
    body = body_of(client.call("events.read"))
    return body.get("events") or []


def wait_note(client, kind, text_part=None, timeout=30):
    """Polls events.read until a note of that kind is kept."""
    deadline = time.monotonic() + timeout
    last = []
    while True:
        last = events_of(client)
        for note in last:
            if note.get("event") != kind:
                continue
            if text_part is None or text_part in str(note.get("text") or ""):
                return note
        if time.monotonic() >= deadline:
            shown = [note.get("event") for note in last[-12:]]
            raise Failure(f"no {kind} note; recent events {shown}")
        time.sleep(0.25)


def planet_leaves(tree):
    """Controls whose text is a planet's facts."""
    found = []
    for leaf in leaves(tree):
        if str(leaf.get("text") or "").startswith("id=") and leaf.get("visible"):
            found.append(leaf)
    return found


def login_open(tree):
    """Whether the login console still lists a control."""
    for leaf in leaves(tree):
        if str(leaf.get("window") or "") == "Login":
            return True
    return False


def dossier_loaded(client):
    """Whether a session note has reported the dossier (level 4)."""
    for note in events_of(client):
        if note.get("event") != "session":
            continue
        if str(note.get("text") or "").startswith("4 "):
            return True
    return False


def viewer_planet_count(tree, enabled_only=False):
    """How many planets have a button on the map viewer.

    With enabled_only, only planets that take a press are counted: while
    the map is locked a planet is listed but disabled.
    """
    seen = set()
    for leaf in planet_leaves(tree):
        if str(leaf.get("window") or "") != "MapPanel" or area_of(leaf) <= 0:
            continue
        if enabled_only and not leaf.get("enabled"):
            continue
        for part in str(leaf.get("text") or "").split():
            if part.startswith("id=") and part[3:].isdigit():
                seen.add(part[3:])
    return len(seen)


# The same bar as the post-login check: that many planet sprites in the viewer.
VIEWER_PLANETS = 20


def map_handed_back(tree):
    """Whether the green layer has handed the map back to the player.

    The console buttons light when the slide ends; until then they are
    disabled, and so is every planet, since the map is locked.
    """
    help_button = find_leaf(tree, "MF_Help_Button", "BYMainFrame")
    return help_button is not None and bool(help_button.get("enabled"))


def wait_for_war_map(client, timeout=90):
    """Waits until login has closed and the player can press a planet.

    The ticker can show while the login console is still up, and before the
    war map has placed a planet. After login the green layer slides off the
    map; until it has, the map is locked and a press on a planet selects
    nothing. Reading the map waits for all of it to finish.
    """
    deadline = time.monotonic() + timeout
    last = {}
    loaded = False
    count = 0
    pressable = 0
    while time.monotonic() < deadline:
        loaded = dossier_loaded(client)
        last = tree_of(client)
        count = viewer_planet_count(last)
        pressable = viewer_planet_count(last, enabled_only=True)
        if (
            not login_open(last)
            and loaded
            and count >= VIEWER_PLANETS
            and pressable == count
            and map_handed_back(last)
        ):
            return
        time.sleep(0.25)
    state = "open" if login_open(last) else "closed"
    raise Failure(
        "the war map was not handed to the player: "
        f"login {state}, dossier {loaded}, planets {count}, {pressable} of them pressable, "
        f"console buttons {'lit' if map_handed_back(last) else 'dark'}"
    )


# The console buttons a selected planet lights.
PLANET_BUTTONS = ("MF_Recon_Button", "MF_Chat_Button", "MF_Battle_Button")


def magnification_of(tree):
    """The war map's magnification, read from its label."""
    label = find_leaf(tree, "Magnification", "BYMainFrame")
    text = str((label or {}).get("text") or "")
    require(text.lstrip("-").isdigit(), f"the magnification label is {label}")
    return int(text)


def wait_magnification(client, wanted, described, timeout=10):
    """Polls the magnification until it is as wanted, and returns it."""
    deadline = time.monotonic() + timeout
    while True:
        value = magnification_of(tree_of(client))
        if wanted(value):
            return value
        require(time.monotonic() < deadline, f"the magnification stayed {value}, not {described}")
        time.sleep(0.25)


def drive_map(client):
    """Reads the map, turns magnification, and clicks a planet on the viewer."""
    step("reading the ticker")
    current, ticker = poll_leaf(client, "Ticker")
    text = str(ticker.get("text") or "")
    # The Ticker leaf carries the ticker's whole line (the main frame keeps it
    # with a four-space spacer), never the label's 99-character scrolling
    # window, which repeats a short line to fill itself.
    whole_lines = (
        "Slag is under siege: every commander to the front lines",
        "Welcome to the Boneyards ...",
    )
    require(
        any(text.strip() == line for line in whole_lines),
        f"the ticker is not one whole line ({len(text)} characters): {text!r}",
    )
    step("reading the banner, the sector and the magnification")
    banner = find_leaf(current, "Ad_Banner", "BYMainFrame")
    require(
        banner is not None and banner.get("visible"),
        f"no Ad_Banner; controls: {sample_names(current)}",
    )
    identity = str(banner.get("text") or "")
    require(
        identity.startswith("sample ") or identity.isdigit(),
        f"the banner identity is {identity!r}",
    )
    sector = find_leaf(current, "Sector", "BYMainFrame")
    require(sector is not None and str(sector.get("text") or ""), f"the Sector label is {sector}")
    lowest = magnification_of(current)
    step("raising the magnification")
    click(client, "incbutton", "Mag_Slider")
    raised = wait_magnification(client, lambda value: value > lowest, f"above {lowest}")
    off = [leaf for leaf in planet_leaves(tree_of(client)) if area_of(leaf) <= 0]
    require(off, f"at magnification {raised} every planet is still on the viewer")
    missed = off[0]
    step(f"clicking {missed['name']}, a planet off the viewer")
    result = client.call_result(
        "ui.click",
        {"name": missed["name"], "window": missed.get("window")},
        CALL_TIMEOUT,
    )
    require(result.get("isError"), f"an off-viewer planet was clicked: {text_of(result)}")
    require("has no area" in text_of(result), text_of(result))
    step("lowering the magnification")
    for _step in range(4):
        click(client, "decbutton", "Mag_Slider")
    wait_magnification(client, lambda value: value == lowest, f"back at {lowest}")
    step("clicking the advert banner")
    click(client, "Ad_Banner", "BYMainFrame")
    shown = find_leaf(tree_of(client), "Ad_Banner", "BYMainFrame")
    require(shown is not None and str(shown.get("text") or ""), f"the banner is {shown}")
    choose_planet(client, SLAG_ID)


# The replays' contested planet: its name, and its id as the viewer lists it.
SLAG = "Slag"
SLAG_ID = "1801"


def choose_planet(client, wanted):
    """Presses a planet on the viewer, by its name or its id, and waits for it to be chosen.

    The planet's button names the planet and its id; the summary then shows
    its name, and RECON, CHAT and BATTLE light.
    """
    current = tree_of(client)
    chosen = find_leaf(current, wanted, "MapPanel")
    require(
        chosen is not None and area_of(chosen) > 0 and chosen.get("enabled"),
        f"planet {wanted} is not a pressable button on the viewer: {chosen}",
    )
    facts = str(chosen.get("text") or "")
    ident = ""
    for part in facts.split():
        if part.startswith("id="):
            ident = part[3:]
    require(ident.isdigit(), f"the planet's facts are {facts!r}")
    named = [
        leaf
        for leaf in planet_leaves(current)
        if leaf.get("window") == "MapPanel"
        and str(leaf.get("text") or "") == facts
        and leaf.get("name") != ident
    ]
    require(named, f"planet {ident} has no button named for it")
    planet_name = str(named[0]["name"]).split(" #")[0]
    step(f"pressing planet {wanted}")
    click(client, chosen["name"], "MapPanel")
    step(f"waiting for {planet_name} in the summary, and RECON, CHAT and BATTLE to light")
    deadline = time.monotonic() + 15
    while True:
        current = tree_of(client)
        dark = [
            name
            for name in PLANET_BUTTONS
            if not (find_leaf(current, name, "BYMainFrame") or {}).get("enabled")
        ]
        summary = find_leaf(current, planet_name, "BYMainFrame")
        if not dark and summary is not None and summary.get("visible") and area_of(summary) > 0:
            return planet_name
        require(
            time.monotonic() < deadline,
            f"pressing planet {wanted} left {', '.join(dark) or 'no button'} disabled"
            f" and the summary {'showing' if summary else 'not showing'} {planet_name}",
        )
        time.sleep(0.25)


def list_rows(client, name, window=None):
    """The rows a list shows, as words."""
    leaf = find_leaf(tree_of(client), name, window)
    return [str(item) for item in (leaf or {}).get("items") or []]


def wait_rows(client, name, wanted, described, timeout=20):
    """Polls a list until its rows are as wanted, and returns them."""
    deadline = time.monotonic() + timeout
    while True:
        rows = list_rows(client, name)
        if wanted(rows):
            return rows
        require(time.monotonic() < deadline, f"{name} never {described}; its rows are {rows}")
        time.sleep(0.25)


def wait_checked(client, name, checked, timeout=10):
    """Polls a check until it shows that state."""
    deadline = time.monotonic() + timeout
    while True:
        leaf = find_leaf(tree_of(client), name)
        if leaf is not None and bool(leaf.get("checked")) == checked:
            return
        state = "checked" if checked else "clear"
        require(time.monotonic() < deadline, f"{name} did not turn {state}")
        time.sleep(0.25)


def wait_text(client, name, wanted, described, timeout=10):
    """Polls a control until its text is as wanted, and returns the text."""
    deadline = time.monotonic() + timeout
    while True:
        leaf = find_text_field(tree_of(client), name)
        text = str((leaf or {}).get("text") or "")
        if wanted(text):
            return text
        require(time.monotonic() < deadline, f"{name} never {described}; it holds {text!r}")
        time.sleep(0.25)


# The replay's other player in the room, his clan, and a line he says there.
OTHER_PLAYER = "Ghost_Commander"
OTHER_CLAN = "Red Dawn"
OTHER_LINE = "Ghost_Commander: Anyone up for a 1v1 on Acid Pools?"


def drive_chat(client):
    """Reads the chat log, the frequencies and the players, chats, pages and whispers."""
    step("opening the chat console")
    click_named(client, "MF_Chat_Button")
    wait_control(client, "ChatList")
    require(
        find_leaf(tree_of(client), "Minimize_Button", "ChatConsole") is None,
        "Chat offers Matchmaking to an ordinary account",
    )
    step("reading the chat log and the frequencies")
    wait_rows(client, "ChatList", lambda rows: OTHER_LINE in rows, f"held {OTHER_LINE!r}")
    rooms = wait_rows(
        client, "Chat rooms", lambda rows: "Staging Area" in rows, "listed Staging Area"
    )
    require("Main" in rooms and "Clan Hall" in rooms, f"the frequencies are {rooms}")
    step("sending a chat line")
    click_named(client, "Chat entry")
    client.call("ui.type", {"text": "hello from mcp", "submit": True}, CALL_TIMEOUT)
    wait_rows(
        client,
        "ChatList",
        lambda rows: any(row.endswith("hello from mcp") for row in rows),
        "showed the line sent",
    )
    step("reading the chat players")
    players = wait_rows(
        client, "Chat players", lambda rows: OTHER_PLAYER in rows, f"listed {OTHER_PLAYER}"
    )
    require("oa_check" in players, f"the players are {players}")
    step(f"choosing {OTHER_PLAYER} and reading the player pane")
    client.call("ui.select", {"list": "Chat players", "item": OTHER_PLAYER}, CALL_TIMEOUT)
    poll_leaf(client, "PAGE")
    poll_leaf(client, OTHER_CLAN)
    _tree, lock = poll_leaf(client, "Whisper Lock")
    step("locking whispers on him and clearing the lock")
    locked = bool(lock.get("checked"))
    click_named(client, "Whisper Lock")
    wait_checked(client, "Whisper Lock", not locked)
    click_named(client, "Whisper Lock")
    wait_checked(client, "Whisper Lock", locked)
    step("pressing PAGE")
    click_named(client, "PAGE")
    wait_text(
        client,
        "Chat entry",
        lambda text: text.startswith(f"+Page {OTHER_PLAYER}"),
        f"took +Page {OTHER_PLAYER}",
    )
    step("sending the page")
    client.call("ui.type", {"text": "meet me on Slag", "submit": True}, CALL_TIMEOUT)
    wait_rows(
        client,
        "ChatList",
        lambda rows: f"<You paged user {OTHER_PLAYER}> meet me on Slag" in rows,
        "showed the page sent",
    )
    step("whispering to him")
    click_named(client, "Chat entry")
    client.call(
        "ui.type", {"text": f"+w {OTHER_PLAYER} see you there", "submit": True}, CALL_TIMEOUT
    )
    wait_rows(
        client,
        "ChatList",
        lambda rows: f"<You whispered to {OTHER_PLAYER}> see you there" in rows,
        "showed the whisper",
    )
    matchmaking = find_leaf(tree_of(client), "Minimize_Button")
    if matchmaking is not None and matchmaking.get("visible") and matchmaking.get("enabled"):
        step("opening and closing matchmaking")
        click(client, matchmaking["name"], matchmaking.get("window"))
        poll_leaf(client, "Any planet", "Matchmaking")
        click(client, matchmaking["name"], matchmaking.get("window"))
    step("waiting for the said and page notes")
    wait_note(client, "said", "hello from mcp")
    wait_note(client, "page", f"<Page from {OTHER_PLAYER}>")


def wait_selected(client, name, row, timeout=10):
    """Polls a list until that row is its selected row."""
    deadline = time.monotonic() + timeout
    while True:
        leaf = find_leaf(tree_of(client), name) or {}
        items = [str(item) for item in leaf.get("items") or []]
        selected = leaf.get("selected")
        if isinstance(selected, int) and 0 <= selected < len(items) and items[selected] == row:
            return
        require(
            time.monotonic() < deadline,
            f"{name} did not select {row!r}; selected {selected} of {items}",
        )
        time.sleep(0.25)


def press_radio(client, name, window=None):
    """Presses a radio or a check and waits for it to show checked."""
    click_named(client, name, window)
    deadline = time.monotonic() + 10
    while True:
        leaf = find_leaf(tree_of(client), name, window)
        if leaf is not None and leaf.get("checked"):
            return
        require(time.monotonic() < deadline, f"{name} did not turn checked")
        time.sleep(0.25)


def wait_enabled(client, name, enabled, window=None, timeout=10):
    """Polls a control until it takes a click, or until it does not."""
    deadline = time.monotonic() + timeout
    while True:
        leaf = find_leaf(tree_of(client), name, window)
        if leaf is not None and leaf.get("visible") and bool(leaf.get("enabled")) == enabled:
            return
        state = "enabled" if enabled else "disabled"
        require(time.monotonic() < deadline, f"{name} did not turn {state}: {leaf}")
        time.sleep(0.25)


# The host pane's options: each group's title and its radios.
HOST_OPTIONS = {
    "Player Skill": ("Novice", "Seasoned", "Expert"),
    "Number of Players": ("2", "3", "4"),
    "On Commander Death": ("Ends", "Continues", "Deathmatch"),
}


def drive_battle(client):
    """Opens the host pane, names the battle, and sets each of its options.

    START lights once the battle has a name. It is read, not pressed.
    """
    step("opening the battle console")
    click_named(client, "MF_Battle_Button")
    wait_control(client, "BattleList")
    wait_rows(client, "BattleList", lambda rows: "Galactic War" in rows, "listed its game types")
    step("opening the host pane")
    click_named(client, "HOST")
    poll_leaf(client, "Battle name")
    current = tree_of(client)
    for title, radios in HOST_OPTIONS.items():
        for name in (title, *radios):
            require(find_leaf(current, name) is not None, f"the host pane has no {name}")
    wait_enabled(client, "START", False)
    step("naming the battle and giving it a password")
    type_into(client, "Battle name", "mcp battle")
    wait_enabled(client, "START", True)
    type_into(client, "Battle password", "pw")
    step("choosing a duel, which seats two")
    rows = list_rows(client, "Game types")
    require("Duel" in rows and "Winner Takes All" in rows, f"the game types are {rows}")
    client.call("ui.select", {"list": "Game types", "item": "Duel"}, CALL_TIMEOUT)
    wait_selected(client, "Game types", "Duel")
    wait_enabled(client, "3", False, "HOST BATTLE")
    wait_enabled(client, "4", False, "HOST BATTLE")
    step("choosing Winner Takes All and each option")
    client.call("ui.select", {"list": "Game types", "item": "Winner Takes All"}, CALL_TIMEOUT)
    wait_selected(client, "Game types", "Winner Takes All")
    for radio in ("Expert", "3", "Continues"):
        press_radio(client, radio)
    novice = find_leaf(tree_of(client), "Novice")
    require(novice is not None and not novice.get("checked"), "Novice stayed checked beside Expert")


def drive_rest(client):
    """Opens recon, battle and profile, then the locator and quick play."""
    step("opening the recon console")
    click_named(client, "MF_Recon_Button")
    wait_control(client, "ReconList")
    wait_rows(client, "ReconList", lambda rows: any(rows), "described the planet")
    poll_leaf(client, SLAG, "ReconConsole")
    drive_battle(client)
    step("opening the profile console")
    click_named(client, "MF_Profile_Button")
    wait_control(client, "ProfileList")
    categories = wait_rows(
        client, "ProfileList", lambda rows: "Local Settings" in rows, "listed Local Settings"
    )
    require("Login" in categories, f"the profile categories are {categories}")
    poll_leaf(client, "Login Name:", "ProfileConsole")
    step("choosing the Local Settings category")
    client.call("ui.select", {"list": "ProfileList", "item": "Local Settings"}, CALL_TIMEOUT)
    wait_selected(client, "ProfileList", "Local Settings")
    deadline = time.monotonic() + 10
    while find_leaf(tree_of(client), "Login Name:", "ProfileConsole") is not None:
        require(time.monotonic() < deadline, "the Login tab stayed up after Local Settings")
        time.sleep(0.25)
    step("opening the locator, which every account has")
    click_named(client, "MF_Locator_Button", "BYMainFrame")
    poll_leaf(client, "Search", "Locator")
    step("opening quick play, which every account has")
    click_named(client, "MF_Quickplay_Button", "BYMainFrame")
    poll_leaf(client, "FIND BATTLE", "QuickPlay")


def drive_chat_replay(client):
    """The chat replay: login, the map, chat, and the other consoles."""
    guest_login(client, "oa_check")
    drive_map(client)
    drive_chat(client)
    drive_rest(client)
    step("reading the lobby notes")
    require_notes(
        client,
        [
            ("session", "4 oa_check"),
            ("ticker", "Slag is under siege: every commander to the front lines"),
            ("console", "chat"),
            ("console", "recon"),
            ("console", "battle"),
            ("console", "profile"),
            ("console", "locator"),
            ("console", "quickplay"),
        ],
    )
    step("quitting")
    client.call("game.quit")


def require_notes(client, wanted):
    """Requires a kept note of each kind whose text, trimmed, is the one given."""
    notes = events_of(client)
    kept = [(note.get("event"), str(note.get("text") or "").strip()) for note in notes]
    missing = [(kind, text) for kind, text in wanted if (kind, text) not in kept]
    lobby = [pair for pair in kept if pair[0] in NOTE_KINDS]
    require(not missing, f"no note for {missing}; the lobby notes were {lobby}")


def drive_side_prompt(client):
    """The side prompt. EXIT is not pressed.

    Choose sends the side. This replay does not answer that change, so the
    prompt stays up and the main frame is not required afterwards.
    """
    member_login(client, "SideCommander")
    step("waiting for the side prompt")
    poll_leaf(client, "Arm", timeout=60)
    current = tree_of(client)
    for name in ("Core", "Choose", "EXIT"):
        require(find_leaf(current, name) is not None, f"the side prompt has no {name}")
    step("choosing Arm")
    click_named(client, "Arm")
    click_named(client, "Choose")
    step("quitting")
    client.call("game.quit")


def wait_gone(client, name, window, described, timeout=10):
    """Polls until no visible control of that name is listed in that window."""
    deadline = time.monotonic() + timeout
    while page_open(find_leaf(tree_of(client), name, window)):
        require(time.monotonic() < deadline, f"{described} stayed up")
        time.sleep(0.25)


def wait_words(client, window, words, described, timeout=20):
    """Polls until a visible label in that window shows those words."""
    deadline = time.monotonic() + timeout
    while True:
        for leaf in leaves(tree_of(client)):
            if (
                leaf.get("window") == window
                and leaf.get("kind") == "label"
                and leaf.get("visible")
                and words in str(leaf.get("text") or "")
            ):
                return leaf
        require(time.monotonic() < deadline, f"{described}: no label in {window} reads {words!r}")
        time.sleep(0.25)


# The beta replay's battles on Slag.
BETA_BATTLES = ("Rumble", "Seated", "Locked")


def drive_beta(client):
    """A Beta Tester's gated consoles: battles to join, matchmaking, the locator, quick play."""
    guest_login(client, "oa_check")
    choose_planet(client, SLAG)
    step("opening the battle console and choosing each battle")
    click_named(client, "MF_Battle_Button")
    wait_control(client, "BattleList")
    wait_rows(
        client,
        "BattleList",
        lambda rows: all(battle in rows for battle in BETA_BATTLES),
        f"listed {', '.join(BETA_BATTLES)}",
    )
    for battle in BETA_BATTLES:
        client.call("ui.select", {"list": "BattleList", "item": battle}, CALL_TIMEOUT)
        wait_selected(client, "BattleList", battle)
        poll_leaf(client, battle, "BATTLE INTELLIGENCE")
        poll_leaf(client, "Host:", "BATTLE INTELLIGENCE")
        wait_enabled(client, "JOIN", True, "BATTLE INTELLIGENCE")
    step("opening matchmaking from the chat console")
    click_named(client, "MF_Chat_Button")
    wait_control(client, "ChatList")
    click_named(client, "Minimize_Button", "ChatConsole")
    poll_leaf(client, "Activate", "Matchmaking")
    for choice in ("Duel", "Near mine", "Arm", "This planet"):
        press_radio(client, choice, "Matchmaking")
    step("closing matchmaking")
    click_named(client, "Close", "Matchmaking")
    wait_gone(client, "Activate", "Matchmaking", "the matchmaking dialog")
    step("opening the locator and finding a player")
    click_named(client, "MF_Locator_Button", "BYMainFrame")
    poll_leaf(client, "Search", "Locator")
    click_named(client, "row_0", "Locator")
    require(
        find_leaf(tree_of(client), "rname0", "Locator") is None,
        "the locator showed a result before its search",
    )
    type_into(client, "Name", OTHER_PLAYER, "Locator")
    click_named(client, "Search", "Locator")
    found = wait_words(client, "Locator", OTHER_PLAYER, "the locate did not answer")
    require(found.get("name") == "rname0", f"the first result's name is {found}")
    where = find_leaf(tree_of(client), "rwhere0", "Locator")
    require(SLAG in str((where or {}).get("text") or ""), f"the first result is at {where}")
    step("choosing the result")
    click_named(client, "rhit0", "Locator")
    wait_enabled(client, "Relocate", True, "Locator")
    step("opening quick play and choosing its options")
    click_named(client, "MF_Quickplay_Button", "BYMainFrame")
    poll_leaf(client, "FIND BATTLE", "QuickPlay")
    for choice in ("Seasoned", "Core", "Fuel Dump only"):
        press_radio(client, choice, "QuickPlay")
    step("reading the lobby notes")
    require_notes(
        client,
        [
            ("console", "battle"),
            ("console", "chat"),
            ("console", "locator"),
            ("console", "quickplay"),
        ],
    )
    step("quitting")
    client.call("game.quit")


def list_leaf(client, name):
    """The list of that name, as the tree reports it."""
    leaf = find_leaf(tree_of(client), name)
    require(leaf is not None, f"no list {name}")
    return leaf


def wait_first_row(client, name, wanted, described, timeout=10):
    """Polls a list until the first row it shows is as wanted, and returns it."""
    deadline = time.monotonic() + timeout
    while True:
        first = int(list_leaf(client, name).get("first_visible") or 0)
        if wanted(first):
            return first
        require(time.monotonic() < deadline, f"{name} did not scroll {described}; it shows {first}")
        time.sleep(0.25)


def drive_help(client):
    """Expands a collapsed help category, scrolls the list, and collapses it again."""
    step("opening the help console")
    click_named(client, "MF_Help_Button")
    wait_control(client, "ContentsList")
    rows = wait_rows(
        client,
        "ContentsList",
        lambda found: any(row.startswith("+ ") for row in found),
        "listed a collapsed category",
    )
    category = next(row for row in rows if row.startswith("+ "))
    title = category[2:]
    step(f"expanding {category}")
    client.call("ui.select", {"list": "ContentsList", "item": category}, CALL_TIMEOUT)
    opened = wait_rows(
        client, "ContentsList", lambda found: f"- {title}" in found, f"expanded {title}"
    )
    require(len(opened) > len(rows), f"expanding {title} listed no headline: {opened}")
    leaf = list_leaf(client, "ContentsList")
    shown = int(leaf.get("rows") or 0)
    require(0 < shown < len(opened), f"the expanded help list fits its {shown} rows")
    # A lobby list's incbutton is the arrow at the top of its bar, and its
    # decbutton the arrow at the bottom.
    first = int(leaf.get("first_visible") or 0)
    step("scrolling the help list down")
    click(client, "decbutton", "ContentsList")
    scrolled = wait_first_row(client, "ContentsList", lambda row: row > first, f"below row {first}")
    step("scrolling the help list back up")
    click(client, "incbutton", "ContentsList")
    wait_first_row(client, "ContentsList", lambda row: row < scrolled, f"above row {scrolled}")
    step(f"collapsing {title}")
    client.call("ui.select", {"list": "ContentsList", "item": f"- {title}"}, CALL_TIMEOUT)
    closed = wait_rows(
        client, "ContentsList", lambda found: category in found, f"collapsed {title}"
    )
    require(len(closed) == len(rows), f"collapsing {title} left {closed}")


def drive_contents(client):
    """News, a ladders search that opens a message box, and the help tree."""
    guest_login(client, "oa_check")
    step("opening the news console")
    click_named(client, "MF_News_Button")
    wait_control(client, "ContentsList")
    wait_rows(client, "ContentsList", lambda rows: any(rows), "listed the news")
    step("opening the ladders console and searching")
    click_named(client, "MF_Ladders_Button")
    poll_leaf(client, "Ladder search")
    type_into(client, "Ladder search", "zzznomatch-mcp")
    step("pressing SEARCH and answering the message box")
    click_named(client, "SEARCH")
    _tree, box = poll_leaf(client, "Text", "BYMessageBox")
    require(str(box.get("text") or ""), "the message box text is empty")
    click_named(client, "Ok", "BYMessageBox")
    deadline = time.monotonic() + 10
    while page_open(find_leaf(tree_of(client), "Text", "BYMessageBox")):
        require(time.monotonic() < deadline, "Ok did not close the message box")
        time.sleep(0.25)
    drive_help(client)
    step("reading the lobby notes")
    require_notes(
        client,
        [("console", "news"), ("console", "ladders"), ("console", "help")],
    )
    alerts = [note for note in events_of(client) if note.get("event") == "alert"]
    require(alerts and str(alerts[-1].get("text") or ""), "the message box posted no alert")
    step("quitting")
    client.call("game.quit")


def describe_control(leaf):
    """A control as a failure lists it: name, window, kind, text, rows."""
    name = str(leaf.get("name") or "?")
    window = leaf.get("window")
    shown = f"{name}@{window}" if window else name
    kind = leaf.get("kind")
    if kind:
        shown += f" [{kind}]"
    if not leaf.get("enabled", True):
        shown += " (disabled)"
    text = str(leaf.get("text") or "")
    if text:
        shown += f" {text[:40]!r}"
    items = leaf.get("items") or []
    if items:
        shown += f" rows {len(items)}"
    return shown


def last_screen(client, shot_path):
    """The screen at a failure: its name, its dialogs and its controls.

    The picture of it is written to shot_path when the game can take one.
    """
    lines = []
    try:
        screen = body_of(client.call("ui.screen", timeout=LAST_SCREEN_TIMEOUT))
        brief = {key: screen.get(key) for key in ("screen", "dialogs", "focus") if key in screen}
        lines.append(f"last screen: {json.dumps(brief)}")
    except (Failure, OSError, ValueError) as error:
        lines.append(f"last screen: not read ({error})")
        return lines
    try:
        tree = body_of(client.call("ui.tree", timeout=LAST_SCREEN_TIMEOUT))
        visible = [leaf for leaf in leaves(tree) if leaf.get("visible")]
        windows = sorted({str(leaf.get("window")) for leaf in visible if leaf.get("window")})
        lines.append(f"windows: {', '.join(windows)}")
        listed = [describe_control(leaf) for leaf in visible[:LAST_SCREEN_CONTROLS]]
        more = len(visible) - len(listed)
        lines.append(
            f"visible controls ({len(visible)}): {'; '.join(listed)}"
            + (f"; and {more} more" if more > 0 else "")
        )
    except (Failure, OSError, ValueError) as error:
        lines.append(f"controls: not read ({error})")
    try:
        result = client.call("capture.screenshot", {}, timeout=LAST_SCREEN_TIMEOUT)
        for block in result.get("content") or []:
            if block.get("type") == "image" and block.get("data"):
                shot_path.write_bytes(base64.b64decode(block["data"]))
                lines.append(f"picture: {shot_path}")
                break
    except (Failure, OSError, ValueError) as error:
        lines.append(f"picture: not taken ({error})")
    return lines


def keep_log(log_dir, kept):
    """Copies the game's log beside the check's outputs and returns its tail."""
    tail = []
    for log in sorted(Path(log_dir).glob("*")):
        if not log.is_file():
            continue
        text = log.read_text(encoding="utf-8", errors="replace")
        kept.mkdir(parents=True, exist_ok=True)
        (kept / log.name).write_text(text, encoding="utf-8")
        tail.append(f"--- {log.name}, the last {LOG_TAIL_LINES} lines (whole log in {kept}) ---")
        tail.extend(text.splitlines()[-LOG_TAIL_LINES:])
    return tail


def run_game(native, game_dir, mod_dir, replay, scratch, drive):
    """Starts one game on the dummy drivers and drives it."""
    root = Path(scratch)
    root.mkdir(parents=True, exist_ok=True)
    PROGRESS["launch"] = drive.__name__
    step(f"starting the game on {Path(replay).name}")
    with tempfile.TemporaryDirectory(prefix="mcp-lobby-", dir=root) as work:
        workdir = Path(work)
        preferences = workdir / "automation.PREFERENCES"
        preferences.write_text(automation.PREFERENCES, encoding="utf-8")
        log_dir = workdir / "logs"
        user_folder = workdir / "user"
        log_dir.mkdir()
        user_folder.mkdir()
        environment = dict(os.environ)
        environment.pop("OA_BY_USER", None)
        environment.pop("OA_BY_PASSWORD", None)
        environment["SDL_VIDEO_DRIVER"] = "dummy"
        environment["SDL_AUDIO_DRIVER"] = "dummy"
        environment["SDL_RENDER_DRIVER"] = "software"
        process = subprocess.Popen(
            [
                *RUNNER,
                str(native),
                "--game-dir",
                str(game_dir),
                "--mod-dir",
                str(mod_dir),
                "--mcp",
                "--skip-intro",
                "--mute",
                "-y",
                "--boneyards-replay",
                str(replay),
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
            client = open_session(process, stdout_lines, stderr_lines)
            drive(client)
            step("waiting for the game to end")
            code = process.wait(timeout=END_TIMEOUT)
            require(code == 0, f"the game exited {code}")
        except (Failure, subprocess.TimeoutExpired, OSError) as error:
            report = [f"{PROGRESS['launch']}, step '{PROGRESS['step']}': {error}"]
            outputs = root / f"mcp-lobby-{PROGRESS['launch']}"
            outputs.mkdir(parents=True, exist_ok=True)
            if client is not None and process.poll() is None:
                report.extend(last_screen(client, outputs / "last-screen.png"))
            elif process.poll() is not None:
                report.append(f"the game had ended with status {process.returncode}")
            err = "".join(stderr_lines)[-2000:].strip()
            if err:
                report.append("--- the game's standard error ---")
                report.append(err)
            report.extend(keep_log(log_dir, outputs))
            raise Failure("\n".join(report)) from error
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
    """Runs the four launches."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--chat-replay", required=True)
    parser.add_argument("--side-replay", required=True)
    parser.add_argument("--contents-replay", required=True)
    parser.add_argument("--beta-replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.chat_replay,
        args.scratch_root,
        drive_chat_replay,
    )
    run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.side_replay,
        args.scratch_root,
        drive_side_prompt,
    )
    run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.contents_replay,
        args.scratch_root,
        drive_contents,
    )
    run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.beta_replay,
        args.scratch_root,
        drive_beta,
    )
    PROGRESS["launch"] = "all"
    step("ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-mcp-lobby: {error}", file=sys.stderr)
        sys.exit(1)
