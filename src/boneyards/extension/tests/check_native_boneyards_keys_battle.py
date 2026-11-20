#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Reach JOIN, choose a host skill, run FIND BATTLE and search the Locator.

One launch. alpha_features_beta.replay has the dossier, the war map, the
battles and the locate answers. quickplay.replay has the search replies.
The scratch replay is named for this check and removed when the launch ends.

Consoles open with a click, because no key opens them. JOIN is only
focused. Right on the host form's skill row chooses Seasoned and the
keyboard stays in that form. FIND BATTLE is pressed, and the offer is not
accepted, so the replay's launch line is never taken. The Locator's name
is the replay's player, and Return there runs the search. The login name
and the password are made up; those replays do not check them, and neither
is a real account.
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


def showing(client, name, window=None):
    """Whether that control is on screen."""
    leaf = lobby.find_leaf(lobby.tree_of(client), name, window)
    return leaf is not None and bool(leaf.get("visible"))


# One script keeps this many reply and then lines.
MATCH_REPLY_LIMIT = 16


def script_text(path):
    """The replay file, ending in a newline."""
    text = Path(path).read_text(encoding="utf-8")
    if text and not text.endswith("\n"):
        text += "\n"
    return text


def reply_count(text):
    """How many reply and then lines the parser would keep."""
    count = 0
    for line in text.splitlines():
        word = line.split(" ", 1)[0] if line.strip() else ""
        if word in ("reply", "then"):
            count += 1
    return count


def join_replay(beta, quickplay, dest):
    """Writes the dossier and the QuickPlay replies."""
    beta_text = script_text(beta)
    quick_text = script_text(quickplay)
    quick_replies = reply_count(quick_text)
    lobby.require(quick_replies > 0, "quickplay.replay has no reply lines")
    lobby.require(
        quick_replies <= MATCH_REPLY_LIMIT,
        f"quickplay.replay has {quick_replies} reply lines; a script holds {MATCH_REPLY_LIMIT}",
    )
    dest.write_text(beta_text + quick_text, encoding="utf-8")


def reach_join(client):
    """Opens the battle console and tabs to JOIN without pressing it."""
    lobby.step("opening the battle console")
    lobby.click_named(client, "MF_Battle_Button")
    lobby.wait_control(client, "BattleList")
    lobby.wait_rows(
        client,
        "BattleList",
        lambda rows: all(battle in rows for battle in lobby.BETA_BATTLES),
        f"listed {', '.join(lobby.BETA_BATTLES)}",
    )
    tap(client, "F6")
    wait_focus(client, "BattleList")
    lobby.step("choosing a forming battle with the arrows")
    deadline = time.monotonic() + 20
    while not showing(client, "JOIN", "BATTLE INTELLIGENCE"):
        lobby.require(time.monotonic() < deadline, "JOIN did not appear")
        tap(client, "Down")
        time.sleep(0.25)
    lobby.step("tabbing to JOIN")
    for _ in range(16):
        if "JOIN" in focused_names(client):
            return
        tap(client, "Tab")
        time.sleep(0.1)
    lobby.require(False, f"JOIN was not reached; the keyboard is on {focused_names(client)}")


def focused_window(client, name):
    """The window of the focused control of that name."""
    for leaf in lobby.leaves(lobby.tree_of(client)):
        if leaf.get("visible") and leaf.get("focused") and str(leaf.get("name") or "") == name:
            return str(leaf.get("window") or "")
    return ""


def down_to_players(client):
    """Down from the skill row lands on the players row."""
    lobby.step("moving down to the players row")
    tap(client, "Down")
    deadline = time.monotonic() + 10
    while True:
        if focused_window(client, "2") == "HOST BATTLE":
            return
        lobby.require(time.monotonic() < deadline, "Down did not reach the players row")
        time.sleep(0.25)


def choose_seasoned(client):
    """Right from Novice chooses Seasoned, and the keyboard stays in the host form."""
    lobby.step("opening the host form")
    lobby.click_named(client, "HOST")
    lobby.poll_leaf(client, "Novice", "HOST BATTLE")
    lobby.step("tabbing to the skill row")
    for _ in range(16):
        if "Novice" in focused_names(client):
            break
        tap(client, "Tab")
        time.sleep(0.1)
    wait_focus(client, "Novice")
    lobby.step("choosing Seasoned")
    tap(client, "Right")
    deadline = time.monotonic() + 10
    while True:
        seasoned = lobby.find_leaf(lobby.tree_of(client), "Seasoned", "HOST BATTLE")
        chosen = seasoned is not None and bool(seasoned.get("checked"))
        if chosen and focused_window(client, "Seasoned") == "HOST BATTLE":
            return
        lobby.require(time.monotonic() < deadline, "Right did not keep Seasoned in HOST BATTLE")
        time.sleep(0.25)


def run_find_battle(client):
    """Opens QuickPlay and presses FIND BATTLE. The offer is left unaccepted."""
    lobby.step("opening quick play")
    lobby.click_named(client, "MF_Quickplay_Button", "BYMainFrame")
    lobby.poll_leaf(client, "FIND BATTLE", "QuickPlay")
    tap(client, "F6")
    lobby.step("tabbing to FIND BATTLE")
    for _ in range(16):
        if "FIND BATTLE" in focused_names(client):
            break
        tap(client, "Tab")
        time.sleep(0.1)
    wait_focus(client, "FIND BATTLE")
    tap(client, "Enter")
    lobby.step("waiting for the search, and not accepting the offer")
    deadline = time.monotonic() + 20
    while True:
        if showing(client, "CANCEL SEARCH", "QuickPlay") or showing(client, "Yes", "QuickPlay"):
            return
        lobby.require(time.monotonic() < deadline, "FIND BATTLE did not start a search")
        time.sleep(0.25)


def run_locate(client):
    """Opens the Locator and runs the search with Return in the name.

    An empty name sends nothing. The replay's player is typed first, and
    Return, not Search, runs the locate.
    """
    lobby.step("opening the locator")
    lobby.click_named(client, "MF_Locator_Button", "BYMainFrame")
    lobby.poll_leaf(client, "Name", "Locator")
    wait_focus(client, "Name")
    lobby.step("typing the player")
    lobby.type_into(client, "Name", lobby.OTHER_PLAYER, "Locator")
    tap(client, "Enter")
    found = lobby.wait_words(client, "Locator", lobby.OTHER_PLAYER, "the locate did not answer")
    lobby.require(found.get("name") == "rname0", f"the first result's name is {found}")


def drive(client):
    """JOIN, the host skill, FIND BATTLE, then a Locator search, from the keyboard."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG)
    reach_join(client)
    choose_seasoned(client)
    down_to_players(client)
    run_find_battle(client)
    run_locate(client)
    lobby.step("quitting")
    client.call("game.quit")


def main():
    """Runs the one launch."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--beta-replay", required=True)
    parser.add_argument("--quickplay-replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    scratch = Path(args.scratch_root) / "boneyards-keys-battle.replay"
    try:
        join_replay(args.beta_replay, args.quickplay_replay, scratch)
        lobby.run_game(
            args.native,
            args.game_dir,
            args.mod_dir,
            scratch,
            args.scratch_root,
            drive,
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
        print(f"native-boneyards-keys-battle: {error}", file=sys.stderr)
        sys.exit(1)
