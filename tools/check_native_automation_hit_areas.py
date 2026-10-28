#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that each control of Open Annihilation's own screens answers where it is drawn.

The game runs on the dummy drivers with --fark in a 1920x1080 window, once
in the classic look and once in the modern one, with Use modern fonts for
game text on so that Interface size offers 150%. Settings itself chooses
each Interface size the setting offers (Auto, 100%, 150%, 200%, 300% and
400%) and each Window size (Auto, Compact, Regular and Large; a size class
that does not fit is held to the largest that does) through its drop-downs
and OK, as a player does. At each pair whose Settings lies at a place and a
size no earlier pair gave, every press goes to the centre of the control's
window rectangle, where the control is drawn:

- Settings opens from the main menu's Settings button and Tab shows the
  focus; each section's entry shows its section;
- in each section, from its top and then a few wheel notches at a time to
  its end, each control that shows and takes a press, the footer's and the
  title bar's included, takes the focus where it is pressed, and the press
  is let go away from every control, so that nothing acts;
- each drop-down opens its list, which a press away from it closes; each
  switch's other half and each strip's other level is chosen and the one
  chosen before is chosen again, but for the rows whose change takes effect
  at once on the window, its presentation or the fonts, whose halves and
  levels only take the press;
- Cancel closes Settings;
- the Library opens from the OA button: each tab, clicked, shows, and on
  each tab a few of its filters, clicked, are chosen; it closes;
- the key editor opens from Controls' Edit keys…: each of its sections,
  filters and views, clicked, is chosen; it closes, and Settings with it.
  (Those screens find a press in the very list they are drawn from, so a
  sample of their controls is enough.)

A control that does not answer where it is drawn fails the check, with the
look, the Interface size, the Window size and the control named.
"""
import argparse
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_automation as automation  # noqa: E402

AutomationFailure = automation.AutomationFailure

# The window the game runs in: large enough for every size class at 100%
# and at 150%, Regular and Compact at 200%, and Compact at 300%.
RESOLUTION = "1920x1080"
LOOKS = ("classic", "modern")
# The Interface sizes and Window sizes Settings offers, by the words their
# drop-downs' items are named by.
INTERFACE_SIZES = ("automatic", "100", "150", "200", "300", "400")
WINDOW_SIZES = ("auto", "compact", "regular", "large")
SETTINGS = "oa.settings."
# Settings' buttons that belong to no row: each only takes the press.
FRAME_BUTTONS = frozenset({
    "oa.settings.restore-defaults",
    "oa.settings.cancel",
    "oa.settings.ok",
    "oa.settings.close-x",
    "oa.settings.restore-profile-values",
    "oa.settings.more-in-library",
    "oa.settings.registries-in-library",
})
# Rows whose button has no word of its own, so that it is named by its row
# alone as a drop-down is: each only takes the press.
BUTTON_ROWS = frozenset({"oa.settings.first-run"})
# Rows whose change takes effect at once on the window, how it presents the
# menus, the renderer or the fonts every screen is drawn in: their halves and
# levels take the press and are never chosen.
KEPT_ROWS = frozenset({
    "oa.settings.look",
    "oa.settings.modern-fonts",
    "oa.settings.menu-scaling",
    "oa.settings.native-density",
    "oa.settings.window-frame",
    "oa.settings.hardware-acceleration",
    "oa.settings.vertical-sync",
    "oa.settings.frame-stats",
    "oa.settings.unicode-chat",
    "oa.settings.update-download",
})
# SDL's scancodes and key codes of the keys the check presses.
KEYS = {"Tab": (43, 0x09)}
# The window pixel a press is let go at: the window's corner, on no control.
AWAY = (1, 1)
# The wheel's notches a step through a section, and the most steps.
WHEEL_NOTCHES = 3
MOST_WHEEL_STEPS = 60
# Seconds a click is given to show what it changed.
SETTLE = 10
# The most of the Library's filters chosen on a tab, and of the key
# editor's sections, filters and views each.
LIBRARY_FILTERS = 4
KEY_EDITOR_CHOICES = 30


def key_events(name):
    """A key pressed and let go."""
    scancode, keycode = KEYS[name]
    return [{"kind": kind, "key": name, "scancode": scancode, "keycode": keycode}
            for kind in ("key_down", "key_up")]


def centre(control):
    """The centre of a control's window rectangle, in the window's pixels."""
    x, y, width, height = control["window_rect"]
    return x + width // 2, y + height // 2


def at_window(kind, point, **fields):
    """A pointer event at a window pixel."""
    return {"kind": kind, "x": point[0], "y": point[1], "space": "window", **fields}


# The release of the last press, sent ahead of the next events.
held_release = []


def send(client, events, what):
    """Sends input events, after a held press's release, and waits until the
    game has taken them."""
    events = held_release + events
    held_release.clear()
    answer = client.request("input", events=events)
    if not answer.get("ok"):
        raise AutomationFailure(f"{what} was answered {answer}")
    return answer


def listed(client):
    """The controls answer's controls."""
    return client.request("controls").get("controls", [])


