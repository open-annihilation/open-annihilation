#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Tab through every base menu, and fail when the keyboard walk breaks.

The game runs on the dummy SDL drivers, started with --mcp, in a 640x480
window. Each launch writes a new preferences file, user folder and data
folder, with update checks off and Always show focus off, so one run cannot
leave an answer the next would read, and no catalogue or download another
run kept in the shared data folder reaches a screen.

At each screen, before any key, ui.screen must say the ring is hidden.
Tab then walks until the focused name returns to the first stop Tab
reached. The recorded order is a rotation of the screen's order: a screen
that opens on its first stop records the cycle from the second stop and
meets the first again at the end. A stop with no label fails. The label
is the tree leaf's label, the words a screen reader reads, and whitespace
does not count. A press that leaves nothing focused fails. While a modal
dialog covers the screen, the focused control's dialog must be that dialog.
A button drawn over the screen is listed as a dialog and does not cover it.

The orders are the ones the menu tests document. A kit name may carry the
oa. prefix the endpoint adds; the comparison ignores that one prefix.
Return on PREFS, in the in-game menu, walks the sound page. The O key
walks the Options hub.
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
START_TIMEOUT = 120
STEP_TIMEOUT = 45
END_TIMEOUT = 60
WAIT_MS = 20000
ADDRESS = "127.0.0.1"
WALK_LIMIT = 80

# Any Mission shows only once every mission is unlocked, which a new
# preferences file never says, so it is not a stop here.
SINGLE = ["NewCamp", "Skirmish", "Options", "LoadGame", "PrevMenu"]
CAMPAIGN = ["Arm", "Core", "Campaign", "Difficulty", "mutators", "Start", "PrevMenu"]
LOAD = ["GAMES", "GAMENAME", "LOAD", "DELETE", "CANCEL"]
MAP_SELECT = ["MAPNAMES", "more-maps", "LOAD", "PREVMENU"]
HUB = ["SOUND", "MUSIC", "SPEEDS", "VISUALS", "PREV", "CANCEL", "RESTORE", "UNDO"]
SECTIONS = ["SOUND", "MUSIC", "SPEEDS", "VISUALS"]
FOOTER = ["PREV", "CANCEL", "RESTORE", "UNDO"]
SOUND = SECTIONS + ["MODE", "FXVOL", "SPEECH", "TEST"] + FOOTER
VISUALS = SECTIONS + ["GAMMA", "VIDSLDR", "SHADING", "ANTI", "BSHADOWS"] + FOOTER
SPEEDS = SECTIONS + ["GAME", "SCREEN", "TXTSCROL", "MAXLINES", "UNITCHAT", "LEFTCLICK"] + FOOTER
MUSIC = SECTIONS + [
    "NOTRAK", "MUSICVOL", "TRACKMODE", "CDPREV", "CDSTOP", "CDPLAY", "CDNEXT", "TRACKTYPE",
] + FOOTER
# The game folder decides the main menu, not the walk: an install with the
# Boneyards shows the strip, and a 3.1c install without them the four plain
# buttons. Each order is pinned whole.
MENU_PLAIN = ["SINGLE", "MULTI", "INTRO", "EXIT", "button", "settings-button"]
MENU_STRIP = [
    "SingleBtn", "MultiBtn", "EnterBYBtn", "CreditsBtn", "IntroBtn", "ExitBtn",
    "button", "settings-button",
]
RULES = ["CommanderDeath", "StartLocation", "Mapping", "LineOfSight", "Difficulty"]
SKIRMISH_ROW = ["Player", "Side", "Color", "Allies", "Metal", "Energy"]
MUTATORS = [
    "mutators.filter", "mutators.search", "mutators.list", "mutators.cancel", "mutators.use",
    "mutators.close",
]
# Controls, with no mod played. Edge stickiness is the next row in the
# section table and is not a stop in this round: Window edge is followed
# by Focus lost.
CONTROLS = [
    "settings.nav.controls",
    "settings.key-layout.change",
    "settings.key-layout.edit",
    "settings.show-keys-on-buttons",
    "settings.wheel-zoom",
    "settings.max-zoom-out",
    "settings.max-zoom-in",
    "settings.view-past-map-edge",
    "settings.middle-click-pan",
    "settings.invert-3d-camera-pan",
    "settings.invert-3d-camera-rotation",
    "settings.invert-mouse-zoom",
    "settings.double-click-selects",
    "settings.window-edge",
    "settings.focus-lost",
    "settings.group-keys-centre",
    "settings.restore-defaults",
    "settings.cancel",
    "settings.ok",
    "settings.close-x",
]
KEY_EDITOR = [
    "key-editor.view.list",
    "key-editor.search",
    "key-editor.find",
    "key-editor.category",
    "key-editor.list",
    "key-editor.undo",
    "key-editor.more",
    "key-editor.cancel",
    "key-editor.save",
    "key-editor.close",
]
LIBRARY = [
    "library.tab.active",
    "library.active.change",
    "library.active.expansion.campaign",
    "library.close",
    "library.refresh",
    "library.close-x",
]
WELCOME = [
    "first-run.language.skip",
    "first-run.language.back",
    "first-run.language.next",
    "first-run.language.previous",
    "first-run.language.card.en",
    "first-run.language.next-card",
]
OVERLAY = ["keys-overlay.edit", "keys-overlay.close", "keys-overlay.list"]
MATCH_HUB = ["CANCEL", "SOUND", "MUSIC", "SPEEDS", "VISUALS", "PREV"]
MATCH_SOUND = [
    "CANCEL", "UNDO", "MODE", "FXVOL", "SPEECH", "TEST", "RESTORE",
    "SOUND", "MUSIC", "SPEEDS", "VISUALS", "PREV",
]
MAP_BROWSER = [
    "maps-filter-all", "maps-players", "maps-size", "maps-search", "maps-card-acid-foursome",
    "maps-cancel", "maps-close",
]
# The host's row, the column, Select Map, Mutators, the sliders, then chat.
# START is absent while nobody is ready.
ROOM = [
    "prevmenu", "ready0", "side0", "logo0", "teamicons0", "commander", "fixedloc", "mapping",
    "lostype", "cheating", "watching", "gameopen", "restrictions", "map", "mutators", "metal",
    "energy", "maxunits", "output", "message",
]


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


