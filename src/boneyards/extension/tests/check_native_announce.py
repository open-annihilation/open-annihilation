#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Spoken updates from the chat replay.

Logs in, chooses Slag and opens chat. The standard replay must announce
Ghost_Commander's line itself, not a burst, and exactly one page two
seconds after the first; a whisper the player sends is then heard once. A
scratch replay sends the member list four pumps after the entry; that list
is not spoken. speak-chat off hears no page, no whisper and no chat line,
though the page arrives and the whisper is sent. With the screen-reader
preference unset, no announce note is posted. The screen-reader
preference is on for the first three launches, so no reader has to be
running.
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

import check_native_mcp_lobby as lobby
_OA_TOOLS = Path(__file__).resolve().parents[4] / "tools"
if str(_OA_TOOLS) not in sys.path:
    sys.path.insert(0, str(_OA_TOOLS))
import native_offline  # noqa: E402

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
END_TIMEOUT = 60
ANNOUNCE_TIMEOUT = 60
PAGE_WAIT_S = 2.0
# Longer than the chat hold, so a spoken member list would have collapsed.
LATE_WAIT_S = 3.0
CHAT_LINE = "Anyone up for a 1v1"
WHISPER_WAIT_S = 10.0
WHISPER_SHOWN = "<You whispered to Ghost_Commander> see you there"
WHISPER_SPOKEN = "Whisper to Ghost_Commander"
# The screen-reader preference is on, so speech runs with no reader.
# Chat and the battle room speak; the ticker stays quiet. The update check is
# never and the catalogue is read only when the Library opens, which this check
# never does: "-y" stops the entry when both are never.
PREFERENCES = (
    "open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"library\"\n"
    '"multiplayer|tcpaddr" "192.0.2.7"\n'
    '"open-annihilation.access.screen-reader" "on"\n'
    '"open-annihilation.access.speak-chat" "all"\n'
    '"open-annihilation.access.speak-lobby" "1"\n'
    '"open-annihilation.access.speak-ticker" "0"\n'
)
PREFERENCES_CHAT_OFF = PREFERENCES.replace(
    '"open-annihilation.access.speak-chat" "all"',
    '"open-annihilation.access.speak-chat" "off"',
)
# The screen-reader preference is absent. Speech stays closed.
PREFERENCES_UNSET = (
    "open-annihilation-preferences 1\n\"open-annihilation.update-check\" \"never\"\n\"open-annihilation.content-updates\" \"library\"\n"
    '"multiplayer|tcpaddr" "192.0.2.7"\n'
    '"open-annihilation.access.speak-chat" "all"\n'
    '"open-annihilation.access.speak-lobby" "1"\n'
    '"open-annihilation.access.speak-ticker" "0"\n'
)
UNSET_ANNOUNCE_COUNT = None


def announce_texts(client):
    """The announce notes still kept, as words."""
    found = []
    for note in lobby.events_of(client):
        if note.get("event") != "announce":
            continue
        found.append(str(note.get("text") or ""))
    return found


def subscribe(client, kinds):
    """Subscribes to these event kinds."""
    subscribed = lobby.body_of(client.call("events.subscribe", {"events": list(kinds)}))
    accepted = subscribed.get("events") or []
    for kind in kinds:
        lobby.require(kind in accepted, f"subscribe refused {kind}; accepted {accepted}")


def reach_chat(client):
    """Logs in, opens chat on Slag, and waits until Ghost_Commander's line is shown."""
    lobby.member_login(client, "oa_check", registration=True)
    lobby.step("waiting for the war map")
    lobby.wait_for_war_map(client)
    lobby.choose_planet(client, lobby.SLAG_ID)
    lobby.step("opening the chat console")
    lobby.click_named(client, "MF_Chat_Button")
    lobby.wait_control(client, "ChatList")
    lobby.step("waiting for the replay's chat line")
    lobby.wait_rows(
        client,
        "ChatList",
        lambda rows: lobby.OTHER_LINE in rows,
        f"held {lobby.OTHER_LINE!r}",
        timeout=45,
    )


