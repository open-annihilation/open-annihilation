#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Choose planet 1801 on the war map with the keyboard.

One launch, on the dummy drivers, against alpha_planets.replay. Login may
be clicked. The planet is not. Just after login nothing has the keyboard,
so the first F6 lands on the frame and the next on DP Alpha. Two Downs
reach Slag, and Return chooses it. Recon, Chat and Battle light, and the
summary reads as the pointer's pick does. The focus ring stays hidden
until F6. Home then moves to the player's capitol without changing the
summary. Back on the map by F6, at the top magnification, each planet the
arrows reach lies wholly in the viewer, scrolled out from under the frame.
"""
import argparse
from pathlib import Path
import sys
import time

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import check_native_mcp_lobby as lobby  # noqa: E402

SUMMARY = (
    'boneyards login check: the summary reads "Slag", Contested, '
    "Battles: 2, Forming: 1, wins Arm 9 Core 17"
)


def screen_of(client):
    """The screen ui.screen names, including the focused control."""
    return lobby.body_of(client.call("ui.screen", timeout=lobby.CALL_TIMEOUT))


def focused_names(client):
    """Names of the visible controls that have the keyboard."""
    names = []
    for leaf in lobby.leaves(lobby.tree_of(client)):
        if leaf.get("visible") and leaf.get("focused"):
            names.append(str(leaf.get("name") or "?"))
    return names


def wait_focus(client, wanted, timeout=15):
    """Polls until that control has the keyboard."""
    return wait_focus_any(client, (wanted,), timeout)


def wait_focus_any(client, wanted, timeout=15):
    """Polls until one of those controls has the keyboard, and returns its name."""
    deadline = time.monotonic() + timeout
    last = []
    while True:
        last = focused_names(client)
        for name in wanted:
            if name in last:
                return name
        lobby.require(
            time.monotonic() < deadline,
            f"the keyboard is on {last or 'nothing'}, not {' or '.join(wanted)}",
        )
        time.sleep(0.25)


def wait_hidden(client, timeout=15):
    """Polls until the focus ring is not drawn."""
    deadline = time.monotonic() + timeout
    focused = None
    while True:
        body = screen_of(client)
        focused = body.get("focused")
        if not body.get("focus_shown"):
            return
        lobby.require(
            time.monotonic() < deadline,
            f"the ring stayed on {focused!r}",
        )
        time.sleep(0.25)


def wait_ring(client, wanted, shown, timeout=15):
    """Polls until ui.screen names that control and the ring is as asked."""
    deadline = time.monotonic() + timeout
    focused = None
    ring = None
    while True:
        body = screen_of(client)
        focused = body.get("focused")
        ring = bool(body.get("focus_shown"))
        if focused == wanted and ring == shown:
            return
        lobby.require(
            time.monotonic() < deadline,
            f"the screen names {focused!r} with the ring {'shown' if ring else 'hidden'}",
        )
        time.sleep(0.25)


def tap(client, chord):
    """Presses one chord and lets it go."""
    lobby.step(f"pressing {chord}")
    client.call("ui.key", {"chord": chord}, lobby.CALL_TIMEOUT)


def shown(tree, name, window):
    """A visible control of that name with an area, in that window."""
    leaf = lobby.find_leaf(tree, name, window)
    if leaf is None or not leaf.get("visible") or lobby.area_of(leaf) <= 0:
        return None
    return leaf


# The war map's viewer, x, y, width and height, as the tree places it.
VIEWER = (23, 119, 498, 328)
# Arrows from DP Alpha at the top magnification. Several land on a planet that
# is partly under the frame unless the map scrolls it into view.
TOUR = (
    "Down", "Down", "Right", "Up", "Down", "Down", "Left", "Left",
    "Up", "Up", "Right", "Right", "Right", "Down", "Down", "Down",
)


def wholly_shown(leaf):
    """Whether a control's rectangle lies inside the viewer."""
    rect = (leaf or {}).get("rect") or [0, 0, 0, 0]
    x, y, width, height = rect[:4]
    return (
        width > 0
        and height > 0
        and x >= VIEWER[0]
        and y >= VIEWER[1]
        and x + width <= VIEWER[0] + VIEWER[2]
        and y + height <= VIEWER[1] + VIEWER[3]
    )


