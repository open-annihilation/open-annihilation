#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Next button moved to F7 and to J walks the host form, and Tab does not.

One launch, on the dummy drivers. The layout is played for this run and is
not saved. The host form opens with a click, because no key opens it. The
game name has the keyboard. J, which is also Next button, is typed into
that field and the keyboard stays there. Tab stays there too. F7, Next
button, moves on to the password. The name and the password used to log in
are made up; the replay does not check them, and neither is a real account.
"""
import argparse
from pathlib import Path
import sys
import time

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import check_native_mcp_lobby as lobby  # noqa: E402


LAYOUT = """oakeys: 1
id: a35-next-f7
name: "A35 next on F7"
version: "1.0"
author: {name: The Open Annihilation Authors}
description: "Next button on F7 and on J."
tags: [window]
base: oa-default
bindings:
  ui.menu-focus-next: ["F7", "J"]
"""


def focused_names(client):
    """Names of the visible controls that have the keyboard."""
    names = []
    for leaf in lobby.leaves(lobby.tree_of(client)):
        if leaf.get("visible") and leaf.get("focused"):
            names.append(str(leaf.get("name") or "?"))
    return names


def wait_focus(client, wanted, timeout=15):
    """Polls until that control has the keyboard."""
    deadline = time.monotonic() + timeout
    last = []
    while True:
        last = focused_names(client)
        if wanted in last:
            return
        lobby.require(
            time.monotonic() < deadline,
            f"the keyboard is on {last or 'nothing'}, not {wanted}",
        )
        time.sleep(0.25)


def tap(client, chord):
    """Presses one chord and lets it go."""
    lobby.step(f"pressing {chord}")
    client.call("ui.key", {"chord": chord}, lobby.CALL_TIMEOUT)


def field_text(client, name, window):
    """The text in that field, or an empty string when it has none."""
    leaf = lobby.find_text_field(lobby.tree_of(client), name, window)
    if leaf is None:
        return ""
    return str(leaf.get("text") or "")


def drive(client):
    """Types J into the game name, leaves Tab there, and walks on with F7."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG)
    lobby.step("opening the battle console")
    lobby.click_named(client, "MF_Battle_Button")
    lobby.wait_control(client, "BattleList")
    lobby.step("opening the host form")
    lobby.click_named(client, "HOST")
    lobby.poll_leaf(client, "Battle name", "HOST BATTLE")
    wait_focus(client, "Battle name")
    lobby.step("typing J, which is also Next button, into the game name")
    tap(client, "J")
    deadline = time.monotonic() + 10
    while True:
        text = field_text(client, "Battle name", "HOST BATTLE")
        stayed = "Battle name" in focused_names(client)
        if "j" in text.lower() and stayed:
            break
        lobby.require(
            time.monotonic() < deadline,
            f"J left the game name ({text!r}, keyboard {focused_names(client)})",
        )
        time.sleep(0.25)
    lobby.step("Tab no longer walks")
    tap(client, "Tab")
    time.sleep(0.3)
    lobby.require(
        "Battle name" in focused_names(client),
        f"Tab walked from the game name to {focused_names(client)}",
    )
    lobby.step("F7 walks to the password")
    tap(client, "F7")
    wait_focus(client, "Battle password")
    lobby.step("quitting")
    client.call("game.quit")


def main():
    """Runs the one launch with Next button on F7 and on J."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    keys = root / "A35-keys"
    keys.mkdir(exist_ok=True)
    (keys / "oakeys.yaml").write_text(LAYOUT, encoding="utf-8")
    lobby.run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.replay,
        args.scratch_root,
        drive,
        extra_args=["--keys", str(keys)],
    )
    lobby.PROGRESS["launch"] = "all"
    lobby.step("ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (lobby.Failure, OSError) as error:
        print(f"native-boneyards-keys-rebound: {error}", file=sys.stderr)
        sys.exit(1)
