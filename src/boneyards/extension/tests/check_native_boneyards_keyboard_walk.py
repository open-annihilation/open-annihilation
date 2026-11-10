#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Tab and F6 through the lobby, and fail when the keyboard walk breaks.

Six launches, each from its own preferences file, user folder and data
folder. The update check is never. The catalogue is read only when the
Library opens, which this walk never does. "-y" stops only when both are
never. alpha_chat.replay walks the login console, the main frame, the war
map, chat, the player pane and Recon. side_prompt.replay walks the side
prompt. alpha_contents.replay walks News, Ladders, Help's list and one
help page. alpha_features_beta.replay walks Battle, the host form, Profile
and the Locator. quickplay.replay and matchmaking.replay are reply
fragments, so each is joined to the beta dossier in a scratch file named
for this check and removed when that launch ends.

Login may be clicked after its round is walked. JOIN, START, FIND BATTLE
and Activate are not pressed, and Return is not pressed in the Locator's
name. The ladders search is not run, so its message box stays closed.
Nothing is dialled. The name and the password are made up; the replays do
not check them.

Before the first key on a screen the ring is hidden, except on a help page
already open on a link. Tab walks until the focused name returns to the
first stop Tab reached. A press that leaves nothing focused fails, a stop
with no label fails, and a repeated stop before the wrap fails. The label
and the window are those of the tree leaf that has the keyboard, not of a
caption that shares its name. Each stop's window is one that station owns.
While a modal dialog is up, the focused control stays in that dialog.
Opening a console leaves the keyboard on the button that opened it, so F6
enters that console before Tab. On the side prompt and on matchmaking, F6
stays inside.
"""
import argparse
import os
from pathlib import Path
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
import time
_OA_TOOLS = Path(__file__).resolve().parents[4] / "tools"
if str(_OA_TOOLS) not in sys.path:
    sys.path.insert(0, str(_OA_TOOLS))
import native_offline  # noqa: E402

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import check_native_mcp_lobby as lobby  # noqa: E402

MATCH_REPLY_LIMIT = 16
PLANET_LIMIT = 500
WALK_LIMIT = 40
CLEAN_PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"multiplayer|tcpaddr" "192.0.2.7"\n'
    '"open-annihilation.access.always-show-focus" "0"\n'
    '"open-annihilation.first-run" "done"\n'
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "library"\n'
)
# LOGIN follows Password only once a password is typed. This walk runs first.
LOGIN = [
    "Password", "REGISTER", "CANCEL", "Server List", "Status", "Registered User", "Username",
]
CHAT = ["Chat rooms", "CREATE", "Chat players", "ChatList", "Chat entry"]
PANE = CHAT + ["GeneralTabBtn", "Whisper Lock", "Muzzle", "PAGE", "HTMLSlider"]
SIDE = ["Choose", "EXIT", "Arm", "Core"]
# START follows Ends only while the form can start. This walk does not fill it.
HOST = [
    "Battle name", "Battle password", "Game types", "Novice", "2", "Ends",
    "BattleList", "HOST", "PlanetIntelList",
]
BATTLE = ["HOST", "PlanetIntelList", "JOIN", "BattleList"]
PROFILE = ["CHANGE", "ProfileList"]
LOCATOR = ["Search", "row_0", "row_1", "row_2", "row_3", "row_4", "Name"]
QUICKPLAY = ["Any planet", "Seasoned", "Either", "Favorite", "FIND BATTLE", "Pick0"]
MATCHMAKING = ["Activate", "Close", "Duel", "Near mine", "Core", "This planet"]
LADDERS = ["ContentsList", "Ladder search", "SEARCH"]
# The page Getting Started's neighbour opens. The walk starts on its first link.
HELP_PAGE = [
    "Link2", "Link3", "Link4", "Link5", "Link6", "Link7", "Link8", "Link9",
    "ContentsList", "Link1",
]
# Release stays disabled until the map is held. Recon, Chat and Battle light
# once a planet is chosen, so the frame is walked after that.
FRAME = [
    "MF_Quickplay_Button", "MF_Recon_Button", "MF_Chat_Button", "MF_Battle_Button",
    "MF_News_Button", "MF_Ladders_Button", "MF_Locator_Button", "MF_Profile_Button",
    "MF_Help_Button", "Mag_Slider", "Map up", "Map up right", "Map right",
    "Map down right", "Map down", "Map down left", "Map left", "Map up left", "ExitBYBtn",
]
# F6 lands on DP Alpha. The next Tab is Core Prime II, and the round ends on DP Alpha.
PLANETS = [
    "Core Prime II", "DP Beta", "Altair", "Deadman Alley", "Slag", "New Hub", "Fuel Dump",
    "Respite", "Abomination", "Cryslon", "Koth", "Lusch II", "Draconis", "Dune", "WTG-01",
    "Forlorn Hope", "Safe Haven", "Spire", "Mined", "Home", "Last Waypoint", "Lost Cause",
    "New Empyrrean", "WDC-02", "DP Alpha",
]
FRAME_WINDOW = "BYMainFrame"
MAP_WINDOW = "MapPanel"
LOGIN_WINDOWS = {"Login"}
FRAME_WINDOWS = {"BYMainFrame"}
MAP_WINDOWS = {"MapPanel"}
CHAT_WINDOWS = {"ChatConsole"}
PANE_WINDOWS = {"ChatConsole", "HTMLView"}
RECON_WINDOWS = {"ReconConsole"}
NEWS_WINDOWS = {"NewsConsole"}
LADDER_WINDOWS = {"LadderConsole"}
HELP_WINDOWS = {"HelpConsole"}
HELP_PAGE_WINDOWS = {"HelpConsole", "HTMLView"}
SIDE_WINDOWS = {"SidePrompt"}
BATTLE_WINDOWS = {"BattleConsole", "BATTLE INTELLIGENCE"}
# The host form covers JOIN's pane. Its round goes on to the battle console.
HOST_WINDOWS = {"HOST BATTLE", "BattleConsole"}
PROFILE_WINDOWS = {"ProfileConsole"}
LOCATOR_WINDOWS = {"Locator"}
QUICK_WINDOWS = {"QuickPlay"}
MATCH_WINDOWS = {"Matchmaking"}


def screen_of(client):
    """What ui.screen reports."""
    return lobby.body_of(client.call("ui.screen", timeout=lobby.CALL_TIMEOUT))


def shown(order):
    """The walked names, for a failure."""
    return ", ".join(order)


def cyclic_equal(got, expected):
    """True when got is a rotation of expected. The names are compared as written."""
    if len(got) != len(expected) or not got:
        return False
    for shift in range(len(expected)):
        if expected[shift:] + expected[:shift] == list(got):
            return True
    return False


def expect_cycle(title, order, expected):
    """The walk is that one round, starting at any of its stops."""
    lobby.require(cyclic_equal(order, expected), f"ORDER {title}: {shown(order)}")


def without(order, name):
    """The round with that one stop taken out."""
    return [item for item in order if item != name]


def expect_after(title, order, name, previous):
    """That stop sits immediately after previous, around the round."""
    index = order.index(previous)
    turned = order[index:] + order[:index]
    lobby.require(len(turned) > 1 and turned[1] == name, f"ORDER {title}: {shown(order)}")


def expect_optional(title, order, cycle, name, previous):
    """name is absent, or it appears once and immediately after previous."""
    if name in order:
        lobby.require(order.count(name) == 1, f"ORDER {title}: {shown(order)}")
        expect_cycle(title, without(order, name), cycle)
        expect_after(title, order, name, previous)
        return
    expect_cycle(title, order, cycle)


def expect_windows(title, seen, allowed):
    """Every stop's window is one this station owns."""
    outside = [name for name in seen if name not in allowed]
    lobby.require(not outside, f"{title}: {shown(outside)} is outside {shown(sorted(allowed))}")