def tour_top_magnification(client):
    """Arrows at the top magnification, each landing wholly in the viewer."""
    lobby.step("F6 to the frame, then to the map")
    tap(client, "F6")
    tap(client, "F6")
    wait_ring(client, "DP Alpha", True)
    start = lobby.magnification_of(lobby.tree_of(client))
    for _press in range(3):
        tap(client, "Shift+=")
    top = lobby.wait_magnification(client, lambda value: value >= start + 3, "the top")
    previous = "DP Alpha"
    for chord in TOUR:
        tap(client, chord)
        deadline = time.monotonic() + 3
        while True:
            names = focused_names(client)
            if names and names[0] != previous:
                break
            if time.monotonic() >= deadline:
                break
            time.sleep(0.25)
        lobby.require(names, f"{chord} from {previous} left nothing with the keyboard")
        previous = names[0]
        planet = lobby.find_leaf(lobby.tree_of(client), previous, "MapPanel")
        lobby.require(
            wholly_shown(planet),
            f"at magnification {top}, {chord} landed on {previous} at "
            f"{(planet or {}).get('rect')}, not wholly inside the viewer {VIEWER}",
        )
        wait_ring(client, previous, True)


def summary_ready(tree):
    """Whether the planet summary shows Slag's census and the three buttons light."""
    dark = [
        name
        for name in lobby.PLANET_BUTTONS
        if not (lobby.find_leaf(tree, name, "BYMainFrame") or {}).get("enabled")
    ]
    if dark:
        return False
    names = ("Slag", "Contested", "Battles:", "2", "Forming:", "1", "9", "17")
    for name in names:
        if shown(tree, name, "BYMainFrame") is None:
            return False
    planet = lobby.find_leaf(tree, "Slag", "MapPanel")
    value = str((planet or {}).get("value_text") or "")
    return "chosen" in value


def drive(client):
    """Logs in, then chooses Slag with F6, Down, Down and Return."""
    lobby.member_login(client, "oa_check")
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.step("the ring stays hidden until F6")
    wait_hidden(client)
    # Nothing has the keyboard, so the first F6 lands on the frame. A
    # keyboard already on the frame reaches the map in that one press.
    lobby.step("F6 from the fresh map")
    tap(client, "F6")
    landed = wait_focus_any(client, ("DP Alpha", "ExitBYBtn"))
    if landed == "ExitBYBtn":
        wait_ring(client, "ExitBYBtn", True)
        lobby.step("F6 from the frame onto the war map")
        tap(client, "F6")
        wait_focus(client, "DP Alpha")
    wait_ring(client, "DP Alpha", True)
    lobby.step("changing the magnification, then putting it back")
    start = lobby.magnification_of(lobby.tree_of(client))
    tap(client, "Shift+=")
    lobby.wait_magnification(client, lambda value: value > start, f"above {start}")
    # "-" is the key. "Minus" is a key position, not a chord name.
    tap(client, "-")
    lobby.wait_magnification(client, lambda value: value == start, f"back at {start}")
    wait_focus(client, "DP Alpha")
    wait_ring(client, "DP Alpha", True)
    tap(client, "Down")
    wait_focus(client, "Lusch II")
    wait_ring(client, "Lusch II", True)
    tap(client, "Down")
    wait_focus(client, "Slag")
    wait_ring(client, "Slag", True)
    tap(client, "Enter")
    lobby.step("waiting for the summary")
    deadline = time.monotonic() + 20
    while True:
        current = lobby.tree_of(client)
        if summary_ready(current):
            break
        lobby.require(time.monotonic() < deadline, "the summary did not show Slag's census")
        time.sleep(0.25)
    print(SUMMARY, flush=True)
    wait_ring(client, "Slag", False)
    lobby.step("Home moves to the capitol")
    tap(client, "Home")
    wait_focus(client, "Core Prime II")
    lobby.require(
        summary_ready(lobby.tree_of(client)),
        "Home changed the summary",
    )
    tour_top_magnification(client)
    lobby.step("quitting")
    client.call("game.quit")


def main():
    """Chooses planet 1801 by keys alone."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    lobby.run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.replay,
        args.scratch_root,
        drive,
    )
    lobby.PROGRESS["launch"] = "all"
    lobby.step("ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (lobby.Failure, OSError) as error:
        print(f"native-boneyards-keys-war-map: {error}", file=sys.stderr)
        sys.exit(1)
