#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check a join whose host is not found, end to end, as the game plays it.

A launched joiner waits on the game list for its host's game; when none is
listed in time, a joiner whose launch is still active is told so by the
extension that launched it and goes back to the main menu, and any other is
told "Host not found.  Exiting..." and, 4 seconds later, leaves the game and
ends the program with exit status 0, showing the local player's disconnect
reason first when there is one. A launch is still active when the wait
begins in every game launched, so only --check-host-not-found, which counts
its "-n1" launch as active until the wait begins, reaches the second
outcome.

open-annihilation runs through its main loop on the dummy SDL drivers with
"-n1:127.0.0.1", which, as in 3.1c, opens multiplayer at once over TCP/IP
and goes to the game list of that address. Alone, -n1 leaves the choice of
a game there to the player; the check's active launch has the game list
wait for the host's game and join it, as a launched joiner does. The game
asks for games at a DirectPlay port of the check's own
(--dplay-port), which the check holds open and never answers, so no game is
found there whatever else runs on this machine. The run must end by itself with status 0 after reporting the text
and a leave about 4 seconds later with no reason and exit status 0.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# The address the joiner asks for its host's game.
ADDRESS = "127.0.0.1"
# Frames the run may take: the 20 s wait and the 4 s text take far fewer.
FRAMES = "20000"
RUN_TIMEOUT = 900
TEXT_SHOWN = 'host-not-found check: the game list shows "Host not found.  Exiting..."'
LEFT = re.compile(r"host-not-found check: (\d+) ms later the game leaves with (.*) and exit status (-?\d+)")
# How long the text must show before the game leaves, in milliseconds: 4 s,
# give or take the frames around it.
SHOWN_AT_LEAST = 3900
SHOWN_AT_MOST = 8000


class HostNotFoundFailure(Exception):
    """A behaviour that differs from the expected one."""



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding="utf-8") if target.is_file() else "open-annihilation-preferences 1\n"
    if "open-annihilation.update-check" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.update-check" "never"\n'
    if "open-annihilation.content-updates" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding="utf-8")


def check(native, game_dir, workdir):
    """Runs the join; raises HostNotFoundFailure when it does not end as expected."""
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
    # The port the game asks for games at: held for the whole run and never answered.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as silent:
        silent.bind((ADDRESS, 0))
        port = silent.getsockname()[1]
        _offline_preferences(workdir / "host-not-found.conf")
        result = subprocess.run(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--check-host-not-found",
             "--frames", FRAMES, "--preferences-file", str(workdir / "host-not-found.conf"),
             "--dplay-port", str(port), f"-n1:{ADDRESS}"],
            cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False, capture_output=True,
            text=True, errors="replace")
    output = result.stdout + result.stderr
    lines = result.stdout.splitlines()
    left = [match for match in map(LEFT.fullmatch, lines) if match is not None]
    try:
        if TEXT_SHOWN not in lines:
            raise HostNotFoundFailure('the game list never showed "Host not found.  Exiting..."')
        if len(left) != 1:
            raise HostNotFoundFailure(f"the game left {len(left)} times after the text, not once")
        shown_ms = int(left[0].group(1))
        if not SHOWN_AT_LEAST <= shown_ms <= SHOWN_AT_MOST:
            raise HostNotFoundFailure(f"the game left {shown_ms} ms after the text, not about 4 s")
        if left[0].group(2) != "no reason" or left[0].group(3) != "0":
            raise HostNotFoundFailure(f"the game left with {left[0].group(2)} and exit status "
                                      f"{left[0].group(3)}, not no reason and 0")
        if result.returncode != 0:
            raise HostNotFoundFailure(f"open-annihilation exited with {result.returncode}")
    except HostNotFoundFailure:
        print(output, end="")
        raise
    print(f"host-not-found check: \"Host not found.  Exiting...\" showed for {shown_ms} ms, then the "
          "game left with no reason and exited with status 0")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-host-not-found-",
                                     dir=args.scratch_root.resolve()) as scratch:
        try:
            check(args.native.resolve(), args.game_dir.resolve(), Path(scratch))
        except HostNotFoundFailure as failure:
            print(f"host-not-found check: {failure}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