def covering_dialog(body, tree):
    """The modal dialog that takes the keys, or None when nothing covers the screen.

    ui.screen names every dialog, and a button drawn over the screen is one of
    them. Only a modal dialog keeps the focus inside itself.
    """
    modal = []
    for leaf in lobby.leaves(tree):
        if leaf.get("hidden") or not leaf.get("modal"):
            continue
        dialog = leaf.get("dialog")
        if dialog and dialog not in modal:
            modal.append(dialog)
    if not modal:
        return None
    for dialog in body.get("dialogs") or []:
        if dialog in modal:
            return dialog
    return modal[0]


def focused_leaf(tree, name):
    """The leaf that has the keyboard, among those of that name.

    A field and its caption, or a radio and its caption, can share a name, as
    the login console's Username and Password and the host form's 2 do. The
    label and the window checked are the focused control's, whatever order the
    tree lists them in.
    """
    for leaf in lobby.leaves(tree):
        if leaf.get("name") == name and leaf.get("focused") and not leaf.get("hidden"):
            return leaf
    for leaf in lobby.leaves(tree):
        if leaf.get("name") == name and leaf.get("focused"):
            return leaf
    return None


def walk(client, title, windows, limit=WALK_LIMIT, check_ring=True):
    """Tabs until the order wraps. Each stop's window is one this station owns."""
    lobby.step(f"walking {title}")
    if check_ring:
        opened = screen_of(client)
        lobby.require(
            not opened.get("focus_shown"),
            f"{title}: the ring shows before a key ({opened.get('focused')!r})",
        )
    order = []
    seen = []
    first = None
    for _step in range(limit):
        client.call("ui.key", {"chord": "Tab", "phase": "tap"}, lobby.CALL_TIMEOUT)
        body = screen_of(client)
        name = body.get("focused")
        lobby.require(name, f"{title}: Tab left no focus after {shown(order)}")
        if first is None:
            first = name
        elif name == first:
            break
        else:
            lobby.require(name not in order, f"{title}: {name} repeats before the order wraps")
        order.append(name)
        tree = lobby.tree_of(client)
        leaf = focused_leaf(tree, name)
        lobby.require(leaf is not None, f"{title}: no {name} in the tree has the keyboard")
        label = str(leaf.get("label") or "").strip()
        lobby.require(label, f"{title}: {name} has no label")
        window = leaf.get("window")
        lobby.require(window, f"{title}: {name} has no window")
        if window not in seen:
            seen.append(window)
        cover = covering_dialog(body, tree)
        if cover is not None:
            lobby.require(
                leaf.get("dialog") == cover,
                f"{title}: {name} is in {leaf.get('dialog')!r}, not the top dialog {cover!r}",
            )
    else:
        raise lobby.Failure(f"{title}: Tab did not wrap after {limit}; ORDER {shown(order)}")
    lobby.require(order, f"{title}: Tab walked no stop")
    print(f"ORDER {title}: {shown(order)}", flush=True)
    print(f"WINDOWS {title}: {shown(seen)}", flush=True)
    expect_windows(title, seen, windows)
    return order


