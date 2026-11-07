#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A network game kept as a replay, with nothing of it sent to the other players.

Five runs of the headless loopback game, each in a scratch user folder:

1. With the setting on and `--record-game` naming a file in Recordings/default,
   the game writes one `.oademo` there and nothing else but its content note.
   `--play-demo` plays it to its end with no record error and no tick error, and
   its meta.json names both players.
2. The same game (with a computer player, whose trace is the same on every run
   of one `--seed`) with the setting on and off writes the same `--trace-digest`
   stream, and the setup blocks present the same recorder (none): the setting
   changes nothing the other machine reads.
3. A game killed part way leaves its recording in Recordings/.replays/partial.
   The next start finishes it into Recordings/default with `recorded.after_crash`
   true, and it plays clean up to where it was cut.
4. Under a profile whose recorder hack is on, a `.tad` is written and no
   `.oademo`, and the `.tad` holds no sector of network addresses.
5. A headless run without `--record-game` writes no Recordings folder.
6. A replay of a game played with a mod names the mod in its manifest by a
   64-digit SHA-256 whatever the mod: the digest of its folder (computed here
   from the folder's files) for a mod folder, and a digest all the same for a
   profile no folder holds.

The scratch folders are removed whatever happens, so a second run starts as the
first did. The script exits 77 when no game data is installed.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time
import zipfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

RUN_TIMEOUT_SECONDS = 900
KILL_WAIT_SECONDS = 240
PREFERENCES_HEADER = "open-annihilation-preferences 1"
LOOPBACK_TICKS = 3000
# The computer-player loopback reads every record one machine sent before the other plays its
# tick, so its trace is the same on every run with one seed; the plain loopback's depends on
# when the other machine's records arrive.
COMPUTER_TICKS = 6000
SEED = "20261106"
# The sector the recorder keeps a player's address in, which no recording of the game holds.
ADDRESS_SECTOR = 6
DEMO_SUMMARY = re.compile(r"demo summary: .*record errors (\d+), tick errors (\d+)")
PRESENTING = re.compile(r"setup blocks presenting a recorder: (\d+) / (\d+)")
PRESENCE = re.compile(r"setup blocks carry no presence bytes: (\d+) / (\d+)")
INSTANT = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z")

RECORDER_PROFILE = """oamod: 1
id: loopback-recorder
name: Loopback recorder
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-11-06, packager: Open Annihilation}
hacks:
  recorder.ta-demo-recorder: true
"""


FOLDER_PROFILE = """oamod: 1
id: loopback-ridge
name: Loopback Ridge
version: "2.8"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-11-06, packager: Open Annihilation}
"""


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_preferences(path, record):
    """A preferences file with the setting on or off, and nothing else that asks a question."""
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.content-updates") + " " + quote_preference("never"),
        quote_preference("open-annihilation.update-check") + " " + quote_preference("never"),
        quote_preference("open-annihilation.first-run") + " " + quote_preference("done"),
        quote_preference("open-annihilation.replays.record")
        + " "
        + quote_preference("on" if record else "off"),
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


def game_command(native, game_dir, arguments, headless=True):
    """The command line of one run of the game, mute, and headless unless the run is a check of its own."""
    return [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        *(["--headless-check"] if headless else []),
        *arguments,
    ]


def run_game(native, game_dir, work, arguments, headless=True):
    """Runs the game to its end. Returns its status and combined output."""
    result = subprocess.run(
        game_command(native, game_dir, arguments, headless),
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


def loopback_arguments(user, preferences, extra, ticks=LOOPBACK_TICKS):
    """A headless loopback game in a scratch user folder."""
    return [
        "--net-loopback-check",
        str(ticks),
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
        "--seed",
        SEED,
        *extra,
    ]


def files_below(folder):
    """The files below a folder, relative to it, sorted."""
    if not folder.is_dir():
        return []
    return sorted(
        str(path.relative_to(folder)) for path in folder.rglob("*") if path.is_file()
    )


def read_meta(replay):
    """The description of a .oademo, and the names of its entries in order."""
    with zipfile.ZipFile(replay) as archive:
        names = archive.namelist()
        meta = json.loads(archive.read("meta.json"))
    return meta, names


def play_clean(native, game_dir, work, replay, label):
    """Plays a replay to its end. Raises ScriptFailure unless no record and no tick failed."""
    user = work / ("play-" + label)
    status, output = run_game(native, game_dir, work, [
        "--play-demo",
        str(replay),
        "--demo-unit-table",
        "strict",
        "--match-ticks",
        "400000",
        "--user-folder",
        str(user),
        "--preferences-file",
        str(work / ("play-" + label + ".conf")),
    ])
    found = DEMO_SUMMARY.search(output)
    if found is None or status != 0:
        show(output)
        raise ScriptFailure(f"playing {replay.name} exited with {status} and no clean summary")
    if found.group(1) != "0" or found.group(2) != "0":
        show(output)
        raise ScriptFailure(
            f"playing {replay.name} had {found.group(1)} record errors and "
            f"{found.group(2)} tick errors"
        )


def tad_sectors(path):
    """The types of the sectors of a .tad recording, in order."""
    data = path.read_bytes()
    chunks = []
    position = 0
    while position + 2 <= len(data):
        length = int.from_bytes(data[position:position + 2], "little")
        if length < 2 or position + length > len(data):
            break
        chunks.append(data[position + 2:position + length])
        position += length
    if len(chunks) < 2 or len(chunks[1]) < 4:
        raise ScriptFailure(f"{path.name} has no sector count")
    count = int.from_bytes(chunks[1][:4], "little")
    types = []
    for body in chunks[2:2 + count]:
        if len(body) < 4:
            raise ScriptFailure(f"{path.name} has a short sector")
        types.append(int.from_bytes(body[:4], "little"))
    return types


def expect_a_replay(native, game_dir, work):
    """Scenario 1: the setting on and --record-game writes one .oademo that plays clean."""
    user = work / "one-user"
    preferences = work / "one.conf"
    write_preferences(preferences, True)
    target = user / "Recordings" / "default" / "loopback.oademo"
    status, output = run_game(native, game_dir, work, loopback_arguments(
        user, preferences, ["--record-game", str(target)]
    ))
    if status != 0 or not target.is_file():
        show(output)
        raise ScriptFailure(f"the recorded loopback exited with {status} and wrote no replay")
    kept = files_below(user / "Recordings")
    expected = ["default/loopback.oademo", "default/loopback.oademo.oa-content"]
    if kept != expected:
        show(output)
        raise ScriptFailure(f"the recordings folder holds {kept}, not {expected}")
    meta, names = read_meta(target)
    if names[:3] != ["oademo", "meta.json", "manifest.json"] or "recording.tad.zst" not in names:
        raise ScriptFailure(f"the replay holds the entries {names}")
    players = [player.get("name", "") for player in meta.get("players", [])]
    if len(players) != 2 or "" in players or len(set(players)) != 2:
        raise ScriptFailure(f"meta.json names the players {players}, not two")
    recorded = meta.get("recorded", {})
    if (
        recorded.get("how") != "multiplayer"
        or recorded.get("after_crash") is not False
        or not INSTANT.fullmatch(recorded.get("at", ""))
        or meta.get("game", {}).get("kind") != "multiplayer"
        or not meta.get("recording", {}).get("addresses_removed")
    ):
        raise ScriptFailure(f"meta.json describes the game as {json.dumps(meta)[:400]}")
    play_clean(native, game_dir, work, target, "one")
    print(f"replays record: {target.name} names {players[0]} and {players[1]} and plays clean")


def expect_the_wire_unchanged(native, game_dir, work):
    """Scenario 2: the setting on and off, with the same seed, leaves the game the same."""
    results = []
    for label, record in (("off", False), ("on", True)):
        user = work / f"wire-{label}-user"
        preferences = work / f"wire-{label}.conf"
        write_preferences(preferences, record)
        trace = work / f"wire-{label}.trace"
        extra = ["--trace-digest", str(trace)]
        if record:
            extra += ["--record-game", str(user / "Recordings" / "default" / "wire.oademo")]
        status, output = run_game(native, game_dir, work, [
            *loopback_arguments(user, preferences, extra, COMPUTER_TICKS),
            "--net-loopback-computer",
        ])
        presenting = PRESENTING.search(output)
        presence = PRESENCE.search(output)
        if status != 0 or presenting is None or presence is None or not trace.is_file():
            show(output)
            raise ScriptFailure(f"the loopback with the setting {label} exited with {status}")
        if record and not (user / "Recordings" / "default" / "wire.oademo").is_file():
            show(output)
            raise ScriptFailure("the loopback with the setting on wrote no replay")
        if not record and (user / "Recordings").exists():
            show(output)
            raise ScriptFailure("the loopback with the setting off wrote a Recordings folder")
        results.append((trace.read_bytes(), presenting.group(0), presence.group(0)))
    (off_trace, off_presenting, off_presence), (on_trace, on_presenting, on_presence) = results
    if not off_trace or off_trace != on_trace:
        raise ScriptFailure(
            f"the trace digests differ with the setting on and off "
            f"({len(off_trace)} and {len(on_trace)} bytes)"
        )
    if off_presenting != on_presenting or off_presence != on_presence:
        raise ScriptFailure(
            f"the setup blocks differ: {off_presenting!r} and {on_presenting!r}, "
            f"{off_presence!r} and {on_presence!r}"
        )
    presented, blocks = (int(count) for count in re.findall(r"\d+", off_presenting))
    if presented != 0 or blocks == 0:
        raise ScriptFailure(f"the game with no profile presents a recorder: {off_presenting!r}")
    print(
        f"replays record: the trace is the same {len(off_trace)} bytes with the setting on and "
        "off; no setup block presents a recorder"
    )


def partial_streams(user):
    """The recordings being made under a user folder, as (stream, size) pairs."""
    root = user / "Recordings" / ".replays" / "partial"
    if not root.is_dir():
        return []
    return [(path, path.stat().st_size) for path in sorted(root.rglob("*.part"))]


def expect_a_crash_finished(native, game_dir, work):
    """Scenario 3: a killed game leaves its recording, and the next start finishes it."""
    user = work / "crash-user"
    preferences = work / "crash.conf"
    write_preferences(preferences, True)
    target = user / "Recordings" / "default" / "killed.oademo"
    arguments = loopback_arguments(user, preferences, ["--record-game", str(target)])
    # A game long enough that it is still being played when it is killed.
    arguments[arguments.index("--net-loopback-check") + 1] = "400000"
    log = work / "crash-game.log"
    with log.open("w", encoding="utf-8", errors="replace") as written:
        game = subprocess.Popen(
            game_command(native, game_dir, arguments),
            cwd=work,
            env=game_environment(),
            stdout=written,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + KILL_WAIT_SECONDS
            grown = 0
            while time.monotonic() < deadline and game.poll() is None:
                streams = partial_streams(user)
                if streams and streams[0][1] > 40000:
                    grown = streams[0][1]
                    break
                time.sleep(0.25)
            if game.poll() is not None or grown == 0:
                show(log.read_text(encoding="utf-8", errors="replace"))
                raise ScriptFailure("the recording did not grow while the game was played")
        finally:
            game.kill()
            game.wait()
    output = log.read_text(encoding="utf-8", errors="replace")
    if target.exists():
        show(output)
        raise ScriptFailure("a killed game wrote its replay")
    # Both machines of the loopback record: each leaves a recording of its own.
    left = files_below(user / "Recordings" / ".replays" / "partial")
    streams = sorted(name for name in left if name.endswith(".part"))
    for stream in streams:
        stem = stream[: -len(".part")]
        for suffix in (".start", ".lock"):
            if stem + suffix not in left:
                show(output)
                raise ScriptFailure(f"the killed game left {left}, with no {stem + suffix}")
    if not streams:
        show(output)
        raise ScriptFailure(f"the killed game left {left}")
    # The next start, of any run, finishes them.
    status, started = run_game(native, game_dir, work, [
        "--check-recording-hook",
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
    ], headless=False)
    finished = sorted((user / "Recordings" / "default").glob("*.oademo"))
    if status != 0 or len(finished) != len(streams) or "finished the recording" not in started:
        show(started)
        raise ScriptFailure(
            f"the next start exited with {status} and finished {len(finished)} of "
            f"{len(streams)} recordings"
        )
    if files_below(user / "Recordings" / ".replays" / "partial"):
        raise ScriptFailure("the files of the finished recordings are still being kept")
    packets = 0
    for index, replay in enumerate(finished):
        meta, _ = read_meta(replay)
        if (
            meta.get("recorded", {}).get("after_crash") is not True
            or meta.get("game", {}).get("ended") != "cut"
            or len(meta.get("players", [])) != 2
            or meta.get("recording", {}).get("packets", 0) < 1
        ):
            raise ScriptFailure(
                f"meta.json describes the finished {replay.name} as {json.dumps(meta)[:400]}"
            )
        packets = max(packets, meta["recording"]["packets"])
        if not Path(str(replay) + ".oa-content").is_file():
            raise ScriptFailure(f"{replay.name} has no content note")
        play_clean(native, game_dir, work, replay, f"crash-{index}")
    print(
        f"replays record: a game killed at {grown} bytes was finished as {len(finished)} replays "
        f"of up to {packets} packets, cut, after a crash, and each plays clean"
    )


def expect_the_hack_layout(native, game_dir, work):
    """Scenario 4: under a profile with the recorder hack, a .tad and no .oademo."""
    user = work / "hack-user"
    preferences = work / "hack.conf"
    write_preferences(preferences, True)
    profile = work / "loopback-recorder.oamod.yaml"
    profile.write_text(RECORDER_PROFILE, encoding="utf-8")
    status, output = run_game(native, game_dir, work, [
        *loopback_arguments(user, preferences, []),
        "--mod",
        str(profile),
    ])
    recordings = user / "Recordings"
    tads = sorted(recordings.rglob("*.tad")) if recordings.is_dir() else []
    replays = sorted(recordings.rglob("*.oademo")) if recordings.is_dir() else []
    if status != 0 or not tads:
        show(output)
        raise ScriptFailure(f"the recorder hack's loopback exited with {status} and wrote no .tad")
    if replays:
        raise ScriptFailure(f"the recorder hack's game also wrote {[path.name for path in replays]}")
    for tad in tads:
        types = tad_sectors(tad)
        if ADDRESS_SECTOR in types:
            raise ScriptFailure(f"{tad.name} holds a sector of network addresses")
    print(f"replays record: the recorder hack wrote {len(tads)} .tad and no .oademo, no address sector")


def expect_no_folder_for_a_headless_run(native, game_dir, work):
    """Scenario 5: a headless game without --record-game writes no Recordings folder."""
    user = work / "none-user"
    preferences = work / "none.conf"
    write_preferences(preferences, True)
    status, output = run_game(native, game_dir, work, [
        "--net-loopback-check",
        "300",
        "--user-folder",
        str(user),
        "--preferences-file",
        str(preferences),
    ])
    if status != 0:
        show(output)
        raise ScriptFailure(f"the headless loopback exited with {status}")
    if (user / "Recordings").exists():
        show(output)
        raise ScriptFailure(f"a headless game wrote {files_below(user / 'Recordings')}")
    print("replays record: a headless game without --record-game writes no Recordings folder")


def folder_digest(folder):
    """The digest of a package folder, as the game makes it: each file's path, a NUL, its size as
    eight little-endian bytes and the SHA-256 of its bytes, the files in the order of their paths."""
    files = sorted(
        (str(path.relative_to(folder)).replace(os.sep, "/"), path)
        for path in folder.rglob("*")
        if path.is_file()
    )
    message = bytearray()
    for relative, path in files:
        data = path.read_bytes()
        message += relative.encode("utf-8") + b"\0" + len(data).to_bytes(8, "little")
        message += hashlib.sha256(data).digest()
    return hashlib.sha256(bytes(message)).hexdigest()


def manifest_needs(replay):
    """The needs of a .oademo's manifest."""
    with zipfile.ZipFile(replay) as archive:
        return json.loads(archive.read("manifest.json")).get("needs", [])


def expect_every_need_has_a_digest(native, game_dir, work):
    """Scenario 6: a replay of a mod game names the mod by a digest, a folder's or the profile's."""
    folder = work / "mods" / "loopback-ridge"
    folder.mkdir(parents=True)
    (folder / "oamod.yaml").write_text(FOLDER_PROFILE, encoding="utf-8")
    (folder / "notes.txt").write_text("a file of the mod folder\n", encoding="utf-8")
    profile = work / "profile-only.oamod.yaml"
    profile.write_text(FOLDER_PROFILE.replace("loopback-ridge", "profile-only"), encoding="utf-8")
    for label, option, mod_id, expected in (
        ("folder", ["--mod-dir", str(folder)], "loopback-ridge", folder_digest(folder)),
        ("profile", ["--mod", str(profile)], "profile-only", None),
    ):
        user = work / f"digest-{label}-user"
        preferences = work / f"digest-{label}.conf"
        write_preferences(preferences, True)
        target = user / "Recordings" / mod_id / f"{label}.oademo"
        status, output = run_game(native, game_dir, work, [
            *loopback_arguments(user, preferences, ["--record-game", str(target), *option], 600),
        ])
        if status != 0 or not target.is_file():
            show(output)
            raise ScriptFailure(f"the {label} mod's loopback exited with {status} and wrote no replay")
        needs = manifest_needs(target)
        mods = [need for need in needs if need.get("kind") == "oamod"]
        if len(mods) != 1 or mods[0].get("id") != mod_id:
            raise ScriptFailure(f"the {label} mod's manifest holds the needs {json.dumps(needs)}")
        digest = mods[0].get("sha256", "")
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ScriptFailure(f"the {label} mod's need has the digest {digest!r}")
        if expected is not None and digest != expected:
            raise ScriptFailure(f"the {label} mod's need has the digest {digest}, not the folder's {expected}")
        for need in needs:
            if need.get("kind") in ("oamod", "oamap", "oamut") and not re.fullmatch(
                r"[0-9a-f]{64}", need.get("sha256", "")
            ):
                raise ScriptFailure(f"{need.get('kind')} {need.get('id')} has no digest")
        play_clean(native, game_dir, work, target, "digest-" + label)
    print("replays record: a mod folder's replay names the folder's digest, a profile's its own; every need has one")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("replays record: skipped, no installed game")
        return 77
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-replays-record-", dir=scratch_root) as temporary:
        work = Path(temporary)
        try:
            expect_a_replay(native, game_dir, work)
            expect_the_wire_unchanged(native, game_dir, work)
            expect_a_crash_finished(native, game_dir, work)
            expect_the_hack_layout(native, game_dir, work)
            expect_no_folder_for_a_headless_run(native, game_dir, work)
            expect_every_need_has_a_digest(native, game_dir, work)
        except ScriptFailure as failure:
            print(f"replays record: FAILED: {failure}")
            return 1
    print("replays record: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