def norm(name):
    """A control name without one leading oa. prefix, for comparison."""
    text = str(name or "")
    if text.casefold().startswith("oa."):
        text = text[3:]
    return text.casefold()


def cyclic_equal(got, expected):
    """Whether got is expected rotated, ignoring one oa. prefix."""
    got_n = [norm(name) for name in got]
    exp_n = [norm(name) for name in expected]
    if len(got_n) != len(exp_n) or not got_n:
        return False
    for shift in range(len(exp_n)):
        if exp_n[shift:] + exp_n[:shift] == got_n:
            return True
    return False


def shown(order):
    """The walked names, for a failure."""
    return ", ".join(str(name) for name in order)


def expect_one(title, order, options):
    """Requires the walk to be one of those orders, rotated."""
    if any(cyclic_equal(order, expected) for expected in options):
        return
    raise Failure(f"ORDER {title}: {shown(order)}")


def skirmish_order():
    """The rules, two full player rows, the other eight players, then the footer."""
    order = list(RULES)
    for slot in range(2):
        order.extend(f"{part}{slot}" for part in SKIRMISH_ROW)
    order.extend(f"Player{slot}" for slot in range(2, 10))
    order.extend(["SelectMap", "mutators", "Start", "PrevMenu"])
    return order


def without(order, name):
    """The round with that one stop taken out."""
    wanted = norm(name)
    return [item for item in order if norm(item) != wanted]


def expect_after(title, order, name, previous):
    """That stop sits immediately after previous, around the round."""
    names = [norm(item) for item in order]
    turned = names[names.index(norm(previous)):] + names[:names.index(norm(previous))]
    require(
        len(turned) > 1 and turned[1] == norm(name),
        f"ORDER {title}: {shown(order)}",
    )


def expect_skirmish(order):
    """One rotation of the skirmish round."""
    expect_one("skirmish", order, (skirmish_order(),))