def tap(client, chord):
    """Presses one chord and lets it go."""
    client.call("ui.key", {"chord": chord, "phase": "tap"}, lobby.CALL_TIMEOUT)


def focused_name(client):
    """The control ui.screen names."""
    return screen_of(client).get("focused")


def wait_ring_hidden(client, where):
    """Polls until the ring is hidden."""
    deadline = time.monotonic() + 20
    focused = None
    while True:
        body = screen_of(client)
        focused = body.get("focused")
        if not body.get("focus_shown"):
            return body
        lobby.require(time.monotonic() < deadline, f"{where}: the ring stayed on {focused!r}")
        time.sleep(0.25)


def expect_login(order):
    """Password, Register, Cancel, the server list, Status, Registered User and the name.

    LOGIN follows Password only once a password is typed. This walk runs
    before that, so LOGIN is not a stop.
    """
    expect_optional("login", order, LOGIN, "LOGIN", "Password")


def expect_chat(order):
    """Rooms, CREATE, players, the log and the entry."""
    expect_cycle("chat", order, CHAT)


def expect_pane(order):
    """The chat round, then General, Whisper Lock, Muzzle, Page and the page bar."""
    expect_cycle("player pane", order, PANE)


def expect_side(order):
    """The first Tab lands on Choose, then Exit, Arm and Core."""
    lobby.require(order == SIDE, f"ORDER side prompt: {shown(order)}")


