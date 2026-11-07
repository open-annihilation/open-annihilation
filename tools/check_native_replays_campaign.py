#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A campaign mission kept as a replay other replayers can read.

Headless games, each in a scratch user folder:

1. Arm Campaign mission 0, 2400 ticks, seed 7, ``--give-orders``, with and
   without ``--record-game``, writes the same ``--trace-digest``. That run
   wins and advances, so its file is the next mission. The victory tick N
   it prints is recorded again with ``--past-outcome`` (mission 0's own
   trace, the same either way) and once more through the end screen, so the
   file is mission 0, ``game.ended`` ``finished``, ``game.length_ticks`` N.
   ``--replay-index`` of that file ends ``finished`` on the same tick: the
   mission ends with no commander's death and no end-screen scores, and the
   index takes ``meta.json``. The recording player's ``result`` is ``won``,
   and the index shows Won.
2. Mission 8, 700 ticks, the mission ``native-campaign-defeat`` plays, the
   same way. It stops at its defeat, the recording player's ``result`` is
   ``lost``, and the index shows Lost.
3. Each recording's players and feature cells match the replay. Playing the
   file for one tick creates as many units as the live tick 0 census.
4. A ``.tad`` from ``--record-game`` and from ``--net-record``, the ``.tad``
   unpacked from mission 8's ``.oademo``, and that ``.tad`` packed back, each
   play strict to the live end.
5. ``--check-away-pause`` on mission 0, while it is recorded, pauses when
   the window loses focus. Its lines say mission.

The scratch folders are removed whatever happens, so a second run starts as
the first did. The script exits 77 when no game data is installed.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import zipfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

RUN_TIMEOUT_SECONDS = 900
PREFERENCES_HEADER = "open-annihilation-preferences 1"
CAMPAIGN = "Arm Campaign"
MISSIONS = {
    0: ("1: A Hero Returns", 2400, True),
    8: ("9: The Hydration Plant", 700, False),
}
PLAY_CEILING = "400000"
DEMO_SUMMARY = re.compile(
    r"demo summary: .*created (\d+) refused \d+ past table \d+, record errors (\d+), "
    r"tick errors (\d+)"
)
DEMO_TICK = re.compile(
    r"demo tick (\d+): digest [0-9a-f]+, (\d+) live units, frames (\d+)/(\d+), finished"
)
PLAYER = re.compile(
    r"(campaign|demo) player: slot (\d+) units (\d+) kills (-?\d+) losses (-?\d+)"
    r" metal ([-+0-9.eE]+) energy ([-+0-9.eE]+)"
)
FEATURE = re.compile(
    r"(campaign features at start|demo features at start|campaign features|demo features):"
    r" cells (\d+) digest ([0-9a-f]+)"
)
CENSUS = re.compile(r"campaign tick\s+(\d+): units (\d+)")
RESULT = re.compile(
    r"campaign result: .*: (\d+) ticks, (\d+) failed ticks, (\d+) distinct errors, "
    r"outcome (\w+) at tick (\d+)"
)
OUTCOME = re.compile(r"campaign outcome: (\w+) at tick (\d+)")
ADVANCE = re.compile(r"campaign advance: run tick \d+: mission (\d+) \([^)]*\), (.*)")
AWAY_HELD = "away pause check: Pause single player held the mission with the paused title"
AWAY_STEP = "away pause: losing the focus pauses the mission, with the paused title"
TRACE_HEADER_BYTES = 16
TRACE_RECORD_BYTES = 24
TRACE_RECORDS_PER_TICK = 8
TRACE_STRIDE = TRACE_RECORD_BYTES * TRACE_RECORDS_PER_TICK


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_preferences(path):
    """A preferences file that records nothing on its own and asks no question."""
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.update-check") + " " + quote_preference("never"),
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


def trace_ticks(path):
    """The ticks a --trace-digest file holds, or None when the size is not a whole run."""
    size = path.stat().st_size
    body = size - TRACE_HEADER_BYTES
    if size < TRACE_HEADER_BYTES or body % TRACE_STRIDE != 0:
        return None
    return body // TRACE_STRIDE


def same_name(left, right):
    """True when two mission names differ only in ASCII case.

    A campaign file and the mission map's own name are the same words, and
    the map's file may spell a letter differently. Playback looks the name
    up without regard to case.
    """
    return str(left).casefold() == str(right).casefold()