def canon_room(name):
    """A battle-room stop, with the local player's slot written as 0."""
    text = norm(name)
    for prefix in ("ready", "side", "logo", "teamicons"):
        rest = text[len(prefix):]
        if text.startswith(prefix) and rest.isdigit():
            return prefix + "0"
    return text


def expect_room(order):
    """The host's round. START follows the chat line only once the room can start.

    This launch never readies a player, so START stays disabled and is left out.
    """
    names = [canon_room(name) for name in order]
    if "start" in names:
        require(names.count("start") == 1, f"ORDER battle room: {shown(order)}")
        expect_one("battle room", without(names, "start"), (ROOM,))
        expect_after("battle room", names, "start", "message")
        return
    expect_one("battle room", names, (ROOM,))


def expect_settings(order):
    """Controls, from the section list through the close mark."""
    expect_one("settings", order, (CONTROLS,))


def expect_library(order):
    """Active's tab, its change and campaign rows, Close, Refresh and the close mark.

    At Compact the search, the keys control, unload and the map links are
    clipped out of the pane, so they are not stops. The library opens on
    the search with the ring showing; the walk checks that before Tab.
    """
    expect_one("library", order, (LIBRARY,))


def expect_key_editor(order):
    """The list view, search, the list and the footer."""
    expect_one("key editor", order, (KEY_EDITOR,))


def expect_map_browser(order):
    """The source pill, the two strips, search, the first card, then Cancel and Close.

    Select is a stop only while the chosen card can be played. This folder's
    first card cannot, so Select is absent. When it is a stop it sits between
    Cancel and Close.
    """
    names = [norm(name) for name in order]
    if "maps-select" in names:
        require(names.count("maps-select") == 1, f"ORDER map browser: {shown(order)}")
        expect_one("map browser", without(names, "maps-select"), (MAP_BROWSER,))
        expect_after("map browser", names, "maps-select", "maps-cancel")
        return
    expect_one("map browser", names, (MAP_BROWSER,))


def expect_mutators(order):
    """The filter, search, the list and the footer.

    A new data folder holds no mutator, so nothing is chosen and the details
    pane has no stop.
    """
    expect_one("mutators", order, (MUTATORS,))


def expect_welcome(order):
    """Skip, the language move, the English card and the next card."""
    expect_one("welcome", order, (WELCOME,))


def expect_overlay(order):
    """Edit keys, Close and the list."""
    expect_one("keys overlay", order, (OVERLAY,))


def expect_match_menu(order):
    """Load Game, Save Game, Options, the briefing, Help, Exit and Resume.

    Options is the PREFS gadget. Return on it opens the sound page.
    """
    names = [norm(name) for name in order]
    wanted = ["loadgame", "savegame", "prefs", "mission", "help", "exit", "ok"]
    require(names == wanted, f"ORDER in-game menu: {shown(order)}")


def expect_match_options(order):
    """The in-game Options hub: Cancel, the four sections and Previous."""
    expect_one("in-game options", order, (MATCH_HUB,))


def expect_match_sound(order):
    """The sound page Return on PREFS opens."""
    expect_one("in-game sound", order, (MATCH_SOUND,))


