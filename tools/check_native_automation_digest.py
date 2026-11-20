#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the automation endpoint leaves a running match as it is.

The game first records a network game of its own: its in-process loopback
check (--net-loopback-check) plays one headless over 127.0.0.1 and
--net-record keeps it. That recording is then played back as the first
scene of the game's main loop, on SDL's dummy drivers, twice, with the
same seed and for the same number of frames, each run writing the match's
per-tick trace stream (--trace-digest):

- once without --fark;
- once with --fark, and a client that subscribes to every kind of event and
  asks screen, match with its saved-state digest (state_hash), room and
  prefs at every frame until the run ends.

The two trace streams must be the same, byte for byte, over the ticks both
runs reached, which must be at least MIN_TICKS: the endpoint, its events
and the digests it works out change nothing of the simulation. The client
must be told the screen shown as it subscribes, number its events from 1
without a gap, and see the match running, its players named and its digest
given, at MIN_MATCH_FRAMES frames at least.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import native_offline

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_automation as automation  # noqa: E402

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds a run may take.
RUN_TIMEOUT = 600
# Seconds the game may take to end once its connection closed.
ENDING_SECONDS = 10
# Ticks of the recorded network game.
RECORDED_TICKS = 600
# Frames each playback runs; the playback starts as the first scene.
PLAYBACK_FRAMES = 1500
# The seed both playbacks are given, so that their matches draw alike.
SEED = "1"
# The window both playbacks open: small, so that frames are cheap.
RESOLUTION = "640x480"
# Ticks both playbacks must reach for the comparison to count.
MIN_TICKS = 150
# Frames at which the client must have read the running match.
MIN_MATCH_FRAMES = 100
# The trace stream: a header, then records of a tick and its sections.
TRACE_HEADER_BYTES = 16
TRACE_RECORD_BYTES = 24
TRACE_RECORDS_PER_TICK = 8
RECORDED = re.compile(r"^multiplayer: recorded the game to (.+)$", re.MULTILINE)
STATE_HASH = re.compile(r"^[0-9a-f]{16}$")


class DigestFailure(Exception):
    """A behaviour that differs from the expected one."""



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding='utf-8') if target.is_file() else 'open-annihilation-preferences 1\n'
    if 'open-annihilation.update-check' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.update-check" "never"\n'
    if 'open-annihilation.content-updates' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding='utf-8')


def environment():
    return dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                SDL_RENDER_DRIVER="software")


def common_arguments(game_dir, workdir, name):
    _offline_preferences(workdir / f"{name}.conf")
    return ["--game-dir", str(game_dir), "--skip-intro", "--mute",
            "--preferences-file", str(workdir / f"{name}.conf"),
            "--user-folder", str(workdir / f"{name}-user")]


