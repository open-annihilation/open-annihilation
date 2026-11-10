#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check input through the automation endpoint of a running game, end to end.

The game runs through its main loop on the dummy SDL drivers, started with
--fark, as check_native_automation.py starts it, and is driven only by the
endpoint's input, which reaches it through SDL's event queue:

- input requests sent far faster than the game takes them, one a frame, are
  held back: the game reads no more of them than about a frame's worth
  while they wait, so the sender stalls long before it has sent
  FLOOD_BYTES and the game's memory stays within FLOOD_GROWTH_BYTES (not
  judged with --sanitized: a build with the address sanitizer keeps the
  memory it frees in a quarantine of its own);
- MULTI on the main menu opens the providers, whose list names TCP/IP; a
  double click on its row, placed from the list's rows in the window's
  pixels, opens the TCP/IP dialog, with its address field focused and holding the
  stored address;
- keys and the text they type, End, BackSpace over the stored address and a
  new one, reach the field: it then holds the new address;
- Cancel and Previous Menu return to the main menu, and Single Player,
  Skirmish and Start begin a skirmish;
- in the match the camera stays still, scrolls right while the endpoint
  holds the right arrow key down, as the game reads a held key, and stops
  once the key is let go, and once a client that left holding it is gone;
- F2 opens the game menu; in Save Game the name field holds the name typed,
  OK saves the match under it, and Load Game lists it, selected; Cancel and
  Resume go back to the match;
- quit there leaves the match with no question and ends the game with
  status 0, as the main menu's Exit does.
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import native_offline

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_native_automation as automation  # noqa: E402

AutomationFailure = automation.AutomationFailure
# The address the check types, replacing the stored one.
TYPED_ADDRESS = "127.0.0.1"
# SDL's scancodes and key codes of the keys the check presses, on a US layout.
KEYS = {
    "End": (77, 0x4000004D),
    "BackSpace": (42, 0x08),
    "Right": (79, 0x4000004F),
    "F2": (59, 0x4000003B),
    ".": (55, ord(".")),
    "0": (39, ord("0")),
    **{str(digit): (29 + digit, ord(str(digit))) for digit in range(1, 10)},
}
# Seconds the camera is given to move while the key is held.
SCROLL_WAIT = 10
# The name the match is saved under.
SAVE_NAME = "endpoint check"
# Bytes of input requests a client sends without reading; the game must hold
# it back well before, and its memory must grow by less than the second.
FLOOD_BYTES = 64 << 20
FLOOD_GROWTH_BYTES = 32 << 20
# Seconds a send must wait for the game to count as held back.
FLOOD_STALL = 2


def key(name, down):
    """A key's event, as a client sends it with the key's scancode and key code."""
    scancode, keycode = KEYS[name]
    return {"kind": "key_down" if down else "key_up", "key": name, "scancode": scancode,
            "keycode": keycode}


def press(name, typed=None):
    """A key pressed and let go, with the text it types between, as a keyboard sends them."""
    events = [key(name, True)]
    if typed is not None:
        events.append({"kind": "text", "text": typed})
    return events + [key(name, False)]


def resident_bytes(pid):
    """The memory a process holds in RAM, in bytes; None where ps cannot say."""
    if automation.RUNNER or shutil.which("ps") is None:
        return None
    result = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
    text = result.stdout.strip()
    return int(text) * 1024 if result.returncode == 0 and text.isdigit() else None


