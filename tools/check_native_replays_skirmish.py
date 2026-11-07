#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish kept as a replay other replayers can read.

Headless games, each in a scratch user folder:

1. Two built-in computer players on Ashap Plateau, 9000 ticks, seed 7, with
   and without ``--record-game``, write the same ``--trace-digest``. The
   replay's meta.json says ``recorded.how`` and ``game.kind`` are
   ``skirmish``, and ``--play-demo`` plays it to that tick, clean and paced.
   At that tick the live units and each player's units, kills and losses
   equal the skirmish's own.
2. A plain headless skirmish, 3000 ticks, seed 7, ``--give-orders``, recorded
   the same way, plays back clean.
3. A fight, ``--give-orders --combat 60 --seed 9 --match-ticks 9000``, writes
   the same trace either way. The replay's kills, metal and energy equal the
   live seats' (9 and 2 kills), and so do the feature cells.
4. A game saved at tick 1500, and the game loaded from that save, write the
   same trace and the same save with recording on and off. The loaded replay
   plays clean, its units match, and it starts from the map's features.
5. A ``--mutator`` skirmish (ridge-tough) plays under the strict unit table.
6. ``--check-away-pause --record-game`` passes: losing the window's focus
   still pauses the skirmish.

The scratch folders are removed whatever happens, so a second run starts as
the first did. The script exits 77 when no game data is installed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import zipfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

RUN_TIMEOUT_SECONDS = 900
PREFERENCES_HEADER = "open-annihilation-preferences 1"
MAP = "Ashap Plateau"
BRAIN_TICKS = 9000
ORDERS_TICKS = 3000
COMBAT_TICKS = 9000
SAVE_AT = 1500
SAVE_TICKS = 1600
LOAD_TICKS = 400
MUTATOR_TICKS = 300
SEED = "7"
COMBAT_SEED = "9"
PLAY_CEILING = "400000"
DEMO_SUMMARY = re.compile(r"demo summary: .*record errors (\d+), tick errors (\d+)")
DEMO_TICK = re.compile(
    r"demo tick (\d+): digest [0-9a-f]+, (\d+) live units, frames (\d+)/(\d+), finished"
)
DEMO_PLAYER = re.compile(r"demo player: slot (\d+) units (\d+) kills (-?\d+) losses (-?\d+)")
ECONOMY_PLAYER = re.compile(
    r"(skirmish|demo) player: slot (\d+) units (\d+) kills (-?\d+) losses (-?\d+)"
    r" metal ([-+0-9.eE]+) energy ([-+0-9.eE]+)"
)
FEATURE_LINE = re.compile(
    r"(skirmish features at start|demo features at start|skirmish features|demo features):"
    r" cells (\d+) digest ([0-9a-f]+)"
)
BRAIN_SEAT = re.compile(r"brain match: seat (\d+) units (\d+) built \d+ lost (\d+) kills (\d+)")
BRAIN_END = re.compile(r"brain match: end tick (\d+)")
BRAIN_DIGEST = re.compile(r"brain match: digest ([0-9a-f]+)")
AWAY_HELD = "away pause check: Pause single player held the skirmish with the paused title"
# The origin hash the replay-mutators check installs ridge-tough under. Playback
# accepts the package only when the installed copy carries the hash the recording stored.
TOUGH_SHA = "d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3"
TRACE_HEADER_BYTES = 16
TRACE_RECORD_BYTES = 24
TRACE_RECORDS_PER_TICK = 8


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_preferences(path):
    """A preferences file that records nothing on its own and asks no question."""
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.content-updates") + " " + quote_preference("never"),
        quote_preference("open-annihilation.first-run") + " " + quote_preference("done"),
        quote_preference("open-annihilation.replays.record") + " " + quote_preference("off"),
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def game_environment():
    """The environment every run of the game takes: no window, no sound."""
    return dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        SDL_RENDER_DRIVER="software",
    )


def game_command(native, game_dir, arguments):
    """The command line of one headless run of the game."""
    return [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        "--headless-check",
        *arguments,
    ]


