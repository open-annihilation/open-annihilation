#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check playback seeking and speed: --play-demo FILE --replay-seek FROM:TO and --replay-speed.

The game first records a network game of its own: its in-process loopback
check (--net-loopback-check) plays one headless over 127.0.0.1 and
--net-record keeps it. The recording is played to its end once, then with a
jump back and a jump forward that lands where the next jump starts, then at
--replay-speed 16 with no jump. Each playback must end on the digest, tick
and frame count the plain replay ends on, with the plain replay's exit
status. A windowed run on the dummy drivers then watches the same recording
at 16x with both jumps, and its trace is compared with a headless trace of
the same seed. The whole check then runs a second time, from a fresh folder.

Only one jump is armed at a time, in the order given. It fires when the play
head reaches or passes FROM, and a FROM already passed when the jump is
armed fires at once. FROM is never 0. The second FROM is the first jump's
TO, so the two run one after the other.

Every file is written under a scratch folder of its own, and the check sets
every preference it reads, so a second run starts from the same state.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import native_offline

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_native_automation import AutomationFailure, Client  # noqa: E402

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds a run may take.
RUN_TIMEOUT = 600
# Ticks of the recorded network game.
RECORDED_TICKS = 900
# More ticks than the game has, so that a playback runs to its end.
PLAY_TICKS = 100000
RECORDED = re.compile(r"^multiplayer: recorded the game to (.+)$", re.MULTILINE)
LAST_LINE = re.compile(
    r"^demo tick (\d+): digest ([0-9a-f]{16}), (\d+) live units, frames (\d+)/(\d+)(, finished)?$",
    re.MULTILINE)
SEEK_LINE = re.compile(
    r"^demo seek (\d+) -> (\d+): digest ([0-9a-f]{16}) in (\d+) ms$", re.MULTILINE)
# A trace stream: four little-endian words, then records of six.
TRACE_MAGIC = 0x3154414F
TRACE_HEADER = 16
TRACE_RECORD = 24
KIND_SECTION = 0x10
# How long the window may play before the check ends it.
WINDOW_TIMEOUT = 180
# A play head that has not moved for this long, once both jumps have printed, has stopped.
TICK_STILL = 2.0
# Preferences every run starts from, including the window. The same two keys the other
# window checks set, so a run does not ask about an update.
PREFERENCES = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.content-updates" "never"\n'
    '"open-annihilation.update-check" "never"\n'
)


class SeekFailure(Exception):
    """A behaviour that differs from the expected one."""


def environment():
    return dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                SDL_RENDER_DRIVER="software")


def common_arguments(game_dir, workdir, name):
    return ["--game-dir", str(game_dir), "--skip-intro", "--mute",
            "--preferences-file", str(workdir / f"{name}.conf"),
            "--user-folder", str(workdir / f"{name}-user")]


def write_preferences(path):
    """Writes the preferences a run starts from."""
    path.write_text(PREFERENCES, encoding="utf-8")


def run(native, arguments, workdir, name):
    """Runs the game on the given arguments; returns the finished process."""
    write_preferences(workdir / f"{name}.conf")
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
        raise SeekFailure(f"the loopback game was not recorded (status {result.returncode}); "
                          f"see {workdir / 'record.txt'}")
    return recording


def play(native, game_dir, workdir, recording, name, *extra):
    """Plays a recording to its end; returns the process, and the last `demo tick` line's tick,
    digest, live units, frames delivered and frames in all."""
    result = run(native, [*common_arguments(game_dir, workdir, name), "--headless-check",
                          "--play-demo", str(recording), "--match-ticks", str(PLAY_TICKS), *extra],
                 workdir, name)
    lines = LAST_LINE.findall(result.stdout)
    if not lines:
        raise SeekFailure(f"the playback {name} printed no `demo tick` line; "
                          f"see {workdir / (name + '.txt')}")
    tick, digest, live, delivered, frames, finished = lines[-1]
    if not finished or delivered != frames:
        raise SeekFailure(f"the playback {name} did not finish: {lines[-1]}; "
                          f"see {workdir / (name + '.txt')}")
    return result, (int(tick), digest, int(live), int(delivered), int(frames))


def ended_as(name, result, ended, plain, status, workdir):
    """The playback ended where the plain one did, and said nothing had failed."""
    page = workdir / (name + ".txt")
    if ended != plain:
        raise SeekFailure(f"the playback {name} ended on {ended}, the plain replay on {plain}; "
                          f"see {page}")
    if result.returncode != status:
        raise SeekFailure(f"the playback {name} exited with status {result.returncode}, the plain "
                          f"replay with {status}; see {page}")
    text = result.stdout + result.stderr
    for marker in ("keyframe fault", "replay seek failed", "replay seek stopped"):
        if marker in text:
            raise SeekFailure(f"the playback {name} reported {marker}; see {page}")