def player_rows(output, kind):
    """slot -> (units, kills, losses, metal, energy), the last line of that kind."""
    rows = {}
    for match in PLAYER.finditer(output):
        if match.group(1) != kind:
            continue
        rows[int(match.group(2))] = match.group(3, 4, 5, 6, 7)
    return rows


def feature_rows(output):
    """label -> (cells, digest), the last line of each feature label."""
    rows = {}
    for match in FEATURE.finditer(output):
        rows[match.group(1)] = (int(match.group(2)), match.group(3))
    return rows


def tick_zero_units(output):
    """The units the mission's first campaign tick 0 line counted."""
    start = CENSUS.search(output)
    if start is None or start.group(1) != "0" or int(start.group(2)) < 1:
        return None
    return int(start.group(2))


def oa_tool_beside(native):
    """The oa-tool built next to the game, including inside a macOS bundle.

    The macOS game is ``build/open-annihilation.app/Contents/MacOS/open-annihilation``
    and oa-tool is ``build/oa-tool``. A Linux or Windows build keeps both in one
    directory.
    """
    names = ("oa-tool", "oa-tool.exe")
    directory = native.parent
    for _level in range(6):
        for name in names:
            candidate = directory / name
            if candidate.is_file():
                return candidate
        parent = directory.parent
        if parent == directory:
            break
        directory = parent
    raise ScriptFailure("oa-tool was not beside the game at " + str(native))