def record(native, game_dir, workdir):
    """Records a network game the game plays with itself; returns the recording."""
    recording = workdir / "loopback.tad"
    result = subprocess.run(
        [*RUNNER, str(native), *common_arguments(game_dir, workdir, "record"), "--headless-check",
         "--net-loopback-check", str(RECORDED_TICKS), "--net-record", str(recording)],
        cwd=workdir, env=environment(), timeout=RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")
    (workdir / "record.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0 or not recording.is_file() or not RECORDED.search(result.stdout):
        raise DigestFailure(f"the loopback game was not recorded (status {result.returncode}); "
                            f"see {workdir / 'record.txt'}")
    return recording


def playback_arguments(native, game_dir, workdir, name, recording):
    return [*RUNNER, str(native), *common_arguments(game_dir, workdir, name),
            "--play-demo", str(recording), "--seed", SEED, "--resolution", RESOLUTION,
            "--frames", str(PLAYBACK_FRAMES), "--trace-digest", str(workdir / f"{name}.trace")]


def play_without_endpoint(native, game_dir, workdir, recording):
    """Plays the recording back in the main loop without the endpoint; returns its trace."""
    result = subprocess.run(
        playback_arguments(native, game_dir, workdir, "plain", recording), cwd=workdir,
        env=environment(), timeout=RUN_TIMEOUT, check=False, capture_output=True, text=True,
        errors="replace")
    (workdir / "plain.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise DigestFailure(f"the playback without the endpoint ended with status "
                            f"{result.returncode}; see {workdir / 'plain.txt'}")
    return (workdir / "plain.trace").read_bytes()


class EventClient(automation.Client):
    """A client that keeps the events that come between its answers."""

    def __init__(self, address):
        super().__init__(address)
        self.events = []

    def answer(self, request_id):
        """Returns the answer to a request, keeping the events before it; None once closed."""
        while True:
            frame = self.read_frame()
            if frame is None:
                return None
            message, _ = frame
            if "event" in message:
                self.events.append(message)
                continue
            if message.get("id") != request_id:
                raise DigestFailure(f"an answer names another request than {request_id}: {message}")
            return message


def read_every_frame(client, token, seen):
    """Subscribes to every event and reads the game at every frame until the game ends."""
    hello = client.request("hello", versions=[1], client="digest", token=token)
    if not hello.get("ok"):
        raise DigestFailure(f"hello was answered {hello}")
    client.next_id += 1
    client.send_raw(automation.encode({"id": client.next_id, "op": "subscribe", "events": "all"}))
    subscribed = client.answer(client.next_id)
    if subscribed is None or not subscribed.get("ok") or "game_over" not in subscribed.get("events", []):
        raise DigestFailure(f"subscribe was answered {subscribed}")
    # The game ends after its frames, closing the connection, or resetting
    # it when requests were still on their way.
    try:
        while True:
            first = client.next_id + 1
            for op, fields in (("screen", {}), ("match", {"state_hash": True}), ("room", {}),
                               ("prefs", {"names": ["multiplayer|tcpaddr"]})):
                client.next_id += 1
                client.send_raw(automation.encode({"id": client.next_id, "op": op, **fields}))
            for request_id in range(first, client.next_id + 1):
                message = client.answer(request_id)
                if message is None:
                    return
                if not message.get("ok"):
                    raise DigestFailure(f"a request was refused: {message}")
                if "in_match" in message:
                    seen.append(message)
    except (ConnectionResetError, BrokenPipeError):
        return


def play_with_endpoint(native, game_dir, workdir, recording):
    """Plays the recording back with the endpoint and a reading client; returns its trace,
    the match answers and the events the client was sent."""
    endpoint_file = workdir / "endpoint.json"
    output = open(workdir / "endpoint.txt", "w", encoding="utf-8", errors="replace")
    process = subprocess.Popen(
        [*playback_arguments(native, game_dir, workdir, "endpoint", recording),
         "--fark", "--fark-file", str(endpoint_file)],
        cwd=workdir, env=environment(), stdout=output, stderr=subprocess.STDOUT)
    seen = []
    client = None
    try:
        deadline = time.monotonic() + RUN_TIMEOUT
        while not endpoint_file.is_file():
            if process.poll() is not None or time.monotonic() > deadline:
                raise DigestFailure(f"the game wrote no endpoint file; see {workdir / 'endpoint.txt'}")
            time.sleep(0.05)
        endpoint = json.loads(endpoint_file.read_text(encoding="utf-8"))
        client = EventClient(endpoint["address"])
        failure = []

        def reader():
            try:
                read_every_frame(client, endpoint["token"], seen)
            except automation.AutomationFailure as error:
                # A frame cut off as the game ends is the end of the run.
                try:
                    process.wait(timeout=ENDING_SECONDS)
                except subprocess.TimeoutExpired:
                    failure.append(error)
            except (DigestFailure, OSError, ValueError) as error:
                failure.append(error)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        status = process.wait(timeout=RUN_TIMEOUT)
        thread.join(timeout=30)
        if failure:
            raise DigestFailure(f"the client failed: {failure[0]}")
        if status != 0:
            raise DigestFailure(f"the playback with the endpoint ended with status {status}; "
                                f"see {workdir / 'endpoint.txt'}")
        return (workdir / "endpoint.trace").read_bytes(), seen, client.events
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if client is not None:
            client.close()
        output.close()


def check_reads(seen, events):
    """Raises DigestFailure unless the client read the running match and was sent its events."""
    if not events or events[0].get("event") != "screen" or events[0].get("previous") is not None:
        raise DigestFailure(f"the client was not told the screen as it subscribed: {events[:1]}")
    for number, event in enumerate(events, start=1):
        if event.get("seq") != number or not isinstance(event.get("frame"), int) or \
                not isinstance(event.get("tick"), int):
            raise DigestFailure(f"event {number} is {event}")
    running = [answer for answer in seen if answer.get("in_match")]
    frames = {answer["frame"] for answer in running}
    if len(frames) < MIN_MATCH_FRAMES:
        raise DigestFailure(f"the client read the running match at {len(frames)} frames only")
    for answer in running:
        names = [player.get("name") for player in answer.get("players", [])]
        if not names or not all(names) or not STATE_HASH.match(answer.get("state_hash", "")):
            raise DigestFailure(f"match was answered {answer}")
    ticks = [answer["tick"] for answer in running]
    if ticks != sorted(ticks) or ticks[-1] <= ticks[0]:
        raise DigestFailure(f"the match's ticks went from {ticks[0]} to {ticks[-1]} out of order")


def compare_traces(plain, endpoint):
    """Raises DigestFailure unless the trace streams agree over the ticks both reached."""
    tick_bytes = TRACE_RECORD_BYTES * TRACE_RECORDS_PER_TICK
    ticks = (min(len(plain), len(endpoint)) - TRACE_HEADER_BYTES) // tick_bytes
    if ticks < MIN_TICKS:
        raise DigestFailure(f"the playbacks reached {ticks} ticks together, fewer than {MIN_TICKS}")
    length = TRACE_HEADER_BYTES + ticks * tick_bytes
    if plain[:length] != endpoint[:length]:
        at = next(index for index in range(length) if plain[index] != endpoint[index])
        tick = max(0, at - TRACE_HEADER_BYTES) // tick_bytes
        raise DigestFailure(f"the trace streams differ in the {tick + 1}th tick recorded "
                            f"(byte {at})")
    return ticks


def check(native, game_dir, workdir):
    recording = record(native, game_dir, workdir)
    plain = play_without_endpoint(native, game_dir, workdir, recording)
    endpoint, seen, events = play_with_endpoint(native, game_dir, workdir, recording)
    check_reads(seen, events)
    ticks = compare_traces(plain, endpoint)
    frames = len({answer["frame"] for answer in seen if answer.get("in_match")})
    return ticks, frames, len(events)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if not args.game_dir.is_dir():
        print(f"native-automation-digest: no installed game at {args.game_dir}; skipped")
        return 77
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="automation-digest-", dir=args.scratch_root))
    try:
        ticks, frames, events = check(args.native, args.game_dir, workdir)
    except (DigestFailure, subprocess.TimeoutExpired, OSError) as failure:
        print(f"native-automation-digest: {failure}; the runs are in {workdir}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print(f"native-automation-digest: the match played the same {ticks} ticks with the endpoint "
          f"read at {frames} frames and {events} events sent as without it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
