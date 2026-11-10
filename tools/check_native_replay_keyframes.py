#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check keyframes of a playback session: --play-demo FILE --replay-keyframe-check TICK.

The game first records a network game of its own: its in-process loopback
check (--net-loopback-check) plays one headless over 127.0.0.1 and
--net-record keeps it. The recording is played to its end once, with no
keyframe, and then with a keyframe check at the start, in the middle and near
the end. Each check must:

- print `keyframe check at TICK: digest A, restored B, same`, with A equal to B,
  and say that the state both runs ended in is the same;
- leave the replay to end on the digest, tick and frame count the plain replay
  ends on, with the plain replay's exit status;
- not move the replay when no keyframe is asked for: a second plain replay
  ends as the first did.

A check at a tick past the end of the recording says where the replay ends, and
its replay ends as the plain one does too.

Every file is written under a scratch folder of its own, and the check sets
every preference it reads, so a second run starts from the same state.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import native_offline

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
CHECK_LINE = re.compile(
    r"^keyframe check at (\d+): digest ([0-9a-f]{16}), restored ([0-9a-f]{16}), (same|differs)$",
    re.MULTILINE)
STATE_LINE = re.compile(r"^keyframe check at (\d+): state at tick (\d+): (.*)$", re.MULTILINE)
KEYFRAME_LINE = re.compile(
    r"^keyframe at (\d+): raw (\d+) bytes \(fixed (\d+), varying (\d+)\), compressed (\d+) bytes, "
    r"without the base (\d+) bytes, held (\d+) bytes, taken in [\d.]+ ms, restored in [\d.]+ ms$",
    re.MULTILINE)
ENDS_LINE = re.compile(r"^keyframe check at (\d+): the replay ends at tick (\d+)$", re.MULTILINE)


class KeyframeFailure(Exception):
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
        raise KeyframeFailure(f"the loopback game was not recorded (status {result.returncode}); "
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
        raise KeyframeFailure(f"the playback {name} printed no `demo tick` line; "
                              f"see {workdir / (name + '.txt')}")
    tick, digest, live, delivered, frames, finished = lines[-1]
    if not finished or delivered != frames:
        raise KeyframeFailure(f"the playback {name} did not finish: {lines[-1]}; "
                              f"see {workdir / (name + '.txt')}")
    return result, (int(tick), digest, int(live), int(delivered), int(frames))


def check_at(native, game_dir, workdir, recording, plain, status, name, at):
    """Plays the recording with a keyframe check at `at`; returns how many bytes the keyframe
    took raw and compressed, or None when `at` is past the end."""
    result, ended = play(native, game_dir, workdir, recording, name,
                         "--replay-keyframe-check", str(at))
    page = workdir / (name + ".txt")
    if ended != plain:
        raise KeyframeFailure(f"the replay with a keyframe check at {at} ended on {ended}, the "
                              f"plain replay on {plain}; see {page}")
    if result.returncode != status:
        raise KeyframeFailure(f"the replay with a keyframe check at {at} exited with status "
                              f"{result.returncode}, the plain replay with {status}; see {page}")
    if "keyframe fault" in result.stderr or "keyframe fault" in result.stdout:
        raise KeyframeFailure(f"the replay with a keyframe check at {at} reported a keyframe "
                              f"fault; see {page}")
    if at > plain[0]:
        ends = ENDS_LINE.findall(result.stdout)
        if len(ends) != 1 or int(ends[0][0]) != at or int(ends[0][1]) != plain[0]:
            raise KeyframeFailure(f"a keyframe check past the end said {ends}; see {page}")
        return None
    checks = CHECK_LINE.findall(result.stdout)
    if len(checks) != 1:
        raise KeyframeFailure(f"the check at {at} printed {len(checks)} result lines; see {page}")
    where, first, second, verdict = checks[0]
    if int(where) != at or verdict != "same" or first != second:
        raise KeyframeFailure(f"the check at {at} says digest {first}, restored {second}, "
                              f"{verdict}; see {page}")
    states = STATE_LINE.findall(result.stdout)
    if len(states) != 1 or states[0][2] != "same":
        raise KeyframeFailure(f"the state the check at {at} left was not the same: {states}; "
                              f"see {page}")
    keyframes = KEYFRAME_LINE.findall(result.stdout)
    if len(keyframes) != 1 or int(keyframes[0][0]) != at:
        raise KeyframeFailure(f"the check at {at} did not say what its keyframe took; see {page}")
    raw, compressed = int(keyframes[0][1]), int(keyframes[0][4])
    if not 0 < compressed < raw:
        raise KeyframeFailure(f"the keyframe at {at} is {raw} bytes raw and {compressed} "
                              f"compressed; see {page}")
    return raw, compressed


def check(native, game_dir, workdir):
    """Runs every step; returns the plain replay's last tick and what each keyframe took."""
    recording = record(native, game_dir, workdir)
    first, plain = play(native, game_dir, workdir, recording, "plain")
    again, repeated = play(native, game_dir, workdir, recording, "again")
    if plain != repeated or first.returncode != again.returncode:
        raise KeyframeFailure(f"two plain replays of one recording ended {plain} and "
                              f"{repeated}; see {workdir / 'plain.txt'}")
    last = plain[0]
    sizes = []
    for label, at in (("start", 0), ("middle", last // 2), ("end", last - last // 20)):
        sizes.append((label, at, check_at(native, game_dir, workdir, recording, plain,
                                          first.returncode, f"check-{label}", at)))
    # A check asked for past the end of the recording says so and changes nothing.
    check_at(native, game_dir, workdir, recording, plain, first.returncode, "check-past",
             last + 500)
    return last, sizes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if not args.game_dir.is_dir():
        print(f"native-replay-keyframes: no installed game at {args.game_dir}; skipped")
        return 77
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="replay-keyframes-", dir=args.scratch_root))
    try:
        last, sizes = check(args.native, args.game_dir, workdir)
    except (KeyframeFailure, subprocess.TimeoutExpired, OSError) as failure:
        print(f"native-replay-keyframes: {failure}; the runs are in {workdir}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    taken = ", ".join(f"{label} (tick {at}) {raw} bytes raw, {compressed} compressed"
                      for label, at, (raw, compressed) in sizes)
    print(f"native-replay-keyframes: a {last}-tick game restored to the same digest and state "
          f"from keyframes at its {taken}, and ended on the plain replay's digest each time")
    return 0


if __name__ == "__main__":
    sys.exit(main())