def read_trace(path):
    """Returns the header seed and the records, each six little-endian words."""
    data = path.read_bytes()
    if len(data) < TRACE_HEADER or (len(data) - TRACE_HEADER) % TRACE_RECORD != 0:
        raise SeekFailure(f"the trace {path.name} is {len(data)} bytes, not a header and records")
    magic, version, record_size, seed = struct.unpack_from("<IIII", data, 0)
    if magic != TRACE_MAGIC or version != 1 or record_size != TRACE_RECORD:
        raise SeekFailure(
            f"the trace {path.name} has magic {magic:#x}, version {version}, record {record_size}")
    records = []
    for offset in range(TRACE_HEADER, len(data), TRACE_RECORD):
        records.append(struct.unpack_from("<IIIIII", data, offset))
    return seed, records


def compare_traces(plain_path, window_path):
    """Every window record equals the plain record of the same kind, tick and section."""
    plain_seed, plain = read_trace(plain_path)
    window_seed, window = read_trace(window_path)
    if plain_seed != 1 or window_seed != 1:
        raise SeekFailure(f"the traces were seeded {plain_seed} and {window_seed}, not 1")
    if not window:
        raise SeekFailure(f"the window wrote no trace records in {window_path.name}")
    indexed = {}
    for words in plain:
        section = words[2] if words[0] == KIND_SECTION else 0
        indexed[(words[0], words[1], section)] = words
    for words in window:
        section = words[2] if words[0] == KIND_SECTION else 0
        key = (words[0], words[1], section)
        expected = indexed.get(key)
        if expected is None:
            raise SeekFailure(f"the window wrote a record the plain replay lacks: {key}")
        if words != expected:
            raise SeekFailure(f"tick {key} differs: window {words}, plain {expected}")


