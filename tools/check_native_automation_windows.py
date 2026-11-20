#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the endpoint sees an extension's window.

The game runs on the dummy drivers with --fark and --fixture-window. The
fixture extension shows a window on the main menu. The check lists that
window's controls, clicks its button, and reads its clock:

- the window's button and clock label are among the controls, ahead of the
  main menu's own, and the button's place lies on the canvas;
- a click on the button is taken, the button then reads pressed, and the
  main menu stays up;
- the clock label reads the fixed clock: the answer's frame times one
  thirtieth of a second, the fixed clock's step, both before the click and
  after it.
"""
import argparse
from pathlib import Path
import sys
import tempfile
import native_offline

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_automation as automation  # noqa: E402

# The fixed clock's step, one thirtieth of a second, in milliseconds.
FIXED_STEP_MS = 1000 // 30


def control_named(answer, name):
    """Returns the control of a name whose window is the fixture's, or fails."""
    found = None
    for control in answer.get("controls", []):
        if control.get("name") == name and control.get("window") == "fixture":
            found = control
            break
    if found is None:
        raise automation.AutomationFailure(f"the fixture's {name} is not listed: {answer}")
    return found


def check_clock(answer, control):
    """The clock label reads the fixed step times the answer's frame."""
    frame = answer.get("frame")
    text = control.get("text")
    if not isinstance(frame, int) or text != str(frame * FIXED_STEP_MS):
        raise automation.AutomationFailure(
            f"the clock reads {text!r} at frame {frame}, not the fixed clock"
        )


def check(native, game_dir, workdir):
    """Drives the fixture's window; raises AutomationFailure when it differs."""
    game = automation.Game(native, game_dir, workdir, ["--fixture-window"])
    clients = []
    try:
        endpoint = game.endpoint()
        address, token = endpoint.get("address", ""), endpoint.get("token", "")
        if not address.startswith("127.0.0.1:") or len(token) != 32:
            raise automation.AutomationFailure(f"the endpoint file holds {endpoint}")
        client = automation.Client(address)
        clients.append(client)
        hello = client.request("hello", versions=[1], client="fixture", token=token)
        if not hello.get("ok"):
            raise automation.AutomationFailure(f"hello was answered {hello}")
        automation.wait_screen(client, "main_menu")

        listed = client.request("controls")
        if listed.get("screen") != "main_menu":
            raise automation.AutomationFailure(f"controls named {listed.get('screen')}")
        button = control_named(listed, "press")
        clock = control_named(listed, "clock")
        if button.get("kind") != "button" or button.get("text") != "ready" or \
                not button.get("enabled") or not button.get("visible") or button.get("dialog"):
            raise automation.AutomationFailure(f"the fixture's button is {button}")
        rect = button.get("rect")
        if not isinstance(rect, list) or len(rect) != 4 or rect[2] <= 0 or rect[3] <= 0 or \
                rect[0] < 0 or rect[1] < 0 or rect[0] + rect[2] > 640 or rect[1] + rect[3] > 480:
            raise automation.AutomationFailure(f"the fixture's button lies at {rect}")
        if clock.get("kind") != "label" or clock.get("enabled"):
            raise automation.AutomationFailure(f"the fixture's clock is {clock}")
        check_clock(listed, clock)
        names = [control.get("name") for control in listed.get("controls", [])]
        if names.index("press") > names.index("SINGLE"):
            raise automation.AutomationFailure(f"the fixture's button is listed after SINGLE: {names}")
        menu_button = next(control for control in listed["controls"] if control.get("name") == "SINGLE")
        if menu_button.get("window") is not None:
            raise automation.AutomationFailure(f"SINGLE names a window: {menu_button}")

        click = automation.click_control(client, "press", "main_menu")
        if click.get("consumed", {}).get("tick") != 0:
            raise automation.AutomationFailure(f"the click on press was answered {click}")
        again = client.request("controls")
        if again.get("screen") != "main_menu":
            raise automation.AutomationFailure(
                f"the click left the main menu for {again.get('screen')}"
            )
        pressed = control_named(again, "press")
        if pressed.get("text") != "pressed":
            raise automation.AutomationFailure(f"the click left the button reading {pressed}")
        check_clock(again, control_named(again, "clock"))

        client.request("quit")
        game.wait_end()
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
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
        try:
            check(args.native, args.game_dir, Path(scratch))
        except automation.AutomationFailure as failure:
            print(f"native-automation-extension-windows: {failure}", file=sys.stderr)
            return 1
    print("native-automation-extension-windows: the fixture's window answered as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
