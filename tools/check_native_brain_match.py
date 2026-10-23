#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A headless brain match reports the same lines twice and refuses a bad seat.

steady-commander draws a point within 600 world units of its commander's
start every 150 ticks, and orders that move when the unit offers one. It
prints the tick every 900 ticks. idle-brain has a start and nothing else.
breaks-at-300 raises from onTick at tick 300. Each play is Ashap Plateau
at seed 4242 for 1800 ticks, with a fresh preferences file.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

MAP = "Ashap Plateau"
SEED = "4242"
TICKS = "1800"
RUN_TIMEOUT_SECONDS = 300

HEADING = re.compile(r"^brain match: map Ashap Plateau seed 4242 ticks 1800$")
STEADY = re.compile(r"^brain match: seat 1 steady-commander 1\.2 level calm side 0$")
BUILTIN_HARD = re.compile(r"^brain match: seat 2 built-in level hard side 1$")
DIGEST = re.compile(r"^brain match: digest ([0-9a-f]{16})$")
TOTALS = re.compile(
    r"^brain match: seat (\d+) units -?\d+ built -?\d+ lost -?\d+ kills -?\d+ "
    r"metal -?\d+ energy -?\d+ metal-produced -?\d+ energy-produced -?\d+ "
    r"instructions (-|\d+) status (playing|built-in|stopped-at \d+)$"
)
STOPPED = re.compile(r"^brain match: seat 1 stopped: .*brain\.lua:\d+:")

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
"""

IDLE_MANIFEST = """oabrain: 1
id: idle-brain
name: "Idle Brain"
version: "1.0"
author:
  name: "Ridge Testers"
description: "Starts and waits."
tags: ["teaching"]
works-with: ["*"]
levels:
  - id: idle
    name: "Idle"
    difficulty: medium
"""

BREAKS_MANIFEST = """oabrain: 1
id: breaks-at-300
name: "Breaks At 300"
version: "1.0"
author:
  name: "Ridge Testers"
description: "Stops at tick 300."
tags: ["teaching"]
works-with: ["*"]
levels:
  - id: break
    name: "Break"
    difficulty: hard
"""

# The draws stay outside the guarded order, so they happen when move is absent.
STEADY_SCRIPT = """local spent = 0
for i = 1, 4000 do
  spent = spent + i
end
local home_x
local home_z
local hooks = {}
function hooks.onTick(tick)
  if tick % 150 == 0 then
    local dx = brain.random(1201) - 601
    local dz = brain.random(1201) - 601
    if tick % 900 == 0 then
      print(tostring(tick))
    end
    pcall(function()
      local units = me:units()
      local unit = units[1]
      if unit == nil then
        return
      end
      if home_x == nil and type(unit.x) ~= "nil" and type(unit.z) ~= "nil" then
        home_x = unit.x
        home_z = unit.z
      end
      if home_x ~= nil and type(unit.move) == "function" then
        unit:move({x = home_x + dx, z = home_z + dz})
      end
    end)
  elseif tick % 900 == 0 then
    print(tostring(tick))
  end
end
return hooks
"""

IDLE_SCRIPT = """local hooks = {}
function hooks.start()
end
return hooks
"""

BREAKS_SCRIPT = """local hooks = {}
function hooks.onTick(tick)
  if tick == 300 then
    error("breaks")
  end
end
return hooks
"""


class ScriptFailure(Exception):
    """A play that did not do what the check asked."""


def plant(folder, manifest, script):
    """Writes one brain package: a manifest and brain.lua."""
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "oabrain.yaml").write_text(manifest, encoding="utf-8")
    (folder / "brain.lua").write_text(script, encoding="utf-8")


def fresh_preferences(path):
    """A preferences file with no remembered skirmish."""
    path.write_text("open-annihilation-preferences 1\n", encoding="utf-8")


def launch(native, game_dir, work, user, preferences, extra):
    """Runs one brain match. Returns the status and the combined output."""
    environment = dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
    )
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        "--preferences-file",
        str(preferences),
        "--user-folder",
        str(user),
        *extra,
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


def report_lines(output):
    """The lines the brain match printed."""
    return [line for line in output.splitlines() if line.startswith("brain match: ")]


def require(condition, output, message):
    """Stops the check when a play did not do what it should."""
    if condition:
        return
    print(output, end="" if output.endswith("\n") or output == "" else "\n")
    raise ScriptFailure(message)


def play(native, game_dir, work, user, name, extra):
    """One fresh preferences file and one run. Returns status and output."""
    preferences = work / f"{name}.conf"
    fresh_preferences(preferences)
    return launch(native, game_dir, work, user, preferences, extra)


def match_command(seats, levels=None):
    """The shared headless command, plus an optional level pair."""
    command = [
        "--headless-check",
        "--brain-match",
        seats,
        "--brain-map",
        MAP,
        "--seed",
        SEED,
        "--match-ticks",
        TICKS,
    ]
    if levels is not None:
        command.extend(["--brain-levels", levels])
    return command


def digest_of(lines):
    """The world digest, or None when the report has none."""
    for line in lines:
        found = DIGEST.match(line)
        if found:
            return found.group(1)
    return None


def totals_of(lines, seat):
    """The instructions and status of one seat's totals line."""
    prefix = f"brain match: seat {seat} "
    for line in lines:
        if not line.startswith(prefix) or " units " not in line:
            continue
        found = TOTALS.match(line)
        if found and found.group(1) == str(seat):
            return found.group(2), found.group(3)
    return None


