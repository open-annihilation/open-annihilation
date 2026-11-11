#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The kit's focus ring on the original menus.

The game runs on the dummy SDL drivers, started with --mcp, in a 640x480
window. This client speaks one request at a time. On the main menu, Single
player and Options it checks the ring around the control the tree names as
focused:

- before any key, no pixel on the ring's path has the ring's colour;
- after Tab, the ring's pixels lie on that path around the focused control;
- after a pointer move, they are gone.

On the main menu, ui.screen names nothing at open. The first Tab leaves
Single player for the next stop of the main menu's order, Multiplayer (the
strip's MultiBtn while the strip is drawn), and draws the ring there. Space
on Load game, with the ring showing, opens the empty folder's message with
the ring hidden. With Always show focus off,
Skirmish's Tab order draws the ring around oa.mutators, a pointer move
hides it, and a skirmish's in-game menu draws it around OK after Tab. O,
the Options button's quick key, then opens the in-game Options page with
the ring hidden, and Tab shows it there.

A second launch, from its own preferences and user folder, turns Always
show focus on and checks that the ring is there when each of those screens
opens, before any key, and on Load game's message. Each launch writes its
preferences at the start, so one run cannot leave an answer the next would
read. Update checks stay off, so the game does not contact a catalogue.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import queue
import shlex
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
import native_offline

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
START_TIMEOUT = 120
STEP_TIMEOUT = 45
END_TIMEOUT = 60
WAIT_MS = 20000
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# kit::colour::focus, the ring's own colour, as three bytes of a picture.
FOCUS = bytes((0xB6, 0xE0, 0x5A))


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


def sample_names(tree):
    """A short list of control names, for a failure."""
    names = [leaf.get("name") for leaf in leaves(tree) if leaf.get("name")]
    return ", ".join(names[:24])


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


def write_preferences(path, always):
    """Writes a fresh preferences file. Always show focus is set either way."""
    rows = [
        ("multiplayer|tcpaddr", "192.0.2.7"),
        ("open-annihilation.update-check", "never"),
        ("open-annihilation.content-updates", "never"),
        ("open-annihilation.access.always-show-focus", "1" if always else "0"),
    ]
    lines = ["open-annihilation-preferences 1"]
    lines.extend(quoted(key) + " " + quoted(value) for key, value in rows)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def png_pixels(data):
    """The size and RGB pixels of a PNG: 8-bit RGB, not interlaced, rows unfiltered."""
    if not data.startswith(PNG_SIGNATURE):
        raise Failure("the picture does not start with PNG's signature")
    at, header, image_data = len(PNG_SIGNATURE), None, b""
    while at + 12 <= len(data):
        length = int.from_bytes(data[at:at + 4], "big")
        kind, body = data[at + 4:at + 8], data[at + 8:at + 8 + length]
        crc_at = at + 8 + length
        if zlib.crc32(kind + body) != int.from_bytes(data[crc_at:crc_at + 4], "big"):
            raise Failure(f"the picture's {kind!r} chunk fails its CRC")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            image_data += body
        at += 12 + length
    if header is None or header[2:4] != (8, 2) or header[6] != 0:
        raise Failure(f"the picture's header is {header}, not 8-bit RGB without interlacing")
    width, height = header[0], header[1]
    rows = zlib.decompress(image_data)
    stride = width * 3 + 1
    if len(rows) != stride * height or any(rows[row * stride] != 0 for row in range(height)):
        raise Failure("the picture's rows are not stored unfiltered")
    pixels = b"".join(rows[row * stride + 1:(row + 1) * stride] for row in range(height))
    return width, height, pixels


def focused_leaf(tree):
    """The control the ring is drawn around: a shown one, else one the strip covers."""
    covered = None
    for leaf in leaves(tree):
        if not leaf.get("focused"):
            continue
        name = str(leaf.get("name") or "")
        # A screen that draws the kit's own ring is not this ring's control.
        # oa.mutators is looked up by name, where that screen does not.
        if name.startswith("oa."):
            continue
        if leaf.get("hidden"):
            if covered is None:
                covered = leaf
            continue
        return leaf
    if covered is not None and not str(covered.get("name") or "").startswith("oa."):
        return covered
    return None


def ring_scale(leaf):
    """Pixels per point, from the window rectangle against the canvas rectangle."""
    canvas = leaf.get("rect") or []
    window = leaf.get("window_rect") or []
    if len(canvas) < 3 or len(window) < 3 or canvas[2] <= 0 or window[2] <= 0:
        return 1
    return max(1, int(round(window[2] / canvas[2])))