def expect_host(order):
    """The name, the password, the game type, the three choices, the list, HOST and the planets.

    START follows Ends only while the form can start. This walk does not
    fill the form, so START stays disabled and is left out.
    """
    expect_optional("host", order, HOST, "START", "Ends")


def expect_battle(order):
    """HOST, the planet log, JOIN and the battle list.

    This replay's battle has no password, so Join password stays hidden.
    """
    expect_cycle("battle", order, BATTLE)


def expect_profile(order):
    """CHANGE, then the categories.

    A clean profile leaves APPLY and RESET disabled, and a disabled face is
    not a stop. This walk does not change the profile.
    """
    expect_cycle("profile", order, PROFILE)


def expect_locator(order):
    """Search, the five find rows and Name. Return is not pressed."""
    expect_cycle("locator", order, LOCATOR)


def expect_quickplay(order):
    """Where, skill, side, the favourite, FIND BATTLE and the chosen game."""
    expect_cycle("quickplay", order, QUICKPLAY)


def expect_matchmaking(order):
    """Activate, Close, and one stop for the game, the place, the side and the planet."""
    expect_cycle("matchmaking", order, MATCHMAKING)


def expect_contents(order, title):
    """News or Help's list is that one list."""
    expect_cycle(title, order, ["ContentsList"])


def expect_recon(order):
    """The description list, and nothing else."""
    expect_cycle("recon", order, ["ReconList"])


def expect_ladders(order):
    """The contents list, the name and SEARCH.

    SEARCH wears its disabled face while the field is empty, and a disabled
    face is not a stop. The walk types one letter and does not press SEARCH,
    so the message box for a name that is not listed stays closed.
    """
    expect_cycle("ladders", order, LADDERS)


def expect_help_page(order):
    """The open page's links, then the contents list."""
    expect_cycle("help page", order, HELP_PAGE)


def expect_frame(order):
    """QuickPlay, the eight lit consoles, zoom, the eight map arrows and Exit.

    Release stays disabled until the map is held, so it is not a stop.
    Recon, Chat and Battle are stops only once a planet is chosen.
    """
    expect_cycle("frame", order, FRAME)


def expect_planets(order):
    """The twenty-five planets, from the stop after DP Alpha back round to it."""
    expect_cycle("war map", order, PLANETS)


def f6_stays(client, allowed, title):
    """F6 keeps the keyboard on one of those stops."""
    lobby.step(f"F6 inside {title}")
    tap(client, "F6")
    name = focused_name(client)
    lobby.require(name in allowed, f"F6 left {title} for {name!r}")


def walk_planets(client):
    """F6 reaches the map, and Tab walks the planets once."""
    wait_ring_hidden(client, "the war map")
    lobby.step("F6 to the frame, then to the map")
    tap(client, "F6")
    frame = screen_of(client)
    lobby.require(
        frame.get("focused") and frame.get("focus_shown"),
        f"F6 left {frame.get('focused')!r}",
    )
    tap(client, "F6")
    planet = screen_of(client)
    lobby.require(
        planet.get("focused") and planet.get("focused") != frame.get("focused"),
        f"F6 stayed on {planet.get('focused')!r}",
    )
    hide_ring(client, "the war map")
    expect_planets(walk(client, "war map", MAP_WINDOWS, limit=PLANET_LIMIT))


def walk_frame(client):
    """F6 reaches the frame after a planet is chosen, and Tab walks it once.

    Recon, Chat and Battle are disabled until then. Release stays disabled
    until the map is held, which this walk does not do.
    """
    lobby.step("F6 to the frame")
    tap(client, "F6")
    frame = screen_of(client)
    lobby.require(
        control_window(client, frame.get("focused")) == FRAME_WINDOW,
        f"F6 left the frame on {frame.get('focused')!r}",
    )
    hide_ring(client, "the frame")
    expect_frame(walk(client, "frame", FRAME_WINDOWS))


def control_window(client, name):
    """The window a control is listed under."""
    if not name:
        return None
    leaf = lobby.find_leaf(lobby.tree_of(client), name)
    if leaf is None:
        return None
    return leaf.get("window") or None


