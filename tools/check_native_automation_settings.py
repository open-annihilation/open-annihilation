#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the endpoint drives Settings by its controls' names.

The game runs on the dummy drivers with --fark, once in a 640x480 window
and once in a 1920x1080 one, where the menus are drawn larger. In each:

- controls lists the OA button as oa.button, with no dialog and enabled, and
  a click at its rectangle's centre opens the Library, which offers no
  SETTINGS… (oa.library.settings) and closes by oa.library.close;
- controls lists the Settings button beside it as oa.settings-button, a
  button with no dialog, enabled, its text "Settings", and a click at its
  centre opens Settings;
- while Settings shows, screen's dialogs holds oa.settings, the main menu's
  SINGLE is listed disabled and after every oa. control, and no two names
  are the same ignoring case;
- a click on oa.settings.nav.controls shows Controls, one on
  oa.settings.wheel-zoom.off checks that half, one on
  oa.settings.max-zoom-out opens its drop-down, and one on its
  oa.settings.max-zoom-out.whole-map item chooses it;
- oa.settings.ok keeps them: prefs then holds wheel zoom 0 and the
  maximum zoom out whole-map;
- once Settings closes, the main menu shows with no Library;
- opened again by oa.settings-button, Settings shows wheel zoom off; a click
  on its On half and then oa.settings.cancel keeps the value stored, and
  goes back to the main menu with no Library, since none was under it;
- every oa. control's window rectangle lies in the window hello reports.
"""
import argparse
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_automation as automation  # noqa: E402

# The window sizes the check runs the game at.
RESOLUTIONS = ("640x480", "1920x1080")
WHEEL_ZOOM = "open-annihilation.wheel-zoom"
MAX_ZOOM_OUT = "open-annihilation.max-zoom-out"


def listed(client):
    """Returns the controls answer's list of controls."""
    return client.request("controls").get("controls", [])


def wait_control(client, name, wanted=lambda control: True):
    """Waits for a control of a name that is as wanted, and returns it."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        controls = listed(client)
        for control in controls:
            if control.get("name") == name and wanted(control):
                return control
        if time.monotonic() > deadline:
            raise automation.AutomationFailure(f"{name} never came as wanted: {controls}")
        time.sleep(0.1)


def wait_gone(client, name):
    """Waits until no control of a name is listed."""
    deadline = time.monotonic() + automation.TIMEOUT
    while any(control.get("name") == name for control in listed(client)):
        if time.monotonic() > deadline:
            raise automation.AutomationFailure(f"{name} is still listed")
        time.sleep(0.1)


def click(client, name):
    """Clicks a control at its rectangle's centre on the main menu."""
    automation.click_control(client, name, "main_menu")