def quoted(text):
    """One preferences value, with quotes and backslashes escaped."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_preferences(path, welcome):
    """Writes a fresh preferences file. The welcome launch leaves the first run open."""
    rows = [
        ("multiplayer|tcpaddr", "192.0.2.7"),
        ("open-annihilation.update-check", "never"),
        ("open-annihilation.content-updates", "never"),
        ("open-annihilation.access.always-show-focus", "0"),
    ]
    if not welcome:
        rows.append(("open-annihilation.first-run", "done"))
    lines = ["open-annihilation-preferences 1"]
    lines.extend(quoted(key) + " " + quoted(value) for key, value in rows)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def tree_of(client):
    """The control tree, checked to be an object."""
    tree = body_of(client.call("ui.tree"))
    require(isinstance(tree, dict), f"the tree is {type(tree).__name__}")
    return tree


def screen_of(client):
    """What ui.screen reports."""
    body = body_of(client.call("ui.screen"))
    require(isinstance(body, dict), f"the screen is {type(body).__name__}")
    return body


def focused_leaf(tree, name):
    """The leaf ui.screen named, preferring the one that has the keyboard."""
    folded = str(name).casefold()
    found = None
    for leaf in leaves(tree):
        if str(leaf.get("name") or "").casefold() != folded:
            continue
        if leaf.get("focused") and not leaf.get("hidden"):
            return leaf
        if found is None:
            found = leaf
    return found


def sample_names(tree):
    """A short list of control names, for a failure."""
    names = [str(leaf.get("name")) for leaf in leaves(tree) if leaf.get("name")]
    return ", ".join(names[:24])


def wait_screen(client, name, timeout_ms=WAIT_MS):
    """Waits until that screen is shown."""
    body = body_of(client.call("ui.wait", {"screen": name, "timeout_ms": timeout_ms}, START_TIMEOUT))
    require(body.get("ok") is True, f"waiting for {name} answered {body}")


def wait_control(client, name, timeout_ms=WAIT_MS):
    """Waits until that control is listed."""
    body = body_of(
        client.call("ui.wait", {"control": name, "timeout_ms": timeout_ms}, START_TIMEOUT)
    )
    require(body.get("ok") is True, f"waiting for {name} answered {body}")


def click_first(client, names):
    """Clicks the first of those controls that is shown and takes a click."""
    tree = tree_of(client)
    for wanted in names:
        folded = wanted.casefold()
        for leaf in leaves(tree):
            if str(leaf.get("name") or "").casefold() != folded:
                continue
            if leaf.get("visible") and leaf.get("enabled", True):
                client.call("ui.click", {"name": leaf["name"]})
                return leaf["name"]
    raise Failure(f"none of {names} is shown; controls: {sample_names(tree)}")


def tab(client):
    """Presses Tab and lets it go."""
    client.call("ui.key", {"chord": "Tab", "phase": "tap"})


def press(client, chord):
    """Presses one chord and lets it go."""
    client.call("ui.key", {"chord": chord, "phase": "tap"})


def focus_named(client, name):
    """Tabs until that control has the keyboard."""
    wanted = norm(name)
    for _step in range(WALK_LIMIT):
        if norm(screen_of(client).get("focused")) == wanted:
            return
        tab(client)
    raise Failure(f"Tab did not reach {name}")


def covering_dialog(body, tree):
    """The modal dialog that takes the keys, or None when nothing covers the screen.

    ui.screen names every dialog, and a button drawn over the screen is one of
    them. Only a modal dialog keeps the focus inside itself.
    """
    modal = []
    for leaf in leaves(tree):
        if leaf.get("hidden") or not leaf.get("modal"):
            continue
        dialog = leaf.get("dialog")
        if dialog and dialog not in modal:
            modal.append(dialog)
    if not modal:
        return None
    for dialog in body.get("dialogs") or []:
        if dialog in modal:
            return dialog
    return modal[0]


def walk(client, title, limit=WALK_LIMIT, check_ring=True):
    """Tabs until the order wraps. The ring must be hidden before the first Tab.

    The Library opens with the ring on its search, so the player can type at
    once. That station passes check_ring false.
    """
    print(f"walking {title}", flush=True)
    opened = screen_of(client)
    if check_ring:
        require(
            not opened.get("focus_shown"),
            f"{title}: the ring shows before a key ({opened.get('focused')!r})",
        )
    order = []
    first = None
    for _step in range(limit):
        tab(client)
        body = screen_of(client)
        name = body.get("focused")
        require(name, f"{title}: Tab left no focus after {shown(order)}")
        if first is None:
            first = name
        elif name == first:
            break
        else:
            require(name not in order, f"{title}: {name} repeats before the order wraps")
        order.append(name)
        tree = tree_of(client)
        leaf = focused_leaf(tree, name)
        require(leaf is not None, f"{title}: {name} is not in the tree")
        label = str(leaf.get("label") or "").strip()
        require(label, f"{title}: {name} has no label")
        cover = covering_dialog(body, tree)
        if cover is not None:
            require(
                leaf.get("dialog") == cover,
                f"{title}: {name} is in {leaf.get('dialog')!r}, not the top dialog {cover!r}",
            )
    else:
        raise Failure(f"{title}: Tab did not wrap after {limit}; ORDER {shown(order)}")
    require(order, f"{title}: Tab walked no stop")
    print(f"ORDER {title}: {shown(order)}", flush=True)
    return order


def free_port():
    """Returns a UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((ADDRESS, 0))
        return probe.getsockname()[1]