def enter_console(client, marker, title):
    """F6 from the frame into the console a click just opened.

    Opening a console leaves the keyboard on its button, and Tab stays in
    that region. Chat takes the focus itself, so a keyboard already in the
    console is left there. F6 shows the ring, and the pointer hides it
    before Tab.
    """
    home = control_window(client, marker)
    focus = focused_name(client)
    where = control_window(client, focus)
    if home and (where == home or where == marker):
        return
    lobby.require(where == FRAME_WINDOW, f"{title}: the keyboard is on {focus!r} in {where!r}")
    lobby.step(f"F6 into {title}")
    tap(client, "F6")
    landed = focused_name(client)
    landed_window = control_window(client, landed)
    lobby.require(
        landed_window not in (None, FRAME_WINDOW, MAP_WINDOW),
        f"F6 left {title} on {landed!r} in {landed_window!r}",
    )
    hide_ring(client, title)


def open_console(client, button, control):
    """Clicks a console button, waits for one of its controls, and enters it."""
    lobby.step(f"opening {button}")
    lobby.click_named(client, button)
    lobby.wait_control(client, control)
    wait_ring_hidden(client, button)
    enter_console(client, control, button)


def selected_row(client, name):
    """The selected row of a list, or an empty string."""
    leaf = lobby.list_leaf(client, name)
    items = [str(item) for item in leaf.get("items") or []]
    selected = leaf.get("selected")
    if isinstance(selected, int) and 0 <= selected < len(items):
        return items[selected]
    return ""


def link_marks(client):
    """Each page link's name and the words it speaks."""
    marks = []
    for leaf in lobby.leaves(lobby.tree_of(client)):
        name = str(leaf.get("name") or "")
        if name.startswith("Link"):
            marks.append((name, str(leaf.get("label") or "")))
    return marks


def wait_row_text(client, previous, described):
    """Polls until the selected row's text changes."""
    deadline = time.monotonic() + 20
    row = previous
    while True:
        row = selected_row(client, "ContentsList")
        if row != previous:
            return row
        lobby.require(
            time.monotonic() < deadline,
            f"ContentsList never {described}; it still shows {row!r}",
        )
        time.sleep(0.25)


def wait_page_listed(client, previous_row, previous_links):
    """Polls until the selected row changes or the page lists a different link.

    A local page is shown before the key returns. A page with no link leaves
    the row and the links as they were, and that is the page.
    """
    deadline = time.monotonic() + 20
    while True:
        row = selected_row(client, "ContentsList")
        links = link_marks(client)
        if row != previous_row or links != previous_links:
            return
        if not links:
            return
        lobby.require(time.monotonic() < deadline, "the help page did not list its link")
        time.sleep(0.25)


def hide_ring(client, where):
    """Moves the pointer, which hides the ring, and waits until it is hidden."""
    body = screen_of(client)
    pointer = body.get("pointer") or {}
    across = int(pointer.get("x") or 0)
    landed = 40 if across > 80 else 200
    client.call("ui.pointer", {"x": landed, "y": 40, "space": "game"}, lobby.CALL_TIMEOUT)
    wait_ring_hidden(client, where)


def focus_contents(client):
    """Tabs until the help list has the keyboard."""
    for _step in range(WALK_LIMIT):
        if focused_name(client) == "ContentsList":
            return
        tap(client, "Tab")
    raise lobby.Failure(f"Tab did not reach ContentsList; the keyboard is on {focused_name(client)!r}")


def tap_list(client, chord):
    """Presses a key on the help list, with the ring showing.

    Down and Right move the list only while the ring shows. Opening a page
    can leave the keyboard on the page, so the list takes it back first.
    """
    if focused_name(client) != "ContentsList" or not screen_of(client).get("focus_shown"):
        if not screen_of(client).get("focus_shown"):
            tap(client, "Tab")
        focus_contents(client)
    tap(client, chord)


def land_on_link(client):
    """Tabs the open page until a link, or returns when the round has no link."""
    wrapped = False
    for _step in range(WALK_LIMIT):
        name = focused_name(client) or ""
        if name.startswith("Link"):
            leaf = lobby.find_leaf(lobby.tree_of(client), name)
            label = str((leaf or {}).get("label") or "")
            lobby.require(label.strip() and not label.startswith("Link"), f"{name} announces {label!r}")
            return True
        if name == "ContentsList":
            if wrapped:
                return False
            wrapped = True
        tap(client, "Tab")
    return False