def send_whisper(client):
    """Whispers to Ghost_Commander and waits until the chat shows it."""
    lobby.step("whispering to Ghost_Commander")
    lobby.click_named(client, "Chat entry")
    client.call(
        "ui.type",
        {"text": "+w Ghost_Commander see you there", "submit": True},
        lobby.CALL_TIMEOUT,
    )
    lobby.wait_rows(
        client, "ChatList", lambda rows: WHISPER_SHOWN in rows, "showed the whisper sent"
    )


def require_no_list(texts):
    """Fails when a member row was spoken, alone or as a burst."""
    bursts = [text for text in texts if "new lines in" in text]
    joined = [text for text in texts if "joined" in text]
    lobby.require(not bursts, "a burst counted the room: " + repr(bursts))
    lobby.require(not joined, "entering the room spoke a join: " + repr(joined))


def drive_standard(client):
    """Requires Ghost_Commander's line and exactly one page, two seconds later."""
    subscribe(client, ["announce"])
    reach_chat(client)
    lobby.step("waiting for the spoken chat line and the page")
    deadline = time.monotonic() + ANNOUNCE_TIMEOUT
    page_at = None
    texts = []
    while time.monotonic() < deadline:
        texts = announce_texts(client)
        require_no_list(texts)
        pages = [text for text in texts if "Page from" in text]
        if pages and page_at is None:
            page_at = time.monotonic()
        line_seen = any(CHAT_LINE in text for text in texts)
        if line_seen and page_at is not None and time.monotonic() - page_at >= PAGE_WAIT_S:
            lobby.require(len(pages) == 1, "expected one page, saw " + repr(pages))
            send_whisper(client)
            lobby.step("waiting for the spoken whisper")
            whisper_deadline = time.monotonic() + WHISPER_WAIT_S
            whispers = []
            while time.monotonic() < whisper_deadline:
                whispers = [text for text in announce_texts(client) if WHISPER_SPOKEN in text]
                if whispers:
                    break
                time.sleep(0.25)
            time.sleep(PAGE_WAIT_S)
            whispers = [text for text in announce_texts(client) if WHISPER_SPOKEN in text]
            lobby.require(len(whispers) == 1, "expected one whisper, saw " + repr(whispers))
            lobby.step("quitting")
            client.call("game.quit")
            return
        time.sleep(0.25)
    lobby.require(
        False,
        "no spoken chat line and one page; announcements were " + repr(texts[-12:]),
    )


def drive_late(client):
    """The member list arrives after the next pump and is not spoken."""
    subscribe(client, ["announce"])
    reach_chat(client)
    lobby.step("waiting to see whether the late member list is spoken")
    time.sleep(LATE_WAIT_S)
    require_no_list(announce_texts(client))
    lobby.step("quitting")
    client.call("game.quit")


def drive_chat_off(client):
    """speak-chat off hears neither the page, a whisper nor the chat line.

    The page note must have arrived and the whisper must show as sent, so
    the silence is the setting's and not a missing line.
    """
    subscribe(client, ["announce", "page"])
    reach_chat(client)
    send_whisper(client)
    lobby.step("waiting to see whether a page, a whisper or the line is spoken")
    time.sleep(PAGE_WAIT_S)
    notes = lobby.events_of(client)
    lobby.require(
        any(note.get("event") == "page" for note in notes),
        "the page never arrived; recent events " + repr(notes[-12:]),
    )
    texts = announce_texts(client)
    pages = [text for text in texts if "Page from" in text]
    whispers = [text for text in texts if "Whisper" in text]
    lines = [text for text in texts if CHAT_LINE in text or "new lines in" in text]
    lobby.require(not pages, "speak-chat off spoke a page: " + repr(pages))
    lobby.require(not whispers, "speak-chat off spoke a whisper: " + repr(whispers))
    lobby.require(not lines, "speak-chat off spoke a chat line: " + repr(lines))
    lobby.step("quitting")
    client.call("game.quit")


