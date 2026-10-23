#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish seats a remembered brain, the default, or the built-in player.

Run A has no brain key and no default: the log has no brains line, and the
preferences file gains no open-annihilation.brains. key. Run B remembers
Steady Commander at firm on place 1. Run C names that brain as the default
and seats it at firm, the level hard takes. Run D remembers Ridge Raider
with no mod played, so the built-in player stays and the log names Ridge.
Run E remembers tide-keeper, which is not installed.

Every run is a headless skirmish of 60 ticks, with --skip-intro --mute, its
own preferences file and SkirmishDifficulty 2. Content updates are never, so
the run does not reach a server. The script exits 77 when no game data is
installed.
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
PLAYS = "brains: player 1 plays steady-commander 1.2 at firm"
RIDGE = "brains: player 1: ridge-raider: Made for ridge; the built-in player plays"
MISSING = "brains: player 1: tide-keeper: it is not installed; the built-in player plays"

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
  - id: calm
    name: "Calm"
    difficulty: easy
  - id: steady
    name: "Steady"
    difficulty: medium
  - id: firm
    name: "Firm"
    difficulty: hard
options:
  aggression:
    type: "choice"
    label: "Aggression"
    values: ["low", "high"]
    default: "low"
"""

RIDGE_MANIFEST = """oabrain: 1
id: ridge-raider
name: "Ridge Raider"
version: "2.0"
author:
  name: "Ridge Testers"
description: "Raids early."
tags: ["rush"]
works-with: ["ridge >= 28"]
levels:
  - id: raid
    name: "Raid"
    difficulty: hard
"""

BRAIN_SCRIPT = "local hooks = {}\nreturn hooks\n"


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def install_brains(user):
    """Writes the two scratch brains, each a manifest and an empty hook table."""
    packages = {
        "steady-commander": STEADY_MANIFEST,
        "ridge-raider": RIDGE_MANIFEST,
    }
    for brain_id, manifest in packages.items():
        folder = user / "Brains" / brain_id
        folder.mkdir(parents=True)
        (folder / "oabrain.yaml").write_text(manifest, encoding="utf-8")
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


def seat_line(brain_id, level):
    """One remembered seat, place 1, as the preferences value."""
    if level:
        text = (
            '{"seats":[{"brain":"' + brain_id + '","level":"' + level + '","place":1}]}'
        )
    else:
        text = '{"seats":[{"brain":"' + brain_id + '","place":1}]}'
    return quote_preference("open-annihilation.brains.skirmish") + " " + quote_preference(text)


def launch(native, game_dir, work, user, preferences):
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
        "60",
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
        print("brain seats: skipped, no installed game")
        return 77
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-brain-seats-", dir=scratch_root) as temporary:
        work = Path(temporary)
        user = work / "user"
        install_brains(user)

        none = work / "none"
        none.mkdir()
        none_preferences = none / "preferences.conf"
        write_preferences(none_preferences, [])
        status, output = launch(native, game_dir, none, user, none_preferences)
        require(status == 0, output, f"run A exited with {status}")
        require(not lines_with(output, "brains:"), output, "run A logged a brain seat")
        stored = none_preferences.read_text(encoding="utf-8", errors="replace")
        require(
            "open-annihilation.brains." not in stored,
            output,
            "run A wrote a brains key",
        )
        print("brain seats: run A seated none")

        firm = work / "firm"
        firm.mkdir()
        firm_preferences = firm / "preferences.conf"
        write_preferences(firm_preferences, [seat_line("steady-commander", "firm")])
        status, output = launch(native, game_dir, firm, user, firm_preferences)
        require(status == 0, output, f"run B exited with {status}")
        require(len(lines_with(output, PLAYS)) == 1, output, "run B did not play Steady Commander at firm")
        print(f"brain seats: run B {PLAYS}")

        default = work / "default"
        default.mkdir()
        default_preferences = default / "preferences.conf"
        write_preferences(
            default_preferences,
            [
                quote_preference("open-annihilation.brain.default")
                + " "
                + quote_preference("steady-commander")
            ],
        )
        status, output = launch(native, game_dir, default, user, default_preferences)
        require(status == 0, output, f"run C exited with {status}")
        require(len(lines_with(output, PLAYS)) == 1, output, "run C did not play the default at firm")
        print(f"brain seats: run C {PLAYS}")

        ridge = work / "ridge"
        ridge.mkdir()
        ridge_preferences = ridge / "preferences.conf"
        write_preferences(ridge_preferences, [seat_line("ridge-raider", "")])
        status, output = launch(native, game_dir, ridge, user, ridge_preferences)
        require(status == 0, output, f"run D exited with {status}")
        require(len(lines_with(output, RIDGE)) == 1, output, "run D did not leave the built-in player")
        require(
            not lines_with(output, "plays ridge-raider"),
            output,
            "run D seated Ridge Raider",
        )
        print(f"brain seats: run D {RIDGE}")

        missing = work / "missing"
        missing.mkdir()
        missing_preferences = missing / "preferences.conf"
        write_preferences(missing_preferences, [seat_line("tide-keeper", "")])
        status, output = launch(native, game_dir, missing, user, missing_preferences)
        require(status == 0, output, f"run E exited with {status}")
        require(len(lines_with(output, MISSING)) == 1, output, "run E did not say tide-keeper is not installed")
        print(f"brain seats: run E {MISSING}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ScriptFailure as failure:
        print(f"brain seats: {failure}")
        raise SystemExit(1) from None