def reach_link(client):
    """Opens a Help page that has a link and leaves the keyboard on it.

    Getting Started's pages have none. Right expands the next category and
    Return opens its headlines until Tab lands on a link.
    """
    lobby.step("opening a help page")
    if control_window(client, focused_name(client)) == FRAME_WINDOW:
        tap(client, "F6")
    focus_contents(client)
    if not screen_of(client).get("focus_shown"):
        tap(client, "Tab")
        focus_contents(client)
    lobby.wait_rows(
        client,
        "ContentsList",
        lambda found: any(row.startswith("+ ") for row in found),
        "listed a collapsed category",
    )
    for _step in range(40):
        if selected_row(client, "ContentsList").startswith("+ "):
            break
        tap_list(client, "Down")
    else:
        raise lobby.Failure(
            f"Down did not reach a collapsed category; the list shows {lobby.list_rows(client, 'ContentsList')}"
        )
    for _attempt in range(40):
        row = selected_row(client, "ContentsList")
        if row.startswith("+ "):
            tap_list(client, "Right")
            wait_row_text(client, row, "expanded the category")
            continue
        if not row or row.startswith("- "):
            tap_list(client, "Down")
            continue
        links = link_marks(client)
        tap_list(client, "Enter")
        wait_page_listed(client, row, links)
        if land_on_link(client):
            return
        tap_list(client, "Down")
    raise lobby.Failure(
        f"no help page offered a link; the list shows {lobby.list_rows(client, 'ContentsList')}"
    )


def drive_chat(client):
    """The login console, the frame, the war map, chat, the player pane and Recon."""
    lobby.step("waiting for the login console")
    lobby.wait_control(client, "Username")
    wait_ring_hidden(client, "login")
    expect_login(walk(client, "login", LOGIN_WINDOWS))
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    walk_planets(client)
    lobby.choose_planet(client, lobby.SLAG)
    walk_frame(client)
    open_console(client, "MF_Chat_Button", "ChatList")
    lobby.wait_rows(client, "Chat players", lambda rows: lobby.OTHER_PLAYER in rows, "listed a player")
    expect_chat(walk(client, "chat", CHAT_WINDOWS))
    client.call("ui.select", {"list": "Chat players", "item": lobby.OTHER_PLAYER}, lobby.CALL_TIMEOUT)
    lobby.poll_leaf(client, "Whisper Lock")
    hide_ring(client, "the player pane")
    expect_pane(walk(client, "player pane", PANE_WINDOWS))
    open_console(client, "MF_Recon_Button", "ReconList")
    expect_recon(walk(client, "recon", RECON_WINDOWS))
    client.call("game.quit")


def drive_side(client):
    """The side prompt. Choose is not pressed, and F6 stays inside."""
    lobby.member_login(client, "SideCommander")
    lobby.step("waiting for the side prompt")
    lobby.poll_leaf(client, "Choose", timeout=60)
    wait_ring_hidden(client, "the side prompt")
    order = walk(client, "side prompt", SIDE_WINDOWS)
    expect_side(order)
    f6_stays(client, set(order), "the side prompt")
    client.call("game.quit")


def arm_ladder_search(client):
    """Types one letter so SEARCH can take the keyboard. The search is not run."""
    lobby.step("typing a ladder name without searching")
    lobby.type_into(client, "Ladder search", "A", "LadderConsole")
    deadline = time.monotonic() + 10
    while True:
        leaf = lobby.find_leaf(lobby.tree_of(client), "SEARCH", "LadderConsole")
        if leaf is not None and leaf.get("enabled", True):
            break
        lobby.require(time.monotonic() < deadline, "SEARCH stayed disabled")
        time.sleep(0.25)
    hide_ring(client, "ladders")


