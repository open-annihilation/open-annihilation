#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Open Help and a page link, then send a chat line, with keys.

Two launches, both on the dummy drivers. Login may still be clicked, and
the planet on the chat launch may still be clicked. Opening Help, opening
a category, following a link, opening Chat and sending the line are ui.key
only. Each launch writes a fresh preferences file in its own temporary
folder, so a second run starts from the same state. The name and the
password are made up; the replays do not check them, and neither is a
real account.
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


def wait_focus(client, wanted, timeout=20):
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
    client.call("ui.key", {"chord": chord}, lobby.CALL_TIMEOUT)


def reach(client, wanted, limit=24):
    """Tabs until that control has the keyboard, starting with F6."""
    lobby.step(f"moving to {wanted}")
    tap(client, "F6")
    for _step in range(limit):
        if wanted in focused_names(client):
            return
        tap(client, "Tab")
    lobby.require(
        wanted in focused_names(client),
        f"Tab did not reach {wanted}; the keyboard is on {focused_names(client) or 'nothing'}",
    )


def selected_row(client, name):
    """The selected row of a list, or an empty string."""
    leaf = lobby.list_leaf(client, name)
    items = [str(item) for item in leaf.get("items") or []]
    selected = leaf.get("selected")
    if isinstance(selected, int) and 0 <= selected < len(items):
        return items[selected]
    return ""


def move_until(client, name, wanted, key, described):
    """Presses a key until the selected row is as wanted, and returns it."""
    for _step in range(40):
        row = selected_row(client, name)
        if wanted(row):
            return row
        tap(client, key)
    lobby.require(
        False,
        f"{name} did not {described}; it shows {lobby.list_rows(client, name)}",
    )


def drive_help(client):
    """Opens a Help category and follows a page link with keys."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    reach(client, "MF_Help_Button")
    lobby.step("opening Help")
    tap(client, "Enter")
    lobby.wait_control(client, "ContentsList")
    lobby.step("landing on the contents list")
    tap(client, "F6")
    wait_focus(client, "ContentsList")
    rows = lobby.wait_rows(
        client,
        "ContentsList",
        lambda found: any(row.startswith("+ ") for row in found),
        "listed a collapsed category",
    )
    category = move_until(
        client,
        "ContentsList",
        lambda row: row.startswith("+ "),
        "Down",
        "reach a collapsed category",
    )
    title = category[2:]
    lobby.step(f"opening {title}")
    tap(client, "Right")
    lobby.wait_rows(
        client,
        "ContentsList",
        lambda found: f"- {title}" in found and len(found) > len(rows),
        f"opened {title}",
    )
    lobby.require(
        selected_row(client, "ContentsList") == f"- {title}",
        f"the open category is not selected: {selected_row(client, 'ContentsList')!r}",
    )
    lobby.step(f"closing {title}")
    tap(client, "Left")
    lobby.wait_rows(
        client,
        "ContentsList",
        lambda found: category in found and len(found) == len(rows),
        f"closed {title}",
    )
    # The closed category is still selected. Open it again and walk its
    # headlines, then later rows, until Return follows a link. Getting
    # Started's pages have none.
    lobby.step("opening a page and following a link")
    tap(client, "Right")
    lobby.wait_rows(
        client,
        "ContentsList",
        lambda found: f"- {title}" in found and len(found) > len(rows),
        f"opened {title} again",
    )
    followed = False
    for _attempt in range(40):
        row = selected_row(client, "ContentsList")
        if row.startswith("+ "):
            tap(client, "Right")
            row = selected_row(client, "ContentsList")
        if row and not row.startswith("+ ") and not row.startswith("- "):
            tap(client, "Enter")
            for _step in range(40):
                names = focused_names(client)
                link = next((name for name in names if name.startswith("Link")), "")
                if link:
                    leaf = lobby.find_leaf(lobby.tree_of(client), link)
                    label = str(leaf.get("label") or "") if leaf else ""
                    lobby.require(
                        bool(label) and not label.startswith("Link"),
                        f"{link} announces {label!r}",
                    )
                    tap(client, "Enter")
                    deadline = time.monotonic() + 15
                    while True:
                        gone = link not in focused_names(client)
                        leaf = lobby.find_leaf(lobby.tree_of(client), link)
                        if gone and (leaf is None or not leaf.get("focused")):
                            followed = True
                            break
                        lobby.require(
                            time.monotonic() < deadline,
                            f"Return left the keyboard on {link}",
                        )
                        time.sleep(0.25)
                    break
                tap(client, "Tab")
            if followed:
                break
            for _back in range(40):
                if "ContentsList" in focused_names(client):
                    break
                tap(client, "Tab")
            lobby.require(
                "ContentsList" in focused_names(client),
                "Tab did not return to the contents list; the keyboard is on "
                + ", ".join(focused_names(client)),
            )
        tap(client, "Down")
    lobby.require(followed, "Return did not follow a page link")
    lobby.step("quitting")
    client.call("game.quit")


def type_line(client, text):
    """Types a line into the focused field and submits it."""
    lobby.step(f"typing {text}")
    for character in text:
        tap(client, character)
    tap(client, "Enter")


def drive_chat(client):
    """Opens Chat and sends a line with keys."""
    lobby.member_login(client, "oa_check")
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG)
    reach(client, "MF_Chat_Button")
    lobby.step("opening Chat")
    tap(client, "Enter")
    wait_focus(client, "Chat entry")
    # A Space chord presses the key and types no character (the US map
    # prints nothing for it), so the line uses a hyphen, which does.
    type_line(client, "hello-from-keys")
    lobby.wait_rows(
        client,
        "ChatList",
        lambda rows: any(row.endswith("hello-from-keys") for row in rows),
        "showed the line sent",
    )
    lobby.step("quitting")
    client.call("game.quit")


def main():
    """Runs Help, then Chat."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--contents-replay", required=True)
    parser.add_argument("--chat-replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    lobby.run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.contents_replay,
        args.scratch_root,
        drive_help,
    )
    lobby.run_game(
        args.native,
        args.game_dir,
        args.mod_dir,
        args.chat_replay,
        args.scratch_root,
        drive_chat,
    )
    lobby.PROGRESS["launch"] = "all"
    lobby.step("ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (lobby.Failure, OSError) as error:
        print(f"native-boneyards-keys-contents: {error}", file=sys.stderr)
        sys.exit(1)