def check_held_back(game, client, judge_memory):
    """Sends input requests without reading the answers, far faster than the game takes them."""
    before = resident_bytes(game.process.pid)
    request = automation.encode({"id": 1, "op": "input", "expect_screen": "main_menu",
                                 "events": [{"kind": "pointer_move", "x": 2, "y": 2}]})
    block = request * max(1, (64 << 10) // len(request))
    client.sock.settimeout(FLOOD_STALL)
    sent = 0
    try:
        while sent < FLOOD_BYTES:
            view = memoryview(block)
            while view:
                taken = client.sock.send(view)
                view = view[taken:]
                sent += taken
    except (socket.timeout, BlockingIOError):
        pass
    finally:
        client.sock.settimeout(automation.TIMEOUT)
    if sent >= FLOOD_BYTES:
        raise AutomationFailure(f"the game read all {sent} bytes of input requests sent faster than it takes them")
    after = resident_bytes(game.process.pid)
    if judge_memory and before is not None and after is not None and after - before >= FLOOD_GROWTH_BYTES:
        raise AutomationFailure(f"the game's memory grew from {before} to {after} bytes while "
                                f"{sent} bytes of input requests waited")


def camera(client):
    """The match camera's place, from screen."""
    answer = client.request("screen")
    place = answer.get("camera")
    if answer.get("screen") != "match" or not isinstance(place, dict):
        raise AutomationFailure(f"screen in the match was answered {answer}")
    return place["x"], place["y"]


def check_address(client):
    """Types a new address into the TCP/IP dialog's field, from the main menu."""
    automation.click_control(client, "MULTI", "main_menu")
    automation.wait_screen(client, "mp_providers")
    providers = automation.find_control(client, "DPLAY")
    rows = [index for index, item in enumerate(providers.get("items", [])) if "TCP/IP" in item]
    if providers.get("kind") != "list" or not rows or providers.get("row_height", 0) <= 0:
        raise AutomationFailure(f"the providers' list is {providers}")
    # The row's middle, in the window's own pixels.
    x, y, width, _ = providers["rect"]
    wx, wy, wwidth, wheight = providers["window_rect"]
    scale = wwidth / width
    row_y = y + (rows[0] - providers["first_visible"]) * providers["row_height"] + \
        providers["row_height"] // 2
    row_x, row_y = round(wx + wwidth / 2), round(wy + (row_y - y) * scale)
    # A double click on a row takes the provider: a press that counts two
    # follows the first click.
    second = [{"kind": kind, "x": row_x, "y": row_y, "space": "window", "button": "left",
               "clicks": 2} for kind in ("button_down", "button_up")]
    clicked = client.request("input", expect_screen="mp_providers",
                             events=automation.click_events(row_x, row_y, "window") + second)
    if not clicked.get("ok"):
        raise AutomationFailure(f"the double click on the TCP/IP row was answered {clicked}")
    dialog = automation.wait_screen(client, "mp_tcp")
    field = automation.find_control(client, "ADDRESS")
    if dialog.get("dialogs") != ["tcp"] or dialog.get("focused") != "ADDRESS" or \
            field.get("kind") != "text field" or field.get("dialog") != "tcp" or \
            field.get("text") != automation.STORED_ADDRESS:
        raise AutomationFailure(f"the TCP/IP dialog is {dialog} with the field {field}")

    events = press("End")
    for _ in field["text"]:
        events += press("BackSpace")
    for character in TYPED_ADDRESS:
        events += press(character, character)
    typed = client.request("input", expect_screen="mp_tcp", events=events)
    if not typed.get("ok") or "consumed" not in typed:
        raise AutomationFailure(f"the typed address was answered {typed}")
    field = automation.find_control(client, "ADDRESS")
    if field.get("text") != TYPED_ADDRESS:
        raise AutomationFailure(f"the field holds {field.get('text')!r}, not {TYPED_ADDRESS!r}")

    automation.click_control(client, "PREV", "mp_tcp")
    automation.wait_screen(client, "mp_providers")
    automation.click_control(client, "PREVMENU", "mp_providers")
    automation.wait_screen(client, "main_menu")


def wait_control(client, name, shown=True):
    """Waits for the screen's control of a name to be shown, or with shown false to be gone."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        controls = client.request("controls").get("controls", [])
        found = any(c.get("name") == name and c.get("visible") for c in controls)
        if found == shown:
            return
        if time.monotonic() > deadline:
            raise AutomationFailure(f"{name} was {'never shown' if shown else 'never gone'}: {controls}")
        time.sleep(0.1)


def check_save(client):
    """Saves the match through the game menu's Save Game and finds the save in its Load Game."""
    if not client.request("input", expect_screen="match", events=press("F2")).get("ok"):
        raise AutomationFailure("F2 in the match was not answered")
    wait_control(client, "SAVEGAME")
    automation.click_control(client, "SAVEGAME", "match")
    automation.wait_screen(client, "load_game")
    automation.click_control(client, "GAMENAME", "load_game")
    typed = client.request("input", expect_screen="load_game",
                           events=[{"kind": "text", "text": SAVE_NAME}])
    if not typed.get("ok"):
        raise AutomationFailure(f"the save's name was answered {typed}")
    field = automation.find_control(client, "GAMENAME")
    if field.get("text") != SAVE_NAME:
        raise AutomationFailure(f"the save dialog's name field is {field}")
    automation.click_control(client, "LOAD", "load_game")
    automation.wait_screen(client, "match")
    wait_control(client, "LOADGAME")
    automation.click_control(client, "LOADGAME", "match")
    automation.wait_screen(client, "load_game")
    games = automation.find_control(client, "GAMES")
    if games.get("items") != [SAVE_NAME] or games.get("selected") != 0:
        raise AutomationFailure(f"Load Game lists {games}")
    automation.click_control(client, "CANCEL", "load_game")
    automation.wait_screen(client, "match")
    wait_control(client, "OK")
    automation.click_control(client, "OK", "match")
    wait_control(client, "SAVEGAME", shown=False)


def check_camera(client, connect):
    """Starts a skirmish from the main menu and scrolls its camera with a held key.

    Returns the client connected at the end: the one connect made after the
    first left holding a key.
    """
    automation.click_control(client, "SINGLE", "main_menu")
    automation.wait_screen(client, "single_player")
    automation.click_control(client, "Skirmish", "single_player")
    automation.wait_screen(client, "skirmish")
    automation.click_control(client, "Start", "skirmish")
    automation.wait_screen(client, "match")

    start = camera(client)
    time.sleep(0.5)
    if camera(client) != start:
        raise AutomationFailure(f"the camera moved from {start} with no key held")
    held = client.request("input", expect_screen="match", events=[key("Right", True)])
    if not held.get("ok"):
        raise AutomationFailure(f"the right arrow key's press was answered {held}")
    deadline = time.monotonic() + SCROLL_WAIT
    place = camera(client)
    while place[0] <= start[0] and time.monotonic() < deadline:
        time.sleep(0.1)
        place = camera(client)
    released = client.request("input", expect_screen="match", events=[key("Right", False)])
    if place[0] <= start[0] or place[1] != start[1]:
        raise AutomationFailure(f"the camera went from {start} to {place} with the right arrow held")
    if not released.get("ok"):
        raise AutomationFailure(f"the right arrow key's release was answered {released}")
    let_go = camera(client)
    time.sleep(0.5)
    if camera(client) != let_go:
        raise AutomationFailure(f"the camera moved on from {let_go} once the key was let go")

    # A client that leaves holding a key has it let go.
    if not client.request("input", expect_screen="match", events=[key("Right", True)]).get("ok"):
        raise AutomationFailure("the right arrow key's second press was not answered")
    client.close()
    client = connect()
    left = camera(client)
    time.sleep(0.5)
    if camera(client) != left:
        raise AutomationFailure(f"the camera moved on from {left} once its client had left")

    check_save(client)
    # quit leaves the match with no question and ends the game, as the main
    # menu's Exit does: the link closes, and the game ends with status 0
    # (wait_end). A further request finds the link closed, or is answered
    # before the game ends, never with the surrender question up.
    if not client.request("quit").get("ok"):
        raise AutomationFailure("quit in the match was not answered")
    deadline = time.monotonic() + automation.TIMEOUT
    while time.monotonic() < deadline:
        client.next_id += 1
        try:
            client.send_raw(automation.encode({"id": client.next_id, "op": "screen"}))
            answer = client.read_frame()
        except OSError:
            break
        if answer is None:
            break
        if answer[0].get("dialogs") == ["yesorno"]:
            raise AutomationFailure(f"quit in the match asked the surrender question: {answer[0]}")
        time.sleep(0.1)
    return client


def check(native, game_dir, workdir, sanitized=False):
    """Drives the game through the endpoint's input; raises AutomationFailure when it differs."""
    game = automation.Game(native, game_dir, workdir)
    client = None
    try:
        endpoint = game.endpoint()

        def connect():
            # Until the endpoint has seen a client that left go, a new one is busy.
            deadline = time.monotonic() + automation.TIMEOUT
            while True:
                connected = automation.Client(endpoint["address"])
                hello = connected.request("hello", versions=[1], client="check",
                                          token=endpoint["token"])
                if hello.get("ok"):
                    return connected
                connected.close()
                if not automation.refused(hello, "busy") or time.monotonic() > deadline:
                    raise AutomationFailure(f"hello was answered {hello}")
                time.sleep(0.1)

        client = connect()
        automation.wait_screen(client, "main_menu")
        check_held_back(game, client, judge_memory=not sanitized)
        # The answers it did not read are dropped with it.
        client.close()
        client = connect()
        check_address(client)
        client = check_camera(client, connect)
        game.wait_end()
    except (AutomationFailure, OSError, ValueError, KeyError, TypeError) as failure:
        game.stop()
        game.take_output()
        print("\n".join(f"game: {line}" for line in game.lines))
        if isinstance(failure, AutomationFailure):
            raise
        raise AutomationFailure(f"{type(failure).__name__}: {failure}") from failure
    finally:
        if client is not None:
            client.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    parser.add_argument("--sanitized", action="store_true",
                        help="the game is built with the address sanitizer, whose quarantine keeps the memory it "
                             "frees, so the game's memory is not judged while input waits")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
        try:
            check(args.native.resolve(), args.game_dir, Path(scratch), args.sanitized)
        except AutomationFailure as failure:
            print(f"native-automation-input: {failure}", file=sys.stderr)
            return 1
    print("native-automation-input: the game took the endpoint's input as a device's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
