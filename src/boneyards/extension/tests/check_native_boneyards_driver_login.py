#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Log in through the login console a program driving the game can see.

The game runs on the dummy drivers with --fark, "-y" and a replay of Rover
replies. "-y" enters the Boneyards. The check waits until the login console
is listed, reads its fields, the guest choice, LOGIN and the server list,
types the replay's guest name and presses LOGIN. The console then closes.
"""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import time

# The engine's helper lives in tools/, as the other automation checks find it.
_TOOLS = Path(__file__).resolve().parents[4] / "tools"
sys.path.insert(0, str(_TOOLS))
import check_native_automation as automation  # noqa: E402

# The guest the replay answers. The password field stays empty.
GUEST = "oa_check"
# SDL's backspace, which the login field deletes with.
BACKSPACE_SCANCODE = 42
BACKSPACE_KEYCODE = 8
CANVAS_WIDTH = 640
CANVAS_HEIGHT = 480


def redact(control):
    """A control safe to print: a password field's text is left out."""
    shown = dict(control)
    if shown.get("name") == "Password":
        shown["text"] = "<omitted>"
    return shown


def login_controls(answer):
    """The login console's controls from one controls answer."""
    return [control for control in answer.get("controls", []) if control.get("window") == "Login"]


def find_login(answer, name, kind):
    """The login control of a name and kind, or None."""
    for control in login_controls(answer):
        if control.get("name") == name and control.get("kind") == kind:
            return control
    return None


def require(answer, name, kind):
    """Returns the login control of a name and kind, or fails."""
    found = find_login(answer, name, kind)
    if found is None:
        listed = [redact(control) for control in login_controls(answer)]
        raise automation.AutomationFailure(f"Login has no {kind} {name}: {listed}")
    return found


def on_canvas(control):
    """Whether a control's rectangle lies on the 640 by 480 canvas."""
    rect = control.get("rect")
    if not isinstance(rect, list) or len(rect) != 4:
        return False
    x, y, width, height = rect
    return (
        isinstance(x, int) and isinstance(y, int) and isinstance(width, int) and
        isinstance(height, int) and width > 0 and height > 0 and x >= 0 and y >= 0 and
        x + width <= CANVAS_WIDTH and y + height <= CANVAS_HEIGHT
    )


def poll(client, ready):
    """Polls controls until `ready` accepts an answer, or the wait runs out."""
    deadline = time.monotonic() + automation.TIMEOUT
    answer = client.request("controls")
    while not ready(answer) and time.monotonic() < deadline:
        time.sleep(0.05)
        answer = client.request("controls")
    return answer


