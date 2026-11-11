#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A held Left repeats at the player's own rate in a Boneyards console.

One launch, on the dummy drivers, with a repeat delay of 300 ms and a rate
of ten a second (Settings, Accessibility) and the game's own layout. The
host form opens with a click, because no key opens it, and the game name
has the keyboard. Left is pressed in the typed name, held a second and
let go, and X is typed where the caret stopped: the press and the repeats
that fell due in that time each moved it one place. With the system's
repeat, or with repeats handed on twice, the count is outside the range.
The name and the password used to log in are made up; the replay does not
check them, and neither is a real account.
"""
import argparse
from pathlib import Path
import sys
import time

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import check_native_mcp_lobby as lobby  # noqa: E402

# The player's own repeat: the first repeat after this delay, then this many
# a second.
REPEAT_DELAY_MS = 300
REPEAT_RATE = 10
# How long Left is held, in seconds.
HOLD = 1.0
# Fewer characters than the game name takes, so X always fits.
TYPED = "abcdefghiklmnopqrstuvwxy"
# The repeat settings. The lobby's launch already writes both online checks
# (update never, catalogue library), and a key twice in a file is refused.
PREFERENCES = (
    f'"open-annihilation.access.repeat-delay-ms" "{REPEAT_DELAY_MS}"\n'
    f'"open-annihilation.access.repeat-rate" "{REPEAT_RATE}"\n'
)


def steps_between(least_ms, most_ms):
    """The fewest and the most moves a press held that long makes.

    The press moves once, and each repeat that falls due while the key is
    down moves once more. A frame may pass between a repeat falling due and
    the release, so a frame's worth is allowed at each end.
    """
    interval = 1000 // REPEAT_RATE

    def moves(held_ms):
        if held_ms < REPEAT_DELAY_MS:
            return 1
        return 2 + int((held_ms - REPEAT_DELAY_MS) // interval)

    return moves(max(0, least_ms - interval)), moves(most_ms) + 1


def field_text(client):
    """The game name's text, or an empty string."""
    leaf = lobby.find_text_field(lobby.tree_of(client), "Battle name", "HOST BATTLE")
    return "" if leaf is None else str(leaf.get("text") or "")


def wait_text(client, wanted, timeout=10):
    """Polls until the game name holds that text."""
    deadline = time.monotonic() + timeout
    while True:
        text = field_text(client)
        if text == wanted:
            return
        lobby.require(time.monotonic() < deadline, f"the game name is {text!r}, not {wanted!r}")
        time.sleep(0.2)


def drive(client):
    """Types the name, holds Left a second, and reads where X lands."""
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
    lobby.step("typing the game name")
    client.call("ui.type", {"text": TYPED}, lobby.CALL_TIMEOUT)
    wait_text(client, TYPED)
    lobby.step(f"holding Left for {HOLD} s")
    began = time.monotonic()
    client.call("ui.key", {"chord": "Left", "phase": "press"}, lobby.CALL_TIMEOUT)
    time.sleep(HOLD)
    client.call("ui.key", {"chord": "Left", "phase": "release"}, lobby.CALL_TIMEOUT)
    most_ms = (time.monotonic() - began) * 1000
    lobby.step("typing X where the caret stopped")
    client.call("ui.type", {"text": "X"}, lobby.CALL_TIMEOUT)
    deadline = time.monotonic() + 10
    text = field_text(client)
    while "X" not in text:
        lobby.require(time.monotonic() < deadline, f"X never reached the game name ({text!r})")
        time.sleep(0.2)
        text = field_text(client)
    moved = len(TYPED) - text.index("X")
    least, most = steps_between(HOLD * 1000, most_ms)
    lobby.require(
        least <= moved <= most,
        f"Left held {HOLD} s, released after at most {most_ms:.0f} ms, moved the caret "
        f"{moved} places, not {least} to {most}, at a {REPEAT_DELAY_MS} ms delay and "
        f"{REPEAT_RATE} a second ({text!r})",
    )
    lobby.step("quitting")
    client.call("game.quit")


def main():
    """Runs the one launch with the player's own repeat."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    # run_game writes the shared automation preferences; this launch adds
    # the repeat.
    lobby.automation.PREFERENCES = lobby.automation.PREFERENCES + PREFERENCES
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
        print(f"native-boneyards-keys-held: {error}", file=sys.stderr)
        sys.exit(1)
