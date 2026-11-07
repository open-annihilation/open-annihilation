#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the index pass: --replay-index RECORDING --output INDEX.

The game first records a network game of its own: its in-process loopback
check (--net-loopback-check) plays one headless over 127.0.0.1 and
--net-record keeps it. Then:

- the recording is indexed twice, and the two indexes are the same bytes;
- the index's length is the last tick of the same recording played with
  --play-demo (its last `demo tick` line);
- the whole game's index says it ended `finished` or `left`, not `cut` (its
  commanders decide the loopback game);
- a copy of the recording cut at half its packets is indexed, and its index
  says the replay ended `cut`, on a tick before the end of the whole game,
  with that tick as its length.

Every file is written under a scratch folder of its own, and the check sets
every preference it reads, so a second run starts from the same state.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds a run may take.
RUN_TIMEOUT = 600
# Ticks of the recorded network game.
RECORDED_TICKS = 900
# More ticks than the game has, so that a playback runs to its end.
PLAY_TICKS = 100000
# The version of the index this check reads.
INDEX_VERSION = 1
RECORDED = re.compile(r"^multiplayer: recorded the game to (.+)$", re.MULTILINE)
LAST_TICK = re.compile(r"^demo tick (\d+): digest [0-9a-f]{16},", re.MULTILINE)
CHUNK_LENGTH_BYTES = 2
# A recording's header chunk: the magic, a u16 version, then the player count.
HEADER_PLAYER_COUNT_AT = 10


class IndexFailure(Exception):
    """A behaviour that differs from the expected one."""


def environment():
    return dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                SDL_RENDER_DRIVER="software")


def common_arguments(game_dir, workdir, name):
    return ["--game-dir", str(game_dir), "--skip-intro", "--mute",
            "--preferences-file", str(workdir / f"{name}.conf"),
            "--user-folder", str(workdir / f"{name}-user")]