def drive_unset(client):
    """With the screen-reader preference unset, no announce note is posted."""
    global UNSET_ANNOUNCE_COUNT
    subscribe(client, ["announce", "said"])
    reach_chat(client)
    lobby.step("counting announce notes with the preference unset")
    time.sleep(PAGE_WAIT_S)
    notes = lobby.events_of(client)
    announces = [note for note in notes if note.get("event") == "announce"]
    said = [
        note
        for note in notes
        if note.get("event") == "said" and CHAT_LINE in str(note.get("text") or "")
    ]
    UNSET_ANNOUNCE_COUNT = len(announces)
    lobby.require(
        UNSET_ANNOUNCE_COUNT == 0,
        "preference unset still announced: " + repr(announces[-8:]),
    )
    lobby.require(bool(said), "the feed lost the chat line; recent events " + repr(notes[-12:]))
    lobby.step("quitting")
    client.call("game.quit")


def late_member_replay(source, dest):
    """Writes the chat replay with the member list four pumps after the entry.

    The list is later than the next pump, which used to speak it, and still
    inside the two-second settlement. The file is built the way the modal
    key check joins two replays: read the text, then write the scratch copy.
    """
    text = Path(source).read_text(encoding="utf-8")
    if not text.endswith("\n"):
        text += "\n"
    marker = "\nmember "
    at = text.find(marker)
    if at < 0:
        raise lobby.Failure("the replay has no member list to send late")
    delayed = text[: at + 1] + "delay 4\n" + text[at + 1 :]
    dest.write_text(delayed, encoding="utf-8")


def run_game(native, game_dir, mod_dir, replay, scratch, preferences, drive, label):
    """Starts one game on the dummy drivers and drives one spoken-update case."""
    root = Path(scratch)
    root.mkdir(parents=True, exist_ok=True)
    lobby.PROGRESS["launch"] = label
    lobby.step(f"starting the game on {Path(replay).name}")
    with tempfile.TemporaryDirectory(prefix="A10-announce-", dir=root) as work:
        workdir = Path(work)
        preferences_path = workdir / "announce.conf"
        preferences_path.write_text(preferences, encoding="utf-8")
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
                *RUNNER,
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
                str(preferences_path),
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
            code = process.wait(timeout=END_TIMEOUT)
            lobby.require(code == 0, f"the game exited {code}")
        except (lobby.Failure, subprocess.TimeoutExpired, OSError) as error:
            report = [f"{lobby.PROGRESS['launch']}, step '{lobby.PROGRESS['step']}': {error}"]
            outputs = root / "A10-announce-failure"
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
    """Runs the spoken-update check once."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--mod-dir", type=Path, required=True)
    parser.add_argument("--replay", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        run_game(
            args.native,
            args.game_dir,
            args.mod_dir,
            args.replay,
            args.scratch_root,
            PREFERENCES,
            drive_standard,
            "announce",
        )
        with tempfile.TemporaryDirectory(prefix="A10-announce-late-", dir=args.scratch_root) as late:
            late_replay = Path(late) / "late-members.replay"
            late_member_replay(args.replay, late_replay)
            run_game(
                args.native,
                args.game_dir,
                args.mod_dir,
                late_replay,
                args.scratch_root,
                PREFERENCES,
                drive_late,
                "late-list",
            )
        run_game(
            args.native,
            args.game_dir,
            args.mod_dir,
            args.replay,
            args.scratch_root,
            PREFERENCES_CHAT_OFF,
            drive_chat_off,
            "speak-chat-off",
        )
        run_game(
            args.native,
            args.game_dir,
            args.mod_dir,
            args.replay,
            args.scratch_root,
            PREFERENCES_UNSET,
            drive_unset,
            "preference-unset",
        )
    except lobby.Failure as failure:
        print(f"native-boneyards-announce: {failure}", file=sys.stderr)
        return 1
    print(
        "native-boneyards-announce: Ghost_Commander's line, one page and one whisper; "
        "a late member list was quiet; speak-chat off heard no page, whisper or line; "
        f"with the preference unset, {UNSET_ANNOUNCE_COUNT} announce notes"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
