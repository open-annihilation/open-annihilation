#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the Library and the tools take one replay, whatever its length.

The replay named by --replay is placed in a scratch player's Recordings
folder (a hard link where the file system allows one, otherwise a copy), and
the game walks it through the Library (--check-library-replay): the Library
lists it, its own index pass gives it a timeline, Watch plays it to the
middle, Watch from jumps back there by the replay's own jump and plays on to
its end with no error, and Export writes it as a .oademo of the same
recording and as a .tad, each the size the dialog measured. The index pass
is also run alone (--replay-index), as the Library starts it, and its peak
memory taken. Then `oa-tool replay info` and the recording dump read the
replay. Every line the check depends on is printed, with each run's peak
memory, so a run on a long replay can be quoted.

Every file is written under a scratch folder of its own, emptied as the check
starts, and the check sets every preference it reads, so a second run starts
from the same state. Nothing is fetched.
"""
import argparse
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import subprocess
import sys
import time
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds the Library walk may take: a replay of hours plays for many minutes.
WALK_TIMEOUT = 4 * 60 * 60
# Seconds a tool run may take.
TOOL_TIMEOUT = 60 * 60
LIBRARY_LINE = re.compile(r"^library replay: .*$", re.MULTILINE)
PLAYED = re.compile(r"^library replay: played to tick (\d+), finished yes, errors (\d+),", re.MULTILINE)
TIMELINE = re.compile(r"^library replay: timeline of (\d+) ticks,", re.MULTILINE)
JUMPED = re.compile(r"^library replay: Watch from jumped to tick (\d+) ", re.MULTILINE)
EXPORTED = re.compile(r"^library replay: exported (\.oademo|\.tad) (\d+) bytes", re.MULTILINE)
PEAK = re.compile(r"^library replay: peak memory (\d+) bytes$", re.MULTILINE)


class WalkFailure(Exception):
    """A behaviour that differs from the expected one."""


def environment():
    return dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                SDL_RENDER_DRIVER="software")


def peak_of_children():
    """The largest peak memory of the children waited for so far, in bytes."""
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return peak if sys.platform == "darwin" else peak * 1024


def run(command, workdir, name, timeout):
    """Runs a command, keeps its output beside the run, and returns the finished process
    with the time it took."""
    started = time.monotonic()
    result = subprocess.run(command, cwd=workdir, env=environment(), timeout=timeout,
                            check=False, capture_output=True, text=True, errors="replace")
    seconds = time.monotonic() - started
    (workdir / f"{name}.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    return result, seconds


def place(replay, recordings):
    """Puts the replay below Recordings/default; returns its path below Recordings."""
    folder = recordings / "default"
    folder.mkdir(parents=True, exist_ok=True)
    placed = folder / replay.name
    try:
        os.link(replay, placed)
    except OSError:
        shutil.copyfile(replay, placed)
    return f"default/{replay.name}"


def game_arguments(game_dir, workdir, name, user):
    preferences = workdir / f"{name}.conf"
    preferences.write_text(native_offline.preferences_text()
                           + '"open-annihilation.language" "en"\n'
                           + '"open-annihilation.first-run" "done"\n', encoding="utf-8")
    return ["--game-dir", str(game_dir), "--skip-intro", "--mute",
            "--preferences-file", str(preferences), "--user-folder", str(user)]


def check(args):
    replay = Path(args.replay).resolve()
    if not replay.is_file():
        raise WalkFailure(f"{replay} is not there")
    workdir = Path(args.scratch_root).resolve() / "library-replays-walk"
    if workdir.exists():
        shutil.rmtree(workdir)
    workdir.mkdir(parents=True)
    size = replay.stat().st_size
    print(f"library replays walk: {replay.name}, {size} bytes")

    # The index pass alone, as the Library starts it, with its own peak memory.
    index_user = workdir / "index-user"
    index_user.mkdir()
    output = workdir / "index.json"
    before = peak_of_children()
    result, seconds = run([*RUNNER, str(args.native),
                           *game_arguments(args.game_dir, workdir, "index", index_user),
                           "--replay-index", str(replay), "--output", str(output)],
                          workdir, "index", WALK_TIMEOUT)
    peak = peak_of_children()
    if result.returncode != 0 or not output.is_file():
        raise WalkFailure(f"the index pass ended with status {result.returncode}; "
                          f"see {workdir / 'index.txt'}")
    index = json.loads(output.read_text(encoding="utf-8"))
    length = int(index.get("length", {}).get("ticks", -1))
    for line in (result.stdout + result.stderr).splitlines():
        if line.startswith("replay index: replayed") or line.startswith("replay index: wrote"):
            print(line)
    print(f"library replays walk: index pass alone took {seconds:.0f} s, peak memory "
          f"{peak if peak > before else 'no more than an earlier run'} bytes; "
          f"length {length} ticks")

    # The Library walk.
    user = workdir / "user"
    path = place(replay, user / "Recordings")
    result, seconds = run([*RUNNER, str(args.native),
                           *game_arguments(args.game_dir, workdir, "walk", user),
                           "--check-library-replay", path], workdir, "walk", WALK_TIMEOUT)
    for line in LIBRARY_LINE.findall(result.stdout):
        print(line)
    if result.returncode != 0:
        raise WalkFailure(f"the Library walk ended with status {result.returncode}; "
                          f"see {workdir / 'walk.txt'}")
    print(f"library replays walk: the Library walk took {seconds:.0f} s")
    played = PLAYED.search(result.stdout)
    timeline = TIMELINE.search(result.stdout)
    jumped = JUMPED.search(result.stdout)
    exported = dict((kind, int(bytes_)) for kind, bytes_ in EXPORTED.findall(result.stdout))
    if not played or int(played.group(2)) != 0:
        raise WalkFailure("the replay did not play to its end with no error")
    if not timeline or int(timeline.group(1)) != length:
        raise WalkFailure("the Library's timeline is not the index pass's")
    if not jumped or int(jumped.group(1)) < int(timeline.group(1)) // 2:
        raise WalkFailure("Watch from did not jump to the middle")
    if set(exported) != {".oademo", ".tad"}:
        raise WalkFailure("the replay was not exported in both formats")
    if not PEAK.search(result.stdout) or "library replay: done" not in result.stdout:
        raise WalkFailure("the Library walk did not finish")

    # The tools.
    info, seconds = run([str(args.tool), "replay", "info", str(replay), "--json"],
                        workdir, "info", TOOL_TIMEOUT)
    if info.returncode != 0:
        raise WalkFailure(f"oa-tool replay info ended with status {info.returncode}: {info.stderr}")
    described = json.loads(info.stdout)
    recording = described.get("meta", {}).get("recording", {})
    print(f"library replays walk: oa-tool replay info in {seconds:.0f} s: "
          f"{recording.get('bytes')} recording bytes, {recording.get('packets')} packets, "
          f"sha256 {recording.get('sha256')}, timeline {'timeline' in described}")
    dump, seconds = run([str(args.dump), str(replay)], workdir, "dump", TOOL_TIMEOUT)
    if dump.returncode != 0:
        raise WalkFailure(f"the recording dump ended with status {dump.returncode}: {dump.stderr}")
    packets = re.search(r"^packets\s+(\d+) over (\S+) \((\d+) ms\)$", dump.stdout, re.MULTILINE)
    if not packets or recording.get("packets") is not None and \
            int(packets.group(1)) != int(recording["packets"]):
        raise WalkFailure("the recording dump does not count the replay's packets")
    for line in dump.stdout.splitlines():
        if line.startswith(("map ", "player ", "packets ", "records ", "decode failures",
                            "split failures")):
            print(f"library replays walk: dump: {line}")
    print(f"library replays walk: dump in {seconds:.0f} s")
    if not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    print("library replays walk: passed")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, help="the game")
    parser.add_argument("--tool", required=True, help="oa-tool")
    parser.add_argument("--dump", required=True, help="oa-formats-tad-dump")
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--replay", required=True, help="the replay to walk")
    parser.add_argument("--scratch-root", required=True)
    parser.add_argument("--keep", action="store_true", help="keep the scratch folder")
    args = parser.parse_args(argv)
    if not args.game_dir or not Path(args.game_dir).is_dir():
        print("library replays walk: skipped: no installed game (OA_GAME_DIR)")
        return 77
    try:
        check(args)
    except (WalkFailure, subprocess.TimeoutExpired, OSError, ValueError) as failure:
        print(f"library replays walk: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