def drive_menus(client):
    """The menus from the main menu through Settings, the editors and the Library."""
    wait_screen(client, "main_menu")
    opened = screen_of(client)
    require(
        opened.get("focused") is None and not opened.get("focus_shown"),
        f"the main menu at open names {opened.get('focused')!r}, ring {opened.get('focus_shown')}",
    )
    expect_one("main menu", walk(client, "main menu"), (MENU_PLAIN, MENU_STRIP))
    click_first(client, ["SINGLE", "SingleBtn"])
    wait_screen(client, "single_player")
    expect_one("single player", walk(client, "single player"), (SINGLE,))
    click_first(client, ["NewCamp"])
    wait_control(client, "Arm")
    expect_one("new campaign", walk(client, "new campaign"), (CAMPAIGN,))
    click_first(client, ["PrevMenu"])
    wait_screen(client, "single_player")
    click_first(client, ["LoadGame"])
    wait_screen(client, "load_game")
    wait_control(client, "OK")
    expect_one("load game message", walk(client, "load game message"), (["OK"],))
    click_first(client, ["OK"])
    wait_control(client, "GAMES")
    expect_one("load game", walk(client, "load game"), (LOAD,))
    click_first(client, ["CANCEL"])
    wait_screen(client, "single_player")
    click_first(client, ["Options"])
    wait_control(client, "SOUND")
    expect_one("options", walk(client, "options"), (HUB,))
    for name, order in (("SOUND", SOUND), ("MUSIC", MUSIC), ("SPEEDS", SPEEDS), ("VISUALS", VISUALS)):
        click_first(client, [name])
        wait_control(client, order[len(SECTIONS)])
        expect_one(name.lower(), walk(client, name.lower()), (order,))
    click_first(client, ["PREV"])
    wait_screen(client, "single_player")
    click_first(client, ["Skirmish"])
    wait_screen(client, "skirmish")
    expect_skirmish(walk(client, "skirmish"))
    click_first(client, ["SelectMap"])
    wait_control(client, "MAPNAMES")
    expect_one("map selection", walk(client, "map selection"), (MAP_SELECT,))
    click_first(client, ["oa.more-maps", "more-maps"])
    wait_control(client, "oa.maps-search")
    expect_map_browser(walk(client, "map browser"))
    click_first(client, ["oa.maps-close", "maps-close"])
    wait_control(client, "MAPNAMES")
    click_first(client, ["PREVMENU"])
    wait_screen(client, "skirmish")
    click_first(client, ["oa.mutators", "Mutators"])
    wait_control(client, "oa.mutators.close")
    expect_mutators(walk(client, "mutators"))
    click_first(client, ["oa.mutators.close", "mutators.close"])
    wait_screen(client, "skirmish")
    click_first(client, ["PrevMenu"])
    wait_screen(client, "single_player")
    click_first(client, ["PrevMenu"])
    wait_screen(client, "main_menu")
    click_first(client, ["oa.settings-button", "settings-button"])
    wait_control(client, "oa.settings.ok")
    expect_settings(walk(client, "settings"))
    click_first(client, ["oa.settings.key-layout.edit"])
    wait_control(client, "oa.key-editor.close")
    expect_key_editor(walk(client, "key editor"))
    click_first(client, ["oa.key-editor.close"])
    wait_control(client, "oa.settings.ok")
    click_first(client, ["oa.settings.close-x"])
    wait_screen(client, "main_menu")
    click_first(client, ["oa.button"])
    wait_control(client, "oa.library.close")
    opened = screen_of(client)
    require(
        opened.get("focus_shown") and norm(opened.get("focused")) == "library.active.search",
        f"the library at open names {opened.get('focused')!r}, ring {opened.get('focus_shown')}",
    )
    expect_library(walk(client, "library", check_ring=False))
    click_first(client, ["oa.library.close-x"])
    wait_screen(client, "main_menu")
    client.call("game.quit")