def run(native, arguments, workdir, name):
    """Runs the game on the given arguments; returns the finished process."""
    result = subprocess.run(
        [*RUNNER, str(native), *arguments], cwd=workdir, env=environment(), timeout=RUN_TIMEOUT,
        check=False, capture_output=True, text=True, errors="replace")
    (workdir / f"{name}.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    return result


def record(native, game_dir, workdir):
    """Records a network game the game plays with itself; returns the recording."""
    recording = workdir / "loopback.tad"
    result = run(native, [*common_arguments(game_dir, workdir, "record"), "--headless-check",
                          "--net-loopback-check", str(RECORDED_TICKS), "--net-record",
                          str(recording)], workdir, "record")
    if result.returncode != 0 or not recording.is_file() or not RECORDED.search(result.stdout):
        raise IndexFailure(f"the loopback game was not recorded (status {result.returncode}); "
                           f"see {workdir / 'record.txt'}")
    return recording


def last_tick(native, game_dir, workdir, recording, name):
    """Plays a recording to its end with --play-demo; returns the tick its last line names."""
    result = run(native, [*common_arguments(game_dir, workdir, name), "--headless-check",
                          "--play-demo", str(recording), "--match-ticks", str(PLAY_TICKS)],
                 workdir, name)
    ticks = LAST_TICK.findall(result.stdout)
    if not ticks:
        raise IndexFailure(f"the playback of {recording.name} printed no tick; "
                           f"see {workdir / (name + '.txt')}")
    return int(ticks[-1])


def index(native, game_dir, workdir, recording, name):
    """Indexes a recording; returns the index's bytes."""
    output = workdir / f"{name}.json"
    result = run(native, [*common_arguments(game_dir, workdir, name), "--replay-index",
                          str(recording), "--output", str(output)], workdir, name)
    if result.returncode != 0 or not output.is_file():
        raise IndexFailure(f"indexing {recording.name} ended with status {result.returncode}; "
                           f"see {workdir / (name + '.txt')}")
    return output.read_bytes()


def cut_at_half(recording, destination):
    """Writes a copy of a recording holding the first half of its packets; returns how many it
    kept and how many there were."""
    data = recording.read_bytes()

    def chunk(at):
        if at + CHUNK_LENGTH_BYTES > len(data):
            raise IndexFailure(f"{recording.name} ends inside a chunk")
        length = struct.unpack_from("<H", data, at)[0]
        if length < CHUNK_LENGTH_BYTES or at + length > len(data):
            raise IndexFailure(f"{recording.name} holds a chunk that runs past its end")
        return at + length

    header_end = chunk(0)
    players = data[CHUNK_LENGTH_BYTES + HEADER_PLAYER_COUNT_AT]
    count_end = chunk(header_end)
    sectors = struct.unpack_from("<I", data, header_end + CHUNK_LENGTH_BYTES)[0]
    at = count_end
    # The sectors, the players, their statuses and the unit checks.
    for _ in range(sectors + 2 * players + 1):
        at = chunk(at)
    packets = [at]
    while packets[-1] < len(data):
        packets.append(chunk(packets[-1]))
    total = len(packets) - 1
    kept = total // 2
    destination.write_bytes(data[:packets[kept]])
    return kept, total


def read_index(content, what):
    try:
        parsed = json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise IndexFailure(f"the index of {what} is not JSON: {error}") from error
    if parsed.get("oademo_timeline") != INDEX_VERSION:
        raise IndexFailure(f"the index of {what} names version {parsed.get('oademo_timeline')}")
    return parsed


def check(native, game_dir, workdir):
    """Runs every step; returns how the whole game ended, the packets the cut copy kept, how many
    there were, how the copy ended, the tick it ended on and the whole game's last tick."""
    recording = record(native, game_dir, workdir)
    first = index(native, game_dir, workdir, recording, "first")
    second = index(native, game_dir, workdir, recording, "second")
    if first != second:
        raise IndexFailure("two indexes of the same recording differ; "
                           f"see {workdir / 'first.json'} and {workdir / 'second.json'}")
    whole = read_index(first, recording.name)
    played = last_tick(native, game_dir, workdir, recording, "play")
    if whole["length"]["ticks"] != played:
        raise IndexFailure(f"the index's length is {whole['length']['ticks']} ticks, and the "
                           f"playback's last tick is {played}")
    if whole["ended"]["tick"] != played or whole["ended"]["how"] not in ("finished", "left", "cut"):
        raise IndexFailure(f"the index says the replay ended {whole['ended']}")
    # The loopback game is decided by its commanders, so the whole game is not cut: an index that
    # calls every game cut would pass the copy's check below.
    if whole["ended"]["how"] == "cut":
        raise IndexFailure(f"the whole game, which its commanders decide, ended cut on tick "
                           f"{whole['ended']['tick']}")
    cut_recording = workdir / "cut.tad"
    kept, total = cut_at_half(recording, cut_recording)
    cut = read_index(index(native, game_dir, workdir, cut_recording, "cut"), cut_recording.name)
    ended = cut["ended"]
    if ended["how"] != "cut":
        raise IndexFailure(f"the copy cut at {kept} of {total} packets ended {ended['how']}, "
                           "not cut")
    if not 0 < ended["tick"] < played or cut["length"]["ticks"] != ended["tick"]:
        raise IndexFailure(f"the copy cut at {kept} of {total} packets ended on tick "
                           f"{ended['tick']} with a length of {cut['length']['ticks']}, before "
                           f"the whole game's {played}")
    return whole["ended"]["how"], kept, total, ended["how"], ended["tick"], played


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if not args.game_dir.is_dir():
        print(f"native-replay-index: no installed game at {args.game_dir}; skipped")
        return 77
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="replay-index-", dir=args.scratch_root))
    try:
        whole_how, kept, total, cut_how, cut_tick, played = check(args.native, args.game_dir,
                                                                  workdir)
    except (IndexFailure, subprocess.TimeoutExpired, OSError) as failure:
        print(f"native-replay-index: {failure}; the runs are in {workdir}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print(f"native-replay-index: a {played}-tick game ended {whole_how} with two indexes of the "
          f"same bytes, and a copy cut at {kept} of {total} packets ended {cut_how} on tick "
          f"{cut_tick}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