def run_tool(tool, work, arguments):
    """Runs oa-tool. Returns its status and combined output."""
    result = subprocess.run(
        [str(tool), *arguments],
        cwd=work,
        timeout=RUN_TIMEOUT_SECONDS,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    return result.returncode, result.stdout + result.stderr


def expect_campaign_meta(replay, mission, mission_name):
    """meta.json and manifest.json of a campaign recording."""
    with zipfile.ZipFile(replay) as archive:
        names = archive.namelist()
        meta = json.loads(archive.read("meta.json"))
        manifest = json.loads(archive.read("manifest.json"))
    if names[:3] != ["oademo", "meta.json", "manifest.json"] or "recording.tad.zst" not in names:
        raise ScriptFailure(f"the replay holds the entries {names}")
    recorded = meta.get("recorded", {})
    game = meta.get("game", {})
    recording = meta.get("recording", {})
    campaign = game.get("campaign")
    expected = {"name": CAMPAIGN, "mission": mission, "mission_name": mission_name}
    if (
        recorded.get("how") != "campaign"
        or game.get("kind") != "campaign"
        or campaign != expected
        or not recording.get("addresses_removed")
        or recording.get("packets", 0) < 2
    ):
        raise ScriptFailure(f"meta.json describes the game as {json.dumps(meta)[:800]}")
    map_name = meta.get("map", {}).get("name")
    if not map_name or not same_name(map_name, mission_name):
        raise ScriptFailure(f"the replay names the map {map_name!r}, not {mission_name!r}")
    expansions = meta.get("content", {}).get("expansions", [])
    needs = manifest.get("needs", [])
    if "campaign" not in expansions or not any(
        need.get("kind") == "expansion" and need.get("id") == "campaign" for need in needs
    ):
        raise ScriptFailure(
            f"the replay does not name the campaign expansion ({expansions}, {needs})"
        )
    players = meta.get("players", [])
    if (
        len(players) != 2
        or players[0].get("kind") != "human"
        or not players[0].get("recorded_here")
        or players[1].get("kind") != "computer"
    ):
        raise ScriptFailure(f"meta.json names the players {players}")
    return meta


def expect_clean_playback(native, game_dir, work, replay, label, ticks=PLAY_CEILING):
    """Plays a replay to its end. Returns the output when it is clean and paced."""
    user = work / ("play-" + label)
    preferences = work / ("play-" + label + ".conf")
    write_preferences(preferences)
    print(f"replays campaign: playing {replay.name}", flush=True)
    status, output = run_game(
        native,
        game_dir,
        work,
        [
            "--play-demo",
            str(replay),
            "--demo-unit-table",
            "strict",
            "--match-ticks",
            str(ticks),
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
        ],
    )
    found = DEMO_SUMMARY.search(output)
    finished = DEMO_TICK.search(output)
    if found is None or finished is None or status != 0 or "demo check failed" in output:
        show(output)
        raise ScriptFailure(
            f"playing {replay.name} exited with {status} and was not clean and paced"
        )
    if found.group(2) != "0" or found.group(3) != "0":
        show(output)
        raise ScriptFailure(
            f"playing {replay.name} had {found.group(2)} record errors and "
            f"{found.group(3)} tick errors"
        )
    if int(finished.group(1)) < 2 or int(finished.group(3)) < 2:
        show(output)
        raise ScriptFailure(
            f"playing {replay.name} finished at tick {finished.group(1)} "
            f"after {finished.group(3)} frames"
        )
    if "demo map" in output and "is not installed" in output:
        show(output)
        raise ScriptFailure(f"playing {replay.name} could not find the mission's map")
    return output


def expect_same_end(live, playback, label):
    """The recording's players and feature cells are the replay's."""
    live_rows = player_rows(live, "campaign")
    played_rows = player_rows(playback, "demo")
    if not live_rows or live_rows != {slot: played_rows.get(slot) for slot in live_rows}:
        show(live)
        show(playback)
        raise ScriptFailure(f"{label}: the seats {live_rows} played back as {played_rows}")
    live_features = feature_rows(live)
    played_features = feature_rows(playback)
    paired = (
        ("campaign features at start", "demo features at start"),
        ("campaign features", "demo features"),
    )
    for live_name, played_name in paired:
        if live_name not in live_features or live_features[live_name] != played_features.get(
            played_name
        ):
            raise ScriptFailure(
                f"{label}: {live_name} is {live_features.get(live_name)}; "
                f"the replay's {played_name} is {played_features.get(played_name)}"
            )


def expect_created(native, game_dir, work, replay, units, label):
    """One tick of playback creates the units the mission placed at tick 0."""
    user = work / ("created-" + label)
    preferences = work / ("created-" + label + ".conf")
    write_preferences(preferences)
    status, output = run_game(
        native,
        game_dir,
        work,
        [
            "--play-demo",
            str(replay),
            "--demo-unit-table",
            "strict",
            "--match-ticks",
            "1",
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
        ],
    )
    found = DEMO_SUMMARY.search(output)
    if found is None or "demo check failed: the replay was not clean" in output:
        show(output)
        raise ScriptFailure(f"{label}: one tick of {replay.name} exited with {status}")
    if found.group(2) != "0" or found.group(3) != "0":
        show(output)
        raise ScriptFailure(
            f"{label}: one tick of {replay.name} had {found.group(2)} record errors and "
            f"{found.group(3)} tick errors"
        )
    created = int(found.group(1))
    if created != units:
        show(output)
        raise ScriptFailure(
            f"{label}: the replay created {created} units at tick 1; "
            f"the mission placed {units} at tick 0"
        )
    return created


def mission_arguments(work, mission, ticks, label, extra):
    """The arguments of one campaign run, preferences and user folder included."""
    mission_name, _ticks, orders = MISSIONS[mission]
    user = work / f"mission-{mission}-{label}"
    preferences = work / f"mission-{mission}-{label}.conf"
    write_preferences(preferences)
    arguments = [
        "--campaign",
        CAMPAIGN,
        "--mission",
        str(mission),
        "--match-ticks",
        str(ticks),
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
        *extra,
    ]
    if orders:
        arguments += ["--give-orders", "--seed", "7"]
    return mission_name, arguments


def run_mission(native, game_dir, work, mission, ticks, label, extra):
    """Runs one mission. Returns the output. The trace, when asked, must exist."""
    _name, arguments = mission_arguments(work, mission, ticks, label, extra)
    print(f"replays campaign: mission {mission}, {label}, {ticks} ticks", flush=True)
    status, output = run_game(native, game_dir, work, arguments)
    if status != 0:
        show(output)
        raise ScriptFailure(f"mission {mission} {label} exited with {status}")
    result = RESULT.search(output)
    if result is None or int(result.group(2)) != 0 or int(result.group(3)) != 0:
        show(output)
        raise ScriptFailure(f"mission {mission} {label} did not run clean")
    return output


def expect_same_traces(native, game_dir, work, mission, ticks, stem, past_outcome=False):
    """The mission's trace is the same with recording on and off. Returns both outputs."""
    traces = {}
    outputs = {}
    for label in ("off", "on"):
        trace = work / f"{stem}-{label}.trace"
        extra = ["--trace-digest", str(trace)]
        if past_outcome:
            extra.append("--past-outcome")
        if label == "on":
            extra += ["--record-game", str(work / f"{stem}-on.oademo")]
        output = run_mission(
            native, game_dir, work, mission, ticks, f"{stem}-{label}", extra
        )
        if not trace.is_file():
            show(output)
            raise ScriptFailure(f"mission {mission} {stem} {label} wrote no trace")
        held = trace_ticks(trace)
        if held is None or held < 1:
            raise ScriptFailure(f"the {label} trace of mission {mission} {stem} holds {held} ticks")
        traces[label] = trace.read_bytes()
        outputs[label] = output
        if label == "off":
            folder = work / f"mission-{mission}-{stem}-{label}"
            written = list(folder.rglob("*.oademo")) if folder.is_dir() else []
            if written:
                raise ScriptFailure(
                    f"mission {mission} {stem} without --record-game wrote {written}"
                )
    if traces["off"] != traces["on"]:
        raise ScriptFailure(
            f"mission {mission} {stem}: the traces differ with recording on and off "
            f"({len(traces['off'])} and {len(traces['on'])} bytes)"
        )
    return outputs, len(traces["off"])


def expect_played_file(native, game_dir, work, replay, live, label, ticks, units):
    """The file plays strict to the live end, with the same players, features and creates."""
    playback = expect_clean_playback(native, game_dir, work, replay, label)
    finished = DEMO_TICK.search(playback)
    if int(finished.group(1)) != ticks:
        raise ScriptFailure(
            f"{label}: the replay plays to tick {finished.group(1)}, not {ticks}"
        )
    expect_same_end(live, playback, label)
    created = expect_created(native, game_dir, work, replay, units, label)
    return playback, created


def expect_indexed(native, game_dir, work, replay, end_tick, players, shows):
    """Indexes a finished mission. Won lists the recording player; Lost does not."""
    user = work / "mission-index"
    preferences = work / "mission-0-index.conf"
    output = work / "mission-0-index.json"
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.update-check") + " " + quote_preference("never"),
        quote_preference("open-annihilation.content-updates") + " " + quote_preference("never"),
        quote_preference("open-annihilation.first-run") + " " + quote_preference("done"),
        quote_preference("open-annihilation.replays.record") + " " + quote_preference("off"),
    ]
    preferences.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"replays campaign: indexing {replay.name}", flush=True)
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        "--replay-index",
        str(replay),
        "--output",
        str(output),
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
    ]
    result = subprocess.run(
        command,
        cwd=work,
        env=game_environment(),
        timeout=RUN_TIMEOUT_SECONDS,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    text = result.stdout + result.stderr
    if result.returncode != 0 or not output.is_file():
        show(text)
        raise ScriptFailure(
            f"indexing {replay.name} ended with status {result.returncode}"
        )
    try:
        parsed = json.loads(output.read_text(encoding="utf-8"))
    except (UnicodeError, ValueError) as error:
        show(text)
        raise ScriptFailure(f"the index of {replay.name} is not JSON: {error}") from error
    ended = parsed.get("ended", {})
    if ended.get("how") != "finished" or ended.get("tick") != end_tick:
        show(text)
        raise ScriptFailure(
            f"{replay.name} indexed ended {ended.get('how')} at tick {ended.get('tick')}, "
            f"not finished at {end_tick}"
        )
    result = parsed.get("result", {})
    winners = result.get("winners")
    player = None
    for one in players:
        if one.get("recorded_here"):
            player = one.get("number")
            break
    if (
        result.get("by") != "mission"
        or not isinstance(winners, list)
        or player is None
        or (shows == "Won" and player not in winners)
        or (shows == "Lost" and (player in winners or not winners))
    ):
        raise ScriptFailure(
            f"{replay.name} does not show {shows} for player {player}: {result}"
        )
    print(
        f"replays campaign: {replay.name} indexed, ended finished at tick {ended.get('tick')}, "
        f"shows {shows}"
    )


def expect_mission_zero(native, game_dir, work):
    """Mission 0's 2400-tick run, its own victory trace, and the file of that victory."""
    mission_name = MISSIONS[0][0]
    long_outputs, long_bytes = expect_same_traces(
        native, game_dir, work, 0, MISSIONS[0][1], "long"
    )
    victory = OUTCOME.search(long_outputs["off"])
    if victory is None or victory.group(1) != "victory":
        show(long_outputs["off"])
        raise ScriptFailure("mission 0's 2400-tick run printed no victory")
    victory_tick = int(victory.group(2))
    own_outputs, own_bytes = expect_same_traces(
        native, game_dir, work, 0, victory_tick, "own", past_outcome=True
    )
    own_ticks = (own_bytes - TRACE_HEADER_BYTES) // TRACE_STRIDE
    if own_bytes != TRACE_HEADER_BYTES + victory_tick * TRACE_STRIDE or own_ticks != victory_tick:
        raise ScriptFailure(
            f"mission 0's own trace is {own_bytes} bytes, {own_ticks} ticks, not victory tick "
            f"{victory_tick}"
        )
    user = work / "mission-0-victory"
    replay = user / "mission.oademo"
    live = run_mission(
        native,
        game_dir,
        work,
        0,
        victory_tick,
        "victory",
        ["--record-game", str(replay)],
    )
    if not replay.is_file() or "campaign: recorded the game to " not in live:
        show(live)
        raise ScriptFailure("mission 0's victory recording wrote no replay")
    units = tick_zero_units(live)
    if units is None:
        show(live)
        raise ScriptFailure("mission 0 placed no units at the start")
    meta = expect_campaign_meta(replay, 0, mission_name)
    game = meta.get("game", {})
    if game.get("ended") != "finished" or game.get("length_ticks") != victory_tick:
        raise ScriptFailure(
            f"mission 0 ended at tick {victory_tick}, and the replay says "
            f"{game.get('ended')} length {game.get('length_ticks')}"
        )
    if game.get("campaign", {}).get("mission") != 0:
        raise ScriptFailure(f"mission 0's replay is mission {game.get('campaign')}")
    _playback, created = expect_played_file(
        native, game_dir, work, replay, live, "mission-0", victory_tick, units
    )
    expect_indexed(
        native, game_dir, work, replay, victory_tick, meta.get("players", []), "Won"
    )
    print(
        f"replays campaign: {CAMPAIGN} mission 0 ({mission_name}), victory at tick {victory_tick}, "
        f"the same {own_bytes}-byte trace with recording on and off; "
        f"the {MISSIONS[0][1]}-tick run's trace matches too ({long_bytes} bytes); "
        f"{replay.name} plays to tick {victory_tick}, ended finished, "
        f"with the mission's players and features, and creates {created} units"
    )
    return victory_tick


def expect_mission_eight(native, game_dir, work):
    """Mission 8's recording, its end, and the .tad round trip."""
    mission_name, ticks, _orders = MISSIONS[8]
    outputs = {}
    replay = None
    for label in ("off", "on"):
        user = work / f"mission-8-{label}"
        trace = work / f"mission-8-{label}.trace"
        extra = ["--trace-digest", str(trace)]
        if label == "on":
            extra += ["--record-game", str(user / "mission.oademo")]
        output = run_mission(native, game_dir, work, 8, ticks, label, extra)
        if not trace.is_file():
            show(output)
            raise ScriptFailure(f"mission 8 with recording {label} wrote no trace")
        outputs[label] = (output, trace.read_bytes())
        if label == "off":
            written = list(user.rglob("*.oademo"))
            if written:
                raise ScriptFailure(f"mission 8 without --record-game wrote {written}")
        else:
            replay = user / "mission.oademo"
            if not replay.is_file() or "campaign: recorded the game to " not in output:
                show(output)
                raise ScriptFailure("mission 8 with --record-game wrote no replay")
    if outputs["off"][1] != outputs["on"][1]:
        raise ScriptFailure(
            f"mission 8: the traces differ with recording on and off "
            f"({len(outputs['off'][1])} and {len(outputs['on'][1])} bytes)"
        )
    live = outputs["on"][0]
    result = RESULT.search(live)
    outcome = result.group(4)
    outcome_tick = int(result.group(5))
    ended = "finished" if outcome in ("victory", "defeat") else "left"
    meta = expect_campaign_meta(replay, 8, mission_name)
    game = meta.get("game", {})
    if game.get("ended") != ended or game.get("length_ticks") != outcome_tick:
        raise ScriptFailure(
            f"mission 8 ended {outcome} at tick {outcome_tick}, and the replay says "
            f"{game.get('ended')} length {game.get('length_ticks')}"
        )
    advances = list(ADVANCE.finditer(live))
    if advances:
        raise ScriptFailure(f"mission 8 advanced to mission {advances[-1].group(1)}")
    units = tick_zero_units(live)
    if units is None:
        show(live)
        raise ScriptFailure("mission 8 placed no units at the start")
    length = game.get("length_ticks")
    _playback, created = expect_played_file(
        native, game_dir, work, replay, live, "mission-8", length, units
    )
    if outcome != "defeat":
        raise ScriptFailure(f"mission 8 ended {outcome}, not a defeat")
    expect_indexed(native, game_dir, work, replay, length, meta.get("players", []), "Lost")
    trace_bytes = len(outputs["off"][1])
    print(
        f"replays campaign: {CAMPAIGN} mission 8 ({mission_name}), "
        f"map {meta.get('map', {}).get('name')!r}, "
        f"the same {trace_bytes}-byte trace with recording on and off; "
        f"{replay.name} plays to tick {length}, ended {ended}, "
        f"with the mission's players and features, and creates {created} units"
    )
    return replay, length


def expect_tad_round_trip(native, game_dir, work, tool, oademo, length):
    """A campaign .tad, an unpacked one, and one packed back, each play strict."""
    recorded = work / "mission.tad"
    net_recorded = work / "mission-net.tad"
    run_mission(
        native,
        game_dir,
        work,
        8,
        MISSIONS[8][1],
        "tad",
        ["--record-game", str(recorded)],
    )
    run_mission(
        native,
        game_dir,
        work,
        8,
        MISSIONS[8][1],
        "net-tad",
        ["--net-record", str(net_recorded)],
    )
    unpacked = work / "unpacked.tad"
    status, output = run_tool(
        tool, work, ["replay", "unpack", str(oademo), "--out", str(unpacked)]
    )
    if status != 0 or not unpacked.is_file():
        show(output)
        raise ScriptFailure(f"unpacking {oademo.name} exited with {status}")
    packed = work / "packed.oademo"
    status, output = run_tool(
        tool, work, ["replay", "pack", str(unpacked), "--out", str(packed)]
    )
    if status != 0 or not packed.is_file():
        show(output)
        raise ScriptFailure(f"packing {unpacked.name} exited with {status}")
    files = (
        ("record-game", recorded),
        ("net-record", net_recorded),
        ("unpacked", unpacked),
        ("packed", packed),
    )
    for label, path in files:
        if not path.is_file():
            raise ScriptFailure(f"the {label} recording {path.name} was not written")
        playback = expect_clean_playback(native, game_dir, work, path, "tad-" + label)
        finished = DEMO_TICK.search(playback)
        if int(finished.group(1)) != length:
            raise ScriptFailure(
                f"the {label} recording plays to tick {finished.group(1)}, not {length}"
            )
    print(
        "replays campaign: a .tad from --record-game and from --net-record, "
        "the .tad unpacked from the .oademo, and that .tad packed back, "
        f"each play strict to tick {length}"
    )


def expect_away_pause(native, game_dir, work):
    """Losing the window's focus pauses mission 0 while it is recorded."""
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
            "--campaign",
            CAMPAIGN,
            "--mission",
            "0",
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
    started = f"campaign start: {CAMPAIGN} mission 0"
    if (
        result.returncode != 0
        or AWAY_HELD not in output
        or AWAY_STEP not in output
        or started not in output
        or not replay.is_file()
    ):
        show(output)
        raise ScriptFailure(
            f"away pause on mission 0 exited with {result.returncode}, "
            f"started the mission: {started in output}, "
            f"its lines say mission: {AWAY_HELD in output and AWAY_STEP in output}, "
            f"and the replay exists: {replay.is_file()}"
        )
    print("replays campaign: losing the focus pauses a recorded mission")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("replays campaign: skipped, no installed game")
        return 77
    native = args.native.resolve()
    tool = oa_tool_beside(native)
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="native-replays-campaign-", dir=scratch_root
    ) as temporary:
        work = Path(temporary)
        try:
            expect_mission_zero(native, game_dir, work)
            oademo, length = expect_mission_eight(native, game_dir, work)
            expect_tad_round_trip(native, game_dir, work, tool, oademo, length)
            expect_away_pause(native, game_dir, work)
        except ScriptFailure as failure:
            print(f"replays campaign: FAILED: {failure}")
            return 1
    print("replays campaign: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
