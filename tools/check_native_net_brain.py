#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A loopback host seats Steady Commander before the match's first tick.

The script writes Brains/steady-commander, whose hooks do nothing, into a
scratch user folder, and a scratch preferences file whose multiplayer key
names that brain at firm for the first computer this machine seats. It runs
one headless loopback of 600 ticks with a computer player, and requires the
host's "plays steady-commander 1.2 at firm" line once and the loopback's own
pass line. The script exits 77 when no game data is installed.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

RUN_TIMEOUT_SECONDS = 900
PREFERENCES_HEADER = "open-annihilation-preferences 1"
PLAYS = "plays steady-commander 1.2 at firm"
BUILT = "net loopback check: the computer player built"

STEADY_MANIFEST = """oabrain: 1
id: steady-commander
name: "Steady Commander"
version: "1.2"
author:
  name: "Ridge Testers"
description: "Holds the line."
tags: ["teaching"]
works-with: ["*"]
levels:
  - id: firm
    name: "Firm"
    difficulty: hard
"""

BRAIN_SCRIPT = "local brain = {}\nreturn brain\n"


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_brain(user):
    """Writes Steady Commander: a manifest and hooks that do nothing."""
    folder = user / "Brains" / "steady-commander"
    folder.mkdir(parents=True)
    (folder / "oabrain.yaml").write_text(STEADY_MANIFEST, encoding="utf-8")
    (folder / "brain.lua").write_text(BRAIN_SCRIPT, encoding="utf-8")


def write_preferences(path):
    """A preferences file that remembers Steady Commander at firm, place 0."""
    seats = '{"seats":[{"brain":"steady-commander","level":"firm","place":0}]}'
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.content-updates") + " " + quote_preference("never"),
        quote_preference("open-annihilation.first-run") + " " + quote_preference("done"),
        quote_preference("open-annihilation.brains.multiplayer") + " " + quote_preference(seats),
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def lines_with(output, phrase):
    """The log lines that hold a phrase."""
    return [line for line in output.splitlines() if phrase in line]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("net brain: skipped, no installed game")
        return 77
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-net-brain-", dir=scratch_root) as temporary:
        work = Path(temporary)
        user = work / "user"
        write_brain(user)
        preferences = work / "preferences.conf"
        write_preferences(preferences)
        environment = dict(
            os.environ,
            SDL_VIDEO_DRIVER="dummy",
            SDL_AUDIO_DRIVER="dummy",
            SDL_RENDER_DRIVER="software",
        )
        command = [
            *RUNNER,
            str(native),
            "--game-dir",
            str(game_dir),
            "--skip-intro",
            "--mute",
            "--headless-check",
            "--net-loopback-check",
            "600",
            "--net-loopback-computer",
            "--user-folder",
            str(user),
            "--preferences-file",
            str(preferences),
        ]
        result = subprocess.run(
            command,
            cwd=work,
            env=environment,
            timeout=RUN_TIMEOUT_SECONDS,
            check=False,
            capture_output=True,
            text=True,
            errors="replace",
        )
        output = result.stdout + result.stderr
        played = lines_with(output, PLAYS)
        try:
            if len(played) != 1:
                raise ScriptFailure(f"the host played Steady Commander {len(played)} times, not once")
            if BUILT not in output:
                raise ScriptFailure("the loopback did not report that the computer player built")
            if result.returncode != 0:
                raise ScriptFailure(f"open-annihilation exited with {result.returncode}")
        except ScriptFailure as failure:
            print(output, end="" if output.endswith("\n") or output == "" else "\n")
            print(f"net brain: {failure}", flush=True)
            return 1
    print("net brain: the host plays steady-commander 1.2 at firm, and the computer player built")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