def click(client, control):
    """Clicks a control at the centre of its rectangle."""
    screen = client.request("screen").get("screen")
    x, y, width, height = control["rect"]
    answer = client.request(
        "input", expect_screen=screen,
        events=automation.click_events(x + width // 2, y + height // 2),
    )
    if not answer.get("ok"):
        raise automation.AutomationFailure(f"the click on {control.get('name')} was answered {answer}")
    return answer


def type_guest(client):
    """Clears the focused field and types the replay's guest name."""
    screen = client.request("screen").get("screen")
    events = []
    for _ in range(16):
        events.append({"kind": "key_down", "scancode": BACKSPACE_SCANCODE, "keycode": BACKSPACE_KEYCODE})
        events.append({"kind": "key_up", "scancode": BACKSPACE_SCANCODE, "keycode": BACKSPACE_KEYCODE})
    events.append({"kind": "text", "text": GUEST})
    answer = client.request("input", expect_screen=screen, events=events)
    if not answer.get("ok"):
        raise automation.AutomationFailure(f"typing the guest name was answered {answer}")


def status_lines(answer):
    """The login status list's rows, for a login that did not finish."""
    status = find_login(answer, "Status", "list")
    if status is None:
        return []
    return list(status.get("items") or [])


def check(native, game_dir, mod_dir, replay, workdir):
    """Lists the login console and logs in; raises AutomationFailure when it differs."""
    # The name is typed here. A name or password in the environment is not used.
    os.environ.pop("OA_BY_USER", None)
    os.environ.pop("OA_BY_PASSWORD", None)
    game = automation.Game(
        native, game_dir, workdir,
        ["-y", "--mod-dir", str(mod_dir), "--boneyards-replay", str(replay)],
    )
    clients = []
    try:
        endpoint = game.endpoint()
        address, token = endpoint.get("address", ""), endpoint.get("token", "")
        if not address.startswith("127.0.0.1:") or len(token) != 32:
            raise automation.AutomationFailure(f"the endpoint file holds {endpoint}")
        client = automation.Client(address)
        clients.append(client)
        hello = client.request("hello", versions=[1], client="boneyards", token=token)
        if not hello.get("ok"):
            raise automation.AutomationFailure(f"hello was answered {hello}")
        automation.wait_screen(client, "main_menu")

        shown = poll(client, lambda answer: find_login(answer, "Username", "text field") is not None)
        if find_login(shown, "Username", "text field") is None:
            raise automation.AutomationFailure(
                f"the login console was not listed; the screen is {shown.get('screen')}"
            )
        username = require(shown, "Username", "text field")
        password = require(shown, "Password", "text field")
        guest = require(shown, "Guest Login", "check box")
        login = require(shown, "LOGIN", "button")
        require(shown, "CANCEL", "button")
        require(shown, "REGISTER", "button")
        servers = require(shown, "Server List", "list")
        for control in (username, password, guest, login, servers):
            if not control.get("visible") or not on_canvas(control):
                raise automation.AutomationFailure(
                    f"{control.get('name')} is not on the canvas: {redact(control)}"
                )
        if not username.get("enabled"):
            raise automation.AutomationFailure(f"Username does not take text: {redact(username)}")
        items = servers.get("items") or []
        if not items:
            raise automation.AutomationFailure(f"the server list is empty: {redact(servers)}")
        names = [control.get("name") for control in shown.get("controls", [])]
        if "SINGLE" in names and names.index("LOGIN") > names.index("SINGLE"):
            raise automation.AutomationFailure(f"LOGIN is not listed ahead of SINGLE: {names}")

        if not guest.get("checked"):
            click(client, guest)
            chosen = poll(
                client,
                lambda answer: (found := find_login(answer, "Guest Login", "check box"))
                and found.get("checked"),
            )
            if not (find_login(chosen, "Guest Login", "check box") or {}).get("checked"):
                raise automation.AutomationFailure("Guest Login stayed unselected")
        password = require(client.request("controls"), "Password", "text field")
        if password.get("enabled"):
            raise automation.AutomationFailure("the password field stayed enabled for a guest")

        username = require(client.request("controls"), "Username", "text field")
        click(client, username)
        focused = poll(
            client,
            lambda answer: (found := find_login(answer, "Username", "text field")) and found.get("focused"),
        )
        if not (find_login(focused, "Username", "text field") or {}).get("focused"):
            raise automation.AutomationFailure("Username did not take the keyboard")
        type_guest(client)
        ready = poll(client, lambda answer: (
            (field := find_login(answer, "Username", "text field")) and field.get("text") == GUEST and
            (button := find_login(answer, "LOGIN", "button")) and button.get("enabled")
        ))
        login = find_login(ready, "LOGIN", "button")
        field = find_login(ready, "Username", "text field")
        if field is None or field.get("text") != GUEST or login is None or not login.get("enabled"):
            raise automation.AutomationFailure(
                f"LOGIN stayed disabled; status: {status_lines(ready)}"
            )
        click(client, login)
        closed = poll(client, lambda answer: find_login(answer, "LOGIN", "button") is None)
        if find_login(closed, "LOGIN", "button") is not None:
            raise automation.AutomationFailure(
                f"the login console stayed open; status: {status_lines(closed)}"
            )
        # Closing the window while the Boneyards is up asks whether to leave
        # and does not end the process, so the check stops the game itself.
        game.stop()
    except (automation.AutomationFailure, OSError, ValueError) as failure:
        game.stop()
        game.take_output()
        print("\n".join(f"game: {line}" for line in game.lines))
        if isinstance(failure, automation.AutomationFailure):
            raise
        raise automation.AutomationFailure(f"{type(failure).__name__}: {failure}") from failure
    finally:
        for client in clients:
            client.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--mod-dir", required=True, type=Path, help="the core mod")
    parser.add_argument("--replay", required=True, type=Path, help="the replayed Rover replies")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
        try:
            check(args.native, args.game_dir, args.mod_dir, args.replay, Path(scratch))
        except automation.AutomationFailure as failure:
            print(f"native-boneyards-driver-login: {failure}", file=sys.stderr)
            return 1
    print("native-boneyards-driver-login: the login console listed its controls and logged in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