def drive_contents(client):
    """News, Ladders and Help. The ladders search is not run."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    open_console(client, "MF_News_Button", "ContentsList")
    expect_contents(walk(client, "news", NEWS_WINDOWS), "news")
    open_console(client, "MF_Ladders_Button", "Ladder search")
    arm_ladder_search(client)
    expect_ladders(walk(client, "ladders", LADDER_WINDOWS))
    open_console(client, "MF_Help_Button", "ContentsList")
    expect_contents(walk(client, "help", HELP_WINDOWS), "help")
    reach_link(client)
    expect_help_page(walk(client, "help page", HELP_PAGE_WINDOWS, check_ring=False))
    client.call("game.quit")


def drive_beta(client):
    """Battle, Host, Profile and the Locator. JOIN and START are not pressed."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG)
    open_console(client, "MF_Battle_Button", "BattleList")
    lobby.wait_rows(
        client,
        "BattleList",
        lambda rows: "Locked" in rows,
        "listed Locked",
    )
    client.call("ui.select", {"list": "BattleList", "item": "Locked"}, lobby.CALL_TIMEOUT)
    lobby.poll_leaf(client, "JOIN", "BATTLE INTELLIGENCE")
    wait_ring_hidden(client, "battle")
    expect_battle(walk(client, "battle", BATTLE_WINDOWS))
    lobby.step("opening the host form")
    lobby.click_named(client, "HOST")
    lobby.poll_leaf(client, "Novice", "HOST BATTLE")
    wait_ring_hidden(client, "host")
    expect_host(walk(client, "host", HOST_WINDOWS))
    open_console(client, "MF_Profile_Button", "ProfileList")
    expect_profile(walk(client, "profile", PROFILE_WINDOWS))
    open_console(client, "MF_Locator_Button", "Search")
    expect_locator(walk(client, "locator", LOCATOR_WINDOWS))
    client.call("game.quit")


def drive_quickplay(client):
    """QuickPlay's groups. FIND BATTLE is not pressed."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    open_console(client, "MF_Quickplay_Button", "FIND BATTLE")
    expect_quickplay(walk(client, "quickplay", QUICK_WINDOWS))
    lobby.step("F6 between the console and the map")
    before = focused_name(client)
    tap(client, "F6")
    moved = focused_name(client)
    lobby.require(moved and moved != before, f"F6 stayed on {moved!r}")
    tap(client, "F6")
    tap(client, "F6")
    client.call("game.quit")


def drive_matchmaking(client):
    """Matchmaking's groups. Activate is not pressed, and F6 stays inside."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG)
    open_console(client, "MF_Chat_Button", "ChatList")
    lobby.click_named(client, "Minimize_Button", "ChatConsole")
    lobby.poll_leaf(client, "Activate", "Matchmaking")
    wait_ring_hidden(client, "matchmaking")
    order = walk(client, "matchmaking", MATCH_WINDOWS)
    expect_matchmaking(order)
    f6_stays(client, set(order), "matchmaking")
    client.call("game.quit")


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


def join_replay(dossier, fragment, dest):
    """Writes the dossier and then the fragment's replies."""
    extra = script_text(fragment)
    count = reply_count(extra)
    lobby.require(count > 0, f"{Path(fragment).name} has no reply lines")
    lobby.require(
        count <= MATCH_REPLY_LIMIT,
        f"{Path(fragment).name} has {count} reply lines; a script holds {MATCH_REPLY_LIMIT}",
    )
    dest.write_text(script_text(dossier) + extra, encoding="utf-8")