class WindowRun:
    """A replay in the window loop, ended through the automation endpoint."""

    def __init__(self, native, game_dir, workdir, recording, seeks):
        self.lines = []
        self._queue = queue.Queue()
        self.file = workdir / "window-automation.json"
        self.trace = workdir / "window.trace"
        write_preferences(workdir / "window.conf")
        self.process = subprocess.Popen(
            [*RUNNER, str(native), *common_arguments(game_dir, workdir, "window"),
             "--play-demo", str(recording), "--replay-speed", "16",
             "--replay-seek", seeks[0], "--replay-seek", seeks[1],
             "--seed", "1", "--trace-digest", str(self.trace),
             "--fark", "--fark-file", str(self.file)],
            cwd=workdir, env=environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", bufsize=1)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def take_output(self):
        while not self._queue.empty():
            line = self._queue.get()
            if line is not None:
                self.lines.append(line)

    def text(self):
        self.take_output()
        return "\n".join(self.lines)

    def endpoint(self):
        """Waits for the endpoint's file and returns what it holds."""
        deadline = time.monotonic() + WINDOW_TIMEOUT
        while time.monotonic() < deadline:
            if self.file.is_file():
                return json.loads(self.file.read_text(encoding="utf-8"))
            if self.process.poll() is not None:
                break
            time.sleep(0.1)
        raise SeekFailure(f"the window wrote no endpoint file; see its output:\n{self.text()}")

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


def watch_window(native, game_dir, workdir, recording, back_from, back_to, forward_to):
    """Plays the recording in the window at 16x with both jumps, and returns its trace."""
    seeks = (f"{back_from}:{back_to}", f"{back_to}:{forward_to}")
    window = WindowRun(native, game_dir, workdir, recording, seeks)
    client = None
    try:
        endpoint = window.endpoint()
        address, token = endpoint.get("address", ""), endpoint.get("token", "")
        if endpoint.get("version") != 1 or not address.startswith("127.0.0.1:") or len(token) != 32:
            raise SeekFailure(f"the endpoint file holds {endpoint}")
        client = Client(address)
        hello = client.request("hello", versions=[1], client="native-replay-seek", token=token)
        if not hello.get("ok"):
            raise SeekFailure(f"hello was answered {hello}")
        expected = [(str(back_from), str(back_to)), (str(back_to), str(forward_to))]
        deadline = time.monotonic() + WINDOW_TIMEOUT
        seen_match = False
        last_tick = None
        still_since = None
        while time.monotonic() < deadline:
            if window.process.poll() is not None:
                break
            jumps = [(jump[0], jump[1]) for jump in SEEK_LINE.findall(window.text())]
            answer = client.request("match")
            clock = answer.get("clock") or {}
            tick = clock.get("tick")
            if answer.get("in_match"):
                seen_match = True
            if tick != last_tick:
                last_tick = tick
                still_since = time.monotonic()
            both = jumps == expected
            stopped = still_since is not None and time.monotonic() - still_since >= TICK_STILL
            ended = seen_match and (
                not answer.get("in_match") or answer.get("game_over") or stopped)
            if both and ended:
                break
            time.sleep(0.2)
        else:
            raise SeekFailure(
                f"the window did not finish both jumps; output:\n{window.text()[-4000:]}")
        if window.process.poll() is None:
            client.request("quit")
            status = window.process.wait(timeout=60)
        else:
            status = window.process.returncode
        if status != 0:
            raise SeekFailure(f"the window exited with status {status}; output:\n{window.text()}")
        jumps = [(jump[0], jump[1]) for jump in SEEK_LINE.findall(window.text())]
        if jumps != expected:
            raise SeekFailure(f"the window's jumps were {jumps}, expected {expected}")
        page = window.text()
        for marker in ("keyframe fault", "replay seek failed", "replay seek stopped"):
            if marker in page:
                raise SeekFailure(f"the window reported {marker}")
        if not window.trace.is_file():
            raise SeekFailure("the window wrote no trace")
        return window.trace
    except AutomationFailure as failure:
        raise SeekFailure(f"the window's endpoint failed: {failure}") from failure
    finally:
        if client is not None:
            client.close()
        window.stop()
        (workdir / "window.txt").write_text(window.text(), encoding="utf-8")


def check(native, game_dir, workdir):
    """Runs every step; returns the plain replay's last tick and the two jumps."""
    recording = record(native, game_dir, workdir)
    first, plain = play(native, game_dir, workdir, recording, "plain")
    again, repeated = play(native, game_dir, workdir, recording, "again")
    if plain != repeated or first.returncode != again.returncode:
        raise SeekFailure(f"two plain replays of one recording ended {plain} and {repeated}; "
                          f"see {workdir / 'plain.txt'}")
    last = plain[0]
    # The second jump starts where the first lands, and neither starts at 0: a FROM of 0 would
    # run at the first tick, before the replay had reached the earlier jump.
    back_from = last // 2
    back_to = max(last // 5, 1)
    forward_to = max(last * 4 // 5, back_to + 1)
    if back_from == 0 or back_from == back_to or forward_to <= back_to:
        raise SeekFailure(f"the recording ends at tick {last}, too soon for a jump back and on")
    sought, sought_end = play(
        native, game_dir, workdir, recording, "seek",
        "--replay-seek", f"{back_from}:{back_to}", "--replay-seek", f"{back_to}:{forward_to}")
    ended_as("seek", sought, sought_end, plain, first.returncode, workdir)
    jumps = SEEK_LINE.findall(sought.stdout)
    expected = [(str(back_from), str(back_to)), (str(back_to), str(forward_to))]
    if [(jump[0], jump[1]) for jump in jumps] != expected:
        raise SeekFailure(f"the jumps printed {jumps}, expected {expected}; "
                          f"see {workdir / 'seek.txt'}")
    paced, paced_end = play(
        native, game_dir, workdir, recording, "speed", "--replay-speed", "16")
    ended_as("speed", paced, paced_end, plain, first.returncode, workdir)
    if SEEK_LINE.search(paced.stdout):
        raise SeekFailure(f"a playback at 16x printed a jump; see {workdir / 'speed.txt'}")
    traced_path = workdir / "plain.trace"
    traced, traced_end = play(
        native, game_dir, workdir, recording, "traced", "--seed", "1", "--trace-digest",
        str(traced_path))
    ended_as("traced", traced, traced_end, plain, first.returncode, workdir)
    window_trace = watch_window(
        native, game_dir, workdir, recording, back_from, back_to, forward_to)
    compare_traces(traced_path, window_trace)
    return last, expected


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if not args.game_dir.is_dir():
        print(f"native-replay-seek: no installed game at {args.game_dir}; skipped")
        return 77
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    folders = []
    try:
        for _ in range(2):
            workdir = Path(tempfile.mkdtemp(prefix="replay-seek-", dir=args.scratch_root))
            folders.append(workdir)
            last, jumps = check(args.native, args.game_dir, workdir)
    except (SeekFailure, subprocess.TimeoutExpired, OSError) as failure:
        kept = folders[-1] if folders else args.scratch_root
        print(f"native-replay-seek: {failure}; the runs are in {kept}", file=sys.stderr)
        return 1
    for workdir in folders:
        shutil.rmtree(workdir, ignore_errors=True)
    taken = ", ".join(f"{start} -> {land}" for start, land in jumps)
    print(f"native-replay-seek: a {last}-tick game jumped {taken} and played at 16x, and ended "
          f"on the plain replay's digest each time, twice, and the window's trace matched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