def run_game(native, game_dir, work, arguments):
    """Runs the game to its end. Returns its status and combined output."""
    result = subprocess.run(
        game_command(native, game_dir, arguments),
        cwd=work,
        env=game_environment(),
        timeout=RUN_TIMEOUT_SECONDS,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    return result.returncode, result.stdout + result.stderr


def show(output):
    """Prints a run's output, so that a failure carries it."""
    print(output, end="" if output.endswith("\n") or output == "" else "\n")


def read_meta(replay):
    """The description of a .oademo, and the names of its entries in order."""
    with zipfile.ZipFile(replay) as archive:
        names = archive.namelist()
        meta = json.loads(archive.read("meta.json"))
    return meta, names


def trace_ticks(path):
    """The ticks a --trace-digest file holds, or None when the size is not a whole run."""
    size = path.stat().st_size
    body = size - TRACE_HEADER_BYTES
    stride = TRACE_RECORD_BYTES * TRACE_RECORDS_PER_TICK
    if size < TRACE_HEADER_BYTES or body % stride != 0:
        return None
    return body // stride


def expect_skirmish_meta(replay):
    """meta.json of a skirmish recording: both names, no address, and a real stream."""
    meta, names = read_meta(replay)
    if names[:3] != ["oademo", "meta.json", "manifest.json"] or "recording.tad.zst" not in names:
        raise ScriptFailure(f"the replay holds the entries {names}")
    recorded = meta.get("recorded", {})
    game = meta.get("game", {})
    recording = meta.get("recording", {})
    players = meta.get("players", [])
    if (
        recorded.get("how") != "skirmish"
        or game.get("kind") != "skirmish"
        or not recording.get("addresses_removed")
        or recording.get("packets", 0) < 2
    ):
        raise ScriptFailure(f"meta.json describes the game as {json.dumps(meta)[:500]}")
    return meta, players


def expect_clean_playback(native, game_dir, work, replay, label, ceiling):
    """Plays a replay to its end. Returns the output when it is clean and paced."""
    user = work / ("play-" + label)
    preferences = work / ("play-" + label + ".conf")
    write_preferences(preferences)
    status, output = run_game(native, game_dir, work, [
        "--play-demo",
        str(replay),
        "--demo-unit-table",
        "strict",
        "--match-ticks",
        str(ceiling),
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
    ])
    found = DEMO_SUMMARY.search(output)
    finished = DEMO_TICK.search(output)
    if found is None or finished is None or status != 0 or "demo check failed" in output:
        show(output)
        raise ScriptFailure(f"playing {replay.name} exited with {status} and was not clean and paced")
    if found.group(1) != "0" or found.group(2) != "0":
        show(output)
        raise ScriptFailure(
            f"playing {replay.name} had {found.group(1)} record errors and "
            f"{found.group(2)} tick errors"
        )
    if int(finished.group(1)) < 2 or int(finished.group(3)) < 2:
        show(output)
        raise ScriptFailure(
            f"playing {replay.name} finished at tick {finished.group(1)} "
            f"after {finished.group(3)} frames"
        )
    return output


def expect_the_brain_match(native, game_dir, work):
    """The computer match is unchanged by recording, and the replay matches its end."""
    traces = {}
    digests = {}
    recorded = None
    for label in ("off", "on"):
        user = work / f"brain-{label}"
        preferences = work / f"brain-{label}.conf"
        write_preferences(preferences)
        trace = work / f"brain-{label}.trace"
        extra = ["--trace-digest", str(trace)]
        replay = user / "skirmish.oademo"
        if label == "on":
            extra += ["--record-game", str(replay)]
        status, output = run_game(native, game_dir, work, [
            "--brain-match",
            "builtin,builtin",
            "--brain-map",
            MAP,
            "--match-ticks",
            str(BRAIN_TICKS),
            "--seed",
            SEED,
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
            *extra,
        ])
        if status != 0 or not trace.is_file():
            show(output)
            raise ScriptFailure(f"the skirmish with recording {label} exited with {status}")
        ticks = trace_ticks(trace)
        if ticks != BRAIN_TICKS:
            raise ScriptFailure(f"the {label} trace holds {ticks} ticks, not {BRAIN_TICKS}")
        digest = BRAIN_DIGEST.search(output)
        if digest is None:
            show(output)
            raise ScriptFailure(f"the skirmish with recording {label} printed no digest")
        traces[label] = trace.read_bytes()
        digests[label] = digest.group(1)
        if label == "off":
            written = list(user.rglob("*.oademo"))
            if written:
                raise ScriptFailure(f"the skirmish without --record-game wrote {written}")
        else:
            recorded = (replay, output)
            if not replay.is_file():
                show(output)
                raise ScriptFailure("the skirmish with --record-game wrote no replay")
    if traces["off"] != traces["on"] or digests["off"] != digests["on"]:
        raise ScriptFailure(
            f"the traces differ with recording on and off "
            f"({len(traces['off'])} and {len(traces['on'])} bytes, "
            f"digests {digests['off']} and {digests['on']})"
        )
    replay, brain_output = recorded
    meta, players = expect_skirmish_meta(replay)
    if meta.get("game", {}).get("length_ticks") != BRAIN_TICKS:
        raise ScriptFailure(
            f"the replay's length is {meta.get('game', {}).get('length_ticks')}, not {BRAIN_TICKS}"
        )
    if len(players) != 2 or any(
        player.get("kind") != "computer" or not player.get("name") for player in players
    ):
        raise ScriptFailure(f"meta.json names the players {players}, not two computer players")
    if meta.get("recording", {}).get("packets", 0) < BRAIN_TICKS:
        raise ScriptFailure(
            f"the replay holds {meta.get('recording', {}).get('packets')} packets for {BRAIN_TICKS} ticks"
        )
    seats = {
        int(match.group(1)): (int(match.group(2)), int(match.group(4)), int(match.group(3)))
        for match in BRAIN_SEAT.finditer(brain_output)
    }
    end = BRAIN_END.search(brain_output)
    if end is None or int(end.group(1)) != BRAIN_TICKS or set(seats) != {1, 2}:
        show(brain_output)
        raise ScriptFailure(f"the skirmish did not end as two seats at tick {BRAIN_TICKS}")
    playback = expect_clean_playback(native, game_dir, work, replay, "brain", PLAY_CEILING)
    finished = DEMO_TICK.search(playback)
    played = {
        int(match.group(1)): (int(match.group(2)), int(match.group(3)), int(match.group(4)))
        for match in DEMO_PLAYER.finditer(playback)
    }
    if int(finished.group(1)) != BRAIN_TICKS:
        show(playback)
        raise ScriptFailure(f"the replay finished at tick {finished.group(1)}, not {BRAIN_TICKS}")
    if int(finished.group(2)) != sum(seat[0] for seat in seats.values()):
        show(playback)
        raise ScriptFailure(
            f"the replay has {finished.group(2)} live units; the skirmish's seats hold "
            f"{sum(seat[0] for seat in seats.values())}"
        )
    for seat, (units, kills, losses) in seats.items():
        played_seat = played.get(seat - 1)
        if played_seat != (units, kills, losses):
            show(playback)
            raise ScriptFailure(
                f"seat {seat} ended at {units} units, {kills} kills and {losses} losses; "
                f"slot {seat - 1} played back as {played_seat}"
            )
    watchers = [slot for slot in played if slot not in {0, 1}]
    if any(played[slot] != (0, 0, 0) for slot in watchers):
        raise ScriptFailure(f"the watcher slots hold { {slot: played[slot] for slot in watchers} }")
    print(
        f"replays skirmish: Ashap Plateau, {BRAIN_TICKS} ticks, the same "
        f"{len(traces['off'])}-byte trace with recording on and off; "
        f"{replay.name} plays to that tick with the same units, kills and losses"
    )


def economy_rows(output, kind):
    """slot -> (units, kills, losses, metal, energy) for one kind of player line."""
    rows = {}
    for match in ECONOMY_PLAYER.finditer(output):
        if match.group(1) != kind:
            continue
        rows[int(match.group(2))] = match.groups()[2:]
    return rows


def feature_rows(output):
    """Each feature line's name mapped to (cells, digest). The longer names win a shared prefix."""
    rows = {}
    for match in FEATURE_LINE.finditer(output):
        rows[match.group(1)] = (int(match.group(2)), match.group(3))
    return rows


def expect_same_traces(traces, what):
    """The on and off traces are the same bytes."""
    if traces["off"] != traces["on"]:
        raise ScriptFailure(
            f"the {what} traces differ with recording on and off "
            f"({len(traces['off'])} and {len(traces['on'])} bytes)"
        )


def expect_the_fight(native, game_dir, work):
    """A fight's kills, metal, energy and features survive into the replay."""
    traces = {}
    recorded = None
    for label in ("off", "on"):
        user = work / f"fight-{label}"
        preferences = work / f"fight-{label}.conf"
        write_preferences(preferences)
        trace = work / f"fight-{label}.trace"
        replay = user / "fight.oademo"
        extra = ["--trace-digest", str(trace)]
        if label == "on":
            extra += ["--record-game", str(replay)]
        status, output = run_game(native, game_dir, work, [
            "--give-orders",
            "--combat",
            "60",
            "--seed",
            COMBAT_SEED,
            "--match-ticks",
            str(COMBAT_TICKS),
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
            *extra,
        ])
        if status != 0 or not trace.is_file():
            show(output)
            raise ScriptFailure(f"the fight with recording {label} exited with {status}")
        ticks = trace_ticks(trace)
        if ticks != COMBAT_TICKS:
            raise ScriptFailure(f"the fight's {label} trace holds {ticks} ticks, not {COMBAT_TICKS}")
        traces[label] = trace.read_bytes()
        if label == "off":
            written = list(user.rglob("*.oademo"))
            if written:
                raise ScriptFailure(f"the fight without --record-game wrote {written}")
        else:
            if not replay.is_file():
                show(output)
                raise ScriptFailure("the fight with --record-game wrote no replay")
            recorded = (replay, output)
    expect_same_traces(traces, "fight")
    replay, live = recorded
    live_rows = economy_rows(live, "skirmish")
    live_features = feature_rows(live)
    playback = expect_clean_playback(native, game_dir, work, replay, "fight", PLAY_CEILING)
    played_rows = economy_rows(playback, "demo")
    played_features = feature_rows(playback)
    if not live_rows or live_rows != {slot: played_rows.get(slot) for slot in live_rows}:
        show(live)
        show(playback)
        raise ScriptFailure(
            f"the fight's seats {live_rows} played back as {played_rows}"
        )
    kills = sorted(int(row[1]) for row in live_rows.values())
    if kills != [2, 9]:
        raise ScriptFailure(f"the fight's kills are {kills}, not 2 and 9")
    paired = (
        ("skirmish features at start", "demo features at start"),
        ("skirmish features", "demo features"),
    )
    for live_name, played_name in paired:
        if live_name not in live_features or live_features[live_name] != played_features.get(played_name):
            raise ScriptFailure(
                f"the fight's {live_name} is {live_features.get(live_name)}; "
                f"the replay's {played_name} is {played_features.get(played_name)}"
            )
    if live_features["skirmish features at start"] == live_features["skirmish features"]:
        raise ScriptFailure(
            f"the fight changed no feature cell ({live_features['skirmish features']})"
        )
    end_cells, end_digest = live_features["skirmish features"]
    print(
        f"replays skirmish: the fight's kills are 9 and 2, its metal and energy match, "
        f"and its {end_cells} feature cells ({end_digest}) match the replay"
    )


def expect_the_saved_game(native, game_dir, work):
    """A save made while recording matches one made without, and the loaded replay starts from the map."""
    traces = {}
    saves = {}
    map_features = None
    for label in ("off", "on"):
        user = work / f"save-{label}"
        preferences = work / f"save-{label}.conf"
        write_preferences(preferences)
        trace = work / f"save-{label}.trace"
        save = work / f"save-{label}.sav"
        extra = ["--trace-digest", str(trace), "--save-file", str(save)]
        if label == "on":
            extra += ["--record-game", str(user / "during-save.oademo")]
        status, output = run_game(native, game_dir, work, [
            "--give-orders",
            "--seed",
            SEED,
            "--save-after",
            str(SAVE_AT),
            "--match-ticks",
            str(SAVE_TICKS),
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
            *extra,
        ])
        if status != 0 or not trace.is_file() or not save.is_file():
            show(output)
            raise ScriptFailure(f"the saved game with recording {label} exited with {status}")
        if f"saveload: saved " not in output or f" at tick {SAVE_AT}" not in output:
            show(output)
            raise ScriptFailure(f"the saved game with recording {label} did not save at tick {SAVE_AT}")
        traces[label] = trace.read_bytes()
        saves[label] = save.read_bytes()
        if label == "on":
            features = feature_rows(output)
            map_features = features.get("skirmish features at start")
            if map_features is None:
                show(output)
                raise ScriptFailure("the saved game printed no feature line at the start")
    expect_same_traces(traces, "saved game")
    if saves["off"] != saves["on"]:
        raise ScriptFailure(
            f"the saves differ with recording on and off "
            f"({len(saves['off'])} and {len(saves['on'])} bytes)"
        )
    kept = work / "save-on.sav"
    load_traces = {}
    loaded = None
    for label in ("off", "on"):
        user = work / f"load-{label}"
        preferences = work / f"load-{label}.conf"
        write_preferences(preferences)
        trace = work / f"load-{label}.trace"
        replay = user / "loaded.oademo"
        extra = ["--trace-digest", str(trace)]
        if label == "on":
            extra += ["--record-game", str(replay)]
        status, output = run_game(native, game_dir, work, [
            "--give-orders",
            "--seed",
            SEED,
            "--load",
            str(kept),
            "--match-ticks",
            str(LOAD_TICKS),
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
            *extra,
        ])
        if status != 0 or not trace.is_file():
            show(output)
            raise ScriptFailure(f"the loaded game with recording {label} exited with {status}")
        load_traces[label] = trace.read_bytes()
        if label == "on":
            if not replay.is_file():
                show(output)
                raise ScriptFailure("the loaded game with --record-game wrote no replay")
            loaded = (replay, output)
    expect_same_traces(load_traces, "loaded game")
    replay, live = loaded
    live_rows = economy_rows(live, "skirmish")
    live_features = feature_rows(live)
    playback = expect_clean_playback(native, game_dir, work, replay, "loaded", PLAY_CEILING)
    played_rows = economy_rows(playback, "demo")
    played_features = feature_rows(playback)
    if not live_rows or live_rows != {slot: played_rows.get(slot) for slot in live_rows}:
        show(live)
        show(playback)
        raise ScriptFailure(f"the loaded game's seats {live_rows} played back as {played_rows}")
    replay_start = played_features.get("demo features at start")
    live_start = live_features.get("skirmish features at start")
    if replay_start != map_features or live_start is None or live_start == replay_start:
        raise ScriptFailure(
            f"the loaded replay starts from {replay_start}; the map is {map_features} "
            f"and the save is {live_start}"
        )
    print(
        f"replays skirmish: a save at tick {SAVE_AT} matches with recording on and off; "
        f"the loaded replay's units match and it starts from the map's "
        f"{replay_start[0]} feature cells, not the save's {live_start[0]}"
    )


def install_mutator(package, user):
    """Copies a package into the user folder the way an installed mutator is stored."""
    dest = user / "Mutators" / package.name
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(package, dest)
    (dest / ".oa-origin.yaml").write_text(
        "# Written by Open Annihilation when it installed this folder; not part of the package.\n"
        "oa-origin: 1\n"
        "origin: file\n"
        f'sha256: "{TOUGH_SHA}"\n'
        'installed: "2026-10-21"\n',
        encoding="utf-8",
    )


def expect_the_mutator(native, game_dir, work, mutator):
    """A mutator that changes two units still plays under the strict unit table."""
    user = work / "mutator"
    user.mkdir()
    install_mutator(mutator, user)
    preferences = work / "mutator.conf"
    write_preferences(preferences)
    replay = user / "mutator.oademo"
    status, output = run_game(native, game_dir, work, [
        "--give-orders",
        "--seed",
        SEED,
        "--match-ticks",
        str(MUTATOR_TICKS),
        "--mutator",
        mutator.name,
        "--record-game",
        str(replay),
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
    ])
    if status != 0 or not replay.is_file():
        show(output)
        raise ScriptFailure(f"the mutator skirmish exited with {status} and wrote no replay")
    play_user = work / "play-mutator"
    play_user.mkdir()
    install_mutator(mutator, play_user)
    expect_clean_playback(native, game_dir, work, replay, "mutator", PLAY_CEILING)
    print(f"replays skirmish: {mutator.name} plays strict")


def expect_away_pause(native, game_dir, work):
    """Losing the window's focus pauses a skirmish that is being recorded."""
    user = work / "away"
    preferences = work / "away.conf"
    write_preferences(preferences)
    replay = work / "away.oademo"
    result = subprocess.run(
        [
            *RUNNER,
            str(native),
            "--game-dir",
            str(game_dir),
            "--skip-intro",
            "--mute",
            "--check-away-pause",
            "--record-game",
            str(replay),
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
        ],
        cwd=work,
        env=game_environment(),
        timeout=RUN_TIMEOUT_SECONDS,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    output = result.stdout + result.stderr
    if result.returncode != 0 or AWAY_HELD not in output or not replay.is_file():
        show(output)
        raise ScriptFailure(
            f"away pause with --record-game exited with {result.returncode} "
            f"and the replay exists: {replay.is_file()}"
        )
    print("replays skirmish: losing the focus pauses a recorded skirmish")


def expect_the_plain_skirmish(native, game_dir, work):
    """A headless skirmish recorded with --give-orders plays back clean."""
    user = work / "orders"
    preferences = work / "orders.conf"
    write_preferences(preferences)
    replay = user / "orders.oademo"
    status, output = run_game(native, game_dir, work, [
        "--match-ticks",
        str(ORDERS_TICKS),
        "--seed",
        SEED,
        "--give-orders",
        "--record-game",
        str(replay),
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
    ])
    if status != 0 or not replay.is_file() or "skirmish: recorded the game to " not in output:
        show(output)
        raise ScriptFailure(f"the plain skirmish exited with {status} and wrote no replay")
    meta, _players = expect_skirmish_meta(replay)
    if meta.get("game", {}).get("kind") != "skirmish":
        raise ScriptFailure("the plain skirmish was not recorded as a skirmish")
    expect_clean_playback(native, game_dir, work, replay, "orders", PLAY_CEILING)
    print(f"replays skirmish: the {ORDERS_TICKS}-tick skirmish with --give-orders plays back clean")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--mutator", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("replays skirmish: skipped, no installed game")
        return 77
    native = args.native.resolve()
    mutator = args.mutator.resolve()
    if not mutator.is_dir():
        print(f"replays skirmish: FAILED: no mutator at {mutator}")
        return 1
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-replays-skirmish-", dir=scratch_root) as temporary:
        work = Path(temporary)
        try:
            expect_the_brain_match(native, game_dir, work)
            expect_the_plain_skirmish(native, game_dir, work)
            expect_the_fight(native, game_dir, work)
            expect_the_saved_game(native, game_dir, work)
            expect_the_mutator(native, game_dir, work, mutator)
            expect_away_pause(native, game_dir, work)
        except ScriptFailure as failure:
            print(f"replays skirmish: FAILED: {failure}")
            return 1
    print("replays skirmish: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
