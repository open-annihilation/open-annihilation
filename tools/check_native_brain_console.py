#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""PrintWeights and ReloadAIProfiles for a seated brain and for the built-in player.

The brain's start counts its calls and its report returns "start count {n}".
A stage file prints the report, reloads the profile and prints it again.
With steady-commander seated on place 1, the first file holds start count 1
and the second start count 2. With no brain key, both files are today's
report, whose first line is the match clock. The script exits 77 when no
game data is installed.
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

BRAIN_SCRIPT = """local starts = 0
local hooks = {}
function hooks.start(game, me, saved)
  starts = starts + 1
end
function hooks.report()
  return "start count " .. tostring(starts)
end
return hooks
"""

STAGE = """console PrintWeights 1 before.txt
console ReloadAIProfiles
console PrintWeights 1 after.txt
"""


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def install_steady(user):
    """Writes steady-commander, whose start counts and whose report names the count."""
    folder = user / "Brains" / "steady-commander"
    folder.mkdir(parents=True)
    (folder / "oabrain.yaml").write_text(STEADY_MANIFEST, encoding="utf-8")
    (folder / "brain.lua").write_text(BRAIN_SCRIPT, encoding="utf-8")


def write_preferences(path, extra):
    """A preferences file at hard, with content updates off, plus extra lines."""
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.content-updates") + " " + quote_preference("never"),
        quote_preference("Total Annihilation|SkirmishDifficulty") + " " + quote_preference("2"),
        *extra,
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def seat_line():
    """Steady Commander at firm on place 1, as the preferences value."""
    text = '{"seats":[{"brain":"steady-commander","level":"firm","place":1}]}'
    return quote_preference("open-annihilation.brains.skirmish") + " " + quote_preference(text)


def launch(native, game_dir, work, user, preferences, stage):
    """Runs one headless skirmish. Returns the status and the combined log."""
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
        "--match-ticks",
        "30",
        "--stage",
        str(stage),
        "--preferences-file",
        str(preferences),
        "--user-folder",
        str(user),
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
    return result.returncode, result.stdout + result.stderr


def require(condition, output, message):
    """Stops the check when a run did not do what it should."""
    if condition:
        return
    print(output, end="" if output.endswith("\n") or output == "" else "\n")
    raise ScriptFailure(message)


def read_report(path):
    """The report file's text, or empty when the run wrote none."""
    if not path.is_file():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("brain console: skipped, no installed game")
        return 77
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-brain-console-", dir=scratch_root) as temporary:
        work = Path(temporary)
        user = work / "user"
        install_steady(user)
        stage = work / "weights.stage"
        stage.write_text(STAGE, encoding="utf-8")

        seated = work / "seated"
        seated.mkdir()
        seated_preferences = seated / "preferences.conf"
        write_preferences(seated_preferences, [seat_line()])
        status, output = launch(native, game_dir, seated, user, seated_preferences, stage)
        require(status == 0, output, f"the seated run exited with {status}")
        before = read_report(seated / "before.txt")
        after = read_report(seated / "after.txt")
        require(before.strip() == "start count 1", output, f"before.txt was {before!r}")
        require(after.strip() == "start count 2", output, f"after.txt was {after!r}")
        print("seated: before.txt start count 1, after.txt start count 2")

        builtin = work / "builtin"
        builtin.mkdir()
        builtin_preferences = builtin / "preferences.conf"
        write_preferences(builtin_preferences, [])
        status, output = launch(native, game_dir, builtin, user, builtin_preferences, stage)
        require(status == 0, output, f"the built-in run exited with {status}")
        for name in ("before.txt", "after.txt"):
            text = read_report(builtin / name)
            first = text.splitlines()[0] if text.splitlines() else ""
            require(first.startswith("Match clock:"), output, f"{name} began {first!r}")
        print("built-in: both reports begin with the match clock")
    print("brain console: a seated report counted starts, and the built-in report was today's")
    return 0


if __name__ == "__main__":
    import sys
    try:
        sys.exit(main())
    except ScriptFailure as failure:
        print(f"brain console: {failure}", file=sys.stderr)
        sys.exit(1)