def named(controls, name):
    """The control of a name among controls; None when none is listed."""
    for control in controls:
        if control.get("name") == name:
            return control
    return None


def has_room(control):
    """Tells whether a control's window rectangle has pixels."""
    rect = control.get("window_rect") or (0, 0, 0, 0)
    return rect[2] > 0 and rect[3] > 0


def wait_for(client, what, test):
    """Waits until test(controls) holds and returns the controls."""
    deadline = time.monotonic() + SETTLE
    while True:
        controls = listed(client)
        if test(controls):
            return controls
        if time.monotonic() > deadline:
            raise AutomationFailure(f"{what}; the controls are {names_of(controls)}")
        time.sleep(0.05)


def names_of(controls):
    """The names of the controls that show, for a failure's message."""
    return [control.get("name") for control in controls if control.get("visible")]


def click(client, control):
    """Clicks a control at the centre of its window rectangle."""
    point = centre(control)
    send(client, [at_window("pointer_move", point),
                  at_window("button_down", point, button="left", clicks=1),
                  at_window("button_up", point, button="left", clicks=1)],
         f"the click on {control.get('name')}")


def click_named(client, name):
    """Clicks the control of a name, once it shows."""
    controls = wait_for(client, f"{name} never showed",
                        lambda found: (named(found, name) or {}).get("visible") and
                        has_room(named(found, name)))
    click(client, named(controls, name))


def press_answers(client, control, owner, release_on=False):
    """Presses a control at its drawn centre, checks that its owner took the
    focus there, and lets go: on the control where release_on asks, else away
    from every control, so that nothing acts. Returns a failure's text."""
    point = centre(control)
    send(client, [at_window("pointer_move", point),
                  at_window("button_down", point, button="left", clicks=1)],
         f"the press on {control.get('name')}")
    focused = client.request("screen").get("focused")
    # Let go with the next events.
    if release_on:
        held_release.append(at_window("button_up", point, button="left", clicks=1))
    else:
        held_release.extend([at_window("pointer_move", AWAY),
                             at_window("button_up", AWAY, button="left", clicks=1)])
    if focused != owner:
        return (f"{control.get('name')} pressed at {point} (window {control.get('window_rect')}) "
                f"gave the focus to {focused}, not {owner}")
    return None


def is_part(control, names):
    """Tells whether a control is a part of another: a switch's half, a strip's level."""
    owner = control.get("name", "").rpartition(".")[0]
    return owner in names


def show_focus(client, own_prefix):
    """Shows the focus with Tab unless a control of the screen already has it."""
    if any(control.get("focused") for control in listed(client)
           if control.get("name", "").startswith(own_prefix)):
        return
    send(client, key_events("Tab"), "Tab")


