#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Choose a side and activate matchmaking with keys.

Two launches, both on the dummy drivers. The side-prompt replay logs in as
SideCommander. The prompt's root keeps the keyboard. The first Left lands
on Choose, and the next Left moves to Core and then Arm. Enter selects
Arm, and Right returns to Choose, which Enter presses. That replay does
not answer the profile change, so the prompt stays. Login may still be
clicked. Choosing the side is ui.key only.

The second launch is a Beta Tester. alpha_features_beta.replay has the
dossier and the war map, and matchmaking.replay has the queue replies.
Neither file alone can do both, so they are joined in a scratch replay
named with this ticket and removed when the launch ends. Chat is opened
with a click, and Minimize_Button opens matchmaking, because no key does.
The first Tab lands on Activate, which Enter then presses. The name and
the password are made up; the replays do not check them, and neither is
a real account.
"""
import argparse
from pathlib import Path
import sys
import time

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import check_native_mcp_lobby as lobby  # noqa: E402


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


def drive_side(client):
    """The side prompt, answered with the arrow keys and Enter."""
    lobby.member_login(client, "SideCommander")
    lobby.step("waiting for the side prompt")
    deadline = time.monotonic() + 60
    while True:
        leaf = lobby.find_leaf(lobby.tree_of(client), "Choose")
        if lobby.page_open(leaf):
            break
        lobby.require(time.monotonic() < deadline, "the side prompt did not open")
        time.sleep(0.25)
    tap(client, "Left")
    wait_focus(client, "Choose")
    current = lobby.tree_of(client)
    for name in ("Arm", "Core", "EXIT"):
        lobby.require(
            lobby.find_leaf(current, name) is not None, f"the side prompt has no {name}"
        )
    tap(client, "Left")
    wait_focus(client, "Core")
    tap(client, "Left")
    wait_focus(client, "Arm")
    tap(client, "Enter")
    checked = time.monotonic() + 10
    while True:
        arm = lobby.find_leaf(lobby.tree_of(client), "Arm")
        if arm is not None and arm.get("checked"):
            break
        lobby.require(time.monotonic() < checked, f"Enter did not select Arm: {arm}")
        time.sleep(0.25)
    tap(client, "Right")
    wait_focus(client, "Core")
    tap(client, "Right")
    wait_focus(client, "Choose")
    tap(client, "Enter")
    lobby.step("the prompt stays, because the replay does not answer the side")
    stayed = time.monotonic() + 3
    while time.monotonic() < stayed:
        choose = lobby.find_leaf(lobby.tree_of(client), "Choose")
        lobby.require(lobby.page_open(choose), "Choose closed after the side was sent")
        time.sleep(0.25)
    arm = lobby.find_leaf(lobby.tree_of(client), "Arm")
    lobby.require(arm is not None and arm.get("checked"), f"Arm did not stay selected: {arm}")
    lobby.step("quitting")
    client.call("game.quit")


def join_replay(beta, match, dest):
    """Writes the dossier replay and then the matchmaking replies."""
    text = Path(beta).read_text(encoding="utf-8")
    if not text.endswith("\n"):
        text += "\n"
    text += Path(match).read_text(encoding="utf-8")
    dest.write_text(text, encoding="utf-8")


def drive_match(client):
    """Opens matchmaking from chat and presses Activate with Enter."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG)
    lobby.step("opening the chat console")
    lobby.click_named(client, "MF_Chat_Button")
    lobby.wait_control(client, "ChatList")
    lobby.step("opening matchmaking")
    lobby.click_named(client, "Minimize_Button", "ChatConsole")
    lobby.step("the first Tab lands on Activate")
    deadline = time.monotonic() + 20
    while True:
        activate = lobby.find_leaf(lobby.tree_of(client), "Activate", "Matchmaking")
        if activate is not None and activate.get("visible"):
            break
        lobby.require(time.monotonic() < deadline, "Activate was not shown")
        time.sleep(0.25)
    tap(client, "Tab")
    wait_focus(client, "Activate")
    tap(client, "Enter")
    lobby.step("waiting for the search")
    _tree, deactivate = lobby.poll_leaf(client, "Deactivate", "Matchmaking", timeout=20)
    lobby.require(deactivate.get("visible"), "Deactivate is not shown")
    activate = lobby.find_leaf(lobby.tree_of(client), "Activate", "Matchmaking")
    lobby.require(
        activate is None or not activate.get("visible"),
        "Activate stayed up after Enter",
    )
    lobby.step("quitting")
    client.call("game.quit")


def main():
    """Runs the side prompt, then matchmaking."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--side-replay", required=True)
    parser.add_argument("--beta-replay", required=True)
    parser.add_argument("--match-replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    lobby.run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.side_replay,
        args.scratch_root,
        drive_side,
    )
    scratch = Path(args.scratch_root) / "A05-matchmaking.replay"
    try:
        join_replay(args.beta_replay, args.match_replay, scratch)
        lobby.run_game(
            args.native,
            args.game_dir,
            args.mod_dir,
            scratch,
            args.scratch_root,
            drive_match,
        )
    finally:
        scratch.unlink(missing_ok=True)
    lobby.PROGRESS["launch"] = "all"
    lobby.step("ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (lobby.Failure, OSError) as error:
        print(f"native-boneyards-keys-modal: {error}", file=sys.stderr)
        sys.exit(1)