def ring_frame(leaf):
    """The ring's outer rectangle and its thickness, in the picture's pixels."""
    window = leaf.get("window_rect")
    require(
        isinstance(window, list) and len(window) == 4 and window[2] > 0 and window[3] > 0,
        f"{leaf.get('name')} has no window rectangle: {window}",
    )
    scale = ring_scale(leaf)
    inset = 2 * scale
    thick = scale
    left, top, width, height = (int(part) for part in window)
    return left - inset, top - inset, width + 2 * inset, height + 2 * inset, thick, scale


def control_radius(point_height):
    """The kit's corner radius for a control of this height, in points.

    The same steps as control_radius_for: Large from 38, Regular from 34,
    Compact from 28, and a shorter control in proportion, at least 2.
    """
    if point_height >= 38:
        return 8
    if point_height >= 34:
        return 7
    if point_height >= 28:
        return 6
    return max(point_height * 6 // 28, 2)


def corner_reach(frame):
    """How far the ring's rounded corner reaches, in picture pixels.

    The kit rounds the ring as a control of this height, then adds the
    two-point inset. On a tall control that radius is wider than the
    straight band, so the corner crosses into the control.
    """
    _left, _top, _wide, high, thick, scale = frame
    inset = thick * 2
    point_height = max((high - 2 * inset) // max(scale, 1), 1)
    return (control_radius(point_height) + 2) * max(scale, 1)


def on_path(column, row, left, top, width, height, thick, corner):
    """True when the pixel is in the band around the control, corners included.

    The straight band is the kit's inset, twice the ring's thickness,
    outside the control and inside the outer frame. A rounded corner also
    covers its own square, which on a tall control reaches into the control.
    """
    if column < left or row < top or column >= left + width or row >= top + height:
        return False
    reach = min(corner, min(width, height) // 2)
    if (column < left + reach and row < top + reach) or (
        column >= left + width - reach and row < top + reach
    ):
        return True
    if (column < left + reach and row >= top + height - reach) or (
        column >= left + width - reach and row >= top + height - reach
    ):
        return True
    inset = thick * 2
    inner_left = left + inset
    inner_top = top + inset
    inner_right = left + width - inset
    inner_bottom = top + height - inset
    if inner_left <= column < inner_right and inner_top <= row < inner_bottom:
        return False
    return True


def pixel(pixels, width, height, column, row):
    """One pixel's RGB, or None when the point is outside the picture."""
    if column < 0 or row < 0 or column >= width or row >= height:
        return None
    at = (row * width + column) * 3
    return pixels[at:at + 3]


def colour_on_path(pixels, width, height, frame):
    """Points on the ring's path whose pixel is the ring's colour."""
    left, top, wide, high, thick, _scale = frame
    corner = corner_reach(frame)
    found = []
    for row in range(max(top, 0), min(top + high, height)):
        for column in range(max(left, 0), min(left + wide, width)):
            if not on_path(column, row, left, top, wide, high, thick, corner):
                continue
            if pixel(pixels, width, height, column, row) == FOCUS:
                found.append((column, row))
    return found


def colour_in_margin(pixels, width, height, frame):
    """Points of the ring's colour in a margin around the path, on it or not."""
    left, top, wide, high, _thick, scale = frame
    pad = scale
    found = []
    row_from = max(top - pad, 0)
    row_to = min(top + high + pad, height)
    column_from = max(left - pad, 0)
    column_to = min(left + wide + pad, width)
    for row in range(row_from, row_to):
        for column in range(column_from, column_to):
            if pixel(pixels, width, height, column, row) == FOCUS:
                found.append((column, row))
    return found


def mid_edges(frame):
    """The middle of each straight side of the ring's outer rectangle."""
    left, top, wide, high, _thick, _scale = frame
    return (
        (left + wide // 2, top),
        (left + wide // 2, top + high - 1),
        (left, top + high // 2),
        (left + wide - 1, top + high // 2),
    )


def shoot(client):
    """The presented picture at the window's own size, and what the tool says of it."""
    result = client.call("capture.screenshot", {"source": "presented"})
    content = result.get("content") or []
    require(len(content) >= 2, f"a picture answered {content}")
    info = json.loads(content[0].get("text") or "")
    image = content[1]
    require(image.get("type") == "image", f"the picture block is {image.get('type')}")
    require(image.get("mimeType") == "image/png", f"the picture type is {image.get('mimeType')}")
    raw = base64.b64decode(image.get("data") or "")
    width, height, pixels = png_pixels(raw)
    require(info.get("source") == "presented", f"the picture source is {info.get('source')}")
    require(width > 0 and height > 0, f"the picture is {width}x{height}")
    return width, height, pixels


def tree_of(client):
    """The control tree, checked to be an object."""
    tree = body_of(client.call("ui.tree"))
    require(isinstance(tree, dict), f"the tree is {type(tree).__name__}")
    return tree


def focus_of(client, screen):
    """The focused control on this screen, and the ring's frame around it."""
    tree = tree_of(client)
    require(screen_name(tree) == screen, f"the tree's screen is {screen_name(tree)}, not {screen}")
    leaf = focused_leaf(tree)
    require(
        leaf is not None,
        f"{screen} names no focused control; controls: {sample_names(tree)}",
    )
    return leaf, ring_frame(leaf)


def describe(leaf, frame, pixels, width, height):
    """Where the ring was looked for, and the colour at the middle of each side."""
    left, top, wide, high, thick, scale = frame
    seen = []
    for column, row in mid_edges(frame):
        colour = pixel(pixels, width, height, column, row)
        text = "outside" if colour is None else "".join(f"{channel:02x}" for channel in colour)
        seen.append(f"({column},{row})={text}")
    return (
        f"{leaf.get('name')} window {leaf.get('window_rect')} scale {scale} "
        f"frame ({left},{top}) {wide}x{high} thick {thick}; mids {', '.join(seen)}"
    )


def expect_absent(pixels, width, height, leaf, frame, where):
    """No pixel on the ring's path has the ring's colour."""
    found = colour_on_path(pixels, width, height, frame)
    require(
        not found,
        f"{where}: the ring's colour is on the path at {found[:8]}; "
        f"{describe(leaf, frame, pixels, width, height)}",
    )


def expect_present(pixels, width, height, leaf, frame, where):
    """The ring's colour is on each straight side, and only on the path nearby."""
    missing = []
    for column, row in mid_edges(frame):
        if pixel(pixels, width, height, column, row) != FOCUS:
            missing.append((column, row))
    require(
        not missing,
        f"{where}: the ring is missing at {missing}; "
        f"{describe(leaf, frame, pixels, width, height)}",
    )
    left, top, wide, high, thick, _scale = frame
    corner = corner_reach(frame)
    off = [
        point
        for point in colour_in_margin(pixels, width, height, frame)
        if not on_path(point[0], point[1], left, top, wide, high, thick, corner)
    ]
    require(
        not off,
        f"{where}: the ring's colour is off the path at {off[:8]}; "
        f"{describe(leaf, frame, pixels, width, height)}",
    )


def wait_screen(client, name):
    """Waits until that screen is shown."""
    body = body_of(client.call("ui.wait", {"screen": name, "timeout_ms": WAIT_MS}, START_TIMEOUT))
    require(body.get("ok") is True, f"waiting for {name} answered {body}")
    require(screen_name(body) == name, f"the screen is {screen_name(body)}, not {name}")


def click_named(client, wanted):
    """Clicks the control the tree names, using the spelling the tree gives."""
    tree = tree_of(client)
    folded = wanted.casefold()
    leaf = None
    for candidate in leaves(tree):
        if str(candidate.get("name") or "").casefold() == folded and candidate.get("visible"):
            leaf = candidate
            break
    require(
        leaf is not None,
        f"no visible control {wanted}; controls: {sample_names(tree)}",
    )
    client.call("ui.click", {"name": leaf["name"]})


def move_pointer(client):
    """Moves the pointer by more than a pixel, in the canvas's own coordinates."""
    screen = body_of(client.call("ui.screen"))
    pointer = screen.get("pointer") or {}
    across = int(pointer.get("x") or 0)
    down = int(pointer.get("y") or 0)
    landed_x, landed_y = (40, 40)
    if abs(across - landed_x) + abs(down - landed_y) < 8:
        landed_x, landed_y = (200, 200)
    client.call("ui.pointer", {"x": landed_x, "y": landed_y, "space": "game"})


def reported_focus(client):
    """The control ui.screen names, and whether its ring is drawn."""
    body = body_of(client.call("ui.screen"))
    return body.get("focused"), bool(body.get("focus_shown"))


def check_screen(client, screen, always):
    """One screen: hidden until Tab unless Always show focus is on, then gone after a move."""
    wait_screen(client, screen)
    if screen == "main_menu":
        focused, shown = reported_focus(client)
        if always:
            require(
                focused == "SINGLE" and shown,
                f"the main menu at open names {focused!r}, ring {shown}",
            )
        else:
            require(
                focused is None and not shown,
                f"the main menu at open names {focused!r}, ring {shown}",
            )
    leaf, frame = focus_of(client, screen)
    width, height, pixels = shoot(client)
    if always:
        expect_present(pixels, width, height, leaf, frame, f"{screen} at open")
        return
    expect_absent(pixels, width, height, leaf, frame, f"{screen} before a key")
    client.call("ui.key", {"action": "ui.menu-focus-next", "phase": "tap"})
    wait_screen(client, screen)
    if screen == "main_menu":
        focused, shown = reported_focus(client)
        require(
            focused in ("MULTI", "MultiBtn") and shown,
            f"the main menu after Tab names {focused!r}, ring {shown}",
        )
    leaf, frame = focus_of(client, screen)
    width, height, pixels = shoot(client)
    expect_present(pixels, width, height, leaf, frame, f"{screen} after Tab")
    move_pointer(client)
    wait_screen(client, screen)
    width, height, pixels = shoot(client)
    expect_absent(pixels, width, height, leaf, frame, f"{screen} after the pointer moved")


def shown_focused(tree):
    """The first shown control the tree marks focused."""
    for leaf in leaves(tree):
        if leaf.get("focused") and not leaf.get("hidden"):
            return leaf
    return None


def tab_until(client, screen, name, limit=40):
    """Tabs until that control is the shown focus, and returns it."""
    folded = name.casefold()
    for _ in range(limit):
        tree = tree_of(client)
        require(
            screen_name(tree) == screen,
            f"the tree's screen is {screen_name(tree)}, not {screen}",
        )
        leaf = shown_focused(tree)
        if leaf is not None and str(leaf.get("name") or "").casefold() == folded:
            return leaf
        client.call("ui.key", {"action": "ui.menu-focus-next", "phase": "tap"})
        wait_screen(client, screen)
    tree = tree_of(client)
    raise Failure(f"Tab did not reach {name} on {screen}; controls: {sample_names(tree)}")


def leaf_named(tree, name):
    """A control of that name, a focused one first."""
    folded = name.casefold()
    found = None
    for leaf in leaves(tree):
        if str(leaf.get("name") or "").casefold() != folded:
            continue
        if leaf.get("focused"):
            return leaf
        if found is None:
            found = leaf
    return found


def check_load_message(client, always):
    """Space on Load game opens the empty folder's message with the ring hidden.

    Always show focus draws the ring on that message's OK.
    """
    tab_until(client, "single_player", "LoadGame")
    client.call("ui.key", {"action": "ui.menu-press", "phase": "tap"})
    wait_screen(client, "load_game")
    body = body_of(
        client.call("ui.wait", {"screen": "load_game", "control": "OK", "timeout_ms": WAIT_MS})
    )
    require(body.get("ok") is True, f"Load game's message did not open: {body}")
    tree = tree_of(client)
    leaf = leaf_named(tree, "OK")
    require(leaf is not None, f"Load game's message has no OK; controls: {sample_names(tree)}")
    frame = ring_frame(leaf)
    width, height, pixels = shoot(client)
    if always:
        expect_present(pixels, width, height, leaf, frame, "Load game's message at open")
    else:
        expect_absent(pixels, width, height, leaf, frame, "Load game's message at open")
    click_named(client, "OK")
    wait_screen(client, "load_game")
    click_named(client, "CANCEL")
    wait_screen(client, "single_player")


def check_mutators_and_match(client):
    """Skirmish's mutators button, then the in-game menu's OK."""
    click_named(client, "Skirmish")
    wait_screen(client, "skirmish")
    leaf = tab_until(client, "skirmish", "oa.mutators")
    frame = ring_frame(leaf)
    width, height, pixels = shoot(client)
    expect_present(pixels, width, height, leaf, frame, "oa.mutators after Tab")
    focused, shown = reported_focus(client)
    require(
        focused == "oa.mutators" and shown,
        f"Skirmish after Tab names {focused!r}, ring {shown}",
    )
    move_pointer(client)
    wait_screen(client, "skirmish")
    width, height, pixels = shoot(client)
    expect_absent(pixels, width, height, leaf, frame, "oa.mutators after the pointer moved")
    focused, shown = reported_focus(client)
    require(
        focused == "oa.mutators" and not shown,
        f"Skirmish after the pointer names {focused!r}, ring {shown}",
    )
    click_named(client, "Start")
    body = body_of(
        client.call("ui.wait", {"screen": "match", "timeout_ms": 90000}, START_TIMEOUT)
    )
    require(body.get("ok") is True, f"the skirmish did not start: {body}")
    client.call("ui.key", {"action": "game.menu", "phase": "tap"})
    body = body_of(
        client.call(
            "ui.wait", {"screen": "match", "control": "OK", "timeout_ms": WAIT_MS}, STEP_TIMEOUT
        )
    )
    require(body.get("ok") is True, f"the in-game menu did not open: {body}")
    # Resume is focused at open; Tab moves to the next stop of the menu's
    # order, Load Game, and shows the ring there.
    client.call("ui.key", {"action": "ui.menu-focus-next", "phase": "tap"})
    wait_screen(client, "match")
    tree = tree_of(client)
    leaf = leaf_named(tree, "LOADGAME")
    require(leaf is not None and leaf.get("focused"), f"the in-game menu's LOADGAME is {leaf}")
    frame = ring_frame(leaf)
    width, height, pixels = shoot(client)
    expect_present(pixels, width, height, leaf, frame, "the in-game menu after Tab")
    focused, shown = reported_focus(client)
    require(
        focused == "LOADGAME" and shown,
        f"the in-game menu after Tab names {focused!r}, ring {shown}",
    )
    # O, the Options button's quick key, turns the in-game panel's page while
    # the ring shows. The page opens with the ring hidden, and Tab shows it.
    client.call("ui.key", {"chord": "O", "phase": "tap"})
    body = body_of(
        client.call(
            "ui.wait", {"screen": "match", "control": "PREV", "timeout_ms": WAIT_MS}, STEP_TIMEOUT
        )
    )
    require(body.get("ok") is True, f"the in-game Options page did not open: {body}")
    tree = tree_of(client)
    leaf = leaf_named(tree, "PREV")
    require(leaf is not None and leaf.get("focused"), f"the Options page's PREV is {leaf}")
    frame = ring_frame(leaf)
    width, height, pixels = shoot(client)
    expect_absent(pixels, width, height, leaf, frame, "the in-game Options page at open")
    focused, shown = reported_focus(client)
    require(
        focused == "PREV" and not shown,
        f"the in-game Options page at open names {focused!r}, ring {shown}",
    )
    # Tab moves from OK to Cancel, the next stop, and shows the ring there.
    client.call("ui.key", {"action": "ui.menu-focus-next", "phase": "tap"})
    wait_screen(client, "match")
    tree = tree_of(client)
    leaf = leaf_named(tree, "CANCEL")
    require(leaf is not None and leaf.get("focused"), f"the Options page's CANCEL is {leaf}")
    frame = ring_frame(leaf)
    width, height, pixels = shoot(client)
    expect_present(pixels, width, height, leaf, frame, "the in-game Options page after Tab")


def drive(client, always):
    """Walks the menus. With the ring hidden at open, it also plays a skirmish."""
    check_screen(client, "main_menu", always)
    click_named(client, "SINGLE")
    check_screen(client, "single_player", always)
    check_load_message(client, always)
    click_named(client, "Options")
    check_screen(client, "options", always)
    if not always:
        click_named(client, "PREV")
        wait_screen(client, "single_player")
        check_mutators_and_match(client)
    client.call("game.quit")


def launch(native, game_dir, workdir, always):
    """Starts the game from a fresh preferences file and runs one pass."""
    write_preferences(workdir / "focus.PREFERENCES", always)
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
            "--resolution",
            "640x480",
            "--preferences-file",
            str(workdir / "focus.PREFERENCES"),
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
        client = Client(process, stdout_lines, stderr_lines)
        client.request(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "native-focus-ring", "version": "0"},
            },
            START_TIMEOUT,
        )
        send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        drive(client, always)
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
    """Runs both passes, each from its own folder."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    for always in (False, True):
        prefix = "A15-focus-shown-" if always else "A15-focus-hidden-"
        with tempfile.TemporaryDirectory(prefix=prefix, dir=root) as work:
            launch(Path(args.native), Path(args.game_dir), Path(work), always)
    print("native-focus-ring: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError, zlib.error) as error:
        print(f"native-focus-ring: {error}", file=sys.stderr)
        sys.exit(1)