class Walk:
    """One pair of sizes in one look: what failed, for the report."""

    def __init__(self, label):
        self.label = label
        self.failures = []
        self.pressed = 0

    def fail(self, text):
        self.failures.append(f"{self.label}: {text}")
        print(f"hit areas: {self.label}: {text}", flush=True)


def open_settings(client):
    """Opens Settings from the main menu's Settings button."""
    click_named(client, "oa.settings-button")
    wait_for(client, "Settings did not open",
             lambda found: (named(found, "oa.settings.ok") or {}).get("visible"))


def close_settings(client):
    """Closes Settings with Cancel."""
    click_named(client, "oa.settings.cancel")
    wait_for(client, "Settings did not close", lambda found: named(found, "oa.settings.ok") is None)


def show_section(client, entry):
    """Shows a section by its entry, scrolling the list where it hides it."""
    click_named(client, entry)
    wait_for(client, f"{entry} did not show its section",
             lambda found: (named(found, entry) or {}).get("checked"))


def bring_into_view(client, name):
    """Turns the wheel over Settings' section until a control shows."""
    for _ in range(MOST_WHEEL_STEPS):
        controls = listed(client)
        control = named(controls, name)
        if control is not None and control.get("visible") and has_room(control):
            return control
        bar = named(controls, "oa.settings.scroll-bar")
        if bar is None:
            break
        x, y, _, height = bar["window_rect"]
        send(client, [{"kind": "wheel", "dx": 0, "dy": -1, "x": x - 4, "y": y + height // 2,
                       "space": "window"}], "the wheel")
    raise AutomationFailure(f"{name} never came into view")


def choose_item(client, field, item):
    """Opens a drop-down and chooses one of its items."""
    control = bring_into_view(client, field)
    click(client, control)
    wanted = f"{field}.{item}"
    controls = wait_for(client, f"{field} did not open",
                        lambda found: (named(found, wanted) or {}).get("visible"))
    click(client, named(controls, wanted))
    wait_for(client, f"{field} did not close after {wanted}",
             lambda found: named(found, wanted) is None)


def choose_sizes(client, interface_size, window_size):
    """Chooses an Interface size and a Window size in Settings, and keeps them with OK."""
    open_settings(client)
    show_section(client, "oa.settings.nav.user-interface")
    choose_item(client, "oa.settings.interface-size", interface_size)
    choose_item(client, "oa.settings.window-size", window_size)
    click_named(client, "oa.settings.ok")
    wait_for(client, "Settings did not close at OK",
             lambda found: named(found, "oa.settings.ok") is None)


def settings_place(client):
    """Where Settings lies: its OK's and its close mark's window rectangles."""
    controls = listed(client)
    return tuple(tuple((named(controls, name) or {}).get("window_rect") or ())
                 for name in ("oa.settings.ok", "oa.settings.close-x"))


def try_parts(client, walk, control, names):
    """Chooses a switch's other half or a strip's other level at its drawn
    centre and then the one chosen before; a kept row's parts take the press."""
    name = control["name"]
    parts = [found for found in listed(client)
             if found.get("name", "").rpartition(".")[0] == name and found.get("kind") == "check box"]
    if not parts:
        return
    if name in KEPT_ROWS:
        for part in parts:
            if part.get("enabled") and part.get("visible") and has_room(part):
                failure = press_answers(client, part, name)
                walk.pressed += 1
                if failure:
                    walk.fail(failure)
        return
    chosen = [part["name"] for part in parts if part.get("checked")]
    others = [part for part in parts if not part.get("checked") and part.get("enabled") and
              part.get("visible") and has_room(part)]
    if not chosen or not others:
        return
    other = others[0]
    click(client, other)
    walk.pressed += 1
    try:
        wait_for(client, f"{other['name']} clicked at {centre(other)} was not chosen",
                 lambda found: (named(found, other["name"]) or {}).get("checked"))
    except AutomationFailure as failure:
        walk.fail(str(failure))
        return
    back = named(listed(client), chosen[0])
    if back is None or not back.get("visible") or not has_room(back):
        walk.fail(f"{chosen[0]} no longer shows after {other['name']} was chosen")
        return
    click(client, back)
    try:
        wait_for(client, f"{chosen[0]} clicked at {centre(back)} was not chosen again",
                 lambda found: (named(found, chosen[0]) or {}).get("checked"))
    except AutomationFailure as failure:
        walk.fail(str(failure))