def launch(native, game_dir, mod_dir, replay, scratch, drive):
    """Starts one game from a fresh preferences file and drives it."""
    root = Path(scratch)
    root.mkdir(parents=True, exist_ok=True)
    lobby.PROGRESS["launch"] = drive.__name__
    lobby.step(f"starting the game on {Path(replay).name}")
    with tempfile.TemporaryDirectory(prefix="A29-keyboard-", dir=root) as work:
        workdir = Path(work)
        preferences = workdir / "keyboard.PREFERENCES"
        preferences.write_text(CLEAN_PREFERENCES, encoding="utf-8")
        log_dir = workdir / "logs"
        user_folder = workdir / "user"
        data_folder = workdir / "data"
        log_dir.mkdir()
        user_folder.mkdir()
        data_folder.mkdir()
        environment = dict(os.environ)
        environment.pop("OA_BY_USER", None)
        environment.pop("OA_BY_PASSWORD", None)
        environment["SDL_VIDEO_DRIVER"] = "dummy"
        environment["SDL_AUDIO_DRIVER"] = "dummy"
        environment["SDL_RENDER_DRIVER"] = "software"
        process = subprocess.Popen(
            [
                *shlex.split(os.environ.get("OA_TEST_RUNNER", "")),
                str(native),
                "--game-dir",
                str(game_dir),
                "--mod-dir",
                str(mod_dir),
                "--mcp",
                "--skip-intro",
                "--mute",
                "-y",
                "--boneyards-replay",
                str(replay),
                "--preferences-file",
                str(preferences),
                "--log-dir",
                str(log_dir),
                "--user-folder",
                str(user_folder),
                "--data-dir",
                str(data_folder),
            ],
            cwd=workdir,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
        )
        stdout_lines = queue.Queue()
        stderr_lines = []
        out_thread = threading.Thread(
            target=lobby.read_pipe, args=(process.stdout, stdout_lines), daemon=True
        )
        err_thread = threading.Thread(
            target=lobby.read_pipe, args=(process.stderr, lobby._Stderr(stderr_lines)), daemon=True
        )
        out_thread.start()
        err_thread.start()
        client = None
        try:
            client = lobby.open_session(process, stdout_lines, stderr_lines)
            drive(client)
            lobby.step("waiting for the game to end")
            code = process.wait(timeout=lobby.END_TIMEOUT)
            lobby.require(code == 0, f"the game exited {code}")
        except (lobby.Failure, subprocess.TimeoutExpired, OSError) as error:
            report = [f"{lobby.PROGRESS['launch']}, step '{lobby.PROGRESS['step']}': {error}"]
            outputs = root / f"A29-keyboard-{lobby.PROGRESS['launch']}"
            outputs.mkdir(parents=True, exist_ok=True)
            if client is not None and process.poll() is None:
                report.extend(lobby.last_screen(client, outputs / "last-screen.png"))
            elif process.poll() is not None:
                report.append(f"the game had ended with status {process.returncode}")
            err = "".join(stderr_lines)[-2000:].strip()
            if err:
                report.append("--- the game's standard error ---")
                report.append(err)
            report.extend(lobby.keep_log(log_dir, outputs))
            raise lobby.Failure("\n".join(report)) from error
        finally:
            if process.poll() is None:
                process.kill()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
            if process.stdin is not None:
                process.stdin.close()


def main():
    """Runs the six launches."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--chat-replay", required=True)
    parser.add_argument("--side-replay", required=True)
    parser.add_argument("--contents-replay", required=True)
    parser.add_argument("--beta-replay", required=True)
    parser.add_argument("--quickplay-replay", required=True)
    parser.add_argument("--match-replay", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    launch(args.native, args.game_dir, args.mod_dir, args.chat_replay, args.scratch_root, drive_chat)
    launch(args.native, args.game_dir, args.mod_dir, args.side_replay, args.scratch_root, drive_side)
    launch(
        args.native, args.game_dir, args.mod_dir, args.contents_replay, args.scratch_root, drive_contents
    )
    launch(args.native, args.game_dir, args.mod_dir, args.beta_replay, args.scratch_root, drive_beta)
    quick = Path(args.scratch_root) / "A29-quickplay.replay"
    match = Path(args.scratch_root) / "A29-matchmaking.replay"
    try:
        join_replay(args.beta_replay, args.quickplay_replay, quick)
        launch(args.native, args.game_dir, args.mod_dir, quick, args.scratch_root, drive_quickplay)
        join_replay(args.beta_replay, args.match_replay, match)
        launch(args.native, args.game_dir, args.mod_dir, match, args.scratch_root, drive_matchmaking)
    finally:
        quick.unlink(missing_ok=True)
        match.unlink(missing_ok=True)
    print("native-boneyards-keyboard-walk: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (lobby.Failure, OSError) as error:
        print(f"native-boneyards-keyboard-walk: {error}", file=sys.stderr)
        sys.exit(1)