def steady_report(lines, output, scratch):
    """The folder play's report: calm against hard, with no path in it."""
    require(HEADING.match(lines[0]) is not None, output, f"heading was {lines[0]!r}")
    require(STEADY.match(lines[1]) is not None, output, f"seat 1 was {lines[1]!r}")
    require(BUILTIN_HARD.match(lines[2]) is not None, output, f"seat 2 was {lines[2]!r}")
    require(any(line.startswith("brain match: end tick ") for line in lines), output, "no end line")
    seat1 = totals_of(lines, 1)
    seat2 = totals_of(lines, 2)
    require(seat1 is not None and seat2 is not None, output, "a totals line is missing")
    require(seat1[1] == "playing", output, f"seat 1 status was {seat1[1]}")
    require(seat1[0] not in ("-", "0"), output, f"seat 1 instructions were {seat1[0]}")
    require(seat2 == ("-", "built-in"), output, f"seat 2 totals were {seat2}")
    require(digest_of(lines) is not None, output, "the digest is missing")
    scratch_text = str(scratch)
    for line in lines:
        require(scratch_text not in line, output, "a report line names the scratch folder")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    native = arguments.native.resolve()
    game_dir = arguments.game_dir.resolve()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="T02-brain-match-", dir=arguments.scratch_root) as scratch:
        work = Path(scratch)
        packages = work / "packages"
        plant(packages / "steady-commander", STEADY_MANIFEST, STEADY_SCRIPT)
        plant(packages / "idle-brain", IDLE_MANIFEST, IDLE_SCRIPT)
        plant(packages / "breaks-at-300", BREAKS_MANIFEST, BREAKS_SCRIPT)
        empty = work / "empty-user"
        empty.mkdir()
        steady = f"{packages / 'steady-commander'},builtin"

        status, first_out = play(
            native, game_dir, work, empty, "steady-first", match_command(steady)
        )
        first_lines = report_lines(first_out)
        require(status == 0, first_out, f"steady-commander,builtin exited with {status}")
        require(len(first_lines) >= 7, first_out, "the first report is short")
        steady_report(first_lines, first_out, work)

        status, second_out = play(
            native, game_dir, work, empty, "steady-second", match_command(steady)
        )
        second_lines = report_lines(second_out)
        require(status == 0, second_out, f"the second play exited with {status}")
        require(first_lines == second_lines, second_out, "the two reports differ")

        status, easy_out = play(
            native, game_dir, work, empty, "easy-hard", match_command("builtin,builtin", "easy,hard")
        )
        easy_lines = report_lines(easy_out)
        require(status == 0, easy_out, f"easy,hard exited with {status}")
        require(
            any(line == "brain match: seat 1 built-in level easy side 0" for line in easy_lines),
            easy_out,
            "seat 1 was not built-in at easy",
        )
        require(
            any(line == "brain match: seat 2 built-in level hard side 1" for line in easy_lines),
            easy_out,
            "seat 2 was not built-in at hard",
        )
        easy_digest = digest_of(easy_lines)
        require(easy_digest is not None, easy_out, "easy,hard printed no digest")

        status, hard_out = play(
            native, game_dir, work, empty, "hard-easy", match_command("builtin,builtin", "hard,easy")
        )
        hard_lines = report_lines(hard_out)
        require(status == 0, hard_out, f"hard,easy exited with {status}")
        hard_digest = digest_of(hard_lines)
        require(hard_digest is not None, hard_out, "hard,easy printed no digest")
        require(easy_digest != hard_digest, hard_out, "swapping the levels kept the digest")

        broken = f"{packages / 'breaks-at-300'},{packages / 'idle-brain'}"
        status, broken_out = play(
            native, game_dir, work, empty, "breaks", match_command(broken)
        )
        broken_lines = report_lines(broken_out)
        require(status == 0, broken_out, f"breaks-at-300 exited with {status}")
        require(
            any("status stopped-at 300" in line for line in broken_lines),
            broken_out,
            "seat 1 did not stop at 300",
        )
        require(
            any(STOPPED.match(line) for line in broken_lines),
            broken_out,
            "the stopped line does not name brain.lua and a line",
        )
        require(digest_of(broken_lines) is not None, broken_out, "the stopped match printed no digest")

        installed = work / "installed-user"
        plant(installed / "Brains" / "steady-commander", STEADY_MANIFEST, STEADY_SCRIPT)
        status, named_out = play(
            native,
            game_dir,
            work,
            installed,
            "by-id",
            match_command("steady-commander,builtin"),
        )
        named_lines = report_lines(named_out)
        require(status == 0, named_out, f"steady-commander by id exited with {status}")
        require(
            any(STEADY.match(line) for line in named_lines),
            named_out,
            "the installed id was not seated",
        )
        require(
            str(installed) not in "\n".join(named_lines),
            named_out,
            "the report names the user folder",
        )

        def refused(name, extra, phrase):
            status_code, output = play(native, game_dir, work, empty, name, extra)
            require(status_code == 1, output, f"{name} exited with {status_code}")
            require(phrase in output, output, f"{name} did not say {phrase!r}")

        refused(
            "unknown-id",
            match_command("ridge-raider,builtin"),
            "brain match: seat 1 ridge-raider was not seated:",
        )
        refused(
            "missing-map",
            [
                "--headless-check",
                "--brain-match",
                "builtin,builtin",
                "--brain-map",
                "Not A Map",
                "--seed",
                SEED,
                "--match-ticks",
                TICKS,
            ],
            "brain match: map Not A Map could not be played",
        )
        refused(
            "one-seat",
            [
                "--headless-check",
                "--brain-match",
                "builtin",
                "--brain-map",
                MAP,
                "--seed",
                SEED,
                "--match-ticks",
                TICKS,
            ],
            "--brain-match expects two seats",
        )
        refused(
            "needs-headless",
            ["--brain-match", "x,y"],
            "--brain-match needs --headless-check",
        )
    print("brain match check: two plays agree, levels change the digest, and refusals exit 1")


if __name__ == "__main__":
    main()