def try_drop_down(client, walk, control):
    """Opens a drop-down at its drawn centre and closes it with Escape."""
    name = control["name"]
    click(client, control)
    walk.pressed += 1
    prefix = name + "."
    try:
        wait_for(client, f"{name} clicked at {centre(control)} did not open",
                 lambda found: any(item.get("name", "").startswith(prefix) and item.get("visible")
                                   for item in found))
    except AutomationFailure as failure:
        walk.fail(str(failure))
        return
    # A press away from the list closes it and does nothing more. (Escape
    # would, but Accessibility's Slow keys, which a press on its slider sets,
    # holds back a key let go at once.)
    send(client, [at_window("pointer_move", AWAY),
                  at_window("button_down", AWAY, button="left", clicks=1),
                  at_window("button_up", AWAY, button="left", clicks=1)], "the press away")
    wait_for(client, f"a press away from {name}'s list did not close it",
             lambda found: not any(item.get("name", "").startswith(prefix) for item in found))


def is_drop_down(control):
    """Tells whether a control is a Settings drop-down: a button named by its row alone."""
    name = control.get("name", "")
    return control.get("kind") == "button" and name.startswith(SETTINGS) and \
        "." not in name[len(SETTINGS):] and name not in FRAME_BUTTONS and \
        name not in BUTTON_ROWS


def takes_press(control, names):
    """Tells whether a Settings control is one a press is tried on here."""
    name = control.get("name", "")
    return control.get("dialog") == "oa.settings" and control.get("visible") and \
        control.get("enabled") and has_room(control) and not is_part(control, names) and \
        not name.startswith("oa.settings.nav.") and name != "oa.settings.scroll-bar"