def drive_welcome(client):
    """The welcome's language step. Return is not pressed."""
    deadline = time.monotonic() + START_TIMEOUT
    while True:
        tree = tree_of(client)
        if any("first-run." in str(leaf.get("name") or "") and leaf.get("visible") for leaf in leaves(tree)):
            break
        require(time.monotonic() < deadline, "the welcome did not open")
        time.sleep(0.25)
    expect_welcome(walk(client, "welcome"))
    client.call("game.quit")


def drive_match(client):
    """A skirmish, then the keys overlay, the in-game menu, its sound page and Options.

    Return on PREFS opens the sound page. Return on Previous comes back to
    Resume. The O key opens the Options hub.
    """
    wait_screen(client, "main_menu")
    click_first(client, ["SINGLE", "SingleBtn"])
    wait_screen(client, "single_player")
    click_first(client, ["Skirmish"])
    wait_screen(client, "skirmish")
    click_first(client, ["Start"])
    body = body_of(client.call("ui.wait", {"screen": "match", "timeout_ms": 90000}, START_TIMEOUT))
    require(body.get("ok") is True, f"the skirmish did not start: {body}")
    client.call("ui.key", {"action": "ui.keys-overlay", "phase": "tap"})
    wait_control(client, "oa.keys-overlay.close")
    expect_overlay(walk(client, "keys overlay"))
    click_first(client, ["oa.keys-overlay.close"])
    client.call("ui.key", {"action": "game.menu", "phase": "tap"})
    wait_control(client, "OK")
    expect_match_menu(walk(client, "in-game menu"))
    focus_named(client, "PREFS")
    press(client, "Return")
    wait_control(client, "FXVOL")
    expect_match_sound(walk(client, "in-game sound"))
    focus_named(client, "PREV")
    press(client, "Return")
    wait_control(client, "OK")
    press(client, "O")
    wait_control(client, "PREV")
    expect_match_options(walk(client, "in-game options"))
    client.call("game.quit")


def drive_room(client):
    """A hosted room on loopback. The match is not started."""
    wait_screen(client, "mp_battleroom")
    opened = screen_of(client)
    require(
        opened.get("focused") == "MESSAGE" and not opened.get("focus_shown"),
        f"the battle room at open names {opened.get('focused')!r}, ring {opened.get('focus_shown')}",
    )
    expect_room(walk(client, "battle room"))
    client.call("game.quit")


def launch(native, game_dir, workdir, drive, extra, welcome):
    """Starts the game from a fresh preferences file and runs one pass."""
    write_preferences(workdir / "keyboard.PREFERENCES", welcome)
    log_dir = workdir / "logs"
    user_folder = workdir / "user"
    data_folder = workdir / "data"
    log_dir.mkdir()
    user_folder.mkdir()
    data_folder.mkdir()
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
            "--resolution",
            "640x480",
            "--preferences-file",
            str(workdir / "keyboard.PREFERENCES"),
            "--log-dir",
            str(log_dir),
            "--user-folder",
            str(user_folder),
            "--data-dir",
            str(data_folder),
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
                "clientInfo": {"name": "native-keyboard-walk", "version": "0"},
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


def run_pass(native, game_dir, root, name, drive, extra, welcome):
    """One launch in its own folder."""
    print(f"launch {name}", flush=True)
    with tempfile.TemporaryDirectory(prefix=f"A29-{name}-", dir=root) as work:
        launch(native, game_dir, Path(work), drive, extra, welcome)


def main():
    """Runs every launch, each from its own preferences and user folder."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    native = Path(args.native)
    game_dir = Path(args.game_dir)
    run_pass(native, game_dir, root, "menus", drive_menus, [], False)
    run_pass(native, game_dir, root, "welcome", drive_welcome, ["--first-run"], True)
    run_pass(native, game_dir, root, "match", drive_match, [], False)
    port = free_port()
    run_pass(
        native,
        game_dir,
        root,
        "room",
        drive_room,
        ["--host", "--dplay-port", str(port), "--player-name", "Hoster", "--game-name", "Open Room"],
        False,
    )
    print("native-keyboard-walk: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError) as error:
        print(f"native-keyboard-walk: {error}", file=sys.stderr)
        sys.exit(1)