def click_in_view(client, name):
    """Scrolls Settings' section until a control is in view, then clicks it.

    The section scrolls as one where its rows do not fit (at 640x480 the key
    layout card leaves Maximum zoom out below the fold); the wheel over the
    control's column brings it up a step at a time.
    """
    for _ in range(40):
        control = wait_control(client, name)
        if control.get("visible"):
            break
        x, _, width, _ = control["rect"]
        _, top, _, height = wait_control(client, "oa.settings.scroll-bar")["rect"]
        answer = client.request("input", expect_screen="main_menu",
                                events=[{"kind": "wheel", "dx": 0, "dy": -1,
                                         "x": x + width // 2, "y": top + height // 2}])
        if not answer.get("ok"):
            raise automation.AutomationFailure(f"the wheel over Settings was answered {answer}")
        time.sleep(0.1)
    else:
        raise automation.AutomationFailure(f"{name} never came into view")
    click(client, name)


def wait_preference(client, name, value):
    """Waits for a preference to hold a value."""
    deadline = time.monotonic() + automation.TIMEOUT
    while True:
        values = client.request("prefs", names=[name]).get("values", {})
        if values.get(name) == value:
            return
        if time.monotonic() > deadline:
            raise automation.AutomationFailure(f"{name} holds {values.get(name)!r}, not {value!r}")
        time.sleep(0.1)


def check_listing(controls, window):
    """Settings' listing: OA's controls first, SINGLE disabled, names unique ignoring case."""
    names = [control.get("name", "") for control in controls]
    folded = [name.lower() for name in names]
    doubled = sorted({name for name in folded if folded.count(name) > 1})
    if doubled:
        raise automation.AutomationFailure(f"names listed twice ignoring case: {doubled}")
    single = [index for index, name in enumerate(names) if name == "SINGLE"]
    if not single or controls[single[0]].get("enabled"):
        raise automation.AutomationFailure(f"SINGLE is not listed disabled under Settings: {controls}")
    last_own = max(index for index, name in enumerate(names) if name.startswith("oa."))
    if last_own > single[0]:
        raise automation.AutomationFailure(f"an oa. control is listed after SINGLE: {names}")
    width, height = window.get("width", 0), window.get("height", 0)
    for control in controls:
        if not control.get("name", "").startswith("oa."):
            continue
        x, y, rect_width, rect_height = control.get("window_rect") or (0, 0, -1, -1)
        # A control scrolled out of its section's view may have no pixels.
        empty = rect_width <= 0 or rect_height <= 0
        if rect_width < 0 or rect_height < 0 or (empty and control.get("visible")) or \
                x < 0 or y < 0 or x + rect_width > width or y + rect_height > height:
            raise automation.AutomationFailure(
                f"{control.get('name')} lies at {control.get('window_rect')}, "
                f"outside the {width}x{height} window"
            )


def open_settings(client):
    """Opens the Library from the OA button and closes it, then opens Settings from its button.

    The Library offers no SETTINGS… since 0.8.0's M22: the main menu's
    Settings button opens Settings.
    """
    button = wait_control(client, "oa.button", lambda control: control.get("enabled"))
    if button.get("dialog") is not None or button.get("kind") != "button":
        raise automation.AutomationFailure(f"the OA button is listed as {button}")
    click(client, "oa.button")
    wait_control(client, "oa.library.close")
    offered = [control for control in listed(client)
               if control.get("name") == "oa.library.settings" and control.get("visible")]
    if offered:
        raise automation.AutomationFailure(f"the Library still offers SETTINGS…: {offered}")
    close_library(client)
    open_settings_directly(client)


def open_settings_directly(client):
    """Clicks the Settings button, and waits for Settings' OK."""
    button = wait_control(client, "oa.settings-button", lambda control: control.get("enabled"))
    if button.get("dialog") is not None or button.get("kind") != "button" or \
            button.get("text") != "Settings":
        raise automation.AutomationFailure(f"the Settings button is listed as {button}")
    click(client, "oa.settings-button")
    wait_control(client, "oa.settings.ok")


def back_on_main_menu(client):
    """Waits for the main menu's buttons to take presses again, with no Library shown."""
    wait_control(client, "oa.settings-button", lambda control: control.get("enabled"))
    library = [control.get("name") for control in listed(client)
               if control.get("name", "").startswith("oa.library.")]
    if library:
        raise automation.AutomationFailure(f"the Library shows on the main menu: {library}")


def close_library(client):
    """Closes the Library, back to the main menu."""
    click(client, "oa.library.close")
    wait_gone(client, "oa.library.close")


def show_controls(client):
    """Shows Settings' Controls section."""
    click(client, "oa.settings.nav.controls")
    wait_control(client, "oa.settings.nav.controls", lambda control: control.get("checked"))


def check_at(native, game_dir, workdir, resolution):
    """Drives Settings in a window of one size; raises AutomationFailure when it differs."""
    game = automation.Game(native, game_dir, workdir, ["--resolution", resolution])
    clients = []
    try:
        endpoint = game.endpoint()
        address, token = endpoint.get("address", ""), endpoint.get("token", "")
        if not address.startswith("127.0.0.1:") or len(token) != 32:
            raise automation.AutomationFailure(f"the endpoint file holds {endpoint}")
        client = automation.Client(address)
        clients.append(client)
        hello = client.request("hello", versions=[1], client="settings", token=token)
        if not hello.get("ok"):
            raise automation.AutomationFailure(f"hello was answered {hello}")
        window = hello.get("window", {})
        automation.wait_screen(client, "main_menu")

        open_settings(client)
        screen = client.request("screen")
        if "oa.settings" not in screen.get("dialogs", []):
            raise automation.AutomationFailure(f"Settings is not among the dialogs: {screen}")
        check_listing(listed(client), window)

        show_controls(client)
        click(client, "oa.settings.wheel-zoom.off")
        wait_control(client, "oa.settings.wheel-zoom.off", lambda control: control.get("checked"))
        click_in_view(client, "oa.settings.max-zoom-out")
        wait_control(client, "oa.settings.max-zoom-out.whole-map")
        click(client, "oa.settings.max-zoom-out.whole-map")
        wait_gone(client, "oa.settings.max-zoom-out.whole-map")
        click(client, "oa.settings.ok")
        wait_gone(client, "oa.settings.ok")
        wait_preference(client, WHEEL_ZOOM, "0")
        wait_preference(client, MAX_ZOOM_OUT, "whole-map")
        back_on_main_menu(client)

        open_settings_directly(client)
        if not any(control.get("name") == "oa.settings.wheel-zoom.off" for control in listed(client)):
            show_controls(client)
        check_listing(listed(client), window)
        wait_control(client, "oa.settings.wheel-zoom.off", lambda control: control.get("checked"))
        click(client, "oa.settings.wheel-zoom.on")
        wait_control(client, "oa.settings.wheel-zoom.on", lambda control: control.get("checked"))
        click(client, "oa.settings.cancel")
        wait_gone(client, "oa.settings.ok")
        wait_preference(client, WHEEL_ZOOM, "0")
        # Settings opened with no Library under it, so it closes to the main menu.
        back_on_main_menu(client)

        client.request("quit")
        game.wait_end()
    except (automation.AutomationFailure, OSError, ValueError) as failure:
        game.stop()
        game.take_output()
        print("\n".join(f"game: {line}" for line in game.lines))
        if isinstance(failure, automation.AutomationFailure):
            raise automation.AutomationFailure(f"{resolution}: {failure}") from failure
        raise automation.AutomationFailure(
            f"{resolution}: {type(failure).__name__}: {failure}"
        ) from failure
    finally:
        for client in clients:
            client.close()
    print(f"automation settings check: {resolution}: passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folders are made")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    for resolution in RESOLUTIONS:
        with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
            try:
                check_at(args.native, args.game_dir, Path(scratch), resolution)
            except automation.AutomationFailure as failure:
                print(f"native-automation-settings: {failure}", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