def walk_section(client, walk, frame_done):
    """Presses and clicks each control of the section shown, top to end. A
    press that only takes the focus moves nothing, so the controls listed
    once are pressed in turn until one is clicked, which may move the rows."""
    tried = set()
    steps = 0
    while True:
        controls = listed(client)
        names = {control.get("name") for control in controls}
        pending = [control for control in controls
                   if takes_press(control, names) and control["name"] not in tried and
                   control["name"] not in frame_done]
        for control in pending:
            name = control["name"]
            tried.add(name)
            if name in FRAME_BUTTONS:
                frame_done.add(name)
            failure = press_answers(client, control, name, release_on=control.get("kind") == "slider")
            walk.pressed += 1
            if failure:
                walk.fail(failure)
            elif is_drop_down(control):
                try_drop_down(client, walk, control)
                break
            elif control.get("kind") == "label" and \
                    any(other.get("name", "").startswith(name + ".") for other in controls):
                try_parts(client, walk, control, names)
                break
        if pending:
            continue
        bar = named(controls, "oa.settings.scroll-bar")
        if bar is None or not has_room(bar) or steps >= MOST_WHEEL_STEPS:
            return
        before = [(c.get("name"), tuple(c.get("window_rect") or ())) for c in controls
                  if c.get("visible")]
        x, y, _, height = bar["window_rect"]
        send(client, [{"kind": "wheel", "dx": 0, "dy": -WHEEL_NOTCHES, "x": x - 4,
                       "y": y + height // 2, "space": "window"}], "the wheel")
        steps += 1
        after = [(c.get("name"), tuple(c.get("window_rect") or ())) for c in listed(client)
                 if c.get("visible")]
        if after == before:
            return


def walk_settings(client, walk):
    """Walks every section of Settings, then closes it with Cancel."""
    open_settings(client)
    show_focus(client, SETTINGS)
    entries = [control["name"] for control in listed(client)
               if control.get("name", "").startswith("oa.settings.nav.")]
    if not entries:
        raise AutomationFailure("Settings lists no section's entry")
    frame_done = set()
    for entry in entries:
        control = named(listed(client), entry)
        if control is None or not control.get("visible") or not has_room(control):
            walk.fail(f"{entry} does not show")
            continue
        click(client, control)
        walk.pressed += 1
        try:
            wait_for(client, f"{entry} clicked at {centre(control)} did not show its section",
                     lambda found: (named(found, entry) or {}).get("checked"))
        except AutomationFailure as failure:
            walk.fail(str(failure))
            continue
        show_focus(client, SETTINGS)
        walk_section(client, walk, frame_done)
    close_settings(client)


def click_choices(client, walk, dialog, prefix, most):
    """Clicks, at its drawn centre, each check box of a kit screen under a
    name's prefix that shows unchecked, and finds it chosen: a tab, a filter,
    a section of the list or a view. A switch's halves and a heading over
    sections are left as they are. At most `most` of them."""
    clicked = set()
    while len(clicked) < most:
        controls = listed(client)
        names = {control.get("name") for control in controls}
        waiting = [control for control in controls
                   if control.get("dialog") == dialog and control.get("kind") == "check box" and
                   control.get("name", "").startswith(prefix) and
                   control["name"] not in clicked and not control.get("checked") and
                   ".heading." not in control["name"] and
                   control.get("visible") and control.get("enabled") and has_room(control) and
                   not is_part(control, names)]
        if not waiting:
            return
        control = waiting[0]
        name = control["name"]
        clicked.add(name)
        click(client, control)
        walk.pressed += 1
        try:
            wait_for(client, f"{name} clicked at {centre(control)} was not chosen",
                     lambda found: (named(found, name) or {}).get("checked"))
        except AutomationFailure as failure:
            walk.fail(str(failure))


def walk_library(client, walk):
    """Opens the Library, shows each tab and chooses its filters, and closes it."""
    click_named(client, "oa.button")
    wait_for(client, "the Library did not open",
             lambda found: (named(found, "oa.library.close") or {}).get("visible"))
    # The tabs, not the arrows that scroll the strip where it holds them all.
    tabs = [control["name"] for control in listed(client)
            if control.get("name", "").startswith("oa.library.tab.") and
            control.get("kind") == "check box"]
    for tab in tabs:
        control = named(listed(client), tab)
        if control is None or not control.get("visible") or not has_room(control):
            continue
        click(client, control)
        walk.pressed += 1
        try:
            wait_for(client, f"{tab} clicked at {centre(control)} did not show",
                     lambda found: (named(found, tab) or {}).get("checked"))
        except AutomationFailure as failure:
            walk.fail(str(failure))
            continue
        click_choices(client, walk, "oa.library", "oa.library.filter.", LIBRARY_FILTERS)
    click_named(client, "oa.library.close")
    wait_for(client, "the Library did not close",
             lambda found: named(found, "oa.library.close") is None)


def walk_key_editor(client, walk):
    """Opens the key editor from Controls, chooses its sections, filters and
    views, and closes it and Settings."""
    open_settings(client)
    show_section(client, "oa.settings.nav.controls")
    click(client, bring_into_view(client, "oa.settings.key-layout.edit"))
    wait_for(client, "the key editor did not open",
             lambda found: (named(found, "oa.key-editor.cancel") or {}).get("visible"))
    for prefix in ("oa.key-editor.nav.", "oa.key-editor.filter.", "oa.key-editor.view."):
        click_choices(client, walk, "oa.key-editor", prefix, KEY_EDITOR_CHOICES)
    click_named(client, "oa.key-editor.cancel")
    wait_for(client, "the key editor did not close",
             lambda found: named(found, "oa.key-editor.cancel") is None)
    close_settings(client)


def preferences_for(look):
    """The preferences file a look's run starts with."""
    return ("open-annihilation-preferences 1\n"
            '"open-annihilation.language" "en"\n'
            '"open-annihilation.first-run" "done"\n'
            '"open-annihilation.content-updates" "never"\n'
            '"open-annihilation.update-check" "never"\n'
            '"open-annihilation.modern-fonts" "1"\n'
            f'"open-annihilation.ui.look" "{look}"\n')


def check_look(native, game_dir, workdir, look, interface_sizes=INTERFACE_SIZES,
               window_sizes=WINDOW_SIZES):
    """Walks Settings, the Library and the key editor at each pair of sizes in a look."""
    automation.PREFERENCES = preferences_for(look)
    game = automation.Game(native, game_dir, workdir, ["--resolution", RESOLUTION])
    clients = []
    failures = []
    started = time.monotonic()
    try:
        endpoint = game.endpoint()
        client = automation.Client(endpoint.get("address", ""))
        clients.append(client)
        hello = client.request("hello", versions=[1], client="hit-areas",
                               token=endpoint.get("token", ""))
        if not hello.get("ok"):
            raise AutomationFailure(f"hello was answered {hello}")
        automation.wait_screen(client, "main_menu")
        wait_for(client, "the Settings button never took presses",
                 lambda found: (named(found, "oa.settings-button") or {}).get("enabled"))
        places = {}
        for interface_size in interface_sizes:
            for window_size in window_sizes:
                label = f"{look}, Interface size {interface_size}, Window size {window_size}"
                choose_sizes(client, interface_size, window_size)
                open_settings(client)
                place = settings_place(client)
                close_settings(client)
                if place in places:
                    print(f"hit areas: {label}: Settings lies as at {places[place]}")
                    continue
                places[place] = label
                walk = Walk(label)
                begun = time.monotonic()
                walk_settings(client, walk)
                walk_library(client, walk)
                walk_key_editor(client, walk)
                failures.extend(walk.failures)
                print(f"hit areas: {label}: {walk.pressed} presses, "
                      f"{len(walk.failures)} failed, {time.monotonic() - begun:.0f} s", flush=True)
        client.request("quit")
        game.wait_end()
    except (AutomationFailure, OSError, ValueError) as failure:
        ended = game.process.poll()
        game.stop()
        game.take_output()
        print("\n".join(f"game: {line}" for line in game.lines[-80:]))
        if ended is not None:
            print(f"hit areas: the game had ended with status {ended}")
        raise AutomationFailure(f"{look}: {type(failure).__name__}: {failure}") from failure
    finally:
        for client in clients:
            client.close()
    print(f"hit areas: {look}: {time.monotonic() - started:.0f} s")
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folders are made")
    parser.add_argument("--look", choices=LOOKS, action="append",
                        help="walk only this look; may be given twice")
    parser.add_argument("--interface-size", choices=INTERFACE_SIZES, action="append",
                        help="walk only this Interface size; may be given again")
    parser.add_argument("--window-size", choices=WINDOW_SIZES, action="append",
                        help="walk only this Window size; may be given again")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    failures = []
    for look in args.look or LOOKS:
        with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
            try:
                failures.extend(check_look(args.native, args.game_dir, Path(scratch), look,
                                           args.interface_size or INTERFACE_SIZES,
                                           args.window_size or WINDOW_SIZES))
            except AutomationFailure as failure:
                print(f"native-automation-hit-areas: {failure}", file=sys.stderr)
                return 1
    for failure in failures:
        print(f"native-automation-hit-areas: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("native-automation-hit-areas: every control answered where it is drawn")
    return 0


if __name__ == "__main__":
    sys.exit(main())
